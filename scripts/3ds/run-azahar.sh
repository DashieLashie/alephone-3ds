#!/usr/bin/env bash
# Run Aleph One in the Azahar 3DS emulator.
#
#   scripts/3ds/run-azahar.sh [options] [path/to/alephone.3dsx]
#
# Stages the emulated SD card the way a real one should look, then starts
# Azahar with the .3dsx and prints sdmc:/3ds/alephone/log.txt when it exits:
#
#   sdmc:/3ds/alephone.3dsx              the game (Azahar loads it from here)
#   sdmc:/3ds/alephone/Marathon2/        scenario data (default: data/Scenarios/Marathon 2)
#   sdmc:/3ds/alephone/log.txt           written by the game
#
# Options:
#   --no-data      do not copy scenario data (to see the "scenario missing" screen)
#   --fresh        delete sdmc:/3ds/alephone first (first-run test; asks first)
#   --timeout N    stop the emulator after N seconds (default: run until closed)
#   --screens      have the game save its screens (sdmc:/3ds/alephone/debug-screens flag)
#                  at the error screen and 3 s / 8 s after the main menu appears; they are
#                  copied as PNGs to build-3ds/azahar-screens/ afterwards (as are the
#                  captures of a script's "screens" command, with or without --screens)
#   --input-debug  log every input event (sdmc:/3ds/alephone/debug-input flag); the bottom
#                  screen shows them instead of the controls legend
#   --script FILE  run FILE as sdmc:/3ds/alephone/debug-input-script once the main menu is
#                  up (scripted buttons, sticks, touches, HOME/sleep; see
#                  ports/3ds/source/input_3ds.cpp and scripts/3ds/tests/input/)
#   --c3d FILE     install FILE as sdmc:/3ds/alephone/debug-c3d for this run (settings of the
#                  citro3d back end, a -DA1_RENDER_C3D=ON build: ports/3ds/render_c3d/core/config.h;
#                  docs/3ds/CITRO3D_DESIGN.md section 7); without it the file is removed
#   --reset-prefs  move the game's preferences aside first (to <file>.bak), so it starts
#                  with the defaults
#   --audio-debug  audio statistics on the bottom screen and in the log every 5 s, plus a
#                  burst of 8 sounds at the main menu (sdmc:/3ds/alephone/debug-audio flag)
#   --decoders     run the decoder self-test at the main menu: build-3ds/audio-test (made by
#                  scripts/3ds/tests/make-audio-test.sh) is copied to sdmc:/3ds/alephone/audio-test
#                  and the debug-decoders flag is set
#   --level-music  install scripts/3ds/tests/plugins/Level Music Test in the scenario's
#                  Plugins (Music.ogg on every level; Marathon 2 has no level music);
#                  without this option it is removed
#   --terminal-test  install scripts/3ds/tests/plugins/Terminal Test (solo Lua: opens the
#                  level's first terminal 3 s in; docs/3ds/STAGE7.md); without it it is removed
#   --load-test    install scripts/3ds/tests/plugins/Load Test (solo Lua: saves 5 s into
#                  level 0, teleports to level 1 at 10 s, saves again 5 s into level 1;
#                  docs/3ds/NEXT-SPRINT.md B1); without it it is removed
#   --no-dspfirm   hide sdmc:/3ds/dspfirm.cdc for this run (restored afterwards), to see
#                  the game run without a DSP: no sound and the one-time notice
#   --perf LEVEL   frame benchmark (an A1_PROFILE build; docs/3ds/PERF.md): writes
#                  sdmc:/3ds/alephone/debug-perf so BEGIN NEW GAME starts level LEVEL, the
#                  profiler measures PERF_MEASURE s (default 20) after PERF_SETTLE s (default 8)
#                  and quits; runs scripts/3ds/tests/input/11-perf-bench.txt unless --script is
#                  given; default --timeout 240. Prints the "[prof] BENCH" line at the end
#   --fatal FILE   copy FILE to sdmc:/3ds/alephone/debug-fatal (source/fatal_test_3ds.cpp: "mode N", "delay S",
#                  "lock 1", "first 0|1"); the run stops by itself, 2 s after the log's "exit" or
#                  "svcExitProcess" line (or the fallback's last lines, "svcExitProcess (aptExit branch...)" and
#                  "APT_CloseApplication via __system_retAddr"). They are printed BEFORE steps that could
#                  still hang (_Exit's __appExit, svcExitProcess): this is not proof that the guest ended
#                  (docs/3ds/CITRO3D_DESIGN.md 5.6.3 tests)
#   -h, --help     this text
#
# Environment:
#   AZAHAR         Azahar.app (or the folder containing it). Default: /Applications/Azahar.app,
#                  ~/Applications/Azahar.app, then the last (by name) portable ~/Applications/Azahar-*/Azahar.app
#   AZAHAR_SDMC    emulated SD card folder. Default: <folder of Azahar.app>/user/sdmc for a
#                  portable install (a "user" folder next to the app), else
#                  ~/Library/Application Support/Azahar/sdmc
#   SCENARIO_SRC   scenario folder to copy (default: <repo>/data/Scenarios/Marathon 2)
#   SCENARIO_NAME  folder name on the SD card (default: Marathon2, the game's default)
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
threedsx="$repo/build-3ds/alephone.3dsx"
copy_data=1
fresh=0
timeout=0
screens=0
input_debug=0
script=
c3d_cfg=
reset_prefs=0
audio_debug=0
no_dspfirm=0
decoders=0
level_music=0
terminal_test=0
load_test=0
perf_level=
fatal_file=

