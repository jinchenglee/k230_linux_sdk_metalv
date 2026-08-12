#!/bin/sh
# Profile the single-threaded, fixed-image detector benchmark on K230.
set -eu
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH=${APRILTAG_PROFILE_BENCH:-"$SCRIPT_DIR/k230_apriltag_bench"}
STAGE_BENCH=${APRILTAG_PROFILE_STAGE_BENCH:-"$SCRIPT_DIR/k230_apriltag_profile_bench"}
WORKLOAD=${APRILTAG_PROFILE_WORKLOAD:-"$SCRIPT_DIR/k230_apriltag_workload"}
PERF=${APRILTAG_PROFILE_PERF:-perf}
PROFILE_READLINK=${APRILTAG_PROFILE_READLINK:-readlink}
FIXTURE=${APRILTAG_PROFILE_FIXTURE:-"$SCRIPT_DIR/fixture.jpg"}
INPUT_SIZE=1280x720
EVENT_CANDIDATES=${APRILTAG_PROFILE_EVENTS:-task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses}
REPEATS=${APRILTAG_PROFILE_REPEATS:-7}
FREQUENCY=${APRILTAG_PROFILE_FREQUENCY:-199}
WARMUP=${APRILTAG_PROFILE_WARMUP:-20}
ITERATIONS=${APRILTAG_PROFILE_ITERATIONS:-50}
BATCHES=${APRILTAG_PROFILE_BATCHES:-10}
ABLATIONS=${APRILTAG_PROFILE_ABLATIONS:-0}
ABLATION_WARMUP=${APRILTAG_ABLATION_WARMUP:-5}
ABLATION_ITERATIONS=${APRILTAG_ABLATION_ITERATIONS:-20}
ABLATION_BATCHES=${APRILTAG_ABLATION_BATCHES:-5}
ABLATION_PERF_WARMUP=${APRILTAG_ABLATION_PERF_WARMUP:-$ABLATION_WARMUP}
ABLATION_PERF_ITERATIONS=${APRILTAG_ABLATION_PERF_ITERATIONS:-$ABLATION_ITERATIONS}
ABLATION_PERF_BATCHES=${APRILTAG_ABLATION_PERF_BATCHES:-$ABLATION_BATCHES}
ACTIVE_FIFO=
ACTIVE_TEE_PID=
PARSER_INPUTS_TMP=
PARSER_PATHS_TMP=
PARSER_LABELS_TMP=
PARSER_DIR_TMP=
PARSER_PUBLICATION_ACTIVE=0
CROSS_SUMMARY_TMP=

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
    [ -z "$PARSER_INPUTS_TMP" ] || rm -f "$PARSER_INPUTS_TMP"
    [ -z "$PARSER_PATHS_TMP" ] || rm -f "$PARSER_PATHS_TMP"
    [ -z "$PARSER_LABELS_TMP" ] || rm -f "$PARSER_LABELS_TMP"
    [ -z "$PARSER_DIR_TMP" ] || rm -rf "$PARSER_DIR_TMP"
    [ -z "$CROSS_SUMMARY_TMP" ] || rm -f "$CROSS_SUMMARY_TMP"
    if [ "$PARSER_PUBLICATION_ACTIVE" -eq 1 ]; then
        rm -rf "$RESULT_DIR/inputs"
        rm -f "$RESULT_DIR/inputs.tsv"
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

profile_inputs_error()
{
    printf 'error: APRILTAG_PROFILE_INPUTS %s\n' "$*" >&2
    return 2
}

profile_inputs_publish_hook()
{
    [ -z "${APRILTAG_PROFILE_PUBLISH_HOOK:-}" ] || "$APRILTAG_PROFILE_PUBLISH_HOOK" "$1"
}

validate_multi_input_args()
{
    for arg in "$@"; do
        case $arg in
            --input|--input=*|--format|--format=*|--size|--size=*)
                echo "error: multi-input mode does not accept user --input, --format, or --size options; specify each input in APRILTAG_PROFILE_INPUTS" >&2
                return 2
                ;;
        esac
    done
}

parse_profile_inputs()
{
    PARSER_INPUTS_TMP=$RESULT_DIR/.inputs.tsv.$$
    PARSER_PATHS_TMP=$RESULT_DIR/.input-paths.tsv.$$
    PARSER_LABELS_TMP=$RESULT_DIR/.input-labels.$$
    PARSER_DIR_TMP=$RESULT_DIR/.inputs.$$
    tab=$(printf '\t')
    newline='
'
    : >"$PARSER_INPUTS_TMP"
    : >"$PARSER_PATHS_TMP"
    : >"$PARSER_LABELS_TMP"

    [ -n "$APRILTAG_PROFILE_INPUTS" ] || profile_inputs_error 'empty matrix'
    cr=$(printf '\r')
    case $APRILTAG_PROFILE_INPUTS in
        *"$cr"*) profile_inputs_error 'carriage return is not allowed';;
    esac
    command -v "$PROFILE_READLINK" >/dev/null 2>&1 || \
        profile_inputs_error "readlink -f command is unavailable: $PROFILE_READLINK"

    line_number=0
    input_count=0
    while IFS= read -r input_line || [ -n "$input_line" ]; do
        line_number=$((line_number + 1))
        case $input_line in *[!" $tab"]*) ;; *) continue;; esac
        case $input_line in
            *=*) label=${input_line%%=*}; input_spec=${input_line#*=};;
            *) profile_inputs_error "line $line_number is missing label=path,size fields";;
        esac
        [ -n "$label" ] || profile_inputs_error "line $line_number has an empty label"
        case $label in
            .|..) profile_inputs_error "line $line_number has invalid label '$label'";;
            *[!A-Za-z0-9._-]*) profile_inputs_error "line $line_number has bad label '$label'";;
        esac
        if awk -F '\t' -v label="$label" '$1 == label { found=1 } END { exit !found }' \
            "$PARSER_INPUTS_TMP"; then
            profile_inputs_error "line $line_number has duplicate label '$label'"
        fi
        case $input_spec in
            *,*) input_path=${input_spec%,*}; input_size=${input_spec##*,};;
            *) profile_inputs_error "line $line_number is missing path,size fields";;
        esac
        case $input_path in
            '') profile_inputs_error "line $line_number has missing path";;
            *,*) profile_inputs_error "line $line_number has comma in path";;
            *"$tab"*) profile_inputs_error "line $line_number has tab in path";;
            *\\*) profile_inputs_error "line $line_number has backslash in path";;
        esac
        [ -n "$input_size" ] || profile_inputs_error "line $line_number has missing size"
        case $input_size in
            native) input_width=; input_height=;;
            *x*) input_width=${input_size%x*}; input_height=${input_size##*x};;
            *) profile_inputs_error "line $line_number has invalid WxH '$input_size'";;
        esac
        if [ "$input_size" != native ]; then
            case $input_width in ''|*[!0-9]*) profile_inputs_error "line $line_number has invalid WxH '$input_size'";; esac
            case $input_height in ''|*[!0-9]*) profile_inputs_error "line $line_number has invalid WxH '$input_size'";; esac
            [ "$input_width" -gt 0 ] 2>/dev/null || profile_inputs_error "line $line_number has invalid WxH '$input_size'"
            [ "$input_height" -gt 0 ] 2>/dev/null || profile_inputs_error "line $line_number has invalid WxH '$input_size'"
        fi
        [ -f "$input_path" ] || profile_inputs_error "line $line_number path does not exist: $input_path"

        canonical_sentinel=.apriltag-profile-readlink-sentinel
        if canonical_with_sentinel=$("$PROFILE_READLINK" -f -- "$input_path" 2>/dev/null && \
            printf '%s' "$canonical_sentinel"); then
            :
        else
            profile_inputs_error "line $line_number cannot canonicalize path with readlink -f: $input_path"
        fi
        case $canonical_with_sentinel in
            *"$canonical_sentinel") canonical_output=${canonical_with_sentinel%"$canonical_sentinel"};;
            *) profile_inputs_error "line $line_number readlink -f output is malformed: $input_path";;
        esac
        case $canonical_output in
            *"$newline") canonical_path=${canonical_output%"$newline"};;
            *) profile_inputs_error "line $line_number readlink -f output lacks newline terminator: $input_path";;
        esac
        [ -n "$canonical_path" ] || \
            profile_inputs_error "line $line_number readlink -f returned an empty path: $input_path"
        case $canonical_path in
            *,*) profile_inputs_error "line $line_number canonical path contains comma: $canonical_path";;
            *"$tab"*) profile_inputs_error "line $line_number canonical path contains tab";;
            *"$cr"*) profile_inputs_error "line $line_number canonical path contains carriage return";;
            *"$newline"*) profile_inputs_error "line $line_number canonical path contains newline";;
            *\\*) profile_inputs_error "line $line_number canonical path contains backslash";;
        esac
        if awk -F '\t' -v path="$canonical_path" '$1 == path { found=1 } END { exit !found }' \
            "$PARSER_PATHS_TMP"; then
            profile_inputs_error "line $line_number has duplicate canonical path: $input_path"
        fi
        printf '%s\t%s\n' "$canonical_path" "$label" >>"$PARSER_PATHS_TMP"
        input_hash=$(hash_file "$canonical_path") || \
            profile_inputs_error "line $line_number cannot hash path: $input_path"
        if [ "$input_size" = native ]; then
            printf '%s\t%s\tnative\t%s\n' "$label" "$canonical_path" "$input_hash" >>"$PARSER_INPUTS_TMP"
        else
            printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$canonical_path" "$input_width" "$input_height" "$input_hash" >>"$PARSER_INPUTS_TMP"
        fi
        printf '%s\n' "$label" >>"$PARSER_LABELS_TMP"
        input_count=$((input_count + 1))
    done <<EOF
$APRILTAG_PROFILE_INPUTS
EOF
    [ "$input_count" -gt 0 ] || profile_inputs_error 'empty matrix'

    mkdir "$PARSER_DIR_TMP"
    while IFS= read -r label; do
        mkdir "$PARSER_DIR_TMP/$label"
    done <"$PARSER_LABELS_TMP"
    rm -f "$PARSER_PATHS_TMP" "$PARSER_LABELS_TMP"
    PARSER_PATHS_TMP=
    PARSER_LABELS_TMP=

    PARSER_PUBLICATION_ACTIVE=1
    mv "$PARSER_DIR_TMP" "$RESULT_DIR/inputs"
    PARSER_DIR_TMP=
    profile_inputs_publish_hook after-inputs
    mv "$PARSER_INPUTS_TMP" "$RESULT_DIR/inputs.tsv"
    PARSER_INPUTS_TMP=
    profile_inputs_publish_hook after-manifest
    profile_inputs_publish_hook before-commit
    PARSER_PUBLICATION_ACTIVE=0
}

