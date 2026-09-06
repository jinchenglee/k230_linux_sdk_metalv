APRILTAG_DEMO_SITE = $(realpath $(TOPDIR))"/package/apriltag_demo"
APRILTAG_DEMO_SITE_METHOD = local
APRILTAG_DEMO_DEPENDENCIES += opencv4 display vvcam libmmz apriltag k230_osd binutils linux
APRILTAG_DEMO_RVV_DIR ?= $(realpath $(TOPDIR)/../../../apriltag-rvv)
# Legacy production-only force control. Workload and profile have separate
# controls so forcing production does not rebuild instrumented variants.
APRILTAG_DEMO_FORCE_RUST_REBUILD ?= NO
APRILTAG_DEMO_FORCE_WORKLOAD_REBUILD ?= NO
APRILTAG_DEMO_FORCE_PROFILE_REBUILD ?= NO
APRILTAG_DEMO_PKGDIR ?= $(TOPDIR)/package/apriltag_demo

# The Rust archives follow the Linux ISA configuration rather than being pinned
# to the big core. apriltag-rvv gates every vector path behind
# #[cfg(target_feature = "v")], and build-capi.sh --no-rvv builds into its own
# --target-dir with -Z build-std, so rust-std is recompiled scalar as well --
# which matters, because most of the vector instructions in a linked
# tinytag_detect.elf come from std/core/alloc, not from apriltag-rvv's own
# kernels. See docs/notes/small-core-rvv-pollution.md.
#
# The variant is part of the source hash below: both variants publish the same
# archive name and the same .apriltag_rvv*.source-hash stamp, so without it a
# reconfigured tree would keep a vector archive and trap with SIGILL on the
# scalar core.
ifeq ($(BR2_RISCV_ISA_RVV),y)
APRILTAG_DEMO_RVV_VARIANT = rvv
APRILTAG_DEMO_RVV_ARGS =
else
APRILTAG_DEMO_RVV_VARIANT = scalar
APRILTAG_DEMO_RVV_ARGS = --no-rvv
endif

APRILTAG_DEMO_RVV_SOURCE_HASH := $(shell $(APRILTAG_DEMO_PKGDIR)/scripts/rust_source_hash.sh "$(APRILTAG_DEMO_RVV_DIR)" production $(APRILTAG_DEMO_RVV_VARIANT) 2>/dev/null)
APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH := $(shell $(APRILTAG_DEMO_PKGDIR)/scripts/rust_source_hash.sh "$(APRILTAG_DEMO_RVV_DIR)" workload $(APRILTAG_DEMO_RVV_VARIANT) 2>/dev/null)
APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH := $(shell $(APRILTAG_DEMO_PKGDIR)/scripts/rust_source_hash.sh "$(APRILTAG_DEMO_RVV_DIR)" profile $(APRILTAG_DEMO_RVV_VARIANT) 2>/dev/null)
APRILTAG_DEMO_RVV_COMBINED_SOURCE_HASH = $(shell printf '%s\n%s\n%s\n' "$(APRILTAG_DEMO_RVV_SOURCE_HASH)" "$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)" "$(APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH)" | sha256sum | cut -d' ' -f1)
APRILTAG_DEMO_RVV_GIT_SHA = $(shell sha=$$(git -C "$(APRILTAG_DEMO_RVV_DIR)" rev-parse --short=12 HEAD 2>/dev/null); test -n "$$sha" && { printf '%s' "$$sha"; test -z "$$(git -C "$(APRILTAG_DEMO_RVV_DIR)" status --porcelain 2>/dev/null)" || printf '%s' "-dirty-$$(printf '%s' "$(APRILTAG_DEMO_RVV_COMBINED_SOURCE_HASH)" | cut -c1-12)"; })
APRILTAG_DEMO_SDK_DIR = $(realpath $(TOPDIR)/../..)
APRILTAG_DEMO_SDK_GIT_SHA = $(shell bash $(APRILTAG_DEMO_PKGDIR)/scripts/git_source_identity.sh "$(APRILTAG_DEMO_SDK_DIR)" buildroot-overlay/package/apriltag_demo 2>/dev/null)
APRILTAG_DEMO_CONF_OPTS += \
	-DAPRILTAG_RVV_GIT_ID=$(APRILTAG_DEMO_RVV_GIT_SHA) \
	-DAPRILTAG_SDK_GIT_ID=$(APRILTAG_DEMO_SDK_GIT_SHA) \
	-DBR2_RISCV_ISA_RVV=$(if $(BR2_RISCV_ISA_RVV),ON,OFF)

