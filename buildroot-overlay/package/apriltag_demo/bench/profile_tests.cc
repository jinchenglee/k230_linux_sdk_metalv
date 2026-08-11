#include "benchmark.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

using namespace apriltag_bench;

namespace {
int failures;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct ProfileState { int detects = 0; int profiles = 0; int fail_profile = 0; };

std::string record_with(const std::string& text, const std::string& type,
                        const std::string& key, const std::string& value)
{
    const std::string selector = " " + key + "=" + value;
    for (std::size_t begin = 0; begin < text.size();) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end - begin);
        if (line.compare(0, type.size() + 1, type + " ") == 0 &&
            line.find(selector) != std::string::npos)
            return line;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

class ProfileBackend final : public Backend {
public:
    explicit ProfileBackend(std::shared_ptr<ProfileState> state) : state_(std::move(state)) {}
    BackendKind kind() const override { return BackendKind::RustRvv; }
    const char* name() const override { return "profile fake"; }
    void set_capture_detections(bool) override {}
    DetectionResult detect(const PreparedImage&) override { ++state_->detects; return {1, 7}; }
    const std::vector<Detection>& detections() const override { return detections_; }
    bool consume_profile(apriltag_ccl_profile_t& profile) override
    {
        ++state_->profiles;
        if (state_->profiles == state_->fail_profile)
            throw std::runtime_error("mock getter failed");
        std::memset(&profile, 0, sizeof(profile));
        profile.version = APRILTAG_CCL_PROFILE_VERSION;
        profile.struct_size = sizeof(profile);
        profile.validity = APRILTAG_CCL_PROFILE_VALID_ALL;
        profile.total_ns = 100 * state_->profiles;
        profile.rle_ns = 10 * state_->profiles;
        profile.unattributed_ns = 90 * state_->profiles;
        profile.runs = 42;
        profile.overlap_comparisons[0] = 9;
        return true;
    }
private:
    std::shared_ptr<ProfileState> state_;
    std::vector<Detection> detections_;
};

void test_collection_and_format()
{
    BenchmarkConfig config;
    config.backends = {BackendKind::RustRvv};
    config.warmup = 2;
    config.iterations = 2;
    config.batches = 2;
    PreparedImage image{1, 1, 1, {0}};
    auto state = std::make_shared<ProfileState>();
    auto backend = std::make_unique<ProfileBackend>(state);
    std::vector<std::unique_ptr<Backend>> backends;
    backends.push_back(std::move(backend));
    std::ostringstream out;
    CHECK(run_benchmark(config, std::move(backends), image, out) == 0);
    CHECK(state->detects == 7);
    CHECK(state->profiles == 4);
    const std::string total = record_with(out.str(), "STAGE", "stage", "total");
    CHECK(total.find(" backend=rust-rvv") != std::string::npos);
    CHECK(total.find(" rvv_mask=all") != std::string::npos);
    CHECK(total.find(" stages=all") != std::string::npos);
    CHECK(total.find(" count=4 min_ns=100") != std::string::npos);
    CHECK(total.find(" median_ns=250.000 mean_ns=250.000 p95_ns=400 max_ns=400") !=
          std::string::npos);
    const std::string rle = record_with(out.str(), "STAGE", "stage", "rle");
    CHECK(rle.find(" backend=rust-rvv") != std::string::npos);
    CHECK(rle.find(" rvv_mask=all") != std::string::npos);
    CHECK(rle.find(" stages=all") != std::string::npos);
    CHECK(rle.find(" count=4 min_ns=10") != std::string::npos);
    const std::string work = record_with(out.str(), "CCL_WORK", "runs", "42");
    CHECK(work.find(" backend=rust-rvv") != std::string::npos);
    CHECK(work.find(" rvv_mask=all") != std::string::npos);
    CHECK(work.find(" stages=all") != std::string::npos);
    CHECK(work.find(" validity=0x1ff") != std::string::npos);
    CHECK(work.find(" overlap_comparisons_horizontal=9") != std::string::npos);
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos;
         at += needle.size()) ++count;
    return count;
}

