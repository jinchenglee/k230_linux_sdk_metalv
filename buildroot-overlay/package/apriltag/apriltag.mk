################################################################################
#
# apriltag
#
################################################################################

APRILTAG_VERSION = 3.4.5
APRILTAG_SITE = $(call github,AprilRobotics,apriltag,v$(APRILTAG_VERSION))
APRILTAG_LICENSE = BSD-2-Clause
APRILTAG_LICENSE_FILES = LICENSE.md
APRILTAG_INSTALL_STAGING = YES
APRILTAG_INSTALL_TARGET = NO

APRILTAG_CONF_OPTS = \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_PYTHON_WRAPPER=OFF \
	-DBUILD_TESTING=OFF \
	-DBUILD_SHARED_LIBS=OFF

$(eval $(cmake-package))
