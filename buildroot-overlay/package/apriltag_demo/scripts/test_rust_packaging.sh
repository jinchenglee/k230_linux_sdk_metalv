#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKLOAD_HEADER="${APRILTAG_WORKLOAD_HEADER:-$PKG_DIR/lib/rust_apriltag_workload.h}"

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

production_before="$(hash_source production)"
workload_before="$(hash_source workload)"
probe="$RVV_DIR/src/.packaging-hash-test.rs"
trap 'rm -f "$probe"' EXIT
printf '%s\n' '// packaging hash mutation probe' >"$probe"
production_after="$(hash_source production)"
workload_after="$(hash_source workload)"
test "$production_before" != "$production_after"
test "$workload_before" != "$workload_after"
rm -f "$probe"
trap - EXIT

test "$(cat "$PKG_DIR/lib/.apriltag_rvv.source-hash")" = "$production_before"
test "$(cat "$PKG_DIR/lib/.apriltag_rvv_workload.source-hash")" = "$workload_before"
production_symbols="$(nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a")"
workload_symbols="$(nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_workload.a")"
grep -Eq ' apriltag_detect$' <<<"$production_symbols"
grep -Eq ' apriltag_get_workload_counters$' <<<"$workload_symbols"
if grep -Eq ' apriltag_get_workload_counters_v2$' <<<"$workload_symbols"; then
    exit 1
fi
grep -Eq '^#define[[:space:]]+APRILTAG_WORKLOAD_SCHEMA_VERSION[[:space:]]+UINT32_C\(1\)[[:space:]]*$' \
    "$WORKLOAD_HEADER"
