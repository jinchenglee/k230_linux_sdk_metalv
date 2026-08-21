#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <nncase/runtime/interpreter.h>
// Not namespace-wrapped in the installed header -- see run_op_profile()'s
// comment for why op_profile ends up unqualified.
#include <nncase/runtime/stackvm/op_profile.h>

#include "scoped_timing.hpp"
#include "sensor_set.h"
#include "tag_crop_decoder.h"
#include "tinytag_det.h"
#include "video_hw_codec.h"

using std::cerr;
using std::cout;
using std::endl;

namespace
{
void print_usage(const char *name)
{
    cout << "Usage: " << name
         << " <kmodel> <input> <heatmap_thres> <max_proposals> <roi_expand> <debug_mode>"
         << " [roi_iou_thres]" << endl
         << "Options:" << endl
         << "  kmodel          tinytag kmodel path\n"
         << "  input           one of:\n"
         << "                    - \"None\": live from the CSI camera, on-screen overlay (like\n"
         << "                      apriltag_demo.elf); 'q' + enter to quit\n"
         << "                    - an image path (anything cv::imread supports): writes\n"
         << "                      tinytag_det.jpg\n"
         << "                    - a video file path (anything cv::VideoCapture/FFmpeg supports,\n"
         << "                      e.g. .mp4): writes tinytag_det.mp4 with detections drawn on\n"
         << "                      every frame. Uses the hardware h264_v4l2m2m video encoder by\n"
         << "                      default (input decode is always software -- see\n"
         << "                      video_hw_codec.h for why); falls back to fully-software\n"
         << "                      encode automatically if the hardware encoder can't open.\n"
         << "                      Set TINYTAG_VIDEO_CODEC=sw to force fully-software encode\n"
         << "                    - \"ProfileOps\": diagnostic mode, no camera/image/video --\n"
         << "                      loads the kmodel, runs it a few times with the nncase runtime's\n"
         << "                      built-in per-op profiler enabled, and prints per-op timing (only\n"
         << "                      heatmap_thres/debug_mode args are used, both ignored)\n"
         << "  heatmap_thres   proposal confidence threshold (0.20 at the training repo's frozen\n"
         << "                  operating point -- see experimental/tinytag-v6/HEADS_AND_POSTPROCESSING.md)\n"
         << "  max_proposals   top-K cap on proposals per frame (20 at that same operating point)\n"
         << "  roi_expand      safety margin multiplied onto each proposal's decoded size before\n"
         << "                  cropping for CV decode (1.5 at that same operating point)\n"
         << "  debug_mode      0 (silent), 1 (per-stage timing), 2 (+ per-ROI timing and verbose logs)\n"
         << "  roi_iou_thres   OPTIONAL (default 0.5). Drop a proposal whose box overlaps a kept\n"
         << "                  higher-confidence one by more than this IoU, before paying for its\n"
         << "                  CV decode. The spec's 3x3 NMS only suppresses heatmap *cells*, so\n"
         << "                  well-separated peaks can still decode to near-identical boxes.\n"
         << "                  Pass 0 to disable and match the reference spec exactly.\n"
         << "\n"
         << "Environment:\n"
         << "  TINYTAG_CV_DETECTOR   CV crop-decode backend: unset/\"c\" (default) uses the\n"
         << "                        reference AprilTag C library; \"rvv\" uses the same\n"
         << "                        RVV-accelerated apriltag-rvv detector apriltag_demo.elf's\n"
         << "                        --rvv mode uses (see tag_crop_decoder.h). Both use\n"
         << "                        quad_decimate factor 1.0 (no decimation).\n"
         << "  TINYTAG_VIDEO_CODEC   Video-file mode output encode: unset/\"hw\" (default) uses\n"
         << "                        the hardware h264_v4l2m2m encoder; \"sw\" forces\n"
         << "                        fully-software encode.\n"
         << endl;
}

// Standalone diagnostic: loads the kmodel via a fresh nncase::runtime::
// interpreter -- deliberately NOT going through AIBase/TinyTagDet, since
// AIBase is shared code used by every ai_demo package and this is a
// one-off diagnostic, not something to add to shared surface -- and runs
// it a few times with the interpreter's built-in per-op profiler enabled.
// 126 MFLOPs (V4C's documented full forward-pass cost) taking 5-6s at
// 1.6GHz works out to ~70-85 CPU cycles per single FLOP, which is too slow
// to explain as "just no KPU/no vectorization" (a naive scalar loop should
// do a multiply-add in a handful of cycles) -- this finds out which
// specific op(s) are actually responsible instead of guessing.
//
// op_profile.h isn't wrapped in a namespace in the installed nncase
// headers -- it has `using namespace nncase::runtime::stackvm;` at file
// scope and relies on the *including* translation unit for the enclosing
// namespace, so the op_profile class it declares ends up unqualified here.
void run_op_profile(const char *kmodel_path, int iterations)
{
    std::ifstream ifs(kmodel_path, std::ios::binary);
    if (!ifs)
    {
        cerr << "run_op_profile: cannot open " << kmodel_path << endl;
        return;
    }

    interpreter interp;
    interp.load_model(ifs).expect("run_op_profile: invalid kmodel");

    for (size_t i = 0; i < interp.inputs_size(); ++i)
    {
        auto desc = interp.input_desc(i);
        auto shape = interp.input_shape(i);
        auto tensor = host_runtime_tensor::create(desc.datatype, shape, hrt::pool_shared)
                          .expect("run_op_profile: cannot create input tensor");
        // Zero-filled: this is a pure timing diagnostic, not a correctness
        // check, so the actual pixel values feeding it don't matter.
        interp.input_tensor(i, tensor).expect("run_op_profile: cannot set input tensor");
    }
    for (size_t i = 0; i < interp.outputs_size(); ++i)
    {
        auto desc = interp.output_desc(i);
        auto shape = interp.output_shape(i);
        auto tensor = host_runtime_tensor::create(desc.datatype, shape, hrt::pool_shared)
                          .expect("run_op_profile: cannot create output tensor");
        interp.output_tensor(i, tensor).expect("run_op_profile: cannot set output tensor");
    }

    interp.set_profiling(1);
    for (int i = 0; i < iterations; ++i)
    {
        auto t0 = std::chrono::steady_clock::now();
        interp.run().expect("run_op_profile: run failed");
        auto t1 = std::chrono::steady_clock::now();
        cout << "run_op_profile: iteration " << i << " wall time "
             << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms" << endl;
    }
    op_profile::print();
}

std::mutex result_mutex;
std::atomic<bool> ai_stop(false);
std::atomic<bool> display_stop(false);
static volatile unsigned ai_frame_count = 0;
// Diagnostic: how many times frame_handler actually got far enough to hand a
// new overlay to the display. If the OSD still looks frozen while this counter
// tracks the AI rate, the problem is downstream of this app (DRM commit /
// plane composition); if it stays ~0 while AI runs, the problem is the gating
// logic above it. Printed in the existing per-second FPS line as "osd:".
static volatile unsigned osd_staged_count = 0;
static struct timeval tv, tv2;
static struct display *g_display;
struct display_buffer *draw_buffer;
cv::Mat osd_frame;
// Incremented only after osd_frame contains a complete new overlay --
// mirrors apriltag_demo.elf's g_overlay_generation exactly (see its main.cc
// comment: "Camera and DRM events can arrive faster than detection, so
// copying on every callback wastes substantial memory bandwidth"). Gating
// frame_handler's copy on this, not on the display thread's own camera
// buffer identity, is the whole fix below -- see the comment at its use.
static std::atomic<uint64_t> g_overlay_generation(0);

// Box-level IoU suppression threshold (see TinyTagDet's constructor doc).
// ai_proc only receives `argv`, not `argc`, so it can't tell whether the
// optional trailing arg was supplied -- main() resolves it once and parks it
// here rather than duplicating that logic.
constexpr float kDefaultRoiIouThres = 0.5f;
static float g_roi_iou_thres = kDefaultRoiIouThres;

// CSI capture + inference thread. Mirrors object_detect_yolov8n's ai_proc.
// Feeds the raw sensor-resolution (1280x720) luma plane straight into
// TinyTagDet::pre_process(), which resizes to the network's 640x360 input
// via the hardware ai2d resizer in one step -- no separate software
// decimate stage (see experimental/README.md for why one used to be here).
void ai_proc(char *argv[], int video_device)
{
    struct v4l2_drm_context context;
#define BUFFER_NUM 3

    // Wait for display_proc to bring up draw_buffer (see frame_handler).
    result_mutex.lock();
    result_mutex.unlock();

    v4l2_drm_default_context(&context);
    context.device = video_device;
    context.display = false;
    context.width = SENSOR_WIDTH;
    context.height = SENSOR_HEIGHT;
    context.video_format = V4L2_PIX_FMT_NV12;
    context.buffer_num = BUFFER_NUM;
    if (v4l2_drm_setup(&context, 1, NULL))
    {
        cerr << "ai_proc: v4l2_drm_setup error" << endl;
        return;
    }
    if (v4l2_drm_start(&context))
    {
        cerr << "ai_proc: v4l2_drm_start error" << endl;
        return;
    }

    // Negotiated geometry can differ slightly from the request -- read it
    // back rather than assuming SENSOR_WIDTH/HEIGHT exactly (mirrors
    // apriltag_demo's own detect_proc).
    size_t csi_width = context.width, csi_height = context.height, csi_stride = context.width;
    struct v4l2_format csi_format = {};
    csi_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(context.video_fd, VIDIOC_G_FMT, &csi_format) == 0)
    {
        csi_width = csi_format.fmt.pix.width;
        csi_height = csi_format.fmt.pix.height;
        csi_stride = csi_format.fmt.pix.bytesperline != 0 ? csi_format.fmt.pix.bytesperline : csi_width;
    }
    fprintf(stderr, "[input] CSI requested %dx%d, negotiated %zux%zu stride=%zu\n", SENSOR_WIDTH,
            SENSOR_HEIGHT, csi_width, csi_height, csi_stride);

