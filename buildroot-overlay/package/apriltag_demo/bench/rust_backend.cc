#include "benchmark.h"

#include "apriltag.h"

#include <limits>
#include <stdexcept>
#include <vector>

namespace apriltag_bench {
namespace {

class RustBackend final : public Backend {
public:
    RustBackend(const BenchmarkConfig& config, BackendKind kind)
        : handle_(apriltag_new(config.min_blob)), kind_(kind),
          factor_(config.factor), mode_(kind == BackendKind::RustRvv ? 1 : 0),
          output_(kMaxDetections), normalized_(kMaxDetections)
    {
        if (!handle_) {
            throw std::runtime_error(std::string(backend_name(kind_)) +
                                     " detector construction failed");
        }
    }

    ~RustBackend() override { apriltag_free(handle_); }
    RustBackend(const RustBackend&) = delete;
    RustBackend& operator=(const RustBackend&) = delete;

    BackendKind kind() const override { return kind_; }
    const char* name() const override { return backend_name(kind_); }

    DetectionResult detect(const PreparedImage& image) override
    {
        if (image.width > std::numeric_limits<int>::max() ||
            image.height > std::numeric_limits<int>::max() ||
            image.stride > std::numeric_limits<int>::max()) {
            throw std::runtime_error("image geometry exceeds detector limits");
        }
        const int count = validate_detection_count(apriltag_detect(
            handle_, image.pixels.data(), image.width, image.height, image.stride,
            factor_, mode_, output_.data(), static_cast<int>(output_.size())));
        for (int i = 0; i < count; ++i) {
            normalized_[i].id = output_[i].id;
            normalized_[i].margin = output_[i].margin;
            for (int j = 0; j < 2; ++j) normalized_[i].center[j] = output_[i].center[j];
            for (int j = 0; j < 8; ++j) normalized_[i].corners[j] = output_[i].corners[j];
        }
        return {count, checksum_detections(normalized_.data(),
                                            static_cast<std::size_t>(count))};
    }

private:
    void* handle_;
    BackendKind kind_;
    int factor_;
    int mode_;
    std::vector<apriltag_det_t> output_;
    std::vector<Detection> normalized_;
};

}  // namespace

std::unique_ptr<Backend> make_rust_backend(const BenchmarkConfig& config,
                                           BackendKind kind)
{
    if (kind != BackendKind::RustRvv && kind != BackendKind::RustScalar) {
        throw std::invalid_argument("invalid Rust backend kind");
    }
    return std::unique_ptr<Backend>(new RustBackend(config, kind));
}

}  // namespace apriltag_bench
