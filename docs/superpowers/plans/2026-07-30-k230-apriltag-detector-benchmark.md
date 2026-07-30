# K230 AprilTag Detector Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and package a single-threaded K230 benchmark that compares Rust RVV, Rust scalar, and official C AprilTag detector performance on identical variable-resolution grayscale input.

**Architecture:** Add a standalone C++ executable to the existing `apriltag_demo` Buildroot package. It prepares JPEG or raw Y8 input outside measurement, drives both detector APIs through small backend adapters, records per-call latency and stable result checksums, and prints live and machine-readable reports without linking camera/display code.

**Tech Stack:** C++17, OpenCV image loading/resizing, Rust C ABI static library, official AprilTag C 3.4.5 library, CMake, Buildroot, Linux `CLOCK_MONOTONIC_RAW`, target `perf`.

---

## File Structure

- Create `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`: shared types, parser, statistics, checksum, and backend interface declarations.
- Create `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`: argument parsing, input preparation, timing, statistics, reporting, and orchestration.
- Create `buildroot-overlay/package/apriltag_demo/bench/rust_backend.cc`: persistent Rust detector adapter.
- Create `buildroot-overlay/package/apriltag_demo/bench/c_backend.cc`: persistent official-C detector adapter; no official source modifications.
- Create `buildroot-overlay/package/apriltag_demo/bench/main.cc`: exception boundary and process exit codes.
- Create `buildroot-overlay/package/apriltag_demo/bench/tests.cc`: host-independent unit tests for parsing, statistics, checksums, and input validation helpers.
- Create `buildroot-overlay/package/apriltag_demo/utils_bench/run_benchmark.sh`: on-device readable comparison and separate `perf stat` runs.
- Copy `apriltag-rvv/tests/data/33369213973_9d9bb4cc96_c.jpg` to `buildroot-overlay/package/apriltag_demo/bench/fixture.jpg` with its existing license provenance documented.
- Modify `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`: benchmark/test targets, links, and installation.
- Modify `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`: include benchmark in the Debian package.
- Modify `buildroot-overlay/package/apriltag_demo/README.md`: usage, interpretation, and perf commands.
- Modify `buildroot-overlay/configs/k230_canmv_defconfig`: retain benchmark symbols.

## Task 1: Parser and Configuration Model

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Create: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] **Step 1: Write failing parser tests**

Define `BenchmarkConfig`, `BackendKind`, `InputFormat`, and `ImageSize`, then add tests invoking `parse_args()` with arrays for:

```cpp
assert(parse({"bench", "--input", "x.jpg"}).size.native);
assert(parse({"bench", "--input", "x.jpg", "--size", "1280x720"}).size.width == 1280);
assert(parse({"bench", "--input", "x.y8", "--format", "raw", "--size", "640x480"}).format == InputFormat::Raw);
assert(parse({"bench", "--backend", "rust-rvv"}).backends == std::vector{BackendKind::RustRvv});
assert(parse({"bench", "--backend", "all"}).backends.size() == 3);
expect_parse_error({"bench", "--factor", "3"});
expect_parse_error({"bench", "--size", "1280"});
expect_parse_error({"bench", "--warmup", "-1"});
expect_parse_error({"bench", "--iterations", "0"});
```

- [ ] **Step 2: Add a temporary CMake unit-test target and verify RED**

Add `apriltag_bench_tests` from `bench/tests.cc` and `bench/benchmark.cc`, defining `APRILTAG_BENCH_TEST`. Configure with the Buildroot toolchain and build the target.

Expected: compilation fails because parser types/functions are undeclared or undefined.

- [ ] **Step 3: Implement strict parsing**

Implement these defaults:

```cpp
input = "fixture.jpg";
format = InputFormat::Auto;
size = ImageSize::native_size();
backends = {RustRvv, CReference, RustScalar};
factor = 2;
factor_value = 2.0;
min_blob = 25;
warmup = 10;
iterations = 100;
batches = 10;
```

Use `std::from_chars` for integers; reject unknown/trailing values and require `--size WxH` for explicit raw format.

- [ ] **Step 4: Build and run parser tests**

Expected: all parser tests pass.

