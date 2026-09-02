# tinytag_detect

Two-stage AprilTag detector for K230: a small neural network (`TinyTagDet`,
architecture `context_k230` -- see `experimental/README.md`) proposes ROIs
on a downscaled frame, then a traditional CV AprilTag decoder
(`AprilTagCDecoder`, the standard AprilRobotics C library) reads each
proposal's ID from the full-resolution image. Ships as `tinytag_detect.elf`.

This file tracks the *current* architecture/state. For the investigation
history behind why it looks like this (the K230 KPU dilated-conv bug and
retrain, the hardware video codec integration, the pipeline optimizations
below), see `experimental/README.md` -- that's the log; this is the map.

## Usage

```
tinytag_detect.elf <kmodel> <input> <heatmap_thres> <max_proposals> <roi_expand> <profile_mode> [roi_iou_thres] [--debug]
```

`<input>` is one of:
- `"None"`: live CSI camera, on-screen overlay, `q`+enter to quit.
- an image path (anything `cv::imread` supports): writes `tinytag_det.jpg`.
- a video file path: writes `tinytag_det.mp4` with detections drawn on
  every frame. Uses the hardware `h264_v4l2m2m` video encoder by default
  (`TINYTAG_VIDEO_CODEC=sw` forces fully-software); input decode is always
  software regardless (see "Hardware video codec" below for why).
- `"ProfileOps"`: diagnostic mode, prints per-op KPU timing.

`profile_mode`: 0 silent, 1 per-stage timing, 2 + per-ROI timing and verbose
proposal/detection logs.

`--debug` enables the opt-in live-loop timing report for confirmed detections
(capture wait/requeue, ROI copy, OSD work, and frame total).

### Operating point: heatmap_thres

`heatmap_thres` trades recall against proposal count (and therefore the
per-ROI CV crop-decode cost, which dominates the live pipeline).

