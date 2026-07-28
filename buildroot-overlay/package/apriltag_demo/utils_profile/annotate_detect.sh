#!/bin/sh
# Annotate cycles inside a retained Rust detector stage using native objdump.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RESULT_DIR=${APRILTAG_PROFILE_OUTPUT:-"$SCRIPT_DIR/perf-results"}
DATA_FILE=${APRILTAG_PERF_DATA:-"$RESULT_DIR/rust.data"}
SYMBOL=${APRILTAG_PERF_SYMBOL:-apriltag_rvv::pipeline::detect}
STDIO=1

if [ "${1:-}" = "--interactive" ]; then
    STDIO=0
    shift
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--interactive]" >&2
    exit 2
fi
if ! command -v objdump >/dev/null 2>&1; then
    echo "objdump is missing; install target binutils first" >&2
    exit 1
fi
if [ ! -f "$DATA_FILE" ]; then
    echo "perf data not found: $DATA_FILE" >&2
    echo "Run profile_apriltag.sh first." >&2
    exit 1
fi

if [ "$STDIO" -eq 1 ]; then
    exec perf annotate -i "$DATA_FILE" --stdio --symbol="$SYMBOL"
else
    exec perf annotate -i "$DATA_FILE" --symbol="$SYMBOL"
fi
