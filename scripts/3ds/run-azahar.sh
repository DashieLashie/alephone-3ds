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
#                  copied as PNGs to build-3ds/azahar-screens/ afterwards
#   -h, --help     this text
#
# Environment:
#   AZAHAR         Azahar.app (or the folder containing it). Default: /Applications/Azahar.app,
#                  then ~/Applications/Azahar.app
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

while [ $# -gt 0 ]; do
	case "$1" in
		--no-data) copy_data=0 ;;
		--fresh) fresh=1 ;;
		--timeout) shift; timeout=${1:?--timeout needs a number of seconds} ;;
		--screens) screens=1 ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
		-*) echo "run-azahar: unknown option $1" >&2; exit 2 ;;
		*) threedsx=$1 ;;
	esac
	shift
done

die() { echo "run-azahar: $*" >&2; exit 1; }

[ -f "$threedsx" ] || die "$threedsx not found (cmake --build build-3ds)"
[ "$(head -c 4 "$threedsx")" = 3DSX ] || die "$threedsx is not a .3dsx file"

app=${AZAHAR:-}
if [ -z "$app" ]; then
	for c in /Applications/Azahar.app "$HOME/Applications/Azahar.app"; do
		[ -d "$c" ] && { app=$c; break; }
	done
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
rm -f "$userdir"/screen-*.bmp
if [ "$screens" = 1 ]; then
	: > "$userdir/debug-screens"
else
	rm -f "$userdir/debug-screens"
fi

if [ "$copy_data" = 1 ]; then
	src=${SCENARIO_SRC:-"$repo/data/Scenarios/Marathon 2"}
	name=${SCENARIO_NAME:-Marathon2}
	[ -d "$src" ] || die "scenario folder $src not found"
	mkdir -p "$userdir/$name"
	# Update in place: keeps anything the game or the user added.
	rsync -a "$src/" "$userdir/$name/"
	echo "run-azahar: scenario $src -> sdmc:/3ds/alephone/$name"
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
	sleep 1
	elapsed=$((elapsed + 1))
done
status=0

echo
if [ "$screens" = 1 ]; then
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
else
	echo "run-azahar: no log.txt in $userdir (the game did not get as far as opening it)"
fi
exit "$status"
