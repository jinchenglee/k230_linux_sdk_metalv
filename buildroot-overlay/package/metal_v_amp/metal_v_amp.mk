################################################################################
#
# Metal-V K230 AMP console payload
#
################################################################################

METAL_V_AMP_SITE = $(realpath $(TOPDIR))/package/metal_v_amp/src
METAL_V_AMP_SITE_METHOD = local
METAL_V_AMP_INSTALL_IMAGES = YES

define METAL_V_AMP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS_COMPILE="$(TARGET_CROSS)" \
		LINUX_CC="$(TARGET_CC)" LINUX_CFLAGS="$(TARGET_CFLAGS)" \
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
