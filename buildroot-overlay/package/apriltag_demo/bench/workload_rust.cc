#include "workload_backend.h"

#include "apriltag.h"
#include "rust_apriltag_workload.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace apriltag_bench {
namespace {
static_assert(sizeof(WorkloadCounters) == sizeof(apriltag_workload_counters_t),
              "Rust workload ABI mismatch");
class RustWorkloadBackend final : public WorkloadBackend {
public:
    RustWorkloadBackend(const BenchmarkConfig& config, BackendKind kind)
        : handle_(apriltag_new(config.min_blob)), kind_(kind), factor_(config.factor),
          mode_(kind == BackendKind::RustRvv ? 1 : 0), output_(kMaxDetections)
    {
        if (!handle_) throw std::runtime_error("Rust workload detector construction failed");
        if (config.rvv_mask_explicit &&
            apriltag_set_kernel_mask_v1(handle_, config.rvv_mask) != 0) {
            apriltag_free(handle_);
            handle_ = nullptr;
            throw std::runtime_error("failed to configure Rust workload RVV stage mask");
        }
    }
    ~RustWorkloadBackend() override { apriltag_free(handle_); }
    BackendKind kind() const override { return kind_; }
    WorkloadResult run(const PreparedImage& image) override
    {
        if (image.width > std::numeric_limits<int>::max() ||
            image.height > std::numeric_limits<int>::max() ||
            image.stride > std::numeric_limits<int>::max()) {
            throw std::runtime_error("image geometry exceeds Rust detector limits");
        }
        int count = validate_detection_count(apriltag_detect(
            handle_, image.pixels.data(), image.width, image.height, image.stride,
            factor_, mode_, output_.data(), output_.size()));
        DetectionChecksum checksum(count);
        for (int i=0; i<count; ++i) {
            Detection d; d.id=output_[i].id; d.margin=output_[i].margin;
            for(int j=0;j<2;++j)d.center[j]=output_[i].center[j];
            for(int j=0;j<8;++j)d.corners[j]=output_[i].corners[j];
            checksum.add(d);
        }
        apriltag_workload_counters_t native{};
        if (apriltag_get_workload_counters(handle_, &native) != 1)
            throw std::runtime_error("Rust workload counter retrieval failed");
        WorkloadResult result{kind_, {count, checksum.value()}, {}};
        std::memcpy(&result.counters, &native, sizeof(native));
        return result;
    }
private:
    void* handle_; BackendKind kind_; int factor_; int mode_;
    std::vector<apriltag_det_t> output_;
};
}  // namespace
std::unique_ptr<WorkloadBackend> make_workload_rust_backend(
    const BenchmarkConfig& config, BackendKind kind)
{
    if (kind != BackendKind::RustRvv && kind != BackendKind::RustScalar)
        throw std::invalid_argument("invalid Rust workload backend");
    return std::unique_ptr<WorkloadBackend>(new RustWorkloadBackend(config, kind));
}
}  // namespace apriltag_bench
