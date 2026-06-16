# AMP plan: small-core Linux + big-core RVV

Plan for an asymmetric multi-processing setup on K230, where the small core
(CPU0, no RVV) runs Linux as the I/O + display master and the big core (CPU1,
with RVV) runs a payload that does the heavy image processing.

## End-to-end vision

```
       ov5647 (CSI)
            |
            v
+---------------------------+         +---------------------------+
|  CPU0  (small, no RVV)    |         |  CPU1  (big, with RVV)    |
|  Linux 6.6                |         |  payload       |
|                           |         |                           |
|  isp_media_server         |         |                           |
|  + vvcam (CSI/ISP)        |         |  rvv_process()            |
|                           |         |                           |
|  V4L2 dequeue /dev/video1 |  --IPI->|  process frame in shared  |
|  -> shared frame ring     |         |  DDR (in place / ping-pong)
|                           |<- IPI-- |                           |
|  DRM/KMS display (HDMI)   |         |                           |
+---------------------------+         +---------------------------+
            ^
            |
         HDMI out
```

Both cores share DDR. Frames live in a reserved (non-Linux) region. Cross-core
notification uses SBI IPI in the first cut, and may later switch to the K230
hardware mailbox.

## What's on the running board

Probes against the live image (built from branch `opt_linux_on_small_core`,
`k230_canmv_defconfig`) gave us:

### SBI / firmware

```
SBI specification v2.0 detected
SBI implementation ID=0x1 Version=0x10004   (OpenSBI 1.4)
SBI TIME / IPI / RFENCE / HSM / SUSP extensions detected
riscv: providing IPIs using SBI IPI extension
```

The **HSM extension is present**. `sbi_hart_start(hartid, start_addr_pa, opaque)`
is therefore the entrypoint we use to release CPU1 to a user payload.

### Per-CPU ISA strings (from DT)

| Core | DT ISA string |
|---|---|
| CPU0 (small, Linux runs here) | `rv64imafdc_zba_zbb_zbc_zbs_zicbom_zicbop_zicboz_svpbmt` |
| CPU1 (big) | `rv64imafdcv_zba_zbb_zbc_zbs_zicbom_zicbop_zicboz_svpbmt` |

Note the `v` flag only on CPU1.

### Why Linux logs `CPU1: failed to come online`

```
[ 0.005814] smp: Bringing up secondary CPUs ...
[10.218096] CPU1: failed to come online
[10.218183] smp: Brought up 1 node, 1 CPU
```

Linux RISC-V SMP requires homogeneous ISA across harts. With CPU1 advertising
the `v` extension that CPU0 does not have, the secondary bring-up either
rejects the mismatch or times out (10 s). CPU1's DT `status` is unset
(defaults to `okay`), so OpenSBI still sees it normally — the failure is a
*Linux* refusal, not a firmware issue.

**This is the state we want.** OpenSBI keeps CPU1 in HSM `STOPPED`, available
for `sbi_hart_start` from our launcher.

### Mailbox / IPC hardware

Only DT trace of a hardware mailbox is the clock gate
`91100000.sysctl_clock:mailbox_pclk_gate`. No mailbox driver bound, no DT
node. So:

- First cut: cross-core notification via **SBI IPI** (no kernel infra needed).
- Later: bring up the K230 hardware mailbox properly (DT node + tiny driver).

### Reserved memory

Only OpenSBI's own footprint is in `/reserved-memory`:

- `mmode_resv0@40000` : 128 KiB at 0x40000  (OpenSBI .text/.data)
- `mmode_resv1@0`     : 256 KiB at 0x0      (boot ROM / vectors)

Nothing carved out for AMP yet — we need to add one.

### Memory budget

`free -h` reports 468 MiB usable RAM (out of ~512 MiB DDR; rest is OpenSBI +
vvcam CMA + kernel reserved). During a live 1080p ISP preview, vvcam allocates
~5 MiB CMA at `0x1d040000`, `0x1e100000`, `0x1e500000`. The reserved region
we add for CPU1 must not overlap these — see "Open items" below.

