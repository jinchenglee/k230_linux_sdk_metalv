#!/bin/sh
# Launch apriltag_demo with the settings used for day-to-day on-board runs.
#
# Anything passed here is appended after the defaults, and the argument parser
# takes the last occurrence of a flag, so extra arguments override:
#
#   ./run.sh                     # RVV detector, decimation 2, display capped 30 fps
#   ./run.sh --no-display        # headless: the production shape, no DRM/OSD
#   ./run.sh --display-fps 0     # uncapped display: the pre-cap A/B baseline
#   ./run.sh --factor 1.5        # overrides the --factor 2 below
#   ./run.sh --debug             # live pipeline views + decode diagnostics
#
# The once-per-second "poll/display/camera/detect/osd/drop" line on stderr is
# printed regardless of --debug; it is the measurement surface for the display
# and capture work tracked in docs/notes/perf_todo.md.
cd "$(dirname "$0")"
exec ./apriltag_demo.elf --rvv --factor 2 "$@"
