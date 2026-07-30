# K230 AprilTag comparison demos

This package builds two applications around the same K230 camera, luma
preprocessing, display, keyboard-control, and FPS-reporting code:

- `apriltag_demo.elf`: the Rust `apriltag-rvv` detector
- `apriltag_c_demo.elf`: the official AprilRobotics C detector, version 3.4.5

It also builds `k230_apriltag_bench`, a fixed-image detector benchmark that
links no camera or display libraries. Its installed JPEG fixture is an unchanged
copy of `apriltag-rvv/tests/data/33369213973_9d9bb4cc96_c.jpg`.

The C detector is linked statically into its application. Its Buildroot source
archive and license are verified by `package/apriltag/apriltag.hash`.

## Build

```sh
make CONF=k230_canmv_defconfig apriltag_demo
```

The applications are installed in the target tree at:

```text
/root/app/apriltag_demo/
/root/app/apriltag_c_demo/
/root/app/apriltag_profile/
/root/app/apriltag_bench/
```

The package containing both is:

```text
output/k230_canmv_defconfig/images/deb/k230-apriltag-demo.deb
```

## Comparable runs

The C application's comparison defaults match the current Rust detector where
the algorithms expose equivalent controls: one detector thread, exact
Tag36h11 codewords (zero corrected bits), edge refinement off, decode
sharpening off, minimum blob size 25, and decimation factor 2.

```sh
cd /root/app/apriltag_demo
./apriltag_demo.elf --rvv --factor 2 --min-blob 25 \
    --csi-size 1280x720 --usb-video 3

cd /root/app/apriltag_c_demo
./apriltag_c_demo.elf --factor 2 --min-blob 25 \
    --csi-size 1280x720 --usb-video 3 --threads 1 \
    --bits-corrected 0 --decode-sharpening 0
```

Both applications start on CSI. Press `u` for the configured USB camera, `c`
for CSI, `n` to cycle the identical luma denoise modes, and `q` to quit.

Use `--upstream-defaults` with the C application to select the upstream
behavior: minimum blob size 5, two corrected bits, edge refinement on, and
decode sharpening 0.25. Use `--threads N` to measure the original detector's
multicore scaling independently of the RVV comparison.

The displayed `detect` rate includes only completed detector calls. Camera and
display rates are reported separately.

## Fixed-image benchmark

Run the default 1280x720 comparison and separate Rust RVV/C `perf stat` jobs:

```sh
cd /root/app/apriltag_bench
./run_benchmark.sh
```

Additional arguments are appended to all three jobs, so later options can
replace the wrapper defaults. For example:

```sh
./run_benchmark.sh --size native --warmup 2 --iterations 10 --batches 3
./run_benchmark.sh --input /root/frame.y8 --format raw --size 640x360
```

Direct benchmark examples include explicit JPEG format regardless of filename
extension and one selected backend:

```sh
./k230_apriltag_bench --input fixture.data --format jpeg --size 1920x1080
./k230_apriltag_bench --input fixture.jpg --backend rust-rvv --size 1280x720
./k230_apriltag_bench --input fixture.jpg --backend c --size 1280x720
```

The internal timing covers each complete public detector call and normal result
lifecycle, but excludes image decode/resize and detector construction. Parse
machine-readable records with `grep '^RESULT '`. If backend checksums differ,
the reported numbers remain production API throughput comparisons but are not
equivalent-output speedups.

The benchmark supports complete result sets of 0 through 4095 detections per
image. It rejects 4096 or more detections because the Rust C ABI cannot report
whether a result count equal to its output capacity was truncated; the same
limit is enforced for the C reference backend to keep comparisons equivalent.

`--factor` controls quad-search decimation only. Both detectors promote fitted
quads to the input image coordinate system and perform homography construction,
border sampling, and payload sampling on the original-resolution image.
Returned centers and corners therefore do not need a decimation multiplier.

## On-device profiling

The package selects target `perf` and GNU binutils, including the native
RISC-V `objdump` required by `perf annotate`. The CanMV defconfig also excludes
the two demo ELFs from final root-filesystem stripping so their local function
symbols remain available to `perf report` and `perf annotate`. Run the
comparison profiler from its installed directory:

```sh
cd /root/app/apriltag_profile
./profile_apriltag.sh
```

By default, each application warms up for three seconds, locates its named
`apriltag-detect` thread, runs `perf stat` on that thread for 15 seconds, and
then records its cycles for 15 seconds. This excludes the DRM/display thread
from detector profiles. Results are written below
`/root/app/apriltag_profile/perf-results`. Durations, output location, and
application arguments can be overridden with environment variables:

```sh
APRILTAG_PROFILE_SECONDS=30 \
APRILTAG_PROFILE_OUTPUT=/root/perf-results \
APRILTAG_COMMON_ARGS="--factor 2 --min-blob 25 --csi-size 1280x720" \
./profile_apriltag.sh
```

Set `APRILTAG_PROFILE_SCOPE=process` when deliberately measuring the complete
camera/detection/display application. Process-wide results include the ARGB
overlay copy and must not attribute that `memcpy` cost to the detector.

Annotate a retained Rust detector stage directly on the board:

```sh
APRILTAG_PERF_SYMBOL=apriltag_rvv::pipeline::ccl_and_boundary_extract \
    ./annotate_detect.sh
./annotate_detect.sh --interactive
```

### Detector stage symbols

The release library always retains coarse detector stages as separate symbols:
CCL/boundary extraction, cluster filtering/sorting, quad fitting, fit-error
calculation, pair-stat precomputation, four-peak search, corner construction,
decoding, and deduplication. This makes on-board `perf` attribution useful
without a special build variant.

This is also the production-performance choice, not merely a profiling aid.
On K230, flattening the stages doubled the hot `detect` function body and was
about 4% slower per detection, with more instructions, branches, and branch
misses. Keeping calls only at coarse stage boundaries improves instruction
locality; small indexing, prefix-query, and covariance helpers remain inline
inside their hot loops.

After changing the sibling `apriltag-rvv` source, force regeneration of the
packaged static library:

```sh
make CONF=k230_canmv_defconfig apriltag_demo-dirclean
make CONF=k230_canmv_defconfig \
    APRILTAG_DEMO_FORCE_RUST_REBUILD=YES \
    apriltag_demo
```

## Interpreting compute and memory costs

IPC by itself does not distinguish a compute-bound loop from a cache miss,
dependency chain, or branch-limited loop. Start with the detector-thread
profile above and use only events listed by the board's `perf list`:

```sh
PID=$(pidof apriltag_demo.elf)
for T in /proc/$PID/task/*; do
    printf '%s ' "${T##*/}"
    cat "$T/comm"
done
TID=$(for T in /proc/$PID/task/*; do
    [ "$(cat "$T/comm")" = apriltag-detect ] && echo "${T##*/}"
done)
perf stat -t "$TID" --timeout 15000 \
    -e cycles,instructions,branches,branch-misses,cache-references,cache-misses
```

High cache-miss rates plus weak improvement when the CPU clock is raised point
to a data/memory bottleneck. Nearly proportional speedup with CPU frequency,
low miss rates, and a stable instruction count point to compute, dependency,
or front-end cost. The most reliable cross-check is a detector-only fixed-image
benchmark: it removes camera waits and HDMI traffic, and permits resolution and
clock sweeps on identical input.

For an unexpected `memcpy`, record a call graph for the relevant named thread:

```sh
perf record -g -t "$TID" -e cycles:u -- sleep 15
perf report --stdio --children -g caller
```

## Frame flow and hardware-offload priorities

The CSI display path is already zero-copy: V4L2 queues DRM DMA-bufs and the
display controller scans them out. The detection path also avoids an input
copy by reading the NV12 Y plane from its mmap buffer, but it currently holds
that dequeued camera buffer until the complete detector call returns. With
three buffers and a detector slower than the camera, this can fill the capture
queue and back-pressure an ISP output.

The preferred next restructuring for factor-2 detection is:

1. dequeue the CSI buffer;
2. run Stage 0 directly from its Y plane into the detector-owned decimated
   buffer;
3. immediately requeue the CSI buffer;
4. run thresholding through decoding from the owned buffer.

This introduces no new image copy—the same decimation already occurs inside
`detect()`—but shortens camera-buffer ownership from one detection to one
decimation. K230 VICAP/DW can alternatively emit a resized detection channel,
and AI2D supports YUV400 resize, but either choice must preserve the exact
sampling used for the C/Rust comparison.

SDMA is useful for an asynchronous full-resolution or strided copy, especially
at factor 1, but it does not remove DDR traffic and adds queue/synchronisation
overhead. The MPP `libdma` userspace API described in the general K230 SDK is
not packaged by this MetalV Linux tree; its current GDMA support is an internal
DRM rotation helper, so using DMA here first requires a suitable userspace
interface. NONAI2D provides CSC/OSD/border operations rather than the grayscale
decimation needed here. For HDMI overlays, a double-buffered DRM OSD plane is
a better eventual zero-copy design.

The current application already removes the largest avoidable overlay traffic:
it copies ARGB only when detection produces a new overlay, and landscape HDMI
uses one direct copy instead of clear + `copyTo` + `memcpy`.

## FPS counters

The four printed rates count different events:

- `poll`: callbacks from the combined V4L2/DRM poll loop;
- `display`: DRM display events, often close to the selected HDMI refresh;
- `camera`: frames dequeued from the display V4L2 channel;
- `detect`: completed detector calls on the separate capture channel.

Therefore `poll` and `display` near 45 do not mean 45 unique camera frames.
The OV5647 driver mode is nominally 1920x1080 at 30 fps. To distinguish sensor
configuration from capture back-pressure, query both nodes while the demo is
stopped and stream them separately:

```sh
v4l2-ctl -d /dev/video5 --get-fmt-video --get-parm
v4l2-ctl -d /dev/video6 --get-fmt-video --get-parm
v4l2-ctl -d /dev/video5 --stream-mmap=4 --stream-count=300 \
    --stream-to=/dev/null
v4l2-ctl -d /dev/video6 --stream-mmap=4 --stream-count=300 \
    --stream-to=/dev/null
```

If camera FPS rises when detection is made faster or disabled, the held
detection buffer/shared ISP path is the limiting factor. If a single-node
stream remains near 23 fps, inspect sensor/ISP timing and exposure next.
