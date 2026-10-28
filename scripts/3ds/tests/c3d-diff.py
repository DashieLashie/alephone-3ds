#!/usr/bin/env python3
# c3d-diff.py - reads the diff harness output of the citro3d renderer (M0 and M1, docs/3ds/CITRO3D_DESIGN.md
# 8.1, 8.3 and section 7 "M1 specification", "Diff line and tolerance").
# Standard library only; run it as `python3 -I`, it reads untrusted files.
#
#   python3 -I scripts/3ds/tests/c3d-diff.py log LOG [--expect-zero]
#                [--coverage CLASS[,CLASS] --band-pct P --min-checks N]
#       Tabulates the "[c3d] DIFF frame=... class=..." lines of a game log per class: checks, differ total,
#       max of max, mean of the means, cov_mismatch sums (edge/non-edge), the band (` band=N`, optional) and
#       the worst edge/band ratio; prints the "DIFF summary" line and the dropped frames, and tabulates the
#       "DIFF frame=F class=C discarded (reason)" and "... skipped (reason)" lines by reason.
#       With --expect-zero (M0's SW-vs-SW test) the exit status is 1 if any check has differ > 0, if there is
#       no DIFF line at all, if a DIFF summary is missing, or if frames were dropped (dropped > 0 in the
#       summary, or a "DIFF frame=N dropped" line). Lines of C3D checks (` b=c3d`) are skipped there.
#       With --coverage (M1: coverage only, colour not judged) the C3D checks (` b=c3d`) of the listed classes
#       are judged and the exit status is 1 on any of: a compared check with non-edge mismatches > 0; the
#       edge mismatches summed over those checks > P % of their summed band (--band-pct, default 2); a
#       missing summary; a dropped frame; a discarded check of a listed class; sentinel > 0 on any DIFF
#       line; fewer than N compared checks of a listed class (--min-checks, default 1). A skipped check is
#       neither compared nor a failure; the table shows why.
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
    """A compared check: a dict; a discarded / skipped check: a dict with "state" and "reason"; else None."""
    m = line.find("[c3d] DIFF ")
    if m < 0:
        return None
    text = line[m + len("[c3d] DIFF "):]
    kv = dict(KV.findall(text))
    if text.startswith("summary") or "class" not in kv or "frame" not in kv:
        return None
    mm = re.search(r"\b(discarded|skipped) \(([^)]*)\)", text)
    if mm:
        try:
            return {"frame": int(kv["frame"]), "class": kv["class"], "state": mm.group(1), "reason": mm.group(2)}
        except ValueError:
            return None
    try:
        cm = kv["cov_mismatch"].split("/")
        return {
            "frame": int(kv["frame"]), "class": kv["class"], "state": "compared",
            "cov_edge": int(cm[0]), "cov_non": int(cm[1]),
            "mean": float(kv["mean"]), "max": int(kv["max"]), "differ": int(kv["differ"]),
            "band": int(kv["band"]) if "band" in kv else None,
            "c3d": kv.get("b") == "c3d",
            "excluded": int(kv["excluded"]) if "excluded" in kv else 0,
            "sentinel": int(kv["sentinel"]) if "sentinel" in kv else 0,
            "ties": int(kv["ties"]) if "ties" in kv else 0,
            "tie_mismatch": int(kv["tie_mismatch"]) if "tie_mismatch" in kv else 0,
        }
    except (KeyError, ValueError, IndexError):
        return None


