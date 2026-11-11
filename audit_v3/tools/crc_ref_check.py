#!/usr/bin/env python3
"""Compare /tmp/crc_bin output with zlib/binascii/struct."""
import sys
import zlib
import struct

bad = 0
n_buf = 0
last = None
for line in sys.stdin:
    f = line.split()
    if not f:
        continue
    if f[0] == "crc32_check":
        if int(f[1], 16) != zlib.crc32(b"123456789") & 0xFFFFFFFF:
            print("crc32 check value wrong:", f[1]); bad += 1
    elif f[0] == "crc16_check":
        if f[1].upper() != "29B1":
            print("crc16 check value wrong:", f[1]); bad += 1
    elif f[0] == "crc16modbus_check":
        if f[1].upper() != "4B37":
            print("crc16 modbus check value wrong:", f[1]); bad += 1
    elif f[0] == "ck8_123456789":
        x = 0
        for c in b"123456789":
            x ^= c
        if int(f[1], 16) != x:
            print("checksum8 wrong"); bad += 1
    elif f[0] == "ck32_123456789":
        if int(f[1], 16) != sum(b"123456789"):
            print("checksum32 wrong"); bad += 1
    elif f[0] == "buf":
        n = int(f[1])
        data = bytes(int(x, 16) for x in f[2:2 + n])
        last = data
        w32 = f[2 + n + 1]
        if int(w32, 16) != zlib.crc32(data) & 0xFFFFFFFF:
            print("crc32 mismatch for a %d-byte buffer: %s, zlib %08X" %
                  (n, w32, zlib.crc32(data) & 0xFFFFFFFF)); bad += 1
        c = 0xFFFF
        for b in data:
            c ^= b << 8
            for _ in range(8):
                c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        if int(f[2 + n + 3], 16) != c:
            print("crc16 ccitt-false mismatch at n=%d" % n); bad += 1
        c = 0xFFFF
        for b in data:
            c ^= b
            for _ in range(8):
                c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        if int(f[2 + n + 5], 16) != c:
            print("crc16 modbus mismatch at n=%d: %s want %04X" %
                  (n, f[2 + n + 5], c)); bad += 1
        n_buf += 1
    elif f[0] == "split":
        n, k, got = int(f[1]), int(f[2]), f[3]
        # chaining must give the same result as one pass over the whole
        # buffer: crc32(0, a) then crc32(that, b) == crc32(0, a+b)
        chained = zlib.crc32(last[k:], zlib.crc32(last[:k])) & 0xFFFFFFFF
        if int(got, 16) != chained:
            print("chained crc32 at n=%d k=%d: %s want %08X" %
                  (n, k, got, chained)); bad += 1
    elif f[0] == "binary_writes":
        want = (struct.pack("<B", 0xA5) +
                struct.pack("<H", 0x1234) + struct.pack(">H", 0x1234) +
                struct.pack("<I", 0xDEADBEEF) + struct.pack(">I", 0xDEADBEEF) +
                struct.pack("<Q", 0x0102030405060708) +
                struct.pack(">Q", 0x0102030405060708) +
                struct.pack("<h", -2) + struct.pack(">i", -2147483648) +
                struct.pack("<q", -(2 ** 63)) +
                struct.pack("<f", 1.5) + struct.pack(">f", 1.5) +
                struct.pack(">d", -2.5))
        got = bytes(int(x, 16) for x in f[1:-2])
        if got != want:
            print("binary writes differ")
            print("  got :", got.hex())
            print("  want:", want.hex())
            bad += 1
print("crc_ref_check: %d buffers, %d problems" % (n_buf, bad))
sys.exit(1 if bad else 0)
