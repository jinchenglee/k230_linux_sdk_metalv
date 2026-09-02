# Shared double-buffered ARGB OSD overlay library. Consumers (apriltag_demo,
# ai_demo/tinytag_detect, future apps) link k230_osd for the OSD buffer/swap/
# rotation plumbing. Installed to staging so other packages can build against
# its header + static archive.
K230_OSD_SITE = $(realpath $(TOPDIR))/package/k230_osd
K230_OSD_SITE_METHOD = local
K230_OSD_INSTALL_STAGING = YES
K230_OSD_INSTALL_TARGET = YES
K230_OSD_DEPENDENCIES = display opencv4
K230_OSD_SUPPORTS_IN_SOURCE_BUILD = NO

$(eval $(cmake-package))
