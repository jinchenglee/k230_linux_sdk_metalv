# Task: get isp_media_server running correctly on the K230 scalar core

Status: **not started**. This is a standalone task brief. If you are picking
this up fresh, you do not need to read any other AMP/dual-OS document to
start -- everything you need to know about *this specific problem* is here.
General K230/dual-OS background (what the AMP branch is, why Linux runs on
the small core, RPMsg, etc.) is assumed; none of that background is needed
to work this task, and nothing in this task touches that machinery.

## The one-sentence goal

Get a userspace ISP media-server daemon -- equivalent in capability to
`buildroot-overlay/package/vvcam/isp_media_server` (the RVV build) -- running
correctly on the K230 small core (RISC-V C908, **no** V extension), driving
the OV5647 sensor at 1280x720@60 with correct auto-exposure, so that a live
camera preview is not black.

## Explicit non-goals -- do not do these

- **Do not move the camera/display/ISP to the big core.** This has been
  ruled out at the project level: the AMP plan's big core is headed toward
  bare-metal/Embassy, not RT-Smart or any full media-processing stack, and
  will never carry a camera pipeline. Canaan's own official firmware always
  pairs "Linux owns the camera" with "Linux runs on the vector-capable
  core" (see Evidence 5 below) -- that pairing is exactly what this task
  must find a way to avoid needing.
- **Do not touch `tinytag_detect`, `apriltag_demo`, or any other
  application code.** They are already fixed for the small core (RVV build
  flags gated on `BR2_RISCV_ISA_RVV` in `buildroot-overlay/package/ai_demo/`)
  and are not part of this problem. Use `tinytag_detect.elf` only as a test
  harness (see "How to test" below); do not modify it.
- **Do not touch the nncase/KPU runtime.** That is a separate, already-solved
  problem (`docs/notes/rvv-free-nncase-v2.11.0.md`) with no relationship to
  the ISP daemon beyond both being RVV-compiled vendor components.
- **Do not accept 1080p@30 as the fix.** That already works today (see
  Evidence 1) and is the fallback the project is using while this task is
  open. This task is specifically about 1280x720@60 (and, ideally, making
  future resolutions/frame rates addable without repeating this exercise).
- **Do not edit the ISP tuning `.xml`/`.json` files' *content*** (renaming/
  moving them to test fallback behavior is fine and has been done safely
  several times -- see "Safety protocol"). Editing their content once
  already crashed the kernel (see Evidence 6) by breaking a lens-shading
  mesh's row-pitch assumption. If you need to understand what these files
  encode, read them; do not guess-edit them on a live board.

## Why this problem exists

The K230 has two cores. The small core is a scalar RISC-V C908 (no V
extension). The big core has RVV 1.0. This repo's AMP branch runs Linux on
the small core and a bare-metal/Embassy-bound firmware on the big core.

