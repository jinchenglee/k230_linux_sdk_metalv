# AprilTag live on-device demo (K230) — design

**Date:** 2026-07-25
**Status:** Approved for implementation planning
**Author:** JC Li (with Claude)

**Superseded coordinate note (2026-07-30):** The detector now follows the
official C resolution boundary. Stages through quad fitting remain decimated;
homography construction and decode use the original image, and final detections
use source-image coordinates. Older decimated-output statements below describe
the initial implementation.

## 1. Goal

Create a new app in the `k230_linux_sdk_metalv` buildroot that mimics
`apriltag-rvv`'s `examples/live_demo.rs`, but runs **on the K230 board** with
**on-device DRM display** instead of an OpenCV/X11 window.

Live camera video is shown on screen; AprilTag detections are overlaid as quad
outlines with `id` and `decision_margin` labels. The detection pipeline reuses
the existing `apriltag-rvv` Rust library — including its RVV kernels — rather
than reimplementing it.

Non-goals: network streaming, recording, multi-camera, tag families other than
Tag36h11, changing the `apriltag-rvv` detection logic.

## 2. Context and constraints

- **Reference app:** `buildroot-overlay/package/face_detect` already does
  camera → process → **DRM display** on this exact platform. We clone its
  structure and delete the AI half.
- **Target defconfig:** `k230_canmv_defconfig` is the **big-core, vector**
  build (`BR2_RISCV_ISA_RVV=y`, `-mcpu=c908v`), so RVV kernels can run in Linux
  userspace here.
- **No Rust/cargo integration exists** in this buildroot; apps are C/C++
  `local` packages (`src/` + build → `/root/app/…`).
- **`apriltag-rvv` library core is HAL-free.** `async-rvv-hal` is `optional`
  and used only by the bare-metal `[[example]]` binaries under the
  `qemu/gem5/k230` features; a `--lib` build never pulls it. The `image`/
  `opencv` crates are likewise optional and unused by `pipeline`/`tag36h11`.
- **On-device display has no X11/GTK**, so OpenCV `highgui` (`imshow`) cannot be
  used. Output is DRM/KMS via the K230 `display` + `v4l2-drm` C libraries.
- Sibling repo layout: `apriltag-rvv` lives at `/work/git_repo/apriltag-rvv`
  (sibling of this SDK). It expects `async-rvv/` and `apriltag/` as siblings,
  but those are **not needed** for the `capi` lib build.

## 3. Architecture overview

A new buildroot package **`apriltag_demo`** with two cleanly separated units
communicating over a small C ABI:

```
┌─────────────────────────────────────────────────────────────┐
│  apriltag_demo  (buildroot local package → /root/app/…)      │
│                                                              │
│  ┌────────────────────────┐     C ABI      ┌──────────────┐  │
│  │  C++ app (from          │ apriltag_detect │ libapriltag_ │  │
│  │  face_detect skeleton)  │ ─────────────► │ rvv.a        │  │
│  │                         │ ◄───────────── │ (Rust, RVV,  │  │
│  │  • v4l2-drm capture     │  detections[]  │  std + capi) │  │
│  │  • NV12 video plane     │                └──────────────┘  │
│  │  • ARGB8888 OSD overlay │                                  │
│  │  • draw quads/id/margin │                                  │
│  └────────────────────────┘                                  │
└─────────────────────────────────────────────────────────────┘
```

**Per-frame data flow** (two threads, mirroring `face_detect`):

1. **Capture thread** (`detect_proc`): `v4l2_drm_dump` yields an NV12 frame.
   Its **Y-plane is grayscale** → passed as `im_gray` straight into
   `apriltag_detect()` — no color convert, no repacking.
2. `apriltag_detect()` runs the Rust pipeline (`decimate → threshold →
   RLE/CCL → quads → decode → fit_gray_model`), scalar or RVV per a runtime
   flag, and returns an array of `{id, margin, center, corners[4]}`.
