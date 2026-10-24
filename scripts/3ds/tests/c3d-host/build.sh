#!/bin/sh
# Host tests for ports/3ds/render_c3d/core/ (design 8.4): plain C++, no 3DS headers.
#   scripts/3ds/tests/c3d-host/build.sh        builds into a temp dir and runs every test
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=$here/../../../../ports/3ds/render_c3d/core
out=$(mktemp -d "${TMPDIR:-/tmp}/c3d-host.XXXXXX")
trap 'rm -rf "$out"' EXIT
CXX=${CXX:-c++}
$CXX -std=c++17 -Wall -Wextra -Werror -I"$core" -o "$out/test_core" "$here/test_core.cpp" "$core/config.cpp"
"$out/test_core"