while [ $# -gt 0 ]; do
	case "$1" in
		--no-data) copy_data=0 ;;
		--fresh) fresh=1 ;;
		--timeout) shift; timeout=${1:?--timeout needs a number of seconds} ;;
		--screens) screens=1 ;;
		--input-debug) input_debug=1 ;;
		--script) shift; script=${1:?--script needs a file} ;;
		--c3d) shift; c3d_cfg=${1:?--c3d needs a file} ;;
		--reset-prefs) reset_prefs=1 ;;
		--audio-debug) audio_debug=1 ;;
		--no-dspfirm) no_dspfirm=1 ;;
		--decoders) decoders=1 ;;
		--level-music) level_music=1 ;;
		--terminal-test) terminal_test=1 ;;
		--load-test) load_test=1 ;;
		--fatal) shift; fatal_file=${1:?--fatal needs a file} ;;
		--perf) shift; perf_level=${1:?--perf needs a level number} ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
		-*) echo "run-azahar: unknown option $1" >&2; exit 2 ;;
		*) threedsx=$1 ;;
	esac
	shift
done

die() { echo "run-azahar: $*" >&2; exit 1; }

if [ -n "$perf_level" ]; then
	[ -n "$script" ] || script="$repo/scripts/3ds/tests/input/11-perf-bench.txt"
	[ "$timeout" -gt 0 ] || timeout=240
fi

[ -f "$threedsx" ] || die "$threedsx not found (cmake --build build-3ds)"
[ "$(head -c 4 "$threedsx")" = 3DSX ] || die "$threedsx is not a .3dsx file"

app=${AZAHAR:-}
if [ -z "$app" ]; then
	for c in /Applications/Azahar.app "$HOME/Applications/Azahar.app"; do
		[ -d "$c" ] && { app=$c; break; }
	done
	if [ -z "$app" ]; then
		# Portable installs, e.g. ~/Applications/Azahar-2126.2/{Azahar.app,user}; the last by name wins.
		for c in "$HOME"/Applications/Azahar-*/Azahar.app; do
			[ -d "$c" ] && app=$c
		done
	fi
fi
[ -n "$app" ] || die "Azahar not found; set AZAHAR=/path/to/Azahar.app"
[ -d "$app/Contents" ] || app="$app/Azahar.app"
exe="$app/Contents/MacOS/azahar"
[ -x "$exe" ] || die "$exe not found or not executable"

if [ -n "${AZAHAR_SDMC:-}" ]; then
	sdmc=$AZAHAR_SDMC
elif [ -d "$(dirname "$app")/user" ]; then
	sdmc="$(dirname "$app")/user/sdmc"
else
	sdmc="$HOME/Library/Application Support/Azahar/sdmc"
fi

# Azahar's startup update check opens a modal "Update Available" dialog when
# a newer release exists; the game does not start until it is dismissed.
config="$(dirname "$sdmc")/config/qt-config.ini"
if [ -f "$config" ] && grep -q '^check_for_update_on_start=true' "$config"; then
	echo "run-azahar: note: Azahar checks for updates on start; an 'Update Available' dialog" \
	     "holds the game until closed (Settings > General, or check_for_update_on_start=false in $config)" >&2
