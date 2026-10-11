#!/usr/bin/env bash
# Cross-build libsndfile for the 3DS as a static library WITHOUT external
# codecs and install it into $DEVKITPRO/portlibs/3ds.
#
# dkp-libs has no 3ds-libsndfile. libsndfile's Ogg Vorbis/Opus/FLAC support
# needs float libvorbis + libvorbisenc, libFLAC and libopus, and its MP3
# support needs libmpg123 + libmp3lame. The 3DS portlibs only have Tremor
# (libvorbisidec, integer decoder, not API compatible with libvorbisenc) and
# mpg123 (no lame), so this build disables every external codec. The result
# decodes the formats libsndfile implements internally: WAV/WAVEX, AIFF/AIFC,
# AU/SND, CAF, W64, RF64, IRCAM, VOC, NIST, PAF, SVX, MAT4/5, PVF, XI, HTK,
# SDS, AVR, SD2, WVE, MPC2K, raw (PCM, float, u/A-law, IMA/MS ADPCM, GSM 6.10,
# G721/G723, DWVW, NMS ADPCM, ALAC). It does NOT decode Ogg Vorbis, Opus,
# FLAC or MP3.
#
# After installing, the script verifies the result with
# scripts/3ds/tests/sndfile-vio-test.c (the libsndfile calls
# Sound/SndfileDecoder.cpp makes, over in-memory WAV and AIFF files):
#   1. compiles and LINKS it for the 3DS (-specs=3dsx.specs, -lctru, link line
#      from `arm-none-eabi-pkg-config --static --libs sndfile`) and packs a
#      .3dsx; this proves the archive is complete, it does not run it;
#   2. builds the same libsndfile tarball with the same options for the host
#      (in $WORK_DIR, never installed), links the test against it and RUNS it.
#
# Usage:  scripts/3ds/build-libsndfile.sh [FILE...]
#   FILEs are extra audio files the host test tries to open, e.g.
#   "data/Scenarios/Marathon 2/Music.ogg"; it reports whether each opens.
#
# Environment overrides:
#   WORK_DIR          download/build directory (default: <repo>/build/3ds-deps)
#   JOBS              parallel build jobs (default: number of CPUs)
#   SKIP_HOST_VERIFY  1 = skip step 2 (e.g. no host C compiler)
#
# Logs: $WORK_DIR/logs/libsndfile-{configure,build,install,verify}.log and
#       libsndfile-host-{configure,build}.log
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/3ds-deps}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
SKIP_HOST_VERIFY="${SKIP_HOST_VERIFY:-0}"
HOST_TEST_FILES=("$@")
TEST_SRC="$REPO_ROOT/scripts/3ds/tests/sndfile-vio-test.c"

fail() { echo "ERROR: $*" >&2; exit 1; }

# Pinned release. 1.2.2 (2023-08-13) is the newest libsndfile release.
#
# SHA-256 computed from the official GitHub release asset below, and
# cross-checked against Homebrew's libsndfile formula (formulae.brew.sh,
# urls.stable.checksum = the same value). Upstream publishes only a GPG
# signature (.tar.xz.asc), no checksum file; Arch Linux's PKGBUILD for
# libsndfile 1.2.2 pins sha512 a6e3ea3a...b5beee352a0 for the same tarball,
# which also matches.
SNDFILE_VERSION=1.2.2
SNDFILE_URL="https://github.com/libsndfile/libsndfile/releases/download/$SNDFILE_VERSION/libsndfile-$SNDFILE_VERSION.tar.xz"
SNDFILE_SHA256=3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set"
[[ -n "${DEVKITARM:-}" ]] || fail "DEVKITARM is not set"
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
[[ -f "$TOOLCHAIN" ]] || fail "$TOOLCHAIN not found (install 3ds-cmake via setup-env.sh)"
[[ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ]] || fail "arm-none-eabi-gcc not found in $DEVKITARM/bin"
command -v cmake >/dev/null || fail "host cmake not found"
command -v curl  >/dev/null || fail "curl not found"
command -v shasum >/dev/null || command -v sha256sum >/dev/null || fail "no shasum/sha256sum"
command -v tar   >/dev/null || fail "tar not found"
[[ -f "$TEST_SRC" ]] || fail "$TEST_SRC not found"
PKG_CONFIG="$DEVKITPRO/portlibs/3ds/bin/arm-none-eabi-pkg-config"
[[ -x "$PKG_CONFIG" ]] || fail "$PKG_CONFIG not found (install 3ds-pkg-config)"

