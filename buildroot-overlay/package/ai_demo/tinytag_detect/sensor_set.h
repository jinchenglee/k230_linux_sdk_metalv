#ifndef TINYTAG_SENSOR_SET_H
#define TINYTAG_SENSOR_SET_H

#include "mmz.h"      /* kd_mpi_get_vvcam_video00(), kd_mpi_mmz_deinit() */
#include "display.h"  /* display_*, DRM_FORMAT_ARGB8888, rotation_* */
#include "v4l2-drm.h" /* v4l2_drm_* */
#include "thead.h"    /* thead_csi_dcache_clean_invalid_range() */

// 1280x720, not the other ai_demo family's 1920x1080 -- deliberately
// matches apriltag_demo's own CSI convention (buildroot-overlay/package/
// apriltag_demo/src/sensor_set.h), because it's exactly 2x the TinyTag
// kmodel's 640x360 input. HEADS_AND_POSTPROCESSING.md's decode_proposals()
// spec assumes a fixed "x2 to full resolution" scale factor; this makes
// that relationship exact rather than an arbitrary ratio.
#define SENSOR_WIDTH (1280)
#define SENSOR_HEIGHT (720)

#endif
