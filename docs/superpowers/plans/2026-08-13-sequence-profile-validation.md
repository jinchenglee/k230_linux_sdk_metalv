# Sequence Profile Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Permit only `pending_growths` to differ between cold and warm sequence profiles while preserving all generic profile validation guarantees and sequence scratch checks.

**Architecture:** Add a public sequence-specific validator beside the generic profile validator. Both use the same schema, timer-conservation, validity, and descriptor-based counter validation; the sequence policy explicitly allows only `pending_growths` to vary. The sequence runner alone calls the new validator, leaving ordinary profile reports strict.

**Tech Stack:** C++17, shell host-test harness, CMake/Buildroot cross-build.

---

### Task 1: Add Regression Coverage

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`

- [ ] Add an end-to-end `run_sequence` test whose cold profile has nonzero `pending_growths`, warm profile has zero, all other counters match, scratch reports cold growth and zero warm `growths_call`, and two `SEQUENCE` records are emitted.
- [ ] Run `buildroot-overlay/package/apriltag_demo/tests/run_sequence_host_tests.sh` and verify the new test fails specifically with `CCL workload counter changed: pending_growths`.
- [ ] Add direct sequence-validator cases proving `pending_growths` variation succeeds while `runs`, `cluster_map_growths`, and `cluster_vector_growths` variation fails.

### Task 2: Implement Narrow Validation

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/profile_format.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`

- [ ] Declare `validate_sequence_profile_sequence` publicly next to `validate_profile_sequence`.
- [ ] Refactor the descriptor-based comparison into shared internal validation with an explicit allowed-vary field list.
- [ ] Keep `validate_profile_sequence` strict by passing no exceptions; permit only the descriptor named `pending_growths` from `validate_sequence_profile_sequence`.
- [ ] Switch only `run_sequence` to the sequence-specific validator.
- [ ] Re-run sequence tests and verify all regression cases pass.

### Task 3: Verify Host and Cross Builds

**Files:**
- No source changes expected.

- [ ] Build and run host profile tests with the same C++17 warning/error flags used by sequence host tests.
- [ ] Run host sequence tests.
- [ ] Run the Buildroot `apriltag_demo` package cross-build without building a full image.
- [ ] Run `git diff --check`.
- [ ] Locate the newly built `k230_apriltag_sequence_bench`, calculate SHA256, and report its copy path.
- [ ] Do not commit.
