/* C ABI for libapriltag_rvv.a (apriltag-rvv crate, feature "capi").
 *
 * Must stay in sync with apriltag-rvv/src/capi.rs. Coordinates returned in
 * apriltag_det_t use the full-resolution input image coordinate system,
 * independently of the quad-search decimation factor.
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
    uint8_t  recovered;
    uint8_t  visible_edges;
    uint8_t  inferred_edges;
    uint8_t  erasure_count;
    uint8_t  corrected_bit_count;
    double   geometry_residual;
    uint32_t recovery_group; /* 0=normal, 1=Group D, 2=Group E */
} apriltag_det_t;

/* Borrowed packed-RGB image from the selected live-debug pipeline stage.
 * pixels remains valid until the next detect/set_debug_enabled/
 * set_debug_stage/free call. */
typedef struct {
    const uint8_t* pixels;
    size_t         width;
    size_t         height;
    size_t         stride;
    uint32_t       stage;
    size_t         item_count;
} apriltag_debug_image_t;

/* Decode outcome counters from the most recent apriltag_detect() call.
 * best_hamming is -1 when no quad reached codeword matching. */
typedef struct {
    size_t   quads;
    size_t   homography_rejects;
    size_t   polarity_rejects;
    size_t   codeword_rejects;
    size_t   detections;
    int32_t  best_hamming;
    uint64_t best_raw_code;
} apriltag_decode_stats_t;

/* One fitted quad's decode outcome from the most recent detection call.
 * Candidate coordinates remain in decimated stage-4 debug-image space. */
typedef struct {
    uint64_t nearest_id; /* UINT64_MAX when no codeword was sampled */
    uint64_t raw_code;
    double   center[2];
    double   area;
    uint32_t status;     /* 0=decoded, 1=homography, 2=polarity, 3=codeword */
    int32_t  hamming;    /* -1 when no codeword was sampled */
} apriltag_decode_candidate_t;

typedef struct {
    uint64_t group_d_total;
    uint64_t group_d_eligible;
    uint64_t group_d_attempted;
    uint64_t group_d_skipped_cap;
    uint64_t group_d_success;
    uint64_t group_d_fail;
    uint64_t group_e_total;
    uint64_t group_e_eligible;
    uint64_t group_e_attempted;
    uint64_t group_e_skipped_cap;
    uint64_t group_e_success;
    uint64_t group_e_fail;
    uint64_t trials_attempted;
    uint64_t erasure_decodes;
} apriltag_recovery_stats_t;

/* Bounded stage-6 recovery-candidate label summary. result is 0=skipped,
 * 1=selected failure, or 2=recovered; id is UINT64_MAX unless recovered. */
typedef struct {
    uint64_t id;
    double   anchor[2];
    uint32_t group; /* 1=Group D, 2=Group E */
    uint8_t  strong_line_count;
    uint8_t  eligible;
    uint8_t  selected;
    uint8_t  result;
    uint8_t  errors;
    uint8_t  erasures;
    uint8_t  reserved[2];
} apriltag_recovery_candidate_t;

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

/* Enable or disable detailed live diagnostics. Disabled by default so normal
 * detection avoids diagnostic image rendering and nearest-code searches.
 * Returns 0 on success or -1 for an invalid handle. */
int   apriltag_set_debug_enabled(void* handle, int enabled);

/* Configure conservative recovery. Disabled by default. min_full_res_extent
 * is the minimum candidate extent in input-image pixels. */
int   apriltag_configure_recovery(void* handle, int enabled,
                                  double min_full_res_extent);

/* Select the live pipeline view:
 *   0 normal camera/detection overlay, 1 decimated grayscale, 2 threshold,
 *   3 boundary clusters, 4 fitted quads, 5 decoded detections, 6 recovery.
 * Returns 0 on success or -1 for an invalid handle/stage. */
int   apriltag_set_debug_stage(void* handle, int stage);

/* Borrow the RGB24 image produced by the most recent detect call.
 * Returns 1 when available, 0 when no debug image is ready, or -1 on error. */
int   apriltag_get_debug_image(void* handle, apriltag_debug_image_t* out);

/* Copy post-quad decode diagnostics from the most recent detect call.
 * Returns 1 when available, 0 when diagnostics are disabled, or -1 for an
 * invalid handle/output pointer. */
int   apriltag_get_decode_stats(void* handle, apriltag_decode_stats_t* out);

/* Copy up to max_out per-quad outcomes. Returns 0 when diagnostics are
 * disabled, the copied count when enabled, or -1 on error. */
int   apriltag_get_decode_candidates(void* handle,
                                     apriltag_decode_candidate_t* out,
                                     int max_out);

/* Copy counters from the most recent recovery-enabled detection call. */
int   apriltag_get_recovery_stats(void* handle,
                                   apriltag_recovery_stats_t* out);

/* Copy up to max_out stage-6 candidate summaries. Returns 0 when diagnostics
 * are disabled, copied count when enabled, or -1 on invalid arguments. */
int   apriltag_get_recovery_candidates(
          void* handle, apriltag_recovery_candidate_t* out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* APRILTAG_CAPI_H */