split_hash_path()
{
    split_path=$1
    HASH_DIR=${split_path%/*}
    HASH_BASE=${split_path##*/}
    if [ "$HASH_DIR" = "$split_path" ]; then
        HASH_DIR=.
    elif [ -z "$HASH_DIR" ]; then
        HASH_DIR=/
    fi
}

hash_file()
{
    split_hash_path "$1"
    hash_output=$(CDPATH= cd -- "$HASH_DIR" && sha256sum -- "./$HASH_BASE") || return 1
    printf '%s\n' "${hash_output%% *}"
}

bench_defaults()
{
    # This helper is used only for displaying the effective prefix.
    printf '%s --input %s --size %s --warmup %s --iterations %s --batches %s' \
        "$BENCH" "$FIXTURE" "$INPUT_SIZE" "$WARMUP" "$ITERATIONS" "$BATCHES"
}

capture_environment()
{
    {
        echo "AprilTag detector profile environment"
        if [ -n "${CURRENT_INPUT_LABEL:-}" ]; then
            echo "Input label: $CURRENT_INPUT_LABEL"
            echo "Canonical path: $CURRENT_INPUT_PATH"
            echo "File SHA-256: $CURRENT_INPUT_FILE_HASH"
            echo "Requested size: $CURRENT_INPUT_SIZE"
            echo "Native RESULT dimensions: $CURRENT_INPUT_WIDTH_IN_RESULT_PATH"
        fi
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
            "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" "$@" \
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
            "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" "$@" \
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
        "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend all --dump-dir "$RESULT_DIR/images"
}

run_ccl_profile()
{
    run_logged "$RESULT_DIR/ccl-profile.log" \
        "$STAGE_BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
        --warmup "$WARMUP" --iterations "$ITERATIONS" --batches "$BATCHES" \
        "$@" --backend rust-rvv --no-dump
}

run_workload()
{
    run_logged "$RESULT_DIR/workload.log" \
        "$WORKLOAD" --input "$FIXTURE" --size "$INPUT_SIZE" "$@" \
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
      detections[b]=field("result_detections"); checksum[b]=field("result_checksum")
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
        "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
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
        "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
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
        "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
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
        if [ -n "${CURRENT_INPUT_LABEL:-}" ]; then
            echo "Input label: $CURRENT_INPUT_LABEL"
            echo "Canonical path: $CURRENT_INPUT_PATH"
            echo "File SHA-256: $CURRENT_INPUT_FILE_HASH"
            echo "Requested size: $CURRENT_INPUT_SIZE"
            echo "Native RESULT dimensions: $CURRENT_INPUT_WIDTH_IN_RESULT_PATH"
        fi
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

ablation_matrix()
{
    cat <<'EOF'
scalar:none
only-decimate:decimate
only-threshold:threshold
only-rle:rle
only-lfps-tuned:lfps-tuned
only-gaussian:gaussian
only-gray-model:gray-model
all:all
without-decimate:threshold,rle,lfps-tuned,gaussian,gray-model
without-threshold:decimate,rle,lfps-tuned,gaussian,gray-model
without-rle:decimate,threshold,lfps-tuned,gaussian,gray-model
without-lfps-tuned:decimate,threshold,rle,gaussian,gray-model
without-gaussian:decimate,threshold,rle,lfps-tuned,gray-model
without-gray-model:decimate,threshold,rle,lfps-tuned,gaussian
EOF
}

run_ablation_config()
{
    label=$1
    stages=$2
    shift 2
    config_dir="$RESULT_DIR/ablations/$label"
    mkdir "$config_dir"
    run_logged "$config_dir/benchmark.log" \
        "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
        --warmup "$ABLATION_WARMUP" --iterations "$ABLATION_ITERATIONS" --batches "$ABLATION_BATCHES" \
        "$@" --backend rust-rvv --rvv-stages "$stages" --no-dump
    run_logged "$config_dir/workload.log" \
        "$WORKLOAD" --input "$FIXTURE" --size "$INPUT_SIZE" "$@" \
        --backend rust-rvv --rvv-stages "$stages" \
        --warmup 0 --iterations 1 --batches 1 --no-dump
    run_logged "$config_dir/profile.log" \
        "$STAGE_BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
        --warmup "$ABLATION_WARMUP" --iterations "$ABLATION_ITERATIONS" --batches "$ABLATION_BATCHES" \
        "$@" --backend rust-rvv --rvv-stages "$stages" --no-dump
    if [ -n "$STAT_EVENTS" ]; then
        run_logged "$config_dir/perf.log" "$PERF" stat -x ';' \
            -e "$STAT_EVENTS" -o "$config_dir/perf.stat" -- \
            "$BENCH" --input "$FIXTURE" --size "$INPUT_SIZE" \
            --warmup "$ABLATION_PERF_WARMUP" --iterations "$ABLATION_PERF_ITERATIONS" --batches "$ABLATION_PERF_BATCHES" \
            "$@" --backend rust-rvv --rvv-stages "$stages" --no-dump
    fi
}

validate_ablation_log()
{
    file=$1
    stages=$2
    expected_mask=$3
    expected_detections=$4
    expected_checksum=$5
    require_profile=$6
    awk -v stages="$stages" -v mask="$expected_mask" -v detections="$expected_detections" \
        -v checksum="$expected_checksum" -v profile="$require_profile" '
      function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}return ""}
      /^RESULT / {results++; if(f("backend")!="rust-rvv"||f("stages")!=stages||f("rvv_mask")!=mask||f("detections")!=detections||f("checksum")!=checksum)bad=1}
      /^STAGE / {stage++; if(f("backend")!="rust-rvv"||f("stages")!=stages||f("rvv_mask")!=mask)bad=1}
      /^CCL_WORK / {work++; if(f("backend")!="rust-rvv"||f("stages")!=stages||f("rvv_mask")!=mask)bad=1}
      /^CCL_TIMER_HEALTH / {health++; if(f("backend")!="rust-rvv"||f("stages")!=stages||f("rvv_mask")!=mask||f("diagnostic_included")!="1")bad=1}
      END {exit !(results==1&&!bad&&(!profile||(stage>0&&work==1&&health==1)))}' "$file"
}

