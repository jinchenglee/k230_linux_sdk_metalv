cmake_minimum_required(VERSION 3.13)
set(CMAKE_CXX_STANDARD 17)
project(apriltag_demo)

message("CMAKE_TOOLCHAIN_FILE:" ${CMAKE_TOOLCHAIN_FILE})

set(common_src
    src/main.cc
    src/apriltag_draw.cc
)
set(rvv_bin apriltag_demo.elf)
set(c_bin apriltag_c_demo.elf)
set(bench_bin k230_apriltag_bench)
set(bench_test_bin apriltag_bench_tests)
set(workload_bin k230_apriltag_workload)
set(workload_test_bin apriltag_workload_format_tests)
set(APRILTAG_RVV_GIT_ID "unversioned" CACHE STRING "apriltag-rvv git identity")
set(APRILTAG_SDK_GIT_ID "unversioned" CACHE STRING "SDK git identity")
set(APRILTAG_RVV_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/libapriltag_rvv.a)
set(APRILTAG_RVV_WORKLOAD_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/libapriltag_rvv_workload.a)

add_executable(${rvv_bin} ${common_src})
add_executable(${c_bin}
    ${common_src}
    src/apriltag_c_adapter.cc
)
add_executable(${bench_bin}
    bench/main.cc
    bench/benchmark.cc
    bench/visual_dump.cc
    bench/rust_backend.cc
    bench/c_backend.cc
)
add_executable(${bench_test_bin}
    bench/tests.cc
    bench/benchmark.cc
    bench/visual_dump.cc
    bench/rust_backend.cc
    bench/c_backend.cc
)
add_executable(${workload_bin}
    bench/workload_main.cc bench/workload_rust.cc bench/workload_c.cc
    bench/benchmark.cc bench/visual_dump.cc)
add_executable(${workload_test_bin}
    bench/workload_tests.cc bench/workload_main.cc bench/benchmark.cc)
target_compile_definitions(${workload_test_bin} PRIVATE
    APRILTAG_WORKLOAD_FORMAT_TEST APRILTAG_BENCH_NO_OPENCV APRILTAG_BENCH_CORE_ONLY)
target_compile_definitions(${workload_bin} PRIVATE APRILTAG_BENCH_CORE_ONLY)
target_include_directories(${workload_bin} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_compile_options(${bench_bin} PRIVATE -fno-omit-frame-pointer)
target_compile_definitions(${bench_test_bin} PRIVATE
    APRILTAG_BENCH_BACKEND_TESTS
    APRILTAG_BENCH_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/bench/fixture.jpg"
)
target_compile_definitions(${c_bin} PRIVATE
    APRILTAG_C_BACKEND
    APRILTAG_C_VERSION="3.4.5"
)

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -mcpu=c908v -mabi=lp64d -mtune=c908 -mrvv-v0p10-compatible -mrvv-auto-vectorize")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=c908v -mabi=lp64d -mtune=c908 -mrvv-v0p10-compatible -mrvv-auto-vectorize")

if(CMAKE_TOOLCHAIN_FILE MATCHES "buildroot/toolchainfile.cmake")
    # Buildroot environment — compilers come from the toolchain file.
else()
    # Standalone build via build_app.sh.
    set(CMAKE_C_COMPILER $ENV{GCC_PATH}/riscv64-unknown-linux-gnu-gcc)
    set(CMAKE_CXX_COMPILER $ENV{GCC_PATH}/riscv64-unknown-linux-gnu-g++)
    set(CMAKE_SYSROOT $ENV{sysroot})
endif()

message("CMAKE_C_COMPILER:" ${CMAKE_C_COMPILER})
message("CMAKE_CXX_COMPILER:" ${CMAKE_CXX_COMPILER})
message("CMAKE_SYSROOT:" ${CMAKE_SYSROOT})

set(usr_root ${CMAKE_SYSROOT}/usr)

