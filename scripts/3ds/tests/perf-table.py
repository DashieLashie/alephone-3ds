#!/usr/bin/env python3
"""Summarise perf-bench.sh results (docs/3ds/PERF.md) as Markdown tables.

    python3 -I scripts/3ds/tests/perf-table.py build/perf/baseline.txt [build/perf/other.txt ...]

One table row per file and level: the mean of its runs (ms per frame), and
the spread of the frame time across runs. With two or more files, a last
table compares each file's frame time with the first file's, level by level.
"""
import re
import sys
from collections import defaultdict

COLS = ["fps", "frame", "max", "world", "render", "hud", "blit", "present", "idle", "other"]


def load(path):
    runs = defaultdict(list)
    for line in open(path, encoding="utf-8", errors="replace"):
        if "BENCH" not in line or "FAILED" in line:
            continue
        kv = dict(re.findall(r"(\w+)=([-\d.]+)", line))
        runs[int(kv["level"])].append({k: float(v) for k, v in kv.items() if k != "level"})
    return runs


def mean(xs):
    return sum(xs) / len(xs)


def main(paths):
    data = [(p, load(p)) for p in paths]
    print("| results | level | runs | " + " | ".join(COLS) + " | frame spread |")
    print("| --- " * (len(COLS) + 4) + "|")
    for path, runs in data:
        name = path.rsplit("/", 1)[-1].removesuffix(".txt")
        for level in sorted(runs):
            rs = runs[level]
            cells = [f"{mean([r[c] for r in rs]):.2f}" for c in COLS]
            frames = [r["frame"] for r in rs]
            spread = f"{min(frames):.2f}-{max(frames):.2f}"
            print(f"| {name} | {level} | {len(rs)} | " + " | ".join(cells) + f" | {spread} |")
    if len(data) > 1:
        base_name, base = data[0]
        print()
        print("| results | level | frame ms | vs " + base_name.rsplit('/', 1)[-1].removesuffix('.txt') + " | fps |")
        print("| --- | --- | --- | --- | --- |")
        for path, runs in data[1:]:
            name = path.rsplit("/", 1)[-1].removesuffix(".txt")
            for level in sorted(runs):
                if level not in base:
                    continue
                f0 = mean([r["frame"] for r in base[level]])
                f1 = mean([r["frame"] for r in runs[level]])
                fps = mean([r["fps"] for r in runs[level]])
                print(f"| {name} | {level} | {f1:.2f} | {100 * (f1 - f0) / f0:+.1f}% | {fps:.2f} |")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
