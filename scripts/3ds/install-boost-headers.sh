#!/usr/bin/env bash
# Install the Boost *headers* (only the boost/ tree) for the 3DS build into
# $DEVKITPRO/portlibs/3ds/include/boost. Nothing is compiled and b2 is never
# run: the 3DS build uses A1_FS_STD (std::filesystem, see
# docs/3ds/ENVIRONMENT.md), and every other Boost library Aleph One uses is
# header-only (boost::system has been header-only since 1.69).
#
# Usage:  scripts/3ds/install-boost-headers.sh
#
# Environment overrides:
#   BOOST_VERSION  dotted version (default: 1.92.0). Changing it also needs
#   BOOST_SHA256   the matching SHA-256 of boost_<ver>.tar.bz2.
#   BOOST_URL      tarball URL (default: archives.boost.io release URL)
#   WORK_DIR       download/extract directory (default: <repo>/build/3ds-deps)
#   FORCE=1        replace an existing include/boost that this script did not
#                  install (no stamp file). Without it the script refuses.
#
# Re-running is idempotent: if the stamp file records the same version and
# hash, nothing is done; otherwise the boost/ dir this script installed is
# replaced atomically (new tree staged next to it, then swapped).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/3ds-deps}"
LOG_DIR="$WORK_DIR/logs"

fail() { echo "ERROR: $*" >&2; exit 1; }

# Pinned release. Boost 1.92.0 was published 2026-08-12 (archives.boost.io
# directory listing). SHA-256 provenance:
#   - computed locally from the downloaded official tarball, and
#   - equal to the "sha256" field of the upstream-published
#     https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2.json
#     (commit afdfa32505af73e3d208144b3f623f0096cb62b6), and
#   - equal to the hash listed on https://www.boost.org/releases/1.92.0/
# The script re-checks the .json at run time when it is reachable.
BOOST_VERSION="${BOOST_VERSION:-1.92.0}"
BOOST_SHA256="${BOOST_SHA256:-5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c}"
BOOST_UNDERSCORE="boost_${BOOST_VERSION//./_}"
BOOST_TARBALL="$BOOST_UNDERSCORE.tar.bz2"
BOOST_URL="${BOOST_URL:-https://archives.boost.io/release/$BOOST_VERSION/source/$BOOST_TARBALL}"

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set"
[[ -n "${DEVKITARM:-}" ]] || fail "DEVKITARM is not set"
[[ -f "$DEVKITPRO/cmake/3DS.cmake" ]] || fail "$DEVKITPRO/cmake/3DS.cmake not found (install 3ds-cmake via setup-env.sh)"
command -v curl >/dev/null || fail "curl not found"
command -v tar >/dev/null  || fail "tar not found"
command -v shasum >/dev/null || command -v sha256sum >/dev/null || fail "need shasum or sha256sum"

PREFIX="$DEVKITPRO/portlibs/3ds"
[[ -d "$PREFIX" ]] || fail "$PREFIX not found (install 3ds-dev via setup-env.sh)"
INCDIR="$PREFIX/include"
DEST="$INCDIR/boost"
STAMP_NAME=".aleph-boost-version"
STAMP="$DEST/$STAMP_NAME"

SUDO=()
mkdir -p "$INCDIR" 2>/dev/null || true
[[ -w "$INCDIR" ]] || SUDO=(sudo)

