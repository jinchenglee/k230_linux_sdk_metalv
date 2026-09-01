# K230 AMP: configurable payload slots

Status: goal statement + design direction. Not yet a plan; no implementation
tasks are sequenced here beyond the staging outline.

## What we want to achieve

**Both K230 cores should be independently usable, with the software that runs
on each core selected by configuration rather than baked into the boot path.**

Concretely, "what runs on core N" becomes a build-time choice per core:

- Linux
- RT-Smart
- a bare-metal / `no_std` payload (the RVV image-processing engine is the
  motivating case)
- nothing

Any combination should be selectable, with the two slots configured
independently. Linux is a convenient starting point because we already have it
working, not because it is privileged in the design.

This is deliberately more general than `docs/amp_bigcore_rvv_plan.md`, which
describes exactly one assignment (Linux on the small core, RVV payload on the
big core). That plan becomes one instance of a slot configuration.

### Non-goals

- **SMP Linux across both cores.** Not pursued. Beyond needing PLIC/CLINT
  routing that does not exist in the current DT, it depends on inter-core
  cache coherency that we have not established (see Open questions). AMP is
  what the silicon is built for -- see the hardware evidence below.
- Dynamic, runtime re-selection of a slot's OS. Build-time selection first;
  runtime payload swap is a later question and depends on whether a core can
  be cleanly re-reset.

## Why AMP is the right shape (evidence)

The SoC is designed for two OSes sharing peripherals:

- **Hardware hardlock unit.** Consumed as a DT property by multiple nodes in
  `arch/riscv/boot/dts/canaan/k230.dtsi`: `sysctl_power` claims locks
  `<3>,<4>,<5>`, `tsensor` claims `<2>`, others `<6>`,`<7>`. These exist so
  two independent OSes can arbitrate shared registers.
- **Hardware mailbox.** `mailbox_pclk_gate` in `k230_clock_provider.dtsi`.
  No DT node or driver bound yet.
- **Per-core reset vectors and reset control**, already exercised by U-Boot
  (see next section).
- Canaan's own product is AMP: Linux plus RT-Smart.

## Verified hardware / current state

Established by inspection of this tree and probes on a running board.

### Core topology and reset

- `u-boot .../board/canaan/common/k230_img.c:170` -- `//小核是0，大核是1;`
  ("little core is 0, big core is 1").
- `k230_img.c:144` `de_reset_big_core()` -- release sequence for core 1:
  write entry address to `cpu1_hart_rstvec` (`0x91102104`), then pulse
  `0x9110100c` with `0x10001000` (clear done) / `0x10001` (set reset) /
  `0x10000` (clear reset). Bit 16 is a write-enable mask for bit 0 (reset);
  bit 28 masks bit 12 (done).
- `arch/riscv/cpu/k230/cpu.c:127` `k230_boot_baremetal` -- **already a
  symmetric per-core launcher.** Takes `boot_cpu` as an argument and handles
  both register pairs: core 0 via `0x91102100`/`0x91101004`, core 1 via
  `0x91102104`/`0x9110100c`. This is the primitive the slot model builds on.
- `cpu.c:83` -- `writel(0x80199805, 0x91100004); //big core 1.6G`.

Register reads from a running `k230_canmv_defconfig` board:

| Addr | Read | Note |
|---|---|---|
| `0x9110100c` core1 reset ctl | `0x00013000` | bit0=0 |
| `0x91101004` core0 reset ctl | `0x00007000` | bit0=0 |
| `0x91102104` cpu1_hart_rstvec | `0x00000000` | never programmed |
| `0x91100004` big-core clk cfg | `0x00099005` | U-Boot's `0x80199805` minus self-clearing bits 31/20/11 -- the 1.6 GHz config committed |

The reset bits are **pulses**, so their resting value of 0 does not
distinguish "released" from "never touched". These reads are therefore not
sufficient to identify which physical core is running Linux.

### Which core runs Linux on `dev`

`k230_canmv_defconfig` and `k230_canmv_01studio_defconfig` are equivalent on
every axis that matters here:

```
BR2_RISCV_ISA_RVV=y
BR2_TARGET_OPTIMIZATION="-mcpu=c908v -mtune=c908 -mrvv-v0p10-compatible -mrvv-auto-vectorize"
BR2_PACKAGE_LIBNNCASE=y / BR2_PACKAGE_FACE_DETECT=y / BR2_PACKAGE_AI2D_KPU=y
```