write_ablation_summary()
{
    summary="$RESULT_DIR/ablations/summary.txt"
    : >"$summary"
    scalar_result=$(awk '
      function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^RESULT / {print f("mean_ms"),f("detections"),f("checksum");exit}' \
        "$RESULT_DIR/ablations/scalar/benchmark.log")
    all_result=$(awk '
      function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^RESULT / {print f("mean_ms"),f("detections"),f("checksum");exit}' \
        "$RESULT_DIR/ablations/all/benchmark.log")
    scalar_work=$(awk '
      function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^WORKLOAD / {print f("result_detections"),f("result_checksum");exit}' \
        "$RESULT_DIR/ablations/scalar/workload.log")
    all_work=$(awk '
      function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^WORKLOAD / {print f("result_detections"),f("result_checksum");exit}' \
        "$RESULT_DIR/ablations/all/workload.log")
    ablation_matrix | while IFS=: read -r label stages; do
        dir="$RESULT_DIR/ablations/$label"
        result_fields=$(awk '
          function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}return "n/a"}
          /^RESULT / {print f("mean_ms"),f("detections"),f("checksum"),f("calls"),f("warmup"),f("stages"),f("rvv_mask");exit}' \
            "$dir/benchmark.log")
        workload_fields=$(awk '
          function f(n, i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}return "n/a"}
          /^WORKLOAD / {print f("result_detections"),f("result_checksum"),f("threshold_checksum"),f("stages"),f("rvv_mask"); exit}' "$dir/workload.log")
        set -- $result_fields
        mean=$1; production_detections=$2; production_checksum=$3
        production_output="$production_detections $production_checksum"
        measured=$4; warmup=$5; result_stages=$6; production_mask=$7
        set -- $workload_fields
        workload_output="$1 $2"; threshold_checksum=$3; workload_stages=$4; workload_mask=$5
        if [ "$result_stages" != "$stages" ] || [ "$workload_stages" != "$stages" ] || \
           [ "$workload_mask" != "$production_mask" ]; then
            echo "error: $label output metadata does not match configured stages '$stages'" >&2
            return 1
        fi
        validate_ablation_log "$dir/profile.log" "$stages" "$production_mask" \
            "$production_detections" "$production_checksum" 1 || {
            echo "error: $label profile metadata/output does not match production benchmark" >&2
            return 1
        }
        instrumented_mean=$(awk '
          function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
          /^RESULT / {print f("mean_ms");exit}' "$dir/profile.log")
        awk -v production="$mean" 'BEGIN {exit !(production + 0 > 0)}' || {
            echo "error: $label production_mean_ms must be greater than zero; got $mean" >&2
            return 1
        }
        overhead_fields=$(awk -v production="$mean" -v instrumented="$instrumented_mean" \
          'BEGIN {delta=instrumented-production; printf "%.3f %.3f",delta,delta/production*100}')
        set -- $overhead_fields
        overhead_ms=$1; overhead_pct=$2
        timer_fields=$(awk '
          function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
          /^CCL_TIMER_HEALTH / {print f("mean_unattributed_ratio_pct"),f("max_unattributed_ratio_pct"),f("warning");exit}' "$dir/profile.log")
        set -- $timer_fields
        timer_mean=$1; timer_max=$2; timer_warning=$3
        validate_ablation_log "$dir/perf.log" "$stages" "$production_mask" \
            "$production_detections" "$production_checksum" 0 || {
            echo "error: $label perf benchmark metadata/output does not match production benchmark" >&2
            return 1
        }
        isolated=n/a; disabled=n/a; equivalent_scalar=n/a; equivalent_all=n/a
        case $label in
          only-*)
            equivalent_scalar=1
            [ "$production_output" = "${scalar_result#* }" ] || equivalent_scalar=0
            [ "$workload_output" = "$scalar_work" ] || equivalent_scalar=0
            if [ "$equivalent_scalar" -eq 1 ]; then
                isolated=$(awk -v baseline="${scalar_result%% *}" -v mean="$mean" \
                    'BEGIN {printf "%.2f%%",(baseline-mean)/baseline*100}')
            fi
            ;;
          without-*)
            equivalent_all=1
            [ "$production_output" = "${all_result#* }" ] || equivalent_all=0
            [ "$workload_output" = "$all_work" ] || equivalent_all=0
            if [ "$equivalent_all" -eq 1 ]; then
                disabled=$(awk -v baseline="${all_result%% *}" -v mean="$mean" \
                    'BEGIN {printf "%.2f%%",(mean-baseline)/baseline*100}')
            fi
            ;;
        esac
        printf '%s stages=%s production_mean_ms=%s instrumented_mean_ms=%s instrumentation_overhead_ms=%s instrumentation_overhead_pct=%s isolated_gain=%s disabled_regression=%s equivalent_to_scalar=%s equivalent_to_all=%s detections=%s checksum=%s calls=%s warmup=%s' \
            "$label" "$stages" "$mean" "$instrumented_mean" "$overhead_ms" "$overhead_pct" \
            "$isolated" "$disabled" "$equivalent_scalar" \
            "$equivalent_all" "$production_detections" "$production_checksum" \
            "$measured" "$warmup" >>"$summary"
        set -- $workload_fields
        printf ' workload_detections=%s workload_checksum=%s threshold_checksum=%s' \
            "${1:-n/a}" "${2:-n/a}" "$threshold_checksum" >>"$summary"
        printf ' timer_mean_unattributed_ratio_pct=%s timer_max_unattributed_ratio_pct=%s timer_warning=%s' \
            "$timer_mean" "$timer_max" "$timer_warning" >>"$summary"
        perf_fields=$(awk '/^RESULT / { for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="calls")m=a[2];if(a[1]=="warmup")w=a[2]} print m,w; exit}' "$dir/perf.log" 2>/dev/null || true)
        set -- $perf_fields
        perf_measured=${1:-0}; perf_warmup=${2:-0}
        calls=$((perf_measured + perf_warmup + 1))
        [ -n "$calls" ] || calls=0
        for event in cycles instructions branches branch-misses; do
            value=n/a
            if [ -s "$dir/perf.stat" ] && [ "$calls" -gt 0 ]; then
                value=$(awk -F';' -v wanted="$event" -v calls="$calls" '
                  function trim(s){gsub(/^[[:space:]]+|[[:space:]]+$/,"",s);return s}
                  trim($3)==wanted && trim($1) ~ /^[0-9]+([.][0-9]+)?$/ {sum+=trim($1);n++}
                  END {if(n)printf "%.3f",sum/n/calls;else print "n/a"}' "$dir/perf.stat")
            fi
            key=$(printf '%s' "$event" | tr '-' '_')
            printf ' %s_per_call=%s' "$key" "$value" >>"$summary"
        done
        printf ' normalization=%s_measured+%s_warmup+1_validation\n' "$perf_measured" "$perf_warmup" >>"$summary"
        [ "$timer_warning" != 1 ] || echo "WARNING: $label CCL timer unattributed ratio exceeds 10%" >>"$summary"
        if [ "$equivalent_scalar" = 0 ]; then
            [ "$production_output" = "${scalar_result#* }" ] || echo "WARNING: $label production output differs from scalar; isolated gain is non-equivalent-output." >>"$summary"
            [ "$workload_output" = "$scalar_work" ] || echo "WARNING: $label workload output differs from scalar; isolated gain is non-equivalent-output." >>"$summary"
        elif [ "$equivalent_all" = 0 ]; then
            [ "$production_output" = "${all_result#* }" ] || echo "WARNING: $label production output differs from all; disabled regression is non-equivalent-output." >>"$summary"
            [ "$workload_output" = "$all_work" ] || echo "WARNING: $label workload output differs from all; disabled regression is non-equivalent-output." >>"$summary"
        fi
    done
}

run_ablations()
{
    mkdir "$RESULT_DIR/ablations"
    measured_calls=$((14 * (ABLATION_WARMUP + ABLATION_ITERATIONS * ABLATION_BATCHES + 1)))
    profile_calls=$measured_calls
    perf_calls=$((14 * (ABLATION_PERF_WARMUP + ABLATION_PERF_ITERATIONS * ABLATION_PERF_BATCHES + 1)))
    total_calls=$((measured_calls + profile_calls + perf_calls + 28))
    {
        echo "ablation_matrix=14"
        echo "ablation_production_calls=$measured_calls"
        echo "ablation_profile_calls=$profile_calls"
        echo "ablation_perf_calls=$perf_calls"
        echo "ablation_workload_calls=28"
        echo "ablation_detector_calls=$total_calls"
    } >>"$ENVIRONMENT"
    ablation_matrix | while IFS=: read -r label stages; do
        run_ablation_config "$label" "$stages" "$@"
    done
    write_ablation_summary
}

validate_input_records()
{
    label=$1 expected_width=$2 expected_height=$3
    metadata=$(awk -v label="$label" -v ew="$expected_width" -v eh="$expected_height" '
      function field_exact(n,kind,    i,a,v,hits){
        for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n){hits++;v=a[2]}}
        if(hits!=1){fail(kind " field " n " must occur exactly once");return ""}
        return v
      }
      function decimal_exact(n,kind,    v){v=field_exact(n,kind);if(v!~/^[0-9]+$/){fail(kind " field " n " is not an unsigned integer");return ""}return normalize_decimal(v)}
      function mean_exact(n,kind,    v){v=field_exact(n,kind);if(v!~/^[0-9]+([.][0-9]+)?$/){fail(kind " field " n " is not numeric");return ""}return v}
      function normalize_decimal(v){sub(/^0+/,"",v);return v==""?"0":v}
      function add_decimal(a,b,    carry,out,i,j,x,y,t){
        a=normalize_decimal(a);b=normalize_decimal(b);i=length(a);j=length(b)
        while(i>0||j>0||carry){x=i>0?substr(a,i--,1)+0:0;y=j>0?substr(b,j--,1)+0:0;t=x+y+carry;out=(t%10) out;carry=int(t/10)}
        return normalize_decimal(out)
      }
      function accept_input(kind,    h,w,x){
        h=field_exact("input_hash",kind);w=decimal_exact("width",kind);x=decimal_exact("height",kind)
        if(hash==""){hash=h;width=w;height=x}else if(h!=hash||w!=width||x!=height)fail(kind " input metadata differs")
        if(ew!=""&&(w!=ew||x!=eh))fail(kind " dimensions differ from requested size")
      }
      function count(kind,key){seen[kind SUBSEP key]++;if(seen[kind SUBSEP key]>1)fail("duplicate " kind " record: " key)}
      function profile_metadata(kind,    b,m,s){b=field_exact("backend",kind);m=field_exact("rvv_mask",kind);s=field_exact("stages",kind);if(b!="rust-rvv"||m!=prod_mask||s!=prod_stages)fail(kind " backend/mask/stages differ from production rust-rvv")}
      function fail(s){print "error: input " label ": " s > "/dev/stderr"; bad=1}
      FILENAME==ARGV[1]&&/^RESULT / {b=field_exact("backend","benchmark");count("benchmark",b);accept_input("benchmark");mean[b]=mean_exact("mean_ms","benchmark");det[b]=decimal_exact("detections","benchmark");sum[b]=field_exact("checksum","benchmark");if(b=="rust-rvv"){prod_mask=field_exact("rvv_mask","benchmark");prod_stages=field_exact("stages","benchmark")}next}
      FILENAME==ARGV[2]&&/^WORKLOAD / {b=field_exact("backend","WORKLOAD");count("workload",b);accept_input("WORKLOAD");wd=decimal_exact("result_detections","WORKLOAD");ws=field_exact("result_checksum","WORKLOAD");if(wd!=det[b]||ws!=sum[b])fail("workload output differs from production " b);if(b=="rust-rvv"){field_exact("rvv_mask","WORKLOAD");field_exact("stages","WORKLOAD");work_pending=decimal_exact("pending_boundary_records","WORKLOAD");work_points=decimal_exact("boundary_points_emitted","WORKLOAD")}next}
      FILENAME==ARGV[3]&&/^RESULT / {b=field_exact("backend","profile RESULT");count("profile-result",b);accept_input("profile RESULT");profile_metadata("profile RESULT");profile_mean=mean_exact("mean_ms","profile RESULT");pd=decimal_exact("detections","profile RESULT");ps=field_exact("checksum","profile RESULT");if(pd!=det["rust-rvv"]||ps!=sum["rust-rvv"])fail("profile output differs from production rust-rvv");next}
      FILENAME==ARGV[3]&&/^STAGE / {s=field_exact("stage","STAGE");count("stage",s);profile_metadata("STAGE " s);stage_mean=mean_exact("mean_ns","STAGE " s);if(s=="group_emit")group=stage_mean;if(s=="root_materialize")root=stage_mean;next}
      FILENAME==ARGV[3]&&/^CCL_WORK / {
        b=field_exact("backend","CCL_WORK");count("ccl-work",b);profile_metadata("CCL_WORK")
        runs=decimal_exact("runs","CCL_WORK");accepted=decimal_exact("accepted_grouping_records","CCL_WORK");keys=decimal_exact("distinct_keys","CCL_WORK")
        pending=points="0"
        for(i=0;i<4;i++){pending=add_decimal(pending,decimal_exact("pending_type_" i,"CCL_WORK"));points=add_decimal(points,decimal_exact("emitted_type_" i,"CCL_WORK"))}
        if(pending!=work_pending)fail("CCL_WORK pending sum differs from WORKLOAD pending_boundary_records")
        if(points!=work_points)fail("CCL_WORK emitted sum differs from WORKLOAD boundary_points_emitted")
        next
      }
      FILENAME==ARGV[3]&&/^CCL_TIMER_HEALTH / {b=field_exact("backend","CCL_TIMER_HEALTH");count("timer-health",b);profile_metadata("CCL_TIMER_HEALTH");next}
      END {
        split("rust-rvv rust-scalar c-reference",required," ")
        for(i=1;i<=3;i++){b=required[i];if(seen["benchmark" SUBSEP b]!=1)fail("missing benchmark RESULT for " b);if(seen["workload" SUBSEP b]!=1)fail("missing WORKLOAD for " b)}
        if(seen["profile-result" SUBSEP "rust-rvv"]!=1)fail("missing profile RESULT")
        if(group==""||root=="")fail("missing required group_emit/root_materialize STAGE")
        if(seen["ccl-work" SUBSEP "rust-rvv"]!=1)fail("missing CCL_WORK")
        if(seen["timer-health" SUBSEP "rust-rvv"]!=1)fail("missing CCL_TIMER_HEALTH")
        if(!bad)printf "%s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s\n",hash,width,height,mean["rust-rvv"],mean["rust-scalar"],mean["c-reference"],det["rust-rvv"],sum["rust-rvv"],profile_mean,group,root,runs,pending,accepted,keys,points
        exit bad
      }' "$RESULT_DIR/comparison.log" "$RESULT_DIR/workload.log" "$RESULT_DIR/ccl-profile.log") || return 1
    set -- $metadata
    [ "$#" -eq 16 ] || { echo "error: input $label: malformed validated metadata" >&2; return 1; }
    INPUT_HASH=$1; INPUT_WIDTH=$2; INPUT_HEIGHT=$3
    RVV_MEAN=$4; SCALAR_MEAN=$5; C_MEAN=$6; INPUT_DETECTIONS=$7; INPUT_CHECKSUM=$8
    PROFILE_MEAN=$9; shift 9
    GROUP_EMIT_MEAN=$1; ROOT_MATERIALIZE_MEAN=$2; CCL_RUNS=$3; CCL_PENDING=$4
    CCL_ACCEPTED=$5; CCL_KEYS=$6; CCL_POINTS=$7

    rvv_call_fields=$(awk -v label="$label" '
      function exact(n,    i,a,v,hits){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n){hits++;v=a[2]}}if(hits!=1)fail("RESULT field " n " must occur exactly once");return v}
      function uint(n,    v){v=exact(n);if(v!~/^[0-9]+$/)fail("RESULT field " n " is not an unsigned integer");return v}
      function fail(s){print "error: input " label ": rust-rvv perf stat " s > "/dev/stderr";bad=1}
      /^RESULT /{backend=exact("backend");if(backend=="rust-rvv"){records++;measured=uint("calls");warmup=uint("warmup")}}
      END{if(records!=1)fail("requires exactly one rust-rvv RESULT");if(!bad)print measured,warmup;exit bad}' \
        "$RESULT_DIR/rust-rvv.log") || return 1
    set -- $rvv_call_fields
    [ "$#" -eq 2 ] || { echo "error: input $label: malformed rust-rvv RESULT call counts" >&2; return 1; }
    rvv_total_calls=$(($1 + $2 + 1))
    RVV_PER_CALL=$(awk -F';' -v label="$label" -v calls="$rvv_total_calls" '
      function trim(s){gsub(/^[[:space:]]+|[[:space:]]+$/, "", s);return s}
      function fail(s){print "error: input " label ": rust-rvv perf stat " s > "/dev/stderr";bad=1}
      {
        value=trim($1);event=trim($3)
        if(event=="cycles"||event=="instructions"||event=="branches"||event=="branch-misses"){
          seen[event]++
          if(seen[event]>1)fail("duplicate counter " event)
          if(value!~/^[0-9]+([.][0-9]+)?$/)fail("counter " event " is not numeric")
          total[event]=value+0
        }
      }
      END{
        split("cycles instructions branches branch-misses",required," ")
        for(i=1;i<=4;i++)if(seen[required[i]]!=1)fail("missing counter " required[i])
        if(!bad)printf "%.6f %.6f %.6f %.6f\n",total["cycles"]/calls,total["instructions"]/calls,total["branches"]/calls,total["branch-misses"]/calls
        exit bad
      }' "$RESULT_DIR/rust-rvv.stat") || return 1
    set -- $RVV_PER_CALL
    [ "$#" -eq 4 ] || { echo "error: input $label: malformed rust-rvv perf counters" >&2; return 1; }
    RVV_CYCLES_PER_CALL=$1; RVV_INSTRUCTIONS_PER_CALL=$2
    RVV_BRANCHES_PER_CALL=$3; RVV_BRANCH_MISSES_PER_CALL=$4

    for backend in rust-rvv rust-scalar c; do
        case $backend in
            rust-rvv) record_backend=rust-rvv; expected_detections=$INPUT_DETECTIONS; expected_checksum=$INPUT_CHECKSUM;;
            rust-scalar) record_backend=rust-scalar; expected_detections=$(awk 'function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}} /^RESULT /&&f("backend")=="rust-scalar"{print f("detections");exit}' "$RESULT_DIR/comparison.log"); expected_checksum=$(awk 'function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}} /^RESULT /&&f("backend")=="rust-scalar"{print f("checksum");exit}' "$RESULT_DIR/comparison.log");;
            c) record_backend=c-reference; expected_detections=$(awk 'function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}} /^RESULT /&&f("backend")=="c-reference"{print f("detections");exit}' "$RESULT_DIR/comparison.log"); expected_checksum=$(awk 'function f(n,i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}} /^RESULT /&&f("backend")=="c-reference"{print f("checksum");exit}' "$RESULT_DIR/comparison.log");;
        esac
        validate_backend_result_log "$label" "$record_backend" "$expected_detections" \
            "$expected_checksum" "$RESULT_DIR/$backend.log" "perf stat" || return 1
        validate_backend_result_log "$label" "$record_backend" "$expected_detections" \
            "$expected_checksum" "$RESULT_DIR/$backend-record.log" "flat sample" || return 1
        if [ "$MODE" = full ] && [ -s "$RESULT_DIR/$backend-callgraph.data" ]; then
            validate_backend_result_log "$label" "$record_backend" "$expected_detections" \
                "$expected_checksum" "$RESULT_DIR/$backend-callgraph-record.log" "callgraph sample" || return 1
        fi
    done
}

