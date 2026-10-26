#!/bin/sh
# Host-side reproduction of the 3dslink mirror's stall path (see stall-repro.c).
# Builds into $TMPDIR and runs three buffer sizes with the in-game stats line size.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out="${TMPDIR:-/tmp}/a1-stall-repro"
cc -O2 -Wall -Wextra -o "$out" "$here/stall-repro.c"
for bufs in "0 0" "65536 65536" "8192 8192"; do
	# shellcheck disable=SC2086
	"$out" $bufs 170 || true
	echo
done