**K230 has exactly one RVV-capable core: the big core.** The small core has
no vector unit. All userspace in these configs is vector-compiled and the
vendor AI blobs are RVV-only, so neither config can run on the small core.
RVV applications execute on the board. **Both configs run Linux on the big
core**, and this is now a hardware-grounded conclusion, not an inference from
a device tree.

### Core selection already exists: `CONFIG_LINUX_RUN_CORE_ID`

**The payload-slot selector exists in embryo today.** U-Boot SPL always starts
on the small core (the boot ROM releases it), and a single compile-time
constant decides where U-Boot proper -- and therefore Linux -- ends up:

`board/canaan/common/sdk_autoconf.h`:
```c
#define CONFIG_LINUX_RUN_CORE_ID 1     /* dev: Linux on the big core */
```

`board/canaan/common/k230_img.c:270` in `k230_boot_uboot_uimage()`:
```c
#if defined(CONFIG_LINUX_RUN_CORE_ID) && (CONFIG_LINUX_RUN_CORE_ID == 1)
    de_reset_big_core(image_get_load(pUh));   /* hand U-Boot to the big core */
    while (1) { asm volatile("wfi"); }        /* and park the small core */
#endif
    uboot(0, (void*)OPENSBI_DTB_ADDR);        /* CORE_ID==0: stay on small core */
```

So:

- `CONFIG_LINUX_RUN_CORE_ID = 1` -- SPL releases the big core to run U-Boot
  proper, then parks itself in `wfi` forever. Linux runs on the **big** core.
  This is `dev`.
- `CONFIG_LINUX_RUN_CORE_ID = 0` -- the block compiles out; SPL falls through
  to `uboot(0, dtb)` and everything continues on the **small** core. This is
  what commit `1e02ce1` ("Try to bringup linux on small core only",
  2025-10-14) selects on the `opt_linux_on_small_core*` branches.

`k230_spl.c:162` keys off the same constant (`quick_boot()` returns 0 when
CORE_ID==1, commented `uboot运行在大核core1`).

Note what this costs today: in the `CORE_ID = 1` configuration the small core
is **parked in `wfi` forever**, not available for a second payload. Generalizing
this constant into a per-slot selection -- and replacing the `wfi` park with a
second payload launch -- is a concrete, small first step toward Model B.

### Hart ID space: partially known, and it gates Model A

- **Big core: architectural hart 0.** OpenSBI reports `Boot HART ID : 0` with
  a vector-capable ISA when running there. That comes from `mhartid` in
  M-mode, independent of any device tree. So `dev`'s
  `cpu@0 { reg = <0>; ... "rv64imafdcv..." }` is **correct**, and
  `kernel(0, dtb)` passes a truthful hartid.
- **Small core: mhartid NOT established.** A big-core boot log showing hart 0
  says nothing about what the small core reports when *it* is the boot core.
  The SoC labels ("CPU0" = small, "CPU1" = big, used by `k230_img.c:170` and
  by the reset/clock register names) are a *subsystem* numbering and must not
  be assumed to equal architectural hart IDs.

**This is now the question that decides whether Model A is even possible.**
K230's two CPU subsystems are not a conventional SMP pair, and each may
present itself as hart 0 when independently booted. If both cores report
`mhartid == 0`:

- a single OpenSBI instance cannot manage both -- domains assume one coherent
  hart-ID space, and `sbi_hsm_device.hart_start(hartid, ...)` cannot address a
  core it cannot name;
- Model A as described collapses, and the realistic target becomes **two
  independent firmware instances**, one per core -- architecturally much
  closer to Model B, with IPC over mailbox/hardlock rather than SBI.

If instead the small core reports `mhartid == 1`, Model A stands as written,
and the `sbi_hsm_device` needs a hartid -> SoC-label mapping (arch hart 0 =
SoC "CPU1" = registers `0x91102104`/`0x9110100c`; small core = SoC "CPU0" =
`0x91102100`/`0x91101004`) which is **inverted** and must not be coded as
identity.

### PMP budget is already partially spent

