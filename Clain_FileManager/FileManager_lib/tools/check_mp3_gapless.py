#!/usr/bin/env python3
"""Optional host differential experiment; requires numpy, ffmpeg with libmp3lame.

Run from the library root: python3 tools/check_mp3_gapless.py /path/to/ffmpeg
It generates its fixtures in a temporary directory; it does not change the
library or the repository. This measures one encoder, not every MP3 encoder.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
FF = sys.argv[1] if len(sys.argv) > 1 else shutil.which("ffmpeg")
if not FF or not pathlib.Path(FF).is_file():
    raise SystemExit("Pass a path to an ffmpeg executable with libmp3lame")

subprocess.run(["make", "-s", "-j2", "libfilemanager.a"], cwd=ROOT, check=True)
with tempfile.TemporaryDirectory(prefix="fm_mp3_gapless_") as d:
    d = pathlib.Path(d)
    cc = os.environ.get("CC", "gcc")
    subprocess.run([cc, "-DFM_HOST_BUILD=1", "-Iinclude", "-Ibackends", "-Isrc",
                    "tools/mp3_gapless_probe.c", "libfilemanager.a", "-lm",
                    "-lpthread", "-o", str(d / "probe")], cwd=ROOT, check=True)
    noise = np.random.default_rng(10471).integers(-16000, 16001, 44100 * 4,
                                                  dtype=np.int16)
    with wave.open(str(d / "noise.wav"), "wb") as out:
        out.setparams((1, 2, 44100, 0, "NONE", ""))
        out.writeframes(noise.astype("<i2").tobytes())
    for name, opts in (("cbr", ["-b:a", "128k"]), ("vbr", ["-q:a", "3"])):
        mp3, ref, lib = (d / (name + ext) for ext in (".mp3", ".ref", ".lib"))
        subprocess.run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i",
                        str(d / "noise.wav"), "-codec:a", "libmp3lame", *opts,
                        str(mp3)], check=True)
        subprocess.run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i",
                        str(mp3), "-f", "s16le", "-acodec", "pcm_s16le",
                        str(ref)], check=True)
        with lib.open("wb") as out:
            subprocess.run([str(d / "probe"), "/" + mp3.name], cwd=d,
                           stdout=out, check=True)
        a = np.fromfile(ref, dtype="<i2").astype(float)
        b = np.fromfile(lib, dtype="<i2").astype(float)
        if len(a) < 30000 or len(b) < 40000:
            raise SystemExit("Unexpectedly short decode; alignment is not meaningful")
        nfft = 1 << (40000 + 30000 - 1).bit_length()
        corr = np.fft.irfft(np.fft.rfft(b[:40000], nfft) *
                             np.conj(np.fft.rfft(a[:30000], nfft)), nfft)
        lead = int(np.argmax(corr[:10000]))
        count = min(len(a), len(b) - lead)
        error = np.abs(b[lead:lead + count] - a[:count])
        print(f"{name}: ffmpeg={len(a)}, library={len(b)}, lead={lead}, "
              f"tail={len(b)-lead-len(a)}, max_delta={error.max():.0f}, "
              f"mean_delta={error.mean():.5f} (mono samples)")
