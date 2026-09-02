# Must be set before $(AI_DEMO_MKF_COMMON) -- it expands $(eval $(cmake-package))
# immediately, so appending to TINYTAG_DETECT_DEPENDENCIES afterward would be
# too late for Buildroot to pick up.
# apriltag_demo: only need its checked-in lib/libapriltag_rvv.a (the
# TINYTAG_CV_DETECTOR=rvv backend, see tag_crop_decoder.h) and src/apriltag.h
# (its C ABI header) -- declared as a full package dependency anyway so a
# clean build always has apriltag_demo's tree synced first, since
# tinytag_detect.mk's own SITE_METHOD=local sync doesn't reach outside its
# own package directory.
TINYTAG_DETECT_DEPENDENCIES += apriltag ffmpeg apriltag_demo k230_osd
$(AI_DEMO_MKF_COMMON)
