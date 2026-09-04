# Building an RVV-free nncase v2.11.0 runtime for K230 Linux

Status: **experimental; archive build, host-side link, and big-core KPU probe
validated; small-core hardware validation pending**. This note records the
experiment performed on 2026-09-04. It is intended to be reproducible and to
prevent the prebuilt K230 runtime from being mistaken for a hard blocker to
running nncase under small-core Linux.

## 1. Goal and scope

The K230 Linux SDK currently downloads a prebuilt nncase v2.11.0 runtime from
the nncase release rather than compiling it:

```
buildroot-overlay/package/libnncase/libnncase.mk
```

The downloaded package contains three relevant static archives:

```
libNncase.Runtime.Native.a
libnncase.rt_modules.k230.a
libfunctional_k230.a
```

All objects in the distributed archives advertise an RVV-capable RISC-V ISA.
The open-source generic runtime also contains actual RVV CPU fallback kernels,
so the distributed `libNncase.Runtime.Native.a` cannot be used safely on a CPU
without the V extension.

The K230 backend source is not public. The experiment therefore uses a hybrid
library set:

- rebuild the open `libNncase.Runtime.Native.a` for `rv64imafdc`;
- retain the exact-version closed `libnncase.rt_modules.k230.a` and
  `libfunctional_k230.a` from the K230 release;
- rebuild all application-side code without RVV before running on the small
  core.

This does not make KPU work execute on the CPU. KPU inference and AI2D still
use their hardware engines. The scalar code replaces host runtime and optional
CPU fallback kernels.

## 2. What was established

The experiment used:

- this SDK's `k230_canmv_defconfig` Buildroot toolchain;
- nncase tag `v2.11.0`, commit
  `1d49a3196ff44573e58c8422274a5c2702234ea1`;
- GNU cross compiler 14.1.1 from
  `output/k230_canmv_defconfig/host`.

The resulting archive:

- builds successfully as `libNncase.Runtime.Native.a`;
- advertises `rv64imafdc` without V in its ELF attributes;
- contains no RVV instruction found by the disassembly audit below;
- exports 2,402 global symbols versus 2,411 in the distributed archive;
- omits only nine RVV implementation helpers, none referenced by either
  retained closed K230 archive;
- successfully links into the current `tinytag_detect` application together
  with the retained K230 archives;
- successfully loads and executes the TinyTag K230 model through AI2D/KPU on
  the big-core Linux board, with correct detections and no runtime fault.

The successful experimental archive was approximately 25 MiB and had SHA-256:

```
44271d2572304aa1c35487ecbc1f9958feae55d9a08ca301495620f5b3127cd6
```

The checksum is a record of this run, not a required value: compiler or archive
tool timestamps can change it.

## 3. Prerequisites

Start at the SDK root and build the normal configuration far enough to produce
the host toolchain and `gsl-lite` staging package:

```sh
cd /path/to/k230_linux_sdk_metalv
make CONF=k230_canmv_defconfig toolchain
make CONF=k230_canmv_defconfig gsl-lite
```

If those narrow targets are unavailable in a future SDK revision, a normal SDK
or application build also produces the required files. Verify these paths:

```sh
test -x output/k230_canmv_defconfig/host/bin/riscv64-unknown-linux-gnu-g++
test -f output/k230_canmv_defconfig/host/riscv64-buildroot-linux-gnu/sysroot/usr/lib/cmake/gsl-lite/gsl-lite-config.cmake
```

The procedure uses CMake, Ninja, Git, `readelf`, `objdump`, `nm`, and standard
shell utilities.

## 4. Fetch the matching nncase source

Use the exact version of the distributed closed libraries. Mixing versions is
not ABI-safe even if linking happens to succeed.

```sh
export K230_SDK_ROOT=/path/to/k230_linux_sdk_metalv
export NNCASE_SRC=/tmp/nncase-v2.11.0

git clone --branch v2.11.0 --depth 1 \
    https://github.com/kendryte/nncase.git "$NNCASE_SRC"
cd "$NNCASE_SRC"
git rev-parse HEAD
git describe --tags --exact-match HEAD
```

