/* OSD overlay drawing for apriltag_demo — mirrors live_demo.rs overlays. */
#include "apriltag_draw.h"
#include <algorithm>

// ARGB (CV_8UC4) colors in OpenCV BGRA order.
static const cv::Scalar kGreen (0,   255, 0,   255);
static const cv::Scalar kBlue  (255, 0,   0,   255);
static const cv::Scalar kRed   (0,   0,   255, 255);
static const cv::Scalar kYellow(0,   255, 255, 255);
static const cv::Scalar kPink  (180, 105, 255, 255);  // "hot pink" -- more readable than yellow for the top-left debug-view status text

static cv::Size aspect_fit(const cv::Mat& osd, int width, int height)
{
    int view_w = osd.cols;
    int view_h = (int)((uint64_t)view_w * height / width);
    if (view_h > osd.rows) {
        view_h = osd.rows;
        view_w = (int)((uint64_t)view_h * width / height);
    }
    return cv::Size(std::max(1, view_w), std::max(1, view_h));
}

const char* apriltag_debug_stage_name(int stage)
{
    switch (stage) {
    case 0: return "camera + detections";
    case 1: return "decimated grayscale";
    case 2: return "adaptive threshold";
    case 3: return "boundary clusters";
    case 4: return "fitted quads";
    case 5: return "decoded detections";
    default: return "unknown";
    }
}

void draw_debug_image(cv::Mat& osd, const apriltag_debug_image_t& image)
{
    if (osd.empty() || !image.pixels || image.width == 0 || image.height == 0 ||
        image.stride < image.width * 3) {
        return;
    }

    // Make the OSD opaque so it replaces the camera plane while debugging.
    osd.setTo(cv::Scalar(0, 0, 0, 255));

    cv::Mat rgb((int)image.height, (int)image.width, CV_8UC3,
                const_cast<uint8_t*>(image.pixels), image.stride);
    cv::Size view = aspect_fit(osd, (int)image.width, (int)image.height);

    cv::Mat scaled_rgb;
    cv::resize(rgb, scaled_rgb, view, 0, 0, cv::INTER_NEAREST);
    cv::Mat scaled_bgra;
    cv::cvtColor(scaled_rgb, scaled_bgra, cv::COLOR_RGB2BGRA);
    scaled_bgra.copyTo(osd(cv::Rect(0, 0, view.width, view.height)));

    char label[96];
    if (image.stage >= 3) {
        snprintf(label, sizeof(label), "%u: %s  count=%zu",
                 image.stage, apriltag_debug_stage_name((int)image.stage),
                 image.item_count);
    } else {
        snprintf(label, sizeof(label), "%u: %s",
                 image.stage, apriltag_debug_stage_name((int)image.stage));
    }
    // Stage 2 ("adaptive threshold") gets its own label color (blue) by
    // request; every other stage keeps the pink used everywhere else.
    const cv::Scalar& label_color = (image.stage == 2) ? kBlue : kPink;
    cv::putText(osd, label, cv::Point(18, 32),
                cv::FONT_HERSHEY_SIMPLEX, 0.75, label_color, 2, cv::LINE_8, false);
}

void draw_decode_candidates(cv::Mat& osd,
                            const apriltag_debug_image_t& image,
                            const apriltag_decode_candidate_t* candidates,
                            size_t count)
{
    if (osd.empty() || !candidates || count == 0 ||
        image.width == 0 || image.height == 0) {
        return;
    }

    cv::Size view = aspect_fit(osd, (int)image.width, (int)image.height);
    const double sx = (double)view.width / (double)image.width;
    const double sy = (double)view.height / (double)image.height;

    std::vector<size_t> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // Suppress tiny background quads, then keep the largest candidates so
        // the labels remain readable in cluttered scenes.
        if (candidates[i].area >= 64.0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return candidates[a].area > candidates[b].area;
    });
    if (order.size() > 24) {
        order.resize(24);
    }

    for (size_t index : order) {
        const auto& candidate = candidates[index];
        cv::Scalar color = kRed;
        char label[64];
        if (candidate.status == 0) {
            color = kGreen;
            snprintf(label, sizeof(label), "OK id=%llu",
                     (unsigned long long)candidate.nearest_id);
        } else if (candidate.status == 1) {
            color = kBlue;
            snprintf(label, sizeof(label), "H");
        } else if (candidate.status == 2) {
            color = kYellow;
            snprintf(label, sizeof(label), "P");
        } else if (candidate.hamming >= 0 &&
                   candidate.nearest_id != UINT64_MAX) {
            snprintf(label, sizeof(label), "id=%llu h=%d",
                     (unsigned long long)candidate.nearest_id,
                     candidate.hamming);
        } else {
            snprintf(label, sizeof(label), "C");
        }

        cv::Point center((int)(candidate.center[0] * sx),
                         (int)(candidate.center[1] * sy));
        cv::drawMarker(osd, center, color, cv::MARKER_CROSS, 9, 2);
        cv::putText(osd, label, center + cv::Point(5, -5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, color, 1,
                    cv::LINE_8, false);
    }
}

