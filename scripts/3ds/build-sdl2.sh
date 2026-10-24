#!/usr/bin/env bash
# Build SDL2 and SDL2_ttf for the 3DS and install them into
# $DEVKITPRO/portlibs/3ds. dkp-libs ships only SDL 1.2 for the 3DS
# (3ds-sdl, 3ds-sdl_ttf, ...), so SDL2 has to be built from upstream.
#
# Usage:  scripts/3ds/build-sdl2.sh
#
# Environment overrides:
#   SDL2_TAG       upstream tag to build (default: newest release-2.* tag)
#   SDL2_TTF_TAG   likewise for SDL_ttf
#   WORK_DIR       clone/build directory (default: <repo>/build/3ds-deps)
#
# Patches in scripts/3ds/patches/<name>-<tag>/ (as for OpenAL) are applied
# after the clone; SDL2-release-2.32.10/0001: the N3DS joystick driver
# releases HID once (backlog item 14). Another tag gets no patches: port
# them first.
#
# Installing into portlibs needs root when it is owned by root; the script
# calls `sudo` for the install step only.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/3ds-deps}"

fail() { echo "ERROR: $*" >&2; exit 1; }

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set"
[[ -n "${DEVKITARM:-}" ]] || fail "DEVKITARM is not set"
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
[[ -f "$TOOLCHAIN" ]] || fail "$TOOLCHAIN not found (install 3ds-cmake via setup-env.sh)"
command -v cmake >/dev/null || fail "host cmake not found"
command -v git >/dev/null   || fail "git not found"

PREFIX="$DEVKITPRO/portlibs/3ds"
[[ -d "$PREFIX" ]] || fail "$PREFIX not found (install 3ds-dev via setup-env.sh)"

SUDO=()
[[ -w "$PREFIX" ]] || SUDO=(sudo)

latest_release2() {
  git ls-remote --tags --refs "$1" 'release-2.*' \
    | sed 's#.*refs/tags/##' | sort -V | tail -n 1
}

# build <name> <repo-url> <tag> [extra cmake args...]
build() {
  local name="$1" url="$2" tag="$3"
  shift 3
  local src="$WORK_DIR/$name-$tag" bld="$WORK_DIR/$name-$tag-build"

  echo "==> $name $tag"
  if [[ ! -d "$src" ]]; then
    git clone --depth 1 --branch "$tag" "$url" "$src"
  fi
  # Port patches for this name and tag; skip the ones already applied.
  local patch
  for patch in "$REPO_ROOT/scripts/3ds/patches/$name-$tag"/*.patch; do
    [[ -f "$patch" ]] || continue
    if git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
      echo "    patch already applied: $(basename "$patch")"
    else
      git -C "$src" apply "$patch" || fail "$(basename "$patch") does not apply to $name $tag"
      echo "    patch applied: $(basename "$patch")"
    fi
  done

  cmake -S "$src" -B "$bld" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    "$@"
  cmake --build "$bld" --parallel
  "${SUDO[@]}" cmake --install "$bld"
}

mkdir -p "$WORK_DIR"

SDL2_TAG="${SDL2_TAG:-$(latest_release2 https://github.com/libsdl-org/SDL.git)}"
SDL2_TTF_TAG="${SDL2_TTF_TAG:-$(latest_release2 https://github.com/libsdl-org/SDL_ttf.git)}"
[[ -n "$SDL2_TAG" && -n "$SDL2_TTF_TAG" ]] || fail "could not resolve release-2.x tags"

build SDL2 https://github.com/libsdl-org/SDL.git "$SDL2_TAG" \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF

build SDL2_ttf https://github.com/libsdl-org/SDL_ttf.git "$SDL2_TTF_TAG" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_SAMPLES=OFF \
  -DSDL2_DIR="$PREFIX/lib/cmake/SDL2"

echo "==> Installed SDL2 $SDL2_TAG and SDL2_ttf $SDL2_TTF_TAG into $PREFIX"
