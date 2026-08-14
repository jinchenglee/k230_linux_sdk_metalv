#include "sequence.h"
#include "apriltag.h"
#include "apriltag_pending_profile.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

using namespace apriltag_bench;

namespace {
int failures;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos;
         at += needle.size()) ++count;
    return count;
}

template <typename Function>
bool throws(Function function)
{
    try { function(); }
    catch (const std::exception&) { return true; }
    return false;
}

template <typename Function>
bool throws_with(Function function, const std::string& needle)
{
    try { function(); }
    catch (const std::exception& error) {
        return std::string(error.what()).find(needle) != std::string::npos;
    }
    return false;
}

void test_sha256_vectors()
{
    CHECK(sha256_hex({}) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex({'a', 'b', 'c'}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::vector<std::uint8_t>(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256_hex(std::vector<std::uint8_t>(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256_hex(std::vector<std::uint8_t>(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(sha256_hex(std::vector<std::uint8_t>(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_manifest_accepts_spaces_and_exact_fields()
{
    const std::string hash(64, 'a');
    std::istringstream input("scene-1\t/tmp/a frame.jpg\t799\t533\t" + hash + "\n");
    const auto rows = parse_sequence_manifest(input);
    CHECK(rows.size() == 1);
    CHECK(rows[0].label == "scene-1");
    CHECK(rows[0].path == "/tmp/a frame.jpg");
    CHECK(rows[0].width == 799 && rows[0].height == 533);
    CHECK(rows[0].sha256 == hash);
}

void test_manifest_rejects_unsafe_syntax()
{
    const std::string hash(64, 'a');
    CHECK(throws([&] { std::istringstream in("bad label\t/x\t1\t1\t" + hash + "\n");
                       (void)parse_sequence_manifest(in); }));
    CHECK(throws([&] { std::istringstream in(".\t/x\t1\t1\t" + hash + "\n");
                       (void)parse_sequence_manifest(in); }));
    CHECK(throws([&] { std::istringstream in("a\t/x\t1\t1\t" + hash + "\textra\n");
                       (void)parse_sequence_manifest(in); }));
    CHECK(throws([&] { std::istringstream in("a\t/x\t0\t1\t" + hash + "\n");
                       (void)parse_sequence_manifest(in); }));
    CHECK(throws([&] { std::istringstream in("a\t/x\t1\t1\t" + std::string(64, 'A') + "\n");
                       (void)parse_sequence_manifest(in); }));
    CHECK(throws([&] { std::istringstream in("\n"); (void)parse_sequence_manifest(in); }));
    const std::string nul_label("a\0b", 3);
    CHECK(throws([&] { std::istringstream in(nul_label + "\t/x\t1\t1\t" + hash + "\n");
                       (void)parse_sequence_manifest(in); }));
    const std::string nul_path("/x\0y", 4);
    CHECK(throws([&] { std::istringstream in("a\t" + nul_path + "\t1\t1\t" + hash + "\n");
                       (void)parse_sequence_manifest(in); }));
}

void test_manifest_rejects_duplicates()
{
    const std::string hash(64, 'b');
    CHECK(throws([&] {
        std::istringstream in("a\t/x\t1\t1\t" + hash + "\na\t/y\t1\t1\t" + hash + "\n");
        (void)parse_sequence_manifest(in);
    }));
}

void test_file_validation_allows_identical_repeated_input()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto file = root / "image with spaces.bin";
    { std::ofstream out(file, std::ios::binary); out << "abc"; }
    const auto alias = root / "alias.bin";
    std::filesystem::create_symlink(file, alias);
    SequenceRow row{"one", file.string(), 1, 1,
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    const auto loaded = load_sequence_inputs({row});
    CHECK(loaded.size() == 1 && loaded[0].file.sha256 == row.sha256);
    row.path = alias.string();
    CHECK(throws([&] { (void)load_sequence_inputs({row}); }));
    row.path = file.string();
    auto duplicate = row;
    duplicate.label = "two";
    auto repeated = load_sequence_inputs({row, duplicate});
    CHECK(repeated.size() == 2);
    const auto hardlink = root / "hardlink.bin";
    std::filesystem::create_hard_link(file, hardlink);
    duplicate.path = hardlink.string();
    repeated = load_sequence_inputs({row, duplicate});
    CHECK(repeated.size() == 2);
    duplicate.sha256 = std::string(64, '0');
    CHECK(throws([&] { (void)load_sequence_inputs({row, duplicate}); }));
    std::filesystem::remove_all(root);
}

void test_snapshot_hash_and_rehash()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-rehash";
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto file = root / "image.bin";
    { std::ofstream out(file, std::ios::binary); out << "abc"; }
    SequenceRow row{"one", file.string(), 1, 1,
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    const auto inputs = load_sequence_inputs({row});
    verify_sequence_inputs_unchanged(inputs);
    { std::ofstream out(file, std::ios::binary); out << "abd"; }
    CHECK(throws([&] { verify_sequence_inputs_unchanged(inputs); }));
    std::filesystem::remove_all(root);
}

void test_verified_read_is_transient_and_rejects_post_snapshot_mutation()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-reread";
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto file = root / "image.bin";
    { std::ofstream out(file, std::ios::binary); out << "abc"; }
    const FileIdentity identity = snapshot_regular_file(file.string());
    CHECK(read_verified_file(identity) == std::vector<std::uint8_t>({'a', 'b', 'c'}));
    { std::ofstream out(file, std::ios::binary); out << "abd"; }
    CHECK(throws([&] { (void)read_verified_file(identity); }));
    std::filesystem::remove_all(root);
}

void test_manifest_snapshot_rejects_symlink_and_detects_replacement()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-manifest-snapshot";
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto manifest = root / "manifest.tsv";
    { std::ofstream out(manifest, std::ios::binary); out << "manifest"; }
    const auto symlink = root / "manifest-link.tsv";
    std::filesystem::create_symlink(manifest, symlink);
    CHECK(throws([&] { (void)snapshot_regular_file(symlink.string()); }));
    const FileIdentity snapshot = snapshot_regular_file(manifest.string());
    verify_file_snapshot(snapshot);
    const auto replacement = root / "replacement.tsv";
    { std::ofstream out(replacement, std::ios::binary); out << "manifest"; }
    std::filesystem::rename(replacement, manifest);
    CHECK(throws([&] { verify_file_snapshot(snapshot); }));
    std::filesystem::remove_all(root);
}

void test_scratch_invariants()
{
    apriltag_ccl_scratch_v1_t scratch{};
    scratch.version = APRILTAG_CCL_SCRATCH_VERSION;
    scratch.struct_size = sizeof(scratch);
    scratch.validity = APRILTAG_CCL_SCRATCH_VALID_ALL;
    scratch.pending = {2, 4, 3, 1, 1, 64, 48, 16};
    scratch.diagonal_left = {1, 2, 2, 0, 0, 48, 48, 24};
    scratch.diagonal_right = scratch.diagonal_left;
    validate_sequence_scratch(scratch);
    auto bad = scratch;
    bad.pending.len = 5;
    CHECK(throws([&] { validate_sequence_scratch(bad); }));
    bad = scratch;
    bad.pending.capacity_bytes++;
    CHECK(throws([&] { validate_sequence_scratch(bad); }));
}

apriltag_ccl_pending_profile_v1_t valid_pending_profile()
{
    apriltag_ccl_pending_profile_v1_t pending{};
    pending.version = APRILTAG_CCL_PENDING_PROFILE_VERSION;
    pending.struct_size = sizeof(pending);
    pending.validity = APRILTAG_CCL_PENDING_PROFILE_VALID_ALL;
    pending.sample_stride = APRILTAG_CCL_PENDING_SAMPLE_STRIDE;
    return pending;
}

apriltag_ccl_pending_profile_v1_t populated_pending_profile(
    apriltag_ccl_profile_t& profile)
{
    auto pending = valid_pending_profile();
    pending.pending_records_by_type[0] = 65;
    pending.pending_units_by_type[0] = 80;
    pending.accepted_records_by_type[0] = 1;
    pending.accepted_units_by_type[0] = 4;
    pending.root_equal_by_type[0] = 63;
    pending.small_component_by_type[0] = 1;
    pending.range_histogram[0][0] = 64;
    pending.range_histogram[0][1] = 1;
    pending.sampled_records = 2;
    pending.sampled_units = 3;
    pending.sampled_accepted_records = 1;
    pending.timer_intervals = 6;
    profile.pending_by_type[0] = 65;
    profile.pending_expanded_points_by_type[0] = 80;
    profile.emitted_by_type[0] = 4;
    profile.root_equal_rejects = 63;
    profile.small_component_rejects = 1;
    profile.accepted_grouping_records = 1;
    return pending;
}

void test_pending_profile_validation_rejects_schema_and_validity_errors()
{
    apriltag_ccl_profile_t profile{};
    const auto valid = populated_pending_profile(profile);
    validate_sequence_pending_profile(valid, profile);
    auto bad = valid;
    bad.version++;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.struct_size--;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.validity |= UINT64_C(1) << 63;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.validity &= ~APRILTAG_CCL_PENDING_PROFILE_VALID_SAMPLED_TIMINGS;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.sample_stride = 32;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
}

void test_pending_profile_validation_rejects_conservation_errors()
{
    apriltag_ccl_profile_t profile{};
    const auto valid = populated_pending_profile(profile);
    auto bad = valid;
    bad.range_histogram[0][0]--;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.root_equal_by_type[0]--;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.accepted_units_by_type[0] = bad.pending_units_by_type[0] + 1;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.sampled_records--;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.sampled_units = bad.sampled_records - 1;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.sampled_units = 81;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.sampled_accepted_records = 3;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
    bad = valid;
    bad.timer_intervals--;
    CHECK(throws([&] { validate_sequence_pending_profile(bad, profile); }));
}

void test_pending_profile_validation_rejects_reserved_fields()
{
    apriltag_ccl_profile_t profile{};
    const auto valid = populated_pending_profile(profile);
    auto bad = valid;
    bad.reserved_u32 = 1;
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, profile); },
                      "reserved_u32"));
    bad = valid;
    bad.reserved[11] = 1;
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, profile); },
                      "reserved array"));
}

void test_pending_profile_validation_rejects_count_overflow()
{
    apriltag_ccl_profile_t profile{};
    const auto valid = populated_pending_profile(profile);
    auto bad = valid;
    bad.range_histogram[0][0] = std::numeric_limits<std::uint64_t>::max();
    bad.range_histogram[0][1] = 1;
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, profile); },
                      "histogram sum overflow"));

    bad = valid;
    bad.accepted_records_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    bad.root_equal_by_type[0] = 1;
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, profile); },
                      "outcome sum overflow"));

    bad = valid;
    bad.pending_records_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    bad.pending_records_by_type[1] = 1;
    bad.range_histogram[0][0] = std::numeric_limits<std::uint64_t>::max();
    bad.range_histogram[0][1] = 0;
    bad.range_histogram[1][0] = 1;
    bad.accepted_records_by_type[0] = 0;
    bad.root_equal_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    bad.root_equal_by_type[1] = 1;
    bad.small_component_by_type[0] = 0;
    bad.pending_units_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    bad.pending_units_by_type[1] = 1;
    profile.pending_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    profile.pending_by_type[1] = 1;
    profile.pending_expanded_points_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    profile.pending_expanded_points_by_type[1] = 1;
    profile.root_equal_rejects = std::numeric_limits<std::uint64_t>::max();
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, profile); },
                      "global sum overflow"));
}

