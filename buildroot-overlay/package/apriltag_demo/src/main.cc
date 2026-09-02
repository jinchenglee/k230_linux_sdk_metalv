/* apriltag_demo — live on-device AprilTag detection with DRM display.
 *
 * Mirrors apriltag-rvv/examples/live_demo.rs, but runs on the K230 board:
 *   - live camera shown on the hardware NV12 video plane (like face_detect);
 *   - detections drawn as quad overlays on a transparent ARGB OSD plane;
 *   - detection runs in libapriltag_rvv.a (Rust apriltag-rvv, feature "capi").
 *
 * Structure cloned from face_detect/src/main.cc (camera + DRM + OSD), with the
 * nncase AI pipeline replaced by the apriltag C ABI (src/apriltag.h).
 *
 * With --debug, keys 1–5 replace the transparent detection overlay with live
 * intermediate pipeline images, making on-device camera/threshold/quad/decode
 * failures visible without writing a debug image sequence to storage.
 */
#include <condition_variable>
#include <iostream>
#include <thread>
#include <string>
#include <cstring>
#include <csignal>
#include <pthread.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "k230_osd.h"
#include "sensor_set.h"
#include "apriltag.h"
#include "apriltag_draw.h"
#include "demo_options.h"
#ifdef APRILTAG_C_BACKEND
#include "apriltag_c_adapter.h"
#endif

using std::cerr;
using std::cout;
using std::endl;

// ── Config (from argv) ──────────────────────────────────────────────────────
static int    g_factor_int   = 2;     // FFI: 0=1.0 1=1.5 2=2.0
static double g_factor_value = 2.0;
static int    g_mode         = 0;     // FFI: 0=scalar 1=rvv
static uint32_t g_min_blob   = 25;
static bool g_debug_enabled  = false;
static bool g_local_ccl_scratch = false;
static std::atomic<int> g_debug_stage(0);
static std::atomic<int> g_input_source(0); // 0=CSI, 1=USB
static std::atomic<int> g_denoise_mode(0);  // 0=off, 1=median3, 2=gaussian3
static int g_usb_video       = -1;    // /dev/videoX; required before pressing 'u'
static unsigned g_csi_width  = SENSOR_WIDTH;
static unsigned g_csi_height = SENSOR_HEIGHT;
#ifdef APRILTAG_C_BACKEND
static int g_c_threads = 1;
static int g_c_bits_corrected = 0;
static int g_c_refine_edges = 0;
static double g_c_decode_sharpening = 0.0;
#endif

#define MAX_DETS 64
#define MAX_DECODE_CANDIDATES 256

// ── Shared state ────────────────────────────────────────────────────────────
static std::mutex start_mutex;
static std::condition_variable start_cv;
static bool display_ready = false;
static std::vector<apriltag_det_t> detections;
std::atomic<bool> detect_stop(false);
std::atomic<bool> display_stop(false);
std::atomic<bool> dump_request(false);  // set on 'q' to dump the current frame
static volatile unsigned detect_frame_count = 0;
static volatile unsigned osd_staged_count = 0;

static struct timeval tv, tv2;
static struct display* display;
static struct k230_osd* g_osd;
static std::atomic<double> g_cam_fps(0.0);
static std::atomic<double> g_det_fps(0.0);

// Advances g_debug_stage the same way a '1'-'5' keypress does (see the
// interactive loop in main() below), but reachable with no controlling
// terminal at all: registered as the SIGUSR1 handler so the 01studio
// KEY-button watcher (S60apriltagkey's cycle_view(), which launches this
// app with --debug so g_debug_enabled is already true) can drive live
// pipeline-view cycling on a short press, wrapping normal -> debug 1 -> 2
// -> 3 -> 4 -> 5 -> normal -> ... A signal handler must stick to
// async-signal-safe operations -- this only touches a lock-free atomic,
// unlike the keyboard path's cout print, which is why there's no log
// line here.
static void handle_view_cycle_signal(int /*signum*/)
{
    int next = (g_debug_stage.load(std::memory_order_relaxed) + 1) % 6;
    g_debug_stage.store(next, std::memory_order_relaxed);
}

static const char* denoise_mode_name(int mode)
{
    switch (mode) {
    case 1: return "median 3x3";
    case 2: return "Gaussian 3x3";
    default: return "off";
    }
}

static void print_key_help()
{
    cout << "keys: c=CSI u=USB n=denoise";
#ifndef APRILTAG_C_BACKEND
    if (g_debug_enabled) {
        cout << " 0=camera 1=gray 2=threshold 3=clusters"
                " 4=quads 5=detections";
    }
#endif
    cout << " q=quit" << endl;
}

