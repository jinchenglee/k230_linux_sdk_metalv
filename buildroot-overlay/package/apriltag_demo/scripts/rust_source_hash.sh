#!/bin/bash
set -euo pipefail

USAGE="usage: rust_source_hash.sh APRILTAG_RVV_DIR production|workload|profile [rvv|scalar]"
RVV_DIR="${1:?$USAGE}"
MODE="${2:?$USAGE}"
# The ISA variant selects a different archive from the same sources (see
# build-capi.sh --no-rvv), published under the same name and the same stamp
# file. It must therefore be part of the digest, or switching
# BR2_RISCV_ISA_RVV would leave a matching stamp and reuse the wrong archive.
VARIANT="${3:-rvv}"
case "$MODE" in production|workload|profile) ;; *) exit 2 ;; esac
case "$VARIANT" in rvv|scalar) ;; *) exit 2 ;; esac

RVV_DIR="$(cd "$RVV_DIR" && pwd -P)"
PARENT="$(cd "$RVV_DIR/.." && pwd)"
{
    printf 'mode\0%s\0' "$MODE"
    printf 'variant\0%s\0' "$VARIANT"
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
