#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MK="$PKG_DIR/apriltag_demo.mk"
BUILD_SCRIPT="$PKG_DIR/scripts/build_rust_lib.sh"
HASH_SCRIPT="$PKG_DIR/scripts/rust_source_hash.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

RVV="$TMP/apriltag-rvv"
ASYNC_RVV="$TMP/async-rvv"
BUILDROOT="$TMP/sdk/output/buildroot"
COPIED="$TMP/build/apriltag_demo"
mkdir -p "$RVV/src" "$RVV/include" "$RVV/scripts" "$RVV/.cargo" \
    "$ASYNC_RVV/src" "$BUILDROOT/package" "$COPIED/lib" "$COPIED/scripts"
printf '%s\n' '[package]' 'name = "fixture"' >"$RVV/Cargo.toml"
printf '%s\n' '# fixture lockfile' >"$RVV/Cargo.lock"
printf '%s\n' 'fn main() {}' >"$RVV/build.rs"
printf '%s\n' '[toolchain]' 'channel = "nightly"' >"$RVV/rust-toolchain.toml"
printf '%s\n' '[build]' 'rustflags = []' >"$RVV/.cargo/config.toml"
printf '%s\n' 'pub fn fixture() {}' >"$RVV/src/lib.rs"
printf '%s\n' 'pub fn pipeline() {}' >"$RVV/src/pipeline.rs"
printf '%s\n' 'pub fn profile() {}' >"$RVV/src/profile.rs"
printf '%s\n' 'pub fn pending_profile() {}' >"$RVV/src/pending_profile.rs"
printf '%s\n' '#!/bin/sh' >"$RVV/scripts/build-capi.sh"
printf '%s\n' '[package]' 'name = "async-fixture"' >"$ASYNC_RVV/Cargo.toml"
printf '%s\n' 'pub fn async_fixture() {}' >"$ASYNC_RVV/src/lib.rs"
cat >"$RVV/include/apriltag_kernel_modes.h" <<'EOF'
#ifndef APRILTAG_KERNEL_MODES_H
#define APRILTAG_KERNEL_MODES_H
#include <stdint.h>
#define APRILTAG_KERNEL_ALL ((UINT64_C(1) << 6) - 1)
int apriltag_set_kernel_mask_v1(void *handle, uint64_t mask);
#endif
EOF
cat >"$RVV/include/apriltag_workload.h" <<'EOF'
#ifndef APRILTAG_WORKLOAD_H
#define APRILTAG_WORKLOAD_H
#include <stdint.h>
#define APRILTAG_WORKLOAD_SCHEMA_VERSION UINT32_C(1)
typedef struct apriltag_workload_counters {
    uint32_t schema_version;
    uint32_t struct_size;
    unsigned char payload[472];
} apriltag_workload_counters_t;
#endif
EOF
cat >"$RVV/include/apriltag_profile.h" <<'EOF'
#ifndef APRILTAG_PROFILE_H
#define APRILTAG_PROFILE_H
#define APRILTAG_CCL_PROFILE_SIZE 768
typedef struct apriltag_ccl_profile {
    unsigned char payload[APRILTAG_CCL_PROFILE_SIZE];
} apriltag_ccl_profile_t;
#endif
EOF
cat >"$RVV/include/apriltag_pending_profile.h" <<'EOF'
#ifndef APRILTAG_PENDING_PROFILE_H
#define APRILTAG_PENDING_PROFILE_H
#include <stddef.h>
typedef void apriltag_t;
typedef struct apriltag_ccl_pending_profile_v1 {
    unsigned char payload[640];
} apriltag_ccl_pending_profile_v1_t;
int apriltag_get_ccl_pending_profile_v1(apriltag_t *,
                                        apriltag_ccl_pending_profile_v1_t *, size_t);
#endif
EOF
cat >"$RVV/include/apriltag_scratch.h" <<'EOF'
#ifndef APRILTAG_SCRATCH_H
#define APRILTAG_SCRATCH_H
#include <stddef.h>
#include <stdint.h>
#include "apriltag_buffer_telemetry.h"
#define APRILTAG_CCL_SCRATCH_VERSION UINT32_C(1)
#define APRILTAG_CCL_SCRATCH_SIZE 256
#define APRILTAG_CCL_SCRATCH_MODE_REUSABLE UINT32_C(0)
#define APRILTAG_CCL_SCRATCH_MODE_LOCAL UINT32_C(1)
typedef void apriltag_t;
typedef struct apriltag_ccl_scratch_v1 {
    uint32_t version, struct_size;
    uint64_t validity;
    apriltag_buffer_telemetry_t pending, diagonal_left, diagonal_right;
    uint64_t reserved[6];
} apriltag_ccl_scratch_v1_t;
int apriltag_get_ccl_scratch_v1(apriltag_t *, apriltag_ccl_scratch_v1_t *, size_t);
int apriltag_set_ccl_scratch_mode_v1(apriltag_t *, uint32_t);
#endif
EOF
cat >"$RVV/include/apriltag_buffer_telemetry.h" <<'EOF'
#ifndef APRILTAG_BUFFER_TELEMETRY_H
#define APRILTAG_BUFFER_TELEMETRY_H
#include <stdint.h>
typedef struct apriltag_buffer_telemetry {
    uint64_t len, capacity, high_water, growths_call, growths_total;
    uint64_t capacity_bytes, high_water_bytes, element_size;
} apriltag_buffer_telemetry_t;
#endif
EOF
hash_mode() {
    "$HASH_SCRIPT" "$RVV" "$1"
}