Expected revision and tag:

```
1d49a3196ff44573e58c8422274a5c2702234ea1
v2.11.0
```

## 5. Patch runtime-only dependency and kernel selection

`ENABLE_RVV` exists and defaults to `OFF`, but in v2.11.0 that switch is not
sufficient by itself. The optimized-kernel CMake code selects the `riscv64/`
directory solely from `CMAKE_SYSTEM_PROCESSOR`. Some files in that directory
directly use RVV intrinsics, including code not completely protected by
`#if __riscv_vector`.

When RVV is disabled, make the selector use the architecture-neutral optimized
implementations, which fall back to the reference kernels as needed.

The top-level build also requires `nlohmann_json` unconditionally even though
runtime-only builds do not use that dependency. Restrict it to the simulator/
compiler build so the SDK sysroot does not need an unrelated package.

Apply both changes:

```sh
git apply <<'PATCH'
diff --git a/CMakeLists.txt b/CMakeLists.txt
index e83390f..ce6665d 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -39,7 +39,9 @@ project(nncase
   VERSION ${NNCASE_VERSION}
   LANGUAGES C CXX ASM)
 
-find_package(nlohmann_json REQUIRED)
+if (NOT BUILDING_RUNTIME)
+    find_package(nlohmann_json REQUIRED)
+endif ()
 include_directories(${nlohmann_json_INCLUDE_DIRS})
 
 option(ENABLE_OPENMP "OpenMP support" OFF)
diff --git a/src/Native/src/kernels/stackvm/optimized/CMakeLists.txt b/src/Native/src/kernels/stackvm/optimized/CMakeLists.txt
index 7a497fa..b5c577b 100644
--- a/src/Native/src/kernels/stackvm/optimized/CMakeLists.txt
+++ b/src/Native/src/kernels/stackvm/optimized/CMakeLists.txt
@@ -1,6 +1,6 @@
 cmake_minimum_required (VERSION 3.13)
 
-if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "riscv64")
+if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "riscv64" AND ENABLE_RVV)
     set(ARCH riscv64)
 elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "(x86)|(X86)|(amd64)|(AMD64)|(x86_64)|(X86_64)")
     set(ARCH x86_64)
PATCH
```

## 6. Add a small-core Linux toolchain file

The supplied `toolchains/k230.linux.toolchain.cmake` explicitly adds V and
RVV-compatibility flags. Do not use it unchanged. Create
`toolchains/k230-small-linux.toolchain.cmake` with the following content:

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

if(DEFINED ENV{RISCV_ROOT_PATH})
    file(TO_CMAKE_PATH $ENV{RISCV_ROOT_PATH} RISCV_ROOT_PATH)
endif()

if(NOT RISCV_ROOT_PATH)
    message(FATAL_ERROR "RISCV_ROOT_PATH env must be defined")
endif()

set(RISCV_ROOT_PATH ${RISCV_ROOT_PATH} CACHE STRING "root path to riscv toolchain")
set(CMAKE_C_COMPILER "${RISCV_ROOT_PATH}/bin/riscv64-unknown-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${RISCV_ROOT_PATH}/bin/riscv64-unknown-linux-gnu-g++")
set(CMAKE_FIND_ROOT_PATH "${RISCV_ROOT_PATH}/riscv64-buildroot-linux-gnu/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENABLE_VULKAN_RUNTIME OFF)
set(ENABLE_OPENMP OFF)
set(ENABLE_HALIDE OFF)
set(DEFAULT_BUILTIN_RUNTIMES OFF)
set(DEFAULT_SHARED_RUNTIME_TENSOR_PLATFORM_IMPL OFF)
set(BUILD_BENCHMARK OFF)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv64imafdc -mabi=lp64d -mcmodel=medany -fstack-protector-strong")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=rv64imafdc -mabi=lp64d -mcmodel=medany -fstack-protector-strong")

