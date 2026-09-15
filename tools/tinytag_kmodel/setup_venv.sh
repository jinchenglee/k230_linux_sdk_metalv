#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
venv=${TINYTAG_NNCASE_VENV:-"$script_dir/.venv"}
python=${PYTHON:-python3}

"$python" -m venv "$venv"
"$venv/bin/pip" install --upgrade pip
"$venv/bin/pip" install -r "$script_dir/requirements.txt"

echo "TinyTag nncase environment: $venv"
echo "Use run_nncase.sh to compile or validate a model."

