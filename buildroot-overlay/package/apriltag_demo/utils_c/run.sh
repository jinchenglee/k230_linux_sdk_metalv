#!/bin/sh
# Launch the AprilTag 3 C reference detector with the comparison defaults.
#
# Additional arguments are appended after these defaults, so options that take
# a value can be overridden. For example:
#
#   ./run.sh                         # strict, single-threaded comparison
#   ./run.sh --upstream-defaults     # AprilTag 3 upstream detector defaults
#   ./run.sh --threads 2             # exercise C detector thread scaling
#   ./run.sh --no-display            # detector-only live-camera operation
#
cd "$(dirname "$0")"
exec ./apriltag_c_demo.elf \
    --factor 2 \
    --min-blob 25 \
    --threads 1 \
    --bits-corrected 0 \
    --decode-sharpening 0 \
    "$@"
