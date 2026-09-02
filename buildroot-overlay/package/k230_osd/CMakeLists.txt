cmake_minimum_required(VERSION 3.10)
project(k230_osd LANGUAGES C CXX)

set(src src/k230_osd.cc)
add_library(k230_osd STATIC ${src})

target_compile_features(k230_osd PUBLIC cxx_std_17)
target_include_directories(k230_osd PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(k230_osd PRIVATE -O3
    # Match the K230 C908 target (implied T-Head extensions) so the static
    # inline dcache.civa in thead.h assembles; without -mcpu=c908v the
    # default rv64gc march doesn't enable xtheadcmo.
    -mcpu=c908v -mabi=lp64d)

# display (libdisplay) provides the DRM dumb-buffer/plane API (via .pc);
# OpenCV provides the cv::Mat canvas. Both are also needed by consumers, so
# propagate them PUBLIC so a consumer linking k230_osd gets opencv + display.
find_package(OpenCV REQUIRED)
include(FindPkgConfig)
pkg_check_modules(DISPLAY REQUIRED IMPORTED_TARGET display)

target_link_libraries(k230_osd PUBLIC
    PkgConfig::DISPLAY
    opencv_core
    opencv_imgproc
    drm
    pthread
)

install(TARGETS k230_osd ARCHIVE DESTINATION lib)
install(FILES include/k230_osd.h DESTINATION include)
