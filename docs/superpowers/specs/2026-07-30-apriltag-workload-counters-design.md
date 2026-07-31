# AprilTag Workload Counters Design

**Date:** 2026-07-30
**Status:** Approved in conversation, pending implementation planning

## Goal

Add non-production workload counters to Rust and official C AprilTag detector
variants so performance differences can be separated into different amounts
of work versus different per-unit implementation cost.

Authoritative latency and perf measurements continue using pristine production
libraries. Counters are collected in separate untimed validation calls.

## Isolation

The package produces separate artifacts:

```text
k230_apriltag_bench
  production libapriltag_rvv.a
  production libapriltag.a

k230_apriltag_workload
  libapriltag_rvv_workload.a
  libapriltag_workload.a
```

The upstream C tarball and production library remain unchanged. A
version-pinned Buildroot patch adds counter support behind
`APRILTAG_WORKLOAD_COUNTERS`; only the workload library enables it. Rust uses a
`workload-counters` Cargo feature. Counter code compiles out of normal builds.

## Collection

Each backend runs twice on identical prepared input. Counters reset at detector
entry and are returned immediately afterward. The tool rejects nondeterministic
counter snapshots. Counter retrieval, formatting, and comparison are untimed.

Instrumentation uses plain local `u64` values and aggregates outside hot inner
loops where possible. The C counter build remains single-threaded.

## Initial Common Schema

```text
schema_version
input_width, input_height
decimated_width, decimated_height
decimated_checksum, threshold_checksum
threshold_black_pixels, threshold_uncertain_pixels, threshold_white_pixels

boundary_candidates, boundary_points_emitted
clusters_created, clusters_after_filters
cluster_reject_too_few_points, cluster_reject_too_large
cluster_reject_extent, cluster_reject_border_polarity
points_entering_sort, points_entering_lfps, points_entering_errors
compute_errors_calls, compute_errors_points
raw_peaks, retained_peaks
quad_fit_attempts, quads

quad_family_candidates, homography_rejects
family_polarity_rejects, decode_attempts
photometric_polarity_rejects, codeword_rejects
raw_detections, duplicate_rejects, detections
```

Comparable-with-metadata fields:

```text
uf_elements, uf_union_attempts, uf_union_successes
uf_granularity=run|pixel
line_fit_queries, line_fit_query_points
```

Rust-specific fields:

```text
rle_runs
pending_boundary_records
pending_boundary_expanded_points
rvv_runs_repacked
pair_stats_computed
pair_stats_span_points
pair_table_lookups
```

C-specific fields:

```text
active_uf_pixels
fit_line_calls
fit_line_span_points
cluster_hash_entries
```

Unavailable fields are explicitly marked invalid rather than set to zero.

## Early Polarity Estimate

Rust computes a counter-only polarity classification after basic point/extent
filters without changing production control flow. It records:

```text
hypothetical_early_polarity_reject_clusters
hypothetical_early_polarity_reject_points
hypothetical_wasted_sort_points
hypothetical_wasted_lfps_points
hypothetical_wasted_compute_errors_points
hypothetical_wasted_peak_search_clusters
```

C records actual early border-family polarity rejects before sorting. The
workload report estimates avoidable Rust fitting volume, not elapsed time.

## Decode Definitions

`decode_attempts` means actual image-sampling decode invocations. Homography
and family-polarity rejects remain separate. `raw_detections` is pre-dedup;
`detections` is final output.

## Output

The workload executable prints a readable side-by-side table plus:

```text
WORKLOAD backend=rust-rvv schema=1 ...
WORKLOAD backend=c-reference schema=1 ...
```

It highlights ratios for emitted points, retained clusters, points entering
sort/LFPS/errors, peaks, quads, and decode attempts. Implementation-specific
counters appear in separate labeled sections.

## Profiler Integration

`profile_detector.sh fast` and `full` run the workload executable once before
perf profiling and store:

```text
workload.log
workload-summary.txt
```

The consolidated summary combines sampled stage time with workload counts and
labels per-unit estimates as approximate.

## Interpretation

- More Rust points/clusters imply workload divergence or missing early reject.
- Similar workload with slower stages implies implementation/codegen cost.
- Large hypothetical polarity waste motivates moving rejection earlier.
- Large pending/repack ratios motivate interface fusion.
- Threshold checksum differences mean divergence predates CCL.

No optimization is made as part of this work.

## Verification

- Rust counter tests run in Docker for scalar and RVV paths.
- C patched and unpatched libraries produce identical detections.
- Production archive hashes remain unchanged when counter variants are built.
- Counter snapshots are deterministic on repeated calls.
- Common identities and implementation-specific validity flags are tested.
- Workload executable runs on JPEG and raw inputs at multiple resolutions.
- Buildroot packages workload and production tools together without symbol
  collision.
- Official C production demo and benchmark continue linking pristine library.
