# Portable AMP zero-copy camera architecture

Status: architectural follow-up to the validated K230 fixed-pool prototype.
The K230 result is the executable reference, not the intended cross-platform
ABI.

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
| Buffer shape | Provider metadata, not protocol constants | Six 2 MiB buffers at `0x1da00000` |

K230 additionally requires reserved pages to retain valid `struct page`
metadata because its exporter builds a one-entry SG table with
`pfn_to_page()`. That is not universal. A platform without such metadata needs
a different provider backend rather than weakening this check.

## Migration sequence

1. Preserve the fixed K230 implementation and its two hardware logs as the
   regression baseline.
2. Introduce a generic pool UAPI using `remote_token`; retain source aliases
   for the existing K230 test during migration.
3. Move pool geometry into a Device Tree provider node and bind a platform
   driver to its `memory-region`. Keep a clearly labelled K230 legacy fallback
   only until the new DT image boots.
4. Change the wire ABI from canonical K230 camera addresses to pool
   registration plus buffer ID/offset, with a K230 firmware validator backend.
5. Run the direct DMA-BUF probe, cross-core CRC test, no-Linux-read throughput
   test, endpoint-restart test, and original Phase 4/5 regression suite.
6. Remove the legacy constants only after the DT-backed K230 path reproduces
   28 FPS, zero CRC mismatches, and zero ownership leaks.

This sequence keeps the proven low-latency path runnable while each portable
boundary is introduced and independently tested.
