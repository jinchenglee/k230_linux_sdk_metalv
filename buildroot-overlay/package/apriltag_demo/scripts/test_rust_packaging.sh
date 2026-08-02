#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a" | grep -q ' apriltag_configure_recovery$'
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a" | grep -q ' apriltag_get_recovery_stats$'
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a" | grep -q ' apriltag_get_recovery_candidates$'
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_workload.a" | grep -q ' apriltag_get_workload_counters_v2$'