    int debug_mode = atoi(argv[6]);
    auto decoder = make_crop_decoder(); // TINYTAG_CV_DETECTOR=rvv for AprilTagRVVDecoder, default AprilTagCDecoder
    TinyTagDet det(argv[1], atof(argv[3]), atoi(argv[4]), atof(argv[5]), g_roi_iou_thres, decoder,
                   debug_mode);
    std::vector<TinyTagResult> results;
    std::vector<Proposal> proposals;

    while (!ai_stop)
    {
        int ret = v4l2_drm_dump(&context, 1000);
        if (ret)
        {
            perror("ai_proc: v4l2_drm_dump error");
            continue;
        }

        // NV12's first plane is 8-bit luma -- exactly the grayscale input
        // this model wants, with zero conversion. detect() below reads this
        // mmap synchronously, so the V4L2 buffer stays dequeued through the
        // whole pipeline (same tradeoff apriltag_demo's detect_proc makes).
        const uint8_t *y_plane =
            reinterpret_cast<const uint8_t *>(context.buffers[context.vbuffer.index].mmap);

        // No image_u8_decimate() here (removed -- see experimental/README.md
        // "pipeline optimization" section): SENSOR_WIDTH/HEIGHT is exactly
        // 1280x720, decimate-by-2 lands on exactly 640x360 (the network's
        // input), so the ai2d resize pre_process() does internally was
        // resizing 640x360 -> 640x360 -- a pure identity operation, paying
        // full hardware dispatch cost for zero geometric change, on top of
        // the decimate step's own RVV/software work and buffer alloc/free.
        // ai2d already resizes an arbitrary input shape to the network's
        // input in one hardware step; feeding it the raw sensor view
        // directly does the whole 1280x720 -> 640x360 downscale itself; no
        // decimate stage needed at all.
        cv::Mat net_input(static_cast<int>(csi_height), static_cast<int>(csi_width), CV_8UC1,
                          const_cast<uint8_t *>(y_plane), csi_stride);
        det.pre_process(net_input);

        det.inference();

        cv::Mat full_res_gray(static_cast<int>(csi_height), static_cast<int>(csi_width), CV_8UC1,
                              const_cast<uint8_t *>(y_plane), csi_stride);

        // post_process() (CV crop-decode dominates: a handful of ms per ROI)
        // deliberately runs OUTSIDE result_mutex. With the float kmodel this
        // didn't matter -- run() alone took ~6.6s, so post_process's few ms
        // under the lock was a rounding error next to that. With the int8
        // kmodel run() dropped to ~1.7ms, so the *same* post_process
        // duration became a large fraction of (or longer than) the display
        // thread's own refresh period -- frame_handler (which needs this
        // same mutex, called synchronously from the display thread's V4L2
        // servicing loop) could stall waiting for it on every call, which
        // is consistent with "runs fast, screen looks frozen." Only the
        // actual osd_frame write needs to be under the lock.
        results.clear();
        proposals.clear();
        det.post_process({csi_width, csi_height}, full_res_gray, results, &proposals);

        if (debug_mode > 0)
            fprintf(stderr, "\n[ai] proposals=%zu detections=%zu\n", proposals.size(), results.size());
        // Per-proposal ROI coordinates + confidence, gated at debug_mode>=2
        // (same convention as cv_crop_decode's per-ROI timing). Added to
        // check actual on-device proposal positions against host-simulator
        // predictions -- host-side comparison of fp32 vs int8 kmodels on
        // held-out validation data (experimental/tinytag/runs/tinytag-v4c/
        // compare_fp32_int8_rois.py) found int8's ROI-vs-ground-truth
        // containment is *better* than fp32's on average, which rules out
        // "quantization degraded proposal position" as the explanation for
        // int8 producing zero confirmed detections while fp32 confirms
        // them on the same physical scene. That leaves either a
        // simulator-vs-real-hardware execution mismatch, or something
        // specific to the live scene not represented in the validation
        // set -- this print is what distinguishes them: compare these
        // on-device coordinates against where the physical tag actually is.
        if (debug_mode > 1)
            for (const auto &p : proposals)
                fprintf(stderr, "[ai]   proposal conf=%.3f roi=(%.0f,%.0f,%.0f,%.0f)\n", p.confidence,
                        p.roi.x, p.roi.y, p.roi.x + p.roi.width, p.roi.y + p.roi.height);

        result_mutex.lock();
        osd_frame.setTo(cv::Scalar(0, 0, 0, 0));
        // Proposals drawn first (cyan boxes + neural confidence), confirmed
        // detections drawn on top (green quads + id) -- so a proposal the
        // CV decoder rejected is still visible, not hidden behind nothing.
        TinyTagDet::draw_proposals_scaled(osd_frame, proposals, csi_width, csi_height);
        TinyTagDet::draw_detections_scaled(osd_frame, results, csi_width, csi_height);
        g_overlay_generation.fetch_add(1, std::memory_order_release);
        result_mutex.unlock();

        ai_frame_count += 1;
        v4l2_drm_dump_release(&context);
    }
    v4l2_drm_stop(&context);
}

