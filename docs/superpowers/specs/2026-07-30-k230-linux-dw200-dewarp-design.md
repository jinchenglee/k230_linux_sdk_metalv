# K230 Linux DW200 dewarp accelerator design

**Date:** 2026-07-30
**Status:** Approved design, pending implementation planning

## 1. Goal

Add Linux-only support for the K230 DW200 image accelerator. One source frame
must produce two synchronized, lens-corrected outputs in one hardware job:

1. a full-resolution DWE output for AprilTag edge refinement and tag-bit
   sampling; and
2. a fixed factor-2 VSE output for thresholding, clustering, and quad fitting.

RT-Smart must not run on the target and no runtime component may depend on it.
The Linux driver owns the hardware resources and uses Linux DMA, interrupt,
reset, clock, and media frameworks.

The accelerator should remain generic enough for non-AprilTag applications,
but the first supported configuration is deliberately narrow: 8-bit NV12,
one full-resolution output, one factor-2 output, and one job in flight.

## 2. Non-goals

- Linking RT-Smart objects or MPP libraries into Linux.
- Reproducing the RT-Smart MPP, VICAP, VB, or virtual-device APIs.
- VSE outputs 1 and 2 in the first milestone.
- Arbitrary scaling, YUV400, RGB, RAW, 10-bit formats, split-screen mode, or
  format conversion in the first milestone.
- Hardware front-end command-buffer scheduling in the first milestone.
- Immediate transparent insertion into the proprietary VVCAM media graph.
- Treating reconstructed behavior as authoritative hardware documentation.

## 3. Source evidence and constraints

The Linux+RT-Smart SDK provides useful public source but keeps the actual
DW200 driver implementation in prebuilt objects:

- `k230_sdk/src/big/mpp/kernel/lib/libvicap.a(dw200_mod.o)` contains the
  RT-Smart hardware driver.
- `k230_sdk/src/big/mpp/userapps/lib/libvicap.a(mpi_dw200.o)` contains the
  userspace wrapper.
- `k230_sdk/src/big/mpp/include/comm/k_dewarp_comm.h` exposes DWE and VSE
  parameter structures.
- `k230_sdk/src/big/mpp/userapps/sample/sample_dw200/sample_dw200.c` exposes
  parameter derivation and the standalone submission sequence.
- `k230_sdk/src/big/mpp/userapps/src/sensor/dewarp/k230dwmapgen/` provides
  source for map generation.

The RT-Smart objects cannot be linked into Linux. They depend on RT-Thread,
MMZ, MPP VB, VICAP, and private ioctl interfaces. The Linux implementation
will be a new driver informed by public source, observable behavior, and
hardware validation.

Visible licensing permits reuse of the map generator and several public
headers when their notices are retained. Binary redistribution and any work
derived from disassembly require separate review and are outside this design.
The implementation should be independently written rather than mechanically
translating the RT-Smart object code.

## 4. Hardware model

DW200 contains two relevant blocks:

- **DWE, Dewarp Engine:** applies the coordinate-map-based geometric
  transformation and writes a corrected full-resolution frame.
- **VSE, Video Scaling Engine:** receives DWE output internally, crops or
  resizes it, and writes up to three additional outputs.

The approved first data path is:

```text
VVCAM full-resolution NV12 DMA-BUF
                  |
                  v
          DWE lens correction
             /          \
            /            \ internal DW200 stream
           v              v
full-resolution       VSE factor-2 resize
corrected NV12        corrected half-size NV12
           \              /
            \            /
             AprilTag detector
```

The vendor sample programs DWE and VSE, provides both sets of destination
buffers, and starts DWE once. VSE `input_select == 4` requires no VSE input
DMA address, while `input_select == 5` explicitly uses a memory input. The
first value is therefore the candidate internal DWE-to-VSE path and must be
validated on hardware.

Known platform information includes:

- DW200 register range: `0x90008000`, size `0x1000`.
- Dedicated ISP-DW reset: RMU offset `0x80`, assert bit 5, done bit 28.
- DWE clock gate: ISP clock register bit 15.
- VSE clock gate: ISP clock register bit 16.
- Frame and map addresses use 16-byte units in the observed DWE path.
- The K230 interconnect is DMA-noncoherent.

The RT-Smart driver uses DWE, VSE, and command-front-end interrupt constants
130, 204, and 131. Their exact Linux PLIC representation and trigger behavior
must be verified rather than copied without testing.

## 5. Linux architecture

