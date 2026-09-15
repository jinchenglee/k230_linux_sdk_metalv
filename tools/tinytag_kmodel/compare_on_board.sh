#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 VIDEO [HEATMAP_THRESHOLD [OUTPUT_DIR]]" >&2
    exit 2
fi

video=$1
threshold=${2:-0.35}
output_dir=${3:-/tmp/tinytag-model-compare}
app_dir=${TINYTAG_APP_DIR:-/root/app/tinytag_detect}
stock_model=${TINYTAG_STOCK_MODEL:-$app_dir/tinytag-v11_k230-v4c.int8.kmodel}
candidate_model=${TINYTAG_CANDIDATE_MODEL:-$app_dir/tinytag-v40c-unfrozen-moderate30ep.int8.kmodel}
binary=${TINYTAG_BINARY:-$app_dir/tinytag_detect.elf}

mkdir -p "$output_dir"

run_model()
{
    name=$1
    model=$2
    echo "[$name] model=$model threshold=$threshold video=$video"
    (
        cd "$output_dir"
        "$binary" "$model" "$video" "$threshold" 20 1.5 1 \
            >"$name.log" 2>&1
        mv tinytag_det.mp4 "$name.mp4"
    )
}

run_model stock "$stock_model"
run_model v40c "$candidate_model"

echo "wrote stock and v40c comparison output under $output_dir"

