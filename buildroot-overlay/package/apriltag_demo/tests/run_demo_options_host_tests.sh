#!/bin/sh
set -eu

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/apriltag-demo-options-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

CXX=${CXX:-c++}
"$CXX" -std=c++17 -Wall -Wextra -Werror \
    -I"$package_dir/src" \
    -I"$package_dir/lib" \
    "$package_dir/src/demo_options_tests.cc" \
    "$package_dir/src/demo_options.cc" \
    -o "$build_dir/demo_options_tests"

"$build_dir/demo_options_tests"