Canaan ships the camera ISP stack (VeriSilicon's `vvcam`/ISP8000 IP) as a
kernel driver (open source, in this repo at
`buildroot-overlay/package/vvcam/`) plus a **stripped, closed-source
userspace daemon**, `isp_media_server`, that implements the actual 3A
control loops (auto-exposure, auto-white-balance, lens-shading correction,
noise reduction, etc.) and mediates between V4L2 and the ISP hardware. This
repo carries two prebuilt copies of that daemon:

```
buildroot-overlay/package/vvcam/isp_media_server          # RVV build, 2,309,800 bytes
buildroot-overlay/package/vvcam/isp_media_server_scalar   # scalar build, 2,293,560 bytes
```

Buildroot selects between them via `BR2_RISCV_ISA_RVV`
(`buildroot-overlay/package/vvcam/vvcam.mk`). The scalar build is what small-
core Linux currently runs. **It works at 1920x1080@30 and produces a black
image at 1280x720@60.** That is the entire problem this task exists to fix.

## Confirmed facts (do not re-derive these; they are established)

1. **1080p@30 works, 720p@60 does not, on the same board/kernel/rootfs,
   same daemon, only the selected sensor mode differing.** Renaming away
   the per-mode 720p scene files (`/etc/vvcam/ov5647-1280x720.{xml,manual.json,
   auto.json}`) makes the app fall back to 1080p@30 and the image returns
   immediately. This rules out the AMP memory carveout, the DRM plane/
   rotation path, the small-core kernel build, and the LSC tuning tables in
   general (1080p's tuning tables are exercised by the same code and work
   fine).

2. **The daemon binary itself is the variable, not the rest of the Linux
   build.** Isolating test done 2026-09-06: copied
   `isp_media_server_scalar` onto a *separate* Linux build running on
   vector-capable hardware (which can execute either daemon binary),
   replacing its working RVV daemon, kernel/rootfs/DTS/tuning files all
   held fixed. Result: black screen at 720p, on hardware with full vector
   support, running the exact rootfs already proven to work with the RVV
   daemon. This directly proves the daemon binary is the actual cause, not
   a small-core-specific kernel/DTS/rootfs difference.

3. **Two daemon binaries are essentially the same code, minus ~5 functions.**
   Both binaries retain their full dynamic symbol table despite being
   marked "stripped" -- this is VeriSilicon's `CamEngine`/ISI ISP
   architecture (`AecGetConfigure`, `AwbSetMode`, `ALscSetMode`,
   `IsiEnumModeIss`, `CamEngineSetMode`, `A2DnrProcessFrame`, etc.), not an
   opaque blob. As of this writing:

   ```
   riscv64-unknown-linux-gnu-readelf -sD buildroot-overlay/package/vvcam/isp_media_server        | awk '$4=="FUNC" && $7!="UND"{print $8}' | sort -u | wc -l   # 2228
   riscv64-unknown-linux-gnu-readelf -sD buildroot-overlay/package/vvcam/isp_media_server_scalar | awk '$4=="FUNC" && $7!="UND"{print $8}' | sort -u | wc -l   # 2223
   ```

   The only functions present in the RVV build and absent from the scalar
   one are `IsiGetHFlipIss`, `IsiSetHFlipIss`, `IsiGetVFlipIss`,
   `IsiSetVFlipIss`, and one `get_verisilicon_[...]` symbol -- mirror/flip
   support, unrelated to scene-switching or resolution. Every AE/mode
   function spot-checked (`CamEngineAeSetMode`, `IsiGetModeIss`,
   `IsiEnumModeIss`, `VsiAeLibSetMode`, `CamerIcSetSensorMode`,
   `AecSceneEvaluation`) has **identical compiled size** in both binaries.
   **Conclusion: whatever is wrong is not "the scalar build is missing a
   whole feature."** It is either in a function nobody has inspected yet
   (2223 minus ~10 leaves most of the binary unchecked), in embedded data/
   constants rather than code (invisible to a symbol-table diff), or in a
   same-named, same-sized function whose actual bytes differ.

4. **A specific, reproducible fault instruction is known** (found while
   diagnosing a related SIGILL in `tinytag_detect.elf`, and independently
   present in `isp_media_server` itself): instruction word `0xcc747057`
   disassembles as `vsetivli zero,8,e8,mf2,ta,ma`. In the current
   `isp_media_server` RVV binary it appears repeatedly in a short run (file
   offsets 0x7f1f6, 0x7f1fe, 0x7f20a as of the current build -- **re-verify
   this against whatever binary you're actually working with**, offsets
   shift between builds):

   ```
   7f1ec: 4701       li  a4,0
   7f1ee: fc042223   sw  zero,-60(s0)
   7f1f2: fc840793   addi a5,s0,-56
   7f1f6: cc747057   vsetivli zero,8,e8,mf2,ta,ma
   7f1fa: 5e0030d7   vmv.v.i v1,0
   7f1fe: cc747057   vsetivli zero,8,e8,mf2,ta,ma
   7f202: 020780a7   vse8.v  v1,(a5)
   7f206: fd040793   addi a5,s0,-48
   7f20a: cc747057   vsetivli zero,8,e8,mf2,ta,ma
   7f20e: 020780a7   vse8.v  v1,(a5)
   ```

   This pattern (repeated 8-byte `vsetivli`+`vmv.v.i`+`vse8.v` triples
   zeroing small fixed-size structures) is a classic GCC auto-vectorized
   `memset`/struct-zeroing idiom on a loop-free straight-line sequence --
   i.e. this is very likely *not* hand-written vector code but the compiler
   vectorizing an ordinary `memset(&struct, 0, N)` or equivalent
   initializer. That matters for feasibility: this class of vector usage is
   usually the easiest to devectorize correctly (a fixed-size zero-fill has
   an obvious, provably-equivalent scalar replacement of `sw`/`sh`/`sb`
   instructions), unlike a genuine vectorized numeric loop (e.g. an AE
   histogram accumulation) where getting the scalar replacement bit-exact
   is much harder to verify.

5. **No newer scalar build, from Canaan or anyone else, is known to exist.**
   Checked 2026-09-06:
   - The scalar blob currently in this repo is frozen at commit `25a6f591`
     (2025-07-30) in Canaan's private blob-update history, before scene-
     switching support (`ea7f25d7`) and before vector instructions first
     appear (`83fce354`) -- confirmed via
     `git merge-base --is-ancestor ea7f25d7 83fce354` (exit 1, not an
     ancestor: `ea7f25d7` postdates `83fce354`).
   - Canaan's own official dual-OS SDK (sibling checkout, if present, at
     `/work/git_repo/k230_sdk`) never runs this daemon, or any camera/ISP
     component, on the small core in any shipped configuration. Its
     `repo.mak` places the entire media pipeline (`MPP_SRC_PATH`) under the
     big core alongside RT-Smart; `find . -iname "*isp_media*"` across that
     whole tree returns nothing. Its `only_linux` fallback configs, used
     when a board needs full camera capability from Linux, move Linux onto
     the *big* (vector) core (`CONFIG_LINUX_RUN_CORE_ID=1`,
     `CONFIG_LINUX_DEFCONFIG="k230_evb_linux_enable_vector"`) and drop RT-
     Smart entirely, rather than running the ISP stack on the scalar core.
   - VeriSilicon's own press materials describe the K230 as "the world's
     first commercial mass production edge AIoT chip supporting the
     RISC-V Vector 1.0 standard" paired with this exact ISP8000 stack --
     there is no other RISC-V SoC vendor shipping this software yet to
     check for a scalar build.
   - `nxp-imx/isp-vvcam` (github.com/nxp-imx/isp-vvcam) is VeriSilicon's
     same architecture family, published by NXP for their ARM-based i.MX8
     parts. Its `SCR-isp-vvcam.txt` states `Type of Content: ISP Kernel
     Module source` -- kernel driver only, matching what this repo already
     has open, with no userspace daemon equivalent anywhere in that tree.
     NXP's own developer forum independently describes their equivalent
     daemon as proprietary. (Also: ARM binaries cannot run on RISC-V
     regardless, so even if NXP's daemon were public, it would not be
     directly usable -- only source would help, and there is none.)
   - **Conclusion: assume no external scalar (or newer, feature-complete)
     build exists to find or obtain by searching.** The two paths that
     remain are (a) ask Canaan directly for source, in parallel with this
     task, and (b) reverse-engineer/patch what we already have.

