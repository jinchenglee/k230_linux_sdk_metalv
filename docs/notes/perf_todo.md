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

- Capture back-pressure (biggest serialization point, both apps). Because the V4L2 buffer is held for the entire pipeline (detect or AI+CV decode), the 3-buffer capture queue can't recycle, so the camera is paced at the processing rate. Throughput is ≈ 1 / pipeline_time, and the held buffer adds a full pipeline-latency lag. The comment in apriltag main.cc:349-354 names this directly.
- Full-frame overlay copy + flush per detection, on the same core. [RESOLVED by the k230_osd module — see §4.] Historically each new detection triggered a whole-canvas ARGB memcpy (osd_frame→draw_buffer; ~3.6 MB at 720p, ~16 MB at 1080p) plus a full dcache_clean_invalid_range. That stole CPU/memory bandwidth from the (memory-heavy) detector. The triple-buffered k230_osd module removes the per-detection copy: the app draws into a CPU-owned plane buffer and the module retires buffers via the event-driven v4l2-drm path, keeping the draw local and the scanout-to-write "handoff" explicit.
- Sequential CV crop-decode on one core (tinytag). The ~7 ROIs are decoded one after another on the single core; it's the dominant cost. Only SIMD (RVV backend) can speed it up here — there's no second core to parallelize across.
- Extra software copy/rotate/allocate in portrait & debug/USB paths (cv::Mat temp_img + copyTo + cv::rotate every update; USB does VideoCapture.read + BGR→gray + opaque camera draw on the OSD). These are the remaining per-update frame-sized allocations/transfers on one CPU.



3. Opportunities (ranked)

A. Requeue the camera buffer earlier — overlap capture with compute. (Largest structural win, both apps.)
- tinytag: after decode_proposals() (ROIs known, ~3–4 ms in) the raw frame is only needed to feed the CV crops, which are tiny. Copy just the few ROI crop regions (~KB) into owned storage and immediately v4l2_drm_dump_release(). The CV crop-decode then runs against owned memory while the next frame is already being captured — removing the ~9–16 ms buffer hold, decoupling throughput from CV-decode latency (that's exactly the "post_process outside the lock" class of fix, extended to the buffer).
- apriltag: implement the Stage-0 split the code already anticipates (main.cc:349-354): decimate/threshold the Y into detector-owned storage and requeue right away; the heavy CCL/flags/quad/decode then overlap the next capture. This is the main way to lift apriltag above 1/detect_time and cut latency.

B. Draw the overlay directly into the OSD plane (kill the per-detection memcpy + shrink the flush).
- Write the quads/text into draw_buffer->map (the persistent plane mmap) in the producer and just thead_csi_dcache_clean_invalid_range it, instead of drawing into osd_frame and then memcpy-ing the whole canvas. Saves the full-frame transfer each detection and frees CPU/bandwidth for the detector.
- If direct-to-plane is awkward, at least do a dirty-rectangle update (the changed content is only a few small quads + a text line on an otherwise-transparent canvas), which slashes the copy and the cache flush cost.

**C. Reduce the per-frame sized /allocate/rotate work in the portrait/debug/USB paths.**
- Draw in panel orientation directly (like the existing draw_frame_lcd90cw/draw_detections_lcd_90cw fast path already does for the CSI-landscape case) so those modes skip the temp_img + rotate + extra copy.
- USB is inherently expensive (VideoCapture + cvtColor + opaque camera draw on the CPU); if USB isn't needed, CSI is the native zero-copy path.

D. Use the hardware offload engines to keep CPU free (already partly done for tinytag).
- tinytag already offloads resize (ai2d) and inference (KPU); the residual CPU cost is the CV crop-decode, whose per-core lever is the RVV crop-decode backend (2.1x on your input) — but close the recall gap first before enabling.
- apriltag has no NPU; it's pure CPU CCL. Its lever is (again) Stage-0 overlap (A) plus CCL kernel tuning (group_emit/direction passes) — but note the instrumented stage numbers are inflated; run the production ablation matrix to confirm which mask helps.

E. Buffer-allocation hygiene.
Most of this is already good (persistent ai2d tensor/builder, persistent osd_frame/draw_frame/filtered_gray, persistent plane mmap). The remaining per-update allocations are the temp_img in portrait/debug paths and the per-frame cv::Mats in USB mode. Fixing those is cheap and removes GC/allocation jitter on the one busy core.



In short: the architecture is sound and already does the right things (zero-copy camera→detector, camera on a hardware plane, persistent buffers, hardware resize/KPU). The OSD-side full-frame copy + flush was the second-biggest serialization point and is now gone via the k230_osd triple-buffered module (draw into a CPU-owned plane buffer; buffers retired on real page flips). The remaining wins are all about the single core, and the top one is now clearly: overlap capture with compute (requeue the camera buffer earlier — the Stage-0 / small-crop split, §3A), plus CCL/RVV tune on the detector. That will move latency and throughput more than further OSD work.

Next step I'd take: prototype the capture-requeue split (the now-clearly-highest-impact one) on tinytag first — copy the ROI crops to owned storage right after decode_proposals and requeue immediately — and measure the effect on frame latency/throughput. It's a contained change to ai_proc/post_process.
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

