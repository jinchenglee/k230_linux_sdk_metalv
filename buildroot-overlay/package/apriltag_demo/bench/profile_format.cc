#include "benchmark.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <ostream>

namespace apriltag_bench {
namespace {
struct TimingField {
    const char* name;
    std::uint64_t apriltag_ccl_profile_t::*member;
    std::uint64_t validity;
};

#define TIMING(name) {#name, &apriltag_ccl_profile_t::name, APRILTAG_CCL_PROFILE_VALID_TIMINGS}
constexpr TimingField kTimingFields[] = {
    TIMING(total_ns), TIMING(rle_ns), TIMING(repack_label_ns),
    TIMING(uf_init_ns), TIMING(horizontal_ns), TIMING(vertical_ns),
    TIMING(diagonal_left_ns), TIMING(diagonal_right_ns),
    TIMING(root_materialize_ns),
    {"resolve_filter_ns", &apriltag_ccl_profile_t::resolve_filter_ns,
     APRILTAG_CCL_PROFILE_VALID_TIMINGS |
         APRILTAG_CCL_PROFILE_VALID_RESOLVE_FILTER_TIMING},
    TIMING(group_emit_ns), TIMING(conversion_ns), TIMING(diagnostic_ns),
    TIMING(unattributed_ns),
};
#undef TIMING

template <typename Visitor>
void visit_counters(const apriltag_ccl_profile_t& p, Visitor visit)
{
#define FIELD(name, validity) visit(#name, p.name, validity)
    FIELD(runs, APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION);
    FIELD(row_runs_min, APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION);
    FIELD(row_runs_max, APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION);
    FIELD(row_runs_sum, APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION);
    static constexpr const char* directions[] = {
        "horizontal", "vertical", "diagonal_left", "diagonal_right"};
    for (std::size_t i = 0; i < APRILTAG_CCL_DIRECTION_COUNT; ++i) {
        visit((std::string("overlap_comparisons_") + directions[i]).c_str(),
              p.overlap_comparisons[i], APRILTAG_CCL_PROFILE_VALID_OVERLAPS);
        visit((std::string("accepted_overlaps_") + directions[i]).c_str(),
              p.accepted_overlaps[i], APRILTAG_CCL_PROFILE_VALID_OVERLAPS);
        visit((std::string("same_color_edges_") + directions[i]).c_str(),
              p.same_color_edges_by_direction[i], APRILTAG_CCL_PROFILE_VALID_OVERLAPS);
        visit((std::string("union_attempts_") + directions[i]).c_str(),
              p.union_attempts_by_direction[i], APRILTAG_CCL_PROFILE_VALID_UNIONS);
    }
    for (std::size_t i = 0; i < APRILTAG_CCL_BOUNDARY_TYPE_COUNT; ++i) {
        visit(("pending_type_" + std::to_string(i)).c_str(), p.pending_by_type[i],
              APRILTAG_CCL_PROFILE_VALID_BOUNDARIES);
        visit(("pending_expanded_points_type_" + std::to_string(i)).c_str(),
              p.pending_expanded_points_by_type[i], APRILTAG_CCL_PROFILE_VALID_BOUNDARIES);
        visit(("emitted_type_" + std::to_string(i)).c_str(), p.emitted_by_type[i],
              APRILTAG_CCL_PROFILE_VALID_BOUNDARIES);
    }
    FIELD(connected_last_suppressions, APRILTAG_CCL_PROFILE_VALID_BOUNDARIES);
    FIELD(root_equal_rejects, APRILTAG_CCL_PROFILE_VALID_UNIONS);
    FIELD(small_component_rejects, APRILTAG_CCL_PROFILE_VALID_GROUPING);
    FIELD(accepted_grouping_records, APRILTAG_CCL_PROFILE_VALID_GROUPING);
    FIELD(distinct_keys, APRILTAG_CCL_PROFILE_VALID_GROUPING);
    FIELD(exact_duplicates, APRILTAG_CCL_PROFILE_VALID_DIAGNOSTICS);
    FIELD(coordinate_duplicates, APRILTAG_CCL_PROFILE_VALID_DIAGNOSTICS);
    FIELD(pending_growths, APRILTAG_CCL_PROFILE_VALID_GROWTH);
    FIELD(cluster_map_growths, APRILTAG_CCL_PROFILE_VALID_GROWTH);
    FIELD(cluster_vector_growths, APRILTAG_CCL_PROFILE_VALID_GROWTH);
    for (std::size_t site = 0; site < APRILTAG_CCL_UF_CALL_SITE_COUNT; ++site)
        for (std::size_t bin = 0; bin < APRILTAG_CCL_UF_HOP_BIN_COUNT; ++bin)
            visit(("uf_hops_site_" + std::to_string(site) + "_bin_" +
                   std::to_string(bin)).c_str(), p.uf_hops[site][bin],
                  APRILTAG_CCL_PROFILE_VALID_UF_HOPS);
#undef FIELD
}

void validate_schema(const apriltag_ccl_profile_t& profile)
{
    if (profile.version != APRILTAG_CCL_PROFILE_VERSION ||
        profile.struct_size != sizeof(profile))
        throw std::runtime_error("incompatible CCL profile schema");
    if (profile.validity & ~APRILTAG_CCL_PROFILE_VALID_KNOWN)
        throw std::runtime_error("CCL profile contains unknown validity bits");
}

long double unattributed_ratio(const apriltag_ccl_profile_t& profile)
{
    if (!(profile.validity & APRILTAG_CCL_PROFILE_VALID_TIMINGS)) return 0;
    std::uint64_t attributed = 0;
    const auto add_attributed = [&](const char* name, std::uint64_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max() - attributed)
            throw std::runtime_error(std::string("CCL attributed timer sum overflow at ") +
                                     name);
        attributed += value;
    };
    add_attributed("rle_ns", profile.rle_ns);
    add_attributed("repack_label_ns", profile.repack_label_ns);
    add_attributed("uf_init_ns", profile.uf_init_ns);
    add_attributed("horizontal_ns", profile.horizontal_ns);
    add_attributed("vertical_ns", profile.vertical_ns);
    add_attributed("diagonal_left_ns", profile.diagonal_left_ns);
    add_attributed("diagonal_right_ns", profile.diagonal_right_ns);
    add_attributed("root_materialize_ns", profile.root_materialize_ns);
    add_attributed("group_emit_ns", profile.group_emit_ns);
    add_attributed("conversion_ns", profile.conversion_ns);
    add_attributed("diagnostic_ns", profile.diagnostic_ns);
    if (profile.validity & APRILTAG_CCL_PROFILE_VALID_RESOLVE_FILTER_TIMING)
        add_attributed("resolve_filter_ns", profile.resolve_filter_ns);
    if (profile.total_ns < attributed)
        throw std::runtime_error("CCL timer stages exceed total");
    if (profile.unattributed_ns != profile.total_ns - attributed)
        throw std::runtime_error("CCL unattributed timer violates conservation");
    return profile.total_ns
        ? static_cast<long double>(profile.unattributed_ns) / profile.total_ns : 0;
}
}

