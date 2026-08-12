#include "workload_backend.h"

#include <cstring>
#include <iomanip>
#include <iostream>
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
    if (a.kind != b.kind || a.detection.count != b.detection.count ||
        a.detection.checksum != b.detection.checksum ||
        std::memcmp(&a.counters, &b.counters, sizeof(a.counters)) != 0) {
        throw std::runtime_error(std::string(backend_name(a.kind)) +
                                 " produced nondeterministic workload counters");
    }
    if (a.counters.schema_version != 1 || a.counters.struct_size != sizeof(WorkloadCounters)) {
        throw std::runtime_error(std::string(backend_name(a.kind)) +
                                 " returned an incompatible workload schema");
    }
}

void print_workload_report(const BenchmarkConfig& config, const PreparedImage& image,
                           const std::vector<WorkloadResult>& results, std::ostream& out)
{
    const std::uint64_t input_hash = checksum_bytes(image.pixels.data(), image.pixels.size());
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
        out << "WORKLOAD backend=" << backend_key(r.kind) << " rvv_mask=";
        if (r.kind == BackendKind::CReference) out << "n/a stages=n/a";
        else if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask
                                                << std::dec << " stages=" << config.rvv_stages;
        else out << (r.kind == BackendKind::RustRvv ? "all stages=all" : "none stages=none");
        out << " schema=" << r.counters.schema_version
            << " input_hash=" << std::hex << input_hash << std::dec
            << " width=" << image.width << " height=" << image.height
            << " validity=0x" << std::hex << r.counters.validity << std::dec
            << " result_detections=" << r.detection.count
            << " result_checksum=" << std::hex << r.detection.checksum << std::dec;
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