static void print_usage(const char* name)
{
    cout << "Usage: " << name;
#ifndef APRILTAG_C_BACKEND
    cout << " [--rvv] [--local-ccl-scratch]";
#endif
    cout << " [--factor 1|1.5|2] [--min-blob N]"
            " [--csi-size WxH] [--usb-video X] [--debug]" << endl;
#ifndef APRILTAG_C_BACKEND
    cout << "  --rvv         use RVV kernels (default: scalar)" << endl;
    cout << "  --local-ccl-scratch  allocate CCL scratch per detection"
            " (default: reusable)" << endl;
#else
    cout << "  --threads N   C detector worker threads (default: 1)" << endl;
    cout << "  --bits-corrected N  accepted bit errors, 0..2 (default: 0)"
         << endl;
    cout << "  --refine-edges  enable upstream edge refinement" << endl;
    cout << "  --decode-sharpening X  payload sharpening (default: 0)"
         << endl;
    cout << "  --upstream-defaults  use C defaults: blob=5, bits=2,"
            " refine on, sharpening=0.25" << endl;
#endif
    cout << "  --factor      decimation factor 1.0/1.5/2.0 (default: 2.0)"
         << endl;
    cout << "  --min-blob    minimum blob size (default: 25)" << endl;
    cout << "  --csi-size    CSI detection stream size (default: "
         << SENSOR_WIDTH << "x" << SENSOR_HEIGHT << ")" << endl;
    cout << "  --usb-video   USB camera node number X for /dev/videoX" << endl;
#ifndef APRILTAG_C_BACKEND
    cout << "  --debug       enable live pipeline views and decode diagnostics"
            " (default: off)" << endl;
#else
    cout << "  --debug       dump one set of upstream debug images and enable"
            " detection logs" << endl;
#endif
    cout << "  c             select CSI camera (default)" << endl;
    cout << "  u             select configured USB camera" << endl;
    cout << "  n             cycle luma denoise: off/median3/Gaussian3" << endl;
#ifndef APRILTAG_C_BACKEND
    cout << "  0..5          select pipeline view (requires --debug)" << endl;
#endif
    cout << "  q             quit" << endl;
}

// Read single keys immediately on a terminal, restoring its settings on exit.
class TerminalRawMode {
public:
    TerminalRawMode() : active_(false)
    {
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_) == 0) {
            struct termios raw = saved_;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            active_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }

    ~TerminalRawMode()
    {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        }
    }

private:
    bool active_;
    struct termios saved_;
};

// ── Detection thread ────────────────────────────────────────────────────────
// CSI uses the NV12 Y-plane directly. USB uses OpenCV's V4L2 backend to
// negotiate/decode the webcam format, then feeds a BGR->gray conversion to the
// same detector and paints the BGR frame opaquely on the OSD.
static void detect_proc(int video_device)
{
    pthread_setname_np(pthread_self(), "apriltag-detect");

    struct v4l2_drm_context context;
    #define DETECT_BUFFER_NUM 3

    // Do not touch g_osd until display_proc has created and committed it.
    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_cv.wait(lock, [] { return display_ready || detect_stop.load(); });
    }
    if (detect_stop)
        return;

    v4l2_drm_default_context(&context);
    context.device = video_device;
    context.display = false;
    context.width = g_csi_width;
    context.height = g_csi_height;
    context.video_format = V4L2_PIX_FMT_NV12;
    context.buffer_num = DETECT_BUFFER_NUM;
    if (v4l2_drm_setup(&context, 1, NULL)) {
        cerr << "detect: v4l2_drm_setup error" << endl;
        return;
    }
    if (v4l2_drm_start(&context)) {
        cerr << "detect: v4l2_drm_start error" << endl;
        return;
    }

    size_t csi_width = context.width;
    size_t csi_height = context.height;
    size_t csi_stride = context.width;
    struct v4l2_format csi_format = {};
    csi_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(context.video_fd, VIDIOC_G_FMT, &csi_format) == 0) {
        csi_width = csi_format.fmt.pix.width;
        csi_height = csi_format.fmt.pix.height;
        csi_stride = csi_format.fmt.pix.bytesperline != 0
                   ? csi_format.fmt.pix.bytesperline : csi_width;
    }
    fprintf(stderr,
            "[input] CSI requested %ux%u, negotiated %zux%zu stride=%zu\n",
            g_csi_width, g_csi_height, csi_width, csi_height, csi_stride);
    struct v4l2_streamparm csi_parm = {};
    csi_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(context.video_fd, VIDIOC_G_PARM, &csi_parm) == 0 &&
        csi_parm.parm.capture.timeperframe.numerator != 0) {
        const auto& tpf = csi_parm.parm.capture.timeperframe;
        fprintf(stderr, "[input] CSI detection interval=%u/%u s (%.2f fps)\n",
                tpf.numerator, tpf.denominator,
                static_cast<double>(tpf.denominator) / tpf.numerator);
    }

    void* det = nullptr;