void test_pending_profile_validation_handles_ceil_and_timer_overflow()
{
    apriltag_ccl_profile_t profile{};
    auto pending = valid_pending_profile();
    pending.pending_records_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    pending.pending_units_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    pending.root_equal_by_type[0] = std::numeric_limits<std::uint64_t>::max();
    pending.range_histogram[0][0] = std::numeric_limits<std::uint64_t>::max();
    pending.sampled_records =
        std::numeric_limits<std::uint64_t>::max() / APRILTAG_CCL_PENDING_SAMPLE_STRIDE + 1;
    pending.sampled_units = pending.sampled_records;
    pending.timer_intervals = 2 * pending.sampled_records;
    profile.pending_by_type[0] = pending.pending_records_by_type[0];
    profile.pending_expanded_points_by_type[0] = pending.pending_units_by_type[0];
    profile.root_equal_rejects = pending.root_equal_by_type[0];
    validate_sequence_pending_profile(pending, profile);

    apriltag_ccl_profile_t ordinary_profile{};
    auto bad = populated_pending_profile(ordinary_profile);
    bad.sampled_records = std::numeric_limits<std::uint64_t>::max();
    bad.sampled_units = std::numeric_limits<std::uint64_t>::max();
    bad.sampled_accepted_records = 1;
    CHECK(throws_with([&] { validate_sequence_pending_profile(bad, ordinary_profile); },
                      "timer interval overflow"));
}