6. **A tuning-file content edit crashed the kernel once already** (not
   `isp_media_server`, but directly relevant as a safety lesson): removing
   the lens-shading-correction sector-table rescale from a 720p profile
   (leaving 1080p-derived sector spacing applied to a 720p frame) caused a
   NULL-pointer dereference inside `vvcam_isp_priv_ioctl` in the kernel
   driver, killing `isp_media_server` and requiring a board power cycle to
   recover. **Only ever rename/hide `.xml`/`.json` files to test fallback
   behavior; never edit their numeric content on a live board without
   independently verifying the edit is self-consistent first.**

## Ruled out -- do not re-investigate these

- ~~The scalar daemon predates scene-switching support and therefore lacks
  the code to handle a scene change.~~ **Retracted.** The symbol-level
  comparison in Evidence 3 shows the AE/mode-related function set is
  essentially identical between builds. If you want the full retraction
  writeup and the dead-end investigation that produced and then disproved
  this theory, see `SCALAR_ISP_MEDIA_SERVER.md` in this same directory --
  you do not need it to do this task, but it may save you from re-treading
  the same disproven idea from a different angle.
- ~~It's an AMP memory carveout / DRM plane / small-core-kernel-specific
  issue.~~ Ruled out by Evidence 1 and Evidence 2 above.
- ~~It's fundamentally a 60fps throughput ceiling the daemon can't keep
  up with.~~ Unlikely: a headless capture run at 720p@60 sustained 56-58fps
  camera+AI throughput with no drops -- the daemon moves pixel data at full
  rate, it just produces the wrong exposure/output for that mode. (Not
  independently re-confirmed on this exact isolating setup; worth a quick
  sanity re-check early in this task, but do not assume it's the frame rate
  without evidence.)

