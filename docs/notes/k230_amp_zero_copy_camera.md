# K230 AMP zero-copy camera experiment

Status: fixed-pool DMA-BUF exporter and direct V4L2 import validated on target;
cross-core in-place CRC validation passed on target.

## Goal

Remove every full-frame CPU copy between ISP completion and big-core work. The
Phase 5 baseline remains the correctness reference and is not modified by this
experiment.

The existing K230 capture queue supports `VB2_DMABUF`, `VB2_USERPTR`, and
`VB2_MMAP` through `vb2_dma_contig_memops`. Its buffer prepare callback obtains
a contiguous DMA address and passes it to the ISP. The quick prototype makes
six already-reserved AMP buffers into DMA-BUFs and imports those fds into V4L2,
so both allocation and ownership remain under application control. No
frame-sized staging or slot copy is present.

## Ownership model

Use six NV12 capture buffers with these mutually exclusive states:

```text
VI_QUEUED -> LATEST_PENDING -> REMOTE_OWNED -> VI_QUEUED
                   |                              ^
                   +-- superseded by newer ------+
```

- VI owns every `VI_QUEUED` buffer and may overwrite it.
- Linux owns at most one `LATEST_PENDING` completed buffer. A newer completed
  frame replaces it and the older pending buffer is immediately QBUF'd.
- The big core owns at most one `REMOTE_OWNED` buffer. Linux must not QBUF it
  until its matching generation/sequence completion arrives.
- With one pending and one remote-owned buffer, four of six buffers remain
  queued to VI. The camera never waits for remote completion.
- After a remote completion, Linux QBUFs the completed buffer and promotes the
  newest pending buffer. Intermediate frames are intentionally dropped.

The state transition, not timing, prevents VI and the big core from accessing
the same buffer concurrently.

## Quick fixed-pool prototype

The loadable `k230_amp_camera_pool` exporter uses this currently unused part of
the existing 48 MiB no-map AMP reservation:

| Property | Prototype value |
|---|---:|
| Physical base | `0x1da00000` |
| Pool size | 12 MiB |
| Buffers | 6 |
| Capacity per buffer | 2 MiB |
| 1280x720 NV12 `sizeimage` | 1,382,400 B |

The root-only `/dev/k230-amp-camera-pool` ioctl returns one fd, stable physical
address, and capacity for each selected ID. The exporter verifies at load time
that every page is still marked reserved, maps each attachment through the DMA
API, and refuses a second simultaneous export of one ID.

The pool has no cached Linux mapping. Attachment mapping deliberately uses
`DMA_ATTR_SKIP_CPU_SYNC` to avoid cache operations through the absent linear
mapping. Its CPU-access callbacks still issue `dma_rmb()` after device
completion and `dma_wmb()` before device ownership; RPMsg publication adds its
own ordering, and the big core fences and invalidates before reading. The
production path does not CPU-map or read frame bytes.

Copy the module and probe to a running matched 6.6.36 image, then run:

```sh
/sbin/insmod /tmp/k230_amp_camera_pool.ko
/root/amp/v4l2-dma-probe --frames 30
```

The probe gets all six fixed fds, configures `/dev/video2` for 1280x720 NV12
`V4L2_MEMORY_DMABUF`, QBUFs every fd, captures and requeues 30 frames, then
STREAMOFFs and releases every attachment. Success proves the camera can DMA
directly into the reserved AMP pool with zero CPU frame copies.

Target result on 2026-09-15:

```text
capture: completed=30 elapsed=1.062 s fps=28.24 seen-mask=0x3f copies=0
PASS fixed-pool DMA-BUF capture: exported=6 queued=6 captured=30
```

All six application-selected physical buffers circulated through V4L2. The
VVCAM CMA allocations still printed by the driver are its internal working
allocations; the captured frames remained in the imported fixed pool.

### Abandoned discovery shortcut

The first probe tried to recover physical addresses from the V4L2 driver's
ordinary MMAP buffers through `/proc/self/pagemap`. Both before and after
faulting the mapping, pagemap reported the special DMA VMA's first page as not
present. This does not demonstrate fragmentation; it demonstrates that
pagemap is not a supported physical-address ABI for these mappings. More
importantly, that route would still make the application depend on the current
V4L2 allocator and an opaque address-discovery accident. It was replaced by
the explicit exporter.

### K230-specific assumptions to revisit

- The fixed base is inside the K230 small-core image's existing no-map AMP
  reservation and is unused by the Phase 4/5 ABI.
- K230 Linux retains sparse `struct page` metadata for that no-map RAM.
- The camera has no active IOMMU translation and can address this sub-512 MiB
  physical region directly.
