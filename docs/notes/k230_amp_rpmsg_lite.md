# K230 Linux/RPMsg-Lite bring-up

This note records the first standards-based transport between Linux on the
scalar C908 and the Metal-V bare-metal payload on the RVV C908. It builds on
the validated bidirectional mailbox and cache-maintenance layers documented in
`k230_amp_mailbox.md`.

This stage is source-complete and build-verified as of 2026-09-04. Hardware
enumeration and echo remain to be validated on the board.

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
