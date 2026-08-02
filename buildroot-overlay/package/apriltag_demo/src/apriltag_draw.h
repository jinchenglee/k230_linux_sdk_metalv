/* OSD overlay drawing for apriltag_demo. */
#ifndef APRILTAG_DRAW_H
#define APRILTAG_DRAW_H

#include <vector>
#include <opencv2/opencv.hpp>
#include "apriltag.h"

/* Draw detections onto a landscape-oriented ARGB (CV_8UC4) OSD Mat.
 *
 * Detection coordinates are in source-image space; map to OSD pixels with:
 *   osd = corner * (osd_landscape_width / sensor_width)
 *
 *   dets       : detections from apriltag_detect()
 *   sensor_w/h : capture resolution the detector ran on
 */
void draw_detections(cv::Mat& osd,
                     const std::vector<apriltag_det_t>& dets,
                     int sensor_w, int sensor_h);

/* Paint a packed-RGB pipeline debug image as an opaque, aspect-preserving
 * landscape view on the ARGB OSD plane. */
void draw_debug_image(cv::Mat& osd, const apriltag_debug_image_t& image);

/* Add compact Group D/E and trial counters to the recovery debug page. */
void draw_recovery_stats(cv::Mat& osd,
                          const apriltag_recovery_stats_t& stats);

/* Draw Rust-provided stage-6 candidate labels at source-image anchors. */
void draw_recovery_candidates(
    cv::Mat& osd, int source_width, int source_height,
    const apriltag_recovery_candidate_t* candidates, size_t count);

/* Label substantial fitted quads in a pipeline debug image with their
 * individual decode outcome and nearest ID/Hamming distance. */
void draw_decode_candidates(cv::Mat& osd,
                            const apriltag_debug_image_t& image,
                            const apriltag_decode_candidate_t* candidates,
                            size_t count);

/* Paint a BGR camera frame opaquely on the OSD while preserving aspect ratio.
 * Used for USB cameras; the CSI camera remains on the zero-copy video plane. */
void draw_camera_frame(cv::Mat& osd, const cv::Mat& bgr);

const char* apriltag_debug_stage_name(int stage);

#endif /* APRILTAG_DRAW_H */
