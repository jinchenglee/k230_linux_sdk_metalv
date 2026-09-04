# Metal-V K230 AMP console payload

This package is the first big-core payload for the small-core Linux AMP
configuration. It reuses the K230 startup and UART3 approach from
[Metal-V](https://github.com/jinchenglee/metal-v) and the UART register sequence
from Canaan's K230 SDK.

The payload is deliberately freestanding and has no dependency on RT-Smart. It
is currently written in C and assembly so it can be built by the SDK toolchain
without requiring Zig. Its platform layer is intended to remain usable by a
later Rust `no_std`/Embassy runtime. It also services a polling shared-memory
test protocol while keeping the UART3 console responsive.

## Memory and console

- Load and entry address: `0x1c000000`
- Maximum firmware region: 16 MiB
- UART: UART3 at `0x91403000`, 50 MHz input, 115200 8N1
- Host port: CH342 channel B, normally `/dev/ttyACM1`
- Shared AMP region: `0x1d000000` (48 MiB reserved from Linux)

Linux reserves the firmware region and disables UART3 in the small-core DTB.
The payload must not be launched with the normal big-core Linux DTB.

## Build

From the SDK root:

```sh
make CONF=k230_canmv_small_core_defconfig metal_v_amp
```

Artifacts:

```text
output/k230_canmv_small_core_defconfig/build/metal_v_amp/metal-v-k230.bin
output/k230_canmv_small_core_defconfig/build/metal_v_amp/metal-v-k230.elf
output/k230_canmv_small_core_defconfig/build/metal_v_amp/amp-shm-test
```

The full root filesystem installs both under `/root/amp/`.

The package also installs two opt-in replacements for `/etc/init.d/rcS`:
`/root/amp/rcS.profile` timestamps every startup service, while
`/root/amp/rcS.fast` keeps the required media path synchronous and defers or
omits nonessential services. Neither is enabled by default; see
`docs/notes/k230_small_core_boot_time.md`.

## First manual launch

Stop at the small-core U-Boot prompt. Assuming the generated root filesystem is
partition 1 on `${mmc_boot_dev_num}`:

```text
ext4load mmc ${mmc_boot_dev_num}:1 0x1c000000 /metal-v-k230.bin
boot_baremetal 1 0x1c000000 ${filesize}
run blinux
```

Open the second console before releasing the core:

```sh
picocom -b 115200 /dev/serial/by-id/usb-1a86_USB_Dual_Serial_588D061066-if02
```

The expected banner begins with:

```text
Metal-V K230 AMP console
big-core UART3 is alive
```

Press Enter to print `mhartid`, `misa`, and the linked image extent again.
Capture these values during the first boot; do not infer architectural hart IDs
from Canaan's CPU0/CPU1 peripheral labels.

## Shared-memory smoke test

After booting Linux and the updated payload, run on ACM0:

```sh
/root/amp/amp-shm-test
```

The Linux utility maps the reserved region through `/dev/mem` with `O_SYNC`,
which gives this RISC-V kernel an uncached mapping. It tests 1-byte, cache-line,
page, 64 KiB, and 1 MiB boundary cases. Linux writes a patterned request; the
big core checks its CRC, XOR-transforms every byte, cleans its cache, and
publishes a response for Linux to validate. Metadata and publication sequences
use separate 64-byte cache lines, and each cache line has a single writer.

Pass a loop count for a longer run:

```sh
/root/amp/amp-shm-test 100
```

This polling transport is intentionally the cache-correctness proof before a
mailbox interrupt or rpmsg-lite is added. The big-core cache operations use the
same physical-address `dcache.cpa`, `dcache.ipa`, and `sync.is` sequence as K230
U-Boot. See `docs/notes/k230_amp_cache_maintenance.md` for the distinction and
the stale-data evidence from the 64 KiB hardware test.

The first exchange can take up to two seconds before the tester reports a
timeout. A successful default run prints nine `PASS` lines. Press Enter on ACM1
afterward to confirm that the firmware reports nine transactions and zero
errors.

Protocol ABI version 3 also reports big-core cycle counts for request cache
invalidation, request CRC, payload transform, response CRC, and response cache
cleaning. Deploy `metal-v-k230.bin` and `amp-shm-test` as a matched pair whenever
the ABI changes. These stage timings establish where mailbox and rpmsg-lite
work should focus before the XOR operation is replaced by ROI processing.

## Mailbox notification test

The bidirectional mailbox stage uses K230 IRQ 109 for Linux-to-big-core request
notification and the reverse DSP-to-CPU doorbell for completion. A minimal
Linux driver exposes completion through `/dev/k230-amp-mailbox`. Run:

```sh
/root/amp/amp-shm-test --mailbox
```

Mailbox-tagged requests are not serviced until the IRQ is observed, so a pass
cannot be produced by the fallback polling loop. Use `--mailbox-poll` to test
request IRQ plus response polling, or no option for pure polling. Deployment,
rollback, expected counters, and diagnosis are documented in
`docs/notes/k230_amp_mailbox.md`.

## Current assumptions

The first payload relies on SPL/U-Boot having enabled the UART3 clock and board
pinmux, as the existing Metal-V and official SDK paths do. If no banner appears,
the next diagnostic step is to add explicit UART3 clock, reset, and pinmux setup
before changing the UART divisor or console mapping.
