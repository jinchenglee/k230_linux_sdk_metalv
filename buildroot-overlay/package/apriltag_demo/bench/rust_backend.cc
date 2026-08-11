#include "benchmark.h"

#include "apriltag.h"

#include <limits>
#include <stdexcept>
#include <vector>

#ifdef APRILTAG_BENCH_PROFILE
#include "rust_apriltag_profile.h"
#endif

namespace apriltag_bench {
namespace {

class RustBackend final : public Backend {
public:
    RustBackend(const BenchmarkConfig& config, BackendKind kind)
        : handle_(apriltag_new(config.min_blob)), kind_(kind),
          factor_(config.factor), mode_(kind == BackendKind::RustRvv ? 1 : 0),
          output_(kMaxDetections)
    {
        if (!handle_) {
            throw std::runtime_error(std::string(backend_name(kind_)) +
                                     " detector construction failed");
        }
        if (config.rvv_mask_explicit &&
            apriltag_set_kernel_mask_v1(handle_, config.rvv_mask) != 0) {
            apriltag_free(handle_);
            handle_ = nullptr;
            throw std::runtime_error("failed to configure Rust RVV stage mask");
        }
        normalized_.reserve(kMaxDetections);
    }

    ~RustBackend() override { apriltag_free(handle_); }
    RustBackend(const RustBackend&) = delete;
    RustBackend& operator=(const RustBackend&) = delete;

    BackendKind kind() const override { return kind_; }
    const char* name() const override { return backend_name(kind_); }
    void set_capture_detections(bool capture) override { capture_ = capture; }
    const std::vector<Detection>& detections() const override
    {
        return normalized_;
    }

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
        if (capture_) normalized_.resize(static_cast<std::size_t>(count));
        DetectionChecksum checksum(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            Detection detection;
            detection.id = output_[i].id;
            detection.margin = output_[i].margin;
            for (int j = 0; j < 2; ++j) detection.center[j] = output_[i].center[j];
            for (int j = 0; j < 8; ++j) detection.corners[j] = output_[i].corners[j];
            checksum.add(detection);
            if (capture_) normalized_[i] = detection;
        }
        return {count, checksum.value()};
    }

#ifdef APRILTAG_BENCH_PROFILE
    bool consume_profile(apriltag_ccl_profile_t& profile) override
    {
        const int result = apriltag_get_ccl_profile_v1(
            static_cast<apriltag_t*>(handle_), &profile, sizeof(profile));
        if (result != 1) throw std::runtime_error("CCL profile getter failed");
        return true;
    }
#endif

private:
    void* handle_;
    BackendKind kind_;
    int factor_;
    int mode_;
    bool capture_ = false;
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
