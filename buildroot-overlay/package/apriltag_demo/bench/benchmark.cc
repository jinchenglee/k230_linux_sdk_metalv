#include "benchmark.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <time.h>

#ifndef APRILTAG_BENCH_BUILD_ID
#define APRILTAG_BENCH_BUILD_ID "unknown"
#endif

#ifndef APRILTAG_BENCH_NO_OPENCV
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace apriltag_bench {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
T parse_integer(const char* value, const char* option, T minimum)
{
    T parsed{};
    const char* end = value + std::strlen(value);
    const auto result = std::from_chars(value, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < minimum) {
        throw ArgumentError(std::string(option) + " has an invalid value: " + value);
    }
    return parsed;
}

const char* require_value(int argc, const char* const argv[], int& index)
{
    if (++index >= argc) {
        throw ArgumentError(std::string(argv[index - 1]) + " requires a value");
    }
    return argv[index];
}

ImageSize parse_size(const char* value)
{
    if (std::strcmp(value, "native") == 0) return ImageSize::native_size();
    const char* separator = std::strchr(value, 'x');
    if (!separator || separator == value || separator[1] == '\0' ||
        std::strchr(separator + 1, 'x')) {
        throw ArgumentError("--size must be native or WxH");
    }
    std::size_t width{};
    std::size_t height{};
    const auto width_result = std::from_chars(value, separator, width);
    const char* end = value + std::strlen(value);
    const auto height_result = std::from_chars(separator + 1, end, height);
    if (width_result.ec != std::errc() || width_result.ptr != separator ||
        height_result.ec != std::errc() || height_result.ptr != end ||
        width == 0 || height == 0) {
        throw ArgumentError("--size must be native or WxH");
    }
    return ImageSize::explicit_size(width, height);
}

void parse_rvv_stages_impl(const std::string& value, BenchmarkConfig& config)
{
    struct Stage { const char* name; std::uint64_t bit; };
    static constexpr Stage stages[] = {
        {"decimate", APRILTAG_KERNEL_DECIMATE},
        {"threshold", APRILTAG_KERNEL_THRESHOLD},
        {"rle", APRILTAG_KERNEL_RLE},
        {"lfps-tuned", APRILTAG_KERNEL_LFPS_TUNED},
        {"gaussian", APRILTAG_KERNEL_GAUSSIAN},
        {"gray-model", APRILTAG_KERNEL_GRAY_MODEL},
    };
    if (value.empty()) throw ArgumentError("--rvv-stages requires a non-empty value");
    config.rvv_mask_explicit = true;
    if (value == "all") config.rvv_mask = APRILTAG_KERNEL_ALL;
    else if (value == "none") config.rvv_mask = 0;
    else {
        config.rvv_mask = 0;
        std::size_t begin = 0;
        while (begin < value.size()) {
            const std::size_t end = value.find(',', begin);
            const std::string name = value.substr(begin, end - begin);
            const auto stage = std::find_if(std::begin(stages), std::end(stages),
                [&](const Stage& candidate) { return name == candidate.name; });
            if (name.empty() || stage == std::end(stages))
                throw ArgumentError("--rvv-stages contains unknown stage: " + name);
            if (config.rvv_mask & stage->bit)
                throw ArgumentError("--rvv-stages contains duplicate stage: " + name);
            config.rvv_mask |= stage->bit;
            if (end == std::string::npos) break;
            begin = end + 1;
            if (begin == value.size())
                throw ArgumentError("--rvv-stages contains an empty stage");
        }
    }
    if (config.rvv_mask == 0) config.rvv_stages = "none";
    else if (config.rvv_mask == APRILTAG_KERNEL_ALL) config.rvv_stages = "all";
    else {
        config.rvv_stages.clear();
        for (const auto& stage : stages) if (config.rvv_mask & stage.bit) {
            if (!config.rvv_stages.empty()) config.rvv_stages += ',';
            config.rvv_stages += stage.name;
        }
    }
}

InputFormat infer_format(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        throw InputError("cannot infer input format; use --format");
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".jpg" || extension == ".jpeg") return InputFormat::Jpeg;
    if (extension == ".y8" || extension == ".gray" || extension == ".raw") {
        return InputFormat::Raw;
    }
    throw InputError("cannot infer input format from extension; use --format");
}

