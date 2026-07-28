#!/bin/sh
#
# Compare the packaged Rust/RVV and upstream C AprilTag detectors under perf.
#
# Optional environment:
#   APRILTAG_PROFILE_SECONDS  Duration of each perf phase (default: 15)
#   APRILTAG_PROFILE_WARMUP   Warm-up time before profiling (default: 3)
#   APRILTAG_PROFILE_OUTPUT   Result directory (default: ./perf-results)
#   APRILTAG_COMMON_ARGS      Arguments passed to both applications
#   APRILTAG_RUST_ARGS        Additional Rust arguments (default: --rvv)
#   APRILTAG_C_ARGS           Additional C arguments (default: empty)
#   APRILTAG_RUST_BIN         Override the Rust executable path
#   APRILTAG_C_BIN            Override the C executable path
#   APRILTAG_PROFILE_SCOPE    "detect" (default) or "process"
#
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_ROOT=$(dirname -- "$SCRIPT_DIR")

RUST_BIN=${APRILTAG_RUST_BIN:-"$APP_ROOT/apriltag_demo/apriltag_demo.elf"}
C_BIN=${APRILTAG_C_BIN:-"$APP_ROOT/apriltag_c_demo/apriltag_c_demo.elf"}
RESULT_DIR=${APRILTAG_PROFILE_OUTPUT:-"$SCRIPT_DIR/perf-results"}
PROFILE_SECONDS=${APRILTAG_PROFILE_SECONDS:-15}
WARMUP_SECONDS=${APRILTAG_PROFILE_WARMUP:-3}
COMMON_ARGS=${APRILTAG_COMMON_ARGS:-}
RUST_ARGS=${APRILTAG_RUST_ARGS:---rvv}
C_ARGS=${APRILTAG_C_ARGS:-}
STAT_EVENTS=${APRILTAG_PERF_EVENTS:-task-clock,cycles,instructions,branches,branch-misses}
PROFILE_SCOPE=${APRILTAG_PROFILE_SCOPE:-detect}

case "$PROFILE_SECONDS" in
    ''|*[!0-9]*) echo "APRILTAG_PROFILE_SECONDS must be a positive integer" >&2; exit 2 ;;
    0) echo "APRILTAG_PROFILE_SECONDS must be greater than zero" >&2; exit 2 ;;
esac
case "$WARMUP_SECONDS" in
    ''|*[!0-9]*) echo "APRILTAG_PROFILE_WARMUP must be a non-negative integer" >&2; exit 2 ;;
esac
case "$PROFILE_SCOPE" in
    detect|process) ;;
    *) echo "APRILTAG_PROFILE_SCOPE must be detect or process" >&2; exit 2 ;;
esac

for required in perf awk tee; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Required profiling command is missing: $required" >&2
        exit 1
    fi
done
if [ ! -x "$RUST_BIN" ]; then
    echo "Rust demo is not executable: $RUST_BIN" >&2
    exit 1
fi
if [ ! -x "$C_BIN" ]; then
    echo "C demo is not executable: $C_BIN" >&2
    exit 1
fi

warn_if_symbols_missing()
{
    binary=$1
    expected_symbol=$2
    if command -v nm >/dev/null 2>&1 &&
       ! nm -C "$binary" 2>/dev/null | grep -F -q "$expected_symbol"; then
        echo "Warning: $binary lacks profiling symbol '$expected_symbol'." >&2
        echo "The ELF may have been stripped; perf will report raw addresses." >&2
    fi
}

warn_if_symbols_missing "$RUST_BIN" "apriltag_rvv::pipeline::detect"
warn_if_symbols_missing "$C_BIN" "do_gradient_clusters"
if command -v nm >/dev/null 2>&1; then
    if nm -C "$RUST_BIN" 2>/dev/null |
       grep -F -q "apriltag_rvv::pipeline::search_peak_quad"; then
        echo "Rust detector stage symbols: present"
    else
        echo "Warning: expected Rust detector stage symbols are missing." >&2
    fi
fi

mkdir -p "$RESULT_DIR"
TIMEOUT_MS=$((PROFILE_SECONDS * 1000))
ACTIVE_PID=
ACTIVE_FIFO=
ACTIVE_OUTPUT_FIFO=
ACTIVE_TEE_PID=
FIFO_OPEN=0

cleanup()
{
    if [ -n "$ACTIVE_PID" ] && kill -0 "$ACTIVE_PID" 2>/dev/null; then
        if [ "$FIFO_OPEN" -eq 1 ]; then
            printf 'q' >&3 2>/dev/null || true
        fi
        kill "$ACTIVE_PID" 2>/dev/null || true
        wait "$ACTIVE_PID" 2>/dev/null || true
    fi
    if [ -n "$ACTIVE_TEE_PID" ]; then
        kill "$ACTIVE_TEE_PID" 2>/dev/null || true
        wait "$ACTIVE_TEE_PID" 2>/dev/null || true
    fi
    if [ "$FIFO_OPEN" -eq 1 ]; then
        exec 3>&-
        FIFO_OPEN=0
    fi
    if [ -n "$ACTIVE_FIFO" ]; then
        rm -f "$ACTIVE_FIFO"
    fi
    if [ -n "$ACTIVE_OUTPUT_FIFO" ]; then
        rm -f "$ACTIVE_OUTPUT_FIFO"
    fi
}
trap cleanup EXIT HUP INT TERM

