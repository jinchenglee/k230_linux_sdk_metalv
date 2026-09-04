# K230 dual-OS execution plan

## Purpose

This document defines the next development phase for using both K230 CPU cores
without discarding the work already completed on the `dev` branch.  The primary
target is:

- Linux on the 800 MHz small core;
- a small bare-metal or Embassy-style RVV service on the 1.6 GHz big core;
- Linux ownership of camera, ISP, display, KPU/AI2D, storage, and networking;
- descriptor-based IPC over RPMsg-Lite plus shared-memory payload buffers; and
- application-specific offload from `apriltag_demo` and `tinytag_detect` to the
  big core.

The current big-core Linux image remains a supported, monolithic configuration.
Both applications should build and run when Linux is placed on either core.  A
dual-Linux configuration may be investigated later, but SMP Linux and
whole-frame parallelism between unequal cores are not goals.

This plan complements two existing documents:

- `docs/amp_bigcore_rvv_plan.md` records earlier investigation and useful
  low-level evidence.  Treat branch-specific details in it as historical.
- `docs/k230_amp_payload_slots.md` describes the longer-term configurable
  payload-slot model in which either core may run Linux, bare-metal/Embassy
  firmware, or nothing.

The reproducible scalar nncase experiment is documented in
`docs/notes/rvv-free-nncase-v2.11.0.md`.

## Decisions and rationale

### Primary topology

Use small-core Linux plus big-core bare-metal/Embassy firmware as the primary
dual-OS topology.

The existing `opt_linux_on_small_core_cherry-picked` branch already proves that
Linux can boot on the small core.  Therefore this topology does not require a
new Linux port.  Its additional platform work--second-core release, memory
partitioning, mailbox interrupts, cache ownership, and shared-memory IPC--is
largely common to either direction of AMP.

The application benefit favors reserving the big core for RVV work.  AprilTag
quad extraction and decoding, and TinyTag ROI decoding, are CPU-heavy and have
working RVV implementations.  Camera, display, control, and KPU orchestration
fit naturally under Linux.  Keeping Linux on the big core while putting a
scalar service on the small core would consume similar AMP effort but leave the
most useful CPU acceleration unavailable.

### Explicit non-goals

- Do not build an SMP kernel for the two asymmetric cores.  It does not match
  the application workload or the heterogeneous ISA/performance characteristics.
- Do not process alternating complete frames independently on each core.  The
  small core is approximately half the clock rate of the big core, so completion
  order and tail latency would be poor even if average throughput rose.
- RT-Smart is not a runtime target. Its official implementation may be read as
  a hardware reference for reset, UART, mailbox, and cache handling, but no
  application or transport code should depend on RT-Smart APIs.
- Do not pass image payloads through RPMsg buffers.  RPMsg carries control and
  descriptors; a separate shared-memory pool carries frames and ROIs.
- Do not redesign camera timing yet.  Keep the 720p camera path at 60 fps for
  initial bring-up and retain the negotiated/capped display path at no more than
  30 fps.

### Why nncase is no longer a hard blocker

The open nncase v2.11.0 runtime has been rebuilt for `rv64imafdc`, audited for
RVV instructions, and linked with the closed K230 runtime/backend archives.  A
hybrid TinyTag binary successfully loaded the model and ran AI2D and KPU on the
current board.  The absent symbols are nine internal RVV helper kernels that
the closed K230 archives do not reference in this workload.

This makes a small-core Linux KPU experiment reasonable.  It is not yet proof
that every model or all K230 devices work from the small core.  The final
small-core executable and every linked library must be audited, then tested on
hardware.  Keep the scalar runtime and closed K230 components at exactly the
compatible v2.11 generation unless a deliberate upgrade is performed.

The official SDK is not evidence that nncase already runs on small-core Linux.
Its AI examples run under the big-core RT-Smart environment and link the native
nncase runtime, K230 module, functional library, and `librvv.a`.  The official
build downloads a prebuilt K230 RTOS nncase package rather than rebuilding a
small-core scalar runtime.

