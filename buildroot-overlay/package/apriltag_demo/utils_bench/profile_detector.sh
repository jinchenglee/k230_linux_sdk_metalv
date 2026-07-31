#!/bin/sh
# Profile the single-threaded, fixed-image detector benchmark on K230.
set -eu
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH=${APRILTAG_PROFILE_BENCH:-"$SCRIPT_DIR/k230_apriltag_bench"}
WORKLOAD=${APRILTAG_PROFILE_WORKLOAD:-"$SCRIPT_DIR/k230_apriltag_workload"}
PERF=${APRILTAG_PROFILE_PERF:-perf}
FIXTURE=${APRILTAG_PROFILE_FIXTURE:-"$SCRIPT_DIR/fixture.jpg"}
EVENT_CANDIDATES=${APRILTAG_PROFILE_EVENTS:-task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses}
REPEATS=${APRILTAG_PROFILE_REPEATS:-7}
FREQUENCY=${APRILTAG_PROFILE_FREQUENCY:-199}
WARMUP=${APRILTAG_PROFILE_WARMUP:-20}
ITERATIONS=${APRILTAG_PROFILE_ITERATIONS:-50}
BATCHES=${APRILTAG_PROFILE_BATCHES:-10}
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

warn()
{
    printf 'WARNING: %s\n' "$*" >&2
    [ -z "${ENVIRONMENT:-}" ] || printf 'WARNING: %s\n' "$*" >>"$ENVIRONMENT"
}

# POSIX sh has no pipefail. A FIFO lets us collect producer and tee statuses.
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
    if "$@" >"$ACTIVE_FIFO" 2>&1; then command_rc=0; else command_rc=$?; fi
    if wait "$ACTIVE_TEE_PID"; then tee_rc=0; else tee_rc=$?; fi
    ACTIVE_TEE_PID=
    rm -f "$ACTIVE_FIFO"
    ACTIVE_FIFO=
    [ "$command_rc" -eq 0 ] || return "$command_rc"
    return "$tee_rc"
}

valid_uint()
{
    value=$1
    name=$2
    allow_zero=$3
    case "$value" in ''|*[!0-9]*) echo "$name must be an integer" >&2; exit 2;; esac
    if [ "$allow_zero" -eq 0 ] && [ "$value" -eq 0 ]; then
        echo "$name must be greater than zero" >&2
        exit 2
    fi
}

bench_defaults()
{
    # This helper is used only for displaying the effective prefix.
    printf '%s --input %s --size 1280x720 --warmup %s --iterations %s --batches %s' \
        "$BENCH" "$FIXTURE" "$WARMUP" "$ITERATIONS" "$BATCHES"
}

capture_environment()
{
    {
        echo "AprilTag detector profile environment"
        echo "mode=$MODE"
        echo "date=$(date 2>/dev/null || true)"
        echo "kernel=$(uname -a 2>/dev/null || true)"
        echo "online_cpus=$(cat /sys/devices/system/cpu/online 2>/dev/null || echo unavailable)"
        echo "benchmark_prefix=$(bench_defaults)"
        printf 'benchmark_extra_args='
        printf ' <%s>' "$@"
        printf '\n\n/proc/cpuinfo:\n'
        cat /proc/cpuinfo 2>/dev/null || echo unavailable
        printf '\ncpufreq:\n'
        found=0
        for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
            [ -d "$cpu/cpufreq" ] || continue
            found=1
            governor=$(cat "$cpu/cpufreq/scaling_governor" 2>/dev/null || echo unavailable)
            current=$(cat "$cpu/cpufreq/scaling_cur_freq" 2>/dev/null || echo unavailable)
            printf '%s governor=%s current_khz=%s\n' "${cpu##*/}" "$governor" "$current"
        done
        [ "$found" -eq 1 ] || echo unavailable
        printf '\nPMU devices:\n'
        for pmu in /sys/bus/event_source/devices/*; do
            [ -e "$pmu" ] && echo "${pmu##*/}"
        done
        printf '\nperf list:\n'
        "$PERF" list 2>&1 || true
        printf '\nbenchmark --help:\n'
        "$BENCH" --help 2>&1 || true
    } >"$ENVIRONMENT"
}

probe_events()
{
    STAT_EVENTS=
    TIMING_EVENTS=0
    old_ifs=$IFS
    IFS=,
    for event in $EVENT_CANDIDATES; do
        IFS=$old_ifs
        safe_event=$(printf '%s' "$event" | tr -c 'A-Za-z0-9_-' '_')
        probe="$RESULT_DIR/.probe-${safe_event}.stat"
        if "$PERF" stat -x ';' -e "$event" -o "$probe" -- \
            "$BENCH" --input "$FIXTURE" --size 1280x720 "$@" \
            --warmup 0 --iterations 1 --batches 1 --backend rust-rvv --no-dump \
            >/dev/null 2>&1; then rc=0; else rc=$?; fi
        if [ "$rc" -eq 0 ] && ! grep -E -q '<not supported>|<not counted>' "$probe" 2>/dev/null; then
            [ -z "$STAT_EVENTS" ] || STAT_EVENTS="$STAT_EVENTS,"
            STAT_EVENTS="$STAT_EVENTS$event"
            case "$event" in task-clock|cpu-clock|cycles|cycles:*) TIMING_EVENTS=$((TIMING_EVENTS + 1));; esac
        else
            warn "perf event '$event' is unavailable and will be omitted"
        fi
        rm -f "$probe"
        IFS=,
    done
    IFS=$old_ifs
    if [ "$TIMING_EVENTS" -eq 0 ]; then
        echo "error: no usable timing perf event (task-clock, cpu-clock, or cycles)" >&2
        return 1
    fi
    echo "stat_events=$STAT_EVENTS" >>"$ENVIRONMENT"
}

