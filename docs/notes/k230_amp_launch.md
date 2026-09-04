# Launching K230 small-core Linux with a big-core bare-metal payload

This procedure launches the Metal-V RVV payload on the K230 big core and then
boots Linux on the small core. No SD-card reflash is required when only the
payload and Linux test utility changed.

## Consoles

- ACM0 is the small-core U-Boot and Linux console.
- ACM1 is UART3 owned by the big-core bare-metal payload.

Open ACM1 at 115200 8N1 before releasing the big core so the early banner is
captured. Keep ACM0 at the U-Boot prompt while loading the payload.

## Launch from the boot partition

Images built by `k230_canmv_small_core_defconfig` place
`/metal-v-k230.bin` in the first ext4 boot partition. On ACM0:

```text
ext4load mmc ${mmc_boot_dev_num}:1 0x1c000000 /metal-v-k230.bin
boot_baremetal 1 0x1c000000 ${filesize}
run blinux
```

`ext4load` sets `${filesize}`. Run `boot_baremetal` before another load command
can replace that value. In Canaan's command, `1` selects the big CPU subsystem;
it must not be interpreted as the architectural `mhartid`, because both K230
CPU subsystems report a local `mhartid` of zero.

The load address `0x1c000000` is the start of the 16 MiB firmware carveout
reserved by the small-core Linux device tree. Do not use these commands with a
normal big-core Linux image whose device tree does not reserve that region.

## Launch a payload copied into the Linux root filesystem

For a firmware-only update, copy the new firmware and tester while Linux is
running:

```sh
scp \
  output/k230_canmv_small_core_defconfig/images/metal-v-k230.bin \
  output/k230_canmv_small_core_defconfig/target/root/amp/amp-shm-test \
  root@10.111.41.234:/root/amp/
```

Reboot to U-Boot. The Linux root filesystem is normally ext4 partition 2. Check
the path before releasing the core:

```text
ext4ls mmc ${mmc_boot_dev_num}:2 /root/amp
ext4load mmc ${mmc_boot_dev_num}:2 0x1c000000 /root/amp/metal-v-k230.bin
boot_baremetal 1 0x1c000000 ${filesize}
run blinux
```

If the `ext4ls` check fails, inspect the partition numbers with U-Boot's
`part list mmc ${mmc_boot_dev_num}` rather than guessing. An alternative is to
mount the boot partition from Linux, replace `/metal-v-k230.bin` there, unmount
it cleanly, and retain the partition-1 launch commands.

The currently running bare-metal image executes from RAM, so overwriting its
file in the Linux root filesystem does not update or disturb that running
instance. A reboot and a new `boot_baremetal` command are required to execute
the replacement.

## Expected sequence

Immediately after `boot_baremetal`, ACM1 should show a banner ending in:

```text
Shared-memory polling service is ready at 0x1d000000.
metal-v>
```

Only then run `run blinux` on ACM0. After Linux reaches its shell, test the
shared-memory path:

```sh
/root/amp/amp-shm-test
/root/amp/amp-shm-test 100
```

Press Enter on ACM1 to display the big-core transaction and error counters.
Do not proceed to mailbox or RPMsg-Lite bring-up until the boundary and stress
tests complete with zero errors.
