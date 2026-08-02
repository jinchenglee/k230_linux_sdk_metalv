# AprilTag Conservative Occlusion Recovery Design

**Date:** 2026-07-31
**Status:** Approved in conversation, pending implementation planning

## Goal

Recover close, partially occluded Tag36h11 tags only when the normal detector
has already produced a high-confidence recovery candidate. Bound additional
work with conservative geometric eligibility and a hard per-frame trial cap.

## Scope

Initial recovery considers only:

- **Group D:** clusters that pass normal point-count, extent, and polarity
  filters but fail line/quad fitting.
- **Group E:** geometrically valid quads that fail payload decoding.

The implementation excludes small-fragment multi-cluster recovery, temporal
tracking, broad whole-frame search, rolling statistical budgets, token buckets,
and RVV optimization.

## Exact Boundary Deduplication

Before recovery experiments, instrument and evaluate exact duplicate boundary
observations after final union-find representatives are available:

```text
key = ordered_component_pair + x + y + gx + gy
```

This distinguishes:

- exact duplicates in one final component-pair cluster;
- coincident coordinates with different gradients; and
- coincident coordinates belonging to different component pairs.

Production deduplication is adopted only after a counterfactual diagnostic run
shows that exact deduplication does not reduce valid recall or degrade quad and
pose geometry. C's traversal-specific `connected_last` suppression is not
copied blindly.

## Recovery Eligibility

Recovery is attempted only when all inexpensive gates pass:

1. Candidate belongs to Group D or E.
2. Apparent full-resolution tag extent exceeds a configurable minimum.
3. Boundary evidence produces at least three strong, spatially distinct line
   hypotheses in orientation/offset space.
4. Selected line support covers a sufficient fraction of the candidate.
5. Outlier fraction remains below a configured maximum.

Point count alone is not an apparent-distance criterion. The initial size gate
uses geometric extent, with a conservative default calibrated from recorded
fixtures rather than permanently assuming 24 points is sufficient.

## Line Hypotheses

Use local tangent observations from circularly ordered cluster points:

```text
tangent_i = point[i+k] - point[i-k]
theta_i   = tangent orientation
rho_i     = signed line offset from candidate center
```

A coarse theta histogram identifies the top five to eight orientations. Each
orientation is split by rho so parallel opposite tag edges remain distinct.
Line peaks are ranked by inlier support, visible length, straightness, and
gradient-normal agreement.

Group D recovery requires:

- four compatible observed lines; or
- three compatible observed lines with a stable inferred fourth edge.

Alien occluder edges are treated as outliers when they do not support the
selected projected-square geometry.

## Group E Erasure Decode

For a valid quad that fails decoding, classify payload cells as visible or
erased using local contrast, border consistency, and spatial concentration of
unreliable samples. Decode with bounded errors and erasures under the family
distance constraint:

```text
2 * errors + erasures < minimum code distance
```

Require a unique codeword and a safe distance margin. Recovery never turns all
uncertain cells into arbitrary black/white values.

## Hard Budget

Defaults:

```text
max Group D candidates considered per frame = 2
max Group E candidates considered per frame = 2
max actual recovery trials per frame         = 1
max retained line hypotheses                 = 8
```

Rank candidates by apparent size, four-line before three-line evidence, line
support, outlier fraction, and Group E codeword distance. Attempt only the
highest-ranked candidate. No rolling activation threshold is added.

## Result Semantics

Recovered detections are marked separately and expose:

```text
recovered
visible_edges
inferred_edges
erasure_count
corrected_bit_count
geometry_residual
recovery_group
```

Recovered pose confidence must be lower than a normal four-edge detection and
must reflect inferred edges/corners.

## Debug Visualization

Add debug stage 6, `Recovery`, alongside existing stages 0-5. It displays only
recovery candidates and geometry:

- dim gray candidate points;
- cyan line inliers;
- red outlier/occluder points;
- green observed lines;
- yellow inferred line;
- white intersections/corners;
- magenta recovered quad.

Labels include Group D/E, strong line count, points/inliers/outliers,
trial/skip state, recovery result, ID, errors, and erasures. If no candidate is
eligible, display `No eligible recovery candidates`.

Normal OSD page 0 distinguishes recovered detections with magenta edges and an
`R` label prefix.

## Profiling Counters

Collect without statistical activation logic:

```text
group_d_candidates
group_e_candidates
recovery_eligible
recovery_trials_attempted
recovery_trials_skipped_frame_cap
recovery_successes
recovery_failures
recovery_time_total_us
recovery_time_max_us
three_line_candidates
four_line_candidates
recovery_errors
recovery_erasures
```

Boundary diagnostics additionally record direction-specific checks,
candidates, qualified/emitted points, exact/coordinate duplicates, perimeter
points, and successful-detection source-cluster provenance. The profiler's CCL
annotation symbol is updated to `ccl_and_boundary_extract_impl`.

## Data Lifetime

Large recovery trace data is allocated only when recovery debug stage 6 is
enabled. Normal recovery retains bounded candidate summaries, counters, and the
final recovered detection. Production detection output remains deterministic
when recovery is disabled.

## Verification

- Synthetic exact-duplicate and diagonal-boundary cases.
- Counterfactual exact-dedup detection comparisons before production adoption.
- Synthetic three-line, four-line, alien-edge, and missing-edge clusters.
- Group E payload masks with known errors/erasures and distance bounds.
- Close-tag occlusion fixtures with ground-truth IDs and geometry.
- Non-occluded regression matrix across factors, strides, scalar, and RVV.
- Per-frame cap and ranking tests.
- Debug stage 6 image and normal OSD recovered styling tests.
- Performance measurements proving recovery-disabled overhead is negligible and
  recovery-enabled cost remains bounded by one trial per frame.
