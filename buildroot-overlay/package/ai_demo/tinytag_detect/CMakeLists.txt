cmake_minimum_required(VERSION 3.10)
if(CMAKE_TOOLCHAIN_FILE MATCHES "buildroot/toolchainfile.cmake")
    get_filename_component(CURRENT_DIR_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
    project(${CURRENT_DIR_NAME})
    set(src main.cc tinytag_det.cc tag_crop_decoder.cc video_hw_codec.cc)
    include(../common/ai_demo_cml_common)

    # ai_demo_cml_common's link line doesn't know about apriltag (no other
    # ai_demo package needs it); add it here for the CV crop-decode stage.
    # apriltag headers/lib are found via the cross toolchain's --sysroot
    # default search paths (same as ai_demo_cml_common's own opencv/display
    # libs), so no extra include_directories/link_directories needed.
    #
    # opencv_videoio: mp4-file input/output mode (cv::VideoCapture /
    # cv::VideoWriter), the software video codec fallback path
    # (video_hw_codec.cc's hardware path is the default -- see
    # experimental/README.md).
    #
    # avcodec/avformat/avutil/swscale: video_hw_codec.cc's direct libavcodec
    # use of the hardware h264_v4l2m2m decoder/encoder (~4.6x/~9x faster
    # than the software cv::VideoCapture/VideoWriter path measured on real
    # hardware -- see experimental/README.md). These are already present in
    # the sysroot as opencv_videoio's own transitive dependencies, but
    # opencv_videoio doesn't re-export their symbols to our own direct API
    # calls, so they need to be linked explicitly here too.
    target_link_libraries(${bin} PRIVATE apriltag opencv_videoio avcodec avformat avutil swscale k230_osd)

    # apriltag-rvv: the same RVV-accelerated AprilTag detector
    # apriltag_demo.elf's --rvv mode uses, offered here as an alternative
    # TagCropDecoder backend (TINYTAG_CV_DETECTOR=rvv, see
    # tag_crop_decoder.h's AprilTagRVVDecoder / make_crop_decoder()).
    # Prebuilt static lib checked into apriltag_demo's own tree (not built
    # from Rust source by this package) -- linked by full path since it
    # isn't installed anywhere -lapriltag_rvv would find it.
    # dl/m/rt: apriltag_demo's own CMakeLists.txt link line for this same
    # lib names these as its required system deps (Rust-std runtime),
    # reused verbatim here. --start-group/--end-group: resolves mutual
    # deps between the rust-std symbols this lib pulls and pthread (already
    # linked above via ai_demo_cml_common) -- same reason apriltag_demo's
    # own link line uses it.
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../apriltag_demo/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../apriltag_demo/lib
    )
    target_link_libraries(${bin} PRIVATE
        -Wl,--start-group
        ${CMAKE_CURRENT_SOURCE_DIR}/../apriltag_demo/lib/libapriltag_rvv.a
        dl
        m
        rt
        -Wl,--end-group
    )

    # tinytag-v11_k230-v4c.int8.kmodel is the K230-safe build: the
    # previous tinytag-v4c-context.* kmodels (removed from utils/ here) hit
    # a known nncase-Simulator-vs-real-KPU execution mismatch on this
    # hardware. On real hardware this wasn't just wrong detections: it
    # corrupted device memory badly enough to segfault unrelated processes
    # (observed live on a 01studio board, 2026-08-22 -- kernel log showed
    # garbage-register SIGSEGVs in `sleep` and an init.d script, not just
    # the AI process). v11_k230-v4c
    # (experimental/tinytag/runs/tinytag-v11_k230-v4c-80ep/) is confirmed
    # safe on real K230 hardware and is a drop-in replacement -- no
    # app-code changes were needed, just this kmodel swap. Do not swap in
    # a different/older kmodel here without first confirming on real
    # hardware that it doesn't reproduce this corruption.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/utils/tinytag-v11_k230-v4c.int8.kmodel
            DESTINATION /root/app/${PROJECT_NAME}/)
else()
    message(FATAL_ERROR
        "tinytag_detect: standalone (non-Buildroot) build is not implemented, "
        "only the real SDK build via 'make CONF=<defconfig> tinytag_detect'. "
        "See object_detect_yolov8n/build_app.sh for the pattern if this is needed later.")
endif()