## Source and branch strategy

1. Commit the current `dev` work, including the TinyTag decoder settings and the
   scalar-nncase build note.
2. Create the dual-OS development branch from the resulting `dev` commit.
3. Inspect `opt_linux_on_small_core_cherry-picked` commit by commit and
   forward-port the minimum small-core Linux boot, toolchain, and configuration
   changes.  Do not move current development onto that older branch.
4. Resolve each ported change against current camera, display, memory, nncase,
   and application code.  Prefer a new AMP defconfig over conditionals that
   silently change the established big-core Linux image.
5. Preserve a buildable big-core Linux configuration throughout bring-up.

This is effectively a rebase in the opposite direction: current `dev` remains
the base of truth and the proven small-core support is replayed onto it.  It
avoids redoing or losing the display refresh, OSD scaling, 720p capture,
buffer-lifetime, TinyTag, and nncase work.

Recommended configuration names should state both roles, for example:

- `k230_linux_big_monolithic_defconfig`;
- `k230_linux_small_amp_defconfig`; and
- a matching big-core firmware configuration selected by the AMP image build.

Exact names may follow the repository's existing convention.

## Official SDK reference points

Use `/mnt/sda_500gb/git_repo/k230_sdk` as the primary hardware reference.  Its
supported dual-OS arrangement is Linux on the small core and RT-Smart on the
big core, which matches the intended core direction even though the big-core
payload will be smaller.

The official `k230_canmv_defconfig` partitions the low 256 MiB as follows:

| Region | Base | Size |
| --- | ---: | ---: |
| IPCM | `0x00100000` | `0x00100000` |
| RT-Smart system | `0x00200000` | `0x07e00000` |
| Linux system | `0x08000000` | `0x08000000` |

It enables both OSes and sets `CONFIG_LINUX_RUN_CORE_ID=0`. SoC labels such as
CPU0/CPU1 must not be assumed to equal architectural hart IDs. Hardware
bring-up on 2026-09-04 confirmed that both independent CPU subsystems report
`mhartid == 0`: the RVV big-core payload read it directly in M-mode while
small-core OpenSBI simultaneously reported boot hart 0 with a scalar ISA. A
single OpenSBI instance therefore cannot address them as globally distinct
harts. Use independent firmware instances and SoC CPU0/CPU1 reset controls for
cross-core launch.

The official IPCM layout contains:

| Purpose | Base | Size |
| --- | ---: | ---: |
| Core 1 to core 0 ring | `0x00100000` | `0x00080000` |
| Core 0 to core 1 ring | `0x00180000` | `0x00079000` |
| Virtual TTY | `0x001f9000` | `0x00004000` |
| IPC descriptors | `0x001fd000` | remaining IPC region |

Relevant implementation evidence includes:

- Linux: `src/common/cdk/kernel/ipcm/arch/k230/platform_riscv_linux.c`;
- RT-Smart: the corresponding `platform_riscv_rtsmart.c`;
- mailbox controller base `0x91104000`; and
- mailbox interrupt 109 in the official implementation.

Linux maps IPC memory with `ioremap_wc`; RT-Smart uses a write-through mapping.
The Linux `__barrier__()` implementation in the reference appears empty while
the RT side calls `dmb()`.  This is a warning to define and verify our own
ordering and cache-maintenance contract, not a pattern to copy without proof.

U-Boot already provides the useful reset-vector and big-core release machinery,
including `k230_boot_baremetal` and `de_reset_big_core`.  Reuse these mechanisms
for the first implementation.

## Boot architecture

### Initial model: independent images launched by U-Boot

Start with U-Boot loading Linux and a separate big-core firmware image into
non-overlapping memory.  Program the big-core reset vector, release it from
reset, and let each payload initialize its own local state.  This minimizes
changes to OpenSBI while hardware ownership and hart identity are still being
verified.

Required boot evidence:

- exact `mhartid`, ISA string, and clock for each core;
- which firmware or boot stage initializes clocks, caches, and interrupt routing;
- Linux's hart mask and the deliberate reason the big core is not brought
  online by Linux;