probe_sampling()
{
    SAMPLE_EVENT=
    for event in cycles:u cpu-clock; do
        probe="$RESULT_DIR/.probe-sample.data"
        rm -f "$probe"
        if "$PERF" record -q -e "$event" -F "$FREQUENCY" -o "$probe" -- \
            "$BENCH" --input "$FIXTURE" --size 1280x720 "$@" \
            --warmup 0 --iterations 1 --batches 1 --backend rust-rvv --no-dump \
            >/dev/null 2>&1; then rc=0; else rc=$?; fi
        if [ "$rc" -eq 0 ] && [ -s "$probe" ]; then
            SAMPLE_EVENT=$event
            rm -f "$probe"
            break
        fi
        warn "perf sampling event '$event' is unavailable"
        rm -f "$probe"
    done
    if [ -z "$SAMPLE_EVENT" ]; then
        echo "error: neither cycles:u nor cpu-clock perf sampling is usable" >&2
        return 1
    fi
    echo "sample_event=$SAMPLE_EVENT" >>"$ENVIRONMENT"
}

run_comparison()
{
    run_logged "$RESULT_DIR/comparison.log" \
        "$BENCH" --input "$FIXTURE" --size 1280x720 \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend all --dump-dir "$RESULT_DIR/images"
}

run_workload()
{
    run_logged "$RESULT_DIR/workload.log" \
        "$WORKLOAD" --input "$FIXTURE" --size 1280x720 "$@" \
        --backend all --warmup 0 --iterations 1 --batches 1 --no-dump
    awk '
    function field(name,    i,a) { for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==name)return a[2]} return "" }
    function hex(s,    i,n,c) { sub(/^0[xX]/,"",s); for(i=1;i<=length(s);i++){c=index("0123456789abcdef",tolower(substr(s,i,1)))-1;if(c<0)return 0;n=n*16+c} return n }
    function valid(b,bit) { return int(validity[b]/bit)%2 }
    function show(label,n,d,bit) { if(valid("rust-rvv",bit) && valid("c-reference",bit) && d+0>0) printf "Rust RVV/C %s: %.3fx\n",label,n/d; else printf "Rust RVV/C %s: n/a\n",label }
    /^WORKLOAD / {
      b=field("backend"); validity[b]=hex(field("validity")); points[b]=field("boundary_points_emitted"); sort[b]=field("points_entering_sort");
      lfps[b]=field("points_entering_lfps"); errors[b]=field("compute_errors_points");
      entering_errors[b]=field("points_entering_errors"); raw_peaks[b]=field("raw_peaks");
      clusters[b]=field("clusters_after_filters"); retained[b]=field("retained_peaks"); quad_attempts[b]=field("quad_fit_attempts");
      quads[b]=field("quads"); decode[b]=field("decode_attempts"); threshold[b]=field("threshold_checksum");
      detections[b]=field("detections"); checksum[b]=field("checksum")
    }
    END {
      print "AprilTag workload ratio summary"
      show("emitted points",points["rust-rvv"],points["c-reference"],4)
      show("clusters after filters",clusters["rust-rvv"],clusters["c-reference"],4)
      show("sort points",sort["rust-rvv"],sort["c-reference"],8)
      show("LFPS points",lfps["rust-rvv"],lfps["c-reference"],8)
      show("points entering errors",entering_errors["rust-rvv"],entering_errors["c-reference"],8)
      show("error-fit points",errors["rust-rvv"],errors["c-reference"],8)
      show("raw peaks",raw_peaks["rust-rvv"],raw_peaks["c-reference"],8)
      show("retained peaks",retained["rust-rvv"],retained["c-reference"],8)
      show("quad fit attempts",quad_attempts["rust-rvv"],quad_attempts["c-reference"],8)
      show("quads",quads["rust-rvv"],quads["c-reference"],8)
      show("decode attempts",decode["rust-rvv"],decode["c-reference"],16)
      if(valid("rust-rvv",2) && valid("c-reference",2)) {
        if(threshold["rust-rvv"] != threshold["c-reference"])
          print "WARNING: workload threshold checksums differ."
        else
          print "Threshold checksum comparison: match"
      } else
        print "Threshold checksum comparison: unavailable"
      if(detections["rust-rvv"] != detections["c-reference"] || checksum["rust-rvv"] != checksum["c-reference"])
        print "WARNING: workload final outputs differ."
    }' "$RESULT_DIR/workload.log" >"$RESULT_DIR/workload-summary.txt"
}