fi

userdir="$sdmc/3ds/alephone"
if [ "$fresh" = 1 ] && [ -d "$userdir" ]; then
	printf 'Delete %s (prefs, saves, logs, scenario data)? [y/N] ' "$userdir"
	read -r answer
	[ "$answer" = y ] || [ "$answer" = Y ] || die "not deleted"
	rm -rf "$userdir"
fi
mkdir -p "$sdmc/3ds" "$userdir"
cp "$threedsx" "$sdmc/3ds/alephone.3dsx"
# libctru's ndspInit() loads sdmc:/3ds/dspfirm.cdc (on a console it is dumped
# with DSP1); without it SDL_OpenAudio fails and the game runs silent.
# Azahar's default HLE DSP does not execute the firmware, so a placeholder
# (zeros, "DSP1" magic and size at 0x100) is enough there. Never copy this
# placeholder to a real SD card.
dspfirm="$sdmc/3ds/dspfirm.cdc"
if [ ! -f "$dspfirm" ]; then
	head -c 4096 /dev/zero > "$dspfirm"
	printf 'DSP1\000\020\000\000' | dd of="$dspfirm" bs=1 seek=256 conv=notrunc 2>/dev/null
	echo "run-azahar: created placeholder $dspfirm (HLE audio only)"
fi
if [ "$no_dspfirm" = 1 ]; then
	mv "$dspfirm" "$dspfirm.hidden"
	echo "run-azahar: hid $dspfirm for this run"
fi
rm -rf "$userdir/audio-test"
if [ "$decoders" = 1 ]; then
	[ -d "$repo/build-3ds/audio-test" ] || die "build-3ds/audio-test missing (scripts/3ds/tests/make-audio-test.sh)"
	cp -R "$repo/build-3ds/audio-test" "$userdir/audio-test"
	: > "$userdir/debug-decoders"
else
	rm -f "$userdir/debug-decoders"
fi
if [ "$audio_debug" = 1 ]; then
	: > "$userdir/debug-audio"
else
	rm -f "$userdir/debug-audio"
fi
rm -f "$userdir"/screen-*.bmp
if [ -n "$perf_level" ]; then
	printf 'level %s\nsettle %s\nmeasure %s\nquit 1\nlog 1\n' "$perf_level" \
		"${PERF_SETTLE:-8}" "${PERF_MEASURE:-20}" > "$userdir/debug-perf"
	# The game quits after the benchmark but Azahar stays open; the loop below
	# stops it once this run's log says "exit". (The game would rotate the
	# old log to log.prev.txt itself.)
	[ -f "$userdir/log.txt" ] && mv "$userdir/log.txt" "$userdir/log.prev.txt"
else
	rm -f "$userdir/debug-perf"
fi
if [ -n "$fatal_file" ]; then
	[ -f "$fatal_file" ] || die "fatal file $fatal_file not found"
	cp "$fatal_file" "$userdir/debug-fatal"
	[ -f "$userdir/log.txt" ] && mv "$userdir/log.txt" "$userdir/log.prev.txt"
else
	rm -f "$userdir/debug-fatal"
fi
if [ "$screens" = 1 ]; then
	: > "$userdir/debug-screens"
else
	rm -f "$userdir/debug-screens"
fi
if [ "$input_debug" = 1 ]; then
	: > "$userdir/debug-input"
else
	rm -f "$userdir/debug-input"
fi
if [ -n "$script" ]; then
	[ -f "$script" ] || die "script $script not found"
	cp "$script" "$userdir/debug-input-script"
else
	rm -f "$userdir/debug-input-script"
fi
if [ -n "$c3d_cfg" ]; then
	[ -f "$c3d_cfg" ] || die "c3d config $c3d_cfg not found"
	cp "$c3d_cfg" "$userdir/debug-c3d"
else
	rm -f "$userdir/debug-c3d"
