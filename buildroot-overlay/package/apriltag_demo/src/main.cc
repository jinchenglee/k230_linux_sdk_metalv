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
 * Milestone 1: apriltag_detect() is a brightness-centroid skeleton, so this
 * shows a box tracking the brightest region. Milestone 2 swaps in real
 * detect() with no change to this file.
 */
#include <iostream>
#include <thread>
#include <string>
#include <cstring>

#include "sensor_set.h"
#include "apriltag.h"
#include "apriltag_draw.h"

using std::cerr;
using std::cout;
using std::endl;

// ── Config (from argv) ──────────────────────────────────────────────────────
static int    g_factor_int   = 2;     // FFI: 0=1.0 1=1.5 2=2.0
static double g_decimate_scale = 2.0;
static int    g_mode         = 0;     // FFI: 0=scalar 1=rvv
static uint32_t g_min_blob   = 25;

#define MAX_DETS 64

// ── Shared state ────────────────────────────────────────────────────────────
static std::mutex result_mutex;
static std::vector<apriltag_det_t> detections;
std::atomic<bool> detect_stop(false);
std::atomic<bool> display_stop(false);
std::atomic<bool> dump_request(false);  // set on 'q' to dump the current frame
static volatile unsigned detect_frame_count = 0;

static struct timeval tv, tv2;
static struct display* display;
struct display_buffer* draw_buffer;
cv::Mat draw_frame;

static void print_usage(const char* name)
{
    cout << "Usage: " << name << " [--rvv] [--factor 1|1.5|2] [--min-blob N]" << endl
         << "  --rvv         use RVV kernels (default: scalar)" << endl
         << "  --factor      decimation factor 1.0/1.5/2.0 (default: 2.0)" << endl
         << "  --min-blob    minimum blob size (default: 25)" << endl
         << "  q<enter>      quit" << endl;
}

// ── Detection thread ────────────────────────────────────────────────────────
// NV12 capture; the Y-plane is fed directly to apriltag_detect() as grayscale.
static void detect_proc(int video_device)
{
    struct v4l2_drm_context context;
    #define DETECT_BUFFER_NUM 3

    // Wait for display_proc to bring up draw_frame (main holds result_mutex
    // until the first displayed frame; see frame_handler).
    result_mutex.lock();
    result_mutex.unlock();

    v4l2_drm_default_context(&context);
    context.device = video_device;
    context.display = false;
    context.width = SENSOR_WIDTH;
    context.height = SENSOR_HEIGHT;
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

    void* det = apriltag_new(g_min_blob);
    if (!det) {
        cerr << "detect: apriltag_new failed" << endl;
        v4l2_drm_stop(&context);
        return;
    }
    std::vector<apriltag_det_t> out(MAX_DETS);

    while (!detect_stop) {
        int ret = v4l2_drm_dump(&context, 1000);
        if (ret) {
            perror("detect: v4l2_drm_dump error");
            continue;
        }
        // NV12 buffer: Y-plane first, stride == width (ISP output, 1280 aligned).
        const uint8_t* y =
            (const uint8_t*)context.buffers[context.vbuffer.index].mmap;

        // Dump the exact grayscale we feed detect() (packed width*height,
        // stride-corrected) when requested — triggered by pressing 'q' so the
        // tag is framed. Set APRILTAG_DEMO_DUMP=path to enable.
        if (dump_request.load()) {
            const char* dump_path = getenv("APRILTAG_DEMO_DUMP");
            if (dump_path) {
                FILE* f = fopen(dump_path, "wb");
                if (f) {
                    for (unsigned r = 0; r < context.height; ++r) {
                        fwrite(y + (size_t)r * context.width, 1, context.width, f);
                    }
                    fclose(f);
                    fprintf(stderr, "\n[dump] wrote %ux%u Y-plane (%u bytes) to %s\n",
                            context.width, context.height,
                            context.width * context.height, dump_path);
                }
            }
            dump_request.store(false);
        }
        int n = apriltag_detect(det, y, context.width, context.height,
                                context.width, g_factor_int, g_mode,
                                out.data(), MAX_DETS);

        // ── Diagnostics (throttled ~1/s): distinguish panic (-1) from
        //    "no detections" (0) from "detected but not drawn" (>0). ──
        {
            static struct timeval dtv = {0, 0};
            static int frames = 0, max_n = 0, panics = 0;
            struct timeval now; gettimeofday(&now, NULL);
            if (dtv.tv_sec == 0) dtv = now;
            frames++;
            if (n < 0) panics++;
            if (n > max_n) max_n = n;
            uint64_t us = 1000000ULL * (now.tv_sec - dtv.tv_sec) + now.tv_usec - dtv.tv_usec;
            if (us >= 1000000) {
                fprintf(stderr, "\n[detect] frames=%d max_dets=%d panics=%d last_n=%d",
                        frames, max_n, panics, n);
                if (n > 0) {
                    fprintf(stderr, "  det0: id=%llu m=%.1f center=(%.0f,%.0f) c0=(%.0f,%.0f)",
                            (unsigned long long)out[0].id, out[0].margin,
                            out[0].center[0], out[0].center[1],
                            out[0].corners[0], out[0].corners[1]);
                }
                fprintf(stderr, "\n");
                dtv = now; frames = 0; max_n = 0; panics = 0;
            }
        }

        result_mutex.lock();
        detections.clear();
        if (n > 0) {
            detections.assign(out.begin(), out.begin() + n);
        }
        draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
        draw_detections(draw_frame, detections, g_decimate_scale,
                        SENSOR_WIDTH, SENSOR_HEIGHT);
        result_mutex.unlock();

        detect_frame_count += 1;
        v4l2_drm_dump_release(&context);
    }

    apriltag_free(det);
    v4l2_drm_stop(&context);
}