validate_backend_result_log()
{
    label=$1 wanted=$2 detections=$3 checksum=$4 log_file=$5 log_kind=$6
    awk -v label="$label" -v wanted="$wanted" -v hash="$INPUT_HASH" \
        -v width="$INPUT_WIDTH" -v height="$INPUT_HEIGHT" -v detections="$detections" \
        -v checksum="$checksum" -v kind="$log_kind" '
          function exact(name,    i,a,v,hits){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==name){hits++;v=a[2]}}if(hits!=1)bad=1;return v}
          /^RESULT /{backend=exact("backend");if(backend==wanted){n++;if(exact("input_hash")!=hash||exact("width")!=width||exact("height")!=height||exact("detections")!=detections||exact("checksum")!=checksum)bad=1}}
          END{if(n!=1||bad){print "error: input " label ": " kind " RESULT metadata/output mismatch for " wanted > "/dev/stderr";exit 1}}' \
        "$log_file"
}

append_cross_input_record()
{
    label=$1 file_hash=$2
    printf 'INPUT label=%s file_sha256=%s input_hash=%s width=%s height=%s rust_rvv_mean_ms=%s rust_scalar_mean_ms=%s c_reference_mean_ms=%s detections=%s checksum=%s instrumented_mean_ms=%s group_emit_mean_ns=%s root_materialize_mean_ns=%s runs=%s pending=%s accepted=%s keys=%s points=%s cycles_per_call=%s instructions_per_call=%s branches_per_call=%s branch_misses_per_call=%s\n' \
        "$label" "$file_hash" "$INPUT_HASH" "$INPUT_WIDTH" "$INPUT_HEIGHT" "$RVV_MEAN" \
        "$SCALAR_MEAN" "$C_MEAN" "$INPUT_DETECTIONS" "$INPUT_CHECKSUM" "$PROFILE_MEAN" \
        "$GROUP_EMIT_MEAN" "$ROOT_MATERIALIZE_MEAN" "$CCL_RUNS" "$CCL_PENDING" \
        "$CCL_ACCEPTED" "$CCL_KEYS" "$CCL_POINTS" "$RVV_CYCLES_PER_CALL" \
        "$RVV_INSTRUCTIONS_PER_CALL" "$RVV_BRANCHES_PER_CALL" \
        "$RVV_BRANCH_MISSES_PER_CALL" >>"$CROSS_SUMMARY_TMP"
}

run_profile_workflow()
{
    run_workload "$@"
    run_profile_workflow_after_workload "$@"
}

run_profile_workflow_after_workload()
{
    capture_environment "$@"
    live_demos=$(pidof apriltag_demo.elf apriltag_c_demo.elf 2>/dev/null || true)
    [ -z "$live_demos" ] || warn "live AprilTag demos are running (PIDs: $live_demos); stop them for uncontended results"
    if [ "${MULTI_INPUT:-0}" -eq 0 ] || [ "${MULTI_PROBED:-0}" -eq 0 ]; then
        probe_events "$@"
        probe_sampling "$@"
        [ "${MULTI_INPUT:-0}" -eq 0 ] || MULTI_PROBED=1
    else
        echo "stat_events=$STAT_EVENTS" >>"$ENVIRONMENT"
        echo "sample_event=$SAMPLE_EVENT" >>"$ENVIRONMENT"
    fi
    run_comparison "$@"
    [ "${MULTI_INPUT:-0}" -eq 0 ] || run_ccl_profile "$@"
    for backend in rust-rvv rust-scalar c; do
        run_stat "$backend" "$@"
        record_flat "$backend" "$@"
        if [ "$MODE" = full ]; then record_callgraph "$backend" "$@"; fi
    done
    if [ "$MODE" = full ]; then
        annotate_full rust-rvv
        annotate_full c
    fi
    if [ "$ABLATIONS" -eq 1 ]; then run_ablations "$@"; fi
    write_summary
}