run_stat()
{
    backend=$1
    shift
    repeat_args=
    if [ "$MODE" = full ]; then repeat_args="-r $REPEATS"; fi
    # Intentional splitting: repeat_args contains perf options, never user data.
    # shellcheck disable=SC2086
    run_logged "$RESULT_DIR/$backend.log" "$PERF" stat -x ';' $repeat_args \
        -e "$STAT_EVENTS" -o "$RESULT_DIR/$backend.stat" -- \
        "$BENCH" --input "$FIXTURE" --size 1280x720 \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend "$backend" --no-dump
}

record_flat()
{
    backend=$1
    shift
    [ -n "$SAMPLE_EVENT" ] || return 0
    run_logged "$RESULT_DIR/$backend-record.log" "$PERF" record -q \
        -e "$SAMPLE_EVENT" -F "$FREQUENCY" -o "$RESULT_DIR/$backend.data" -- \
        "$BENCH" --input "$FIXTURE" --size 1280x720 \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend "$backend" --no-dump
    run_logged "$RESULT_DIR/$backend.report" "$PERF" report \
        -i "$RESULT_DIR/$backend.data" --stdio --sort=dso,symbol --percent-limit=0.1
}

record_callgraph()
{
    backend=$1
    shift
    [ -n "$SAMPLE_EVENT" ] || return 0
    data="$RESULT_DIR/$backend-callgraph.data"
    if run_logged "$RESULT_DIR/$backend-callgraph-record.log" "$PERF" record -q \
        -g --call-graph fp -e "$SAMPLE_EVENT" -F "$FREQUENCY" -o "$data" -- \
        "$BENCH" --input "$FIXTURE" --size 1280x720 \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend "$backend" --no-dump; then rc=0; else rc=$?; fi
    if [ "$rc" -eq 0 ]; then
        if run_logged "$RESULT_DIR/$backend-callgraph.report" "$PERF" report \
            -i "$data" --stdio --children -g caller --sort=dso,symbol --percent-limit=0.1; then
            rc=0
        else
            rc=$?
        fi
    fi
    if [ "$rc" -ne 0 ]; then
        warn "$backend frame-pointer callgraph is unavailable (status $rc); flat profile retained"
        return 0
    fi
    if ! grep -E -q '^[[:space:]]*\|[[:space:]]*$|^[[:space:]]+[-+`|]+[^[:space:]]' \
        "$RESULT_DIR/$backend-callgraph.report"; then
        warn "$backend frame-pointer callgraph has no child callchains; flat profile retained"
    fi
}

annotate_full()
{
    backend=$1
    data="$RESULT_DIR/$backend.data"
    [ -s "$data" ] || return 0
    if [ "$backend" = rust-rvv ]; then
        symbols='apriltag_rvv::pipeline::detect
apriltag_rvv::pipeline::ccl_and_boundary_extract
apriltag_rvv::pipeline::filter_and_sort_clusters_impl
apriltag_rvv::pipeline::sort_by_angle
apriltag_rvv::pipeline::compute_errors_into
apriltag_rvv::pipeline::precompute_peak_pair_stats
apriltag_rvv::pipeline::search_peak_quad
apriltag_rvv::pipeline::fit_quad_from_cluster_with_scratch
apriltag_rvv::pipeline::decode_quad_detailed
apriltag_rvv::pipeline::deduplicate_detections'
    elif [ "$backend" = c ]; then
        symbols='apriltag_detector_detect
apriltag_quad_thresh
threshold
do_gradient_clusters
fit_quad'
    else
        return 0
    fi
    old_ifs=$IFS
    IFS='
'
    for symbol in $symbols; do
        IFS=$old_ifs
        safe=$(printf '%s' "$symbol" | tr -c 'A-Za-z0-9._-' '_')
        if command -v nm >/dev/null 2>&1 && nm -C "$BENCH" 2>/dev/null | grep -F -q "$symbol"; then
            if APRILTAG_PROFILE_PERF="$PERF" "$SCRIPT_DIR/annotate_benchmark.sh" \
                "$data" "$symbol" >"$RESULT_DIR/$backend-annotate-$safe.txt" 2>&1; then
                rc=0
            else
                rc=$?
            fi
            [ "$rc" -eq 0 ] || warn "annotation unavailable for '$symbol' (status $rc)"
        else
            warn "symbol '$symbol' is unavailable; annotation skipped"
        fi
        IFS='
'
    done
    IFS=$old_ifs
}

