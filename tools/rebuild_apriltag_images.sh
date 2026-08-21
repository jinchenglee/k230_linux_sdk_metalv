#!/bin/bash
# Rebuild k230 linux images to pick up changes from the sibling apriltag-rvv
# repository (built via the rvv-dev docker image, see rvv-shell in your shell rc).
#
# apriltag_demo uses Buildroot's "local" site method: sources are rsynced
# into the per-defconfig build dir once, at extract time. A plain
# "apriltag_demo-reconfigure" reuses that stale copy, so local C++/CMake
# changes here (not just Rust changes in apriltag-rvv) need a real
# "apriltag_demo-dirclean" to force a fresh resync. The Rust archives
# themselves are separately gated by content hash in
# buildroot-overlay/package/apriltag_demo/scripts/rust_source_hash.sh and
# rebuild automatically inside the package's pre-configure hook whenever
# that hash no longer matches apriltag-rvv's current source.
#
# apriltag_demo links against the C "apriltag" package's staged headers/lib
# (workload counter ABI, etc.), but Buildroot never rebuilds an
# already-built dependency just because apriltag_demo is being forced. If
# buildroot-overlay/package/apriltag/*.patch changed since apriltag was last
# built here, apriltag_demo will happily compile against stale, ABI-mismatched
# staged headers and fail (or worse, silently link wrong). So this also
# forces "apriltag-dirclean" every run.
#
# Usage:
#   tools/rebuild_apriltag_images.sh                          # both default boards
#   tools/rebuild_apriltag_images.sh k230_canmv_defconfig      # just one
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

DEFCONFIGS=("$@")
if [ ${#DEFCONFIGS[@]} -eq 0 ]; then
    DEFCONFIGS=(k230_canmv_01studio_defconfig k230_canmv_defconfig)
fi

APRILTAG_RVV_DIR="$(cd .. && pwd)/apriltag-rvv"
if [ ! -d "$APRILTAG_RVV_DIR" ]; then
    echo "error: sibling apriltag-rvv repo not found at $APRILTAG_RVV_DIR" >&2
    exit 1
fi
if ! docker image inspect rvv-dev:latest >/dev/null 2>&1; then
    echo "error: rvv-dev:latest docker image not found (build it the way rvv-shell expects)" >&2
    exit 1
fi

for defconfig in "${DEFCONFIGS[@]}"; do
    echo "### $defconfig: apriltag-dirclean (forces rebuild of the C apriltag package + ABI headers) ###"
    make CONF="$defconfig" apriltag-dirclean
    echo "### $defconfig: apriltag_demo-dirclean (forces resync of local package sources) ###"
    make CONF="$defconfig" apriltag_demo-dirclean
    echo "### $defconfig: rebuilding apriltag_demo (triggers Rust rebuild if apriltag-rvv changed) ###"
    make CONF="$defconfig" apriltag_demo
    echo "### $defconfig: rebuilding full image ###"
    make CONF="$defconfig"
done

echo "### Done. Images: ###"
for defconfig in "${DEFCONFIGS[@]}"; do
    ls -l "output/$defconfig/images/"*.img.gz 2>/dev/null || true
done
