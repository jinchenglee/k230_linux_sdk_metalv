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
WORKLOAD=0
NO_RVV=0
for arg in "$@"; do
    case "$arg" in
        --no-rvv) NO_RVV=1 ;;
        --workload-only) WORKLOAD=1 ;;
        *) echo "error: unknown Rust build argument: $arg" >&2; exit 2 ;;
    esac
done
if [ "$WORKLOAD" -eq 1 ] && [ -z "${APRILTAG_WORKLOAD_SOURCE_HASH:-}" ]; then
    echo "error: APRILTAG_WORKLOAD_SOURCE_HASH is required with --workload-only" >&2
    exit 2
fi
if [ "$WORKLOAD" -eq 0 ] && [ -z "${APRILTAG_SOURCE_HASH:-}" ]; then
    APRILTAG_SOURCE_HASH="$($PKG_DIR/scripts/rust_source_hash.sh "$APRILTAG_RVV_DIR" production)"
fi
RVV_ARGS=()
[ "$NO_RVV" -eq 0 ] || RVV_ARGS+=(--no-rvv)
[ "$WORKLOAD" -eq 0 ] || RVV_ARGS+=(--workload-counters)
RVV_ARG_STRING="${RVV_ARGS[*]}"

if [ ! -d "$APRILTAG_RVV_DIR" ]; then
    echo "error: apriltag-rvv not found at $APRILTAG_RVV_DIR" >&2
    echo "       set APRILTAG_RVV_DIR to its location" >&2
    exit 1
fi

# apriltag-rvv references ../async-rvv as a sibling; mount the common parent so
# relative path deps resolve inside the container.
PARENT="$(cd "$APRILTAG_RVV_DIR/.." && pwd)"
UID_HOST="$(id -u)"; GID_HOST="$(id -g)"

echo "Building libapriltag_rvv.a in $RVV_DOCKER_IMAGE ..."
docker run --rm \
    -v "$PARENT":"$PARENT" \
    -w "$APRILTAG_RVV_DIR" \
    "$RVV_DOCKER_IMAGE" \
    bash -lc "
        set -e
        rustup target add riscv64gc-unknown-linux-gnu >/dev/null 2>&1 || true
        bash scripts/build-capi.sh ${RVV_ARG_STRING}
        chown -R ${UID_HOST}:${GID_HOST} target
    "

if [ "$WORKLOAD" -eq 1 ]; then
    ARCHIVE=libapriltag_rvv_workload.a
else
    ARCHIVE=libapriltag_rvv.a
fi
SRC_A="$APRILTAG_RVV_DIR/target/riscv64gc-unknown-linux-gnu/release/$ARCHIVE"
if [ ! -f "$SRC_A" ]; then
    echo "error: build did not produce $SRC_A" >&2
    exit 1
fi
mkdir -p "$PKG_DIR/lib"
if [ "$WORKLOAD" -eq 1 ]; then
    ARCHIVE_TMP="$(mktemp "$PKG_DIR/lib/.${ARCHIVE}.XXXXXX")"
    HEADER_TMP="$(mktemp "$PKG_DIR/lib/.rust_apriltag_workload.h.XXXXXX")"
    STAMP_TMP="$(mktemp "$PKG_DIR/lib/.source-hash.XXXXXX")"
    trap 'rm -f "$ARCHIVE_TMP" "$HEADER_TMP" "$STAMP_TMP"' EXIT
    cp -f "$SRC_A" "$ARCHIVE_TMP"
    cp -f "$APRILTAG_RVV_DIR/include/apriltag_workload.h" "$HEADER_TMP"
    printf '%s\n' "$APRILTAG_WORKLOAD_SOURCE_HASH" >"$STAMP_TMP"
    rm -f "$PKG_DIR/lib/.apriltag_rvv_workload.source-hash"
    mv -f "$ARCHIVE_TMP" "$PKG_DIR/lib/$ARCHIVE"
    mv -f "$HEADER_TMP" "$PKG_DIR/lib/rust_apriltag_workload.h"
    mv -f "$STAMP_TMP" "$PKG_DIR/lib/.apriltag_rvv_workload.source-hash"
    trap - EXIT
else
    ARCHIVE_TMP="$(mktemp "$PKG_DIR/lib/.${ARCHIVE}.XXXXXX")"
    STAMP_TMP="$(mktemp "$PKG_DIR/lib/.source-hash.XXXXXX")"
    trap 'rm -f "$ARCHIVE_TMP" "$STAMP_TMP"' EXIT
    cp -f "$SRC_A" "$ARCHIVE_TMP"
    printf '%s\n' "$APRILTAG_SOURCE_HASH" >"$STAMP_TMP"
    rm -f "$PKG_DIR/lib/.apriltag_rvv.source-hash"
    mv -f "$ARCHIVE_TMP" "$PKG_DIR/lib/$ARCHIVE"
    mv -f "$STAMP_TMP" "$PKG_DIR/lib/.apriltag_rvv.source-hash"
    trap - EXIT
fi
echo "Copied -> $PKG_DIR/lib/$ARCHIVE"
ls -l "$PKG_DIR/lib/$ARCHIVE"
