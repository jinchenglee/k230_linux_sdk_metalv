/* Minimal K230 platform includes + sensor geometry for apriltag_demo.
 *
 * Derived from face_detect/src/sensor_set.h with the nncase/AI dependencies
 * removed. Provides the camera/DRM/OSD headers used by main.cc.
 */
#ifndef APRILTAG_SENSOR_SET_H
#define APRILTAG_SENSOR_SET_H

#include <cassert>
#include <iostream>
#include <linux/videodev2.h>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <vector>

#include <opencv2/opencv.hpp>

#include "mmz.h"        /* kd_mpi_get_vvcam_video00(), kd_mpi_mmz_deinit() */
#include "display.h"    /* display_*, DRM_FORMAT_ARGB8888, rotation_* */
#include "v4l2-drm.h"   /* v4l2_drm_* */
#include "thead.h"      /* thead_csi_dcache_clean_invalid_range() */

/* Camera capture geometry (NV12). Matches face_detect's default sensor mode. */
#define SENSOR_CHANNEL (3)
#define SENSOR_HEIGHT  (720)
#define SENSOR_WIDTH   (1280)

#endif /* APRILTAG_SENSOR_SET_H */
