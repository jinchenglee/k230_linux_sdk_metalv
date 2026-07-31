# AprilTag Early Polarity Rejection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Match the C detector's family-border polarity rejection before sorting and line fitting, removing measured wasted Rust fitting work without losing valid Tag36h11 detections.

**Architecture:** Introduce unconditional family polarity metadata and one C-compatible cluster classifier using bounding-box center, fixed perturbations, and f32 accumulation. Apply it after cheap size/extent filters and before sort; carry the classification into quad construction so the expensive fit path does not recompute it.

**Tech Stack:** Rust 2021, Docker `rvv-dev:latest`, existing workload counters/benchmark/profiler, C reference as read-only behavioral specification.

---

### Task 1: Polarity Classifier Tests

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`

- [ ] Add failing unit tests for outward normal gradients, reversed gradients, exact-zero dot, near-zero perturbation behavior, and unsorted input.
- [ ] Verify tests fail because only the old arithmetic-mean/f64 estimator exists.
- [ ] Implement `BorderPolarity` and a single helper using C bbox center `+0.05118/-0.028581`, f32 dot accumulation, and strict `dot < 0` reversed classification.
- [ ] Run focused tests in Docker.

### Task 2: Family Metadata And Early Filter

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/tag36h11.rs`

- [ ] Add unconditional `TagFamily::border_polarity()` defaulting to normal and explicit Tag36h11 normal metadata.
- [ ] Extend the detector-only internal filter with family polarity; keep the public generic filter policy-neutral.
- [ ] Reject mismatched clusters after point/extent checks and before `sort_by_angle()`.
- [ ] Store the accepted cluster's polarity or recompute only with the shared helper, removing duplicate arithmetic-mean classifier logic.
- [ ] Add tests proving reversed clusters never enter sort/LFPS/errors.

### Task 3: Workload Counter Semantics

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/workload.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/tests/verify_workload_counters.rs`

- [ ] Convert hypothetical reject counters into actual `cluster_reject_border_polarity` and removed-work accounting.
- [ ] Mark actual polarity validity and retire or invalidate hypothetical fields once no longer hypothetical.
- [ ] Assert counter identities and expected workload reduction on the fixed fixture.
- [ ] Verify threshold/CCL counters remain unchanged while sort/LFPS/error points drop.

### Task 4: Detection Regression Matrix

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/tests/verify_pipeline_resolution.rs`
- Create or Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/tests/verify_early_polarity.rs`

- [ ] Freeze expected IDs and coordinate tolerances for all three repository fixtures.
- [ ] Test factors 1, 1.5, and 2, packed and padded stride, scalar and RVV dispatch.
- [ ] Run actual Linux RVV validation through the existing qemu-riscv64 workload path.
- [ ] Confirm full-resolution homography/decode behavior remains unchanged for retained tags.

### Task 5: Checksum Formatting

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_main.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/workload_tests.cc`

- [ ] Add failing formatting test for three maximum-width u64 checksums.
- [ ] Print checksum rows as fixed-width 16-digit hexadecimal with at least one separator between columns.
- [ ] Keep machine-readable `WORKLOAD` records unchanged.
- [ ] Run workload format tests.

### Task 6: Package And Performance Verification

- [ ] Rebuild Rust production and workload archives; verify source-hash regeneration and production archive isolation.
- [ ] Run Docker full tests and actual RVV checks.
- [ ] Rebuild Buildroot package/image tools.
- [ ] On K230 rerun `k230_apriltag_workload` and verify polarity rejects become actual and expensive-point counts drop toward the previous estimate.
- [ ] Run `profile_detector.sh fast`; compare detections/images and measure latency change.
- [ ] Run `git diff --check` in both repositories.
