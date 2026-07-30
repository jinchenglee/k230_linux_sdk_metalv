#include "benchmark.h"

#include <apriltag/apriltag.h>
#include <apriltag/tag36h11.h>

#include <climits>
#include <stdexcept>
#include <vector>

namespace apriltag_bench {
namespace {

class CBackend final : public Backend {
public:
    explicit CBackend(const BenchmarkConfig& config)
        : detector_(apriltag_detector_create()), family_(tag36h11_create())
    {
        if (!detector_ || !family_) {
            cleanup();
            throw std::runtime_error("C reference detector construction failed");
        }
        detector_->nthreads = 1;
        detector_->quad_decimate = static_cast<float>(config.factor_value);
        if (config.min_blob > static_cast<std::uint32_t>(INT_MAX)) {
            cleanup();
            throw std::invalid_argument("min_blob exceeds INT_MAX");
        }
        detector_->qtp.min_cluster_pixels = static_cast<int>(config.min_blob);
        detector_->refine_edges = false;
        detector_->decode_sharpening = 0.0;
        apriltag_detector_add_family_bits(detector_, family_, 0);
        normalized_.reserve(kMaxDetections);
    }

    ~CBackend() override { cleanup(); }
    CBackend(const CBackend&) = delete;
    CBackend& operator=(const CBackend&) = delete;

    BackendKind kind() const override { return BackendKind::CReference; }
    const char* name() const override { return backend_name(kind()); }
    void set_capture_detections(bool capture) override { capture_ = capture; }
    const std::vector<Detection>& detections() const override
    {
        return normalized_;
    }

    DetectionResult detect(const PreparedImage& image) override
    {
        if (image.width > INT_MAX || image.height > INT_MAX || image.stride > INT_MAX) {
            throw std::runtime_error("image geometry exceeds C detector limits");
        }
        image_u8_t view = {
            static_cast<int32_t>(image.width),
            static_cast<int32_t>(image.height),
            static_cast<int32_t>(image.stride),
            const_cast<std::uint8_t*>(image.pixels.data()),
        };
        zarray_t* detections = apriltag_detector_detect(detector_, &view);
        if (!detections) throw std::runtime_error("C reference detect failed");
        const int count = zarray_size(detections);
        if (count >= kMaxDetections) {
            apriltag_detections_destroy(detections);
            (void)validate_detection_count(count);
        }
        if (capture_) normalized_.clear();
        DetectionChecksum checksum(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            apriltag_detection_t* detection = nullptr;
            zarray_get(detections, i, &detection);
            Detection normalized;
            normalized.id = static_cast<std::uint64_t>(detection->id);
            normalized.margin = detection->decision_margin;
            normalized.center[0] = detection->c[0];
            normalized.center[1] = detection->c[1];
            for (int corner = 0; corner < 4; ++corner) {
                normalized.corners[corner * 2] = detection->p[corner][0];
                normalized.corners[corner * 2 + 1] = detection->p[corner][1];
            }
            checksum.add(normalized);
            if (capture_) normalized_.push_back(normalized);
        }
        apriltag_detections_destroy(detections);
        return {count, checksum.value()};
    }

private:
    void cleanup()
    {
        if (detector_) apriltag_detector_destroy(detector_);
        if (family_) tag36h11_destroy(family_);
        detector_ = nullptr;
        family_ = nullptr;
    }

    apriltag_detector_t* detector_;
    apriltag_family_t* family_;
    bool capture_ = false;
    std::vector<Detection> normalized_;
};

}  // namespace

std::unique_ptr<Backend> make_c_backend(const BenchmarkConfig& config)
{
    return std::unique_ptr<Backend>(new CBackend(config));
}

}  // namespace apriltag_bench
