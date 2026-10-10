#!/usr/bin/env bash
# Verify the devkitPro environment and install the 3DS packages the Aleph One
# port needs. Writes docs/3ds/portlibs-inventory.txt.
#
# Usage:  scripts/3ds/setup-env.sh
# Package installation needs root; the script calls `sudo` for that step only,
# so run it as your normal user.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INVENTORY="$REPO_ROOT/docs/3ds/portlibs-inventory.txt"

PACKAGES=(
  3ds-dev 3ds-cmake 3ds-pkg-config
  3ds-zlib 3ds-libpng 3ds-freetype
  3ds-libogg 3ds-libvorbisidec 3ds-mpg123
  picasso 3dslink
)

fail() { echo "ERROR: $*" >&2; exit 1; }

# --- environment -------------------------------------------------------------
[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set (expected /opt/devkitpro).
  export DEVKITPRO=/opt/devkitpro"
[[ -d "$DEVKITPRO" ]]     || fail "DEVKITPRO=$DEVKITPRO does not exist."
[[ -n "${DEVKITARM:-}" ]] || fail "DEVKITARM is not set (expected \$DEVKITPRO/devkitARM).
  export DEVKITARM=\$DEVKITPRO/devkitARM"

# Prefer the `dkp-pacman` wrapper (the macOS installer puts one in
# /usr/local/bin); fall back to the raw binary.
PACMAN="$(command -v dkp-pacman || true)"
if [[ -z "$PACMAN" && -x "$DEVKITPRO/pacman/bin/pacman" ]]; then
  PACMAN="$DEVKITPRO/pacman/bin/pacman"
fi
[[ -n "$PACMAN" ]] || fail "dkp-pacman not found. Install devkitpro-pacman:
  https://github.com/devkitPro/pacman/releases"

# GPGME locates gpgconf/gpg via PATH. On macOS they live only in
# $DEVKITPRO/pacman/bin; calling the raw pacman binary without it fails every
# signature check with "GPGME error: Invalid crypto engine".
if [[ -d "$DEVKITPRO/pacman/bin" ]]; then
  export PATH="$DEVKITPRO/pacman/bin:$PATH"
fi

echo "DEVKITPRO=$DEVKITPRO"
echo "DEVKITARM=$DEVKITARM"
echo "dkp-pacman: $PACMAN"

# sudo resets PATH, so pass it through explicitly.
SUDO=()
[[ $EUID -eq 0 ]] || SUDO=(sudo env "PATH=$PATH")

# --- install -----------------------------------------------------------------
echo "==> Syncing package databases"
"${SUDO[@]}" "$PACMAN" -Sy

# Check every package against the sync DB first so a missing one is reported by
# name instead of aborting the whole transaction. 3ds-dev is a group, not a
# package, so -Si alone would report it missing.
available=()
missing=()
for pkg in "${PACKAGES[@]}"; do
  if "$PACMAN" -Si "$pkg" >/dev/null 2>&1 || "$PACMAN" -Sg "$pkg" >/dev/null 2>&1; then
    available+=("$pkg")
  else
    missing+=("$pkg")
  fi
done

if (( ${#available[@]} )); then
  echo "==> Installing: ${available[*]}"
  "${SUDO[@]}" "$PACMAN" -S --needed --noconfirm "${available[@]}"
fi

# --- inventory ---------------------------------------------------------------
mkdir -p "$(dirname "$INVENTORY")"
echo "==> Writing $INVENTORY"
"$PACMAN" -Sl dkp-libs | grep '3ds-' > "$INVENTORY"
echo "    $(wc -l < "$INVENTORY" | tr -d ' ') 3ds-* packages in dkp-libs"

for lib in 3ds-sdl 3ds-sdl2 3ds-sdl2_ttf 3ds-sdl2_image 3ds-sdl2_mixer 3ds-sdl2_net; do
  if grep -q " $lib " "$INVENTORY"; then
    echo "    $lib: available"
  else
    echo "    $lib: NOT in dkp-libs"
  fi
done

# --- host tools --------------------------------------------------------------
if ! command -v cmake >/dev/null 2>&1; then
  echo "WARNING: host cmake not found on PATH; the 3DS CMake toolchain needs it." >&2
fi

if (( ${#missing[@]} )); then
  fail "these packages do not exist in the configured repos: ${missing[*]}"
fi
echo "==> Done"
