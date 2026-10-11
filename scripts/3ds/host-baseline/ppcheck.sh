#!/usr/bin/env bash
# Non-3DS check for engine edits (docs/3ds/PATCHES.md): preprocess each TU
# with HEAD's and the working tree's versions of the changed files, in the
# aleph-host-build and aleph-host-build-nonet volumes, and compare.
#
#   scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"
#   e.g. scripts/3ds/host-baseline/ppcheck.sh Source_Files/Misc/alephversion.h \
#            "Source_Files/shell.cpp Source_Files/CSeries/csstrings.cpp"
#
# IDENTICAL is the goal. DIFFER can be acceptable if only __LINE__ values
# inside logging/assert macros moved; inspect with the files left in the
# container's /tmp (re-run by hand) before accepting it.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
changed=${1:?changed files}; tus=${2:?translation units}
tmp=$(mktemp -d -t ppcheck); trap 'rm -rf "$tmp"' EXIT
for f in $changed; do
	mkdir -p "$tmp/head/$(dirname "$f")" "$tmp/work/$(dirname "$f")"
	git -C "$repo" show "HEAD:$f" > "$tmp/head/$f"
	cp "$repo/$f" "$tmp/work/$f"
done
for vol in ${VOLUMES:-aleph-host-build aleph-host-build-nonet}; do
	echo "### $vol"
	docker run --rm -v "$vol":/build -v "$tmp/head":/head:ro -v "$tmp/work":/work:ro \
		-v "$repo/scripts/3ds/host-baseline/ppcheck-in-container.sh":/ppcheck.sh:ro \
		aleph-host-baseline bash /ppcheck.sh "$changed" "$tus"
done
