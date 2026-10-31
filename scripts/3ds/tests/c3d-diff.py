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
#       With --colour CLASS[,CLASS] (M2, revised gate, design 7 "M2 specification", "Colour metric and tolerance"):
#                [--colour CLASS[,CLASS] --nonedge-pct 0.1 --within 98 --tol 2 --mean 1.0 --min-checks N]
#       per listed class, over the C3D checks of the run: non-edge coverage mismatches summed / A's covered
#       pixels summed <= --nonedge-pct %; neighbourhood within % (sum of within_nb over the sum of common;
#       a line's within_nb count is reconstructed as common x within_nb%, printed with 2 decimals, so each
#       count is rounded to the nearest pixel) >= --within; neighbourhood mean (sum of common x mean_nb over
#       sum of common) <= --mean. The strict figures (Delta to A's own pixel) are printed, informative.
#       Plus every --coverage failure rule (missing summary, dropped frame, a discarded check of a listed
#       class, sentinel > 0, fewer than N compared checks of a listed class), and: the "[c3d] debug-c3d:"
#       line must show `fullbright 1` and not `textures 0` (colour is meaningless otherwise), and the DIFF
#       lines' "(tol T)" must equal --tol. Per-check worst figures (lowest within_nb, highest mean_nb and max_nb) are
#       printed. Needs the lines of a build with the neighbourhood fields (` within_nb=` ...).
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
            "cov_a": int(kv["covered"].split("/")[0]), "common": int(kv["common"]),
            "within_tol": float(kv["within_tol"].rstrip("%")),
            "tol": int(re.search(r"\(tol (\d+)\)", text).group(1)),
            "within_nb": float(kv["within_nb"].rstrip("%")) if "within_nb" in kv else None,
            "mean_nb": float(kv["mean_nb"]) if "mean_nb" in kv else None,
            "max_nb": int(kv["max_nb"]) if "max_nb" in kv else None,
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


def cmd_log(path, expect_zero, coverage=None, band_pct=2.0, min_checks=1, colour=None, colour_opts=None):
    rows, other, summary, dropped, dbg = [], [], None, [], None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "[c3d] DIFF summary" in line:
                summary = line.strip()
            elif "[c3d] debug-c3d:" in line and "fullbright" in line:
                dbg = line.strip()
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
    if colour:
        if colour_check(colour, colour_opts, rows, other, summary, n_dropped, dbg, min_checks):
            failed = True
    return 1 if failed else 0


def colour_check(colour, o, rows, other, summary, n_dropped, dbg, min_checks):
    """The M2 colour gate (revised, neighbourhood rule). Prints the figures; returns True on failure."""
    problems = []
    c3d_rows = [r for r in rows if r["c3d"] and r["class"] in colour]
    print("%-8s %6s %10s %10s %9s %9s %9s %9s %7s" % (
        "class", "checks", "non-edge", "covered_A", "within_nb", "mean_nb", "strict_in", "strict_mn", "max_nb"))
    for c in colour:
        rs = [r for r in c3d_rows if r["class"] == c]
        if len(rs) < min_checks:
            problems.append("class %s: %d compared check(s), fewer than --min-checks %d" % (c, len(rs), min_checks))
        nd = [x for x in other if x["state"] == "discarded" and x["class"] == c]
        if nd:
            problems.append("class %s: %d discarded check(s)" % (c, len(nd)))
        if any(r["within_nb"] is None for r in rs):
            problems.append("class %s: DIFF lines without within_nb / mean_nb (a build before M2 part 5)" % c)
            continue
        common = sum(r["common"] for r in rs)
        cov_a = sum(r["cov_a"] for r in rs)
        non = sum(r["cov_non"] for r in rs)
        w_nb = sum(round(r["common"] * r["within_nb"] / 100.0) for r in rs)
        w_st = sum(round(r["common"] * r["within_tol"] / 100.0) for r in rs)
        s_nb = sum(r["common"] * r["mean_nb"] for r in rs)
        s_st = sum(r["common"] * r["mean"] for r in rs)
        non_pct = 100.0 * non / cov_a if cov_a else (0.0 if not non else float("inf"))
        in_pct = 100.0 * w_nb / common if common else 100.0
        mean_nb = s_nb / common if common else 0.0
        print("%-8s %6d %10s %10d %8.3f%% %9.3f %8.3f%% %9.3f %7d" % (
            c, len(rs), "%d (%.3f%%)" % (non, non_pct), cov_a, in_pct, mean_nb,
            100.0 * w_st / common if common else 100.0, s_st / common if common else 0.0,
            max((r["max_nb"] for r in rs), default=0)))
        if rs:
            lo = min(rs, key=lambda r: r["within_nb"])
            hi = max(rs, key=lambda r: r["mean_nb"])
            print("  worst per check: within_nb %.2f%% (frame %d), mean_nb %.2f (frame %d), max_nb %d (frame %d), non-edge %d (frame %d)" % (
                lo["within_nb"], lo["frame"], hi["mean_nb"], hi["frame"],
                max(r["max_nb"] for r in rs), max(rs, key=lambda r: r["max_nb"])["frame"],
                max(r["cov_non"] for r in rs), max(rs, key=lambda r: r["cov_non"])["frame"]))
        if non_pct > o["nonedge"]:
            problems.append("class %s: non-edge coverage mismatches %.3f%% of A's covered, over %.3f%%" % (c, non_pct, o["nonedge"]))
        if in_pct < o["within"]:
            problems.append("class %s: within (neighbourhood) %.3f%%, under %.3f%%" % (c, in_pct, o["within"]))
        if mean_nb > o["mean"]:
            problems.append("class %s: neighbourhood mean %.3f, over %.3f" % (c, mean_nb, o["mean"]))
    for r in c3d_rows:
        if r["tol"] != o["tol"]:
            problems.append("class %s frame %d: DIFF line has (tol %d), --tol is %d" % (r["class"], r["frame"], r["tol"], o["tol"]))
            break
    if dbg is None:
        problems.append("no [c3d] debug-c3d: line (cannot confirm fullbright 1 / textures)")
    else:
        if not re.search(r"\bfullbright 1\b", dbg):
            problems.append("debug-c3d line lacks `fullbright 1`")
        if re.search(r"\btextures 0\b", dbg):
            problems.append("debug-c3d line has `textures 0` (flat colour: the colour figures mean nothing)")
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
        print("c3d-diff: FAIL (--colour)")
    return bool(problems)


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
        opts = {"--coverage": None, "--band-pct": "2", "--min-checks": "1", "--colour": None,
                "--nonedge-pct": "0.1", "--within": "98", "--tol": "2", "--mean": "1.0"}
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
            colour_opts = {"nonedge": float(opts["--nonedge-pct"]), "within": float(opts["--within"]),
                           "tol": int(opts["--tol"]), "mean": float(opts["--mean"])}
        except ValueError:
            print("c3d-diff: --band-pct, --nonedge-pct, --within, --mean need numbers; --min-checks, --tol integers",
                  file=sys.stderr)
            return 2
        colour = [c for c in opts["--colour"].split(",") if c] if opts["--colour"] else None
        coverage = [c for c in opts["--coverage"].split(",") if c] if opts["--coverage"] else None
        return cmd_log(argv[2], "--expect-zero" in flags, coverage, band_pct, min_checks, colour, colour_opts)
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
    print("usage: c3d-diff.py log LOG [--expect-zero] [--coverage CLASS[,CLASS] --band-pct P --min-checks N]\n"
          "                         [--colour CLASS[,CLASS] --nonedge-pct P --within P --tol T --mean M] "
          "| bmp A.bmp B.bmp [--class C]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
