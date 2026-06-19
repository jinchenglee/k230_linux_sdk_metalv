#!/bin/bash
# Script to check OpenCV build configuration and verify RVV is disabled

CONF=${CONF:-BPI-CanMV-K230D-Zero_defconfig}
BRW_BUILD_DIR="output/${CONF}"

echo "=========================================="
echo "Checking OpenCV Configuration for ${CONF}"
echo "=========================================="
echo ""

# Check if BR2_RISCV_ISA_RVV is set in defconfig
echo "1. Checking defconfig for BR2_RISCV_ISA_RVV:"
if grep -q "^BR2_RISCV_ISA_RVV=y" "buildroot-overlay/configs/${CONF}"; then
    echo "   ❌ BR2_RISCV_ISA_RVV is ENABLED (should be disabled for non-vector CPU)"
    RVV_ENABLED=1
elif grep -q "^# BR2_RISCV_ISA_RVV is not set" "buildroot-overlay/configs/${CONF}"; then
    echo "   ✓ BR2_RISCV_ISA_RVV is DISABLED (correct for non-vector CPU)"
    RVV_ENABLED=0
else
    echo "   ⚠ BR2_RISCV_ISA_RVV not found in defconfig"
    RVV_ENABLED=0
fi

echo ""
echo "2. Checking if build directory exists:"
if [ -d "${BRW_BUILD_DIR}" ]; then
    echo "   ✓ Build directory exists: ${BRW_BUILD_DIR}"
    
    echo ""
    echo "3. Checking OpenCV build configuration in build directory:"
    if [ -f "${BRW_BUILD_DIR}/.config" ]; then
        if grep -q "^BR2_RISCV_ISA_RVV=y" "${BRW_BUILD_DIR}/.config"; then
            echo "   ❌ BR2_RISCV_ISA_RVV is ENABLED in .config (needs reconfigure)"
            echo "   Run: make -C ${BRW_BUILD_DIR} olddefconfig"
        elif grep -q "^# BR2_RISCV_ISA_RVV is not set" "${BRW_BUILD_DIR}/.config"; then
            echo "   ✓ BR2_RISCV_ISA_RVV is DISABLED in .config"
        else
            echo "   ⚠ BR2_RISCV_ISA_RVV not found in .config"
        fi
    else
        echo "   ⚠ .config not found - need to run: make CONF=${CONF}"
    fi
    
    echo ""
    echo "4. Checking OpenCV CMake cache (if built):"
    OPENCV_BUILD_DIR=$(find "${BRW_BUILD_DIR}/build" -type d -name "opencv4-*" 2>/dev/null | head -1)
    if [ -n "${OPENCV_BUILD_DIR}" ] && [ -f "${OPENCV_BUILD_DIR}/CMakeCache.txt" ]; then
        echo "   Found OpenCV build: ${OPENCV_BUILD_DIR}"
        if grep -q "BUILD_CSI_CV:BOOL=ON" "${OPENCV_BUILD_DIR}/CMakeCache.txt"; then
            echo "   ❌ BUILD_CSI_CV is ON (should be OFF for non-vector CPU)"
        elif grep -q "BUILD_CSI_CV:BOOL=OFF" "${OPENCV_BUILD_DIR}/CMakeCache.txt"; then
            echo "   ✓ BUILD_CSI_CV is OFF (correct)"
        else
            echo "   ⚠ BUILD_CSI_CV not found in CMakeCache.txt"
        fi
        
        if grep -q "CORE:STRING=C908V" "${OPENCV_BUILD_DIR}/CMakeCache.txt"; then
            echo "   ❌ CORE is set to C908V (should not be set for non-vector CPU)"
        else
            echo "   ✓ CORE is not C908V (correct)"
        fi
        
        echo ""
        echo "   CMAKE_C_FLAGS:"
        grep "^CMAKE_C_FLAGS:" "${OPENCV_BUILD_DIR}/CMakeCache.txt" | head -1 | sed 's/^/     /'
        
        echo ""
        echo "   CMAKE_CXX_FLAGS:"
        grep "^CMAKE_CXX_FLAGS:" "${OPENCV_BUILD_DIR}/CMakeCache.txt" | head -1 | sed 's/^/     /'
    else
        echo "   ⚠ OpenCV not built yet or build directory not found"
    fi
else
    echo "   ❌ Build directory does not exist: ${BRW_BUILD_DIR}"
    echo "   Run: make CONF=${CONF}"
fi

echo ""
echo "=========================================="
echo "Summary"
echo "=========================================="
if [ "${RVV_ENABLED}" -eq 0 ]; then
    echo "Configuration looks correct for non-vector CPU."
    echo ""
    echo "If you're still getting illegal instruction errors, try:"
    echo "  1. Run: ./rebuild_opencv.sh"
    echo "  2. Or manually:"
    echo "     make -C ${BRW_BUILD_DIR} opencv4-dirclean"
    echo "     make -C ${BRW_BUILD_DIR} opencv4-rebuild"
    echo "     make -C ${BRW_BUILD_DIR} all"
else
    echo "⚠ WARNING: RVV is enabled in defconfig!"
    echo "This will cause illegal instruction errors on non-vector CPU."
fi

