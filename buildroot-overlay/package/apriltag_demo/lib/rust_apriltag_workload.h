#ifndef APRILTAG_WORKLOAD_H
#define APRILTAG_WORKLOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_WORKLOAD_SCHEMA_VERSION UINT32_C(1)
#define APRILTAG_WORKLOAD_VALID_INPUT (UINT64_C(1) << 0)
#define APRILTAG_WORKLOAD_VALID_COMMON_THRESHOLD (UINT64_C(1) << 1)
#define APRILTAG_WORKLOAD_VALID_COMMON_SEGMENTATION (UINT64_C(1) << 2)
#define APRILTAG_WORKLOAD_VALID_COMMON_FITTING (UINT64_C(1) << 3)
#define APRILTAG_WORKLOAD_VALID_COMMON_DECODE (UINT64_C(1) << 4)
#define APRILTAG_WORKLOAD_VALID_COMPARABLE_UF (UINT64_C(1) << 5)
#define APRILTAG_WORKLOAD_VALID_COMPARABLE_LINE_FIT (UINT64_C(1) << 6)
#define APRILTAG_WORKLOAD_VALID_RUST_RLE (UINT64_C(1) << 7)
#define APRILTAG_WORKLOAD_VALID_RUST_BOUNDARY (UINT64_C(1) << 8)
#define APRILTAG_WORKLOAD_VALID_RUST_PAIR_STATS (UINT64_C(1) << 9)
#define APRILTAG_WORKLOAD_VALID_C_SPECIFIC (UINT64_C(1) << 10)
#define APRILTAG_WORKLOAD_VALID_CLUSTER_REJECT_BORDER_POLARITY (UINT64_C(1) << 11)
#define APRILTAG_WORKLOAD_VALID_FAMILY_POLARITY_REJECTS (UINT64_C(1) << 12)
#define APRILTAG_WORKLOAD_VALID_COMPARABLE_BOUNDARY_CANDIDATES (UINT64_C(1) << 13)
#define APRILTAG_WORKLOAD_VALID_HYPOTHETICAL_EARLY_POLARITY (UINT64_C(1) << 14)

typedef void apriltag_t;

typedef struct apriltag_workload_counters {
    uint32_t schema_version;
    uint32_t struct_size;
    uint64_t validity;
    uint64_t input_width;
    uint64_t input_height;
    uint64_t decimated_width;
    uint64_t decimated_height;
    uint64_t decimated_checksum;
    uint64_t threshold_checksum;
    uint64_t threshold_black_pixels;
    uint64_t threshold_uncertain_pixels;
    uint64_t threshold_white_pixels;
    /* Reserved; zero unless COMPARABLE_BOUNDARY_CANDIDATES is valid. */
    uint64_t boundary_candidates;
    uint64_t boundary_points_emitted;
    uint64_t clusters_created;
    uint64_t clusters_after_filters;
    uint64_t cluster_reject_too_few_points;
    uint64_t cluster_reject_too_large;
    uint64_t cluster_reject_extent;
    uint64_t cluster_reject_border_polarity;
    uint64_t points_entering_sort;
    uint64_t points_entering_lfps;
    uint64_t points_entering_errors;
    uint64_t compute_errors_calls;
    uint64_t compute_errors_points;
    uint64_t raw_peaks;
    uint64_t retained_peaks;
    uint64_t quad_fit_attempts;
    uint64_t quads;
    uint64_t quad_family_candidates;
    uint64_t homography_rejects;
    uint64_t family_polarity_rejects;
    uint64_t decode_attempts;
    uint64_t photometric_polarity_rejects;
    uint64_t codeword_rejects;
    uint64_t raw_detections;
    uint64_t duplicate_rejects;
    uint64_t detections;
    uint64_t uf_elements;
    uint64_t uf_union_attempts;
    uint64_t uf_union_successes;
    uint64_t uf_granularity;
    uint64_t line_fit_queries;
    uint64_t line_fit_query_points;
    uint64_t rle_runs;
    uint64_t pending_boundary_records;
    uint64_t pending_boundary_expanded_points;
    uint64_t rvv_runs_repacked;
    uint64_t pair_stats_computed;
    uint64_t pair_stats_span_points;
    uint64_t pair_table_lookups;
    uint64_t active_uf_pixels;
    uint64_t fit_line_calls;
    uint64_t fit_line_span_points;
    uint64_t cluster_hash_entries;
    uint64_t hypothetical_early_polarity_reject_clusters;
    uint64_t hypothetical_early_polarity_reject_points;
    uint64_t hypothetical_wasted_sort_points;
    uint64_t hypothetical_wasted_lfps_points;
    uint64_t hypothetical_wasted_compute_errors_points;
    uint64_t hypothetical_wasted_peak_search_clusters;
} apriltag_workload_counters_t;

apriltag_t *apriltag_new(uint32_t min_blob_size);
void apriltag_free(apriltag_t *handle);
int apriltag_get_workload_counters(apriltag_t *handle,
                                  apriltag_workload_counters_t *out);

#ifdef __cplusplus
}
#endif

#endif
