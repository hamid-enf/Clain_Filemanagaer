#!/usr/bin/env python3
"""Host gapless PCM and exact-seek differential. Requires numpy and FFmpeg/libmp3lame.

python3 tools/test_mp3_gapless.py /path/to/ffmpeg [--asan]
Forces a library rebuild; do not run concurrently with other builds.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import wave
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('ffmpeg')
p.add_argument('--asan', action='store_true')
args = p.parse_args()
cc = shlex.split(os.environ.get('CC', 'gcc'))
san = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.asan else []
def run(cmd, cwd=ROOT, **kwargs):
    subprocess.run([str(x) for x in cmd], cwd=cwd, check=True, timeout=120, **kwargs)
run(['make', '-s', '-B', '-j2', 'libfilemanager.a', 'CC=' + shlex.join(cc),
     'CFLAGS_EXTRA=' + ' '.join(san)])
with tempfile.TemporaryDirectory(prefix='fm_gapless_') as temp:
    d = Path(temp)
    run([*cc, *san, '-Wall', '-Wextra', '-Werror', '-DFM_HOST_BUILD=1',
         '-Iinclude', '-Ibackends', '-Isrc', 'tools/mp3_gapless_seek_test.c',
         'libfilemanager.a', '-lm', '-o', d / 'probe'])
    run([*cc, *san, '-Wall', '-Wextra', '-Werror', '-DFM_HOST_BUILD=1',
         '-Iinclude', '-Ibackends', '-Isrc', 'tools/mp3_gapless_probe.c',
         'libfilemanager.a', '-lm', '-o', d / 'raw_probe'])
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    rng = np.random.default_rng(42)
    for rate, channels, frames in [(8000, 1, 441), (12000, 1, 12000), (22050, 2, 18123),
                                    (44100, 1, 176400), (44100, 2, 44117),
                                    (48000, 2, 12345)]:
        data = rng.integers(-16000, 16001, frames * channels, dtype=np.int16)
        with wave.open(str(d / 'input.wav'), 'wb') as w:
            w.setparams((channels, 2, rate, 0, 'NONE', ''))
            w.writeframes(data.astype('<i2').tobytes())
        for mode, opts in [('cbr', ['-b:a', '64k']), ('vbr', ['-q:a', '3'])]:
            common = [args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y']
            run([*common, '-i', 'input.wav', '-c:a', 'libmp3lame', *opts, 'input.mp3'], cwd=d)
            run([*common, '-i', 'input.mp3', '-f', 's16le', '-acodec', 'pcm_s16le', 'reference.pcm'], cwd=d)
            size = (d / 'reference.pcm').stat().st_size
            if size != frames * channels * 2:
                raise AssertionError(f'FFmpeg reference length {size} for {frames} frames')
            run([d / 'probe', '/input.mp3', 'reference.pcm', rate, channels, frames], cwd=d, env=env)
            print(f'{rate} Hz, {channels} ch, {frames} frames, {mode}: PASS', flush=True)
    # Unrecognized, nonsensical or inconsistent metadata must not trim.
    original = bytearray((d / 'input.mp3').read_bytes())
    pos = original.find(b'Xing')
    if pos < 0:
        pos = original.find(b'Info')
    if pos < 0:
        raise AssertionError('expected a Xing header')
    flags = int.from_bytes(original[pos+4:pos+8], 'big')
    ext = pos + 8 + sum(size for bit, size in ((1, 4), (2, 4), (4, 100), (8, 4)) if flags & bit)
    assert bytes(original[ext:ext+4]) in (b'Lavc', b'Lavf', b'LAME')
    malformed = []
    unknown = original.copy(); unknown[ext:ext+4] = b'fake'; malformed.append(('unknown encoder', unknown))
    low_padding = original.copy(); low_padding[ext+21:ext+24] = b'\x24\x00\x00'; malformed.append(('unusable padding', low_padding))
    truncated_size = original.copy(); truncated_size[pos+12:pos+16] = (len(original) * 2).to_bytes(4, 'big'); malformed.append(('invalid size', truncated_size))
    malformed.append(('concatenated streams', original + original))
    if flags & 3 != 3:
        raise AssertionError('fixture must have a Xing size')
    for name, blob in malformed:
        (d / 'bad.mp3').write_bytes(blob)
        result = subprocess.run([str(d / 'raw_probe'), '/bad.mp3'], cwd=d, env=env,
                                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if len(result.stdout) <= frames * channels * 2:
            raise AssertionError(f'{name}: unexpectedly trimmed')
        print(f'{name}: no unsafe trim PASS', flush=True)
    run([args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', 'input.wav',
         '-c:a', 'libmp3lame', '-write_xing', '0', 'no_xing.mp3'], cwd=d)
    result = subprocess.run([str(d / 'raw_probe'), '/no_xing.mp3'], cwd=d, env=env,
                            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if len(result.stdout) <= frames * channels * 2:
        raise AssertionError('headerless MP3 unexpectedly trimmed')
    print('no Xing: legacy untrimmed decode PASS')
print('MP3 gapless differential and exact seek PASS')