std::size_t profile_timing_descriptor_count() { return std::size(kTimingFields); }

std::size_t profile_counter_descriptor_count()
{
    std::size_t count = 0;
    apriltag_ccl_profile_t profile{};
    visit_counters(profile, [&](const char*, std::uint64_t, std::uint64_t) { ++count; });
    return count;
}

void validate_profile_sequence(const std::vector<apriltag_ccl_profile_t>& profiles)
{
    if (profiles.empty()) throw std::runtime_error("no CCL profile snapshots");
    for (const auto& profile : profiles) {
        validate_schema(profile);
        (void)unattributed_ratio(profile);
    }
    const auto& first = profiles.front();
    for (std::size_t i = 1; i < profiles.size(); ++i) {
        const auto& current = profiles[i];
        if (current.validity != first.validity)
            throw std::runtime_error("CCL profile validity changed across measured calls");
        std::size_t index = 0;
        std::vector<std::uint64_t> expected;
        visit_counters(first, [&](const char*, std::uint64_t value, std::uint64_t validity) {
            if ((first.validity & validity) == validity) expected.push_back(value);
        });
        visit_counters(current, [&](const char* name, std::uint64_t value,
                                    std::uint64_t validity) {
            if ((first.validity & validity) != validity) return;
            if (value != expected[index++])
                throw std::runtime_error(std::string("CCL workload counter changed: ") + name);
        });
    }
}

