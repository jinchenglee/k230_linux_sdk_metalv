# Sequence Scratch Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add explicit local/reusable scratch selection to the sequence CLI, mode-aware telemetry validation, and profile-only ABI packaging while preserving compatibility and production symbol isolation.

**Architecture:** Thread a typed scratch mode from CLI parsing through detector setup, sequence orchestration, validation, and records. Keep the existing reusable validator policy unchanged; local mode validates fresh per-call allocation totals and repeated-call stability without imposing cross-row monotonicity. Consume the setter only from the profile archive used by profile/sequence executables.

**Tech Stack:** C++17, C ABI, shell packaging scripts, CMake, Buildroot cross-toolchain, `nm`, SHA-256.

---

### Task 1: Specify CLI and ABI Call Ordering

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_main.cc`

- [ ] Add failing parser/CLI tests for default `reusable`, explicit `local` and `reusable`, missing values, and unknown values.
- [ ] Extend the ABI mock to record exact `new`, scratch setter, optional kernel setter, detect, and free order; add setter failure cleanup coverage.
- [ ] Run `buildroot-overlay/package/apriltag_demo/tests/run_sequence_host_tests.sh`; expect compilation/link failure because the mode and setter do not exist.
- [ ] Add `ScratchMode`, exact text conversion, `--scratch-mode` parsing with compatibility default, usage text, and setter invocation immediately after detector construction and before kernel selection.
- [ ] Re-run the sequence host tests; expect the CLI and order tests to pass.

### Task 2: Implement Mode-Aware Scratch Validation and Records

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`

- [ ] Add failing local-mode tests accepting growth on repeated calls and cross-row capacity decrease, rejecting `growths_total != growths_call`, and rejecting capacity/high-water/length instability within a row.
- [ ] Add regression assertions that reusable mode retains zero-growth warm calls and monotonic cross-row invariants.
- [ ] Add failing output tests requiring exactly one `scratch_mode=local|reusable` field in every `SEQUENCE` record.
- [ ] Run sequence host tests and confirm the new local-mode/record tests fail for the expected missing behavior.
- [ ] Parameterize `SequenceScratchValidator`, records, and `run_sequence`; implement local per-call totals and same-row stability while leaving reusable helpers unchanged.
- [ ] Re-run sequence host tests; expect all scratch and record tests to pass.

### Task 3: Apply Mode-Specific Profile Semantics

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/profile_format.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`

- [ ] Add failing direct and orchestration tests requiring local repeated calls to have equal `pending_growths`, while reusable mode continues to permit cold-to-warm variation only for that counter.
- [ ] Run sequence host tests and confirm local unequal `pending_growths` is incorrectly accepted.
- [ ] Pass mode policy into sequence profile validation, allowing `pending_growths` variation only for reusable mode while preserving all generic schema, timing, validity, and counter checks.
- [ ] Re-run sequence and profile host tests; expect all tests to pass.

### Task 4: Refresh Profile Package and Build Identity Inputs

**Files:**
- Modify generated package artifacts under `buildroot-overlay/package/apriltag_demo/lib/`
- Verify: `buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh`
- Verify: `buildroot-overlay/package/apriltag_demo/scripts/rust_source_hash.sh`
- Verify: `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`
- Verify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`

- [ ] Run the existing profile-only packaging flow against the sibling `apriltag-rvv`; expect it to publish the updated profile archive, scratch header, and matching profile source-hash stamp atomically.
- [ ] Confirm CMake hashes the refreshed scratch header into profile and sequence build IDs and that the SDK copy remains packaging-owned rather than manually edited.
- [ ] Run package verification scripts; expect archive/header/hash consistency checks to pass.

### Task 5: Documentation and End-to-End Verification

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`
- Verify: `buildroot-overlay/package/apriltag_demo/tests/verify_benchmark_build.cmake`

- [ ] Update sequence examples and semantics for same-executable A/B runs differing only by `--scratch-mode local|reusable`.
- [ ] Run host sequence tests and host profile tests with their warning/error flags; expect zero failures.
- [ ] Cross-build the configured `apriltag_demo` package; expect benchmark verification and installation to pass.
- [ ] Use target `nm` to require the scratch setter in sequence/profile executables and reject it in the production benchmark.
- [ ] Run `git diff --check`; expect no whitespace errors.
- [ ] Locate `k230_apriltag_sequence_bench`, calculate SHA-256, and report the executable path and digest without committing.