def cmd_log(path, expect_zero, coverage=None, band_pct=2.0, min_checks=1):
    rows, other, summary, dropped = [], [], None, []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "[c3d] DIFF summary" in line:
                summary = line.strip()
            elif "[c3d] DIFF frame=" in line and " dropped" in line:
                dropped.append(line.strip())
            else:
                r = parse_diff_line(line)
                if r and r["state"] == "compared":
                    rows.append(r)
                elif r:
                    other.append(r)
    if expect_zero:   # M0's SW-vs-SW test: the C3D checks are judged by --coverage, not here
        rows = [r for r in rows if not r["c3d"]]
    classes = []
    for r in rows:
        if r["class"] not in classes:
            classes.append(r["class"])
    print("%-10s %6s %12s %6s %10s %12s %10s %9s %8s %7s" % (
        "class", "checks", "differ", "max", "mean(mean)", "cov_mismatch", "band", "edge/band", "ties", "tie_mm"))
    bad = 0
    for c in classes:
        rs = [r for r in rows if r["class"] == c]
        d = sum(r["differ"] for r in rs)
        bad += d
        bands = [r["band"] for r in rs if r["band"] is not None]
        worst = max((r["cov_edge"] / r["band"] for r in rs if r["band"]), default=None)
        print("%-10s %6d %12d %6d %10.3f %6d/%-6d %10s %9s %8d %7d" % (
            c, len(rs), d, max(r["max"] for r in rs), sum(r["mean"] for r in rs) / len(rs),
            sum(r["cov_edge"] for r in rs), sum(r["cov_non"] for r in rs),
            sum(bands) if bands else "-", "%.2f%%" % (100.0 * worst) if worst is not None else "-",
            sum(r["ties"] for r in rs), sum(r["tie_mismatch"] for r in rs)))
    print("checks: %d, frames: %d, differ total: %d" % (len(rows), len({r["frame"] for r in rows}), bad))
    for state in ("discarded", "skipped"):
        by = {}
        for o in other:
            if o["state"] == state:
                by.setdefault((o["class"], o["reason"]), []).append(o["frame"])
        for (cls, reason), frames in sorted(by.items()):
            print("%s: class %s (%s): %d" % (state, cls, reason, len(frames)))
    print(summary or "no DIFF summary line")
    for d in dropped:
        print(d)
    m = re.search(r"dropped=(\d+)", summary or "")
    n_dropped = max(len(dropped), int(m.group(1)) if m else 0)
    failed = False
    if expect_zero and (bad or not rows or not summary or n_dropped):
        if n_dropped:
            print("c3d-diff: %d dropped frame(s)" % n_dropped)
        print("c3d-diff: FAIL (--expect-zero)")
        failed = True
    if coverage:
        problems = []
        c3d_rows = [r for r in rows if r["c3d"] and r["class"] in coverage]
        for c in coverage:
            rs = [r for r in c3d_rows if r["class"] == c]
            if len(rs) < min_checks:
                problems.append("class %s: %d compared check(s), fewer than --min-checks %d" % (c, len(rs), min_checks))
            for r in rs:
                if r["cov_non"] > 0:
                    problems.append("class %s frame %d: %d non-edge coverage mismatch(es)" % (c, r["frame"], r["cov_non"]))
            nd = [o for o in other if o["state"] == "discarded" and o["class"] == c]
            if nd:
                problems.append("class %s: %d discarded check(s)" % (c, len(nd)))
        edge = sum(r["cov_edge"] for r in c3d_rows)
        band = sum(r["band"] or 0 for r in c3d_rows)
        pct = 100.0 * edge / band if band else (0.0 if not edge else float("inf"))
        print("coverage %s: edge mismatches %d of band %d = %.3f%% (limit %.3f%%), %d compared check(s)" % (
            ",".join(coverage), edge, band, pct, band_pct, len(c3d_rows)))
        print("coverage %s: tie pixels %d, tie mismatches %d (inside the band and the edge count above)" % (
            ",".join(coverage), sum(r["ties"] for r in c3d_rows), sum(r["tie_mismatch"] for r in c3d_rows)))
        if pct > band_pct:
            problems.append("edge mismatches %.3f%% of the band, over %.3f%%" % (pct, band_pct))
        if not summary:
            problems.append("no DIFF summary line")
        if n_dropped:
            problems.append("%d dropped frame(s)" % n_dropped)
        sent = sum(r["sentinel"] for r in rows)
        if sent:
            problems.append("sentinel > 0 (%d pixel(s) the readback never wrote)" % sent)
        for p_ in problems:
            print("c3d-diff: %s" % p_)
        if problems:
            print("c3d-diff: FAIL (--coverage)")
            failed = True
    return 1 if failed else 0


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
        rest = argv[3:]
        opts = {"--coverage": None, "--band-pct": "2", "--min-checks": "1"}
        flags = set()
        i = 0
        while i < len(rest):
            if rest[i] in opts and i + 1 < len(rest):
                opts[rest[i]] = rest[i + 1]
                i += 2
            elif rest[i] == "--expect-zero":
                flags.add(rest[i])
                i += 1
            else:
                print("c3d-diff: unknown or incomplete option %s" % rest[i], file=sys.stderr)
                return 2
        try:
            band_pct, min_checks = float(opts["--band-pct"]), int(opts["--min-checks"])
        except ValueError:
            print("c3d-diff: --band-pct needs a number and --min-checks an integer", file=sys.stderr)
            return 2
        coverage = [c for c in opts["--coverage"].split(",") if c] if opts["--coverage"] else None
        return cmd_log(argv[2], "--expect-zero" in flags, coverage, band_pct, min_checks)
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
    print("usage: c3d-diff.py log LOG [--expect-zero] [--coverage CLASS[,CLASS] --band-pct P --min-checks N] "
          "| bmp A.bmp B.bmp [--class C]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
