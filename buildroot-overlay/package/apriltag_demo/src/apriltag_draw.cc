/* OSD overlay drawing for apriltag_demo — mirrors live_demo.rs overlays. */
#include "apriltag_draw.h"
#include <algorithm>

// ARGB (CV_8UC4) colors in OpenCV BGRA order.
static const cv::Scalar kGreen (0,   255, 0,   255);
static const cv::Scalar kBlue  (255, 0,   0,   255);
static const cv::Scalar kRed   (0,   0,   255, 255);
static const cv::Scalar kYellow(0,   255, 255, 255);
static const cv::Scalar kMagenta(255, 0, 255, 255);

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
    case 6: return "recovery";
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
    cv::putText(osd, label, cv::Point(18, 32),
                 cv::FONT_HERSHEY_SIMPLEX, 0.75, kYellow, 2, cv::LINE_8, false);
}

void draw_recovery_stats(cv::Mat& osd,
                         const apriltag_recovery_stats_t& stats)
{
    if (osd.empty()) {
        return;
    }
    char label[160];
    snprintf(label, sizeof(label),
             "D total/eligible/ok=%llu/%llu/%llu  "
             "E=%llu/%llu/%llu  trials=%llu erasures=%llu",
             (unsigned long long)stats.group_d_total,
             (unsigned long long)stats.group_d_eligible,
             (unsigned long long)stats.group_d_success,
             (unsigned long long)stats.group_e_total,
             (unsigned long long)stats.group_e_eligible,
             (unsigned long long)stats.group_e_success,
             (unsigned long long)stats.trials_attempted,
             (unsigned long long)stats.erasure_decodes);
    cv::putText(osd, label, cv::Point(18, osd.rows - 18),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, kYellow, 1,
                cv::LINE_8, false);
    if (stats.group_d_eligible + stats.group_e_eligible == 0) {
        cv::putText(osd, "No eligible recovery candidates", cv::Point(18, 64),
                    cv::FONT_HERSHEY_SIMPLEX, 0.65, kYellow, 2,
                    cv::LINE_8, false);
    }
}

void draw_recovery_candidates(
    cv::Mat& osd, int source_width, int source_height,
    const apriltag_recovery_candidate_t* candidates, size_t count)
{
    if (osd.empty() || !candidates || source_width <= 0 || source_height <= 0) {
        return;
    }
    const cv::Size view = aspect_fit(osd, source_width, source_height);
    const double sx = (double)view.width / source_width;
    const double sy = (double)view.height / source_height;
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = candidates[i];
        char label[96];
        const char* result = candidate.result == 2 ? "ok" :
                             candidate.result == 1 ? "fail" : "skip";
        if (candidate.id != UINT64_MAX) {
            snprintf(label, sizeof(label),
                     "%c L%u %s%s %s id=%llu e=%u/%u",
                     candidate.group == 1 ? 'D' : 'E',
                     candidate.strong_line_count,
                     candidate.eligible ? "eligible" : "ineligible",
                     candidate.selected ? " selected" : "",
                     result, (unsigned long long)candidate.id,
                     candidate.errors, candidate.erasures);
        } else {
            snprintf(label, sizeof(label), "%c L%u %s%s %s",
                     candidate.group == 1 ? 'D' : 'E',
                     candidate.strong_line_count,
                     candidate.eligible ? "eligible" : "ineligible",
                     candidate.selected ? " selected" : "", result);
        }
        cv::Point anchor((int)(candidate.anchor[0] * sx),
                         (int)(candidate.anchor[1] * sy));
        anchor.x = std::clamp(anchor.x + 5, 0, std::max(0, osd.cols - 1));
        anchor.y = std::clamp(anchor.y - 5, 14, std::max(14, osd.rows - 1));
        cv::putText(osd, label, anchor, cv::FONT_HERSHEY_SIMPLEX, 0.42,
                    candidate.result == 2 ? kMagenta : kYellow, 1,
                    cv::LINE_8, false);
    }
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
        if (d.recovered) {
            for (int i = 0; i < 4; ++i) {
                cv::line(osd, p[i], p[(i + 1) % 4], kMagenta, 2,
                         cv::LINE_8, 0);
            }
        } else {
            // Quad edges (color-coded like live_demo / opencv_demo).
            cv::line(osd, p[0], p[1], kGreen, 2, cv::LINE_8, 0);
            cv::line(osd, p[0], p[3], kBlue,  2, cv::LINE_8, 0);
            cv::line(osd, p[1], p[2], kRed,   2, cv::LINE_8, 0);
            cv::line(osd, p[2], p[3], kRed,   2, cv::LINE_8, 0);
        }

        int cx = (int)(d.center[0] * s);
        int cy = (int)(d.center[1] * s);
        char label[64];
        snprintf(label, sizeof(label), "%sid=%llu m=%.1f",
                  d.recovered ? "R " : "",
                  (unsigned long long)d.id, d.margin);
        cv::putText(osd, label, cv::Point(cx - 40, cy),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    d.recovered ? kMagenta : kYellow, 2, cv::LINE_8, false);
    }
}