std::uint64_t hash_u64(std::uint64_t hash, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t double_bits(double value)
{
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint64_t monotonic_raw_ns()
{
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(time.tv_nsec);
}

std::string format_eta(double seconds)
{
    const auto rounded = static_cast<unsigned long long>(std::max(0.0, seconds));
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << rounded / 60 << ':'
         << std::setw(2) << rounded % 60;
    return text.str();
}

DetectionResult detect_with_context(Backend& backend,
                                    const PreparedImage& image,
                                    const std::string& context)
{
    try {
        return backend.detect(image);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(backend.name()) + " " + context +
                                 ": " + error.what());
    }
}

}  // namespace

ImageSize ImageSize::native_size() { return {}; }

ImageSize ImageSize::explicit_size(std::size_t width, std::size_t height)
{
    return {false, width, height};
}

void parse_rvv_stages(const std::string& value, BenchmarkConfig& config)
{
    parse_rvv_stages_impl(value, config);
}

int validate_detection_count(int count)
{
    if (count < 0) throw std::runtime_error("detect failed");
    if (count >= kMaxDetections) {
        throw std::runtime_error(
            "detection count reached the supported maximum (4096); result may be truncated");
    }
    return count;
}

BenchmarkConfig parse_args(int argc, const char* const argv[])
{
    BenchmarkConfig config;
#ifdef APRILTAG_BENCH_PROFILE
    config.backends = {BackendKind::RustRvv};
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--input") {
            config.input = require_value(argc, argv, i);
        } else if (option == "--format") {
            const std::string value(require_value(argc, argv, i));
            if (value == "auto") config.format = InputFormat::Auto;
            else if (value == "raw") config.format = InputFormat::Raw;
            else if (value == "jpeg") config.format = InputFormat::Jpeg;
            else throw ArgumentError("--format must be auto, raw, or jpeg");
        } else if (option == "--size") {
            config.size = parse_size(require_value(argc, argv, i));
        } else if (option == "--backend") {
            const std::string value(require_value(argc, argv, i));
            if (value == "all") {
                config.backends = {BackendKind::RustRvv, BackendKind::CReference,
                                   BackendKind::RustScalar};
            } else if (value == "rust-rvv") {
                config.backends = {BackendKind::RustRvv};
            } else if (value == "rust-scalar") {
                config.backends = {BackendKind::RustScalar};
            } else if (value == "c") {
                config.backends = {BackendKind::CReference};
            } else {
                throw ArgumentError(
                    "--backend must be all, rust-rvv, rust-scalar, or c");
            }
        } else if (option == "--factor") {
            const std::string value(require_value(argc, argv, i));
            if (value == "1") {
                config.factor = 0; config.factor_value = 1.0;
            } else if (value == "1.5") {
                config.factor = 1; config.factor_value = 1.5;
            } else if (value == "2") {
                config.factor = 2; config.factor_value = 2.0;
            } else {
                throw ArgumentError("--factor must be 1, 1.5, or 2");
            }
        } else if (option == "--rvv-stages") {
            parse_rvv_stages_impl(require_value(argc, argv, i), config);
        } else if (option == "--min-blob") {
            config.min_blob = parse_integer<std::uint32_t>(
                require_value(argc, argv, i), "--min-blob", 1);
            if (config.min_blob > static_cast<std::uint32_t>(
                                      std::numeric_limits<int>::max())) {
                throw ArgumentError("--min-blob exceeds INT_MAX");
            }
        } else if (option == "--warmup") {
            config.warmup = parse_integer<int>(
                require_value(argc, argv, i), "--warmup", 0);
        } else if (option == "--iterations") {
            config.iterations = parse_integer<int>(
                require_value(argc, argv, i), "--iterations", 1);
        } else if (option == "--batches") {
            config.batches = parse_integer<int>(
                require_value(argc, argv, i), "--batches", 1);
        } else if (option == "--dump-dir") {
            config.dump_dir = require_value(argc, argv, i);
        } else if (option == "--no-dump") {
            config.dump_dir.clear();
        } else if (option == "--help" || option == "-h") {
            config.help = true;
            return config;
        } else {
            throw ArgumentError("unknown argument: " + option);
        }
    }
    const InputFormat resolved = config.format == InputFormat::Auto
                                     ? infer_format(config.input) : config.format;
    if (resolved == InputFormat::Raw && config.size.native) {
        throw ArgumentError("raw input requires --size WxH");
    }
