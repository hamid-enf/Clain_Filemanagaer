#!/usr/bin/env python3
"""Host-only deterministic alias differential for Path_Join; requires a C compiler."""
import ctypes
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
rng = random.Random(20261110)
with tempfile.TemporaryDirectory(prefix='fm_path_') as d:
    lib = Path(d) / 'path.so'
    subprocess.run([*shlex.split(os.environ.get('CC', 'gcc')), '-shared', '-fPIC', '-O2',
                    '-DFM_HOST_BUILD=1', '-Iinclude', 'src/fm_path.c', '-o', str(lib)],
                   cwd=ROOT, check=True)
    join = ctypes.CDLL(str(lib)).Path_Join
    join.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    join.restype = ctypes.c_int
    for i in range(30000):
        buf = ctypes.create_string_buffer(512)
        addr = ctypes.addressof(buf)
        if i % 2:
            b = ''.join(rng.choice('abc/') for _ in range(rng.randint(1, 80))).encode()
            buf.value = b
            aoff, boff = rng.randint(0, len(b)), 0
        else:
            a = ''.join(rng.choice('abc/') for _ in range(rng.randint(1, 70))).encode()
            b = ''.join(rng.choice('abc/') for _ in range(rng.randint(1, 70))).encode()
            boff, aoff = 0, rng.randint(len(b) + 1, 100)
            ctypes.memmove(addr + boff, b + b'\0', len(b) + 1)
            ctypes.memmove(addr + aoff, a + b'\0', len(a) + 1)
        a, b = ctypes.string_at(addr + aoff), ctypes.string_at(addr + boff)
        out = rng.choice([aoff, boff])
        trimmed = b.lstrip(b'/')
        want = b if not a else a if not b or not trimmed else a + (b'' if a.endswith(b'/') else b'/') + trimmed
        result = join(addr + aoff, addr + boff, addr + out, 512 - out)
        actual = ctypes.string_at(addr + out)
        if result or actual != want:
            raise AssertionError(f'case={i} r={result} a={a!r} b={b!r} got={actual!r} want={want!r}')
print('30,000 deterministic Path_Join alias cases PASS (seed 20261110)')
