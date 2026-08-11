#include "benchmark.h"

#include <cmath>
#include <climits>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#ifndef APRILTAG_BENCH_NO_OPENCV
#include <opencv2/imgcodecs.hpp>
#endif

using namespace apriltag_bench;

#ifndef APRILTAG_BENCH_BUILD_ID
#define APRILTAG_BENCH_BUILD_ID "unknown"
#endif

namespace {

int failures = 0;

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            ("apriltag-bench-visual-test-" + std::to_string(getpid()) + "-");
        for (std::uintmax_t counter = 0;; ++counter) {
            path_ = base.string() + std::to_string(counter);
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
            if (error) {
                throw std::filesystem::filesystem_error(
                    "cannot create temporary test directory", path_, error);
            }
        }
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "   \
                      << #condition << '\n';                                   \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

BenchmarkConfig parse(std::initializer_list<const char*> args)
{
    std::vector<const char*> argv(args);
    return parse_args(static_cast<int>(argv.size()), argv.data());
}

void expect_parse_error(std::initializer_list<const char*> args)
{
    try {
        (void)parse(args);
        CHECK(false);
    } catch (const ArgumentError&) {
    }
}

void test_parser()
{
    const auto defaults = parse({"bench"});
    CHECK(defaults.input == "fixture.jpg");
    CHECK(defaults.size.native);
    CHECK(defaults.backends.size() == 3);
    CHECK(defaults.factor == 2 && defaults.factor_value == 2.0);
    CHECK(!defaults.rvv_mask_explicit);
    CHECK(defaults.dump_dir.empty());

    CHECK(parse({"bench", "--dump-dir", "output"}).dump_dir == "output");
    CHECK(parse({"bench", "--dump-dir", "first", "--no-dump"})
              .dump_dir.empty());
    CHECK(parse({"bench", "--no-dump", "--dump-dir", "last"}).dump_dir ==
          "last");

    CHECK(parse({"bench", "--input", "x.jpg"}).size.native);
    const auto sized = parse({"bench", "--input", "x.jpg", "--size",
                              "1280x720"});
    CHECK(!sized.size.native && sized.size.width == 1280 &&
          sized.size.height == 720);
    const auto raw = parse({"bench", "--input", "x.y8", "--format", "raw",
                            "--size", "640x480"});
    CHECK(raw.format == InputFormat::Raw && raw.size.width == 640);
    CHECK(parse({"bench", "--backend", "rust-rvv"}).backends ==
          std::vector<BackendKind>{BackendKind::RustRvv});
    CHECK(parse({"bench", "--backend", "all"}).backends.size() == 3);
    const auto masked = parse({"bench", "--rvv-stages",
                               "gray-model,decimate,rle"});
    CHECK(masked.rvv_mask_explicit);
    CHECK(masked.rvv_mask == (APRILTAG_KERNEL_GRAY_MODEL |
                              APRILTAG_KERNEL_DECIMATE |
                              APRILTAG_KERNEL_RLE));
    CHECK(masked.rvv_stages == "decimate,rle,gray-model");
    CHECK(masked.backends == std::vector<BackendKind>(
        {BackendKind::RustRvv, BackendKind::CReference}));
    CHECK(parse({"bench", "--backend", "all", "--rvv-stages", "all"})
              .rvv_mask == APRILTAG_KERNEL_ALL);
    CHECK(parse({"bench", "--rvv-stages", "none", "--backend", "rust-rvv"})
              .rvv_stages == "none");
    CHECK(parse({"bench", "--factor", "1.5"}).factor == 1);
    CHECK(parse({"bench", "--help"}).help);
    CHECK(parse({"bench", "--format", "raw", "--size", "native",
                 "--help"}).help);

    expect_parse_error({"bench", "--factor", "3"});
    expect_parse_error({"bench", "--factor", "1.0"});
    expect_parse_error({"bench", "--factor", "2.0"});
    expect_parse_error({"bench", "--backend", "c-reference"});
    expect_parse_error({"bench", "--rvv-stages", "rle,rle"});
    expect_parse_error({"bench", "--rvv-stages", "unknown"});
    expect_parse_error({"bench", "--rvv-stages", ""});
    expect_parse_error({"bench", "--backend", "rust-scalar",
                        "--rvv-stages", "all"});
    expect_parse_error({"bench", "--rvv-stages", "all", "--backend", "c"});
    expect_parse_error({"bench", "--size", "1280"});
    expect_parse_error({"bench", "--warmup", "-1"});
    expect_parse_error({"bench", "--iterations", "0"});
    expect_parse_error({"bench", "--batches", "1x"});
    expect_parse_error({"bench", "--min-blob", "4294967296"});
    expect_parse_error({"bench", "--min-blob", "2147483648"});
    expect_parse_error({"bench", "--unknown"});
    expect_parse_error({"bench", "--input"});
    expect_parse_error({"bench", "--dump-dir"});
    expect_parse_error({"bench", "extra"});
    expect_parse_error({"bench", "--format", "raw", "--size", "native"});
}

void test_raw_and_geometry()
{
    const std::string path = "/tmp/apriltag_bench_test.y8";
    {
        std::ofstream file(path, std::ios::binary);
        for (int i = 0; i < 16; ++i) file.put(static_cast<char>(i));
    }
    const auto image = load_raw(path, ImageSize::explicit_size(4, 4));
    CHECK(image.width == 4 && image.height == 4 && image.stride == 4);
    CHECK(image.pixels.size() == 16 && image.pixels[15] == 15);

    bool threw = false;
    try {
        (void)load_raw(path, ImageSize::explicit_size(3, 5));
    } catch (const InputError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try {
        (void)load_raw(path, ImageSize::native_size());
    } catch (const InputError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try {
        validate_image(0, 4, 4, 16);
    } catch (const InputError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try {
        validate_image(static_cast<std::size_t>(-1), 2,
                       static_cast<std::size_t>(-1), 16);
    } catch (const InputError&) { threw = true; }
    CHECK(threw);
    BenchmarkConfig jpeg;
    jpeg.input = path;
    jpeg.format = InputFormat::Jpeg;
    threw = false;
    try {
        (void)load_image(jpeg);
    } catch (const InputError& error) {
#ifdef APRILTAG_BENCH_NO_OPENCV
        threw = std::string(error.what()).find("JPEG support is unavailable") !=
                    std::string::npos ||
                std::string(error.what()).find("cannot decode JPEG input") !=
                    std::string::npos;
#else
        threw = std::string(error.what()).find("cannot decode JPEG input") !=
                std::string::npos;
#endif
    }
    CHECK(threw);
    std::remove(path.c_str());
}

Detection sample_detection()
{
    Detection detection;
    detection.id = 7;
    detection.margin = 42.25;
    detection.center[0] = 10;
    detection.center[1] = 20;
    for (int i = 0; i < 8; ++i) detection.corners[i] = i + 0.5;
    return detection;
}

void test_statistics_and_checksums()
{
    const std::vector<std::uint64_t> ns = {
        1000000, 2000000, 3000000, 4000000, 10000000};
    const auto stats = compute_stats(ns);
    CHECK(stats.count == 5);
    CHECK(stats.min_ms == 1 && stats.median_ms == 3 && stats.mean_ms == 4);
    CHECK(stats.p95_ms == 10 && stats.max_ms == 10);
    CHECK(std::abs(stats.stddev_ms - std::sqrt(10.0)) < 1e-12);
    CHECK(stats.mean_fps == 250);
    CHECK(std::abs(stats.median_fps - 1000.0 / 3.0) < 1e-12);

    Detection original = sample_detection();
    const auto hash = checksum_detections(&original, 1);
    CHECK(hash == checksum_detections(&original, 1));
    Detection changed = original;
    ++changed.id;
    CHECK(hash != checksum_detections(&changed, 1));
    changed = original; changed.center[0] += 1;
    CHECK(hash != checksum_detections(&changed, 1));
    changed = original; changed.margin += 1;
    CHECK(hash != checksum_detections(&changed, 1));
    changed = original; changed.corners[7] += 1;
    CHECK(hash != checksum_detections(&changed, 1));
    CHECK(checksum_detections(nullptr, 0) != hash);

    DetectionChecksum incremental(1);
    incremental.add(original);
    CHECK(incremental.value() == hash);

    bool threw = false;
    try {
        (void)compute_stats({0});
    } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void test_detection_count_limit()
{
    CHECK(validate_detection_count(kMaxDetections - 1) ==
          kMaxDetections - 1);

    bool threw = false;
    try {
        (void)validate_detection_count(kMaxDetections);
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    threw = false;
    try {
        (void)validate_detection_count(kMaxDetections + 1);
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    threw = false;
    try {
        (void)validate_detection_count(-1);
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

struct FakeBackendState {
    int captured_calls = 0;
    std::vector<bool> capture_changes;
};

class FakeBackend final : public Backend {
public:
    explicit FakeBackend(BackendKind kind, bool unstable = false,
                           std::uint64_t checksum = 1234, int fail_call = 0,
                           std::shared_ptr<FakeBackendState> state = {})
        : kind_(kind), unstable_(unstable), checksum_(checksum),
          fail_call_(fail_call), state_(std::move(state)) {}
    BackendKind kind() const override { return kind_; }
    const char* name() const override { return backend_name(kind_); }
    void set_capture_detections(bool capture) override
    {
        capture_ = capture;
        if (state_) state_->capture_changes.push_back(capture);
    }
    DetectionResult detect(const PreparedImage&) override
    {
        ++calls_;
        if (calls_ == fail_call_) throw std::runtime_error("detect failed");
        if (capture_) {
            detections_ = {sample_detection()};
            detections_[0].id = static_cast<std::uint64_t>(calls_);
            if (state_) ++state_->captured_calls;
        }
        return {2, unstable_ ? static_cast<std::uint64_t>(calls_) : checksum_};
    }
    const std::vector<Detection>& detections() const override
    {
        return detections_;
    }
private:
    BackendKind kind_;
    bool unstable_;
    std::uint64_t checksum_;
    int fail_call_;
    int calls_ = 0;
    bool capture_ = false;
    std::shared_ptr<FakeBackendState> state_;
    std::vector<Detection> detections_;
};

void test_latest_detections()
{
    auto state = std::make_shared<FakeBackendState>();
    FakeBackend backend(BackendKind::RustRvv, false, 1234, 0, state);
    PreparedImage image{1, 1, 1, {0}};
    (void)backend.detect(image);
    CHECK(backend.detections().empty());
    backend.set_capture_detections(true);
    (void)backend.detect(image);
    CHECK(backend.detections().size() == 1);
    CHECK(backend.detections()[0].id == 2);
    backend.set_capture_detections(false);
    (void)backend.detect(image);
    CHECK(backend.detections()[0].id == 2);
    CHECK(state->captured_calls == 1);
}

#ifndef APRILTAG_BENCH_NO_OPENCV
void test_visual_dumps()
{
    TemporaryDirectory temporary_directory;
    const std::string directory = temporary_directory.path().string();
    const std::string empty_directory = directory + "/empty";
    const std::string failed_path = directory + "/not-a-directory";
    const std::string failed_write_directory = directory + "/write-failure";

    PreparedImage image{160, 100, 160,
                        std::vector<std::uint8_t>(16000, 80)};
    Detection detection = sample_detection();
    detection.center[0] = 80;
    detection.center[1] = 55;
    const double corners[] = {40, 30, 120, 30, 120, 80, 40, 80};
    std::copy(std::begin(corners), std::end(corners), detection.corners);
    write_visual_dumps(directory, image,
                       {{BackendKind::RustRvv, {detection}}});

    const cv::Mat input = cv::imread(directory + "/input.png",
                                     cv::IMREAD_UNCHANGED);
    const cv::Mat overlay = cv::imread(
        directory + "/rust-rvv-detections.png", cv::IMREAD_UNCHANGED);
    CHECK(input.rows == 100 && input.cols == 160 && input.channels() == 1);
    CHECK(overlay.rows == 100 && overlay.cols == 160 && overlay.channels() == 3);
    CHECK(cv::countNonZero(overlay.reshape(1) != 80) > 0);

    write_visual_dumps(empty_directory, image,
                       {{BackendKind::CReference, {}}});
    const cv::Mat empty = cv::imread(
        empty_directory + "/c-reference-detections.png",
        cv::IMREAD_UNCHANGED);
    CHECK(empty.rows == 100 && empty.cols == 160 && empty.channels() == 3);
    CHECK(cv::countNonZero(empty(cv::Rect(0, 24, 150, 45)).reshape(1) != 80) >
          0);

    {
        std::ofstream file(failed_path);
        file << "file";
    }
    bool threw = false;
    try {
        write_visual_dumps(failed_path, image,
                           {{BackendKind::RustScalar, {}}});
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    std::filesystem::create_directories(failed_write_directory + "/input.png");
    threw = false;
    try {
        write_visual_dumps(failed_write_directory, image,
                           {{BackendKind::RustScalar, {}}});
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
#endif

void test_schedule_and_stability()
{
    CHECK(batch_order(2, 0) == std::vector<std::size_t>({0, 1}));
    CHECK(batch_order(2, 1) == std::vector<std::size_t>({1, 0}));
    CHECK(batch_order(3, 0) == std::vector<std::size_t>({0, 1, 2}));
    CHECK(batch_order(3, 1) == std::vector<std::size_t>({2, 1, 0}));

    BenchmarkConfig config;
    config.warmup = 1;
    config.iterations = 2;
    config.batches = 2;
    PreparedImage image{1, 1, 1, {0}};
    std::vector<std::unique_ptr<Backend>> stable;
    auto capture_state = std::make_shared<FakeBackendState>();
    stable.emplace_back(new FakeBackend(BackendKind::RustRvv, false, 1234, 0,
                                        capture_state));
    stable.emplace_back(new FakeBackend(BackendKind::CReference));
    std::ostringstream output;
    CHECK(run_benchmark(config, std::move(stable), image, output) == 0);
    CHECK(capture_state->captured_calls == 1);
    CHECK(capture_state->capture_changes == std::vector<bool>({true, false}));
    CHECK(output.str().find("RESULT backend=rust-rvv") != std::string::npos);
    CHECK(output.str().find("RESULT backend=c-reference") != std::string::npos);
    CHECK(output.str().find(std::string("Build       : ") +
                            APRILTAG_BENCH_BUILD_ID) !=
          std::string::npos);
    CHECK(output.str().find("Bytes       : 1") != std::string::npos);
    CHECK(output.str().find("RVV stages  : uniform by backend") !=
          std::string::npos);
    CHECK(output.str().find("Total measured") != std::string::npos);
    CHECK(output.str().find("equivalent-output speed") != std::string::npos);
    const std::string result = output.str().substr(
        output.str().find("RESULT backend=rust-rvv"));
    for (const char* field : {
             " total_ms=", " input_hash=", " width=1",
             " height=1", " bytes=1", " factor=2", " min_blob=25",
             " warmup=1", " iterations=2", " batches=2"}) {
        CHECK(result.find(field) != std::string::npos);
    }
    CHECK(result.find(" rvv_mask=all stages=all") != std::string::npos);
    const auto c_result = output.str().find("RESULT backend=c-reference");
    CHECK(output.str().substr(c_result).find(" rvv_mask=n/a stages=n/a") !=
          std::string::npos);
    CHECK(result.find(std::string(" build=") + APRILTAG_BENCH_BUILD_ID) !=
          std::string::npos);

    std::vector<std::unique_ptr<Backend>> differing;
    differing.emplace_back(new FakeBackend(BackendKind::RustRvv, false, 111));
    differing.emplace_back(new FakeBackend(BackendKind::CReference, false, 222));
    std::ostringstream mismatch;
    CHECK(run_benchmark(config, std::move(differing), image, mismatch) == 0);
    CHECK(mismatch.str().find("raw mean-latency comparison") !=
          std::string::npos);
    CHECK(mismatch.str().find("equivalent-output speedup") ==
          std::string::npos);

    std::vector<std::unique_ptr<Backend>> unstable;
    unstable.emplace_back(new FakeBackend(BackendKind::RustScalar, true));
    std::ostringstream rejected;
    bool threw = false;
    try {
        (void)run_benchmark(config, std::move(unstable), image, rejected);
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(rejected.str().find("RESULT ") == std::string::npos);

    std::vector<std::unique_ptr<Backend>> failed;
    failed.emplace_back(new FakeBackend(BackendKind::RustRvv, false, 1234, 3));
    std::ostringstream failed_output;
    std::string failure_message;
    try {
        (void)run_benchmark(config, std::move(failed), image, failed_output);
    } catch (const std::runtime_error& error) {
        failure_message = error.what();
    }
    CHECK(failure_message.find("Rust RVV") != std::string::npos);
    CHECK(failure_message.find("batch 1") != std::string::npos);
    CHECK(failure_message.find("call 1") != std::string::npos);
    CHECK(failure_message.find("detect failed") != std::string::npos);
}

#ifdef APRILTAG_BENCH_BACKEND_TESTS
void check_jpeg_preparation(const char* fixture, ImageSize size,
                            std::size_t expected_width,
                            std::size_t expected_height)
{
    BenchmarkConfig config;
    config.input = fixture;
    config.format = InputFormat::Jpeg;
    config.size = size;
    const PreparedImage image = load_image(config);
    CHECK(image.width == expected_width);
    CHECK(image.height == expected_height);
    CHECK(image.stride == expected_width);
    CHECK(image.pixels.size() == expected_width * expected_height);
}

void test_jpeg_preparation()
{
    const char* fixture = APRILTAG_BENCH_FIXTURE_PATH;
    const PreparedImage native = load_image(BenchmarkConfig{
        fixture, InputFormat::Jpeg, ImageSize::native_size()});
    CHECK(native.width > 0 && native.height > 0);
    CHECK(native.stride == native.width);
    CHECK(native.pixels.size() == native.width * native.height);

    check_jpeg_preparation(fixture, ImageSize::explicit_size(640, 360),
                           640, 360);
    check_jpeg_preparation(fixture, ImageSize::explicit_size(1280, 720),
                           1280, 720);
    check_jpeg_preparation(fixture, ImageSize::explicit_size(1920, 1080),
                           1920, 1080);
}

void test_persistent_backends()
{
    BenchmarkConfig config;
    config.input = APRILTAG_BENCH_FIXTURE_PATH;
    config.format = InputFormat::Jpeg;
    const PreparedImage image = load_image(config);
    for (const BackendKind kind : {BackendKind::RustScalar,
                                   BackendKind::CReference}) {
        std::unique_ptr<Backend> backend = kind == BackendKind::CReference
            ? make_c_backend(config) : make_rust_backend(config, kind);
        backend->set_capture_detections(true);
        const DetectionResult first = backend->detect(image);
        const DetectionResult second = backend->detect(image);
        CHECK(first.count >= 0);
        CHECK(backend->detections().size() ==
              static_cast<std::size_t>(second.count));
        CHECK(checksum_detections(backend->detections().data(),
                                  backend->detections().size()) ==
              second.checksum);
        CHECK(first.count == second.count);
        CHECK(first.checksum == second.checksum);
    }
}
#endif

}  // namespace

int main()
{
    test_parser();
    test_raw_and_geometry();
    test_statistics_and_checksums();
    test_detection_count_limit();
    test_latest_detections();
#ifndef APRILTAG_BENCH_NO_OPENCV
    test_visual_dumps();
#endif
    test_schedule_and_stability();
#ifdef APRILTAG_BENCH_BACKEND_TESTS
    test_jpeg_preparation();
    test_persistent_backends();
#endif
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all benchmark tests passed\n";
    return 0;
}
