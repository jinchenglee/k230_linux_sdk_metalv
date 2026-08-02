#include "workload_backend.h"

#include <apriltag/apriltag.h>
#include <apriltag/c_apriltag_workload.h>

#include <climits>
#include <cstring>
#include <stdexcept>

namespace apriltag_bench {
namespace {
static_assert(sizeof(WorkloadCounters) == sizeof(apriltag_workload_counters_t),
              "C workload ABI mismatch");
class CWorkloadBackend final : public WorkloadBackend {
public:
    explicit CWorkloadBackend(const BenchmarkConfig& config)
        : detector_(c_apriltag_detector_create()), family_(c_tag36h11_create())
    {
        if (!detector_ || !family_) { cleanup(); throw std::runtime_error("C workload detector construction failed"); }
        detector_->nthreads=1; detector_->quad_decimate=config.factor_value;
        detector_->qtp.min_cluster_pixels=static_cast<int>(config.min_blob);
        detector_->refine_edges=false; detector_->decode_sharpening=0;
        c_apriltag_detector_add_family_bits(detector_, family_, 0);
    }
    ~CWorkloadBackend() override { cleanup(); }
    BackendKind kind() const override { return BackendKind::CReference; }
    WorkloadResult run(const PreparedImage& image) override
    {
        if (image.width > INT_MAX || image.height > INT_MAX || image.stride > INT_MAX)
            throw std::runtime_error("image geometry exceeds C detector limits");
        image_u8_t view{static_cast<int32_t>(image.width),static_cast<int32_t>(image.height),
                        static_cast<int32_t>(image.stride),const_cast<uint8_t*>(image.pixels.data())};
        zarray_t* detections=c_apriltag_detector_detect(detector_,&view);
        if(!detections) throw std::runtime_error("C workload detect failed");
        int count=validate_detection_count(zarray_size(detections)); DetectionChecksum checksum(count);
        for(int i=0;i<count;++i){ apriltag_detection_t* d=nullptr; zarray_get(detections,i,&d);
            Detection n; n.id=d->id; n.margin=d->decision_margin; n.center[0]=d->c[0]; n.center[1]=d->c[1];
            for(int j=0;j<4;++j){n.corners[j*2]=d->p[j][0];n.corners[j*2+1]=d->p[j][1];} checksum.add(n); }
        c_apriltag_detections_destroy(detections);
        apriltag_workload_counters_t native{};
        if(c_apriltag_get_workload_counters_v2(detector_,&native,sizeof(native))!=0) throw std::runtime_error("C workload counter retrieval failed");
        WorkloadResult result{kind(),{count,checksum.value()},{}}; std::memcpy(&result.counters,&native,sizeof(native)); return result;
    }
private:
    void cleanup(){if(detector_)c_apriltag_detector_destroy(detector_);if(family_)c_tag36h11_destroy(family_);detector_=nullptr;family_=nullptr;}
    apriltag_detector_t* detector_; apriltag_family_t* family_;
};
}  // namespace
std::unique_ptr<WorkloadBackend> make_workload_c_backend(const BenchmarkConfig& config)
{ return std::unique_ptr<WorkloadBackend>(new CWorkloadBackend(config)); }
}  // namespace apriltag_bench