void print_profile_report(const BenchmarkConfig& config, BackendKind kind,
                          const std::vector<apriltag_ccl_profile_t>& profiles,
                          std::ostream& out)
{
    validate_profile_sequence(profiles);
    long double ratio_total = 0;
    long double ratio_max = 0;
    for (const auto& profile : profiles) {
        const long double ratio = unattributed_ratio(profile);
        ratio_total += ratio;
        ratio_max = std::max(ratio_max, ratio);
    }
    out << "CCL_TIMER_HEALTH backend=" << backend_key(kind) << " rvv_mask=";
    if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask << std::dec;
    else out << "all";
    out << " stages=" << (config.rvv_mask_explicit ? config.rvv_stages : "all")
        << " count=" << profiles.size() << " diagnostic_included=1"
        << std::fixed << std::setprecision(3)
        << " mean_unattributed_ratio_pct="
        << static_cast<double>(ratio_total * 100 / profiles.size())
        << " max_unattributed_ratio_pct=" << static_cast<double>(ratio_max * 100)
        << " warning=" << (ratio_max > 0.10L ? 1 : 0) << '\n';
    if (ratio_max > 0.10L)
        out << "WARNING: CCL timer unattributed ratio exceeds 10%\n";
    if (!(profiles.front().validity & APRILTAG_CCL_PROFILE_VALID_TIMINGS)) {
        out << "STAGE backend=" << backend_key(kind) << " rvv_mask=";
        if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask << std::dec;
        else out << "all";
        out << " stages=" << (config.rvv_mask_explicit ? config.rvv_stages : "all")
            << " available=0 reason=timings-invalid\n";
    }
    for (const auto& field : kTimingFields) {
        if ((profiles.front().validity & field.validity) != field.validity) continue;
        std::vector<std::uint64_t> values;
        values.reserve(profiles.size());
        for (const auto& profile : profiles) values.push_back(profile.*field.member);
        std::sort(values.begin(), values.end());
        const long double total = std::accumulate(values.begin(), values.end(), 0.0L);
        const long double median = values.size() % 2
            ? values[values.size() / 2]
            : (static_cast<long double>(values[values.size() / 2 - 1]) +
               values[values.size() / 2]) / 2;
        const std::size_t p95 = static_cast<std::size_t>(
            std::ceil(0.95 * values.size())) - 1;
        std::string stage(field.name);
        stage.resize(stage.size() - 3);
        out << "STAGE backend=" << backend_key(kind) << " rvv_mask=";
        if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask << std::dec;
        else out << "all";
        out << " stages=" << (config.rvv_mask_explicit ? config.rvv_stages : "all")
            << " stage=" << stage
            << " count=" << values.size() << " min_ns=" << values.front()
            << std::fixed << std::setprecision(3)
            << " median_ns=" << static_cast<double>(median)
            << " mean_ns=" << static_cast<double>(total / values.size())
            << std::defaultfloat << " p95_ns=" << values[p95]
            << " max_ns=" << values.back() << '\n';
    }
    const auto& profile = profiles.back();
    out << "CCL_WORK backend=" << backend_key(kind) << " rvv_mask=";
    if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask << std::dec;
    else out << "all";
    out << " stages=" << (config.rvv_mask_explicit ? config.rvv_stages : "all")
        << " validity=0x"
        << std::hex << profile.validity << std::dec;
    visit_counters(profile, [&](const char* name, std::uint64_t value,
                                std::uint64_t validity) {
        if ((profile.validity & validity) == validity) out << ' ' << name << '=' << value;
    });
    out << '\n';
}
}  // namespace apriltag_bench
