#ifndef APRILTAG_PROFILE_H
#define APRILTAG_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRILTAG_CCL_PROFILE_VERSION UINT32_C(1)
#define APRILTAG_CCL_PROFILE_SIZE 768

#define APRILTAG_CCL_DIRECTION_HORIZONTAL 0
#define APRILTAG_CCL_DIRECTION_VERTICAL 1
#define APRILTAG_CCL_DIRECTION_DIAGONAL_LEFT 2
#define APRILTAG_CCL_DIRECTION_DIAGONAL_RIGHT 3
#define APRILTAG_CCL_DIRECTION_COUNT 4
#define APRILTAG_CCL_BOUNDARY_TYPE_COUNT 4
#define APRILTAG_CCL_UF_CALL_SITE_COUNT 7
#define APRILTAG_CCL_UF_HOP_BIN_COUNT 5

#define APRILTAG_CCL_PROFILE_VALID_TIMINGS (UINT64_C(1) << 0)
#define APRILTAG_CCL_PROFILE_VALID_RUN_DISTRIBUTION (UINT64_C(1) << 1)
#define APRILTAG_CCL_PROFILE_VALID_OVERLAPS (UINT64_C(1) << 2)
#define APRILTAG_CCL_PROFILE_VALID_UNIONS (UINT64_C(1) << 3)
#define APRILTAG_CCL_PROFILE_VALID_BOUNDARIES (UINT64_C(1) << 4)
#define APRILTAG_CCL_PROFILE_VALID_UF_HOPS (UINT64_C(1) << 5)
#define APRILTAG_CCL_PROFILE_VALID_GROUPING (UINT64_C(1) << 6)
#define APRILTAG_CCL_PROFILE_VALID_GROWTH (UINT64_C(1) << 7)
#define APRILTAG_CCL_PROFILE_VALID_DIAGNOSTICS (UINT64_C(1) << 8)
#define APRILTAG_CCL_PROFILE_VALID_RESOLVE_FILTER_TIMING (UINT64_C(1) << 9)
#define APRILTAG_CCL_PROFILE_VALID_KNOWN ((UINT64_C(1) << 10) - 1)
#define APRILTAG_CCL_PROFILE_VALID_ALL \
    (APRILTAG_CCL_PROFILE_VALID_KNOWN & \
     ~APRILTAG_CCL_PROFILE_VALID_RESOLVE_FILTER_TIMING)

/* resolve_filter_ns is zero unless RESOLVE_FILTER_TIMING is valid. Version 1
 * preserves the production single-pass resolve/group/emit traversal and
 * reports that combined interval in group_emit_ns. diagnostic_ns contains the
 * post-group duplicate scan and is included in total_ns and unattributed
 * conservation as its own phase. */

typedef void apriltag_t;

typedef struct apriltag_ccl_profile {
    uint32_t version;
    uint32_t struct_size;
    uint64_t validity;
    uint64_t total_ns;
    uint64_t rle_ns;
    uint64_t repack_label_ns;
    uint64_t uf_init_ns;
    uint64_t horizontal_ns;
    uint64_t vertical_ns;
    uint64_t diagonal_left_ns;
    uint64_t diagonal_right_ns;
    uint64_t root_materialize_ns;
    uint64_t resolve_filter_ns;
    uint64_t group_emit_ns;
    uint64_t conversion_ns;
    uint64_t diagnostic_ns;
    uint64_t unattributed_ns;
    uint64_t runs;
    uint64_t row_runs_min;
    uint64_t row_runs_max;
    uint64_t row_runs_sum;
    /* Direction order: horizontal, vertical, down-left, down-right. */
    uint64_t overlap_comparisons[APRILTAG_CCL_DIRECTION_COUNT];
    uint64_t accepted_overlaps[APRILTAG_CCL_DIRECTION_COUNT];
    uint64_t same_color_edges_by_direction[APRILTAG_CCL_DIRECTION_COUNT];
    uint64_t union_attempts_by_direction[APRILTAG_CCL_DIRECTION_COUNT];
    uint64_t pending_by_type[APRILTAG_CCL_BOUNDARY_TYPE_COUNT];
    uint64_t pending_expanded_points_by_type[APRILTAG_CCL_BOUNDARY_TYPE_COUNT];
    uint64_t emitted_by_type[APRILTAG_CCL_BOUNDARY_TYPE_COUNT];
    uint64_t connected_last_suppressions;
    uint64_t root_equal_rejects;
    uint64_t small_component_rejects;
    uint64_t accepted_grouping_records;
    uint64_t distinct_keys;
    uint64_t exact_duplicates;
    uint64_t coordinate_duplicates;
    uint64_t pending_growths;
    uint64_t cluster_map_growths;
    uint64_t cluster_vector_growths;
    /* Bins are 0, 1, 2, 3, and 4-or-more path-halving hops. */
    uint64_t uf_hops[APRILTAG_CCL_UF_CALL_SITE_COUNT][APRILTAG_CCL_UF_HOP_BIN_COUNT];
    uint64_t reserved[3];
} apriltag_ccl_profile_t;

#ifdef __cplusplus
#define APRILTAG_CCL_PROFILE_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define APRILTAG_CCL_PROFILE_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif
#define APRILTAG_CCL_PROFILE_ASSERT_OFFSET(field, expected) \
    APRILTAG_CCL_PROFILE_STATIC_ASSERT(offsetof(apriltag_ccl_profile_t, field) == (expected), \
                                       "CCL profile offset mismatch: " #field)

APRILTAG_CCL_PROFILE_STATIC_ASSERT(sizeof(apriltag_ccl_profile_t) == APRILTAG_CCL_PROFILE_SIZE,
                                   "CCL profile ABI size mismatch");
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(version, 0);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(struct_size, 4);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(validity, 8);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(total_ns, 16);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(rle_ns, 24);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(repack_label_ns, 32);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(uf_init_ns, 40);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(horizontal_ns, 48);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(vertical_ns, 56);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(diagonal_left_ns, 64);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(diagonal_right_ns, 72);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(root_materialize_ns, 80);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(resolve_filter_ns, 88);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(group_emit_ns, 96);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(conversion_ns, 104);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(diagnostic_ns, 112);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(unattributed_ns, 120);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(runs, 128);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(row_runs_min, 136);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(row_runs_max, 144);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(row_runs_sum, 152);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(overlap_comparisons, 160);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(accepted_overlaps, 192);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(same_color_edges_by_direction, 224);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(union_attempts_by_direction, 256);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(pending_by_type, 288);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(pending_expanded_points_by_type, 320);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(emitted_by_type, 352);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(connected_last_suppressions, 384);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(root_equal_rejects, 392);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(small_component_rejects, 400);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(accepted_grouping_records, 408);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(distinct_keys, 416);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(exact_duplicates, 424);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(coordinate_duplicates, 432);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(pending_growths, 440);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(cluster_map_growths, 448);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(cluster_vector_growths, 456);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(uf_hops, 464);
APRILTAG_CCL_PROFILE_ASSERT_OFFSET(reserved, 744);

#undef APRILTAG_CCL_PROFILE_ASSERT_OFFSET
#undef APRILTAG_CCL_PROFILE_STATIC_ASSERT

/*
 * Copies the latest completed detect call's profile. Returns 1 on success or
 * -1 for a null argument or non-exact out_size. The snapshot remains valid
 * until the next detect call. Externally synchronize all operations on one
 * detector handle; this getter must not race detect or free.
 */
int apriltag_get_ccl_profile_v1(apriltag_t *handle,
                               apriltag_ccl_profile_t *out,
                               size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