fi
if [ "$reset_prefs" = 1 ]; then
	for prefs in "$userdir"/*Preferences; do
		[ -f "$prefs" ] || continue
		mv "$prefs" "$prefs.bak"
		echo "run-azahar: moved $(basename "$prefs") to $(basename "$prefs").bak"
	done
fi

if [ "$copy_data" = 1 ]; then
	src=${SCENARIO_SRC:-"$repo/data/Scenarios/Marathon 2"}
	name=${SCENARIO_NAME:-Marathon2}
	[ -d "$src" ] || die "scenario folder $src not found"
	mkdir -p "$userdir/$name"
	# Update in place: keeps anything the game or the user added.
	rsync -a "$src/" "$userdir/$name/"
	echo "run-azahar: scenario $src -> sdmc:/3ds/alephone/$name"
	plugin="$userdir/$name/Plugins/Level Music Test"
	rm -rf "$plugin"
	if [ "$level_music" = 1 ]; then
		cp -R "$repo/scripts/3ds/tests/plugins/Level Music Test" "$plugin"
		echo "run-azahar: installed the Level Music Test plugin"
	fi
	plugin="$userdir/$name/Plugins/Terminal Test"
	rm -rf "$plugin"
	if [ "$terminal_test" = 1 ]; then
		cp -R "$repo/scripts/3ds/tests/plugins/Terminal Test" "$plugin"
		echo "run-azahar: installed the Terminal Test plugin"
	fi
	plugin="$userdir/$name/Plugins/Load Test"
	rm -rf "$plugin"
	if [ "$load_test" = 1 ]; then
		cp -R "$repo/scripts/3ds/tests/plugins/Load Test" "$plugin"
		echo "run-azahar: installed the Load Test plugin"
	fi
fi

echo "run-azahar: SD card $sdmc"
echo "run-azahar: starting $exe"
# Start Azahar detached (double fork, so launchd becomes its parent). Run as
# a plain child of this script ("azahar & wait"), it stops at a modal dialog
# during startup and never boots the game (seen with Azahar 2126.2 on
# macOS 26). The script polls the PID instead of waiting for it.
pidfile=$(mktemp -t run-azahar)
( "$exe" "$sdmc/3ds/alephone.3dsx" & echo $! > "$pidfile" )
pid=$(cat "$pidfile")
rm -f "$pidfile"
[ -n "$pid" ] || die "could not start $exe"
trap 'kill "$pid" 2>/dev/null' INT TERM
elapsed=0
while kill -0 "$pid" 2>/dev/null; do
	if [ "$timeout" -gt 0 ] && [ "$elapsed" -ge "$timeout" ]; then
		# Azahar ignores SIGTERM while a modal dialog is open; follow up with KILL.
		kill "$pid" 2>/dev/null || true
		sleep 5
		kill -9 "$pid" 2>/dev/null || true
		echo "run-azahar: stopped Azahar after ${timeout} s"
		break
	fi
	if { [ -n "$perf_level" ] && grep -q ' ms: exit$' "$userdir/log.txt" 2>/dev/null; } ||
	   { [ -n "$fatal_file" ] && grep -Eq ' ms: exit$|svcExitProcess$|svcExitProcess \(aptExit branch.*\)$|via __system_retAddr$' "$userdir/log.txt" 2>/dev/null; }; then
		sleep 2
		kill "$pid" 2>/dev/null || true
		sleep 3
		kill -9 "$pid" 2>/dev/null || true
		echo "run-azahar: the exit line was reached; stopped Azahar after ${elapsed} s (2 s later)"
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done
status=0
if [ "$no_dspfirm" = 1 ]; then
	mv "$dspfirm.hidden" "$dspfirm"
	echo "run-azahar: restored $dspfirm"
fi

echo
if [ "$screens" = 1 ] || [ -n "$script" ]; then
	out="$repo/build-3ds/azahar-screens"
	rm -rf "$out"
	mkdir -p "$out"
	for bmp in "$userdir"/screen-*.bmp; do
		[ -f "$bmp" ] || continue
		png="$out/$(basename "${bmp%.bmp}").png"
		sips -s format png "$bmp" --out "$png" >/dev/null && echo "run-azahar: $png"
	done
fi
if [ -f "$userdir/log.txt" ]; then
	echo "===== sdmc:/3ds/alephone/log.txt ====="
	cat "$userdir/log.txt"
	if [ -n "$perf_level" ]; then
		echo "=====" 
		grep '\[prof\] BENCH' "$userdir/log.txt" || echo "run-azahar: no BENCH line (not an A1_PROFILE build, or the run was cut short)"
	fi
else
	echo "run-azahar: no log.txt in $userdir (the game did not get as far as opening it)"
fi
exit "$status"
