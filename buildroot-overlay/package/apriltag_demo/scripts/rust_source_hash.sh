#!/bin/bash
set -euo pipefail

RVV_DIR="${1:?usage: rust_source_hash.sh APRILTAG_RVV_DIR production|workload}"
MODE="${2:?usage: rust_source_hash.sh APRILTAG_RVV_DIR production|workload}"
case "$MODE" in production|workload) ;; *) exit 2 ;; esac

PARENT="$(cd "$RVV_DIR/.." && pwd)"
{
    printf 'mode\0%s\0' "$MODE"
    for root in "$RVV_DIR" "$PARENT/async-rvv"; do
        [ -d "$root" ] || continue
        while IFS= read -r -d '' file; do
            relative="${file#"$PARENT/"}"
            printf '%s\0' "$relative"
            sha256sum "$file" | cut -d' ' -f1
        done < <(find "$root" -type f \
            \( -name '*.rs' -o -name 'Cargo.toml' -o -name 'Cargo.lock' \
               -o -name 'build.rs' -o -name 'rust-toolchain' \
               -o -name 'rust-toolchain.toml' -o -path '*/.cargo/config' \
               -o -path '*/.cargo/config.toml' -o -path '*/scripts/build-capi.sh' \) \
            -not -path '*/target/*' -print0 | LC_ALL=C sort -z)
    done
    if [ "$MODE" = workload ]; then
        printf 'header\0'
        sha256sum "$RVV_DIR/include/apriltag_workload.h" | cut -d' ' -f1
    fi
} | sha256sum | cut -d' ' -f1
