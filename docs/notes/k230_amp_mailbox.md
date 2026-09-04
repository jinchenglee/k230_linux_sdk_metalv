# K230 AMP mailbox notification bring-up

This note records the first interrupt-backed notification layer between
small-core Linux and the big-core Metal-V payload. It deliberately keeps the
validated shared-memory ABI and cache-maintenance path unchanged. RPMsg-Lite
will reuse the same mailbox directions after this lower-level test passes.

## Hardware contract

The implementation follows Canaan's official IPCM platform code in
`/mnt/sda_500gb/git_repo/k230_sdk/src/common/cdk/kernel/ipcm/arch/k230/`.

| Item | Value |
| --- | ---: |
| Mailbox controller | `0x91104000` |
| Linux/small core to big core set register | `CPU2DSP_INT_SET0`, offset `0x04` |
| Big-core acknowledgement | `CPU2DSP_INT_CLEAR0`, offset `0x08` |
| Big core to Linux/small core set register | `DSP2CPU_INT_SET0`, offset `0x18` |
| Linux/small-core acknowledgement | `DSP2CPU_INT_CLEAR0`, offset `0x1c` |
| Big-core PLIC source | 109 |
| Big-core PLIC base | `0x0f00000000` |

The official Linux/RT-Smart pair writes zero to a SET or CLEAR register to
operate mailbox channel zero. The enable value is bit 16 (raw enable) plus bit
0 (interrupt enable), or `0x00010001`.

`boot_baremetal` starts the big core directly in M-mode. Metal-V therefore
selects PLIC M context zero by clearing the K230 `S_PER` register at
`0x0f001ffffc`, gives source 109 priority one, enables it in context zero,
sets a zero threshold, and enables `mie.MEIE` and `mstatus.MIE`. This differs
from official RT-Smart, which runs in S-mode and uses PLIC context one.

## Bidirectional behavior

Shared-memory ABI version 3 adds `AMP_SHM_REQUEST_F_MAILBOX` to each request.
A mailbox-tagged request is not processed until the bare-metal IRQ handler has
claimed PLIC source 109 and cleared `CPU2DSP_INT_CLEAR0`. This prevents the
publication polling loop from creating a false-positive mailbox test.

The completed notification path is:

1. Linux publishes the request and rings `CPU2DSP_INT_SET0`.
2. The big-core interrupt handler acknowledges the mailbox and records a
   pending event.
3. The main loop performs the validated invalidate/CRC/XOR/clean flow.
4. The big core cleans the response publication, executes an I/O fence, and
   rings `DSP2CPU_INT_SET0`.
5. The small-core Linux driver acknowledges the interrupt, increments its
   completion counter, and wakes `poll(2)`/`read(2)` waiters on
   `/dev/k230-amp-mailbox`.
6. The test client executes an acquire fence and confirms that the matching
   response publication is visible before reading response data.

The `k230_amp_mailbox` out-of-tree platform/misc driver binds to the
`canaan,k230-amp-mailbox` DT node. `S29k230_amp_mailbox` loads it before the
ISP service. The no-option test still polls both ways. `--mailbox-poll` rings
the request doorbell through `/dev/mem` but polls the response. These fallbacks
separate driver/DT failures from shared-memory and big-core IRQ failures.

This is intentionally a one-outstanding-request protocol. Hardware doorbells
can coalesce and the completion counter is not a queued message transport;
RPMsg-Lite supplies that next layer.

## Build and artifact audit

Build all four matched pieces from the SDK root:

```sh
make CONF=k230_canmv_small_core_defconfig metal_v_amp-dirclean
make CONF=k230_canmv_small_core_defconfig metal_v_amp
make CONF=k230_canmv_small_core_defconfig linux-dirclean
make CONF=k230_canmv_small_core_defconfig linux
make CONF=k230_canmv_small_core_defconfig k230_amp_mailbox-dirclean
make CONF=k230_canmv_small_core_defconfig k230_amp_mailbox
```

Artifacts:

```text
output/k230_canmv_small_core_defconfig/build/metal_v_amp/metal-v-k230.bin
output/k230_canmv_small_core_defconfig/build/metal_v_amp/amp-shm-test
output/k230_canmv_small_core_defconfig/images/k230-canmv.dtb
output/k230_canmv_small_core_defconfig/target/lib/modules/6.6.36/updates/k230_amp_mailbox.ko
output/k230_canmv_small_core_defconfig/target/etc/init.d/S29k230_amp_mailbox
```

The Linux test must remain scalar. Its `readelf -A` architecture must not
contain `v`; the 2026-09-04 build reports `rv64imafdc`. Decompile the DTB
and confirm `mailbox@91104000` has interrupt `109` before deployment.

## Deploy to an existing card without reflashing

Copy the five artifacts to `/root/amp` on the board. The module and init script
may additionally be copied directly to their final rootfs paths. Firmware and
DTB still need an ACM0 install because U-Boot reads partition 1 before Linux
can mount it.

