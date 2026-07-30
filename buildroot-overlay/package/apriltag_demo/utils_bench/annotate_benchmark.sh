#!/bin/sh
set -eu

PERF=${APRILTAG_PROFILE_PERF:-perf}
interactive=0
if [ "${1:-}" = --interactive ]; then interactive=1; shift; fi
if [ "$#" -ne 2 ]; then
    echo "usage: $0 [--interactive] PERF_DATA SYMBOL" >&2
    exit 2
fi
data=$1
symbol=$2
[ -f "$data" ] || { echo "perf data not found: $data" >&2; exit 1; }
command -v objdump >/dev/null 2>&1 || { echo "objdump is required by perf annotate" >&2; exit 1; }
if [ "$interactive" -eq 1 ]; then
    exec "$PERF" annotate -i "$data" --symbol="$symbol"
fi
exec "$PERF" annotate -i "$data" --stdio --symbol="$symbol"
