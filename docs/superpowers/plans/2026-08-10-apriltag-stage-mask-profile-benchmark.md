# AprilTag Stage Mask and Profile Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add canonical per-stage RVV mask configuration and metadata to the production benchmark, plus a Rust-only profile benchmark that aggregates CCL timing and workload records from measured calls.

**Architecture:** Package the Rust-owned kernel mode header as a shared generated input in every archive transaction, and consume it from the C ABI and benchmark parser. Keep one benchmark core and backend interface; profile builds enable an optional profile snapshot hook and formatter while linking only the profile Rust archive. Production links remain free of the profile getter.

**Tech Stack:** C++17, CMake, POSIX shell, Buildroot package hooks, Rust static C ABI archives, host/cross tests, `nm`/`cmp` artifact verification.

---

### Task 1: Package the shared kernel mode ABI header

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/rust_source_hash.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/build_rust_lib.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`
- Modify: `buildroot-overlay/package/apriltag_demo/scripts/test_rust_packaging.sh`
- Modify: `buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh`
- Generate: `buildroot-overlay/package/apriltag_demo/lib/apriltag_kernel_modes.h`

- [ ] Add failing fixture tests proving `apriltag_kernel_modes.h` changes every mode hash, is copied byte-for-byte, participates in each transactional publication, and is required by Buildroot freshness checks.
- [ ] Run `bash buildroot-overlay/package/apriltag_demo/tests/verify_rust_packaging.sh`; expect failure because the general header is not packaged.
- [ ] Extend hashing and publication so every production/workload/profile set atomically publishes the same Rust header before its stamp.
- [ ] Run the packaging test and header parity checks; expect pass.

### Task 2: Parse and validate canonical stage masks

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/src/apriltag.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] Add parser tests for `all`, `none`, and comma lists in canonical `decimate,threshold,rle,lfps-tuned,gaussian,gray-model` order; unknown, empty, and duplicate names fail.
- [ ] Add configuration tests proving explicit masks apply to `rust-rvv`, turn backend `all` into masked Rust plus C, reject C-only and `rust-scalar`, and remain order-independent relative to `--backend`.
- [ ] Run host core tests; expect parser failures.
- [ ] Add mask fields/helpers, include the packaged Rust ABI header from `apriltag.h`, and implement final configuration validation after argument parsing.
- [ ] Run host core tests; expect pass.

### Task 3: Apply masks and report production metadata

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/rust_backend.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] Add fake-backend output tests for human stage configuration and `RESULT rvv_mask=/stages=` fields, including `n/a` on C.
- [ ] Add backend construction coverage that observes setter success/failure with a host mock where practical.
- [ ] Run benchmark tests; expect metadata and setter failures.
- [ ] Call `apriltag_set_kernel_mask_v1` only for explicit Rust RVV masks; preserve uniform RVV/scalar behavior otherwise. Format canonical metadata for all records.
- [ ] Run benchmark tests; expect pass.

### Task 4: Collect and aggregate measured profile snapshots

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/profile_backend.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/profile_format.cc`
- Create: `buildroot-overlay/package/apriltag_demo/bench/profile_tests.cc`

- [ ] Add mock tests proving snapshots are consumed after each measured detect only, never validation/warmup, and getter errors abort with call context.
- [ ] Add formatter tests for every CCL timing field as `STAGE` records with count/min/median/mean/p95/max, and all workload counters as deterministic `CCL_WORK` latest-value records with cross-call stability validation.
- [ ] Run profile host tests; expect missing collector/formatter failures.
- [ ] Add an optional backend `consume_profile()` interface, measured-loop collection, Rust profile getter implementation, timing aggregation, and complete stable-counter formatting.
- [ ] Run profile host tests; expect pass.

### Task 5: Build, link, and install distinct executables

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/benchmark.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/tests.cc`

- [ ] Add build/config tests proving profile mode rejects C and defaults to Rust only.
- [ ] Add `k230_apriltag_profile_bench`, compile profile sources/macro, link `libapriltag_rvv_profile.a` without the C detector, and install beside `k230_apriltag_bench`.
- [ ] Keep production benchmark linked to `libapriltag_rvv.a` and add profile tests to the build.
- [ ] Build host/cross test targets; expect all tests pass.

### Task 6: Rebuild and verify package artifacts

**Files:**
- Update generated package archives, headers, and source-hash stamps under `buildroot-overlay/package/apriltag_demo/lib/`.

- [ ] Rebuild with `APRILTAG_DEMO_RVV_DIR=/mnt/sda_500gb/git_repo/apriltag-rvv` through the package target.
- [ ] Run benchmark, profile, and packaging tests.
- [ ] Verify `cmp` parity for both generated Rust headers and verify archive/executable symbols with `nm`/`readelf`: profile getter exists only in profile artifacts, never production archive/binary.
- [ ] Inspect `git diff` and `git status`, retaining all unrelated pre-existing changes and making no commit.