#ifdef APRILTAG_C_BACKEND
    det = apriltag_new(g_min_blob);
#else
    det = create_configured_detector(g_min_blob, g_local_ccl_scratch,
                                     apriltag_new,
                                     apriltag_set_ccl_scratch_mode_v1,
                                     apriltag_free);
#endif
    if (!det) {
        cerr << "detect: cannot create/configure detector" << endl;
        v4l2_drm_stop(&context);
        return;
    }
#ifdef APRILTAG_C_BACKEND
    if (apriltag_c_configure(det, g_c_threads, g_c_bits_corrected,
                             g_c_refine_edges,
                             g_c_decode_sharpening) != 0) {
        cerr << "detect: cannot configure official C detector" << endl;
        apriltag_free(det);
        v4l2_drm_stop(&context);
        return;
    }
#endif
    if (apriltag_set_debug_enabled(det, g_debug_enabled ? 1 : 0) != 0) {
        cerr << "detect: cannot configure diagnostics" << endl;
        apriltag_free(det);
        v4l2_drm_stop(&context);
        return;
    }
    std::vector<apriltag_det_t> out(MAX_DETS);
    std::vector<apriltag_decode_candidate_t> decode_candidates;
    if (g_debug_enabled) {
        decode_candidates.resize(MAX_DECODE_CANDIDATES);
    }
    cv::VideoCapture usb_capture;
    cv::Mat usb_bgr;
    cv::Mat usb_gray;
    cv::Mat filtered_gray;

    while (!detect_stop) {
        const int selected_source = g_input_source.load();
        const int selected_debug_stage =
            g_debug_enabled ? g_debug_stage.load() : 0;
        const bool use_lcd_fastpath =
            selected_debug_stage == 0 && selected_source == 0 &&
            display->width < display->height;
        k230_osd_set_mode(g_osd,
            (display->width > display->height || use_lcd_fastpath)
                ? K230_OSD_MODE_FAST : K230_OSD_MODE_SLOW_ROTATE);
        k230_osd_prepare(g_osd);
        const char* source_name = selected_source == 0 ? "csi" : "usb";
        const uint8_t* gray = nullptr;
        size_t frame_width = 0;
        size_t frame_height = 0;
        size_t frame_stride = 0;
        bool csi_frame_held = false;

        if (selected_source == 0) {
            if (usb_capture.isOpened()) {
                usb_capture.release();
                usb_bgr.release();
                usb_gray.release();
                fprintf(stderr, "\n[input] switched to CSI camera\n");
            }
            int ret = v4l2_drm_dump_latest(&context, 1000);
            if (ret) {
                perror("detect: v4l2_drm_dump_latest error");
                continue;
            }
            csi_frame_held = true;
            frame_width = csi_width;
            frame_height = csi_height;
            // The Rust detector's persistent stage-0/1 buffers are the right
            // ownership boundary for an early requeue. Until that split C API
            // exists, keep this zero-copy mmap alive for the current call
            // rather than taking a full-resolution private snapshot here.
            gray = static_cast<const uint8_t *>(context.buffers[context.vbuffer.index].mmap);
            frame_stride = csi_stride;
        } else {
            if (g_usb_video < 0) {
                g_input_source.store(0);
                fprintf(stderr, "\n[input] USB camera is not configured; "
                        "start with --usb-video X\n");
                continue;
            }
            if (!usb_capture.isOpened()) {
                fprintf(stderr, "\n[input] opening USB camera /dev/video%d ...\n",
                        g_usb_video);
                if (!usb_capture.open(g_usb_video, cv::CAP_V4L2)) {
                    cv::Mat &osd = k230_osd_begin(g_osd);
                    osd.setTo(cv::Scalar(0, 0, 0, 255));
                    char message[96];
                    snprintf(message, sizeof(message),
                             "cannot open USB camera /dev/video%d", g_usb_video);
                    cv::putText(osd, message, cv::Point(18, 32),
                                cv::FONT_HERSHEY_SIMPLEX, 0.75,
                                cv::Scalar(0, 255, 255, 255), 2);
                    k230_osd_publish(g_osd);
                    usleep(500000);
                    continue;
                }
                // Ask for the CSI geometry but accept whatever the webcam
                // negotiates. A one-frame backend queue limits display lag.
                usb_capture.set(cv::CAP_PROP_FRAME_WIDTH, SENSOR_WIDTH);
                usb_capture.set(cv::CAP_PROP_FRAME_HEIGHT, SENSOR_HEIGHT);
                usb_capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
                fprintf(stderr, "[input] USB /dev/video%d opened at %.0fx%.0f\n",
                        g_usb_video,
                        usb_capture.get(cv::CAP_PROP_FRAME_WIDTH),
                        usb_capture.get(cv::CAP_PROP_FRAME_HEIGHT));
            }
            if (!usb_capture.read(usb_bgr) || usb_bgr.empty()) {
                fprintf(stderr, "\n[input] USB read failed; reopening /dev/video%d\n",
                        g_usb_video);
                usb_capture.release();
                usleep(250000);
                continue;
            }
            if (usb_bgr.channels() == 3) {
                cv::cvtColor(usb_bgr, usb_gray, cv::COLOR_BGR2GRAY);
            } else if (usb_bgr.channels() == 4) {
                cv::cvtColor(usb_bgr, usb_gray, cv::COLOR_BGRA2GRAY);
                cv::Mat converted_bgr;
                cv::cvtColor(usb_bgr, converted_bgr, cv::COLOR_BGRA2BGR);
                usb_bgr = converted_bgr;
            } else {
                usb_gray = usb_bgr;
                cv::cvtColor(usb_gray, usb_bgr, cv::COLOR_GRAY2BGR);
            }
            gray = usb_gray.data;
            frame_width = usb_gray.cols;
            frame_height = usb_gray.rows;
            frame_stride = usb_gray.step;
        }

        // Filter only the grayscale input consumed by the detector. Keep the
        // V4L2 mmap and USB display frame untouched, and reuse filtered_gray's
        // allocation across frames.
        const int selected_denoise = g_denoise_mode.load();
        if (selected_denoise != 0 && frame_width >= 3 && frame_height >= 3) {
            cv::Mat input_gray(static_cast<int>(frame_height),
                               static_cast<int>(frame_width),
                               CV_8UC1,
                               const_cast<uint8_t*>(gray),
                               frame_stride);
            if (selected_denoise == 1) {
                cv::medianBlur(input_gray, filtered_gray, 3);
            } else {
                cv::GaussianBlur(input_gray, filtered_gray, cv::Size(3, 3),
                                 0.8, 0.8, cv::BORDER_REPLICATE);
            }
            gray = filtered_gray.data;
            frame_stride = filtered_gray.step;
        }

        // Dump the exact grayscale we feed detect() (packed width*height,
        // stride-corrected) when requested — triggered by pressing 'q' so the
        // tag is framed. Set APRILTAG_DEMO_DUMP=path to enable.
        if (g_debug_enabled && dump_request.load()) {
            const char* dump_path = getenv("APRILTAG_DEMO_DUMP");
            if (dump_path) {
                FILE* f = fopen(dump_path, "wb");
                if (f) {
                    for (size_t r = 0; r < frame_height; ++r) {
                        fwrite(gray + r * frame_stride, 1, frame_width, f);
                    }
                    fclose(f);
                    fprintf(stderr, "\n[dump] wrote %zux%zu %s grayscale "
                            "(%zu bytes) to %s\n",
                            frame_width, frame_height, source_name,
                            frame_width * frame_height, dump_path);
                }
            }
            dump_request.store(false);
        }
        if (g_debug_enabled) {
            apriltag_set_debug_stage(det, selected_debug_stage);
        }
        int n = apriltag_detect(det, gray, frame_width, frame_height,
                                frame_stride, g_factor_int, g_mode,
                                out.data(), MAX_DETS);
        apriltag_decode_stats_t decode_stats = {};
        bool have_decode_stats = false;
        int decode_candidate_count = 0;
        if (g_debug_enabled) {
            have_decode_stats =
                apriltag_get_decode_stats(det, &decode_stats) == 1;
            decode_candidate_count = apriltag_get_decode_candidates(
                det, decode_candidates.data(), MAX_DECODE_CANDIDATES);
            if (decode_candidate_count < 0) {
                decode_candidate_count = 0;
            }
        }

        // ── Diagnostics (throttled ~1/s): distinguish panic (-1) from
        //    "no detections" (0) from "detected but not drawn" (>0). ──
        if (g_debug_enabled) {
            static struct timeval dtv = {0, 0};
            static int frames = 0, max_n = 0, panics = 0;
            struct timeval now; gettimeofday(&now, NULL);
            if (dtv.tv_sec == 0) dtv = now;
            frames++;
            if (n < 0) panics++;
            if (n > max_n) max_n = n;
            uint64_t us = 1000000ULL * (now.tv_sec - dtv.tv_sec) + now.tv_usec - dtv.tv_usec;
            if (us >= 1000000) {
                fprintf(stderr, "\n[detect] source=%s denoise=%s %zux%zu stride=%zu frames=%d "
                        "max_dets=%d panics=%d last_n=%d",
                        source_name, denoise_mode_name(selected_denoise),
                        frame_width, frame_height, frame_stride,
                        frames, max_n, panics, n);
                if (n > 0) {
                    fprintf(stderr, "  det0: id=%llu m=%.1f center=(%.0f,%.0f) c0=(%.0f,%.0f)",
                            (unsigned long long)out[0].id, out[0].margin,
                            out[0].center[0], out[0].center[1],
                            out[0].corners[0], out[0].corners[1]);
                }
                if (have_decode_stats) {
                    fprintf(stderr,
                            "  decode: q=%zu hom=%zu pol=%zu code=%zu ok=%zu",
                            decode_stats.quads,
                            decode_stats.homography_rejects,
                            decode_stats.polarity_rejects,
                            decode_stats.codeword_rejects,
                            decode_stats.detections);
                    if (decode_stats.best_hamming >= 0) {
                        fprintf(stderr, " best_h=%d raw=%09llx",
                                decode_stats.best_hamming,
                                (unsigned long long)decode_stats.best_raw_code);
                    }
                    const apriltag_decode_candidate_t* largest_code = nullptr;
                    for (int i = 0; i < decode_candidate_count; ++i) {
                        const auto& candidate = decode_candidates[i];
                        if (candidate.status == 3 &&
                            (!largest_code ||
                             candidate.area > largest_code->area)) {
                            largest_code = &candidate;
                        }
                    }
                    if (largest_code) {
                        fprintf(stderr,
                                " large_code: id=%llu h=%d center=(%.0f,%.0f)"
                                " area=%.0f raw=%09llx",
                                (unsigned long long)largest_code->nearest_id,
                                largest_code->hamming,
                                largest_code->center[0],
                                largest_code->center[1],
                                largest_code->area,
                                (unsigned long long)largest_code->raw_code);
                    }
                }
                fprintf(stderr, "\n");
                dtv = now; frames = 0; max_n = 0; panics = 0;
            }
        }

        detections.clear();
        if (n > 0) {
            detections.assign(out.begin(), out.begin() + n);
        }

        cv::Mat &osd = k230_osd_begin(g_osd);
        if (use_lcd_fastpath) {
            // Plain CSI detections can be transformed while drawing, avoiding
            // a full-frame portrait rotation.
            draw_detections_lcd_90cw(osd, detections,
                                     frame_width, frame_height,
                                     display->height, display->width);
            draw_fps_stats(osd,
                           g_cam_fps.load(std::memory_order_relaxed),
                           g_det_fps.load(std::memory_order_relaxed));
        } else if (selected_debug_stage == 0) {
            if (selected_source == 1) {
                draw_camera_frame(osd, usb_bgr);
            }
            draw_detections(osd, detections, frame_width, frame_height);
            draw_fps_stats(osd,
                           g_cam_fps.load(std::memory_order_relaxed),
                           g_det_fps.load(std::memory_order_relaxed));
        } else {
            apriltag_debug_image_t debug_image = {};
            if (apriltag_get_debug_image(det, &debug_image) == 1 &&
                debug_image.stage == (uint32_t)selected_debug_stage) {
                // Copy the borrowed Rust image before the next detect call can
                // reuse its storage.
                draw_debug_image(osd, debug_image);
                if (selected_debug_stage == 4) {
                    draw_decode_candidates(
                        osd, debug_image, decode_candidates.data(),
                        (size_t)decode_candidate_count);
                }
            } else {
                osd.setTo(cv::Scalar(0, 0, 0, 255));
                cv::putText(osd, "waiting for pipeline debug image",
                            cv::Point(18, 32), cv::FONT_HERSHEY_SIMPLEX,
                            0.75, cv::Scalar(180, 105, 255, 255), 2);
            }
        }
        k230_osd_publish(g_osd);

        detect_frame_count += 1;
        if (csi_frame_held) {
            v4l2_drm_dump_release(&context);
        }
    }

    usb_capture.release();
    apriltag_free(det);
    v4l2_drm_stop(&context);
}

