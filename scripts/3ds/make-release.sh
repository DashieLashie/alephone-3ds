#!/bin/bash
# Build a release of the Aleph One 3DS port: the GPU build (A1_RENDER_C3D=ON, shipping flags, no profiler),
# packaged as a .3dsx zip and a .cia. See docs/3ds/RELEASE.md.
#
#   scripts/3ds/make-release.sh VERSION          # VERSION = MAJOR.MINOR.PATCH[-label], e.g. 0.1.0-beta1
#
# Needs devkitPro (DEVKITPRO, DEVKITARM) and, for the CIA, makerom and bannertool on PATH or in
# $TOOLS_DIR (default ~/Documents/3ds-tools-src/bin). Without them only the 3dsx zip is made.
# Output: build/release/VERSION/ (ignored by git through .git/info/exclude's build/).
set -euo pipefail

VERSION="${1:-}"
if [[ ! "$VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-[A-Za-z0-9.]+)?$ ]]; then
	echo "usage: $0 MAJOR.MINOR.PATCH[-label]   (e.g. 0.1.0-beta1)" >&2
	exit 2
fi
MAJOR="${BASH_REMATCH[1]}"; MINOR="${BASH_REMATCH[2]}"; MICRO="${BASH_REMATCH[3]}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-3ds-release"
OUT="$ROOT/build/release/$VERSION"
RES="$ROOT/ports/3ds/res"
UNIQUE_ID="0xA1E30"   # title ID 000400000A1E3000; keep it the same in every release so a CIA updates in place
TOOLS_DIR="${TOOLS_DIR:-$HOME/Documents/3ds-tools-src/bin}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
export PATH="$TOOLS_DIR:$PATH"

if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]]; then
	echo "warning: the checkout has uncommitted changes; the release will not match a commit" >&2
fi
COMMIT="$(git -C "$ROOT" rev-parse --short=9 HEAD)"

echo "== configure + build ($BUILD, A1_RENDER_C3D=ON, profiler OFF)"
cmake -S "$ROOT/ports/3ds" -B "$BUILD" -DA1_RENDER_C3D=ON -DA1_PROFILE=OFF > /dev/null
cmake --build "$BUILD" -j8 2>&1 | tee "$BUILD/build.log" | grep -iE "warning|error" || true
if grep -qiE "warning" "$BUILD/build.log"; then
	echo "error: the build printed warnings (the port keeps 0)" >&2
	exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/zip/3ds/alephone"
cp "$BUILD/alephone.3dsx" "$OUT/zip/3ds/alephone/alephone.3dsx"
cp "$ROOT/COPYING" "$OUT/zip/COPYING.txt"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@COMMIT@/$COMMIT/g" "$ROOT/docs/3ds/release-readme.txt" > "$OUT/zip/README-3DS.txt"
(cd "$OUT/zip" && zip -qr "../alephone-3ds-$VERSION-3dsx.zip" .)
echo "== 3dsx: $OUT/alephone-3ds-$VERSION-3dsx.zip"

if command -v makerom > /dev/null && command -v bannertool > /dev/null; then
	bannertool makebanner -i "$RES/banner.png" -a "$RES/banner.wav" -o "$OUT/banner.bnr" > /dev/null
	makerom -f cia -o "$OUT/alephone-3ds-$VERSION.cia" -target t -exefslogo \
		-elf "$BUILD/alephone.elf" -rsf "$RES/alephone.rsf" \
		-icon "$BUILD/alephone.smdh" -banner "$OUT/banner.bnr" \
		-DAPP_ROMFS="$BUILD/romfs" -DAPP_UNIQUE_ID="$UNIQUE_ID" \
		-major "$MAJOR" -minor "$MINOR" -micro "$MICRO"
	rm -f "$OUT/banner.bnr"
	echo "== cia:  $OUT/alephone-3ds-$VERSION.cia"
else
	echo "== cia skipped: makerom and/or bannertool not found (docs/3ds/RELEASE.md, 'Tools')" >&2
fi

(cd "$OUT" && shasum -a 256 ./*.zip ./*.cia 2> /dev/null > SHA256SUMS.txt; cat SHA256SUMS.txt)
echo "== commit $COMMIT, version $VERSION"
