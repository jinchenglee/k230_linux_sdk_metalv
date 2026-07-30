# AprilTag Full-Resolution Decode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Rust AprilTag pipeline match the C reference architecture by finding quads on the decimated image, promoting those quads to source-image coordinates, and performing homography construction and tag decoding against the original-resolution image.

**Architecture:** Keep stages 0-8 and their diagnostics in decimated space. Add one explicit quad-coordinate promotion boundary before stages 9-10, then decode against the original image and return source-image coordinates. Update the Rust C ABI, host demo, official-C adapter, and K230 overlay so every final `Detection` uses source-image coordinates independently of the selected quad-search decimation factor.

**Tech Stack:** Rust 2021, AprilTag Tag36h11 pipeline, Cargo host tests, optional `image` host feature, C++17, OpenCV drawing, Buildroot/CMake, RISC-V staticlib C ABI.

---

## Scope

This plan corrects the resolution boundary and coordinate contract only. It does
not add C's optional `refine_edges`, decode sharpening, Hamming correction, or
the remaining pixel-sampling parity changes. Those are independent follow-ups:

- C truncates border samples while Rust currently interpolates them.
- C payload interpolation uses a `-0.5` pixel-center offset.
- C skips out-of-bounds samples while Rust currently returns intensity zero.
- C can refine promoted quad edges against the source image.

The implementation spans two repositories:

- Rust detector: `/mnt/sda_500gb/git_repo/apriltag-rvv`
- K230 consumer: `/mnt/sda_500gb/git_repo/k230_linux_sdk_metalv`

At execution time, use isolated worktrees if the repositories' current dirty
state must be preserved. Do not modify the existing untracked `PanGPA.log` or
other unrelated workspace files.

## File Structure

### `apriltag-rvv`

- Modify `src/pipeline.rs`: define factor-to-source scaling, promote quads at
  the stage 8/9 boundary, decode from the source image, keep stage-4 candidate
  diagnostics decimated, and render stage 5 at source resolution.
- Modify `src/capi.rs`: document and expose source-coordinate detections and
  make skeleton mode follow the same contract.
- Modify `examples/live_demo.rs`: draw source-coordinate detections directly.
- Modify `src/pipeline.md`: document the two-resolution flow.
- Create `tests/verify_pipeline_resolution.rs`: real-image regression for
  factor-independent source coordinates, source stride, IDs, and debug image
  dimensions.

### `k230_linux_sdk_metalv`

- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag.h`: document the
  source-coordinate ABI while preserving binary layout.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag_c_adapter.cc`:
  stop dividing already-source-coordinate C results by the factor.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.h`: remove
  the decimation scale from the final-detection drawing contract.
- Modify `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.cc`: map
  source coordinates to the display with only the display/source ratio.
- Modify `buildroot-overlay/package/apriltag_demo/src/main.cc`: retain factor
  as a quad-search setting, remove it from drawing, and reject invalid values.
- Modify `buildroot-overlay/package/apriltag_demo/README.md`: record the new
  coordinate and full-resolution decode behavior.
- Modify `docs/superpowers/specs/2026-07-25-apriltag-live-demo-design.md`: add a
  short superseding note without rewriting the historical design.
- Regenerate
  `buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv.a` only after
  Rust and host integration tests pass.

## Baseline Evidence

Before changes, record these known conditions:

- `pipeline::detect()` decodes from `bufs.decimated` in both diagnostic and
  normal paths at `src/pipeline.rs:2285-2294` and `:2352-2357`.
- Factor-2 real-image output is in half-resolution coordinates; for
  `tests/data/33369213973_9d9bb4cc96_c.jpg`, the current source reports seven
  factor-2 detections centered around values such as `(170.3, 179.8)` rather
  than source values around `(341, 359)`.
- Plain `cargo test --target x86_64-unknown-linux-gnu` currently tries to build
  feature-gated examples and fails independently of this work. Use the scoped
  commands in this plan and report that pre-existing full-command failure.

### Task 1: Lock the source-coordinate quad boundary

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs:189-210`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs:2148-2169`
- Test: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs` (`#[cfg(test)]` module)

