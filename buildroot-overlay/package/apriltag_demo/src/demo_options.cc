#include "demo_options.h"

const char* ccl_scratch_mode_name(bool local_ccl_scratch)
{
    return local_ccl_scratch ? "local" : "reusable";
}

void* create_configured_detector(uint32_t min_blob_size,
                                 bool local_ccl_scratch,
                                 ApriltagNewFn new_detector,
                                 ApriltagSetScratchFn set_scratch_mode,
                                 ApriltagFreeFn free_detector)
{
    void* detector = new_detector(min_blob_size);
    if (!detector) {
        return nullptr;
    }
    const uint32_t mode = local_ccl_scratch
                        ? APRILTAG_CCL_SCRATCH_MODE_LOCAL
                        : APRILTAG_CCL_SCRATCH_MODE_REUSABLE;
    if (set_scratch_mode(static_cast<apriltag_t*>(detector), mode) != 0) {
        free_detector(detector);
        return nullptr;
    }
    return detector;
}

int parse_ccl_scratch_option(const std::string& option, bool c_backend,
                             bool& local_ccl_scratch, std::string& error)
{
    if (option != "--local-ccl-scratch") {
        return 0;
    }
    if (c_backend) {
        error = "--local-ccl-scratch is only valid for apriltag_demo; "
                "apriltag_c_demo uses the upstream C detector";
        return -1;
    }
    local_ccl_scratch = true;
    error.clear();
    return 1;
}
