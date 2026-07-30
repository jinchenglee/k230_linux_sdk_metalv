#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH="$APP_DIR/k230_apriltag_bench"
FIXTURE="$APP_DIR/fixture.jpg"
STAMP=$(date +%Y%m%d-%H%M%S)
RESULT_DIR=${APRILTAG_BENCH_OUTPUT:-"$APP_DIR/results/$STAMP"}
ACTIVE_FIFO=
ACTIVE_TEE_PID=

cleanup()
{
    if [ -n "$ACTIVE_TEE_PID" ]; then
        kill "$ACTIVE_TEE_PID" 2>/dev/null || true
        wait "$ACTIVE_TEE_PID" 2>/dev/null || true
        ACTIVE_TEE_PID=
    fi
    if [ -n "$ACTIVE_FIFO" ]; then
        rm -f "$ACTIVE_FIFO"
        ACTIVE_FIFO=
    fi
}

trap cleanup 0
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

run_logged()
{
    log_file=$1
    shift

    fifo_index=0
    while :; do
        ACTIVE_FIFO="$log_file.fifo.$$.${fifo_index}"
        [ -e "$ACTIVE_FIFO" ] || break
        fifo_index=$((fifo_index + 1))
    done
    mkfifo "$ACTIVE_FIFO"

    tee "$log_file" <"$ACTIVE_FIFO" &
    ACTIVE_TEE_PID=$!
    set +e
    "$@" >"$ACTIVE_FIFO" 2>&1
    command_rc=$?
    wait "$ACTIVE_TEE_PID"
    tee_rc=$?
    ACTIVE_TEE_PID=
    rm -f "$ACTIVE_FIFO"
    ACTIVE_FIFO=
    set -e
    if [ "$command_rc" -ne 0 ]; then
        return "$command_rc"
    fi
    return "$tee_rc"
}

run_logged_self_test()
{
    test_dir=${TMPDIR:-/tmp}/run-benchmark-self-test.$$
    test_index=0
    while [ -e "$test_dir.$test_index" ]; do
        test_index=$((test_index + 1))
    done
    test_dir=$test_dir.$test_index
    mkdir "$test_dir"

    run_logged "$test_dir/test.log" sh -c \
        'printf "first-line\n"; sleep 2; printf "second-line\n"; exit 7' \
        >"$test_dir/output" &
    test_pid=$!
    sleep 1

    live_ok=0
    if grep -q '^first-line$' "$test_dir/output" &&
       ! grep -q '^second-line$' "$test_dir/output" &&
       kill -0 "$test_pid" 2>/dev/null; then
        live_ok=1
    fi

    set +e
    wait "$test_pid"
    test_rc=$?
    set -e

    final_ok=0
    if grep -q '^second-line$' "$test_dir/output" &&
       cmp -s "$test_dir/test.log" "$test_dir/output"; then
        final_ok=1
    fi
    rm -f "$test_dir/test.log" "$test_dir/output"
    rmdir "$test_dir"

    if [ "$live_ok" -ne 1 ] || [ "$final_ok" -ne 1 ] || [ "$test_rc" -ne 7 ]; then
        echo "run_logged self-test failed: live=$live_ok final=$final_ok rc=$test_rc" >&2
        return 1
    fi
    echo "run_logged self-test passed: live output observed, log matched, rc=7"
}

if [ "${1:-}" = "--self-test" ]; then
    run_logged_self_test
    exit $?
fi

if [ ! -x "$BENCH" ]; then
    echo "error: benchmark executable not found: $BENCH" >&2
    exit 1
fi
if [ ! -f "$FIXTURE" ]; then
    echo "error: default fixture not found: $FIXTURE" >&2
    exit 1
fi

echo "CPU frequency state:"
found_cpufreq=0
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    cpufreq="$cpu/cpufreq"
    [ -d "$cpufreq" ] || continue
    found_cpufreq=1
    governor=$(cat "$cpufreq/scaling_governor" 2>/dev/null || echo unavailable)
    current=$(cat "$cpufreq/scaling_cur_freq" 2>/dev/null || echo unavailable)
    printf '  %s governor=%s current_khz=%s\n' "${cpu##*/}" "$governor" "$current"
done
[ "$found_cpufreq" -eq 1 ] || echo "  unavailable"

live_demos=$(pidof apriltag_demo.elf apriltag_c_demo.elf 2>/dev/null || true)
if [ -n "$live_demos" ]; then
    echo "WARNING: live AprilTag demo processes are running (PIDs: $live_demos)." >&2
    echo "Stop camera/display demos before collecting benchmark results." >&2
fi

mkdir -p "$RESULT_DIR"

echo "Writing results to $RESULT_DIR"
run_logged "$RESULT_DIR/comparison.log" \
    "$BENCH" --input "$FIXTURE" --size 1280x720 "$@" --backend all

for backend in rust-rvv c; do
    echo "Running perf stat for $backend"
    run_logged "$RESULT_DIR/perf-$backend.log" perf stat \
        -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses \
        "$BENCH" --input "$FIXTURE" --size 1280x720 "$@" --backend "$backend"
done
