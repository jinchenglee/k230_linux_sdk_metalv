cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)
project(ai_demo)

#add_definitions(-DWITH_OPENCV)
# The vector flags below are appended after Buildroot's own
# BR2_TARGET_OPTIMIZATION and therefore override it. On a small-core (AMP)
# configuration the CPU has no V extension, so applying them unconditionally
# produces binaries that take an illegal-instruction trap before main() --
# GCC auto-vectorizes even static initializers. Bind them to the Kconfig
# symbol instead; ai_mk_common passes it in.
if(BR2_RISCV_ISA_RVV)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -mcpu=c908v -mabi=lp64d  -mtune=c908 -mrvv-v0p10-compatible  -mrvv-auto-vectorize")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}   -mcpu=c908v -mabi=lp64d  -mtune=c908 -mrvv-v0p10-compatible  -mrvv-auto-vectorize")
endif()

# 设置编译器路径
set(CMAKE_C_COMPILER $ENV{GCC_PATH}/riscv64-unknown-linux-gnu-gcc)
set(CMAKE_SYSROOT $ENV{sysroot})

message("CMAKE_SYSROOT =" ${CMAKE_SYSROOT})

include_directories(./common)
include_directories(${CMAKE_SYSROOT}/usr/include/libdrm)
include_directories(${CMAKE_SYSROOT}/usr/include/freetype2)
include_directories(${CMAKE_SYSROOT}/usr/include/alsa)
include_directories(${CMAKE_SYSROOT}/usr/include/opencv4)


# 定义要排除的目录列表
set(exclude_dirs "k230_bin" "shell" "out" ".git" "common" "kmodel")

file(GLOB files "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(file ${files})
    # 判断当前文件是否是一个目录
    if(IS_DIRECTORY ${file})
        # 获取目录名称
        get_filename_component(dir_name ${file} NAME)
        # 检查目录是否在排除列表中
        list(FIND exclude_dirs ${dir_name} is_exclude_dir)
        # 如果目录不在排除列表中，则处理该目录
        if(${is_exclude_dir} EQUAL -1)
            # 处理该目录，例如添加到项目中
            if(${dir_name})
                add_subdirectory(${dir_name})
            endif()
        endif()
    endif()
endforeach()
