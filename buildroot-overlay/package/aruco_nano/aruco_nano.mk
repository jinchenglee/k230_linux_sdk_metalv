################################################################################
#
# aruco nano
#
################################################################################

ARUCO_NANO_VERSION = 961b18b747d64cc3692c570dff35334c098b9e62
ARUCO_NANO_SITE = $(call github,rmsalinas,aruco_nano,$(ARUCO_NANO_VERSION))
ARUCO_NANO_LICENSE = MIT, Apache-2.0
ARUCO_NANO_LICENSE_FILES = LICENSE LICENSE.opencv
ARUCO_NANO_INSTALL_STAGING = YES
ARUCO_NANO_INSTALL_TARGET = NO
ARUCO_NANO_DEPENDENCIES = opencv4

ARUCO_NANO_OPENCV_OBJDETECT_DIR = $(OPENCV4_DIR)/modules/objdetect

define ARUCO_NANO_COPY_OPENCV_LICENSE
	cp $(OPENCV4_DIR)/LICENSE $(@D)/LICENSE.opencv
endef
ARUCO_NANO_POST_EXTRACT_HOOKS += ARUCO_NANO_COPY_OPENCV_LICENSE

# Nano calls cv::aruco::Dictionary::identify but does not use OpenCV's
# detector. Build that exact OpenCV 4.10 translation unit alone to avoid the
# full objdetect -> calib3d -> features2d/flann/ml dependency chain.
define ARUCO_NANO_BUILD_CMDS
	cp $(ARUCO_NANO_OPENCV_OBJDETECT_DIR)/src/aruco/aruco_dictionary.cpp \
		$(@D)/opencv_aruco_dictionary_lite.cpp
	sed -i 's|#include "../precomp.hpp"|#include <opencv2/core.hpp>\n#include <opencv2/imgproc.hpp>\n#include <array>\n#include <numeric>\n#include <sstream>\n#include <vector>|' \
		$(@D)/opencv_aruco_dictionary_lite.cpp
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -std=c++17 -c \
		$(@D)/opencv_aruco_dictionary_lite.cpp \
		-o $(@D)/opencv_aruco_dictionary_lite.o \
		-I$(ARUCO_NANO_OPENCV_OBJDETECT_DIR)/include \
		-I$(ARUCO_NANO_OPENCV_OBJDETECT_DIR)/src/aruco \
		-I$(STAGING_DIR)/usr/include/opencv4
	$(TARGET_AR) rcs $(@D)/libaruco_nano_dictionary.a \
		$(@D)/opencv_aruco_dictionary_lite.o
endef

define ARUCO_NANO_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/aruco_nano.h \
		$(STAGING_DIR)/usr/include/aruco_nano/aruco_nano.h
	$(INSTALL) -D -m 0644 \
		$(ARUCO_NANO_OPENCV_OBJDETECT_DIR)/include/opencv2/objdetect/aruco_dictionary.hpp \
		$(STAGING_DIR)/usr/include/opencv4/opencv2/objdetect/aruco_dictionary.hpp
	$(INSTALL) -D -m 0644 $(@D)/libaruco_nano_dictionary.a \
		$(STAGING_DIR)/usr/lib/libaruco_nano_dictionary.a
endef

$(eval $(generic-package))
