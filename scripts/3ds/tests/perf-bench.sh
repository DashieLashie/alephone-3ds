#!/usr/bin/env bash
# Frame benchmark series in Azahar (docs/3ds/PERF.md).
#
#   scripts/3ds/tests/perf-bench.sh LABEL [3dsx]
#
# Runs run-azahar.sh --perf for every level in LEVELS (default "0 6 20"),
# RUNS times each (default 2), without the DSP firmware (Azahar runs the
# audio thread on the main thread's core, so with sound its time would be
# spread over the measured zones; on a console it runs on core 1). Set
# AUDIO=1 to keep sound.
#
# Results: build/perf/LABEL.txt (one "[prof] BENCH" line per run, prefixed
# with the run) and build/perf/LABEL-L<level>-{top,bottom}.png (the screens
# at the end of the last run of each level). The .3dsx defaults to
# build-3ds-prof/alephone.3dsx (configure with -DA1_PROFILE=ON).
#
# PREFS="attr=value ..." sets attributes of the <graphics> preferences for
# the series (e.g. PREFS="scmode_highres=false fps_target=60", or
# "bottom_screen=2" for Classic, docs/3ds/STAGE7.md); missing attributes are
# added. The file is restored afterwards. The game must have written its
# preferences once.
#
# C3D=FILE installs FILE as debug-c3d for every run (run-azahar.sh --c3d; a
# -DA1_RENDER_C3D=ON build, docs/3ds/CITRO3D_DESIGN.md section 7).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
label=${1:?usage: perf-bench.sh LABEL [3dsx]}
threedsx=${2:-$repo/build-3ds-prof/alephone.3dsx}
levels=${LEVELS:-0 6 20}
runs=${RUNS:-2}
out="$repo/build/perf"
mkdir -p "$out"
results="$out/$label.txt"
: > "$results"
dsp=(--no-dspfirm)
[ "${AUDIO:-0}" = 1 ] && dsp=()
c3d=()
[ -n "${C3D:-}" ] && c3d=(--c3d "$C3D")

if [ -n "${PREFS:-}" ]; then
	prefs="$HOME/Applications/Azahar-2126.2/user/sdmc/3ds/alephone/Marathon 2 Preferences"
	[ -n "${PREFS_FILE:-}" ] && prefs=$PREFS_FILE
	[ -f "$prefs" ] || { echo "perf-bench: $prefs not found (set PREFS_FILE)" >&2; exit 1; }
	cp "$prefs" "$out/prefs.bak"
	trap 'cp "$out/prefs.bak" "$prefs"; echo "perf-bench: preferences restored"' EXIT
	for kv in $PREFS; do
		k=${kv%%=*}; v=${kv#*=}
		if grep -q " $k=\"" "$prefs"; then
			sed -i '' "s/ $k=\"[^\"]*\"/ $k=\"$v\"/" "$prefs"
		else
			# Not written yet (e.g. bottom_screen before a build that knows it).
			sed -i '' "s/<graphics /<graphics $k=\"$v\" /" "$prefs"
		fi
	done
	echo "perf-bench: preferences: $PREFS" | tee -a "$results"
fi

for level in $levels; do
	for run in $(seq 1 "$runs"); do
		log="$out/$label-L$level-r$run.log"
		echo "perf-bench: $label level $level run $run"
		"$repo/scripts/3ds/run-azahar.sh" --perf "$level" ${dsp[@]+"${dsp[@]}"} ${c3d[@]+"${c3d[@]}"} "$threedsx" > "$log" 2>&1 || true
		line=$(grep -a '^\[prof\] BENCH' "$log" | tail -n 1 || true)
		if [ -z "$line" ]; then
			echo "perf-bench: no BENCH line; see $log" >&2
			line="[prof] BENCH level=$level FAILED"
		fi
		echo "run=$run $line" | tee -a "$results"
		for side in top bottom; do
			png="$repo/build-3ds/azahar-screens/screen-perf-L$level-$side.png"
			[ -f "$png" ] && cp "$png" "$out/$label-L$level-$side.png"
		done
	done
done
echo "perf-bench: $results"
