# K230 small-core Linux boot-time measurement

The current development root filesystem reaches its login prompt much later
than the desired ten-second target. Do not begin by removing multimedia,
camera, display, or accelerator support: those facilities are required by the
vision applications, and package size alone does not identify synchronous boot
delays.

The `metal_v_amp` package installs an opt-in `/root/amp/rcS.profile`. It follows
the normal Buildroot `rcS` execution rules and prints the uptime before and
after every `/etc/init.d/S??*` script. Messages appear on ACM0 and in the kernel
log with the `BOOTTRACE` prefix.

## Root console autologin

The small-core defconfig sets the serial getty options to invoke
`/sbin/autologin-root`, which executes BusyBox `login -f root`. This affects the
local ACM0 console only; it does not relax SSH authentication.

For an existing card, deploy it without reflashing:

```sh
scp buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/sbin/autologin-root \
  root@10.111.41.234:/sbin/autologin-root
```

Then run on ACM0:

```sh
chmod 755 /sbin/autologin-root
cp -p /etc/inittab /root/amp/inittab.before-autologin
sed -i 's#^console::respawn:.*#console::respawn:/sbin/getty -L -n -l /sbin/autologin-root console 0 vt100#' /etc/inittab
sync
reboot
```

To restore the login prompt:

```sh
cp /root/amp/inittab.before-autologin /etc/inittab
sync
reboot
```

Physical access to ACM0 now grants a root shell, so this setting is intended
for the development appliance rather than an untrusted-access deployment.

## Enable profiling without reflashing

Back up the active script, install the profiler, and reboot:

```sh
cp -p /etc/init.d/rcS /root/amp/rcS.original
cp /root/amp/rcS.profile /etc/init.d/rcS
chmod 755 /etc/init.d/rcS
sync
reboot
```

After boot, collect the timings:

```sh
dmesg | grep BOOTTRACE
```

Restore normal startup after collecting a representative boot:

```sh
cp /root/amp/rcS.original /etc/init.d/rcS
chmod 755 /etc/init.d/rcS
sync
```

If the current card predates the packaged profiler, copy just that file from
the host; no image flash is needed:

```sh
scp buildroot-overlay/package/metal_v_amp/src/rcS.profile \
  root@10.111.41.234:/root/amp/rcS.profile
```

## Measured baseline

The first traced boot reached `rcS` at 7.46 seconds and completed it at 24.35
seconds. The largest synchronous costs were:

| Startup work | Duration |
| --- | ---: |
| Wi-Fi module load | 10.46 s |
| SNTP request | 2.28 s |
| Ethernet/DHCP | 1.42 s |
| ISP/media startup | 0.76 s |
| mdev | 0.62 s |
| SSH server | 0.33 s |

The U-Boot fallback command line also contains `rootdelay=4`. This accounts for
four seconds before `rcS`: Linux sleeps even if the SD root device is already
ready. The SDK source now uses `rootwait`, which blocks only when necessary.

Test that change on the existing card before rebuilding U-Boot. First use Linux
to confirm whether this board's root device is `mmcblk0p2` or `mmcblk1p2`:

```sh
cat /proc/cmdline
```

Then interrupt U-Boot and set the matching command line. For `mmcblk0p2`:

```text
setenv bootargs 'root=/dev/mmcblk0p2 loglevel=8 rw rootwait rootfstype=ext4 console=ttyS0,115200 earlycon=sbi'
run amp_boot
```

Use `mmcblk1p2` instead if that is what the current Linux command line reports.
After a successful test, interrupt the next boot, repeat the `setenv bootargs`
command, verify it with `printenv bootargs`, and run `saveenv` before leaving
that same U-Boot session. An unsaved value is lost as soon as Linux boots. To
return to the image's built-in command line, run `setenv bootargs` followed by
`saveenv`.

## Fast profile without reflashing

The packaged `rcS.fast` keeps device management, logging, sysctl, ISP/media,
the AprilTag key handler, and version reporting synchronous. Ethernet/DHCP and
SSH run as a background chain. These installed services are skipped:

- D-Bus and Bluetooth
- Wi-Fi module loading
- ADB/MTP USB gadget
- SNTP and NTP
- cron and telnet

Hardware validation on the K230 reached `FASTBOOT ... rcS complete` at 5.32
seconds with `rootwait`; automatic big-core Metal-V and small-core Linux boot
remained functional. This is a 19.03-second reduction from the traced 24.35
second baseline. Ethernet and SSH become available later because DHCP is
intentionally outside the console critical path. If no DHCP server responds,
BusyBox `udhcpc` forks into the background while `sshd` begins listening.

On the minimal BusyBox image, check for unwanted ADB/MTP processes with
`ps | grep -E '[a]dbd|[u]mtprd'`; `pgrep` is not installed.

Copy and enable the fast profile:

```sh
scp buildroot-overlay/package/metal_v_amp/src/rcS.fast \
  root@10.111.41.234:/root/amp/rcS.fast
```

On ACM0:

```sh
[ -e /root/amp/rcS.original ] || cp -p /etc/init.d/rcS /root/amp/rcS.original
cp /root/amp/rcS.fast /etc/init.d/rcS
chmod 755 /etc/init.d/rcS
sync
reboot
```

Check asynchronous networking if SSH does not appear:

```sh
cat /tmp/network-ssh-start.log
ip address show eth0
```

Restore the original startup sequence with:

```sh
cp /root/amp/rcS.original /etc/init.d/rcS
chmod 755 /etc/init.d/rcS
sync
```

The small-core defconfig also disables VVCAM's optional USB-gadget support, so a
newly generated final image omits `adbd` and `umtprd`. They are not ISP
dependencies. Existing cards retain the binaries, but the fast profile does not
start them, so they consume no process address space or runtime memory.

Package removal reduces image size, but only removing or deferring work on the
synchronous `rcS` path improves time-to-application. Validate camera capture,
HDMI, inference, AMP service, Ethernet, and SSH before making further kernel or
package reductions.
