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

Next step I'd take: see §6 — the detector is no longer the bottleneck. Live apriltag runs at ~50% of the detector's standalone throughput, so the work is cutting the capture/display load, starting with a 1280x720@30 sensor mode.
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

## 6. The detector is no longer the bottleneck (2026-09-02)

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

1. **Cap the display rate.** Do this first: it is independent of the camera
   mode, the detector and the sensor tuning, so once it is settled it stays
   settled regardless of what happens to the rest. The display thread flips at
   ~60 fps while detections change at ~9 fps; the camera image is on a hardware
   plane and the OSD only updates at detection rate, so most of those
   poll/atomic-commit/buffer-recycle cycles are CPU overhead for no visible
   benefit. Test at 30 fps, and note the panel is 480x800 — there is no
   information above the detection rate to show.
2. **Then the capture side: a native 1280x720@30 sensor mode.** §5 already
   priced 60 fps capture: 1080p30 yields 15 fps detect, 720p60 yields 9 fps for
   the same 1280x720 detector input — i.e. the app runs at 80% of standalone
   detector throughput at 30 fps capture but only 50% at 60 fps. That is ~40% of
   detection throughput spent on freshness that §3A bounded at ~19 ms. A 720p30
   mode keeps the native binned readout (no ISP downscale) at half the capture
   load. In the register table it is only VTS: `0x380e`/`0x380f` 851 -> 1702,
   everything else unchanged. It also **doubles the exposure ceiling**, which
   should cut the analog gain the AE currently rails at (14.7-24x) and with it
   the sensor noise — see docs/notes/ov5647-720p-mode.md §8.
3. **Then detector work.** RVV (+12% standalone) and factor/min-blob tuning
   become worth enabling once the contention above is removed.

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