3. **Display thread** (`display_proc` + `frame_handler`): clears the ARGB8888
   OSD `draw_frame`, draws each detection's quad edges + `id=… m=…` label onto
   it (OpenCV on the OSD Mat), then `display_update_buffer`.
4. The **live camera is shown by hardware** on the NV12 video plane; the app
   only ever paints the transparent OSD overlay — the same efficient trick
   `face_detect` uses.

Each unit is independently testable: the Rust lib on host/QEMU (as it already
is), the C++ app against a stub detector.

### 3.1 As-built buffer and thread topology

Recorded 2026-07-29 against `b94660a`. Two things differ from the plan above:
the app runs **three** threads, not two, and the **detect thread draws the
overlay** — step 3's display thread only copies the finished ARGB image into
the DRM buffer. Sizes assume the defaults `--csi-size 1280x720`, `--factor 2`,
and a 1920x1080 HDMI mode.

```
                     OV5647 / GC2093  ──►  MIPI CSI-2  ──►  VICAP + ISP
                                                                │
                     ┌──────────────────────────────────────────┴───────────────────┐
                     │   TWO INDEPENDENT V4L2 CAPTURE CHANNELS off the same ISP     │
                     ▼                                                              ▼
   /dev/video<N+1>  "detect" channel                       /dev/video<N>  "display" channel
   V4L2_MEMORY_MMAP   buffer_num = 3                        V4L2_MEMORY_DMABUF  buffer_num = 4
   NV12 1280x720  (1.38 MB/buf, 4.1 MB total)               NV12 1920x1080 (3.11 MB/buf, 12.4 MB)
   main.cc:170-192                                          main.cc:584-604   (= DRM dumb buffers!)

╔═════════════════════════════════════════════╗   ╔══════════════════════════════════════════════╗
║ THREAD "apriltag-detect"      main.cc:166   ║   ║ THREAD "apriltag-disp"        main.cc:580    ║
╠═════════════════════════════════════════════╣   ╠══════════════════════════════════════════════╣
║                                             ║   ║  v4l2_drm_run()  lib.c:333                   ║
║  kernel queue  [B0][B1][B2]  (FIFO)         ║   ║  poll(video_fd, drm_fd)  ── 1 s timeout      ║
║        │  poll + VIDIOC_DQBUF               ║   ║        │                                     ║
║        │  ⚠ returns OLDEST frame            ║   ║        │ QBUF(buffer_hold[wp]) then DQBUF    ║
║        ▼                                    ║   ║        ▼                                     ║
║  ┌───────────────────────────────┐          ║   ║   buffer_hold[2] ring  +  wp   (lib.c:405)   ║
║  │ buffers[idx].mmap  1.38 MB    │          ║   ║   holds idx of frame being scanned out       ║
║  │  Y plane 1280x720  921 KB  ───┼── ① ZERO ║   ║        │                                     ║
║  │  UV plane          unused     │   COPY   ║   ║        └──► NO CPU TOUCH — the DMABUF *is*   ║
║  └───────────────────────────────┘          ║   ║             the DRM video-plane buffer       ║
║        │                                    ║   ║                                              ║
║        │ optional 'n' denoise (full-res!)   ║   ║   ┌─── frame_handler()  main.cc:510 ───────┐ ║
║        ▼                                    ║   ║   │ if g_overlay_generation changed:       │ ║
║   filtered_gray  cv::Mat 921 KB  ── ②       ║   ║   │   lock(result_mutex)                   │ ║
║        │                                    ║   ║   │   ③ memcpy 8.29 MB  draw_frame→OSD     │ ║
║        ▼  apriltag_detect()  (SYNCHRONOUS)  ║   ║   │   unlock                               │ ║
║  ┌──────────────────────────────────────┐   ║   ║   │   ④ dcache_clean_invalid  8.29 MB      │ ║
║  │ Rust DetectBuffers (persistent)      │   ║   ║   │   display_update_buffer(draw_buffer)   │ ║
║  │  decimated  640x360    230 KB        │   ║   ║   └────────────────────────────────────────┘ ║
║  │  threshim   640x360    230 KB        │   ║   ║        │                                     ║
║  │  scratch / rvv_buf / run_rows        │   ║   ║        ▼  display_commit(d)                  ║
║  │  quad_fit scratch                    │   ║   ╚══════════════════════════════════════════════╝
║  └──────────────────────────────────────┘   ║
║        │  Vec<Detection>                    ║        ┌──────── DRM / KMS planes ──────────┐
║        ▼                                    ║        │                                    │
║   out[64]  apriltag_det_t                   ║        │  VIDEO plane   NV12 1920x1080      │
║        │                                    ║        │    ← 1 of the 4 capture DMABUFs    │
║        │  lock(result_mutex)                ║        │      (hardware scanout, zero copy) │
║        ▼                                    ║        │                                    │
║   ⚠ camera buffer STILL HELD here           ║        │  OSD plane     ARGB8888 1920x1080  │
║        │                                    ║        │    ← draw_buffer  ⚠ SINGLE buffer  │
║        └──► [ SHARED STATE ] ────────────────╬───────►│                                    │
║        │                                    ║        │  hardware alpha-composite → HDMI   │
║   VIDIOC_QBUF  (release, main.cc:497)       ║        └────────────────────────────────────┘
╚═════════════════════════════════════════════╝

   ┌──────────── SHARED STATE — guarded by the single result_mutex ────────────┐
   │                                                                          │
   │   std::vector<apriltag_det_t> detections      (a few hundred bytes)       │
   │   cv::Mat draw_frame   ARGB8888 1920x1080  = 8.29 MB  ON THE HEAP         │
   │      ⚠ setTo(0,0,0,0) full-frame clear EVERY detect iteration            │
   │   std::atomic<uint64_t> g_overlay_generation  (publish handshake)         │
   │                                                                          │
   │   producer: detect thread (clear + draw)   consumer: frame_handler (copy) │
   └──────────────────────────────────────────────────────────────────────────┘

   Third thread: main() — blocking read(STDIN) for hotkeys c/u/n/0-5/q; sets atomics only.
   Startup handshake: main locks result_mutex before spawning; frame_handler unlocks it on the
   first displayed frame, so detect_proc cannot touch draw_frame before display_proc creates it.
```