write_summary()
{
    awk '
    function field(name,    i,a) { for (i=1;i<=NF;i++) { split($i,a,"="); if (a[1]==name) return a[2] } return "" }
    /^RESULT / {
        b=field("backend"); mean[b]=field("mean_ms"); median[b]=field("median_ms"); calls[b]=field("calls")
        detections[b]=field("detections"); checksum[b]=field("checksum"); input_hash=field("input_hash"); build=field("build")
    }
    END {
        print "AprilTag detector profile summary"
        print "Input hash: " input_hash
        print "Build: " build
        for (b in mean) printf "%s: mean %.3f ms, median %.3f ms, calls %s, detections %s, checksum %s\n", b,mean[b],median[b],calls[b],detections[b],checksum[b]
        if (mean["rust-rvv"] && mean["rust-scalar"])
            printf "RVV scalar gain: %.2fx (%.1f%% lower mean latency)\n", mean["rust-scalar"]/mean["rust-rvv"],(1-mean["rust-rvv"]/mean["rust-scalar"])*100
        if (mean["rust-rvv"] && mean["c-reference"])
            printf "RVV versus C gap: %.2fx (%.1f%% lower mean latency)\n", mean["c-reference"]/mean["rust-rvv"],(1-mean["rust-rvv"]/mean["c-reference"])*100
        first=""; mismatch=0; for (b in checksum) { if (first=="") first=checksum[b]; else if (checksum[b]!=first) mismatch=1 }
        if (mismatch) print "WARNING: backend checksums differ; speed comparisons are not equivalent-output."
    }' "$RESULT_DIR/comparison.log" >"$RESULT_DIR/.summary-results"
    {
        cat "$RESULT_DIR/.summary-results"
        echo
        cat "$RESULT_DIR/workload-summary.txt"
        for backend in rust-rvv rust-scalar c; do
            echo
            echo "$backend perf counters:"
            parse_stat "$RESULT_DIR/$backend.stat" "$RESULT_DIR/$backend.log" "$backend"
            if [ -f "$RESULT_DIR/$backend.report" ]; then
                echo "$backend top sampled symbols (percent and whole-process normalized sampled estimates):"
                parse_report "$RESULT_DIR/$backend.report" "$RESULT_DIR/$backend.task-clock-per-call-ms"
                correlate_report "$RESULT_DIR/$backend.report" "$RESULT_DIR/$backend.task-clock-per-call-ms" \
                     "$RESULT_DIR/workload.log" "$backend"
            fi
        done
        echo
        echo "Perf values are whole-process counters normalized over detector calls; whole-process counters include benchmark setup and teardown."
        echo "Sample percentages applied to whole-process task-clock/call are normalized sampled estimates, not stage timers."
    } >"$RESULT_DIR/summary.txt"
    rm -f "$RESULT_DIR/.summary-results"
    cat "$RESULT_DIR/summary.txt"
}

parse_stat()
{
    stat_file=$1
    log_file=$2
    backend=$3
    call_fields=$(awk -v wanted="$backend" '
        function f(n, i,a) { for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]} }
        /^RESULT / && (f("backend")==wanted || (wanted=="c" && f("backend")=="c-reference")) {
            print f("calls"), f("warmup"); exit
        }' "$log_file")
    set -- $call_fields
    measured=${1:-0}
    warmup=${2:-0}
    calls=$((measured + warmup + 1))
    awk -F';' -v calls="$calls" -v measured="$measured" -v warmup="$warmup" \
        -v task_clock_file="$RESULT_DIR/$backend.task-clock-per-call-ms" '
    function trim(s) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", s); return s }
    function number(s) { return trim(s)+0 }
    {
        count=trim($1); unit=trim($2); event=trim($3)
        if (count ~ /^[0-9]+([.][0-9]+)?$/ && event != "") {
            v[event]+=number(count); n[event]++
            if (unit != "") units[event]=unit
        }
    }
    END {
        printf "  normalized over %d detector calls (%d measured + %d warmup + 1 validation)\n",calls,measured,warmup
        for (e in v) {
            average=v[e]/n[e]
            if (units[e] != "") printf "  %s: %.3f %s",e,average,units[e]
            else printf "  %s: %.0f",e,average
            if (calls>0) {
                if (units[e] != "") printf " (%.3f %s per whole-process detector call)",average/calls,units[e]
                else printf " (%.3f per whole-process detector call)",average/calls
            }
            print ""
        }
        if (calls>0 && v["task-clock"]) {
            task_clock=v["task-clock"]/n["task-clock"]/calls
            if (units["task-clock"]=="sec") task_clock*=1000
            else if (units["task-clock"]=="usec") task_clock/=1000
            else if (units["task-clock"]=="nsec") task_clock/=1000000
            printf "%.9f\n",task_clock > task_clock_file
        }
        if (v["cycles"] && v["instructions"]) printf "  IPC: %.3f\n",(v["instructions"]/n["instructions"])/(v["cycles"]/n["cycles"])
    }' "$stat_file"
}

parse_report()
{
    report=$1 task_clock_file=$2
    task_clock=n/a
    [ ! -s "$task_clock_file" ] || task_clock=$(cat "$task_clock_file")
    awk -v task_clock="$task_clock" '
        /^[[:space:]]*[0-9.]+%/ && shown<5 {
            p=$1; gsub(/%/,"",p)
            symbol=substr($0,index($0,$3))
            if (task_clock=="n/a") printf "  %s (whole-process normalized sampled estimate n/a) %s\n",$1,symbol
            else printf "  %s (whole-process normalized sampled estimate %.3f ms) %s\n",$1,task_clock*p/100,symbol
            shown++
        }
    ' "$report"
}

