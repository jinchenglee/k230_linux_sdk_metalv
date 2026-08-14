#!/bin/bash
# Build libapriltag_rvv.a (apriltag-rvv crate, feature "capi") for riscv64
# Linux inside the rvv-dev docker image, and copy it into this package's lib/.
#
# The Rust toolchain (nightly + riscv64gc-unknown-linux-gnu + RVV) lives in the
# rvv-dev:latest image, not in the K230 SDK, so the staticlib is built there and
# consumed as a prebuilt artifact by the C++ CMake build.
#
# Usage:
#   scripts/build_rust_lib.sh            # with RVV (+v, default)
#   scripts/build_rust_lib.sh --no-rvv   # scalar fallback
#   scripts/build_rust_lib.sh --workload-only # separate instrumented archive
#   scripts/build_rust_lib.sh --profile-only  # separate profiling archive
#
# The source-tree invocation auto-detects a sibling apriltag-rvv repository.
# Buildroot passes that path explicitly because its copied package has no .git.
# Override the location or docker image with env vars:
#   APRILTAG_RVV_DIR=/path/to/apriltag-rvv  RVV_DOCKER_IMAGE=rvv-dev:latest
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${APRILTAG_RVV_DIR:-}" ]; then
    REPO_ROOT="$(git -C "$PKG_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -n "$REPO_ROOT" ] &&
       [ -d "$(dirname "$REPO_ROOT")/apriltag-rvv" ]; then
        APRILTAG_RVV_DIR="$(dirname "$REPO_ROOT")/apriltag-rvv"
    else
        APRILTAG_RVV_DIR="/work/git_repo/apriltag-rvv"
    fi
fi
RVV_DOCKER_IMAGE="${RVV_DOCKER_IMAGE:-rvv-dev:latest}"
MODE=production
NO_RVV=0
for arg in "$@"; do
    case "$arg" in
        --no-rvv) NO_RVV=1 ;;
        --workload-only)
            if [ "$MODE" = profile ]; then
                echo "error: --workload-only and --profile-only are mutually exclusive" >&2
                exit 2
            fi
            MODE=workload
            ;;
        --profile-only)
            if [ "$MODE" = workload ]; then
                echo "error: --workload-only and --profile-only are mutually exclusive" >&2
                exit 2
            fi
            MODE=profile
            ;;
        *) echo "error: unknown Rust build argument: $arg" >&2; exit 2 ;;
    esac
done
if [ "$MODE" = workload ] && [ -z "${APRILTAG_WORKLOAD_SOURCE_HASH:-}" ]; then
    echo "error: APRILTAG_WORKLOAD_SOURCE_HASH is required with --workload-only" >&2
    exit 2
fi
if [ "$MODE" = profile ] && [ -z "${APRILTAG_PROFILE_SOURCE_HASH:-}" ]; then
    echo "error: APRILTAG_PROFILE_SOURCE_HASH is required with --profile-only" >&2
    exit 2
fi
if [ "$MODE" = production ] && [ -z "${APRILTAG_SOURCE_HASH:-}" ]; then
    APRILTAG_SOURCE_HASH="$($PKG_DIR/scripts/rust_source_hash.sh "$APRILTAG_RVV_DIR" production)"
fi
RVV_ARGS=()
[ "$NO_RVV" -eq 0 ] || RVV_ARGS+=(--no-rvv)
[ "$MODE" != workload ] || RVV_ARGS+=(--workload-counters)
[ "$MODE" != profile ] || RVV_ARGS+=(--ccl-profile)
OUTPUT_ROOT="${APRILTAG_CAPI_OUTPUT_ROOT:-target}"

if [ ! -d "$APRILTAG_RVV_DIR" ]; then
    echo "error: apriltag-rvv not found at $APRILTAG_RVV_DIR" >&2
    echo "       set APRILTAG_RVV_DIR to its location" >&2
    exit 1
fi