// ── DRM display heartbeat (per displayed frame) ─────────────────────────────
int frame_handler(struct v4l2_drm_context* context, bool displayed)
{
    static bool first_frame = true;
    if (first_frame) {
        {
            std::lock_guard<std::mutex> lock(start_mutex);
            display_ready = true;
            first_frame = false;
        }
        start_cv.notify_all();
    }

    static unsigned response = 0, display_frame_count = 0;
    response += 1;
    if (displayed) {
        k230_osd_on_frame(g_osd, true);
        if (display->osd_disp_buffer)
            osd_staged_count += 1;
        display_frame_count += 1;
    }

    // FPS counter.
    gettimeofday(&tv2, NULL);
    uint64_t duration = 1000000 * (tv2.tv_sec - tv.tv_sec) + tv2.tv_usec - tv.tv_usec;
    if (duration >= 1000000) {
        fprintf(stderr, " poll: %.2f, ", response * 1000000. / duration);
        response = 0;
        if (display) {
            fprintf(stderr, "display: %.2f, ", display_frame_count * 1000000. / duration);
            display_frame_count = 0;
        }
        double cam_fps = context[0].frame_count * 1000000. / duration;
        fprintf(stderr, "camera: %.2f, ", cam_fps);
        context[0].frame_count = 0;
        double det_fps = detect_frame_count * 1000000. / duration;
        fprintf(stderr, "detect: %.2f, ", det_fps);
        detect_frame_count = 0;
        fprintf(stderr, "osd: %.2f, ",
                osd_staged_count * 1000000. / duration);
        osd_staged_count = 0;
        fprintf(stderr, "drop: %llu",
                (unsigned long long)k230_osd_dropped_frames(g_osd));
        // Feed the on-screen "cam:/det:" HUD (draw_fps_stats(), drawn from
        // detect_proc) -- same numbers just printed above, no new work.
        g_cam_fps.store(cam_fps, std::memory_order_relaxed);
        g_det_fps.store(det_fps, std::memory_order_relaxed);
        fprintf(stderr, "          \r");
        fflush(stderr);
        gettimeofday(&tv, NULL);
    }

    if (display_stop) {
        return 'q';
    }
    return 0;
}

