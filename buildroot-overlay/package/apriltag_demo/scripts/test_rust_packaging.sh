#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKLOAD_HEADER="${APRILTAG_WORKLOAD_HEADER:-$PKG_DIR/lib/rust_apriltag_workload.h}"
PROFILE_HEADER="${APRILTAG_PROFILE_HEADER:-$PKG_DIR/lib/rust_apriltag_profile.h}"
SCRATCH_HEADER="${APRILTAG_SCRATCH_HEADER:-$PKG_DIR/lib/apriltag_scratch.h}"
BUFFER_HEADER="${APRILTAG_BUFFER_HEADER:-$PKG_DIR/lib/apriltag_buffer_telemetry.h}"
PENDING_HEADER="${APRILTAG_PENDING_HEADER:-$PKG_DIR/lib/apriltag_pending_profile.h}"

if [ -e "$PKG_DIR/lib/apriltag_grouping.h" ]; then
    echo "obsolete grouping header is packaged" >&2
    exit 1
fi

validate_workload_header_size() (
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp "$WORKLOAD_HEADER" "$tmp/rust_apriltag_workload.h"
    cat >"$tmp/check_workload_size.c" <<'EOF'
#include "rust_apriltag_workload.h"
_Static_assert(sizeof(apriltag_workload_counters_t) == 480,
               "schema-v1 workload ABI must be exactly 480 bytes");
EOF
    "${CC:-cc}" -std=c11 -I"$tmp" -c "$tmp/check_workload_size.c" \
        -o "$tmp/check_workload_size.o"
)

validate_workload_header_size
validate_profile_header_size() (
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp "$PROFILE_HEADER" "$tmp/rust_apriltag_profile.h"
    cat >"$tmp/check_profile_size.c" <<'EOF'
#include "rust_apriltag_profile.h"
_Static_assert(sizeof(apriltag_ccl_profile_t) == 768,
               "profile ABI must be exactly 768 bytes");
EOF
    cat >"$tmp/check_profile_size.cpp" <<'EOF'
#include "rust_apriltag_profile.h"
static_assert(sizeof(apriltag_ccl_profile_t) == 768,
              "profile ABI must be exactly 768 bytes");
EOF
    "${CC:-cc}" -std=c11 -I"$tmp" -c "$tmp/check_profile_size.c" \
        -o "$tmp/check_profile_size.o"
    "${CXX:-c++}" -std=c++17 -I"$tmp" -c "$tmp/check_profile_size.cpp" \
        -o "$tmp/check_profile_size_cpp.o"
)

validate_profile_header_size
validate_pending_header_size() (
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp "$PENDING_HEADER" "$tmp/apriltag_pending_profile.h"
    cat >"$tmp/check_pending_size.c" <<'EOF'
#include "apriltag_pending_profile.h"
_Static_assert(sizeof(apriltag_ccl_pending_profile_v1_t) == 640,
               "pending profile ABI must be exactly 640 bytes");
EOF
    "${CC:-cc}" -std=c11 -I"$tmp" -c "$tmp/check_pending_size.c" \
        -o "$tmp/check_pending_size.o"
)
validate_pending_header_size
validate_scratch_header_layout() (
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp "$SCRATCH_HEADER" "$tmp/apriltag_scratch.h"
    cp "$BUFFER_HEADER" "$tmp/apriltag_buffer_telemetry.h"
    cat >"$tmp/check_scratch.cpp" <<'EOF'
#include "apriltag_scratch.h"
#include <cstddef>
static_assert(sizeof(apriltag_buffer_telemetry_t) == 64);
static_assert(sizeof(apriltag_ccl_scratch_v1_t) == 256);
static_assert(offsetof(apriltag_ccl_scratch_v1_t, pending) == 16);
static_assert(offsetof(apriltag_ccl_scratch_v1_t, diagonal_left) == 80);
static_assert(offsetof(apriltag_ccl_scratch_v1_t, diagonal_right) == 144);
static_assert(offsetof(apriltag_ccl_scratch_v1_t, reserved) == 208);
static_assert(APRILTAG_CCL_SCRATCH_MODE_REUSABLE == UINT32_C(0));
static_assert(APRILTAG_CCL_SCRATCH_MODE_LOCAL == UINT32_C(1));
EOF
    "${CXX:-c++}" -std=c++17 -I"$tmp" -c "$tmp/check_scratch.cpp" -o "$tmp/check.o"
)
validate_scratch_header_layout
if [ "${APRILTAG_PACKAGING_HEADER_ONLY:-0}" = 1 ]; then
    exit 0
