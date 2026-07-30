# K230 AprilTag Detector Benchmark Design

**Date:** 2026-07-30
**Status:** Approved design, pending implementation planning

## 1. Goal

Create a deterministic, single-threaded K230 Linux benchmark that compares the
complete Rust/RVV and official AprilRobotics C AprilTag detector invocations on
identical grayscale input. The timed path must exclude camera capture,
V4L2 buffering, ISP operation, display threads, DRM, OSD drawing, and image
loading or conversion.

The benchmark reports the practical production API throughput of each detector
under matched settings. It does not modify the official C implementation.

## 2. Measured Backends

One C++ benchmark executable supports three backends:

- `rust-rvv`: the Rust detector through `apriltag_detect()` with RVV mode.
- `rust-scalar`: the same Rust detector through `apriltag_detect()` with scalar
  mode.
- `c-reference`: official AprilTag C 3.4.5 through
  `apriltag_detector_detect()`.

Runtime selection:

```text
--backend rust-rvv
--backend rust-scalar
--backend c
--backend all
```

`all` is the default comparison mode. Backends execute sequentially, never
concurrently.

The Rust timing includes its public C ABI argument checks, panic boundary, and
copy into the caller's fixed output array. The C timing includes creation of
the returned `zarray` and detections and their destruction. These are each
backend's normal production API costs and are part of the comparison.

## 3. Matched Detector Configuration

All backends receive exactly the same immutable packed Y8 image buffer,
dimensions, and stride. The comparison configuration is:

| Setting | Rust | C reference |
|---|---|---|
| Input | packed Y8 | same bytes |
| Tag family | Tag36h11 | Tag36h11 |
| Quad decimation | runtime factor | `quad_decimate` |
| Minimum cluster | runtime value | `qtp.min_cluster_pixels` |
| Corrected bits | exact code only | 0 |
| Edge refinement | unavailable/off | off |
| Decode sharpening | unavailable/off | 0.0 |
| Threads | one benchmark thread | `nthreads = 1` |
| Debug output | disabled | disabled |
| Decode image | original benchmark image | same image |

Defaults are factor 2, minimum blob size 25, RVV enabled for the primary Rust
comparison, and exact Tag36h11 matching.

## 4. Input Handling

The executable supports variable benchmark resolutions. Runtime input contents
and geometry are replaceable.

Supported formats:

- **Raw Y8:** packed grayscale whose exact dimensions are supplied by `--size`.
- **JPEG:** decoded once, converted to grayscale, and either retained at native
  dimensions or resized before detector initialization, validation, warmup, or
  timing.

Format may be selected explicitly or inferred from the extension:

```text
--input PATH
--format auto|raw|jpeg
--size native|WxH
```

For JPEG input, `--size native` is the executable default. `--size WxH`
resizes directly to that exact geometry. For raw input, `--size WxH` is
mandatory because dimensions cannot be inferred from packed bytes, and file
size must equal `width * height` exactly.

The installed default fixture is
`apriltag-rvv/tests/data/33369213973_9d9bb4cc96_c.jpg`. The wrapper selects
`--size 1280x720` by default to represent the primary robot workload, while
users may select native resolution or another size. Direct resizing may alter
the source aspect ratio, but it is deterministic and outside measurement.

Users can replace the fixture with a captured packed grayscale frame at any
supported resolution by supplying its dimensions. The benchmark prints input
width, height, byte count, and checksum so results are compared only when the
post-conversion bytes and geometry are identical.

JPEG loading and resizing use OpenCV already available in the Buildroot SDK.
Neither operation is included in warmup or measurement.

## 5. Benchmark Lifecycle

The executable performs these phases:

1. Parse and validate arguments.
2. Load raw or JPEG input and produce one packed Y8 buffer at the requested
   geometry.
3. Compute and print the checksum of the converted input.
4. Construct one persistent detector instance per selected backend.
5. Run one untimed validation call per backend.
6. Verify stable per-backend detection count and output checksum.
7. Run untimed warmup calls for each backend.
8. Run alternating measured batches.
9. Consume every output into a checksum.
10. Print live batch progress, final tables, and machine-readable records.
11. Destroy detector instances after all reporting.

No image conversion, allocation of benchmark output arrays, detector
construction, validation, logging, or checksum formatting occurs inside the
timed invocation. Detector-internal allocation remains part of the measured
implementation.

## 6. Warmup and Batches

Defaults:

```text
--warmup 10
--iterations 100
--batches 10
```

Warmup grows Rust's persistent `DetectBuffers`, initializes C detector state,
faults code and data pages, and warms the input and detector working sets.