void draw_camera_frame(cv::Mat& osd, const cv::Mat& bgr)
{
    if (osd.empty() || bgr.empty() || bgr.type() != CV_8UC3) {
        return;
    }

    osd.setTo(cv::Scalar(0, 0, 0, 255));
    cv::Size view = aspect_fit(osd, bgr.cols, bgr.rows);
    cv::Mat scaled_bgr;
    cv::resize(bgr, scaled_bgr, view, 0, 0, cv::INTER_LINEAR);
    cv::Mat scaled_bgra;
    cv::cvtColor(scaled_bgr, scaled_bgra, cv::COLOR_BGR2BGRA);
    scaled_bgra.copyTo(osd(cv::Rect(0, 0, view.width, view.height)));
}

void draw_detections(cv::Mat& osd,
                     const std::vector<apriltag_det_t>& dets,
                     int sensor_w, int sensor_h)
{
    if (osd.empty() || sensor_w <= 0 || sensor_h <= 0) {
        return;
    }
    // Use the same aspect-fit transform as the USB preview/debug images.
    const cv::Size view = aspect_fit(osd, sensor_w, sensor_h);
    const double s = (double)view.width / (double)sensor_w;

    for (const auto& d : dets) {
        cv::Point p[4];
        for (int i = 0; i < 4; ++i) {
            p[i] = cv::Point((int)(d.corners[2 * i]     * s),
                             (int)(d.corners[2 * i + 1] * s));
        }
        // Quad edges (color-coded like live_demo / opencv_demo).
        cv::line(osd, p[0], p[1], kGreen, 2, cv::LINE_8, 0);
        cv::line(osd, p[0], p[3], kBlue,  2, cv::LINE_8, 0);
        cv::line(osd, p[1], p[2], kRed,   2, cv::LINE_8, 0);
        cv::line(osd, p[2], p[3], kRed,   2, cv::LINE_8, 0);

        int cx = (int)(d.center[0] * s);
        int cy = (int)(d.center[1] * s);
        char label[64];
        snprintf(label, sizeof(label), "id=%llu m=%.1f",
                 (unsigned long long)d.id, d.margin);
        cv::putText(osd, label, cv::Point(cx - 40, cy),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, kYellow, 2, cv::LINE_8, false);
    }
}