- [ ] **Step 1: Write failing factor-scale and quad-promotion tests**

Add tests near the existing homography tests. The tests define the required
coordinate mapping independently of decoding:

```rust
#[test]
fn quad_to_source_scales_every_corner_for_all_factors() {
    let quad = Quad {
        p: [[1.25, 2.5], [9.0, 2.75], [8.5, 7.0], [1.0, 6.5]],
        reversed_border: false,
    };

    for (factor, scale) in [
        (DecimateFactor::One, 1.0),
        (DecimateFactor::ThreeHalves, 1.5),
        (DecimateFactor::Two, 2.0),
    ] {
        let promoted = quad_to_source(&quad, factor);
        for i in 0..4 {
            assert_eq!(promoted.p[i][0], quad.p[i][0] * scale);
            assert_eq!(promoted.p[i][1], quad.p[i][1] * scale);
        }
        assert_eq!(promoted.reversed_border, quad.reversed_border);
    }
}

#[test]
fn source_scale_matches_reference_decimation_values() {
    assert_eq!(source_scale(DecimateFactor::One), 1.0);
    assert_eq!(source_scale(DecimateFactor::ThreeHalves), 1.5);
    assert_eq!(source_scale(DecimateFactor::Two), 2.0);
}
```

- [ ] **Step 2: Run the tests and verify the missing boundary fails**

Run:

```bash
cargo test --lib --target x86_64-unknown-linux-gnu \
  pipeline::tests::source_scale_matches_reference_decimation_values \
  -- --exact
```

Expected: compilation fails because `source_scale` does not exist. Run the
quad test after that symbol exists but before `quad_to_source` is implemented;
it must likewise fail to compile.

- [ ] **Step 3: Implement the minimal promotion helpers**

Add private helpers immediately before `detect()`:

```rust
fn source_scale(factor: DecimateFactor) -> f64 {
    match factor {
        DecimateFactor::One => 1.0,
        DecimateFactor::ThreeHalves => 1.5,
        DecimateFactor::Two => 2.0,
    }
}

fn quad_to_source(quad: &Quad, factor: DecimateFactor) -> Quad {
    let scale = source_scale(factor);
    Quad {
        p: quad.p.map(|[x, y]| [x * scale, y * scale]),
        reversed_border: quad.reversed_border,
    }
}
```

Do not mutate `quads` in place: stage-4 debug rendering and
`DecodeCandidate.center/area` must remain in decimated coordinates.

- [ ] **Step 4: Run both focused tests**

Run:

```bash
cargo test --lib --target x86_64-unknown-linux-gnu \
  pipeline::tests::source_scale_matches_reference_decimation_values \
  -- --exact
cargo test --lib --target x86_64-unknown-linux-gnu \
  pipeline::tests::quad_to_source_scales_every_corner_for_all_factors \
  -- --exact
```

Expected: both pass.

- [ ] **Step 5: Commit the coordinate boundary**

```bash
git add src/pipeline.rs
git commit -m "pipeline: define source-space quad promotion"
```

### Task 2: Decode promoted quads from the source image

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs:2262-2359`
- Test: `/mnt/sda_500gb/git_repo/apriltag-rvv/tests/verify_pipeline_resolution.rs`

- [ ] **Step 1: Create a failing real-image coordinate regression**

Create `tests/verify_pipeline_resolution.rs`. Gate it on `host` because it uses
the optional image decoder:

```rust
#![cfg(feature = "host")]

use apriltag_rvv::decimate::DecimateFactor;
use apriltag_rvv::pipeline::{detect, DetectBuffers};
use apriltag_rvv::tag36h11::Tag36h11;
use apriltag_rvv::KernelMode;

