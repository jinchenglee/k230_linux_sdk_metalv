#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MK="$PKG_DIR/apriltag_demo.mk"
BUILD_SCRIPT="$PKG_DIR/scripts/build_rust_lib.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

RVV="$TMP/apriltag-rvv"
BUILDROOT="$TMP/sdk/output/buildroot"
COPIED="$TMP/build/apriltag_demo"
mkdir -p "$RVV/src" "$RVV/include" "$RVV/scripts" "$RVV/.cargo" "$BUILDROOT/package" \
    "$COPIED/lib" "$COPIED/scripts"
printf '%s\n' '[package]' 'name = "fixture"' >"$RVV/Cargo.toml"
printf '%s\n' '# fixture lockfile' >"$RVV/Cargo.lock"
printf '%s\n' 'fn main() {}' >"$RVV/build.rs"
printf '%s\n' '[toolchain]' 'channel = "nightly"' >"$RVV/rust-toolchain.toml"
printf '%s\n' '[build]' 'rustflags = []' >"$RVV/.cargo/config.toml"
printf '%s\n' 'pub fn fixture() {}' >"$RVV/src/lib.rs"
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
printf '%s\n' '#!/bin/sh' >"$RVV/scripts/build-capi.sh"

cat >"$TMP/hash.mk" <<EOF
TOPDIR := $BUILDROOT
APRILTAG_DEMO_RVV_DIR := $RVV
APRILTAG_DEMO_PKGDIR := $PKG_DIR
cmake-package =
include $MK
print-hash:
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)'
EOF

actual_hash="$(make -s -f "$TMP/hash.mk" print-hash)"
expected_hash="$("$PKG_DIR/scripts/rust_source_hash.sh" "$RVV" workload)"
[ "$actual_hash" = "$expected_hash" ] || {
    echo "source hash mismatch: expected $expected_hash, got $actual_hash" >&2
    exit 1
}

# The single-quoted strings intentionally defer expansion to the generated script.
# shellcheck disable=SC2016
printf '%s\n' '#!/bin/bash' 'set -euo pipefail' \
    'pkg_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"' \
    'printf "%s\\n" "$*" >>"$PACKAGING_TEST_LOG"' \
    'if [ "$*" = "--workload-only" ]; then' \
    '  tmp="$pkg_dir/lib/.archive.tmp"; printf archive >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/libapriltag_rvv_workload.a"' \
    '  tmp="$pkg_dir/lib/.header.tmp"; printf header >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/rust_apriltag_workload.h"' \
    '  tmp="$pkg_dir/lib/.stamp.tmp"; printf "%s\\n" "$APRILTAG_WORKLOAD_SOURCE_HASH" >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/.apriltag_rvv_workload.source-hash"' \
    'else' \
    '  tmp="$pkg_dir/lib/.archive.tmp"; printf production >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/libapriltag_rvv.a"' \
    '  tmp="$pkg_dir/lib/.stamp.tmp"; printf "%s\\n" "$APRILTAG_SOURCE_HASH" >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/.apriltag_rvv.source-hash"' \
    'fi' \
    >"$COPIED/scripts/build_rust_lib.sh"
chmod +x "$COPIED/scripts/build_rust_lib.sh"
printf production >"$COPIED/lib/libapriltag_rvv.a"
production_hash="$("$PKG_DIR/scripts/rust_source_hash.sh" "$RVV" production)"
printf '%s\n' "$production_hash" >"$COPIED/lib/.apriltag_rvv.source-hash"

cat >>"$TMP/hash.mk" <<EOF
PACKAGING_TEST_LOG := $TMP/build.log
export PACKAGING_TEST_LOG
$COPIED/run-hook:
	\$(APRILTAG_DEMO_BUILD_RUST_LIB)
EOF

make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
[ "$(cat "$TMP/build.log")" = "--workload-only" ]
[ "$(cat "$COPIED/lib/.apriltag_rvv_workload.source-hash")" = "$actual_hash" ]
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
[ "$(wc -l <"$TMP/build.log")" -eq 1 ]

printf '%s\n' '# changed' >>"$RVV/Cargo.lock"
changed_hash="$(make -s -f "$TMP/hash.mk" print-hash)"
[ "$changed_hash" != "$actual_hash" ]
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
[ "$(wc -l <"$TMP/build.log")" -eq 3 ]
[ "$(tail -n 2 "$TMP/build.log")" = $'\n--workload-only' ] ||
    [ "$(tail -n 2 "$TMP/build.log")" = $'--workload-only' ]
[ "$(cat "$COPIED/lib/.apriltag_rvv_workload.source-hash")" = "$changed_hash" ]

