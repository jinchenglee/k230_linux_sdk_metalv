################################################################################
#
# Metal-V K230 AMP console payload
#
################################################################################

METAL_V_AMP_SITE = $(realpath $(TOPDIR))/package/metal_v_amp/src
METAL_V_AMP_SITE_METHOD = local
METAL_V_AMP_INSTALL_IMAGES = YES

define METAL_V_AMP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS_COMPILE="$(TARGET_CROSS)"
endef

define METAL_V_AMP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.bin \
		$(TARGET_DIR)/root/amp/metal-v-k230.bin
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.elf \
		$(TARGET_DIR)/root/amp/metal-v-k230.elf
endef

define METAL_V_AMP_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.bin \
		$(BINARIES_DIR)/metal-v-k230.bin
	$(INSTALL) -D -m 0644 $(@D)/metal-v-k230.elf \
		$(BINARIES_DIR)/metal-v-k230.elf
endef

$(eval $(generic-package))