#ifdef APRILTAG_BENCH_PROFILE
    if (config.backends != std::vector<BackendKind>{BackendKind::RustRvv})
        throw ArgumentError("profile benchmark supports only rust-rvv");
#endif
    if (config.rvv_mask_explicit) {
        if (config.backends == std::vector<BackendKind>{BackendKind::RustRvv,
                                                        BackendKind::CReference,
                                                        BackendKind::RustScalar}) {
            config.backends.pop_back();
        }
        const bool has_rvv = std::find(config.backends.begin(), config.backends.end(),
                                       BackendKind::RustRvv) != config.backends.end();
        if (!has_rvv || std::find(config.backends.begin(), config.backends.end(),
                                  BackendKind::RustScalar) != config.backends.end()) {
            throw ArgumentError("--rvv-stages requires rust-rvv (optionally with C)");
        }
    }
    return config;
}

void validate_image(std::size_t width, std::size_t height,
                    std::size_t stride, std::size_t storage_size)
{
    if (width == 0 || height == 0 || stride < width ||
        height > std::numeric_limits<std::size_t>::max() / stride ||
        storage_size < stride * height) {
        throw InputError("invalid image geometry or storage size");
    }
}

PreparedImage load_raw(const std::string& path, ImageSize size)
{
    if (size.native) throw InputError("raw input requires explicit dimensions");
    if (size.width == 0 || size.height == 0 ||
        size.height > std::numeric_limits<std::size_t>::max() / size.width) {
        throw InputError("raw input dimensions overflow");
    }
    const std::size_t expected = size.width * size.height;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw InputError("cannot open input: " + path);
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) != expected) {
        throw InputError("raw input size does not equal width * height");
    }
    PreparedImage image{size.width, size.height, size.width,
                        std::vector<std::uint8_t>(expected)};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(image.pixels.data()),
              static_cast<std::streamsize>(expected));
    if (!file) throw InputError("cannot read complete raw input: " + path);
    return image;
}

PreparedImage load_image(const BenchmarkConfig& config)
{
    const InputFormat format = config.format == InputFormat::Auto
                                    ? infer_format(config.input) : config.format;
    if (format == InputFormat::Raw) return load_raw(config.input, config.size);
#ifdef APRILTAG_BENCH_NO_OPENCV
    throw InputError("JPEG support is unavailable in this build");
#else
    cv::Mat gray = cv::imread(config.input, cv::IMREAD_GRAYSCALE);
    if (gray.empty()) throw InputError("cannot decode JPEG input: " + config.input);
    if (!config.size.native) {
        if (config.size.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            config.size.height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw InputError("requested JPEG dimensions are too large");
        }
        cv::Mat resized;
        cv::resize(gray, resized,
                   cv::Size(static_cast<int>(config.size.width),
                            static_cast<int>(config.size.height)),
                   0, 0, cv::INTER_LINEAR);
        gray = resized;
    }
    PreparedImage image;
    image.width = static_cast<std::size_t>(gray.cols);
    image.height = static_cast<std::size_t>(gray.rows);
    image.stride = image.width;
    image.pixels.resize(image.width * image.height);
    for (std::size_t row = 0; row < image.height; ++row) {
        std::memcpy(image.pixels.data() + row * image.width,
                    gray.ptr(static_cast<int>(row)), image.width);
    }
    validate_image(image.width, image.height, image.stride, image.pixels.size());
    return image;
#endif
}

PreparedImage load_encoded_image(const std::vector<std::uint8_t>& bytes,
                                 ImageSize size)
{
#ifdef APRILTAG_BENCH_NO_OPENCV
    (void)bytes;
    (void)size;
    throw InputError("encoded-image support is unavailable in this build");
#else
    if (bytes.empty()) throw InputError("cannot decode empty encoded input");
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw InputError("encoded sequence input is too large");
    const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1,
                          const_cast<std::uint8_t*>(bytes.data()));
    cv::Mat gray = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE);
    if (gray.empty()) throw InputError("cannot decode sequence input");
    if (!size.native) {
        if (size.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            size.height > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw InputError("requested sequence dimensions are too large");
        cv::Mat resized;
        cv::resize(gray, resized,
                   cv::Size(static_cast<int>(size.width), static_cast<int>(size.height)),
                   0, 0, cv::INTER_LINEAR);
        gray = resized;
    }
    PreparedImage image;
    image.width = static_cast<std::size_t>(gray.cols);
    image.height = static_cast<std::size_t>(gray.rows);
    image.stride = image.width;
    image.pixels.resize(image.width * image.height);
    for (std::size_t row = 0; row < image.height; ++row)
        std::memcpy(image.pixels.data() + row * image.width,
                    gray.ptr(static_cast<int>(row)), image.width);
    validate_image(image.width, image.height, image.stride, image.pixels.size());
    return image;
#endif
}