run_input_profile()
{
    input_label=$1 input_path=$2 input_size=$3 expected_file_hash=$4 input_dir=$5
    shift 5
    FIXTURE=$input_path
    INPUT_SIZE=$input_size
    RESULT_DIR=$input_dir
    ENVIRONMENT=$RESULT_DIR/environment.txt
    CURRENT_INPUT_LABEL=$input_label
    CURRENT_INPUT_PATH=$input_path
    CURRENT_INPUT_FILE_HASH=$expected_file_hash
    CURRENT_INPUT_SIZE=$input_size
    CURRENT_INPUT_WIDTH_IN_RESULT_PATH=$(awk '
      function f(n, i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^WORKLOAD /&&f("backend")=="rust-rvv"{print f("width") "x" f("height");exit}' \
        "$RESULT_DIR/workload.log" 2>/dev/null || true)
    if [ "${MULTI_PROBED:-0}" -eq 0 ]; then
        STAT_EVENTS=
        SAMPLE_EVENT=
        TIMING_EVENTS=0
    fi
    echo "Writing input '$input_label' profile to $RESULT_DIR"
    run_workload "$@"
    CURRENT_INPUT_WIDTH_IN_RESULT_PATH=$(awk '
      function f(n, i,a){for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==n)return a[2]}}
      /^WORKLOAD /&&f("backend")=="rust-rvv"{print f("width") "x" f("height");exit}' \
        "$RESULT_DIR/workload.log")
    run_profile_workflow_after_workload "$@"
    expected_width= expected_height=
    if [ "$input_size" != native ]; then expected_width=${input_size%x*}; expected_height=${input_size#*x}; fi
    validate_input_records "$input_label" "$expected_width" "$expected_height"
    final_file_hash=$(hash_file "$input_path") || return 1
    if [ "$final_file_hash" != "$expected_file_hash" ]; then
        echo "error: input $input_label: input file changed during profiling" >&2
        return 1
    fi
    append_cross_input_record "$input_label" "$expected_file_hash"
    CURRENT_INPUT_LABEL=
    CURRENT_INPUT_PATH=
    CURRENT_INPUT_FILE_HASH=
    CURRENT_INPUT_SIZE=
    CURRENT_INPUT_WIDTH_IN_RESULT_PATH=
}

self_test_profile_detector()
{
    test_root=${TMPDIR:-/tmp}/profile-detector-self-test.$$
    mkdir "$test_root" "$test_root/bin"
    trap 'rm -rf "$test_root"' 0 HUP INT TERM
    split_hash_path /image.jpg
    [ "$HASH_DIR" = / ]
    [ "$HASH_BASE" = image.jpg ]
    fake_bench="$test_root/fake-bench"
    fake_workload="$test_root/fake-workload"
    fake_profile="$test_root/fake-profile"
    fake_perf="$test_root/fake-perf"
    fake_tee="$test_root/bin/tee"
    fake_nm="$test_root/bin/nm"
cat >"$fake_bench" <<'EOF'
#!/bin/sh
[ "${LC_ALL:-}" = C ] || exit 42
backend=all
stages=default
warmup=; iterations=; batches=
input=; size=; dump_dir=
for arg in "$@"; do
  prev=${current:-}; current=$arg
  [ "$prev" = --backend ] && backend=$arg
  [ "$prev" = --rvv-stages ] && stages=$arg
  [ "$prev" = --warmup ] && warmup=$arg
  [ "$prev" = --iterations ] && iterations=$arg
  [ "$prev" = --batches ] && batches=$arg
  [ "$prev" = --input ] && input=$arg
  [ "$prev" = --size ] && size=$arg
  [ "$prev" = --dump-dir ] && dump_dir=$arg
done
[ -z "$dump_dir" ] || mkdir -p "$dump_dir"
case $input in *second.y8) hash=222;width=640;height=360;detections=9;checksum=cc;; *) hash=111;width=1280;height=720;detections=7;checksum=aa;; esac
[ "$size" != native ] || { width=640; height=360; }
echo "bench backend=$backend stages=$stages warmup=$warmup iterations=$iterations batches=$batches" >>"${FAKE_COMMAND_TRACE:-/dev/null}"
[ "${MUTATE_INPUT:-}" != "$input" ] || [ -e "${MUTATE_SENTINEL:-/dev/null}" ] || {
  printf mutation >>"$input"
  : >"$MUTATE_SENTINEL"
}
[ "${FAIL_INPUT:-}" != "$input" ] || exit 24
[ "${FAIL_BENCH:-0}" -eq 0 ] || [ "$backend" != all ] || exit 23
[ "$backend" = all ] && backends='rust-rvv rust-scalar c-reference' || { [ "$backend" = c ] && backends=c-reference || backends=$backend; }
for b in $backends; do
  case $b in
    rust-rvv)
      case $stages in
        none) m=20.000;; all|default) m=10.000;; decimate) m=18.000;;
        decimate,threshold,rle,lfps-tuned,gaussian) m=11.000;; *) m=12.000;;
      esac
      [ "${ZERO_PRODUCTION_MEAN:-0}" -eq 0 ] || [ "$stages" != decimate ] || m=0.000
      case $stages in threshold) c=prod-mismatch;; *) c=$checksum;; esac;;
    rust-scalar) m=20.000;c=$checksum;; *) m=15.000;[ "$hash" = 222 ] && c=dd || c=bb;;
  esac
  result_stages=$stages
  result_mask=mock
  [ "${FAIL_METADATA:-}" != perf-mask ] || [ -z "${FAKE_UNDER_PERF:-}" ] || result_mask=wrong
  if [ "${FAKE_PERF_CONTEXT:-}" = stat ] && [ "${MATRIX_FAILURE:-}" = perf-stat-output ] && [ "$b" = rust-rvv ]; then c=perf-wrong; fi
  if [ "${FAKE_PERF_CONTEXT:-}" = flat ] && [ "${MATRIX_FAILURE:-}" = perf-flat-output ] && [ "$b" = rust-rvv ]; then c=flat-wrong; fi
  if [ "${FAKE_PERF_CONTEXT:-}" = callgraph ] && [ "${MATRIX_FAILURE:-}" = perf-callgraph-output ] && [ "$b" = rust-rvv ]; then c=callgraph-wrong; fi
  calls=$((iterations * batches))
  result_extra=
  [ "${MATRIX_FAILURE:-}" != duplicate-result-field ] || [ "$b" != rust-rvv ] || result_extra=' mean_ms=99'
  [ "${MATRIX_FAILURE:-}" != malformed-result-mean ] || [ "$b" != rust-rvv ] || m=bad
  [ "${MATRIX_FAILURE:-}" != malformed-result-dimension ] || [ "$b" != rust-rvv ] || width=bad
  echo "RESULT backend=$b rvv_mask=$result_mask stages=$result_stages calls=$calls mean_ms=$m median_ms=$m detections=$detections checksum=$c input_hash=$hash width=$width height=$height build=fake warmup=$warmup iterations=$iterations batches=$batches$result_extra"
done
EOF
    cat >"$fake_profile" <<'EOF'
#!/bin/sh
stages=default
warmup=; iterations=; batches=
input=; size=
for arg in "$@"; do
  prev=${current:-}; current=$arg
  [ "$prev" = --rvv-stages ] && stages=$arg
  [ "$prev" = --warmup ] && warmup=$arg
  [ "$prev" = --iterations ] && iterations=$arg
  [ "$prev" = --batches ] && batches=$arg
  [ "$prev" = --input ] && input=$arg
  [ "$prev" = --size ] && size=$arg
done
case $input in
  *second.y8) hash=222;width=640;height=360;detections=9;checksum=cc;runs=52;accepted=62;keys=72
    p0=9007199254740993;p1=0002;p2=3;p3=4;pending=09007199254741002
    e0=9007199254740995;e1=5;e2=6;e3=7;points=9007199254741013;;
  *) hash=111;width=1280;height=720;detections=7;checksum=aa;runs=42;accepted=60;keys=70
    p0=10;p1=20;p2=30;p3=40;pending=100;e0=20;e1=30;e2=40;e3=110;points=200;;
esac
[ "$size" != native ] || { width=640; height=360; }
echo "profile stages=$stages warmup=$warmup iterations=$iterations batches=$batches" >>"${FAKE_COMMAND_TRACE:-/dev/null}"
[ "${FAIL_PROFILE_STAGE:-}" != "$stages" ] || exit 37
case $stages in
  none) mean=20.000;; all) mean=10.000;; decimate) mean=18.000;;
  threshold) mean=17.000;; rle) mean=16.000;; lfps-tuned) mean=15.000;;
  gaussian) mean=14.000;; gray-model) mean=13.000;;
  decimate,threshold,rle,lfps-tuned,gaussian) mean=11.000;; *) mean=12.000;;
esac
profile_stages=$stages
profile_mask=mock
[ "${FAIL_METADATA:-}" != profile-mask ] || profile_mask=wrong
profile_backend=rust-rvv
[ "${MATRIX_FAILURE:-}" != profile-backend ] || profile_backend=rust-scalar
[ "${MATRIX_FAILURE:-}" != profile-mask ] || profile_mask=wrong
profile_checksum=$checksum
[ "$stages" != threshold ] || profile_checksum=prod-mismatch
instrumented_mean=$(awk -v mean="$mean" 'BEGIN {printf "%.3f",mean+2}')
echo "RESULT backend=$profile_backend rvv_mask=$profile_mask stages=$profile_stages calls=100 mean_ms=$instrumented_mean median_ms=$instrumented_mean detections=$detections checksum=$profile_checksum input_hash=$hash width=$width height=$height build=fake-profile warmup=$warmup iterations=$iterations batches=$batches"
echo "STAGE backend=rust-rvv rvv_mask=$profile_mask stages=$profile_stages stage=total count=100 min_ns=1 median_ns=2 mean_ns=3 p95_ns=4 max_ns=5"
stage_backend=rust-rvv
[ "${MATRIX_FAILURE:-}" != stage-backend ] || stage_backend=rust-scalar
stage_extra=
[ "${MATRIX_FAILURE:-}" != duplicate-stage-field ] || stage_extra=' mean_ns=99'
stage_mean=30
[ "${MATRIX_FAILURE:-}" != malformed-stage-mean ] || stage_mean=bad
[ "${MATRIX_FAILURE:-}" = stage-missing ] || echo "STAGE backend=$stage_backend rvv_mask=$profile_mask stages=$profile_stages stage=group_emit count=100 min_ns=10 median_ns=20 mean_ns=$stage_mean p95_ns=40 max_ns=50$stage_extra"
echo "STAGE backend=rust-rvv rvv_mask=$profile_mask stages=$profile_stages stage=root_materialize count=100 min_ns=11 median_ns=21 mean_ns=31 p95_ns=41 max_ns=51"
[ "${MATRIX_FAILURE:-}" != ccl-pending-mismatch ] || p3=41
[ "${MATRIX_FAILURE:-}" != ccl-emitted-mismatch ] || e3=111
ccl_extra=
[ "${MATRIX_FAILURE:-}" != duplicate-ccl-field ] || ccl_extra=' runs=99'
[ "${MATRIX_FAILURE:-}" != malformed-ccl-counter ] || runs=bad
[ "${MATRIX_FAILURE:-}" = ccl-work-missing ] || echo "CCL_WORK backend=rust-rvv rvv_mask=$profile_mask stages=$profile_stages validity=0x1ff runs=$runs accepted_grouping_records=$accepted distinct_keys=$keys pending_type_0=$p0 pending_type_1=$p1 pending_type_2=$p2 pending_type_3=$p3 emitted_type_0=$e0 emitted_type_1=$e1 emitted_type_2=$e2 emitted_type_3=$e3$ccl_extra"
health_warning=0
[ "$stages" != gaussian ] || health_warning=1
timer_extra=
[ "${MATRIX_FAILURE:-}" != duplicate-timer-field ] || timer_extra=' backend=rust-rvv'
[ "${MATRIX_FAILURE:-}" = timer-health-missing ] || echo "CCL_TIMER_HEALTH backend=rust-rvv rvv_mask=$profile_mask stages=$profile_stages count=100 diagnostic_included=1 mean_unattributed_ratio_pct=5.000 max_unattributed_ratio_pct=12.000 warning=$health_warning$timer_extra"
[ "$health_warning" -eq 0 ] || echo 'WARNING: CCL timer unattributed ratio exceeds 10%'
EOF
    cat >"$fake_workload" <<'EOF'
#!/bin/sh
backend=all
stages=default
warmup=; iterations=; batches=
input=; size=
for arg in "$@"; do
  prev=${current:-}; current=$arg
  [ "$prev" = --backend ] && backend=$arg
  [ "$prev" = --rvv-stages ] && stages=$arg
  [ "$prev" = --warmup ] && warmup=$arg
  [ "$prev" = --iterations ] && iterations=$arg
  [ "$prev" = --batches ] && batches=$arg
  [ "$prev" = --input ] && input=$arg
  [ "$prev" = --size ] && size=$arg
done
case $input in *second.y8) hash=222;width=640;height=360;detections=9;checksum=cc;pending=9007199254741002;points=9007199254741013;; *) hash=111;width=1280;height=720;detections=7;checksum=aa;pending=100;points=200;; esac
[ "$size" != native ] || { width=640; height=360; }
echo "workload backend=$backend stages=$stages warmup=$warmup iterations=$iterations batches=$batches" >>"${FAKE_COMMAND_TRACE:-/dev/null}"
echo workload >>"${FAKE_PERF_TRACE:-/dev/null}"
    rust_validity=0x7f
