#!/usr/bin/env python3
# M6 direct-vs-readback capture comparison (design 7, M6 specification, acceptance (2); Review log "M6 acceptance, capture gate").
# scripts/3ds/tests/c3d-capcmp.py A B [--within PCT]: within_1 >= PCT (default 99.9; 99.5 for captures with overlay text) and 0 pixels > 32.
import sys
limit = float(sys.argv[sys.argv.index("--within") + 1]) if "--within" in sys.argv else 99.9
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); b = Image.open(sys.argv[2]).convert('RGB')
assert a.size == b.size, (a.size, b.size)
pa, pb = a.load(), b.load(); W, H = a.size; over1 = over32 = 0; mx = 0; first = None
for y in range(H):
    for x in range(W):
        d = max(abs(p - q) for p, q in zip(pa[x, y], pb[x, y]))
        mx = max(mx, d)
        if d > 1:
            over1 += 1
            first = first or (x, y)
        if d > 32: over32 += 1
within = 100.0 * (W * H - over1) / (W * H)
ok = within >= limit and over32 == 0
print(f"{sys.argv[1].split('/')[-1]} vs {sys.argv[2].split('/')[-1]}: within_1={within:.3f}% over_1={over1} over_32={over32} max={mx} first={first} -> {'ok' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