### 5.1 Resource ownership

A dedicated K230 DW200 platform driver owns:

- the `0x90008000..0x90008fff` register range;
- the ISP-DW reset through the Linux reset framework;
- DWE and VSE clocks through the common clock framework;
- confirmed DWE and VSE interrupts;
- the serialized hardware job state; and
- all DMA mappings used by a job.

The existing ISP resource currently overlaps the DW200 range. The ISP
resource must be corrected to end at `0x90007fff`, including VVCAM's hard-coded
resource size, before the DW200 driver claims its range. This split must be
validated against actual ISP accesses.

Linux currently lacks common-clock-framework entries for the DWE and VSE
gates. Those entries and device-tree references are prerequisites for the
production driver. Direct CMU writes are allowed only in a disposable
bring-up experiment, not in the final design.

### 5.2 Driver layering

The implementation is divided into three bounded components:

1. **DW200 hardware core:** register definitions, reset/start/stop, DWE map and
   frame configuration, VSE channel-0 configuration, IRQ acknowledgement,
   timeout recovery, and one-job serialization.
2. **Linux media frontend:** queueing, DMA-BUF import/export, format validation,
   paired output completion, sequence numbers, and timestamps.
3. **Map tool:** userspace generation and validation of the Q12.4 coordinate
   map. Floating-point calibration and map generation stay out of the kernel.

The hardware core must not depend on VVCAM internals. This keeps static-image
testing possible with the ISP stack unloaded.

### 5.3 Userspace interface

The final interface uses coordinated V4L2 nodes backed by one hardware job:

```text
/dev/video-dw200-input   full-resolution NV12 input
/dev/video-dw200-full    full-resolution corrected DWE output
/dev/video-dw200-half    factor-2 corrected VSE output
```

The exact registration may consolidate input control into one of the nodes,
but the externally visible semantics are fixed:

- one input buffer and both output buffers are required before a job starts;
- the two output buffers are indivisible products of the same job;
- both outputs carry the same source sequence and timestamp;
- a job is complete only after both DWE and enabled VSE completion are seen;
- an error or timeout marks both outputs failed; and
- closing or stopping either output aborts the paired stream safely.

This coordinated multi-node model is preferred over treating two complete
images as planes of one V4L2 format. If V4L2 framework constraints make the
coordinated interface unreliable, a request-based accelerator interface may
be used, but the paired-job semantics must remain unchanged.

### 5.4 DMA and buffer rules

Initial queues use VB2 DMA-contiguous memory and support MMAP and DMA-BUF.
DMA-BUF permits a VVCAM capture buffer to become DW200 input without a CPU
copy and permits either output to feed display, detection, encoding, or
another accelerator.

The driver must use Linux DMA APIs for all mappings and synchronization. It
must not accept userspace physical addresses or copy RT-Smart cache-flush
patterns. Initial constraints are:

- 32-bit DMA addresses unless 34-bit encoding is proven;
- 16-byte base-address and stride alignment;
- active full dimensions and strides validated against register field sizes;
- half dimensions fixed by the verified factor-2 rule;
- contiguous Y and UV layout matching the recovered NV12 programming model;
- no buffer reuse before the corresponding output completion; and
- no CPU access without the required DMA-BUF synchronization.

## 6. Job lifecycle

For each frame, the driver performs the following sequence:

1. Wait for one input, one full output, one half output, and a valid map.
2. Map all buffers for the DW200 DMA device and validate addressability.
3. Enable clocks and establish required NoC state through an owned Linux
   mechanism.
4. Reset or verify the idle DWE/VSE state.
5. Clear stale status and program the correction map.
6. Program DWE source geometry and the full-resolution destination.
7. Program VSE channel 0 for factor-2 NV12 output and internal input selector
   4; leave channels 1 and 2 disabled.
8. Arm VSE before starting DWE.
9. Start the DWE source DMA and engine.
10. Record DWE and VSE completion independently in the interrupt handlers.
11. Complete both outputs only when both required status events have arrived.
12. On error or timeout, stop the bus, reset the dedicated DW block, return all
    three buffers with errors, and leave the next job able to proceed.

One hardware job may be active at a time. Multiple userspace contexts may
queue work, but the media frontend serializes them fairly.

## 7. Map handling

The map remains a userspace-generated artifact. Normal maps use a 16x16 image
grid:

```text
map_width  = ceil(image_width / 16) + 1
map_height = ceil(image_height / 16) + 1
```