[ "${INVALID_FITTING:-0}" -eq 0 ] || rust_validity=0x77
[ "${INVALID_THRESHOLD:-0}" -eq 0 ] || rust_validity=0x7d
workload_checksum=$checksum
workload_mask=mock
[ "${FAIL_METADATA:-}" != workload-mask ] || workload_mask=wrong
[ "$stages" != decimate,threshold,lfps-tuned,gaussian,gray-model ] || workload_checksum=work-mismatch
[ "${MATRIX_FAILURE:-}" != workload-hash ] || hash=wrong
[ "${MATRIX_FAILURE:-}" != workload-output ] || workload_checksum=wrong
workload_extra=
[ "${MATRIX_FAILURE:-}" != duplicate-workload-field ] || workload_extra=' result_checksum=again'
[ "${MATRIX_FAILURE:-}" != malformed-workload-counter ] || pending=bad
[ "${MATRIX_FAILURE:-}" = workload-missing ] || echo "WORKLOAD backend=rust-rvv rvv_mask=$workload_mask stages=$stages schema=1 validity=$rust_validity boundary_points_emitted=$points pending_boundary_records=$pending clusters_after_filters=20 points_entering_sort=160 points_entering_lfps=140 points_entering_errors=130 compute_errors_points=120 raw_peaks=12 retained_peaks=10 quad_fit_attempts=4 quads=8 decode_attempts=5 uf_elements=90 line_fit_queries=40 line_fit_query_points=80 threshold_checksum=10 result_detections=$detections result_checksum=$workload_checksum input_hash=$hash width=$width height=$height detections=$detections$workload_extra"
[ "${MATRIX_FAILURE:-}" != workload-duplicate ] || echo "WORKLOAD backend=rust-rvv rvv_mask=$workload_mask stages=$stages schema=1 validity=$rust_validity result_detections=$detections result_checksum=$workload_checksum input_hash=$hash width=$width height=$height detections=$detections"
echo "WORKLOAD backend=rust-scalar schema=1 validity=0x7f boundary_points_emitted=200 clusters_after_filters=20 points_entering_sort=160 points_entering_lfps=140 points_entering_errors=130 compute_errors_points=120 raw_peaks=12 retained_peaks=10 quad_fit_attempts=4 quads=8 decode_attempts=5 uf_elements=90 line_fit_queries=40 line_fit_query_points=80 threshold_checksum=10 result_detections=$detections result_checksum=$checksum input_hash=$hash width=$width height=$height detections=$detections"
[ "$hash" = 222 ] && c_checksum=dd || c_checksum=bb
echo "WORKLOAD backend=c-reference schema=1 validity=0x7f boundary_points_emitted=100 clusters_after_filters=10 points_entering_sort=80 points_entering_lfps=70 points_entering_errors=65 compute_errors_points=60 raw_peaks=6 retained_peaks=5 quad_fit_attempts=2 quads=4 decode_attempts=3 uf_elements=50 line_fit_queries=30 line_fit_query_points=60 threshold_checksum=20 result_detections=$detections result_checksum=$c_checksum input_hash=$hash width=$width height=$height detections=$detections"
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
  stat) echo perf-stat >>"${FAKE_PERF_TRACE:-/dev/null}"; [ "$delimiter" = ';' ] || exit 41; case " $* " in *'matrix files'*) matrix_stat=1;; *) matrix_stat=0;; esac; [ "$event" != branches ] || [ "$matrix_stat" -eq 1 ] || { echo '<not supported>' >"$out"; exit 1; }; case " $* " in *second.y8*) scale=2;; *) scale=1;; esac; FAKE_UNDER_PERF=1 FAKE_PERF_CONTEXT=stat "$@"; rc=$?; { if [ "${MATRIX_FAILURE:-}" = perf-stat-malformed-counter ]; then echo 'bad;;cycles;100.00;100.00'; else echo "$((1000 * scale));;cycles;100.00;100.00"; fi; echo "$((2000 * scale));;instructions;100.00;100.00"; if [ "$matrix_stat" -eq 1 ]; then [ "${MATRIX_FAILURE:-}" = perf-stat-missing-counter ] || echo "$((300 * scale));;branches;100.00;100.00"; awk -v scale="$scale" 'BEGIN{printf "%.1f;;branch-misses;100.00;100.00\n",30.5*scale}'; [ "${MATRIX_FAILURE:-}" != perf-stat-duplicate-counter ] || echo '31;;branch-misses;100.00;100.00'; fi; echo '3.0;msec;task-clock;100.00;100.00'; echo '4;;cache/misses:u;100.00;100.00'; } >"$out"; exit $rc;;
 record) echo perf-record >>"${FAKE_PERF_TRACE:-/dev/null}"; [ "$event" = cycles:u ] && exit 1; [ "$callgraph" -eq 0 ] || [ "${FAIL_CALLGRAPH:-0}" -eq 0 ] || exit 29; if [ "$callgraph" -eq 1 ]; then FAKE_PERF_CONTEXT=callgraph "$@"; else FAKE_PERF_CONTEXT=flat "$@"; fi; rc=$?; [ "$rc" -eq 0 ] && echo data >"$out"; exit $rc;;
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
    chmod +x "$fake_bench" "$fake_profile" "$fake_workload" "$fake_perf" "$fake_tee" "$fake_nm"
    touch "$test_root/fixture"
    mkdir "$test_root/matrix files"
    touch "$test_root/matrix files/first image.jpg" "$test_root/matrix files/second.y8" \
        "$test_root/matrix files/-" "$test_root/matrix files/-leading.jpg"
    matrix_root="$test_root/matrix-results"
    whitespace_line=$(printf '\t  ')
    matrix_inputs="
$whitespace_line
first=$test_root/matrix files/first image.jpg,1280x720
second=$test_root/matrix files/second.y8,native
$whitespace_line"
    option_matrix_inputs="
dash=$test_root/matrix files/-,native
leading=$test_root/matrix files/-leading.jpg,native
$whitespace_line"
    PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
        APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
        APRILTAG_PROFILE_OUTPUT="$matrix_root" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >"$test_root/matrix-run.log" 2>&1 || {
                cat "$test_root/matrix-run.log" >&2
                return 1
            }
    matrix_output=$(ls -d "$matrix_root"/run-* 2>/dev/null)
    tab=$(printf '\t')
    first_canonical=$(readlink -f "$test_root/matrix files/first image.jpg")
    second_canonical=$(readlink -f "$test_root/matrix files/second.y8")
    grep -F -q "first${tab}$first_canonical${tab}1280${tab}720${tab}" \
        "$matrix_output/inputs.tsv"
    grep -F -q "second${tab}$second_canonical${tab}native${tab}" \
        "$matrix_output/inputs.tsv"
    probe_stats=$(grep -c '^perf-stat$' "$test_root/perf-trace")
    probe_records=$(grep -c '^perf-record$' "$test_root/perf-trace")
    [ "$probe_stats" -eq 8 ]
    [ "$probe_records" -eq 8 ]
    for label in first second; do
        for artifact in environment.txt workload.log workload-summary.txt comparison.log images \
            ccl-profile.log rust-rvv.stat rust-rvv.data rust-rvv.report rust-scalar.stat \
            rust-scalar.data rust-scalar.report c.stat c.data c.report summary.txt; do
            [ -e "$matrix_output/inputs/$label/$artifact" ]
        done
    done
    [ -s "$matrix_output/cross-input-summary.txt" ] || { echo 'matrix cross summary missing' >&2; return 1; }
    grep -q '^INPUT label=first file_sha256=.* input_hash=111 width=1280 height=720 ' "$matrix_output/cross-input-summary.txt"
    grep -q '^INPUT label=second file_sha256=.* input_hash=222 width=640 height=360 ' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=first .*group_emit_mean_ns=30 .*root_materialize_mean_ns=31' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=second .*group_emit_mean_ns=30 .*root_materialize_mean_ns=31' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=first .*runs=42 pending=100 accepted=60 keys=70 points=200' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=second .*runs=52 pending=9007199254741002 accepted=62 keys=72 points=9007199254741013' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=first .*cycles_per_call=1.919386 .*instructions_per_call=3.838772 .*branches_per_call=0.575816 .*branch_misses_per_call=0.058541' "$matrix_output/cross-input-summary.txt"
    grep -q 'label=second .*cycles_per_call=3.838772 .*instructions_per_call=7.677543 .*branches_per_call=1.151631 .*branch_misses_per_call=0.117083' "$matrix_output/cross-input-summary.txt"
    grep -F -q 'Input label: first' "$matrix_output/inputs/first/summary.txt"
    grep -F -q "Canonical path: $first_canonical" "$matrix_output/inputs/first/summary.txt"
    grep -F -q 'Requested size: 1280x720' "$matrix_output/inputs/first/summary.txt"
    grep -F -q 'Native RESULT dimensions: 1280x720' "$matrix_output/inputs/first/summary.txt"
    grep -F -q 'Input label: second' "$matrix_output/inputs/second/environment.txt"
    grep -F -q "Canonical path: $second_canonical" "$matrix_output/inputs/second/environment.txt"
    grep -F -q 'Requested size: native' "$matrix_output/inputs/second/environment.txt"
    grep -F -q 'Native RESULT dimensions: 640x360' "$matrix_output/inputs/second/environment.txt"
    ! grep -F -q 'Input label: second' "$matrix_output/inputs/first/summary.txt"
    ! grep -F -q 'Input label: first' "$matrix_output/inputs/second/environment.txt"
    ! grep -q 'input_hash=222' "$matrix_output/inputs/first/summary.txt"
    ! grep -q 'input_hash=111' "$matrix_output/inputs/second/summary.txt"
    option_root="$test_root/option-name-results"
    PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$option_matrix_inputs" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
        APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$option_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    option_output=$(ls -d "$option_root"/run-* 2>/dev/null)
    [ -s "$option_output/cross-input-summary.txt" ]

    mutation_root="$test_root/mutation-results"
    set +e
    MUTATE_INPUT="$second_canonical" MUTATE_SENTINEL="$test_root/mutated" \
        PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
        APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$mutation_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' "$TEST_SHELL" "$0" fast \
        >"$test_root/mutation.log" 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ]
    grep -q 'input file changed during profiling' "$test_root/mutation.log"
    mutation_output=$(ls -d "$mutation_root"/run-* 2>/dev/null)
    [ ! -e "$mutation_output/cross-input-summary.txt" ]
    assert_matrix_failure()
    {
        failure=$1
        failure_root="$test_root/matrix-negative-$failure"
        set +e
        MATRIX_FAILURE="$failure" PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
            APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
            APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
            APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$failure_root" \
            APRILTAG_PROFILE_EVENTS='task-clock,cycles' "$TEST_SHELL" "$0" fast >/dev/null 2>&1
        rc=$?
        set -e
        [ "$rc" -ne 0 ]
        failure_output=$(ls -d "$failure_root"/run-* 2>/dev/null)
        [ ! -e "$failure_output/cross-input-summary.txt" ]
    }
    for matrix_failure in workload-hash workload-output workload-missing workload-duplicate \
        profile-backend profile-mask stage-backend stage-missing ccl-work-missing \
        ccl-pending-mismatch ccl-emitted-mismatch duplicate-result-field \
        duplicate-workload-field duplicate-stage-field duplicate-ccl-field \
        duplicate-timer-field malformed-result-mean malformed-result-dimension \
        malformed-workload-counter malformed-stage-mean malformed-ccl-counter \
        timer-health-missing perf-stat-output perf-flat-output perf-stat-missing-counter \
        perf-stat-duplicate-counter perf-stat-malformed-counter; do
        assert_matrix_failure "$matrix_failure"
    done
    failure_root="$test_root/matrix-negative-perf-callgraph-output"
    set +e
    MATRIX_FAILURE=perf-callgraph-output PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
        APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
        APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
        APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$failure_root" \
        APRILTAG_PROFILE_EVENTS='task-clock,cycles' "$TEST_SHELL" "$0" full >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ]
    failure_output=$(ls -d "$failure_root"/run-* 2>/dev/null)
    [ ! -e "$failure_output/cross-input-summary.txt" ]

    for conflicting_args in '--input override.jpg' '--input=override.jpg' '--format raw' \
        '--format=raw' '--size 1x1' '--size=1x1'; do
        set +e
        # Intentional splitting: these fixed test strings exercise both CLI forms.
        # shellcheck disable=SC2086
        PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
            APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
            APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
            APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$test_root/conflict" \
            APRILTAG_PROFILE_EVENTS='task-clock,cycles' "$TEST_SHELL" "$0" fast $conflicting_args \
            >"$test_root/conflict.log" 2>&1
        rc=$?
        set -e
        [ "$rc" -eq 2 ]
        grep -q 'multi-input mode does not accept user --input, --format, or --size options' "$test_root/conflict.log"
    done
    fail_matrix_root="$test_root/matrix-failure"
    set +e
    FAIL_INPUT="$test_root/matrix files/second.y8" PATH="$test_root/bin:$PATH" \
        APRILTAG_PROFILE_INPUTS="$matrix_inputs" APRILTAG_PROFILE_BENCH="$fake_bench" \
        APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
        APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
        APRILTAG_PROFILE_OUTPUT="$fail_matrix_root" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ]
    fail_matrix_output=$(ls -d "$fail_matrix_root"/run-* 2>/dev/null)
    [ -s "$fail_matrix_output/inputs/first/summary.txt" ]
    [ -d "$fail_matrix_output/inputs/second" ]
    [ ! -e "$fail_matrix_output/cross-input-summary.txt" ]
    [ -z "$(ls "$fail_matrix_output"/.cross-input-summary.txt.* 2>/dev/null || true)" ]

    publish_hook="$test_root/publish-hook"
    cat >"$publish_hook" <<'EOF'
