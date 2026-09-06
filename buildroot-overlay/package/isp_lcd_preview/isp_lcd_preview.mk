################################################################################
#
# isp_lcd_preview — universal live camera preview for the K230.
#
# Standalone, core-agnostic preview utility driven by the actual display
# connector geometry. Built against the v4l2-drm and display libraries.
#
################################################################################

ISP_LCD_PREVIEW_SITE = $(realpath $(TOPDIR))"/package/isp_lcd_preview/src"
ISP_LCD_PREVIEW_SITE_METHOD = local
ISP_LCD_PREVIEW_DEPENDENCIES += display vvcam

define ISP_LCD_PREVIEW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		STAGING_DIR="$(STAGING_DIR)" -C $(@D)
endef

define ISP_LCD_PREVIEW_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/bin
	$(INSTALL) -m 0755 $(@D)/isp_lcd_preview $(TARGET_DIR)/usr/bin/isp_lcd_preview
endef

$(eval $(generic-package))
