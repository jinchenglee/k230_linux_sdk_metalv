#include "workload_backend.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

namespace apriltag_bench {
namespace {
struct Field {
    const char* name;
    std::uint64_t WorkloadCounters::*member;
    std::uint64_t validity;
    bool hexadecimal;
};
struct TimerField {
    const char* name;
    std::uint64_t WorkloadCounters::*member;
    std::uint64_t timer_validity;
};
#define F(name, validity) {#name, &WorkloadCounters::name, validity, false}
#define FH(name, validity) {#name, &WorkloadCounters::name, validity, true}
const Field common_fields[] = {
    F(input_width,kWorkloadValidInput), F(input_height,kWorkloadValidInput),
    F(decimated_width,kWorkloadValidInput), F(decimated_height,kWorkloadValidInput),
    FH(decimated_checksum,kWorkloadValidInput), FH(threshold_checksum,kWorkloadValidThreshold),
    F(threshold_black_pixels,kWorkloadValidThreshold), F(threshold_uncertain_pixels,kWorkloadValidThreshold),
    F(threshold_white_pixels,kWorkloadValidThreshold), F(boundary_candidates,kWorkloadValidBoundaryCandidates),
    F(boundary_points_emitted,kWorkloadValidSegmentation), F(clusters_created,kWorkloadValidSegmentation),
    F(clusters_after_filters,kWorkloadValidSegmentation), F(cluster_reject_too_few_points,kWorkloadValidSegmentation),
    F(cluster_reject_too_large,kWorkloadValidSegmentation), F(cluster_reject_extent,kWorkloadValidSegmentation),
    F(cluster_reject_border_polarity,kWorkloadValidBorderPolarity), F(points_entering_sort,kWorkloadValidFitting),
    F(points_entering_lfps,kWorkloadValidFitting), F(points_entering_errors,kWorkloadValidFitting),
    F(compute_errors_calls,kWorkloadValidFitting), F(compute_errors_points,kWorkloadValidFitting),
    F(raw_peaks,kWorkloadValidFitting), F(retained_peaks,kWorkloadValidFitting),
    F(quad_fit_attempts,kWorkloadValidFitting), F(quads,kWorkloadValidFitting),
    F(quad_family_candidates,kWorkloadValidDecode), F(homography_rejects,kWorkloadValidDecode),
    F(family_polarity_rejects,kWorkloadValidFamilyPolarity), F(decode_attempts,kWorkloadValidDecode),
    F(photometric_polarity_rejects,kWorkloadValidDecode), F(codeword_rejects,kWorkloadValidDecode),
    F(raw_detections,kWorkloadValidDecode), F(duplicate_rejects,kWorkloadValidDecode),
    F(detections,kWorkloadValidDecode), F(uf_elements,kWorkloadValidUf),
    F(uf_union_attempts,kWorkloadValidUf), F(uf_union_successes,kWorkloadValidUf),
    F(uf_granularity,kWorkloadValidUf), F(line_fit_queries,kWorkloadValidLineFit),
    F(line_fit_query_points,kWorkloadValidLineFit),
};
const Field rust_fields[] = {
    F(rle_runs,kWorkloadValidRustRle), F(pending_boundary_records,kWorkloadValidRustBoundary),
    F(pending_boundary_expanded_points,kWorkloadValidRustBoundary), F(rvv_runs_repacked,kWorkloadValidRustRle),
    F(pair_stats_computed,kWorkloadValidRustPairStats), F(pair_stats_span_points,kWorkloadValidRustPairStats),
    F(pair_table_lookups,kWorkloadValidRustPairStats),
};
const Field c_fields[] = {
    F(active_uf_pixels,kWorkloadValidCSpecific), F(fit_line_calls,kWorkloadValidCSpecific),
    F(fit_line_span_points,kWorkloadValidCSpecific), F(cluster_hash_entries,kWorkloadValidCSpecific),
};
const Field boundary_fields[] = {
    F(boundary_right_checks,kWorkloadValidBoundaryDiagnostics), F(boundary_right_contrast,kWorkloadValidBoundaryDiagnostics),
    F(boundary_right_size_qualified,kWorkloadValidBoundaryDiagnostics), F(boundary_right_emitted,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_checks,kWorkloadValidBoundaryDiagnostics), F(boundary_down_contrast,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_size_qualified,kWorkloadValidBoundaryDiagnostics), F(boundary_down_emitted,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_left_checks,kWorkloadValidBoundaryDiagnostics), F(boundary_down_left_contrast,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_left_size_qualified,kWorkloadValidBoundaryDiagnostics), F(boundary_down_left_emitted,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_right_checks,kWorkloadValidBoundaryDiagnostics), F(boundary_down_right_contrast,kWorkloadValidBoundaryDiagnostics),
    F(boundary_down_right_size_qualified,kWorkloadValidBoundaryDiagnostics), F(boundary_down_right_emitted,kWorkloadValidBoundaryDiagnostics),
    F(boundary_connected_last_suppressions,kWorkloadValidBoundaryDiagnostics),
    F(boundary_exact_duplicates,kWorkloadValidBoundaryDiagnostics), F(boundary_coordinate_duplicates,kWorkloadValidBoundaryDiagnostics),
    F(boundary_exact_unique,kWorkloadValidBoundaryDiagnostics), F(boundary_coordinate_unique,kWorkloadValidBoundaryDiagnostics),
    F(boundary_perimeter_points,kWorkloadValidBoundaryDiagnostics),
};
#define TF(name, validity) {#name, &WorkloadCounters::name, validity}
const TimerField timer_fields[] = {
    TF(ccl_total_ns,kWorkloadTimerTotal), TF(ccl_rle_repack_ns,kWorkloadTimerRleRepack),
    TF(ccl_uf_init_ns,kWorkloadTimerUfInit), TF(ccl_connected_components_ns,kWorkloadTimerConnectedComponents),
    TF(ccl_diag_left_ns,kWorkloadTimerDiagLeft), TF(ccl_diag_right_ns,kWorkloadTimerDiagRight),
    TF(ccl_root_materialize_ns,kWorkloadTimerRootMaterialize), TF(ccl_gradient_clustering_ns,kWorkloadTimerGradientClustering),
    TF(ccl_conversion_ns,kWorkloadTimerConversion),
};
#undef TF
#undef FH
#undef F

bool valid(const WorkloadCounters& c, const Field& f) { return (c.validity & f.validity) == f.validity; }
std::string field_value(const WorkloadCounters& counters, const Field& field)
{
    if (!valid(counters, field)) return "n/a";
    if (!field.hexadecimal) return std::to_string(counters.*field.member);
    std::ostringstream value;
    value << "0x" << std::hex << std::setfill('0') << std::setw(16)
          << counters.*field.member;
    return value.str();
}
void print_fields(const Field* first, const Field* last,
                  const std::vector<WorkloadResult>& results, std::ostream& out)
{
    for (; first != last; ++first) {
        out << std::left << std::setw(42) << first->name;
        for (const auto& result : results) {
            out << std::right << std::setw(22) << field_value(result.counters, *first);
        }
        out << '\n';
    }
}
void print_timers(const std::vector<WorkloadResult>& results, std::ostream& out)
{
    for (const auto& field : timer_fields) {
        out << std::left << std::setw(42) << field.name;
        for (const auto& result : results) {
            const bool available =
                (result.counters.validity & kWorkloadValidCclTimers) != 0 &&
                (result.counters.ccl_timer_validity & field.timer_validity) == field.timer_validity;
            out << std::right << std::setw(22)
                << (available ? std::to_string(result.counters.*field.member) : "n/a");
        }
        out << '\n';
    }
}
std::string ratio(std::uint64_t numerator, std::uint64_t denominator)
{
    if (!denominator) return "n/a";
    std::ostringstream value;
    value << std::fixed << std::setprecision(6)
          << static_cast<double>(numerator) / denominator;
    return value.str();
}

std::string valid_ratio(const WorkloadCounters& counters,
                        std::uint64_t numerator,
                        std::uint64_t denominator,
                        std::uint64_t required_validity)
{
    if ((counters.validity & required_validity) != required_validity)
        return "n/a";
    return ratio(numerator, denominator);
}

void print_ratio(std::ostream& out, const char* label,
                 const WorkloadCounters& numerator_counters,
                 const WorkloadCounters& denominator_counters,
                 std::uint64_t WorkloadCounters::*member,
                 std::uint64_t required_validity)
{
    const bool available =
        (numerator_counters.validity & required_validity) == required_validity &&
        (denominator_counters.validity & required_validity) == required_validity;
    const std::uint64_t denominator = denominator_counters.*member;
    out << "  " << label << ": "
        << (available ? ratio(numerator_counters.*member, denominator) : "n/a");
    if (available && denominator) out << 'x';
    out << '\n';
}
}  // namespace

void validate_workload_pair(const WorkloadResult& a, const WorkloadResult& b)
{
    WorkloadCounters ac = a.counters, bc = b.counters;
    for (auto* counters : {&ac, &bc}) {
        counters->ccl_total_ns = counters->ccl_rle_repack_ns = counters->ccl_uf_init_ns = 0;
        counters->ccl_connected_components_ns = 0;
        counters->ccl_diag_left_ns = counters->ccl_diag_right_ns = counters->ccl_root_materialize_ns = 0;
        counters->ccl_gradient_clustering_ns = counters->ccl_conversion_ns = 0;
    }
    if (a.kind != b.kind || a.detection.count != b.detection.count ||
        a.detection.checksum != b.detection.checksum ||
        std::memcmp(&ac, &bc, sizeof(ac)) != 0) {
        throw std::runtime_error(std::string(backend_name(a.kind)) +
                                 " produced nondeterministic workload counters");
    }
    if (a.counters.schema_version != 2 || a.counters.struct_size != sizeof(WorkloadCounters)) {
        throw std::runtime_error(std::string(backend_name(a.kind)) +
                                 " returned an incompatible workload schema");
    }
}

void print_workload_report(const BenchmarkConfig& config, const PreparedImage& image,
                           const std::vector<WorkloadResult>& results, std::ostream& out)
{
    out << "K230 AprilTag detector workload\nImage: " << image.width << 'x' << image.height
        << "  Factor: " << config.factor_value << "  Min blob: " << config.min_blob << "\n\n"
        << "Common workload counters\n" << std::left << std::setw(42) << "Metric";
    for (const auto& result : results) out << std::right << std::setw(22) << backend_key(result.kind);
    out << '\n';
    print_fields(std::begin(common_fields), std::end(common_fields), results, out);
    out << "\nRust-specific counters\n";
    print_fields(std::begin(rust_fields), std::end(rust_fields), results, out);
    out << "\nC-specific counters\n";
    print_fields(std::begin(c_fields), std::end(c_fields), results, out);
    out << "\nBoundary diagnostics\n";
    print_fields(std::begin(boundary_fields), std::end(boundary_fields), results, out);
    out << "\nCCL substage timers\n";
    print_timers(results, out);
    out << "\nSuccessful detection provenance\n";
    for (const auto& r : results) {
        const bool provenance_valid =
            (r.counters.validity & kWorkloadValidDetectionProvenance) != 0;
        const bool counterfactual_valid =
            (r.counters.validity & kWorkloadValidCounterfactualDedup) != 0;
        out << backend_key(r.kind) << " traces="
            << (provenance_valid ? std::to_string(r.counters.provenance_count) : "n/a")
            << " dropped="
            << (provenance_valid ? std::to_string(r.counters.provenance_dropped) : "n/a") << '\n';
        if (!provenance_valid) continue;
        for (std::uint64_t i=0; i<r.counters.provenance_count && i<16; ++i) {
            const auto& p=r.counters.provenance[i];
            out << "PROVENANCE backend=" << backend_key(r.kind) << " id=" << p.detection_id
                << " raw_index=" << p.raw_index << " final_index=" << p.final_index
                << " component_pair=" << p.component_pair << " bbox=" << p.bbox_min_x << ',' << p.bbox_min_y
                << ',' << p.bbox_max_x << ',' << p.bbox_max_y << " points=" << p.point_count
                << " exact_duplicates=" << p.exact_duplicates << " coordinate_duplicates=" << p.coordinate_duplicates
                << " directions=" << p.right_points << ',' << p.down_points << ',' << p.down_left_points << ',' << p.down_right_points
                << " center=" << std::setprecision(9) << bits_double(p.detection_center_x_bits) << ',' << bits_double(p.detection_center_y_bits)
                << " perimeter=" << p.perimeter_points << " dedup_survives="
                << (counterfactual_valid ? std::to_string(p.dedup_survives) : "n/a")
                << " dedup_id_match=" << (counterfactual_valid ? std::to_string(p.dedup_id_matches) : "n/a")
                << " dedup_geometry_match=" << (counterfactual_valid ? std::to_string(p.dedup_geometry_matches) : "n/a")
                << " dedup_points=" << (counterfactual_valid ? std::to_string(p.dedup_point_count) : "n/a")
                << " dedup_id=" << (counterfactual_valid ? std::to_string(p.dedup_detection_id) : "n/a")
                << " dedup_geometry=" << (counterfactual_valid ? std::to_string(p.dedup_geometry_checksum) : "n/a") << '\n';
        }
    }
    if (results.size() >= 2) {
        for (std::size_t ai = 0; ai < results.size(); ++ai) for (std::size_t bi = ai + 1; bi < results.size(); ++bi) {
            const auto& a = results[ai]; const auto& b = results[bi];
            if (!(a.counters.validity & kWorkloadValidDetectionProvenance) ||
                !(b.counters.validity & kWorkloadValidDetectionProvenance)) continue;
            for (std::uint64_t i = 0; i < a.counters.provenance_count && i < 16; ++i) {
                const auto& p = a.counters.provenance[i]; const DetectionProvenance* nearest = nullptr;
                double best = std::numeric_limits<double>::infinity();
                for (std::uint64_t j = 0; j < b.counters.provenance_count && j < 16; ++j) {
                    const auto& q = b.counters.provenance[j];
                    if (q.detection_id != p.detection_id ||
                        q.detection_geometry_checksum != p.detection_geometry_checksum) continue;
                    const double dx = bits_double(q.detection_center_x_bits) - bits_double(p.detection_center_x_bits);
                    const double dy = bits_double(q.detection_center_y_bits) - bits_double(p.detection_center_y_bits);
                    const double distance = std::sqrt(dx * dx + dy * dy);
                    if (distance < best) { best = distance; nearest = &q; }
                }
                if (nearest) out << "PROVENANCE_MATCH id=" << p.detection_id
                    << " from=" << backend_key(a.kind) << " to=" << backend_key(b.kind)
                    << " center_distance=" << std::fixed << std::setprecision(6) << best
                    << " nearest_component_pair=" << nearest->component_pair
                    << " counterfactual_id_match="
                    << ((a.counters.validity & kWorkloadValidCounterfactualDedup)
                            ? std::to_string(p.dedup_id_matches) : "n/a")
                    << " counterfactual_geometry_match="
                    << ((a.counters.validity & kWorkloadValidCounterfactualDedup)
                            ? std::to_string(p.dedup_geometry_matches) : "n/a") << '\n';
            }
        }
    }
    out << "\nEarly polarity estimates\n";
    for (const auto& r : results) {
        if (r.kind == BackendKind::CReference) continue;
        const auto& c = r.counters;
        out << backend_key(r.kind)
            << " rejected_clusters="
            << ((c.validity & kWorkloadValidHypotheticalEarlyPolarity)
                    ? std::to_string(c.hypothetical_early_polarity_reject_clusters) : "n/a")
            << " rejected_points="
            << ((c.validity & kWorkloadValidHypotheticalEarlyPolarity)
                    ? std::to_string(c.hypothetical_early_polarity_reject_points) : "n/a")
            << " sort_waste_ratio="
            << valid_ratio(c, c.hypothetical_wasted_sort_points, c.points_entering_sort,
                           kWorkloadValidHypotheticalEarlyPolarity | kWorkloadValidFitting)
            << " lfps_waste_ratio="
            << valid_ratio(c, c.hypothetical_wasted_lfps_points, c.points_entering_lfps,
                           kWorkloadValidHypotheticalEarlyPolarity | kWorkloadValidFitting)
            << " errors_waste_ratio="
            << valid_ratio(c, c.hypothetical_wasted_compute_errors_points,
                           c.compute_errors_points,
                            kWorkloadValidHypotheticalEarlyPolarity | kWorkloadValidFitting) << '\n';
    }
    const WorkloadResult* rvv = nullptr; const WorkloadResult* cref = nullptr;
    for (const auto& r : results) { if (r.kind == BackendKind::RustRvv) rvv=&r; if (r.kind == BackendKind::CReference) cref=&r; }
    if (rvv && cref) {
        out << "\nRust RVV/C workload ratios\n";
        print_ratio(out, "boundary points emitted", rvv->counters, cref->counters, &WorkloadCounters::boundary_points_emitted, kWorkloadValidSegmentation);
        print_ratio(out, "clusters after filters", rvv->counters, cref->counters, &WorkloadCounters::clusters_after_filters, kWorkloadValidSegmentation);
        print_ratio(out, "points entering sort", rvv->counters, cref->counters, &WorkloadCounters::points_entering_sort, kWorkloadValidFitting);
        print_ratio(out, "points entering LFPS", rvv->counters, cref->counters, &WorkloadCounters::points_entering_lfps, kWorkloadValidFitting);
        print_ratio(out, "points entering errors", rvv->counters, cref->counters, &WorkloadCounters::points_entering_errors, kWorkloadValidFitting);
        print_ratio(out, "compute-errors points", rvv->counters, cref->counters, &WorkloadCounters::compute_errors_points, kWorkloadValidFitting);
        print_ratio(out, "raw peaks", rvv->counters, cref->counters, &WorkloadCounters::raw_peaks, kWorkloadValidFitting);
        print_ratio(out, "retained peaks", rvv->counters, cref->counters, &WorkloadCounters::retained_peaks, kWorkloadValidFitting);
        print_ratio(out, "quad fit attempts", rvv->counters, cref->counters, &WorkloadCounters::quad_fit_attempts, kWorkloadValidFitting);
        print_ratio(out, "quads", rvv->counters, cref->counters, &WorkloadCounters::quads, kWorkloadValidFitting);
        print_ratio(out, "decode attempts", rvv->counters, cref->counters, &WorkloadCounters::decode_attempts, kWorkloadValidDecode);
        const bool boundary_valid =
            (rvv->counters.validity & kWorkloadValidSegmentation) == kWorkloadValidSegmentation &&
            (cref->counters.validity & kWorkloadValidSegmentation) == kWorkloadValidSegmentation;
        const bool threshold_valid =
            (rvv->counters.validity & kWorkloadValidThreshold) == kWorkloadValidThreshold &&
            (cref->counters.validity & kWorkloadValidThreshold) == kWorkloadValidThreshold;
        out << "ratio_rust_rvv_to_c_reference="
            << (boundary_valid ? ratio(rvv->counters.boundary_points_emitted,
                                       cref->counters.boundary_points_emitted) : "n/a")
            << " threshold_checksum_match="
            << (threshold_valid
                    ? std::to_string(rvv->counters.threshold_checksum ==
                                     cref->counters.threshold_checksum)
                    : "n/a")
            << " output_match=" << (rvv->detection.count == cref->detection.count &&
                                      rvv->detection.checksum == cref->detection.checksum) << '\n';
    }
    for (const auto& r : results) {
        out << "WORKLOAD backend=" << backend_key(r.kind) << " schema=" << r.counters.schema_version
            << " validity=0x" << std::hex << r.counters.validity << std::dec
            << " detections=" << r.detection.count
            << " checksum=" << std::hex << r.detection.checksum << std::dec;
#define PRINT_COUNTER(name) out << " " #name "=" << r.counters.name;
        WORKLOAD_COUNTER_FIELDS(PRINT_COUNTER)
#undef PRINT_COUNTER
        out << '\n';
    }
}

#ifndef APRILTAG_WORKLOAD_FORMAT_TEST
int workload_main(int argc, const char* const argv[], std::ostream& out, std::ostream& err)
{
    try {
        BenchmarkConfig config = parse_args(argc, argv);
        if (config.help) { print_usage(out, argv[0]); return 0; }
        PreparedImage image = load_image(config);
        std::vector<WorkloadResult> results;
        for (BackendKind kind : config.backends) {
            auto backend = kind == BackendKind::CReference
                ? make_workload_c_backend(config) : make_workload_rust_backend(config, kind);
            WorkloadResult first = backend->run(image);
            WorkloadResult second = backend->run(image);
            validate_workload_pair(first, second);
            results.push_back(first);
        }
        print_workload_report(config, image, results, out);
        return 0;
    } catch (const ArgumentError& e) { err << "argument error: " << e.what() << '\n'; return 2; }
      catch (const std::exception& e) { err << "workload error: " << e.what() << '\n'; return 1; }
}
#endif
}  // namespace apriltag_bench

#ifndef APRILTAG_WORKLOAD_FORMAT_TEST
int main(int argc, char* argv[])
{
    return apriltag_bench::workload_main(argc, const_cast<const char* const*>(argv),
                                         std::cout, std::cerr);
}
#endif
