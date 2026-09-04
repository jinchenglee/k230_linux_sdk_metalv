#!/bin/sh
# Launch tinytag_detect live on the CSI camera with the production operating
# point from README.md: heatmap_thres 0.35, max_proposals 20, roi_expand 1.5,
# profile_mode 0 (silent).
#
# The six positional arguments below are required and fixed; anything passed
# here is appended, so the named options can be added:
#
#   ./run.sh                     # production settings, display capped 30 fps
#   ./run.sh --no-display        # headless: production shape, no DRM/OSD
#   ./run.sh --display-fps 0     # uncapped display preview
#   ./run.sh --debug             # live-loop per-stage timing
#
# 0.35 halves the proposal count (and roughly halves post_process) versus the
# training repo's frozen 0.20, for only a slight recall drop -- see README.md
# "Operating point: heatmap_thres". Note /etc/init.d/S60apriltagkey on 01studio
# launches at 0.20 instead; that is deliberate, not a copy of this file.
cd "$(dirname "$0")"
exec ./tinytag_detect.elf tinytag-v11_k230-v4c.int8.kmodel None 0.35 20 1.5 0 "$@"