## Task 2: Input Preparation

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/fixture.jpg`

- [ ] **Step 1: Write failing raw and geometry tests**

Add tests for:

```cpp
auto image = load_raw(path_16_bytes, {4, 4});
assert(image.width == 4 && image.height == 4 && image.stride == 4);
assert(image.pixels.size() == 16);
expect_input_error(path_15_bytes, {4, 4});
expect_input_error(raw_path, ImageSize::native_size());
```

Add a pure `validate_image()` test rejecting zero dimensions and overflow.

- [ ] **Step 2: Verify tests fail because input helpers are absent**

- [ ] **Step 3: Implement `PreparedImage` and loading**

For raw: read exact `width * height` bytes. For JPEG: use `cv::imread(..., IMREAD_GRAYSCALE)` and optionally `cv::resize(..., INTER_LINEAR)`, then copy active rows into packed storage. Infer JPEG format for `.jpg`/`.jpeg`, raw for `.y8`/`.gray`/`.raw`, otherwise require `--format`.

- [ ] **Step 4: Copy and document fixture provenance**

Copy the existing repository JPEG unchanged to `bench/fixture.jpg`; add a README note identifying its source path. Do not generate a resized file because runtime `--size` determines geometry.

- [ ] **Step 5: Run unit tests at native, 640x360, 1280x720, and 1920x1080 preparation sizes**

Expected: packed output size equals `width * height` for each.

## Task 3: Statistics and Checksums

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] **Step 1: Write failing deterministic statistics tests**

For samples `{1, 2, 3, 4, 10}` milliseconds assert:

```cpp
min == 1;
median == 3;
mean == 4;
p95 == 10; // nearest rank
max == 10;
stddev == sqrt(10.0);
mean_fps == 250;
median_fps == 1000.0 / 3.0;
```

Add checksum tests proving one changed ID, center, margin, or corner changes the checksum and identical output remains stable.

- [ ] **Step 2: Verify RED**

- [ ] **Step 3: Implement `compute_stats()` and FNV-1a-style output hashing**

Hash integer fields and `std::bit_cast<uint64_t>` representations of doubles. Sort a copy for percentiles while preserving raw samples for batch output.

- [ ] **Step 4: Run tests and verify GREEN**

## Task 4: Rust Backend Adapter

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/bench/rust_backend.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] **Step 1: Write a failing Rust backend smoke test**

Construct a backend with factor 2/mode scalar, process the prepared fixture once, assert a nonnegative count, and assert two calls have identical count/checksum.

- [ ] **Step 2: Verify linkage/test failure before adapter implementation**

- [ ] **Step 3: Implement persistent Rust adapter**

Call `apriltag_new(min_blob)` once, allocate `std::vector<apriltag_det_t>(256)` once, map factor to 0/1/2 and mode to scalar/RVV, call `apriltag_detect()`, and hash returned fields. Throw on negative return. Call `apriltag_free()` in the destructor.

- [ ] **Step 4: Run scalar smoke test**

Expected: stable result. RVV execution is deferred to K230; x86 mode dispatch falls back safely but is not treated as RVV performance evidence.

## Task 5: Official C Backend Adapter

**Files:**
- Create: `buildroot-overlay/package/apriltag_demo/bench/c_backend.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] **Step 1: Write a failing C backend smoke test**

Run the same prepared fixture twice and assert stable count/checksum.

- [ ] **Step 2: Verify RED before implementation**

- [ ] **Step 3: Implement official API adapter without source modifications**

Create `apriltag_detector_t` and `tag36h11`, configure:

```cpp
detector->nthreads = 1;
detector->quad_decimate = factor_value;
detector->qtp.min_cluster_pixels = min_blob;
detector->refine_edges = false;
detector->decode_sharpening = 0.0;
apriltag_detector_add_family_bits(detector, family, 0);
```

Wrap `PreparedImage` in a non-owning `image_u8_t`, call `apriltag_detector_detect()`, hash all result fields, destroy detections per call, and destroy detector/family in the adapter destructor.

- [ ] **Step 4: Run C and Rust smoke tests and report expected cross-backend differences without failing**

## Task 6: Timing Engine and Live Output

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/main.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] **Step 1: Add failing schedule and stability tests**