include_directories(
    ${usr_root}/include
    ${usr_root}/include/libdrm
    ${usr_root}/include/opencv4
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

foreach(bin IN ITEMS ${rvv_bin} ${c_bin} ${bench_bin} ${bench_test_bin}
                     ${workload_bin})
    target_link_directories(${bin} PRIVATE
        ${usr_root}/lib
        ${CMAKE_SYSROOT}
        ${CMAKE_SYSROOT}/usr/lib64/lp64d
    )
endforeach()

# Prebuilt Rust static library (built in the rvv-dev docker image; see
# scripts/build_rust_lib.sh). Contains the apriltag_* C ABI + RVV kernels.
set(APRILTAG_C_LIB ${usr_root}/lib/libapriltag.a)
set(APRILTAG_C_WORKLOAD_LIB ${usr_root}/lib/libapriltag_workload.a)

# Hash the exact archives, ABI header, and sources CMake will consume. The
# compiler fields identify the configured cross-toolchain without hashing the
# final executable (which would make the build ID self-referential).
file(SHA256 "${APRILTAG_RVV_LIB}" APRILTAG_RVV_LIB_SHA256)
file(SHA256 "${APRILTAG_C_LIB}" APRILTAG_C_LIB_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/apriltag.h" APRILTAG_ABI_SHA256)
file(GLOB APRILTAG_BENCH_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.h"
)
list(APPEND APRILTAG_BENCH_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT APRILTAG_BENCH_SOURCE_FILES)
set(APRILTAG_BENCH_SOURCE_CONTENTS "")
foreach(source_file IN LISTS APRILTAG_BENCH_SOURCE_FILES)
    file(READ "${source_file}" source_contents)
    file(RELATIVE_PATH source_name "${CMAKE_CURRENT_SOURCE_DIR}" "${source_file}")
    string(APPEND APRILTAG_BENCH_SOURCE_CONTENTS
        "${source_name}\n${source_contents}\n")
endforeach()
string(SHA256 APRILTAG_BENCH_SOURCE_SHA256 "${APRILTAG_BENCH_SOURCE_CONTENTS}")
string(SUBSTRING "${APRILTAG_RVV_LIB_SHA256}" 0 12 APRILTAG_RVV_LIB_HASH)
string(SUBSTRING "${APRILTAG_C_LIB_SHA256}" 0 12 APRILTAG_C_LIB_HASH)
string(SUBSTRING "${APRILTAG_ABI_SHA256}" 0 12 APRILTAG_ABI_HASH)
string(SUBSTRING "${APRILTAG_BENCH_SOURCE_SHA256}" 0 12 APRILTAG_BENCH_SOURCE_HASH)
set(APRILTAG_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_COMPILER_VERSION}")
string(REGEX REPLACE "[^A-Za-z0-9._-]" "-" APRILTAG_COMPILER_ID
    "${APRILTAG_COMPILER_ID}")
set(APRILTAG_BENCH_BUILD_ID
    "rvv-${APRILTAG_RVV_GIT_ID}_sdk-${APRILTAG_SDK_GIT_ID}_lib-${APRILTAG_RVV_LIB_HASH}_bench-${APRILTAG_BENCH_SOURCE_HASH}_clib-${APRILTAG_C_LIB_HASH}_abi-${APRILTAG_ABI_HASH}_cc-${APRILTAG_COMPILER_ID}")
string(REGEX REPLACE "[^A-Za-z0-9._-]" "-" APRILTAG_BENCH_BUILD_ID
    "${APRILTAG_BENCH_BUILD_ID}")
message(STATUS "AprilTag benchmark build ID: ${APRILTAG_BENCH_BUILD_ID}")
foreach(bin IN ITEMS ${bench_bin} ${bench_test_bin})
    target_compile_definitions(${bin} PRIVATE
        APRILTAG_BENCH_BUILD_ID="${APRILTAG_BENCH_BUILD_ID}"
    )
endforeach()

# --start-group/--end-group resolves the mutual deps between the OpenCV/K230
# static libs and the rust-std symbols pulled by libapriltag_rvv.a.
target_link_libraries(${rvv_bin}
    -Wl,--start-group
    ${APRILTAG_RVV_LIB}
    opencv_videoio
    opencv_imgcodecs
    opencv_imgproc
    opencv_core
    v4l2-drm
    drm
    display
    mmz
    pthread
    dl
    m
    rt
    -Wl,--end-group
)

target_link_libraries(${c_bin}
    -Wl,--start-group
    ${APRILTAG_C_LIB}
    opencv_videoio
    opencv_imgcodecs
    opencv_imgproc
    opencv_core
    v4l2-drm
    drm
    display
    mmz
    pthread
    dl
    m
    rt
    -Wl,--end-group
)

# The fixed-image benchmark deliberately excludes all camera, DRM, display, and
# MMZ libraries so process-level perf counters cover detector work only.
target_link_libraries(${bench_bin}
    -Wl,--start-group
    ${APRILTAG_RVV_LIB}
    ${APRILTAG_C_LIB}
    opencv_imgcodecs
    opencv_imgproc
    opencv_core
    pthread
    dl
    m
    rt
    -Wl,--end-group
)
target_link_libraries(${bench_test_bin}
    -Wl,--start-group
    ${APRILTAG_RVV_LIB}
    ${APRILTAG_C_LIB}
    opencv_imgcodecs
    opencv_imgproc
    opencv_core
    pthread
    dl
    m
    rt
    -Wl,--end-group
)
target_link_libraries(${workload_bin}
    -Wl,--start-group
    ${APRILTAG_RVV_WORKLOAD_LIB}
    ${APRILTAG_C_WORKLOAD_LIB}
    opencv_imgcodecs opencv_imgproc opencv_core pthread dl m rt
    -Wl,--end-group)

if(CMAKE_TOOLCHAIN_FILE MATCHES "buildroot/toolchainfile.cmake")
    message(STATUS "Buildroot build -> AprilTag demos, profile tools, and benchmark")
    install(TARGETS ${rvv_bin} DESTINATION /root/app/apriltag_demo)
    install(DIRECTORY utils/ DESTINATION /root/app/apriltag_demo USE_SOURCE_PERMISSIONS)
    install(TARGETS ${c_bin} DESTINATION /root/app/apriltag_c_demo)
    install(DIRECTORY utils_c/ DESTINATION /root/app/apriltag_c_demo USE_SOURCE_PERMISSIONS)
    install(DIRECTORY utils_profile/ DESTINATION /root/app/apriltag_profile USE_SOURCE_PERMISSIONS)
    install(TARGETS ${bench_bin} ${workload_bin} DESTINATION /root/app/apriltag_bench)
    install(FILES bench/fixture.jpg DESTINATION /root/app/apriltag_bench)
    install(DIRECTORY utils_bench/ DESTINATION /root/app/apriltag_bench USE_SOURCE_PERMISSIONS)
else()
    message(STATUS "Standalone build -> ./bin/")
    install(TARGETS ${rvv_bin} ${c_bin} ${bench_bin} ${workload_bin} DESTINATION bin)
endif()
