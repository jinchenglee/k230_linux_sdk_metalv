I've now read both apps end-to-end. Both use the same underlying architecture (cloned from the ai_demo framework / face_detect), so I'll describe the shared model and then call out where each app differs and where the real opportunities are. This analysis is grounded in the actual code.



1. The architecture (shared by both apriltag_demo and tinytag_detect)

Threads / processes
A single process, 3 user threads + kernel/v4l2 IRQ threads:
- Detect thread (apriltag-detect / ai_proc) — camera capture + detector/AI.
- Display thread (apriltag-disp / display_proc) — runs v4l2_drm_run(..., frame_handler), the DRM event-poll loop.
- Main thread — interactive input loop (keys) only.
- Shared state: result_mutex (protects detections/results + the overlay canvas draw_frame/osd_frame), and an atomic g_overlay_generation that the producer bumps after publishing a complete overlay and the display handler uses to decide whether to copy.

Camera feed + buffer model
The camera is an NV12 V4L2/DMA capture (v4l2_drm_setup, buffer_num=3). The detect thread calls v4l2_drm_dump() which dequeues and holds a buffer; framework_gray is a zero-copy cv::Mat view over the mmap (no frame copy). The detector reads the Y-plane directly.
- apriltag_demo src/main.cc:355: gray = context.buffers[...].mmap, frame_stride = csi_stride — and the comment at :349-354 explicitly states the buffer stays dequeued through all pipeline stages, and that this causes "capture-queue back-pressure," with a planned "future split Stage-0 API ... decimate Y into detector-owned storage and requeue immediately."
- tinytag main.cc:273-280: net_input and full_res_gray are both zero-copy views over the same mmap; the buffer is held until :331 (v4l2_drm_dump_release) after post_process.

Display consumption + OSD surface
- The camera image is shown on the hardware NV12 video plane (the display thread sets up its own v4l2 context with display=true); so the camera feed to screen is a hardware fab/scaler → zero software copy for the camera image. Good.
- Detections are drawn in software onto a transparent ARGB8888 OSD plane (draw_buffer = a persistent DRM mmap allocated once in display_proc), through a CPU canvas (draw_frame/osd_frame, a persistent cv::Mat).
- frame_handler (display thread, per displayed frame) copies the canvas into the plane only when g_overlay_generation changed (a dedup both apps implemented — apriltag main.cc:640-648, tinytag main.cc:363-385). Then thead_csi_dcache_clean_invalid_range() flushes the whole buffer, then display_update_buffer().

ai2d engine + NPU (tinytag)
- pre_process(): a persistent ai2d_in_tensor_ + ai2d_builder_ (built once per input shape), a small row-copy of the band into the tensor (~0.8 ms), then a hardware ai2d resize to 640×360 (~2 ms). Good — persistent, hardware-resized.
- inference() → run() + get_output(): the KPU executes the kmodel (~2.2 ms); get_output() maps the output tensor zero-copy.
- post_process(): the CPU-bound part — decode_proposals + per-ROI AprilTag C crop-decode (historically the dominant cost).

Synchronization / latency-relevant design choices already made
- post_process (CV decode) deliberately runs outside result_mutex (tinytag main.cc:282-292), and only the brief overlay write to osd_frame is under the lock — this fixed a real display-stall bug when the int8 kmodel made inference fast.
- The overlay g_overlay_generation gating avoids redundant full-frame copies on camera/display events (which arrive faster than detection).

Critical context — there is only ONE application CPU on this board.
The board reports nproc=1, only cpu0 online (the README notes "the target exposes one application CPU"). Neither app pins threads. So all user-space compute is serial on one core: the detector, the overlay draw, the plane memcpy, and the dcache flush all timeslice on the same CPU. That reframes every opportunity: you can't split work across cores; you can only (a) reduce total CPU work, (b) overlap CPU work with hardware offload/async capture, or (c) reduce memory-bandwidth pressure.



2. How this currently hurts throughput & latency

- Capture back-pressure (biggest serialization point, both apps). Because the V4L2 buffer is held for the entire pipeline (detect or AI+CV decode), the 3-buffer capture queue can't recycle, so the camera is paced at the processing rate. Throughput is ≈ 1 / pipeline_time, and the held buffer adds a full pipeline-latency lag. The comment in apriltag main.cc:349-354 names this directly. [PARTIALLY ADDRESSED — see §5.] `v4l2_drm_dump_latest()` (b27c25e, lib.c:1123) now drains the done-queue and requeues every older buffer, handing the caller only the newest one. That removes the FIFO *staleness* half of the lag — you no longer detect on a frame that has been sitting behind others in the queue — but the caller still holds one buffer for the whole pipeline, so the back-pressure and the 1/pipeline_time throughput ceiling are unchanged. §3A is done for tinytag and still open for apriltag — see §3A and §5.
- Full-frame overlay copy + flush per detection, on the same core. [RESOLVED by the k230_osd module — see §4.] Historically each new detection triggered a whole-canvas ARGB memcpy (osd_frame→draw_buffer; ~3.6 MB at 720p, ~16 MB at 1080p) plus a full dcache_clean_invalid_range. That stole CPU/memory bandwidth from the (memory-heavy) detector. The triple-buffered k230_osd module removes the per-detection copy: the app draws into a CPU-owned plane buffer and the module retires buffers via the event-driven v4l2-drm path, keeping the draw local and the scanout-to-write "handoff" explicit.
- Sequential CV crop-decode on one core (tinytag). The ~7 ROIs are decoded one after another on the single core; it's the dominant cost. Only SIMD (RVV backend) can speed it up here — there's no second core to parallelize across.
- Extra software copy/rotate/allocate in portrait & debug/USB paths (cv::Mat temp_img + copyTo + cv::rotate every update; USB does VideoCapture.read + BGR→gray + opaque camera draw on the OSD). These are the remaining per-update frame-sized allocations/transfers on one CPU.



3. Opportunities (ranked)

