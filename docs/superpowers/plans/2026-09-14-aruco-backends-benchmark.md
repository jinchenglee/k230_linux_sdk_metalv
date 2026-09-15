# ArUco Nano and ArUco2 integration and experiment record

## Completed scope

ArUco Nano and ArUco2 are independent, reusable Tag36h11 detector backends in
`k230_apriltag_bench` and in a selectable live-camera application. The
existing scalar small-core Linux configuration was used to compare latency and
detection behavior with the three existing backends.

The five benchmark backends are:

1. Rust RVV mode (`rust-rvv`), which remains unchanged and executes its
   compiled scalar fallback in the vector-free small-core build;
2. AprilTag 3 C reference (`c` / `c-reference`);
3. Rust scalar (`rust-scalar`);
4. ArUco Nano (`aruco-nano`); and
5. ArUco2 (`aruco2`).

This experiment does not build or benchmark a big-core Linux image and does
not claim that either ArUco implementation has an RVV backend. A future RVV
ArUco implementation would require separately designed vector kernels.

## Upstream versions and packaging

The packages pin exact upstream revisions and record archive hashes and licenses:

- ArUco Nano `961b18b747d64cc3692c570dff35334c098b9e62`, MIT, plus Apache-2.0 for the narrow
  OpenCV dictionary unit;
- ArUco2 `93bfda4f5fec85037a3d3baa8daeed06cfe37c3c`, Apache-2.0.

Separate Buildroot packages make either implementation reusable by the live
application or a future AMP service.

- `aruco_nano` installs its single public header into staging and installs
  nothing into the target rootfs.
- `aruco2` builds a detector-only static library and installs that library plus
  its public header into staging. Its utility-only upstream CMake project is
  patched to expose a normal library target, omit unused board/pose/fractal
  APIs, and make utility programs optional. The fiducial detector code remains
  intact.
- Both packages use the SDK OpenCV 4.10 build. Selections remain minimal:
  ArUco2 needs only core and imgproc. ArUco Nano calls OpenCV
  `Dictionary::identify`; its exact OpenCV 4.10 dictionary translation unit is
  compiled as a small static archive rather than enabling the full objdetect module
  and its mandatory calib3d/features2d/FLANN/ML chain.

## Benchmark integration

Each implementation has its own adapter translation unit:

- wrap `PreparedImage` as a non-owning, stride-aware `CV_8UC1` matrix;
- explicitly select the AprilTag 36h11 dictionary;
- use standard black-on-white detection, exact dictionary matching, and no
  accepted border errors;
- normalize ID, source-image corners, and computed center into the existing
  `Detection` structure;
- represent unavailable AprilTag decision margin explicitly rather than
  fabricating a comparable confidence value; and
- keep per-call output stable so the existing warmup/measurement guards still
  detect nondeterminism.

The integration extends the CLI, names, `--backend all`, alternating batch
order, `RESULT` records, visual dumps, build identity, and unit tests for five
backends. The existing Rust and AprilTag 3 backend implementations remain
unchanged.

The profile and workload executables remain Rust/AprilTag-specific. The new
backends belong to the ordinary fixed-image benchmark and `aruco_demo.elf`.
The live application reuses the existing camera, grayscale, display, keyboard,
and FPS shell and selects `nano|aruco2` and `strict|tolerant` at runtime.

## Decimation and input scaling

ArUco input scaling is explicitly distinct from AprilTag `quad_decimate`.
The existing backends preserve their established `--factor` behavior, and
controlled comparisons use factor 1.

The adapters implement explicit per-ArUco input scaling for factors 1, 1.5, and
2. Each backend reports input scaling, native detector, adapter/normalization
residual, and end-to-end latency separately. Factor 1 reports exactly
zero scaling time. This scaling is benchmark harness work, not part of either
native ArUco algorithm. Detections are mapped back to source-image coordinates.

## Acceptance presets

`strict` and `tolerant` are project-defined convenience presets, not modes
named by either upstream implementation. Strict sets payload correction and
accepted border-error rates to zero. Tolerant sets both rates to 1, the maximum
allowed by these controls. Tolerant is an experimental recall-oriented setting,
not the recommended production default: it costs additional runtime and can
increase false acceptance. The benchmark also exposes both rates independently.

## Accuracy boundary and observed results

The benchmark exports normalized detections with `--detections-out`, but no
independent ground-truth corpus was added. Counts, cross-backend agreement,
manual inspection, and temporal consistency are evidence, not precision/recall.
The AprilTag C backend remains a comparison implementation rather than truth.

On the 1280x720 fixture, all AprilTag paths found 12 real tags while strict Nano
and strict ArUco2 found 9. Payload correction alone did not change that result;
relaxing border acceptance recovered tags. At the full tolerant operating point,
Nano found 11 and ArUco2 found 14. Manual inspection confirmed all 14 are real;
the two beyond the AprilTag result are extreme-angle box-top tags with limited
practical pose value. This difficult, small-tag fixture is useful as a stress
case but cannot establish general detector quality.

The 65-frame, 1280x800 `220-225.mp4` experiment at factor 1 produced:

| Backend / mode | Detections | Mean/frame | Speed vs C |
| --- | ---: | ---: | ---: |
| Rust RVV scalar fallback | 131 | 353.678 ms | 1.28x |
| AprilTag 3 C | 139 | 453.207 ms | 1.00x |
| Rust scalar | 131 | 352.533 ms | 1.29x |
| ArUco Nano strict | 200 | 82.235 ms | 5.51x |
| ArUco2 strict | 200 | 83.290 ms | 5.44x |
| ArUco Nano tolerant | 218 | 97.345 ms | 4.66x |
| ArUco2 tolerant | 301 | 95.353 ms | 4.75x |

Strict Nano and strict ArUco2 agreed on count and ID multiset in all 65 frames;
their 200 matched centers differed by 0.180 pixels on average and 1.079 pixels
at worst. Tolerant Nano added 18 instances and tolerant ArUco2 added 101; 91 of
the latter repeated with the same ID and nearby position in an adjacent frame.
Tolerance increased Nano mean time by 18.4 percent and ArUco2 by 14.5 percent.

Per-backend PNGs and a six-panel low-compression video were generated for visual
review. Large media and per-run logs remain ignored under `output/`; this note
retains the durable measurements. A future labeled evaluator should report
TP/FP/FN, precision, recall, F1, duplicates, and localization error. Until then,
strict and tolerant are operating points, not accuracy rankings.

## Verification completed

1. Benchmark parser/core host tests cover five fake backends, JSON export,
   runtime breakdown, CLI selectors, and deterministic repeated output.
2. Both pinned packages build under `k230_canmv_small_core_defconfig`.
3. The five-backend benchmark and all three live applications compile, install
   into the target tree and Debian package, and pass the final vector audit.
4. Fixed-image and video runs completed on K230 small-core Linux.
5. `aruco_demo.elf` completed an on-board live-camera test.

## Permanent image integration

`k230_canmv_small_core_defconfig` already selects
`BR2_PACKAGE_APRILTAG_DEMO=y`. That package selects the two minimal ArUco
packages and installs `/root/app/aruco_demo/`, the five-backend benchmark,
and all earlier AprilTag tools. Full image builds therefore include this work
without a separate board copy. The same application directories are included
in `k230-apriltag-demo.deb`. AMP Phase 4 protocol work remains out of scope.
