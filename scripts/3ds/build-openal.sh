#!/usr/bin/env bash
# Cross-build OpenAL Soft as a static library for the 3DS and install it into
# $DEVKITPRO/portlibs/3ds (AL/*.h, libopenal.a, openal.pc, lib/cmake/OpenAL).
#
# Aleph One uses OpenAL Soft only as a software mixer: it opens a loopback
# device (ALC_SOFT_loopback) and pulls samples with alcRenderSamplesSOFT from
# the SDL audio callback. No real output backend is needed, so every
# ALSOFT_BACKEND_* option is OFF. The null and loopback backends are not
# options in 1.25.2: CMakeLists.txt always compiles alc/backends/null.cpp and
# alc/backends/loopback.cpp ("Default backends, always available").
#
# Usage:  scripts/3ds/build-openal.sh
#
# Environment overrides:
#   OPENAL_VERSION   release to build (default: 1.25.2). Changing it also needs
#   OPENAL_SHA256    the matching SHA-256 of openal-soft-<ver>.tar.bz2.
#   OPENAL_URL       tarball URL (default: openal-soft.org release URL)
#   OPENAL_EMBED_HRTF ON|OFF  embed the default HRTF data set (default: ON,
#                    see the comment at ALSOFT_EMBED_HRTF_DATA below)
#   OPENAL_STL_HARDENING None|Performance|Debug  (default: Performance, the
#                    upstream default = -D_GLIBCXX_ASSERTIONS; None trades the
#                    bounds checks for some mixer speed)
#   WORK_DIR         download/build directory (default: <repo>/build/3ds-deps)
#   JOBS             parallel build jobs (default: number of CPUs)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/3ds-deps}"
LOG_DIR="$WORK_DIR/logs"

fail() { echo "ERROR: $*" >&2; exit 1; }

# Pinned release: OpenAL Soft 1.25.2, published 2026-05-12 (GitHub release
# kcat/openal-soft "1.25.2"; newest release as of 2026-10-10).
# SHA-256 provenance (upstream publishes no checksum or signature for the
# tarball):
#   - computed locally from https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2
#   - equal to the sha256 in Homebrew's formula (homebrew-core
#     Formula/o/openal-soft.rb, url = the same openal-soft.org tarball)
#   - equal to FreeBSD ports audio/openal-soft/distinfo
#     (SHA256 (openal-soft-1.25.2.tar.bz2), SIZE 1132689)
# 1.25.x requires C++20 (CXX_STANDARD 20, REQUIRED); devkitARM's GCC 16.1 is
# fine. Its optional C++20 *modules* need Ninja or VS 2022, so with the
# Unix Makefiles generator they are never used; ALSOFT_ENABLE_MODULES=OFF
# makes that explicit.
OPENAL_VERSION="${OPENAL_VERSION:-1.25.2}"
OPENAL_SHA256="${OPENAL_SHA256:-1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089}"
OPENAL_TARBALL="openal-soft-$OPENAL_VERSION.tar.bz2"
OPENAL_URL="${OPENAL_URL:-https://openal-soft.org/openal-releases/$OPENAL_TARBALL}"
OPENAL_EMBED_HRTF="${OPENAL_EMBED_HRTF:-ON}"
OPENAL_STL_HARDENING="${OPENAL_STL_HARDENING:-Performance}"

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set"
[[ -n "${DEVKITARM:-}" ]] || fail "DEVKITARM is not set"
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
[[ -f "$TOOLCHAIN" ]] || fail "$TOOLCHAIN not found (install 3ds-cmake via setup-env.sh)"
[[ -x "$DEVKITARM/bin/arm-none-eabi-g++" ]] || fail "$DEVKITARM/bin/arm-none-eabi-g++ not found"
command -v cmake >/dev/null || fail "host cmake not found"
command -v curl >/dev/null  || fail "curl not found"
command -v tar >/dev/null   || fail "tar not found"
command -v shasum >/dev/null || command -v sha256sum >/dev/null || fail "need shasum or sha256sum"

PREFIX="$DEVKITPRO/portlibs/3ds"
[[ -d "$PREFIX" ]] || fail "$PREFIX not found (install 3ds-dev via setup-env.sh)"

SUDO=()
[[ -w "$PREFIX" ]] || SUDO=(sudo)

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

