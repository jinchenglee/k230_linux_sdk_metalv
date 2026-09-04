# Metal-V K230 AMP console payload

This package is the first big-core payload for the small-core Linux AMP
configuration. It reuses the K230 startup and UART3 approach from
[Metal-V](https://github.com/jinchenglee/metal-v) and the UART register sequence
from Canaan's K230 SDK.

The payload is deliberately freestanding and has no dependency on RT-Smart. It
is currently written in C and assembly so it can be built by the SDK toolchain
without requiring Zig. Its platform layer is intended to remain usable by a
later Rust `no_std`/Embassy runtime.

## Memory and console

- Load and entry address: `0x1c000000`
- Maximum firmware region: 16 MiB
- UART: UART3 at `0x91403000`, 50 MHz input, 115200 8N1
- Host port: CH342 channel B, normally `/dev/ttyACM1`

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
```

The full root filesystem installs both under `/root/amp/`.

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

## Current assumptions

The first payload relies on SPL/U-Boot having enabled the UART3 clock and board
pinmux, as the existing Metal-V and official SDK paths do. If no banner appears,
the next diagnostic step is to add explicit UART3 clock, reset, and pinmux setup
before changing the UART divisor or console mapping.
