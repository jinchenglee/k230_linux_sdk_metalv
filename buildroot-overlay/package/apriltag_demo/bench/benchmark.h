#ifndef K230_APRILTAG_BENCHMARK_H
#define K230_APRILTAG_BENCHMARK_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace apriltag_bench {

// Both backends support complete result sets of 0..4095 detections.
constexpr int kMaxDetections = 4096;

enum class BackendKind { RustRvv, CReference, RustScalar };
enum class InputFormat { Auto, Raw, Jpeg };

struct ImageSize {
    bool native = true;
    std::size_t width = 0;
    std::size_t height = 0;

    static ImageSize native_size();
    static ImageSize explicit_size(std::size_t width, std::size_t height);
};

struct BenchmarkConfig {
    std::string input = "fixture.jpg";
    InputFormat format = InputFormat::Auto;
    ImageSize size = ImageSize::native_size();
    std::vector<BackendKind> backends = {
        BackendKind::RustRvv, BackendKind::CReference,
        BackendKind::RustScalar};
    int factor = 2;
    double factor_value = 2.0;
    std::uint32_t min_blob = 25;
    int warmup = 10;
    int iterations = 100;
    int batches = 10;
    std::string dump_dir;
    bool help = false;
};

struct PreparedImage {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t stride = 0;
    std::vector<std::uint8_t> pixels;
};

struct Detection {
    std::uint64_t id = 0;
    double margin = 0;
    double center[2] = {};
    double corners[8] = {};
};

struct DetectionResult {
    int count = 0;
    std::uint64_t checksum = 0;
};

struct VisualDump {
    BackendKind kind;
    std::vector<Detection> detections;
};

struct Statistics {
    std::size_t count = 0;
    double total_ms = 0;
    double min_ms = 0;
    double median_ms = 0;
    double mean_ms = 0;
    double p95_ms = 0;
    double max_ms = 0;
    double stddev_ms = 0;
    double mean_fps = 0;
    double median_fps = 0;
};

class DetectionChecksum {
public:
    explicit DetectionChecksum(std::size_t count);
    void add(const Detection& detection);
    std::uint64_t value() const;

private:
    std::uint64_t hash_;
};

class ArgumentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual BackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual void set_capture_detections(bool capture) = 0;
    virtual DetectionResult detect(const PreparedImage& image) = 0;
    virtual const std::vector<Detection>& detections() const = 0;
};

BenchmarkConfig parse_args(int argc, const char* const argv[]);
void validate_image(std::size_t width, std::size_t height,
                    std::size_t stride, std::size_t storage_size);
PreparedImage load_raw(const std::string& path, ImageSize size);
PreparedImage load_image(const BenchmarkConfig& config);
Statistics compute_stats(const std::vector<std::uint64_t>& samples_ns);
std::uint64_t checksum_bytes(const void* data, std::size_t size);
std::uint64_t checksum_detections(const Detection* detections,
                                  std::size_t count);
int validate_detection_count(int count);
std::vector<std::size_t> batch_order(std::size_t backend_count,
                                     std::size_t batch_index);
std::unique_ptr<Backend> make_rust_backend(const BenchmarkConfig& config,
                                           BackendKind kind);
std::unique_ptr<Backend> make_c_backend(const BenchmarkConfig& config);
int run_benchmark(const BenchmarkConfig& config,
                  std::vector<std::unique_ptr<Backend>> backends,
                  const PreparedImage& image, std::ostream& out);
int benchmark_main(int argc, const char* const argv[], std::ostream& out,
                   std::ostream& err);
void print_usage(std::ostream& out, const char* program);
const char* backend_key(BackendKind kind);
const char* backend_name(BackendKind kind);
void write_visual_dumps(const std::string& directory,
                        const PreparedImage& image,
                        const std::vector<VisualDump>& dumps);

#ifdef APRILTAG_BENCH_NO_OPENCV
inline void write_visual_dumps(const std::string&, const PreparedImage&,
                               const std::vector<VisualDump>&)
{
    throw std::runtime_error("visual dumps are unavailable in this build");
}
#endif

}  // namespace apriltag_bench

#endif