PREFIX="$DEVKITPRO/portlibs/3ds"
[[ -d "$PREFIX" ]] || fail "$PREFIX not found (install 3ds-dev via setup-env.sh)"

SUDO=()
[[ -w "$PREFIX" ]] || SUDO=(sudo)

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

BASE="$WORK_DIR/libsndfile-$SNDFILE_VERSION"
DL_DIR="$BASE/download"        # tarball only, never executed from
SRC_PARENT="$BASE/src"          # fresh extraction
SRC="$SRC_PARENT/libsndfile-$SNDFILE_VERSION"
BLD="$BASE/build-3ds"
HOST_BLD="$BASE/build-host"     # host copy for running the test; not installed
TEST_DIR="$BASE/test"
LOG_DIR="$WORK_DIR/logs"
TARBALL="$DL_DIR/libsndfile-$SNDFILE_VERSION.tar.xz"

mkdir -p "$WORK_DIR" "$LOG_DIR"

echo "==> libsndfile $SNDFILE_VERSION: download"
rm -rf "$DL_DIR"
mkdir -p "$DL_DIR"
curl -fsSL --retry 3 -o "$TARBALL" "$SNDFILE_URL"
got="$(sha256 "$TARBALL")"
[[ "$got" == "$SNDFILE_SHA256" ]] \
  || fail "SHA-256 mismatch for $TARBALL: expected $SNDFILE_SHA256, got $got"
echo "    sha256 OK: $got"

echo "==> libsndfile $SNDFILE_VERSION: extract"
rm -rf "$SRC_PARENT" "$BLD" "$HOST_BLD" "$TEST_DIR"
mkdir -p "$SRC_PARENT"
tar -xJf "$TARBALL" -C "$SRC_PARENT"
[[ -f "$SRC/CMakeLists.txt" ]] || fail "unexpected tarball layout: $SRC/CMakeLists.txt missing"

# Option names verified against libsndfile 1.2.2's CMakeLists.txt.
# BUILD_TESTING comes from include(CTest). ENABLE_EXTERNAL_LIBS/ENABLE_MPEG
# OFF alone are enough to keep codecs out of the library, but
# SndFileChecks.cmake still calls find_package() for Ogg/Vorbis/FLAC/Opus/
# mpg123/mp3lame/Speex/SQLite3/ALSA unconditionally, and the 3DS portlibs do
# contain libogg and libmpg123; CMAKE_DISABLE_FIND_PACKAGE_* makes the
# configuration independent of whatever else is in the prefix.
# CPU_CLIPS_* are set by ClipMode.cmake to FALSE when cross-compiling anyway
# (it cannot run a test program); set explicitly for clarity.
# CMAKE_POLICY_VERSION_MINIMUM: 1.2.2 starts with
# `cmake_minimum_required (VERSION 3.1..3.18)` (two dots, not the `...`
# range syntax), which CMake >= 4.0 rejects with "Compatibility with CMake
# < 3.5 has been removed from CMake".
#
# CFLAGS=-Wno-error=incompatible-pointer-types: src/alac.c passes its `int *`
# sample buffer to alac_decode()/alac_encode(), which take `int32_t *`. On
# arm-none-eabi newlib int32_t is `long int` (32 bits, same size and
# representation as int), and GCC >= 14 makes this mismatch a hard error:
#   alac.c:430:45: error: passing argument 3 of 'alac_decode' from
#   incompatible pointer type [-Wincompatible-pointer-types]
# The diagnostic is downgraded to a warning instead of patching upstream; the
# buffer is only ever accessed as int in alac.c and as int32_t in
# ALAC/alac_{de,en}coder.c (separate translation units, no LTO).
# CFLAGS from the environment is prepended to the toolchain's
# CMAKE_C_FLAGS_INIT (checked below), so the 3DS arch flags are kept.
SNDFILE_OPTS=(
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DENABLE_EXTERNAL_LIBS=OFF
  -DENABLE_MPEG=OFF
  -DENABLE_EXPERIMENTAL=OFF
  -DBUILD_PROGRAMS=OFF
  -DBUILD_EXAMPLES=OFF
  -DBUILD_TESTING=OFF
  -DBUILD_REGTEST=OFF
  -DENABLE_CPACK=OFF
  -DENABLE_PACKAGE_CONFIG=ON
  -DINSTALL_PKGCONFIG_MODULE=ON
  -DINSTALL_MANPAGES=OFF
  -DENABLE_CPU_CLIP=OFF
  -DCPU_CLIPS_POSITIVE=FALSE -DCPU_CLIPS_NEGATIVE=FALSE
  -DCMAKE_DISABLE_FIND_PACKAGE_Ogg=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_Vorbis=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_FLAC=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_Opus=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_mpg123=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_mp3lame=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_Speex=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_ALSA=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_Sndio=ON
)

