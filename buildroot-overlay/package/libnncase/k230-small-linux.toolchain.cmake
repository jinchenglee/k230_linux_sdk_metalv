# Small-core (scalar C908) K230 Linux toolchain for the nncase runtime.
#
# The nncase tree ships toolchains/k230.linux.toolchain.cmake, which adds the V
# extension and -mrvv-v0p10-compatible. That is correct for the big core and
# wrong here: the small core has no vector unit, so any RVV instruction traps
# with SIGILL at runtime rather than failing the build.
#
# See docs/notes/rvv-free-nncase-v2.11.0.md section 6.

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
# Note: the sysroot's target tuple differs from the compiler executable name.
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

# -march=rv64imafdc: no V component, and no RVV auto-vectorization option.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv64imafdc -mabi=lp64d -mcmodel=medany -fstack-protector-strong")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=rv64imafdc -mabi=lp64d -mcmodel=medany -fstack-protector-strong")

set(BUILDING_RUNTIME ON)
set(ENABLE_K230_RUNTIME ON)
set(BUILD_SHARED_LIBS OFF)

add_definitions(-DLINUX_RUNTIME)
