#include "workload_backend.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace apriltag_bench;

namespace {
int failures;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

WorkloadResult sample(BackendKind kind, std::uint64_t scale)
{
    WorkloadResult result{};
    result.kind = kind;
    result.detection = {2, 0x1234};
    auto& c = result.counters;
    c.schema_version = 2;
    c.struct_size = sizeof(c);
    c.validity = kWorkloadValidInput | kWorkloadValidThreshold |
                 kWorkloadValidSegmentation | kWorkloadValidFitting |
                 kWorkloadValidDecode | kWorkloadValidUf;
    c.input_width = 8; c.input_height = 6;
    c.decimated_width = 4; c.decimated_height = 3;
    c.decimated_checksum = 10; c.threshold_checksum = 20;
    c.boundary_points_emitted = 100 * scale;
    c.clusters_after_filters = 10 * scale;
    c.points_entering_sort = 80 * scale;
    c.points_entering_lfps = 70 * scale;
    c.points_entering_errors = 60 * scale;
    c.compute_errors_points = 50 * scale;
    c.raw_peaks = 30 * scale;
    c.retained_peaks = 20 * scale;
    c.quad_fit_attempts = 5 * scale;
    c.quads = 4 * scale; c.decode_attempts = 3 * scale;
    c.detections = 2;
    return result;
}

void test_determinism_rejection()
{
    const auto a = sample(BackendKind::RustRvv, 1);
    validate_workload_pair(a, a);
    auto changed = a;
    ++changed.counters.points_entering_sort;
    bool threw = false;
    try { validate_workload_pair(a, changed); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    auto timer_changed = a;
    ++timer_changed.counters.ccl_connected_components_ns;
    validate_workload_pair(a, timer_changed);
    auto provenance_changed = a;
    provenance_changed.counters.provenance[0].dedup_detection_id = 7;
    threw = false;
    try { validate_workload_pair(a, provenance_changed); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void test_schema_output()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    const std::vector<WorkloadResult> results = {
        sample(BackendKind::RustRvv, 2), sample(BackendKind::CReference, 1)};
    std::ostringstream out;
    print_workload_report(config, image, results, out);
    const std::string text = out.str();
    CHECK(text.find("Common workload counters") != std::string::npos);
    CHECK(text.find("Early polarity estimates") != std::string::npos);
    CHECK(text.find("WORKLOAD backend=rust-rvv schema=2") != std::string::npos);
    CHECK(text.find("WORKLOAD backend=c-reference schema=2") != std::string::npos);
    CHECK(text.find("validity=0x3f") != std::string::npos);
    CHECK(text.find("boundary_points_emitted=200") != std::string::npos);
    CHECK(text.find("points_entering_sort=160") != std::string::npos);
    CHECK(text.find("boundary points emitted: 2.000000x") != std::string::npos);
    CHECK(text.find("clusters after filters: 2.000000x") != std::string::npos);
    CHECK(text.find("points entering sort: 2.000000x") != std::string::npos);
    CHECK(text.find("points entering LFPS: 2.000000x") != std::string::npos);
    CHECK(text.find("points entering errors: 2.000000x") != std::string::npos);
    CHECK(text.find("compute-errors points: 2.000000x") != std::string::npos);
    CHECK(text.find("raw peaks: 2.000000x") != std::string::npos);
    CHECK(text.find("retained peaks: 2.000000x") != std::string::npos);
    CHECK(text.find("quad fit attempts: 2.000000x") != std::string::npos);
    CHECK(text.find("quads: 2.000000x") != std::string::npos);
    CHECK(text.find("decode attempts: 2.000000x") != std::string::npos);
    CHECK(text.find("threshold_checksum_match=1") != std::string::npos);
    CHECK(text.find("output_match=1") != std::string::npos);
    CHECK(text.find("CCL substage timers") != std::string::npos);
    CHECK(text.find("Successful detection provenance") != std::string::npos);
    CHECK(text.find("boundary_down_left_contrast") != std::string::npos);
    CHECK(text.find("boundary_down_right_size_qualified") != std::string::npos);
}

void test_direction_and_timer_validity_are_precise()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto c = sample(BackendKind::CReference, 1);
    c.counters.validity |= kWorkloadValidBoundaryDiagnostics | kWorkloadValidCclTimers;
    c.counters.boundary_right_checks = 8;
    c.counters.boundary_down_checks = 7;
    c.counters.boundary_down_left_checks = 6;
    c.counters.boundary_down_right_checks = 5;
    c.counters.boundary_connected_last_suppressions = 4;
    c.counters.ccl_timer_validity =
        kWorkloadTimerUfInit | kWorkloadTimerConnectedComponents |
        kWorkloadTimerGradientClustering;
    c.counters.ccl_uf_init_ns = 1000;
    c.counters.ccl_connected_components_ns = 2000;
    c.counters.ccl_gradient_clustering_ns = 3000;
    std::ostringstream out;
    print_workload_report(config, image, {c}, out);
    const std::string text = out.str();
    CHECK(text.find("boundary_right_checks") != std::string::npos);
    CHECK(text.find("ccl_uf_init_ns") != std::string::npos);
    CHECK(text.find("ccl_total_ns") != std::string::npos);
    CHECK(text.find("n/a") != std::string::npos);
}

void test_cross_backend_provenance_matches_id_and_nearest_center()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 1);
    auto c = sample(BackendKind::CReference, 1);
    rust.counters.validity |= kWorkloadValidDetectionProvenance | kWorkloadValidCounterfactualDedup;
    c.counters.validity |= kWorkloadValidDetectionProvenance;
    rust.counters.provenance_count = c.counters.provenance_count = 1;
    rust.counters.provenance[0].detection_id = c.counters.provenance[0].detection_id = 7;
    rust.counters.provenance[0].detection_center_x_bits = double_bits(10.0);
    rust.counters.provenance[0].detection_center_y_bits = double_bits(12.0);
    c.counters.provenance[0].detection_center_x_bits = double_bits(11.0);
    c.counters.provenance[0].detection_center_y_bits = double_bits(12.0);
    rust.counters.provenance[0].detection_geometry_checksum = 0x1234;
    c.counters.provenance[0].detection_geometry_checksum = 0x1234;
    rust.counters.provenance[0].dedup_id_matches = 1;
    rust.counters.provenance[0].dedup_geometry_matches = 0;
    std::ostringstream out;
    print_workload_report(config, image, {rust, c}, out);
    const std::string text = out.str();
    CHECK(text.find("PROVENANCE_MATCH id=7") != std::string::npos);
    CHECK(text.find("center_distance=1.000000") != std::string::npos);
    CHECK(text.find("counterfactual_id_match=1 counterfactual_geometry_match=0") != std::string::npos);
}

