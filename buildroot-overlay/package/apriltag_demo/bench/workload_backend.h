#ifndef K230_APRILTAG_WORKLOAD_BACKEND_H
#define K230_APRILTAG_WORKLOAD_BACKEND_H

#include "benchmark.h"

#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace apriltag_bench {

constexpr std::uint64_t kWorkloadValidInput = UINT64_C(1) << 0;
constexpr std::uint64_t kWorkloadValidThreshold = UINT64_C(1) << 1;
constexpr std::uint64_t kWorkloadValidSegmentation = UINT64_C(1) << 2;
constexpr std::uint64_t kWorkloadValidFitting = UINT64_C(1) << 3;
constexpr std::uint64_t kWorkloadValidDecode = UINT64_C(1) << 4;
constexpr std::uint64_t kWorkloadValidUf = UINT64_C(1) << 5;
constexpr std::uint64_t kWorkloadValidLineFit = UINT64_C(1) << 6;
constexpr std::uint64_t kWorkloadValidRustRle = UINT64_C(1) << 7;
constexpr std::uint64_t kWorkloadValidRustBoundary = UINT64_C(1) << 8;
constexpr std::uint64_t kWorkloadValidRustPairStats = UINT64_C(1) << 9;
constexpr std::uint64_t kWorkloadValidCSpecific = UINT64_C(1) << 10;
constexpr std::uint64_t kWorkloadValidBorderPolarity = UINT64_C(1) << 11;
constexpr std::uint64_t kWorkloadValidFamilyPolarity = UINT64_C(1) << 12;
constexpr std::uint64_t kWorkloadValidBoundaryCandidates = UINT64_C(1) << 13;
constexpr std::uint64_t kWorkloadValidHypotheticalEarlyPolarity = UINT64_C(1) << 14;
constexpr std::uint64_t kWorkloadValidBoundaryDiagnostics = UINT64_C(1) << 15;
constexpr std::uint64_t kWorkloadValidCclTimers = UINT64_C(1) << 16;
constexpr std::uint64_t kWorkloadValidDetectionProvenance = UINT64_C(1) << 17;
constexpr std::uint64_t kWorkloadValidCounterfactualDedup = UINT64_C(1) << 18;
constexpr std::uint64_t kWorkloadTimerTotal = UINT64_C(1) << 0;
constexpr std::uint64_t kWorkloadTimerRleRepack = UINT64_C(1) << 1;
constexpr std::uint64_t kWorkloadTimerUfInit = UINT64_C(1) << 2;
constexpr std::uint64_t kWorkloadTimerConnectedComponents = UINT64_C(1) << 3;
constexpr std::uint64_t kWorkloadTimerDiagLeft = UINT64_C(1) << 4;
constexpr std::uint64_t kWorkloadTimerDiagRight = UINT64_C(1) << 5;
constexpr std::uint64_t kWorkloadTimerRootMaterialize = UINT64_C(1) << 6;
constexpr std::uint64_t kWorkloadTimerGradientClustering = UINT64_C(1) << 7;
constexpr std::uint64_t kWorkloadTimerConversion = UINT64_C(1) << 8;

struct DetectionProvenance {
    std::uint64_t detection_id = 0, component_pair = 0;
    std::uint32_t raw_index = 0, final_index = 0;
    std::uint32_t bbox_min_x = 0, bbox_min_y = 0, bbox_max_x = 0, bbox_max_y = 0;
    std::uint32_t point_count = 0, right_points = 0, down_points = 0;
    std::uint32_t down_left_points = 0, down_right_points = 0;
    std::uint32_t exact_duplicates = 0, coordinate_duplicates = 0, perimeter_points = 0;
    std::uint32_t dedup_survives = 0, dedup_id_matches = 0;
    std::uint32_t dedup_geometry_matches = 0, dedup_point_count = 0;
    std::uint64_t dedup_detection_id = 0, dedup_center_x_bits = 0, dedup_center_y_bits = 0;
    std::uint64_t dedup_geometry_checksum = 0;
    std::uint64_t detection_center_x_bits = 0, detection_center_y_bits = 0;
    std::uint64_t detection_geometry_checksum = 0;
};