Per-frame DDR traffic, CPU-side only. This is the figure that matters most,
because the DTS in this build exposes a single C908v node, so all three
threads time-share one core:

| Path | Bytes touched by the CPU |
|---|---|
| ① Y-plane read into `detect()` | 921 KB read |
| Decimate + threshold + CCL | ~700 KB read/write over a 230 KB working set |
| ② Denoise, if `n` is enabled | +1.8 MB (runs at full 1280x720, before decimation) |
| ③ `draw_frame` clear | 8.29 MB write |
| ③ `memcpy` to OSD buffer | 8.29 MB read + 8.29 MB write |
| ④ dcache clean+invalidate | 8.29 MB range writeback |

The overlay plumbing (③ + ④ — roughly 25 MB of load/store plus an 8.3 MB cache
flush) therefore moves **more than ten times the data the detector itself
does**, on the same core the detector needs.

Two structural costs the diagram makes visible:

1. **Camera-buffer hold** (⚠ on the detect lane). The dequeued 1.38 MB camera
   buffer stays out of the kernel queue from `DQBUF` through the entire
   synchronous detection. With three buffers and no queue draining, `DQBUF`
   also returns the *oldest* queued frame, so overlays lag reality by up to two
   extra camera periods before detection even begins. This is what
   back-pressures the ISP and depresses the reported `camera:` rate.
