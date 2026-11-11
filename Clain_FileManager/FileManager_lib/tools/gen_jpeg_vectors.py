#!/usr/bin/env python3
"""
Still-JPEG test vectors for the layouts picojpeg did not have.

    test/vectors/jpeg_gray_h2v2.jpg    one component, sampling factors 2x2,
                                       37x21, restart interval 3.  The
                                       factors of a single component mean
                                       nothing (ITU T.81 A.2.2) and some
                                       encoders write 2x2; picojpeg refused
                                       everything but 1x1.
    test/vectors/jpeg_gray_h2v2.gray   ffmpeg's decode of it, 8-bit gray
    test/vectors/jpeg_422_33x17.jpg    4:2:2 the way ffmpeg writes it (luma
                                       2x2, chroma 1x2), odd width and height
    test/vectors/jpeg_444_9x17.jpg     4:4:4 the way ffmpeg writes it (every
                                       component 1x2)
    test/vectors/jpeg_*.rgb            the reference for those two: ffmpeg's
                                       decoded planes, converted to RGB here
                                       with the JFIF matrix and each chroma
                                       sample repeated.  (ffmpeg's own RGB
                                       output interpolates chroma when a
                                       dimension is odd, which a decoder that
                                       repeats it is 20 dB away from.)

The one-component file is written by the small baseline encoder below
(tables of T.81 Annex K): ffmpeg writes "gray" as three components.

    python3 tools/gen_jpeg_vectors.py          (FFMPEG=/path/to/ffmpeg)
"""
import math
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "test", "vectors")

ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
      12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
      58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
     14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
     18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