mkdir -p "$TMP/links"
ln -s "$RVV" "$TMP/links/apriltag-rvv"
for mode in production workload profile; do
    absolute_hash="$($HASH_SCRIPT "$RVV" "$mode")"
    relative_hash="$(cd "$TMP" && "$HASH_SCRIPT" apriltag-rvv "$mode")"
    symlink_hash="$($HASH_SCRIPT "$TMP/links/apriltag-rvv" "$mode")"
    test "$relative_hash" = "$absolute_hash"
    test "$symlink_hash" = "$absolute_hash"
done

production_hash="$(hash_mode production)"
workload_hash="$(hash_mode workload)"
profile_hash="$(hash_mode profile)"
test "$production_hash" != "$workload_hash"
test "$production_hash" != "$profile_hash"
test "$workload_hash" != "$profile_hash"

printf '%s\n' '/* shared kernel ABI change */' >>"$RVV/include/apriltag_kernel_modes.h"
test "$(hash_mode production)" != "$production_hash"
test "$(hash_mode workload)" != "$workload_hash"
test "$(hash_mode profile)" != "$profile_hash"
production_hash="$(hash_mode production)"
workload_hash="$(hash_mode workload)"
profile_hash="$(hash_mode profile)"

printf '%s\n' '// shared pipeline change' >>"$RVV/src/pipeline.rs"
production_pipeline_hash="$(hash_mode production)"
workload_pipeline_hash="$(hash_mode workload)"
profile_pipeline_hash="$(hash_mode profile)"
test "$production_pipeline_hash" != "$production_hash"
test "$workload_pipeline_hash" != "$workload_hash"
test "$profile_pipeline_hash" != "$profile_hash"

printf '%s\n' '/* workload ABI change */' >>"$RVV/include/apriltag_workload.h"
test "$(hash_mode production)" = "$production_pipeline_hash"
test "$(hash_mode workload)" != "$workload_pipeline_hash"
test "$(hash_mode profile)" = "$profile_pipeline_hash"
workload_hash="$(hash_mode workload)"
printf '%s\n' '// profile implementation change' >>"$RVV/src/profile.rs"
test "$(hash_mode production)" = "$production_pipeline_hash"
test "$(hash_mode workload)" = "$workload_hash"
test "$(hash_mode profile)" != "$profile_pipeline_hash"
profile_pipeline_hash="$(hash_mode profile)"
production_pending_source_hash="$(hash_mode production)"
workload_pending_source_hash="$(hash_mode workload)"
profile_pending_source_hash="$(hash_mode profile)"
printf '%s\n' '// pending profile implementation change' >>"$RVV/src/pending_profile.rs"
test "$(hash_mode production)" = "$production_pending_source_hash"
test "$(hash_mode workload)" = "$workload_pending_source_hash"
test "$(hash_mode profile)" != "$profile_pending_source_hash"
profile_pipeline_hash="$(hash_mode profile)"
printf '%s\n' '/* profile ABI change */' >>"$RVV/include/apriltag_profile.h"
test "$(hash_mode production)" = "$production_pipeline_hash"
test "$(hash_mode profile)" != "$profile_pipeline_hash"
profile_scratch_hash="$(hash_mode profile)"
production_pending_hash="$(hash_mode production)"
workload_pending_hash="$(hash_mode workload)"
profile_pending_hash="$(hash_mode profile)"
printf '%s\n' '/* pending profile ABI change */' >>"$RVV/include/apriltag_pending_profile.h"
test "$(hash_mode production)" = "$production_pending_hash"
test "$(hash_mode workload)" = "$workload_pending_hash"
test "$(hash_mode profile)" != "$profile_pending_hash"
printf '%s\n' '/* scratch ABI change */' >>"$RVV/include/apriltag_scratch.h"
test "$(hash_mode production)" != "$production_pipeline_hash"
test "$(hash_mode workload)" != "$workload_hash"
test "$(hash_mode profile)" != "$profile_scratch_hash"
production_buffer_hash="$(hash_mode production)"
workload_buffer_hash="$(hash_mode workload)"
profile_buffer_hash="$(hash_mode profile)"
printf '%s\n' '/* shared telemetry ABI change */' >>"$RVV/include/apriltag_buffer_telemetry.h"
test "$(hash_mode production)" != "$production_buffer_hash"
test "$(hash_mode workload)" != "$workload_buffer_hash"
test "$(hash_mode profile)" != "$profile_buffer_hash"

profile_hash="$(hash_mode profile)"
workload_hash="$(hash_mode workload)"
production_hash="$(hash_mode production)"
cat >"$TMP/hash.mk" <<EOF
TOPDIR := $BUILDROOT
APRILTAG_DEMO_RVV_DIR := $RVV
APRILTAG_DEMO_PKGDIR := $PKG_DIR
cmake-package =
include $MK
print-profile-hash:
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH)'
print-rust-id:
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_GIT_SHA)'
print-sdk-id:
	@printf '%s\\n' '\$(APRILTAG_DEMO_SDK_GIT_SHA)'
EOF
test "$(make -s -f "$TMP/hash.mk" print-profile-hash)" = "$profile_hash"

