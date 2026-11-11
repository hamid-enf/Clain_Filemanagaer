#!/usr/bin/env python3
"""Make the two AVI test files with an MP3 audio track.

    test/vectors/avi_mp3.avi         as ffmpeg writes it: one MP3 frame per
                                     audio chunk (what every muxer does)
    test/vectors/avi_mp3_packed.avi  the same stream with the audio chunks
                                     merged eight at a time, to exercise a
                                     packet that holds several frames

Needs ffmpeg with libmp3lame: either on the PATH, in $FFMPEG, or from
"pip install imageio-ffmpeg".

    python3 tools/gen_avi_mp3_vectors.py
"""
import os
import shutil
import struct
import subprocess
import sys

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


def main():
    ff = ffmpeg_path()
    plain = os.path.join(OUT, "avi_mp3.avi")
    subprocess.check_call([
        ff, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc=size=32x24:rate=5:duration=1.2",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1.2",
        "-c:v", "mjpeg", "-q:v", "20",
        "-c:a", "libmp3lame", "-b:a", "64k", "-ac", "2",
        "-bitexact", "-flags", "+bitexact", plain])
    with open(plain, "rb") as f:
        src = f.read()
    packed = os.path.join(OUT, "avi_mp3_packed.avi")
    with open(packed, "wb") as f:
        f.write(pack_audio(src, 8))
    for p in (plain, packed):
        print("%s  %d bytes" % (os.path.relpath(p), os.path.getsize(p)))


if __name__ == "__main__":
    main()
