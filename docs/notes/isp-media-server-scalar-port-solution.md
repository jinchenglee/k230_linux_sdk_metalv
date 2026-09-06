# Solution: de-vectorizing isp_media_server for the K230 scalar core

Companion to `isp-media-server-scalar-port-task.md`.

Status: **scalar binary generated and statically/emulator verified;
LCD and capture validation in progress on the scalar board**.

## Scope and evidence

The RVV daemon works at 720p60 on vector-capable hardware; the older scalar
daemon produces a black image with the otherwise identical setup. The exact
behavioral difference remains unknown. Missing 720p tables have not been
demonstrated, and fixing the older daemon has not been ruled out.

The chosen approach preserves the working RVV daemon's behavior by replacing
its vector zero-initialization instructions with scalar instructions. No
tuning content, applications, kernel, or nncase runtime needs modification.

Input: `buildroot-overlay/package/vvcam/isp_media_server`, SHA-256
`92d9a7b548a1e1233a82f728ee8320f82778383ce7f4e580b9cf54a4966d62a3`.
Its `.rodata` size is `0x2aa8e`, versus `0x2a65e` in the older scalar daemon:
a 1,072-byte difference. Previously quoted values `0x1aa108` and `0x1a6e20`
were section addresses, not sizes, and do not establish the data's purpose.

## Instruction inventory and mapping