// ── Display thread ──────────────────────────────────────────────────────────
void display_proc(int video_device)
{
    pthread_setname_np(pthread_self(), "apriltag-disp");

    struct v4l2_drm_context context;
    v4l2_drm_default_context(&context);
    context.device = video_device;
    if (display->width > display->height) {
        // Landscape.
        context.width = display->width;
        context.height = (display->width * SENSOR_HEIGHT / SENSOR_WIDTH) & 0xfff8;
        context.video_format = V4L2_PIX_FMT_NV12;
        context.display_format = 0;
        context.drm_rotation = rotation_0;
    } else {
        // Portrait. Raw panel dimensions (NOT aspect-corrected against the
        // sensor) -- deliberately reverted back from an aspect-preserving
        // attempt (context.height derived from context.width the same way
        // the landscape branch above does). That version syntax-checked
        // clean but regressed on real hardware: detection corners landed
        // visibly off from the tag, plus a stray-colored band appeared at
        // one edge of the panel. Root cause not fully pinned down (most
        // likely the video plane's actual destination rect on the CRTC
        // isn't simply "post-rotation source size, top-left anchored" the
        // way this code assumed -- e.g. the driver may center or rescale a
        // source smaller than the plane's configured extent -- but that
        // needs dedicated on-hardware investigation, not another blind
        // guess). This raw-dimensions form (non-uniform stretch to fill
        // 800x480 exactly, sensor aspect 1280x720 doesn't match 800/480)
        // is the LAST CONFIRMED-WORKING geometry on real hardware, matched
        // by draw_detections_lcd_90cw()'s independent sx/sy scale factors
        // below -- restore both together, don't drift them apart again.
        // detect_proc()'s own capture (g_csi_width/height,
        // context.display=false) is entirely separate from this
        // display-only plane and is unaffected either way. See project
        // memory project_apriltag_demo_lcd_osd_rotation.md.
        context.width = display->height;
        context.height = display->width;
        context.video_format = V4L2_PIX_FMT_NV12;
        context.display_format = 0;
        context.drm_rotation = rotation_90;
    }
    if (v4l2_drm_setup(&context, 1, &display)) {
        cerr << "display: v4l2_drm_setup error" << endl;
        detect_stop.store(true);
        start_cv.notify_all();
        return;
    }
    struct v4l2_streamparm display_parm = {};
    display_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(context.video_fd, VIDIOC_G_PARM, &display_parm) == 0 &&
        display_parm.parm.capture.timeperframe.numerator != 0) {
        const auto& tpf = display_parm.parm.capture.timeperframe;
        fprintf(stderr, "[input] CSI display interval=%u/%u s (%.2f fps)\n",
                tpf.numerator, tpf.denominator,
                static_cast<double>(tpf.denominator) / tpf.numerator);
    }

    const bool landscape = display->width > display->height;
    k230_osd_config osd_config = {};
    osd_config.width = display->width;
    // Landscape OSD exactly matches the camera's 16:9 destination rectangle.
    osd_config.height = landscape ? context.height : display->height;
    osd_config.lcd_fastpath = !landscape;
    osd_config.mode = K230_OSD_MODE_FAST;
    g_osd = k230_osd_create(display, &osd_config);
    const struct display_buffer *front = k230_osd_front_buffer(g_osd);
    if (!g_osd || !front || display_commit_buffer(front, 0, 0) != 0) {
        cerr << "display: k230_osd setup error" << endl;
        k230_osd_destroy(g_osd);
        g_osd = nullptr;
        detect_stop.store(true);
        start_cv.notify_all();
        return;
    }

    print_key_help();
    gettimeofday(&tv, NULL);
    v4l2_drm_run_event_driven(&context, 1, frame_handler);

    if (display) {
        k230_osd_destroy(g_osd);
        g_osd = nullptr;
        display_exit(display);
    }
}

