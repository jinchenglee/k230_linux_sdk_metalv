# K230 AMP shared payload slots

Status: shared-slot transport and the deliberately high-latency live-camera
baseline validated on matched hardware on 2026-09-15.

## Purpose

RPMsg is the control plane, not the image transport. Phase 5 proves that Linux
and the big core can transfer ownership of frame-sized shared-memory buffers by
descriptor, preserve cache visibility, bound outstanding work, and recover
from a peer endpoint restart. The service performs CRC work rather than tag
detection so transport correctness is isolated from the Phase 6 algorithm.

## Memory map

The small-core DTB reserves `0x1d000000..0x1fffffff` from Linux. Phase 5 uses:

| Region | Physical range | Size |
|---|---:|---:|
| Existing diagnostics | `0x1d000000..0x1d201000` | about 2 MiB |
| RPMsg resource table | `0x1d300000..0x1d300fff` | 4 KiB |
| RPMsg vrings | `0x1d400000..0x1d40ffff` | 64 KiB |
| RPMsg buffer pool | `0x1d500000..0x1d53ffff` | 256 KiB |
| Phase 5 payload slots | `0x1d600000..0x1d9fffff` | 4 x 1 MiB |

Slot base addresses and sizes are multiples of the K230's 64-byte cache line.
The wire descriptor carries `data_length` and `padded_length` separately. CRC
covers only `data_length`; cache maintenance covers `padded_length`. This
allows non-aligned future ROI payloads without letting one owner invalidate a
cache line belonging to another.

## Wire contract

The shared definitions are in
`buildroot-overlay/package/metal_v_amp/src/rpmsg_protocol.h`.

`SLOT_SUBMIT` contains the versioned 40-byte `K2AM` header plus slot ID,
pool-relative offset, data and padded lengths, expected CRC32, format, and
optional width/height/stride. It is 88 bytes. The firmware accepts only the
canonical offset for the slot ID; a descriptor cannot make the big core read
an arbitrary physical address.

`SLOT_COMPLETE` is 80 bytes and returns the slot identity, observed CRC32, and
big-core cycles spent invalidating cache and computing CRC. A completion with
`CRC_MISMATCH` still returns ownership: the consumer has finished reading the
slot and the mismatch is a reported result, not an indefinitely busy buffer.

The ownership state machine is:

```text
Linux free/filling -> SLOT_SUBMIT -> big-core queued/reading
big-core queued/reading -> SLOT_COMPLETE -> Linux free
big-core queued/reading -> endpoint restart -> discarded; new generation
```

Linux must not modify a submitted slot before receiving its completion. It
must match both the free-running 64-bit sequence and generation, not infer
freshness from the wrapping slot ID.

## Firmware scheduling

The RPMsg receive callback validates and enqueues a descriptor but does no
frame-sized work. The main firmware polling loop processes one queued slot at a
time. There are exactly four queue entries and a four-bit ownership mask, so
memory use and outstanding work are bounded. Same-slot reuse is rejected with
`SLOT_BUSY`; invalid slot, range, format, and stale-generation errors are
distinct.

On endpoint restart, queued jobs are counted as dropped, queue indices and the
busy mask are cleared, and the protocol generation advances. Linux then
handshakes again and can safely reclaim every slot; an old-generation submit
is rejected.

## Build and deploy

Build the small-core configuration from the SDK root:

```sh
make -C buildroot-overlay/package/metal_v_amp/src check
make CONF=k230_canmv_small_core_defconfig metal_v_amp-rebuild
```

The first command builds a native host copy of the client and checks the wire
structure sizes, padding rules, known CRC32 vector, and shared descriptor
validator without requiring a board.

Deploy these four matched files to the board:

```text
output/k230_canmv_small_core_defconfig/images/metal-v-k230.bin
output/k230_canmv_small_core_defconfig/target/root/amp/rpmsg-echo-test
output/k230_canmv_small_core_defconfig/target/root/amp/rpmsg-slot-test
output/k230_canmv_small_core_defconfig/target/root/amp/rpmsg-regression.sh
```

The firmware must replace `/root/amp/metal-v-k230.bin` on the root filesystem,
then the board must reboot: U-Boot loads it before Linux starts. Replacing only
the userspace programs does not update the running big-core service.

## Validation

After reboot, use the full matched gate as the first RPMsg traffic:

```sh
/root/amp/rpmsg-regression.sh --post-boot 2>&1 | \
    tee /tmp/rpmsg-phase5-postboot.log
```

For focused iteration:

```sh
/root/amp/rpmsg-slot-test --loops 1 --timeout-ms 5000
/root/amp/rpmsg-echo-test --stats
```

The standalone test checks boundary sizes, a realistic 1280x720 Y8 payload,
invalid descriptors, four outstanding slots, premature same-slot reuse, and
restart/generation recovery. The regression's final accounting requires:

- zero CRC mismatches and failed sends;
- busy mask and queue depth both zero;
- `submitted == completed + dropped_on_restart + queue_depth`;
- queue high-water mark of four;
- the original RPMsg ring/fetch/callback invariants still hold.