Model A enforces domain isolation with PMP, and this platform does not start
with a clean PMP slate. U-Boot's `harts_early_init()`
(`arch/riscv/cpu/k230/cpu.c`) locks `pmpaddr0`/`pmpaddr1`/`pmpcfg0` to protect
a burntool-only region. Canaan's OpenSBI overlay exists largely to cope with
this: relative to pristine 1.4 it adds `sbi_hart_pmp_reserved()` and offsets
every PMP programming loop by that reserved count
(`lib/sbi/sbi_hart.c`, `pmp_idx = sbi_hart_pmp_reserved(scratch)` in place of
`pmp_idx = 0`), plus `hart_pmp_probe_reserved()` to discover which entries are
already locked, and a `Boot HART PMP Reserved` banner line.

So the domain-region budget is `Boot HART PMP Count` minus
`Boot HART PMP Reserved`, both printed in the banner. Worth reading before
designing the region layout -- a two-domain split with separate DDR windows,
a shared IPC window, and per-domain device regions can consume entries
quickly.

Note also that this overlay is confined to PMP handling: **it does not touch
hartid**, so the banner's `Boot HART ID` is unmodified upstream behavior
printing raw `mhartid`, and can be trusted.

**How to settle it:** boot a `CONFIG_LINUX_RUN_CORE_ID = 0` image and read the
OpenSBI banner. `Boot HART ID` gives the small core's `mhartid`, and
`Boot HART Base ISA` / `Boot HART ISA Extensions` (`sbi_init.c:174,176`) are
read from `misa` -- hardware truth, DT-independent -- so they also confirm
which core it is. `Boot HART PMP Count` in the same block tells us how much
PMP is available for domain isolation.

### Hart numbering: truthful, but only because one hart is described

`k230_boot_linux_uimage` (`k230_img.c:230`) calls `kernel(0, dtb)` -- it does
not release a core, it jumps on the core it is already running on, and asserts
hartid 0. Linux then takes `a0` as truth:

```
arch/riscv/kernel/smp.c:45   cpuid_to_hartid_map(0) = boot_cpu_hartid;
arch/riscv/kernel/cpu.c:285  seq_printf(m, "hart\t\t: %lu\n", cpuid_to_hartid_map(cpu_id));
```

So `/proc/cpuinfo`'s `hart` field echoes U-Boot's assertion and cannot
identify the physical hart. `mhartid` is M-mode-only and unreadable from
Linux. This is harmless on one hart and becomes a correctness problem the
moment a second hart and firmware domains are introduced, because OpenSBI
addresses harts by physical ID.

**Confirmed benign on `dev`.** OpenSBI reports `Boot HART ID : 0` with a
vector-capable ISA, so the boot hart really is 0 and `kernel(0, dtb)` is
truthful. The hazard is not a present-day bug; it is that `a0` is a hardcoded
constant rather than a derived value. The moment a slot is meant to boot on
the small core, `kernel(0, ...)` becomes actively wrong and must be replaced
by the real hartid. Under Model A this disappears entirely -- OpenSBI hands
each domain its own `next_addr` with the correct hartid.

## The slot model

A slot descriptor is the unit of configuration. Per slot:

- entry address and memory window (`reserved-memory`, `no-map`)
- privilege mode: S-mode payload needing SBI, or M-mode bare-metal
- device set -- each device belongs to exactly one slot, or is explicitly
  shared under a hardlock
- console UART (five available: `serial@91400000`..`91404000`, so a dedicated
  console per slot is free)
- DTB (for slots running an OS that wants one)
- ISA string and core clock -- the only genuinely core-specific items

Things that are currently *role*-specific in `amp_bigcore_rvv_plan.md` and
must be lifted to be slot-generic:

1. **The launcher.** That plan has Linux calling `sbi_hart_start()` from a
   kernel module, which only works when Linux is the already-running slot.
   The launcher belongs in firmware.
2. **Reserving the second core.** That plan relies on Linux *refusing* a hart
   with a mismatched ISA string (`CPU1: failed to come online`). That is an
   accident, not a mechanism. Use `status = "disabled"` / `maxcpus=1`, and
   under domains make hart ownership an explicit declaration.
3. **RVV state init.** `sstatus.VS=Initial` plus a `vsetvli` before the first
   vector op is required of *any* S-mode payload landing on a vector core.
   Belongs in shared slot-entry boilerplate, not in one payload.

## Staging: Model B then Model A