fi

if [ -n "${APRILTAG_RVV_DIR:-}" ]; then
    RVV_DIR="$APRILTAG_RVV_DIR"
else
    REPO_ROOT="$(git -C "$PKG_DIR" rev-parse --show-toplevel)"
    RVV_DIR="$(dirname "$REPO_ROOT")/apriltag-rvv"
fi

hash_source() {
    "$PKG_DIR/scripts/rust_source_hash.sh" "$RVV_DIR" "$1"
}

SOURCE_TEST_LOCK="$(dirname "$RVV_DIR")/.apriltag-rvv-source-hash-test.lock"
exec 8>"$SOURCE_TEST_LOCK"
flock 8
production_before="$(hash_source production)"
workload_before="$(hash_source workload)"
profile_before="$(hash_source profile)"
probe="$(mktemp "$RVV_DIR/src/.packaging-hash-test.XXXXXX.rs")"
trap 'rm -f "$probe"' EXIT
printf '%s\n' '// packaging hash mutation probe' >"$probe"
production_after="$(hash_source production)"
workload_after="$(hash_source workload)"
profile_after="$(hash_source profile)"
test "$production_before" != "$production_after"
test "$workload_before" != "$workload_after"
test "$profile_before" != "$profile_after"
rm -f "$probe"
trap - EXIT

test "$(cat "$PKG_DIR/lib/.apriltag_rvv.source-hash")" = "$production_before"
test "$(cat "$PKG_DIR/lib/.apriltag_rvv_workload.source-hash")" = "$workload_before"
test "$(cat "$PKG_DIR/lib/.apriltag_rvv_profile.source-hash")" = "$profile_before"
production_symbols="$(nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a")"
workload_symbols="$(nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_workload.a")"
profile_symbols="$(nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_profile.a")"
has_exact_symbol() {
    grep -Eq " [A-Za-z] $1$| $1$" <<<"$2"
}
grep -Eq ' apriltag_detect$' <<<"$production_symbols"
grep -Eq ' apriltag_set_ccl_scratch_mode_v1$' <<<"$production_symbols"
grep -Eq ' apriltag_get_workload_counters$' <<<"$workload_symbols"
grep -Eq ' apriltag_set_ccl_scratch_mode_v1$' <<<"$workload_symbols"
grep -Eq ' apriltag_get_ccl_profile_v1$' <<<"$profile_symbols"
grep -Eq ' apriltag_get_ccl_scratch_v1$' <<<"$profile_symbols"
has_exact_symbol apriltag_get_ccl_pending_profile_v1 "$profile_symbols"
grep -Eq ' apriltag_set_ccl_scratch_mode_v1$' <<<"$profile_symbols"
for symbols in "$production_symbols" "$workload_symbols" "$profile_symbols"; do
    if grep -Eq ' apriltag_(get_ccl_grouping_profile_v1|set_ccl_grouping_mode_v1)$' <<<"$symbols"; then
        exit 1
    fi
done
if grep -Eq ' apriltag_get_ccl_profile_v1$' <<<"$production_symbols"; then
    exit 1
fi
if grep -Eq ' apriltag_get_ccl_scratch_v1$' <<<"$production_symbols"; then
    exit 1
fi
if has_exact_symbol apriltag_get_ccl_pending_profile_v1 \
    "$production_symbols"$'\n'"$workload_symbols"; then
    exit 1
fi
if grep -Eq ' apriltag_get_workload_counters$' <<<"$profile_symbols"; then
    exit 1
fi
if grep -Eq ' apriltag_get_workload_counters_v2$' <<<"$workload_symbols"; then
    exit 1
fi
grep -Eq '^#define[[:space:]]+APRILTAG_WORKLOAD_SCHEMA_VERSION[[:space:]]+UINT32_C\(1\)[[:space:]]*$' \
    "$WORKLOAD_HEADER"
grep -Eq '^#define[[:space:]]+APRILTAG_CCL_PROFILE_SIZE[[:space:]]+768[[:space:]]*$' \
    "$PROFILE_HEADER"