- The VVCAM driver imports single-plane contiguous DMA-BUFs using
  `vb2_dma_contig_memops`.
- Linux never accesses the frame through a cached mapping; the ISP is the sole
  writer while `VI_QUEUED`, and the big core is the sole reader while
  `REMOTE_OWNED`.
- The ISP's buffer-done event is assumed to mean its preceding DMA writes are
  globally visible. A diagnostic cross-core CRC test must verify this under
  sustained reuse before the assumption is accepted.

These are prototype constraints, not intended generic ABI requirements.

## Cross-core integrity gate

The quick prototype adds a registered-camera-buffer capability rather than
allowing arbitrary physical addresses in every work request:

1. Linux registers each buffer ID, physical base, and capacity after HELLO.
2. Firmware validates alignment, DRAM bounds, uniqueness, and capacity, then
   stores the table for the current protocol generation.
3. Frame submission names a registered buffer ID plus data/format geometry.
4. Endpoint restart clears the table and ownership; Linux re-handshakes and
   registers the still-allocated V4L2 buffers again.
5. Firmware invalidates and reads the Y extent in place. Completion transfers
   ownership back to Linux, which may then QBUF that buffer.

This preserves the Phase 5 descriptor-safety principle while using the fixed
capture pool.

Firmware accepts only the canonical six-buffer table above. Registration is
idempotent within one generation, but an endpoint restart clears registration,
queued work, and remote ownership. The Linux exerciser implements the ownership
state machine at the top of this document and never has more than one
remote-owned plus one latest-pending frame.

For the integrity gate, Linux maps the returned DMA-BUF only after the remote
completion and calculates a second CRC using `DMA_BUF_IOCTL_SYNC`. That
diagnostic CPU read is not part of the intended production pipeline and is
disabled with `--no-verify-crc`. Neither mode copies the frame.

The matched firmware, exerciser, and module are staged in `/root/amp` on the
development board. Reboot so U-Boot loads the new `metal-v-k230.bin`, then run:

```sh
/root/amp/run-zero-copy-camera.sh 2>&1 |
    tee /tmp/rpmsg-zero-copy-camera.log
```

The helper loads `k230_amp_camera_pool.ko` only if its device node is absent,
then runs a 10-second test with a 5-second completion timeout. Expected success
has `copies=0`, `crc-mismatch=0`, equal submitted/completed counts, and a PASS
line. `superseded` is an intentional latest-frame drop count, not corruption.

Target integrity result on 2026-09-15:

```text
capture: frames=152 submitted=52 completed=52 superseded=100
         seen-mask=0x3f elapsed=10.226 s copies=0
integrity: verified=52 crc-mismatch=0 max-remote-hold=23.480 ms
           big-ms{invalidate-avg=0.036,crc-avg=20.817}
PASS zero-copy camera-to-big-core CRC test
```

This verifies the complete ownership and visibility path on 52 frames. The
maximum remote hold is below the roughly 35 ms camera period. The low submitted
rate and 100 intentional supersessions in this run are not a big-core limit:
the post-completion Linux diagnostic CRC reads the write-combined mapping and
blocks the event loop. The exerciser now times that diagnostic separately.

After the integrity pass, measure the intended no-Linux-read path with:

```sh
/root/amp/run-zero-copy-camera.sh --no-verify-crc 2>&1 |
    tee /tmp/rpmsg-zero-copy-camera-production.log
```

Target production-path result on 2026-09-15:

```text
capture: frames=282 submitted=282 completed=282 superseded=0
         seen-mask=0x3f elapsed=10.003 s copies=0
integrity: verified=0 crc-mismatch=0 max-remote-hold=20.970 ms
           big-ms{invalidate-avg=0.036,crc-avg=20.817}
PASS zero-copy camera-to-big-core CRC test
```

This is 28.19 frames/s, matching the direct camera probe's 28.24 frames/s.
Every captured frame completed remotely before the next frame needed the
remote-owned slot, so the latest-pending path never had to supersede a frame.
The preceding diagnostic run supplies the integrity evidence; `verified=0` is
expected here because this path deliberately performs no Linux frame read.

## Portability follow-up

After the K230 path works end to end, refactor the exporter into a generic
reserved-memory AMP DMA-BUF pool. Device Tree or module parameters will supply
the memory region, remote-visible base, cache policy, and minimum alignment;
userspace will select buffer count, size, and alignment. The wire protocol
will prefer pool ID plus offset over a raw K230 physical address. Platform
documentation must separately state DMABUF-import, IOMMU/address-translation,
DMA reachability, completion-ordering, and cache-maintenance requirements.