The target output supplies descriptor round-trip time, effective payload rate,
and separate big-core cache-invalidate and CRC cycle counts. Linux pattern-fill
time is intentionally outside descriptor RTT; this is a by-address transport
test, not a benchmark of copying a camera frame through an uncached `/dev/mem`
mapping.

## Matched target result

The rebuilt firmware and utilities were copied to the K230, copied back and
verified byte-for-byte, then booted together. The first RPMsg traffic after
boot was:

```sh
/root/amp/rpmsg-regression.sh --post-boot
```

All 26 checks passed. Final accounting was:

```text
rvq_avail=18516 rvq_consumed=18516 fetch_rx=18516 rx_callbacks=18516
tx_failed=0 endpoint_restarts=2 restart_failures=0
slot_submitted=15 slot_completed=15 slot_rejected=4
slot_dropped_restart=0 slot_crc_mismatch=0
slot_busy_mask=0 slot_queue_depth=0 slot_queue_high_water=4
```

Thus every RPMsg buffer was consumed and callback-delivered, every accepted
slot was completed, all four slots were simultaneously exercised, and no CRC,
ownership, send, or endpoint-restart error occurred. The restart arrived after
the queued CRC work had completed (`slot_dropped_restart=0`), then generation
advance, old-generation rejection, and a new-generation slot transaction all
passed.

### Standalone timing

A subsequent `rpmsg-slot-test --loops 1 --timeout-ms 5000` run measured:

| Payload | Descriptor RTT | Cache invalidate | Big-core CRC | Effective rate |
|---:|---:|---:|---:|---:|
| 64 B | 0.098 ms | <0.001 ms | 0.002 ms | 0.62 MiB/s |
| 4 KiB | 0.219 ms | <0.001 ms | 0.120 ms | 17.83 MiB/s |
| 64 KiB | 2.003 ms | 0.003 ms | 1.899 ms | 31.20 MiB/s |
| 1280x720 Y8 | 26.863 ms | 0.036 ms | 26.708 ms | 32.72 MiB/s |
| 1 MiB | 30.556 ms | 0.041 ms | 30.387 ms | 32.73 MiB/s |

At 720p, about 99.4% of descriptor RTT is the deliberately scalar diagnostic
CRC. Cache invalidation is only 0.036 ms and the remaining RPMsg/userspace
round-trip overhead is about 0.119 ms. The 64-byte case gives a roughly
0.1 ms end-to-end control baseline from userspace, distinct from the lower
firmware notification-only measurement.

The four-slot pressure case took 133.874 ms. The premature slot-0 resubmission
was serviced only after its first completion (`busy=0 serialized=1`), so the
elapsed time corresponds to five serialized 720p CRC jobs. This is safe but
also makes the camera contract quantitative: diagnostic CRC capacity is about
37.4 720p frames/s, below a 60 fps producer. Camera integration must therefore
be latest-wins, immediately requeue a captured VI buffer, and drop rather than
wait whenever all AMP slots are owned by the remote core.

## Live-camera baseline

The target client implements a deliberately simple camera producer. It uses
six V4L2 capture buffers and latest-wins dequeue, copies the Y plane directly
from the held VI buffer into a free uncached AMP slot while calculating the
Linux-side expected CRC, calls `v4l2_drm_dump_release()`, and only then
publishes the descriptor. There is one full-frame CPU copy. The big core reads
the shared slot in place and returns a completion; that completion frees the
AMP slot, not the already-requeued VI buffer. If all slots are owned, the
client requeues immediately and counts a no-slot drop. Run:

```sh
/root/amp/rpmsg-slot-test --camera-seconds 10 --timeout-ms 5000 2>&1 | \
    tee /tmp/rpmsg-phase5-camera.log
```

The matched 10-second run produced:

```text
produced=281 delivered=169 requeued=169 submitted=169 completed=169
no-slot-drop=0 capture-errors=0 submit-errors=0 outstanding=0
produced-fps=26.99 delivered-fps=16.23 max-vi-hold=71.894 ms
completion-ms{p50=58.793,p95=61.593,p99=62.461,max=463.183}
```

Here `produced` counts every buffer dequeued by latest-wins; 112 older buffers
were immediately recycled while draining the driver's done queue. Every frame
handed to the producer was requeued, every submitted shared slot completed,
and no ownership leaked. The producer never waits for a remote completion
while holding a VI buffer. The earlier transport pressure test separately
reached a four-slot queue high-water mark and safely serialized a fifth job.
This camera run did not itself exhaust the four slots (`no-slot-drop=0`).

The 71.894 ms maximum VI hold is an accepted limitation of this functionality
baseline: uncached slot writes and expected-CRC calculation happen before
QBUF. Adding a cached staging copy would shorten that hold but add another
full-frame copy, so it is intentionally not part of this baseline.

The low-latency follow-up is a zero-copy capture pool. VI DMA rotates through
shared buffers; completed buffers are published latest-wins; the big core owns
one immutable buffer while processing it; and Linux recycles completed but
unclaimed buffers to VI. This requires explicit VI/free/published/remote-owned
state transitions, sufficient NV12-sized buffers, and V4L2 support for queuing
the reserved shared memory. It is a separate experiment rather than an
unproven optimization hidden inside the Phase 5 baseline.
