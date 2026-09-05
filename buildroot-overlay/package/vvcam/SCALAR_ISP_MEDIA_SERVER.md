# Scalar ISP media server

`isp_media_server_scalar` is the historical `isp_media_server` blob from
commit `25a6f59170640bbd9779f9a24167f1451fb2370f`
(`fix v4l2 run err`), blob ID
`00d911824c59ff7df4168500360fc875476d669a`. This is the exact daemon
inherited by the tip of `opt_linux_on_small_core_cherry-picked`, where the
CSI/ISP path was exercised on the scalar core.

Its ELF attributes declare RVV because it was compiled with an RVV-capable
`-march`, but complete disassembly contains no vector instructions. The name
`scalar` describes its executable instruction stream rather than its
over-broad ELF attribute.

Commit `83fce3541008bf5f6de9a0d3e7b312cb50ba1e19`
(`libmmz: Add kd_mpi_get_vvcam_video00()...`) replaced it with the first
daemon containing actual vector instructions. That revision and every later
daemon through the current blob contain 1458 vector instructions. On the
small core, the current daemon faults on instruction word `0xcc747057`,
which disassembles as `vsetivli zero,8,e8,mf2,ta,ma`.

Buildroot selects the scalar file when `BR2_RISCV_ISA_RVV` is disabled and
installs it as `/usr/bin/isp_media_server`. RVV-enabled configurations retain
the current daemon.

The daemon is a Canaan-supplied stripped prebuilt; source is not present in
this repository. Features added from `83fce35` onward require regression
testing after any daemon swap.

## Confirmed 2026-09-05: the scalar daemon cannot drive the 720p mode

The scalar blob produces a black/frozen preview at 1280x720 on CanMV-K230 V3
small-core Linux, while the same rootfs at 1920x1080 and the RVV daemon at
1280x720 on big-core Linux both work. This is not a vector-instruction issue;
it is a feature-freeze-date issue that happens to correlate with the ISA
split. The two are independent axes -- ISA is a compile flag, camera-mode
support is C source -- and they only appear linked because of which commit
Canaan chose as the last vector-free build.

### The A/B evidence

Same small-core rootfs, same `tinytag_detect.elf`, only the requested/selected
sensor mode differs:

| capture mode | daemon | image on LCD | AI decodes |
| --- | --- | --- | --- |
| 1920x1080@30 (fallback) | scalar | yes | yes (`id=18`, `id=27`, hamming 0) |
| 1280x720@60 (preferred) | scalar | **black** | **none** |
| 1280x720@60 (preferred) | RVV, big-core Linux | yes | yes (`id=18`, `id=27`, hamming 0) |

Forcing 1080p on the small core (by temporarily hiding the per-mode 720p scene
files so `v4l2_drm_select_scene_profile()` falls through to the fallback
resolution) restores the live image immediately, with no other change. This
rules out the AMP carveout, the DRM plane/rotation path, the LSC tuning
tables, and the small-core kernel: all of those are identical between the
working 1080p run and the broken 720p run on the same board.

### Retracted 2026-09-06: the scene-switching-freeze theory

This note originally claimed the scalar blob's freeze date (`25a6f591`,
2025-07-30, before `ea7f25d7` "add switch scenes function") meant the daemon
itself lacked scene-switching code, and that this was why 720p failed. That
claim does not survive a direct check and should not be trusted:

```
readelf -sD isp_media_server_scalar | awk '$4=="FUNC" && $7!="UND"{print $8}' | sort -u > scalar_funcs.txt
readelf -sD isp_media_server        | awk '$4=="FUNC" && $7!="UND"{print $8}' | sort -u > rvv_funcs.txt
comm -13 scalar_funcs.txt rvv_funcs.txt   # functions only in the RVV (working) build
```