Q = [max(1, (q + 1) // 2) for q in Q]
DC_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_VAL = list(range(12))
AC_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d]
AC_VAL = [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa]
assert len(AC_VAL) == 162 and sum(AC_BITS) == 162


def codes(bits, vals):
    table = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            table[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return table


DC = codes(DC_BITS, DC_VAL)
AC = codes(AC_BITS, AC_VAL)


def pixel(x, y):
    """A picture with edges, a ramp and some texture."""
    v = 128 + 90 * math.sin(x * 0.35) * math.cos(y * 0.22) + (x * 3 + y * 5) % 23
    if (x // 5 + y // 7) % 4 == 0:
        v = 235
    if abs(x - y) < 2:
        v = 16
    return max(0, min(255, int(v)))


def encode_gray(out, w, h, samp, ri=0):
    """One-component baseline JPEG; `samp` is the sampling-factor byte,
    `ri` the restart interval in blocks (0 = none)."""
    bw = (w + 7) // 8
    bh = (h + 7) // 8
    cos = [[(math.sqrt(0.125) if u == 0 else 0.5) *
            math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)]
           for u in range(8)]
    state = {"acc": 0, "nb": 0}
    data = bytearray()

    def put(code, length):
        state["acc"] = (state["acc"] << length) | code
        state["nb"] += length
        while state["nb"] >= 8:
            b = (state["acc"] >> (state["nb"] - 8)) & 0xFF
            data.append(b)
            if b == 0xFF:
                data.append(0)
            state["nb"] -= 8
        state["acc"] &= (1 << state["nb"]) - 1

    def flush():
        if state["nb"]:
            put((1 << (8 - state["nb"])) - 1, 8 - state["nb"])

    def mag(v):
        s = abs(v).bit_length()
        return s, (v if v >= 0 else v + (1 << s) - 1)

    pred = 0
    n = 0
    for by in range(bh):
        for bx in range(bw):
            if ri and n and n % ri == 0:
                flush()
                data.extend(b"\xff" + bytes([0xD0 + ((n // ri - 1) & 7)]))
                pred = 0
            n += 1
            blk = [[pixel(min(bx * 8 + x, w - 1), min(by * 8 + y, h - 1)) - 128
                    for x in range(8)] for y in range(8)]
            tmp = [[sum(cos[u][x] * blk[y][x] for x in range(8))
                    for u in range(8)] for y in range(8)]
            f = [[sum(cos[v][y] * tmp[y][u] for y in range(8))
                  for u in range(8)] for v in range(8)]
            q = [int(round(f[ZZ[i] // 8][ZZ[i] % 8] / Q[ZZ[i]]))
                 for i in range(64)]
            s, bits = mag(q[0] - pred)
            pred = q[0]
            put(*DC[s])
            if s:
                put(bits, s)
            run = 0
            last = max([i for i in range(1, 64) if q[i]] or [0])
            for i in range(1, last + 1):
                if q[i] == 0:
                    run += 1
                    continue
                while run > 15:
                    put(*AC[0xF0])
                    run -= 16
                s, bits = mag(q[i])
                put(*AC[(run << 4) | s])
                put(bits, s)
                run = 0
            if last < 63:
                put(*AC[0])
    flush()
    jpg = bytearray(b"\xff\xd8")
    jpg += b"\xff\xdb" + struct.pack(">HB", 67, 0) + bytes(Q[ZZ[i]] for i in range(64))
    jpg += b"\xff\xc0" + struct.pack(">HBHHB", 11, 8, h, w, 1) + bytes([1, samp, 0])
    jpg += b"\xff\xc4" + struct.pack(">HB", 19 + 12, 0x00) + bytes(DC_BITS) + bytes(DC_VAL)
    jpg += b"\xff\xc4" + struct.pack(">HB", 19 + 162, 0x10) + bytes(AC_BITS) + bytes(AC_VAL)
    if ri:
        jpg += b"\xff\xdd" + struct.pack(">HH", 4, ri)
    jpg += b"\xff\xda" + struct.pack(">HB", 8, 1) + bytes([1, 0x00, 0, 63, 0])
    jpg += data + b"\xff\xd9"
    open(out, "wb").write(jpg)


def ffmpeg_path():
    p = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if p:
        return p
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        sys.exit("ffmpeg not found: set FFMPEG=/path/to/ffmpeg")


def planes_to_rgb(yuv, w, h, sx, sy):
    cw = (w + sx - 1) // sx
    ch = (h + sy - 1) // sy
    assert len(yuv) == w * h + 2 * cw * ch
    yp = yuv[:w * h]
    up = yuv[w * h:w * h + cw * ch]
    vp = yuv[w * h + cw * ch:]
    out = bytearray()

    def clamp(x):
        return 0 if x < 0 else 255 if x > 255 else int(x)

    for y in range(h):
        for x in range(w):
            lum = yp[y * w + x]
            u = up[(y // sy) * cw + x // sx] - 128
            v = vp[(y // sy) * cw + x // sx] - 128
            out += bytes((clamp(lum + 1.402 * v + 0.5),
                          clamp(lum - 0.344136 * u - 0.714136 * v + 0.5),
                          clamp(lum + 1.772 * u + 0.5)))
    return bytes(out)


def main():
    ff = ffmpeg_path()
    quiet = [ff, "-hide_banner", "-loglevel", "error", "-y"]
    made = []

    p = os.path.join(OUT, "jpeg_gray_h2v2.jpg")
    encode_gray(p, 37, 21, 0x22, 3)
    gray = subprocess.check_output(quiet + ["-i", p, "-f", "rawvideo",
                                            "-pix_fmt", "gray", "-"])
    assert len(gray) == 37 * 21
    open(p[:-4] + ".gray", "wb").write(gray)
    made += [p, p[:-4] + ".gray"]

    for name, fmt, w, h, sx, sy in (("jpeg_422_33x17", "yuvj422p", 33, 17, 2, 1),
                                    ("jpeg_444_9x17", "yuvj444p", 9, 17, 1, 1)):
        p = os.path.join(OUT, name + ".jpg")
        subprocess.check_call(quiet + [
            "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=1:duration=1",
            "-vf", "scale=%d:%d:flags=bilinear,format=%s" % (w, h, fmt),
            "-frames:v", "1", "-q:v", "3", "-bitexact", "-flags", "+bitexact",
            p])
        yuv = subprocess.check_output(quiet + ["-i", p, "-f", "rawvideo",
                                               "-pix_fmt", fmt, "-"])
        open(p[:-4] + ".rgb", "wb").write(planes_to_rgb(yuv, w, h, sx, sy))
        made += [p, p[:-4] + ".rgb"]

    for p in made:
        print("%-40s %6d bytes" % (os.path.relpath(p), os.path.getsize(p)))


if __name__ == "__main__":
    main()