# check_no_codecs <build dir>: refuse a build that picked up a codec.
check_no_codecs() {
  local b="$1" v
  for v in HAVE_EXTERNAL_XIPH_LIBS HAVE_MPEG ENABLE_EXTERNAL_LIBS ENABLE_MPEG; do
    if grep -Eq "^$v:[A-Z]+=(ON|1|TRUE)$" "$b/CMakeCache.txt"; then
      fail "$v is enabled in $b/CMakeCache.txt; expected a no-codec build"
    fi
  done
  grep -Eq '^#define HAVE_EXTERNAL_XIPH_LIBS 0' "$b/src/config.h" \
    || fail "$b/src/config.h does not have HAVE_EXTERNAL_XIPH_LIBS 0"
  grep -Eq '^#define HAVE_MPEG 0' "$b/src/config.h" \
    || fail "$b/src/config.h does not have HAVE_MPEG 0"
}

echo "==> libsndfile $SNDFILE_VERSION: configure (log: $LOG_DIR/libsndfile-configure.log)"
if ! CFLAGS="${CFLAGS:-} -Wno-error=incompatible-pointer-types" cmake -S "$SRC" -B "$BLD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    "${SNDFILE_OPTS[@]}" \
    >"$LOG_DIR/libsndfile-configure.log" 2>&1; then
  tail -n 40 "$LOG_DIR/libsndfile-configure.log" >&2
  fail "configure failed; see $LOG_DIR/libsndfile-configure.log"
fi

C_FLAGS_LINE="$(grep -E '^CMAKE_C_FLAGS:STRING=' "$BLD/CMakeCache.txt")"
for f in -march=armv6k -mfloat-abi=hard -mtp=soft -D__3DS__ -Wno-error=incompatible-pointer-types; do
  [[ "$C_FLAGS_LINE" == *" $f"* || "$C_FLAGS_LINE" == *"=$f"* ]] \
    || fail "CMAKE_C_FLAGS lacks $f: $C_FLAGS_LINE"
done

check_no_codecs "$BLD"

echo "==> libsndfile $SNDFILE_VERSION: build (log: $LOG_DIR/libsndfile-build.log)"
if ! cmake --build "$BLD" --parallel "$JOBS" >"$LOG_DIR/libsndfile-build.log" 2>&1; then
  tail -n 40 "$LOG_DIR/libsndfile-build.log" >&2
  fail "build failed; see $LOG_DIR/libsndfile-build.log"
fi

echo "==> libsndfile $SNDFILE_VERSION: install into $PREFIX (log: $LOG_DIR/libsndfile-install.log)"
if ! ${SUDO[@]+"${SUDO[@]}"} cmake --install "$BLD" >"$LOG_DIR/libsndfile-install.log" 2>&1; then
  tail -n 40 "$LOG_DIR/libsndfile-install.log" >&2
  fail "install failed; see $LOG_DIR/libsndfile-install.log"
fi

# Static only: nothing shared may have been installed.
if grep -Eq '\.(so|dylib|dll)(\.|$)' "$BLD/install_manifest.txt"; then
  fail "a shared library was installed; see $BLD/install_manifest.txt"
fi

echo "==> Installed libsndfile $SNDFILE_VERSION (no external codecs) into $PREFIX:"
sed 's/^/    /' "$BLD/install_manifest.txt"
echo

# ---------------------------------------------------------------------------
# Verification 1: compile + link the test for the 3DS against the installed
# library, using the pkg-config static link line.
VLOG="$LOG_DIR/libsndfile-verify.log"
: >"$VLOG"
echo "==> verify: 3DS compile + link (log: $VLOG)"
mkdir -p "$TEST_DIR"
SF_CFLAGS="$("$PKG_CONFIG" --cflags sndfile)"
SF_LIBS="$("$PKG_CONFIG" --static --libs sndfile)"
CC3DS="$DEVKITARM/bin/arm-none-eabi-gcc"
CTR_ROOT="$DEVKITPRO/libctru"
# shellcheck disable=SC2086  # pkg-config output is word-split on purpose
LINK_3DS=("$CC3DS" -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
  -mword-relocations -ffunction-sections -D__3DS__ -O2 -std=gnu99 -Wall -Wextra
  -I"$CTR_ROOT/include" $SF_CFLAGS
  "$TEST_SRC" -o "$TEST_DIR/sndfile-vio-test.elf"
  -specs=3dsx.specs -L"$CTR_ROOT/lib" $SF_LIBS -lctru -lm
  -Wl,-Map,"$TEST_DIR/sndfile-vio-test.map")
{
  echo "pkg-config --modversion sndfile: $("$PKG_CONFIG" --modversion sndfile)"
  echo "pkg-config --cflags sndfile: $SF_CFLAGS"
  echo "pkg-config --static --libs sndfile: $SF_LIBS"
  echo "+ ${LINK_3DS[*]}"
} >>"$VLOG"
"${LINK_3DS[@]}" >>"$VLOG" 2>&1 || { tail -n 40 "$VLOG" >&2; fail "3DS test link failed; see $VLOG"; }
"$DEVKITPRO/tools/bin/3dsxtool" "$TEST_DIR/sndfile-vio-test.elf" "$TEST_DIR/sndfile-vio-test.3dsx" >>"$VLOG" 2>&1 \
  || fail "3dsxtool failed; see $VLOG"
