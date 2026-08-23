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

/* LCD-only: same detection overlay as draw_detections(), but computes each
 * point's FINAL position in a 90-degree-clockwise-rotated (physically
 * sideways-mounted panel) coordinate space directly, instead of drawing
 * into a landscape canvas that a separate step then rotates as a whole
 * image. See the long comment above this function's definition in
 * apriltag_draw.cc for the full rationale, and main.cc's
 * draw_frame_lcd90cw for how the two paths are kept separate so
 * landscape/HDMI displays are entirely unaffected.
 *
 *   osd_portrait   : destination buffer, already shaped like the actual
 *                    panel (e.g. 480x800) -- NOT the landscape shape
 *                    draw_detections() expects.
 *   sensor_w/h     : capture resolution the detector ran on (same meaning
 *                    as draw_detections()'s sensor_w/h).
 *   landscape_w/h  : the dimensions the equivalent LANDSCAPE canvas would
 *                    have used (i.e. what draw_frame is normally allocated
 *                    as on this panel) -- used with INDEPENDENT per-axis
 *                    (sx, sy) scale factors, matching what the camera video
 *                    plane itself renders: display_proc()'s portrait branch
 *                    sets the video plane to the panel's raw dimensions
 *                    (non-uniform stretch, sensor aspect != panel aspect),
 *                    so this reproduces that same stretch rather than a
 *                    uniform aspect-preserving scale -- an aspect-preserving
 *                    version of both was tried and regressed on real
 *                    hardware (see the long comment on this function's
 *                    definition in apriltag_draw.cc), so keep this in sync
 *                    with display_proc()'s context.width/height, not with
 *                    draw_detections()'s aspect_fit().
 *
 * Note: quad corner POSITIONS are fully rotated and land geometrically
 * correct in the viewer's frame. Label TEXT positions are rotated too, but
 * the glyphs themselves are drawn upright in the buffer's own pixel grid
 * (cv::putText has no rotation parameter) -- an accepted trade-off, so
 * labels read top-to-bottom rather than left-to-right on this panel. See
 * the definition in apriltag_draw.cc for the full rationale.
 */
void draw_detections_lcd_90cw(cv::Mat& osd_portrait,
                              const std::vector<apriltag_det_t>& dets,
                              int sensor_w, int sensor_h,
                              int landscape_w, int landscape_h);

/* Paint a packed-RGB pipeline debug image as an opaque, aspect-preserving
 * landscape view on the ARGB OSD plane. */
void draw_debug_image(cv::Mat& osd, const apriltag_debug_image_t& image);

/* Label substantial fitted quads in a pipeline debug image with their
 * individual decode outcome and nearest ID/Hamming distance. */
void draw_decode_candidates(cv::Mat& osd,
                            const apriltag_debug_image_t& image,
                            const apriltag_decode_candidate_t* candidates,
                            size_t count);

/* Paint a BGR camera frame opaquely on the OSD while preserving aspect ratio.
 * Used for USB cameras; the CSI camera remains on the zero-copy video plane. */
void draw_camera_frame(cv::Mat& osd, const cv::Mat& bgr);

/* Draw a compact "cam: X.XX, det: X.XX" status line at the bottom-left of
 * the given OSD buffer, in yellow. `osd` is drawn into directly in its own
 * coordinate space --
 * landscape draw_frame or the already-panel-oriented draw_frame_lcd90cw --
 * so no rotation/remap is needed here, unlike detection corners (this text
 * isn't derived from sensor coordinates, it's a fixed HUD position).
 * cam_fps/det_fps are passed in already computed (see main.cc's
 * once-a-second FPS block), so this adds no new per-frame computation --
 * just one cheap putText riding along whatever redraw is already
 * happening. */
void draw_fps_stats(cv::Mat& osd, double cam_fps, double det_fps);

const char* apriltag_debug_stage_name(int stage);

#endif /* APRILTAG_DRAW_H */