- big-core entry address and reset-vector constraints;
- PMP regions visible to each payload; and
- independent serial output or another unambiguous heartbeat channel.

The Linux boot log should show an intentional single-hart configuration, not a
second hart that merely failed to come online.

### Later model: OpenSBI domains

OpenSBI domains can be considered after the U-Boot launcher is stable if they
provide useful hart assignment and PMP isolation.  They are not a phase-one
dependency.  Any transition must preserve the same memory and IPC ABI so that
application work is not discarded.

## Big-core runtime bring-up

Bring up the smallest possible firmware first:

1. assembly/C reset entry and UART or shared-counter heartbeat;
2. linker script, stack, `.bss` initialization, trap vector, timer, and mailbox
   interrupt acknowledgement;
3. RVV enablement, including a valid vector-state setting and a known vector
   computation checked by Linux;
4. cache clean/invalidate primitives and memory barriers;
5. RPMsg-Lite transport; and
6. a minimal Rust `no_std`/Embassy-style service loop if Rust is retained as the
   long-term runtime.

The first heartbeat should not depend on an allocator, scheduler, RPMsg, or
camera stack.  Once reset, vector, interrupt, and cache behavior are proven, the
service may use heapless queues or a deliberately bounded allocator. RT-Smart
sources may be consulted for low-level register behavior only; the payload
targets freestanding bare metal and a later Rust `no_std`/Embassy runtime.

## Memory map and isolation

The official 1 MiB IPCM region is a structural example, not a size requirement.
Define an explicit AMP memory map containing:

- big-core firmware text, read-only data, data, BSS, stack, and optional heap;
- RPMsg vrings and transport buffers;
- shared protocol descriptors and counters; and
- one or more large payload pools for frames or ROI images.

Represent every non-Linux region under Device Tree `reserved-memory`, normally
with `no-map`, so the page allocator, CMA, MMZ, display, camera, and KPU buffers
cannot overlap it.  Audit the current board's physical map before selecting
addresses; old AMP placeholders may conflict with current multimedia
reservations.

Use a static carveout and fixed-size slots first.  dma-buf export/import and true
zero-copy sharing can follow after correctness and cache behavior are measured.
An initial copy into a known shared slot is preferable to debugging implicit
ownership of ISP buffers.

## IPC design

### Control plane

Use RPMsg-Lite on the big-core firmware.  The checked official SDK does not
vendor RPMsg-Lite, so import and pin a known version rather than assuming it is
already present.  Use the K230 mailbox to kick virtqueues.

On Linux, first evaluate interoperability with the standard virtio-rpmsg and
`rpmsg_char` stack.  The official tree contains generic RPMsg sources and
headers, but its K230 configuration does not enable `RPMSG_VIRTIO`.  If the
standard path cannot match the static RPMsg-Lite resource/vring arrangement,
implement the smallest K230-specific remoteproc/RPMsg endpoint or transport
driver necessary.  Do not expose the official IPCM ABI directly to application
code.

Control messages should include fixed-width fields such as:

- protocol magic, version, message type, and total size;
- boot generation and service capability bits;
- request sequence and source frame sequence;
- capture, submission, start, and completion timestamps;
- shared slot or buffer ID and physical offset;
- payload length, width, height, stride, and pixel format;
- ROI coordinates and detector parameters;
- completion status and result count; and
- optional CRC for bring-up and fault diagnosis.

Prefer offsets from an agreed carveout base to process-specific pointers.  State
endianness, alignment, packing, cache-line size, and structure sizes in the ABI.
Keep one shared protocol header or generated definition and enforce layouts with
compile-time assertions on both sides.

### Data plane and ownership

Large image data lives in fixed shared-memory slots.  A simple ownership cycle
is:

```text
FREE -> LINUX_READY -> BIG_BUSY -> BIG_DONE -> FREE
```

