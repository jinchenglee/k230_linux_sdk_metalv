/* OSD overlay drawing for apriltag_demo. */
#ifndef APRILTAG_DRAW_H
#define APRILTAG_DRAW_H

#include <vector>
#include <opencv2/opencv.hpp>
#include "apriltag.h"

/* Draw detections onto a landscape-oriented ARGB (CV_8UC4) OSD Mat.
 *
 * Detection coordinates are in DECIMATED space; map to OSD pixels with:
 *   osd = corner * decimate_scale * (osd_landscape_width / sensor_width)
 *
 *   dets           : detections from apriltag_detect()
 *   decimate_scale : 1.0 / 1.5 / 2.0 (matches the factor passed to detect)
 *   sensor_w/h     : capture resolution the detector ran on
 */
void draw_detections(cv::Mat& osd,
                     const std::vector<apriltag_det_t>& dets,
                     double decimate_scale,
                     int sensor_w, int sensor_h);

#endif /* APRILTAG_DRAW_H */