Each 32-bit little-endian map entry stores source coordinates as Q12.4:

```text
bits 15:0  = source X
bits 31:16 = source Y
```

The SDK file format has an eight-byte split-settings header followed by map
entries. The Linux userspace tool parses that file, validates dimensions and
size, and submits only the validated map payload through the chosen V4L2
control or DMA-BUF interface. The kernel independently validates payload size,
alignment, and map dimensions before programming DMA.

An identity map generated independently from the public sample is the first
bring-up vector.

## 8. AprilTag integration

### 8.1 Correct two-resolution algorithm

The full-resolution corrected DWE image and half-resolution corrected VSE
image from one frame have separate roles.

The half image is used for:

1. adaptive thresholding;
2. connected components and boundary clustering;
3. line fitting; and
4. quad construction.

Quad corners are then transformed into full-resolution corrected-image space.
The full image is used for:

1. optional edge refinement;
2. homography construction from full-resolution corners;
3. white and black border-model sampling;
4. payload-bit sampling;
5. optional decode sharpening; and
6. codeword comparison and final detection.

This follows the official C detector architecture. The current Rust pipeline
incorrectly performs border and payload sampling on its internally decimated
image and must be split before it can consume the paired hardware outputs.

### 8.2 Detector API

The detector receives two explicit grayscale views plus their coordinate
transform:

```c
int apriltag_detect_dual(
    void *detector,
    const apriltag_gray_view_t *quad_image,
    const apriltag_gray_view_t *decode_image,
    const apriltag_affine2d_t *quad_to_decode,
    apriltag_det_t *out,
    int max_out);
```

The initial transform is expected to be:

```text
x_full = 2 * x_half
y_full = 2 * y_half
```

It remains explicit because VSE filter phase, crop, and pixel-center behavior
are not yet documented. All returned centers and corners are in the
full-resolution corrected-image coordinate space. The current adapter's
divide-then-display-multiply convention is removed from the dual-image path.

Both views include active width, height, stride, and frame sequence. The API
rejects mismatched sequences. Both buffers remain valid until the synchronous
call returns. A later split-phase API may release the half image after quad
fitting, but that is not required initially.

### 8.3 C-reference parity

Using the full-resolution image for decoding fixes the major Rust/C mismatch,
but complete parity additionally requires review of:

- C integer border sampling versus Rust bilinear border sampling;
- C payload interpolation's half-pixel convention;
- out-of-bounds sample handling;
- optional full-resolution edge refinement;
- decode sharpening; and
- codeword correction policy.

These detector corrections are a separate implementation workstream from the
DW200 driver but are required before claiming C-reference equivalence.

## 9. Sampling validation

The C factor-2 quad image uses phase-zero point sampling:

```text
half[y][x] = full[2*y][2*x]
```

VSE filtering, phase, rounding, and border behavior are undocumented. Before
using the VSE image as a drop-in C quad image, tests must compare it against
the full DWE output using coordinate ramps, impulses, checkerboards, and odd
dimensions.

Possible outcomes are:

1. **Exact point sampling:** use the transform `full = 2 * half` and compare
   quad results directly with C.
2. **Stable shifted phase:** record the measured affine offset and use it in
   `quad_to_decode`; quad candidates may still differ from C.
3. **Filtered scaling:** retain VSE for performance if detection quality is
   acceptable, but do not claim bit-exact C quad preprocessing.
4. **Unusable behavior:** use the full DWE output and software phase-zero
   decimation while retaining full-resolution decode correctness.

The fourth outcome is a fallback, not part of the preferred steady-state data
path.

## 10. Performance model

For a 1280x720 NV12 input:

```text
DWE input read                 1.382 MB/frame
DWE full output write          1.382 MB/frame
VSE half output write          0.346 MB/frame
approximate DW200 traffic      3.110 MB/frame
approximate traffic at 30 fps 93.3 MB/s
```

Map reads and ISP, display, detector, and interconnect traffic are additional.
Compared with a separate memory-fed resize pass, internal DWE-to-VSE streaming
avoids rereading the full corrected frame, approximately 41.5 MB/s at 30 fps
for this resolution. It also removes a second submission, scheduler boundary,
and completion wait.

The full DWE write is intentional because full-resolution tag sampling needs
that image. There is no requirement to suppress it.

Steady-state pipelining may overlap camera capture, DW200 processing, and CPU
detection on different frames. Queue depth must remain bounded so throughput
pressure causes explicit frame dropping rather than unbounded latency.