#!/bin/sh
case ${PUBLISH_HOOK_MODE:-}:$1 in
  fail-inputs:after-inputs|fail-manifest:after-manifest) exit 55;;
  pause-inputs:after-inputs|pause-before-commit:before-commit)
    : >"$PUBLISH_HOOK_READY"
    kill -STOP "$PPID"
    ;;
esac
EOF
    chmod +x "$publish_hook"
    for publish_failure in inputs manifest; do
        failure_root="$test_root/publish-fail-$publish_failure"
        set +e
        PATH="$test_root/bin:$PATH" PUBLISH_HOOK_MODE="fail-$publish_failure" \
            APRILTAG_PROFILE_PUBLISH_HOOK="$publish_hook" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
            APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
            APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
            APRILTAG_PROFILE_OUTPUT="$failure_root" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
                "$TEST_SHELL" "$0" fast >/dev/null 2>&1
        rc=$?
        set -e
        [ "$rc" -eq 55 ]
        failure_output=$(ls -d "$failure_root"/run-* 2>/dev/null)
        [ ! -e "$failure_output/inputs" ]
        [ ! -e "$failure_output/inputs.tsv" ]
        [ -z "$(ls -d "$failure_output"/.inputs* "$failure_output"/.input-* 2>/dev/null || true)" ]
    done

    for pause_point in inputs before-commit; do
        term_root="$test_root/publish-term-$pause_point"
        term_ready="$test_root/publish-term-$pause_point.ready"
        PATH="$test_root/bin:$PATH" PUBLISH_HOOK_MODE="pause-$pause_point" PUBLISH_HOOK_READY="$term_ready" \
            APRILTAG_PROFILE_PUBLISH_HOOK="$publish_hook" APRILTAG_PROFILE_INPUTS="$matrix_inputs" \
            APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
            APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
            APRILTAG_PROFILE_OUTPUT="$term_root" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
                "$TEST_SHELL" "$0" fast >/dev/null 2>&1 &
        term_pid=$!
        term_wait=0
        while [ ! -e "$term_ready" ] && [ "$term_wait" -lt 50 ]; do
            sleep 1
            term_wait=$((term_wait + 1))
        done
        [ -e "$term_ready" ]
        kill -TERM "$term_pid"
        kill -CONT "$term_pid" 2>/dev/null || true
        set +e
        wait "$term_pid"
        rc=$?
        set -e
        [ "$rc" -eq 143 ]
        term_output=$(ls -d "$term_root"/run-* 2>/dev/null)
        [ ! -e "$term_output/inputs" ]
        [ ! -e "$term_output/inputs.tsv" ]
        [ -z "$(ls -d "$term_output"/.inputs* "$term_output"/.input-* 2>/dev/null || true)" ]
    done

    ln -s "$test_root/matrix files/first image.jpg" "$test_root/matrix files/first-link.jpg"
    invalid_index=0
    assert_invalid_input()
    {
        description=$1
        value=$2
        readlink_command=${3:-readlink}
        invalid_index=$((invalid_index + 1))
        invalid_log="$test_root/invalid-input-$invalid_index.log"
        set +e
        PATH="$test_root/bin:$PATH" APRILTAG_PROFILE_INPUTS="$value" \
            APRILTAG_PROFILE_READLINK="$readlink_command" \
            APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
            APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
            APRILTAG_PROFILE_OUTPUT="$test_root/invalid-input-$invalid_index" \
            APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
                "$TEST_SHELL" "$0" fast >"$invalid_log" 2>&1
        rc=$?
        set -e
        [ "$rc" -ne 0 ]
        grep -F -q "APRILTAG_PROFILE_INPUTS" "$invalid_log"
        grep -F -q "$description" "$invalid_log"
        invalid_output=$(ls -d "$test_root/invalid-input-$invalid_index"/run-* 2>/dev/null)
        [ ! -e "$invalid_output/inputs.tsv" ]
        [ ! -e "$invalid_output/inputs" ]
        [ -z "$(ls -d "$invalid_output"/.inputs* "$invalid_output"/.input-paths* 2>/dev/null || true)" ]
    }
    assert_invalid_input 'empty matrix' ''
    assert_invalid_input 'empty matrix' "$(printf ' \t\n\t  ')"
    assert_invalid_input 'empty label' \
        "=$test_root/matrix files/first image.jpg,1x1"
    assert_invalid_input 'bad label' \
        "bad label=$test_root/matrix files/first image.jpg,1x1"
    assert_invalid_input "invalid label '.'" \
        ".=$test_root/matrix files/first image.jpg,1x1"
    assert_invalid_input "invalid label '..'" \
        "..=$test_root/matrix files/first image.jpg,1x1"
    assert_invalid_input 'bad label' \
        " leading=$test_root/matrix files/first image.jpg,1x1"
    assert_invalid_input 'duplicate label' \
        "same=$test_root/matrix files/first image.jpg,1x1
same=$test_root/matrix files/second.y8,1x1"
    assert_invalid_input 'missing path' 'missing=,1x1'
    assert_invalid_input 'missing size' \
        "missing=$test_root/matrix files/first image.jpg,"
    assert_invalid_input 'invalid WxH' \
        "badsize=$test_root/matrix files/first image.jpg,1X1"
    assert_invalid_input 'invalid WxH' \
        "zerosize=$test_root/matrix files/first image.jpg,00x1"
    assert_invalid_input 'invalid WxH' \
        "spacesize=$test_root/matrix files/first image.jpg,1x1 "
    assert_invalid_input 'path does not exist' \
        "absent=$test_root/matrix files/not-there.jpg,1x1"
    assert_invalid_input 'path does not exist' \
        "valid-first=$test_root/matrix files/first image.jpg,1x1
late-invalid=$test_root/matrix files/not-there.jpg,1x1"
    tab_path="$test_root/matrix files/tab$(printf '\t')name.jpg"
    touch "$tab_path"
    assert_invalid_input 'tab in path' "tab=$tab_path,1x1"
    backslash_path="$test_root/matrix files/back\\slash.jpg"
    touch "$backslash_path"
    assert_invalid_input 'backslash in path' "backslash=$backslash_path,1x1"
    assert_invalid_input 'duplicate canonical path' \
        "original=$test_root/matrix files/first image.jpg,1x1
alias=$test_root/matrix files/first-link.jpg,2x2"
    assert_invalid_input 'comma in path' \
        "comma=$test_root/matrix files/first,image.jpg,1x1"
    assert_invalid_input 'carriage return' \
        "cr=$test_root/matrix files/first image.jpg,1x1$(printf '\r')"
    unsafe_target="$test_root/matrix files/canonical$(printf '\t')target.jpg"
    touch "$unsafe_target"
    ln -s "$unsafe_target" "$test_root/matrix files/unsafe-link.jpg"
    assert_invalid_input 'canonical path contains tab' \
        "unsafe=$test_root/matrix files/unsafe-link.jpg,1x1"
    ln -s "$backslash_path" "$test_root/matrix files/backslash-link.jpg"
    assert_invalid_input 'canonical path contains backslash' \
        "unsafe-backslash=$test_root/matrix files/backslash-link.jpg,1x1"
    newline_char='
'
    newline_target="$test_root/matrix files/canonical-newline$newline_char"
    touch "$newline_target"
    ln -s "$newline_target" "$test_root/matrix files/newline-link.jpg"
    assert_invalid_input 'canonical path contains newline' \
        "unsafe-newline=$test_root/matrix files/newline-link.jpg,1x1"
    fake_readlink="$test_root/fake-readlink"
    cat >"$fake_readlink" <<'EOF'