static void parse_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string scratch_error;
        const int scratch_option = parse_ccl_scratch_option(
            a,
#ifdef APRILTAG_C_BACKEND
            true,
#else
            false,
#endif
            g_local_ccl_scratch, scratch_error);
        if (scratch_option < 0) {
            cerr << scratch_error << endl;
            exit(2);
        } else if (scratch_option > 0) {
            continue;
        } else if (a == "--rvv") {
#ifdef APRILTAG_C_BACKEND
            cerr << "--rvv is only valid for apriltag_demo; "
                    "apriltag_c_demo uses the upstream C detector" << endl;
            exit(2);
#else
            g_mode = 1;
#endif
        } else if (a == "--debug") {
            g_debug_enabled = true;
#ifdef APRILTAG_C_BACKEND
        } else if (a == "--threads" && i + 1 < argc) {
            g_c_threads = atoi(argv[++i]);
            if (g_c_threads < 1) {
                cerr << "--threads must be at least 1" << endl;
                exit(2);
            }
        } else if (a == "--bits-corrected" && i + 1 < argc) {
            g_c_bits_corrected = atoi(argv[++i]);
            if (g_c_bits_corrected < 0 || g_c_bits_corrected > 2) {
                cerr << "--bits-corrected must be 0, 1, or 2" << endl;
                exit(2);
            }
        } else if (a == "--refine-edges") {
            g_c_refine_edges = 1;
        } else if (a == "--decode-sharpening" && i + 1 < argc) {
            char* end = nullptr;
            g_c_decode_sharpening = strtod(argv[++i], &end);
            if (!end || *end != '\0') {
                cerr << "--decode-sharpening must be numeric" << endl;
                exit(2);
            }
        } else if (a == "--upstream-defaults") {
            g_min_blob = 5;
            g_c_bits_corrected = 2;
            g_c_refine_edges = 1;
            g_c_decode_sharpening = 0.25;
#endif
        } else if (a == "--factor" && i + 1 < argc) {
            std::string f = argv[++i];
            if (f == "1" || f == "1.0") {
                g_factor_int = 0;
                g_factor_value = 1.0;
            } else if (f == "1.5") {
                g_factor_int = 1;
                g_factor_value = 1.5;
            } else if (f == "2" || f == "2.0") {
                g_factor_int = 2;
                g_factor_value = 2.0;
            } else {
                cerr << "--factor must be 1, 1.5, or 2" << endl;
                exit(2);
            }
        } else if (a == "--min-blob" && i + 1 < argc) {
            g_min_blob = (uint32_t)atoi(argv[++i]);
        } else if (a == "--csi-size" && i + 1 < argc) {
            unsigned width = 0, height = 0;
            char trailing = '\0';
            const char* size = argv[++i];
            int fields = sscanf(size, "%ux%u%c", &width, &height, &trailing);
            if (fields != 2) {
                fields = sscanf(size, "%uX%u%c", &width, &height, &trailing);
            }
            if (fields != 2 || width < 16 || height < 16 ||
                (width & 1) != 0 || (height & 1) != 0) {
                cerr << "--csi-size must be an even WxH, for example 640x480"
                     << endl;
                exit(2);
            }
            g_csi_width = width;
            g_csi_height = height;
        } else if (a == "--usb-video" && i + 1 < argc) {
            g_usb_video = atoi(argv[++i]);
            if (g_usb_video < 0) {
                cerr << "--usb-video must be a non-negative /dev/videoX number"
                     << endl;
                exit(2);
            }
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            exit(0);
        }
    }
}