2. **Single-buffered OSD** (⚠ on the OSD plane). `draw_frame` and the one OSD
   dumb buffer are two separate 8.29 MB images reconciled by a `memcpy` that
   runs *while holding* `result_mutex` — so the two threads serialise on the
   largest memory operation in the program.

The proposed remedies (split Stage 0 so the camera buffer is requeued right
after decimation; double-buffer the OSD plane and clear only dirty rectangles;
optionally let VICAP/ISP emit the decimated detection channel) are written up
in `package/apriltag_demo/README.md` under "Frame flow and hardware-offload
priorities".

### 3.2 Current buffer and thread topology (updated 2026-09-01)

The 2026-07-29 diagram above is intentionally retained as the **before**
snapshot. The current implementation has removed the single-buffered OSD copy
and uses the opt-in event-driven DRM loop, but it has **not yet shortened the
detection camera-buffer lifetime**. The remaining warning marker is therefore
on the detect-channel MMAP buffer, not the OSD handoff.

Sizes below use the same 1280x720 detect channel and 1920x1080 HDMI example as
the original drawing. Landscape OSD height follows the camera destination
rectangle; for a 1920x1080 mode that is still 1920x1080.

```
                     OV5647 / GC2093 ──► MIPI CSI-2 ──► VICAP + ISP
                                                              │
                    ┌─────────────────────────────────────────┴──────────────────┐
                    │  TWO INDEPENDENT V4L2 CAPTURE CHANNELS off the same ISP    │
                    ▼                                                            ▼
 /dev/video<N+1> "detect" channel                      /dev/video<N> "display" channel
 MMAP, 3 × NV12 1280x720                               DMABUF, 4 × NV12 1920x1080

╔═════════════════════════════════════════════╗   ╔═════════════════════════════════════════════╗
║ THREAD "apriltag-detect"                   ║   ║ THREAD "apriltag-disp"                     ║
║ detect_proc                                 ║   ║ display_proc + frame_handler              ║
╠═════════════════════════════════════════════╣   ╠═════════════════════════════════════════════╣
║                                             ║   ║                                             ║
║ k230_osd_prepare()                          ║   ║ v4l2_drm_run_event_driven()                ║
║   FREE or replaceable READY → DRAWING       ║   ║   poll(display video_fd, DRM fd)           ║
║   memset mapped DRM OSD buffer              ║   ║   at most ONE atomic commit in flight      ║
║        │                                    ║   ║        │                                    ║
║        ▼                                    ║   ║        ▼                                    ║
║ detect kernel queue [B0][B1][B2]            ║   ║ display DMABUF ring [V0][V1][V2][V3]      ║
║        │ VIDIOC_DQBUF                       ║   ║   newest uncommitted camera frame wins     ║
║        ▼                                    ║   ║   older pending arrival is DQBUF/QBUF      ║
║ ┌───────────────────────────────────────┐   ║   ║        │                                    ║
║ │ B[idx].mmap: NV12 1280x720            │   ║   ║        └──► no CPU video copy              ║
║ │ Y plane: zero-copy detector input     │   ║   ║                                             ║
║ │ UV plane: unused                      │   ║   ║ completed page flip:                       ║
║ └───────────────────────────────────────┘   ║   ║   OSD PENDING → FRONT                     ║
║        │                                    ║   ║   old OSD FRONT → FREE                    ║
║        │ optional full-resolution denoise   ║   ║   newest OSD READY → PENDING              ║
║        ▼                                    ║   ║        │                                    ║
║ apriltag_detect() — synchronous             ║   ║        ▼                                    ║
║ decimate → threshold → CCL → quads → decode ║   ║ next atomic commit includes:              ║
║        │                                    ║   ║   • freshest pending camera DMABUF         ║
║        ▼                                    ║   ║   • optional new OSD PENDING buffer        ║
║ detections[] (detect-thread local)           ║   ║                                             ║
║        │                                    ║   ╚═════════════════════════════════════════════╝
║        ▼                                    ║
║ draw quads/text DIRECTLY into mapped        ║
║ ARGB8888 OSD buffer in DRAWING state        ║
║   no draw_frame heap canvas                 ║
║   no full-frame OSD memcpy                  ║
║        │                                    ║
║        ▼                                    ║
║ k230_osd_publish()                          ║
║   dcache clean mapped buffer                ║
║   DRAWING → READY (latest wins)             ║
║   superseded READY → FREE, drop++           ║
║        │                                    ║
║        ▼                                    ║
║ ⚠ B[idx] IS STILL DEQUEUED HERE             ║
║        │                                    ║
║        └── VIDIOC_QBUF after detect + OSD   ║
╚═════════════════════════════════════════════╝

              ┌──────── shared k230_osd ownership (short mutex only) ────────┐
              │                                                             │
              │ Three pitch-aware ARGB8888 DRM buffers (~8.29 MB each):     │
              │                                                             │
              │   FREE → DRAWING → READY → PENDING → FRONT ─────────┐       │
              │     ▲                                               │       │
              │     └────────────── completed flip ─────────────────┘       │
              │                                                             │
              │ Producer never touches FRONT/PENDING. If it outruns display,│
              │ it repaints uncommitted READY: bounded memory, latest wins. │
              └─────────────────────────────────────────────────────────────┘

                       ┌────────── DRM / KMS planes ──────────┐
                       │ VIDEO: one of four NV12 DMABUFs      │
                       │ OSD:   FRONT of three ARGB buffers   │
                       │ hardware alpha-composite → HDMI/LCD │
                       └──────────────────────────────────────┘

 Third thread: main() — blocking STDIN hotkeys; atomics only.
 Startup: condition-variable handshake. Shutdown: producer joins before OSD/display teardown.

 ─ ─ ─ ─ ─ ─ ─ ─ NEXT CHANGE (not implemented yet) ─ ─ ─ ─ ─ ─ ─ ─

 DQBUF B[idx] → short Stage-0 ingest/decimate into detector-owned storage → QBUF B[idx]
                                                                    │
                                                                    ▼
                  remaining threshold/CCL/quad/decode + OSD proceeds
                  without holding a capture buffer or back-pressuring ISP
```

