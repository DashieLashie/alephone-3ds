#!/usr/bin/env bash
# Compare the source list the 3DS CMake build derives from
# Source_Files/**/Makefile.am (ports/3ds/cmake/AutomakeSources.cmake, with the
# conditionals in ports/3ds/cmake/A1AutomakeConditions.cmake) against an
# expected list.
#
# Usage:  scripts/3ds/check-sources.sh <expected-list> [<derived-list>]
#
# <expected-list> may be either
#   - an object list from an automake build, as recorded for stage 2
#     ("./CSeries/BStream.o" ..., paths relative to Source_Files; any lines
#     that are not *.o, such as trailing config.h defines, are ignored), or
#   - a source list (paths relative to Source_Files, *.c/*.cpp).
# <derived-list> defaults to a fresh run of the parser in CMake script mode
# (no toolchain or build directory needed); pass build-3ds/aleph-sources.txt
# to check what a configured build actually uses.
#
# Both lists are reduced to object stems (CSeries/BStream) before diffing.
# Exit status: 0 = identical, 1 = differences (printed as a diff), 2 = error.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

fail() { echo "ERROR: $*" >&2; exit 2; }

[[ $# -ge 1 && $# -le 2 ]] || fail "usage: $0 <expected-list> [<derived-list>]"
EXPECTED="$1"
[[ -f "$EXPECTED" ]] || fail "$EXPECTED not found"
command -v cmake >/dev/null || fail "host cmake not found"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/a1-check-sources.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

if [[ $# -eq 2 ]]; then
  DERIVED="$2"
  [[ -f "$DERIVED" ]] || fail "$DERIVED not found"
else
  DERIVED="$TMP/derived-sources.txt"
  cmake -DOUTPUT="$DERIVED" -P "$REPO_ROOT/ports/3ds/cmake/PrintSources.cmake" >&2 \
    || fail "parser failed (cmake -P ports/3ds/cmake/PrintSources.cmake)"
fi

# Normalise to sorted object stems: strip "./", keep only object or source
# lines, drop the extension.
stems() {
  sed -e 's#^\./##' "$1" \
    | grep -E '\.(o|c|cc|cpp|cxx)$' \
    | sed -E 's#\.(o|c|cc|cpp|cxx)$##' \
    | LC_ALL=C sort
}

stems "$EXPECTED" > "$TMP/expected"
stems "$DERIVED"  > "$TMP/derived"

echo "expected: $(wc -l < "$TMP/expected" | tr -d ' ') objects ($EXPECTED)"
echo "derived:  $(wc -l < "$TMP/derived" | tr -d ' ') sources ($DERIVED)"

if diff -u --label expected --label derived "$TMP/expected" "$TMP/derived"; then
  echo "RESULT: identical (0 differences)"
else
  echo "RESULT: lists differ ('-' only expected, '+' only derived)"
  exit 1
fi