set(BUILDING_RUNTIME ON)
set(ENABLE_K230_RUNTIME ON)
set(BUILD_SHARED_LIBS OFF)

add_definitions(-DLINUX_RUNTIME)
```

The important properties are:

- `-march=rv64imafdc`, with no V extension;
- no `-mrvv-v0p10-compatible` or automatic RVV-vectorization option;
- the Buildroot sysroot path, whose target tuple differs from the compiler
  executable name;
- `BUILDING_RUNTIME=ON` and `ENABLE_K230_RUNTIME=ON`.

## 7. Configure and build

`RISCV_ROOT_PATH` must be an environment variable. Merely passing a CMake
cache variable is insufficient because CMake's nested `try_compile` reloads
the toolchain file.

```sh
cd "$NNCASE_SRC"
export RISCV_ROOT_PATH="$K230_SDK_ROOT/output/k230_canmv_defconfig/host"

cmake -S . -B build-small-linux -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=toolchains/k230-small-linux.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_RVV=OFF \
    -DBUILDING_RUNTIME=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_BENCHMARK=OFF \
    -DBUILD_PYTHON_BINDING=OFF \
    -DBUILD_CSHARP_BINDING=OFF

cmake --build build-small-linux --target nncaseruntime -j"$(nproc)"
```

The output is:

```sh
export NNCASE_SCALAR="$NNCASE_SRC/build-small-linux/src/Native/src/runtime/libNncase.Runtime.Native.a"
test -f "$NNCASE_SCALAR"
```

## 8. Verify that the archive is RVV-free

Use the target binutils, not host x86 binutils:

```sh
export K230_BIN="$RISCV_ROOT_PATH/bin"

"$K230_BIN/riscv64-unknown-linux-gnu-readelf" -A "$NNCASE_SCALAR" \
    | sort -u
```

The `Tag_RISCV_arch` line from the successful build was:

```
rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zmmul1p0
```

It must not contain a `v` ISA component. Next inspect decoded instructions:

```sh
"$K230_BIN/riscv64-unknown-linux-gnu-objdump" -d "$NNCASE_SCALAR" \
    | grep -En $'\t(vset|v[a-z0-9_.]+)([[:space:]]|$)'
```

No output and `grep` status 1 are expected. This expression is an audit aid,
not a mathematical proof that every possible vector encoding was recognized;
execution on the non-RVV core remains the final test.

It is also useful to confirm the compile commands themselves:

```sh
grep -R -- '-march=' build-small-linux/compile_commands.json 2>/dev/null || true
```

If `compile_commands.json` is desired, add
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` during configuration.

## 9. Check compatibility with the closed K230 archives

Locate the distributed libraries installed by Buildroot:

```sh
export NNCASE_SHIPPED_DIR="$K230_SDK_ROOT/output/k230_canmv_defconfig/build/libnncase/nncase/lib"
export NNCASE_SHIPPED="$NNCASE_SHIPPED_DIR/libNncase.Runtime.Native.a"
export K230_MODULE="$NNCASE_SHIPPED_DIR/libnncase.rt_modules.k230.a"
export K230_FUNCTIONAL="$NNCASE_SHIPPED_DIR/libfunctional_k230.a"
```

Compare exported symbols. The following requires Bash process substitution:

```bash
export K230_NM="$K230_BIN/riscv64-unknown-linux-gnu-nm"

comm -23 \
  <("$K230_NM" -g --defined-only "$NNCASE_SHIPPED" \
      | awk 'NF >= 3 {print $3}' | sort -u) \
  <("$K230_NM" -g --defined-only "$NNCASE_SCALAR" \
      | awk 'NF >= 3 {print $3}' | sort -u)
```

For v2.11.0 the only symbols present in the shipped runtime but absent from
the scalar runtime were these RVV-specific helpers:

