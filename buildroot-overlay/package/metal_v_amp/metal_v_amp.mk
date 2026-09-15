################################################################################
#
# Metal-V K230 AMP console payload
#
################################################################################

METAL_V_AMP_SITE = $(realpath $(TOPDIR))/package/metal_v_amp/src
METAL_V_AMP_SITE_METHOD = local
METAL_V_AMP_INSTALL_IMAGES = YES
METAL_V_AMP_DEPENDENCIES = vvcam libmmz k230_amp_camera_pool

define METAL_V_AMP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS_COMPILE="$(TARGET_CROSS)" \
		LINUX_CC="$(TARGET_CC)" \
		LINUX_CFLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include/libdrm" \
		RPMSG_LITE_DIR="$(realpath $(TOPDIR)/../../third_party/rpmsg-lite)"
endef

define METAL_V_AMP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.bin \
		$(TARGET_DIR)/root/amp/metal-v-k230.bin
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.elf \
		$(TARGET_DIR)/root/amp/metal-v-k230.elf
	$(INSTALL) -D -m 0755 $(@D)/amp-shm-test \
		$(TARGET_DIR)/root/amp/amp-shm-test
	$(INSTALL) -D -m 0755 $(@D)/rpmsg-echo-test \
		$(TARGET_DIR)/root/amp/rpmsg-echo-test
	$(INSTALL) -D -m 0755 $(@D)/rpmsg-slot-test \
		$(TARGET_DIR)/root/amp/rpmsg-slot-test
	$(INSTALL) -D -m 0755 $(@D)/v4l2-dma-probe \
		$(TARGET_DIR)/root/amp/v4l2-dma-probe
	$(INSTALL) -D -m 0755 $(@D)/rpmsg-zero-copy-camera \
		$(TARGET_DIR)/root/amp/rpmsg-zero-copy-camera
	$(INSTALL) -D -m 0755 $(@D)/run-zero-copy-camera.sh \
		$(TARGET_DIR)/root/amp/run-zero-copy-camera.sh
	$(INSTALL) -D -m 0755 $(@D)/rpmsg-regression.sh \
		$(TARGET_DIR)/root/amp/rpmsg-regression.sh
	$(INSTALL) -D -m 0755 $(@D)/amp-shm-cost.sh \
		$(TARGET_DIR)/root/amp/amp-shm-cost.sh
	$(INSTALL) -D -m 0755 $(@D)/rcS.profile \
		$(TARGET_DIR)/root/amp/rcS.profile
	$(INSTALL) -D -m 0755 $(@D)/rcS.fast \
		$(TARGET_DIR)/root/amp/rcS.fast
	$(INSTALL) -D -m 0755 $(@D)/rcS.full \
		$(TARGET_DIR)/root/amp/rcS.full
	$(INSTALL) -D -m 0755 $(@D)/amp-boot-profile \
		$(TARGET_DIR)/usr/sbin/amp-boot-profile
endef

define METAL_V_AMP_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.bin \
		$(BINARIES_DIR)/metal-v-k230.bin
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.elf \
		$(BINARIES_DIR)/metal-v-k230.elf
endef

$(eval $(generic-package))