correlate_report()
{
    report=$1 task_clock_file=$2 workload=$3 backend=$4
    task_clock=n/a
    [ ! -s "$task_clock_file" ] || task_clock=$(cat "$task_clock_file")
    awk -v wanted="$backend" -v task_clock="$task_clock" '
    function field(name,    i,a) { for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==name)return a[2]} return "" }
    function hex(s,    i,n,c) { sub(/^0[xX]/,"",s); for(i=1;i<=length(s);i++){c=index("0123456789abcdef",tolower(substr(s,i,1)))-1;if(c<0)return 0;n=n*16+c} return n }
    function valid(bit) { return int(workload_validity/bit)%2 }
    function sample(symbol, key, label, bit,    ms) {
        if (!(symbol in pct)) return
        if (!valid(bit) || !(key in units)) { printf "    %s: workload unit unavailable\n",symbol; return }
        if (units[key]+0<=0) { printf "    %s: workload unit count is zero\n",symbol; return }
        if (task_clock=="n/a") { printf "    %s: whole-process normalized sampled estimate n/a\n",symbol; return }
        ms=task_clock*pct[symbol]/100
        printf "    %s: whole-process normalized sampled estimate %.3f ms, %.3f us/%s (%s %ss)\n",symbol,ms,ms*1000/units[key],label,units[key],label
    }
    FILENAME==ARGV[1] && /^WORKLOAD / {
        b=field("backend"); if (wanted=="c") wanted_workload="c-reference"; else wanted_workload=wanted
        if (b==wanted_workload) {
            workload_validity=hex(field("validity"))
            units["boundary"]=field("boundary_points_emitted")
            units["sort"]=field("points_entering_sort")
            units["lfps"]=field("points_entering_lfps")
            units["errors"]=field("compute_errors_points")
            units["decode"]=field("decode_attempts")
            units["uf"]=field("uf_elements")
            units["line"]=field("line_fit_query_points")
            units["quad"]=field("quad_fit_attempts")
        }
        next
    }
    FILENAME==ARGV[2] && /^[[:space:]]*[0-9]+([.][0-9]+)?%/ {
        p=$1; gsub(/%/,"",p)
        if (wanted=="c") {
            names[1]="do_gradient_clusters"; names[2]="do_unionfind"; names[3]="fit_line"; names[4]="fit_quad"; count=4
        } else {
            names[1]="ccl_and_boundary_extract"; names[2]="sort_by_angle"; names[3]="compute_lfps_rvv"
            names[4]="compute_lfps_scalar"; names[5]="compute_errors_into"; names[6]="decode_quad_detailed"; count=6
        }
        for(i=1;i<=count;i++) if(index($0,names[i])) pct[names[i]]+=p
    }
    END {
        print "  sampled stage workload correlations (approximate):"
        if (wanted=="c") {
            sample("do_gradient_clusters","boundary","boundary point",4)
            sample("do_unionfind","uf","UF element",32)
            sample("fit_line","line","line-fit point",64)
            sample("fit_quad","quad","quad attempt",8)
        } else {
            sample("ccl_and_boundary_extract","boundary","boundary point",4)
            sample("sort_by_angle","sort","point entering sort",8)
            sample("compute_lfps_rvv","lfps","point entering LFPS",8)
            sample("compute_lfps_scalar","lfps","point entering LFPS",8)
            sample("compute_errors_into","errors","error-fit point",8)
            sample("decode_quad_detailed","decode","decode attempt",16)
        }
    }' "$workload" "$report"
}

self_test_profile_detector()
{
    test_root=${TMPDIR:-/tmp}/profile-detector-self-test.$$
    mkdir "$test_root" "$test_root/bin"
    trap 'rm -rf "$test_root"' 0 HUP INT TERM
    fake_bench="$test_root/fake-bench"
    fake_workload="$test_root/fake-workload"
    fake_perf="$test_root/fake-perf"
    fake_tee="$test_root/bin/tee"
    fake_nm="$test_root/bin/nm"
cat >"$fake_bench" <<'EOF'
#!/bin/sh
[ "${LC_ALL:-}" = C ] || exit 42
backend=all
for arg in "$@"; do prev=${current:-}; current=$arg; [ "$prev" = --backend ] && backend=$arg; done
[ "${FAIL_BENCH:-0}" -eq 0 ] || [ "$backend" != all ] || exit 23
[ "$backend" = all ] && backends='rust-rvv rust-scalar c-reference' || { [ "$backend" = c ] && backends=c-reference || backends=$backend; }
for b in $backends; do
  case $b in rust-rvv) m=10.000;c=aa;; rust-scalar) m=20.000;c=aa;; *) m=15.000;c=bb;; esac
  echo "RESULT backend=$b calls=2 mean_ms=$m median_ms=$m detections=7 checksum=$c input_hash=123 build=fake warmup=3 iterations=1 batches=2"
done
EOF
    cat >"$fake_workload" <<'EOF'