### Toolchain

Kernel was built with **Xuantie-900 GCC 14.1.1** (`riscv64-unknown-linux-gnu-gcc
… linux-6.6.0 glibc gcc Toolchain V3.0.2 B-20250410 14.1.1 20240710`). This
toolchain supports `-march=rv64gcv` with RVV intrinsics, so the CPU1 payload
can be compiled with the same toolchain used for the kernel; only the
small-core artifacts (kernel image, userspace) stay RV64GC.

## Design

### Data plane

A reserved physically-contiguous region in DDR, invisible to Linux's page
allocator (`no-map`). Holds:

- CPU1 payload `.text` + `.rodata` + `.bss` + `.stack`,
- a small **control page** (4 KiB) — shared SPSC ring + status flags,
- N frame slots (NV12 1080p = ~3.1 MiB each; start with 4 slots = 12 MiB).

DT addition:

```
reserved-memory {
    cpu1_payload: cpu1_payload@<addr> {
        reg = <0x0 <addr> 0x0 0x04000000>;   /* 64 MiB; address TBD */
        no-map;
    };
};
```

Eventually, instead of carving a fixed region, we can pass V4L2 CMA dma-buf
physical addresses to CPU1 directly (zero copy with the existing ISP output).
The reserved region is still needed for CPU1's own `.text`/`.bss`/`.stack`,
but the frame buffers themselves can stay in CMA.

### Control plane

- **Boot CPU1:** Linux issues `sbi_hart_start(1, 0x<reserved_pa>, opaque_pa)`
  via inline `ecall`. CPU1 lands in S-mode at the entry address with
  `a0=hartid`, `a1=opaque`.
- **Linux -> CPU1 notify:** SBI IPI `sbi_send_ipi(hart_mask=0b10, base=0)`.
  Lands as a supervisor software interrupt on CPU1.
- **CPU1 -> Linux notify:** SBI IPI from CPU1 to hart 0. First iteration can
  skip this and have Linux poll a `done` flag in the control page; promote to
  IPI once the dataflow works.
- **Protocol:** a single-producer / single-consumer ring in the control page
  per direction. Each entry is `{ paddr, len, fourcc, w, h, stride, seq }`.

### Big-core runtime

The CPU1 payload could be `no_std` bare-metal code. C is the easiest starting
point for the heartbeat milestone; Rust is the intended target for the real
pipeline payload. The runtime is intentionally minimal:

- IPI handler: a supervisor software-interrupt vector that clears `sip.SSIP`
  and signals work to the main loop (via a shared flag in the control page).
- "Mailbox": MMIO accessors for the control page (volatile reads/writes over
  a known paddr).
- Frame ring: SPSC ring in the control page, drained by the main loop.
- RVV: `core::arch::riscv64::*` intrinsics, or `asm!` blocks with
  `.option arch, +v`. The auto-vectoriser is largely off in stable Rust on
  RISC-V; intrinsics are the practical path.
- No heap. Static buffers via `static_cell` / `heapless`.

Skeleton (Rust):

```rust
#![no_std]
#![no_main]

#[no_mangle]
extern "C" fn rust_main(_hartid: usize, opaque: usize) -> ! {
    let ctrl = unsafe { Mailbox::from_paddr(opaque) };
    setup_traps();
    loop {
        wait_for_ipi();                          // wfi until Linux signals
        while let Some(in_desc) = ctrl.ring_in.pop() {
            let out_desc = process_rvv(in_desc);
            ctrl.ring_out.push(out_desc);
            notify_linux();
        }
    }
}
```

Boot is straight bare-metal:
1. Linker script anchored at the reserved-region base.
2. Entry asm sets up `sp`, clears `.bss`, hands `a0`/`a1` to `rust_main`.
3. `rust_main` programs `stvec`, enables `sie.SSIE`, enters the main loop.