// ── LCD-only fast path: draw already rotated, so the hardware blend stays
// free ───────────────────────────────────────────────────────────────────
//
// BACKGROUND (K230 01studio LCD investigation, 2026-08-22 -- see project
// memory project_apriltag_demo_lcd_osd_rotation.md for the full writeup):
//
// The 01studio's ST7701 panel is a physically PORTRAIT-shaped module
// (480x800 native pixel buffer) that is mounted SIDEWAYS in the enclosure
// to present a landscape view -- confirmed with the hardware owner, not
// guessed. So a 90-degree rotation between "landscape sensor/detector
// space" and "panel pixel space" is a real, unavoidable requirement, not
// an arbitrary software choice; it cannot simply be dropped.
//
// The live camera plane already gets this rotation for free, in K230 VO
// hardware: display_proc()'s v4l2_drm_context sets drm_rotation=rotation_90
// for portrait displays, and display.c's display_get_plane() specifically
// searches for the one VO plane that exposes a DRM "rotation" property
// (drivers/gpu/drm/canaan/canaan_vo.c's static plane table: only
// VO_LAYER1/"video_3" has non-zero supported_rotations -- every OSD/ARGB
// plane has it hardwired to 0) and fails setup outright if it can't find
// it, so this path is confirmed working, not a maybe.
//
// The detection overlay is a SEPARATE ARGB OSD plane, and no ARGB-capable
// plane on this SoC supports hardware rotation -- so there is no "give the
// overlay a rotation-capable plane too" option. Two other hardware
// avenues were considered and rejected before landing on the approach
// below:
//   - AI2D affine transform: real hardware, genuinely supports arbitrary
//     affine transforms (including rotation) via ai2d_affine_param_t::M,
//     but its format list (nncase's gnne_tile_utils.h: YUV420_NV12/NV21/
//     I420, NCHW_FMT, RGB_packed, RAW16) has no ARGB/alpha format, so using
//     it would need a format-conversion bridge and would lose true alpha
//     blending for no real gain once that conversion cost is counted.
//   - Burning the overlay into the camera's own (already hardware-rotated)
//     NV12 buffer: rejected because the video plane free-runs continuously
//     at full camera fps, decoupled from detection rate, while the overlay
//     today is deliberately only redrawn when a NEW detection is ready
//     (see g_overlay_generation in main.cc). Merging them would force an
//     overlay-write on every camera frame instead of every detection --
//     very likely a net regression, not an improvement -- and/or introduce
//     real display latency by gating fresh camera frames on overlay
//     readiness.
//
// The actual fix needs no new hardware at all: the CPU has to rasterize
// the overlay either way (there is no vector-graphics hardware here), so
// the only real waste was rotating the FINISHED, ALREADY-RASTERIZED image
// afterward -- a full-frame allocate + clear + copy + cv::rotate() (an
// inherently cache-hostile transpose over the whole buffer) + a final
// memcpy, every time a new detection was ready, versus landscape's single
// direct memcpy. Computing each detection's already-rotated position while
// drawing is O(detections); rotating the rasterized image is O(pixels).
// Since we're drawing the points ourselves anyway, we can just draw them
// in the right place the first time.
//
// One deliberate accuracy trade-off: cv::putText has no rotation
// parameter, so it can only draw glyphs upright in osd_portrait's own
// pixel grid -- which, because the panel is physically sideways, means
// detection ID/margin labels read top-to-bottom to the viewer instead of
// left-to-right. This is an accepted product decision (label READING
// DIRECTION doesn't matter), not an oversight: only label POSITION and
// the quad corners' POSITIONS need to be geometrically correct in the
// viewer's frame (so a box visibly lands on its tag), and the point
// rotation above already guarantees that for both. Getting glyphs
// upright too would need drawing each label into its own small scratch
// buffer and rotating that -- real, if small, added allocate/rotate cost
// this function has no need to pay.
//
// This function is a deliberate near-duplicate of draw_detections(), not a
// parameterized variant of it: draw_detections() itself is completely
// unmodified, still used exactly as before by landscape/HDMI displays
// (which never call this function -- main.cc only reaches it when
// display->width < display->height), and by --debug/USB-camera-source
// modes even on a portrait LCD (draw_debug_image()/draw_decode_candidates()/
// draw_camera_frame() still only know how to draw landscape, so those
// modes keep using the original draw-then-rotate path in main.cc's
// frame_handler, unchanged).

/* Maps a point's position in the equivalent LANDSCAPE canvas's pixel space
 * to where that same point lands after rotating that whole canvas 90
 * degrees CLOCKWISE -- i.e. algebraically what cv::rotate(src, dst,
 * ROTATE_90_CLOCKWISE) does to pixels, but applied to one point instead of
 * a whole image.
 *
 * Derivation: under a clockwise rotation, the source image's TOP edge
 * becomes the rotated image's RIGHT edge -- concretely, the point sitting
 * at the source's top-left corner (0, 0) ends up at the rotated image's
 * top-RIGHT corner. Working that through for a general point (x, y) in a
 * `landscape_h`-tall source:
 *
 *     rotated_x = landscape_h - 1 - y
 *     rotated_y = x
 *
 * which also explains why the rotated canvas ends up landscape_h pixels
 * wide and landscape_w pixels tall -- exactly draw_buffer's real
 * (portrait) dimensions on this panel: x ranges over [0, landscape_w), so
 * rotated_y (=x) does too, and y ranges over [0, landscape_h), so
 * rotated_x (=landscape_h-1-y) does too.
 */
