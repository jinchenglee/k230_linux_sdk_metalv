################################################################################
#
# aruco2
#
################################################################################

ARUCO2_VERSION = 93bfda4f5fec85037a3d3baa8daeed06cfe37c3c
ARUCO2_SITE = $(call github,rmsalinas,aruco2,$(ARUCO2_VERSION))
ARUCO2_LICENSE = Apache-2.0
ARUCO2_LICENSE_FILES = LICENSE
ARUCO2_INSTALL_STAGING = YES
ARUCO2_INSTALL_TARGET = NO
ARUCO2_DEPENDENCIES = opencv4
ARUCO2_SUPPORTS_IN_SOURCE_BUILD = NO
ARUCO2_CONF_OPTS = -DARUCO2_BUILD_UTILS=OFF -DARUCO2_DETECTOR_ONLY=ON

$(eval $(cmake-package))
