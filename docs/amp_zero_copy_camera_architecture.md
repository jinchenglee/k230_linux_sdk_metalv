# Portable AMP zero-copy camera architecture

Status: the portable UAPI, allocator, remote-token protocol, and remote
platform-operation layers are implemented and target-validated on K230. The
K230 backend remains an executable reference, not the cross-platform ABI. The
reflashed image has validated the Device Tree provider path, and the temporary
module-parameter fallback has been removed.

## Proven invariant

The portable core is an ownership state machine, not a physical-address trick:

```text
DEVICE_QUEUED -> LATEST_PENDING -> REMOTE_OWNED -> DEVICE_QUEUED
                         |                              ^
                         +-- newer frame supersedes ---+
```

Only the capture device may write `DEVICE_QUEUED`; only Linux may change
`LATEST_PENDING`; only the remote may read `REMOTE_OWNED`. A generation and
sequence-matched completion is the sole transition out of remote ownership.
This policy works with V4L2 or another producer and with bare-metal, an RTOS,
or another OS on the remote.

## Layer boundaries

### 1. Application policy

The application chooses buffer count, which completed frame is retained, when
work is submitted, and when a returned buffer is requeued. It consumes opaque
buffer IDs and remote-visible tokens. It must not depend on Linux PFNs or infer
addresses through `/proc/pagemap`.

### 2. Linux buffer provider

The provider exports pinned DMA-BUFs and metadata:

- pool ID and buffer ID;
- capacity and alignment;
- an opaque remote-visible token;
- supported CPU mapping/cache policy;
- lifetime tied to the DMA-BUF file descriptor.

The first backend is reserved contiguous memory. A later backend may allocate
from CMA or a DMA heap when the remote can consume the resulting address or
scatter-gather/IOMMU mapping. Applications use the same UAPI for either.

Device Tree should describe a reserved-memory region and a provider node with
`memory-region`, buffer size/count/alignment, and the remote address-translation
method. Compile-time addresses are a platform backend detail, not generic UAPI.

### 3. Wire protocol

Protocol registration binds an opaque pool/buffer token to one protocol
generation. Work submission carries pool ID, buffer ID, offset, length, format,
and geometry; it does not repeatedly send an unchecked physical address.

The remote platform validates registrations against its own allowlist. An
endpoint restart clears registrations, queued work, and ownership. Stale
generation or sequence values can never release a buffer.

### 4. Remote platform operations

Generic work handling calls platform operations for:

- translating a registered token into a local address;
- validating reachability and bounds;
- invalidating or synchronizing the remote cache;
- acquire/release ordering around ownership transfer.

The CRC or detector is independent of these operations. A coherent platform
may implement cache synchronization as barriers only; a non-coherent platform
must perform the required maintenance.

## Portable contracts versus K230 facts

| Contract | Portable requirement | Current K230 backend |
|---|---|---|
| Producer import | Accepts DMA-BUF or equivalent pinned buffer | VVCAM `VB2_DMABUF` with contiguous SG |
| Remote token | Provider-defined opaque 64-bit value | Identity-mapped physical address |
| Storage | Pinned for complete exported lifetime | 12 MiB no-map reserved DRAM |
| Address translation | Provider/remote agreement | No active IOMMU; direct DRAM address |
| Linux cache policy | Declared by provider | Write-combine diagnostic map; no production map |
| Remote cache policy | Platform operation | 64-byte RISC-V invalidate plus acquire fence |
| Device completion | DMA writes globally visible before ownership publication | VVCAM DQBUF assumption, CRC-validated on target |
| Buffer shape | Provider metadata, not protocol constants | Six runtime allocations from a 12 MiB pool; 1,384,448 bytes each for the validated NV12 mode |

K230 additionally requires reserved pages to retain valid `struct page`
metadata because its exporter builds a one-entry SG table with
`pfn_to_page()`. That is not universal. A platform without such metadata needs
a different provider backend rather than weakening this check.

Sensor timing is also a producer-platform concern, not part of the buffer or
wire ABI. On K230, setting the V4L2 output to 1280x720 does not change the
OV5647 source mode. The application must request 1280x720 at 60 fps through
the VVCAM scene/mode control before `STREAMON`; otherwise it inherits the
nominal 30 fps source and delivers only about 28 fps. The application retains
1920x1080 at 30 fps as a compatibility fallback.

## Target validation checkpoint (2026-09-15)

The portable provider UAPI allocated six page-rounded 1,384,448-byte DMA-BUFs
from the 12 MiB pool. VVCAM imported all six directly, and the big core read
the 921,600-byte Y plane in place. The tested sensor selection reported
1280x720 at 60 fps; this V4L2 driver does not implement `VIDIOC_G_PARM`, so
measured delivery is the authoritative rate.

- Production-path run, with remote full-frame CRC retained as representative
  work and the redundant Linux CRC disabled: 564 captures, submissions, and
  completions in 10.017 seconds (56.30 fps), zero supersessions, zero copies,
  15.057 ms average big-core CRC, and 17.036 ms maximum remote hold.
- Integrity run, with the intentionally expensive scalar Linux CRC enabled:
  50/50 matching CRCs, zero mismatches, zero copies, and 18.309 ms maximum
  remote hold. Linux CRC averaged 190.191 ms, so 121 newer pending frames were
  deliberately superseded; this is diagnostic overhead, not production-path
  throughput.
- The capability library now declares its own `libm` dependency. This matters
  because it is loaded with `dlopen()`; larger demo processes had previously
  hidden the missing dependency by loading `libm` indirectly.

## K230 Device Tree closure (2026-09-21)

The reflashed image initialized the 12 MiB pool from its Device Tree provider
without a legacy-fallback warning. Its production run completed 565/565 frames
in 10.024 seconds (56.37 fps), with zero supersessions, zero copies, and a
16.633 ms maximum remote hold. The integrity run verified 50/50 CRCs with zero
mismatches and zero copies.

The temporary module parameters were then removed, making absence of the
provider a hard module-load error. The rebuilt module exposed no parameter
directory and completed a final 169/169-frame hot-load test in 3.026 seconds
(55.85 fps), with zero supersessions and zero copies. The firmware's fixed K230
address allowlist remains a platform-backend policy, while the application and
wire protocol continue to use opaque remote tokens.