# One Make invocation must snapshot each mode once at parse time. Mutating a
# shared Rust input in the recipe must not change the hash passed to the helper.
snapshot_pkg="$TMP/snapshot-package"
snapshot_tools="$TMP/snapshot-tools"
mkdir -p "$snapshot_pkg/lib" "$snapshot_pkg/scripts" "$snapshot_tools/scripts"
cat >"$snapshot_tools/scripts/rust_source_hash.sh" <<EOF
#!/bin/bash
printf '%s\n' "\$2" >>"$TMP/hash-calls"
exec "$HASH_SCRIPT" "\$@"
EOF
cat >"$snapshot_tools/scripts/git_source_identity.sh" <<EOF
#!/bin/bash
exec "$PKG_DIR/scripts/git_source_identity.sh" "\$@"
EOF
cat >"$snapshot_pkg/scripts/build_rust_lib.sh" <<'EOF'
#!/bin/bash
set -euo pipefail
case "${1:-}" in
    --workload-only) printf '%s\n' "workload:$APRILTAG_WORKLOAD_SOURCE_HASH" ;;
    --profile-only) printf '%s\n' "profile:$APRILTAG_PROFILE_SOURCE_HASH" ;;
    *) printf '%s\n' "production:$APRILTAG_SOURCE_HASH" ;;
esac >>"$SNAPSHOT_BUILD_LOG"
EOF
chmod +x "$snapshot_tools/scripts/"*.sh "$snapshot_pkg/scripts/build_rust_lib.sh"
cat >"$TMP/snapshot.mk" <<EOF
TOPDIR := $BUILDROOT
APRILTAG_DEMO_RVV_DIR := $RVV
APRILTAG_DEMO_PKGDIR := $snapshot_tools
cmake-package =
include $MK
SNAPSHOT_BUILD_LOG := $TMP/snapshot-build.log
export SNAPSHOT_BUILD_LOG
$snapshot_pkg/run:
	@mkdir -p $snapshot_pkg/lib
	@printf archive >$snapshot_pkg/lib/libapriltag_rvv.a
	@printf archive >$snapshot_pkg/lib/libapriltag_rvv_workload.a
	@printf archive >$snapshot_pkg/lib/libapriltag_rvv_profile.a
	@printf header >$snapshot_pkg/lib/rust_apriltag_workload.h
	@printf header >$snapshot_pkg/lib/rust_apriltag_profile.h
	@printf header >$snapshot_pkg/lib/apriltag_pending_profile.h
	@printf header >$snapshot_pkg/lib/apriltag_scratch.h
	@printf header >$snapshot_pkg/lib/apriltag_buffer_telemetry.h
	@printf header >$snapshot_pkg/lib/apriltag_kernel_modes.h
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_SOURCE_HASH)' >$snapshot_pkg/lib/.apriltag_rvv.source-hash
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)' >$snapshot_pkg/lib/.apriltag_rvv_workload.source-hash
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_PROFILE_SOURCE_HASH)' >$snapshot_pkg/lib/.apriltag_rvv_profile.source-hash
	@printf '%s\\n' '// mutate after make parse' >>$RVV/src/pipeline.rs
	@rm $snapshot_pkg/lib/libapriltag_rvv_profile.a
	@\$(APRILTAG_DEMO_BUILD_RUST_LIB)
EOF
rm -f "$TMP/hash-calls" "$TMP/snapshot-build.log"
make -s -f "$TMP/snapshot.mk" "$snapshot_pkg/run"
test ! -e "$snapshot_pkg/lib/apriltag_grouping.h"
for mode in production workload profile; do
    test "$(grep -c "^$mode$" "$TMP/hash-calls")" -eq 1
done
test "$(cat "$TMP/snapshot-build.log")" = "profile:$profile_hash"

git -C "$RVV" init -q
git -C "$RVV" config user.name fixture
git -C "$RVV" config user.email fixture@example.com
git -C "$RVV" add .
git -C "$RVV" commit -qm fixture
rvv_git_sha="$(git -C "$RVV" rev-parse --short=12 HEAD)"
test "$(make -s -f "$TMP/hash.mk" print-rust-id)" = "$rvv_git_sha"
printf '%s\n' '# dirty identity probe' >>"$RVV/Cargo.lock"
production_hash="$(hash_mode production)"
workload_hash="$(hash_mode workload)"
profile_hash="$(hash_mode profile)"
combined_hash="$(printf '%s\n%s\n%s\n' "$production_hash" "$workload_hash" \
    "$profile_hash" | sha256sum | cut -d' ' -f1)"
test "$(make -s -f "$TMP/hash.mk" print-rust-id)" = \
    "$rvv_git_sha-dirty-${combined_hash:0:12}"
rust_identity_before_profile="$(make -s -f "$TMP/hash.mk" print-rust-id)"
printf '%s\n' '/* profile-only identity mutation */' >>"$RVV/include/apriltag_profile.h"
rust_identity_after_profile="$(make -s -f "$TMP/hash.mk" print-rust-id)"
test "$rust_identity_before_profile" != "$rust_identity_after_profile"
printf '%s\n' 'pub fn untracked_identity_probe() {}' >"$RVV/src/untracked_identity.rs"
test "$(make -s -f "$TMP/hash.mk" print-rust-id)" != "$rust_identity_after_profile"
production_hash="$(hash_mode production)"
workload_hash="$(hash_mode workload)"
profile_hash="$(hash_mode profile)"

