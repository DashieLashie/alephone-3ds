#!/usr/bin/env bash
# Stage 2 runtime check (inside aleph-host-baseline): plays Marathon 2 under
# Xvfb with SDL's disk audio driver, drives levels 1-2 with harness/harness.lua
# (quick saves + level change), quits, edits a preference, then reloads the
# newest quick save from the command line.
#
#   docker run --rm -v "$PWD":/src:ro -v <build volume>:/build -v "$PWD/build/<out>":/out \
#       aleph-host-baseline bash /src/scripts/3ds/host-baseline/play-verify.sh \
#       /build/src/Source_Files/alephone "/src/data/Scenarios/Marathon 2"
#
# Output in /out: p1-*.png, p2-*.png, p{1,2}-audio.raw (S16 stereo 44.1 kHz),
# events.txt (wall-clock offsets of each phase), harness.log, prefs before and
# after, quick save listing.
set -uo pipefail
BIN="${1:?binary}"; DATA="${2:?data dir}"; OUT=/out
HERE="$(cd "$(dirname "$0")" && pwd)"
export HOME=/tmp/a1home DISPLAY=:99 SDL_AUDIODRIVER=disk ALSOFT_DRIVERS=null
rm -rf "$HOME" "$OUT"/*; mkdir -p "$HOME/.alephone/Plugins" "$OUT"
cp -r "$HERE/harness" "$HOME/.alephone/Plugins/A1Harness"
Xvfb :99 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
sleep 2
T0=$(date +%s%N)
ev() { echo "$(( ($(date +%s%N) - T0) / 1000000 ))ms $1" >> "$OUT/events.txt"; }
shot() { import -window root "$OUT/$1.png"; }
# No window manager under Xvfb: focus the window directly, never block on --sync
key() {
  local w; w=$(timeout 5 xdotool search --onlyvisible --class alephone 2>/dev/null | head -1)
  [ -n "$w" ] && timeout 5 xdotool windowfocus "$w" 2>/dev/null
  timeout 10 xdotool key --delay 80 "$@"
}
quit_engine() {  # SIGTERM -> SDL_QUIT: in game -> menu (recent save, no prompt), at menu -> exit
  for _ in 1 2 3; do kill -0 "$PID" 2>/dev/null || break; kill -TERM "$PID"; sleep 4; done
  wait "$PID"; echo "exit=$?" >> "$OUT/events.txt"
}

# Phase 1: menu music, new game, SFX, saves, level 1 -> 2
export SDL_DISKAUDIOFILE="$OUT/p1-audio.raw"
"$BIN" -w -g -i "$DATA" > "$OUT/p1-stdout.txt" 2>&1 &
PID=$!; ev "p1 start"
sleep 10; ev "p1 menu"; key Return; sleep 1; key Escape; sleep 6; shot p1-01-menu   # Return dismisses the -i warning
# --disable-network builds: Gather/Join must be inert (shortcuts and a click do
# nothing). Set NET_PROBE=0 for network builds, where they open real dialogs.
if [ "${NET_PROBE:-1}" = 1 ]; then
  key g; sleep 2; key j; sleep 2; timeout 5 xdotool mousemove 310 420 click 1; sleep 2; shot p1-01b-after-gather-join
fi
ev "p1 new game"; key n; sleep 10; key Return; sleep 4   # Return skips the chapter screen; Escape would quit
shot p1-02-level1; ev "p1 level1 fire"
timeout 5 xdotool keydown space; sleep 3; timeout 5 xdotool keyup space; ev "p1 fire end"
key --delay 60 w w w w w w w w w w; sleep 1
timeout 5 xdotool keydown space; sleep 2; timeout 5 xdotool keyup space
sleep 22; ev "p1 after teleport"; key Return; sleep 6
shot p1-03-level2; ev "p1 level2 fire"
timeout 5 xdotool keydown space; sleep 3; timeout 5 xdotool keyup space; sleep 6
shot p1-04-level2-later; ev "p1 quit"
quit_engine
cp "$HOME/.alephone/Marathon 2 Preferences" "$OUT/prefs-after-p1.xml" 2>/dev/null
( cd "$HOME/.alephone" && find . -type f | sort ) > "$OUT/home-after-p1.txt"

# Preference round trip: change two values the engine must keep
PREFS="$HOME/.alephone/Marathon 2 Preferences"
sed -i -e 's/fps_target="30"/fps_target="31"/' -e 's/crosshairs_active="false"/crosshairs_active="true"/' "$PREFS"
cp "$PREFS" "$OUT/prefs-edited.xml"

# Phase 2: load the newest quick save from the command line
SAVE=$(ls -t "$HOME/.alephone/Quick Saves/"*/*.sgaA 2>/dev/null | head -1)
echo "loading: $SAVE" >> "$OUT/events.txt"
export SDL_DISKAUDIOFILE="$OUT/p2-audio.raw"
"$BIN" -w -g -i "$DATA" "$SAVE" > "$OUT/p2-stdout.txt" 2>&1 &
PID=$!; ev "p2 start"
sleep 6; key Return; sleep 8   # dismiss the -i "Insecure Lua" warning
 shot p2-01-loaded; ev "p2 fire"
timeout 5 xdotool keydown space; sleep 3; timeout 5 xdotool keyup space; sleep 5
shot p2-02-later; ev "p2 quit"
quit_engine
cp "$PREFS" "$OUT/prefs-after-p2.xml"
cp "$HOME/harness.log" "$OUT/" 2>/dev/null
find "$HOME" -name '*Log.txt' -exec cp {} "$OUT/" \;
ls -la "$HOME/.alephone/Quick Saves/"* > "$OUT/quicksaves.txt" 2>&1
echo done
