#!/bin/sh
# Launch the v40c candidate with the same 8-proposal live cap as stock run.sh.
# The stock model remains the default in run.sh and the boot-time service.
cd "$(dirname "$0")"
exec ./tinytag_detect.elf \
    tinytag-v40c-unfrozen-moderate30ep.int8.kmodel \
    None 0.35 8 1.5 0 "$@"