## Open questions -- this is what you're actually solving

1. Where, specifically, does the scalar and RVV code diverge in behavior,
   given the function set is nearly identical? Candidates, cheapest-to-check
   first:
   - A same-named, same-*sized* function whose bytes actually differ (the
     size check in Evidence 3 does not rule this out -- only a byte-for-byte
     or semantic diff would).
   - An embedded data/constant table (`.rodata`/`.data`) that differs
     between the two builds independent of any function's code -- e.g. a
     compiled-in default AE gain ceiling, exposure-time table, or sensor-
     mode capability table.
   - A function outside the ~10 already spot-checked. There are 2223+
     candidates; prioritize anything with `Ae`, `Aec`, `Gc` (gain control),
     `Exp`, `Hist` (AE typically uses a histogram), or `Mode`/`Scene` in the
     name.
2. Is the one known SIGILL site (`0xcc747057`, Evidence 4) actually on the
   code path exercised during real 720p capture+AE+streaming, or was it
   found via an unrelated crash in a different binary (`tinytag_detect.elf`)
   and only *happens* to also exist in `isp_media_server`? This needs
   verifying against `isp_media_server` specifically -- see "Suggested
   first steps."
3. Does the scalar daemon ever actually reach a vector instruction and
   SIGILL when driving 720p on the *small* core, or does it run to
   completion without crashing and just produce wrong output? These are two
   different bugs requiring different fixes (a SIGILL needs the faulting
   site patched; wrong-but-non-crashing output needs the actual AE/tuning
   logic difference found). **This has not been directly observed** --
   every 720p test so far either ran on vector-capable hardware (where a
   vector instruction would execute, not crash) or was the small-core board
   at 1080p (fallback, not the failing case). Getting a small-core dmesg/
   strace of the *actual* 720p failure, including whether it SIGILLs or
   runs to completion, is probably the single highest-value first step.

## Available tools and artifacts

- **Toolchain**: `/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-{gcc,g++,objdump,readelf,addr2line,nm,c++filt,...}`.
  This is the same toolchain used to build the rest of this SDK. Note that
  **both** `isp_media_server` and `isp_media_server_scalar` declare the
  identical, vector-capable `Tag_RISCV_arch` in their ELF attributes
  (`readelf -A`) -- `v1p0`, `zve32f`/`zve64d`/etc. all present in both. The
  ELF attribute is not a reliable way to tell them apart; it reflects the
  `-march` passed to the compiler, not what instructions actually got
  emitted. Confirmed by direct disassembly: `isp_media_server_scalar`
  contains zero decoded vector instructions (`vset*`/`v*.*` mnemonics);
  `isp_media_server` contains roughly 1500+ (exact count is toolchain/regex-
  dependent -- regenerate it yourself with `objdump -d | grep -c` against
  a real RVV mnemonic list rather than trusting a specific number here).
  Neither binary declares any Xuantie/T-Head vendor extension
  (`xthead*`) in its ELF attributes, unlike some other binaries in this
  tree (e.g. `tinytag_detect.elf`, which links Rust AprilTag code using
  `xtheadvdot`) -- not expected to be relevant to this task, but worth
  re-checking if disassembly ever looks like it's misdecoding an
  instruction.
- **No Ghidra, radare2, or other interactive disassembler was found
  installed** on the machine this was investigated from (`which ghidra
  ghidraRun radare2 r2` all failed; only plain `objdump`/`readelf` were
  available). If this task needs a decompiler, install one first -- Ghidra
  has RISC-V support and is a reasonable default given the rich symbol
  names (this is a naming problem, not primarily a decompilation-quality
  problem, so even objdump + symbol names may get you further than usual).
- **Binaries to work from**: both already in this repo at
  `buildroot-overlay/package/vvcam/isp_media_server{,_scalar}` (paths
  above). Copy them to a scratch location before disassembling/patching;
  never edit the repo copies in place until you have a verified-working
  replacement.
- **Kernel driver source** (open, in this repo, useful for understanding
  the userspace/kernel protocol the daemon must implement):
  `buildroot-overlay/package/vvcam/v4l2/isp/vvcam_isp_driver.c`,
  `vvcam_isp_event.c` / `vvcam_isp_event.h` (the `VVCAM_ISP_DEAMON_EVENT`
  synchronous notification protocol -- event IDs below), `vvcam_isp_procfs.c`.
  Sensor mode/timing source: `buildroot-overlay/package/vvcam/src/ov5647.c`.
