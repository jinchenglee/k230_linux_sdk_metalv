#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODEL=${1:?usage: run_profile.sh MODEL.so [profiler arguments...]}
shift

exec "$APP_DIR/nn_profile" \
    --model "$MODEL" \
    --cpu "${NN_PROFILE_CPU:-0}" \
    --warmup "${NN_PROFILE_WARMUP:-10}" \
    --iterations "${NN_PROFILE_ITERATIONS:-100}" \
    "$@"