## 11. Error handling

The driver treats the input and two outputs as one transaction.

- Invalid dimensions, stride, format, map, or DMA address fail before start.
- DWE or VSE error status fails both outputs.
- A bounded watchdog handles lost interrupts and hardware stalls.
- Recovery disables the bus, clears status, resets only ISP-DW, and restores
  an idle state before another job.
- Repeated recovery failure disables streaming and reports a persistent error
  rather than looping resets.
- Stream-off and process death cancel queued work and safely terminate active
  work.
- Driver logs include status registers and the job sequence but not frame
  contents or unbounded register dumps.

The reset provider's hardware-auto-done path currently has no timeout, so its
clock ordering and failure behavior must be corrected or guarded before the
driver relies on it for recovery.

## 12. Staged delivery

### Phase 0: establish hardware facts

- Confirm MMIO ownership and reduce the ISP aperture without regression.
- Confirm DWE and VSE Linux IRQ identities and trigger types.
- Add DWE/VSE clocks to the common clock framework.
- Confirm required shared ISP, AXI, DDR, and NoC configuration.
- Confirm safe reset and status-clear ordering.

### Phase 1: static DWE bring-up

- Probe resources without VVCAM.
- Run one identity-map NV12 memory-to-memory correction.
- Start with polling if needed, then verify DWE interrupt completion.
- Compare output bytes against input, excluding documented padding.

### Phase 2: fused paired outputs

- Configure internal VSE input selector 4.
- Produce full and half NV12 outputs from one start.
- Track DWE and VSE completion independently.
- Validate frame pairing and factor-2 sampling semantics.

### Phase 3: V4L2 and DMA-BUF

- Add coordinated V4L2 queues/nodes.
- Pass V4L2 compliance for supported operations.
- Import VVCAM DMA-BUF input without CPU copy.
- Export both outputs and exercise cancellation, timeout, and concurrent
  userspace contexts.

### Phase 4: detector integration

- Split Rust quad discovery from full-resolution decode.
- Feed half VSE luma to quad discovery and full DWE luma to decode.
- Return full-resolution corrected coordinates.
- Compare detections and sampled codewords with official C.

### Phase 5: live pipeline optimization

- Pair outputs by sequence and timestamp in the live application.
- Bound queue depth and select an explicit frame-drop policy.
- Measure camera-to-detection latency, throughput, DDR bandwidth, CPU usage,
  and detector quality.
- Consider inline VVCAM graph integration only if M2M scheduling overhead is
  material after measurement.

## 13. Verification

Completion requires evidence from all of the following categories:

- **Resource tests:** exclusive MMIO ownership, expected clocks, reset
  behavior, and incrementing DWE/VSE interrupts.
- **Static vectors:** identity, lens correction, perspective, border cases,
  alignment cases, and deterministic hashes compared with RT-Smart where
  possible.
- **Paired output tests:** identical sequence/timestamp and no cross-frame
  pairing under load or cancellation.
- **Sampling tests:** measured VSE filter and phase relative to DWE output.
- **DMA tests:** MMAP, imported VVCAM DMA-BUF, exported output DMA-BUF, and
  repeated CPU/device ownership transitions on the noncoherent SoC.
- **V4L2 tests:** format negotiation, queueing, polling, stream on/off,
  cancellation, process death, and `v4l2-compliance` for applicable nodes.
- **Recovery tests:** malformed map, undersized buffer, forced timeout, reset,
  interrupt storm prevention, and continued operation after recoverable
  failure.
- **Stress tests:** thousands of jobs under camera, display, and detector load
  with bounded latency and no stale frame pairing.
- **Detector tests:** full-resolution bit samples and detection results against
  official C, with remaining algorithm differences reported explicitly.

## 14. Success criteria

The first production milestone succeeds when:

1. Linux runs alone and exclusively controls DW200.
2. One NV12 DMA-BUF input produces synchronized full-resolution DWE and
   factor-2 VSE NV12 outputs from one hardware start.
3. No CPU frame copy or software geometric correction occurs in that path.
4. Both outputs are correctly synchronized and recover together on errors.
5. AprilTag quad discovery uses the half output while final border and payload
   sampling use the paired full output.
6. Detection coordinates are reported in full-resolution corrected-image
   space.
7. Measured throughput sustains the selected live-camera rate with bounded
   latency.
8. Any VSE sampling difference from the C factor-2 reference is measured and
   documented rather than assumed.