Only one side may write a slot in each state.  State publication occurs after
payload cache clean and a release barrier; consumption occurs after an acquire
barrier and payload invalidation.  Descriptors, producer/consumer counters, and
frequently updated state must be cache-line aligned so the two cores do not
write the same cache line.

Assume the cores are non-coherent until a hardware test proves otherwise:

- producer writes payload, cleans it, executes a release barrier, publishes the
  descriptor, then kicks the peer;
- consumer observes the descriptor with acquire ordering, invalidates payload
  lines, then reads;
- consumer writes results, cleans them, publishes completion with release
  ordering, and kicks the peer; and
- Linux invalidates the result before reading it.

RPMsg ordering alone does not make external payload buffers coherent.

### Queue policy and recovery

Use bounded queues and a latest-useful-result policy.  Every request/result is
tagged with frame sequence and timestamps.  Linux rejects stale completions and
does not let detector work hold an ISP capture buffer indefinitely.

For live vision, a single pending job plus one active job is a useful starting
point: a newer pending request replaces an older pending request.  This limits
latency while allowing capture and display to continue.  Expose queue depth,
drops/replacements, execution time, transport time, and end-to-end age.

Include a boot-generation value in every endpoint handshake.  If either side
restarts, invalidate all old descriptors, return slots to a known state, and
reject completions from the previous generation.  Linux must have timeouts and
a local/no-detection fallback rather than blocking the camera loop forever.

## Device ownership

Initial ownership should be deliberately conservative:

| Resource | Owner | Notes |
| --- | --- | --- |
| Camera sensor, CSI, ISP | Linux | Preserve current 720p/60 capture path. |
| KPU and AI2D | Linux | Required for TinyTag proposal inference initially. |
| Display/DRM/OSD | Linux | Preserve negotiated HDMI cap and camera-to-display mapping. |
| Storage, network, shell | Linux | Avoid duplicating general-purpose services. |
| Big-core local timer/trap | Big firmware | CPU-local runtime facilities. |
| Mailbox endpoint | Split by direction | Interrupt and registers documented explicitly. |
| Shared carveout | Shared by protocol | Exclusive slot ownership at any instant. |
| GSDMA/nonai2d | Linux initially | Current `dev` assigns these to Linux; do not seize them. |

Device ownership belongs to the payload-slot configuration, not to a global
assumption.  A later static DMA-channel split is possible only after register,
interrupt, reset, and cache interactions are understood.

## Application partitioning

### `apriltag_demo`

Small-core Linux performs capture, display/OSD, user control, and scheduling.
The big core runs the full decimated AprilTag detector for one selected frame.
Linux sends a descriptor for a copied/shared grayscale frame and detector
parameters; the big core returns tag IDs, corners, centers, hamming counts, and
decision margins.

Do not assign alternate frames to independent detectors on both cores.  The
desired service is a single low-latency RVV detector with bounded latest-wins
input.  Keep a local detector backend behind the same interface for the
big-core Linux monolithic image, testing, and graceful fallback.

AprilTag is the first application offload because it isolates CPU work from KPU
questions and exercises the complete frame-descriptor/result path.

### `tinytag_detect`

Small-core Linux performs:

- camera capture and display;
- AI2D preprocessing;
- nncase/KPU proposal inference;
- proposal thresholding, NMS, ROI expansion, and scheduling; and
- result association and OSD drawing.

The big core performs RVV crop-and-decode for proposed ROIs.  A request may
refer to one source frame plus a bounded array of ROI rectangles, or to
pre-copied grayscale ROI slots.  Start with the design that has the clearest
ownership and fewest cache operations, then benchmark both if necessary.

The measured 720p TinyTag path currently spends roughly 1.9 ms in AI2D and
about 2.2 ms in KPU inference.  With RVV and decimation 1.0, two large true-tag
ROIs took about 13.2 ms and 11.0 ms, while false ROIs completed in approximately
0.6--0.8 ms.  Total crop decoding for five proposals was about 26.5 ms.  Thus a
120 fps source cannot enqueue every frame for full ROI decoding.  NPU inference
on the small core can still be useful because it overlaps big-core ROI work,
but requests must be bounded and replace stale pending work.

