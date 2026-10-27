#!/usr/bin/env python3
# c3d-diff.py - reads the diff harness output of the citro3d renderer (M0, docs/3ds/CITRO3D_DESIGN.md 8.1).
# Standard library only; run it as `python3 -I`, it reads untrusted files.
#
#   python3 -I scripts/3ds/tests/c3d-diff.py log LOG [--expect-zero]
#       Tabulates the "[c3d] DIFF frame=... class=..." lines of a game log per class: checks, differ total,
#       max of max, mean of the means, cov_mismatch sums (edge/non-edge); prints the "DIFF summary" line and
#       the dropped frames. With --expect-zero the exit status is 1 if any check has differ > 0, if there is
#       no DIFF line at all, if a DIFF summary is missing, or if frames were dropped (dropped > 0 in the
#       summary, or a "DIFF frame=N dropped" line).
#
#   python3 -I scripts/3ds/tests/c3d-diff.py bmp A.bmp B.bmp [--class C]
#       Reads two 32-bit BMPs (c3d-diff-<frame>-<class>-{sw,c3d}.bmp) and recomputes the metric of
#       core/diff.cpp (tolerance 0): coverage TopByte (alpha byte != 0), or "pixel != clear" for the class
#       tinted (clear 0xFF808080) and static (clear 0); then prints a DIFF-like line.
#       A truncated or garbled BMP, or a missing file, is a clean error message and exit status 2.
import re
import struct
import sys

KV = re.compile(r"(\w+)=(\S+)")


def parse_diff_line(line):
    m = line.find("[c3d] DIFF ")
    if m < 0:
        return None
    text = line[m + len("[c3d] DIFF "):]
    kv = dict(KV.findall(text))
    if text.startswith("summary") or "class" not in kv:
        return None
    try:
        cm = kv["cov_mismatch"].split("/")
        return {
            "frame": int(kv["frame"]), "class": kv["class"],
            "cov_edge": int(cm[0]), "cov_non": int(cm[1]),
            "mean": float(kv["mean"]), "max": int(kv["max"]), "differ": int(kv["differ"]),
        }
    except (KeyError, ValueError, IndexError):
        return None


def cmd_log(path, expect_zero):
    rows, summary, dropped = [], None, []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "[c3d] DIFF summary" in line:
                summary = line.strip()
            elif "[c3d] DIFF frame=" in line and " dropped" in line:
                dropped.append(line.strip())
            else:
                r = parse_diff_line(line)
                if r:
                    rows.append(r)
    classes = []
    for r in rows:
        if r["class"] not in classes:
            classes.append(r["class"])
    print("%-10s %6s %12s %6s %10s %12s" % ("class", "checks", "differ", "max", "mean(mean)", "cov_mismatch"))
    bad = 0
    for c in classes:
        rs = [r for r in rows if r["class"] == c]
        d = sum(r["differ"] for r in rs)
        bad += d
        print("%-10s %6d %12d %6d %10.3f %6d/%-6d" % (
            c, len(rs), d, max(r["max"] for r in rs), sum(r["mean"] for r in rs) / len(rs),
            sum(r["cov_edge"] for r in rs), sum(r["cov_non"] for r in rs)))
    print("checks: %d, frames: %d, differ total: %d" % (len(rows), len({r["frame"] for r in rows}), bad))
    print(summary or "no DIFF summary line")
    for d in dropped:
        print(d)
    m = re.search(r"dropped=(\d+)", summary or "")
    n_dropped = max(len(dropped), int(m.group(1)) if m else 0)
    if expect_zero and (bad or not rows or not summary or n_dropped):
        if n_dropped:
            print("c3d-diff: %d dropped frame(s)" % n_dropped)
        print("c3d-diff: FAIL (--expect-zero)")
        return 1
    return 0


class BmpError(Exception):
    pass


def read_bmp(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise BmpError("%s: %s" % (path, e.strerror or e))
    if data[:2] != b"BM":
        raise BmpError("%s is not a BMP" % path)
    try:
        offset = struct.unpack_from("<I", data, 10)[0]
        hdr, w, h, planes, bpp, comp = struct.unpack_from("<IiiHHI", data, 14)
        if bpp != 32 or comp not in (0, 3) or w <= 0 or h == 0:
            raise BmpError("%s is not a 32-bit BMP (bpp %d, compression %d)" % (path, bpp, comp))
        rows = abs(h)
        if w * rows > 64 * 1024 * 1024:
            raise BmpError("%s: %dx%d is implausibly large" % (path, w, rows))
        px = list(struct.unpack_from("<%dI" % (w * rows), data, offset))   # little-endian: 0xAARRGGBB
    except struct.error:
        raise BmpError("%s is truncated or garbled (%d bytes)" % (path, len(data)))
    if h > 0:   # bottom-up: flip so that row 0 is the top
        px = [p for y in range(rows - 1, -1, -1) for p in px[y * w:(y + 1) * w]]
    return w, rows, px


def metric(w, h, a, b, cls):
    """Same as core/diff.cpp with tolerance 0. Returns a dict of the figures of the DIFF line."""
    if cls == "tinted":
        covered = lambda p: p != 0xFF808080
    elif cls == "static":
        covered = lambda p: p != 0
    else:
        covered = lambda p: (p >> 24) != 0
    cov = [covered(p) for p in a]
    m = dict(cov_a=0, cov_b=0, edge=0, non=0, common=0, within=0, sum_d=0, max_d=0, differ=0)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            ca, cb = cov[i], covered(b[i])
            m["cov_a"] += ca
            m["cov_b"] += cb
            m["differ"] += a[i] != b[i]
            edge = any((dx or dy) and 0 <= x + dx < w and 0 <= y + dy < h and cov[(y + dy) * w + x + dx] != ca
                       for dy in (-1, 0, 1) for dx in (-1, 0, 1))
            if ca != cb:
                m["edge" if edge else "non"] += 1
            elif ca and not edge:
                d = max(abs(((a[i] >> s) & 255) - ((b[i] >> s) & 255)) for s in (0, 8, 16))
                m["common"] += 1
                m["sum_d"] += d
                m["max_d"] = max(m["max_d"], d)
                m["within"] += d == 0
    return m


def cmd_bmp(pa, pb, cls):
    wa, ha, a = read_bmp(pa)
    wb, hb, b = read_bmp(pb)
    if (wa, ha) != (wb, hb):
        raise BmpError("sizes differ: %dx%d and %dx%d" % (wa, ha, wb, hb))
    m = metric(wa, ha, a, b, cls)
    n = m["common"]
    print("[c3d] DIFF frame=0 class=%s covered=%d/%d cov_mismatch=%d/%d common=%d within_tol=%.2f%% (tol 0) "
          "mean=%.2f max=%d differ=%d" % (cls, m["cov_a"], m["cov_b"], m["edge"], m["non"], n,
                                          100.0 * m["within"] / n if n else 100.0,
                                          m["sum_d"] / n if n else 0.0, m["max_d"], m["differ"]))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "log":
        return cmd_log(argv[2], "--expect-zero" in argv[3:])
    if len(argv) >= 4 and argv[1] == "bmp":
        cls = "frame"
        rest = argv[4:]
        if rest[:1] == ["--class"] and len(rest) >= 2:
            cls = rest[1]
        try:
            return cmd_bmp(argv[2], argv[3], cls)
        except BmpError as e:
            print("c3d-diff: %s" % e, file=sys.stderr)
            return 2
    print("usage: c3d-diff.py log LOG [--expect-zero] | bmp A.bmp B.bmp [--class C]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
