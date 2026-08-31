cmake_minimum_required(VERSION 3.10)
project(nn_profile LANGUAGES C CXX)

option(NN_PROFILE_K230_TUNING "Build for the K230 C908 vector core" OFF)

add_executable(nn_profile main.cc)
target_compile_features(nn_profile PRIVATE cxx_std_17)
target_compile_options(nn_profile PRIVATE -O3 -fno-omit-frame-pointer)
target_link_libraries(nn_profile PRIVATE dl m)
target_include_directories(nn_profile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(NN_PROFILE_K230_TUNING)
    target_compile_options(nn_profile PRIVATE
        -mcpu=c908v -mabi=lp64d -mtune=c908
        -mrvv-v0p10-compatible -mrvv-auto-vectorize)
    target_link_options(nn_profile PRIVATE -mabi=lp64d)
endif()

install(TARGETS nn_profile
    RUNTIME DESTINATION /root/app/nn_profile)
install(FILES include/nn_profile_plugin.h
    DESTINATION /root/app/nn_profile)
install(PROGRAMS run_profile.sh
    DESTINATION /root/app/nn_profile)

include(CTest)
if(BUILD_TESTING)
    add_library(nn_profile_mock MODULE tests/mock_model_plugin.c)
    target_include_directories(nn_profile_mock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME profile_smoke COMMAND nn_profile --model $<TARGET_FILE:nn_profile_mock> --cpu -1 --warmup 1 --iterations 3 --no-perf)
endif()