# apriltag-rvv references ../async-rvv as a sibling; mount the common parent so
# relative path deps resolve inside the container.
PARENT="$(cd "$APRILTAG_RVV_DIR/.." && pwd)"
UID_HOST="$(id -u)"; GID_HOST="$(id -g)"
RVV_REAL="$(realpath "$APRILTAG_RVV_DIR")"
if [[ "$OUTPUT_ROOT" == *"'"* ]]; then
    echo "error: APRILTAG_CAPI_OUTPUT_ROOT may not contain a single quote" >&2
    exit 2
fi
if [[ "$OUTPUT_ROOT" = /* ]]; then
    OUTPUT_REAL="$(realpath -m "$OUTPUT_ROOT")"
else
    OUTPUT_REAL="$(realpath -m "$RVV_REAL/$OUTPUT_ROOT")"
fi
case "$OUTPUT_REAL" in
    "$RVV_REAL"/*) OUTPUT_RELATIVE="${OUTPUT_REAL#"$RVV_REAL/"}" ;;
    *)
        echo "error: APRILTAG_CAPI_OUTPUT_ROOT must resolve inside $RVV_REAL" >&2
        exit 2
        ;;
esac

echo "Building libapriltag_rvv.a in $RVV_DOCKER_IMAGE ..."
docker run --rm \
    -v "$PARENT":"$PARENT" \
    -w "$APRILTAG_RVV_DIR" \
    -e "APRILTAG_CAPI_OUTPUT_ROOT=$OUTPUT_RELATIVE" \
    -e "APRILTAG_BUILD_UID=$UID_HOST" \
    -e "APRILTAG_BUILD_GID=$GID_HOST" \
    "$RVV_DOCKER_IMAGE" \
    bash -c 'set -e; rustup target add riscv64gc-unknown-linux-gnu >/dev/null 2>&1 || true; bash scripts/build-capi.sh "$@"; chown -R "$APRILTAG_BUILD_UID:$APRILTAG_BUILD_GID" "$APRILTAG_CAPI_OUTPUT_ROOT"' \
    bash "${RVV_ARGS[@]}"

case "$MODE" in
    production) ARCHIVE=libapriltag_rvv.a ;;
    workload) ARCHIVE=libapriltag_rvv_workload.a ;;
    profile) ARCHIVE=libapriltag_rvv_profile.a ;;
esac
OUTPUT_DIR="$OUTPUT_REAL"
SRC_A="$OUTPUT_DIR/riscv64gc-unknown-linux-gnu/release/$ARCHIVE"
if [ ! -f "$SRC_A" ]; then
    echo "error: build did not produce $SRC_A" >&2
    exit 1
fi
mkdir -p "$PKG_DIR/lib"
if ! command -v flock >/dev/null 2>&1; then
    echo "error: flock is required for package publication" >&2
    exit 1
fi
ARCHIVE_TMP=
HEADER=
HEADER_TMP=
SCRATCH_HEADER=
SCRATCH_HEADER_TMP=
BUFFER_HEADER=
BUFFER_HEADER_TMP=
PENDING_HEADER=
PENDING_HEADER_TMP=
KERNEL_HEADER_TMP=
STAMP=
STAMP_TMP=
BACKUP_DIR=
PUBLISHING=0
ROLLBACK_DONE=0
rollback_publication() {
    local destination backup
    [ "$ROLLBACK_DONE" -eq 0 ] || return 0
    ROLLBACK_DONE=1
    [ "$PUBLISHING" -eq 1 ] || return 0
    for destination in "$ARCHIVE" "$HEADER" "$SCRATCH_HEADER" "$BUFFER_HEADER" apriltag_kernel_modes.h "$PENDING_HEADER" "$STAMP"; do
        [ -n "$destination" ] || continue
        backup="$BACKUP_DIR/$destination"
        if [ -e "$backup" ]; then
            mv -f "$backup" "$PKG_DIR/lib/$destination"
        else
            rm -f "$PKG_DIR/lib/$destination"
        fi
    done
}
cleanup_resources() {
    rm -f "$ARCHIVE_TMP" "$HEADER_TMP" "$SCRATCH_HEADER_TMP" \
        "$BUFFER_HEADER_TMP" "$PENDING_HEADER_TMP" "$KERNEL_HEADER_TMP" \
        "$STAMP_TMP"
    [ -z "$BACKUP_DIR" ] || rm -rf "$BACKUP_DIR"
}
cleanup_publication() {
    local status=$?
    rollback_publication
    cleanup_resources
    return "$status"
}
signal_publication() {
    local status=$1
    trap - EXIT HUP INT TERM
    rollback_publication
    cleanup_resources
    exit "$status"
}
trap cleanup_publication EXIT
trap 'signal_publication 129' HUP
trap 'signal_publication 130' INT
trap 'signal_publication 143' TERM
if [ "$MODE" = workload ]; then
    ARCHIVE_TMP="$(mktemp "$PKG_DIR/lib/.${ARCHIVE}.XXXXXX")"
    HEADER_TMP="$(mktemp "$PKG_DIR/lib/.rust_apriltag_workload.h.XXXXXX")"
    STAMP_TMP="$(mktemp "$PKG_DIR/lib/.source-hash.XXXXXX")"
    cp -f "$SRC_A" "$ARCHIVE_TMP"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_workload.h" "$HEADER_TMP"
    SCRATCH_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_scratch.h.XXXXXX")"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_scratch.h" "$SCRATCH_HEADER_TMP"
    printf '%s\n' "$APRILTAG_WORKLOAD_SOURCE_HASH" >"$STAMP_TMP"
    HEADER=rust_apriltag_workload.h
    SCRATCH_HEADER=apriltag_scratch.h
    STAMP=.apriltag_rvv_workload.source-hash
elif [ "$MODE" = profile ]; then
    ARCHIVE_TMP="$(mktemp "$PKG_DIR/lib/.${ARCHIVE}.XXXXXX")"
    HEADER_TMP="$(mktemp "$PKG_DIR/lib/.rust_apriltag_profile.h.XXXXXX")"
    SCRATCH_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_scratch.h.XXXXXX")"
    STAMP_TMP="$(mktemp "$PKG_DIR/lib/.source-hash.XXXXXX")"
    cp -f "$SRC_A" "$ARCHIVE_TMP"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_profile.h" "$HEADER_TMP"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_scratch.h" "$SCRATCH_HEADER_TMP"
    BUFFER_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_buffer_telemetry.h.XXXXXX")"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_buffer_telemetry.h" "$BUFFER_HEADER_TMP"
    PENDING_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_pending_profile.h.XXXXXX")"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_pending_profile.h" "$PENDING_HEADER_TMP"
    printf '%s\n' "$APRILTAG_PROFILE_SOURCE_HASH" >"$STAMP_TMP"
    HEADER=rust_apriltag_profile.h
    SCRATCH_HEADER=apriltag_scratch.h
    BUFFER_HEADER=apriltag_buffer_telemetry.h
    PENDING_HEADER=apriltag_pending_profile.h
    STAMP=.apriltag_rvv_profile.source-hash
else
    ARCHIVE_TMP="$(mktemp "$PKG_DIR/lib/.${ARCHIVE}.XXXXXX")"
    STAMP_TMP="$(mktemp "$PKG_DIR/lib/.source-hash.XXXXXX")"
    cp -f "$SRC_A" "$ARCHIVE_TMP"
    SCRATCH_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_scratch.h.XXXXXX")"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_scratch.h" "$SCRATCH_HEADER_TMP"
    printf '%s\n' "$APRILTAG_SOURCE_HASH" >"$STAMP_TMP"
    HEADER=
    HEADER_TMP=
    SCRATCH_HEADER=apriltag_scratch.h
    STAMP=.apriltag_rvv.source-hash
fi
BUFFER_HEADER=apriltag_buffer_telemetry.h
if [ -z "${BUFFER_HEADER_TMP:-}" ]; then
    BUFFER_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_buffer_telemetry.h.XXXXXX")"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_buffer_telemetry.h" "$BUFFER_HEADER_TMP"
fi

chmod 0644 "$ARCHIVE_TMP" "$STAMP_TMP"
[ -z "$HEADER_TMP" ] || chmod 0644 "$HEADER_TMP"
[ -z "$SCRATCH_HEADER_TMP" ] || chmod 0644 "$SCRATCH_HEADER_TMP"
[ -z "$BUFFER_HEADER_TMP" ] || chmod 0644 "$BUFFER_HEADER_TMP"
[ -z "$PENDING_HEADER_TMP" ] || chmod 0644 "$PENDING_HEADER_TMP"
KERNEL_HEADER_TMP="$(mktemp "$PKG_DIR/lib/.apriltag_kernel_modes.h.XXXXXX")"
cp -f "$APRILTAG_RVV_DIR/include/apriltag_kernel_modes.h" "$KERNEL_HEADER_TMP"
chmod 0644 "$KERNEL_HEADER_TMP"
BACKUP_DIR="$(mktemp -d "$PKG_DIR/lib/.publish-backup.XXXXXX")"
if [ "${APRILTAG_PACKAGE_LOCK_HELD:-0}" != 1 ]; then
    exec 9>"$PKG_DIR/lib/.apriltag-rvv-package.lock"
    flock 9
fi
for current in "$ARCHIVE" "$HEADER" "$SCRATCH_HEADER" "$BUFFER_HEADER" apriltag_kernel_modes.h "$PENDING_HEADER" "$STAMP"; do
    [ -n "$current" ] || continue
    [ ! -e "$PKG_DIR/lib/$current" ] || cp -p "$PKG_DIR/lib/$current" "$BACKUP_DIR/$current"
done
PUBLISHING=1
# Removing the commit marker before replacing payloads prevents an unlocked
# reader from accepting the old matching stamp during the publication window.
rm -f "$PKG_DIR/lib/$STAMP"
mv -f "$ARCHIVE_TMP" "$PKG_DIR/lib/$ARCHIVE"
[ "${APRILTAG_PACKAGE_TEST_PAUSE_STEP:-}" != after_archive ] || {
    : >"${APRILTAG_PACKAGE_TEST_READY_FILE:?}"
    while [ ! -e "${APRILTAG_PACKAGE_TEST_RELEASE_FILE:-/nonexistent}" ]; do sleep 0.02; done
}
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_archive ] || false
if [ -n "$HEADER" ]; then
    mv -f "$HEADER_TMP" "$PKG_DIR/lib/$HEADER"
fi
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_profile_header ] || false
if [ -n "$SCRATCH_HEADER" ]; then
    mv -f "$SCRATCH_HEADER_TMP" "$PKG_DIR/lib/$SCRATCH_HEADER"
fi
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_scratch_header ] || false
if [ -n "$BUFFER_HEADER" ]; then
    mv -f "$BUFFER_HEADER_TMP" "$PKG_DIR/lib/$BUFFER_HEADER"
fi
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_buffer_header ] || false
mv -f "$KERNEL_HEADER_TMP" "$PKG_DIR/lib/apriltag_kernel_modes.h"
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_kernel_header ] || false
if [ -n "$PENDING_HEADER" ]; then
    mv -f "$PENDING_HEADER_TMP" "$PKG_DIR/lib/$PENDING_HEADER"
fi
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_pending_header ] || false
# The stamp is the commit marker and is always published last. Buildroot holds
# this lock synchronously and accepts a set only after its stamp hash matches.
mv -f "$STAMP_TMP" "$PKG_DIR/lib/$STAMP"
[ "${APRILTAG_PACKAGE_TEST_FAIL_STEP:-}" != after_stamp ] || false
PUBLISHING=0
rm -rf "$BACKUP_DIR"
trap - EXIT HUP INT TERM
echo "Copied -> $PKG_DIR/lib/$ARCHIVE"
ls -l "$PKG_DIR/lib/$ARCHIVE"