void test_pending_profile_validation_rejects_ccl_profile_disagreement()
{
    apriltag_ccl_profile_t profile{};
    const auto pending = populated_pending_profile(profile);
    auto bad = profile;
    bad.pending_by_type[0]++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
    bad = profile;
    bad.pending_expanded_points_by_type[0]++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
    bad = profile;
    bad.emitted_by_type[0]++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
    bad = profile;
    bad.root_equal_rejects++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
    bad = profile;
    bad.small_component_rejects++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
    bad = profile;
    bad.accepted_grouping_records++;
    CHECK(throws([&] { validate_sequence_pending_profile(pending, bad); }));
}

void test_record_contains_exact_sequence_identity_and_buffers()
{
    SequenceRecord record{};
    record.scratch_mode = ScratchMode::Local;
    record.index = 7;
    record.label = "scene";
    record.phase = "cold";
    record.repetition = 0;
    record.width = 10;
    record.height = 20;
    record.file_sha256 = std::string(64, 'c');
    record.manifest_sha256 = std::string(64, 'd');
    record.elapsed_ns = 99;
    record.result = {2, 0x123};
    record.profile.version = APRILTAG_CCL_PROFILE_VERSION;
    record.profile.struct_size = sizeof(record.profile);
    record.profile.validity = APRILTAG_CCL_PROFILE_VALID_ALL;
    record.profile.total_ns = 50;
    record.profile.group_emit_ns = 7;
    record.profile.root_materialize_ns = 8;
    record.profile.pending_growths = 9;
    record.pending_profile = populated_pending_profile(record.profile);
    record.scratch.version = APRILTAG_CCL_SCRATCH_VERSION;
    record.scratch.struct_size = sizeof(record.scratch);
    record.scratch.validity = APRILTAG_CCL_SCRATCH_VALID_ALL;
    std::ostringstream out;
    print_sequence_record(record, 2.0, 25, 3, "rle", "build-id", out);
    const std::string line = out.str();
    CHECK(line.rfind("SEQUENCE ", 0) == 0);
    CHECK(occurrences(line, " scratch_mode=local") == 1);
    for (const char* field : {" grouping_mode=", " grouping_version=", " grouping_total_ns=",
                              " grouping_"})
        CHECK(line.find(field) == std::string::npos);
    CHECK(line.find(" index=7 label=scene phase=cold repetition=0") != std::string::npos);
    CHECK(line.find(" latency_ns=99") != std::string::npos);
    CHECK(line.find(" input_sha256=" + std::string(64, 'c')) != std::string::npos);
    CHECK(line.find(" elapsed_ns=") == std::string::npos);
    CHECK(line.find(" file_sha256=") == std::string::npos);
    CHECK(line.find(" manifest_sha256=" + std::string(64, 'd')) != std::string::npos);
    CHECK(line.find(" profile_struct_size=" + std::to_string(sizeof(record.profile)) +
                    " scratch_struct_size=" + std::to_string(sizeof(record.scratch))) !=
          std::string::npos);
    CHECK(line.find(" ccl_total_ns=50 ccl_group_emit_ns=7 ccl_root_materialize_ns=8") != std::string::npos);
    CHECK(occurrences(line, " ccl_pending_growths=9") == 1);
    for (const char* field : {
             "pending_profile_version", "pending_profile_struct_size",
             "pending_profile_validity", "pending_sample_stride",
             "pending_sampled_records", "pending_sampled_units",
             "pending_sampled_accepted_records", "pending_timer_intervals",
             "pending_timer_overhead_ns", "pending_construct_sample_ns",
             "pending_resolve_sample_ns", "pending_lookup_sample_ns",
             "pending_emit_sample_ns"})
        CHECK(occurrences(line, std::string(" ") + field + "=") == 1);
    for (int type = 0; type < APRILTAG_CCL_PENDING_BOUNDARY_TYPES; ++type) {
        for (const char* field : {"pending_records_type_", "pending_units_type_",
                                  "pending_accepted_records_type_",
                                  "pending_accepted_units_type_",
                                  "pending_root_equal_type_",
                                  "pending_small_component_type_"})
            CHECK(occurrences(line, std::string(" ") + field + std::to_string(type) + "=") == 1);
        for (int bin = 0; bin < APRILTAG_CCL_PENDING_RANGE_BINS; ++bin)
            CHECK(occurrences(line, " pending_range_type_" + std::to_string(type) +
                                    "_bin_" + std::to_string(bin) + "=") == 1);
    }
    for (const char* buffer : {"pending", "diagonal_left", "diagonal_right"})
        for (const char* field : {"len", "capacity", "high_water", "growths_call",
                                  "growths_total", "capacity_bytes", "high_water_bytes",
                                  "element_size"})
            CHECK(line.find(std::string(" ") + buffer + "_" + field + "=") != std::string::npos);
    CHECK(line.back() == '\n' && line.find('\n') == line.size() - 1);
}

apriltag_ccl_profile_t valid_profile()
{
    apriltag_ccl_profile_t profile{};
    profile.version = APRILTAG_CCL_PROFILE_VERSION;
    profile.struct_size = sizeof(profile);
    profile.validity = APRILTAG_CCL_PROFILE_VALID_TIMINGS;
    profile.total_ns = 10;
    profile.unattributed_ns = 10;
    return profile;
}