## DMA / data movement

K230 has several data-mover blocks on the SHRM (shared-memory) subsystem.
Their clock tree, summarised from `k230_clock_provider.dtsi`:

```
pll0_div4 -> shrm axi master clk -> { gsdma axi, nonai2d axi, pdma axi }
                                  -> shrm sram clk, shrm slave/apb clk
```

| Block | MMIO base | Purpose | Used by Linux on this branch? | Available to either core? |
|---|---|---|---|---|
| **GSDMA** | `0x80800000` | Mem-to-mem, channelised, linked-list descriptors | No - only the `gsdma_aclk_gate` clock gate is in DT; no DT node, no driver | Yes |
| **PDMA** | `0x80804000` | Peripheral <-> memory; 8 channels, 35 HW request lines, IRQ 203 | Yes - `canaan,k230-pdma`, consumed by I2S audio | Shared (PDMA driver owns it) |
| **nonai2d** | TBD | 2D blitter / image scaler | No - only `nonai2d_aclk_gate` | Yes |
| **decompress** | TBD | HW gzip / decompress | Used by u-boot SPL only; idle after boot | Yes |

### GSDMA is the right tool for AMP frame movement

GSDMA is a generic memory-to-memory DMA on a dedicated AXI master separate
from the CPUs. Linux does not bind any driver to it on this branch (no DT
node, only a clock gate). CPU1 bare-metal can take it over without
coordination with Linux.

The register layout is already documented and exercised by u-boot's SPL
gzip-decompression path:

- `buildroot-overlay/boot/uboot/u-boot-2022.10-overlay/arch/riscv/cpu/k230/platform.h:102`
  defines `GSDMA_CTRL_ADDR 0x80800000`.
- `buildroot-overlay/boot/uboot/u-boot-2022.10-overlay/arch/riscv/cpu/k230/unzip.c`
  drives it: channel enable/status at `+0x00`/`+0x08`, per-channel config at
  `+0x50`/`+0x58`, linked-list descriptor base at `+0x60`, start trigger at
  `+0x50` (`ch_ctl`). Eight channels, linked-list-transfer mode.

After u-boot hands off to Linux, GSDMA sits idle for the rest of the system
lifetime.

### Implications for the AMP design

- **Frame copy without burning CPU cycles.** When CPU1 needs to bring an
  input frame from CMA into the reserved processing area (or ping-pong to a
  result buffer), it programs a GSDMA descriptor and waits on the
  channel-done interrupt instead of CPU memcpy. DDR bandwidth, not core
  bandwidth, becomes the bound. A 1080p NV12 frame is ~3.1 MiB; moving that
  by CPU memcpy wastes cycles that could be doing RVV work.
- **No driver conflict with Linux.** Since there is no DT node and no
  driver, CPU1 just touches `0x80800000` directly.
- **Clock gate.** The `gsdma_aclk_gate` may be off by default at Linux boot
  if nothing claims it. The Linux-side launcher module needs to enable that
  gate (via `clk_get` / `clk_prepare_enable`) once before releasing CPU1.

### Other blocks

- **PDMA is taken by audio.** Driving it from CPU1 would race with the
  Linux PDMA driver. Avoid for AMP.
- **nonai2d** is interesting as a *processing* offload: a 2D blitter can do
  colour-space conversion, scaling, and rectangle copies in HW. Worth
  exploring once the basic AMP plumbing works (e.g. RVV does the per-pixel
  math, nonai2d does the scale-to-display step). MMIO base and TRM register
  doc still need to be found.
- **decompress** is unlikely to be useful for video pipelines but is
  available if some side-task needs HW gzip.

### Open items for DMA

- Confirm whether `gsdma_aclk_gate` is enabled at Linux boot (probe by
  reading the gate register from the live board).