```
_Z14layernorm_implPKfPfS0_S0_N3gsl4spanIKmEEif
_Z15reduce_max_implPKfPfmmm
_Z15reduce_min_implPKfPfmmm
_Z15reduce_sum_implPKfPfmmm
_Z16reduce_prod_implPKfPfmmm
_Z17log_softmax_blockiiPKfPfi
_Z17log_softmax_step1iPKfPf
_Z21log_softmax_step_not1iPKfPfi
_Z9tile_implIlEN6nncase6resultIvEEPKT_PS3_N3gsl4spanIKmEESA_SA_SA_RSA_
```

Confirm that the closed archives do not reference them:

```sh
for k230_archive in "$K230_MODULE" "$K230_FUNCTIONAL"; do
    echo "checking $k230_archive"
    "$K230_NM" -u "$k230_archive" \
        | grep -E 'layernorm_impl|reduce_(max|min|sum|prod)_impl|log_softmax_(block|step)|tile_impl'
done
```

No matches are expected. Repeat this audit whenever the nncase version,
compiler, or source patches change.

The v2.11.0 closed archives advertise V in their ELF attributes, although the
same disassembly search found no decoded RVV instructions in either archive.
That discrepancy is probably caused by global build flags. It is encouraging,
but because the backend source is unavailable it cannot replace a real
small-core execution test.

## 10. Relink TinyTag as an ABI/KPU probe

This optional step validates that the scalar generic runtime can link with the
closed backend and the current application. First build normal TinyTag so its
objects and generated `link.txt` exist:

```sh
cd "$K230_SDK_ROOT"
make CONF=k230_canmv_defconfig tinytag_detect
```

Create a separate link script that substitutes only the generic runtime and
changes the output path:

```sh
export TINYTAG_BUILD="$K230_SDK_ROOT/output/k230_canmv_defconfig/build/tinytag_detect/buildroot-build"

sed \
  -e "s# -lNncase.Runtime.Native # $NNCASE_SCALAR #" \
  -e 's# -o tinytag_detect.elf # -o /tmp/tinytag_detect.scalar-nncase.elf #' \
  "$TINYTAG_BUILD/CMakeFiles/tinytag_detect.elf.dir/link.txt" \
  > /tmp/link-tinytag-scalar-nncase.sh

(cd "$TINYTAG_BUILD" && sh /tmp/link-tinytag-scalar-nncase.sh)
file /tmp/tinytag_detect.scalar-nncase.elf
```

Do not overwrite the normal SDK binary with this file. Despite its name, this
probe executable as a whole is **not RVV-free**: the current Buildroot
configuration compiles application code with C908V/RVV options, and TinyTag
explicitly links `libapriltag_rvv.a`. It is suitable on the big core for
testing scalar-nncase/closed-backend ABI compatibility and KPU behavior. It is
not suitable for execution on the small core.

## 11. Copy and run the big-core compatibility probe

Copy it under a distinct name:

```sh
scp /tmp/tinytag_detect.scalar-nncase.elf \
    root@10.111.41.234:/root/app/tinytag_detect/tinytag_detect.scalar-nncase.elf
```

If SSH login is available, run it from the application directory after
stopping any process that owns the camera/display:

```sh
ssh root@10.111.41.234
cd /root/app/tinytag_detect
chmod +x tinytag_detect.scalar-nncase.elf
./tinytag_detect.scalar-nncase.elf \
    tinytag-v11_k230-v4c.int8.kmodel None 0.35 20 1.5 0
```

Verify at minimum:

- interpreter creation and kmodel loading;
- AI2D setup and invocation;
- KPU invocation and output retrieval;
- detection output parity with the distributed runtime;
- absence of illegal-instruction, page-fault, KPU, IOMMU, and MMZ errors;
- inference and end-to-end timing.

This big-core run validates the hybrid library ABI and backend behavior, but
not small-core ISA safety.

### 11.1 Result recorded on 2026-09-04

The probe was copied to `root@10.111.41.234` and run as:

```sh
./tinytag_detect.scalar-nncase.elf ./tinytag-v11_k230-v4c.int8.kmodel None 0.2 10 1.3 2
```

