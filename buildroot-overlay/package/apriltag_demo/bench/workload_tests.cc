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
    c.schema_version = 1;
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
}

void test_incompatible_schema_rejection()
{
    for (int field = 0; field < 2; ++field) {
        auto first = sample(BackendKind::RustRvv, 1);
        auto second = first;
        if (field == 0) {
            first.counters.schema_version = 2;
            second.counters.schema_version = 2;
        } else {
            first.counters.struct_size = sizeof(WorkloadCounters) - 1;
            second.counters.struct_size = sizeof(WorkloadCounters) - 1;
        }

        bool rejected = false;
        try { validate_workload_pair(first, second); }
        catch (const std::runtime_error& e) {
            rejected = std::string(e.what()).find("incompatible workload schema") !=
                       std::string::npos;
        }
        CHECK(rejected);
    }
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
    CHECK(text.find("WORKLOAD backend=rust-rvv schema=1") != std::string::npos);
    CHECK(text.find("WORKLOAD backend=c-reference schema=1") != std::string::npos);
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
    test_incompatible_schema_rejection();
    test_schema_output();
    test_max_u64_checksum_table_columns_are_readable();
    test_invalid_ratio_numerator_is_not_available();
    test_zero_denominator_ratios_are_not_available();
    test_early_polarity_ratios_require_valid_counters();
    test_early_polarity_ratios_use_hypothetical_validity();
    test_checksum_matches_require_valid_counters();
    if (failures) return 1;
    std::cout << "all workload format tests passed\n";
    return 0;
}
