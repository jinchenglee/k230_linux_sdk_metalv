#!/bin/sh
# Measure what a shared-memory exchange actually costs, split between the big
# core's cache maintenance and Linux's passes over the uncached window.
#
# Why this exists: the shared window is mapped through /dev/mem with O_SYNC, so
# Linux accesses it uncached. That cost is invisible in the big core's own
# timing, and it decides whether a payload may cross this transport at all --
# which is why RPMsg carries control and descriptors, and bulk data is passed
# by address instead of copied.
#
# Usage:
#   ./amp-shm-cost.sh                # poll and mailbox modes, 1 loop each
#   ./amp-shm-cost.sh --mailbox      # one mode only
#   ./amp-shm-cost.sh --mailbox 5    # 5 loops (medians are steadier)
#
# Run from /root/amp on a board whose big-core firmware is up. Needs no
# arguments in the common case.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TEST="$HERE/amp-shm-test"

if [ ! -x "$TEST" ]; then
    echo "amp-shm-cost: $TEST not found or not executable" >&2
    exit 1
fi

MODES="--poll --mailbox"
LOOPS=1
case "${1:-}" in
    --mailbox|--mailbox-poll) MODES="$1"; shift ;;
    --poll) MODES="--poll"; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
esac
[ $# -eq 0 ] || LOOPS="$1"

for mode in $MODES; do
    # amp-shm-test takes no flag for polling mode; it is the default.
    if [ "$mode" = "--poll" ]; then
        set -- "$LOOPS"
        label=poll
    else
        set -- "$mode" "$LOOPS"
        label="${mode#--}"
    fi

    echo "=============================================================="
    echo "notify mode: $label   loops: $LOOPS"
    echo "=============================================================="
    if ! "$TEST" "$@" >/tmp/amp-shm-cost.$$ 2>&1; then
        echo "amp-shm-cost: exchange failed in $label mode:" >&2
        tail -5 /tmp/amp-shm-cost.$$ >&2
        rm -f /tmp/amp-shm-cost.$$
        exit 1
    fi

    awk '
    /^PASS / {
        bytes=""; big=""; lin=""; exch="";
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^bytes=/)           { split($i, a, "="); bytes = a[2] }
            if ($i ~ /^big_core_window=/) { split($i, a, "="); big  = a[2] }
            if ($i ~ /^linux_side=/)      { split($i, a, "="); lin  = a[2] }
            if ($i ~ /^exchange=/)        { split($i, a, "="); exch = a[2] }
        }
        # Keep the last sample per size; loops repeat the same size list.
        if (bytes != "") { B[bytes]=big; L[bytes]=lin; E[bytes]=exch; seen[bytes]=1 }
        # Per-pass throughput, printed only for payloads >= 4 KiB.
        if (match($0, /MBps\{[^}]*\}/)) {
            mb = substr($0, RSTART+5, RLENGTH-6); M[bytes]=mb
        }
    }
    END {
        printf "%10s  %14s  %12s  %12s   %s\n",
               "bytes", "big_core (ms)", "linux (ms)", "total (ms)",
               "per-pass MB/s (uncached)";
        n = asorti(seen, idx, "@ind_num_asc");
        for (i = 1; i <= n; i++) {
            b = idx[i];
            printf "%10s  %14s  %12s  %12s   %s\n", b, B[b], L[b], E[b], M[b];
        }
    }' /tmp/amp-shm-cost.$$ 2>/dev/null || {
        # busybox awk has no asorti; fall back to unsorted output.
        awk '
        /^PASS / {
            bytes=""; big=""; lin=""; exch=""; mb="";
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^bytes=/)           { split($i, a, "="); bytes = a[2] }
                if ($i ~ /^big_core_window=/) { split($i, a, "="); big  = a[2] }
                if ($i ~ /^linux_side=/)      { split($i, a, "="); lin  = a[2] }
                if ($i ~ /^exchange=/)        { split($i, a, "="); exch = a[2] }
            }
            if (match($0, /MBps\{[^}]*\}/))
                mb = substr($0, RSTART+5, RLENGTH-6);
            printf "%10s  %14s  %12s  %12s   %s\n", bytes, big, lin, exch, mb;
        }' /tmp/amp-shm-cost.$$
    }
    rm -f /tmp/amp-shm-cost.$$
    echo
done

cat <<'NOTE'
Reading this table:
  big_core (ms)  invalidate + CRC + transform + CRC + clean on the 1.6 GHz core
  linux (ms)     fill + CRC + verify + CRC, all over the UNCACHED mapping
  total (ms)     what an exchange of that size really costs end to end

Per-exchange detail (the five big-core cycle counters and the four Linux
timings) is in the raw amp-shm-test output; run it directly to see it.

If the Linux column dominates, the cost is the uncached mapping rather than
cache maintenance -- expected, and the reason bulk data is passed by address
rather than copied through this window.
NOTE
