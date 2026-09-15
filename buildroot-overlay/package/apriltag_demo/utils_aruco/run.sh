#!/bin/sh
# Launch the selectable ArUco live detector with native-resolution input.
# Examples:
#   ./run.sh --backend nano --mode strict
#   ./run.sh --backend aruco2 --mode tolerant
# Additional arguments override the defaults because the parser uses the last
# occurrence of each option.
cd "$(dirname "$0")"
exec ./aruco_demo.elf --backend nano --mode strict --factor 1 "$@"