- **Event ID enum** (`vvcam_isp_event.h`), used by the kernel driver to
  notify the daemon of state changes via a V4L2 event
  (`VVCAM_ISP_DEAMON_EVENT = V4L2_EVENT_PRIVATE_START + 2000`), with a
  200-second synchronous wait-for-ack over shared memory
  (`vvcam_isp_post_event()` in `vvcam_isp_event.c`):

  ```c
  enum vvcam_isp_vevent_id {
      VVCAM_ISP_EVENT_SET_FMT,      // 0
      VVCAM_ISP_EVENT_REQBUFS,      // 1
      VVCAM_ISP_EVENT_QBUF,         // 2
      VVCAM_ISP_EVENT_BUF_DONE,     // 3
      VVCAM_ISP_EVENT_STREAMON,     // 4
      VVCAM_ISP_EVENT_STREAMOFF,    // 5
      VVCAM_ISP_EVENT_S_CTRL,       // 6
      VVCAM_ISP_EVENT_G_CTRL,       // 7
      VVCAM_ISP_EVENT_S_SELECTION,  // 8
      VVCAM_ISP_EVENT_MAX,
  };
  ```

  Observed on a big-core/scalar-daemon 720p test: `post event 6 not
  subscribed` (S_CTRL) immediately after the kernel's scene-switch ioctl
  logs `scene set OK`. This means the scalar daemon never registered
  interest in `S_CTRL` events at all (`vvcam_isp_event_subscribed()`
  returns false immediately, no wait, no ack attempted) -- worth checking
  whether the RVV daemon *is* subscribed to it, and if so, what it does
  when it receives one. This is the most concrete behavioral lead currently
  known; it was not chased further before this handoff. **The comparable
  1080p-with-scalar-daemon check (does the same "not subscribed" line
  appear there too, i.e. is it universal or 720p-specific) was attempted
  and inconclusive** -- hiding the per-mode files on that particular test
  board produced "active sensor has neither 1280x720@60 nor 1920x1080@30"
  instead of a clean 1080p fallback, for reasons not understood (possibly
  leftover state from repeated daemon kill/restart cycles on that board;
  restoring the files fixed it immediately, nothing was left broken). Redo
  this comparison cleanly, ideally starting from a fresh boot, as an early
  step.
- **Sensor mode table** (`ov5647.c`): exactly two complete, independent
  register-sequence modes exist, each with its own baked-in timing --
  there is no runtime frame-interval control decoupling resolution from
  fps in this driver:

  ```
  1920x1080 @ 30fps   frame_length = 1199 lines
  1280x720  @ 60fps   frame_length =  851 lines   (true binned sensor mode)
  ```

  If you need a third data point to separate "resolution" from "fps" as a
  variable (e.g. to test 1280x720@30), you would need to write a new
  register sequence -- real sensor bring-up work, not expected to be
  necessary for this task, but noted in case the investigation leads there.

## How to test

Small-core board access: SSH as root (empty password / key-based, matches
whatever access this board already has configured), IP address varies by
whichever card is flashed -- confirm with whoever hands off the board, or
check `/proc/cpuinfo`'s `isa` line to confirm you're on the scalar core
(`rv64imafdc...`, no `v`) before testing, since a "big core Linux" test
card with a *different* IP may also be in rotation (vector-capable,
`rv64imafdcv...` -- useful for isolating-test purposes exactly as done in
Evidence 2, but not the target platform).

Fastest smoke test, no camera/display dependency at all (does not exercise
`isp_media_server`, but confirms the daemon process itself is alive and the
board is in a sane state):

```sh
ps | grep isp_media_server
```

Camera test, headless (no display dependency, fastest signal):

```sh
cd /root/app/tinytag_detect
{ sleep 14; echo q; } | TINYTAG_CV_DETECTOR=c ./run.sh --no-display
```

Look for `sensor selected 1280x720@60 (preferred)` (mode negotiation
succeeded) versus `... (fallback)` (720p was rejected and it fell back to
1080p -- if you see this, something about the *scene selection*, not the
daemon's runtime behavior, has regressed, which is a different bug from the
one this task is about). Then look at the per-frame `camera:`/`AI:` fps
counters and `tags/s:` -- a live AprilTag needs to be in the camera's view
for `tags/s` to read anything but zero; a black/underexposed image will
show `tags/s: 0.00` even at full frame rate, which is the current failure
signature.

