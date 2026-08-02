APRILTAG_DEMO_SITE = $(realpath $(TOPDIR))"/package/apriltag_demo"
APRILTAG_DEMO_SITE_METHOD = local
APRILTAG_DEMO_DEPENDENCIES += opencv4 display vvcam libmmz apriltag binutils linux
APRILTAG_DEMO_RVV_DIR ?= $(realpath $(TOPDIR)/../../../apriltag-rvv)
# Set to YES after changing the sibling apriltag-rvv source so Buildroot does
# not reuse an older static archive from this local package's copied tree.
APRILTAG_DEMO_FORCE_RUST_REBUILD ?= NO
APRILTAG_DEMO_RVV_SOURCE_HASH = $(shell $(APRILTAG_DEMO_PKGDIR)/scripts/rust_source_hash.sh "$(APRILTAG_DEMO_RVV_DIR)" production 2>/dev/null)
APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH = $(shell $(APRILTAG_DEMO_PKGDIR)/scripts/rust_source_hash.sh "$(APRILTAG_DEMO_RVV_DIR)" workload 2>/dev/null)
APRILTAG_DEMO_RVV_GIT_SHA = $(shell git -C "$(APRILTAG_DEMO_RVV_DIR)" rev-parse --short=12 HEAD 2>/dev/null)$(shell test -z "$$(git -C "$(APRILTAG_DEMO_RVV_DIR)" status --porcelain 2>/dev/null)" || printf '%s' -dirty)
APRILTAG_DEMO_SDK_GIT_SHA = $(shell git -C "$(realpath $(TOPDIR)/../..)" rev-parse --short=12 HEAD 2>/dev/null)$(shell test -z "$$(git -C "$(realpath $(TOPDIR)/../..)" status --porcelain 2>/dev/null)" || printf '%s' -dirty)
APRILTAG_DEMO_CONF_OPTS += \
	-DAPRILTAG_RVV_GIT_ID=$(APRILTAG_DEMO_RVV_GIT_SHA) \
	-DAPRILTAG_SDK_GIT_ID=$(APRILTAG_DEMO_SDK_GIT_SHA)

# Ensure the Rust staticlib (built in the rvv-dev docker image) is present in
# Buildroot's copied source tree before CMake configures. Local package sources
# are rsynced into $(@D) before pre-configure hooks run, so build the library
# there rather than in $(APRILTAG_DEMO_PKGDIR).
define APRILTAG_DEMO_BUILD_RUST_LIB
	if [ ! -f $(@D)/lib/libapriltag_rvv.a ] || \
	   [ ! -f $(@D)/lib/.apriltag_rvv.source-hash ] || \
	   [ "$$(cat $(@D)/lib/.apriltag_rvv.source-hash 2>/dev/null)" != \
	     "$(APRILTAG_DEMO_RVV_SOURCE_HASH)" ] || \
	   [ "$(APRILTAG_DEMO_FORCE_RUST_REBUILD)" = "YES" ]; then \
		echo "apriltag_demo: rebuilding production Rust library (source hash mismatch, missing artifact, or forced)..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
		APRILTAG_SOURCE_HASH="$(APRILTAG_DEMO_RVV_SOURCE_HASH)" \
			bash $(@D)/scripts/build_rust_lib.sh; \
	else \
		echo "apriltag_demo: production Rust source hash matches; using packaged archive"; \
	fi
	if [ ! -f $(@D)/lib/libapriltag_rvv_workload.a ] || \
	   [ ! -f $(@D)/lib/rust_apriltag_workload.h ] || \
	   [ ! -f $(@D)/lib/.apriltag_rvv_workload.source-hash ] || \
	   [ "$$(cat $(@D)/lib/.apriltag_rvv_workload.source-hash 2>/dev/null)" != \
	     "$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)" ]; then \
		echo "apriltag_demo: rebuilding Rust workload library (source hash mismatch or missing artifact)..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
		APRILTAG_WORKLOAD_SOURCE_HASH="$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)" \
			bash $(@D)/scripts/build_rust_lib.sh --workload-only; \
	else \
		echo "apriltag_demo: Rust workload source hash matches; using packaged archive"; \
	fi
endef
APRILTAG_DEMO_PRE_CONFIGURE_HOOKS += APRILTAG_DEMO_BUILD_RUST_LIB

define APRILTAG_DEMO_BUILD_DEB
	mkdir -p $(@D)/deb/DEBIAN
	mkdir -p $(@D)/deb/root/app/
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_demo,$(@D)/deb/root/app/)
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_c_demo,$(@D)/deb/root/app/)
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_profile,$(@D)/deb/root/app/)
	$(call COPYFILE,$(TARGET_DIR)/root/app/apriltag_bench,$(@D)/deb/root/app/)
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
