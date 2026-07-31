# AprilTag Workload Counters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build separate instrumented Rust and C detector variants plus a K230 workload comparison tool that explains whether performance gaps come from more work or slower per-unit implementation.

**Architecture:** Feature-gated Rust counters and macro-gated C counters collect deterministic untimed workload snapshots. Production detector libraries and latency benchmark remain unchanged; a dedicated workload executable links only instrumented variants and feeds profiler summaries.

**Tech Stack:** Rust 2021/C ABI, AprilTag C 3.4.5, C/C++, CMake, Buildroot patch/package infrastructure, Docker `rvv-dev:latest`.

---

### Task 1: Rust Counter Schema And C ABI

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/Cargo.toml`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/scripts/build-capi.sh`
- Test: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`

- [ ] Add failing layout/default/reset tests for `WorkloadCounters` and validity flags.
- [ ] Add `workload-counters` feature and schema-v1 plain `u64` fields.
- [ ] Store latest counters in `DetectBuffers`, reset at detect entry, and expose an independent getter that does not enable debug diagnostics.
- [ ] Export `apriltag_workload_counters_t` and `apriltag_get_workload_counters()` under the feature.
- [ ] Extend `build-capi.sh --workload-counters` to emit `libapriltag_rvv_workload.a` without overwriting production archive.
- [ ] Run Docker host tests and C-ABI staticlib builds; confirm production archive hash is unchanged.

### Task 2: Rust Segmentation And Fit Counters

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/lfps.rs`

- [ ] Add synthetic failing tests with known threshold/run/cluster counts.
- [ ] Count decimated/threshold checksum and class pixels.
- [ ] Count runs, run UF elements, local union attempts/successes, pending records/expanded units, repacked RVV runs, emitted points, and clusters.
- [ ] Count filters, points entering sort/LFPS/errors, error calls/points, peaks, pair stats/spans/lookups, quad attempts/rejects/quads.
- [ ] Add counter-only early polarity classification after point/extent filters and record hypothetical rejected clusters/points without changing control flow.
- [ ] Aggregate locally rather than incrementing shared state inside tight loops.
- [ ] Verify scalar/RVV deterministic counter snapshots for fixed fixtures.

### Task 3: Rust Decode Counters

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs`

- [ ] Add failing tests for homography, polarity, codeword, raw detection, duplicate, and final counts.
- [ ] Count decode outcomes in normal mode without nearest-code searches or candidate vectors.
- [ ] Keep definitions distinct: family candidate, homography reject, actual decode attempt, photometric polarity reject, codeword reject, raw/final detection.
- [ ] Verify enabling counters does not change detections/checksums versus production feature-disabled build.

### Task 4: Separate Instrumented C Library

**Files:**
- Create: `buildroot-overlay/package/apriltag/0002-add-workload-counter-instrumentation.patch`
- Modify: `buildroot-overlay/package/apriltag/apriltag.mk`

- [ ] Patch 3.4.5 behind `APRILTAG_WORKLOAD_COUNTERS` with schema-v1 struct, reset/get API, and detector-local plain counters.
- [ ] Instrument threshold checksum/classes, active pixel UF, union operations, boundary candidates/emissions, clusters/points/filters/polarity, LFPS/errors/peaks/fit-line queries/quads, decode outcomes, and detections.
- [ ] Build production `libapriltag.a` normally and a separately compiled/renamed `libapriltag_workload.a` with counter macro enabled.
- [ ] Ensure single-thread benchmark counters need no atomics; aggregate task-local state if patched paths can use workers.
- [ ] Compare production and instrumented detections on fixture; verify production library hash/code remains unchanged.

### Task 5: Workload Comparison Executable

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/bench/workload_main.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/workload_backend.h`
- Create: `buildroot-overlay/package/apriltag_demo/bench/workload_rust.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/workload_c.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`

- [ ] Reuse strict input preparation and detector options from the benchmark.
- [ ] Link instrumented Rust and C variants without symbol collision; if necessary expose prefixed C workload symbols from the patch.
- [ ] Run each backend twice untimed and reject differing snapshots.
- [ ] Print common side-by-side table, implementation-specific sections, early polarity waste ratios, and stable `WORKLOAD` records.
- [ ] Install `/root/app/apriltag_bench/k230_apriltag_workload` and retain symbols.

### Task 6: Profiler Integration

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`

- [ ] Run workload tool before perf runs using identical input/options.
- [ ] Save `workload.log` and derive `workload-summary.txt`.
- [ ] Merge workload ratios into `summary.txt`, including points per stage and approximate time per point.
- [ ] Preserve warnings when threshold checksums or final outputs differ.
- [ ] Keep instrumented workload runs separate from authoritative latency/perf.

### Task 7: Verification

- [ ] Docker: run Rust workload tests, production tests, scalar/RVV cross-checks, and build both archives.
- [ ] Verify production Rust archive hash before/after workload build.
- [ ] Buildroot: build pristine and instrumented C archives, workload executable, benchmark, demos, and tests.
- [ ] Verify production C demo/benchmark link pristine `libapriltag.a`; workload tool links instrumented library.
- [ ] Run workload tool on JPEG native/1280x720 and raw input; verify deterministic records.
- [ ] Run fast profiler and verify workload files and combined summary.
- [ ] Run `git diff --check` in both repositories.
