#ifndef APRILTAG_DEMO_OPTIONS_H
#define APRILTAG_DEMO_OPTIONS_H

#include <cstdint>
#include <string>

#include "apriltag_scratch.h"

using ApriltagNewFn = void* (*)(uint32_t min_blob_size);
using ApriltagSetScratchFn = int (*)(apriltag_t* detector, uint32_t mode);
using ApriltagFreeFn = void (*)(void* detector);

// Returns 1 when handled, 0 for another option, and -1 when rejected.
int parse_ccl_scratch_option(const std::string& option, bool c_backend,
                             bool& local_ccl_scratch, std::string& error);

const char* ccl_scratch_mode_name(bool local_ccl_scratch);

void* create_configured_detector(uint32_t min_blob_size,
                                 bool local_ccl_scratch,
                                 ApriltagNewFn new_detector,
                                 ApriltagSetScratchFn set_scratch_mode,
                                 ApriltagFreeFn free_detector);

#endif
