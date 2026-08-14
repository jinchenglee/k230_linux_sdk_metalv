# AprilTag CCL Integration Checkpoint Summary

## Status

Seal the SDK side of the measured AprilTag CCL performance checkpoint. The SDK
now packages the accepted detector configuration, exposes production scratch
selection, provides deterministic sequence profiling, and verifies artifact and
ABI isolation from Rust source through the final K230 executables.

The detector-side performance rationale and final optimization decision live in
the corresponding `apriltag-rvv` decision records. This document summarizes SDK
integration and verification.

## Application Integration

The Rust live demo keeps reusable CCL scratch as its default and supports an
explicit local-scratch fallback for memory-constrained deployments. Startup
configuration is applied before detection and reports the selected mode. The C
backend rejects the Rust-only option rather than silently ignoring it.

The accepted production detector contains immutable-root grouping and reusable
pending/diagonal scratch. The rejected exact-count grouping mode, CLI, getter,
setter, header, telemetry, and build-identity component were removed.

## Sequence Benchmark

The SDK includes a one-handle sequence benchmark that:

- validates regular-file identities and SHA-256 values;
- rereads and rehashes each logical input before native decode;
- runs one cold/transition call and configurable warm repetitions;
- records exact detector count and checksum identity;
- retrieves CCL, pending-attribution, and scratch snapshots after each call;
- enforces same-call counter and timer conservation;
- rejects malformed, incomplete, stale, or inconsistent snapshots; and
- emits flat machine-readable `SEQUENCE` records.

The benchmark times detector execution only. File I/O, hashing, and image decode
remain outside the measured interval.

## Profile And ABI Packaging

Production, workload, and profile Rust archives remain separate. Packaging is
transactional and publishes the archive, source-hash stamp, and applicable
headers under one lock with rollback for both pre-existing and initially absent
artifacts.

The profile package includes versioned headers for:

- CCL phase and workload profiling;
- reusable scratch telemetry;
- shared buffer telemetry;
- pending-record attribution; and
- kernel-stage selection.

Pending-attribution source and headers affect only profile identity. Shared
production sources and headers affect each applicable archive. Packaging tests
cover mode-specific source hashes, clean publication, file modes, interrupted
publication, rollback, and missing-artifact freshness.

## Build Identity And Symbol Isolation

Production, profile, and sequence executables embed distinct content-derived
identities. Profile and sequence identities include profile, pending, scratch,
buffer, and kernel ABI hashes. Production identities exclude profile-only ABI
components.

Executable verification parses exact defined symbols and enforces:

- the pending-profile getter only in profile and sequence artifacts;
- profile/scratch getters only where supported;
- the production scratch setter in Rust production consumers;
- no Rust CCL APIs in the C demo; and
- no rejected grouping symbols or grouping ABI identity.

The workload executable is included in final symbol verification rather than
being inferred only from its archive.

## Board Workflow

The SDK sequence runner was exercised on four prescribed orders covering 18
physical inputs and 1,111 calls. The board logs passed strict checksum, schema,
grid, output, profile, scratch, histogram, outcome, and timer validation.

The consolidated two-input confirmation also passed with exact outputs, zero
warm scratch growth, and warm CCL means below the matching accepted baseline.

The final profiling run identified structural CCL opportunity but did not prove
removable production latency. No additional detector pipeline change is selected
for SDK integration at this checkpoint.

## Verification

The integrated checkpoint passed:

- sequence, demo-option, benchmark, profile, and packaging host tests;
- transactional publication and rollback tests;
- exact executable symbol-verifier tests;
- production/workload/profile archive rebuilds with current source stamps;
- profile-header identity and C/C++ layout checks;
- the full `apriltag_demo` Buildroot cross-build;
- benchmark and live-demo installation;
- Debian package generation; and
- board sequence validation for the consolidated and full-corpus runs.

## Privacy

Private input media, extracted frames, private manifests, and raw board logs are
not included in the SDK package or repository. Only aggregate results and public
artifact interfaces are documented.

## Next State

Keep the accepted production configuration and profiling infrastructure. Do not
add compact ranges, sort/radix grouping, arena-backed clusters, or RVV point
emission until a separately approved experiment demonstrates exact output and a
production latency improvement.
