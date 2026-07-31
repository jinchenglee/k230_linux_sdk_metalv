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
printf '%s\n' '#define FIXTURE 1' >"$RVV/include/apriltag_workload.h"
printf '%s\n' '#!/bin/sh' >"$RVV/scripts/build-capi.sh"

cat >"$TMP/hash.mk" <<EOF
TOPDIR := $BUILDROOT
APRILTAG_DEMO_RVV_DIR := $RVV
cmake-package =
include $MK
print-hash:
	@printf '%s\\n' '\$(APRILTAG_DEMO_RVV_WORKLOAD_SOURCE_HASH)'
EOF

actual_hash="$(make -s -f "$TMP/hash.mk" print-hash)"
expected_hash="$({ cd "$RVV"; { for source_file in .cargo/config.toml Cargo.lock Cargo.toml build.rs include/apriltag_workload.h rust-toolchain.toml scripts/build-capi.sh src/lib.rs; do printf '%s\0' "$source_file"; cat "$source_file"; done; } | sha256sum | cut -d' ' -f1; })"
[ "$actual_hash" = "$expected_hash" ] || {
    echo "source hash mismatch: expected $expected_hash, got $actual_hash" >&2
    exit 1
}

printf '%s\n' '#!/bin/bash' 'set -euo pipefail' \
    'pkg_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"' \
    'printf "%s\\n" "$*" >>"$PACKAGING_TEST_LOG"' \
    '[ "$*" = "--workload-only" ]' \
    'tmp="$pkg_dir/lib/.archive.tmp"; printf archive >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/libapriltag_rvv_workload.a"' \
    'tmp="$pkg_dir/lib/.header.tmp"; printf header >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/rust_apriltag_workload.h"' \
    'tmp="$pkg_dir/lib/.stamp.tmp"; printf "%s\\n" "$APRILTAG_WORKLOAD_SOURCE_HASH" >"$tmp"; mv -f "$tmp" "$pkg_dir/lib/.apriltag_rvv_workload.source-hash"' \
    >"$COPIED/scripts/build_rust_lib.sh"
chmod +x "$COPIED/scripts/build_rust_lib.sh"
printf production >"$COPIED/lib/libapriltag_rvv.a"

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
[ "$(wc -l <"$TMP/build.log")" -eq 2 ]
[ "$(cat "$COPIED/lib/.apriltag_rvv_workload.source-hash")" = "$changed_hash" ]

printf '%s\n' 'target-dir = "alternate-target"' >>"$RVV/.cargo/config.toml"
config_hash="$(make -s -f "$TMP/hash.mk" print-hash)"
[ "$config_hash" != "$changed_hash" ]
make -s -f "$TMP/hash.mk" "$COPIED/run-hook"
[ "$(wc -l <"$TMP/build.log")" -eq 3 ]
[ "$(cat "$COPIED/lib/.apriltag_rvv_workload.source-hash")" = "$config_hash" ]

fake_bin="$TMP/bin"
mkdir -p "$fake_bin"
cat >"$fake_bin/docker" <<'EOF'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$*" >"$BUILD_SCRIPT_TEST_LOG"
mkdir -p "$APRILTAG_RVV_DIR/target/riscv64gc-unknown-linux-gnu/release"
printf workload >"$APRILTAG_RVV_DIR/target/riscv64gc-unknown-linux-gnu/release/libapriltag_rvv_workload.a"
EOF
chmod +x "$fake_bin/docker"
script_pkg="$TMP/script-package"
mkdir -p "$script_pkg/scripts"
cp "$BUILD_SCRIPT" "$script_pkg/scripts/build_rust_lib.sh"
BUILD_SCRIPT_TEST_LOG="$TMP/docker.log" PATH="$fake_bin:$PATH" \
    APRILTAG_RVV_DIR="$RVV" APRILTAG_WORKLOAD_SOURCE_HASH="$config_hash" \
    bash "$script_pkg/scripts/build_rust_lib.sh" --workload-only
grep -q 'scripts/build-capi.sh --workload-counters' "$TMP/docker.log"
! grep -q -- '--workload-only' "$TMP/docker.log"
[ "$(cat "$script_pkg/lib/.apriltag_rvv_workload.source-hash")" = "$config_hash" ]

echo "Rust workload packaging tests passed"
