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

for level in $levels; do
	for run in $(seq 1 "$runs"); do
		log="$out/$label-L$level-r$run.log"
		echo "perf-bench: $label level $level run $run"
		"$repo/scripts/3ds/run-azahar.sh" --perf "$level" ${dsp[@]+"${dsp[@]}"} "$threedsx" > "$log" 2>&1 || true
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