A. Requeue the camera buffer earlier — overlap capture with compute. (Largest structural win, both apps.)
- tinytag: **DONE (b27c25e).** Implemented as described below: `copy_proposal_crops()` copies the ROI pixels, `v4l2_drm_dump_release()` runs immediately after, and `decode_proposal_crops()` then works on owned memory (main.cc:309-343). `--debug` reports the two new stages as `copy_roi_crops` and `capture_requeue`. The OSD is drawn after the release (main.cc:384), so no part of the overlay is inside the hold window. Original plan: after decode_proposals() (ROIs known, ~3–4 ms in) the raw frame is only needed to feed the CV crops, which are tiny. Copy just the few ROI crop regions (~KB) into owned storage and immediately v4l2_drm_dump_release(). The CV crop-decode then runs against owned memory while the next frame is already being captured — removing the ~9–16 ms buffer hold, decoupling throughput from CV-decode latency (that's exactly the "post_process outside the lock" class of fix, extended to the buffer).
- apriltag: **DONE, via a full-frame snapshot rather than the Stage-0 split.** The detector needs the original pixels through decode, so the planned "decimate/threshold into detector-owned storage" split would require a new detector C API. Instead the detect loop now takes one packed full-resolution copy of the Y plane into a persistent `owned_gray` and calls `v4l2_drm_dump_release()` immediately, before the debug dump, `apriltag_detect()` and the OSD. The denoise path pays nothing extra, since `filtered_gray` is already a private copy. `APRILTAG_DEMO_LATE_REQUEUE=1` restores the old hold-across-detect behaviour in the same binary for A/B.
  - **Measured: throughput unchanged** (detect ~9-10 fps, camera ~52-55 fps, `ok=1`, both paths, 720p). Expected — the detector is CPU-bound and the copy is ~1 ms. The win is freshness, and it is **bounded by DETECT_BUFFER_NUM**: the sensor produces a frame every ~19 ms while an iteration takes ~110 ms, so the driver fills every free buffer and then idles. Early requeue leaves it 3 free buffers instead of 2, i.e. it keeps capturing one buffer-period longer — roughly **19 ms less staleness**.
  - **Could not measure staleness directly**: the vvcam video driver returns `v4l2_buffer.timestamp == 0` even though its queue advertises `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` (`vvcam_video_register.c:1221`). A frame-age metric was written, found inert on-board, and removed. Fixing the driver to stamp buffers would make this whole class of latency work measurable and is worth doing before further latency tuning.
  - **Next, complementary:** reaching one-frame staleness needs `DETECT_BUFFER_NUM >= iteration_period / frame_period` (~6 at these rates) — currently 3 in both apps. Each extra 1280x720 NV12 buffer costs ~1.4 MB of CMA, so 3 -> 6 is ~4 MB. Untested; worth an experiment now that the requeue no longer wastes one of the buffers.
- apriltag, OSD hold: **DONE as a side effect of the above.** The single `v4l2_drm_dump_release()` used to sit after `k230_osd_begin()`/`draw_detections*()`/`k230_osd_publish()`, so the camera buffer was held across the overlay write too. In the CSI path that block touches no camera pixels — `draw_detections*()` takes the copied `detections` vector plus width/height ints, `draw_camera_frame()` is USB-only, and the debug-stage path reads detector-owned storage — so moving the release above the detector removed this as well. The canvas clear was already outside the window (`k230_osd_prepare()` runs before capture).

B. Draw the overlay directly into the OSD plane (kill the per-detection memcpy + shrink the flush).
- Write the quads/text into draw_buffer->map (the persistent plane mmap) in the producer and just thead_csi_dcache_clean_invalid_range it, instead of drawing into osd_frame and then memcpy-ing the whole canvas. Saves the full-frame transfer each detection and frees CPU/bandwidth for the detector.
- If direct-to-plane is awkward, at least do a dirty-rectangle update (the changed content is only a few small quads + a text line on an otherwise-transparent canvas), which slashes the copy and the cache flush cost.

**C. Reduce the per-frame sized /allocate/rotate work in the portrait/debug/USB paths.**
- Draw in panel orientation directly (like the existing draw_frame_lcd90cw/draw_detections_lcd_90cw fast path already does for the CSI-landscape case) so those modes skip the temp_img + rotate + extra copy.
- USB is inherently expensive (VideoCapture + cvtColor + opaque camera draw on the CPU); if USB isn't needed, CSI is the native zero-copy path.

D. Use the hardware offload engines to keep CPU free (already partly done for tinytag).
- tinytag already offloads resize (ai2d) and inference (KPU); the residual CPU cost is the CV crop-decode, whose per-core lever is the RVV crop-decode backend (2.1x on your input) — but close the recall gap first before enabling.
- apriltag has no NPU; it's pure CPU CCL. Its lever is (again) Stage-0 overlap (A) plus CCL kernel tuning (group_emit/direction passes) — but note the instrumented stage numbers are inflated; run the production ablation matrix to confirm which mask helps.
  - **Superseded in priority by §6 (2026-09-02).** `--rvv` measured live: **no change** (detect 8.9-9.9 fps, same as scalar), even though the detector-only benchmark puts RVV 12% ahead of scalar. The detector is not what binds the live app, so detector-side tuning is currently invisible. Revisit after the non-detector load is cut.

E. Buffer-allocation hygiene.
Most of this is already good (persistent ai2d tensor/builder, persistent osd_frame/draw_frame/filtered_gray, persistent plane mmap). The remaining per-update allocations are the temp_img in portrait/debug paths and the per-frame cv::Mats in USB mode. Fixing those is cheap and removes GC/allocation jitter on the one busy core.



In short: the architecture is sound and already does the right things (zero-copy camera→detector, camera on a hardware plane, persistent buffers, hardware resize/KPU). The OSD-side full-frame copy + flush was the second-biggest serialization point and is now gone via the k230_osd triple-buffered module (draw into a CPU-owned plane buffer; buffers retired on real page flips). The remaining wins are all about the single core. §3A (requeue the camera buffer earlier) is now done in both apps and turned out to be throughput-neutral — it buys freshness, not fps. Measurement since then (§6) shows the detector is no longer the bottleneck at all: the live app reaches only ~50% of the detector's standalone throughput, so the top item is now cutting the capture/display CPU load, not tuning CCL/RVV.

Next step I'd take: see §6, then §6.1 which measures it. The detector is no longer the bottleneck; live apriltag runs at ~50% of the detector's standalone throughput. §6.1 then isolated the display side and cleared it — capping submissions 4x moved detection by nothing — leaving the capture rate as the one app-level lever. So: a native 1280x720@30 sensor mode, together with the `--sensor-mode` flag, since §6.1 also showed the two apps want different capture rates.
╰───────────────────────────────────────────

---

## 4. OSD double-buffering (k230_osd) — progress & status (2026-09-01)

Goal: kill the per-detection `osd_frame -> plane` memcpy + full dcache flush,
double-buffer the OSD plane (no tearing/lag), pre-clear the back buffer so the
clear overlaps capture/detect, and do it as ONE shared `k230_osd` library both
apps link. Full detail: docs/notes/osd-doublebuffer-2026-09-01.md.

### What happened
- Built `package/k230_osd` (static lib) + wired both apps (tinytag
  first, then apriltag). Module built clean; tinytag built/links and the
  video-file regression held (185 detections @0.35 unchanged).
- Alignment fix: sizing the OSD plane to the CAMERA's 16:9 displayed region
  (not the full display) removed the pink bar + the detection offset.
- FAILURE: once the module was active in live view, the OSD became FLICKERY
  (both apps) and apriltag_demo SEGFAULTED (signal 11) in the detect thread.
  Root cause: the 2-buffer FRONT/BACK swap/recycle timing — the producer could
  draw into a buffer the scanout was still reading, and the deferred-clear
  path could deref a bad pointer. Both apps were reverted to the stable
  single-buffer OSD.

### The fix (committed, not yet validated on-board by us)
`1f5cc88 feat(osd): add update-driven triple-buffered tag overlays`
- `k230_osd` is now a TRIPLE-buffered (`K230_OSD_BUFFER_COUNT = 3`) state
  machine: `free -> front -> pending -> ready/drawing`, latest-wins.
  Producer NEVER writes `front`/`pending`; if it outruns vsync it repaints the
  uncommitted `ready` buffer and bumps `dropped_frames` instead of blocking.
  `publish` for `SLOW_ROTATE` rotates directly into the mapped destination (no
  temp/memcpy). Exposes `generation`, `displayed_generation`, `dropped_frames`
  telemetry.
- v4l2-drm library gained `v4l2_drm_run_event_driven` (additive; legacy path
  kept): tracks the outstanding atomic page flip and consumes its completion
  before the next commit, and retires OSD/LVGL buffers after their REAL flips
  — this is what makes the module's `pending -> front` promotion correct.
  Tag apps opt into the event-driven scheduler.
- Both apps moved to the shared OSD API + camera-region geometry + startup/
  teardown ordering; TinyTag FPS telemetry kept live during zero-detection
  intervals.

### Current state / next
- Commits: df0920e (nn_profile), 94d593b (tinytag crop + 0.35), 1f5cc88
  (OSD triple-buffer fix), + a follow-up that makes both apps `select
  BR2_PACKAGE_K230_OSD` in their Config.in so v3/01studio (and any board
  building the apps) auto-build the module (the main defconfig already had it
  enabled; without the select those two boards would have failed the build).
- The OSD-side opportunity (old §3 "draw directly into the plane / dirty
  regions") is now effectively DONE via the k230_osd module — that work is
  closed. The remaining, top-priority item is the capture-requeue split (§3A).
- TODO to validate on the board:
  - rebuild + deploy tinytag_detect and apriltag_demo (and the v4l2-drm lib
    change) and confirm: no segfault, OSD stable, **`dropped_frames` stays ~0**
    (i.e. the producer rarely outruns vsync), `generation`/`displayed_generation`
    advance, and the camera-region alignment holds. (This is being done by the
    user on the respective boards; can't be verified here.)
  - the capture-requeue split (Section 3A) is the next big structural win,
    independent of the OSD work. And 01studio has an `S60apriltagkey` init
    daemon that launches/cycles both apps from the KEY button (gpiochip0 line
    21) at boot — relevant when validating on an 01studio board (note it
    launches tinytag at the 0.20 threshold, not 0.35).


---

## 5. Camera mode & capture rate (2026-09-02)

Two camera-side changes landed since this note was written: the newest-buffer
dequeue (`b27c25e`, folded into §2 above) and a native OV5647 1280x720@60
sensor mode, whose broken ISP profile was fixed in `f717dcb`. Full detail:
docs/notes/ov5647-720p-mode.md.

### The measurement that matters here

Both tag apps now prefer the native 720p60 mode over 1080p30. The detector
sees a 1280x720 image either way (in 1080p mode the ISP downscales), same
scene, same detector:

| sensor mode | camera | display | **detect** |
|---|---|---|---|
| native 720p@60 | ~53 fps | ~60 fps | **~9 fps** |
| 1080p@30 (ISP downscale) | ~28 fps | ~42 fps | **~15 fps** |

Detection is **slower** at 720p, and it is not because the detector does more
work: quad counts per frame are comparable (q ~ 13-26 vs 22-26). It is the
single core. `/proc/stat` showed **exactly zero idle ticks** over a 5-second
sample during a 720p run, so the doubled capture and display work comes
directly out of the detection thread's budget.

### Why this sharpens §3A rather than competing with it

This is the "Critical context" section's thesis measured directly: on one core,
*any* rate increase upstream is paid for downstream. The 720p mode buys
freshness (16.7 ms readout instead of 33.3 ms) and spends detection throughput
to get it, because the detector still holds the capture buffer for the whole
pipeline and is scheduled against a 60 fps capture/display load.

The capture-requeue split (§3A) is what would let the mode deliver both: once
the buffer is released early, capture stops being paced by the detector and the
extra camera rate turns into lower latency instead of contention. **tinytag
already has this** (b27c25e) and **apriltag now does too** (see §3A) — though
measurement showed the split is throughput-neutral, since the detector is
CPU-bound; it buys freshness, bounded by the capture buffer count. So the fps
table above still stands: treat 720p-vs-1080p as an explicit
latency-vs-throughput choice.

**Follow-up (§6):** that choice is avoidable. The 60 fps capture rate, not the
resolution, is what costs the detection throughput here, so a native 720p@30
mode would keep the freshness benefit at half the capture load. See §6.1.

Note there is currently **no runtime switch** between the two — the
preferred/fallback pair is hardcoded at both call sites (`apriltag_demo`
main.cc:845, `tinytag_detect` main.cc:652), and `--csi-size` does *not* select
the sensor mode. A `--sensor-mode` flag would make this trade-off measurable
without swapping libraries, which is how the numbers above had to be obtained.


---

## 6. The detector is no longer the bottleneck (2026-09-02) — SUPERSEDED

> **Read §6.2 first.** The central claim below — that ~50% of the core goes to
> non-detection work — is a benchmarking artifact: the live throughput was
> compared against a benchmark run on `fixture.jpg`, a much easier image than
> any real scene. Measured on the actual scene the overhead is ~11%, and the
> detector *is* still the bottleneck. The ranking in this section is wrong;
> §6.2 replaces it. Left in place because the raw numbers are still valid for
> the input they were taken on.

Measured with the on-board detector-only benchmark (`/root/app/apriltag_bench`,
`fixture.jpg`, 1280x720, factor 2, 15 iterations x 10 batches, warmup 3) — no
capture, no display, no OSD:

| backend | mean latency | throughput |
|---|---|---|
| Rust RVV | 47.6 ms | **21.0 fps** |
| Rust scalar | 53.3 ms | **18.8 fps** |
| C reference | 69.7 ms | 14.4 fps |

The live app, same input size and factor, runs at **~9.4 fps**.

**Live throughput is half the detector's standalone rate.** Roughly 50% of the
single core goes to everything except detection: the display thread's ~60 fps
poll/atomic-commit/buffer-recycle loop, the detect context capturing at 60 fps
and discarding ~5 of every 6 frames, the ISP writing those discarded NV12
frames to DDR, the OSD draw/publish, and the §3A snapshot copy.

This also explains a null result that would otherwise be puzzling: `--rvv` is
12% faster than scalar in the benchmark but makes **no difference live**
(8.9-9.9 fps either way). The detector has ~9 fps of headroom it cannot use.

### Ranked from here

1. ~~**Cap the display rate.**~~ **DONE and MEASURED — it does not recover
   detection throughput. See §6.1.** The reasoning below was that the ~60 fps
   flip loop was stealing the detector's budget. It is not: detect is flat at
   ~9.9 fps whether the display submits at 60, 30 or 15 fps. The freed CPU goes
   to the *capture* path instead. Kept on at 30 fps anyway for the display-side
   savings, but it is not the throughput lever. Original reasoning, for the
   record: "The display thread flips at ~60 fps while detections change at
   ~9 fps; the camera image is on a hardware plane and the OSD only updates at
   detection rate, so most of those poll/atomic-commit/buffer-recycle cycles are
   CPU overhead for no visible benefit."
2. **The capture side: a native 1280x720@30 sensor mode. This is now the
   load-bearing item, not merely the next one** — §6.1 isolated the display
   thread and cleared it, and showed the capture path absorbs any CPU freed
   upstream of it. §5 already
   priced 60 fps capture: 1080p30 yields 15 fps detect, 720p60 yields 9 fps for
   the same 1280x720 detector input — i.e. the app runs at 80% of standalone
   detector throughput at 30 fps capture but only 50% at 60 fps. That is ~40% of
   detection throughput spent on freshness that §3A bounded at ~19 ms. A 720p30
   mode keeps the native binned readout (no ISP downscale) at half the capture
   load. It also **doubles the exposure ceiling**, which should cut the analog
   gain the AE currently rails at (14.7-24x) and with it the sensor noise — see
   docs/notes/ov5647-720p-mode.md §8.

   **The dispatch path was checked and is clean — no new tuning assets are
   needed.** `vvcam_sensor_find_mode()` (`vvcam/src/capabilities.c:36`) matches
   on width **+ height + fps**, so two 1280x720 modes disambiguate correctly;
   and `v4l2_drm_select_scene_profile()` is keyed on resolution only, so a
   720p30 mode reuses the existing 720p ISP profile that `f717dcb` fixed. The
   edit is a duplicated `modes[]` entry at `vvcam/src/ov5647.c:411`:
   - VTS `0x380e`/`0x380f` 851 -> 1702 (`0x06`,`0xa6`) in the register table,
   - `frame_length` / `cur_frame_length` 851 -> 1702,
   - the four `max_*_integraion_line` / `max_*_integraion_time` bounds
     re-derived from `1702 - 12`,
   - `cur_fps` 60 -> 30.

   **Do it together with `--sensor-mode`, which §6.1 promoted from nicety to
   requirement.** The preferred/fallback pair is hardcoded at both call sites
   (`apriltag_demo` main.cc:845, `tinytag_detect` main.cc:652) and `--csi-size`
   is *not* that switch. Two reasons it is now mandatory: the A/B is otherwise
   unmeasurable without swapping libraries, and §6.1 showed the two apps want
   *different* capture rates (apriltag 30, tinytag 60), so a single hardcoded
   default cannot serve both.
3. **Then detector work.** RVV (+12% standalone) and factor/min-blob tuning
   become worth enabling once the contention above is removed. Low priority:
   this ground has been covered before, and §6.1 reconfirms that detector-side
   gains stay invisible while the system-level contention is unresolved. Keep
   the focus system-level for now.

**Deliberately not next:** raising `DETECT_BUFFER_NUM`/`BUFFER_NUM` from 3. It
buys freshness only, and at 30 fps capture the bound changes anyway —
iteration_period / frame_period drops from ~6 to ~3, which 3 buffers already
covers.

**Prerequisite for any latency claim:** the vvcam video driver returns
`v4l2_buffer.timestamp == 0` despite advertising
`V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` (`vvcam_video_register.c:1221`), so
staleness cannot be measured, only derived. Fix that before tuning latency
further.


---

## 6.1 The display cap, measured — and the two apps are in different regimes (2026-09-03)

§6 ranked "cap the display rate" first. It is now implemented, deployed and
measured on real hardware. **The hypothesis was wrong for apriltag_demo and
actively harmful for tinytag_detect**, and the measurement relocates the
bottleneck. Read this before acting on §6's list.

### What landed

`89c0250` capped HDMI only (`display_is_hdmi(...) ? 30 : 0`), which made it
**inert on every LCD board** — the LCD-first DTB has no HDMI connector at all,
so `display_is_hdmi()` is false and both halves of that commit were switched
off. Both apps now take a `--display-fps N` flag instead (`0` = uncapped, which
also restores the pre-cap connector-mode choice, i.e. a true A/B baseline in one
binary), applied to **every** connector. The panel's fixed timing cannot be
renegotiated, but `max_display_fps` throttles the atomic submissions themselves
inside `v4l2_drm_run_impl`, independently of scanout rate — that is where the
CPU goes.

### Test bed

`k230_canmv_v3_defconfig` board, ST7701 480x800 DSI panel, native 720p60 sensor
mode, one stationary scene with one visible tag, same scene for every run.
12 s warm-up, then a 5 s `/proc/stat` cpu0 window and 8 s of the once-a-second
telemetry line. apriltag runs `--rvv --factor 2`; tinytag runs the production
operating point `0.35 20 1.5 0`.

**Both boards share this panel.** `k230-canmv-v3-lcd.dts` and
`k230-canmv-01studio-lcd.dts` both `#include "display-st7701-480x800.dtsi"` —
identical timing (39.6 MHz / htotal 600 / vtotal 1100 = **exactly 60.00 Hz**),
identical init sequence. They differ only in reset/touch GPIO line numbers and
01studio's button/LED/uart1/spi0 nodes. So these numbers carry to 01studio.

### apriltag_demo: the cap changes nothing that matters

| `--display-fps` | display | camera | detect | osd staged | osd drop | cpu0 idle / 5 s |
|---|---|---|---|---|---|---|
| 0 (baseline) | 59.9 | 52.4 | **9.9** | 9.9 | 0 | **0** / 504 |
| 30 | 27.8 | 56.4 | **9.9** | 9.9 | 1, constant | **0** / 503 |
| 15 | 14.1 | 56.4 | **9.9** | 6.4 | +4/s, growing | **0** / 503 |

1. **Detection throughput does not move.** Flat at ~9.9 fps across a 4x change
   in display rate. The ~60 fps flip loop was not stealing the detector's
   budget. §6 item 1's premise is falsified.
2. **The freed CPU goes to capture, not detection.** Camera rises 52.4 -> 56.4
   fps and then saturates — halving the display again (30 -> 15) buys no further
   camera fps. The capture path takes whatever the core gives it, up to the
   60 fps sensor rate, and those extra frames are *discarded* (the detector
   takes only the newest buffer).
3. **cpu0 idle is exactly zero in all three.** The core never idles; the work
   only redistributes.
4. **30 is the safe floor, 15 is not.** At 15 the OSD producer (~10 fps)
   outruns the retire path: staged overlays fall to 6.4 fps and
   `dropped_frames` climbs steadily at ~4/s. At 30, drop is one startup event
   and then constant.

Kept at 30 by default for apriltag: it halves display-side work at zero cost to
detection, and lets capture run ~8% closer to the sensor rate. But it is **not**
a throughput win and must not be sold as one — it does not touch the gap
between the detector's 21 fps standalone (§6) and the app's 9.9 fps live.

### tinytag_detect: fast enough at 60 fps capture, and the cap hurts it

| `--display-fps` | display | camera | **AI** | osd staged | osd drop | cpu0 idle / 5 s |
|---|---|---|---|---|---|---|
| 0 (baseline) | 60.0 | 46.7 | **~35** | 24.4 | +10/s | **60** / 500 (12%) |
| 30 | 27.9 | 56.4 | **~36.7** | 13.9 | +24/s | 62 / 499 (12%) |

This app is **not** in the same regime as apriltag and should not inherit its
conclusions:

- **It is fast enough at 60 fps capture.** ~35 fps AI *with ~12% cpu0 idle to
  spare*. The core is not saturated. Contrast apriltag, which is pinned at 0%
  idle and 9.9 fps.
- **Capping to 30 costs overlay quality and buys nothing.** AI throughput and
  idle are unchanged (both within noise), but staged overlays fall 24 -> 14 fps
  and `dropped_frames` more than doubles to ~24/s. The AI loop produces
  overlays at ~35 fps, which is *above* a 30 fps cap, so the cap throttles the
  producer's output. apriltag detects at ~10 fps, comfortably under 30, which
  is the whole reason the same knob is harmless there.

So the two apps ship **different defaults, for a measured reason**: apriltag
`--display-fps 30`, tinytag uncapped. The rule is "cap above the producer's
rate, or not at all."

### HDMI is still unverified, deliberately

`89c0250`'s HDMI-specific half — preferring a connector mode at or below 30 Hz
— **cannot be tested on any LCD board**, because the LCD-first DTB exposes no
HDMI connector (`/sys/class/drm/` shows only `card0-DSI-1`). It is left in
place, untested, and should be described that way until someone runs it.

Two things make that an acceptable place to leave it. First, §6.1 gives no
reason to expect HDMI to behave differently in kind: if halving submissions
does not buy detection throughput on a 60 Hz panel, it will not on a 60 Hz
monitor either. Second, it *is* reachable on this hardware if it ever matters —
`/boot/k230-canmv-v3.dtb` (the non-LCD DTB) is built and present alongside the
LCD one, so re-pointing `k.dtb` and rebooting gives HDMI on the same board, at
the cost of the LCD. Not worth a reboot now: LCD is the configuration 01studio
ships and the one both apps are validated on.

### The measurement nobody has taken: headless

Everything in §6.1 and most of §6 measures a configuration that **production
does not use**. In production there is no display at all — the output is the
detected tag and a camera pose, not a preview. So the entire display thread
(the ~60 fps poll / atomic-commit / buffer-recycle loop), the OSD draw and the
OSD publish are development-only costs that the shipping configuration does not
pay.

That reframes the OSD-drop item below as a preview-quality concern only, and it
means **the real production budget for the detector is unmeasured.** Neither
app currently has a live-camera-without-display mode, so this needs a small
`--no-display` path before it can be measured at all. It should be measured
before more effort goes into §6 item 2: if headless apriltag lands near the
detector's 21 fps standalone, the case for halving the capture rate weakens
considerably — and if it does not, that isolates the remaining cost to capture,
which is exactly what item 2 targets.

### Loose end this exposed

tinytag's `dropped_frames` climbs at ~10/s **even uncapped** (AI ~35 fps, staged
overlays ~24 fps — roughly a third of overlays never reach the screen). §4's
on-board validation criterion was "`dropped_frames` stays ~0"; that holds for
apriltag (0-1) but **not** for tinytag. Latest-wins means this is not a
correctness bug, but it was not known and is not yet explained.

**Low priority, though:** this is a preview-quality issue only, and production
is headless (see above). Dropped overlays cost nothing the shipping
configuration cares about. Worth knowing before trusting OSD-rate numbers from
that app; not worth chasing on its own.

### What this does to the ranking

- §6 item 1 is **done and closed as a null result.** Keep the flag; stop
  expecting throughput from it.
- §6 item 2 (native 720p@30) is **promoted to load-bearing, and its mechanism
  is now confirmed rather than inferred.** The capture path is demand-driven
  and absorbs any CPU freed upstream of it, so the only way to convert that CPU
  into detection throughput is to stop producing the frames. This also predicts
  the result: §5 measured 1080p30 at 15 fps detect vs 720p60 at 9 fps for the
  same detector input, so 720p30 should land near 15 fps.
- **But 720p@30 is an apriltag-only win, and the sensor mode is global.**
  tinytag's AI runs at ~35 fps; a 30 fps capture would cap it at 30 and cost it
  ~15% for nothing, since it already has idle headroom at 60. This makes the
  `--sensor-mode` flag (see docs/notes/ov5647-720p-mode.md §2) a **requirement,
  not a nicety** — the two apps want different capture rates, and the mode is
  currently hardcoded at both call sites.
- §6 item 3 (detector tuning) stays last and is explicitly low-value: this
  ground was covered before, and §6.1 reconfirms that detector-side gains are
  invisible while system-level contention is unresolved.

### Reproducing

Both apps ship a `run.sh` with the standard settings (installed on a fresh
flash; extra arguments are appended and override the defaults):

```sh
/root/app/apriltag_demo/run.sh  --display-fps 0    # uncapped A/B baseline
/root/app/tinytag_detect/run.sh --display-fps 30   # cap, for comparison
```

The once-a-second stderr line (`poll/display/camera/detect|AI/osd/drop`) prints
without `--debug` and is the whole measurement surface; tinytag prints it only
for intervals that had a confirmed detection. Pair it with cpu0 idle ticks from
`/proc/stat` — the board has no `timeout` or `pkill`, only `killall`.

---

## 6.2 Headless measured — §6's central claim was a benchmarking artifact (2026-09-03)

**§6 said "roughly 50% of the single core goes to everything except
detection." That is wrong. The real figure is ~11%.** The 50% came from
comparing the live app's throughput against a benchmark run on
`fixture.jpg` — a *different, much easier image*. Corrected below. This
supersedes §6's ranking and most of what §6.1 concluded from it.

Both apps now take `--no-display` (no DRM output, no OSD, no display thread,
no second capture context) — the shape production actually runs in. Each
prints a per-second line with detect/AI fps **and tag counts**, because
whether a tag is present changes per-frame cost and two runs are not
comparable without it.

### The measurement that settles it

A live camera frame was dumped from the board (`APRILTAG_DEMO_DUMP`, packed
Y8) and fed to the same standalone benchmark that produced §6's numbers. Same
binary, same board, same detector settings (factor 2, min_blob 25) — the only
change is the input image:

| backend | `fixture.jpg` | **live scene** |
|---|---|---|
| Rust RVV | 47.6 ms / 21.0 fps | **85.7 ms / 11.7 fps** |
| Rust scalar | 53.3 ms / 18.8 fps | 93.4 ms / 10.7 fps |
| C reference | 69.7 ms / 14.4 fps | 104.9 ms / 9.5 fps |

**The real scene is 1.8x more expensive for the detector than the fixture.**
`fixture.jpg` contains 11 clean tags; the live scene contains 1 tag plus a
cluttered background that generates far more candidate work.

Now compare like with like:

- detector alone, live scene, no capture, no display: **85.7 ms**
- full headless app on that same scene: **~95 ms** (10.5 fps)

**Everything that is not the detector costs ~9-10 ms per frame, about 11%.**
Not 50%.

### Headless vs display, both apps

| config | detect / AI | camera (dequeued) | cpu0 idle / 5 s |
|---|---|---|---|
| apriltag, display @30 | 9.9 | 56.4 | 0% |
| apriltag, **headless** | **10.5** | 21.1 | 0% |
| tinytag, display @30 | ~36.7 | 56.4 | 12% |
| tinytag, **headless** | **28.3** | 28.3 | **73%** |

Two things fall out:

1. **Removing the entire display path buys apriltag ~4%** (9.9 -> 10.5 fps).
   The display thread, the OSD draw/publish, the DRM flips *and* the second
   1280x720 capture context together were worth half a frame per second.
   Consistent with §6.1, where a 4x cut in display submissions moved detection
   by nothing at all.
2. **tinytag headless is capture-limited, not compute-limited.** `camera` and
   `AI` are *exactly equal* (28.3 = 28.3, one dequeue per iteration) and the
   core sits **73% idle**. The loop is blocked in `poll()` waiting for frames.
   Its AI rate is lower than the 36 fps it shows with a display precisely
   because there is no longer a second capture context pulling the ISP along.

### The detect node does not deliver 60 fps

§6 assumed "the detect context capturing at 60 fps and discarding ~5 of every
6 frames." The dequeue counter added to `v4l2_drm_dump_latest()` says
otherwise: the detect/AI node delivers roughly **28-30 fps**, not 60.

tinytag's numbers are the strong evidence — one dequeue per iteration with 73%
idle means the app consumed every frame offered and then waited. (apriltag's
21.1 dequeues/s is weaker evidence on its own, since with 3 buffers and ~95 ms
iterations the driver would also be dropping frames for want of a free buffer.)

The ~60 fps capture load is on the **display** context, which production does
not have.

### Consequences — this re-ranks everything

1. **§6 item 2 (native 720p@30) is largely dead as a production lever.** It
   was aimed at halving a 60 fps capture load. In the headless configuration
   that load is already ~30 fps on the detect node, and the 60 fps consumer is
   the display context that production does not run. What remains is a
   dev-preview improvement and the exposure/noise benefit
   (ov5647-720p-mode.md §8) — both real, neither a throughput lever. **Do not
   start this expecting apriltag throughput.**
2. **Detector work is now the top lever for apriltag, not the bottom.** It is
   85.7 of the ~95 ms. §6 deprioritized it on the grounds that detector gains
   were "invisible live"; that conclusion came from the same artifact.
3. **Evaluate detector work on dumped live frames, never on `fixture.jpg`.**
   The fixture is 1.8x too optimistic and ranks backends differently in
   magnitude. Dumping is one flag: run with `--debug --no-display` and
   `APRILTAG_DEMO_DUMP=/tmp/live.gray`, press `q`, then feed that file to
   `k230_apriltag_bench --format raw --size 1280x720`.
4. **RVV does help — it was hidden in noise.** On the live scene RVV is 9%
   faster than scalar (85.7 vs 93.4 ms). Live that is ~10.5 vs ~9.7 fps, which
   is inside the 8.9-9.9 fps band §6 was eyeballing when it concluded "no
   difference". Keep `--rvv` on.
5. **tinytag has no throughput problem in production.** 28.3 fps with 73% of
   the core idle, limited by frame delivery. Nothing in §6 or §6.1 helps it,
   and nothing needs to.

### New open question, worth more than item 2 — ANSWERED, see §6.3

**Why does the detect node deliver only ~28-30 fps when the sensor runs at
60?** Answered in §6.3: the node was **buffer-starved**, not rate-limited. Both
apps requested 3 capture buffers; the ISP fills every free buffer and stalls
until one is returned, capping delivery near `buffer_num - 1` per iteration.
Raising both apps to 6 buffers took tinytag headless from 28.3 to **56.6 fps**
— the doubling predicted here, for ~4 MB of CMA.

---

## 6.3 Answered: the detect node was buffer-starved, not rate-limited (2026-09-03)

§6.2 closed with "why does the detect node deliver only ~28-30 fps when the
sensor runs at 60?" and called it worth more than anything left in §6. It was.

**The ISP fills every free capture buffer and then stalls until one is
returned.** Delivery is therefore capped at roughly `buffer_num - 1` frames per
loop iteration. Both apps requested **3** buffers, which is what produced the
28-30 fps. Nothing was rate-limiting the node; it was starved.

### Result: raising both apps to 6 buffers

| app / config | before (3 bufs) | after (6 bufs) |
|---|---|---|
| **tinytag headless** | 28.3 fps AI, 73% idle | **56.6 fps AI**, 37% idle |
| apriltag headless | 9.19 fps detect, camera 18.4 | 10.49 fps detect, camera 51.5 |
| apriltag w/ display | 9.9 fps detect, drop 0 | 9.91 fps detect, drop 0 |

**tinytag's production throughput doubles** and it now consumes every frame the
sensor produces — with 37% of the core still idle. apriltag gains ~14% on
detect and ~2.8x on capture freshness. Neither display configuration regressed.

Cost: 3 extra 1280x720 NV12 buffers at ~1.4 MB each, ~4 MB per context, against
~400 MB `CmaFree`.

### The sweeps

Headless, v3 board, 720p60 sensor, same scene:

| bufs | apriltag camera / detect | tinytag camera / AI |
|---|---|---|
| 3 | 18.4 / 9.19 | 28.3 / 28.3 |
| 4 | 28.2 / 9.40 | 27.9 / 27.9 |
| 6 | 52.3 / 10.45 | 56.2 / 55.2 |
| 8 | 56.0 / 10.62 | 56.4 / 56.4 |
| 12 | 56.2 / 10.47 | 56.4 / 56.4 |

apriltag's camera column is an exact fit for `(buffer_num - 1)` frames per
iteration until it saturates at the sensor rate: 2/109 ms = 18.4, 3/106 ms =
28.2, 5/96 ms = 52.3. **6 is the knee for both apps**; beyond it, nothing
changes.

Note this is precisely the mechanism the §3A comment in `apriltag_demo`
main.cc already described — "the driver fills every free buffer and then has
nowhere to put frames until one is returned" — and §3A even predicted the
buffer count needed (~6). It was never connected to the observed frame rate,
and §6 went on to assume the node was delivering 60 fps.

### What was ruled out on the way

Worth recording so none of it is re-investigated:

- **The sensor.** Read over I2C (OV5647 at 0x36 on i2c-0) *while streaming*:
  `0x3036=0x6e` (60 fps PLL), HTS `0x0704`=1796, **VTS `0x0353`=851,
  unextended**. So AE was not stretching the frame length; the sensor really
  produces 60 fps. Exposure 509 lines, analog gain 0x121 = 18.1x (dim scene,
  matching ov5647-720p-mode.md §8).
- **A per-channel rate cap.** Streamed each ISP channel standalone with
  `v4l2-ctl`: video1, video2 and video3 all deliver **46.8 fps**. No channel is
  privileged. (video1 = `vvcam-video.0.0` = ISP pad1 = the display node;
  video2 = pad2 = the detect node.)
- **Buffer count, in the wrong regime.** `v4l2-ctl --stream-mmap=N` for N in
  2,3,4,6,8 gives 46.8 fps throughout — which is why buffer count looked
  irrelevant at first. `v4l2-ctl` requeues immediately; the apps *hold* a
  buffer across the loop body. Buffer depth only matters for a holding
  consumer.
- **`isp_media_server` being CPU-starved.** Sampled its `utime+stime`: 0.2%
  idle, 9.5% under a v4l2-ctl stream, 6.8% under tinytag headless, 10.5% under
  apriltag headless. It never approaches saturation, and the tinytag case had
  75% of the core free anyway.
- **CPU / memory-bandwidth contention.** Running `k230_apriltag_bench` (full
  core, memory-heavy, no V4L2 at all) alongside a v4l2-ctl stream moved
  delivery only 46.8 -> 43.5 fps, and it recovered to 47.0 after. A 7% effect,
  nowhere near the 2x being explained.

Also confirmed in passing: `VIDIOC_G_PARM`/`S_PARM` are **commented out** of
`vvcam_video_ioctl_ops` (`vvcam_video_register.c:966`), so both apps' `G_PARM`
calls have always failed silently — the frame-interval logging they feed is
dead code. And `v4l2-ctl` reports `0.00 fps` for every stream, which is the
same `timestamp == 0` driver bug §3A hit; external wall-clock timing is the
only way to measure rate here.

### Consequences

1. **tinytag needs nothing further.** 56.6 fps headless at the sensor rate with
   37% idle. It is now sensor-limited, which is the correct place to stop.
2. **§6 item 2 (native 720p@30) is now clearly wrong for both apps, not just
   arguably.** It would halve the frame rate tinytag just gained. Keep it only
   as an exposure/noise option (ov5647-720p-mode.md §8), never as a throughput
   change.
3. **apriltag is unchanged in character** — still CPU-bound at 0% idle, still
   ~85.7 ms of genuine detector work per §6.2. Buffers bought it freshness and
   ~14%, not a fix.
4. **Re-check DETECT_BUFFER_NUM whenever the loop period changes.** The knee is
   `iteration_period x sensor_rate`, so a faster detector needs *fewer* buffers
   and a slower one needs more. 6 covers both apps at 720p60 today.

---

## 7. The second core: what §6 implies for the AMP plan (2026-09-02)

Everything in §6 redistributes one core. The second core changes how many
there are, and §6 sized that prize: the live app reaches ~50% of the
detector's standalone throughput, so giving the compute a core to itself is
worth roughly **2x** — more than the rest of this note combined.

**This is already planned work, not a new idea.** See
`docs/amp_bigcore_rvv_plan.md` and `docs/k230_amp_payload_slots.md`. This
section only records what §6's measurements add to them, and two constraints
that a perf-motivated reading should not trip over.

### What §6 contributes

A concrete value case. Those documents motivate AMP mainly by RVV capability
and architecture; §6 supplies the throughput argument, measured: apriltag's
detector needs 47.6 ms/frame and gets ~106 ms of wall clock, because the
capture, display and OSD work is timesharing the same core. That is the gap a
second slot would close.

**§6.1 sharpens this considerably, in AMP's favour.** Two findings matter:

1. **The contention is not divisible from user space.** §6.1 cut display
   submissions by 4x and apriltag's detect rate did not move by a single frame,
   because the capture path immediately absorbed the freed CPU. cpu0 idle stayed
   at *exactly zero* through every variant. Redistributing work on one core is
   not producing wins; the remaining app-level lever (§6 item 2) works only by
   *destroying* capture work — accepting half the frame rate — rather than by
   finding spare capacity. A second slot is the only option on the table that
   adds capacity instead of trading it.
2. **The two apps now want conflicting global settings.** apriltag wants 30 fps
   capture (it is core-bound at 0% idle); tinytag wants 60 (it has ~12% idle at
   ~35 fps AI and loses throughput at 30). On one core with one sensor mode
   those cannot both be satisfied. That is a scheduling problem a second slot
   dissolves.

**The value estimate for apriltag survives §6.2 and is arguably strengthened.**
§6.2 shows the ~95 ms apriltag spends per frame is ~85.7 ms of *real detector
compute* on a realistic scene, not contention — and detector compute is exactly
what parallelizes frame-to-frame across two slots. The 2x is a genuine compute
doubling, not a contention recovery.

**The two apps differ, and §6.2 changed which way** — an earlier draft of this
section claimed the payoff was concentrated in apriltag because tinytag has
~12% idle. That was wrong: "has some idle" is not "cannot go faster". A second
slot raises *aggregate* throughput regardless of how busy either core is:

- **apriltag** is core-starved (0% idle, 9.9 fps live vs 47.6 ms/frame
  standalone). It gains on both latency and throughput.
- **tinytag gains nothing from a second core, and §6.3 closes the argument.**
  It now runs headless at **56.6 fps — the full sensor rate — with 37% of the
  core still idle.** It is sensor-limited, and a second core cannot produce
  frames. (§6.2 reached the same conclusion for a different reason, when it was
  still capture-starved at 28.3 fps; fixing that starvation in §6.3 did not
  change the verdict, it strengthened it.)

**But the RVV constraint decides the shape of the split, and it differs per
app.** The small core has no vector unit and the AI stack (libnncase, ai2d,
KPU) is RVV-only, so *symmetric* frame ping-pong is available to apriltag and
not to tinytag:

- **Symmetric frame ping-pong is the wrong shape, because the cores are not
  symmetric.** The small core runs at ~800 MHz against the big core's 1.6 GHz
  (`cpu.c:83` sets "big core 1.6G"), so alternating frames between them
  produces a badly unbalanced pair — the slow half gates the pipeline — *and*
  the small core has no vector unit, so it would additionally be running the
  scalar detector path. Two penalties stacked.
- **Asymmetric allocation is the right shape**: put the headless production
  pipeline on the big core (vector, full clock) and push display/OSD/preview
  work to the small core. Those are non-vector by nature and latency-tolerant,
  which matches what the small core is.
  **But size it against §6.2 before building it:** headless measurement puts
  the entire display+OSD+second-capture path at ~9-10 ms of a ~95 ms apriltag
  frame, about **11%**. Moving it to the other core recovers that 11%, not the
  50% §6 implied. The remaining ~85.7 ms is real detector compute on a
  realistic scene, and *that* is what a second slot has to attack to be worth
  the effort — which means running a detector instance there, back to the
  vector/clock problem above.
- **tinytag would split by *stage*, not by frame**, if it needed splitting at
  all (§6.2 says it does not — it is capture-limited with 73% idle). The
  boundary exists if ever wanted: §3A's `copy_proposal_crops()` already copies
  ROI pixels into owned storage, and the C crop decoder is scalar-capable.

**Verified, since it gates all of the above:** the RVV dependency of the AI
stack is a *software packaging* constraint, not silicon. The KPU and ai2d are
separate hardware blocks reached through a kernel driver; nothing ties them to
a particular hart. What ties them is that `package/libnncase` **downloads a
prebuilt binary release** (`nncase_k230_v2.11.0_runtime_linux.tgz` from
GitHub) rather than building from source, and all three archives in it report
`rv64i..._v1p0_..._zvfh1p0_zvl128b1p0` — vector, RVV 1.0. So as shipped they
cannot execute on a non-vector core. nncase itself is open source, so a
non-vector rebuild is *conceivable*, but it means replacing the release-tarball
package with a from-source build, and the `zve*`/`zvfh` extensions in that
attribute string say the runtime's CPU-side kernels are written for vector —
a scalar build would be slower and may not be a configuration upstream
supports. Treat it as a real but non-trivial escape hatch, not a checkbox.

Unquantified: shared KPU/ai2d duty (~4 ms of a ~28 ms tinytag frame, so
contention is real but not blocking) and shared DDR bandwidth.

### Two constraints on the "infrastructure on the small core" split

The natural perf framing — put camera capture, OSD draw, display flip, ai2d
setup and KPU launch on one core and leave the heavy compute on the other —
runs into two facts already established in those docs:

- **The small core has no vector unit,** and the AI/NN stack (libnncase,
  AI2D_KPU) is RVV-only. So `ai2d` setup and KPU launch specifically *cannot*
  move to the small core; they have to stay wherever the vector core is. The
  non-vector infrastructure (V4L2 dequeue, DRM flips, OSD drawing) is the part
  that could move.
- **Userspace on `dev` is built vector-wide** (`-mcpu=c908v
  -mrvv-auto-vectorize`), so nothing currently built can execute on a
  non-vector core. A small-core slot needs its own non-vector sysroot, not
  just a scheduling decision.

### And SMP is not the mechanism

`docs/k230_amp_payload_slots.md` lists SMP as an explicit **non-goal**:
inter-core cache coherency is unresolved pending the TRM, and K230's two CPU
subsystems are not a conventional SMP pair. Consistent with that, the
small-core branch logged `CPU1: failed to come online`, and `dev`'s device
tree describes a single hart on purpose. So the way to a second core here is
an AMP payload slot with explicitly flushed shared buffers — not booting Linux
SMP across both harts. A perf-driven attempt to "just enable the second CPU"
would be rediscovering a settled question.

### Verified on k230_canmv_v3 while measuring §6

Consistent with those docs, and worth having in one place:

- `/sys/devices/system/cpu/possible` is `0` — one hart, by DT design
  (`k230.dtsi` declares only `cpu@0`), not a core sitting offline.
- The kernel is built SMP (`Linux version 6.6.36 ... #2 SMP`), so the config
  is not what bounds this.
- `/proc/cpuinfo` reports `hart : 0` with ISA `rv64imafdcv...`. Per
  `k230_amp_payload_slots.md`, the big RVV core *is* architectural hart 0, so
  this confirms `dev` runs Linux on the **big core with vector** — which is
  also why §6's RVV benchmark beats scalar. (Note for anyone carrying the
  older assumption: on `dev` this is *not* the no-vector small core.)

### Sequencing

Independent of §6. The §6 items are app-level, cheap and measurable today; the
AMP work is a boot/firmware project gated by an open question that one serial
console boot would settle (does the small core report `mhartid == 0` or `1`?).
Do §6 first regardless — a second core is not a reason to keep spending ~40%
of the first one on frames that get discarded.
