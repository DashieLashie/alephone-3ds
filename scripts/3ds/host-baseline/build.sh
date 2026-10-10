#!/usr/bin/env bash
# Build Aleph One on Linux inside the aleph-host-baseline container.
#
# Usage (from the repo root, on any Docker host):
#   docker build -t aleph-host-baseline scripts/3ds/host-baseline
#   docker run --rm -v "$PWD":/src:ro -v aleph-host-build:/build \
#       aleph-host-baseline bash /src/scripts/3ds/host-baseline/build.sh
#
# The source tree is copied into the /build volume so the autotools/compiler
# output never lands in the checkout.
set -euo pipefail
rm -rf /build/src
mkdir -p /build/src
# Copy the checkout minus build products and the scenario data.
tar -C /src --exclude=./build --exclude='./data/Scenarios/Marathon 2' -cf - . | tar -C /build/src -xf -
cd /build/src
{
  autoreconf -i
  ./configure
} 2>&1 | tee /build/configure.log
make -j"$(nproc)" 2>&1 | tee /build/make.log
ls -la Source_Files/alephone
