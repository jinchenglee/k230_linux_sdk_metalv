################################################################################
#
# k230_amp_mailbox
#
################################################################################

K230_AMP_MAILBOX_SITE = $(realpath $(TOPDIR))/package/k230_amp_mailbox/src
K230_AMP_MAILBOX_SITE_METHOD = local
K230_AMP_MAILBOX_SUPPORTS_IN_SOURCE_BUILD = NO

define K230_AMP_MAILBOX_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/S29k230_amp_mailbox \
		$(TARGET_DIR)/etc/init.d/S29k230_amp_mailbox
endef

$(eval $(kernel-module))
$(eval $(generic-package))
