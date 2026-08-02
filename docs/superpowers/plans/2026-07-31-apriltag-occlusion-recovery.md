# AprilTag Conservative Occlusion Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded Group D/E partial-occlusion recovery with exact-duplicate diagnostics, line-hypothesis gating, erasure-aware decode, and dedicated visualization.

**Architecture:** Preserve the normal detector as the primary path. Instrument boundary provenance and failure reasons, select at most one close/high-confidence recovery candidate per frame, and run recovery on retained diagnostic geometry without broad cluster merging. Keep recovery optional and disabled by default until explicitly configured.

**Tech Stack:** Rust 2021, Tag36h11, OpenCV K230 OSD, workload C ABI, Docker/QEMU/K230 verification.

---

### Task 1: Boundary Direction And Duplicate Diagnostics

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/workload.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/include/apriltag_workload.h`
- Modify: `buildroot-overlay/package/apriltag/0002-add-workload-counter-instrumentation.patch`
- Modify: workload SDK headers/report code

- [ ] Add failing synthetic 2x2 tests distinguishing right/down/down-left/down-right observations and C-style diagonal duplication.
- [ ] Add direction checks/candidates/qualified/emitted counters, connected-last suppression, perimeter counts, exact and coordinate duplicate counts.
- [ ] Calculate exact duplicate keys only after final component representatives are known.
- [ ] Keep production boundary output unchanged; diagnostics never suppress points.
- [ ] Update Rust/C workload ABI, validity, readable table, and machine records.
- [ ] Verify deterministic counters and production archive isolation.

### Task 2: CCL Substage Timers And Profiler Symbol

**Files:**
- Modify: Rust workload schema/pipeline
- Modify: C workload patch/schema
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] Add instrumented-only timers for RLE/repack, UF init, axis scan, each diagonal scan, root materialization, pending resolution/grouping/emission, and conversion.
- [ ] Add timer conservation tests and ensure production builds contain no timer calls.
- [ ] Change annotation target to `ccl_and_boundary_extract_impl`.
- [ ] Report substage times separately from authoritative production timing.

### Task 3: Successful Detection Provenance

**Files:**
- Modify: Rust pipeline/workload ABI
- Modify: C instrumented patch
- Modify: workload executable/report

- [ ] Add bounded diagnostic trace structs linking cluster -> quad -> raw detection.
- [ ] Record component-pair fingerprint, bbox, point count, directional counts, duplicates, and perimeter points for successful detections.
- [ ] Match Rust/C detections by ID and geometry; report nearest source clusters.
- [ ] Add counterfactual exact-dedup pass on copied diagnostic clusters and report whether each successful detection survives.
- [ ] Do not change authoritative production detections.

### Task 4: Recovery Candidate Failure Records

**Files:**
- Modify: Rust pipeline
- Create: Rust recovery module/tests

- [ ] Record Group D reasons: too few peaks, no candidate, fit score, invalid geometry.
- [ ] Record Group E reasons: photometric polarity and codeword failure with nearest-code distance and sample reliability.
- [ ] Retain bounded summaries only for candidates passing apparent-size gate.
- [ ] Add counters for candidate groups and skip reasons.

### Task 5: Theta-Rho Line Hypothesis Gate

**Files:**
- Create/Modify: Rust recovery module

- [ ] Add synthetic tests for three/four tag lines, fragmented one edge, alien connected edge, circles/noise, and small/distant candidates.
- [ ] Compute local tangent observations, coarse theta bins, rho-separated peaks, support/length/straightness/outlier metrics.
- [ ] Require close-enough geometric extent and at least three compatible spatially distinct line peaks among top eight.
- [ ] Rank candidates conservatively and expose diagnostics without recovery output yet.

### Task 6: Group D Quad Recovery

**Files:**
- Modify: Rust recovery module/pipeline

- [ ] Add robust line refinement and outlier rejection tests.
- [ ] Support four observed lines and three observed lines with one inferred edge.
- [ ] Enforce convex projected-square compatibility and geometry residual limits.
- [ ] Refine observed edges at full resolution and mark inferred edges/corners.

### Task 7: Group E Erasure-Aware Decode

**Files:**
- Modify: Rust recovery/decode code

- [ ] Add error/erasure decoder tests for Tag36h11 distance constraints and ambiguity rejection.
- [ ] Classify unreliable cells as erasures from local contrast/spatial masks.
- [ ] Require unique codeword with `2*errors + erasures < distance` and safe margin.
- [ ] Expose errors, erasures, and visible/inferred geometry metadata.

### Task 8: Hard Per-Frame Budget And Result API

**Files:**
- Modify: Rust config/C ABI/pipeline

- [ ] Add defaults: max two Group D considered, two Group E considered, top eight lines, one actual trial per frame.
- [ ] Test deterministic ranking and cap enforcement.
- [ ] Mark recovered detections and report recovery group, visible/inferred edges, errors/erasures, and residual.
- [ ] Keep recovery disabled by default and preserve normal ABI behavior when disabled.

### Task 9: Recovery Debug Page And OSD

**Files:**
- Modify: Rust DebugStage/C ABI
- Modify: K230 `apriltag.h`, drawing, and main controls

- [ ] Add stage 6 recovery debug image and tests.
- [ ] Draw cluster points, line inliers, outliers, observed/inferred lines, corners, quad, and labels.
- [ ] Draw recovered normal-page detections in magenta with `R` prefix.
- [ ] Update hotkey help and no-candidate message.

### Task 10: Verification

- [ ] Run full Docker suite and actual RVV checks.
- [ ] Verify recovery-disabled outputs/performance against frozen baseline.
- [ ] Run occlusion fixtures and validate IDs/geometry/confidence.
- [ ] Verify one-trial cap and profiling counters.
- [ ] Rebuild Buildroot package and inspect K230 debug images.
- [ ] Run `git diff --check` in both repositories.
