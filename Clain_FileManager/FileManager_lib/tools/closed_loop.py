#!/usr/bin/env python3
"""Deterministic, host-only cross-check with FFmpeg.

Generate images, PCM, MJPEG/PCM AVI and text through the library, decode them
with FFmpeg, and also generate independent media with FFmpeg for the library
to decode. Temporary data are removed on exit. Requires numpy, make, a C compiler and FFmpeg with PNG, FLAC, MJPEG,
libmp3lame, libvorbis, libopus and libx264 support. No hardware, timing or full codec coverage implied.

    python3 tools/closed_loop.py /path/to/ffmpeg
    python3 tools/closed_loop.py /path/to/ffmpeg --asan

The sanitizer mode rebuilds the library with ASan+UBSan, then instruments the
host program as well. Run it in an isolated checkout if other builds are active.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
W, H, FRAMES, RATE = 96, 64, 12, 12000
PIXELS = W * H * 3


def run(argv, cwd, *, env=None):
    subprocess.run([str(x) for x in argv], cwd=cwd, env=env, check=True,
                   timeout=120)


def image_comparison(path, reference, frames, limit):
    data = np.fromfile(path, dtype=np.uint8)
    expected = reference[:frames * PIXELS]
    if len(data) != len(expected):
        raise AssertionError(f"{path.name}: size {len(data)} != {len(expected)}")
    delta = np.abs(data.astype(np.int16) - expected.astype(np.int16))
    mean, max_error = float(delta.mean()), int(delta.max())
    if mean > limit:
        raise AssertionError(f"{path.name}: MAE {mean:.4f} > {limit}")
    print(f"  {path.name}: {frames} frame(s), MAE={mean:.3f}, max={max_error}")


def audio_comparison(path, expected):
    data = path.read_bytes()
    if data != expected:
        raise AssertionError(f"{path.name}: PCM differs ({len(data)} vs {len(expected)} bytes)")
    print(f"  {path.name}: {len(data)//2} PCM samples bit-exact")


def ffmpeg(ff, work, *args):
    run([ff, "-hide_banner", "-loglevel", "error", "-y", *args], work)


def exercise(ff, executable, work, seed, env):
    env = dict(env, CLOSED_LOOP_SEED=str(seed))
    run([executable, "generate"], work, env=env)
    reference = np.fromfile(work / "reference.rgb", dtype=np.uint8)
    expected_pcm = (work / "reference.pcm").read_bytes()
    if len(reference) != FRAMES * PIXELS or len(expected_pcm) != RATE * 2:
        raise AssertionError("generated reference size mismatch")
    if (work / "library.txt").read_bytes() != b"alpha omega\ninserted\nlast\n":
        raise AssertionError("text editing result mismatch")

    # Independently encode the same source using FFmpeg, then let the library
    # decode it. The sound and image are one second, 12 frames, 12 kHz mono.
    rawvideo = ["-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                "-framerate", "12", "-i", "reference.rgb"]
    rawpcm = ["-f", "s16le", "-ar", str(RATE), "-ac", "1", "-i", "reference.pcm"]
    ffmpeg(ff, work, *rawvideo, "-frames:v", "1", "-threads", "1", "external.png")
    ffmpeg(ff, work, *rawvideo, "-frames:v", "1", "external.bmp")
    ffmpeg(ff, work, *rawvideo, "-frames:v", "1", "-q:v", "2", "-pix_fmt", "yuvj444p", "external.jpg")
    ffmpeg(ff, work, *rawpcm, "-c:a", "pcm_s16le", "external.wav")
    ffmpeg(ff, work, *rawpcm, "-c:a", "flac", "external.flac")
    ffmpeg(ff, work, *rawpcm, "-c:a", "libmp3lame", "-b:a", "48k", "external.mp3")
    ffmpeg(ff, work, *rawpcm, "-c:a", "libvorbis", "-q:a", "5", "external.ogg")
    ffmpeg(ff, work, *rawpcm, "-c:a", "libopus", "-ar", "48000", "-b:a", "48k", "external.opus")
    for fmt in ("ogg", "opus"):
        ffmpeg(ff, work, *(["-c:a", "libopus"] if fmt == "opus" else []), "-i", f"external.{fmt}", "-f", "s16le", "-acodec", "pcm_s16le", f"ff_external_{fmt}.pcm")
    ffmpeg(ff, work, *rawvideo, "-c:v", "libx264", "-profile:v", "baseline",
           "-pix_fmt", "yuv420p", "-preset", "ultrafast", "-tune", "zerolatency",
           "-x264-params", "bframes=0", "external_h264.mp4")
    ffmpeg(ff, work, "-i", "external.mp3", "-f", "s16le", "-acodec", "pcm_s16le", "ff_external_mp3.pcm")
    ffmpeg(ff, work, *rawvideo, *rawpcm, "-c:v", "mjpeg", "-pix_fmt", "yuvj444p",
           "-q:v", "2", "-c:a", "pcm_s16le", "-shortest", "external.avi")
    run([executable, "verify"], work, env=env)

    # The other direction: decode the library's files with FFmpeg.
    for fmt in ("png", "bmp", "jpg"):
        ffmpeg(ff, work, "-i", f"library.{fmt}", "-frames:v", "1", "-f", "rawvideo",
               "-pix_fmt", "rgb24", f"ff_{fmt}.rgb")
    for fmt in ("wav", "flac"):
        ffmpeg(ff, work, "-i", f"library.{fmt}", "-f", "s16le", "-acodec",
               "pcm_s16le", f"ff_{fmt}.pcm")
    ffmpeg(ff, work, "-i", "library.avi", "-map", "0:v:0", "-f", "rawvideo",
           "-pix_fmt", "rgb24", "ff_avi.rgb", "-map", "0:a:0", "-f", "s16le",
           "-acodec", "pcm_s16le", "ff_avi.pcm")
    for name in ("ff_png.rgb", "ff_bmp.rgb", "decoded_external.rgb", "decoded_external_bmp.rgb"):
        image_comparison(work / name, reference, 1, 0)
    image_comparison(work / "ff_jpg.rgb", reference, 1, 8)
    image_comparison(work / "decoded_external_jpg.rgb", reference, 1, 8)
    for fmt in ("ogg", "opus"):
        ref = np.fromfile(work / f"ff_external_{fmt}.pcm", dtype="<i2").astype(np.int32)
        got = np.fromfile(work / f"decoded_external_{fmt}.pcm", dtype="<i2").astype(np.int32)
        if len(ref) != len(got):
            raise AssertionError(f"{fmt}: sample count {len(got)} != {len(ref)}")
        delta = np.abs(got-ref)
        if delta.size == 0 or delta.max() > 2:
            raise AssertionError(f"{fmt}: max PCM delta {delta.max() if delta.size else 'empty'}")
        print(f"  external {fmt}: {len(got)} samples, max PCM delta {delta.max()}")
    image_comparison(work / "decoded_external_h264.rgb", reference, FRAMES, 12)
    # FFmpeg/libmp3lame writes a recognized gapless tag: the complete
    # decoded output must match, not merely an aligned interior window.
    refmp3 = np.fromfile(work / "ff_external_mp3.pcm", dtype="<i2").astype(np.int32)
    libmp3 = np.fromfile(work / "decoded_external_mp3.pcm", dtype="<i2").astype(np.int32)
    if len(refmp3) == 0 or len(refmp3) != len(libmp3):
        raise AssertionError("gapless MP3 sample count mismatch")
    delta = np.abs(libmp3 - refmp3)
    if delta.max() > 2:
        raise AssertionError(f"gapless MP3 max sample delta {delta.max()} exceeds 2")
    print(f"  external MP3: {len(refmp3)} samples, max PCM delta {delta.max()}")
    for name in ("ff_avi.rgb", "decoded_external_video.rgb", "decoded_library_video.rgb"):
        image_comparison(work / name, reference, FRAMES, 8)
    for name in ("ff_wav.pcm", "ff_flac.pcm", "ff_avi.pcm", "decoded_external.pcm",
                 "decoded_external_flac.pcm", "decoded_external_video.pcm",
                 "decoded_library_video.pcm"):
        audio_comparison(work / name, expected_pcm)
    print("  text replace/insert/delete: exact match")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ffmpeg", nargs="?", default=shutil.which("ffmpeg"))
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    if not args.ffmpeg or not Path(args.ffmpeg).is_file():
        parser.error("provide a path to ffmpeg")
    cc = shlex.split(os.environ.get("CC", "gcc"))
    san = ["-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.asan else []
    run(["make", "-s", "-B", "-j2", "libfilemanager.a", "CC=" + shlex.join(cc),
         "CFLAGS_EXTRA=" + " ".join(san), "FM_USE_OPUS_LIB=1"], ROOT)
    with tempfile.TemporaryDirectory(prefix="fm_closed_loop_") as temp:
        work = Path(temp)
        executable = work / "closed_loop"
        run([*cc, *san, "-Wall", "-Wextra", "-Werror", "-DFM_HOST_BUILD=1",
             "-Iinclude", "-Ibackends", "-Isrc", "tools/closed_loop_host.c",
             "libfilemanager.a", "-lm", "-o", executable], ROOT)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        for seed in (0, 3, 17):
            print(f"seed={seed}", flush=True)
            exercise(args.ffmpeg, executable, work, seed, env)
    print("Closed-loop checks PASS (3 deterministic inputs).")


if __name__ == "__main__":
    main()