It completed a live-camera run successfully. One representative frame
reported five proposals and correctly decoded two tags, IDs 25 and 14, with
zero Hamming errors. Representative timing was:

```
memcpy + cache sync                 0.444 ms
AI2D invocation                    1.937 ms
total pre-process                  2.442 ms
KPU run                            2.213 ms
get output                         0.004 ms
sigmoid + NMS                      0.344 ms
threshold + top-k                  0.013 ms
decode + ROI expand + clamp        0.004 ms
ROI IoU suppression                0.006 ms
CV crop decode, five ROIs         55.204 ms
```

The process released VICAP/MIPI and ISP normally and cleaned up memory. There
was no illegal instruction, nncase load/invoke failure, KPU fault, or memory
fault. This confirms that the scalar `libNncase.Runtime.Native.a` is ABI-
compatible with the retained v2.11.0 K230 backend for this real model and
execution path.

The roughly 55 ms crop-decode stage is outside the scalar nncase experiment.
Because `TINYTAG_CV_DETECTOR` was unset, this run used the reference C decoder
at `quad_decimate=1.0`; the RVV decoder was linked into the executable but was
not selected. The result reinforces the intended AMP split--keep AI2D/KPU
under Linux and move ROI decoding to the big core--but says nothing yet about
whole-application small-core ISA safety. A subsequent TinyTag change made
RVV the production default while retaining factor 1; set
`TINYTAG_CV_DETECTOR=c` to select the now demo-aligned C fallback.

## 12. Requirements for a genuinely RVV-free small-core application

Replacing one archive is not enough. A small-core Linux image must also:

1. Compile the application and `ai_demo_common` without `-mcpu=c908v`,
   `-mrvv-auto-vectorize`, V-bearing `-march`, or RVV compatibility flags.
2. Audit every static and shared library pulled into the executable. The
   linker can extract a single RVV-bearing object from an otherwise unused
   archive.
3. Remove or replace TinyTag's direct `libapriltag_rvv.a` dependency. Use a
   scalar fallback for bring-up or send ROI work to the big-core service.
4. Keep the scalar v2.11.0 generic runtime paired with v2.11.0 K230 backend
   archives.
5. Ensure the small-core kernel configuration includes the current KPU, AI2D,
   MMZ/IOMMU, camera, and display drivers needed by the application.
6. Audit the final executable with `readelf -A` and `objdump -d`, then execute
   it on the small core.

For the intended TinyTag AMP pipeline, small-core Linux can own camera,
AI2D/KPU inference, proposal generation, display, and control. The big core can
decode proposed ROI crops concurrently. At a 120 fps camera rate, the roughly
2.5 ms NPU stage can overlap big-core ROI decoding for the preceding frame;
there is no need to assign complete frames independently to unequal cores.

## 13. Integration recommendation

Do not silently replace the normal `libnncase` package. Add an explicit
small-core variant or Buildroot configuration that:

- fetches the pinned nncase source tag;
- carries the two small source patches above;
- builds and installs only the scalar `libNncase.Runtime.Native.a`;
- obtains the two closed archives and public headers from the matching K230
  release package;
- fails the build if the source and closed package versions differ;
- runs the ISA and undefined-symbol audits as a post-build check.

Keep the current prebuilt RVV runtime for big-core Linux. This makes both
configurations reproducible and avoids degrading optional CPU fallback
performance on the big core.

## 14. Remaining proof points

The experiment removes the known open-runtime RVV dependency, but the work is
not complete until all of the following pass on small-core Linux:

- a minimal interpreter/kmodel smoke test;
- AI2D-only tests (crop, resize, pad, affine as applicable);
- TinyTag KPU inference with known input and output comparison;
- sustained live-camera execution;
- final executable instruction audit;
- performance and error-log capture.

Treat an illegal-instruction-free, output-correct small-core run as the
decisive result. The closed archives' lack of decoded RVV instructions is a
strong reason to run the experiment, not a guarantee.