static cv::Point lcd_rotate_90cw_point(double x, double y, int landscape_h)
{
    return cv::Point((int)(landscape_h - 1 - y), (int)x);
}

void draw_detections_lcd_90cw(cv::Mat& osd_portrait,
                              const std::vector<apriltag_det_t>& dets,
                              int sensor_w, int sensor_h,
                              int landscape_w, int landscape_h)
{
    if (osd_portrait.empty() || sensor_w <= 0 || sensor_h <= 0 ||
        landscape_w <= 0 || landscape_h <= 0) {
        return;
    }
    // Independent per-axis scale, matching display_proc()'s portrait
    // branch, which sets the camera video plane's context.width/height
    // straight to the panel's raw dimensions (display->height/
    // display->width, e.g. 800x480) -- a non-uniform stretch, since sensor
    // aspect (1280x720, 16:9 ~= 1.778) doesn't match 800/480 = 1.667. An
    // aspect-preserving version of both this function and display_proc()'s
    // sizing was tried (a single uniform scale, matching
    // draw_detections()'s own aspect_fit() philosophy) but regressed on
    // real hardware -- see the long comment on display_proc()'s portrait
    // branch in main.cc for what broke and why it was reverted. This sx/sy
    // form is the LAST CONFIRMED-WORKING geometry on real hardware; don't
    // drift it out of sync with display_proc()'s context.width/height
    // again without re-testing on the actual panel.
    const double sx = (double)landscape_w / (double)sensor_w;
    const double sy = (double)landscape_h / (double)sensor_h;

    for (const auto& d : dets) {
        cv::Point p[4];
        for (int i = 0; i < 4; ++i) {
            p[i] = lcd_rotate_90cw_point(d.corners[2 * i] * sx,
                                         d.corners[2 * i + 1] * sy,
                                         landscape_h);
        }
        // Quad edges: unlike text (below), a line has no inherent "up", so
        // rotating its two rotated endpoints is the entire fix -- nothing
        // else about how it's drawn needs to change.
        cv::line(osd_portrait, p[0], p[1], kGreen, 2, cv::LINE_8, 0);
        cv::line(osd_portrait, p[0], p[3], kBlue,  2, cv::LINE_8, 0);
        cv::line(osd_portrait, p[1], p[2], kRed,   2, cv::LINE_8, 0);
        cv::line(osd_portrait, p[2], p[3], kRed,   2, cv::LINE_8, 0);

        cv::Point c = lcd_rotate_90cw_point(d.center[0] * sx, d.center[1] * sy, landscape_h);
        char label[64];
        snprintf(label, sizeof(label), "id=%llu m=%.1f",
                 (unsigned long long)d.id, d.margin);

        // Text glyph orientation is intentionally NOT corrected here, by
        // explicit product decision: cv::putText always draws glyphs
        // upright in the DESTINATION buffer's own pixel grid (no rotation
        // parameter exists -- see the long discussion this is drawn from),
        // so on this physically-sideways-mounted panel the label reads
        // top-to-bottom/bottom-to-top to the viewer rather than left-to-
        // right. That's accepted: only the label's POSITION needs to track
        // the (still fully rotated, see `c` above) detection, not its
        // reading direction. This keeps text drawing exactly as cheap as
        // draw_detections()'s -- one direct putText call, no scratch
        // buffer, no rotate, no mask-composite -- unlike the quad lines
        // above, whose CORNER POSITIONS do have to be geometrically
        // correct in the viewer's frame (a box has to visually land on the
        // tag), which the point rotation already guarantees on its own.
        cv::putText(osd_portrait, label, cv::Point(c.x - 40, c.y),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, kYellow, 2, cv::LINE_8, false);
    }
}

void draw_fps_stats(cv::Mat& osd, double cam_fps, double det_fps)
{
    char label[64];
    snprintf(label, sizeof(label), "cam: %.2f, det: %.2f", cam_fps, det_fps);
    cv::putText(osd, label, cv::Point(12, osd.rows - 12),
                cv::FONT_HERSHEY_SIMPLEX, 0.65, kYellow, 2, cv::LINE_8, false);
}