#!/bin/sh
echo workload >>"${FAKE_PERF_TRACE:-/dev/null}"
rust_validity=0x7f
[ "${INVALID_FITTING:-0}" -eq 0 ] || rust_validity=0x77
[ "${INVALID_THRESHOLD:-0}" -eq 0 ] || rust_validity=0x7d
echo "WORKLOAD backend=rust-rvv schema=1 validity=$rust_validity boundary_points_emitted=200 clusters_after_filters=20 points_entering_sort=160 points_entering_lfps=140 points_entering_errors=130 compute_errors_points=120 raw_peaks=12 retained_peaks=10 quad_fit_attempts=4 quads=8 decode_attempts=5 uf_elements=90 line_fit_queries=40 line_fit_query_points=80 threshold_checksum=10 detections=2 checksum=aa"
echo 'WORKLOAD backend=rust-scalar schema=1 validity=0x7f boundary_points_emitted=200 clusters_after_filters=20 points_entering_sort=160 points_entering_lfps=140 points_entering_errors=130 compute_errors_points=120 raw_peaks=12 retained_peaks=10 quad_fit_attempts=4 quads=8 decode_attempts=5 uf_elements=90 line_fit_queries=40 line_fit_query_points=80 threshold_checksum=10 detections=2 checksum=aa'
echo 'WORKLOAD backend=c-reference schema=1 validity=0x7f boundary_points_emitted=100 clusters_after_filters=10 points_entering_sort=80 points_entering_lfps=70 points_entering_errors=65 compute_errors_points=60 raw_peaks=6 retained_peaks=5 quad_fit_attempts=2 quads=4 decode_attempts=3 uf_elements=50 line_fit_queries=30 line_fit_query_points=60 threshold_checksum=20 detections=2 checksum=bb'
EOF
    cat >"$fake_perf" <<'EOF'
#!/bin/sh
cmd=$1; shift
    [ "$cmd" = list ] && { echo perf-list >>"${FAKE_PERF_TRACE:-/dev/null}"; echo fake-events; exit 0; }
out=; event=; callgraph=0; delimiter=
while [ $# -gt 0 ]; do
  case $1 in
    -o|-e|-i|-F|--sort|--percent-limit|-x) key=$1; shift; [ $# -gt 0 ] && { [ "$key" = -o ] && out=$1; [ "$key" = -e ] && event=$1; [ "$key" = -x ] && delimiter=$1; };;
    -g) callgraph=1;;
    --call-graph) callgraph=1; shift;;
    -r) echo "repeat=$2" >>"${FAKE_PERF_TRACE:-/dev/null}"; shift;;
    --) shift; break;;
  esac
  shift
done
case $cmd in
  stat) echo perf-stat >>"${FAKE_PERF_TRACE:-/dev/null}"; [ "$delimiter" = ';' ] || exit 41; [ "$event" = branches ] && { echo '<not supported>' >"$out"; exit 1; }; "$@"; rc=$?; { echo '1000;;cycles;100.00;100.00'; echo '2000;;instructions;100.00;100.00'; echo '3.0;msec;task-clock;100.00;100.00'; echo '4;;cache/misses:u;100.00;100.00'; } >"$out"; exit $rc;;
 record) echo perf-record >>"${FAKE_PERF_TRACE:-/dev/null}"; [ "$event" = cycles:u ] && exit 1; [ "$callgraph" -eq 0 ] || [ "${FAIL_CALLGRAPH:-0}" -eq 0 ] || exit 29; "$@"; rc=$?; [ "$rc" -eq 0 ] && echo data >"$out"; exit $rc;;
 report)
   if [ "$callgraph" -ne 0 ]; then
      echo callgraph >>"${FAKE_PERF_TRACE:-/dev/null}"
     [ "${EMPTY_CALLGRAPH:-0}" -ne 0 ] || { echo '  50.00% fake-dso symbol with spaces'; echo '          |'; echo '          ---child'; }
    else
      echo '  20.00% fake-dso apriltag_rvv::pipeline::ccl_and_boundary_extract'
      echo '  10.00% fake-dso apriltag_rvv::pipeline::sort_by_angle'
      echo '   7.00% fake-dso apriltag_rvv::pipeline::compute_lfps_rvv'
      echo '   6.00% fake-dso apriltag_rvv::pipeline::compute_errors_into'
      echo '   5.00% fake-dso apriltag_rvv::pipeline::decode_quad_detailed'
      echo '  15.00% fake-dso do_gradient_clusters'
      echo '   8.00% fake-dso do_unionfind'
      echo '   6.00% fake-dso fit_line'
      echo '   4.00% fake-dso fit_quad'
   fi
   ;;
 annotate) echo annotation;;
esac
EOF
    cat >"$fake_tee" <<'EOF'
#!/bin/sh
if [ "${FAIL_TEE:-0}" -ne 0 ]; then /usr/bin/tee "$@" >/dev/null; exit 31; fi
exec /usr/bin/tee "$@"
EOF
    cat >"$fake_nm" <<'EOF'
