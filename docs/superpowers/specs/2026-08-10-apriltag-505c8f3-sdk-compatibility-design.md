# AprilTag 505c8f3 SDK Compatibility Design

## Goal

Build `k230_linux_sdk_metalv` against `apriltag-rvv` branch
`dev/explicit-dma` at commit `505c8f3c69c3932a97a4c878a81982cbc5d68ff3`
without importing the unsuccessful partial-occlusion recovery experiment from
commit `60e5e4a`.

The K230 demo and benchmark package must retain the normal Rust detector, C
reference detector, detector benchmark, and schema-v1 workload comparison.

## Compatibility Boundary

Commit `505c8f3` provides the normal detector C ABI, debug stages 0 through 5,
and workload-counter schema v1. It does not provide recovery configuration,
recovery statistics, recovery candidates, recovered-detection metadata, debug
stage 6, workload schema v2, or `apriltag_get_workload_counters_v2`.

The SDK package will consume only APIs present at `505c8f3`. The
`apriltag-rvv` repository will remain unchanged.

## SDK Changes

Remove recovery-specific behavior from the K230 package:

- Remove recovery command-line options and runtime configuration.
- Limit live Rust debug selection to stages 0 through 5.
- Remove recovery statistics, candidate overlays, and recovered-result styling.
- Restore `apriltag_det_t` to the ABI emitted by `505c8f3`.
- Remove the recovery ABI integration executable and related build checks.

Restore the workload consumer to schema v1:

- Package the schema-v1 header from
  `apriltag-rvv/include/apriltag_workload.h`.
- Use the 480-byte `apriltag_workload_counters_t` layout.
- Call `apriltag_get_workload_counters` rather than the v2 sized-copy API.
- Remove report columns and tests that depend on schema-v2 boundary details,
  timers, provenance, and counterfactual dedup data.
- Retain the ordinary cross-backend workload report and schema-v1 counters.

Retain artifact freshness handling:

- Continue hashing relevant Rust sources separately for production and workload
  archives.
- Rebuild an archive when its source hash differs or its artifact is missing.
- Keep production and workload archives isolated.
- Copy the matching workload header atomically with the workload archive.

## Build Flow

1. Confirm `apriltag-rvv/dev/explicit-dma` resolves to `505c8f3`.
2. Build RVV-enabled production and workload static libraries in the existing
   `rvv-dev:latest` container.
3. Copy each archive and its matching source-hash stamp into the SDK package.
4. Clean and rebuild the Buildroot `apriltag_demo` package.
5. Run package-level ABI, formatting, and packaging checks.
6. Build the complete current SDK configuration, `k230_canmv_defconfig`.

## Error Handling

The build must fail rather than silently combining mismatched components:

- Packaging checks validate the schema-v1 symbol and header layout.
- The C++ workload adapter uses a compile-time size assertion.
- The Rust source hash forces stale archives to be regenerated.
- The package build ID identifies the exact `apriltag-rvv` commit.

## Verification

Verification covers:

- `dev/explicit-dma` is at `505c8f3`.
- Production and workload archives export their expected schema-v1 symbols.
- Production and workload source-hash stamps match current source hashes.
- Package tests pass without recovery or schema-v2 assumptions.
- `apriltag_demo`, `apriltag_c_demo`, detector benchmark, and workload benchmark
  link successfully.
- The built AprilTag binaries identify `505c8f3c69c3` in their build metadata.
- The full `k230_canmv_defconfig` image build exits successfully and produces
  the expected image output.

Existing unrelated worktree changes, including the modified
`k230_canmv_defconfig`, remain untouched and are included in the build exactly
as they currently exist.