// ── DRM display heartbeat (per displayed frame) ─────────────────────────────
// Copies the annotated OSD into the display buffer. Cloned from face_detect.
int frame_handler(struct v4l2_drm_context* context, bool displayed)
{
    static bool first_frame = true;
    if (first_frame) {
        result_mutex.unlock();  // release detect_proc once display is up
        first_frame = false;
    }

    static unsigned response = 0, display_frame_count = 0;
    response += 1;
    if (displayed) {
        if (context[0].buffer_hold[context[0].wp] >= 0) {
            static struct display_buffer* last_drawed_buffer = nullptr;
            auto buffer =
                context[0].display_buffers[context[0].buffer_hold[context[0].wp]];
            if (buffer != last_drawed_buffer) {
                if (draw_buffer->width > draw_buffer->height) {
                    // Landscape.
                    cv::Mat temp_img(draw_buffer->height, draw_buffer->width, CV_8UC4);
                    temp_img.setTo(cv::Scalar(0, 0, 0, 0));
                    result_mutex.lock();
                    draw_frame.copyTo(temp_img);
                    result_mutex.unlock();
                    memcpy(draw_buffer->map, temp_img.data, draw_buffer->size);
                } else {
                    // Portrait (e.g. ST7701): draw landscape, then rotate.
                    cv::Mat temp_img(draw_buffer->width, draw_buffer->height, CV_8UC4);
                    temp_img.setTo(cv::Scalar(0, 0, 0, 0));
                    result_mutex.lock();
                    draw_frame.copyTo(temp_img);
                    result_mutex.unlock();
                    cv::rotate(temp_img, temp_img, cv::ROTATE_90_CLOCKWISE);
                    memcpy(draw_buffer->map, temp_img.data, draw_buffer->size);
                }
                last_drawed_buffer = buffer;
                thead_csi_dcache_clean_invalid_range(draw_buffer->map, draw_buffer->size);
                display_update_buffer(draw_buffer, 0, 0);
            }
        }
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
        fprintf(stderr, "camera: %.2f, ", context[0].frame_count * 1000000. / duration);
        context[0].frame_count = 0;
        fprintf(stderr, "detect: %.2f", detect_frame_count * 1000000. / duration);
        detect_frame_count = 0;
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
        // Portrait.
        context.width = display->height;
        context.height = display->width;
        context.video_format = V4L2_PIX_FMT_NV12;
        context.display_format = 0;
        context.drm_rotation = rotation_90;
    }
    if (v4l2_drm_setup(&context, 1, &display)) {
        cerr << "display: v4l2_drm_setup error" << endl;
        return;
    }

    struct display_plane* plane = display_get_plane(display, DRM_FORMAT_ARGB8888);
    draw_buffer = display_allocate_buffer(plane, display->width, display->height);
    display_commit_buffer(draw_buffer, 0, 0);

    if (draw_buffer->width > draw_buffer->height) {
        draw_frame = cv::Mat(draw_buffer->height, draw_buffer->width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    } else {
        draw_frame = cv::Mat(draw_buffer->width, draw_buffer->height, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    }

    cout << "press 'q'<enter> to exit" << endl;
    gettimeofday(&tv, NULL);
    v4l2_drm_run(&context, 1, frame_handler);

    if (display) {
        display_free_plane(plane);
        display_exit(display);
    }
}

static void parse_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rvv") {
            g_mode = 1;
        } else if (a == "--factor" && i + 1 < argc) {
            std::string f = argv[++i];
            if (f == "1" || f == "1.0")      { g_factor_int = 0; g_decimate_scale = 1.0; }
            else if (f == "1.5")             { g_factor_int = 1; g_decimate_scale = 1.5; }
            else                             { g_factor_int = 2; g_decimate_scale = 2.0; }
        } else if (a == "--min-blob" && i + 1 < argc) {
            g_min_blob = (uint32_t)atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            exit(0);
        }
    }
}

int main(int argc, char* argv[])
{
    cout << "apriltag_demo built at " << __DATE__ << " " << __TIME__ << endl;
    parse_args(argc, argv);
    cout << "mode=" << (g_mode ? "rvv" : "scalar")
         << " factor=" << g_decimate_scale
         << " min_blob=" << g_min_blob << endl;

    display = display_init(0);
    if (!display) {
        cerr << "display_init error, exit" << endl;
        return -1;
    }

    // Hold until the first displayed frame unlocks detect_proc.
    result_mutex.lock();

    std::thread detect_thread(detect_proc, kd_mpi_get_vvcam_video00() + 1);
    std::thread display_thread(display_proc, kd_mpi_get_vvcam_video00());

    cout << "type 'q'<enter> to quit" << endl;
    std::string input;
    while (true) {
        std::getline(std::cin, input);
        if (input == "q") {
            // If dumping is enabled, capture the currently-framed frame before
            // tearing down (wait up to ~1s for the detect thread to write it).
            if (getenv("APRILTAG_DEMO_DUMP")) {
                dump_request.store(true);
                for (int i = 0; i < 20 && dump_request.load(); ++i) {
                    usleep(50000);
                }
            }
            display_stop.store(true);
            usleep(100000);
            detect_stop.store(true);
            break;
        } else {
            usleep(100000);
        }
    }

    display_thread.join();
    detect_thread.join();
    return 0;
}