int frame_handler(struct v4l2_drm_context *context, bool displayed)
{
    static bool first_frame = true;
    if (first_frame)
    {
        result_mutex.unlock();
        first_frame = false;
    }

    static unsigned response = 0, display_frame_count = 0;
    response += 1;
    if (displayed)
    {
        if (context[0].buffer_hold[context[0].wp] >= 0)
        {
            // Gate on "ai_proc produced a new overlay" (g_overlay_generation),
            // not on "the display thread's own camera buffer identity
            // changed" (context[0].display_buffers[...] != some previous
            // pointer). Those are unrelated signals: the latter tracks
            // display_proc's own separate v4l2 capture stream, which can be
            // slow/stalled independent of whether ai_proc is producing fresh
            // results every frame -- exactly what happened on real hardware
            // once the int8 kmodel made ai_proc fast (log showed proposals
            // computed every ~10-40ms, but the screen only refreshed every
            // several seconds). Matches apriltag_demo.elf's frame_handler,
            // which already solved this exact class of problem.
            static uint64_t displayed_overlay_generation = UINT64_MAX;
            uint64_t generation = g_overlay_generation.load(std::memory_order_acquire);
            if (generation != displayed_overlay_generation)
            {
                // Landscape: copy osd_frame straight into the DRM buffer, no
                // intermediate Mat. The previous version (inherited from
                // object_detect_yolov8n) allocated an 8MB temp Mat, cleared
                // it, copied osd_frame into it, then copied that into
                // draw_buffer -- ~32MB of memory traffic per overlay update
                // at 1920x1080x4, vs ~16MB now. apriltag_demo removed exactly
                // this, and its comment says why: "the old temp Mat added a
                // second full-frame copy plus a full-frame clear". Invisible
                // at the float kmodel's one-update-per-6.8s, but at int8
                // speed the overlay updates ~25x/s and that extra traffic
                // competes with the display pipeline for the same memory.
                // (The clear was redundant regardless: osd_frame is fully
                // overwritten by ai_proc's own setTo() each frame, and
                // copyTo into a same-size Mat overwrites every pixel.)
                if (draw_buffer->width > draw_buffer->height)
                {
                    result_mutex.lock();
                    memcpy(draw_buffer->map, osd_frame.data, draw_buffer->size);
                    displayed_overlay_generation = g_overlay_generation.load(std::memory_order_relaxed);
                    result_mutex.unlock();
                }
                else
                {
                    // Portrait still needs the temp: cv::rotate can't write
                    // in place into a differently-shaped destination.
                    cv::Mat temp_img(draw_buffer->width, draw_buffer->height, CV_8UC4);
                    result_mutex.lock();
                    osd_frame.copyTo(temp_img);
                    displayed_overlay_generation = g_overlay_generation.load(std::memory_order_relaxed);
                    result_mutex.unlock();
                    cv::rotate(temp_img, temp_img, cv::ROTATE_90_CLOCKWISE);
                    memcpy(draw_buffer->map, temp_img.data, draw_buffer->size);
                }
                // Flush the buffer we just CPU-wrote (draw_buffer), NOT the
                // camera video-plane buffer. This previously flushed
                // `context[0].display_buffers[...]->map` -- copied verbatim
                // from object_detect_yolov8n/main.cc:175, which has the same
                // bug. apriltag_demo/src/main.cc:558 gets it right.
                thead_csi_dcache_clean_invalid_range(draw_buffer->map, draw_buffer->size);

                // The memcpy + cache flush above is what actually makes new
                // overlay content appear: the ARGB plane is committed to
                // this exact draw_buffer once, at setup
                // (display_commit_buffer in display_proc), and the display
                // keeps scanning that same FB out every frame. Updating its
                // pixels in place is therefore sufficient -- no per-frame
                // commit is required for content changes.
                //
                // This display_update_buffer() call is kept only because
                // apriltag_demo (the one implementation confirmed working on
                // this hardware) has it, and matching it exactly is worth
                // more than reasoning about whether it is needed. Note it is
                // very likely a no-op: v4l2_drm_run() calls
                // display_handle_vsync() at the top of its display block
                // (vvcam/v4l2-drm/src/lib.c:628), and that does
                // `drmModeAtomicFree(display->req); display->req = NULL;` --
                // discarding anything staged here, since frame_handler runs
                // *before* that block.
                //
                // Deliberately NOT using display->osd_disp_buffer here: that
                // hook forces have_data_to_display() (lib.c:489-499) true,
                // which forces display_handle_vsync() + display_commit() to
                // run on that iteration. With the loop already polling far
                // faster than the camera delivers (observed poll: ~3300/s vs
                // camera: ~16fps), adding forced commits risks flooding DRM
                // with atomic commits/page-flip events rather than helping.
                display_update_buffer(draw_buffer, 0, 0);
                osd_staged_count += 1;
            }
        }
        display_frame_count += 1;
    }

    gettimeofday(&tv2, NULL);
    uint64_t duration = 1000000 * (tv2.tv_sec - tv.tv_sec) + tv2.tv_usec - tv.tv_usec;
    if (duration >= 1000000)
    {
        fprintf(stderr, " poll: %.2f, ", response * 1000000. / duration);
        response = 0;
        if (g_display)
        {
            fprintf(stderr, "display: %.2f, ", display_frame_count * 1000000. / duration);
            display_frame_count = 0;
        }
        fprintf(stderr, "camera: %.2f, ", context[0].frame_count * 1000000. / duration);
        context[0].frame_count = 0;
        fprintf(stderr, "AI: %.2f, ", ai_frame_count * 1000000. / duration);
        ai_frame_count = 0;
        fprintf(stderr, "osd: %.2f", osd_staged_count * 1000000. / duration);
        osd_staged_count = 0;
        fprintf(stderr, "          \r");
        fflush(stderr);
        gettimeofday(&tv, NULL);
    }

    if (display_stop)
        return 'q';
    return 0;
}