#!/bin/sh
echo '00000000 T apriltag_rvv::pipeline::detect'
echo '00000000 T apriltag_detector_detect'
EOF
    chmod +x "$fake_bench" "$fake_workload" "$fake_perf" "$fake_tee" "$fake_nm"
    touch "$test_root/fixture"
    for mode in fast full; do
        output_root="$test_root/$mode"
        : >"$test_root/perf-trace"
        PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$output_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles,instructions,branches,cache/misses:u' \
        APRILTAG_PROFILE_REPEATS=2 APRILTAG_PROFILE_WARMUP=3 APRILTAG_PROFILE_ITERATIONS=1 APRILTAG_PROFILE_BATCHES=2 \
            "$TEST_SHELL" "$0" "$mode" --size 7x9 >/dev/null 2>&1
        output=$(ls -d "$output_root"/run-* 2>/dev/null)
        grep -q 'sample_event=cpu-clock' "$output/environment.txt"
        grep -q "event 'branches'.*omitted" "$output/environment.txt"
        grep -q 'RVV scalar gain: 2.00x' "$output/summary.txt"
        grep -q 'c-reference: mean 15.000 ms, median 15.000 ms, calls 2, detections 7, checksum bb' "$output/summary.txt"
        grep -q 'cycles: 1000 (166.667 per whole-process detector call)' "$output/summary.txt"
        grep -q 'task-clock: 3.000 msec (0.500 msec per whole-process detector call)' "$output/summary.txt"
        grep -q 'cache/misses:u: 4' "$output/summary.txt"
        grep -q 'normalized over 6 detector calls (2 measured + 3 warmup + 1 validation)' "$output/summary.txt"
        grep -q 'whole-process counters include benchmark setup and teardown' "$output/summary.txt"
        grep -q 'c top sampled symbols' "$output/summary.txt"
        grep -q '20.00% (whole-process normalized sampled estimate 0.100 ms) apriltag_rvv::pipeline::ccl_and_boundary_extract' "$output/summary.txt"
        grep -q 'Input hash: 123' "$output/summary.txt"
        grep -q 'Build: fake' "$output/summary.txt"
        grep -q 'WARNING: backend checksums differ' "$output/summary.txt"
        grep -q 'Rust RVV/C emitted points: 2.000x' "$output/workload-summary.txt"
        grep -q 'Rust RVV/C clusters after filters: 2.000x' "$output/workload-summary.txt"
        grep -q 'Rust RVV/C retained peaks: 2.000x' "$output/workload-summary.txt"
        grep -q 'Rust RVV/C points entering errors: 2.000x' "$output/workload-summary.txt"
        grep -q 'Rust RVV/C raw peaks: 2.000x' "$output/workload-summary.txt"
        grep -q 'Rust RVV/C quad fit attempts: 2.000x' "$output/workload-summary.txt"
        grep -q 'ccl_and_boundary_extract: whole-process normalized sampled estimate 0.100 ms, 0.500 us/boundary point' "$output/summary.txt"
        grep -q 'sort_by_angle: whole-process normalized sampled estimate 0.050 ms, 0.312 us/point entering sort' "$output/summary.txt"
        grep -q 'compute_lfps_rvv: whole-process normalized sampled estimate 0.035 ms, 0.250 us/point entering LFPS' "$output/summary.txt"
        grep -q 'compute_errors_into: whole-process normalized sampled estimate 0.030 ms, 0.250 us/error-fit point' "$output/summary.txt"
        grep -q 'decode_quad_detailed: whole-process normalized sampled estimate 0.025 ms, 5.000 us/decode attempt' "$output/summary.txt"
        grep -q 'do_gradient_clusters: whole-process normalized sampled estimate 0.075 ms, 0.750 us/boundary point' "$output/summary.txt"
        grep -q 'do_unionfind: whole-process normalized sampled estimate 0.040 ms, 0.800 us/UF element' "$output/summary.txt"
        grep -q 'fit_line: whole-process normalized sampled estimate 0.030 ms, 0.500 us/line-fit point' "$output/summary.txt"
        grep -q 'fit_quad: whole-process normalized sampled estimate 0.020 ms, 10.000 us/quad attempt' "$output/summary.txt"
        first_trace=$(sed -n '1p' "$test_root/perf-trace")
        [ "$first_trace" = workload ]
        grep -q 'WARNING: workload threshold checksums differ' "$output/summary.txt"
        [ -s "$output/workload.log" ]
        [ ! -e "$output/.probe-cache/misses:u.stat" ]
        [ -s "$output/rust-rvv.report" ]
        [ -s "$output/c.report" ]
        if [ "$mode" = full ]; then
            [ -s "$output/rust-rvv-callgraph.report" ]
            [ -s "$output/c-callgraph.report" ]
            [ -s "$output/rust-rvv-annotate-apriltag_rvv__pipeline__detect.txt" ]
            [ -s "$output/c-annotate-apriltag_detector_detect.txt" ]
            grep -q '^repeat=2$' "$test_root/perf-trace"
        fi
    done
    invalid_root="$test_root/invalid-validity"
    INVALID_FITTING=1 PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$invalid_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    invalid_output=$(ls -d "$invalid_root"/run-* 2>/dev/null)
    grep -q 'Rust RVV/C emitted points: 2.000x' "$invalid_output/workload-summary.txt"
    grep -q 'Rust RVV/C sort points: n/a' "$invalid_output/workload-summary.txt"
    grep -q 'Rust RVV/C raw peaks: n/a' "$invalid_output/workload-summary.txt"
    grep -q 'Rust RVV/C decode attempts: 1.667x' "$invalid_output/workload-summary.txt"
    grep -q 'sort_by_angle: workload unit unavailable' "$invalid_output/summary.txt"
    grep -q 'compute_errors_into: workload unit unavailable' "$invalid_output/summary.txt"
    invalid_threshold_root="$test_root/invalid-threshold-validity"
    INVALID_THRESHOLD=1 PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$invalid_threshold_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    invalid_threshold_output=$(ls -d "$invalid_threshold_root"/run-* 2>/dev/null)
    grep -q 'Threshold checksum comparison: unavailable' "$invalid_threshold_output/workload-summary.txt"
    ! grep -q 'WARNING: workload threshold checksums differ' "$invalid_threshold_output/summary.txt"
    output="$test_root/callgraph-fail"
    FAIL_CALLGRAPH=1 PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$output" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" full >"$test_root/callgraph-fail.log" 2>&1
    output=$(ls -d "$output"/run-* 2>/dev/null)
    grep -q 'frame-pointer callgraph is unavailable (status 29)' "$test_root/callgraph-fail.log"
    grep -q 'AprilTag detector profile summary' "$output/summary.txt"
    output_root="$test_root/callgraph-empty"
    EMPTY_CALLGRAPH=1 PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$output_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" full >"$test_root/callgraph-empty.log" 2>&1
    grep -q 'frame-pointer callgraph has no child callchains; flat profile retained' "$test_root/callgraph-empty.log"
    mkdir "$test_root/nonempty"
    : >"$test_root/nonempty/stale"
    PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$test_root/nonempty" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    [ -e "$test_root/nonempty/stale" ]
    output_count=$(ls -d "$test_root/nonempty"/run-* 2>/dev/null | wc -l)
    [ "$output_count" -eq 1 ]
    set +e
    FAIL_BENCH=1 APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$test_root/fail" \
        sh "$0" fast >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 23 ]
    set +e
    FAIL_TEE=1 PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$test_root/tee-fail" \
        sh "$0" fast >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 31 ]
    set +e
    sh "$0" invalid >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 2 ]
    default_app="$test_root/default-app"
    mkdir -p "$default_app"
    cp "$0" "$default_app/profile_detector.sh"
    APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
        "$TEST_SHELL" "$default_app/profile_detector.sh" fast >/dev/null 2>&1
    default_output_count=$(find "$default_app/results" -mindepth 1 -maxdepth 1 -type d | wc -l)
    [ "$default_output_count" -eq 1 ]
    rm -rf "$test_root"
    trap - 0 HUP INT TERM
    echo "profile_detector self-test passed ($TEST_SHELL): CSV parsers, artifacts, call accounting, and failures"
}

