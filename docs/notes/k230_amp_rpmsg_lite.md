# K230 Linux/RPMsg-Lite bring-up

This note records the first standards-based transport between Linux on the
scalar C908 and the Metal-V bare-metal payload on the RVV C908. It builds on
the validated bidirectional mailbox and cache-maintenance layers documented in
`k230_amp_mailbox.md`.

This stage is source-complete and build-verified as of 2026-09-04. Hardware
enumeration, a 21-byte echo, and the threaded mailbox IRQ path have been
validated on the board.

## Architecture

Linux is the RPMsg host/master and uses its standard remoteproc,
remoteproc-virtio, RPMsg, name-service, and RPMsg character drivers. U-Boot
starts the big core before Linux, so the K230 driver attaches to an already
running remote processor rather than loading or starting firmware.

The big core uses NXP RPMsg-Lite v5.4.0 in remote mode. The exact upstream
source is pinned as the `third_party/rpmsg-lite` submodule. Initialize a fresh
checkout with:

```sh
git submodule update --init --recursive
```

The existing `k230_amp_mailbox` driver owns IRQ 109 and both mailbox
directions. It also implements the remoteproc operations, avoiding two drivers
racing to acknowledge the same hardware interrupt. Because the K230 channel
does not carry a virtqueue ID, both sides inspect both vrings after each
doorbell; a virtqueue with no work returns immediately.

The original `/dev/k230-amp-mailbox` diagnostic ABI remains available.
Do not run its mailbox test concurrently with RPMsg traffic because both
protocols share the doorbell and its diagnostic completion count.

## Shared-memory layout

All addresses are physical/device addresses and lie inside Linux's existing
`0x1d000000`--`0x1fffffff` no-map reservation.

| Region | Address | Size | Purpose |
| --- | ---: | ---: | --- |
| Diagnostic ABI | `0x1d000000` | about 2 MiB | Existing CRC/XOR test |
| RPMsg resource table | `0x1d300000` | 4 KiB | One virtio RPMsg vdev |
| RPMsg vring 0 | `0x1d400000` | 32 KiB | Remote TX / Linux RX |
| RPMsg vring 1 | `0x1d408000` | 32 KiB | Linux TX / remote RX |

Each vring has 256 descriptors and 4 KiB alignment. Linux allocates the RPMsg
message buffers from coherent memory and writes their physical addresses into
the descriptors. The big core uses identity physical addressing and performs
`dcache.ipa`/`dcache.cpa` maintenance for descriptor-selected buffers.

Linux's standard RPMsg buffer is 512 bytes: a 16-byte header and at most 496
bytes of payload. This transport is the control plane. Camera frames, grayscale
ROIs, and result arrays should remain in a separately managed shared bulk pool;
RPMsg messages should carry buffer addresses/handles, dimensions, strides,
formats, sequence numbers, and completion status. Do not fragment image data
into 496-byte RPMsg messages.

With 256 descriptors per direction, Linux allocates 512 such buffers, or
256 KiB total. That is small beside the 48 MiB AMP reservation and preserves
the conventional Linux/RPMsg-Lite layout. If profiling later shows that the
queue is unnecessarily deep, reduce the descriptor count on both sides; keep

## Big-core service

The firmware uses RPMsg-Lite's static API, so no heap is required. Its K230
platform port provides:

- mailbox notification in each direction;
- deferred virtqueue dispatch from the main loop, not trap context;
- identity physical/virtual translation;
- RISC-V memory barriers; and
- physical-address cache clean/invalidate operations.

Endpoint 30 is an echo service. Once Linux initializes the link, the firmware
announces it as `rpmsg-raw`, allowing the stock Linux character driver to
create `/dev/rpmsg0`. The UART3 status display reports
`RPMsg link/rx/tx`.

## Build

From the SDK root:

```sh
make CONF=k230_canmv_small_core_defconfig linux-dirclean
make CONF=k230_canmv_small_core_defconfig
```

A clean Linux rebuild is required the first time because this stage enables
`CONFIG_REMOTEPROC=y` and changes the mailbox DT node's resources. The
complete image is:

```text
output/k230_canmv_small_core_defconfig/images/sysimage-sdcard.img
```

Relevant individual artifacts are:

```text
output/k230_canmv_small_core_defconfig/images/metal-v-k230.bin
output/k230_canmv_small_core_defconfig/images/k230-canmv.dtb
output/k230_canmv_small_core_defconfig/target/lib/modules/6.6.36/updates/k230_amp_mailbox.ko
output/k230_canmv_small_core_defconfig/target/root/amp/rpmsg-echo-test
```

Unlike the earlier mailbox-only stages, copying just the firmware and module
onto the old installation is insufficient: its kernel lacks remoteproc symbols
and its DT node lacks the three RPMsg memory resources. Flash the matched image,
or manually replace the kernel, DTB, module, firmware, and rootfs test utility
as one versioned set.

## Hardware validation

After automatic dual-core boot, on ACM0 run:

```sh
dmesg | grep -Ei 'k230-amp|remoteproc|virtio|rpmsg'
ls -l /sys/class/remoteproc
cat /sys/class/remoteproc/remoteproc0/state
ls -l /sys/bus/rpmsg/devices /dev/rpmsg*
/root/amp/rpmsg-echo-test
```

Expected state is `attached`, an `rpmsg-raw` channel is present,
`/dev/rpmsg0` exists, and the test prints one `PASS` line. Press Enter on
ACM1 afterward; `RPMsg link/rx/tx` should show link 1 and nonzero equal echo
receive/transmit counts.

Then confirm the fallback transport still works while the RPMsg test is idle:

```sh
/root/amp/amp-shm-test
```

If no RPMsg device appears, collect the complete remoteproc/RPMsg dmesg lines
and the ACM1 status. A detached remoteproc with link zero points to host attach
or first-kick handling. Link one without an RPMsg device points to name-service
announcement or queue/cache visibility. A created device with echo timeout
points to endpoint dispatch, reverse notification, or descriptor-buffer cache
maintenance.

## Correctness and performance matrix

The RPMsg test accepts a deterministic payload size and loop count. Each reply
is compared byte-for-byte with the transmitted pattern; failures and timeouts
make the command fail even if some earlier exchanges succeeded.

```sh
# 1,000 exchanges at the default 21-byte payload
/root/amp/rpmsg-echo-test --loops 1000 --timeout-ms 1000

# Exercise the complete Linux/RPMsg-Lite payload range
/root/amp/rpmsg-echo-test --sweep --loops 1000 --timeout-ms 1000
```

`--sweep` tests 1, 16, 64, 128, 256, and 496 bytes (the 512-byte RPMsg
buffer minus its 16-byte header). Each line reports minimum, average, p50,
p95, p99, maximum, and successful messages per second. Repeat each matrix
after a cold boot, after a warm reboot, and while observing ACM1's firmware
`RPMsg link/rx/tx` counters. Do not run the mailbox-only test concurrently;
both protocols use the same doorbell.

The measured round trip includes Linux `write(2)`, the mailbox interrupt and
thread wakeup, virtqueue/RPMsg-Lite dispatch, the firmware echo callback, the
reverse notification, Linux `poll(2)` wakeup, and `read(2)`. It is therefore
the application-visible control-plane cost, not raw mailbox latency. Compare
the size sweep against a local loopback baseline if isolating syscall and
userspace timing. For sustained-load testing, run separate finite batches and
record timeout/mismatch counts, p95/p99, and rate; do not infer frame-payload
performance from RPMsg messages because large images belong in the Phase 5
shared-slot data plane.

## Bring-up debug results (2026-09-04)

### Root cause: descriptor read before the coherency fix could apply

`virtqueue_k230.c` overrode `virtqueue_get_available_buffer()` to invalidate and
reread the descriptor, because Linux writes `desc->len` before publishing and
upstream RPMsg-Lite assumes descriptors are coherent. The override was correct
in intent but applied too late:

    buffer = virtqueue_get_available_buffer_unfixed(vq, avail_idx, len);
    if (buffer == VQ_NULL)
            return VQ_NULL;              /* bails out here ... */
    desc = &vq->vq_ring.desc[*avail_idx];
    VQUEUE_INVALIDATE(desc, sizeof(*desc));   /* ... before this ever runs */

