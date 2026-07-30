# AprilTag Detector Profile Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add fast and full one-command profiling workflows for the detector-only K230 benchmark.

**Architecture:** A POSIX shell orchestrator runs the existing benchmark in isolated backend processes, probes usable perf events, records stats and samples, generates reports/annotations, and creates a consolidated summary. It reuses benchmark `RESULT` records as authoritative latency/configuration data and never profiles camera/display applications.

**Tech Stack:** POSIX shell, Linux perf, target binutils, existing `k230_apriltag_bench`, awk.

---

### Task 1: Shared Logging and Environment Capture

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] Add `--self-test` first, exercising live logging, producer/tee status propagation, mode parsing, and temporary cleanup.
- [ ] Verify the self-test fails before helper implementation.
- [ ] Implement `run_logged()` using the existing FIFO/tee pattern.
- [ ] Parse exactly `fast|full` followed by benchmark options; invalid/missing mode exits 2.
- [ ] Record kernel, `/proc/cpuinfo`, online CPUs, PMU devices, `perf list`, cpufreq governor/current frequency, benchmark `--help`, and full command configuration to `environment.txt`.
- [ ] Warn when live demo processes are running; do not use taskset because Linux exposes one application CPU.
- [ ] Run `sh -n`, `dash -n`, and self-test.

### Task 2: Event Probing and Sampling Fallback

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] Add self-test fixtures using a fake `perf` command that marks selected events unsupported.
- [ ] Probe configured events individually with a one-call Rust benchmark and retain only events whose `perf stat` succeeds without `<not supported>`.
- [ ] Default candidates: `task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses`.
- [ ] Fail if no timing event is usable; warn for each omitted event.
- [ ] Probe `cycles:u` sampling with a short `perf record`; fall back to `cpu-clock` when it fails or produces an empty data file.
- [ ] Record selected stat/sampling events in `environment.txt`.

### Task 3: Fast Mode

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] Run one all-backend comparison with `--dump-dir "$RESULT_DIR/images"` and save `comparison.log`.
- [ ] For `rust-rvv`, `rust-scalar`, and `c`, run separate `perf stat` processes with `--no-dump` and save `.stat`/`.log` files.
- [ ] Record one flat sampled `.data` per backend using the selected event.
- [ ] Generate `.report` files with `perf report --stdio --sort=dso,symbol --percent-limit=0.1`.
- [ ] Preserve every command/tee/perf/report failure.

### Task 4: Full Mode Deep Profiles

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`
- Create: `buildroot-overlay/package/apriltag_demo/utils_bench/annotate_benchmark.sh`

- [ ] In full mode, use configurable repeated `perf stat` runs (`APRILTAG_PROFILE_REPEATS`, default 7).
- [ ] Record frame-pointer callgraphs for each backend and generate caller/children reports.
- [ ] If frame-pointer callchains are unusable, retain the flat report and record a warning rather than failing the entire suite.
- [ ] Generate Rust annotations for the approved coarse-stage symbol list.
- [ ] Generate C annotations for `apriltag_detector_detect`, threshold/cluster/quad symbols that exist in `nm -C`; skip missing symbols with a logged warning.
- [ ] Implement `annotate_benchmark.sh` to accept backend data file and symbol, with stdio and interactive modes.

### Task 5: Consolidated Summary

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/profile_detector.sh`

- [ ] Add parser self-tests for representative benchmark `RESULT` and perf-stat records.
- [ ] Extract mean/median latency, calls, counts, checksums, input hash, and build ID from `comparison.log`.
- [ ] Extract supported cycles/instructions/task-clock and calculate per-call counters and IPC only when valid.
- [ ] Extract top symbols from flat reports and convert percentages to approximate milliseconds using backend mean latency.
- [ ] Print Rust RVV versus scalar gain and Rust RVV versus C gap.
- [ ] Label stage milliseconds as sampled estimates and preserve mismatch warnings when checksums differ.
- [ ] Write and display `summary.txt`.

### Task 6: Configuration and Packaging

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`

- [ ] Install both scripts with executable permissions under `/root/app/apriltag_bench/`.
- [ ] Document `fast` and `full` commands, environment controls, output tree, event fallback, and how to interpret percentages versus milliseconds.
- [ ] Document defaults: warmup 20, iterations 50, batches 10, sample frequency 199, repeats 7 in full mode.
- [ ] Rebuild the Buildroot package and verify scripts appear in the Debian package.

### Task 7: End-to-End Verification

- [ ] Run both script self-tests and POSIX syntax checks.
- [ ] Run Docker `apriltag-rvv/scripts/test-all.sh`.
- [ ] Rebuild benchmark/test/package targets.
- [ ] On K230, run `profile_detector.sh fast` and verify comparison, images, stat/data/report, and summary files.
- [ ] On K230, run a shortened full mode and verify callgraphs and available annotations.
- [ ] Verify unsupported PMU events are omitted with warnings.
- [ ] Verify forced benchmark/perf/report failures propagate nonzero.
- [ ] Run `git diff --check`.