void test_max_u64_checksum_table_columns_are_readable()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rvv = sample(BackendKind::RustRvv, 1);
    auto scalar = sample(BackendKind::RustScalar, 1);
    auto c = sample(BackendKind::CReference, 1);
    for (auto* result : {&rvv, &scalar, &c}) {
        result->counters.decimated_checksum = std::numeric_limits<std::uint64_t>::max();
        result->counters.threshold_checksum = std::numeric_limits<std::uint64_t>::max();
        result->counters.threshold_black_pixels = std::numeric_limits<std::uint64_t>::max();
    }
    std::ostringstream out;
    print_workload_report(config, image, {rvv, scalar, c}, out);
    const std::string text = out.str();
    const auto table_row = [](const std::string& label, const std::string& value) {
        return label + std::string(42 - label.size(), ' ') +
               std::string(22 - value.size(), ' ') + value +
               std::string(22 - value.size(), ' ') + value +
               std::string(22 - value.size(), ' ') + value + '\n';
    };
    CHECK(text.find(table_row("decimated_checksum", "0xffffffffffffffff")) != std::string::npos);
    CHECK(text.find(table_row("threshold_checksum", "0xffffffffffffffff")) != std::string::npos);
    CHECK(text.find(table_row("threshold_black_pixels", "18446744073709551615")) != std::string::npos);
    CHECK(text.find("decimated_checksum=18446744073709551615") != std::string::npos);
    CHECK(text.find("threshold_checksum=18446744073709551615") != std::string::npos);
}