fn detect_fixture(factor: DecimateFactor, padded_stride: bool) -> Vec<apriltag_rvv::pipeline::Detection> {
    let image = image::open("tests/data/33369213973_9d9bb4cc96_c.jpg")
        .expect("fixture")
        .to_luma8();
    let width = image.width() as usize;
    let height = image.height() as usize;
    let packed = image.into_raw();
    let (storage, stride) = if padded_stride {
        let stride = width + 17;
        let mut padded = vec![0xa5; stride * height];
        for y in 0..height {
            padded[y * stride..y * stride + width]
                .copy_from_slice(&packed[y * width..(y + 1) * width]);
        }
        (padded, stride)
    } else {
        (packed, width)
    };

    detect(
        &storage,
        width,
        height,
        stride,
        factor,
        &Tag36h11,
        25,
        KernelMode::Scalar,
        None,
        &mut DetectBuffers::new(),
    )
}

#[test]
fn factor_two_returns_source_image_coordinates() {
    let detections = detect_fixture(DecimateFactor::Two, false);
    assert_eq!(detections.len(), 7);
    assert!(detections.iter().all(|d| d.center[0] > 200.0));
    assert!(detections.iter().any(|d| {
        (d.center[0] - 340.6).abs() < 4.0 &&
        (d.center[1] - 359.6).abs() < 4.0
    }));
}

#[test]
fn factor_two_source_decode_honors_padded_stride() {
    let packed = detect_fixture(DecimateFactor::Two, false);
    let padded = detect_fixture(DecimateFactor::Two, true);
    assert_eq!(packed.len(), padded.len());
    for (a, b) in packed.iter().zip(&padded) {
        assert_eq!(a.id, b.id);
        assert!((a.center[0] - b.center[0]).abs() < 1e-9);
        assert!((a.center[1] - b.center[1]).abs() < 1e-9);
        assert_eq!(a.corners, b.corners);
    }
}
```

The `> 200` assertion is intentionally specific to the checked fixture and
catches the current half-coordinate output. If detector tuning changes the
fixture count later, update the fixture baseline and explain the change rather
than weakening this to a generic non-empty assertion.

- [ ] **Step 2: Run the regression and verify it fails against current behavior**

Run:

```bash
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution -- --nocapture
```

Expected: `factor_two_returns_source_image_coordinates` fails because current
factor-2 centers are in decimated space. The padded-stride test may pass before
the fix; it protects the replacement source-image decode call.

- [ ] **Step 3: Promote each quad only at decode time**

In both diagnostic and normal loops, preserve the existing decimated `quad`
for candidate diagnostics, create `source_quad`, and decode from the source
image:

```rust
let source_quad = quad_to_source(quad, factor);
match decode_quad_detailed(
    im_gray,
    width,
    height,
    stride,
    &source_quad,
    family,
    mode,
    true,
) {
    // existing result handling
}
```

Normal path:

```rust
for quad in &quads {
    let source_quad = quad_to_source(quad, factor);
    if let Some(det) = decode_quad(
        im_gray,
        width,
        height,
        stride,
        &source_quad,
        family,
        mode,
    ) {
        detections.push(det);
    }
}
```

Keep diagnostic `DecodeCandidate.center` and `area` computed from `quad`, not
`source_quad`, because stage 4 remains a decimated diagnostic image.

- [ ] **Step 4: Update the `detect()` contract comment**

Replace the stale lines describing decimated return coordinates with:

```rust
/// Quad discovery runs in the selected decimated image. Before homography and
/// decode, fitted quads are promoted into the coordinate space of `im_gray`.
/// Returned detections therefore always use source-image pixel coordinates,
/// independently of `factor`.
```

Also correct the stale `stride = width` wording: `im_gray` supports the
explicit supplied `stride`.

- [ ] **Step 5: Run the real-image regression**

Run:

```bash
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution -- --nocapture
```

Expected: both tests pass; the factor-2 fixture still yields seven detections,
but their centers and homographies are in source coordinates.

- [ ] **Step 6: Run the Rust library tests**

Run:

```bash
cargo test --lib --features host --target x86_64-unknown-linux-gnu
```

Expected: all library tests pass. Existing warnings may remain; no new warning
should be introduced.

- [ ] **Step 7: Commit source-image decoding**

```bash
git add src/pipeline.rs tests/verify_pipeline_resolution.rs
git commit -m "pipeline: decode tags at source resolution"
```

### Task 3: Keep debug coordinate spaces internally consistent

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs:733-773`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.rs:2370-2380`
- Test: `/mnt/sda_500gb/git_repo/apriltag-rvv/tests/verify_pipeline_resolution.rs`

- [ ] **Step 1: Add a failing debug-dimension test**

Extend `verify_pipeline_resolution.rs` with a helper that enables each debug
stage and verifies the image dimensions:

```rust
use apriltag_rvv::pipeline::DebugStage;