sha256_of() {
  if command -v shasum >/dev/null; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

STAMP_LINE="$BOOST_VERSION $BOOST_SHA256"
if [[ -f "$STAMP" ]] && [[ "$(cat "$STAMP")" == "$STAMP_LINE" ]]; then
  echo "==> Boost $BOOST_VERSION headers already installed in $DEST (stamp matches); nothing to do"
  exit 0
fi
if [[ -e "$DEST" && ! -f "$STAMP" && "${FORCE:-0}" != 1 ]]; then
  fail "$DEST exists but was not installed by this script (no $STAMP_NAME). Re-run with FORCE=1 to replace it."
fi

mkdir -p "$WORK_DIR" "$LOG_DIR"
LOG="$LOG_DIR/boost-headers-install.log"
: >"$LOG"
log() { echo "$*" | tee -a "$LOG"; }

# Fresh, dedicated download and extraction directories.
DL_DIR="$WORK_DIR/boost-$BOOST_VERSION-download"
SRC_DIR="$WORK_DIR/boost-$BOOST_VERSION-src"
rm -rf "$DL_DIR" "$SRC_DIR"
mkdir -p "$DL_DIR" "$SRC_DIR"

log "==> Downloading $BOOST_URL"
curl -fL --retry 3 -o "$DL_DIR/$BOOST_TARBALL" "$BOOST_URL" 2>>"$LOG" \
  || fail "download failed (see $LOG)"

ACTUAL="$(sha256_of "$DL_DIR/$BOOST_TARBALL")"
log "    sha256 actual:   $ACTUAL"
log "    sha256 expected: $BOOST_SHA256"
[[ "$ACTUAL" == "$BOOST_SHA256" ]] || fail "SHA-256 mismatch for $BOOST_TARBALL"

# Cross-check against the upstream-published JSON (best effort: a pinned
# hash match above is already sufficient; a *mismatch* here is fatal).
if UPSTREAM_JSON="$(curl -fsL --retry 2 "$BOOST_URL.json" 2>/dev/null)"; then
  UPSTREAM_SHA="$(printf '%s' "$UPSTREAM_JSON" | tr -d ' \n' | sed -n 's/.*"sha256":"\([0-9a-f]\{64\}\)".*/\1/p')"
  log "    sha256 upstream: ${UPSTREAM_SHA:-<not found in json>} ($BOOST_URL.json)"
  if [[ -n "$UPSTREAM_SHA" && "$UPSTREAM_SHA" != "$BOOST_SHA256" ]]; then
    fail "upstream $BOOST_TARBALL.json sha256 disagrees with pinned hash"
  fi
else
  log "    (upstream .json not reachable; relying on pinned hash)"
fi

log "==> Extracting $BOOST_UNDERSCORE/boost into $SRC_DIR"
tar -xjf "$DL_DIR/$BOOST_TARBALL" -C "$SRC_DIR" "$BOOST_UNDERSCORE/boost" "$BOOST_UNDERSCORE/LICENSE_1_0.txt" >>"$LOG" 2>&1 \
  || fail "extraction failed (see $LOG)"
HDRS="$SRC_DIR/$BOOST_UNDERSCORE/boost"
[[ -f "$HDRS/version.hpp" ]] || fail "$HDRS/version.hpp missing after extraction"
LIBVER="$(sed -n 's/^#define BOOST_LIB_VERSION "\(.*\)"/\1/p' "$HDRS/version.hpp")"
[[ "$LIBVER" == "$(echo "$BOOST_VERSION" | awk -F. '{print ($3 == 0) ? $1"_"$2 : $1"_"$2"_"$3}')" ]] \
  || fail "boost/version.hpp says BOOST_LIB_VERSION=$LIBVER, expected $BOOST_VERSION"

# Stage next to the destination, then swap, so a failure never leaves a
# half-populated include/boost behind.
STAGE="$INCDIR/.boost-staging.$$"
OLD="$INCDIR/.boost-old.$$"
${SUDO[@]+"${SUDO[@]}"} rm -rf "$STAGE" "$OLD"
log "==> Installing headers into $DEST"
${SUDO[@]+"${SUDO[@]}"} cp -R "$HDRS" "$STAGE"
${SUDO[@]+"${SUDO[@]}"} cp "$SRC_DIR/$BOOST_UNDERSCORE/LICENSE_1_0.txt" "$STAGE/LICENSE_1_0.txt"
echo "$STAMP_LINE" | ${SUDO[@]+"${SUDO[@]}"} tee "$STAGE/$STAMP_NAME" >/dev/null
if [[ -e "$DEST" ]]; then ${SUDO[@]+"${SUDO[@]}"} mv "$DEST" "$OLD"; fi
${SUDO[@]+"${SUDO[@]}"} mv "$STAGE" "$DEST"
${SUDO[@]+"${SUDO[@]}"} rm -rf "$OLD"

NFILES="$(find "$DEST" -type f | wc -l | tr -d ' ')"
log "==> Installed Boost $BOOST_VERSION headers ($NFILES files, BOOST_LIB_VERSION=$LIBVER) into $DEST"
log "    stamp: $STAMP"