Statistics compute_stats(const std::vector<std::uint64_t>& samples_ns)
{
    if (samples_ns.empty()) throw std::invalid_argument("no timing samples");
    if (std::find(samples_ns.begin(), samples_ns.end(), 0) != samples_ns.end()) {
        throw std::invalid_argument("timing samples must be greater than zero");
    }
    std::vector<std::uint64_t> sorted(samples_ns);
    std::sort(sorted.begin(), sorted.end());
    const long double total_ns = std::accumulate(
        samples_ns.begin(), samples_ns.end(), static_cast<long double>(0));
    const long double mean_ns = total_ns / samples_ns.size();
    long double squared = 0;
    for (const auto sample : samples_ns) {
        const long double difference = sample - mean_ns;
        squared += difference * difference;
    }
    const std::size_t median_index = sorted.size() / 2;
    const long double median_ns = sorted.size() % 2
        ? sorted[median_index]
        : (static_cast<long double>(sorted[median_index - 1]) +
           sorted[median_index]) / 2;
    const std::size_t p95_index =
        static_cast<std::size_t>(std::ceil(0.95 * sorted.size())) - 1;
    Statistics result;
    result.count = samples_ns.size();
    result.total_ms = static_cast<double>(total_ns / 1000000.0L);
    result.min_ms = sorted.front() / 1000000.0;
    result.median_ms = static_cast<double>(median_ns / 1000000.0L);
    result.mean_ms = static_cast<double>(mean_ns / 1000000.0L);
    result.p95_ms = sorted[p95_index] / 1000000.0;
    result.max_ms = sorted.back() / 1000000.0;
    result.stddev_ms = static_cast<double>(
        std::sqrt(squared / samples_ns.size()) / 1000000.0L);
    result.mean_fps = 1000.0 / result.mean_ms;
    result.median_fps = 1000.0 / result.median_ms;
    return result;
}