void display_proc(int video_device)
{
    struct v4l2_drm_context context;
    v4l2_drm_default_context(&context);
    context.device = video_device;
    if (g_display->width > g_display->height)
    {
        context.width = g_display->width;
        context.height = (g_display->width * SENSOR_HEIGHT / SENSOR_WIDTH) & 0xfff8;
        context.video_format = V4L2_PIX_FMT_NV12;
        context.display_format = 0;
        context.drm_rotation = rotation_0;
    }
    else
    {
        context.width = g_display->height;
        context.height = g_display->width;
        context.video_format = V4L2_PIX_FMT_NV12;
        context.display_format = 0;
        context.drm_rotation = rotation_90;
    }

    if (v4l2_drm_setup(&context, 1, &g_display))
    {
        cerr << "display_proc: v4l2_drm_setup error" << endl;
        return;
    }

    struct display_plane *plane = display_get_plane(g_display, DRM_FORMAT_ARGB8888);
    draw_buffer = display_allocate_buffer(plane, g_display->width, g_display->height);
    display_commit_buffer(draw_buffer, 0, 0);

    if (draw_buffer->width > draw_buffer->height)
        osd_frame = cv::Mat(draw_buffer->height, draw_buffer->width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    else
        osd_frame = cv::Mat(draw_buffer->width, draw_buffer->height, CV_8UC4, cv::Scalar(0, 0, 0, 0));

    gettimeofday(&tv, NULL);
    v4l2_drm_run(&context, 1, frame_handler);

    if (g_display)
    {
        display_free_plane(plane);
        display_exit(g_display);
    }
}

// Offline mp4 (or anything else cv::VideoCapture/FFmpeg opens) mode: no
// camera, no DRM/OSD -- decode each frame, run the same pipeline as
// image-file mode, draw, and mux into an output video. Uses "mp4v"
// (MPEG-4 Part 2, FFmpeg's built-in encoder) rather than an H.264 fourcc,
// since this SDK's OpenCV build isn't guaranteed to have libx264.
int run_video_file(const char *kmodel_path, const char *video_path, float heatmap_thres,
                    int max_proposals, float roi_expand, float roi_iou_thres, int debug_mode)
{
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened())
    {
        cerr << "failed to open as image or video: " << video_path << endl;
        return -1;
    }

    int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (!(fps > 0.0))
        fps = 25.0; // some containers/streams don't report a valid fps

    const char *out_path = "tinytag_det.mp4";
    cv::VideoWriter writer(out_path, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, cv::Size(w, h));
    if (!writer.isOpened())
    {
        cerr << "failed to open output video for writing: " << out_path << endl;
        return -1;
    }

    auto decoder = make_crop_decoder(); // TINYTAG_CV_DETECTOR=rvv for AprilTagRVVDecoder, default AprilTagCDecoder
    TinyTagDet det(kmodel_path, heatmap_thres, max_proposals, roi_expand, roi_iou_thres, decoder,
                   debug_mode);

    cv::Mat frame, gray;
    int frame_idx = 0;
    int total_detections = 0;
    while (cap.read(frame))
    {
        ScopedTiming st_frame("frame " + std::to_string(frame_idx) + " total", debug_mode);
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

        std::vector<TinyTagResult> results;
        std::vector<Proposal> proposals;
        det.pre_process(gray);
        det.inference();
        det.post_process({static_cast<size_t>(gray.cols), static_cast<size_t>(gray.rows)}, gray, results,
                          &proposals);

        if (debug_mode > 0)
            cout << "frame " << frame_idx << ": proposals=" << proposals.size()
                 << " detections=" << results.size() << endl;
        for (const auto &r : results)
        {
            if (debug_mode > 1)
                cout << "  id=" << r.id << " hamming=" << r.hamming << " margin=" << r.decision_margin
                     << " center=(" << r.center.x << "," << r.center.y << ")" << endl;
        }
        total_detections += static_cast<int>(results.size());

        det.draw_proposals(frame, proposals);
        det.draw_detections(frame, results);
        writer.write(frame);
        ++frame_idx;
    }
    writer.release();
    cout << "wrote " << out_path << " (" << frame_idx << " frames, " << total_detections
         << " total detections)" << endl;
    return 0;
}
} // namespace