print_mean_fps()
{
    log_file=$1
    tr '\r' '\n' < "$log_file" |
        awk -F'detect: ' '
            NF > 1 {
                split($2, value, /[^0-9.]/)
                if (value[1] != "") {
                    sum += value[1]
                    count++
                }
            }
            END {
                if (count)
                    printf "%.3f FPS (%d samples)\n", sum / count, count
                else
                    print "unavailable (no detector FPS samples)"
            }
        '
}

find_detect_tid()
{
    process_pid=$1
    for task_dir in /proc/"$process_pid"/task/*; do
        [ -r "$task_dir/comm" ] || continue
        if [ "$(cat "$task_dir/comm")" = "apriltag-detect" ]; then
            basename "$task_dir"
            return 0
        fi
    done
    return 1
}

run_profile()
{
    label=$1
    binary=$2
    extra_args=$3
    log_file="$RESULT_DIR/$label.log"
    stat_file="$RESULT_DIR/$label.stat"
    data_file="$RESULT_DIR/$label.data"
    report_file="$RESULT_DIR/$label.report"
    ACTIVE_FIFO="$RESULT_DIR/.$label.stdin"
    ACTIVE_OUTPUT_FIFO="$RESULT_DIR/.$label.output"

    rm -f "$ACTIVE_FIFO" "$ACTIVE_OUTPUT_FIFO" \
        "$log_file" "$stat_file" "$data_file" "$report_file"
    mkfifo "$ACTIVE_FIFO" "$ACTIVE_OUTPUT_FIFO"
    exec 3<> "$ACTIVE_FIFO"
    FIFO_OPEN=1

    echo "Starting $label..."
    tee "$log_file" <"$ACTIVE_OUTPUT_FIFO" &
    ACTIVE_TEE_PID=$!
    # Intentional word splitting permits shell-style argument lists in the
    # documented APRILTAG_*_ARGS environment variables.
    # shellcheck disable=SC2086
    "$binary" $COMMON_ARGS $extra_args \
        <"$ACTIVE_FIFO" >"$ACTIVE_OUTPUT_FIFO" 2>&1 &
    ACTIVE_PID=$!

    sleep "$WARMUP_SECONDS"
    if ! kill -0 "$ACTIVE_PID" 2>/dev/null; then
        echo "$label exited during warm-up:" >&2
        tail -n 40 "$log_file" >&2 || true
        return 1
    fi

    if [ "$PROFILE_SCOPE" = "detect" ]; then
        PROFILE_TID=$(find_detect_tid "$ACTIVE_PID" || true)
        if [ -z "$PROFILE_TID" ]; then
            echo "Cannot find the apriltag-detect thread in process $ACTIVE_PID." >&2
            echo "Use a newly built demo or APRILTAG_PROFILE_SCOPE=process." >&2
            return 1
        fi
        echo "Profiling $label detection thread TID $PROFILE_TID"
        perf stat -t "$PROFILE_TID" --timeout "$TIMEOUT_MS" \
            -e "$STAT_EVENTS" -o "$stat_file"
        perf record -q -t "$PROFILE_TID" -e cycles:u \
            -o "$data_file" -- sleep "$PROFILE_SECONDS"
    else
        echo "Profiling $label whole process PID $ACTIVE_PID"
        perf stat -p "$ACTIVE_PID" --timeout "$TIMEOUT_MS" \
            -e "$STAT_EVENTS" -o "$stat_file"
        perf record -q -p "$ACTIVE_PID" -e cycles:u \
            -o "$data_file" -- sleep "$PROFILE_SECONDS"
    fi

    printf 'q' >&3
    wait "$ACTIVE_PID" 2>/dev/null || true
    ACTIVE_PID=
    wait "$ACTIVE_TEE_PID" 2>/dev/null || true
    ACTIVE_TEE_PID=
    exec 3>&-
    FIFO_OPEN=0
    rm -f "$ACTIVE_FIFO" "$ACTIVE_OUTPUT_FIFO"
    ACTIVE_FIFO=
    ACTIVE_OUTPUT_FIFO=

    # Symbolization is CPU-intensive on the single application core. Generate
    # the report only after the live demo has stopped so it cannot depress the
    # FPS samples collected from the application log.
    perf report -i "$data_file" --stdio \
        --sort=comm,dso,symbol --percent-limit=0.5 >"$report_file"

    printf '\n===== %s perf stat =====\n' "$label"
    cat "$stat_file"
    printf '\n===== %s mean detector FPS =====\n' "$label"
    print_mean_fps "$log_file"
    printf '\n===== %s top functions =====\n' "$label"
    cat "$report_file"
    printf '\n'
}

run_profile rust "$RUST_BIN" "$RUST_ARGS"
run_profile c "$C_BIN" "$C_ARGS"

echo "Full results are under $RESULT_DIR"
