################################################################################
#
# k230_amp_camera_pool
#
################################################################################

K230_AMP_CAMERA_POOL_SITE = $(realpath $(TOPDIR))/package/k230_amp_camera_pool/src
K230_AMP_CAMERA_POOL_SITE_METHOD = local
K230_AMP_CAMERA_POOL_SUPPORTS_IN_SOURCE_BUILD = NO
K230_AMP_CAMERA_POOL_INSTALL_STAGING = YES

define K230_AMP_CAMERA_POOL_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/k230_amp_camera_pool.h \
		$(STAGING_DIR)/usr/include/k230_amp_camera_pool.h
endef

define K230_AMP_CAMERA_POOL_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/S30k230_amp_camera_pool \
		$(TARGET_DIR)/etc/init.d/S30k230_amp_camera_pool
endef

$(eval $(kernel-module))
$(eval $(generic-package))
