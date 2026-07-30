# AprilTag Benchmark Visual Dumps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add untimed per-backend detection overlay images to the K230 AprilTag benchmark.

**Architecture:** Preserve normalized detection fields from each backend's validation call, then render the common prepared grayscale input and backend-specific detections through OpenCV before warmup. Keep all conversion, drawing, labels, directory creation, and PNG encoding outside detector timing.

**Tech Stack:** C++17, OpenCV core/imgproc/imgcodecs, existing benchmark backend adapters and Buildroot package.

---

### Task 1: Expose Validation Detections

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/rust_backend.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/c_backend.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] Add a failing fake-backend test requiring `Backend::detections()` to return the normalized output from the latest call.
- [ ] Run benchmark tests and verify compilation fails for the missing interface.
- [ ] Add `virtual const std::vector<Detection>& detections() const = 0` and persistent normalized storage to both adapters.
- [ ] Keep the Rust and C count/checksum path unchanged and reject potentially truncated results before exposing detections.
- [ ] Run pure and cross-built benchmark tests.

### Task 2: Parse Dump Options

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] Add failing parser tests for `--dump-dir output`, `--no-dump`, missing values, and last-option-wins behavior.
- [ ] Add optional `dump_dir` configuration, default disabled.
- [ ] Update usage output.
- [ ] Run parser tests.

### Task 3: Render Validation Images

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/bench/visual_dump.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`

- [ ] Add failing tests using a small synthetic grayscale image and one synthetic detection; require input PNG plus backend PNG and verify they decode at source dimensions.
- [ ] Implement directory creation, grayscale-to-BGR conversion, color-coded edges, center marker, ID/margin labels, backend/count header, and `No detections` label.
- [ ] Name files `input.png`, `rust-rvv-detections.png`, `rust-scalar-detections.png`, and `c-reference-detections.png`.
- [ ] Throw on directory or `imwrite` failure.
- [ ] Invoke dumps after all validation calls and before warmup; do not place rendering in backend `detect()` or measured loops.
- [ ] Add `visual_dump.cc` to benchmark and cross-test targets and run tests.

### Task 4: Wrapper and Documentation

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/utils_bench/run_benchmark.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`

- [ ] Pass `--dump-dir "$RESULT_DIR/images"` to the all-backend comparison.
- [ ] Pass `--no-dump` to separate perf runs to avoid repeated files.
- [ ] Document image names and that they are generated outside timing.
- [ ] Run wrapper self-test and syntax checks.

### Task 5: Verification

- [ ] Run pure benchmark tests.
- [ ] Rebuild the Buildroot package and cross-test target.
- [ ] Run Docker `apriltag-rvv/scripts/test-all.sh`.
- [ ] On K230, run a short all-backend benchmark and inspect all four PNG files.
- [ ] Verify benchmark timing with and without dumps differs only by run-level setup, not per-call samples.
- [ ] Run `git diff --check`.
