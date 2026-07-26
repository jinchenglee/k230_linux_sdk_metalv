/* C ABI for libapriltag_rvv.a (apriltag-rvv crate, feature "capi").
 *
 * Must stay in sync with apriltag-rvv/src/capi.rs. Coordinates returned in
 * apriltag_det_t are in DECIMATED image space (divided by the decimation
 * factor), matching what pipeline::detect() returns; the caller maps them back
 * to display pixels via decimation scale * (display_dim / sensor_dim).
 */
#ifndef APRILTAG_CAPI_H
#define APRILTAG_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One detection. corners are [x0,y0, x1,y1, x2,y2, x3,y3], counter-clockwise. */
typedef struct {
    uint64_t id;
    double   margin;
    double   center[2];
    double   corners[8];
} apriltag_det_t;

/* Allocate a detector (holds persistent buffers + Tag36h11 + min_blob_size).
 * min_blob_size mirrors detect()'s parameter (live_demo uses 25). */
void* apriltag_new(uint32_t min_blob_size);

/* Free a detector allocated by apriltag_new. NULL is ignored. */
void  apriltag_free(void* handle);

/* Run detection on a grayscale image (e.g. an NV12 Y-plane).
 *   y/width/height/stride : row-major u8 luma, stride bytes per row (>= width)
 *   factor                : 0 = 1.0, 1 = 1.5, 2 = 2.0
 *   mode                  : 0 = scalar, 1 = rvv
 *   out/max_out           : caller array; up to max_out detections written
 * Returns count written (>= 0), or -1 on bad args / internal panic. */
int   apriltag_detect(void* handle,
                      const uint8_t* y, size_t width, size_t height, size_t stride,
                      int factor, int mode,
                      apriltag_det_t* out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* APRILTAG_CAPI_H */