apriltag_ccl_scratch_v1_t scratch_with(std::uint64_t capacity,
                                       std::uint64_t high_water,
                                       std::uint64_t growths_call,
                                       std::uint64_t growths_total)
{
    apriltag_ccl_scratch_v1_t scratch{};
    scratch.version = APRILTAG_CCL_SCRATCH_VERSION;
    scratch.struct_size = sizeof(scratch);
    scratch.validity = APRILTAG_CCL_SCRATCH_VALID_ALL;
    const apriltag_buffer_telemetry_t buffer = {
        1, capacity, high_water, growths_call, growths_total,
        capacity * 8, high_water * 8, 8};
    scratch.pending = buffer;
    scratch.diagonal_left = buffer;
    scratch.diagonal_right = buffer;
    return scratch;
}

void set_valid_pending_callback(SequenceCallbacks& callbacks)
{
    callbacks.get_pending_profile = [] { return valid_pending_profile(); };
}

void test_scratch_sequence_allows_cold_growth_and_requires_stable_warm()
{
    SequenceScratchValidator validator;
    validator.accept_cold(scratch_with(8, 6, 1, 1));
    validator.accept_warm(scratch_with(8, 6, 0, 1));
    validator.accept_warm(scratch_with(8, 6, 0, 1));
    validator.accept_cold(scratch_with(16, 10, 1, 2));
    validator.accept_warm(scratch_with(16, 10, 0, 2));
}

void test_scratch_sequence_rejects_warm_growth_or_change()
{
    SequenceScratchValidator validator;
    validator.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { validator.accept_warm(scratch_with(8, 6, 1, 2)); }));
    SequenceScratchValidator capacity;
    capacity.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { capacity.accept_warm(scratch_with(16, 6, 0, 1)); }));
    SequenceScratchValidator high_water;
    high_water.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { high_water.accept_warm(scratch_with(8, 7, 0, 1)); }));
    SequenceScratchValidator cumulative;
    cumulative.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { cumulative.accept_warm(scratch_with(8, 6, 0, 2)); }));
    SequenceScratchValidator length;
    auto cold = scratch_with(8, 6, 1, 1);
    length.accept_cold(cold);
    auto changed = scratch_with(8, 6, 0, 1);
    changed.pending.len = 2;
    CHECK(throws([&] { length.accept_warm(changed); }));
    SequenceScratchValidator element_size;
    element_size.accept_cold(cold);
    changed = scratch_with(8, 6, 0, 1);
    changed.pending.element_size = 4;
    changed.pending.capacity_bytes = 32;
    changed.pending.high_water_bytes = 24;
    CHECK(throws([&] { element_size.accept_warm(changed); }));
}

void test_scratch_sequence_rejects_cold_regression_across_rows()
{
    SequenceScratchValidator validator;
    validator.accept_cold(scratch_with(16, 10, 1, 2));
    validator.accept_warm(scratch_with(16, 10, 0, 2));
    CHECK(throws([&] { validator.accept_cold(scratch_with(8, 10, 0, 2)); }));
    SequenceScratchValidator high_water;
    high_water.accept_cold(scratch_with(16, 10, 1, 2));
    CHECK(throws([&] { high_water.accept_cold(scratch_with(16, 9, 0, 2)); }));
    SequenceScratchValidator cumulative;
    cumulative.accept_cold(scratch_with(16, 10, 1, 2));
    CHECK(throws([&] { cumulative.accept_cold(scratch_with(16, 10, 0, 1)); }));
    SequenceScratchValidator element_size;
    element_size.accept_cold(scratch_with(16, 10, 1, 2));
    auto changed = scratch_with(16, 10, 0, 2);
    changed.pending.element_size = 4;
    changed.pending.capacity_bytes = 64;
    changed.pending.high_water_bytes = 40;
    CHECK(throws([&] { element_size.accept_cold(changed); }));
}

void test_local_scratch_allows_per_call_growth_and_cross_row_regression()
{
    SequenceScratchValidator validator(ScratchMode::Local);
    validator.accept_cold(scratch_with(16, 10, 2, 2));
    validator.accept_warm(scratch_with(16, 10, 2, 2));
    validator.accept_cold(scratch_with(8, 6, 1, 1));
    validator.accept_warm(scratch_with(8, 6, 1, 1));
}

void test_local_scratch_rejects_nonlocal_totals_and_same_row_instability()
{
    SequenceScratchValidator totals(ScratchMode::Local);
    CHECK(throws([&] { totals.accept_cold(scratch_with(8, 6, 1, 2)); }));
    SequenceScratchValidator capacity(ScratchMode::Local);
    capacity.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { capacity.accept_warm(scratch_with(16, 6, 1, 1)); }));
    SequenceScratchValidator high_water(ScratchMode::Local);
    high_water.accept_cold(scratch_with(8, 6, 1, 1));
    CHECK(throws([&] { high_water.accept_warm(scratch_with(8, 7, 1, 1)); }));
    SequenceScratchValidator length(ScratchMode::Local);
    length.accept_cold(scratch_with(8, 6, 1, 1));
    auto changed = scratch_with(8, 6, 1, 1);
    changed.pending.len = 2;
    CHECK(throws([&] { length.accept_warm(changed); }));
}

void test_run_sequence_orchestrates_every_call_and_final_verification()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')}, {"/first", "/first", 1, 1, std::string(64, 'a')}},
        {{"second", "/second", 1, 1, std::string(64, 'b')}, {"/second", "/second", 1, 2, std::string(64, 'b')}},
    };
    std::vector<std::string> order;
    const LoadPreparedSequenceImage loader = [&](const SequenceInput& input) {
        order.push_back("load:" + input.row.label);
        return PreparedImage{1, 1, 1, {input.row.label == "first" ? std::uint8_t{1} : std::uint8_t{2}}};
    };
    int detects = 0;
    int profiles = 0;
    int scratches = 0;
    int verified = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [&](const PreparedImage& image) {
        ++detects;
        order.push_back(image.pixels[0] == 1 ? "detect:first" : "detect:second");
        return DetectionResult{1, 7};
    };
    callbacks.get_profile = [&] { ++profiles; return valid_profile(); };
    callbacks.get_scratch = [&] {
        ++scratches;
        return scratch_with(scratches <= 3 ? 8 : 16, scratches <= 3 ? 6 : 10,
                            scratches == 1 || scratches == 4 ? 1 : 0,
                            scratches <= 3 ? 1 : 2);
    };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [&] { ++verified; order.push_back("verify"); };
    std::ostringstream out;
    run_sequence(inputs, loader, 2, std::string(64, 'm'), 2.0, 25, 3, "rle",
                  "build", ScratchMode::Reusable, callbacks, out);
    CHECK(detects == 6 && profiles == 6 && scratches == 6);
    CHECK(verified == 1);
    CHECK(order == std::vector<std::string>({
        "load:first", "detect:first", "detect:first", "detect:first",
        "load:second", "detect:second", "detect:second", "detect:second", "verify"}));
    const std::string text = out.str();
    CHECK(occurrences(text, "SEQUENCE ") == 6);
    CHECK(occurrences(text, " phase=cold repetition=0") == 2);
    CHECK(occurrences(text, " phase=warm repetition=1") == 2);
    CHECK(occurrences(text, " phase=warm repetition=2") == 2);
    CHECK(occurrences(text, " manifest_sha256=" + std::string(64, 'm')) == 6);
    CHECK(occurrences(text, " scratch_mode=reusable") == 6);
    for (int index = 0; index < 6; ++index)
        CHECK(occurrences(text, " index=" + std::to_string(index) + " ") == 1);
}