The default TinyTag crop decoder remains RVV for big-core Linux.  Its C fallback
uses AprilTag-demo-like detector settings while retaining `quad_decimate=1.0`.
The small-core Linux build may use the C/scalar path for smoke tests, but normal
AMP operation should select the remote RVV backend.

### Common application API

Refactor toward an application-facing detector service with at least:

- local RVV implementation;
- local scalar/C implementation where practical; and
- remote AMP implementation.

The request/result types, parameter validation, sequencing, timing, and result
comparison tests should be shared.  The application should not know whether a
job is executed in-process or over RPMsg.  This is the main protection against
throw-away work and allows both apps to remain runnable in either Linux-core
configuration.

## Implementation phases and acceptance gates

### Phase 0: freeze the current baseline

- Commit current TinyTag and scalar-nncase work on `dev`.
- Rebuild both demos and archive executable hashes and board commands.
- Record monolithic correctness and timing at 720p/60 camera and capped display.
- Preserve the current big-core Linux defconfig and known-good images.

Gate: both applications still run on big-core Linux and reproduce the current
display alignment and detector behavior.

### Phase 1: forward-port small-core Linux

- Create the AMP branch from current `dev`.
- Port the minimum relevant changes from
  `opt_linux_on_small_core_cherry-picked`.
- Rebuild the complete small-core userspace without `-v`/RVV ISA requirements.
- Audit ELF attributes and disassembly of startup objects, shared libraries,
  plugins, and final executables.
- Boot and record OpenSBI/U-Boot/Linux hart IDs, ISA strings, clocks, PMP, and
  memory reservations.

Gate: small-core Linux reaches a shell reliably and the big core is deliberately
reserved rather than accidentally offline.

### Phase 2: release a minimal big-core payload

- Reuse official U-Boot reset-vector/release code.
- Boot an independently linked heartbeat image in reserved memory.
- Confirm UART or shared-counter output, traps, timer, and RVV arithmetic.
- Reboot repeatedly and verify no Linux memory corruption.

Gate: at least 100 cold/warm boot cycles complete without overlap, trap, or
heartbeat loss.

### Phase 3: prove shared memory and cache maintenance

- Add an aligned shared counter and patterned buffer exchange.
- Exercise both producer directions, sizes crossing cache lines/pages, and ring
  wraparound.
- Use sequence numbers and CRCs to detect stale or partially visible data.
- Measure clean/invalidate costs and mailbox interrupt latency.

Gate: sustained bidirectional integrity testing reports zero corruption and
documents the required cache/barrier sequence.

### Phase 4: RPMsg-Lite transport

- Pin/import RPMsg-Lite and define static vrings in reserved memory.
- Connect mailbox notifications.
- Implement Linux standard RPMsg interoperability or the minimal driver needed.
- Add capability/version handshake, echo, timeout, restart generation, and
  transport statistics.

Gate: bidirectional messages, endpoint restart, queue-full behavior, and stale
generation rejection pass reproducibly.

### Phase 5: shared payload slots

- Implement fixed-size buffer descriptors and ownership transitions.
- Add host-side protocol/ring tests and target CRC/pattern tests.
- Benchmark descriptor-only and realistic image transfers.
- Confirm camera buffers are requeued independently from remote completion.

Gate: no leaked slots, overwrite, stale completion, or camera starvation under
queue saturation and peer restart.

### Phase 6: AprilTag offload

- Move the RVV full-frame detector into the big-core service.
- Add local/remote backend selection to `apriltag_demo`.
- Compare IDs, corners, hamming, margins, and ordering against the monolithic
  implementation over a recorded corpus and live camera.
- Enable latest-wins scheduling and measure result age.

Gate: correctness is equivalent within documented numeric tolerances, display
remains responsive, and p50/p95/p99 latency plus drop rate are recorded.

### Phase 7: small-core nncase/KPU validation

