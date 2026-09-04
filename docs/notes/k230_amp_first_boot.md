# K230 AMP first hardware boot

Date: 2026-09-04

## Simultaneous execution

The small-core U-Boot console launched the freestanding RVV payload with:

```text
ext4load mmc ${mmc_boot_dev_num}:1 0x1c000000 /metal-v-k230.bin
boot_baremetal 1 0x1c000000 ${filesize}
run blinux
```

UART3 on ACM1 reported:

```text
Metal-V K230 AMP console
big-core UART3 is alive
mhartid: 0x0000000000000000
misa:    0x8000000000b4112f
image:   0x000000001c000000 - 0x000000001c004430
```

After Linux completed booting, UART3 RX, echo, and Enter-triggered status output
remained operational. Small-core OpenSBI reported one scalar hart with
`Boot HART ID: 0`; Linux reported
`rv64imafdc_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt`.

Both CPU subsystems therefore use local `mhartid == 0`. The two independent
firmware instances ran simultaneously and Linux did not reset the big core or
claim UART3.

Linux reserved `0x1c000000-0x1fffffff`: 16 MiB for big-core firmware followed
by 48 MiB for shared AMP memory. MMZ registered separately, with CMA reserving
256 MiB at `0x0c000000`.

## Scalar ISP daemon regression

The current Canaan `isp_media_server` contains 1458 vector instructions and
faulted on the scalar core. The reported fault word `0xcc747057` is
`vsetivli zero,8,e8,mf2,ta,ma`.

The exact daemon inherited by `opt_linux_on_small_core_cherry-picked` is from
commit `25a6f59170640bbd9779f9a24167f1451fb2370f`, blob
`00d911824c59ff7df4168500360fc875476d669a`. Its ELF attribute advertises
RVV, but complete disassembly contains zero vector instructions. The first
successor with actual vector instructions is commit
`83fce3541008bf5f6de9a0d3e7b312cb50ba1e19`.

The branch did not rebuild this daemon. It rebuilt open supporting components;
the daemon remained a Canaan prebuilt. Its RPATH names Canaan's internal
`vvcam-back` build tree, while no corresponding daemon objects or archives
exist in this repository.

With the zero-vector daemon:

- OV5647 configured 1280x720 NV12 successfully.
- `camera_rtsp_demo` streamed H.264 from the board.
- VLC displayed `rtsp://10.111.41.234:8554/test`.
- MIPI and ISP released cleanly when the demo stopped.
- No new illegal-instruction fault occurred.

Repeated `AecExposureConvert NewGain` lines are noisy userspace AE diagnostics.
The old `SIGILL` remains in the persistent `dmesg` ring from the failed
current-daemon run. VLC VA-API/VDPAU messages are host-side fallback diagnostics
and did not prevent display.

## Remaining work

- Reflash and smoke-test an image that selects the zero-vector daemon
  automatically.
- Exercise scene switching and required mirror/flip controls.
- Run and stress the new polling shared-memory CRC test on hardware, then add
  sequence/ring-wrap coverage and cache-cost instrumentation.
- Implement mailbox notification after polling integrity is established, then
  integrate rpmsg-lite.