The current input contains 1,572 vector instructions (including the 18
zero-VL configuration instructions; the review's 1,590 total was an arithmetic error):

| Instruction | Count | Intended replacement |
|---|---:|---|
| `vsetivli zero,8,e8,mf2,ta,ma` | 768 | `nop` |
| `vsetivli zero,0,e16/e32/e8,mf2,ta,ma` | 18 | `nop` |
| `vmv.v.i v1,0` | 96 | `nop` |
| `vse8.v v1,(a5/a4)` | 672 | Exactly eight zero bytes at the original destination |
| `vmv.x.s a5,v1` | 18 | `addi a5,zero,0` |

These mappings require verification of vector configuration, zero provenance,
and control flow at each site. A mnemonic count alone is not a semantic proof.
The original executable mixes 16-bit compressed and 32-bit instructions;
the vector instructions being replaced are all 32-bit. Preserve instruction
addresses, branch targets, and all bytes outside approved patches.

## Review findings and resolution

1. The old patcher overwrites three preceding instructions unconditionally
   for halfword expansion. At `0x147f52` and `0x173112` this destroys the
   address calculation and part of the preceding store. Expansions must use
   checked, nonoverlapping windows with preserved scalar effects. Resolved
   in the maintained package patcher with five explicit expansion windows.
2. The existing `isp_media_server_rvv_scalar` artifact still encodes `t0`
   instead of `a5` and retains one vector store. It is not suitable for use;
   use the newly generated `isp_media_server_scalar_v2` instead.
3. `experimental/resolve_final.py` reports 657 aligned, five misaligned,
   and ten unresolved stores (in `AEeGetStatus`). Its linear scan and assumed
   initial frame pointer are not a complete proof. The maintained patcher
   resolves all 672 destinations: 667 aligned, four halfword-aligned, one
   odd address. The ten large-frame destinations use `s0-4096-32-offset`.
   Frame-pointer reaching definitions are checked across direct control flow
   and the relevant nine-entry switch table; local address slices reject
   unknown definitions and branches. Function-entry stack alignment and
   callee preservation follow the RV64 ABI.
4. Verification prints failures without a failing exit status and writes
   output before checking it. The maintained patcher rejects unexpected
   hashes, instructions, incomplete analysis, and failed disassembly. Output
   is published only after verification succeeds; negative tests confirm
   failures leave existing output untouched.

## Misaligned stores

Prefer naturally aligned scalar stores; avoid introducing a dependency on
hardware/kernel handling of misaligned scalar accesses.

Four vector stores address `s0-42`, `s0-34`, `s0-382`, and `s0-374`.
Their expansions must preserve the base-register updates between adjacent
stores. Following configuration slots are available only if scalar extraction
and control-flow semantics are preserved.

The odd-address store at `0x1730ae`, with `a5=s0-437`, does not require a
trampoline. The four consecutive vector slots starting at `0x1730a2`
can hold these naturally aligned stores, subject to checking entry points:

```asm
sb zero,0(a5)
sw zero,1(a5)
sh zero,5(a5)
sb zero,7(a5)
```

This covers exactly eight bytes without changing a scalar register.

For the four halfword-aligned destinations, `sh 0; sw 2; sh 6` covers the
eight bytes. The second store in each pair uses the following configuration
slot for its third scalar store. The intervening scalar address calculation
and subsequent scalar extraction remain intact.

## Reproduction and integration

The maintained implementation and execution test live beside the vendor
binaries. The old experimental patcher is now a compatibility entry point.
Run from the repository root:

```sh
python3 -B buildroot-overlay/package/vvcam/devectorize_isp.py \
  buildroot-overlay/package/vvcam/isp_media_server \
  buildroot-overlay/package/vvcam/isp_media_server_scalar_v2
python3 -B buildroot-overlay/package/vvcam/test_devectorize_isp.py \
  buildroot-overlay/package/vvcam/isp_media_server_scalar_v2
```

Requirements: Python 3, SDK binutils at the default prefix (override with
`--tool-prefix`), and `qemu-riscv64-static` for the execution test. There is
no pyelftools dependency. The assembler runs with `-march=rv64gc` and
compression disabled; every replacement occupies its original four bytes.

Verified output SHA-256:
`82e01f8e47255939f27e25cd487430618aa4f877262ceb6924adbc8851b38c6b`.
All 1,572 vector instructions are replaced. File length, instruction
addresses, and every byte outside the approved vector slots are unchanged.
The emulator test executes the actual patched windows on a SiFive U54 CPU
model, checking exact zero coverage, guard bytes, and scalar register effects.

`vvcam.mk` selects `isp_media_server_scalar_v2` for scalar rootfs and Debian
packages. RVV selections remain `isp_media_server` and
`isp_media_server_debian`. Both selections were checked using make. Normal
SDK builds install the verified prebuilt artifact; regeneration is explicit
and refuses any vendor input whose hash differs from the pinned version.

## Board validation record (in progress)

- Board: `root@192.168.10.125`, Linux 6.6.36, hart 0, C908, ISA without V.
- Original daemon SHA-256:
  `4212115a233e72a389eb44e19de86329c6848a829c464c7db63f2200d2c2ca6b`.
  Backup: `/usr/bin/isp_media_server.before-scalar-port-4212115a`.
- Stopped original PID 527, checked it had exited, installed the verified
  candidate, and started it as PID 648. Installed hash matches the output.
- Baseline `TINYTAG_CV_DETECTOR=c ./run.sh --no-display` selected 720p60 but
  the installed `tinytag_detect.elf` itself raised SIGILL at relative address
  `0xb6d8c`, instruction `0x003572d7`. The ISP daemon remained alive. This
  occurred before replacing the daemon, so this test-harness failure is
  independent of the port. Application/runtime code has not been changed.
- The user is checking `v4l2-drm -d 1 -w 1280 -h 720 --rotation 1` on the
  LCD. That command sets the output geometry; physical sensor mode must also
  be verified, not inferred from output size alone.
- `experimental/isp_scalar_capture.c` is a separate validation utility. It
  uses the existing sensor-mode API without editing tuning files, captures
  NV12 through V4L2, reports sequence gaps/luminance/frame rate, and saves
  the last luma frame as PGM. Frame count zero selects a mode without capture.
  Built with the small-core SDK compiler using `-march=rv64gc -mabi=lp64d
  -O2 -Wall -Wextra` and `-lv4l2-drm -Wl,--no-as-needed -lm`, then staged as
  `/tmp/isp_scalar_capture`. Explicit libm loading is needed because the
  board's capability library has an otherwise unresolved `powf` symbol.
- `/tmp/isp_scalar_capture 720 600 /tmp/isp-scalar-720.pgm` selected physical
  1280x720@60 and negotiated NV12 with stride 1280. It captured 600 frames at
  approximately 56.34 fps including final image-writing overhead. Luma mean
  rose from 0.58 on the first frame to 106.33 at frame 60 and stabilized near
  107.49 by frame 599 (min 0, max 195). The saved frame clearly shows the
  room and a tag. The driver's sequence field remained zero throughout, so
  a dropped-frame count cannot be inferred. The diagnostic now reports this
  limitation and excludes final image-writing time from its FPS measurement.
- The unmodified upstream AprilTag C example, compiled with the small-core
  compiler against the existing scalar `libapriltag`, decoded the saved
  frame on the board: **tag36h11 ID 0, hamming 0, margin 94.707**. This is
  captured-frame validation, not a successful TinyTag live-loop test.
- The user reported no useful LCD image and a pink column with the initial
  plain `v4l2-drm` command. Capture results separate that symptom from black
  ISP output. Read `ov5647-720p-mode.md` and `native-720p-2026-09-02.md` before
  further investigation; tuning content remains untouched.
- Plain `v4l2-drm` cannot set a portrait destination rectangle independently
  of its source buffer. `experimental/isp_scalar_lcd_preview.c` uses the
  tag app's portrait DSI branch: display-derived landscape capture buffer,
  explicit 480x800 destination, 90-degree rotation, event-driven display
  updates capped at 30 fps. It rejects HDMI and is staged in `/tmp/`.
  This is a separate test tool, not a change to either tag application.
- **1080p30 regression PASS (2026-09-20)**: `isp_scalar_capture 1080 600 /tmp/isp-scalar-1080.pgm`
  → physical 1920x1080@30 selected; NV12 stride 1920; 600 frames at 28.25 fps
  (measurement includes a per-frame luma print); luma mean stable at ~108;
  exit 0. Full luma + saved PGM. Driver sequence counter constant (as in 720p),
  so dropped-frame count unavailable. Confirms the devectorized daemon retains
  the 1080p fallback path (no regression from scalarizing).

## Verification and delivery

- Pin the source hash and validate original instructions before patching.
  Establish zero provenance and alignment for every store, including the ten
  previously unresolved sites.
- Validate encodings against the toolchain assembler. Check replacement
  windows, scalar register effects, memory coverage, and incoming branches.
- Re-disassemble and reject remaining vector instructions or unexpected
  changes. Require identical length, ELF layout, and bytes outside approved
  instruction slots. Fail with a nonzero status; publish only verified output.
- Inspect `root@192.168.10.125` first. Confirm the core and back up/hash the
  installed daemon. Stop camera clients and the old daemon before replacement.
- Validate 1280x720@60 capture, exposure, preview, and AprilTag decoding;
  check 1080p for regressions. Absence of SIGILL does not establish good images.
- Integrate the verified artifact into Buildroot's scalar selection. Record
  hashes, commands, results, and unverified criteria here. Restore the original
  board daemon after testing per the task brief unless deployment is requested.

Static verification covers the patched instructions; hardware validation covers
exercised runtime paths. Neither a mnemonic count nor a smoke test proves the
entire closed-source application correct.