What changed relative to the original topology:

1. **OSD bottleneck resolved.** The heap `draw_frame`, 8.29 MB per-overlay
   memcpy, and long `result_mutex` critical section are gone. The producer
   draws into one of three mapped DRM buffers; exact FRONT/PENDING retirement
   prevents scanout reuse, while READY replacement bounds latency when
   detection outruns OSD commits.
2. **Display scheduling is opt-in and event-driven.** Only the tag apps call
   `v4l2_drm_run_event_driven()`. It drains a completed flip before submitting
   another and commits only when a fresh camera or OSD plane update exists;
   legacy users of `v4l2_drm_run()` retain their old behavior.
3. **Camera-buffer hold remains.** `detect_proc` still passes the dequeued
   MMAP Y plane directly into the synchronous detector and calls
   `v4l2_drm_dump_release()` only after drawing and publishing the overlay.
   This is now the next structural bottleneck.

The CPU-side OSD traffic is now approximately one mapped-buffer clear plus one
cache writeback per produced overlay. The full-frame `draw_frame → DRM` read +
write pair has been eliminated:

| Current path | Bytes touched by the CPU |
|---|---|
| Y-plane read into `detect()` | 921 KB read |
| Decimate + threshold + CCL | ~700 KB read/write over a 230 KB working set |
| Denoise, if enabled | +1.8 MB at 1280x720 |
| Clear DRAWING OSD buffer | ~8.29 MB write |
| Draw overlay primitives | proportional to quads/text, directly in mapped OSD |
| Old full-frame OSD memcpy | **0** (removed: 8.29 MB read + 8.29 MB write) |
| Publish OSD | cache clean/writeback of the mapped buffer range |

