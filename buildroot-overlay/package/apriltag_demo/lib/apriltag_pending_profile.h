#ifndef APRILTAG_PENDING_PROFILE_H
#define APRILTAG_PENDING_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_CCL_PENDING_PROFILE_VERSION UINT32_C(1)
#define APRILTAG_CCL_PENDING_PROFILE_SIZE 640
#define APRILTAG_CCL_PENDING_BOUNDARY_TYPES 4
#define APRILTAG_CCL_PENDING_RANGE_BINS 8
#define APRILTAG_CCL_PENDING_SAMPLE_STRIDE UINT32_C(64)

#define APRILTAG_CCL_PENDING_PROFILE_VALID_STRUCTURE (UINT64_C(1) << 0)
#define APRILTAG_CCL_PENDING_PROFILE_VALID_SAMPLED_TIMINGS (UINT64_C(1) << 1)
#define APRILTAG_CCL_PENDING_PROFILE_VALID_ALL ((UINT64_C(1) << 2) - 1)

typedef void apriltag_t;

/* Boundary types are horizontal, vertical, down-left, and down-right. Range
 * bins represent lengths 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, and 65+. */
typedef struct apriltag_ccl_pending_profile_v1 {
    uint32_t version;
    uint32_t struct_size;
    uint64_t validity;
    uint32_t sample_stride;
    uint32_t reserved_u32;
    uint64_t sampled_records;
    uint64_t sampled_units;
    uint64_t timer_intervals;
    uint64_t timer_overhead_ns;
    uint64_t construct_sample_ns;
    uint64_t resolve_sample_ns;
    uint64_t lookup_sample_ns;
    uint64_t emit_sample_ns;
    uint64_t pending_records_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t pending_units_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t accepted_records_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t accepted_units_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t root_equal_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t small_component_by_type[APRILTAG_CCL_PENDING_BOUNDARY_TYPES];
    uint64_t range_histogram[APRILTAG_CCL_PENDING_BOUNDARY_TYPES]
                            [APRILTAG_CCL_PENDING_RANGE_BINS];
    uint64_t sampled_accepted_records;
    uint64_t reserved[12];
} apriltag_ccl_pending_profile_v1_t;

#ifdef __cplusplus
#define APRILTAG_CCL_PENDING_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define APRILTAG_CCL_PENDING_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif
#define APRILTAG_CCL_PENDING_ASSERT_OFFSET(field, expected) \
    APRILTAG_CCL_PENDING_STATIC_ASSERT(offsetof(apriltag_ccl_pending_profile_v1_t, field) == \
                                           (expected), \
                                       "CCL pending profile offset mismatch: " #field)

APRILTAG_CCL_PENDING_STATIC_ASSERT(
    sizeof(apriltag_ccl_pending_profile_v1_t) == APRILTAG_CCL_PENDING_PROFILE_SIZE,
    "CCL pending profile ABI size mismatch");
APRILTAG_CCL_PENDING_ASSERT_OFFSET(version, 0);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(struct_size, 4);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(validity, 8);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(sample_stride, 16);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(sampled_records, 24);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(sampled_units, 32);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(timer_intervals, 40);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(timer_overhead_ns, 48);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(construct_sample_ns, 56);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(resolve_sample_ns, 64);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(lookup_sample_ns, 72);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(emit_sample_ns, 80);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(pending_records_by_type, 88);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(pending_units_by_type, 120);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(accepted_records_by_type, 152);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(accepted_units_by_type, 184);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(root_equal_by_type, 216);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(small_component_by_type, 248);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(range_histogram, 280);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(sampled_accepted_records, 536);
APRILTAG_CCL_PENDING_ASSERT_OFFSET(reserved, 544);

#undef APRILTAG_CCL_PENDING_ASSERT_OFFSET
#undef APRILTAG_CCL_PENDING_STATIC_ASSERT

/* Copies the latest completed detect call's profile. Sampled times are raw
 * diagnostic intervals: analyzers must subtract timer_overhead_ns, and must
 * never treat them as authoritative production latency. Externally synchronize
 * all operations on one detector handle; this getter must not race detect or
 * free. The getter implementation is supplied by the profile-enabled library. */
int apriltag_get_ccl_pending_profile_v1(apriltag_t *handle,
                                        apriltag_ccl_pending_profile_v1_t *out,
                                        size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
