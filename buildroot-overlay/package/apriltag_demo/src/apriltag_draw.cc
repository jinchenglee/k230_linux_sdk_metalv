/* OSD overlay drawing for apriltag_demo — mirrors live_demo.rs overlays. */
#include "apriltag_draw.h"

// ARGB (CV_8UC4) colors in OpenCV BGRA order.
static const cv::Scalar kGreen (0,   255, 0,   255);
static const cv::Scalar kBlue  (255, 0,   0,   255);
static const cv::Scalar kRed   (0,   0,   255, 255);
static const cv::Scalar kYellow(0,   255, 255, 255);

void draw_detections(cv::Mat& osd,
                     const std::vector<apriltag_det_t>& dets,
                     double decimate_scale,
                     int sensor_w, int /*sensor_h*/)
{
    if (osd.empty() || sensor_w <= 0) {
        return;
    }
    // Uniform scale: the camera fills the OSD width; aspect ratio is preserved,
    // so a single factor maps sensor pixels -> OSD pixels. Combine with the
    // decimation scale (detections are in decimated space).
    const double s = decimate_scale * (double)osd.cols / (double)sensor_w;

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