Both binaries keep their full dynamic symbol table despite being marked
"stripped" (2223 vs 2228 named functions -- this is VeriSilicon's
`CamEngine`/ISI ISP architecture, the same family other SoC vendors license,
not an opaque blob). The only functions present in the RVV build and absent
from scalar are `IsiGetHFlipIss`, `IsiSetHFlipIss`, `IsiGetVFlipIss`,
`IsiSetVFlipIss`, and `get_verisilicon_[...]` -- mirror/flip support, matching
the unrelated `ff2af2ed`/`1b695b83` mirror-flip commits. Nothing scene- or
resolution-related was added. Spot-checking AE/mode functions
(`CamEngineAeSetMode`, `IsiGetModeIss`, `IsiEnumModeIss`, `VsiAeLibSetMode`,
`CamerIcSetSensorMode`, `AecSceneEvaluation`) shows **identical compiled size**
in both binaries. The scene-switching feature from `ea7f25d7` almost certainly
lives entirely in the kernel driver (`vvcam_isp_driver.c`,
`vvcam_isp_procfs.c`) and the standalone `v4l2-drm-scene` tool, not in
`isp_media_server` itself -- consistent with the AE/AWB/LSC algorithm library
being generic and data-driven (it reads whatever `.xml`/`.json` profile the
kernel points it at) rather than hardcoding known resolutions.

This also exposes a methodology gap in the original A/B evidence: the
"1280x720@60 works on big-core Linux" data point changed two things at once
-- the daemon binary *and* the entire Linux kernel/rootfs (a different
defconfig, not just a different `/usr/bin/isp_media_server`). It was never a
single-variable swap. The 1080p-vs-720p comparison on the *same* small-core
image, same daemon, same kernel, only the selected scene differing, remains
solid; the daemon-attribution part does not.

### Resolved 2026-09-06: the daemon is confirmed as the variable