struct WorkloadCounters {
    std::uint32_t schema_version = 0;
    std::uint32_t struct_size = 0;
    std::uint64_t validity = 0;
#define WORKLOAD_COUNTER_FIELDS(X) \
    X(input_width) X(input_height) X(decimated_width) X(decimated_height) \
    X(decimated_checksum) X(threshold_checksum) X(threshold_black_pixels) \
    X(threshold_uncertain_pixels) X(threshold_white_pixels) \
    X(boundary_candidates) X(boundary_points_emitted) X(clusters_created) \
    X(clusters_after_filters) X(cluster_reject_too_few_points) \
    X(cluster_reject_too_large) X(cluster_reject_extent) \
    X(cluster_reject_border_polarity) X(points_entering_sort) \
    X(points_entering_lfps) X(points_entering_errors) X(compute_errors_calls) \
    X(compute_errors_points) X(raw_peaks) X(retained_peaks) \
    X(quad_fit_attempts) X(quads) X(quad_family_candidates) \
    X(homography_rejects) X(family_polarity_rejects) X(decode_attempts) \
    X(photometric_polarity_rejects) X(codeword_rejects) X(raw_detections) \
    X(duplicate_rejects) X(detections) X(uf_elements) X(uf_union_attempts) \
    X(uf_union_successes) X(uf_granularity) X(line_fit_queries) \
    X(line_fit_query_points) X(rle_runs) X(pending_boundary_records) \
    X(pending_boundary_expanded_points) X(rvv_runs_repacked) \
    X(pair_stats_computed) X(pair_stats_span_points) X(pair_table_lookups) \
    X(active_uf_pixels) X(fit_line_calls) X(fit_line_span_points) \
    X(cluster_hash_entries) X(hypothetical_early_polarity_reject_clusters) \
    X(hypothetical_early_polarity_reject_points) X(hypothetical_wasted_sort_points) \
    X(hypothetical_wasted_lfps_points) X(hypothetical_wasted_compute_errors_points) \
    X(hypothetical_wasted_peak_search_clusters) \
    X(boundary_right_checks) X(boundary_right_contrast) X(boundary_right_size_qualified) X(boundary_right_emitted) \
    X(boundary_down_checks) X(boundary_down_contrast) X(boundary_down_size_qualified) X(boundary_down_emitted) \
    X(boundary_down_left_checks) X(boundary_down_left_contrast) X(boundary_down_left_size_qualified) X(boundary_down_left_emitted) \
    X(boundary_down_right_checks) X(boundary_down_right_contrast) X(boundary_down_right_size_qualified) X(boundary_down_right_emitted) \
    X(boundary_connected_last_suppressions) X(boundary_exact_duplicates) X(boundary_coordinate_duplicates) \
    X(boundary_exact_unique) X(boundary_coordinate_unique) X(boundary_perimeter_points) \
    X(ccl_timer_validity) X(ccl_total_ns) X(ccl_rle_repack_ns) X(ccl_uf_init_ns) X(ccl_connected_components_ns) \
    X(ccl_diag_left_ns) X(ccl_diag_right_ns) X(ccl_root_materialize_ns) X(ccl_gradient_clustering_ns) X(ccl_conversion_ns) \
    X(provenance_count) X(provenance_dropped)
#define DECLARE_COUNTER(name) std::uint64_t name = 0;
    WORKLOAD_COUNTER_FIELDS(DECLARE_COUNTER)
#undef DECLARE_COUNTER
    DetectionProvenance provenance[16]{};
};

struct WorkloadResult {
    BackendKind kind;
    DetectionResult detection;
    WorkloadCounters counters;
};

inline std::uint64_t double_bits(double value)
{
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline double bits_double(std::uint64_t bits)
{
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

class WorkloadBackend {
public:
    virtual ~WorkloadBackend() = default;
    virtual BackendKind kind() const = 0;
    virtual WorkloadResult run(const PreparedImage& image) = 0;
};

std::unique_ptr<WorkloadBackend> make_workload_rust_backend(
    const BenchmarkConfig& config, BackendKind kind);
std::unique_ptr<WorkloadBackend> make_workload_c_backend(
    const BenchmarkConfig& config);
void validate_workload_pair(const WorkloadResult& first,
                            const WorkloadResult& second);
void print_workload_report(const BenchmarkConfig& config,
                           const PreparedImage& image,
                           const std::vector<WorkloadResult>& results,
                           std::ostream& out);
int workload_main(int argc, const char* const argv[], std::ostream& out,
                  std::ostream& err);

}  // namespace apriltag_bench

#endif