std::uint64_t checksum_bytes(const void* data, std::size_t size)
{
    std::uint64_t hash = kFnvOffset;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t checksum_detections(const Detection* detections,
                                  std::size_t count)
{
    DetectionChecksum checksum(count);
    for (std::size_t i = 0; i < count; ++i) {
        checksum.add(detections[i]);
    }
    return checksum.value();
}

DetectionChecksum::DetectionChecksum(std::size_t count)
    : hash_(hash_u64(kFnvOffset, count))
{
}

void DetectionChecksum::add(const Detection& detection)
{
    hash_ = hash_u64(hash_, detection.id);
    hash_ = hash_u64(hash_, double_bits(detection.margin));
    for (double value : detection.center) {
        hash_ = hash_u64(hash_, double_bits(value));
    }
    for (double value : detection.corners) {
        hash_ = hash_u64(hash_, double_bits(value));
    }
}

std::uint64_t DetectionChecksum::value() const
{
    return hash_;
}

std::vector<std::size_t> batch_order(std::size_t backend_count,
                                     std::size_t batch_index)
{
    std::vector<std::size_t> order(backend_count);
    std::iota(order.begin(), order.end(), 0);
    if ((batch_index & 1U) != 0) std::reverse(order.begin(), order.end());
    return order;
}

const char* backend_key(BackendKind kind)
{
    switch (kind) {
    case BackendKind::RustRvv: return "rust-rvv";
    case BackendKind::CReference: return "c-reference";
    case BackendKind::RustScalar: return "rust-scalar";
    }
    return "unknown";
}

const char* backend_name(BackendKind kind)
{
    switch (kind) {
    case BackendKind::RustRvv: return "Rust RVV";
    case BackendKind::CReference: return "C reference";
    case BackendKind::RustScalar: return "Rust scalar";
    }
    return "unknown";
}

int run_benchmark(const BenchmarkConfig& config,
                  std::vector<std::unique_ptr<Backend>> backends,
                  const PreparedImage& image, std::ostream& out)
{
    if (backends.empty()) throw std::runtime_error("no benchmark backends");
    validate_image(image.width, image.height, image.stride, image.pixels.size());
    const std::uint64_t input_hash =
        checksum_bytes(image.pixels.data(), image.pixels.size());
    out << "K230 AprilTag detector benchmark\n"
        << "Build       : " << APRILTAG_BENCH_BUILD_ID << '\n'
        << "Input       : " << config.input << '\n'
        << "Image       : " << image.width << 'x' << image.height << " Y8\n"
        << "Bytes       : " << image.pixels.size() << '\n'
        << "Input hash  : " << std::hex << input_hash << std::dec << '\n'
        << "Factor      : " << config.factor_value << '\n'
        << "Min blob    : " << config.min_blob << '\n'
        << "RVV stages  : " << (config.rvv_mask_explicit ? config.rvv_stages
                                                          : "uniform by backend") << '\n'
        << "Warmup      : " << config.warmup << " calls/backend\n"
        << "Measurement : " << config.batches << " batches x "
        << config.iterations << " calls/backend\n"
        << "Backends    : ";
    for (std::size_t i = 0; i < backends.size(); ++i) {
        if (i) out << ", ";
        out << backends[i]->name();
    }
    out << "\n\n" << std::flush;

    std::vector<DetectionResult> expected(backends.size());
    std::vector<std::vector<std::uint64_t>> samples(backends.size());
#ifdef APRILTAG_BENCH_PROFILE
    std::vector<std::vector<apriltag_ccl_profile_t>> profiles(backends.size());
    std::vector<std::vector<apriltag_ccl_scratch_v1_t>> scratches(backends.size());
#endif
    std::vector<std::vector<double>> batch_means(
        static_cast<std::size_t>(config.batches),
        std::vector<double>(backends.size()));
    for (const auto& backend : backends) backend->set_capture_detections(true);
    for (std::size_t i = 0; i < backends.size(); ++i) {
        expected[i] = detect_with_context(*backends[i], image, "validation call 1");
    }
    for (const auto& backend : backends) backend->set_capture_detections(false);
    if (!config.dump_dir.empty()) {
        std::vector<VisualDump> dumps;
        dumps.reserve(backends.size());
        for (const auto& backend : backends) {
            dumps.push_back({backend->kind(), backend->detections()});
        }
        write_visual_dumps(config.dump_dir, image, dumps);
    }
    for (std::size_t i = 0; i < backends.size(); ++i) {
        out << "Warming up " << backends[i]->name() << " ... " << std::flush;
        for (int call = 0; call < config.warmup; ++call) {
            const DetectionResult result = detect_with_context(
                *backends[i], image,
                "warmup call " + std::to_string(call + 1));
            if (result.count != expected[i].count ||
                result.checksum != expected[i].checksum) {
                throw std::runtime_error(std::string(backends[i]->name()) +
                                         " produced unstable warmup output");
            }
        }
        out << expected[i].count << " detections\n" << std::flush;
    }

    out << "\nBatch";
    for (const auto& backend : backends) out << "   " << std::setw(14) << backend->name();
    out << "   ETA\n" << std::flush;
    const std::uint64_t progress_start = monotonic_raw_ns();
    for (int batch = 0; batch < config.batches; ++batch) {
        for (const std::size_t index : batch_order(backends.size(), batch)) {
            std::uint64_t batch_total = 0;
            for (int call = 0; call < config.iterations; ++call) {
                DetectionResult result;
                const std::uint64_t start = monotonic_raw_ns();
                try {
                    result = backends[index]->detect(image);
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        std::string(backends[index]->name()) +
                        " failed in batch " + std::to_string(batch + 1) +
                        " call " + std::to_string(call + 1) + ": " +
                        error.what());
                }
                const std::uint64_t finish = monotonic_raw_ns();
#ifdef APRILTAG_BENCH_PROFILE
                apriltag_ccl_profile_t profile{};
                apriltag_ccl_scratch_v1_t scratch{};
                try {
                    if (backends[index]->consume_profile(profile, scratch)) {
                        profiles[index].push_back(profile);
                        scratches[index].push_back(scratch);
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        std::string(backends[index]->name()) + " (" +
                        backend_key(backends[index]->kind()) +
                        ") profile failed in batch " + std::to_string(batch + 1) +
                        " call " + std::to_string(call + 1) + ": " + error.what());
                }
#endif
                if (result.count != expected[index].count ||
                    result.checksum != expected[index].checksum) {
                    throw std::runtime_error(std::string(backends[index]->name()) +
                                             " produced unstable measured output");
                }
                const std::uint64_t elapsed = finish - start;
                samples[index].push_back(elapsed);
                batch_total += elapsed;
            }
            batch_means[batch][index] =
                batch_total / (1000000.0 * config.iterations);
        }
        const double elapsed_seconds =
            (monotonic_raw_ns() - progress_start) / 1000000000.0;
        const double remaining = elapsed_seconds / (batch + 1) *
                                 (config.batches - batch - 1);
        out << std::setw(3) << batch + 1 << '/' << config.batches;
        for (const double mean : batch_means[batch]) {
            out << "   " << std::setw(10) << std::fixed << std::setprecision(3)
                << mean << " ms";
        }
        out << "   " << format_eta(remaining) << '\n' << std::flush;
    }

    std::vector<Statistics> stats;
    for (const auto& backend_samples : samples) stats.push_back(compute_stats(backend_samples));
    std::size_t baseline = backends.size();
    for (std::size_t i = 0; i < backends.size(); ++i) {
        if (backends[i]->kind() == BackendKind::CReference) baseline = i;
    }
    bool outputs_differ = false;
    for (std::size_t i = 1; i < expected.size(); ++i) {
        outputs_differ |= expected[i].count != expected[0].count ||
                          expected[i].checksum != expected[0].checksum;
    }

    out << "\nAprilTag detect() results\n=========================\n\n"
        << std::left << std::setw(22) << "Metric";
    for (const auto& backend : backends) out << std::right << std::setw(18) << backend->name();
    out << '\n';
    auto metric = [&](const char* label, auto value) {
        out << std::left << std::setw(22) << label;
        for (std::size_t i = 0; i < backends.size(); ++i) {
            out << std::right << std::setw(18) << value(i);
        }
        out << '\n';
    };
    metric("Detections/call", [&](std::size_t i) { return std::to_string(expected[i].count); });
    auto milliseconds = [&](double value) {
        std::ostringstream text; text << std::fixed << std::setprecision(3) << value << " ms"; return text.str();
    };
    metric("Minimum", [&](std::size_t i) { return milliseconds(stats[i].min_ms); });
    metric("Median", [&](std::size_t i) { return milliseconds(stats[i].median_ms); });
    metric("Mean", [&](std::size_t i) { return milliseconds(stats[i].mean_ms); });
    metric("95th percentile", [&](std::size_t i) { return milliseconds(stats[i].p95_ms); });
    metric("Maximum", [&](std::size_t i) { return milliseconds(stats[i].max_ms); });
    metric("Standard deviation", [&](std::size_t i) { return milliseconds(stats[i].stddev_ms); });
    metric("Total measured", [&](std::size_t i) { return milliseconds(stats[i].total_ms); });
    auto fps = [&](double value) {
        std::ostringstream text; text << std::fixed << std::setprecision(2) << value << " FPS"; return text.str();
    };
    metric("Mean throughput", [&](std::size_t i) { return fps(stats[i].mean_fps); });
    metric("Median throughput", [&](std::size_t i) { return fps(stats[i].median_fps); });
    metric("Relative speed", [&](std::size_t i) {
        if (baseline == backends.size()) return std::string("n/a");
        if (i == baseline) return std::string("baseline");
        std::ostringstream text; text << std::fixed << std::setprecision(2)
                                     << stats[baseline].mean_ms / stats[i].mean_ms << 'x'; return text.str();
    });
    metric("Checksum", [&](std::size_t i) {
        std::ostringstream text; text << std::hex << expected[i].checksum; return text.str();
    });

    if (outputs_differ) {
        out << "\nWARNING: backend results differ\n";
        for (std::size_t i = 0; i < backends.size(); ++i) {
            out << "  " << backends[i]->name() << ": " << expected[i].count
                << " detections, checksum " << std::hex << expected[i].checksum
                << std::dec << '\n';
        }
        out << "Performance numbers remain valid, but this is not equivalent-output speed.\n";
    }
    if (baseline != backends.size()) {
        for (std::size_t i = 0; i < backends.size(); ++i) {
            if (i == baseline) continue;
            const double percent =
                (1.0 - stats[i].mean_ms / stats[baseline].mean_ms) * 100.0;
            out << (outputs_differ
                        ? "Comparison type: raw mean-latency comparison. "
                        : "Comparison type: equivalent-output speed. ")
                << backends[i]->name() << " is " << std::fixed
                << std::setprecision(1) << std::abs(percent) << "% "
                << (percent >= 0 ? "faster" : "slower")
                << " than C reference";
            if (outputs_differ) out << " (outputs differ)";
            out << ".\n";
        }
    }
    out << '\n';
    for (std::size_t i = 0; i < backends.size(); ++i) {
        out << "RESULT backend=" << backend_key(backends[i]->kind())
            << " rvv_mask=";
        if (backends[i]->kind() == BackendKind::CReference) out << "n/a stages=n/a";
        else if (config.rvv_mask_explicit) out << "0x" << std::hex << config.rvv_mask
                                               << std::dec << " stages=" << config.rvv_stages;
        else out << (backends[i]->kind() == BackendKind::RustRvv ? "all stages=all"
                                                                  : "none stages=none");
        out
            << " calls=" << stats[i].count << std::fixed << std::setprecision(3)
            << " total_ms=" << stats[i].total_ms
            << " min_ms=" << stats[i].min_ms
            << " median_ms=" << stats[i].median_ms
            << " mean_ms=" << stats[i].mean_ms
            << " p95_ms=" << stats[i].p95_ms
            << " max_ms=" << stats[i].max_ms
            << " stddev_ms=" << stats[i].stddev_ms
            << std::setprecision(2)
            << " mean_fps=" << stats[i].mean_fps
            << " median_fps=" << stats[i].median_fps
            << " detections=" << expected[i].count
            << " checksum=" << std::hex << expected[i].checksum
            << " input_hash=" << input_hash << std::dec
            << " build=" << APRILTAG_BENCH_BUILD_ID
            << " width=" << image.width
            << " height=" << image.height
            << " bytes=" << image.pixels.size()
            << " factor=" << config.factor_value
            << " min_blob=" << config.min_blob
            << " warmup=" << config.warmup
            << " iterations=" << config.iterations
             << " batches=" << config.batches << '\n';
#ifdef APRILTAG_BENCH_PROFILE
        if (!profiles[i].empty()) print_profile_report(
            config, backends[i]->kind(), profiles[i], scratches[i], out);
#endif
    }
    out << std::flush;
    return 0;
}