Upstream derives `buffer` from a possibly stale `desc->addr` and has already
advanced `vq_available_idx` by that point. On the first pass over the ring this
core still holds the zeroed descriptor lines it cached before Linux populated
them, so the stale `addr` reads as 0, `env_map_patova(0)` yields NULL, and the
wrapper returns "no buffer" *after* the ring slot was consumed. The message is
silently dropped and never retried.

Symptom: exactly 192 of the first 256 messages after every boot were lost, then
the link was perfect forever (each surviving access refreshes a 64-byte line
covering four descriptors, so the table self-heals after one pass).

Fix: implement the accessor directly -- invalidate `avail->idx`, the avail ring
slot, and the descriptor, and only advance `vq_available_idx` once the buffer
has actually been taken. See `virtqueue_k230.c`.

### Second defect: name service announced before Linux attached

Scanning the vrings unconditionally (rather than only on a mailbox edge) meant
`link_up` could be raised from uninitialized carveout memory before Linux
attached, burning the one-shot `rpmsg_ns_announce()` into a vring Linux had not
set up. `/dev/rpmsg0` then never appeared. Both the scan and the announce are
now gated on the virtio status byte in the resource table having DRIVER_OK set.

### Verified after the fix

All on hardware, cold boot each time:

- First traffic after boot: 1000 ping-pong and 300-message burst, zero loss.
  This is the case that used to lose 192 messages.
- Sweep 6 x 2000 messages, 1..496 B: zero timeouts, zero mismatches.
- Soak 50,000 x 496 B and 50,000 x 1 B: zero loss.
- Queue full: bursts of 64/1024/4096 with no reader draining: zero loss,
  back-pressure correct, `tx_failed == 0`.
- Firmware accounting exact: `rvq_avail_idx == rvq_consumed == fetch_rx ==
  rx_callbacks`, across a counter wrap.
- Latency 0.076..0.090 ms avg, p99 ~0.13 ms, ~11-13k msg/s.

### Measurements that were reported earlier and are wrong

U-Boot loads the payload from `/root/amp/metal-v-k230.bin` on the rootfs, not
from `/boot` (`default.env:8`). Firmware builds deployed to `/boot` were never
executed, so an earlier round of conclusions was drawn from an unchanged
firmware. Specifically:

- The "unconditional poll fixes a lost doorbell" result was not real. With the
  descriptor fix in place, an edge-gated build is equally lossless, so the
  doorbell was never being missed. The unconditional scan is retained only as
  defence in depth (`K230_RPMSG_EDGE_GATED_POLL=1` selects the edge-gated
  build); measured cost on the small core is within noise: memcpy 1134/1130 vs
  1128 MB/s, compute 2.748/2.733 vs 2.735 s.
- The "latency improved 0.210 -> 0.088 ms" claim was an artifact: 0.210 ms was
  a single cold sample, 0.088 ms is a warm average. There was no speedup.
- `used->flags` is still cleared at init because this core is the virtio device
  for both vrings and nothing else initializes that field, but it was not the
  cause of any observed failure.

### Peer restart

Not an issue: a small-core `reboot` resets the big core too (firmware counters
reset), so there is no stale-generation desync.

### Diagnostics

The big-core UART3 drops characters under sustained output, which made console
counters unreliable and cost real debugging time. The firmware now publishes a
counter block into the AMP shm page at `0x1d000200`; read it from Linux with
`rpmsg-echo-test --stats`. Prefer it over the UART for anything quantitative.

### Regression suite

`/root/amp/rpmsg-regression.sh` (installed by the package). Run
`--post-boot` as the first rpmsg traffic after a boot to cover the cold-start
case; plain for a full run including soak; `--quick` for a smoke check. Verified
to catch the original defect: `--burst --loops 300` reported `lost=192` before
the fix and `lost=0` after.