For `root=/dev/mmcblk1p2`, install on ACM0 as follows:

```sh
chmod 755 /root/amp/amp-shm-test /etc/init.d/S29k230_amp_mailbox
mkdir -p /mnt/amp-boot
mount /dev/mmcblk1p1 /mnt/amp-boot
cp -p /mnt/amp-boot/metal-v-k230.bin /mnt/amp-boot/metal-v-k230.bin.stage1
cp -p /mnt/amp-boot/k230-canmv.dtb /mnt/amp-boot/k230-canmv.dtb.stage1
cp /root/amp/metal-v-k230.bin /mnt/amp-boot/metal-v-k230.bin
cp /root/amp/k230-canmv.dtb /mnt/amp-boot/k230-canmv.dtb
sync
umount /mnt/amp-boot
reboot
```

Use `mmcblk0p1` if the command line names `mmcblk0p2`. The saved `blinux`
command loads `/k.dtb`, which the generated boot filesystem links to
`k230-canmv.dtb`. Merely replacing the rootfs copies does not update U-Boot's
firmware or DTB.

Rollback by restoring both `.stage1` files, removing
`S29k230_amp_mailbox` and `k230_amp_mailbox.ko`, and rebooting. The startup
script uses the module's exact path because the lean image omits `depmod`.

## Hardware test

After both consoles boot, run on ACM0:

```sh
dmesg | grep k230-amp-mailbox
ls -l /dev/k230-amp-mailbox
/root/amp/amp-shm-test --mailbox 100
/root/amp/amp-shm-test --mailbox-poll
/root/amp/amp-shm-test
```

The first run should end with:

```text
AMP shared-memory bidirectional-mailbox test passed: 100 loops, 900 exchanges
Linux completion IRQ count: 900
```

Press Enter on ACM1. After a fresh payload and only the 100-loop run, expect
`transactions=0x384`, `mailbox IRQs=0x384`, and zero errors, poll fallbacks,
and unhandled IRQs. The two fallback runs then each add nine transactions; only
the pure-poll run adds nine polling-fallback transactions.

If `--mailbox` times out:

- zero big-core mailbox IRQs points to request direction, mailbox enable, or
  big-core PLIC routing;
- increased big-core IRQs and transactions with no Linux completion points to
  the reverse SET register, DT IRQ routing, or Linux driver;
- `completion before response publication` points to response cache/order
  handling; and
- CRC/data errors point to ABI pairing or cache visibility.

Do not proceed to RPMsg-Lite until all three modes and the 100-loop
bidirectional integrity run pass.

## Hardware validation

Validated on 2026-09-04 on the K230 at `10.111.41.49`:

- bidirectional mailbox: 100 loops, 900 exchanges, 900 Linux completion IRQs;
- request-mailbox/response-poll fallback: 9 of 9 exchanges; and
- pure-poll fallback: 9 of 9 exchanges.

All modes completed without timeout, publication-order failure, CRC error, or
data mismatch. Representative 64 KiB round trips were 3.963 ms
(bidirectional), 3.936 ms (request IRQ/response poll), and 3.929 ms (pure
poll). Representative 1 MiB results were 62.800, 62.764, and 62.755 ms.
Their close agreement confirms that large-message time is dominated by the
deliberately diagnostic CRC/byte-transform passes rather than mailbox
notification. Measure zero-payload notification latency separately before
using this test to choose RPMsg-Lite queue or payload sizing.

## Notification latency profiling

After the integrity tests pass, isolate transport overhead with a zero-payload
run:

```sh
/root/amp/amp-shm-test --latency
/root/amp/amp-shm-test --latency 100000
```

The default is 10,000 measured samples after 100 unreported warm-up exchanges;
the accepted maximum is 1,000,000. Output reports minimum, mean, p50, p95, p99,
and maximum in microseconds. The interval starts immediately before the
userspace mailbox-driver `write(2)` and ends after Linux receives the reverse
IRQ and `poll(2)` plus `read(2)` complete. It therefore measures the complete
request/completion notification path plus the fixed zero-payload protocol work,
not raw mailbox hardware latency. Keep the system workload and logging state in
the benchmark record because scheduler interference primarily affects the tail.

## Profiling results

Idle-system measurements on 2026-09-04:

| Samples | Minimum | p50 | p95 | p99 | Maximum | Mean |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 7.592 us | 8.000 us | 15.556 us | 32.630 us | 217.926 us | 9.640 us |
| 100,000 | 7.777 us | 8.185 us | 27.186 us | 32.741 us | 178.185 us | 10.002 us |

The stable approximately 8 us median and 33 us p99 show that descriptor-sized
mailbox notification is negligible relative to millisecond-scale detection
work. The p95 movement with a stable p99 indicates a secondary latency band,
most likely Linux scheduling, timer, or background-service interference; this
is an inference rather than a component-level measurement. Repeat the same
benchmark under camera/display load and report p50/p95/p99/max before finalizing
application queue depth or latency budgets. The idle result is sufficient to
start RPMsg-Lite integration.
