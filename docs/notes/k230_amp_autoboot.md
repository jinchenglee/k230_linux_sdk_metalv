# Persistent automatic K230 AMP boot without reflashing

The small-core U-Boot build supports `saveenv` and persistent environment
storage. The following recipe loads the big-core Metal-V payload from the Linux
root filesystem, releases the big core, and then boots small-core Linux.

These commands assume the firmware was copied to
`/root/amp/metal-v-k230.bin` in ext4 partition 2. First verify it at the U-Boot
prompt on ACM0:

```text
ext4ls mmc ${mmc_boot_dev_num}:2 /root/amp
```

Define and persist the automatic boot sequence:

```text
setenv amp_addr 0x1c000000
setenv amp_load 'ext4load mmc ${mmc_boot_dev_num}:2 ${amp_addr} /root/amp/metal-v-k230.bin'
setenv amp_start 'boot_baremetal 1 ${amp_addr} ${filesize}'
setenv amp_boot 'if run amp_load; then run amp_start; run blinux; else echo AMP firmware load failed - Linux boot stopped; fi'
setenv bootcmd 'run amp_boot'
saveenv
```

Run it immediately or reset the board:

```text
run amp_boot
```

On later power cycles U-Boot executes `bootcmd` automatically. ACM1 should
display the Metal-V banner before Linux starts on ACM0. The sequence fails
closed: if the big-core file cannot be loaded, U-Boot does not silently boot
Linux alone.

To return to Linux-only automatic boot, interrupt U-Boot and run:

```text
setenv bootcmd 'run blinux'
saveenv
```

To test Linux-only boot once without changing the saved default:

```text
run blinux
```

Updating the payload does not require reflashing. Copy the replacement while
Linux is running, synchronize the filesystem, and reboot:

```sh
scp output/k230_canmv_small_core_defconfig/images/metal-v-k230.bin \
  root@10.111.41.234:/root/amp/metal-v-k230.bin
ssh root@10.111.41.234 sync
```

The payload currently executing on the big core remains in RAM until reset;
replacing its file only changes what the next automatic boot loads.

## Freshly flashed small-core image

A newly built and fully flashed `k230_canmv_small_core_defconfig` image is
self-contained. No interactive U-Boot setup is required:

- the generated U-Boot environment sets `bootcmd=run amp_boot`;
- `amp_boot` loads `/root/amp/metal-v-k230.bin` from rootfs partition 2,
  releases the big core, and boots small-core Linux;
- U-Boot supplies `rootwait`, with no fixed four-second root delay;
- ACM0 automatically logs in as root;
- the measured `rcS.fast` profile is active by default;
- Ethernet/DHCP and SSH start in the background; and
- ADB/MTP userspace is omitted from the lean image.

Build the image with:

```sh
make CONF=k230_canmv_small_core_defconfig
```

Flash
`output/k230_canmv_small_core_defconfig/images/sysimage-sdcard.img`. A complete
image flash overwrites the saved U-Boot environment, so custom values from an
older card do not need to be cleared.

After first boot, verify both consoles and the defaults:

```sh
cat /proc/cmdline
dmesg | grep FASTBOOT
cat /tmp/network-ssh-start.log
/root/amp/amp-shm-test
```

On ACM1, verify the Metal-V banner. At an interrupted U-Boot prompt,
`printenv bootcmd amp_boot amp_load` should show the generated automatic
sequence.

## Selecting startup services

The root filesystem keeps three startup profiles:

- `fast`: the default measured profile; required media setup is synchronous,
  Ethernet and SSH are deferred, and optional daemons are skipped.
- `full`: normal Buildroot service startup, including Wi-Fi, Bluetooth/D-Bus,
  time services, cron, and telnet.
- `profile`: normal startup with per-service `BOOTTRACE` timestamps.

Select a profile for the next and subsequent boots:

```sh
amp-boot-profile fast
amp-boot-profile full
amp-boot-profile profile
reboot
```

Only run one selection command before reboot. The command copies the chosen
implementation to `/etc/init.d/rcS`; it does not modify U-Boot or the other
stored profiles.

The lean image does not contain `adbd` or `umtprd`. To restore those two
services as well, enable `BR2_PACKAGE_VVCAM_USB_GADGET=y` in
`buildroot-overlay/configs/k230_canmv_small_core_defconfig`, rebuild from a
clean output tree, and flash the resulting image. That option selects Android
tools and uMTP responder without making them ISP dependencies. Then select the
`full` profile to start the USB gadget during boot.

To retain all services but disable automatic big-core launch, interrupt U-Boot
and persist:

```text
setenv bootcmd 'run blinux'
saveenv
```

Restore generated automatic dual-core boot with:

```text
setenv bootcmd 'run amp_boot'
saveenv
```
