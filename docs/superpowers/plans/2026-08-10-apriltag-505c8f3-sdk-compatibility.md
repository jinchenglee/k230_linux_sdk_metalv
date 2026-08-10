# AprilTag 505c8f3 SDK Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the current K230 SDK build its AprilTag demos and schema-v1 workload benchmark against the detached `apriltag-rvv` worktree pinned exactly at `505c8f3`, without partial-occlusion recovery.

**Architecture:** Restore the K230 runtime and benchmark consumers to their pre-recovery interfaces, while retaining the newer source-hash-driven Rust archive packaging. Treat `/tmp/opencode/task5-rust-source/apriltag-rvv/include/apriltag_workload.h` as the workload ABI source of truth and reject stale or mismatched archives before the full image build. This detached, clean, commit-pinned worktree avoids accidentally building a later change from the mutable sibling checkout.

**Tech Stack:** Buildroot, CMake, C++17, Rust static libraries, Bash packaging tests, Docker `rvv-dev:latest`, RISC-V GNU toolchain

---

## File Structure

- Modify `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`: remove the recovery-only integration target and links.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag.h`: restore the detector ABI available at `505c8f3`.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag_c_adapter.cc`: remove recovery stubs and result metadata.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.{h,cc}`: remove recovery-only rendering.
- Modify `buildroot-overlay/package/apriltag_demo/src/main.cc`: remove recovery options, stage 6, and recovery diagnostics.
- Delete `buildroot-overlay/package/apriltag_demo/src/recovery_integration_tests.cc`: no recovery ABI remains.
- Modify `buildroot-overlay/package/apriltag_demo/bench/workload_{backend.h,c.cc,main.cc,rust.cc,tests.cc}`: restore schema-v1 structures, retrieval, reports, and tests.
- Modify `buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh`: verify schema-v1 symbols instead of recovery/schema-v2 symbols.
- Modify `buildroot-overlay/package/apriltag_demo/README.md`: remove recovery and schema-v2 instructions.
- Modify `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`: restore the symbol name available at `505c8f3`.
- Regenerate `buildroot-overlay/package/apriltag_demo/lib/*`: package matching production/workload archives, header, and stamps.

### Task 1: Add Compatibility Guards

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh`

- [ ] **Step 1: Change the artifact test to express the required schema-v1 ABI**

Replace the recovery/v2 symbol checks with these checks:

```bash
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv.a" |
    grep -q ' apriltag_detect$'
nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_workload.a" |
    grep -q ' apriltag_get_workload_counters$'
! nm -g --defined-only "$PKG_DIR/lib/libapriltag_rvv_workload.a" |
    grep -q ' apriltag_get_workload_counters_v2$'
grep -q '#define APRILTAG_WORKLOAD_SCHEMA_VERSION UINT32_C(1)' \
    "$PKG_DIR/lib/rust_apriltag_workload.h"
```

Extend the fake Docker section in `tests/verify_rust_packaging.sh` so the fake workload build copies its fixture header into `rust_apriltag_workload.h`, then assert that copied content matches. This keeps the test focused on archive/header atomicity without requiring real object files.

- [ ] **Step 2: Run the focused test and verify the current artifacts fail**

Run:

```bash
APRILTAG_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
  bash buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh
```

Expected: FAIL because the currently packaged workload archive/header are schema v2 from the interrupted build.

- [ ] **Step 3: Run the isolated packaging behavior test**

Run:

```bash
bash buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh
```

Expected: PASS, proving source hash invalidation and production/workload isolation still work.

- [ ] **Step 4: Commit the guard changes**

```bash
git add buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh \
        buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh
git commit -m "test(apriltag): require schema-v1 packaged ABI"
```

If commits are blocked by environment permissions, leave the changes uncommitted and continue without altering unrelated files.

### Task 2: Restore the Detector SDK ABI

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag.h`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_c_adapter.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.h`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/src/main.cc`
- Delete: `buildroot-overlay/package/apriltag_demo/src/recovery_integration_tests.cc`

- [ ] **Step 1: Capture the build failure as the integration test**

Run:

```bash
make apriltag_demo-dirclean
make apriltag_demo
```

Expected: FAIL with unresolved `apriltag_configure_recovery`, `apriltag_get_recovery_stats`, and `apriltag_get_recovery_candidates` references.

- [ ] **Step 2: Restore the normal detection ABI**

Use the versions immediately before SDK commit `e9ad2e5` as the exact reference for the five runtime files. The resulting `apriltag_det_t` must end after `corners[8]`:

```c
typedef struct {
    uint64_t id;
    uint32_t hamming;
    double margin;
    double center[2];
    double corners[8];
} apriltag_det_t;
```

Remove all `apriltag_recovery_*` types and function declarations. Keep debug stages 0 through 5 and the existing normal decode diagnostics.

- [ ] **Step 3: Remove runtime recovery behavior**

In `main.cc`, remove `--recovery`, `--recovery-min-extent`, stage 6 key handling, recovery configuration, candidate retrieval, statistics, and output. Preserve camera selection, denoise, scalar/RVV selection, stages 0 through 5, and ordinary detection/decode diagnostics.

In `apriltag_draw.{h,cc}`, remove recovery statistics/candidate functions and recovered-result styling. Preserve normal detection and stages 0 through 5 rendering.

In `apriltag_c_adapter.cc`, remove recovery stubs and writes to fields no longer present in `apriltag_det_t`.

- [ ] **Step 4: Delete the recovery-only integration source**

Delete `src/recovery_integration_tests.cc`; its only purpose is to require the API intentionally excluded from `505c8f3`.

- [ ] **Step 5: Verify no runtime recovery references remain**

Run:

```bash
rg -n 'recovery|Recovery|stage 6|0\.\.6' \
  buildroot-overlay/package/apriltag_demo/src
```

Expected: no matches.

- [ ] **Step 6: Commit the ABI restoration**

```bash
git add buildroot-overlay/package/apriltag_demo/src
git commit -m "fix(apriltag): restore pre-recovery detector ABI"
```

If commits are blocked, leave the intended changes uncommitted.

### Task 3: Restore Schema-v1 Workload Reporting

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_backend.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_c.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_main.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_rust.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_tests.cc`

- [ ] **Step 1: Restore the schema-v1 workload model and tests**

Use each file's version immediately before SDK commit `e9ad2e5` as the exact schema-v1 reference. `WorkloadCounters` must contain only the 480-byte schema-v1 fields and constants through bit 14. Remove boundary-direction details, timers, provenance, and counterfactual dedup fields.

Retain tests for deterministic counters, schema output, validity-aware ratios, zero denominators, checksum comparisons, and readable `uint64_t` output. Remove tests that require schema-v2 timers or provenance.

- [ ] **Step 2: Restore the schema-v1 Rust retrieval call**

In `bench/workload_rust.cc`, retain the size assertion and use:

```cpp
apriltag_workload_counters_t native{};
if (apriltag_get_workload_counters(handle_, &native) != 1)
    throw std::runtime_error("Rust workload counter retrieval failed");
```

The assertion remains:

```cpp
static_assert(sizeof(WorkloadCounters) ==
              sizeof(apriltag_workload_counters_t),
              "Rust workload ABI mismatch");
```

- [ ] **Step 3: Restore schema-v1 validation and report sections**

`validate_workload_pair` must compare counters directly and require:

```cpp
if (a.counters.schema_version != 1 ||
    a.counters.struct_size != sizeof(WorkloadCounters)) {
    throw std::runtime_error(std::string(backend_name(a.kind)) +
                             " returned an incompatible workload schema");
}
```

The report must include common, Rust-specific, C-specific, ratios, and checksums, but no boundary diagnostics, CCL timers, or provenance section.

- [ ] **Step 4: Build and run the host-independent format test**

Run through the configured Buildroot tree after Task 5 refreshes the source:

```bash
make apriltag_demo-dirclean
make apriltag_demo
```

Expected after all compatibility edits: `apriltag_workload_format_tests` compiles and the package build has no schema-v2 member errors.

- [ ] **Step 5: Commit workload compatibility**

```bash
git add buildroot-overlay/package/apriltag_demo/bench
git commit -m "fix(apriltag): restore schema-v1 workload reporting"
```

If commits are blocked, leave the intended changes uncommitted.

### Task 4: Remove Recovery Build Targets and Documentation

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] **Step 1: Remove the recovery test target from CMake**

Delete `recovery_test_bin`, its `add_executable`, its inclusion in the link-directory loop, its link libraries, and its post-build symbol checks. Keep all four deployed programs:

```cmake
set(rvv_bin apriltag_demo.elf)
set(c_bin apriltag_c_demo.elf)
set(bench_bin k230_apriltag_bench)
set(workload_bin k230_apriltag_workload)
```

Keep `apriltag_bench_tests` and `apriltag_workload_format_tests` as build-time validation executables.

- [ ] **Step 2: Restore the profiler symbol available at 505c8f3**

Replace `apriltag_rvv::pipeline::ccl_and_boundary_extract_impl` with:

```text
apriltag_rvv::pipeline::ccl_and_boundary_extract
```

in the full annotation and correlation symbol lists.

- [ ] **Step 3: Remove recovery/schema-v2 documentation**

Remove CLI examples for `--recovery`, stage 6, recovery overlays, CCL timers, provenance, and schema-v2-only output. Keep schema-v1 workload usage and normal debug stages 0 through 5.

- [ ] **Step 4: Check for stale package references**

Run:

```bash
rg -n 'apriltag_configure_recovery|apriltag_get_recovery|workload_counters_v2|schema.?v2|stage 6|0\.\.6' \
  buildroot-overlay/package/apriltag_demo \
  -g '!lib/libapriltag_rvv*.a'
```

Expected: no matches.

- [ ] **Step 5: Commit build and documentation cleanup**

```bash
git add buildroot-overlay/package/apriltag_demo/CMakeLists.txt \
        buildroot-overlay/package/apriltag_demo/README.md \
        buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh
git commit -m "build(apriltag): drop recovery-only SDK targets"
```

If commits are blocked, leave the intended changes uncommitted.

### Task 5: Rebuild and Package Matching Rust Archives

**Files:**
- Regenerate: `buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv.a`
- Regenerate: `buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv_workload.a`
- Regenerate: `buildroot-overlay/package/apriltag_demo/lib/rust_apriltag_workload.h`
- Regenerate: `buildroot-overlay/package/apriltag_demo/lib/.apriltag_rvv.source-hash`
- Regenerate: `buildroot-overlay/package/apriltag_demo/lib/.apriltag_rvv_workload.source-hash`

- [ ] **Step 1: Verify the source branch and commit**

Run:

```bash
test -z "$(git -C /tmp/opencode/task5-rust-source/apriltag-rvv branch --show-current)"
test "$(git -C /tmp/opencode/task5-rust-source/apriltag-rvv rev-parse HEAD)" = \
  505c8f3c69c3932a97a4c878a81982cbc5d68ff3
test -z "$(git -C /tmp/opencode/task5-rust-source/apriltag-rvv status --porcelain)"
```

Expected: all checks exit 0, proving detached HEAD is exactly `505c8f3c69c3932a97a4c878a81982cbc5d68ff3` and the worktree is clean. Use this pinned worktree, not the mutable sibling checkout, for every following source hash and build.

- [ ] **Step 2: Build and package the production archive**

Run:

```bash
APRILTAG_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
  bash buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh
```

Expected: RVV-enabled build succeeds and copies `libapriltag_rvv.a` plus its production source-hash stamp.

- [ ] **Step 3: Build and package the workload archive and header**

Run:

```bash
hash=$(buildroot-overlay/package/apriltag_demo/scripts/rust_source_hash.sh \
  /tmp/opencode/task5-rust-source/apriltag-rvv workload)
APRILTAG_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
APRILTAG_WORKLOAD_SOURCE_HASH="$hash" \
  bash buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh --workload-only
```

Expected: workload build succeeds and atomically copies the schema-v1 archive, header, and stamp without changing the production archive.

- [ ] **Step 4: Run both packaging tests**

Run:

```bash
APRILTAG_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
  bash buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh
bash buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh
```

Expected: both print their success messages and exit 0.

### Task 6: Build and Verify the K230 Package

**Files:**
- Build output: `output/k230_canmv_defconfig/build/apriltag_demo/`
- Installed programs: `output/k230_canmv_defconfig/target/root/app/apriltag_*`

- [ ] **Step 1: Clean the stale Buildroot package copy**

Run:

```bash
make APRILTAG_DEMO_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
  apriltag_demo-dirclean
```

Expected: the copied package build directory is removed.

- [ ] **Step 2: Rebuild the package with fresh Rust artifacts**

Run:

```bash
make APRILTAG_DEMO_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv \
  APRILTAG_DEMO_FORCE_RUST_REBUILD=YES apriltag_demo
```

Expected: all demo, detector benchmark, workload benchmark, and test targets compile and link; no recovery or schema-v2 symbol errors occur.

- [ ] **Step 3: Verify symbols and build provenance**

Run:

```bash
nm -g --defined-only \
  buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv_workload.a |
  rg ' apriltag_get_workload_counters$'
strings output/k230_canmv_defconfig/target/root/app/apriltag_bench/k230_apriltag_bench |
  rg 'rvv-505c8f3c69c3'
```

Expected: exactly the schema-v1 getter is found and the benchmark includes the `505c8f3c69c3` source identity.

- [ ] **Step 4: Verify installed executables**

Run:

```bash
file output/k230_canmv_defconfig/target/root/app/apriltag_demo/apriltag_demo.elf \
     output/k230_canmv_defconfig/target/root/app/apriltag_c_demo/apriltag_c_demo.elf \
     output/k230_canmv_defconfig/target/root/app/apriltag_bench/k230_apriltag_bench \
     output/k230_canmv_defconfig/target/root/app/apriltag_bench/k230_apriltag_workload
```

Expected: all four are RISC-V 64-bit ELF executables.

### Task 7: Build and Verify the Complete SDK Image

**Files:**
- Build output: `output/k230_canmv_defconfig/images/`

- [ ] **Step 1: Build the current SDK configuration**

Run:

```bash
make APRILTAG_DEMO_RVV_DIR=/tmp/opencode/task5-rust-source/apriltag-rvv
```

Expected: exit 0 for `k230_canmv_defconfig`. Do not revert the user's existing defconfig modification.

- [ ] **Step 2: Verify the image and package outputs are fresh**

Run:

```bash
test -s output/k230_canmv_defconfig/images/sysimage-sdcard.img.gz
test -s output/k230_canmv_defconfig/images/deb/k230-apriltag-demo.deb
stat output/k230_canmv_defconfig/images/sysimage-sdcard.img.gz \
     output/k230_canmv_defconfig/images/deb/k230-apriltag-demo.deb
```

Expected: both files exist, are non-empty, and have timestamps from the completed build.

- [ ] **Step 3: Review only intended source changes**

Run:

```bash
git status --short
git diff --check
git diff -- buildroot-overlay/package/apriltag_demo \
  docs/superpowers/specs/2026-08-10-apriltag-505c8f3-sdk-compatibility-design.md \
  docs/superpowers/plans/2026-08-10-apriltag-505c8f3-sdk-compatibility.md
```

Expected: no whitespace errors; unrelated pre-existing changes remain present but untouched.

- [ ] **Step 4: Commit final generated metadata if permitted**

```bash
git add buildroot-overlay/package/apriltag_demo \
        docs/superpowers/specs/2026-08-10-apriltag-505c8f3-sdk-compatibility-design.md \
        docs/superpowers/plans/2026-08-10-apriltag-505c8f3-sdk-compatibility.md
git commit -m "fix(apriltag): align SDK with explicit-dma ABI"
```

Do not stage the user's unrelated `buildroot-overlay/configs/k230_canmv_defconfig` modification or untracked files. If commits remain blocked, report that the verified changes are uncommitted.
