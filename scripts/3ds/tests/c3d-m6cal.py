#!/usr/bin/env python3
# M6 frame-indexed test (design 7, M6 specification "Calibration"): scripts/3ds/tests/c3d-m6cal.py DIR (screen-c3d-cal-N-top.png|bmp).
import sys, os, re
from PIL import Image
d = sys.argv[1]; ok = bad = 0
for f in sorted(os.listdir(d), key=lambda x: (len(x), x)):
    m = re.match(r'screen-c3d-cal-(\d+)-top\.(png|bmp)$', f)
    if not m: continue
    n = int(m.group(1)); im = Image.open(os.path.join(d, f)).convert('RGB'); px = im.load()
    W, H = im.size
    bg = px[200, 120]
    shown = None
    for c in (n, n - 1):
        if bg == (c & 0xFF, (c >> 8) & 0xFF, 0xA5): shown = c
    rows = [y for y in range(H) if px[200, y] == (255, 255, 255)]
    band = (rows[0], rows[-1]) if rows else None
    want = [8 * (c % 30) for c in (n, n - 1)]
    band_ok = shown is not None and band == (8 * (shown % 30), 8 * (shown % 30) + 7)
    def corner(x, y, want):
        return px[x, y] == want or (band and band[0] <= y <= band[1])
    corners = corner(4, 100 if False else 20, (255, 0, 0)) and corner(395, 20, (0, 255, 0)) and corner(4, 235, (0, 0, 255)) and corner(395, 235, (255, 255, 255))
    corners = (px[4, 4] == (255,0,0) or (band and band[0] <= 4 <= band[1])) and (px[395, 4] == (0,255,0) or (band and band[0] <= 4 <= band[1])) and px[4, 235] == (0,0,255) and px[395, 235] == (255,255,255)
    magenta = any(px[x, y] == (255, 0, 255) for x in (0, 200, 399) for y in range(H))
    good = shown is not None and band_ok and corners and not magenta
    ok += good; bad += not good
    print(f"present {n}: shows {shown} ({'N' if shown == n else 'N-1' if shown == n - 1 else 'OTHER'}) bg={bg} band={band} corners={'ok' if corners else 'BAD'} magenta={magenta} -> {'ok' if good else 'FAIL'}")
print(f"cal: {ok} ok, {bad} failed")