printf '%s\n' 'target-dir = "alternate-target"' >>"$RVV/.cargo/config.toml"
config_hash="$(make -s -f "$TMP/hash.mk" print-hash)"
[ "$config_hash" != "$changed_hash" ]
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
[ "$(wc -l <"$TMP/build.log")" -eq 5 ]
[ "$(cat "$COPIED/lib/.apriltag_rvv_workload.source-hash")" = "$config_hash" ]

fake_bin="$TMP/bin"
mkdir -p "$fake_bin"
cat >"$fake_bin/docker" <<'EOF'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$*" >"$BUILD_SCRIPT_TEST_LOG"
rvv_dir="${APRILTAG_RVV_DIR:-}"
previous=""
for argument in "$@"; do
    if [ "$previous" = "-w" ]; then rvv_dir="$argument"; break; fi
    previous="$argument"
done
mkdir -p "$rvv_dir/target/riscv64gc-unknown-linux-gnu/release"
printf workload >"$rvv_dir/target/riscv64gc-unknown-linux-gnu/release/libapriltag_rvv_workload.a"
printf production >"$rvv_dir/target/riscv64gc-unknown-linux-gnu/release/libapriltag_rvv.a"
EOF
chmod +x "$fake_bin/docker"
script_pkg="$TMP/script-package"
mkdir -p "$script_pkg/scripts"
cp "$BUILD_SCRIPT" "$script_pkg/scripts/build_rust_lib.sh"
BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_WORKLOAD_SOURCE_HASH="$config_hash" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --workload-only
grep -q 'scripts/build-capi.sh --workload-counters' "$TMP/docker.log"
if grep -q -- '--workload-only' "$TMP/docker.log"; then
    exit 1
fi
[ "$(cat "$script_pkg/lib/.apriltag_rvv_workload.source-hash")" = "$config_hash" ]
cmp "$RVV/include/apriltag_workload.h" "$script_pkg/lib/rust_apriltag_workload.h"
[ "$(stat -c '%a' "$script_pkg/lib/libapriltag_rvv_workload.a")" = 644 ]
[ "$(stat -c '%a' "$script_pkg/lib/rust_apriltag_workload.h")" = 644 ]
[ "$(stat -c '%a' "$script_pkg/lib/.apriltag_rvv_workload.source-hash")" = 644 ]

APRILTAG_PACKAGING_HEADER_ONLY=1 APRILTAG_WORKLOAD_HEADER="$RVV/include/apriltag_workload.h" \
    bash "$PKG_DIR/scripts/test_rust_packaging.sh"
cp "$RVV/include/apriltag_workload.h" "$TMP/malformed_workload.h"
sed -i 's/payload\[472\]/payload[464]/' "$TMP/malformed_workload.h"
if APRILTAG_PACKAGING_HEADER_ONLY=1 APRILTAG_WORKLOAD_HEADER="$TMP/malformed_workload.h" \
    bash "$PKG_DIR/scripts/test_rust_packaging.sh" >/dev/null 2>&1; then
    echo "malformed workload ABI header was accepted" >&2
    exit 1
fi

# The checked-out SDK and apriltag-rvv repositories are siblings. Recreate that
# layout in isolation to verify default discovery without touching packaged
# production artifacts, then verify the explicit Buildroot override.
default_sdk="$TMP/default-sdk"
default_pkg="$default_sdk/buildroot-overlay/package/apriltag_demo"
mkdir -p "$default_pkg/scripts"
cp "$BUILD_SCRIPT" "$default_pkg/scripts/build_rust_lib.sh"
cp "$PKG_DIR/scripts/rust_source_hash.sh" "$default_pkg/scripts/rust_source_hash.sh"
git -C "$default_sdk" init -q
default_log="$TMP/default-docker.log"
BUILD_SCRIPT_TEST_LOG="$default_log" PATH="$fake_bin:$PATH" \
    APRILTAG_SOURCE_HASH="$config_hash" APRILTAG_RVV_DIR='' \
    bash "$default_pkg/scripts/build_rust_lib.sh"
grep -q -- "-w $RVV" "$default_log"
[ "$(stat -c '%a' "$default_pkg/lib/libapriltag_rvv.a")" = 644 ]
[ "$(stat -c '%a' "$default_pkg/lib/.apriltag_rvv.source-hash")" = 644 ]

explicit_log="$TMP/explicit-docker.log"
BUILD_SCRIPT_TEST_LOG="$explicit_log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_SOURCE_HASH="$config_hash" \
    bash "$default_pkg/scripts/build_rust_lib.sh"
grep -q -- "-w $RVV" "$explicit_log"

echo "Rust workload packaging tests passed"
