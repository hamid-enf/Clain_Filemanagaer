#!/usr/bin/env python3
"""Make the small video files that test/audit_v3_tests.h decodes.

Audio track:

    test/vectors/avi_mp3.avi             MP3, one frame per chunk (as every
                                         muxer writes it)
    test/vectors/avi_mp3_packed.avi      the same stream, eight frames a chunk
    test/vectors/avi_adpcm_ima.avi       IMA ADPCM, stereo, one block a chunk
    test/vectors/avi_adpcm_ima_packed.avi  the same, eight blocks a chunk
    test/vectors/avi_adpcm_ms.avi        Microsoft ADPCM, stereo
    test/vectors/mkv_vorbis.mkv          Vorbis in Matroska

Picture (four patches: white, black / red, blue; 64x48, two frames):

    test/vectors/mp4_h264_patches.mp4       H.264 baseline, video range
                                            (16..235, what every encoder
                                            writes unless told otherwise)
    test/vectors/mp4_h264_patches_full.mp4  the same, full range, said so
                                            in the VUI
    test/vectors/mp4_mjpeg_patches.mp4      Motion JPEG in an .mp4: an
                                            "mp4v" entry of object type 0x6C

For the two ADPCM files it prints the length and CRC-32 of the PCM that
ffmpeg decodes from them: ADPCM is integer arithmetic, the library has to
produce exactly those bytes, and the test compares against these numbers.

Needs ffmpeg with libmp3lame, libvorbis and libx264: on the PATH, in $FFMPEG, or
from "pip install imageio-ffmpeg".

    python3 tools/gen_video_audio_vectors.py
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "test", "vectors")


def ffmpeg_path():
    p = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if p:
        return p
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        sys.exit("ffmpeg not found (set FFMPEG, or pip install imageio-ffmpeg)")


def chunks(buf, start, end):
    """Yield (fourcc, payload_offset, size) of the chunks in buf[start:end]."""
    p = start
    while p + 8 <= end:
        cc = buf[p:p + 4]
        size = struct.unpack_from("<I", buf, p + 4)[0]
        yield cc, p + 8, size
        p += 8 + size + (size & 1)


def pack_audio(src, group):
    """Merge every `group` consecutive audio chunks of the movi list."""
    assert src[:4] == b"RIFF" and src[8:12] == b"AVI "
    head = None
    movi = None
    for cc, off, size in chunks(src, 12, len(src)):
        if cc == b"LIST" and src[off:off + 4] == b"movi":
            head = src[:off - 8]
            movi = (off + 4, off + size)
    assert movi is not None, "no movi list"

    body = bytearray(b"movi")
    index = bytearray()
    pending = bytearray()
    count = 0

    def emit(cc, payload):
        index.extend(cc + struct.pack("<III", 0x10, len(body), len(payload)))
        body.extend(cc + struct.pack("<I", len(payload)) + payload)
        if len(payload) & 1:
            body.append(0)

    for cc, off, size in chunks(src, movi[0], movi[1]):
        payload = src[off:off + size]
        if cc[2:] == b"wb":
            pending.extend(payload)
            count += 1
            if count == group:
                emit(cc, bytes(pending))
                pending = bytearray()
                count = 0
        else:
            emit(cc, payload)
    if pending:
        emit(b"01wb", bytes(pending))

    out = bytearray(head)
    out.extend(b"LIST" + struct.pack("<I", len(body)) + body)
    out.extend(b"idx1" + struct.pack("<I", len(index)) + index)
    struct.pack_into("<I", out, 4, len(out) - 8)
    return bytes(out)


def make(ff, name, rate, seconds, audio_args, container_args=()):
    path = os.path.join(OUT, name)
    subprocess.check_call([
        ff, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc=size=32x24:rate=5:duration=%s" % seconds,
        "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=%d:duration=%s" % (rate, seconds),
        "-c:v", "mjpeg", "-q:v", "20"] + list(audio_args) +
        ["-bitexact", "-flags", "+bitexact"] + list(container_args) + [path])
    return path


PATCHES = ("color=c=white:s=32x24:r=5:d=0.4[a];"
           "color=c=black:s=32x24:r=5:d=0.4[b];"
           "color=c=red:s=32x24:r=5:d=0.4[c];"
           "color=c=blue:s=32x24:r=5:d=0.4[d];"
           "[a][b]hstack[t];[c][d]hstack[u];[t][u]vstack[o]")


def make_patches(ff, name, video_args):
    path = os.path.join(OUT, name)
    subprocess.check_call([
        ff, "-hide_banner", "-loglevel", "error", "-y",
        "-filter_complex", PATCHES, "-map", "[o]"] + list(video_args) +
        ["-an", "-bitexact", "-flags", "+bitexact", path])
    return path


def reference_pcm(ff, path):
    return subprocess.check_output([
        ff, "-hide_banner", "-loglevel", "error", "-i", path, "-vn",
        "-f", "s16le", "-acodec", "pcm_s16le", "-"])


def packed(path, name, group):
    with open(path, "rb") as f:
        src = f.read()
    out = os.path.join(OUT, name)
    with open(out, "wb") as f:
        f.write(pack_audio(src, group))
    return out


def main():
    ff = ffmpeg_path()
    made = []

    p = make(ff, "avi_mp3.avi", 44100, "1.2",
             ["-c:a", "libmp3lame", "-b:a", "64k", "-ac", "2"])
    made += [p, packed(p, "avi_mp3_packed.avi", 8)]

    refs = []
    p = make(ff, "avi_adpcm_ima.avi", 11025, "1",
             ["-c:a", "adpcm_ima_wav", "-ac", "2"])
    refs.append((p, reference_pcm(ff, p)))
    made += [p, packed(p, "avi_adpcm_ima_packed.avi", 8)]
    p = make(ff, "avi_adpcm_ms.avi", 11025, "1",
             ["-c:a", "adpcm_ms", "-ac", "2"])
    refs.append((p, reference_pcm(ff, p)))
    made.append(p)

    p = make(ff, "mkv_vorbis.mkv", 22050, "1",
             ["-c:a", "libvorbis", "-q:a", "1", "-ac", "2"])
    made.append(p)

    x264 = ["-c:v", "libx264", "-profile:v", "baseline", "-g", "2"]
    made.append(make_patches(ff, "mp4_h264_patches.mp4",
                             x264 + ["-pix_fmt", "yuv420p"]))
    made.append(make_patches(ff, "mp4_h264_patches_full.mp4",
                             x264 + ["-pix_fmt", "yuvj420p",
                                     "-color_range", "pc"]))
    made.append(make_patches(ff, "mp4_mjpeg_patches.mp4",
                             ["-c:v", "mjpeg", "-q:v", "3",
                              "-pix_fmt", "yuvj420p"]))

    for p in made:
        print("%-44s %6d bytes" % (os.path.relpath(p), os.path.getsize(p)))
    for p, pcm in refs:
        print("%-44s PCM %d bytes, CRC-32 0x%08X" %
              (os.path.relpath(p), len(pcm), zlib.crc32(pcm) & 0xFFFFFFFF))


if __name__ == "__main__":
    main()