void test_run_sequence_final_verification_failure_emits_nothing()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')}, {"/first", "/first", 1, 1, std::string(64, 'a')}},
        {{"second", "/second", 1, 1, std::string(64, 'b')}, {"/second", "/second", 1, 2, std::string(64, 'b')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [&](const PreparedImage&) { ++calls; return DetectionResult{1, 7}; };
    callbacks.get_profile = [] { return valid_profile(); };
    callbacks.get_scratch = [&] {
        return scratch_with(calls == 1 ? 8 : 16, calls == 1 ? 6 : 10, 1, calls);
    };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] { throw InputError("changed"); };
    std::ostringstream out;
    CHECK(throws([&] { run_sequence(inputs, loader, 0, std::string(64, 'm'), 2.0, 25,
                                     3, "rle", "build", ScratchMode::Reusable, callbacks, out); }));
    CHECK(calls == 2 && out.str().empty());
}

void test_run_sequence_repeated_identity_must_preserve_output()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/same", 1, 1, std::string(64, 'a')}, {"/same", "/same", 1, 1, std::string(64, 'a')}},
        {{"again", "/same", 1, 1, std::string(64, 'a')}, {"/same", "/same", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [&](const PreparedImage&) { return DetectionResult{1, ++calls == 1 ? 7U : 8U}; };
    callbacks.get_profile = [] { return valid_profile(); };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, calls == 1, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    CHECK(throws([&] { run_sequence(inputs, loader, 0, std::string(64, 'm'), 2.0, 25,
                                     3, "rle", "build", ScratchMode::Reusable, callbacks, out); }));
    CHECK(out.str().empty());
}

void test_run_sequence_rejects_unstable_output_before_final_verify()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')}, {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; };
    int calls = 0;
    bool verified = false;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [&](const PreparedImage&) { return DetectionResult{1, ++calls == 1 ? 7U : 8U}; };
    callbacks.get_profile = [] { return valid_profile(); };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, calls == 1, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [&] { verified = true; };
    std::ostringstream out;
    CHECK(throws([&] { run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25,
                                     3, "rle", "build", ScratchMode::Reusable, callbacks, out); }));
    CHECK(calls == 2 && !verified);
    CHECK(out.str().empty());
}

void test_run_sequence_rejects_missing_timings_without_output()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')}, {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [](const PreparedImage&) { return DetectionResult{1, 7}; };
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        if (++calls == 2) profile.validity = APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION;
        return profile;
    };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, calls == 1, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    CHECK(throws([&] { run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25,
                                     3, "rle", "build", ScratchMode::Reusable, callbacks, out); }));
    CHECK(out.str().empty());
}

void test_run_sequence_validates_profile_counters_across_repetitions()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')}, {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [](const PreparedImage&) { return DetectionResult{1, 7}; };
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION;
        profile.runs = ++calls;
        return profile;
    };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, calls == 1, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    CHECK(throws([&] { run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25,
                                     3, "rle", "build", ScratchMode::Reusable, callbacks, out); }));
    CHECK(out.str().empty());
}

void test_run_sequence_allows_pending_growth_to_drop_after_cold_call()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')},
         {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) {
        return PreparedImage{1, 1, 1, {1}};
    };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [](const PreparedImage&) { return DetectionResult{1, 7}; };
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION |
                            APRILTAG_CCL_PROFILE_VALID_GROWTH;
        profile.runs = 4;
        profile.pending_growths = calls++ == 0 ? 1 : 0;
        profile.cluster_map_growths = 3;
        profile.cluster_vector_growths = 4;
        return profile;
    };
    callbacks.get_scratch = [&] {
        return scratch_with(8, 6, calls == 1 ? 1 : 0, 1);
    };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25, 3, "rle",
                 "build", ScratchMode::Reusable, callbacks, out);
    CHECK(occurrences(out.str(), "SEQUENCE ") == 2);
    CHECK(occurrences(out.str(), " phase=cold repetition=0") == 1);
    CHECK(occurrences(out.str(), " phase=warm repetition=1") == 1);
}