Live-board telemetry after this change reached roughly `camera: 28`,
`detect: 28`, `osd: 21`, `display: 51`, and `poll: 80` fps in the reported
sample. `drop:` is cumulative and counts READY overlays superseded by newer
results before staging; it demonstrates the intended latest-wins policy, not
an unbounded queue.

The next change should split the detector immediately after input ingest
(factor-2 decimation in the common case), retain the decimated image in
detector-owned persistent storage, and requeue the MMAP capture buffer before
threshold/CCL/quad/decode. That shortens ownership without adding another
full-resolution copy. Whether acquisition later deserves its own thread is a
separate scheduling decision; it is not required for the first lifetime fix.

## 4. NV12 Y-plane ↔ `detect()` compatibility

`detect()` takes an explicit `stride` separate from `width`, which makes the
NV12 Y-plane a clean fit:

- Pass `stride = v4l2 bytesperline (Y plane)`, `width/height = active pixels`.
  `decimate` reads `im[y*stride + x]`, so row padding is absorbed with **zero
  copy**. The Y-plane length `stride*height` fully covers the reads — **no
  input over-read**.
- **Soft constraint (quality, not crashes):** threshold works on 4×4 tiles
  (`TILESZ=4`, `tw=dw/TILESZ`, `th=dh/TILESZ`, integer division). Leftover
  `dw%4 / dh%4` pixels on the right/bottom edge are simply not thresholded.
  Choose capture WxH so the **decimated** dims are multiples of 4 (e.g. 640×480
  at `factor=2` → 320×240, both ÷4). No hard requirement, no crash otherwise.
- **No stride *alignment* requirement** — RVV `vle8` handles unaligned loads.
- The RVV threshold kernel's documented read-ahead of up to `VLEN` bytes past
  the tile grid operates on `detect()`'s **internal decimated buffer** (already
  sized/handled since apriltag-rvv runs on k230), **not** on the NV12 input.

## 5. Component: Rust C-ABI static lib (`libapriltag_rvv.a`)

Built from the existing `apriltag-rvv` crate; adds only a thin FFI and one
feature. No fork.

### 5.1 Cargo changes
- New feature `capi` that enables `std` + exposes `pipeline` + `tag36h11`
  **without** the `image` dep. Broaden three cfgs in `src/lib.rs`:
  - `#![cfg_attr(not(any(test, feature="host", feature="capi")), no_std)]`
  - `#[cfg(any(test, feature="host", feature="capi"))] pub mod pipeline;`
  - `#[cfg(any(feature="host", feature="capi"))] pub mod tag36h11;`
- `[lib] crate-type = ["rlib", "staticlib"]` — `rlib` keeps existing host
  tests/examples building; `staticlib` emits the `.a`.
- `async-rvv-hal`, `image`, `opencv` remain out of a `--features capi --lib`
  build (all optional / example-only). This is the requested "strip".

### 5.2 New `src/capi.rs` (gated on `capi`)
`#[no_mangle] pub extern "C"` functions. All panics caught with
`std::panic::catch_unwind` → return `-1`; never unwind across FFI.

```c
typedef struct {
    uint64_t id;
    double   margin;       // decision_margin
    double   center[2];    // decimated-space center
    double   corners[8];   // 4 corners (x,y) in decimated space, CCW
} apriltag_det_t;

void* apriltag_new(uint32_t min_blob_size);   // stows DetectBuffers + Tag36h11 + min_blob
void  apriltag_free(void* handle);

// factor: 0 = 1.0, 1 = 1.5, 2 = 2.0
// mode:   0 = scalar, 1 = rvv
// returns detection count (>=0), or -1 on panic/error.
int   apriltag_detect(void* handle,
                      const uint8_t* y, size_t width, size_t height, size_t stride,
                      int factor, int mode,
                      apriltag_det_t* out, int max_out);
```

Mapping to the real `detect()`:

| `detect()` param | FFI | Notes |
|---|---|---|
| `im_gray, width, height, stride` | `y, width, height, stride` | direct |
| `factor: DecimateFactor` | `int factor` | enum map 0/1/2 |
| `mode: KernelMode` | `int mode` | enum map 0/1 |
| `family: &dyn TagFamily` | — | encapsulated in handle (`Tag36h11`) |
| `min_blob_size: u32` | `apriltag_new` arg | `live_demo` uses **25** |
| `debug: Option<…>` | — | always `None` |
| `bufs: &mut DetectBuffers` | `void* handle` | persistent across frames |
| `-> Vec<Detection>` | `out[]` + return count | marshaled, capped at `max_out` |

`apriltag_det_t` deliberately surfaces only OSD-relevant fields (drops
`hamming`, `homography`); these can be added later without breaking the ABI.

### 5.3 Build
- rustup **nightly** (repo pins it via `rust-toolchain.toml`), target
  `riscv64gc-unknown-linux-gnu` (lp64d — matches `c908v`).
- `RUSTFLAGS="-C target-feature=+v"` so `#[cfg(target_feature="v")]` asm
  kernels compile in. Vector regs are not part of the C ABI → no ABI hazard.
- `cargo build --release --features capi --lib` produces
  `target/riscv64gc-unknown-linux-gnu/release/libapriltag_rvv.a`.

## 6. Component: C++ app (cloned from `face_detect`)

Clone `face_detect`'s skeleton; **delete the AI half** (nncase, kmodel,
`ai_base`, `anchors_*`, `sensor_buf_manager`); keep camera + DRM + OSD.

- **`main.cc`:** same `display_init(0)` + two-thread model + `q`-to-quit + FPS
  counters. Threads renamed `detect_proc` / `display_proc`.
- **`detect_proc`:** `v4l2_drm_context` with `video_format =
  V4L2_PIX_FMT_NV12`. Per frame: `v4l2_drm_dump` → hand the buffer's Y-plane
  pointer + `stride` + w/h to `apriltag_detect(handle, …)` → store results under
  the existing `result_mutex`.
- **`display_proc` / `frame_handler`:** unchanged from `face_detect` — NV12
  video plane shown by hardware; ARGB8888 OSD plane via `display_get_plane`
  (`DRM_FORMAT_ARGB8888`) / `display_allocate_buffer`; landscape/portrait
  (ST7701 rotate) handling kept.
- **`draw_detections(osd_mat, dets, n, scale)`** replaces `draw_result_video`:
  for each detection, draw 4 quad edges from `corners[]` + `id=… m=…` label,
  using the same OpenCV `line`/`put_text` calls `live_demo.rs` uses.
- **Coordinate mapping:** `detect()` returns corners in **decimated** space, so
  map to display as `corner × decimate_scale × (display_dim / sensor_dim)`.
  Centralize in one helper. (`live_demo` applies only `× decimate_scale`; the
  display/sensor ratio is the on-device addition.)
- **Runtime flags:** `--rvv` (→ `mode`), `--factor 1|1.5|2` (like `live_demo`).
  Sensor defaults from `face_detect`'s `sensor_set.h`.

### 6.1 Build (`CMakeLists.txt`, `build_app.sh`, `.mk`)
- **Drop** `Nncase.*`, `functional_k230`, kmodel assets.
- **Keep** `opencv_core`, `opencv_imgproc` (OSD drawing), `v4l2-drm`, `drm`,
  `display`, `mmz`, `pthread`.
- **Add** `libapriltag_rvv.a` + Rust-std link deps: `-lpthread -ldl -lm -lrt`
  (plus `-lgcc_s` / unwind as needed — pinned at Milestone 1).
- **`build_app.sh`** runs `cargo build --features capi …` **first**, then cmake
  links the `.a` — same standalone-vs-buildroot dual mode `face_detect`
  supports (`CMAKE_TOOLCHAIN_FILE MATCHES buildroot/toolchainfile.cmake`).
