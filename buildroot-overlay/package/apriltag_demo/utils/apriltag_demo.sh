#!/bin/sh
# Launch the AprilTag live demo. Pass through any flags:
#   ./apriltag_demo.sh --rvv --factor 2
cd "$(dirname "$0")"
./apriltag_demo.elf "$@"
