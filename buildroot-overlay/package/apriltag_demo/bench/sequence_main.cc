#include "sequence.h"

#include "apriltag.h"

#include <charconv>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <time.h>

#ifndef APRILTAG_BENCH_BUILD_ID
#define APRILTAG_BENCH_BUILD_ID "unknown"
#endif

namespace apriltag_bench {
namespace {

struct SequenceConfig {
    std::string manifest;
    BenchmarkConfig detector;
    int warm_reps = 1;
    ScratchMode scratch_mode = ScratchMode::Reusable;
    bool help = false;
};

const char* require_value(int argc, const char* const argv[], int& index)
{
    if (++index >= argc) throw ArgumentError(std::string(argv[index - 1]) + " requires a value");
    return argv[index];
}

template <typename T>
T positive_integer(const char* text, const char* option, T minimum)
{
    T value{};
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc() || result.ptr != end || value < minimum)
        throw ArgumentError(std::string(option) + " has an invalid value: " + text);
    return value;
}

SequenceConfig parse_sequence_args(int argc, const char* const argv[])
{
    SequenceConfig config;
    config.detector.backends = {BackendKind::RustRvv};
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--manifest") config.manifest = require_value(argc, argv, i);
        else if (option == "--factor") {
            const std::string value(require_value(argc, argv, i));
            if (value == "1") { config.detector.factor = 0; config.detector.factor_value = 1; }
            else if (value == "1.5") { config.detector.factor = 1; config.detector.factor_value = 1.5; }
            else if (value == "2") { config.detector.factor = 2; config.detector.factor_value = 2; }
            else throw ArgumentError("--factor must be 1, 1.5, or 2");
        } else if (option == "--min-blob") {
            config.detector.min_blob = positive_integer<std::uint32_t>(
                require_value(argc, argv, i), "--min-blob", 1);
            if (config.detector.min_blob > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                throw ArgumentError("--min-blob exceeds INT_MAX");
        } else if (option == "--rvv-stages") {
            parse_rvv_stages(require_value(argc, argv, i), config.detector);
        } else if (option == "--warm-repetitions" || option == "--warm-reps") {
            config.warm_reps = positive_integer<int>(
                require_value(argc, argv, i), option.c_str(), 0);
        } else if (option == "--scratch-mode") {
            const std::string value(require_value(argc, argv, i));
            if (value == "local") config.scratch_mode = ScratchMode::Local;
            else if (value == "reusable") config.scratch_mode = ScratchMode::Reusable;
            else throw ArgumentError("--scratch-mode must be local or reusable");
        } else if (option == "--help" || option == "-h") {
            config.help = true;
        } else throw ArgumentError("unknown argument: " + option);
    }
    if (!config.help && config.manifest.empty()) throw ArgumentError("--manifest is required");
    return config;
}

std::uint64_t monotonic_raw_ns()
{
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0)
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

DetectionResult detect(void* handle, const BenchmarkConfig& config,
                       const PreparedImage& image, std::vector<apriltag_det_t>& output)
{
    const int count = validate_detection_count(apriltag_detect(
        handle, image.pixels.data(), image.width, image.height, image.stride,
        config.factor, 1, output.data(), static_cast<int>(output.size())));
    DetectionChecksum checksum(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Detection item;
        item.id = output[i].id;
        item.margin = output[i].margin;
        for (int j = 0; j < 2; ++j) item.center[j] = output[i].center[j];
        for (int j = 0; j < 8; ++j) item.corners[j] = output[i].corners[j];
        checksum.add(item);
    }
    return {count, checksum.value()};
}

void print_sequence_usage(std::ostream& out, const char* program)
{
    out << "Usage: " << program << " --manifest PATH [options]\n"
        << "  --manifest PATH       label<TAB>path<TAB>width<TAB>height<TAB>sha256\n"
        << "  --factor FACTOR       1, 1.5, or 2 (default 2)\n"
        << "  --min-blob N          minimum cluster pixels (default 25)\n"
        << "  --rvv-stages STAGES   all, none, or canonical stage list\n"
        << "  --warm-repetitions N  calls after each row's cold call (default 1)\n"
        << "  --scratch-mode MODE   local or reusable (default reusable)\n"
        << "  --warm-reps N         deprecated alias for --warm-repetitions\n";
}
}

