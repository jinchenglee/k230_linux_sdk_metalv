#!/bin/bash
set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for executable in production profile sequence workload demo c-demo; do
    : >"$TMP/$executable"
done

cat >"$TMP/nm" <<'EOF'
#!/bin/bash
case "${!#}" in
    */production)
        printf '%s\n' \
            '00000000 T apriltag_set_ccl_scratch_mode_v1' \
            '00000000 T apriltag_set_ccl_grouping_mode_v1_suffix'
        ;;
    */profile|*/sequence)
        printf '%s\n' \
            '00000000 T apriltag_get_ccl_profile_v1' \
            '00000000 T apriltag_get_ccl_pending_profile_v1' \
            '00000000 T apriltag_get_ccl_scratch_v1' \
            '00000000 T apriltag_set_ccl_scratch_mode_v1' \
            '00000000 T apriltag_get_ccl_grouping_profile_v1_suffix'
        ;;
    */demo)
        printf '%s\n' '00000000 T apriltag_set_ccl_scratch_mode_v1'
        ;;
    */workload) printf '%s\n' '00000000 T apriltag_get_workload_counters' ;;
esac
EOF

cat >"$TMP/strings" <<'EOF'
#!/bin/bash
case "$1" in
    */production) printf '%s\n' "$PRODUCTION_ID" ;;
    */profile) printf '%s\n' "$PROFILE_ID" ;;
    */sequence) printf '%s\n' "$SEQUENCE_ID" ;;
    */demo) printf '%s\n' '--local-ccl-scratch' 'ccl_scratch=' ;;
    */c-demo) printf '%s\n' '--local-ccl-scratch is only valid for apriltag_demo' ;;
esac
EOF
chmod +x "$TMP/nm" "$TMP/strings"

export PRODUCTION_ID='rvv-test_kernelabi-0123456789ab_cc-test'
export PROFILE_ID='profile-rvv-test_profile-0123456789ab_profileabi-0123456789ab_pendingabi-0123456789ab_scratchabi-0123456789ab_bufferabi-0123456789ab_kernelabi-0123456789ab_cc-test'
export SEQUENCE_ID='sequence-rvv-test_sequence-0123456789ab_profileabi-0123456789ab_pendingabi-0123456789ab_scratchabi-0123456789ab_bufferabi-0123456789ab_kernelabi-0123456789ab_cc-test'

cmake \
    -DPRODUCTION="$TMP/production" \
    -DPROFILE="$TMP/profile" \
    -DSEQUENCE="$TMP/sequence" \
    -DWORKLOAD="$TMP/workload" \
    -DDEMO="$TMP/demo" \
    -DC_DEMO="$TMP/c-demo" \
    -DNM="$TMP/nm" \
    -DSTRINGS="$TMP/strings" \
    -DPRODUCTION_ID="$PRODUCTION_ID" \
    -DPROFILE_ID="$PROFILE_ID" \
    -DSEQUENCE_ID="$SEQUENCE_ID" \
    -P "$PKG_DIR/tests/verify_benchmark_build.cmake"

printf '%s\n' '00000000 T apriltag_set_ccl_grouping_mode_v1' >>"$TMP/c-demo.nm"
cat >"$TMP/nm" <<'EOF'
#!/bin/bash
case "${!#}" in
    */production)
        printf '%s\n' \
            '00000000 T apriltag_set_ccl_scratch_mode_v1' \
            '00000000 T apriltag_set_ccl_grouping_mode_v1_suffix'
        ;;
    */profile|*/sequence)
        printf '%s\n' \
            '00000000 T apriltag_get_ccl_profile_v1' \
            '00000000 T apriltag_get_ccl_pending_profile_v1' \
            '00000000 T apriltag_get_ccl_scratch_v1' \
            '00000000 T apriltag_set_ccl_scratch_mode_v1' \
            '00000000 T apriltag_get_ccl_grouping_profile_v1_suffix'
        ;;
    */demo) printf '%s\n' '00000000 T apriltag_set_ccl_scratch_mode_v1' ;;
    */workload) printf '%s\n' '00000000 T apriltag_get_workload_counters' ;;
    */c-demo) cat "${!#}.nm" ;;
esac
EOF
if cmake \
    -DPRODUCTION="$TMP/production" \
    -DPROFILE="$TMP/profile" \
    -DSEQUENCE="$TMP/sequence" \
    -DWORKLOAD="$TMP/workload" \
    -DDEMO="$TMP/demo" \
    -DC_DEMO="$TMP/c-demo" \
    -DNM="$TMP/nm" \
    -DSTRINGS="$TMP/strings" \
    -DPRODUCTION_ID="$PRODUCTION_ID" \
    -DPROFILE_ID="$PROFILE_ID" \
    -DSEQUENCE_ID="$SEQUENCE_ID" \
    -P "$PKG_DIR/tests/verify_benchmark_build.cmake" >"$TMP/rejected.out" 2>&1; then
    echo "exact obsolete grouping symbol was accepted" >&2
    exit 1
fi
grep -q 'obsolete grouping symbol' "$TMP/rejected.out"
grep -q 'apriltag_set_ccl_grouping_mode_v1' "$TMP/rejected.out"

cat >"$TMP/nm" <<'EOF'
#!/bin/bash
case "${!#}" in
    */production)
        printf '%s\n' '00000000 T apriltag_set_ccl_scratch_mode_v1'
        ;;
    */profile|*/sequence)
        printf '%s\n' \
            '00000000 T apriltag_get_ccl_profile_v1' \
            '00000000 T apriltag_get_ccl_pending_profile_v1' \
            '00000000 T apriltag_get_ccl_scratch_v1' \
            '00000000 T apriltag_set_ccl_scratch_mode_v1'
        ;;
    */demo) printf '%s\n' '00000000 T apriltag_set_ccl_scratch_mode_v1' ;;
    */workload)
        printf '%s\n' \
            '00000000 T apriltag_get_workload_counters' \
            '00000000 T apriltag_get_ccl_pending_profile_v1'
        ;;
esac
EOF
if cmake \
    -DPRODUCTION="$TMP/production" -DPROFILE="$TMP/profile" \
    -DSEQUENCE="$TMP/sequence" -DWORKLOAD="$TMP/workload" \
    -DDEMO="$TMP/demo" -DC_DEMO="$TMP/c-demo" \
    -DNM="$TMP/nm" -DSTRINGS="$TMP/strings" \
    -DPRODUCTION_ID="$PRODUCTION_ID" -DPROFILE_ID="$PROFILE_ID" \
    -DSEQUENCE_ID="$SEQUENCE_ID" \
    -P "$PKG_DIR/tests/verify_benchmark_build.cmake" >"$TMP/pending-rejected.out" 2>&1; then
    echo "pending profile getter leaked into workload without rejection" >&2
    exit 1
fi
grep -q 'pending profile getter is not isolated' "$TMP/pending-rejected.out"

echo "apriltag_demo: benchmark build verifier self-test passed"