- `0.20` (the training repo's frozen value) is **very conservative**: it
  keeps the most ROIs (highest recall) but proposes many low-confidence
  boxes, and the CV decoder pays for each of them even when they decode to
  nothing. Measured on 1280x800 footage: ~14 proposals/frame, ~16.7 ms
  post_process.
- `0.35` is the recommended **production** setting: it cuts the proposal
  count roughly in half (~7/frame) and roughly halves post_process
  (~9.1 ms), for only a **slight drop in recall** (e.g. 204 -> 185
  detections across a 65-frame clip). Use this unless an application
  specifically needs maximum recall over throughput.

Both were measured with the 16:9 crop described below; see the profiling
notes in `docs/commits/tinytag-crop-threshold-commit.txt`.


## Pipeline: live camera (real deployment)

```
                    V4L2 camera capture (NV12, 1280x720)
                                |
                                | mmap, ZERO-COPY -- buffer stays locked/
                                | dequeued through the whole loop below
                                v
                    y_plane (raw luma, csi_stride)
                                |
              +-----------------+-----------------+
              |                                    |
              v ZERO-COPY cv::Mat view              v ZERO-COPY cv::Mat view
        net_input (1280x720)                 full_res_gray (1280x720)
              |                                    |
   [1] pre_process()                               |  (held until post_process
              |                                    |   below is done with it)
   row-copy -> ai2d_in_tensor_ (PERSISTENT,         |
   allocated once, memcpy respects .step)           |
              |                                    |
   ai2d_builder_->invoke()  [HARDWARE, ~1.95ms,     |
   PERSISTENT builder -- built once per input       |
   shape, not every frame]                          |
              |                                    |
              v                                    |
   ai2d_out_tensor_ = kmodel input (640x360, u8)     |
              |                                    |
   [2] inference()  [KPU, int8, ~2.2ms]             |
              |                                    |
              v                                    |
   p_outputs_: f32[1,21,45,80]                      |
              |                                    |
   [3] decode_proposals()  [sigmoid+NMS+topk+IoU,    |
        <0.5ms total]                               |
              |                                    |
              v                                    |
   proposals: [{confidence, roi}] (<=max_proposals)  |
              |                                    |
              +--------------------+---------------+
                                   |
                     [4] post_process(): per proposal
                                   |
                     crop = full_res_gray(roi)   ZERO-COPY view
                     (no .clone() -- AprilTagCDecoder::detect()
                      reads crop.step as a real stride, see below)
                                   |
                     AprilTagCDecoder::detect(crop)
                     [quad_decimate=1.0, traditional CV,
                      ~2.2ms/ROI -- the dominant real-deployment cost]
                                   |
                                   v
                     TagDetection[] -> TinyTagResult[] -> dedupe()
                                   |
                                   v
                     draw_proposals/draw_detections onto osd_frame
                     (under result_mutex, DRM overlay plane)
```

Real per-frame budget (measured, `bos_logs_video` real match footage):
**~15.25ms** = pre_process (2.4ms) + inference (2.2ms) + post_process
(10.7ms, crop-decode dominated). See "Pipeline optimizations" below for
what each stage used to cost and why.

## Pipeline: video-file mode (offline evaluation only)

Same `pre_process` -> `inference` -> `post_process` core as above, but the
source/sink stages differ -- and this mode is not what real deployment
runs, so its cost (dominated by video codec work below) doesn't reflect
live-camera speed:

```
  mp4 file --[cv::VideoCapture, SOFTWARE decode]--> frame (BGR)
                                                        |
                                          cvtColor(BGR2GRAY) -> gray
                                                        |
                                    [same pre_process/inference/post_process
                                     as the live-camera diagram above, with
                                     `frame` itself standing in for
                                     full_res_gray's role]
                                                        |
                                          draw_proposals/draw_detections
                                          drawn onto `frame` (BGR, in place)
                                                        |
                                    cv::cvtColor(BGR2YUV_I420)  [~6.6ms --
                                    NOT libswscale, see below]
                                                        |
                                    memcpy x3 planes -> persistent enc_frame
                                    (AVFrame, YUV420P, alignment=1 so planes
                                     are byte-for-byte what cvtColor produced)
                                                        |
                                    avcodec_send_frame()/receive_packet()
                                    -> h264_v4l2m2m HARDWARE encoder
                                    [~12.3ms + ~3.6ms, ~9x faster than the
                                     software mpeg4 path it replaced]
                                                        |
                                                        v
                                                  tinytag_det.mp4
```

## Buffers

| Buffer | Owner / lifetime | Copy? |
|---|---|---|
| V4L2 camera mmap | driver, locked for the whole frame iteration | source of truth, never copied wholesale |
| `net_input`, `full_res_gray` (live) | `cv::Mat` views over the mmap | zero-copy |
| `ai2d_in_tensor_` | `TinyTagDet` member, allocated **once** (mmz pool) | one row-copy in (unavoidable: dest is a different memory pool from the source) |
| `ai2d_out_tensor_` | `TinyTagDet` member = kmodel's own input tensor | n/a (ai2d writes directly) |
| `p_outputs_` | mapped view of the kmodel's own output tensor | zero-copy (`AIBase::get_output()`) |
| crop (`full_res_gray(roi)`) | ROI-decode loop, per proposal | **zero-copy** view, not cloned (see below) |
| `enc_frame` (video-file mode) | `HwEncoderState` member, allocated once | one 3-plane memcpy per frame (unavoidable: `AVFrame` needs its own buffer for the encoder to own) |

## Pipeline optimizations (2026-08-17)

Four fixes, all measured on real hardware against `bos_logs_video`
(`experimental/README.md` has the full narrative and numbers):

1. **Persistent `ai2d_builder`/`ai2d_in_tensor_`.** `pre_process()` used to
   construct a fresh `ai2d_builder` and call `build_schedule()` on *every
   frame*, plus double-copy through an intermediate `std::vector` before
   ever reaching the tensor ai2d actually reads. Now built once (lazily, on
   first call or if the input shape changes) and written into directly.
   Matches the pattern other `ai_demo` packages already use (e.g.
   `face_detection.cc`). `pre_process`: 3.20ms -> 2.38ms avg.

2. **Zero-copy crop-decode.** `post_process()`'s ROI loop used to
   `.clone()` each proposal's crop before decoding, with a comment
   claiming the AprilTag library needed contiguous memory. It doesn't --
   `image_u8_t` has a first-class `stride` field and the whole library
   indexes through it (`y*stride + x`, confirmed in `image_u8.c`), so a
   plain non-contiguous `cv::Mat` view works correctly. Removed the clone
   and the incorrect `CV_Assert(isContinuous())` guarding it.

3. **`quad_decimate` default changed from 2.0 to 1.0.** The AprilTag
   library's own default (2.0) exists for full-frame detection, where
   halving resolution before quad-finding is a real speed win on a large
   image. `AprilTagCDecoder` never overrode it, so it was silently
   inherited for an *already-cropped, already-upscaled* small ROI -- the
   wrong setting for that case. At 1.0: **705 -> 1209 detections** on the
   same 499-frame real-footage test (1.71x), crop-decode cost roughly
   doubled per ROI (0.92ms -> 2.2ms) as expected. Net real-deployment
   frame cost: 10.26ms -> 15.25ms (+49%) for 1.71x more detections --
   accepted as the right tradeoff.

4. **Redundant `image_u8_decimate` removed from the live-camera path.**
   `SENSOR_WIDTH`/`HEIGHT` is exactly 1280x720; decimating by 2 lands on
   exactly 640x360 (the network's input), so the ai2d resize
   `pre_process()` does internally was resizing 640x360->640x360 (a no-op)
   on top of the decimate step's own work. ai2d already resizes an
   arbitrary input to the network's input in one hardware step, so the
   raw 1280x720 sensor view is now fed directly to it, and the decimate
   stage is gone entirely.
   - Measured overhead directly, since it's not obvious which way this
     cuts: software `image_u8_decimate(1280x720, factor=2.0)` costs
     ~2.02ms/call; the hardware ai2d resize doing the *same* 1280x720->
     640x360 transformation costs ~1.95ms/call -- **essentially tied**,
     hardware is not dramatically faster than software for this specific
     op. The actual win here is doing the resize *once* instead of
     *twice* (redundant decimate + redundant identity ai2d resize),
     not "hardware beats software."

**Not yet done**: detecting with more aggressively upscaled ROI crops
(distinct from `roi_expand`'s box-margin widening -- deliberately
upscaling crop *pixels* before decode, an idea raised alongside the
`quad_decimate` fix, not yet implemented).

## CV detector backend: swappable, `TINYTAG_CV_DETECTOR`

Two crop-decode backends, both `TagCropDecoder` implementations, both
zero-copy (accept a plain `cv::Mat` ROI view, no clone -- see above), both
default to no quad-search decimation (factor 1.0):

- **`AprilTagCDecoder`** (default): the reference AprilRobotics C library,
  same one this app always used.
- **`AprilTagRVVDecoder`** (`TINYTAG_CV_DETECTOR=rvv`): the apriltag-rvv
  Rust crate (`apriltag_demo/lib/libapriltag_rvv.a`), the same
  RVV-accelerated detector `apriltag_demo.elf --rvv` uses. Selected via
  `make_crop_decoder()`, used consistently by every call site (live
  camera, video-file, image mode) so they can't drift out of sync.

**Measured, 499-frame real footage (`bos_logs_video/log181_main_bot_left.mp4`,
2228 total ROIs), per-ROI, split by outcome** -- a naive aggregate
comparison is confounded here: a detector that gives up on more ROIs does
*less work on average* for reasons unrelated to raw per-op speed (a miss
is inherently cheaper than a full decode for both backends), so the fair
comparison controls for that by comparing hits-to-hits and misses-to-misses
separately, not just the overall average:

| | avg time, hit (found >=1 tag) | avg time, miss (found 0) | total detections |
|---|---:|---:|---:|
| `c` (default) | 2.70ms (n=1209) | 1.55ms (n=1019) | 1209 |
| `rvv` | 1.57ms (n=1116) | 0.84ms (n=1112) | 1116 |
| **ratio (c/rvv)** | **1.72x** | **1.85x** | -- |

RVV is genuinely **~1.7-1.9x faster** even controlling for outcome (not
the far larger ratio a single-frame example or the raw unmatched aggregate
suggested -- both of those were skewed by RVV's higher miss rate, misses
being cheaper). It also finds **~7.7% fewer** detections than the
reference C library on the same footage -- two real, separate effects,
not one masquerading as the other. Neither is root-caused yet (candidates
for the recall gap: `min_blob_size=25`, copied from `apriltag_demo`'s own
default without checking whether it's right for an already-cropped small
ROI, same category of issue `quad_decimate` turned out to be; or a
genuine algorithmic difference between the reference and Rust-reimplemented
detector). **`c` stays the default** until the recall gap is understood --
`rvv` is available today for anyone who wants faster crop-decode with a
real, understood, if not-yet-explained recall cost.

## Hardware video codec (video-file mode only)

Uses the K230's hardware H.264 encoder (`h264_v4l2m2m`, the "mvx" Linlon
VPU) for output, software decode for input. See `experimental/README.md`
("Hardware video codec" section) for the full story: why hardware
decode+encode can't run concurrently (hangs -- driver/hardware-core
contention), the exact non-default buffer-count options this driver
needs, and why `libswscale` was replaced with `cv::cvtColor` for the pixel
format conversion (a ~64ms/frame hidden bottleneck). Net: ~2.1x faster
offline video evaluation (5.67fps -> ~12fps). Does not affect live-camera
deployment speed at all -- there's no video to decode/encode there.

## Where normalization happens

The uint8->float `(x-mean)/std` input normalization is **not** a software
step in this app at all -- `nncase`'s `CompileOptions.preprocess=True`
bakes it into the compiled kmodel graph itself (`SubMean`/`DivStd` ops,
confirmed by name in the compiled IR), executed by the KPU as part of
`inference()`. There's nothing to optimize in application code here; it's
already fully included in the ~2.2ms inference cost.
