#!/usr/bin/env python3
"""Build the four inputs the AUD5 reproducers need, next to this script.

    python3 make_vectors.py          # writes tagged_*.ogg/.flac/.wav/.mp3
"""
import os, struct, subprocess, imageio_ffmpeg

FF = imageio_ffmpeg.get_ffmpeg_exe()
HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(os.path.dirname(HERE), "..", "Clain_FileManager",
                   "FileManager_lib", "test", "vectors")


def id3(payload_len, footer=False):
    n = 10 + payload_len
    body = b"TIT2" + struct.pack(">I", max(payload_len - 10, 0)) + b"\x00\x00" + \
           b"A" * max(payload_len - 16, 0)
    body = body[:payload_len].ljust(payload_len, b"\x00")
    hdr = b"ID3\x04\x00" + bytes([0x10 if footer else 0x00]) + bytes(
        [(payload_len >> 21) & 0x7F, (payload_len >> 14) & 0x7F,
         (payload_len >> 7) & 0x7F, payload_len & 0x7F])
    out = hdr + body
    if footer:
        out += b"3DI\x04\x00" + hdr[5:10]
    return out


def main():
    subprocess.run([FF, "-y", "-loglevel", "error",
                    "-f", "lavfi", "-i", "sine=frequency=440:duration=1:sample_rate=44100",
                    "-c:a", "libopus", os.path.join(HERE, "test_opus.ogg")], check=True)
    for name in ("test_flac.flac", "test_vorbis.ogg", "wav_alaw_8k.wav",
                 "test_opus.ogg", "mp3_m2_22k_32k.mp3"):
        src = os.path.join(VEC, name)
        if not os.path.exists(src):
            src = os.path.join(HERE, name)
        base = os.path.splitext(name)[0]
        data = open(src, "rb").read()
        for footer in (False, True):
            suffix = "_footer" if footer else ""
            open(os.path.join(HERE, "tagged_%s%s.bin" % (base, suffix)), "wb") \
                .write(id3(1000, footer) + data)
        print(name, len(data), "bytes")


if __name__ == "__main__":
    main()