- Build the documented scalar nncase hybrid for the small-core rootfs.
- Audit every final object for unsupported vector instructions.
- Load the TinyTag kmodel; run AI2D, KPU inference, and output comparison.
- Stress device initialization, repeated inference, and recovery.

Gate: model outputs match the big-core Linux baseline within expected numeric
tolerance and no unsupported-instruction trap or device-ownership conflict
occurs.

### Phase 8: TinyTag ROI offload

- Add batched ROI request/result ABI and remote crop-decoder backend.
- Preserve frame/ROI sequence through proposal, remote decode, dedupe, and OSD.
- Reject stale results and bound active/pending jobs.
- Compare detections and timings with the local RVV backend.

Gate: live detections and OSD remain correctly associated with displayed frames,
with measured queue depth, replacement rate, and end-to-end result age.

### Phase 9: optimize only after measurement

Possible later optimizations include adaptive ROI decimation, proposal merging,
tracking-driven scheduling, grayscale sharing, larger slot batches, DMA-assisted
copies, and dma-buf integration.  Profile first; preserve the transport ABI or
version it explicitly.

Dual Linux may be evaluated here if a concrete Linux-only service requires it.
The boot, mailbox, shared-memory, cache, protocol, and application backend work
from earlier phases should remain reusable.

## Verification matrix

### Build and ABI checks

- clean builds of big-core monolithic and small-core AMP configurations;
- ISA/ELF-attribute/disassembly audit of all small-core binaries;
- compile-time structure size/offset assertions in C/C++ and Rust;
- protocol magic/version/capability mismatch tests; and
- reproducible firmware and userspace hashes.

### Host tests

- ring empty/full/wrap and producer/consumer race cases;
- slot state-machine validity;
- sequence wrap, stale result, timeout, and restart generation;
- malformed descriptor, length, stride, ROI, and offset rejection; and
- recorded-image parity for local versus remote detectors.

### Board tests

- repeated dual-payload boots and peer restart;
- mailbox interrupt and polling fallback behavior;
- cache-line/page-boundary patterns with CRC;
- queue overload with continuous 720p/60 capture;
- HDMI and LCD display paths, including OSD alignment;
- TinyTag AI2D/KPU execution from small-core Linux; and
- long-running thermal and memory-integrity tests.

### Performance reporting

Report p50, p95, and p99 rather than only FPS:

- capture-to-submit age;
- queue wait, remote execution, and completion transport;
- capture-to-detection and capture-to-OSD age;
- queue depth, replacements/drops, and stale-result count;
- cache maintenance and copy cost; and
- CPU utilization on both cores.

Throughput is useful only if latency remains bounded and results correspond to
the current scene.

## Failure handling and rollback

- Keep the current big-core Linux build and images intact.
- Add dual-OS support through a separate defconfig/image composition.
- Keep local detector backends selectable by build or runtime configuration.
- Apply finite timeouts to remote jobs; never block capture/display indefinitely.
- On protocol mismatch or peer failure, stop submitting, reclaim only slots from
  the current generation according to the recovery protocol, and report a clear
  health state.
- Add a big-core watchdog or controlled remote restart only after restart-safe
  slot generations work.
- Treat memory map, protocol ABI, cache policy, and device ownership changes as
  reviewed interfaces, not incidental implementation details.

## Immediate next actions

1. Commit the current `dev` progress and create the AMP branch.
2. Diff `opt_linux_on_small_core_cherry-picked` against its base and classify
   commits into boot/config, toolchain/ISA, memory/device tree, and application
   changes.
3. Capture the official SDK's exact U-Boot release path, linker addresses,
   mailbox register sequence, and interrupt routing in a small bring-up note.
4. Draft the first AMP memory map against the current DT/CMA/MMZ reservations.
5. Boot small-core Linux and archive the complete OpenSBI/U-Boot/Linux log.
6. Build the minimal big-core heartbeat before introducing RPMsg-Lite or Rust.

This ordering produces useful proof at each step, keeps the current product path
available, and makes the platform work reusable by AprilTag, TinyTag, a future
bare-metal/Embassy payload, or a later dual-Linux experiment.