#!/bin/sh
printf '/unsafe/canonical\npath.jpg\n'
EOF
    chmod +x "$fake_readlink"
    assert_invalid_input 'canonical path contains newline' \
        "unsafe-newline=$test_root/matrix files/first image.jpg,1x1" "$fake_readlink"
    assert_invalid_input 'readlink -f command is unavailable' \
        "readlink=$test_root/matrix files/first image.jpg,1x1" \
        missing-readlink
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
        [ ! -e "$output/inputs" ]
        [ ! -e "$output/inputs.tsv" ]
        for existing_artifact in environment.txt workload.log workload-summary.txt \
            comparison.log rust-rvv.stat rust-rvv.data rust-rvv.report \
            rust-scalar.stat rust-scalar.data rust-scalar.report c.stat c.data c.report summary.txt; do
            [ -e "$output/$existing_artifact" ]
        done
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
        grep -q 'Input hash: 111' "$output/summary.txt"
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
    ablation_root="$test_root/ablations"
    : >"$test_root/command-trace"
    PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
    FAKE_COMMAND_TRACE="$test_root/command-trace" APRILTAG_PROFILE_ABLATIONS=1 \
    APRILTAG_PROFILE_BENCH="$fake_bench" APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" \
    APRILTAG_PROFILE_WORKLOAD="$fake_workload" APRILTAG_PROFILE_PERF="$fake_perf" \
    APRILTAG_PROFILE_FIXTURE="$test_root/fixture" APRILTAG_PROFILE_OUTPUT="$ablation_root" \
    APRILTAG_PROFILE_EVENTS='task-clock,cycles,instructions,branches' \
    APRILTAG_PROFILE_WARMUP=3 APRILTAG_PROFILE_ITERATIONS=1 APRILTAG_PROFILE_BATCHES=2 \
    APRILTAG_ABLATION_PERF_WARMUP=1 APRILTAG_ABLATION_PERF_ITERATIONS=10 \
    APRILTAG_ABLATION_PERF_BATCHES=5 \
        "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    ablation_output=$(ls -d "$ablation_root"/run-* 2>/dev/null)
    expected_matrix='scalar:none
only-decimate:decimate
only-threshold:threshold
only-rle:rle
only-lfps-tuned:lfps-tuned
only-gaussian:gaussian
only-gray-model:gray-model
all:all
without-decimate:threshold,rle,lfps-tuned,gaussian,gray-model
without-threshold:decimate,rle,lfps-tuned,gaussian,gray-model
without-rle:decimate,threshold,lfps-tuned,gaussian,gray-model
without-lfps-tuned:decimate,threshold,rle,gaussian,gray-model
without-gaussian:decimate,threshold,rle,lfps-tuned,gray-model
without-gray-model:decimate,threshold,rle,lfps-tuned,gaussian'
    old_ifs=$IFS
    IFS='
'
    matrix_count=0
    for entry in $expected_matrix; do
        label=${entry%%:*}; mask=${entry#*:}; matrix_count=$((matrix_count + 1))
        [ -d "$ablation_output/ablations/$label" ]
        grep -q "bench backend=rust-rvv stages=$mask warmup=5 iterations=20 batches=5" "$test_root/command-trace"
        grep -q "workload backend=rust-rvv stages=$mask warmup=0 iterations=1 batches=1" "$test_root/command-trace"
        grep -q "profile stages=$mask warmup=5 iterations=20 batches=5" "$test_root/command-trace"
        grep -q '^RESULT backend=rust-rvv ' "$ablation_output/ablations/$label/benchmark.log"
        grep -q '^WORKLOAD backend=rust-rvv ' "$ablation_output/ablations/$label/workload.log"
        grep -q '^STAGE backend=rust-rvv ' "$ablation_output/ablations/$label/profile.log"
        grep -q '^CCL_WORK backend=rust-rvv ' "$ablation_output/ablations/$label/profile.log"
        grep -q "^WORKLOAD backend=rust-rvv .* stages=$mask " "$ablation_output/ablations/$label/workload.log"
        [ -s "$ablation_output/ablations/$label/perf.stat" ]
    done
    IFS=$old_ifs
    [ "$matrix_count" -eq 14 ]
    grep -q '^scalar .*isolated_gain=n/a disabled_regression=n/a equivalent_to_scalar=n/a equivalent_to_all=n/a' "$ablation_output/ablations/summary.txt"
    grep -q '^all .*isolated_gain=n/a disabled_regression=n/a equivalent_to_scalar=n/a equivalent_to_all=n/a' "$ablation_output/ablations/summary.txt"
    grep -q 'only-decimate.*isolated_gain=10.00% disabled_regression=n/a equivalent_to_scalar=1 equivalent_to_all=n/a' "$ablation_output/ablations/summary.txt"
    grep -q 'only-threshold.*isolated_gain=n/a disabled_regression=n/a equivalent_to_scalar=0 equivalent_to_all=n/a' "$ablation_output/ablations/summary.txt"
    grep -q 'without-rle.*isolated_gain=n/a disabled_regression=n/a equivalent_to_scalar=n/a equivalent_to_all=0' "$ablation_output/ablations/summary.txt"
    grep -q 'without-gray-model.*isolated_gain=n/a disabled_regression=10.00% equivalent_to_scalar=n/a equivalent_to_all=1' "$ablation_output/ablations/summary.txt"
    grep -q 'only-decimate.*production_mean_ms=18.000 instrumented_mean_ms=20.000 instrumentation_overhead_ms=2.000 instrumentation_overhead_pct=11.111' "$ablation_output/ablations/summary.txt"
    grep -q 'only-gaussian.*timer_mean_unattributed_ratio_pct=5.000 timer_max_unattributed_ratio_pct=12.000 timer_warning=1' "$ablation_output/ablations/summary.txt"
    grep -q 'WARNING: only-gaussian CCL timer unattributed ratio exceeds 10%' "$ablation_output/ablations/summary.txt"
    grep -q 'WARNING: only-threshold production output differs from scalar; isolated gain is non-equivalent-output.' "$ablation_output/ablations/summary.txt"
    grep -q 'WARNING: without-rle workload output differs from all; disabled regression is non-equivalent-output.' "$ablation_output/ablations/summary.txt"
    ! grep -q 'WARNING: without-gray-model.*scalar' "$ablation_output/ablations/summary.txt"
    grep -q 'cycles_per_call=19.231' "$ablation_output/ablations/summary.txt"
    grep -q 'instructions_per_call=38.462' "$ablation_output/ablations/summary.txt"
    grep -q 'branches_per_call=n/a' "$ablation_output/ablations/summary.txt"
    grep -q 'normalization=50_measured+1_warmup+1_validation' "$ablation_output/ablations/summary.txt"
    [ "$(grep -c '^workload backend=rust-rvv stages=' "$test_root/command-trace")" -eq 14 ]
    [ "$(grep -c '^bench backend=rust-rvv stages=.* warmup=5 iterations=20 batches=5$' "$test_root/command-trace")" -eq 14 ]
    [ "$(grep -c '^bench backend=rust-rvv stages=.* warmup=1 iterations=10 batches=5$' "$test_root/command-trace")" -eq 14 ]
    grep -q 'ablation_detector_calls=3724' "$ablation_output/environment.txt"
    set +e
    FAIL_PROFILE_STAGE=rle PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
    APRILTAG_PROFILE_ABLATIONS=1 APRILTAG_PROFILE_BENCH="$fake_bench" \
    APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
    APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
    APRILTAG_PROFILE_OUTPUT="$test_root/ablation-fail" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
        "$TEST_SHELL" "$0" fast >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 37 ]
    set +e
    ZERO_PRODUCTION_MEAN=1 PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
    APRILTAG_PROFILE_ABLATIONS=1 APRILTAG_PROFILE_BENCH="$fake_bench" \
    APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
    APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
    APRILTAG_PROFILE_OUTPUT="$test_root/zero-production-mean-fail" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
        "$TEST_SHELL" "$0" fast >"$test_root/zero-production-mean.log" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 1 ]
    grep -q 'error: only-decimate production_mean_ms must be greater than zero; got 0.000' \
        "$test_root/zero-production-mean.log"
    for mismatch in profile-mask perf-mask workload-mask; do
        set +e
        FAIL_METADATA=$mismatch PATH="$test_root/bin:$PATH" FAKE_PERF_TRACE="$test_root/perf-trace" \
        APRILTAG_PROFILE_ABLATIONS=1 APRILTAG_PROFILE_BENCH="$fake_bench" \
        APRILTAG_PROFILE_STAGE_BENCH="$fake_profile" APRILTAG_PROFILE_WORKLOAD="$fake_workload" \
        APRILTAG_PROFILE_PERF="$fake_perf" APRILTAG_PROFILE_FIXTURE="$test_root/fixture" \
        APRILTAG_PROFILE_OUTPUT="$test_root/$mismatch-metadata-fail" APRILTAG_PROFILE_EVENTS='task-clock,cycles' \
            "$TEST_SHELL" "$0" fast >/dev/null 2>&1
        rc=$?
        set -e
        [ "$rc" -eq 1 ]
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
valid_uint "$ABLATION_WARMUP" APRILTAG_ABLATION_WARMUP 1
valid_uint "$ABLATION_ITERATIONS" APRILTAG_ABLATION_ITERATIONS 0
valid_uint "$ABLATION_BATCHES" APRILTAG_ABLATION_BATCHES 0
valid_uint "$ABLATION_PERF_WARMUP" APRILTAG_ABLATION_PERF_WARMUP 1
valid_uint "$ABLATION_PERF_ITERATIONS" APRILTAG_ABLATION_PERF_ITERATIONS 0
valid_uint "$ABLATION_PERF_BATCHES" APRILTAG_ABLATION_PERF_BATCHES 0
case "$ABLATIONS" in 0|1) ;; *) echo "APRILTAG_PROFILE_ABLATIONS must be 0 or 1" >&2; exit 2;; esac
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
if [ "${APRILTAG_PROFILE_INPUTS+x}" = x ]; then
    validate_multi_input_args "$@"
    command -v sha256sum >/dev/null 2>&1 || { echo "required command not found: sha256sum" >&2; exit 1; }
    parse_profile_inputs
fi
[ "$ABLATIONS" -eq 0 ] && [ "${APRILTAG_PROFILE_INPUTS+x}" != x ] || [ -x "$STAGE_BENCH" ] || { echo "profile benchmark executable not found: $STAGE_BENCH" >&2; exit 1; }
ENVIRONMENT="$RESULT_DIR/environment.txt"
echo "Writing profile to $RESULT_DIR"
if [ "${APRILTAG_PROFILE_INPUTS+x}" = x ]; then
    MULTI_INPUT=1
    MULTI_PROBED=0
    MATRIX_RESULT_DIR=$RESULT_DIR
    CROSS_SUMMARY_TMP="$MATRIX_RESULT_DIR/.cross-input-summary.txt.$$"
    : >"$CROSS_SUMMARY_TMP"
    tab=$(printf '\t')
    while IFS="$tab" read -r input_label input_path input_width input_height input_file_hash; do
        if [ "$input_width" = native ]; then
            input_size=native
            input_file_hash=$input_height
        else
            input_size=${input_width}x${input_height}
        fi
        run_input_profile "$input_label" "$input_path" "$input_size" "$input_file_hash" \
            "$MATRIX_RESULT_DIR/inputs/$input_label" "$@"
    done <"$MATRIX_RESULT_DIR/inputs.tsv"
    mv "$CROSS_SUMMARY_TMP" "$MATRIX_RESULT_DIR/cross-input-summary.txt"
    CROSS_SUMMARY_TMP=
else
    MULTI_INPUT=0
    run_profile_workflow "$@"
fi
