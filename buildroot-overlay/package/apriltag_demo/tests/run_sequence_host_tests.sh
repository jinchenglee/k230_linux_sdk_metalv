#!/bin/sh
set -eu

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/apriltag-sequence-host-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

CXX=${CXX:-c++}
"$CXX" -std=c++17 -Wall -Wextra -Werror \
    -DAPRILTAG_BENCH_PROFILE \
    -DAPRILTAG_BENCH_NO_OPENCV \
    -DAPRILTAG_BENCH_CORE_ONLY \
    -DAPRILTAG_SEQUENCE_NO_MAIN \
    -I"$package_dir/bench" \
    -I"$package_dir/src" \
    -I"$package_dir/lib" \
    "$package_dir/bench/sequence_tests.cc" \
    "$package_dir/bench/sequence.cc" \
    "$package_dir/bench/sequence_main.cc" \
    "$package_dir/bench/profile_format.cc" \
    "$package_dir/bench/benchmark.cc" \
    -o "$build_dir/apriltag_sequence_tests"

"$build_dir/apriltag_sequence_tests"
printf '%s\n' 'apriltag_demo: host sequence tests passed'
