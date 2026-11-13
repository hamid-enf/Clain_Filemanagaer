#!/usr/bin/env python3
"""Differential: library decode vs ffmpeg decode.
   cmp.py <file> [ch]  -- aligns with a lag search and reports maxdiff/excess."""
import sys, subprocess, numpy as np, imageio_ffmpeg
FF = imageio_ffmpeg.get_ffmpeg_exe()

def lib(path, seek=None):
    p = subprocess.run(["/home/user/aud5/audtool", "s16", path] + ([seek] if seek else []),
                       capture_output=True, cwd="/")
    return np.frombuffer(p.stdout, dtype="<i2").astype(np.int32)

def ref(path):
    p = subprocess.run([FF, "-loglevel", "error", "-i", path, "-f", "s16le", "-acodec",
                        "pcm_s16le", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype="<i2").astype(np.int32)

for path in sys.argv[1:]:
    ch = 1
    L = lib(path)
    R = ref(path)
    if len(L) == 0 or len(R) == 0:
        print(f"{path}: lib={len(L)} ffmpeg={len(R)}  EMPTY"); continue
    best = (0, 1e18)
    seg = R[:4000] if len(R) >= 4000 else R
    for lag in range(0, 12000, 1):
        if lag + len(seg) > len(L): break
        d = np.abs(L[lag:lag+len(seg)] - seg).mean()
        if d < best[1]: best = (lag, d)
    lag = best[0]
    n = min(len(L) - lag, len(R))
    md = np.abs(L[lag:lag+n] - R[:n]).max() if n > 0 else -1
    print(f"{path.split('/')[-1]:22s} lib={len(L):7d} ffmpeg={len(R):7d} lag={lag:5d} "
          f"maxdiff={md:3d} excess={len(L)-len(R)-lag:5d}")
