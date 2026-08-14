#ifndef APRILTAG_SCRATCH_H
#define APRILTAG_SCRATCH_H

#include <stddef.h>
#include <stdint.h>
#include "apriltag_buffer_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_CCL_SCRATCH_VERSION UINT32_C(1)
#define APRILTAG_CCL_SCRATCH_SIZE 256
#define APRILTAG_CCL_SCRATCH_VALID_BUFFERS (UINT64_C(1) << 0)
#define APRILTAG_CCL_SCRATCH_VALID_ALL APRILTAG_CCL_SCRATCH_VALID_BUFFERS
#define APRILTAG_CCL_SCRATCH_MODE_REUSABLE UINT32_C(0)
#define APRILTAG_CCL_SCRATCH_MODE_LOCAL UINT32_C(1)

typedef void apriltag_t;

/* len is the final logical length. Diagonal lengths therefore describe the
 * final processed row; high_water describes all rows and calls. */
typedef struct apriltag_ccl_scratch_v1 {
    uint32_t version;
    uint32_t struct_size;
    uint64_t validity;
    apriltag_buffer_telemetry_t pending;
    apriltag_buffer_telemetry_t diagonal_left;
    apriltag_buffer_telemetry_t diagonal_right;
    uint64_t reserved[6];
} apriltag_ccl_scratch_v1_t;

#ifdef __cplusplus
#define APRILTAG_SCRATCH_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define APRILTAG_SCRATCH_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif
#define APRILTAG_SCRATCH_ASSERT_OFFSET(type, field, expected) \
    APRILTAG_SCRATCH_STATIC_ASSERT(offsetof(type, field) == (expected), \
                                   "CCL scratch offset mismatch: " #field)

APRILTAG_SCRATCH_STATIC_ASSERT(sizeof(apriltag_ccl_scratch_v1_t) == APRILTAG_CCL_SCRATCH_SIZE,
                               "CCL scratch ABI size mismatch");
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, version, 0);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, struct_size, 4);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, validity, 8);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, pending, 16);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, diagonal_left, 80);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, diagonal_right, 144);
APRILTAG_SCRATCH_ASSERT_OFFSET(apriltag_ccl_scratch_v1_t, reserved, 208);

#undef APRILTAG_SCRATCH_ASSERT_OFFSET
#undef APRILTAG_SCRATCH_STATIC_ASSERT

/* Copies the latest completed detect call's scratch telemetry. Returns 1 on
 * success or -1 for a null argument or non-exact out_size. Externally
 * synchronize all operations on one detector handle; this getter must not run
 * concurrently with detect, another getter, or free on that handle. */
int apriltag_get_ccl_scratch_v1(apriltag_t *handle,
                                 apriltag_ccl_scratch_v1_t *out,
                                 size_t out_size);

/* Selects retained detector-owned scratch (REUSABLE) or fresh per-detect
 * scratch (LOCAL). Returns 0 on success or -1 for a null handle or invalid
 * mode. Switching modes does not clear retained reusable capacity. Externally
 * synchronize this setter with detect, getters, other setters, and free. */
int apriltag_set_ccl_scratch_mode_v1(apriltag_t *handle, uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif
