# Small-core RVV pollution in tinytag_detect / apriltag_demo

Status: **root cause established; fix (A) implemented and verified on hardware
2026-09-06; fix (B) (libnncase) still open.** Investigated 2026-09-05
against `k230_canmv_v3_small_core_defconfig` on the CanMV-K230 V3 board at
192.168.10.125. Everything below is measured, not inferred; commands are
included so the numbers can be re-derived.

Companions: `docs/k230_dual_os_plan.md` (Phase 7),
`docs/notes/rvv-free-nncase-v2.11.0.md` (the nncase half).

## 1. Symptom

`tinytag_detect` dies with SIGILL immediately after camera negotiation. The
last line printed is always the same, which makes distinct failures look
identical:

```
[input] CSI requested 1280x720, negotiated 1280x720 stride=1280
tinytag_detect.[330]: unhandled signal 4 code 0x1 at 0x0000002abc77333a
  ... status: 0000000200004020 badaddr: 00000000cd817057 cause: 0000000000000002
Illegal instruction
```

The board is genuinely vector-free, so the trap is correct behaviour:

```
# ssh root@192.168.10.125 'head -5 /proc/cpuinfo'
isa   : rv64imafdc_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt
uarch : thead,c908
```

## 2. How to decode this class of trap (do not guess)

On RISC-V Linux, `cause: 0x2` is illegal-instruction and **`badaddr` holds the
faulting instruction word**, not an address. Decode it directly:

- `0xcd817057` -> opcode `0x57` = OP-V, funct3 `7` = vset*, bits[31:30] = `11`
  = `vsetivli` -> **`vsetivli zero,2,e64,m1,ta,ma`** (a 16-byte vector op).
- The second observed word `0x003572d7` -> **`vsetvli t0,a0,e8,m8,tu,mu`**.

Get the PIE load base from the dmesg line itself -- it prints
`tinytag_detect.elf[<base>+<size>]`. Subtract to get the file offset, then
resolve the symbol:

```sh
ELF=output/k230_canmv_v3_small_core_defconfig/build/tinytag_detect/buildroot-build/tinytag_detect.elf
OD=output/k230_canmv_v3_small_core_defconfig/host/bin/riscv64-unknown-linux-gnu-objdump
$OD -d "$ELF" > /tmp/tt.dis
# offset = epc - base ; e.g. 0x2acaea533a - 0x2acaa43000 = 0x46233a
awk '/^[0-9a-f]+ <.*>:$/{sym=$0} /^ +46233a:/{print sym; exit}' /tmp/tt.dis
```

Whole-binary attribution by symbol. Note objdump output is tab-separated and
awk has no `\s`, which is why this splits on `-F'\t'` and matches field 3:

```sh
awk -F'\t' '
/^[0-9a-f]+ <.*>:$/{sym=$0; sub(/^[0-9a-f]+ </,"",sym); sub(/>:$/,"",sym); next}
NF>=3 && $3 ~ /^v[a-z0-9._]+$/ {cnt[sym]++}
END{for(s in cnt) printf "%6d\t%s\n", cnt[s], s}' /tmp/tt.dis | sort -rn
```

## 3. Root cause: three independent RVV sources

Measured on the shipped binary (built Sep 5 2026 22:53:57), **9510 vector
instructions total**:

| Source | Vector insns | Trap site observed |
|---|---:|---|
| `libapriltag_rvv.a` **plus its bundled rust-std** (`std`, `core`, `alloc`, `miniz_oxide`, `backtrace`) | 7379 (595 symbols) | offset `0x46233a`, `apriltag_rvv::pipeline::DetectBuffers::new` (fn @ `0x462320`), called from `apriltag_new` (@ `0x3f831a`, ra offset `0x3f83a8`) |
| `libNncase.Runtime.Native.a` (distributed RVV archive) | ~2131 | offset `0xb6d8c`, label `loop1cpy_data5932`, inside `nncase::(anon)::slice_contiguous_impl<unsigned>` (@ `0xb6ad6`) |
| tinytag's own C++ | ~0 | -- (already fixed by `32d7492`) |

A **fourth** source exists but not in `tinytag_detect`: `apriltag_demo`'s own
C++ was compiled with hardcoded vector flags, giving `apriltag_demo.elf` and
`apriltag_c_demo.elf` (337 vector instructions) their own pollution. See
correction 4 below.

Top contributors for orientation: `apriltag_rvv::threshold::threshold_rvv` 420,
`apriltag_rvv::pipeline::decode_quad_detailed` 297,
`nncase::kernels::stackvm::optimized::binary` 256,
`nncase::kernels::stackvm::optimized::unary` 224,
`alloc::collections::btree` 159, `optimized_safe_softmax` 150,
`miniz_oxide::inflate::core::decompress` 135, `std::sys::fs::unix::copy` 100.

Two consequences of that table that are easy to get wrong:

- **A large share of the Rust total is rust-std itself**, not apriltag-rvv
  source. `scripts/build-capi.sh` uses `-Z build-std=std,panic_abort`, so std is
  recompiled with whatever `RUSTFLAGS` are in force -- `+v` by default, from
  `.cargo/config.toml`'s `[target.riscv64gc-unknown-linux-gnu]`. No change to
  the C/C++ compile flags can ever reach this code.
- **The nncase hits are not calls into a vector kernel.** `loop1cpy_data*` are
  GCC's auto-vectorized inline-`memcpy` expansion labels sitting inside an
  ordinary copy loop, so plain data movement traps.

## 4. Corrections to earlier claims

Three statements in the tree are wrong and cost time; they are corrected here
rather than edited in place, since they are part of committed history.

1. **`32d7492`'s attribution is backwards.** Its message records vector
   instructions dropping 9472 -> 2337 and says "the remainder comes from
   libapriltag_rvv.a, the Rust RVV crop decoder". The 2337 remainder was
   **libnncase**; `libapriltag_rvv` is the 7379. The commit's actual fix
   (gating `-mcpu=c908v -mrvv-*` on `BR2_RISCV_ISA_RVV` in
   `ai_demo_cml_common`) is correct and verified -- tinytag's own C++ now
   contributes ~0 vector instructions. It simply never addressed either archive.

2. **`TINYTAG_CV_DETECTOR=c` is not a workaround.** `make_crop_decoder()`
   (`tag_crop_decoder.cc:138`) defaults to `AprilTagRVVDecoder` when the
   variable is unset -- see `tag_crop_decoder.h:112-114`, "unset/`rvv` ->
   AprilTagRVVDecoder (production default)". Its constructor
   (`tag_crop_decoder.cc:96`) calls `apriltag_new()` eagerly, which is exactly
   the `ra` in the traceback. Setting `=c` only *moves* the trap into libnncase.
   Verified on hardware -- both configurations SIGILL, and because both logs end
   at the identical `[input] CSI requested ...` line, a `=c` run is easily
   misread as a pass. Always confirm against `dmesg | grep 'unhandled signal'`.

3. **`apriltag_c_demo.elf` does not link `libapriltag_rvv.a`** -- it links
   `${APRILTAG_C_LIB}` = the staged `libapriltag.a` from the upstream `apriltag`
   package (`CMakeLists.txt:254`, `:122`), a plain `cmake-package` using
   Buildroot's own `BR2_TARGET_OPTIMIZATION`. Only `apriltag_demo.elf` and
   `tinytag_detect.elf` link the Rust archive. `apriltag_demo.elf` does not link
   libnncase, so it needs only fix (A); `tinytag_detect.elf` needs both.

   **But that did not make `apriltag_c_demo.elf` scalar** -- an earlier revision
   of this note claimed it did. Measured, it carried **337 vector
   instructions**, all in apriltag_demo's own C++ (`draw_detections`,
   `draw_camera_frame`, `draw_debug_image`, `main`). See source (4) below.

4. **A fourth source: apriltag_demo's own C++ flags.**
   `buildroot-overlay/package/apriltag_demo/CMakeLists.txt` appended
   `-mcpu=c908v -mabi=lp64d -mtune=c908 -mrvv-v0p10-compatible
   -mrvv-auto-vectorize` to `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`
   unconditionally -- the exact bug `32d7492` fixed in `ai_demo_cml_common`,
   which was never applied to this package. Because those flags land after
   `BR2_TARGET_OPTIMIZATION`, they override the defconfig's `-mcpu=c908`.
   It affects **both** `apriltag_demo.elf` and `apriltag_c_demo.elf`, and is the
   only source that reaches `apriltag_c_demo.elf` at all. Fixed by gating on
   `BR2_RISCV_ISA_RVV`, passed from `apriltag_demo.mk` as a CMake cache
   variable, mirroring `32d7492`.

5. **Audit the build-tree ELF, not the installed one.** The copies under
   `target/root/app/` are stripped, so per-symbol attribution degrades to
   dynamic symbols (`main@@Base-0x4480`) and tells you nothing about origin.
   Instruction totals are still valid on a stripped binary.

## 5. The libnncase scalar archive was solved, but is not durable

`docs/notes/rvv-free-nncase-v2.11.0.md` section 15 documents a working,
verified scalar `libNncase.Runtime.Native.a`. That work was real. Section 15.3
also states the swap is "local to the output tree and not tracked by the build
system: `make CONF=$SC libnncase-dirclean` silently restores the distributed RVV
archive."

That is what happened here. The archive staged in the current build tree is the
RVV one:

```sh
$OD -d output/k230_canmv_v3_small_core_defconfig/build/libnncase/nncase/lib/libNncase.Runtime.Native.a \
 | awk -F'\t' 'NF>=3 && $3 ~ /^v[a-z0-9._]+$/{n++} END{print n+0}'
# -> 2001
```

**Check this first** before re-debugging any small-core nncase SIGILL. Section
13's packaged small-core libnncase variant remains the real fix.

## 6. Fix (A): apriltag_rvv and the apriltag_demo C++ flags -- DONE

Selection follows `BR2_RISCV_ISA_RVV`; no new Kconfig symbol. The scalar path
already exists end to end and needs wiring, not new code: every vector path in
the crate is gated behind `#[cfg(target_feature = "v")]`
(`src/threshold.rs`, `decimate.rs`, `rle.rs`, `lfps.rs`, ...),
`scripts/build-capi.sh` accepts `--no-rvv`, and the SDK's
`scripts/build_rust_lib.sh` already forwards it. `--no-rvv` builds into a
separate `--target-dir` (`capi-production/scalar` vs `.../rvv`) so std is
rebuilt scalar too.

1. `apriltag_demo.mk`: set `APRILTAG_DEMO_RVV_VARIANT` = `rvv`/`scalar` and
   `APRILTAG_DEMO_RVV_ARGS` = ``/`--no-rvv` from `BR2_RISCV_ISA_RVV`; append the
   args to all three `build_rust_lib.sh` calls in
   `APRILTAG_DEMO_BUILD_RUST_LIB` (production, `--workload-only`,
   `--profile-only`).
2. `scripts/rust_source_hash.sh`: **take the variant as a third argument and
   fold it into the digest.** Without this the knob is decorative -- the script
   currently hashes sources plus mode only, the stamp is
   `.apriltag_rvv.source-hash` regardless of variant, so flipping the defconfig
   leaves a matching hash and the wrong archive is silently reused. Pass the
   variant at all four call sites (three per-mode hashes plus the combined one).
3. `apriltag-rvv/scripts/build-capi.sh`: the `--no-rvv` branch runs
   `RUSTFLAGS="" ...`, and `RUSTFLAGS` (even empty) overrides
   `.cargo/config.toml` wholesale -- so the scalar build also loses
   `-C symbol-mangling-version=legacy`, which that config file keeps because
   "K230 Linux 6.6 perf does not demangle Rust's v0 `_R` symbols". Set
   `RUSTFLAGS="-Z unstable-options -C symbol-mangling-version=legacy"` instead.
   Note this edit is in the **other repo**, `/work/git_repo/apriltag-rvv`.
4. Post-install audit hook, active only when `BR2_RISCV_ISA_RVV` is unset:
   `$(TARGET_OBJDUMP) -d` the installed ELFs and fail on any vector mnemonic.
   `tinytag_detect` is an `ai_demo` package built after `apriltag_demo`, so its
   check belongs in `tinytag_detect.mk`, sharing one script under
   `apriltag_demo/scripts/`. Report counts per source library so the
   apriltag_rvv and libnncase causes stay distinguishable.

5. `apriltag_demo/CMakeLists.txt`: gate the hardcoded `-mcpu=c908v
   -mrvv-auto-vectorize` flags on `BR2_RISCV_ISA_RVV`, passed from
   `apriltag_demo.mk` as `-DBR2_RISCV_ISA_RVV=ON|OFF` (source 4 above).

### Result, measured 2026-09-06

Rebuilt with `make CONF=k230_canmv_v3_small_core_defconfig apriltag_demo-dirclean
apriltag_demo` (exit 0):

| Binary | Vector insns before | After |
|---|---:|---:|
| `apriltag_demo.elf` | 7840 | **0** |
| `apriltag_c_demo.elf` | 337 | **0** |
| `lib/libapriltag_rvv.a` | (RVV) | **0** |

On the board at 192.168.10.125, both binaries run headless with no SIGILL
(`dmesg | grep -c "unhandled signal"` = 0). `apriltag_demo.elf --rvv --factor 2`
-- the Rust detector, i.e. the exact path that used to trap in
`DetectBuffers::new` -- sustains camera ~29-34 fps, detect ~5.8-6.9 fps, tags
decoded on 100% of frames. `apriltag_c_demo.elf` sustains camera ~27-28 fps,
detect ~5.5 fps. Scalar throughput on the 800 MHz core, as expected.

Docker: `apriltag-rvv` builds inside `rvv-dev:latest` (present locally).
`build_rust_lib.sh` already reproduces the `rvv-shell` invocation --
`-u $(id -u):$(id -g)`, `/etc/passwd` + `/etc/group` read-only, shared
`~/.cargo/{registry,git}` mounts, `HOME=$APRILTAG_RVV_DIR` -- and mounts the
common *parent* directory so the `../async-rvv` path dependency resolves.
`--no-rvv` rides through as an extra argument; the docker side needs no change.

`APRILTAG_DEMO_RVV_DIR` defaults to `$(TOPDIR)/../../../apriltag-rvv`, resolving
to **`/work/git_repo/apriltag-rvv`** (not `/work/git_hub/...`).

## 7. Fix (B): libnncase

Section 13 of `rvv-free-nncase-v2.11.0.md` -- a packaged small-core libnncase
variant that survives `dirclean`. Not designed yet. This is Phase 7 work.

**Fix (A) alone will not make `tinytag_detect` run**; it relocates the SIGILL
into nncase. It should make `apriltag_demo.elf` run, since that binary does not
link libnncase. Both fixes are required for `tinytag_detect`.

## 8. Open decision

With a scalar `libapriltag_rvv.a`, `make_crop_decoder()` still defaults to
`AprilTagRVVDecoder`, so the small core would run the Rust detector's scalar
path rather than the C detector. Functionally fine, slower than
`AprilTagCDecoder`. Whether the default should follow the ISA is a behaviour
choice, deliberately not bundled into the build fix.

## 9. Reproduction on the board

`timeout` is not present on the target; bound runs from the client side.

```sh
ssh root@192.168.10.125 '
cd /root/app/tinytag_detect
./run.sh --no-display > /tmp/tt.log 2>&1 & p=$!
sleep 15; kill -INT $p 2>/dev/null; sleep 2; kill -9 $p 2>/dev/null
tail -4 /tmp/tt.log; dmesg | grep "unhandled signal" | tail -2'
```

`run.sh` supplies six fixed positional arguments and appends anything passed.
Use `--no-display` to keep DRM/OSD out of the picture. The SSH host key changed
after the reflash; `known_hosts` was intentionally left untouched.
