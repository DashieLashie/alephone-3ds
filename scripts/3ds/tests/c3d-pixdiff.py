#!/usr/bin/env python3
"""Pixel tools for the stage 10 citro3d milestones (docs/3ds/CITRO3D_DESIGN.md).

Python stdlib only; run as `python3 -I scripts/3ds/tests/c3d-pixdiff.py ...`.
Reads 8-bit PNG (RGB or RGBA, as `sips` writes the Azahar captures) and the 24-bit
BMPs the game saves (screen-*.bmp).

  c3d-pixdiff.py diff A B [--tol N]
      Compares two images of the same size. Prints the size, the number of pixels that
      differ, the maximum difference per channel, and the share of pixels whose every
      channel differs by at most N (default 1). Exit 0 if every channel of every pixel
      differs by at most N, else 1 (so "--tol 0" is byte identity).

  c3d-pixdiff.py cal FILE...
      Calibration captures (debug-c3d "calibrate 1", design 5.1 test): the screen is
      one colour R = counter & 255, G = counter >> 8, B = 0xA5. Prints the colour of
      the image, whether the image is uniform (a mixed image means two frames shown at
      once), and the decoded counter.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG: ' + path)
    pos = 8
    idat = b''
    width = height = depth = ctype = None
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError('unsupported PNG (need 8-bit RGB/RGBA, not interlaced): ' + path)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif ft != 0:
            raise ValueError('bad PNG filter %d' % ft)
        rows.append(line)
        prev = line
    return width, height, [bytes(r[i] for i in range(len(r)) if bpp == 3 or i % 4 != 3) for r in rows]


def read_bmp(path):
    data = open(path, 'rb').read()
    if data[:2] != b'BM':
        raise ValueError('not a BMP: ' + path)
    offset, = struct.unpack('<I', data[10:14])
    width, height, planes, bits = struct.unpack('<iiHH', data[18:30])
    if bits != 24:
        raise ValueError('need a 24-bit BMP: ' + path)
    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    rows = []
    for y in range(height):
        r = data[offset + y * stride:offset + y * stride + width * 3]
        # BGR -> RGB
        rows.append(bytes(c for i in range(0, len(r), 3) for c in (r[i + 2], r[i + 1], r[i])))
    if not top_down:
        rows.reverse()
    return width, height, rows


def read_image(path):
    return read_bmp(path) if path.lower().endswith('.bmp') else read_png(path)


def cmd_diff(a, b, tol):
    wa, ha, ra = read_image(a)
    wb, hb, rb = read_image(b)
    if (wa, ha) != (wb, hb):
        print('SIZE MISMATCH %dx%d vs %dx%d' % (wa, ha, wb, hb))
        return 1
    differ = within = 0
    mx = [0, 0, 0]
    for y in range(ha):
        la, lb = ra[y], rb[y]
        if la == lb:
            within += wa
            continue
        for x in range(wa):
            pa, pb = la[3 * x:3 * x + 3], lb[3 * x:3 * x + 3]
            if pa != pb:
                differ += 1
                d = [abs(pa[i] - pb[i]) for i in range(3)]
                for i in range(3):
                    mx[i] = max(mx[i], d[i])
                if max(d) <= tol:
                    within += 1
            else:
                within += 1
    total = wa * ha
    print('%dx%d: %d pixels differ, max per channel R%d G%d B%d, %.4f%% within %d' %
          (wa, ha, differ, mx[0], mx[1], mx[2], 100.0 * within / total, tol))
    return 0 if max(mx) <= tol else 1


def cmd_cal(files):
    status = 0
    for f in files:
        w, h, rows = read_image(f)
        first = rows[0][0:3]
        uniform = all(r == first * w for r in rows)
        colours = set()
        if not uniform:
            for r in rows:
                for x in range(w):
                    colours.add(r[3 * x:3 * x + 3])
        n = first[0] | (first[1] << 8)
        print('%s: %dx%d %s colour R%d G%d B%d => counter %d%s' %
              (f, w, h, 'uniform' if uniform else 'MIXED(%d colours)' % len(colours),
               first[0], first[1], first[2], n, '' if first[2] == 0xA5 else ' (B != 0xA5)'))
        if not uniform:
            status = 1
    return status


def main(argv):
    if len(argv) >= 4 and argv[1] == 'diff':
        tol = 1
        args = argv[2:]
        if '--tol' in args:
            i = args.index('--tol')
            tol = int(args[i + 1])
            del args[i:i + 2]
        return cmd_diff(args[0], args[1], tol)
    if len(argv) >= 3 and argv[1] == 'cal':
        return cmd_cal(argv[2:])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
