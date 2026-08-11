#ifndef APRILTAG_KERNEL_MODES_H
#define APRILTAG_KERNEL_MODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_KERNEL_DECIMATE (UINT64_C(1) << 0)
#define APRILTAG_KERNEL_THRESHOLD (UINT64_C(1) << 1)
#define APRILTAG_KERNEL_RLE (UINT64_C(1) << 2)
#define APRILTAG_KERNEL_LFPS_TUNED (UINT64_C(1) << 3)
#define APRILTAG_KERNEL_GAUSSIAN (UINT64_C(1) << 4)
#define APRILTAG_KERNEL_GRAY_MODEL (UINT64_C(1) << 5)
#define APRILTAG_KERNEL_ALL ((UINT64_C(1) << 6) - 1)

/*
 * Set a persistent per-stage kernel override. This override takes precedence
 * over the legacy uniform mode argument to apriltag_detect. A mask of 0 means
 * all scalar and does not clear the override.
 *
 * The caller must externally synchronize all operations on a single detector
 * handle. apriltag_detect, this setter, and apriltag_free must not run
 * concurrently for the same handle.
 *
 * Returns 0 on success or -1 for a null handle or unknown mask bits.
 */
int apriltag_set_kernel_mask_v1(void *handle, uint64_t rvv_mask);

#ifdef __cplusplus
}
#endif

#endif