void test_run_sequence_rejects_contradictory_cold_growth_snapshots()
{
    const std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')},
         {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [](const PreparedImage&) { return DetectionResult{1, 7}; };
    callbacks.get_profile = [] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_GROWTH;
        profile.pending_growths = 2;
        return profile;
    };
    callbacks.get_scratch = [] { return scratch_with(8, 6, 1, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    CHECK(throws_with([&] {
        run_sequence(inputs, [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; },
                     0, std::string(64, 'm'), 2.0, 25, 3, "rle", "build",
                     ScratchMode::Reusable, callbacks, out);
    }, "pending growth telemetry disagrees"));
    CHECK(out.str().empty());
}

void test_run_sequence_rejects_contradictory_warm_growth_snapshots()
{
    const std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')},
         {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [](const PreparedImage&) { return DetectionResult{1, 7}; };
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_GROWTH;
        profile.pending_growths = 1;
        ++calls;
        return profile;
    };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, calls == 1 ? 1 : 0, 1); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    CHECK(throws_with([&] {
        run_sequence(inputs, [](const SequenceInput&) { return PreparedImage{1, 1, 1, {1}}; },
                     1, std::string(64, 'm'), 2.0, 25, 3, "rle", "build",
                     ScratchMode::Reusable, callbacks, out);
    }, "pending growth telemetry disagrees"));
    CHECK(out.str().empty());
}

void test_run_sequence_local_accepts_repeated_growth_and_rejects_profile_change()
{
    std::vector<SequenceInput> inputs = {
        {{"first", "/first", 1, 1, std::string(64, 'a')},
         {"/first", "/first", 1, 1, std::string(64, 'a')}},
    };
    const LoadPreparedSequenceImage loader = [](const SequenceInput&) {
        return PreparedImage{1, 1, 1, {1}};
    };
    int calls = 0;
    SequenceCallbacks callbacks;
    set_valid_pending_callback(callbacks);
    callbacks.detect = [&](const PreparedImage&) { ++calls; return DetectionResult{1, 7}; };
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_GROWTH;
        profile.pending_growths = 2;
        return profile;
    };
    callbacks.get_scratch = [&] { return scratch_with(8, 6, 2, 2); };
    callbacks.now_ns = [time = std::uint64_t{0}]() mutable { return ++time; };
    callbacks.verify_unchanged = [] {};
    std::ostringstream out;
    run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25, 3, "rle",
                 "build", ScratchMode::Local, callbacks, out);
    CHECK(calls == 2 && occurrences(out.str(), " scratch_mode=local") == 2);

    calls = 0;
    callbacks.get_profile = [&] {
        auto profile = valid_profile();
        profile.validity |= APRILTAG_CCL_PROFILE_VALID_GROWTH;
        profile.pending_growths = calls == 1 ? 2 : 1;
        return profile;
    };
    out.str("");
    CHECK(throws([&] {
        run_sequence(inputs, loader, 1, std::string(64, 'm'), 2.0, 25, 3, "rle",
                     "build", ScratchMode::Local, callbacks, out);
    }));
    CHECK(out.str().empty());
}

void test_sequence_profile_validation_allows_only_pending_growth_variation()
{
    auto cold = valid_profile();
    cold.validity |= APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION |
                     APRILTAG_CCL_PROFILE_VALID_GROWTH;
    cold.runs = 4;
    cold.pending_growths = 2;
    cold.cluster_map_growths = 3;
    cold.cluster_vector_growths = 4;
    auto warm = cold;
    warm.pending_growths = 0;
    validate_sequence_profile_sequence({cold, warm});

    auto changed = warm;
    changed.runs++;
    CHECK(throws([&] { validate_sequence_profile_sequence({cold, changed}); }));
    changed = warm;
    changed.cluster_map_growths++;
    CHECK(throws([&] { validate_sequence_profile_sequence({cold, changed}); }));
    changed = warm;
    changed.cluster_vector_growths++;
    CHECK(throws([&] { validate_sequence_profile_sequence({cold, changed}); }));
}

void test_local_sequence_profile_requires_stable_pending_growths()
{
    auto first = valid_profile();
    first.validity |= APRILTAG_CCL_PROFILE_VALID_GROWTH;
    first.pending_growths = 2;
    auto repeated = first;
    validate_sequence_profile_sequence({first, repeated}, ScratchMode::Local);
    repeated.pending_growths = 1;
    CHECK(throws([&] {
        validate_sequence_profile_sequence({first, repeated}, ScratchMode::Local);
    }));
}

void test_sequence_main_help_and_missing_manifest()
{
    std::ostringstream out;
    std::ostringstream err;
    const char* help[] = {"sequence", "--help"};
    CHECK(sequence_main(2, help, out, err) == 0);
    CHECK(out.str().find("--manifest PATH") != std::string::npos);
    CHECK(out.str().find("grouping") == std::string::npos);
    CHECK(out.str().find("legacy") == std::string::npos);
    CHECK(out.str().find("exact-count") == std::string::npos);
    CHECK(err.str().empty());

    out.str("");
    err.str("");
    const char* grouping[] = {"sequence", "--grouping-mode", "legacy"};
    CHECK(sequence_main(3, grouping, out, err) == 2);
    CHECK(err.str().find("argument error:") != std::string::npos);

    out.str("");
    err.str("");
    const char* missing[] = {"sequence", "--manifest", "/definitely/missing.tsv"};
    CHECK(sequence_main(3, missing, out, err) == 1);
    CHECK(err.str().find("cannot securely open sequence input") != std::string::npos);
}

struct MockAbiState {
    int news = 0, scratch_sets = 0, sets = 0, detects = 0,
        profiles = 0, pending_profiles = 0, scratches = 0, frees = 0;
    int scratch_mode = -1;
    bool fail_scratch_set = false, fail_set = false, fail_profile = false,
         fail_pending_profile = false;
    std::vector<std::string> order;
} mock_abi;

void reset_mock_abi() { mock_abi = {}; }

std::filesystem::path write_cli_manifest(const std::filesystem::path& root)
{
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto image = root / "image.bin";
    { std::ofstream out(image, std::ios::binary); out << "abc"; }
    const auto manifest = root / "manifest.tsv";
    { std::ofstream out(manifest, std::ios::binary);
      out << "scene\t" << image.string() << "\t1\t1\t"
          << "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"; }
    return manifest;
}

std::filesystem::path write_two_row_cli_manifest(const std::filesystem::path& root)
{
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    { std::ofstream out(first, std::ios::binary); out << "abc"; }
    { std::ofstream out(second, std::ios::binary); out << "abc"; }
    const auto manifest = root / "manifest.tsv";
    { std::ofstream out(manifest, std::ios::binary);
      out << "first\t" << first.string() << "\t1\t1\t"
          << "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
          << "second\t" << second.string() << "\t1\t1\t"
          << "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"; }
    return manifest;
}

const LoadSequenceImage kTestLoader = [](const std::vector<std::uint8_t>& bytes,
                                         ImageSize size) {
    CHECK(bytes == std::vector<std::uint8_t>({'a', 'b', 'c'}));
    CHECK(size.native);
    return PreparedImage{1, 1, 1, {42}};
};

