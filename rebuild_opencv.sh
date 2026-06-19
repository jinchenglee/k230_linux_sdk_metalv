#!/bin/bash
# Script to properly rebuild OpenCV after fixing RVV configuration
# This ensures all cached artifacts are removed and OpenCV is rebuilt from scratch

set -e

CONF=${CONF:-BPI-CanMV-K230D-Zero_defconfig}
BRW_BUILD_DIR="output/${CONF}"

echo "=========================================="
echo "Rebuilding OpenCV for ${CONF}"
echo "=========================================="
echo ""

# Check if build directory exists
if [ ! -d "${BRW_BUILD_DIR}" ]; then
    echo "Error: Build directory ${BRW_BUILD_DIR} does not exist"
    echo "Please run: make CONF=${CONF}"
    exit 1
fi

echo "Step 1: Cleaning OpenCV build directory..."
make -C "${BRW_BUILD_DIR}" opencv4-dirclean || true

echo ""
echo "Step 2: Cleaning OpenCV Python bindings..."
make -C "${BRW_BUILD_DIR}" opencv4-python-dirclean || true

echo ""
echo "Step 3: Removing any cached OpenCV configuration..."
# Remove CMake cache if it exists
rm -rf "${BRW_BUILD_DIR}/build/opencv4-*/.stamp_configured" || true
rm -rf "${BRW_BUILD_DIR}/build/opencv4-*/.stamp_built" || true
rm -rf "${BRW_BUILD_DIR}/build/opencv4-*/.stamp_target_installed" || true
rm -rf "${BRW_BUILD_DIR}/build/opencv4-*/.stamp_staging_installed" || true

echo ""
echo "Step 4: Rebuilding OpenCV from scratch..."
make -C "${BRW_BUILD_DIR}" opencv4-rebuild

echo ""
echo "Step 5: Rebuilding root filesystem to include new OpenCV..."
make -C "${BRW_BUILD_DIR}" target-finalize

echo ""
echo "=========================================="
echo "OpenCV rebuild complete!"
echo "=========================================="
echo ""
echo "To create a new image with the rebuilt OpenCV, run:"
echo "  make -C ${BRW_BUILD_DIR} all"