if [ "${1:-}" = --self-test ]; then
    TEST_SHELL=${TEST_SHELL:-sh}
    self_test_profile_detector
    exit $?
fi
case "${1:-}" in fast|full) MODE=$1; shift;; *) echo "usage: $0 fast|full [benchmark options]" >&2; exit 2;; esac
valid_uint "$REPEATS" APRILTAG_PROFILE_REPEATS 0
valid_uint "$FREQUENCY" APRILTAG_PROFILE_FREQUENCY 0
valid_uint "$WARMUP" APRILTAG_PROFILE_WARMUP 1
valid_uint "$ITERATIONS" APRILTAG_PROFILE_ITERATIONS 0
valid_uint "$BATCHES" APRILTAG_PROFILE_BATCHES 0
for command in "$PERF" awk tee; do command -v "$command" >/dev/null 2>&1 || { echo "required command not found: $command" >&2; exit 1; }; done
[ -x "$BENCH" ] || { echo "benchmark executable not found: $BENCH" >&2; exit 1; }
[ -x "$WORKLOAD" ] || { echo "workload executable not found: $WORKLOAD" >&2; exit 1; }
[ -f "$FIXTURE" ] || { echo "default fixture not found: $FIXTURE" >&2; exit 1; }
if [ -n "${APRILTAG_PROFILE_OUTPUT:-}" ]; then
    OUTPUT_ROOT=$APRILTAG_PROFILE_OUTPUT
    RUN_PREFIX=run
else
    OUTPUT_ROOT="$SCRIPT_DIR/results"
    RUN_PREFIX=profile-$MODE
fi
mkdir -p "$OUTPUT_ROOT"
STAMP=$(date +%Y%m%d-%H%M%S)
RESULT_DIR="$OUTPUT_ROOT/$RUN_PREFIX-$STAMP-$$"
mkdir "$RESULT_DIR"
ENVIRONMENT="$RESULT_DIR/environment.txt"
echo "Writing profile to $RESULT_DIR"
run_workload "$@"
capture_environment "$@"
live_demos=$(pidof apriltag_demo.elf apriltag_c_demo.elf 2>/dev/null || true)
[ -z "$live_demos" ] || warn "live AprilTag demos are running (PIDs: $live_demos); stop them for uncontended results"
probe_events "$@"
probe_sampling "$@"
run_comparison "$@"
for backend in rust-rvv rust-scalar c; do
    run_stat "$backend" "$@"
    record_flat "$backend" "$@"
    if [ "$MODE" = full ]; then record_callgraph "$backend" "$@"; fi
done
if [ "$MODE" = full ]; then
    annotate_full rust-rvv
    annotate_full c
fi
write_summary