void test_sequence_cli_success_uses_one_handle_and_all_getters()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli";
    const auto manifest = write_cli_manifest(root);
    reset_mock_abi();
    const std::string path = manifest.string();
    const char* args[] = {"sequence", "--manifest", path.c_str(),
                          "--rvv-stages", "all", "--warm-repetitions", "1"};
    std::ostringstream out, err;
    CHECK(sequence_cli(7, args, kTestLoader, out, err) == 0);
    CHECK(mock_abi.news == 1 && mock_abi.scratch_sets == 1 &&
          mock_abi.scratch_mode == APRILTAG_CCL_SCRATCH_MODE_REUSABLE);
    CHECK(mock_abi.sets == 1 && mock_abi.detects == 2);
    CHECK(mock_abi.profiles == 2 && mock_abi.pending_profiles == 2 &&
          mock_abi.scratches == 2 && mock_abi.frees == 1);
    CHECK(occurrences(out.str(), "SEQUENCE ") == 2 && err.str().empty());
    CHECK(occurrences(out.str(), " scratch_mode=reusable") == 2);
    CHECK(mock_abi.order == std::vector<std::string>({
        "new",
        "scratch:" + std::to_string(APRILTAG_CCL_SCRATCH_MODE_REUSABLE),
        "kernel",
        "detect", "profile", "pending", "scratch-get",
        "detect", "profile", "pending", "scratch-get",
        "free"}));
    std::filesystem::remove_all(root);
}

void test_sequence_cli_local_mode_sets_profile_abi_before_detect()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli-local";
    const auto manifest = write_cli_manifest(root);
    reset_mock_abi();
    const std::string path = manifest.string();
    const char* args[] = {"sequence", "--manifest", path.c_str(),
                          "--scratch-mode", "local", "--warm-reps", "0"};
    std::ostringstream out, err;
    CHECK(sequence_cli(7, args, kTestLoader, out, err) == 0);
    CHECK(mock_abi.scratch_sets == 1 &&
          mock_abi.scratch_mode == APRILTAG_CCL_SCRATCH_MODE_LOCAL);
    CHECK(mock_abi.order.size() >= 3 && mock_abi.order[0] == "new" &&
          mock_abi.order[1] == "scratch:" +
                                   std::to_string(APRILTAG_CCL_SCRATCH_MODE_LOCAL) &&
          mock_abi.order[2] == "detect");
    CHECK(occurrences(out.str(), " scratch_mode=local") == 1);
    std::filesystem::remove_all(root);
}

void test_sequence_cli_rejects_invalid_scratch_mode()
{
    std::ostringstream out, err;
    const char* missing[] = {"sequence", "--scratch-mode"};
    CHECK(sequence_main(2, missing, out, err) == 2);
    out.str(""); err.str("");
    const char* invalid[] = {"sequence", "--scratch-mode", "shared"};
    CHECK(sequence_main(3, invalid, out, err) == 2);
    CHECK(err.str().find("local or reusable") != std::string::npos);
}

void test_sequence_cli_decodes_rows_only_after_prior_row_calls()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli-lazy";
    const auto manifest = write_two_row_cli_manifest(root);
    reset_mock_abi();
    int loads = 0;
    const LoadSequenceImage ordered_loader = [&](const std::vector<std::uint8_t>& bytes,
                                                  ImageSize size) {
        CHECK(bytes == std::vector<std::uint8_t>({'a', 'b', 'c'}));
        CHECK(size.native);
        CHECK(mock_abi.detects == loads * 2);
        ++loads;
        return PreparedImage{1, 1, 1, {static_cast<std::uint8_t>(loads)}};
    };
    const std::string path = manifest.string();
    const char* args[] = {"sequence", "--manifest", path.c_str(), "--warm-repetitions", "1"};
    std::ostringstream out, err;
    CHECK(sequence_cli(5, args, ordered_loader, out, err) == 0);
    CHECK(loads == 2 && mock_abi.detects == 4);
    CHECK(occurrences(out.str(), "SEQUENCE ") == 4 && err.str().empty());
    std::filesystem::remove_all(root);
}

void test_sequence_cli_securely_rereads_each_row_before_decode()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli-reread";
    const auto manifest = write_two_row_cli_manifest(root);
    const auto second = root / "second.bin";
    reset_mock_abi();
    int loads = 0;
    const LoadSequenceImage mutating_loader = [&](const std::vector<std::uint8_t>&,
                                                   ImageSize) {
        ++loads;
        if (loads == 1) {
            std::ofstream out(second, std::ios::binary);
            out << "abd";
        }
        return PreparedImage{1, 1, 1, {42}};
    };
    const std::string path = manifest.string();
    const char* args[] = {"sequence", "--manifest", path.c_str(), "--warm-reps", "0"};
    std::ostringstream out, err;
    CHECK(sequence_cli(5, args, mutating_loader, out, err) == 1);
    CHECK(loads == 1 && mock_abi.detects == 1);
    CHECK(out.str().empty());
    CHECK(err.str().find("file changed during sequence run") != std::string::npos);
    std::filesystem::remove_all(root);
}

void test_sequence_cli_rejects_native_dimension_mismatch_before_detector()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli-size";
    const auto manifest = write_cli_manifest(root);
    reset_mock_abi();
    const std::string path = manifest.string();
    const char* args[] = {"sequence", "--manifest", path.c_str()};
    const LoadSequenceImage wrong_size = [](const std::vector<std::uint8_t>&, ImageSize size) {
        CHECK(size.native);
        return PreparedImage{2, 1, 2, {42, 42}};
    };
    std::ostringstream out, err;
    CHECK(sequence_cli(3, args, wrong_size, out, err) == 1);
    CHECK(mock_abi.news == 1 && mock_abi.detects == 0 && mock_abi.frees == 1);
    CHECK(out.str().empty());
    CHECK(err.str().find("native dimensions") != std::string::npos);
    std::filesystem::remove_all(root);
}

