#ifndef APRILTAG_WORKLOAD_H
#define APRILTAG_WORKLOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_WORKLOAD_SCHEMA_VERSION UINT32_C(2)
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
#define APRILTAG_WORKLOAD_VALID_BOUNDARY_DIAGNOSTICS (UINT64_C(1) << 15)
#define APRILTAG_WORKLOAD_VALID_CCL_TIMERS (UINT64_C(1) << 16)
#define APRILTAG_WORKLOAD_VALID_DETECTION_PROVENANCE (UINT64_C(1) << 17)
#define APRILTAG_WORKLOAD_VALID_COUNTERFACTUAL_DEDUP (UINT64_C(1) << 18)
#define APRILTAG_WORKLOAD_PROVENANCE_CAPACITY 16

typedef void apriltag_t;

typedef struct apriltag_workload_detection_provenance {
    uint64_t detection_id, component_pair;
    uint32_t raw_index, final_index;
    uint32_t bbox_min_x, bbox_min_y, bbox_max_x, bbox_max_y;
    uint32_t point_count, right_points, down_points, down_left_points, down_right_points;
    uint32_t exact_duplicates, coordinate_duplicates, perimeter_points;
    uint32_t dedup_survives, dedup_id_matches, dedup_geometry_matches, dedup_point_count;
    uint64_t dedup_detection_id, dedup_center_x_bits, dedup_center_y_bits;
    uint64_t dedup_geometry_checksum;
    uint64_t detection_center_x_bits, detection_center_y_bits;
    uint64_t detection_geometry_checksum;
} apriltag_workload_detection_provenance_t;

typedef struct apriltag_workload_counters {
    uint32_t schema_version, struct_size;
    uint64_t validity;
    uint64_t input_width, input_height, decimated_width, decimated_height;
    uint64_t decimated_checksum, threshold_checksum;
    uint64_t threshold_black_pixels, threshold_uncertain_pixels, threshold_white_pixels;
    uint64_t boundary_candidates, boundary_points_emitted, clusters_created, clusters_after_filters;
    uint64_t cluster_reject_too_few_points, cluster_reject_too_large, cluster_reject_extent;
    uint64_t cluster_reject_border_polarity, points_entering_sort, points_entering_lfps;
    uint64_t points_entering_errors, compute_errors_calls, compute_errors_points;
    uint64_t raw_peaks, retained_peaks, quad_fit_attempts, quads, quad_family_candidates;
    uint64_t homography_rejects, family_polarity_rejects, decode_attempts;
    uint64_t photometric_polarity_rejects, codeword_rejects, raw_detections;
    uint64_t duplicate_rejects, detections, uf_elements, uf_union_attempts;
    uint64_t uf_union_successes, uf_granularity, line_fit_queries, line_fit_query_points;
    uint64_t rle_runs, pending_boundary_records, pending_boundary_expanded_points;
    uint64_t rvv_runs_repacked, pair_stats_computed, pair_stats_span_points, pair_table_lookups;
    uint64_t active_uf_pixels, fit_line_calls, fit_line_span_points, cluster_hash_entries;
    uint64_t hypothetical_early_polarity_reject_clusters, hypothetical_early_polarity_reject_points;
    uint64_t hypothetical_wasted_sort_points, hypothetical_wasted_lfps_points;
    uint64_t hypothetical_wasted_compute_errors_points, hypothetical_wasted_peak_search_clusters;
    uint64_t boundary_right_checks, boundary_right_contrast, boundary_right_size_qualified, boundary_right_emitted;
    uint64_t boundary_down_checks, boundary_down_contrast, boundary_down_size_qualified, boundary_down_emitted;
    uint64_t boundary_down_left_checks, boundary_down_left_contrast, boundary_down_left_size_qualified, boundary_down_left_emitted;
    uint64_t boundary_down_right_checks, boundary_down_right_contrast, boundary_down_right_size_qualified, boundary_down_right_emitted;
    uint64_t boundary_connected_last_suppressions;
    uint64_t boundary_exact_duplicates, boundary_coordinate_duplicates;
    uint64_t boundary_exact_unique, boundary_coordinate_unique, boundary_perimeter_points;
    uint64_t ccl_timer_validity, ccl_total_ns, ccl_rle_repack_ns, ccl_uf_init_ns;
    uint64_t ccl_connected_components_ns, ccl_diag_left_ns, ccl_diag_right_ns;
    uint64_t ccl_root_materialize_ns, ccl_gradient_clustering_ns, ccl_conversion_ns;
    uint64_t provenance_count, provenance_dropped;
    apriltag_workload_detection_provenance_t provenance[APRILTAG_WORKLOAD_PROVENANCE_CAPACITY];
} apriltag_workload_counters_t;

apriltag_t *apriltag_new(uint32_t min_blob_size);
void apriltag_free(apriltag_t *handle);
int apriltag_get_workload_counters_v2(apriltag_t *handle,
                                     apriltag_workload_counters_t *out,
                                     size_t out_size);

#ifdef __cplusplus
}
#endif
#endif
