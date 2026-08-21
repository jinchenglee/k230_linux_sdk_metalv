#ifndef TINYTAG_VIDEO_HW_CODEC_H
#define TINYTAG_VIDEO_HW_CODEC_H

// Offline mp4-in/mp4-out mode using the K230's hardware H.264 encoder
// (v4l2m2m, backed by the "mvx" Linlon V5276 VPU driver) for the *output*
// stream, while keeping cv::VideoCapture's existing software decode for
// the input (same as run_video_file() in main.cc; only the encode/write
// side differs here).
//
// Why not hardware decode too: tried it first, and running the hardware
// h264_v4l2m2m *decoder* and *encoder* concurrently (two V4L2 M2M
// instances open on the same /dev/video0 at once) hangs indefinitely on
// the encoder's first avcodec_send_frame() call, even after loosening
// buffer counts -- looks like genuine driver/hardware-core contention
// between the two instances, not a tunable parameter. Software decode +
// hardware encode is the exact combination already validated working
// end-to-end via a raw ffmpeg CLI test (`ffmpeg -i in.mp4 -c:v
// h264_v4l2m2m ... -f null -`, which uses ffmpeg's default *software*
// h264 decoder for the input side), so it carries much lower risk than
// the concurrent-hardware combination. See experimental/README.md for
// the full investigation, including the measured per-stage numbers:
// hardware encode alone alone is ~13.4ms/frame vs ~120ms/frame software
// (the bigger of the two bottlenecks), while decode was always the
// smaller win (25.7fps software already, vs 119fps hardware) -- so this
// combination keeps most of the available speedup without the hang.
//
// Only tested/expected to work for H.264 *output*; falls back to the
// software path (return nonzero) if the hardware encoder can't be opened,
// so the caller (main.cc) can retry with run_video_file().
int run_video_file_hw(const char *kmodel_path, const char *video_path, float heatmap_thres,
                       int max_proposals, float roi_expand, float roi_iou_thres, int debug_mode);

#endif // TINYTAG_VIDEO_HW_CODEC_H
