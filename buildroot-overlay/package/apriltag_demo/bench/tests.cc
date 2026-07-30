#include "benchmark.h"

#include <cmath>
#include <climits>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace apriltag_bench;

#ifndef APRILTAG_BENCH_BUILD_ID
#define APRILTAG_BENCH_BUILD_ID "unknown"
#endif

namespace {

int failures = 0;

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
    CHECK(parse({"bench", "--factor", "1.5"}).factor == 1);
    CHECK(parse({"bench", "--help"}).help);
    CHECK(parse({"bench", "--format", "raw", "--size", "native",
                 "--help"}).help);

    expect_parse_error({"bench", "--factor", "3"});
    expect_parse_error({"bench", "--factor", "1.0"});
    expect_parse_error({"bench", "--factor", "2.0"});
    expect_parse_error({"bench", "--backend", "c-reference"});
    expect_parse_error({"bench", "--size", "1280"});
    expect_parse_error({"bench", "--warmup", "-1"});
    expect_parse_error({"bench", "--iterations", "0"});
    expect_parse_error({"bench", "--batches", "1x"});
    expect_parse_error({"bench", "--min-blob", "4294967296"});
    expect_parse_error({"bench", "--min-blob", "2147483648"});
    expect_parse_error({"bench", "--unknown"});
    expect_parse_error({"bench", "--input"});
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

class FakeBackend final : public Backend {
public:
    explicit FakeBackend(BackendKind kind, bool unstable = false,
                          std::uint64_t checksum = 1234, int fail_call = 0)
        : kind_(kind), unstable_(unstable), checksum_(checksum),
          fail_call_(fail_call) {}
    BackendKind kind() const override { return kind_; }
    const char* name() const override { return backend_name(kind_); }
    DetectionResult detect(const PreparedImage&) override
    {
        ++calls_;
        if (calls_ == fail_call_) throw std::runtime_error("detect failed");
        return {2, unstable_ ? static_cast<std::uint64_t>(calls_) : checksum_};
    }
private:
    BackendKind kind_;
    bool unstable_;
    std::uint64_t checksum_;
    int fail_call_;
    int calls_ = 0;
};

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
    stable.emplace_back(new FakeBackend(BackendKind::RustRvv));
    stable.emplace_back(new FakeBackend(BackendKind::CReference));
    std::ostringstream output;
    CHECK(run_benchmark(config, std::move(stable), image, output) == 0);
    CHECK(output.str().find("RESULT backend=rust-rvv") != std::string::npos);
    CHECK(output.str().find("RESULT backend=c-reference") != std::string::npos);
    CHECK(output.str().find(std::string("Build       : ") +
                            APRILTAG_BENCH_BUILD_ID) !=
          std::string::npos);
    CHECK(output.str().find("Bytes       : 1") != std::string::npos);
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
        const DetectionResult first = backend->detect(image);
        const DetectionResult second = backend->detect(image);
        CHECK(first.count >= 0);
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