Assert two backends alternate `A,B` then `B,A`; three backends alternate forward/reverse. Add a fake backend returning a changing checksum and assert benchmark validation fails before printing performance conclusions.

- [ ] **Step 2: Verify RED**

- [ ] **Step 3: Implement benchmark lifecycle**

Use `clock_gettime(CLOCK_MONOTONIC_RAW)` immediately before and after each backend call. Validate once, warm each backend, then collect individual nanosecond samples per alternating batch. Check each result against that backend's validation count/checksum.

- [ ] **Step 4: Implement readable output**

Print and flush startup/warmup status, one row per batch with per-call batch mean and ETA, final aligned metric table, relative speed, mismatch warning, and one `RESULT` line per backend.

- [ ] **Step 5: Add `main()` exception boundary**

Return 0 on success, 2 for argument errors, and 1 for input/backend/runtime failures. `--help` returns 0.

- [ ] **Step 6: Run unit and short smoke tests**

Run `--warmup 1 --iterations 2 --batches 2` for native and 1280x720 JPEG sizes, plus a generated raw fixture.

## Task 7: CMake and Buildroot Packaging

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`
- Modify: `buildroot-overlay/configs/k230_canmv_defconfig`
- Create: `buildroot-overlay/package/apriltag_demo/utils_bench/run_benchmark.sh`

- [ ] **Step 1: Add standalone benchmark CMake target**

Build `k230_apriltag_bench` from benchmark sources. Link Rust archive, official C library, `opencv_imgcodecs`, `opencv_imgproc`, `opencv_core`, `pthread`, `dl`, `m`, and `rt`; do not link V4L2, DRM, display, or MMZ.

- [ ] **Step 2: Install benchmark, fixture, and wrapper**

Install under `/root/app/apriltag_bench/` and copy it into the existing Debian staging tree.

- [ ] **Step 3: Add no-strip rule**

Extend `BR2_STRIP_EXCLUDE_FILES` with `k230_apriltag_bench` so `perf report` resolves symbols.

- [ ] **Step 4: Implement wrapper script**

Print cpufreq state if available, warn about running demo PIDs, run `--backend all --size 1280x720`, then separate `perf stat` runs for Rust RVV and C with outputs captured through `tee` in a timestamped directory.

- [ ] **Step 5: Rebuild package**

Run:

```bash
make CONF=k230_canmv_defconfig apriltag_demo-dirclean
make CONF=k230_canmv_defconfig APRILTAG_DEMO_FORCE_RUST_REBUILD=YES apriltag_demo
```

Expected: existing demos plus benchmark build/install and Debian package succeeds.

## Task 8: Documentation and Verification

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`

- [ ] **Step 1: Document commands and interpretation**

Include native/resized JPEG, arbitrary raw dimensions, all-backend comparison, individual backend perf commands, readable output, `RESULT` parsing, and the distinction between production API throughput and direct Rust function timing.

- [ ] **Step 2: Run Docker Rust verification**

```bash
docker run --rm -v "/mnt/sda_500gb/git_repo:/app" \
  -w /app/apriltag-rvv rvv-dev:latest bash -lc 'bash scripts/test-all.sh'
```

Expected: all steps pass.

- [ ] **Step 3: Run Buildroot verification**

Build package, inspect benchmark with `file`, and verify expected symbols with target `nm -C`.

- [ ] **Step 4: Run on-device matrix**

Run short comparisons at native, 640x360, 1280x720, and 1920x1080; run a captured raw image with explicit size; verify stable backend checksums, readable progress, parseable `RESULT` lines, and `perf stat` counters.

- [ ] **Step 5: Inspect final diffs**

Run `git diff --check` and `git status --short` in both repositories. Preserve unrelated untracked documentation/log files.

## Completion Gate

Do not claim completion until:

- parser/statistics/input/backend tests pass;
- a short all-backend smoke run succeeds;
- Docker `scripts/test-all.sh` passes;
- Buildroot packages all three executables;
- benchmark ELF is RISC-V and retains symbols;
- on-device output is stable for at least one JPEG and one raw fixture; and
- official AprilTag C source has no diff.
