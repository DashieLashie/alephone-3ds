#!/usr/bin/env bash
# Builds the decoder test files for the debug-decoders self-test
# (ports/3ds/source/selftest.cpp, docs/3ds/STAGE6.md):
#
#   scripts/3ds/tests/make-audio-test.sh [OUT_DIR]     (default build-3ds/audio-test)
#
# Needs ffmpeg and lame (Homebrew). run-azahar.sh --decoders copies OUT_DIR to
# sdmc:/3ds/alephone/audio-test; for a console, put it there with
# scripts/3ds/sd-ftp.sh IP putdir OUT_DIR /3ds/alephone/audio-test and create
# an empty sdmc:/3ds/alephone/debug-decoders.
#
# Valid files must PASS; the damaged ones must end (PASS, FAIL or "not
# decodable") without hanging or crashing the game.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-"$repo/build-3ds/audio-test"}
music="$repo/data/Scenarios/Marathon 2/Music.ogg"
command -v ffmpeg >/dev/null || { echo "make-audio-test: ffmpeg not found" >&2; exit 1; }
command -v lame >/dev/null || { echo "make-audio-test: lame not found" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
# 6 s: 440 Hz left, 660 Hz right
ff -f lavfi -i "sine=f=440:d=6:r=44100" -f lavfi -i "sine=f=660:d=6:r=44100" \
   -filter_complex "[0][1]amerge=inputs=2" -c:a pcm_s16le "$tmp/st44.wav"
ff -i "$tmp/st44.wav" -ac 1 -ar 22050 "$tmp/mono22.wav"
ff -i "$tmp/st44.wav" -ar 48000 "$tmp/st48.wav"

# valid
lame --quiet -b 128 "$tmp/st44.wav" "$out/01-mp3-44k-stereo-cbr.mp3"
lame --quiet -V 5 --tt "Aleph test" --ta "3DS port" --id3v2-only "$tmp/mono22.wav" "$out/02-mp3-22k-mono-vbr-id3.mp3"
lame --quiet -b 160 -t "$tmp/st48.wav" "$out/03-mp3-48k-stereo-noxing.mp3"
# ffmpeg's own Vorbis encoder (Homebrew's ffmpeg has no libvorbis) is stereo only
ff -i "$tmp/st44.wav" -ar 22050 -c:a vorbis -strict -2 "$out/04-ogg-22k-stereo.ogg"
ff -i "$tmp/st44.wav" -c:a vorbis -strict -2 "$out/05-ogg-44k-stereo.ogg"
cp "$tmp/mono22.wav" "$out/06-wav-22k-mono.wav"          # libsndfile, as before
# unsupported: Ogg Opus (must be rejected)
ff -i "$tmp/st44.wav" -c:a libopus -b:a 64k "$out/07-ogg-opus.ogg"
# damaged
size=$(wc -c < "$music")
head -c $((size * 6 / 10)) "$music" > "$out/08-ogg-truncated.ogg"
cp "$music" "$out/09-ogg-hole.ogg"
dd if=/dev/zero of="$out/09-ogg-hole.ogg" bs=4096 seek=40 count=2 conv=notrunc 2>/dev/null
mp3=$out/01-mp3-44k-stereo-cbr.mp3
head -c $(($(wc -c < "$mp3") / 2 + 117)) "$mp3" > "$out/10-mp3-truncated.mp3"
head -c 65536 /dev/urandom > "$out/11-garbage.mp3"
: > "$out/12-empty.mp3"
ls -l "$out"