# The engine's entry points must have been pulled from libsndfile.a.
NM="$DEVKITARM/bin/arm-none-eabi-nm"
for sym in sf_open_virtual sf_read_float sf_seek sf_close sf_strerror sf_command; do
  "$NM" "$TEST_DIR/sndfile-vio-test.elf" | grep -Eq " T $sym$" \
    || fail "$sym not defined in $TEST_DIR/sndfile-vio-test.elf"
done
grep -q 'libsndfile.a(sndfile.o)' "$TEST_DIR/sndfile-vio-test.map" \
  || fail "link map does not show libsndfile.a(sndfile.o)"
{
  echo "--- file / size"
  file "$TEST_DIR/sndfile-vio-test.elf" "$TEST_DIR/sndfile-vio-test.3dsx" 2>/dev/null || true
  "$DEVKITARM/bin/arm-none-eabi-size" "$TEST_DIR/sndfile-vio-test.elf"
} >>"$VLOG" 2>&1
echo "    OK: $TEST_DIR/sndfile-vio-test.3dsx (linked, not run)"

# ---------------------------------------------------------------------------
# Verification 2: host build of the same tarball and options, run the test.
if [[ "$SKIP_HOST_VERIFY" == 1 ]]; then
  echo "==> verify: host run skipped (SKIP_HOST_VERIFY=1)"
  exit 0
fi
HOST_CC="${HOST_CC:-cc}"
command -v "$HOST_CC" >/dev/null || fail "host C compiler '$HOST_CC' not found (set SKIP_HOST_VERIFY=1 to skip)"

echo "==> verify: host build of libsndfile $SNDFILE_VERSION, same options (logs: $LOG_DIR/libsndfile-host-*.log)"
if ! env -u CFLAGS CC="$HOST_CC" cmake -S "$SRC" -B "$HOST_BLD" "${SNDFILE_OPTS[@]}" \
    >"$LOG_DIR/libsndfile-host-configure.log" 2>&1; then
  tail -n 40 "$LOG_DIR/libsndfile-host-configure.log" >&2
  fail "host configure failed; see $LOG_DIR/libsndfile-host-configure.log"
fi
check_no_codecs "$HOST_BLD"
if ! cmake --build "$HOST_BLD" --parallel "$JOBS" >"$LOG_DIR/libsndfile-host-build.log" 2>&1; then
  tail -n 40 "$LOG_DIR/libsndfile-host-build.log" >&2
  fail "host build failed; see $LOG_DIR/libsndfile-host-build.log"
fi

echo "==> verify: host run (log: $VLOG)"
HOST_INC=(-I"$SRC/include")
[[ -f "$HOST_BLD/include/sndfile.h" ]] && HOST_INC=(-I"$HOST_BLD/include" "${HOST_INC[@]}")
"$HOST_CC" -O2 -std=gnu99 -Wall -Wextra "${HOST_INC[@]}" "$TEST_SRC" \
  "$HOST_BLD/libsndfile.a" -lm -o "$TEST_DIR/sndfile-vio-test-host" >>"$VLOG" 2>&1 \
  || { tail -n 40 "$VLOG" >&2; fail "host test build failed; see $VLOG"; }
set +e
"$TEST_DIR/sndfile-vio-test-host" ${HOST_TEST_FILES[@]+"${HOST_TEST_FILES[@]}"} 2>&1 | tee -a "$VLOG"
rc=${PIPESTATUS[0]}
set -e
[[ $rc -eq 0 ]] || fail "host test failed (exit $rc); see $VLOG"
echo "==> libsndfile $SNDFILE_VERSION: installed and verified"