mkdir -p "$TMP/sdk/buildroot-overlay/package/apriltag_demo"
printf '%s\n' fixture >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/identity.txt"
git -C "$TMP/sdk" init -q
git -C "$TMP/sdk" config user.name fixture
git -C "$TMP/sdk" config user.email fixture@example.com
git -C "$TMP/sdk" add .
git -C "$TMP/sdk" commit -qm fixture
sdk_git_sha="$(git -C "$TMP/sdk" rev-parse --short=12 HEAD)"
test "$(make -s -f "$TMP/hash.mk" print-sdk-id)" = "$sdk_git_sha"
printf '%s\n' dirty >>"$TMP/sdk/buildroot-overlay/package/apriltag_demo/identity.txt"
sdk_tracked_dirty="$(make -s -f "$TMP/hash.mk" print-sdk-id)"
test "$sdk_tracked_dirty" != "$sdk_git_sha"
case "$sdk_tracked_dirty" in "$sdk_git_sha-dirty-"????????????) ;; *) exit 1 ;; esac
printf '%s\n' first >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/untracked.txt"
sdk_untracked_first="$(make -s -f "$TMP/hash.mk" print-sdk-id)"
printf '%s\n' second >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/untracked.txt"
sdk_untracked_second="$(make -s -f "$TMP/hash.mk" print-sdk-id)"
test "$sdk_untracked_first" != "$sdk_untracked_second"
printf '%s\n' '*.a' '.apriltag-rvv-package.lock' '.publish-backup.*' \
    >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/.gitignore"
git -C "$TMP/sdk" add buildroot-overlay/package/apriltag_demo/.gitignore
git -C "$TMP/sdk" commit -qm 'ignore generated files'
sdk_identity_before_ignored="$(make -s -f "$TMP/hash.mk" print-sdk-id)"
printf ignored >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/generated.a"
printf ignored >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/.apriltag-rvv-package.lock"
mkdir "$TMP/sdk/buildroot-overlay/package/apriltag_demo/.publish-backup.fixture"
printf ignored >"$TMP/sdk/buildroot-overlay/package/apriltag_demo/.publish-backup.fixture/data"
test "$(make -s -f "$TMP/hash.mk" print-sdk-id)" = "$sdk_identity_before_ignored"

ignore_pkg="$TMP/ignore-sdk/buildroot-overlay/package/apriltag_demo"
mkdir -p "$ignore_pkg/lib"
cp "$PKG_DIR/.gitignore" "$ignore_pkg/.gitignore"
git -C "$TMP/ignore-sdk" init -q
for ignored in \
    lib/.publish-backup.fixture/data \
    lib/.source-hash.fixture \
    lib/.libapriltag_rvv_profile.a.fixture \
    lib/.rust_apriltag_profile.h.fixture \
    lib/.apriltag-rvv-package.lock; do
    mkdir -p "$(dirname "$ignore_pkg/$ignored")"
    printf ignored >"$ignore_pkg/$ignored"
    git -C "$TMP/ignore-sdk" check-ignore -q \
        "buildroot-overlay/package/apriltag_demo/$ignored"
done
for tracked in lib/.apriltag_rvv_profile.source-hash lib/rust_apriltag_profile.h \
    lib/apriltag_pending_profile.h; do
    printf generated >"$ignore_pkg/$tracked"
    if git -C "$TMP/ignore-sdk" check-ignore -q \
        "buildroot-overlay/package/apriltag_demo/$tracked"; then
        echo "final generated input is ignored: $tracked" >&2
        exit 1
    fi
done

# Exercise the real helper with a fake Docker executable. The fake publishes to
# the requested build-capi output root, exactly as the real inner script does.
fake_bin="$TMP/bin"
mkdir -p "$fake_bin"
cat >"$fake_bin/docker" <<'EOF'
#!/bin/bash
set -euo pipefail
printf '%q ' "$@" >"$BUILD_SCRIPT_TEST_LOG"
printf '\n' >>"$BUILD_SCRIPT_TEST_LOG"
test "${FAIL_BUILD:-0}" != 1 || exit 1
rvv_dir="${APRILTAG_RVV_DIR:?}"
output_root=target
for argument in "$@"; do
    case "$argument" in
        APRILTAG_CAPI_OUTPUT_ROOT=*) output_root=${argument#*=} ;;
    esac
done
case "$*" in
    *--workload-counters*) archive=libapriltag_rvv_workload.a ;;
    *--ccl-profile*) archive=libapriltag_rvv_profile.a ;;
    *) archive=libapriltag_rvv.a ;;
