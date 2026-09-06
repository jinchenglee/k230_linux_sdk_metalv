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

# Scalar-core builds: report where any remaining vector instructions come from.
# This is deliberately report-only. tinytag_detect links the distributed RVV
# libNncase.Runtime.Native.a, and until the small-core libnncase of
# rvv-free-nncase-v2.11.0.md section 13 is packaged, a hard failure here would
# make the small-core image unbuildable. Drop --report-only once that lands.
# See docs/notes/small-core-rvv-pollution.md.
ifneq ($(BR2_RISCV_ISA_RVV),y)
define TINYTAG_DETECT_AUDIT_VECTOR_FREE
	bash $(TOPDIR)/package/apriltag_demo/scripts/audit_vector_free.sh \
		--report-only $(TARGET_OBJDUMP) \
		$(@D)/buildroot-build/tinytag_detect.elf
endef
TINYTAG_DETECT_POST_INSTALL_TARGET_HOOKS += TINYTAG_DETECT_AUDIT_VECTOR_FREE
endif
