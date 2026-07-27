cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)
project(apriltag_demo)

message("CMAKE_TOOLCHAIN_FILE:" ${CMAKE_TOOLCHAIN_FILE})

file(GLOB src CONFIGURE_DEPENDS src/*.cc)
set(bin apriltag_demo.elf)

add_executable(${bin} ${src})

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

target_link_directories(${bin} PRIVATE
    ${usr_root}/lib
    ${CMAKE_SYSROOT}
    ${CMAKE_SYSROOT}/usr/lib64/lp64d
)

# Prebuilt Rust static library (built in the rvv-dev docker image; see
# scripts/build_rust_lib.sh). Contains the apriltag_* C ABI + RVV kernels.
set(APRILTAG_RVV_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/libapriltag_rvv.a)

# --start-group/--end-group resolves the mutual deps between the OpenCV/K230
# static libs and the rust-std symbols pulled by libapriltag_rvv.a.
target_link_libraries(${bin}
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

if(CMAKE_TOOLCHAIN_FILE MATCHES "buildroot/toolchainfile.cmake")
    message(STATUS "Buildroot build -> /root/app/apriltag_demo/")
    install(TARGETS ${bin} DESTINATION /root/app/apriltag_demo)
    install(DIRECTORY utils/ DESTINATION /root/app/apriltag_demo USE_SOURCE_PERMISSIONS)
else()
    message(STATUS "Standalone build -> ./bin/")
    install(TARGETS ${bin} DESTINATION bin)
endif()