Kernel-side evidence: `dmesg -c` before a run clears the ring buffer;
`dmesg` after shows `scene set OK`/`scene get`/`post event N ...` lines from
the kernel driver (`vvcam_isp_driver.c`/`vvcam_isp_event.c`) -- this is your
best window into what the daemon is (or isn't) doing, since the daemon
itself has no useful stdout logging in these tests (its own log level may
be adjustable; not investigated).

`TINYTAG_CV_DETECTOR=c` forces the scalar AprilTag decoder instead of the
default RVV one (`libapriltag_rvv.a`, a Rust crate, unrelated to this task
but will itself SIGILL on the small core if left at its default). Always
set this when testing on the small core; it is irrelevant on vector-capable
test boards but harmless to leave set everywhere.

## Safety protocol (all learned the hard way during this investigation)

- **Always back up a binary before replacing it on a live board**:
  `cp -n /usr/bin/isp_media_server /usr/bin/isp_media_server.orig` (the
  `-n` makes this idempotent -- it won't overwrite an existing backup with
  a possibly-already-swapped file). Verify the backup's md5sum before
  proceeding, and verify it again when restoring.
- **`/etc/init.d/S31canaan_isp restart` does not reliably stop the old
  daemon process before starting a new one.** Check `ps | grep
  isp_media_server` after any restart; if more than one process is
  running, `kill -9` all of them and start fresh
  (`/etc/init.d/S31canaan_isp start`) rather than trusting `restart`.
- **Never edit a `.xml`/`.json` tuning file's numeric content on a live
  board.** Renaming/hiding whole files to test fallback behavior is safe
  and has been done repeatedly without incident; editing the numbers inside
  one caused a kernel NULL-deref crash (Evidence 6) requiring a power
  cycle. If you need to test a modified profile, understand its full
  internal cross-references first (this one crash was caused by a lens-
  shading mesh's row-pitch/sector-table relationship, which was not obvious
  from the file alone).
- **Leave the board as you found it.** If you swap `isp_media_server` for
  testing, restore the original binary (verify via md5sum) and restart the
  service cleanly before ending a session, whether or not your test
  succeeded.

## Suggested first steps, in order

1. Reproduce the failure on an actual small-core board (not the vector-
   capable isolating-test board) and capture `dmesg` during a 720p attempt.
   Determine: does it SIGILL (crash, visible in `dmesg` as `unhandled
   signal 4`/an oops), or does it run to completion producing a black
   image? This resolves Open Question 3 and determines which of two very
   different next steps applies.
2. If it SIGILLs: get the faulting instruction word and offset from
   `dmesg` (`epc`/`badaddr` fields, same technique used to find the
   `0xcc747057` site in Evidence 4), confirm it's a vector instruction, and
   determine the smallest correct scalar patch for that specific site.
   Resume and repeat for whatever is reached next. This is the "patch only
   what's actually hit" approach -- much smaller in scope than devectorizing
   the whole binary up front, since most of the binary's ~1500+ vector
   instructions are never on the code path a given workload exercises.
3. If it runs to completion without crashing: the bug is behavioral, not a
   missing-instruction crash. Go back to Open Question 1 -- diff data
   sections, or disassemble/compare specific AE/mode-related functions
   byte-for-byte (not just by size) between the two binaries, prioritizing
   names containing `Ae`, `Aec`, `Gc`, `Exp`, `Hist`.
4. In parallel, independent of the RE track: ask Canaan for
   `isp_media_server` source, or at minimum a scalar build newer than
   `25a6f591` that includes 720p/scene-switching support. Low probability
   of success per Evidence 5, but cheap to pursue alongside the RE work and
   would obsolete all of it if it succeeds.
5. Whichever path succeeds, the fix criterion is: `run.sh` (no
   `--no-display`) at 1280x720@60 on an actual small-core board shows a
   correctly-exposed live image, and a real AprilTag placed in view is
   detected (`tags/s` > 0, matching hamming-0 decodes comparable to the
   working 1080p or big-core baselines already established). Update
   `SCALAR_ISP_MEDIA_SERVER.md` with the result either way -- a confirmed
   dead end is as valuable to record as a fix.