esac
case "$output_root" in /*) output_dir="$output_root" ;; *) output_dir="$rvv_dir/$output_root" ;; esac
mkdir -p "$output_dir/riscv64gc-unknown-linux-gnu/release"
printf '%s' "$archive" >"$output_dir/riscv64gc-unknown-linux-gnu/release/$archive"
EOF
chmod +x "$fake_bin/docker"

script_pkg="$TMP/script-package"
mkdir -p "$script_pkg/scripts" "$script_pkg/lib"
cp "$BUILD_SCRIPT" "$script_pkg/scripts/build_rust_lib.sh"
cp "$HASH_SCRIPT" "$script_pkg/scripts/rust_source_hash.sh"
printf packaged-production >"$script_pkg/lib/libapriltag_rvv.a"
printf packaged-workload >"$script_pkg/lib/libapriltag_rvv_workload.a"
production_archive_before="$(sha256sum "$script_pkg/lib/libapriltag_rvv.a")"
workload_archive_before="$(sha256sum "$script_pkg/lib/libapriltag_rvv_workload.a")"

if PATH="$fake_bin:$PATH" APRILTAG_RVV_DIR="$RVV" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
    echo "profile-only accepted without APRILTAG_PROFILE_SOURCE_HASH" >&2
    exit 1
fi
for args in '--profile-only --workload-only' '--workload-only --profile-only'; do
    if PATH="$fake_bin:$PATH" APRILTAG_RVV_DIR="$RVV" \
        APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
        APRILTAG_WORKLOAD_SOURCE_HASH="$workload_hash" \
        bash "$script_pkg/scripts/build_rust_lib.sh" $args >/dev/null 2>&1; then
        echo "mutually exclusive package modes accepted: $args" >&2
        exit 1
    fi
done

# Every standalone mode must publish scratch and its buffer-header dependency
# from an otherwise clean package directory.
for mode in production workload profile; do
    clean_pkg="$TMP/clean-$mode-package"
    mkdir -p "$clean_pkg/scripts" "$clean_pkg/lib"
    cp "$BUILD_SCRIPT" "$clean_pkg/scripts/build_rust_lib.sh"
    cp "$HASH_SCRIPT" "$clean_pkg/scripts/rust_source_hash.sh"
    case "$mode" in
        production)
            args=(); hash_env=(APRILTAG_SOURCE_HASH="$production_hash")
            archive=libapriltag_rvv.a; header=; stamp=.apriltag_rvv.source-hash
            ;;
        workload)
            args=(--workload-only); hash_env=(APRILTAG_WORKLOAD_SOURCE_HASH="$workload_hash")
            archive=libapriltag_rvv_workload.a; header=rust_apriltag_workload.h
            stamp=.apriltag_rvv_workload.source-hash
            ;;
        profile)
            args=(--profile-only); hash_env=(APRILTAG_PROFILE_SOURCE_HASH="$profile_hash")
            archive=libapriltag_rvv_profile.a; header=rust_apriltag_profile.h
            stamp=.apriltag_rvv_profile.source-hash
            ;;
    esac
    BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
        APRILTAG_RVV_DIR="$RVV" env "${hash_env[@]}" \
        bash "$clean_pkg/scripts/build_rust_lib.sh" "${args[@]}" >/dev/null
    cmp "$RVV/include/apriltag_scratch.h" "$clean_pkg/lib/apriltag_scratch.h"
    cmp "$RVV/include/apriltag_buffer_telemetry.h" \
        "$clean_pkg/lib/apriltag_buffer_telemetry.h"
    if [ "$mode" = profile ]; then
        cmp "$RVV/include/apriltag_pending_profile.h" \
            "$clean_pkg/lib/apriltag_pending_profile.h"
        test "$(stat -c '%a' "$clean_pkg/lib/apriltag_pending_profile.h")" = 644
    else
        test ! -e "$clean_pkg/lib/apriltag_pending_profile.h"
    fi

    printf old-archive >"$clean_pkg/lib/$archive"
    test -z "$header" || printf old-header >"$clean_pkg/lib/$header"
    printf old-scratch >"$clean_pkg/lib/apriltag_scratch.h"
    printf old-kernel >"$clean_pkg/lib/apriltag_kernel_modes.h"
    test "$mode" != profile || rm -f "$clean_pkg/lib/apriltag_pending_profile.h"
    printf '%s\n' old-stamp >"$clean_pkg/lib/$stamp"
    rm "$clean_pkg/lib/apriltag_buffer_telemetry.h"
    if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
        APRILTAG_RVV_DIR="$RVV" APRILTAG_PACKAGE_TEST_FAIL_STEP=after_buffer_header \
        env "${hash_env[@]}" bash "$clean_pkg/scripts/build_rust_lib.sh" \
        "${args[@]}" >/dev/null 2>&1; then
        echo "missing-buffer rollback unexpectedly succeeded: $mode" >&2
        exit 1
    fi
    test "$(cat "$clean_pkg/lib/$archive")" = old-archive
    test -z "$header" || test "$(cat "$clean_pkg/lib/$header")" = old-header
    test "$(cat "$clean_pkg/lib/apriltag_scratch.h")" = old-scratch
    test "$(cat "$clean_pkg/lib/apriltag_kernel_modes.h")" = old-kernel
    test "$(cat "$clean_pkg/lib/$stamp")" = old-stamp
    test ! -e "$clean_pkg/lib/apriltag_buffer_telemetry.h"
    test "$mode" != profile || test ! -e "$clean_pkg/lib/apriltag_pending_profile.h"
done

BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_CAPI_OUTPUT_ROOT="$RVV/custom output" \
    APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only
grep -q -- '--ccl-profile' "$TMP/docker.log"
grep -q 'APRILTAG_CAPI_OUTPUT_ROOT=custom\\ output' "$TMP/docker.log"
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = "$profile_hash"
cmp "$RVV/include/apriltag_profile.h" "$script_pkg/lib/rust_apriltag_profile.h"
cmp "$RVV/include/apriltag_scratch.h" "$script_pkg/lib/apriltag_scratch.h"
cmp "$RVV/include/apriltag_buffer_telemetry.h" "$script_pkg/lib/apriltag_buffer_telemetry.h"
cmp "$RVV/include/apriltag_kernel_modes.h" "$script_pkg/lib/apriltag_kernel_modes.h"
cmp "$RVV/include/apriltag_pending_profile.h" "$script_pkg/lib/apriltag_pending_profile.h"
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = libapriltag_rvv_profile.a
test "$(sha256sum "$script_pkg/lib/libapriltag_rvv.a")" = "$production_archive_before"
test "$(sha256sum "$script_pkg/lib/libapriltag_rvv_workload.a")" = "$workload_archive_before"
for generated in libapriltag_rvv_profile.a rust_apriltag_profile.h apriltag_scratch.h \
    apriltag_buffer_telemetry.h apriltag_kernel_modes.h \
    apriltag_pending_profile.h \
    .apriltag_rvv_profile.source-hash; do
    test "$(stat -c '%a' "$script_pkg/lib/$generated")" = 644
done
test ! -e "$script_pkg/lib/apriltag_grouping.h"
test -z "$(find "$script_pkg/lib" -maxdepth 1 -name '.*.??????' -print -quit)"

# A profile staging failure must clean every temporary resource without
# touching existing destinations or the package lock.
printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
printf old-buffer >"$script_pkg/lib/apriltag_buffer_telemetry.h"
printf old-kernel >"$script_pkg/lib/apriltag_kernel_modes.h"
printf old-pending >"$script_pkg/lib/apriltag_pending_profile.h"
printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
mv "$RVV/include/apriltag_pending_profile.h" \
    "$RVV/include/apriltag_pending_profile.h.unavailable"
if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
    echo "missing pending source header unexpectedly staged" >&2
    exit 1
fi
mv "$RVV/include/apriltag_pending_profile.h.unavailable" \
    "$RVV/include/apriltag_pending_profile.h"
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
test "$(cat "$script_pkg/lib/apriltag_buffer_telemetry.h")" = old-buffer
test "$(cat "$script_pkg/lib/apriltag_kernel_modes.h")" = old-kernel
test "$(cat "$script_pkg/lib/apriltag_pending_profile.h")" = old-pending
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp
test -e "$script_pkg/lib/.apriltag-rvv-package.lock"
test -z "$(find "$script_pkg/lib" -maxdepth 1 \
    \( -name '.*.??????' -o -name '.publish-backup.??????' \) -print -quit)"

for unsafe_root in "$TMP/outside" "$RVV/quote'output"; do
    if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
        APRILTAG_RVV_DIR="$RVV" APRILTAG_CAPI_OUTPUT_ROOT="$unsafe_root" \
        APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
        bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
        echo "unsafe output root accepted: $unsafe_root" >&2
        exit 1
    fi
done

printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
printf old-buffer >"$script_pkg/lib/apriltag_buffer_telemetry.h"
printf old-pending >"$script_pkg/lib/apriltag_pending_profile.h"
printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" FAIL_BUILD=1 PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
    echo "failed profile build unexpectedly succeeded" >&2
    exit 1
fi
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
test "$(cat "$script_pkg/lib/apriltag_buffer_telemetry.h")" = old-buffer
test "$(cat "$script_pkg/lib/apriltag_pending_profile.h")" = old-pending
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp
test -z "$(find "$script_pkg/lib" -maxdepth 1 -name '.apriltag_buffer_telemetry.h.??????' -print -quit)"

# A destination absent before publication must remain absent after rollback,
# while destinations with prior contents are restored.
printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
rm -f "$script_pkg/lib/apriltag_buffer_telemetry.h"
rm -f "$script_pkg/lib/apriltag_pending_profile.h"
printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    APRILTAG_PACKAGE_TEST_FAIL_STEP=after_kernel_header \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
    echo "missing-artifact publication failure unexpectedly succeeded" >&2
    exit 1
fi
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
test ! -e "$script_pkg/lib/apriltag_buffer_telemetry.h"
test ! -e "$script_pkg/lib/apriltag_pending_profile.h"
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp

# If pending was absent before publication, failure after publishing it must
# remove it while restoring every pre-existing profile artifact.
printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
printf old-buffer >"$script_pkg/lib/apriltag_buffer_telemetry.h"
printf old-kernel >"$script_pkg/lib/apriltag_kernel_modes.h"
rm -f "$script_pkg/lib/apriltag_pending_profile.h"
printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    APRILTAG_PACKAGE_TEST_FAIL_STEP=after_pending_header \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
    echo "absent pending-header rollback unexpectedly succeeded" >&2
    exit 1
fi
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
test "$(cat "$script_pkg/lib/apriltag_buffer_telemetry.h")" = old-buffer
test "$(cat "$script_pkg/lib/apriltag_kernel_modes.h")" = old-kernel
test ! -e "$script_pkg/lib/apriltag_pending_profile.h"
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp

for failure_step in after_archive after_profile_header after_scratch_header \
    after_buffer_header after_kernel_header after_pending_header after_stamp; do
    printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
    printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
    printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
    printf old-buffer >"$script_pkg/lib/apriltag_buffer_telemetry.h"
    printf old-pending >"$script_pkg/lib/apriltag_pending_profile.h"
    printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
    if BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
        APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
        APRILTAG_PACKAGE_TEST_FAIL_STEP="$failure_step" \
        bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1; then
        echo "injected publication failure unexpectedly succeeded: $failure_step" >&2
        exit 1
    fi
    test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
    test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
    test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
    test "$(cat "$script_pkg/lib/apriltag_buffer_telemetry.h")" = old-buffer
    test "$(cat "$script_pkg/lib/apriltag_pending_profile.h")" = old-pending
    test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp
done

printf old-archive >"$script_pkg/lib/libapriltag_rvv_profile.a"
printf old-header >"$script_pkg/lib/rust_apriltag_profile.h"
printf old-scratch >"$script_pkg/lib/apriltag_scratch.h"
printf old-buffer >"$script_pkg/lib/apriltag_buffer_telemetry.h"
printf old-pending >"$script_pkg/lib/apriltag_pending_profile.h"
printf '%s\n' old-stamp >"$script_pkg/lib/.apriltag_rvv_profile.source-hash"
signal_ready="$TMP/signal-ready"
BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    APRILTAG_PACKAGE_TEST_PAUSE_STEP=after_archive \
    APRILTAG_PACKAGE_TEST_READY_FILE="$signal_ready" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1 &
signal_pid=$!
for _ in $(seq 1 100); do
    test ! -e "$signal_ready" || break
    sleep 0.02
done
test -e "$signal_ready"
kill -TERM "$signal_pid"
if wait "$signal_pid"; then
    echo "TERM during publication unexpectedly succeeded" >&2
    exit 1
fi
test "$(cat "$script_pkg/lib/libapriltag_rvv_profile.a")" = old-archive
test "$(cat "$script_pkg/lib/rust_apriltag_profile.h")" = old-header
test "$(cat "$script_pkg/lib/apriltag_scratch.h")" = old-scratch
test "$(cat "$script_pkg/lib/apriltag_buffer_telemetry.h")" = old-buffer
test "$(cat "$script_pkg/lib/apriltag_pending_profile.h")" = old-pending
test "$(cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash")" = old-stamp

# A reader honoring the package lock cannot observe the invalidated stamp or
# partially replaced payload while publication is paused.
rm -f "$signal_ready"
APRILTAG_PACKAGE_TEST_RELEASE_FILE="$TMP/release-publication"
export APRILTAG_PACKAGE_TEST_RELEASE_FILE
BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_PROFILE_SOURCE_HASH="$profile_hash" \
    APRILTAG_PACKAGE_TEST_PAUSE_STEP=after_archive \
    APRILTAG_PACKAGE_TEST_READY_FILE="$signal_ready" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --profile-only >/dev/null 2>&1 &
publisher_pid=$!
for _ in $(seq 1 100); do
    test ! -e "$signal_ready" || break
    sleep 0.02
done
test -e "$signal_ready"
reader_result="$TMP/reader-result"
(
    exec 8>"$script_pkg/lib/.apriltag-rvv-package.lock"
    flock 8
    cat "$script_pkg/lib/.apriltag_rvv_profile.source-hash" >"$reader_result"
) &
reader_pid=$!
sleep 0.1
test ! -e "$reader_result"
touch "$APRILTAG_PACKAGE_TEST_RELEASE_FILE"
wait "$publisher_pid"
wait "$reader_pid"
test "$(cat "$reader_result")" = "$profile_hash"
unset APRILTAG_PACKAGE_TEST_RELEASE_FILE

APRILTAG_PACKAGING_HEADER_ONLY=1 APRILTAG_WORKLOAD_HEADER="$RVV/include/apriltag_workload.h" \
    APRILTAG_PROFILE_HEADER="$RVV/include/apriltag_profile.h" \
    APRILTAG_PENDING_HEADER="$RVV/include/apriltag_pending_profile.h" \
    APRILTAG_SCRATCH_HEADER="$RVV/include/apriltag_scratch.h" \
    APRILTAG_BUFFER_HEADER="$RVV/include/apriltag_buffer_telemetry.h" \
    bash "$PKG_DIR/scripts/test_rust_packaging.sh"

# Verify copied-package freshness and force semantics with a fully isolated fake
# helper. Production and workload begin valid, so a missing profile builds alone.
cat >"$COPIED/scripts/build_rust_lib.sh" <<'EOF'
#!/bin/bash
set -euo pipefail
pkg_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scratch_header=
printf '%s\n' "${*:-production}" >>"$PACKAGING_TEST_LOG"
test "${APRILTAG_PACKAGE_LOCK_HELD:-0}" = 1
if flock -n "$pkg_dir/lib/.apriltag-rvv-package.lock" true; then
    echo "Buildroot helper invocation did not retain package lock" >&2
    exit 1
fi
case "${1:-}" in
    --workload-only)
        archive=libapriltag_rvv_workload.a
        header=rust_apriltag_workload.h
        scratch_header=apriltag_scratch.h
        buffer_header=apriltag_buffer_telemetry.h
        stamp=.apriltag_rvv_workload.source-hash
        hash=$APRILTAG_WORKLOAD_SOURCE_HASH
        ;;
    --profile-only)
        archive=libapriltag_rvv_profile.a
        header=rust_apriltag_profile.h
        scratch_header=apriltag_scratch.h
        buffer_header=apriltag_buffer_telemetry.h
        pending_header=apriltag_pending_profile.h
        stamp=.apriltag_rvv_profile.source-hash
        hash=$APRILTAG_PROFILE_SOURCE_HASH
        ;;
    *)
        archive=libapriltag_rvv.a
        header=
        scratch_header=apriltag_scratch.h
        buffer_header=apriltag_buffer_telemetry.h
        stamp=.apriltag_rvv.source-hash
        hash=$APRILTAG_SOURCE_HASH
        ;;
esac
printf '%s' "$archive" >"$pkg_dir/lib/$archive"
test -z "$header" || printf '%s' "$header" >"$pkg_dir/lib/$header"
test -z "$scratch_header" || cp "$APRILTAG_RVV_DIR/include/apriltag_scratch.h" \
    "$pkg_dir/lib/$scratch_header"
test -z "${buffer_header:-}" || cp "$APRILTAG_RVV_DIR/include/apriltag_buffer_telemetry.h" \
    "$pkg_dir/lib/$buffer_header"
test -z "${pending_header:-}" || cp "$APRILTAG_RVV_DIR/include/apriltag_pending_profile.h" \
    "$pkg_dir/lib/$pending_header"
cp "$APRILTAG_RVV_DIR/include/apriltag_kernel_modes.h" \
    "$pkg_dir/lib/apriltag_kernel_modes.h"
printf '%s\n' "$hash" >"$pkg_dir/lib/$stamp"
chmod 0644 "$pkg_dir/lib/$archive" "$pkg_dir/lib/$stamp"
test -z "$header" || chmod 0644 "$pkg_dir/lib/$header"
EOF
chmod +x "$COPIED/scripts/build_rust_lib.sh"
printf production >"$COPIED/lib/libapriltag_rvv.a"
printf workload >"$COPIED/lib/libapriltag_rvv_workload.a"
printf workload-header >"$COPIED/lib/rust_apriltag_workload.h"
printf profile >"$COPIED/lib/libapriltag_rvv_profile.a"
printf profile-header >"$COPIED/lib/rust_apriltag_profile.h"
cp "$RVV/include/apriltag_scratch.h" "$COPIED/lib/apriltag_scratch.h"
cp "$RVV/include/apriltag_buffer_telemetry.h" "$COPIED/lib/apriltag_buffer_telemetry.h"
cp "$RVV/include/apriltag_kernel_modes.h" "$COPIED/lib/apriltag_kernel_modes.h"
cp "$RVV/include/apriltag_pending_profile.h" "$COPIED/lib/apriltag_pending_profile.h"
printf '%s\n' "$production_hash" >"$COPIED/lib/.apriltag_rvv.source-hash"
printf '%s\n' "$workload_hash" >"$COPIED/lib/.apriltag_rvv_workload.source-hash"
printf '%s\n' "$profile_hash" >"$COPIED/lib/.apriltag_rvv_profile.source-hash"
cat >>"$TMP/hash.mk" <<EOF
PACKAGING_TEST_LOG := $TMP/build.log
export PACKAGING_TEST_LOG
$COPIED/run-hook:
	\$(APRILTAG_DEMO_BUILD_RUST_LIB)
$COPIED/run-force-hook:
	\$(eval APRILTAG_DEMO_FORCE_RUST_REBUILD := YES)
	\$(APRILTAG_DEMO_BUILD_RUST_LIB)
$COPIED/run-force-workload-hook:
	\$(eval APRILTAG_DEMO_FORCE_WORKLOAD_REBUILD := YES)
	\$(APRILTAG_DEMO_BUILD_RUST_LIB)
$COPIED/run-force-profile-hook:
	\$(eval APRILTAG_DEMO_FORCE_PROFILE_REBUILD := YES)
	\$(APRILTAG_DEMO_BUILD_RUST_LIB)
EOF
rm "$COPIED/lib/apriltag_pending_profile.h"
production_before="$(sha256sum "$COPIED/lib/libapriltag_rvv.a")"
workload_before="$(sha256sum "$COPIED/lib/libapriltag_rvv_workload.a")"
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
test "$(cat "$TMP/build.log")" = --profile-only
test "$(sha256sum "$COPIED/lib/libapriltag_rvv.a")" = "$production_before"
test "$(sha256sum "$COPIED/lib/libapriltag_rvv_workload.a")" = "$workload_before"
cmp "$RVV/include/apriltag_pending_profile.h" \
    "$COPIED/lib/apriltag_pending_profile.h"
test "$(cat "$COPIED/lib/.apriltag_rvv_profile.source-hash")" = "$profile_hash"
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
test "$(wc -l <"$TMP/build.log")" -eq 1
make -s -f "$TMP/hash.mk" "$COPIED/run-force-hook"
test "$(tail -n 1 "$TMP/build.log")" = production
test "$(wc -l <"$TMP/build.log")" -eq 2
make -s -f "$TMP/hash.mk" "$COPIED/run-force-workload-hook"
test "$(tail -n 1 "$TMP/build.log")" = --workload-only
test "$(wc -l <"$TMP/build.log")" -eq 3
make -s -f "$TMP/hash.mk" "$COPIED/run-force-profile-hook"
test "$(tail -n 1 "$TMP/build.log")" = --profile-only
test "$(wc -l <"$TMP/build.log")" -eq 4

bash "$PKG_DIR/tests/test_verify_benchmark_build.sh"

echo "Rust production/workload/profile packaging tests passed"