void __attribute__((destructor)) cleanup()
{
    std::cout << "Cleaning up memory..." << std::endl;
    shrink_memory_pool();
    kd_mpi_mmz_deinit();
}

int main(int argc, char *argv[])
{
    cout << "case " << argv[0] << " built at " << __DATE__ << " " << __TIME__ << endl;
    if (argc != 7 && argc != 8)
    {
        print_usage(argv[0]);
        return -1;
    }

    const char *kmodel_path = argv[1];
    const char *image_path = argv[2];
    float heatmap_thres = atof(argv[3]);
    int max_proposals = atoi(argv[4]);
    float roi_expand = atof(argv[5]);
    int debug_mode = atoi(argv[6]);
    float roi_iou_thres = (argc == 8) ? atof(argv[7]) : kDefaultRoiIouThres;
    g_roi_iou_thres = roi_iou_thres; // ai_proc reads this (it only gets argv)

    if (strcmp(image_path, "ProfileOps") == 0)
    {
        run_op_profile(kmodel_path, 3);
        return 0;
    }

    if (strcmp(image_path, "None") == 0)
    {
        g_display = display_init(0);
        if (!g_display)
        {
            cerr << "display_init error, exit" << endl;
            return -1;
        }

        result_mutex.lock();

        std::thread ai_thread(ai_proc, argv, kd_mpi_get_vvcam_video00() + 1);
        std::thread display_thread(display_proc, kd_mpi_get_vvcam_video00());

        cout << "输入 'q'回车退出" << endl;
        std::string input;
        while (true)
        {
            std::getline(std::cin, input);
            if (input == "q")
            {
                ai_stop.store(true);
                break;
            }
            usleep(100000);
        }

        // ai_thread first, display_thread second -- not the reverse. Both
        // v4l2 contexts (different device nodes) sit on the same underlying
        // vvcam/ISP hardware; display_proc's teardown (display_exit, after
        // v4l2_drm_run returns) releases it. Signaling display_stop first
        // let that teardown race ai_proc's still-in-flight last iteration
        // (v4l2_drm_dump has up to a 1000ms timeout) -- observed on real
        // hardware as "vvcam_mipi_release"/"vvcam_isp_release" log lines
        // interleaved mid-frame during ai_proc's final post_process call.
        // Waiting for ai_thread to fully stop its own v4l2 context first
        // means display keeps showing the last valid frame a little longer
        // on quit, not a functional loss.
        ai_thread.join();
        display_stop.store(true);
        display_thread.join();
        return 0;
    }

    cv::Mat gray = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
    if (gray.empty())
    {
        // Not a still image cv::imread recognizes -- try it as a video
        // (mp4, or anything else FFmpeg's demuxer handles).
        //
        // Hardware h264_v4l2m2m *encode* is the default (input decode is
        // always software -- see video_hw_codec.h for why hardware decode
        // isn't used too): ~2.1x faster end-to-end than the fully-software
        // path on real hardware (see experimental/README.md). Falls back
        // to run_video_file()'s fully-software encode automatically if the
        // hardware encoder can't open (run_video_file_hw returns nonzero).
        // TINYTAG_VIDEO_CODEC=sw skips straight to fully-software instead
        // of paying for the doomed hardware attempt first.
        const char *codec_env = getenv("TINYTAG_VIDEO_CODEC");
        bool want_hw = !(codec_env && strcmp(codec_env, "sw") == 0);
        if (want_hw)
        {
            int hw_ret = run_video_file_hw(kmodel_path, image_path, heatmap_thres, max_proposals, roi_expand,
                                            roi_iou_thres, debug_mode);
            if (hw_ret == 0)
                return 0;
            cerr << "hw video codec path failed (see above), falling back to software" << endl;
        }
        return run_video_file(kmodel_path, image_path, heatmap_thres, max_proposals, roi_expand,
                               roi_iou_thres, debug_mode);
    }

    auto decoder = make_crop_decoder(); // TINYTAG_CV_DETECTOR=rvv for AprilTagRVVDecoder, default AprilTagCDecoder
    TinyTagDet det(kmodel_path, heatmap_thres, max_proposals, roi_expand, roi_iou_thres, decoder,
                   debug_mode);

    std::vector<TinyTagResult> results;
    std::vector<Proposal> proposals;
    {
        ScopedTiming st_total("total (pre_process + inference + post_process)", debug_mode);
        det.pre_process(gray);
        det.inference();
        det.post_process({static_cast<size_t>(gray.cols), static_cast<size_t>(gray.rows)}, gray, results,
                          &proposals);
    }

    cout << "proposals: " << proposals.size() << endl;
    for (const auto &p : proposals)
        cout << "  confidence=" << p.confidence << " roi=(" << p.roi.x << "," << p.roi.y << ","
             << p.roi.width << "," << p.roi.height << ")" << endl;

    cout << "detections: " << results.size() << endl;
    for (const auto &r : results)
    {
        cout << "  id=" << r.id << " hamming=" << r.hamming << " margin=" << r.decision_margin
             << " proposal_conf=" << r.proposal_confidence << " center=(" << r.center.x << ","
             << r.center.y << ")" << endl;
    }

    cv::Mat vis;
    cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    det.draw_proposals(vis, proposals);
    det.draw_detections(vis, results);
    cv::imwrite("tinytag_det.jpg", vis);
    cout << "wrote tinytag_det.jpg" << endl;

    return 0;
}