int main(int argc, char* argv[])
{
#ifdef APRILTAG_C_BACKEND
    cout << "apriltag_c_demo (AprilTag C " APRILTAG_C_VERSION
            ") built at " << __DATE__ << " " << __TIME__ << endl;
#else
    cout << "apriltag_demo built at " << __DATE__ << " " << __TIME__ << endl;
#endif
    parse_args(argc, argv);
#ifdef APRILTAG_C_BACKEND
    cout << "backend=official-c"
         << " threads=" << g_c_threads
         << " bits_corrected=" << g_c_bits_corrected
         << " refine_edges=" << (g_c_refine_edges ? "on" : "off")
         << " decode_sharpening=" << g_c_decode_sharpening
#else
    cout << "mode=" << (g_mode ? "rvv" : "scalar")
         << " ccl_scratch=" << ccl_scratch_mode_name(g_local_ccl_scratch)
#endif
         << " factor=" << g_factor_value
         << " min_blob=" << g_min_blob
         << " debug=" << (g_debug_enabled ? "on" : "off")
         << " input=CSI"
         << " csi_request=" << g_csi_width << "x" << g_csi_height;
    if (g_usb_video >= 0) {
        cout << " usb=/dev/video" << g_usb_video;
    }
    cout << endl;

    signal(SIGUSR1, handle_view_cycle_signal);

    display = display_init(0);
    if (!display) {
        cerr << "display_init error, exit" << endl;
        return -1;
    }
    fprintf(stderr, "[display] selected mode %ux%u@%u\n",
            display->width, display->height, display->mode.vrefresh);
    uint16_t sensor_width = 0, sensor_height = 0;
    uint32_t sensor_fps = 0;
    int sensor_mode_result = v4l2_drm_request_sensor_mode(
        kd_mpi_get_vvcam_video00(),
        1280, 720, 60,
        1920, 1080, 30,
        &sensor_width, &sensor_height, &sensor_fps);
    if (sensor_mode_result < 0) {
        cerr << "apriltag_demo: active camera supports neither "
             << "1280x720@60 nor 1920x1080@30" << endl;
        display_exit(display);
        return -1;
    }
    fprintf(stderr, "[input] sensor selected %ux%u@%u%s\n",
            sensor_width, sensor_height, sensor_fps,
            sensor_mode_result == 0 ? " (preferred)" : " (fallback)");

    if (v4l2_drm_set_luma_only(kd_mpi_get_vvcam_video00(), true) < 0)
        fprintf(stderr, "[input] luma-only unavailable, keeping color path\n");

    std::thread detect_thread(detect_proc, kd_mpi_get_vvcam_video00() + 1);
    std::thread display_thread(display_proc, kd_mpi_get_vvcam_video00());

    print_key_help();
    TerminalRawMode terminal_mode;
    while (true) {
        char key = '\0';
        ssize_t got = read(STDIN_FILENO, &key, 1);
        if (got <= 0) {
            usleep(100000);
            continue;
        }
        if (key == 'q' || key == 'Q') {
            // If dumping is enabled, capture the currently-framed frame before
            // tearing down (wait up to ~1s for the detect thread to write it).
            if (g_debug_enabled && getenv("APRILTAG_DEMO_DUMP")) {
                dump_request.store(true);
                for (int i = 0; i < 20 && dump_request.load(); ++i) {
                    usleep(50000);
                }
            }
            detect_stop.store(true);
            break;
        } else if (key >= '0' && key <= '5') {
#ifdef APRILTAG_C_BACKEND
            cerr << "\nlive pipeline views are available in apriltag_demo; "
                    "use --debug to dump one set of upstream C images" << endl;
#else
            if (!g_debug_enabled) {
                cerr << "\npipeline views are disabled; restart with --debug"
                     << endl;
            } else {
                int stage = key - '0';
                g_debug_stage.store(stage);
                cout << "\nview " << stage << ": "
                     << apriltag_debug_stage_name(stage) << endl;
            }
#endif
        } else if (key == 'c' || key == 'C') {
            g_input_source.store(0);
            cout << "\ninput: CSI camera" << endl;
        } else if (key == 'u' || key == 'U') {
            if (g_usb_video < 0) {
                cerr << "\nUSB camera not configured; restart with "
                        "--usb-video X" << endl;
            } else {
                g_input_source.store(1);
                cout << "\ninput: USB /dev/video" << g_usb_video << endl;
            }
        } else if (key == 'n' || key == 'N') {
            const int next_mode = (g_denoise_mode.load() + 1) % 3;
            g_denoise_mode.store(next_mode);
            cout << "\nluma denoise: " << denoise_mode_name(next_mode) << endl;
        }
    }

    detect_thread.join();
    display_stop.store(true);
    display_thread.join();
    return 0;
}