void test_invalid_ratio_numerator_is_not_available()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 2);
    auto c = sample(BackendKind::CReference, 1);
    rust.counters.validity &= ~kWorkloadValidFitting;
    std::ostringstream out;
    print_workload_report(config, image, {rust, c}, out);
    const std::string text = out.str();
    CHECK(text.find("boundary points emitted: 2.000000x") != std::string::npos);
    CHECK(text.find("points entering sort: n/a") != std::string::npos);
    CHECK(text.find("raw peaks: n/a") != std::string::npos);
    CHECK(text.find("quad fit attempts: n/a") != std::string::npos);
    CHECK(text.find("decode attempts: 2.000000x") != std::string::npos);
}

void test_zero_denominator_ratios_are_not_available()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 1);
    auto c = sample(BackendKind::CReference, 0);
    rust.counters.points_entering_sort = 0;
    std::ostringstream out;
    print_workload_report(config, image, {rust, c}, out);
    const std::string text = out.str();
    CHECK(text.find("boundary points emitted: n/a") != std::string::npos);
    CHECK(text.find("sort_waste_ratio=n/a") != std::string::npos);
    CHECK(text.find("ratio_rust_rvv_to_c_reference=n/a") != std::string::npos);
}

void test_early_polarity_ratios_require_valid_counters()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 1);
    rust.counters.hypothetical_wasted_sort_points = 40;
    rust.counters.hypothetical_wasted_lfps_points = 35;
    rust.counters.hypothetical_wasted_compute_errors_points = 25;
    std::ostringstream out;
    print_workload_report(config, image, {rust}, out);
    const std::string text = out.str();
    CHECK(text.find("sort_waste_ratio=n/a") != std::string::npos);
    CHECK(text.find("lfps_waste_ratio=n/a") != std::string::npos);
    CHECK(text.find("errors_waste_ratio=n/a") != std::string::npos);
}

void test_early_polarity_ratios_use_hypothetical_validity()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 1);
    rust.counters.validity |= kWorkloadValidHypotheticalEarlyPolarity;
    rust.counters.hypothetical_early_polarity_reject_clusters = 4;
    rust.counters.hypothetical_early_polarity_reject_points = 40;
    rust.counters.hypothetical_wasted_sort_points = 40;
    rust.counters.hypothetical_wasted_lfps_points = 35;
    rust.counters.hypothetical_wasted_compute_errors_points = 25;
    std::ostringstream out;
    print_workload_report(config, image, {rust}, out);
    const std::string text = out.str();
    CHECK(text.find("rejected_clusters=4 rejected_points=40") != std::string::npos);
    CHECK(text.find("sort_waste_ratio=0.500000") != std::string::npos);
    CHECK(text.find("lfps_waste_ratio=0.500000") != std::string::npos);
    CHECK(text.find("errors_waste_ratio=0.500000") != std::string::npos);
}

void test_checksum_matches_require_valid_counters()
{
    BenchmarkConfig config;
    PreparedImage image{8, 6, 8, std::vector<std::uint8_t>(48)};
    auto rust = sample(BackendKind::RustRvv, 1);
    auto c = sample(BackendKind::CReference, 1);
    rust.counters.validity &= ~kWorkloadValidThreshold;
    rust.counters.threshold_checksum = c.counters.threshold_checksum;
    std::ostringstream out;
    print_workload_report(config, image, {rust, c}, out);
    const std::string text = out.str();
    CHECK(text.find("threshold_checksum_match=n/a") != std::string::npos);
    CHECK(text.find("output_match=1") != std::string::npos);
}
}  // namespace

int main()
{
    test_determinism_rejection();
    test_schema_output();
    test_direction_and_timer_validity_are_precise();
    test_max_u64_checksum_table_columns_are_readable();
    test_invalid_ratio_numerator_is_not_available();
    test_zero_denominator_ratios_are_not_available();
    test_early_polarity_ratios_require_valid_counters();
    test_early_polarity_ratios_use_hypothetical_validity();
    test_checksum_matches_require_valid_counters();
    test_cross_backend_provenance_matches_id_and_nearest_center();
    if (failures) return 1;
    std::cout << "all workload format tests passed\n";
    return 0;
}