void test_getter_failure_has_measured_call_context()
{
    BenchmarkConfig config;
    config.backends = {BackendKind::RustRvv};
    config.warmup = 1;
    config.iterations = 2;
    config.batches = 2;
    PreparedImage image{1, 1, 1, {0}};
    auto state = std::make_shared<ProfileState>();
    state->fail_profile = 3;
    std::vector<std::unique_ptr<Backend>> backends;
    backends.emplace_back(new ProfileBackend(state));
    std::ostringstream out;
    std::string message;
    try { (void)run_benchmark(config, std::move(backends), image, out); }
    catch (const std::runtime_error& error) { message = error.what(); }
    CHECK(message.find("profile fake (rust-rvv)") != std::string::npos);
    CHECK(message.find("batch 2") != std::string::npos);
    CHECK(message.find("call 1") != std::string::npos);
    CHECK(message.find("mock getter failed") != std::string::npos);
}

void test_exhaustive_profile_keys()
{
    apriltag_ccl_profile_t profile{};
    profile.version = APRILTAG_CCL_PROFILE_VERSION;
    profile.struct_size = sizeof(profile);
    profile.validity = APRILTAG_CCL_PROFILE_VALID_ALL;
    std::ostringstream out;
    print_profile_report(BenchmarkConfig{}, BackendKind::RustRvv, {profile}, out);
    const std::string text = out.str();
    for (const char* stage : {"total", "rle", "repack_label", "uf_init",
             "horizontal", "vertical", "diagonal_left", "diagonal_right",
             "root_materialize", "group_emit", "conversion",
             "diagnostic", "unattributed"}) {
        const std::string record = record_with(text, "STAGE", "stage", stage);
        CHECK(!record.empty());
        CHECK(record.find(" backend=rust-rvv") != std::string::npos);
        CHECK(record.find(" rvv_mask=all") != std::string::npos);
        CHECK(record.find(" stages=all") != std::string::npos);
    }
    CHECK(text.find("stage=resolve_filter") == std::string::npos);
    std::vector<std::string> keys = {
        "runs", "row_runs_min", "row_runs_max", "row_runs_sum",
        "connected_last_suppressions", "root_equal_rejects",
        "small_component_rejects", "accepted_grouping_records", "distinct_keys",
        "exact_duplicates", "coordinate_duplicates", "pending_growths",
        "cluster_map_growths", "cluster_vector_growths"};
    for (const char* prefix : {"overlap_comparisons_", "accepted_overlaps_",
                               "same_color_edges_", "union_attempts_"})
        for (const char* direction : {"horizontal", "vertical", "diagonal_left",
                                      "diagonal_right"})
            keys.emplace_back(std::string(prefix) + direction);
    for (int type = 0; type < APRILTAG_CCL_BOUNDARY_TYPE_COUNT; ++type)
        for (const char* prefix : {"pending_type_", "pending_expanded_points_type_",
                                   "emitted_type_"})
            keys.emplace_back(std::string(prefix) + std::to_string(type));
    for (int site = 0; site < APRILTAG_CCL_UF_CALL_SITE_COUNT; ++site)
        for (int bin = 0; bin < APRILTAG_CCL_UF_HOP_BIN_COUNT; ++bin)
            keys.emplace_back("uf_hops_site_" + std::to_string(site) + "_bin_" +
                              std::to_string(bin));
    CHECK(keys.size() == 77);
    CHECK(profile_timing_descriptor_count() == 14);
    CHECK(profile_counter_descriptor_count() == 77);
    for (const auto& key : keys) CHECK(occurrences(text, " " + key + "=") == 1);
    CHECK(occurrences(text, "CCL_WORK backend=rust-rvv") == 1);
}