- Locate the **nonai2d MMIO base** and TRM register doc.
- Pull the full GSDMA register description if available; u-boot's `unzip.c`
  uses only the subset relevant to the gzip-decompress flow.

## First-cut implementation order

1. **Quiet Linux about CPU1.** Add `maxcpus=1` to the u-boot bootargs, or set
   `cpu@1 { status = "disabled"; }` in DTS. Removes the 10 s boot delay and the
   noisy "failed to come online" message.
2. **Heartbeat milestone.** Tiny C/asm payload (no Rust yet) that writes an
   incrementing u64 into the control page in a busy loop. Linux-side kernel
   module:
   - loads the payload to the reserved region via `memremap()`,
   - calls `sbi_hart_start(1, reserved_pa, opaque_pa)`,
   - exposes `/dev/cpu1heart` whose `read()` returns the current counter.

   Proves SBI HSM works, memory coherency works, the address picked is sane.
3. **Frame ring milestone.** Replace heartbeat with a real ring. CPU1 payload
   just memcpys `in -> out`. Linux side: V4L2 DQBUF from `/dev/video1`, push
   `{paddr,...}` into the ring, wait for `done` (polled at first), wrap result
   paddr as a DMABUF, hand to DRM/KMS (same primitives as
   `buildroot-overlay/package/display/src/test.cpp`). HDMI should show the
   round-tripped frame.
4. **RVV processing.** Replace memcpy with grayscale + 3x3 box blur (small,
   easy to validate visually). Verifies RVV state init + register-save across
   IPI re-entry.
5. **Promote CPU1 -> Linux notify to SBI IPI** instead of polling. Adds a
   small kernel ISR; nothing in the data plane changes.
6. **Offload frame copies to GSDMA** on CPU1. Replace CPU memcpy in the
   frame-ring path with a GSDMA linked-list descriptor; CPU1 `wfi`'s on the
   channel-done interrupt. Frees CPU1 cycles for RVV work. See the "DMA /
   data movement" section for details.

## Open items

- **DDR map / address for the reserved region.** The placeholder above
  (`0x1c000000` was suggested in chat) collides with the current 48 MiB CMA
  range (`0x1d040000`+). Need to read the `/memory` node and the `cma`
  reserved-memory node from the live DT to pick a safe base. Easiest fix is
  to also shift CMA via `linux-canmv.fragment`.
- **OpenSBI HSM `hart_start` arbitrary `start_addr_pa` support.** Some vendor
  builds restrict it to PMP-allowed regions. The heartbeat milestone is the
  cheap way to find out — the SBI call returns an error code if blocked.
- **RVV register-state initialisation.** OpenSBI starts CPU1 in S-mode with
  the vector unit disabled (mstatus.VS=Off). CPU1 payload must set
  `sstatus.VS=Initial` and run a `vsetvli` before first vector op. Document
  the exact sequence when we write the payload.
- **K230 hardware mailbox.** We can postpone this. If we ever want
  lower-latency / payload-carrying notification than SBI IPI, the gate clock
  hint (`91100000.sysctl_clock:mailbox_pclk_gate`) is where to start digging
  for the MMIO block.
- **u-boot env access for cmdline edits.** Confirm `fw_setenv bootargs ...`
  works on the running image so we can iterate on `maxcpus=` and any reserved
  region overrides without rebuilding.

## References inside this repo

- `docs/small_core_linux.md` - state of the small-core Linux build (CSI/USB
  capture recipes, V4L2 device map, isp_media_server provenance).
- `buildroot-overlay/board/canaan/k230-soc/linux-canmv.fragment` - kernel
  fragment; CMA size + future reserved-memory edits land here.
- `buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99canaanboot`
  - boot-time vvcam modprobe + `isp_media_server` launch (unchanged for AMP).
- `buildroot-overlay/package/display/src/test.cpp` - reference for the
  small-core DMABUF + DRM/KMS pattern that the AMP display step reuses.
