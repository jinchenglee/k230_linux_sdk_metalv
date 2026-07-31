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

ifeq ($(BR2_PACKAGE_APRILTAG_DEMO),y)
APRILTAG_BUILD_WORKLOAD_COUNTERS = ON
else
APRILTAG_BUILD_WORKLOAD_COUNTERS = OFF
endif

APRILTAG_CONF_OPTS = \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_PYTHON_WRAPPER=OFF \
	-DBUILD_TESTING=OFF \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_WORKLOAD_COUNTERS=$(APRILTAG_BUILD_WORKLOAD_COUNTERS)

$(eval $(cmake-package))