Each call is timed separately with `CLOCK_MONOTONIC_RAW`. Per-call samples are
retained so percentile and variation statistics can be calculated. Printing
occurs only after a batch, never during a detector invocation.

When multiple backends are selected, batch order alternates to reduce thermal
and ordering bias:

```text
batch 1: Rust RVV, C reference, Rust scalar
batch 2: Rust scalar, C reference, Rust RVV
batch 3: Rust RVV, C reference, Rust scalar
...
```

If only Rust RVV and C are selected, their order simply reverses each batch.

## 7. Output Consumption and Validation

The benchmark consumes all result fields after each invocation:

- detection count;
- ID;
- decision margin bit representation;
- center coordinates; and
- all four corners.

These values feed a deterministic 64-bit rolling checksum. This prevents
optimizers from discarding detector work and detects unstable results.

Validation rules:

- Each backend must return a stable detection count and checksum across its own
  warmup and measured calls.
- Instability is a benchmark failure and suppresses performance conclusions.
- Different backends may produce different counts or checksums because their
  remaining sampling and algorithm details are not yet identical.
- Cross-backend differences produce a prominent warning but do not abort the
  timing run.
- The report must not label differing outputs as equivalent-output speedups.

The input checksum, detector output checksum, build identity, and complete
configuration are printed with every result.

## 8. Statistics

For every selected backend, calculate over all individual measured calls:

- call count;
- stable detections per call;
- total measured time;
- minimum latency;
- median latency;
- arithmetic mean latency;
- 95th percentile latency;
- maximum latency;
- population standard deviation;
- mean throughput in detector calls per second; and
- median-equivalent throughput.

Percentiles use sorted samples and nearest-rank indexing. Values are reported
in milliseconds with sufficient precision to distinguish backends.

Relative comparison uses mean latency. C reference is the baseline when
selected:

```text
speedup = C mean latency / backend mean latency
faster_percent = (1 - backend mean latency / C mean latency) * 100
```

## 9. Live Terminal Output

Startup summary:

```text
K230 AprilTag detector benchmark
Input       : fixture.jpg
Image       : 1280x720 Y8
Input hash  : 7e4d...
Factor      : 2.0
Min blob    : 25
Warmup      : 10 calls/backend
Measurement : 10 batches x 100 calls/backend
Backends    : Rust RVV, C reference

Warming up Rust RVV ..... 8 detections
Warming up C reference .. 8 detections
```

Live progress is one flushed row per batch:

```text
Batch   Rust RVV       C reference     ETA
-----   -------------- --------------- --------
 1/10     18.42 ms       31.87 ms       00:43
 2/10     18.36 ms       31.65 ms       00:38
```

Each displayed batch value is the mean latency per detector call in that
batch. ETA is based on elapsed completed batches.

Final human-readable report:

```text
AprilTag detect() results
=========================

Metric                 Rust RVV       C reference
---------------------  -------------  -------------
Detections/call        8              8
Minimum                18.21 ms       31.42 ms
Median                 18.37 ms       31.70 ms
Mean                   18.39 ms       31.76 ms
95th percentile        18.61 ms       32.20 ms
Maximum                18.94 ms       32.83 ms
Standard deviation      0.12 ms        0.31 ms
Mean throughput         54.38 FPS      31.49 FPS
Median throughput       54.44 FPS      31.55 FPS
Relative speed          1.73x          baseline
Checksum                a7c2...        a7c2...

Rust RVV is 42.1% faster than the C reference.
```

If outputs differ:

```text
WARNING: backend results differ
  Rust RVV   : 8 detections, checksum a7c2...
  C reference: 7 detections, checksum 84ef...
Performance numbers remain valid, but this is not equivalent-output speed.
```

After the readable table, emit one stable line per backend for scripts:

```text
RESULT backend=rust-rvv calls=1000 min_ms=18.21 median_ms=18.37 mean_ms=18.39 p95_ms=18.61 max_ms=18.94 stddev_ms=0.12 mean_fps=54.38 median_fps=54.44 detections=8 checksum=a7c2...
RESULT backend=c-reference calls=1000 min_ms=31.42 median_ms=31.70 mean_ms=31.76 p95_ms=32.20 max_ms=32.83 stddev_ms=0.31 mean_fps=31.49 median_fps=31.55 detections=8 checksum=a7c2...
```

## 9.1 Visual Validation Dumps

The benchmark can write one visual validation image per selected backend from
the untimed validation call:

```text
<dump-dir>/input.png
<dump-dir>/rust-rvv-detections.png
<dump-dir>/rust-scalar-detections.png
<dump-dir>/c-reference-detections.png
```

Every backend image uses the identical prepared grayscale input and overlays:

- color-coded quadrilateral edges;
- a center marker;
- tag ID and decision margin;
- backend name; and
- detection count.

An image with no detections is still written and labeled `No detections`.
These files are generated after validation and before warmup. Color conversion,
drawing, text rendering, and image encoding are outside every timed detector
invocation.

Runtime controls:

```text
--dump-dir PATH       write input and backend overlay images
--no-dump             disable visual dumps
```

The executable does not dump unless `--dump-dir` is supplied. The packaged
wrapper enables dumps by default under its timestamped result directory:

```text
results/<timestamp>/images/
```

Failure to create the requested directory or write any requested image aborts
before measurement so a run cannot silently omit its validation artifacts.

## 10. Runtime Interface

```text
k230_apriltag_bench [options]

--input PATH          raw Y8 or JPEG input
--format FORMAT       auto, raw, or jpeg
--size SIZE           native or WxH; raw requires WxH
--backend BACKEND     all, rust-rvv, rust-scalar, or c
--factor FACTOR       1, 1.5, or 2
--min-blob N          minimum cluster pixels
--warmup N            untimed calls per backend
--iterations N        calls per measured batch
--batches N           measured batches
--dump-dir PATH       write visual validation images
--no-dump             disable visual validation images
--help                 print usage
```

All numeric arguments reject malformed, zero where prohibited, overflowing,
or trailing input. Unknown options and unsupported factor/backend/format values
exit with status 2 and a concise message.

## 11. Perf Integration

The internal clock is authoritative for detector-call latency. Linux `perf`
provides process-level counters:

```sh
perf stat \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  /root/app/apriltag_bench/k230_apriltag_bench \
  --backend rust-rvv \
  --input /root/app/apriltag_bench/fixture.jpg \
  --size 1280x720 --factor 2 --warmup 10 --iterations 100 --batches 10
```

Run each backend separately under `perf stat`; process scope is appropriate
because the benchmark is single-threaded and has no camera or display workers.
Perf includes setup and warmup, so measured iterations must dominate total
runtime. No raw `rdcycle` dependency is added initially.

The benchmark wrapper records available CPU governor and frequency information
from sysfs. It does not fail if cpufreq is unavailable and does not silently
change system policy. Tests should run with live camera/display applications
stopped.

## 12. Build and Packaging

The benchmark is added to the existing `apriltag_demo` Buildroot package.
This package already supplies:

- the official C AprilTag library;
- the Rust static library produced in `rvv-dev:latest`;
- OpenCV image decode and resize support;
- target `perf` and binutils; and
- Debian packaging.

The benchmark target links only the libraries needed for image preparation and
the two detectors. It does not link V4L2, DRM, display, or MMZ.

Installed layout:

```text
/root/app/apriltag_bench/k230_apriltag_bench
/root/app/apriltag_bench/fixture.jpg
/root/app/apriltag_bench/run_benchmark.sh
```

The benchmark ELF is added to the no-strip list so `perf report` can resolve
Rust and C detector symbols.

All `apriltag-rvv` builds and tests run in `rvv-dev:latest`. The SDK package is
then rebuilt with the generated RISC-V static archive.

## 13. Wrapper Script

`run_benchmark.sh` provides convenient board operation:

- verifies the benchmark and default fixture exist;
- warns if live AprilTag demo processes are running;
- prints cpufreq governor/current-frequency state when available;
- creates a timestamped result directory;
- runs the readable `--backend all` comparison;
- runs separate `perf stat` commands for Rust RVV and C reference; and
- stores terminal and perf output while preserving live display through `tee`.

The wrapper accepts additional benchmark arguments and permits replacing the
input path. It does not hide benchmark failures.

## 13.1 Detector-Only Profiling Suite

The packaged benchmark includes a one-command profiler:

```text
/root/app/apriltag_bench/profile_detector.sh
```

It accepts two modes followed by normal benchmark input/configuration options:

```sh
./profile_detector.sh fast [benchmark options]
./profile_detector.sh full [benchmark options]
```

The target Linux configuration exposes one application CPU, so the profiler
does not require or attempt CPU affinity control. It records online CPUs and
frequency/governor state for reproducibility.

### Fast mode

- runs one all-backend benchmark comparison with visual dumps;
- runs Rust RVV, Rust scalar, and C reference separately under `perf stat`;
- records one flat sampled profile per backend;
- generates top-symbol reports; and
- writes a consolidated summary.

