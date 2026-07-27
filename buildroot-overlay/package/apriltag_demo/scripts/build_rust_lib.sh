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
RVV_ARG="${1:-}"

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
        bash scripts/build-capi.sh ${RVV_ARG}
        chown -R ${UID_HOST}:${GID_HOST} target
    "

SRC_A="$APRILTAG_RVV_DIR/target/riscv64gc-unknown-linux-gnu/release/libapriltag_rvv.a"
if [ ! -f "$SRC_A" ]; then
    echo "error: build did not produce $SRC_A" >&2
    exit 1
fi
mkdir -p "$PKG_DIR/lib"
cp -f "$SRC_A" "$PKG_DIR/lib/libapriltag_rvv.a"
echo "Copied -> $PKG_DIR/lib/libapriltag_rvv.a"
ls -l "$PKG_DIR/lib/libapriltag_rvv.a"