void test_validity_aware_reporting()
{
    apriltag_ccl_profile_t profile{};
    profile.version = APRILTAG_CCL_PROFILE_VERSION;
    profile.struct_size = sizeof(profile);
    profile.validity = APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION;
    profile.total_ns = 999;
    profile.resolve_filter_ns = 888;
    profile.runs = 7;
    profile.overlap_comparisons[0] = 123;
    std::ostringstream out;
    print_profile_report(BenchmarkConfig{}, BackendKind::RustRvv, {profile}, out);
    const std::string text = out.str();
    const std::string unavailable = record_with(text, "STAGE", "available", "0");
    CHECK(unavailable.find(" backend=rust-rvv") != std::string::npos);
    CHECK(unavailable.find(" rvv_mask=all") != std::string::npos);
    CHECK(unavailable.find(" stages=all") != std::string::npos);
    CHECK(unavailable.find(" reason=timings-invalid") != std::string::npos);
    CHECK(text.find("stage=total") == std::string::npos);
    CHECK(text.find("stage=resolve_filter") == std::string::npos);
    const std::string work = record_with(text, "CCL_WORK", "runs", "7");
    CHECK(work.find(" backend=rust-rvv") != std::string::npos);
    CHECK(work.find(" rvv_mask=all") != std::string::npos);
    CHECK(work.find(" stages=all") != std::string::npos);
    CHECK(work.find(" validity=0x2") != std::string::npos);
    CHECK(text.find("overlap_comparisons_horizontal") == std::string::npos);
    CHECK(text.find("=999") == std::string::npos);
    CHECK(text.find("=888") == std::string::npos);
    CHECK(text.find("=123") == std::string::npos);
}

apriltag_ccl_profile_t timing_profile(std::uint64_t total,
                                      std::uint64_t rle,
                                      std::uint64_t diagnostic,
                                      std::uint64_t unattributed)
{
    apriltag_ccl_profile_t profile{};
    profile.version = APRILTAG_CCL_PROFILE_VERSION;
    profile.struct_size = sizeof(profile);
    profile.validity = APRILTAG_CCL_PROFILE_VALID_TIMINGS;
    profile.total_ns = total;
    profile.rle_ns = rle;
    profile.diagnostic_ns = diagnostic;
    profile.unattributed_ns = unattributed;
    return profile;
}

