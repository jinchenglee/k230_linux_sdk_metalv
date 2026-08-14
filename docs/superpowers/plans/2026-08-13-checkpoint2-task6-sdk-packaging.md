# Checkpoint2 Task6 SDK Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package the pending-profile ABI atomically with the profile Rust archive and isolate its hash, symbol, and build identity from production and workload artifacts.

**Architecture:** Extend the existing profile-mode hash and transactional publication set with `apriltag_pending_profile.h`; do not add a packaging mode. CMake hashes the packaged header and identifies only profile consumers, while existing exact-symbol verification proves the getter cannot leak into non-profile binaries or archives.

**Tech Stack:** Bash, GNU Make/Buildroot, CMake, C/C++, `nm`, `sha256sum`

---

### Task 1: Add Failing Packaging Coverage

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/tests/verify_benchmark_build.cmake`

- [ ] Add a fixture pending header and assert mutating it changes only the profile hash.
- [ ] Assert profile clean publication copies the pending header byte-for-byte with mode `0644`.
- [ ] Add the pending header to absent/present rollback cases and every injected publication failure.
- [ ] Require the exact pending getter in profile and sequence, and forbid it in production, workload, Rust demo, and C demo.
- [ ] Require `_pendingabi-<12>_` in profile and sequence IDs and forbid it in production.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh` and confirm failure occurs because implementation does not yet package/hash the pending header.

### Task 2: Implement Profile Packaging

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/rust_source_hash.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`

- [ ] Hash `include/apriltag_pending_profile.h` only in profile mode.
- [ ] Stage the pending header and include it in backup, publication, rollback, signal cleanup, and temporary-file cleanup.
- [ ] Set the staged pending header mode to `0644` and publish it before the profile stamp.
- [ ] Require the pending header in Buildroot profile freshness checks and create it in the isolated fake helper.
- [ ] Run the packaging verifier and confirm all publication/hash tests pass.

### Task 3: Implement Build Identity And Consumption

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: the sequence source that calls profile getters

- [ ] SHA-256 the packaged pending header and truncate it to 12 hexadecimal characters.
- [ ] Add `_pendingabi-${APRILTAG_PENDING_ABI_HASH}_` to profile and sequence IDs only.
- [ ] Include the pending header in the sequence source and call `apriltag_get_ccl_pending_profile_v1` in the completed-frame profile path so linking requires the symbol.
- [ ] Run benchmark verifier self-tests and sequence host tests.

### Task 4: Refresh Generated Header And Verify

**Files:**
- Verify: `buildroot-overlay/package/apriltag_demo/lib/apriltag_pending_profile.h`
- Verify: sibling `apriltag-rvv/include/apriltag_pending_profile.h`

- [ ] Copy the current Rust source header byte-for-byte into the SDK package if they differ.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh` without cross-building archives.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh`.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/tests/test_verify_benchmark_build.sh`.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/tests/run_sequence_host_tests.sh`.
- [ ] Review `git diff --check` and the scoped diff; do not commit and do not run archive cross-builds before Task8.