int sequence_cli(int argc, const char* const argv[], const LoadSequenceImage& load_image,
                 std::ostream& out, std::ostream& err)
{
    SequenceConfig config;
    try {
        config = parse_sequence_args(argc, argv);
        if (config.help) { print_sequence_usage(out, argv[0]); return 0; }
    } catch (const ArgumentError& error) {
        err << "argument error: " << error.what() << '\n';
        return 2;
    }
    try {
        const FileIdentity manifest_snapshot = snapshot_regular_file(config.manifest);
        const std::string manifest_hash = manifest_snapshot.sha256;
        std::vector<SequenceInput> inputs;
        {
            const auto manifest_bytes = read_verified_file(manifest_snapshot);
            std::istringstream manifest(std::string(manifest_bytes.begin(), manifest_bytes.end()));
            inputs = load_sequence_inputs(parse_sequence_manifest(manifest));
        }
        void* raw_handle = apriltag_new(config.detector.min_blob);
        if (!raw_handle) throw std::runtime_error("detector construction failed");
        const std::unique_ptr<void, decltype(&apriltag_free)> handle(
            raw_handle, &apriltag_free);
        if (apriltag_set_ccl_scratch_mode_v1(
                static_cast<apriltag_t*>(handle.get()),
                config.scratch_mode == ScratchMode::Local
                    ? APRILTAG_CCL_SCRATCH_MODE_LOCAL
                    : APRILTAG_CCL_SCRATCH_MODE_REUSABLE) != 0)
            throw std::runtime_error("failed to configure CCL scratch mode");
        if (config.detector.rvv_mask_explicit &&
            apriltag_set_kernel_mask_v1(handle.get(), config.detector.rvv_mask) != 0)
            throw std::runtime_error("failed to configure Rust RVV stage mask");
        std::vector<apriltag_det_t> detections(kMaxDetections);
        const LoadPreparedSequenceImage prepare = [&](const SequenceInput& input) {
            const auto bytes = read_verified_file(input.file);
            auto image = load_image(bytes, ImageSize::native_size());
            if (image.width != input.row.width || image.height != input.row.height)
                throw InputError("decoded native dimensions disagree with sequence manifest: " +
                                 input.row.path);
            return image;
        };
        SequenceCallbacks callbacks;
        callbacks.detect = [&](const PreparedImage& image) {
            return detect(handle.get(), config.detector, image, detections);
        };
        callbacks.get_profile = [&] {
            apriltag_ccl_profile_t profile{};
            if (apriltag_get_ccl_profile_v1(static_cast<apriltag_t*>(handle.get()),
                                            &profile, sizeof(profile)) != 1)
                throw std::runtime_error("CCL profile getter failed");
            return profile;
        };
        callbacks.get_pending_profile = [&] {
            apriltag_ccl_pending_profile_v1_t profile{};
            if (apriltag_get_ccl_pending_profile_v1(
                    static_cast<apriltag_t*>(handle.get()), &profile,
                    sizeof(profile)) != 1)
                throw std::runtime_error("CCL pending profile getter failed");
            return profile;
        };
        callbacks.get_scratch = [&] {
            apriltag_ccl_scratch_v1_t scratch{};
            if (apriltag_get_ccl_scratch_v1(static_cast<apriltag_t*>(handle.get()),
                                            &scratch, sizeof(scratch)) != 1)
                throw std::runtime_error("CCL scratch getter failed");
            return scratch;
        };
        callbacks.now_ns = monotonic_raw_ns;
        callbacks.verify_unchanged = [&] {
            verify_sequence_inputs_unchanged(inputs);
            verify_file_snapshot(manifest_snapshot);
        };
        run_sequence(inputs, prepare, config.warm_reps, manifest_hash,
                     config.detector.factor_value, config.detector.min_blob,
                     config.detector.rvv_mask, config.detector.rvv_stages,
                     APRILTAG_BENCH_BUILD_ID, config.scratch_mode, callbacks, out);
        return 0;
    } catch (const std::exception& error) {
        err << "sequence error: " << error.what() << '\n';
        return 1;
    }
}

int sequence_main(int argc, const char* const argv[], std::ostream& out, std::ostream& err)
{
    return sequence_cli(argc, argv, load_encoded_image, out, err);
}

}  // namespace apriltag_bench

#ifndef APRILTAG_SEQUENCE_NO_MAIN
int main(int argc, const char* const argv[])
{
    return apriltag_bench::sequence_main(argc, argv, std::cout, std::cerr);
}
#endif
