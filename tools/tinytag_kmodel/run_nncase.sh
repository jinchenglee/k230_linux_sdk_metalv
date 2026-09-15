#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
venv=${TINYTAG_NNCASE_VENV:-"$script_dir/.venv"}

if [ ! -x "$venv/bin/python" ]; then
    echo "missing $venv; run $script_dir/setup_venv.sh first" >&2
    exit 1
fi

site_packages=$(
    "$venv/bin/python" -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])'
)

if [ -n "${TINYTAG_DOTNET_ROOT:-}" ]; then
    DOTNET_ROOT=$TINYTAG_DOTNET_ROOT
    export DOTNET_ROOT
    PATH=$DOTNET_ROOT:$PATH
fi
if ! command -v dotnet >/dev/null 2>&1; then
    echo "dotnet not found; install the .NET 7 runtime or set TINYTAG_DOTNET_ROOT" >&2
    exit 1
fi
if ! dotnet --list-runtimes | grep -q "^Microsoft.NETCore.App 7\\."; then
    echo "Microsoft.NETCore.App 7.x is required by nncase 2.11" >&2
    exit 1
fi


NNCASE_PLUGIN_PATH=${NNCASE_PLUGIN_PATH:-"$site_packages/nncase/modules"}
export NNCASE_PLUGIN_PATH
PATH=$site_packages:$PATH
LD_LIBRARY_PATH=$site_packages${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export PATH LD_LIBRARY_PATH

exec "$venv/bin/python" "$@"