The planned isolating test was run: `isp_media_server_scalar` copied onto
big-core Linux (vector-capable hardware, so it can execute either daemon
build), replacing the working RVV binary, kernel/rootfs/DTS/tuning files all
held fixed. Backed up the original first (`cp -n isp_media_server
isp_media_server.rvv-orig`), restarted the ISP service cleanly (a stale
process from an earlier restart had to be `kill -9`'d first -- `S31canaan_isp
restart` does not reliably stop the old process before starting a new one),
and re-ran `tinytag_detect` at 1280x720@60:

```
scene set OK - sensor=ov5647 xml=/etc/vvcam/ov5647-1280x720.xml ...
```

Result: black screen, on hardware with full vector support, running the
exact rootfs already proven to work with the RVV daemon. This rules out
candidate 3 above (a small-core-specific kernel/DTS/rootfs difference) --
the daemon binary itself is the variable. The original RVV daemon was
restored afterward (`md5sum` verified against the backup) and the board
left in its prior state. Candidates 1 and 2 (an unchecked function, or a
data/constant difference invisible to symbol-table diffing) remain open.

A cleaner mechanism signal came out of this run than the earlier G_CTRL
noise:

```
post event 6 not subscribed
```

Event id 6 is `VVCAM_ISP_EVENT_S_CTRL`. This fires immediately after the
scene-switch ioctl (`vvcam_isp_post_event()` checks
`vvcam_isp_event_subscribed()` before even attempting delivery, per
`v4l2/isp/vvcam_isp_event.c`) and returns `-EINVAL` at once, with no 200
second wait -- unlike the earlier G_CTRL case, this is not "the daemon was
asked and failed," it is "the daemon never registered interest in this event
class at all." A follow-up attempt to check whether the *same* scalar daemon
also emits this on a working 1080p run (the natural next comparison) hit an
unrelated problem on this particular board: temporarily hiding the per-mode
720p scene files, which cleanly falls back to 1920x1080@30 on the small-core
board, instead produced `active sensor 'ov5647' has neither 1280x720@60 nor
1920x1080@30` here. Restoring the files immediately fixed it, so nothing was
left broken, but the cause (a board/rootfs difference from what the
small-core image expects, or leftover state from the repeated
kill+restart+swap cycle) is unexplained and this comparison was not
completed. Treat "not subscribed to S_CTRL" as a real, cleaner data point
than the earlier G_CTRL evidence, but not yet proven to be 720p-specific
rather than universal.

### Can the RVV daemon just avoid its vector instructions at runtime?

No -- there is no toggle to find. The 1458 vector instructions in the current
daemon are not a switchable code path guarded by a runtime CPU-feature check;
they are what `-mcpu=c908v` produced when GCC auto-vectorized ordinary loops
at compile time. The scalar and RVV blobs are two separate compilations of
(presumably) the same source line existing in one binary or the other, not
one binary containing both a vector path and a dormant scalar fallback.
There is no config file, environment variable, or ioctl that makes the RVV
build "not use" the vector instructions already baked into its machine code.

### Disassemble the RVV daemon and rewrite it to scalar-only?

Two very different scopes hide under this question:

**Whole-binary devectorization** -- locate all ~1458 vector instructions
across the 2.3 MB stripped binary, reconstruct each one's enclosing loop,
write a correct scalar replacement, and fit it into the same code slot (or
relink the whole binary) without disturbing register conventions the
surrounding code depends on, with no source and no reference output to
validate the numerics against. This is a real, narrow area of binary
analysis -- closer to writing a partial RISC-V vector-to-scalar static binary
translator than to patching a function -- and realistically weeks-to-months
of specialized effort with a material risk that a version which boots
without crashing is still numerically wrong in ways this project has no way
to detect automatically (the exact failure mode already being chased). Not
recommended as a first move.

**Patch only what the small core actually reaches, one fault at a time** --
trap the SIGILL, identify the faulting instruction (the doc's known example:
`0xcc747057` = `vsetivli zero,8,e8,mf2,ta,ma`), patch just that site with a
scalar equivalent, resume execution, and repeat for whatever is reached next.
This only requires devectorizing code paths a real workload actually
exercises, not all 1458 sites blindly, which is a meaningfully smaller job --
though how many distinct sites real capture+AE+AWB+LSC execution would
surface is not yet scoped, and it could still be more than a handful.

### 2026-09-06: is source, or any usable RISC-V scalar build, public anywhere?

Broadened past "source only": is there *any* prebuilt RISC-V build of this
daemon (scalar or otherwise, from any vendor) worth trying directly, even
without source? Checked both.

**Source.** `isp_media_server`'s dynamic symbol names (`CamEngine*`, `Aec*`,
`Awb*`, `A2Dnr*`, `IsiGetModeIss`, etc.) are VeriSilicon's ISP algorithm
stack, licensed to multiple SoC vendors beyond Canaan, so it was worth
checking whether another licensee had published source. One public repo
exists, [`nxp-imx/isp-vvcam`](https://github.com/nxp-imx/isp-vvcam), NXP's
i.MX8-series counterpart to this repository's `buildroot-overlay/package/vvcam`
kernel driver tree -- confirmed by its `SCR-isp-vvcam.txt`:

```
Type of Content:    ISP Kernel Module source
Origin:             VeriSilicon Holdings Co., Ltd. (GPL-2.0)
```

Kernel driver only, matching what this repository already has open. No
`isp_media_server`/3A-daemon equivalent exists anywhere in that tree. NXP's
own developer community independently confirms their equivalent daemon is
equally closed: a coredump path shared on the NXP forum reads
`/backup/build/users/.../vsi-isp/verisilicon_sw_isp/`, and their forum
describes `isp_media_server` there as "proprietary" -- "only NXP and their
partners have information about sensor-specific calibration configuration."
This is VeriSilicon's standard licensing model across its customer base, not
something specific to Canaan.

**Any other RISC-V build to try, source or not.** No candidate exists to
try, for a structural reason rather than a search gap: VeriSilicon's own
press materials describe the K230 as
"[the world's first commercial mass production edge AIoT chip supporting the
RISC-V Vector 1.0 standard](https://verisilicon.com/en/PressRelease/CANAAN)"
paired with this exact ISP8000 stack. There is no other RISC-V vendor
shipping this software to check -- K230 is the pioneering, and so far only,
deployment. Community search (Kendryte/CanMV forums, GitHub issues) surfaces
no discussion of this specific combination (small-core Linux driving the
camera daemon at 720p) anywhere, consistent with the "Canaan's own precedent"
section above: nobody, including Canaan itself in its own shipped configs,
has ever needed a full-featured scalar build of this daemon, because nobody
ever puts a non-vector core in charge of the camera. The one scalar build in
existence (`25a6f591`) was very likely an incidental byproduct of
pre-production bring-up before RVV support landed, not a maintained variant
-- there is no reason to expect a newer one exists anywhere, published or
not. The only two ways to obtain real source remain asking Canaan directly,
or the RE approaches above.

### Canaan's own precedent: this component does not exist on small-core Linux

Checked 2026-09-06 against two Canaan-maintained trees, both outside this
repository:

**`k230_sdk`** (sibling checkout at `/work/git_repo/k230_sdk`) is Canaan's
official dual-OS SDK: small-core Linux plus big-core RT-Smart, the same
topology this repo's AMP branch targets. `repo.mak` places the camera stack
entirely under the big core:

```
RT-SMART_SRC_PATH = src/big/rt-smart
MPP_SRC_PATH       = src/big/mpp        # media pipeline: ISP, sensor, VO
LINUX_SRC_PATH     = src/little/linux   # small core
```

`src/big/mpp/kernel/sensor` holds the sensor/ISP driver. `find . -iname
"*vvcam*"` and `*isp_media*"` across the entire tree return nothing --
neither vvcam nor `isp_media_server` exists on the small-core side, in any
form, vector or scalar. `configs/k230_canmv_v3_defconfig` (their v3 board)
confirms this is the shipped default, not a stripped-down variant:
`CONFIG_LINUX_RUN_CORE_ID=0` with `CONFIG_SUPPORT_RTSMART=y`.

The revealing case is `k230_canmv_only_linux_defconfig`, Canaan's fallback
for a board that needs full camera capability from Linux:

```
CONFIG_LINUX_DEFCONFIG="k230_evb_linux_enable_vector"
CONFIG_LINUX_RUN_CORE_ID=1        # Linux moves to the big/vector core
# CONFIG_SUPPORT_RTSMART is not set
```

When Linux needs the camera, Canaan moves Linux onto the vector-capable core
and drops RT-Smart, rather than running the ISP stack on the scalar core.
They do not attempt small-core-Linux-owns-the-camera in any config.

**`canmv_k230`** (the MicroPython/CanMV firmware, `github.com/kendryte/
canmv_k230`) corroborates this independently. Its checkout is MicroPython
bindings only -- no vvcam or ISP code -- fetched via a `repo` manifest that
also pulls a separate `canmv-k230/rtsmart` repo (named in `CHANGELOG.md`)
with its own release cadence, matching the same big-core-owns-media pattern.
This is corroboration by naming/lineage convention: the manifest's other
repos were not synced (several need Canaan's internal `g.a-bug.org` gitlab),
so this is not an independent byte-level confirmation the way the `k230_sdk`
finding is.

The conclusion this repository draws from that: there is no upstream scalar
`isp_media_server` to find, current or historical, because Canaan has never
shipped small-core Linux driving this camera stack. The 720p failure here
is not a gap behind a fixable daemon update -- it is the first place this
repo's AMP branch has gone somewhere Canaan's own firmware never goes.

### Practical options

1. Run the small core at 1080p@30 for now: works today, no code change, but
   loses the 720p/60 capture path the dual-OS plan assumes and the LSC/AE
   tuning done for that mode.
2. Obtain the current daemon's source from Canaan and rebuild it without
   vector instructions (same approach as the nncase scalar rebuild in
   `docs/notes/rvv-free-nncase-v2.11.0.md`), or reverse-engineer the existing
   RVV blob and either recompile or binary-patch out its vector instructions.
   The rich VeriSilicon `CamEngine`/ISI symbol names surviving in the
   "stripped" binary (~2900 named functions, not opaque addresses) make the
   latter more tractable than typical blob RE, but neither path is proven to
   fix 720p until the daemon is confirmed as the actual variable -- see the
   open question above. Source is not present in this repository either way,
   and Canaan's own precedent above suggests no vector-free build past the
   scalar freeze point has ever existed to simply obtain instead.

Moving camera/display ownership to the big core, matching Canaan's own
pattern, is **not** an option for this project: the AMP plan's big core is
headed toward bare-metal/Embassy, not RT-Smart or an MPP-equivalent media
stack, so it will never carry a camera/ISP pipeline. Any fix has to work
within small-core Linux driving vvcam directly, which is precisely the
configuration Canaan has never shipped -- so this is genuinely unexplored
territory, not a known-solvable gap.

Phase 7 KPU inference (`docs/notes/rvv-free-nncase-v2.11.0.md`) is unaffected
by any of this: it reads pre-decoded image files from disk and never touches
the ISP daemon.