#[test]
fn debug_stages_keep_their_native_coordinate_spaces() {
    let image = image::open("tests/data/33369213973_9d9bb4cc96_c.jpg")
        .expect("fixture")
        .to_luma8();
    let width = image.width() as usize;
    let height = image.height() as usize;

    for stage in [
        DebugStage::Decimated,
        DebugStage::Threshold,
        DebugStage::Clusters,
        DebugStage::Quads,
    ] {
        let mut bufs = DetectBuffers::new();
        bufs.set_debug_stage(stage);
        let _ = detect(
            image.as_raw(), width, height, width,
            DecimateFactor::Two, &Tag36h11, 25,
            KernelMode::Scalar, None, &mut bufs,
        );
        let (_, debug_w, debug_h, actual_stage, _) = bufs.debug_image().unwrap();
        assert_eq!(actual_stage, stage);
        assert_eq!((debug_w, debug_h), ((width + 1) / 2, (height + 1) / 2));
    }

    let mut bufs = DetectBuffers::new();
    bufs.set_debug_stage(DebugStage::Detections);
    let _ = detect(
        image.as_raw(), width, height, width,
        DecimateFactor::Two, &Tag36h11, 25,
        KernelMode::Scalar, None, &mut bufs,
    );
    let (_, debug_w, debug_h, stage, _) = bufs.debug_image().unwrap();
    assert_eq!(stage, DebugStage::Detections);
    assert_eq!((debug_w, debug_h), (width, height));
}
```

- [ ] **Step 2: Run the debug test and verify stage 5 fails**

Run:

```bash
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution \
  debug_stages_keep_their_native_coordinate_spaces -- --exact
```

Expected: stage 5 reports decimated dimensions before the fix.

- [ ] **Step 3: Render stage 5 from the source grayscale image**

Change `capture_detections` to accept source pixels and stride rather than
reading `self.decimated`:

```rust
fn capture_detections(
    &mut self,
    im: &[u8],
    detections: &[Detection],
    width: usize,
    height: usize,
    stride: usize,
) {
    if self.debug_stage != DebugStage::Detections {
        return;
    }
    self.prepare_debug_rgb(width, height, 0);
    for y in 0..height {
        for x in 0..width {
            let value = im[y * stride + x];
            self.debug_rgb[(y * width + x) * 3..(y * width + x + 1) * 3]
                .fill(value);
        }
    }
    // Keep existing full-resolution detection edge and center drawing.
}
```

Update the call:

```rust
bufs.capture_detections(im_gray, &detections, width, height, stride);
```

Update `DebugDumper::dump_detections` to receive the same original image and
geometry:

```rust
let _ = dbg.dump_detections(
    "detections",
    im_gray,
    width,
    height,
    stride,
    &detections,
);
```

- [ ] **Step 4: Run the debug and full integration tests**

Run:

```bash
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution -- --nocapture
```

Expected: all tests pass; stages 1-4 are decimated and stage 5 is source sized.

- [ ] **Step 5: Commit debug-space consistency**

```bash
git add src/pipeline.rs tests/verify_pipeline_resolution.rs
git commit -m "pipeline: render detections in source image space"
```

### Task 4: Update Rust C ABI and skeleton coordinate semantics

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs:1-47`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs:105-121`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs:320-431`
- Test: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/capi.rs` (`#[cfg(test)]` module)