### Full mode

Includes fast mode plus:

- repeated `perf stat` runs;
- frame-pointer callgraph profiles and caller/children reports;
- Rust coarse-stage annotation reports;
- available C detector annotation reports; and
- a more detailed consolidated stage summary.

The profiler probes event support before measurement. Unsupported events are
removed with warnings. Sampling prefers `cycles:u` and falls back to
`cpu-clock` if hardware sampling is unavailable. The chosen event and all
omissions are recorded.

Default Rust annotation targets are:

```text
apriltag_rvv::pipeline::detect
apriltag_rvv::pipeline::ccl_and_boundary_extract
apriltag_rvv::pipeline::filter_and_sort_clusters_impl
apriltag_rvv::pipeline::sort_by_angle
apriltag_rvv::pipeline::compute_errors_into
apriltag_rvv::pipeline::precompute_peak_pair_stats
apriltag_rvv::pipeline::search_peak_quad
apriltag_rvv::pipeline::fit_quad_from_cluster_with_scratch
apriltag_rvv::pipeline::decode_quad_detailed
apriltag_rvv::pipeline::deduplicate_detections
```

Each backend runs in a separate single-threaded process. Repeated profiling
runs disable image dumps after the initial visual validation comparison.

Output is stored under a timestamped directory containing environment data,
comparison logs, images, per-backend stat/data/report files, optional
callgraphs and annotations, and `summary.txt`. An explicit
`APRILTAG_PROFILE_OUTPUT` is an output root: each invocation creates a unique
`run-<timestamp>-<pid>` child and never deletes existing content.

Environment controls:

```text
APRILTAG_PROFILE_EVENTS
APRILTAG_PROFILE_REPEATS
APRILTAG_PROFILE_FREQUENCY
APRILTAG_PROFILE_OUTPUT
APRILTAG_PROFILE_WARMUP
APRILTAG_PROFILE_ITERATIONS
APRILTAG_PROFILE_BATCHES
```

The consolidated summary reports internal mean/median latency, supported
cycles and instructions per call, IPC when valid, top symbols, approximate
milliseconds per sampled symbol, Rust RVV versus scalar improvement, and Rust
RVV versus C gap. Approximate sampled stage times are labeled as estimates.

## 14. Error Handling

- Input loading or decode failure exits before detector construction.
- Raw input without explicit dimensions or with a size other than
  `width * height` is rejected.
- JPEG conversion must produce exactly the requested dimensions and packed Y8
  layout.
- Detector construction failure identifies the backend and exits.
- Rust negative return values abort the run and report the call and batch.
- Unexpected output instability aborts that backend's result.
- Empty detections are valid if stable and are still timed.
- Cross-backend differences warn but do not fail.
- Timing arithmetic uses checked sizes and finite floating-point results.

## 15. Verification

Host/build verification:

- argument parser tests for valid and invalid options;
- raw-size and JPEG-conversion tests;
- statistics tests with known sample vectors;
- percentile and standard-deviation tests;
- checksum stability tests;
- backend configuration tests;
- a short benchmark smoke run for all backends;
- visual dump tests for overlays, no-detection labels, and write failures;
- Buildroot cross-build of benchmark and both existing demos;
- ELF architecture and symbol checks; and
- `git diff --check` in both repositories.

On-device verification:

- default fixture runs with stable counts/checksums;
- alternate captured raw input runs without conversion;
- native and resized JPEG runs report correct geometry;
- representative 640x360, 1280x720, and 1920x1080 runs complete;
- Rust RVV, Rust scalar, and C reference all complete;
- live batch rows and final tables remain readable;
- input and backend overlay images are written before measurement;
- `RESULT` lines parse with `grep '^RESULT '`;
- `perf stat` records counters for each backend;
- repeated runs at idle produce bounded variance; and
- output mismatches are clearly warned rather than hidden.

## 16. Success Criteria

The benchmark succeeds when:

1. Rust RVV and C reference process identical packed bytes and geometry under
   matched detector settings.
2. The timed interval contains one complete public detector invocation and its
   normal result lifecycle, but no input conversion, camera, display, or log
   work.
3. Results include min, median, mean, p95, max, standard deviation, throughput,
   detections, and checksums.
4. Live output is readable and machine-readable records remain available.
5. Output instability fails the benchmark and backend differences are visible.
6. The benchmark runs from the packaged K230 Linux filesystem and under target
   `perf stat`.
7. The official C source remains unchanged.