sha256_of() {
  if command -v shasum >/dev/null; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

mkdir -p "$WORK_DIR" "$LOG_DIR"

# Fresh, dedicated download, source and build directories.
DL_DIR="$WORK_DIR/openal-$OPENAL_VERSION-download"
SRC_PARENT="$WORK_DIR/openal-$OPENAL_VERSION-src"
SRC="$SRC_PARENT/openal-soft-$OPENAL_VERSION"
BLD="$WORK_DIR/openal-$OPENAL_VERSION-build"
rm -rf "$DL_DIR" "$SRC_PARENT" "$BLD" "$BLD-nopic.cmake"
mkdir -p "$DL_DIR" "$SRC_PARENT"

echo "==> Downloading $OPENAL_URL"
curl -fL --retry 3 -o "$DL_DIR/$OPENAL_TARBALL" "$OPENAL_URL" || fail "download failed"
ACTUAL="$(sha256_of "$DL_DIR/$OPENAL_TARBALL")"
echo "    sha256 actual:   $ACTUAL"
echo "    sha256 expected: $OPENAL_SHA256"
[[ "$ACTUAL" == "$OPENAL_SHA256" ]] || fail "SHA-256 mismatch for $OPENAL_TARBALL"

echo "==> Extracting into $SRC_PARENT"
tar -xjf "$DL_DIR/$OPENAL_TARBALL" -C "$SRC_PARENT"
[[ -f "$SRC/CMakeLists.txt" ]] || fail "$SRC/CMakeLists.txt missing after extraction"
grep -q "VERSION $OPENAL_VERSION" "$SRC/CMakeLists.txt" \
  || fail "$SRC/CMakeLists.txt does not declare VERSION $OPENAL_VERSION"

# Local patches (each .patch file documents its problem and fix in its
# header). Only one is needed for 1.25.2: altypes.hpp has no strict-type
# mapping for `int` when int32_t is `long`, as on arm-none-eabi/newlib.
PATCH_DIR="$REPO_ROOT/scripts/3ds/patches/openal-soft-$OPENAL_VERSION"
if [[ -d "$PATCH_DIR" ]]; then
  command -v patch >/dev/null || fail "patch not found"
  for p in "$PATCH_DIR"/*.patch; do
    [[ -e "$p" ]] || continue
    echo "==> Applying $(basename "$p")"
    patch -p1 --forward --batch -d "$SRC" <"$p" || fail "patch $(basename "$p") did not apply"
  done
fi

# --- Configure ---------------------------------------------------------------
# Upstream forces POSITION_INDEPENDENT_CODE TRUE on its helper targets (the
# bundled fmt objects end up in libopenal.a compiled with -fPIC, adding a GOT
# to the executable). The 3DS executable is not position independent, so
# PIC only costs an extra indirection per global access, and GOT slots carry
# no --emit-relocs relocation for 3dsxtool to record. Blank the compiler's
# PIC option after every project() call (top level and fmt-11.2.0).
NOPIC_INCLUDE="$BLD-nopic.cmake"
cat >"$NOPIC_INCLUDE" <<'NOPIC'
# Generated by scripts/3ds/build-openal.sh (CMAKE_PROJECT_INCLUDE).
set(CMAKE_C_COMPILE_OPTIONS_PIC "")
set(CMAKE_CXX_COMPILE_OPTIONS_PIC "")
NOPIC

# Options are the complete set of ALSOFT_* options that 1.25.2 defines on a
# non-Windows, non-Apple target (ALSOFT_BACKEND_WASAPI/DSOUND/WINMM,
# ALSOFT_BUILD_ROUTER, ALSOFT_BUILD_IMPORT_LIB, ALSOFT_NO_UID_DEFS,
# ALSOFT_STATIC_WINPTHREAD and ALSOFT_OSX_FRAMEWORK exist only under
# if(WIN32)/if(APPLE); ALSOFT_ENABLE_SSE2_CODEGEN only when SSE2 is found).
OPTS=(
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
  -DCMAKE_BUILD_TYPE=Release            # dkp toolchain: -g -O2 -DNDEBUG
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_INSTALL_LIBDIR=lib
  -G "Unix Makefiles"
  -DCMAKE_PROJECT_INCLUDE="$NOPIC_INCLUDE"
  -DLIBTYPE=STATIC
  # Programs / installed extras
  -DALSOFT_UTILS=OFF
  -DALSOFT_NO_CONFIG_UTIL=ON
  -DALSOFT_EXAMPLES=OFF
  -DALSOFT_TESTS=OFF
  -DALSOFT_INSTALL=ON
  -DALSOFT_INSTALL_CONFIG=OFF
  -DALSOFT_INSTALL_HRTF_DATA=OFF
  -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF
  -DALSOFT_INSTALL_EXAMPLES=OFF
  -DALSOFT_INSTALL_UTILS=OFF
  -DALSOFT_SEARCH_INSTALL_DATADIR=OFF
  -DALSOFT_UPDATE_BUILD_VERSION=OFF
  # Language / toolchain features
  -DALSOFT_ENABLE_MODULES=OFF
  -DALSOFT_WERROR=OFF
  -DALSOFT_EAX=OFF                      # legacy EAX: Windows-only default, unused by the engine
  -DALSOFT_DLOPEN=OFF                   # no dynamic loader on the 3DS
  -DALSOFT_DLOPEN_NOTES=OFF
  -DALSOFT_STATIC_LIBGCC=OFF            # everything is static on the 3DS anyway
  -DALSOFT_STATIC_STDCXX=OFF
  -DALSOFT_RTKIT=OFF                    # D-Bus/RTKit: Linux desktop only
  -DALSOFT_REQUIRE_RTKIT=OFF
  -DALSOFT_STL_HARDENING="$OPENAL_STL_HARDENING"
  # ELF hardening link flags. Upstream probes them with check_linker_flag and,
  # because arm-none-eabi-ld accepts them, records them in openal.pc's
  # Libs.private and the exported CMake target. They are meaningless for a
  # static 3DS executable ("ld: warning: -z relro ignored") and --as-needed
  # would leak into the engine's link line, so pre-seed the check results.
  -DHAVE_WL_Z_NOEXECSTACK=OFF
  -DHAVE_WL_Z_RELRO=OFF
  -DHAVE_WL_Z_NOW=OFF
  -DHAVE_WL_AS_NEEDED=OFF
  # CPU extensions: the ARM11 MPCore (ARMv6K + VFPv2) has neither SSE nor NEON.
  -DALSOFT_CPUEXT_SSE=OFF    -DALSOFT_REQUIRE_SSE=OFF
  -DALSOFT_CPUEXT_SSE2=OFF   -DALSOFT_REQUIRE_SSE2=OFF
  -DALSOFT_CPUEXT_SSE3=OFF   -DALSOFT_REQUIRE_SSE3=OFF
  -DALSOFT_CPUEXT_SSE4_1=OFF -DALSOFT_REQUIRE_SSE4_1=OFF
  -DALSOFT_CPUEXT_NEON=OFF   -DALSOFT_REQUIRE_NEON=OFF
  # Output backends: all OFF. Null + loopback are always compiled in.
  -DALSOFT_BACKEND_PIPEWIRE=OFF   -DALSOFT_REQUIRE_PIPEWIRE=OFF
  -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_REQUIRE_PULSEAUDIO=OFF
  -DALSOFT_BACKEND_ALSA=OFF       -DALSOFT_REQUIRE_ALSA=OFF
  -DALSOFT_BACKEND_OSS=OFF        -DALSOFT_REQUIRE_OSS=OFF
  -DALSOFT_BACKEND_SOLARIS=OFF    -DALSOFT_REQUIRE_SOLARIS=OFF
  -DALSOFT_BACKEND_SNDIO=OFF      -DALSOFT_REQUIRE_SNDIO=OFF
  -DALSOFT_BACKEND_JACK=OFF       -DALSOFT_REQUIRE_JACK=OFF
  -DALSOFT_BACKEND_COREAUDIO=OFF  -DALSOFT_REQUIRE_COREAUDIO=OFF
  -DALSOFT_BACKEND_OBOE=OFF       -DALSOFT_REQUIRE_OBOE=OFF
  -DALSOFT_BACKEND_OPENSL=OFF     -DALSOFT_REQUIRE_OPENSL=OFF
  -DALSOFT_BACKEND_PORTAUDIO=OFF  -DALSOFT_REQUIRE_PORTAUDIO=OFF
  -DALSOFT_BACKEND_SDL3=OFF       -DALSOFT_REQUIRE_SDL3=OFF
  -DALSOFT_BACKEND_SDL2=OFF       -DALSOFT_REQUIRE_SDL2=OFF
  -DALSOFT_BACKEND_WAVE=OFF       # wave writer has no REQUIRE option
  # HRTF: the engine exposes an opt-in "HRTF (Headphones)" preference (off by
  # default) and asks ALC_HRTF_STATUS_SOFT whether it is available. The 3DS
  # has no OpenAL data directory (no XDG_DATA_DIRS, no /usr/share), so
  # without the embedded data set no HRTF would ever be found and the option
  # would be greyed out. Embedding "Default HRTF.mhr" costs ~160 KB of
  # .data in the final executable; HRTF mixing is CPU-heavy on the ARM11,
  # but it only runs when the player turns it on. OPENAL_EMBED_HRTF=OFF drops
  # the data (and therefore the feature) if the size matters more.
  -DALSOFT_EMBED_HRTF_DATA="$OPENAL_EMBED_HRTF"
)

echo "==> Configuring OpenAL Soft $OPENAL_VERSION (log: $LOG_DIR/openal-configure.log)"
cmake -S "$SRC" -B "$BLD" "${OPTS[@]}" >"$LOG_DIR/openal-configure.log" 2>&1 \
  || { tail -n 40 "$LOG_DIR/openal-configure.log" >&2; fail "configure failed (see $LOG_DIR/openal-configure.log)"; }
grep -E "Building OpenAL with support for|Enabled backends|Building with support for CPU extensions|Embedding HRTF|Installing" -A3 \
  "$LOG_DIR/openal-configure.log" || true

echo "==> Building (log: $LOG_DIR/openal-build.log)"
cmake --build "$BLD" --parallel "$JOBS" --verbose >"$LOG_DIR/openal-build.log" 2>&1 \
  || { grep -n -E "error:|Error [0-9]" "$LOG_DIR/openal-build.log" | head -n 40 >&2; fail "build failed (see $LOG_DIR/openal-build.log)"; }
echo "    $(grep -c 'warning:' "$LOG_DIR/openal-build.log" || true) compiler warnings (see log)"
if grep -q -- "-fPIC" "$LOG_DIR/openal-build.log"; then
  fail "some objects were still compiled with -fPIC (see $LOG_DIR/openal-build.log)"
fi

LIB="$BLD/libopenal.a"
[[ -f "$LIB" ]] || fail "$LIB was not produced"

echo "==> Installing into $PREFIX (log: $LOG_DIR/openal-install.log)"
${SUDO[@]+"${SUDO[@]}"} cmake --install "$BLD" >"$LOG_DIR/openal-install.log" 2>&1 \
  || { tail -n 20 "$LOG_DIR/openal-install.log" >&2; fail "install failed (see $LOG_DIR/openal-install.log)"; }
cp "$BLD/install_manifest.txt" "$LOG_DIR/openal-install-manifest.txt"

# --- Post-install sanity checks ---------------------------------------------
for f in include/AL/al.h include/AL/alc.h include/AL/alext.h include/AL/efx.h \
         lib/libopenal.a lib/pkgconfig/openal.pc lib/cmake/OpenAL/OpenALConfig.cmake; do
  [[ -f "$PREFIX/$f" ]] || fail "missing after install: $PREFIX/$f"
done
if find "$PREFIX/lib" -maxdepth 1 \( -name 'libopenal*.so*' -o -name 'libopenal*.dylib' \) | grep -q .; then
  fail "a shared libopenal was installed; expected static only"
fi
# (Captured once: `nm | grep -q` would SIGPIPE nm and trip pipefail.)
DEFINED_SYMS="$("$DEVKITARM/bin/arm-none-eabi-nm" -g --defined-only "$PREFIX/lib/libopenal.a" 2>/dev/null)"
for sym in alcLoopbackOpenDeviceSOFT alcRenderSamplesSOFT alcIsRenderFormatSupportedSOFT \
           alGenFilters alcGetProcAddress alGetProcAddress; do
  grep -q " T $sym\$" <<<"$DEFINED_SYMS" || fail "$sym is not defined in libopenal.a"
done

echo "==> Installed OpenAL Soft $OPENAL_VERSION into $PREFIX"

# --- Link test ----------------------------------------------------------------
# Compile and link scripts/3ds/tests/openal-loopback-test.cpp into a .3dsx with
# the flags from the installed openal.pc. This proves the static library links
# for the 3DS (it cannot be run here; copy the .3dsx to a 3DS or emulator).
PKGCONF="$PREFIX/bin/arm-none-eabi-pkg-config"
[[ -x "$PKGCONF" ]] || fail "$PKGCONF not found (install 3ds-pkg-config)"
OAL_CFLAGS="$("$PKGCONF" --cflags openal)" || fail "pkg-config --cflags openal failed"
OAL_LIBS="$("$PKGCONF" --static --libs openal)" || fail "pkg-config --static --libs openal failed"
echo "    pkg-config --cflags openal:        $OAL_CFLAGS"
echo "    pkg-config --static --libs openal: $OAL_LIBS"

TEST_SRC="$REPO_ROOT/scripts/3ds/tests/openal-loopback-test.cpp"
TEST_DIR="$WORK_DIR/openal-$OPENAL_VERSION-linktest"
TEST_LOG="$LOG_DIR/openal-linktest.log"
rm -rf "$TEST_DIR"; mkdir -p "$TEST_DIR"
ARCH=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
CXX="$DEVKITARM/bin/arm-none-eabi-g++"
echo "==> Link test (log: $TEST_LOG)"
run_logged() { echo "+ $*" >>"$TEST_LOG"; "$@" >>"$TEST_LOG" 2>&1; }
: >"$TEST_LOG"
# Each step is checked explicitly: errexit does not apply inside `|| ...`.
# shellcheck disable=SC2086  # pkg-config output is a word list
run_logged "$CXX" "${ARCH[@]}" -mword-relocations -ffunction-sections -D__3DS__ -O2 \
    -std=gnu++17 -fexceptions -frtti -Wall -Wextra \
    -isystem "$DEVKITPRO/libctru/include" $OAL_CFLAGS \
    -c "$TEST_SRC" -o "$TEST_DIR/openal-loopback-test.o" \
  || { tail -n 40 "$TEST_LOG" >&2; fail "link test: compile failed (see $TEST_LOG)"; }
# libctru lacks libsysbase's pthread-key hooks; without them alcCreateContext
# fails at run time (al::Context holds a tss<>). Link the same shim as the game.
HOOKS_SRC="$REPO_ROOT/ports/3ds/source/sysbase_hooks.c"
run_logged "$DEVKITARM/bin/arm-none-eabi-gcc" "${ARCH[@]}" -mword-relocations -ffunction-sections -D__3DS__ -O2 \
    -Wall -Wextra -isystem "$DEVKITPRO/libctru/include" \
    -c "$HOOKS_SRC" -o "$TEST_DIR/sysbase_hooks.o" \
  || { tail -n 40 "$TEST_LOG" >&2; fail "link test: compile of sysbase_hooks.c failed (see $TEST_LOG)"; }
# shellcheck disable=SC2086
run_logged "$CXX" "${ARCH[@]}" -specs=3dsx.specs -Wl,-Map,"$TEST_DIR/openal-loopback-test.map" \
    "$TEST_DIR/openal-loopback-test.o" "$TEST_DIR/sysbase_hooks.o" -o "$TEST_DIR/openal-loopback-test.elf" \
    -L"$DEVKITPRO/libctru/lib" $OAL_LIBS -lctru -lm \
  || { tail -n 40 "$TEST_LOG" >&2; fail "link test: link failed (see $TEST_LOG)"; }
run_logged "$DEVKITPRO/tools/bin/3dsxtool" "$TEST_DIR/openal-loopback-test.elf" \
    "$TEST_DIR/openal-loopback-test.3dsx" \
  || { tail -n 40 "$TEST_LOG" >&2; fail "link test: 3dsxtool failed (see $TEST_LOG)"; }
"$DEVKITARM/bin/arm-none-eabi-size" "$TEST_DIR/openal-loopback-test.elf" | tee -a "$TEST_LOG"
"$DEVKITARM/bin/arm-none-eabi-nm" "$TEST_DIR/openal-loopback-test.elf" | grep -q ' T __syscall_tls_create$' \
  || fail "link test: __syscall_tls_create is not defined in the test ELF"
echo "==> Link test OK: $TEST_DIR/openal-loopback-test.3dsx"