void print_usage(std::ostream& out, const char* program)
{
    out << "Usage: " << program << " [options]\n"
        << "  --input PATH          raw Y8 or JPEG input\n"
        << "  --format FORMAT       auto, raw, or jpeg\n"
        << "  --size SIZE           native or WxH; raw requires WxH\n"
#ifdef APRILTAG_BENCH_PROFILE
        << "  --backend BACKEND     rust-rvv only\n"
#else
        << "  --backend BACKEND     all, rust-rvv, rust-scalar, or c\n"
#endif
        << "  --factor FACTOR       1, 1.5, or 2\n"
        << "  --rvv-stages STAGES   all, none, or comma-separated:\n"
        << "                        decimate,threshold,rle,lfps-tuned,gaussian,gray-model\n"
        << "  --min-blob N          minimum cluster pixels\n"
        << "  --warmup N            untimed calls per backend\n"
        << "  --iterations N        calls per measured batch\n"
        << "  --batches N           measured batches\n"
        << "  --dump-dir PATH       write visual validation images\n"
        << "  --no-dump             disable visual validation images\n"
        << "  --help                 print this help\n";
}

#ifndef APRILTAG_BENCH_CORE_ONLY
int benchmark_main(int argc, const char* const argv[], std::ostream& out,
                   std::ostream& err)
{
    BenchmarkConfig config;
    try {
        config = parse_args(argc, argv);
        if (config.help) {
            print_usage(out, argv[0]);
            return 0;
        }
    } catch (const ArgumentError& error) {
        err << "argument error: " << error.what() << '\n';
        return 2;
    }
    try {
        PreparedImage image = load_image(config);
        std::vector<std::unique_ptr<Backend>> backends;
        for (const BackendKind kind : config.backends) {
#ifdef APRILTAG_BENCH_PROFILE
            backends.push_back(make_rust_backend(config, kind));
#else
            if (kind == BackendKind::CReference) backends.push_back(make_c_backend(config));
            else backends.push_back(make_rust_backend(config, kind));
#endif
        }
        return run_benchmark(config, std::move(backends), image, out);
    } catch (const std::exception& error) {
        err << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
#endif

}  // namespace apriltag_bench
