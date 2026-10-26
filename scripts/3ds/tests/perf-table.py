#!/usr/bin/env python3
"""Summarise perf-bench.sh results (docs/3ds/PERF.md) as Markdown tables.

    python3 -I scripts/3ds/tests/perf-table.py build/perf/baseline.txt [build/perf/other.txt ...]

One table row per file and level: the mean of its runs (ms per frame), and
the spread of the frame time across runs. "busy" is frame - idle: the work
per frame, which keeps showing gains once the 30 fps cap is reached. With
two or more files, a last table compares each file's busy time with the
first file's, level by level.

Logs of a citro3d profile build carry the zones c3d_wait, c3d_fill, c3d_upload
and c3d_vblank (all inside "present", exclusive); when any file has them, those
columns and "present total" (= present + the four, design 5.7.11) are added.
Logs with none of them (software builds) print exactly as before; a P1-era log (c3d_wait
only, the fill still inside "present") gets the columns with "-" where a zone is missing.
"""
import re
import sys
from collections import defaultdict

COLS = ["fps", "frame", "busy", "max", "world", "render", "hud", "blit", "present", "bottom", "idle", "other"]
# Zones of the citro3d back end (A1_RENDER_C3D profile builds), inside "present".
C3D_COLS = ["c3d_wait", "c3d_fill", "c3d_upload", "c3d_vblank"]


def load(path):
    runs = defaultdict(list)
    for line in open(path, encoding="utf-8", errors="replace"):
        if "BENCH" not in line or "FAILED" in line:
            continue
        kv = dict(re.findall(r"(\w+)=([-\d.]+)", line))
        r = {k: float(v) for k, v in kv.items() if k != "level"}
        r["busy"] = r["frame"] - r["idle"]
        r.setdefault("bottom", 0.0)   # results from before the bottom zone (STAGE7.md)
        if all(c in r for c in C3D_COLS):   # a build from before the plan B zones has c3d_wait only
            r["present total"] = r["present"] + sum(r[c] for c in C3D_COLS)
        runs[int(kv["level"])].append(r)
    return runs


def mean(xs):
    return sum(xs) / len(xs)


def main(paths):
    data = [(p, load(p)) for p in paths]
    cols = list(COLS)
    if any(c in r for _, runs in data for rs in runs.values() for r in rs for c in C3D_COLS):
        i = cols.index("present") + 1
        cols[i:i] = ["present total"] + C3D_COLS
    print("| results | level | runs | " + " | ".join(cols) + " | frame spread |")
    print("| --- " * (len(cols) + 4) + "|")
    for path, runs in data:
        name = path.rsplit("/", 1)[-1].removesuffix(".txt")
        for level in sorted(runs):
            rs = runs[level]
            # "-" for a column the file does not have (no c3d zones: a software or older log)
            cells = [f"{mean([r[c] for r in rs]):.2f}" if all(c in r for r in rs) else "-" for c in cols]
            frames = [r["frame"] for r in rs]
            spread = f"{min(frames):.2f}-{max(frames):.2f}"
            print(f"| {name} | {level} | {len(rs)} | " + " | ".join(cells) + f" | {spread} |")
    if len(data) > 1:
        base_name, base = data[0]
        print()
        print("| results | level | busy ms | vs " + base_name.rsplit('/', 1)[-1].removesuffix('.txt') + " | fps |")
        print("| --- | --- | --- | --- | --- |")
        for path, runs in data[1:]:
            name = path.rsplit("/", 1)[-1].removesuffix(".txt")
            for level in sorted(runs):
                if level not in base:
                    continue
                f0 = mean([r["busy"] for r in base[level]])
                f1 = mean([r["busy"] for r in runs[level]])
                fps = mean([r["fps"] for r in runs[level]])
                print(f"| {name} | {level} | {f1:.2f} | {100 * (f1 - f0) / f0:+.1f}% | {fps:.2f} |")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
