APRILTAG_DEMO_SITE = $(realpath $(TOPDIR))"/package/apriltag_demo"
APRILTAG_DEMO_SITE_METHOD = local
APRILTAG_DEMO_DEPENDENCIES += opencv4 display vvcam libmmz apriltag binutils linux
APRILTAG_DEMO_RVV_DIR ?= $(realpath $(TOPDIR)/../../../apriltag-rvv)
# Set to YES after changing the sibling apriltag-rvv source so Buildroot does
# not reuse an older static archive from this local package's copied tree.
APRILTAG_DEMO_FORCE_RUST_REBUILD ?= NO

# Ensure the Rust staticlib (built in the rvv-dev docker image) is present in
# Buildroot's copied source tree before CMake configures. Local package sources
# are rsynced into $(@D) before pre-configure hooks run, so build the library
# there rather than in $(APRILTAG_DEMO_PKGDIR).
define APRILTAG_DEMO_BUILD_RUST_LIB
	if [ ! -f $(@D)/lib/libapriltag_rvv.a ] || \
	   [ "$(APRILTAG_DEMO_FORCE_RUST_REBUILD)" = "YES" ]; then \
		echo "apriltag_demo: building Rust library via docker..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
			bash $(@D)/scripts/build_rust_lib.sh; \
	fi
endef
APRILTAG_DEMO_PRE_CONFIGURE_HOOKS += APRILTAG_DEMO_BUILD_RUST_LIB

define APRILTAG_DEMO_BUILD_DEB
	mkdir -p $(@D)/deb/DEBIAN
	mkdir -p $(@D)/deb/root/app/
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_demo,$(@D)/deb/root/app/)
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_c_demo,$(@D)/deb/root/app/)
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_profile,$(@D)/deb/root/app/)
	echo "Package: k230-apriltag-demo"                    >  $(@D)/deb/DEBIAN/control
	echo "Version: 1.1"                                   >> $(@D)/deb/DEBIAN/control
	echo "Section: base"                                  >> $(@D)/deb/DEBIAN/control
	echo "Priority: optional"                             >> $(@D)/deb/DEBIAN/control
	echo "Architecture: riscv64"                          >> $(@D)/deb/DEBIAN/control
	echo "Maintainer: K230 Dev <dev@example.com>"         >> $(@D)/deb/DEBIAN/control
	echo "Description: AprilTag Rust/C demos and profiling tools for K230" >> $(@D)/deb/DEBIAN/control
	mkdir -p $(BINARIES_DIR)/deb
	dpkg -b $(@D)/deb $(BINARIES_DIR)/deb/$(call LOWERCASE,k230-$(PKG)).deb
endef

APRILTAG_DEMO_POST_INSTALL_TARGET_HOOKS += APRILTAG_DEMO_BUILD_DEB

$(eval $(cmake-package))
