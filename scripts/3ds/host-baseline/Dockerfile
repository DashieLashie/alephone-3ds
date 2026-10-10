# Linux host-baseline build environment for Aleph One (stage 1 of the 3DS port).
# Package list = upstream .github/actions/configure-linux (non-vcpkg path),
# plus Xvfb/xdotool/ImageMagick/gdb for headless smoke tests.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -y && apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config git ca-certificates \
      autoconf-archive libsdl2-dev libsdl2-image-dev libasio-dev libsdl2-ttf-dev \
      libpng-dev libcurl4-gnutls-dev libopenal-dev libglu1-mesa-dev \
      catch2 libboost-all-dev libmatroska-dev libebml-dev libyuv-dev \
      libsndfile-dev libvorbis-dev libvorbisenc2 libzzip-dev libvpx-dev libminiupnpc-dev \
      xvfb xdotool imagemagick gdb procps \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
