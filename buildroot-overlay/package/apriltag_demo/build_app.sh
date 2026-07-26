#!/bin/bash
# Standalone build for apriltag_demo (outside buildroot), mirroring
# face_detect/build_app.sh. Uses the SDK's external toolchain + staging sysroot.
#
# Prerequisite: the Rust staticlib must exist at lib/libapriltag_rvv.a. Build it
# first with:  scripts/build_rust_lib.sh   (runs in the rvv-dev docker image).
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="$(cd "${PKG_DIR}/../../../" && pwd)"
CONF="$(cat "${SDK_ROOT}/.last_conf" | cut -d= -f2)"
GCC_PATH="$(grep BR2_TOOLCHAIN_EXTERNAL_PATH "${SDK_ROOT}/output/${CONF}/.config" | cut -d\" -f2)/bin"
sysroot="$(realpath "${SDK_ROOT}/output/${CONF}/staging")"
export GCC_PATH
export sysroot

echo "GCC_PATH=${GCC_PATH}"
echo "sysroot=${sysroot}"

if [ ! -f "${PKG_DIR}/lib/libapriltag_rvv.a" ]; then
    echo "lib/libapriltag_rvv.a missing — building it via docker ..."
    bash "${PKG_DIR}/scripts/build_rust_lib.sh"
fi

cd "${PKG_DIR}"
rm -rf out k230_bin
mkdir -p out k230_bin

pushd out
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$(pwd)" \
      -DCMAKE_C_COMPILER="${GCC_PATH}/riscv64-unknown-linux-gnu-gcc" \
      -DCMAKE_CXX_COMPILER="${GCC_PATH}/riscv64-unknown-linux-gnu-g++" \
      ..
make -j"$(nproc)" && make install
popd

if [ -f out/bin/apriltag_demo.elf ]; then
    cp out/bin/apriltag_demo.elf k230_bin/
fi
cp -r utils/* k230_bin/ 2>/dev/null || true
rm -rf out
echo "Done: k230_bin/apriltag_demo.elf"
