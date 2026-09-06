LIBNNCASE_VERSION =
NNCASE_VERSION_NUM=2.11.0
NNCASE_VERSION = v$(NNCASE_VERSION_NUM)

LIBNNCASE_SOURCE = nncase_k230_$(NNCASE_VERSION)_runtime_linux.tgz
LIBNNCASE_SITE = https://github.com/kendryte/nncase/releases/download/$(NNCASE_VERSION)

LIBNNCASE_RISCV_WHL = nncaseruntime_k230-$(NNCASE_VERSION_NUM)-py3-none-linux_riscv64.whl

LIBNNCASE_EXTRA_DOWNLOADS := $(LIBNNCASE_SITE)/$(LIBNNCASE_RISCV_WHL)

define LIBNNCASE_EXTRACT_CMDS
	tar zxf $(LIBNNCASE_DL_DIR)/$(LIBNNCASE_SOURCE) -C $(@D)
	mv $(@D)/nncase_k230_$(NNCASE_VERSION)_runtime_linux $(@D)/nncase
endef

# On a scalar-core (AMP) configuration the distributed libNncase.Runtime.Native.a
# cannot be used: it advertises rv64i..._v1p0_..._zve64d1p0 and carries ~2000
# vector instructions, so tinytag_detect traps with SIGILL inside
# nncase::(anon)::slice_contiguous_impl. That archive is the open-source part of
# the runtime, so it is rebuilt from the matching pinned nncase tag with
# ENABLE_RVV=OFF and a V-free -march. The two closed K230 archives
# (libfunctional_k230.a, libnncase.rt_modules.k230.a) and all headers keep
# coming from the distributed package -- they decode to no vector instructions.
#
# The big core keeps the distributed RVV runtime unchanged, so its optional CPU
# fallback kernels stay vectorized.
#
# See docs/notes/rvv-free-nncase-v2.11.0.md sections 5-8 and 13, and
# docs/notes/small-core-rvv-pollution.md.
ifneq ($(BR2_RISCV_ISA_RVV),y)

LIBNNCASE_SRC_TARBALL = $(NNCASE_VERSION).tar.gz
LIBNNCASE_EXTRA_DOWNLOADS += \
	https://github.com/kendryte/nncase/archive/refs/tags/$(LIBNNCASE_SRC_TARBALL)
LIBNNCASE_DEPENDENCIES += host-cmake host-ninja toolchain

LIBNNCASE_SCALAR_SRC = $(@D)/nncase-src
LIBNNCASE_SCALAR_BUILD = $(LIBNNCASE_SCALAR_SRC)/build-small-linux
LIBNNCASE_SCALAR_ARCHIVE = \
	$(LIBNNCASE_SCALAR_BUILD)/src/Native/src/runtime/libNncase.Runtime.Native.a

define LIBNNCASE_BUILD_CMDS
	rm -rf $(LIBNNCASE_SCALAR_SRC)
	mkdir -p $(LIBNNCASE_SCALAR_SRC)
	tar zxf $(LIBNNCASE_DL_DIR)/$(LIBNNCASE_SRC_TARBALL) \
		-C $(LIBNNCASE_SCALAR_SRC) --strip-components=1
	# Mixing source and closed-archive versions is not ABI-safe even when it
	# links, so refuse to build if the tag does not match the runtime package.
	grep -q 'set(NNCASE_VERSION "$(NNCASE_VERSION_NUM)")' \
		$(LIBNNCASE_SCALAR_SRC)/CMakeLists.txt || \
		{ echo "libnncase: source is not $(NNCASE_VERSION_NUM)" >&2; exit 1; }
	for p in $(LIBNNCASE_PKGDIR)/nncase-src-patches/*.patch; do \
		patch -d $(LIBNNCASE_SCALAR_SRC) -p1 <$$p || exit 1; \
	done
	cp $(LIBNNCASE_PKGDIR)/k230-small-linux.toolchain.cmake \
		$(LIBNNCASE_SCALAR_SRC)/toolchains/
	# RISCV_ROOT_PATH must be in the environment: CMake's nested try_compile
	# reloads the toolchain file, where a plain cache variable is not visible.
	cd $(LIBNNCASE_SCALAR_SRC) && RISCV_ROOT_PATH=$(HOST_DIR) \
		$(HOST_DIR)/bin/cmake -S . -B build-small-linux -G Ninja \
			-DCMAKE_TOOLCHAIN_FILE=toolchains/k230-small-linux.toolchain.cmake \
			-DCMAKE_BUILD_TYPE=Release \
			-DENABLE_RVV=OFF \
			-DBUILDING_RUNTIME=ON \
			-DBUILD_TESTING=OFF \
			-DBUILD_BENCHMARK=OFF \
			-DBUILD_PYTHON_BINDING=OFF \
			-DBUILD_CSHARP_BINDING=OFF
	cd $(LIBNNCASE_SCALAR_SRC) && RISCV_ROOT_PATH=$(HOST_DIR) \
		$(HOST_DIR)/bin/cmake --build build-small-linux \
			--target nncaseruntime -j$(PARALLEL_JOBS)
	# Audit before the archive can reach the sysroot: a vector instruction here
	# does not fail any later build, it SIGILLs on the board. Kept inline rather
	# than calling apriltag_demo's audit script, because libnncase must build in
	# configurations where that package is not enabled.
	if $(TARGET_READELF) -A $(LIBNNCASE_SCALAR_ARCHIVE) | \
	    grep -q 'Tag_RISCV_arch.*_v[0-9]'; then \
		echo "libnncase: scalar archive still advertises the V extension" >&2; \
		exit 1; \
	fi
	if $(TARGET_OBJDUMP) -d $(LIBNNCASE_SCALAR_ARCHIVE) | \
	    awk -F'\t' 'NF>=3 && $$3 ~ /^v[a-z0-9._]+$$/ {n++} END {exit !(n>0)}'; then \
		echo "libnncase: scalar archive still contains vector instructions" >&2; \
		$(TARGET_OBJDUMP) -d $(LIBNNCASE_SCALAR_ARCHIVE) | \
			awk -F'\t' 'NF>=3 && $$3 ~ /^v[a-z0-9._]+$$/ {print "  " $$3}' | \
			sort | uniq -c | sort -rn | head >&2; \
		exit 1; \
	fi
	cp -f $(LIBNNCASE_SCALAR_ARCHIVE) $(@D)/nncase/lib/libNncase.Runtime.Native.a
endef

endif

ifeq ($(BR2_PACKAGE_PYTHON3),y)
define LIBNNCASE_INSTALL_TARGET_CMDS
	cp -r $(@D)/nncase/* $(STAGING_DIR)/usr/
	mkdir -p $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages
	unzip -o $(LIBNNCASE_DL_DIR)/$(LIBNNCASE_RISCV_WHL) -d $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages
endef
else
define LIBNNCASE_INSTALL_TARGET_CMDS
	cp -r $(@D)/nncase/* $(STAGING_DIR)/usr/
endef
endif

$(eval $(generic-package))
