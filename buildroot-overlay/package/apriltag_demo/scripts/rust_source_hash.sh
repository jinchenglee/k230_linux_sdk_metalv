#!/bin/bash
set -euo pipefail

RVV_DIR="${1:?usage: rust_source_hash.sh APRILTAG_RVV_DIR production|workload|profile}"
MODE="${2:?usage: rust_source_hash.sh APRILTAG_RVV_DIR production|workload|profile}"
case "$MODE" in production|workload|profile) ;; *) exit 2 ;; esac

RVV_DIR="$(cd "$RVV_DIR" && pwd -P)"
PARENT="$(cd "$RVV_DIR/.." && pwd)"
{
    printf 'mode\0%s\0' "$MODE"
    for root in "$RVV_DIR" "$PARENT/async-rvv"; do
        [ -d "$root" ] || continue
        while IFS= read -r -d '' file; do
            if [ "$MODE" != profile ]; then
                case "$file" in
                    "$RVV_DIR/src/profile.rs"|"$RVV_DIR/src/pending_profile.rs") continue ;;
                esac
            fi
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
    printf 'kernel-header\0'
    sha256sum "$RVV_DIR/include/apriltag_kernel_modes.h" | cut -d' ' -f1
    printf 'scratch-header\0'
    sha256sum "$RVV_DIR/include/apriltag_scratch.h" | cut -d' ' -f1
    printf 'buffer-telemetry-header\0'
    sha256sum "$RVV_DIR/include/apriltag_buffer_telemetry.h" | cut -d' ' -f1
    if [ "$MODE" = workload ]; then
        printf 'header\0'
        sha256sum "$RVV_DIR/include/apriltag_workload.h" | cut -d' ' -f1
    elif [ "$MODE" = profile ]; then
        printf 'header\0'
        sha256sum "$RVV_DIR/include/apriltag_profile.h" | cut -d' ' -f1
        printf 'pending-profile-header\0'
        sha256sum "$RVV_DIR/include/apriltag_pending_profile.h" | cut -d' ' -f1
    fi
} | sha256sum | cut -d' ' -f1
