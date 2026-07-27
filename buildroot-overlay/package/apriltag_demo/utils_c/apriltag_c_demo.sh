#!/bin/sh
# Launch the official AprilRobotics C detector with the same K230
# camera/display shell used by apriltag_demo.
cd "$(dirname "$0")"
exec ./apriltag_c_demo.elf "$@"
