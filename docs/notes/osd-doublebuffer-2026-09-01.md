# OSD double-buffering attempt (k230_osd) — 2026-09-01

Status: **resumed; redesigned and locally validated, live-board validation
pending**. The original double-buffer attempt described in sections 1–6 was
shelved after flaky rendering and an apriltag detect-thread segfault. Section 7
records the confirmed races/lifetime bugs and the replacement three-buffer
latest-wins design now wired into both in-tree consumers.

This doc records exactly what was modified, what worked, and the failure, so
we don't lose the context when we pick this back up.

---

## 1. Goal / motivating context

Both apps (`apriltag_demo`, `ai_demo/tinytag_detect`) follow the same K230
`ai_demo` camera→hardware-plane + software-OSD pipeline. Per-frame the
producer CPU-draws an overlay into a canvas, then the display's
`frame_handler` **memcpy's** the whole canvas into the ARGB OSD plane and does
a full `thead_csi_dcache_clean_invalid_range`. On the single exposed CPU core
that overlay plumbing moves far more bytes than the detector itself.

We wanted to:
- remove the per-detection full-frame OSD **memcpy** (draw straight into the
  plane's device buffer, no separate canvas),
- **double-buffer** the OSD plane (front/back) so the producer draws a shadow
  buffer while the display scans the other — no tearing, no lag,
- **pre-clear** the back buffer at the start of the frame so the clear
  overlaps capture/detect, and the draw-on-publish is housekeeping-free,
- do it as **one shared `k230_osd` library** that both apps (and future ones)
  link, and port the LCD (portrait) fast path into it.

---

## 2. What we changed

### 2.1 New package `k230_osd` (the single shared copy)
- `buildroot-overlay/package/k230_osd/include/k230_osd.h` — API + docs on the
  **lifecycle** (`osd_prepare → capture/detect → osd_begin(draw) →
  osd_publish → osd_on_frame(swap)`) and the **fast-detect caveat** (only
  clear the free BACK buffer; if a swap is in flight, defer the clear so we
  never clear a buffer the scanout is still reading; `K230_OSD_BUFFER_COUNT`
  exists so going to 3 buffers is a one-line change).
- `.../src/k230_osd.cc` — double-buffered FRONT/BACK ARGB dumb buffers on the
  OSD plane, tear-free swap via `display->osd_disp_buffer`, generation gating,
  landscape direct-draw + `SLOW_ROTATE` (rotate) fallback, deferred-clear.
  Geometry modelled on the confirmed-working `apriltag_demo` branch.
- `CMakeLists.txt` / `Config.in` / `k230_osd.mk` — static lib, depends on
  `display` + `opencv4`, installed to staging so consumers can build/link it.
  Registered in `buildroot-overlay/package/Config_canaan.in` and enabled via
  `BR2_PACKAGE_K230_OSD=y` in `k230_canmv_defconfig`.

Build fixes hit along the way:
- `include(FindPkgConfig)` before `pkg_check_modules` (CMake configure error).
- the CSI `dcache.civa` helper is a `static inline` in `thead.h` (from the
  `display` package), so include `thead.h` rather than forward-declare
  (`extern` left an undefined symbol at link).
- compile with `-mcpu=c908v -mabi=lp64d` so the `xtheadcmo`-gated asm in
  `thead.h` assembles (default `rv64gc` doesn't enable it).

### 2.2 tinytag_detect wired to the module
- `tinytag_detect.mk`: `TINYTAG_DETECT_DEPENDENCIES += ... k230_osd`.
- `CMakeLists.txt`: `target_link_libraries(... k230_osd)`.
- `main.cc`:
  - include `k230_osd.h`, replaced the `osd_frame`/`draw_buffer` globals with
    `struct k230_osd *g_osd`.
  - `display_proc`: create the OSD via `k230_osd_create` + commit the front.
  - `ai_proc`: `k230_osd_prepare` at frame start; draw into
    `k230_osd_begin`; `k230_osd_publish` on the overlay.
  - `frame_handler`: replace the whole memcpy/dcache/rotate block with
    `k230_osd_on_frame`.
  - **Alignment fix that later got reverted with the rest:** size the OSD
    plane to the CAMERA's displayed region (16:9,
    `display_width × (display_width·720/1280 & 0xfff8)`), not the full
    display, so detections align with the camera image and don't leave an
    offset strip.

### 2.3 apriltag_demo wired to the module
- `apriltag_demo.mk`: `APRILTAG_DEMO_DEPENDENCIES += ... k230_osd`.
- `CMakeLists.txt`: added `k230_osd` inside both `--start-group` link groups.
- `main.cc`:
  - include `k230_osd.h`; replaced `draw_buffer`/`draw_frame` (and the
    `draw_frame_lcd90cw` fast path) with `g_osd`.
  - `display_proc`: `k230_osd_create` + commit front (camera-region-sized for
    landscape).
  - `detect_proc`: `k230_osd_prepare` at loop start; draw into
    `k230_osd_begin`; `k230_osd_publish`.
  - `frame_handler`: replaced the 3-branch (landscape / LCD-fastpath /
    temp+rotate) memcpy block with `k230_osd_on_frame`.
  - the USB-open-failure message path routed through the module canvas.
  - teardown: replace `display_free_plane(plane)` with
    `k230_osd_destroy(g_osd)`.
  - (The LCD `draw_detections_lcd_90cw` panel-orientation fast path was
    deferred — landscape drives the app in the test, and portrait is served
    by the module's `SLOW_ROTATE` rotate fallback.)

---

## 3. What worked

- `k230_osd` **builds** as a static lib against the real sysroot
  (`make ... k230_osd` → `libk230_osd.a`).
- **tinytag** after wiring it: **builds, links, and the video-file regression
  passed** — running the new binary on `220-225.mp4` @0.35 gave RC=0,
  **185 detections (unchanged)**, no errors. (Video-file mode doesn't hit the
  OSD path, so this confirmed the module links and the app still processes.)
- The **OSD-alignment fix worked**: sizing the OSD plane to the camera's 16:9
  displayed region removed the pink bar at the bottom of the screen and the
  detection-overlay offset-by-that-bar-height. The tag drawing aligned with
  the real tag on the live view.

---

## 4. What failed

### 4.1 Flaky OSD (both apps)
Once the OSD module was active in the live view, the overlay became
intermittent/unstable — hard to tell whether it was unstable ROI proposals
(the NN proposing different ROIs frame-to-frame) or the OSD rendering itself.
The plan was to A/B by enabling the same module on apriltag_demo.

### 4.2 Segmentation fault (apriltag_demo detect thread)
Enabling the module on apriltag_demo produced a hard crash. Kernel log:
```
[v4l2-drm] /dev/video2 support format BG3P
[v4l2-drm] /dev/video2 support format P010
[input] CSI requested 1280x720, negotiated 1280x720 stride=1280
[91773.737537] apriltag-detect[735]: unhandled signal 11 code 0x1 at 0x0000003f93f474b4
[91773.749755] CPU: 0 PID: 735 Comm: apriltag-detect Tainted: G O 6.6.36 #2
[91773.757894] Hardware name: Canaan CanMV-K230 (DT)
[91773.762621] epc : 0000003f9c1e0fcc ra : 0000003f9c1e0eda sp : 0000003f99ebbba0
[91773.769867]  gp : 0000002ae5e8b800 tp : 0000003f99ebd7c0 t0 : 0000003f8c002690
[91773.777103]  t1 : 0000000000000780 t2 : 0000000000001240 s0 : 0000003f93817000
[91773.784346]  s1 : 0000000000001e00 a0 : 0000000000000000 a1 : 0000000000010000
[91773.791589]  a2 : ffffffffffffa9e1 a3 : 0000003f93f474b4 a4 : 0000000000000144
...
[91773.840371] status: 8000000200006620 badaddr: 0000003f93f474b4 cause: 000000000000000f
[91774.246277] vvcam-mipi 90009800.mipi.0: vvcam_mipi_release
[91774.256519] vvcam-isp 90000000.isp.0: vvcam_isp_release:125
Segmentation fault
```
- Signal 11 (`SIGSEGV`), `cause=0xf` (page fault), `badaddr` a heap address,
  `a0=0`. Crash is in the `apriltag-detect` thread while drawing into the OSD
  canvas.
- Because BOTH apps became flaky and apriltag segfaulted, the common factor
  is the shared `k230_osd` module (not app-specific ROI instability) — the
  tinytag-only flakiness was a symptom, confirmed by reproducing it on
  apriltag.

### 4.3 Root-cause analysis (not yet confirmed)
Most likely in the **double-buffer swap / recycle** logic:
- The producer draws into "back" while the scanout reads "front", but the
  swap (`display->osd_disp_buffer` set + front/back toggle in
  `k230_osd_on_frame`) isn't airtight, so at times the detect thread draws
  into a buffer the scanout is still reading (tearing) or a buffer it
  shouldn't yet reuse.
- The **deferred-clear** path can also leave stale content, or the
  `fast_view[back_idx]` `cv::Mat` can be pointed at a buffer that isn't
  actually free, leading to a bad deref on the draw path (`a0=0`,
  heap `badaddr`).
- The single-CPU reality: the display thread (which commits/drains the swap at
  vsync) and the detect thread timeshare one core, so the buffer-reuse timing
  is fragile in the current design.

Hypotheses to test when resuming:
1. Relax the swap so the producer only ever draws into a buffer that is
   **definitively free** — i.e. drain the swap commit before reusing (needs a
   "swap completed" signal) or go to **triple-buffering**
   (`K230_OSD_BUFFER_COUNT = 3`).
2. Make `k230_osd_prepare`/`k230_osd_begin` refuse (or fall back to clearing
   in `publish`) unless the back buffer is genuinely free, and never draw into
   a view whose plane buffer was swapped away.
3. Validate the geometry/mapping of the camera-region-sized canvas on the
   actual monitor resolution before re-enabling.

---

## 5. What we reverted / current state

- **Reverted** `buildroot-overlay/package/apriltag_demo/src/main.cc` and
  `buildroot-overlay/package/ai_demo/tinytag_detect/main.cc` via `git
  checkout` back to their pre-module (stable single-buffer OSD) form. Both
  apps now use the original `osd_frame`/`draw_frame` → plane memcpy + dcache
  flush path.
- The `k230_osd` package (source, header, CMake/Config/.mk) and the
  `k230_osd` **link/dependency additions** in both apps' `.mk`/`CMakeLists`
  are **kept** (harmless — the apps link an un-used `libk230_osd.a`) so we can
  resume quickly.
- The committed, GOOD work from earlier in the session remains and is
  **separate from this**: the tinytag 16:9 crop + `heatmap_thres=0.35`
  operating point (commit `94d593b`), measured on-device.

### Board state after revert
The two `main.cc` are clean, but the **deployed binaries on the board are
still the bad (module) builds**. Rebuild + redeploy to restore stability:
```
make CONF=k230_canmv_defconfig tinytag_detect-dirclean && make CONF=k230_canmv_defconfig tinytag_detect
scp output/k230_canmv_defconfig/target/root/app/tinytag_detect/tinytag_detect.elf root@10.111.41.234:/root/app/tinytag_detect/

make CONF=k230_canmv_defconfig apriltag_demo-dirclean && make CONF=k230_canmv_defconfig apriltag_demo
scp output/k230_canmv_defconfig/target/root/app/apriltag_demo/apriltag_demo.elf root@10.111.41.234:/root/app/apriltag_demo/
```

---

## 6. Next steps (when we resume)

1. Decide whether to **pursue the double-buffered OSD** (risky, needs careful
   swap/recycle debugging on hardware) or **keep the stable single-buffer
   path** (simpler, current state).
2. If pursuing: fix the swap so the producer only draws a definitively-free
   buffer (triple-buffering is the robust option), remove/rework the
   deferred-clear path, and validate geometry on the actual monitor before
   re-enabling either app.
3. Note the valuable takeaway that DID work: sizing the OSD plane to the
   camera's 16:9 displayed region fixed the pink-bar / detection-offset — that
   fix is worth re-capturing independently of the double-buffer swap.

---

## 7. Resumed debugging / replacement design (same day)

The module was re-enabled after tracing the actual ordering in
`vvcam/v4l2-drm/src/lib.c`: the app callback runs **before**
`display_handle_vsync()`, then `osd_disp_buffer` is added to the next atomic
commit. The old two-buffer code toggled FRONT/BACK in that callback, before the
new FB had completed scanout. A fast producer could therefore clear and draw
the old FRONT while it was still scanned. The deferred-clear workaround did
not reserve the buffer; worse, `publish()` performed the deferred clear
*after* the app had drawn, erasing that overlay.

Additional correctness bugs found:

- `last_committed` and unused entries in `buffers[]` were uninitialized.
  This made generation gating nondeterministic and made allocation-failure
  teardown capable of freeing garbage pointers.
- Both apps used a mutex as a startup semaphore: main locked it and the display
  thread unlocked it. Unlocking a mutex from a non-owner is undefined behavior.
  This is now a condition-variable handshake.
- `apriltag_demo` stopped/joined the display thread before the detect thread.
  The display thread could destroy the OSD mappings while detect was still
  drawing. Shutdown now joins the producer before stopping/destroying display.

The replacement is a three-buffer, latest-wins state machine:

```
FRONT (scanout) -> PENDING (next atomic commit) -> DRAWING / READY (CPU)
```

A PENDING buffer becomes FRONT only when the following displayed callback
reports completion of the preceding commit. The producer only acquires FREE or
uncommitted READY buffers, never FRONT/PENDING. If inference outruns vsync, the
producer repaints READY and counts the superseded overlay as dropped instead of
blocking detection or growing display latency.

Performance changes:

- landscape and portrait-fast paths draw directly into the pitch-aware DRM
  mapping: no per-overlay full-frame memcpy;
- the buffer is pre-cleared at frame start with `memset`;
- portrait fallback rotates directly from the persistent landscape canvas into
  the mapped DRM buffer: no rotate temp and no final memcpy;
- the display callback only retires/stages buffer ownership;
- both apps print `osd:` staging rate and cumulative `drop:` telemetry;
- landscape OSD uses the camera's 16:9 destination height, retaining the
  confirmed alignment fix.

Current local validation:

- `k230_osd` cross-build: PASS.
- `tinytag_detect` cross-build/link: PASS.
- `apriltag_demo.elf` and `apriltag_c_demo.elf`: PASS.
- apriltag host sequence/options tests run by the package build: PASS.
- Live board validation is still required; direct board access was unavailable
  from the development environment.

No distinct `tinytag_dev` target/source exists in this checkout, its
`origin/dev` tree, `upstream/dev`, or sibling project names. If that is a
board-only wrapper or another repository, its source path is needed to port its
display loop; `tinytag_detect` is the only TinyTag OSD consumer present here.