- [ ] **Step 1: Add a failing skeleton coordinate test**

Add a `#[cfg(test)]` module to `capi.rs` that calls the private skeleton
function on one bright region at all factors:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn skeleton_coordinates_do_not_depend_on_quad_decimation_factor() {
        let width = 20;
        let height = 12;
        let stride = 24;
        let mut image = vec![0u8; stride * height];
        for y in 4..8 {
            for x in 7..13 {
                image[y * stride + x] = 255;
            }
        }

        let mut outputs = Vec::new();
        for factor in 0..=2 {
            let mut out = [apriltag_det_t {
                id: 0,
                margin: 0.0,
                center: [0.0; 2],
                corners: [0.0; 8],
            }];
            assert_eq!(
                run_detect_skeleton(&image, width, height, stride, factor, &mut out),
                1,
            );
            outputs.push((out[0].center, out[0].corners));
        }
        assert_eq!(outputs[0], outputs[1]);
        assert_eq!(outputs[0], outputs[2]);
    }
}
```

- [ ] **Step 2: Run the C-ABI test and verify it fails**

Run:

```bash
cargo test --lib --features capi --target x86_64-unknown-linux-gnu \
  capi::tests::skeleton_coordinates_do_not_depend_on_quad_decimation_factor \
  -- --exact
```

Expected: the outputs differ because skeleton mode divides coordinates by the
factor.

- [ ] **Step 3: Remove skeleton coordinate scaling**

Delete `scale_from_int`. Remove `factor` from `run_detect_skeleton` and update
its caller. Compute source-space values directly:

```rust
let cx = sx as f64 / n as f64;
let cy = sy as f64 / n as f64;
let bw = width as f64 * 0.15;
let bh = height as f64 * 0.15;
```

Update the test call and production dispatch to the new signature without
`factor`.

- [ ] **Step 4: Update C-ABI documentation**

State that `apriltag_det_t.center` and `corners` are always in the input
`width x height` coordinate system. Replace the stale comment above
`pipeline::detect()` with:

```rust
// detect() returns source-image coordinates for every factor, so the C ABI
// forwards them unchanged.
```

Do not change structure layout or exported function signatures.

- [ ] **Step 5: Run C-ABI and pipeline tests**

Run:

```bash
cargo test --lib --features capi --target x86_64-unknown-linux-gnu
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution
```

Expected: all scoped tests pass.

- [ ] **Step 6: Commit the ABI semantic update**

```bash
git add src/capi.rs
git commit -m "capi: return source-image detection coordinates"
```

### Task 5: Update Rust documentation and host demo

**Files:**
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/examples/live_demo.rs:277-293`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/src/pipeline.md:191-214`
- Modify: `/mnt/sda_500gb/git_repo/apriltag-rvv/README.md:274-328`

- [ ] **Step 1: Add a source-coordinate drawing helper test**

Extract the conversion used by `live_demo` into a small local helper:

```rust
fn source_point([x, y]: [f64; 2]) -> Point {
    Point::new(x.round() as i32, y.round() as i32)
}
```

Add an example-local test module:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn source_coordinates_are_not_scaled_by_search_factor() {
        assert_eq!(source_point([123.4, 56.6]), Point::new(123, 57));
    }
}
```

If OpenCV's `Point` does not implement `PartialEq` in this binding, assert its
`x` and `y` fields separately.

- [ ] **Step 2: Remove the obsolete draw multiplier**

Replace:

```rust
Point::new((x * scale) as i32, (y * scale) as i32)
```

with `source_point([x, y])`, and use the same helper for `det.center`. Retain
the selected `factor` in the detector call and status output; it still controls
quad-search decimation.

- [ ] **Step 3: Update pipeline documentation**

Document this boundary in `pipeline.md`:

```text
Stages 0-8 operate in the decimated coordinate system. Before stages 9-10,
each fitted quad is multiplied by the selected decimation factor. Homography,
border sampling, payload sampling, and returned detections use the original
source image and source pixel coordinates.
```

Update README live-demo text to state that the factor controls search cost,
not returned-coordinate scale.

- [ ] **Step 4: Build the host demo and run the fixture at factors 1 and 2**

Run:

```bash
cargo check --features live --example live_demo \
  --target x86_64-unknown-linux-gnu
cargo run --quiet --features host --target x86_64-unknown-linux-gnu \
  --example pipeline_demo -- tests/data/33369213973_9d9bb4cc96_c.jpg 1.0
cargo run --quiet --features host --target x86_64-unknown-linux-gnu \
  --example pipeline_demo -- tests/data/33369213973_9d9bb4cc96_c.jpg 2.0
```

Expected: both fixture runs report source-sized coordinates. Factor 2 may find
fewer quads than factor 1, but matching tags must occupy the same source-image
regions. `debug_output/06-detections.ppm` must be `799x533` for both runs.

- [ ] **Step 5: Commit host consumers and docs**

```bash
git add examples/live_demo.rs src/pipeline.md README.md
git commit -m "docs: describe full-resolution AprilTag decode"
```

### Task 6: Update the K230 C and Rust backend coordinate contract

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag.h:1-24`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_c_adapter.cc:149-183`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.h:9-21`
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag_draw.cc:145-176`
- Modify: `buildroot-overlay/package/apriltag_demo/src/main.cc:34-38`
- Modify: `buildroot-overlay/package/apriltag_demo/src/main.cc:461-472`
- Modify: `buildroot-overlay/package/apriltag_demo/src/main.cc:678-682`
- Test: `buildroot-overlay/package/apriltag_demo/tests/coordinate_contract_test.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`

- [ ] **Step 1: Add a failing host-buildable drawing contract test**

Create `tests/coordinate_contract_test.cc` that draws a source-space detection
onto a same-sized image:

```cpp
#include "apriltag_draw.h"

#include <cassert>

int main()
{
    cv::Mat osd(100, 200, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    apriltag_det_t detection = {};
    detection.id = 7;
    detection.center[0] = 100.0;
    detection.center[1] = 50.0;
    const double corners[8] = {80, 30, 120, 30, 120, 70, 80, 70};
    std::copy(std::begin(corners), std::end(corners), detection.corners);

    draw_detections(osd, {detection}, 200, 100);

    const cv::Vec4b center = osd.at<cv::Vec4b>(50, 100);
    assert(center != cv::Vec4b(0, 0, 0, 0));
    return 0;
}
```

Add a `BUILD_TESTING` target in CMake:

```cmake
include(CTest)
if(BUILD_TESTING)
    add_executable(apriltag_coordinate_contract_test
        tests/coordinate_contract_test.cc
        src/apriltag_draw.cc)
    target_include_directories(apriltag_coordinate_contract_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${usr_root}/include/opencv4)
    target_link_libraries(apriltag_coordinate_contract_test PRIVATE
        opencv_imgproc opencv_core)
    add_test(NAME apriltag_coordinate_contract
             COMMAND apriltag_coordinate_contract_test)
endif()
```

- [ ] **Step 2: Configure the test and verify the new API does not compile**

Use the existing Buildroot toolchain through the package build, or configure a
host build with OpenCV available. The Buildroot path is:

```bash
make apriltag_demo-dirclean
make APRILTAG_DEMO_FORCE_RUST_REBUILD=NO apriltag_demo
```

Expected before changing `draw_detections`: compilation fails because the
four-argument source-coordinate signature does not exist.

- [ ] **Step 3: Change final detection drawing to source coordinates**

Change the declaration and definition to:

```cpp
void draw_detections(cv::Mat& osd,
                     const std::vector<apriltag_det_t>& dets,
                     int sensor_w, int sensor_h);
```

Compute:

```cpp
const double s = (double)view.width / (double)sensor_w;
```

Update the main call:

```cpp
draw_detections(draw_frame, detections, frame_width, frame_height);
```

Leave `draw_decode_candidates` unchanged because those diagnostics remain in
the stage-4 decimated coordinate system.

- [ ] **Step 4: Stop dividing official-C output coordinates**

Keep `factor_scale()` to set `detector->quad_decimate`, but copy C detections
directly:

```cpp
out[i].center[0] = detection->c[0];
out[i].center[1] = detection->c[1];
for (int corner = 0; corner < 4; ++corner) {
    out[i].corners[corner * 2] = detection->p[corner][0];
    out[i].corners[corner * 2 + 1] = detection->p[corner][1];
}
```

- [ ] **Step 5: Separate factor logging from coordinate conversion**

Rename `g_decimate_scale` to `g_factor_value`. Retain it only for startup
logging. Parse only accepted spellings:

```cpp
if (f == "1" || f == "1.0") {
    g_factor_int = 0;
    g_factor_value = 1.0;
} else if (f == "1.5") {
    g_factor_int = 1;
    g_factor_value = 1.5;
} else if (f == "2" || f == "2.0") {
    g_factor_int = 2;
    g_factor_value = 2.0;
} else {
    cerr << "--factor must be 1, 1.5, or 2" << endl;
    exit(2);
}
```

Update `apriltag.h` and drawing comments to say detections use the input image
coordinate system. Clarify that `DecodeCandidate` remains decimated because it
overlays stage 4.

- [ ] **Step 6: Build both backends before refreshing the Rust archive**

Run:

```bash
make apriltag_demo-dirclean
make APRILTAG_DEMO_FORCE_RUST_REBUILD=NO apriltag_demo
```

Expected: both `apriltag_demo.elf` and `apriltag_c_demo.elf` compile against
the same source-coordinate header. The Rust binary still contains the old
archive at this step, so do not run it on target yet.

- [ ] **Step 7: Commit the K230 source-coordinate integration**

```bash
git add \
  buildroot-overlay/package/apriltag_demo/CMakeLists.txt \
  buildroot-overlay/package/apriltag_demo/tests/coordinate_contract_test.cc \
  buildroot-overlay/package/apriltag_demo/src/apriltag.h \
  buildroot-overlay/package/apriltag_demo/src/apriltag_c_adapter.cc \
  buildroot-overlay/package/apriltag_demo/src/apriltag_draw.h \
  buildroot-overlay/package/apriltag_demo/src/apriltag_draw.cc \
  buildroot-overlay/package/apriltag_demo/src/main.cc
git commit -m "apriltag_demo: use source-image detection coordinates"
```

### Task 7: Refresh the K230 Rust library and document the compatibility change

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv.a`
- Modify: `buildroot-overlay/package/apriltag_demo/README.md`
- Modify: `docs/superpowers/specs/2026-07-25-apriltag-live-demo-design.md`

- [ ] **Step 1: Run all scoped Rust verification before producing a binary**

Run in `apriltag-rvv`:

```bash
cargo test --lib --features host --target x86_64-unknown-linux-gnu
cargo test --lib --features capi --target x86_64-unknown-linux-gnu
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution
```

Expected: all pass.

- [ ] **Step 2: Build and copy the RISC-V static library**

Run from the SDK repository:

```bash
APRILTAG_RVV_DIR=/mnt/sda_500gb/git_repo/apriltag-rvv \
  bash buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh
```

Expected: `libapriltag_rvv.a` is rebuilt with RVV enabled and copied to the
package's `lib/` directory.

- [ ] **Step 3: Force a clean Buildroot package rebuild**

Run:

```bash
make apriltag_demo-dirclean
make APRILTAG_DEMO_FORCE_RUST_REBUILD=YES apriltag_demo
```

Expected: both applications build and install. Verify artifacts exist:

```bash
file output/k230_canmv_defconfig/target/root/app/apriltag_demo/apriltag_demo.elf
file output/k230_canmv_defconfig/target/root/app/apriltag_c_demo/apriltag_c_demo.elf
```

Expected: both are 64-bit RISC-V ELF executables.

- [ ] **Step 4: Document the new behavior**

Add a README section stating:

```text
`--factor` controls quad-search decimation only. The Rust and official-C
backends promote fitted quads to the input image coordinate system and perform
homography construction, border sampling, and payload sampling on the original
full-resolution image. Returned coordinates are therefore independent of the
factor.
```

Add a superseding note near the top of the 2026-07-25 historical design:

```text
Superseded coordinate note (2026-07-30): the implemented detector now follows
the official C resolution boundary. Stages through quad fitting remain
decimated; decode and returned detections use source-image coordinates. The
older decimated-output statements below describe the initial implementation.
```

- [ ] **Step 5: Run repository checks**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors; only intended files plus pre-existing
unrelated untracked files appear.

- [ ] **Step 6: Commit generated library and documentation**

```bash
git add \
  buildroot-overlay/package/apriltag_demo/lib/libapriltag_rvv.a \
  buildroot-overlay/package/apriltag_demo/README.md \
  docs/superpowers/specs/2026-07-25-apriltag-live-demo-design.md
git commit -m "apriltag_demo: ship full-resolution Rust decode"
```

### Task 8: On-device C/Rust validation

**Files:**
- No source changes expected
- Runtime evidence: board console output and optional captured raw frame

- [ ] **Step 1: Run matched Rust and C configurations**

On the K230, run the same camera mode with exact codeword matching and no edge
refinement or sharpening:

```bash
/root/app/apriltag_demo/apriltag_demo.elf \
  --rvv --factor 2 --min-blob 25 --csi-size 1280x720 --debug

/root/app/apriltag_c_demo/apriltag_c_demo.elf \
  --factor 2 --min-blob 25 --csi-size 1280x720 \
  --threads 1 --bits-corrected 0 --decode-sharpening 0
```

Expected: both overlays use source coordinates and align with the same tag
regions. Rust diagnostics should show fewer codeword failures than the old
decimated decode for quads that remain detectable.

- [ ] **Step 2: Compare factor 1 and factor 2 geometry**

Run the Rust application at factor 1 and factor 2 on a static camera/tag setup.
Record centers and corners for matching IDs.

Expected: coordinates remain in the same 1280x720 coordinate system. Factor 2
may find fewer quads, but matching detections must not be halved or doubled.

- [ ] **Step 3: Verify debug-stage dimensions and overlays**

Use keys `1` through `5`:

- stages 1-4 must show the decimated pipeline images;
- stage 4 candidate labels must remain aligned with decimated quads;
- stage 5 must show the source-resolution grayscale image and source-space
  detections;
- normal camera view must not multiply source coordinates by factor 2.

- [ ] **Step 4: Record remaining C-parity differences**

If C and Rust still disagree, categorize failures using existing counters:

```text
quads
homography_rejects
polarity_rejects
codeword_rejects
best_hamming
detections
```

Do not fold border sampling, half-pixel interpolation, edge refinement, or
decode sharpening changes into this implementation. Open focused follow-up
work using the measured rejection category.

## Final Verification

Before claiming completion, run fresh verification in both repositories.

In `apriltag-rvv`:

```bash
cargo test --lib --features host --target x86_64-unknown-linux-gnu
cargo test --lib --features capi --target x86_64-unknown-linux-gnu
cargo test --features host --target x86_64-unknown-linux-gnu \
  --test verify_pipeline_resolution
cargo check --features live --example live_demo \
  --target x86_64-unknown-linux-gnu
git status --short
```

In `k230_linux_sdk_metalv`:

```bash
make apriltag_demo-dirclean
make APRILTAG_DEMO_FORCE_RUST_REBUILD=YES apriltag_demo
git diff --check
git status --short
```

Report the pre-existing failure of unscoped plain `cargo test` separately if
it still attempts incompatible feature-gated examples. Do not describe the
entire suite as passing unless `bash scripts/test-all.sh` is also run and exits
successfully.