**Model B -- independent payloads, U-Boot as launcher.** Each slot gets a
self-contained image; U-Boot loads both and releases the second core via the
existing `k230_boot_baremetal` path. No firmware surgery. This is how Canaan
already ships, and steps 1-5 of `amp_bigcore_rvv_plan.md` apply directly with
roles swapped. Cheap, and it validates the reset sequence, the memory split,
and the console split. No enforced isolation.

**Model A -- one OpenSBI, two domains.** The target. OpenSBI 1.4 in
`buildroot-overlay/boot/opensbi/opensbi-1.4-overlay/` already ships
`lib/sbi/sbi_domain.c` and `lib/utils/fdt/fdt_domain.c`, and `k230.dtsi`
already carries a `chosen/opensbi-domains` node. Domains give each slot
PMP-enforced memory isolation and its own `next_addr`/`next_mode`, which is
what makes "independently selectable" real rather than conventional.

The one hard dependency is a K230 `sbi_hsm_device`:

- interface: `struct sbi_hsm_device { int (*hart_start)(u32 hartid, ulong saddr); ... }`
  in `include/sbi/sbi_hsm.h`, registered via `sbi_hsm_set_device()`
- location: alongside the existing MAEE quirk in
  `platform/generic/thead/thead-generic.c`, registered from
  `thead_generic_early_init()`
- body: a switch over the two reset-vector register pairs -- a transcription
  of `k230_boot_baremetal`

Under Model A, `kernel(0, dtb)` goes away: OpenSBI hands each domain its own
`next_addr` with the correct hartid.

This also resolves an open item in `amp_bigcore_rvv_plan.md` ("does HSM
`hart_start` accept an arbitrary `start_addr_pa`? some vendor builds restrict
it to PMP-allowed regions") -- we author the domain regions, so we define what
is permitted.

## Device ownership, and the GSDMA conflict

`amp_bigcore_rvv_plan.md` plans to hand GSDMA (`0x80800000`) wholesale to the
second core, on the premise that Linux binds no driver to it. **That premise
is false on `dev`**: `buildroot-overlay/linux/0023-add-gdma-vo-rotation.patch`
binds GSDMA into the video-output rotation path, and
`0019-dts-add-nonai2d.patch` makes nonai2d Linux-owned.

Resolution consistent with the slot model: do not assign whole IP blocks by
core. Make **device ownership a per-slot config item**, and statically
partition GSDMA's 8 channels between slots. This avoids both reverting the VO
rotation feature and runtime arbitration. PDMA stays Linux-owned (the audio
driver has it).

## Open questions

Ordered by how much they gate work.

1. **Does the small core report `mhartid == 0` or `1`?** Gates whether Model A
   (one OpenSBI, two domains) is possible at all -- see "Hart ID space" above.
   Settled by one boot of a `CONFIG_LINUX_RUN_CORE_ID = 0` image with the
   serial console attached; no patch required.
2. **Inter-core cache coherency.** Unresolved; needs the TRM. Gates SMP
   entirely (hence the non-goal). AMP works either way via explicitly flushed
   shared buffers, but the answer determines how much flushing the IPC layer
   must do.
3. **PLIC hart-context numbering.** Current DT wires one hart only:
   `plic: interrupts-extended = <&cpu0_intc 11>, <&cpu0_intc 9>` and
   `clint: <&cpu0_intc 3>, <&cpu0_intc 7>`. Real context ordering needed from
   the TRM before a second slot can take interrupts.
4. **Can a core be cleanly re-reset at runtime?** Decides whether swapping a
   slot's payload requires a full reboot.
5. **DDR map.** `amp_bigcore_rvv_plan.md`'s placeholder reserved region
   collides with the current CMA range. Needs a real map; CMA is adjustable
   via the `linux-*.fragment` files.

## Related documents

- `docs/amp_bigcore_rvv_plan.md` -- one slot assignment in depth (small-core
  Linux + big-core RVV payload), including the data/control plane design,
  IPC protocol sketch, and DMA survey. Written on the small-core branch;
  carries `[premise]` annotations where its assumptions need re-verification.
- `docs/small_core_linux.md` -- state of the small-core Linux build (on the
  `opt_linux_on_small_core*` branch): what must be disabled and why, V4L2
  device map, capture recipes.

## Note on branch state

`opt_linux_on_small_core_cherry-picked` is **behind** `dev`, not merely
divergent -- it lacks the gt6700 and junroc boards, ili9881, and the
GDMA/nonai2d work. Bringing the AMP work forward is a forward-port onto `dev`,
not a merge.
