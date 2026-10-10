#!/usr/bin/env bash
# Stage 2: Linux build restricted to the planned 3DS feature set.
#
#   docker run --rm -v "$PWD":/src:ro -v aleph-host-build-3ds:/build \
#       aleph-host-baseline bash /src/scripts/3ds/host-baseline/build-3ds-features.sh [extra configure args...]
#
# Extra arguments are appended, e.g. --disable-network or CPPFLAGS=-DA1_FS_STD.
set -euo pipefail
exec bash "$(dirname "$0")/build.sh" \
  --disable-opengl --without-sdl_image --without-curl --without-zzip --without-png \
  --without-miniupnpc --without-vpx --without-matroska --without-ebml --without-vorbis \
  --without-vorbisenc --without-libyuv --without-nfd "$@"
