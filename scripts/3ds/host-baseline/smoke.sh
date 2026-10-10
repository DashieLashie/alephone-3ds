#!/usr/bin/env bash
# Headless smoke test: software renderer, Marathon 2 data, main menu -> level.
# Runs inside aleph-host-baseline after build.sh.
#
#   docker run --rm -v "$PWD":/src:ro -v aleph-host-build:/build \
#       -v "$PWD/build/host-smoke":/out --cap-add=SYS_PTRACE aleph-host-baseline \
#       bash /src/scripts/3ds/host-baseline/smoke.sh "/src/data/Scenarios/Marathon 2"
#
# Writes screenshots, the engine log, per-phase thread lists and the files the
# engine created under $HOME to /out.
set -uo pipefail
DATA="${1:?usage: smoke.sh <Marathon 2 data dir>}"
OUT=/out
BIN=/build/src/Source_Files/alephone
mkdir -p "$OUT"
export HOME=/tmp/a1home DISPLAY=:99 SDL_AUDIODRIVER=dummy
rm -rf "$HOME"; mkdir -p "$HOME"

Xvfb :99 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
sleep 2

threads() {  # threads <label>
  { echo "== $1"; for t in /proc/$PID/task/*; do echo "$(basename "$t") $(cat "$t/comm")"; done; } >> "$OUT/threads.txt"
}
shot() { import -window root "$OUT/$1.png"; }
key() { xdotool search --sync --onlyvisible --class alephone windowactivate --sync 2>/dev/null; xdotool key --delay 100 "$@"; }

: > "$OUT/threads.txt"
"$BIN" -w -g "$DATA" > "$OUT/stdout.txt" 2>&1 &
PID=$!
sleep 12
threads "after startup (intro/menu)"
key Escape; sleep 1; key Escape; sleep 3   # skip intro screens if any
shot 01-main-menu
threads "main menu"

key n; sleep 12                             # New Game -> chapter screen -> level
key Escape; sleep 1; key Return; sleep 8    # skip chapter heading if still up
shot 02-level
threads "in level"
key --delay 50 w w w w w w w w; sleep 3     # nudge (default keys: w = forward)
shot 03-level-moved
threads "in level after input"

kill -0 $PID 2>/dev/null && echo "alive in level" >> "$OUT/threads.txt"
# Backtraces identify the unnamed threads; needs --cap-add=SYS_PTRACE.
gdb -p $PID -batch -ex 'thread apply all bt 12' > "$OUT/threads-bt.txt" 2>&1 || true
kill $PID; sleep 2; kill -9 $PID 2>/dev/null
( cd "$HOME" && find . -type f | sort ) > "$OUT/home-files.txt"
find "$HOME" -name 'Aleph One Log.txt' -exec cp {} "$OUT/" \;
cp -r "$HOME" "$OUT/home" 2>/dev/null
echo done