void test_timer_conservation_rejects_sum_over_total()
{
    bool threw = false;
    try { validate_profile_sequence({timing_profile(9, 8, 2, 0)}); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void test_timer_conservation_rejects_wrong_unattributed()
{
    bool threw = false;
    try { validate_profile_sequence({timing_profile(100, 60, 10, 29)}); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void test_timer_conservation_rejects_attributed_sum_overflow()
{
    auto profile = timing_profile(UINT64_MAX, UINT64_MAX, 1, UINT64_MAX);
    bool threw = false;
    try { validate_profile_sequence({profile}); }
    catch (const std::runtime_error& error) {
        threw = std::string(error.what()) ==
            "CCL attributed timer sum overflow at diagnostic_ns";
    }
    CHECK(threw);

    profile = timing_profile(UINT64_MAX, UINT64_MAX, 0, UINT64_MAX);
    profile.validity |= APRILTAG_CCL_PROFILE_VALID_RESOLVE_FILTER_TIMING;
    profile.resolve_filter_ns = 1;
    threw = false;
    try { validate_profile_sequence({profile}); }
    catch (const std::runtime_error& error) {
        threw = std::string(error.what()) ==
            "CCL attributed timer sum overflow at resolve_filter_ns";
    }
    CHECK(threw);
}

void test_timer_health_reports_healthy_ratios_and_diagnostic_inclusion()
{
    const auto first = timing_profile(100, 80, 15, 5);
    const auto second = timing_profile(200, 170, 20, 10);
    std::ostringstream out;
    print_profile_report(BenchmarkConfig{}, BackendKind::RustRvv,
                         {first, second}, out);
    const std::string health = record_with(out.str(), "CCL_TIMER_HEALTH",
                                           "warning", "0");
    CHECK(health.find(" diagnostic_included=1") != std::string::npos);
    CHECK(health.find(" mean_unattributed_ratio_pct=5.000") != std::string::npos);
    CHECK(health.find(" max_unattributed_ratio_pct=5.000") != std::string::npos);
}

void test_timer_health_warns_above_ten_percent()
{
    const auto profile = timing_profile(100, 80, 0, 20);
    std::ostringstream out;
    print_profile_report(BenchmarkConfig{}, BackendKind::RustRvv, {profile}, out);
    CHECK(record_with(out.str(), "CCL_TIMER_HEALTH", "warning", "1")
              .find(" max_unattributed_ratio_pct=20.000") != std::string::npos);
    CHECK(out.str().find("WARNING: CCL timer unattributed ratio exceeds 10%") !=
          std::string::npos);
}

void test_fieldwise_stability_honors_validity()
{
    apriltag_ccl_profile_t first{};
    first.version = APRILTAG_CCL_PROFILE_VERSION;
    first.struct_size = sizeof(first);
    first.validity = APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION;
    first.runs = 4;
    auto changed = first;
    changed.reserved[0] = 1;
    validate_profile_sequence({first, changed});
    changed = first;
    changed.overlap_comparisons[0] = 9;
    validate_profile_sequence({first, changed});
    changed = first;
    changed.runs = 5;
    bool rejected = false;
    try { validate_profile_sequence({first, changed}); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    changed = first;
    changed.validity |= APRILTAG_CCL_PROFILE_VALID_OVERLAPS;
    rejected = false;
    try { validate_profile_sequence({first, changed}); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
}

void test_profile_help()
{
    std::ostringstream out;
    print_usage(out, "profile-bench");
    const std::string text = out.str();
    CHECK(text.find("--backend BACKEND     rust-rvv only") != std::string::npos);
    CHECK(text.find("decimate,threshold,rle,lfps-tuned,gaussian,gray-model") !=
          std::string::npos);
    CHECK(text.find("rust-scalar") == std::string::npos);
    CHECK(text.find("--backend BACKEND     all") == std::string::npos);
}

void test_counter_instability_rejected()
{
    apriltag_ccl_profile_t first{};
    first.version = APRILTAG_CCL_PROFILE_VERSION;
    first.struct_size = sizeof(first);
    first.validity = APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION;
    first.runs = 1;
    auto second = first;
    second.runs = 2;
    bool threw = false;
    try { validate_profile_sequence({first, second}); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void test_profile_configuration()
{
    const char* defaults_argv[] = {"profile-bench"};
    CHECK(parse_args(1, defaults_argv).backends ==
          std::vector<BackendKind>{BackendKind::RustRvv});
    for (const char* value : {"c", "all", "rust-scalar"}) {
        const char* argv[] = {"profile-bench", "--backend", value};
        bool threw = false;
        try { (void)parse_args(3, argv); }
        catch (const ArgumentError&) { threw = true; }
        CHECK(threw);
    }
}
}

int main()
{
    test_collection_and_format();
    test_getter_failure_has_measured_call_context();
    test_exhaustive_profile_keys();
    test_validity_aware_reporting();
    test_timer_conservation_rejects_sum_over_total();
    test_timer_conservation_rejects_wrong_unattributed();
    test_timer_conservation_rejects_attributed_sum_overflow();
    test_timer_health_reports_healthy_ratios_and_diagnostic_inclusion();
    test_timer_health_warns_above_ten_percent();
    test_fieldwise_stability_honors_validity();
    test_profile_help();
    test_counter_instability_rejected();
    test_profile_configuration();
    if (failures) return 1;
    std::cout << "all profile benchmark tests passed\n";
    return 0;
}