void test_sequence_cli_failures_release_handle()
{
    const auto root = std::filesystem::temp_directory_path() / "apriltag-sequence-cli-fail";
    const auto manifest = write_cli_manifest(root);
    const std::string path = manifest.string();
    const char* set_args[] = {"sequence", "--manifest", path.c_str(),
                              "--rvv-stages", "all"};
    reset_mock_abi();
    mock_abi.fail_scratch_set = true;
    std::ostringstream out, err;
    CHECK(sequence_cli(5, set_args, kTestLoader, out, err) == 1);
    CHECK(mock_abi.news == 1 && mock_abi.scratch_sets == 1 && mock_abi.sets == 0 &&
          mock_abi.detects == 0 && mock_abi.frees == 1);
    reset_mock_abi();
    mock_abi.fail_set = true;
    out.str(""); err.str("");
    CHECK(sequence_cli(5, set_args, kTestLoader, out, err) == 1);
    CHECK(mock_abi.news == 1 && mock_abi.frees == 1 && mock_abi.detects == 0);
    reset_mock_abi();
    mock_abi.fail_profile = true;
    const char* getter_args[] = {"sequence", "--manifest", path.c_str(),
                                 "--warm-reps", "0"};
    out.str(""); err.str("");
    CHECK(sequence_cli(5, getter_args, kTestLoader, out, err) == 1);
    CHECK(mock_abi.news == 1 && mock_abi.detects == 1 && mock_abi.frees == 1);
    CHECK(out.str().empty());
    reset_mock_abi();
    mock_abi.fail_pending_profile = true;
    out.str(""); err.str("");
    CHECK(sequence_cli(5, getter_args, kTestLoader, out, err) == 1);
    CHECK(mock_abi.news == 1 && mock_abi.detects == 1 &&
          mock_abi.pending_profiles == 1 && mock_abi.frees == 1);
    CHECK(out.str().empty());
    std::filesystem::remove_all(root);
}
}

extern "C" {
void* apriltag_new(std::uint32_t) { ++mock_abi.news; mock_abi.order.push_back("new"); return &mock_abi; }
void apriltag_free(void*) { ++mock_abi.frees; mock_abi.order.push_back("free"); }
int apriltag_detect(void*, const std::uint8_t*, std::size_t, std::size_t,
                    std::size_t, int, int, apriltag_det_t* out, int) {
    ++mock_abi.detects;
    mock_abi.order.push_back("detect");
    out[0] = {};
    out[0].id = 1;
    return 1;
}
int apriltag_set_kernel_mask_v1(void*, std::uint64_t) {
    ++mock_abi.sets;
    mock_abi.order.push_back("kernel");
    return mock_abi.fail_set ? -1 : 0;
}
int apriltag_set_ccl_scratch_mode_v1(apriltag_t*, uint32_t mode) {
    ++mock_abi.scratch_sets;
    mock_abi.scratch_mode = mode;
    mock_abi.order.push_back("scratch:" + std::to_string(mode));
    return mock_abi.fail_scratch_set ? -1 : 0;
}
int apriltag_get_ccl_profile_v1(apriltag_t*, apriltag_ccl_profile_t* out, std::size_t) {
    ++mock_abi.profiles;
    mock_abi.order.push_back("profile");
    if (mock_abi.fail_profile) return -1;
    *out = valid_profile();
    return 1;
}
int apriltag_get_ccl_pending_profile_v1(apriltag_t*,
                                        apriltag_ccl_pending_profile_v1_t* out,
                                        std::size_t) {
    ++mock_abi.pending_profiles;
    mock_abi.order.push_back("pending");
    if (mock_abi.fail_pending_profile) return -1;
    *out = valid_pending_profile();
    return 1;
}
int apriltag_get_ccl_scratch_v1(apriltag_t*, apriltag_ccl_scratch_v1_t* out, std::size_t) {
    ++mock_abi.scratches;
    mock_abi.order.push_back("scratch-get");
    *out = scratch_with(8, 6, 0, 0);
    return 1;
}
}

int main()
{
    test_sha256_vectors();
    test_manifest_accepts_spaces_and_exact_fields();
    test_manifest_rejects_unsafe_syntax();
    test_manifest_rejects_duplicates();
    test_file_validation_allows_identical_repeated_input();
    test_snapshot_hash_and_rehash();
    test_verified_read_is_transient_and_rejects_post_snapshot_mutation();
    test_manifest_snapshot_rejects_symlink_and_detects_replacement();
    test_scratch_invariants();
    test_pending_profile_validation_rejects_schema_and_validity_errors();
    test_pending_profile_validation_rejects_conservation_errors();
    test_pending_profile_validation_rejects_reserved_fields();
    test_pending_profile_validation_rejects_count_overflow();
    test_pending_profile_validation_handles_ceil_and_timer_overflow();
    test_pending_profile_validation_rejects_ccl_profile_disagreement();
    test_record_contains_exact_sequence_identity_and_buffers();
    test_scratch_sequence_allows_cold_growth_and_requires_stable_warm();
    test_scratch_sequence_rejects_warm_growth_or_change();
    test_scratch_sequence_rejects_cold_regression_across_rows();
    test_local_scratch_allows_per_call_growth_and_cross_row_regression();
    test_local_scratch_rejects_nonlocal_totals_and_same_row_instability();
    test_run_sequence_orchestrates_every_call_and_final_verification();
    test_run_sequence_final_verification_failure_emits_nothing();
    test_run_sequence_repeated_identity_must_preserve_output();
    test_run_sequence_rejects_unstable_output_before_final_verify();
    test_run_sequence_rejects_missing_timings_without_output();
    test_run_sequence_validates_profile_counters_across_repetitions();
    test_run_sequence_allows_pending_growth_to_drop_after_cold_call();
    test_run_sequence_rejects_contradictory_cold_growth_snapshots();
    test_run_sequence_rejects_contradictory_warm_growth_snapshots();
    test_run_sequence_local_accepts_repeated_growth_and_rejects_profile_change();
    test_sequence_profile_validation_allows_only_pending_growth_variation();
    test_local_sequence_profile_requires_stable_pending_growths();
    test_sequence_main_help_and_missing_manifest();
    test_sequence_cli_success_uses_one_handle_and_all_getters();
    test_sequence_cli_local_mode_sets_profile_abi_before_detect();
    test_sequence_cli_rejects_invalid_scratch_mode();
    test_sequence_cli_decodes_rows_only_after_prior_row_calls();
    test_sequence_cli_securely_rereads_each_row_before_decode();
    test_sequence_cli_rejects_native_dimension_mismatch_before_detector();
    test_sequence_cli_failures_release_handle();
    if (failures) std::cerr << failures << " sequence test(s) failed\n";
    return failures ? 1 : 0;
}