# Ensure the Rust staticlib (built in the rvv-dev docker image) is present in
# Buildroot's copied source tree before CMake configures. Local package sources
# are rsynced into $(@D) before pre-configure hooks run, so build the library
# there rather than in $(APRILTAG_DEMO_PKGDIR). The helper publishes each set
# under a package lock and moves its hash stamp last; this synchronous hook does
# not accept an archive/header set until that commit-marker stamp matches.
define APRILTAG_DEMO_BUILD_RUST_LIB
	exec 8>$(@D)/lib/.apriltag-rvv-package.lock; flock 8; \
	if [ ! -f $(@D)/lib/libapriltag_rvv.a ] || \
	   [ ! -f $(@D)/lib/apriltag_kernel_modes.h ] || \
	   [ ! -f $(@D)/lib/apriltag_scratch.h ] || \
	   [ ! -f $(@D)/lib/apriltag_buffer_telemetry.h ] || \
	   [ ! -f $(@D)/lib/.apriltag_rvv.source-hash ] || \
	   [ "$$(cat $(@D)/lib/.apriltag_rvv.source-hash 2>/dev/null)" != \
	     "$(APRILTAG_DEMO_RVV_SOURCE_HASH)" ] || \
	   [ "$(APRILTAG_DEMO_FORCE_RUST_REBUILD)" = "YES" ]; then \
		echo "apriltag_demo: rebuilding production Rust library (source hash mismatch, missing artifact, or forced)..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
		APRILTAG_SOURCE_HASH="$(APRILTAG_DEMO_RVV_SOURCE_HASH)" \
		APRILTAG_PACKAGE_LOCK_HELD=1 \
			bash $(@D)/scripts/build_rust_lib.sh $(APRILTAG_DEMO_RVV_ARGS); \
	else \
		echo "apriltag_demo: production Rust source hash matches; using packaged archive"; \
	fi
	exec 8>$(@D)/lib/.apriltag-rvv-package.lock; flock 8; \
	if [ ! -f $(@D)/lib/libapriltag_rvv_workload.a ] || \
	   [ ! -f $(@D)/lib/apriltag_kernel_modes.h ] || \
	   [ ! -f $(@D)/lib/apriltag_scratch.h ] || \
	   [ ! -f $(@D)/lib/apriltag_buffer_telemetry.h ] || \
	   [ ! -f $(@D)/lib/rust_apriltag_workload.h ] || \
	   [ ! -f $(@D)/lib/.apriltag_rvv_workload.source-hash ] || \
	   [ "$$(cat $(@D)/lib/.apriltag_rvv_workload.source-hash 2>/dev/null)" != \
	     "$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)" ] || \
	   [ "$(APRILTAG_DEMO_FORCE_WORKLOAD_REBUILD)" = "YES" ]; then \
		echo "apriltag_demo: rebuilding Rust workload library (source hash mismatch or missing artifact)..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
		APRILTAG_WORKLOAD_SOURCE_HASH="$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)" \
		APRILTAG_PACKAGE_LOCK_HELD=1 \
			bash $(@D)/scripts/build_rust_lib.sh --workload-only $(APRILTAG_DEMO_RVV_ARGS); \
	else \
		echo "apriltag_demo: Rust workload source hash matches; using packaged archive"; \
	fi
	exec 8>$(@D)/lib/.apriltag-rvv-package.lock; flock 8; \
	if [ ! -f $(@D)/lib/libapriltag_rvv_profile.a ] || \
	   [ ! -f $(@D)/lib/apriltag_kernel_modes.h ] || \
	   [ ! -f $(@D)/lib/rust_apriltag_profile.h ] || \
	   [ ! -f $(@D)/lib/apriltag_scratch.h ] || \
	   [ ! -f $(@D)/lib/apriltag_buffer_telemetry.h ] || \
	   [ ! -f $(@D)/lib/apriltag_pending_profile.h ] || \
	   [ ! -f $(@D)/lib/.apriltag_rvv_profile.source-hash ] || \
	   [ "$$(cat $(@D)/lib/.apriltag_rvv_profile.source-hash 2>/dev/null)" != \
	     "$(APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH)" ] || \
	   [ "$(APRILTAG_DEMO_FORCE_PROFILE_REBUILD)" = "YES" ]; then \
		echo "apriltag_demo: rebuilding Rust profile library (source hash mismatch or missing artifact)..."; \
		APRILTAG_RVV_DIR="$(APRILTAG_DEMO_RVV_DIR)" \
		APRILTAG_PROFILE_SOURCE_HASH="$(APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH)" \
		APRILTAG_PACKAGE_LOCK_HELD=1 \
			bash $(@D)/scripts/build_rust_lib.sh --profile-only $(APRILTAG_DEMO_RVV_ARGS); \
	else \
		echo "apriltag_demo: Rust profile source hash matches; using packaged archive"; \
	fi
endef
APRILTAG_DEMO_PRE_CONFIGURE_HOOKS += APRILTAG_DEMO_BUILD_RUST_LIB

define APRILTAG_DEMO_RUN_SEQUENCE_HOST_TESTS
	CXX="$(HOSTCXX)" bash $(@D)/tests/run_sequence_host_tests.sh
	CXX="$(HOSTCXX)" bash $(@D)/tests/run_demo_options_host_tests.sh
	$(HOST_DIR)/bin/cmake -DPACKAGE_DIR=$(@D) \
		-P $(@D)/tests/verify_sequence_host_tests.cmake
endef
APRILTAG_DEMO_POST_BUILD_HOOKS += APRILTAG_DEMO_RUN_SEQUENCE_HOST_TESTS

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

# On a scalar-core configuration a stray vector instruction does not fail the
# build, it SIGILLs on the board -- and the traceback looks like a camera fault
# because it lands right after capture negotiation. Gate it here instead.
# Audit the build-tree binaries: the installed copies are stripped, so their
# per-symbol attribution is unusable. apriltag_demo.elf links libapriltag_rvv.a
# but not libnncase, so unlike tinytag_detect it can be held to zero.
ifneq ($(BR2_RISCV_ISA_RVV),y)
define APRILTAG_DEMO_AUDIT_VECTOR_FREE
	bash $(APRILTAG_DEMO_PKGDIR)/scripts/audit_vector_free.sh \
		$(TARGET_OBJDUMP) \
		$(@D)/apriltag_demo.elf \
		$(@D)/apriltag_c_demo.elf
endef
APRILTAG_DEMO_POST_INSTALL_TARGET_HOOKS += APRILTAG_DEMO_AUDIT_VECTOR_FREE
endif

$(eval $(cmake-package))
