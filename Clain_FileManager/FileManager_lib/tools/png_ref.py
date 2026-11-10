#!/usr/bin/env python3
"""Reference PNG decoder for the test vectors (python3, stdlib only).

Decodes every colour type / bit depth / Adam7 PNG to RGB888 the way the
library documents it (alpha dropped, 16-bit rounded like libpng's
png_set_scale_16, sub-byte grey stretched to full scale) and prints the
CRC-32 table used by test_png_formats() in test/main.c.

When a <name>.rgb next to <name>.png exists (ImageMagick's own decode,
see gen_png_vectors.sh) the two are compared: "diff 0" means ImageMagick
agrees byte for byte.  The 16-bit files differ by at most 1 because
ImageMagick 6 floors v/257 instead of rounding.

    python3 tools/png_ref.py test/vectors/png
"""
import glob
import struct
import sys
import zlib

A7 = list(zip([0, 4, 0, 2, 0, 1, 0], [0, 0, 4, 0, 2, 0, 1],
              [8, 8, 4, 4, 2, 2, 1], [8, 8, 8, 4, 4, 2, 2]))


def s8(b):
    v = (b[0] << 8) | b[1]
    return (v * 255 + 32895) >> 16


def decode(d):
    w, h, dep, col, _, _, il = struct.unpack('>IIBBBBB', d[16:29])
    p = 8
    idat = b''
    pal = None
    while p < len(d):
        n, = struct.unpack('>I', d[p:p + 4])
        t = d[p + 4:p + 8]
        body = d[p + 8:p + 8 + n]
        assert zlib.crc32(t + body) == struct.unpack('>I', d[p + 8 + n:p + 12 + n])[0]
        p += 12 + n
        if t == b'IDAT':
            idat += body
        if t == b'PLTE':
            pal = body
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[col]
    bits = ch * dep
    bpp = max(1, bits // 8)
    out = [[None] * w for _ in range(h)]
    pos = 0
    for (x0, y0, dx, dy) in ([(0, 0, 1, 1)] if il == 0 else A7):
        pw = (w - x0 + dx - 1) // dx if w > x0 else 0
        ph = (h - y0 + dy - 1) // dy if h > y0 else 0
        if pw == 0 or ph == 0:
            continue
        rb = (pw * bits + 7) // 8
        prev = bytearray(rb)
        for py in range(ph):
            f = raw[pos]
            row = bytearray(raw[pos + 1:pos + 1 + rb])
            pos += 1 + rb
            for i in range(rb):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                if f == 1:
                    row[i] = (row[i] + a) & 255
                elif f == 2:
                    row[i] = (row[i] + b) & 255
                elif f == 3:
                    row[i] = (row[i] + ((a + b) >> 1)) & 255
                elif f == 4:
                    pp = a + b - c
                    pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                    pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    row[i] = (row[i] + pr) & 255
            prev = row
            for px in range(pw):
                if dep < 8:
                    bit = px * dep
                    v = (row[bit >> 3] >> (8 - dep - (bit & 7))) & ((1 << dep) - 1)
                    if col == 0:
                        v *= 255 // ((1 << dep) - 1)
                        rgb = (v, v, v)
                    else:
                        rgb = tuple(pal[v * 3:v * 3 + 3])
                else:
                    s = row[px * (bits // 8):(px + 1) * (bits // 8)]
                    vals = [s8(s[i:i + 2]) for i in range(0, len(s), 2)] if dep == 16 else list(s)
                    if col in (2, 6):
                        rgb = tuple(vals[:3])
                    elif col == 3:
                        rgb = tuple(pal[vals[0] * 3:vals[0] * 3 + 3])
                    else:
                        rgb = (vals[0],) * 3
                out[y0 + py * dy][x0 + px * dx] = rgb
    assert pos == len(raw)
    return w, h, bytes(v for r in out for px in r for v in px)


def main():
    for f in sorted(glob.glob(sys.argv[1] + '/*.png')):
        w, h, rgb = decode(open(f, 'rb').read())
        name = f.split('/')[-1][:-4]
        try:
            ref = open(f[:-4] + '.rgb', 'rb').read()
            note = '   /* ImageMagick: max diff %d */' % max(abs(a - b) for a, b in zip(rgb, ref))
        except OSError:
            note = ''
        print('        { "%s", %du, %du, 0x%08Xu },%s' % (name, w, h, zlib.crc32(rgb), note))


if __name__ == '__main__':
    main()