- **`apriltag_demo.mk`:** `local` site, `$(eval $(cmake-package))`,
  `DEPENDENCIES += opencv4 display vvcam` (+ the cargo build step), installs to
  `/root/app/apriltag_demo/`, mirrors `face_detect`'s `.deb` hook.

## 7. Milestones

**Milestone 1 — walking skeleton (de-risk integration).**
The *full* build is real (std, `+v`, `capi`, staticlib, linked into the C++
app), but `apriltag_detect`'s **body** is trivial: read the Y-plane via
`(ptr,w,h,stride)`, compute the brightness centroid (or brightest-blob bbox),
return **one** `apriltag_det_t` box (`id = mean luma`). On screen: a box that
tracks a bright object. This validates, end to end:
- cargo → `.a` → C++ link, and the rust-std/glibc ABI on riscv64;
- the DRM/OSD plumbing and NV12 Y-plane + stride contract;
before any detection complexity exists.

**Milestone 2 — real detector.**
Swap the `apriltag_detect` body to call `detect(…, None, &mut bufs)`. Returns N
quads instead of 1 box. **Nothing on the C++ side changes** (it already loops
over `corners[]`).

## 8. Error handling

- FFI never unwinds: `catch_unwind` → `-1`; C++ treats `<0` as "no detections
  this frame" and logs once.
- `apriltag_detect` clamps output to `max_out`; C++ sizes the array to a fixed
  cap (e.g. 32).
- Camera/DRM setup failures reuse `face_detect`'s existing checks
  (`v4l2_drm_setup`/`start`, `display_init` returning error → exit).
- Missing/rotated panel handled by `face_detect`'s landscape/portrait paths.

## 9. Testing

- **Rust lib:** existing host tests + QEMU correctness/cross-checks continue to
  pass (the `capi` feature only adds an FFI surface and broadens cfgs). Add a
  minimal host test that calls the C ABI (`apriltag_new`/`detect`/`free`) on a
  synthetic gray buffer with non-trivial `stride` to lock the stride contract.
- **C++ app M1:** visual smoke test — bright object → tracking box on the panel;
  FPS counters non-zero (`camera`, `detect`, `display`).
- **C++ app M2:** print a known Tag36h11 target; confirm `id` matches and the
  quad aligns with the physical tag; compare `decision_margin` sanity vs
  `live_demo` on the same tag.
- **Scalar vs RVV:** run with and without `--rvv`; detections should match
  (RVV kernels are cross-checked against scalar in apriltag-rvv already), RVV
  `detect` fps ≥ scalar.

## 10. Risks

1. **Rust std ↔ Xuantie glibc ABI.** rustup's prebuilt `riscv64gc-
   unknown-linux-gnu` std is built against a generic glibc. Linking into a
   binary built with the Xuantie glibc may mismatch. Usual outcome for a
   *staticlib* is fine (libc symbols resolve at the final C++ link). **Fallback:**
   `-Z build-std` against the buildroot staging sysroot. **Surfaced at M1** by
   design.
2. **RVV asm under rustc/LLVM vs the Xuantie assembler.** apriltag-rvv already
   builds RVV for k230, so this is low, but the linux target + `+v` combination
   is new here. M1 compiles the kernels in (even though the trivial body doesn't
   call them) to surface build issues early.
3. **Link-time symbol list for rust-std** (`-lgcc_s`/unwind/`-lrt`) — pinned
   empirically at M1.
4. **Coordinate mapping** between decimated detection space, sensor space, and
   display space — validated visually at M2 against a physical tag.

## 11. Out of scope / future

- Additional tag families, network streaming, recording.
- Exposing `homography`/`hamming` through the ABI.
- Buildroot-native Rust packaging (`pkg-cargo`) instead of the `build_app.sh`
  cargo invocation — deferred; the shell-invoked cargo mirrors the existing
  `face_detect` standalone build path.
