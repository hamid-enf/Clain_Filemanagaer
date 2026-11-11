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

    test/vectors/mp4_h264_patches_709.mp4   H.264, BT.709 matrix, said so
                                            in the VUI
    test/vectors/avi_mjpeg422_patches.avi   Motion JPEG 4:2:2 and 4:4:4 the
    test/vectors/avi_mjpeg444_patches.avi   way ffmpeg writes them: chroma
                                            sampling factors 1x2

Uncompressed audio (two tones, 440 Hz left and 660 Hz right, 8 kHz, made
as 16-bit samples, so that every wider format holds the same numbers and
has to come back as the same 16-bit PCM):

    test/vectors/avi_pcm24.avi           24-bit, WAVE_FORMAT_EXTENSIBLE
    test/vectors/avi_pcm_f32.avi         32-bit float
    test/vectors/mov_pcm_s16.mov         "sowt", one sample frame a packet
    test/vectors/mov_pcm24be.mov         "in24"
    test/vectors/mov_pcm24le.mov         "in24" with an "enda" box
    test/vectors/mov_pcm_f32be.mov       "fl32"
    test/vectors/mov_pcm_s8.mov          8-bit signed
    test/vectors/mp4_ipcm24.mp4          "ipcm" with a "pcmC" box
    test/vectors/mkv_pcm32.mkv           A_PCM/INT/LIT, 32-bit
    test/vectors/mkv_pcm_s16be.mkv       A_PCM/INT/BIG
    test/vectors/mkv_pcm_f32.mkv         A_PCM/FLOAT/IEEE
    test/vectors/mp4_flac24.mp4          FLAC, 24 bits (lossless: the same
                                         PCM again)

FLAC with 32 bits per sample, which the decoder (dr_flac) does not do:

    test/vectors/mp4_flac32.mp4
    test/vectors/flac32_8k.flac

Encoder delay (0.2 s of silence, then a 440 Hz tone: where the tone
starts in the decoded track shows whether the delay was removed):

    test/vectors/mp4_mp3_delay.mp4       MP3, the delay in the edit list
    test/vectors/mkv_mp3_delay.mkv       MP3, the delay in CodecDelay
    test/vectors/mkv_vorbis_delay.mkv    Vorbis (a CodecDelay that is not
                                         to be applied)
    test/vectors/mkv_opus_delay.mkv      Opus, CodecDelay / pre-skip
    test/vectors/mp4_opus_delay.mp4      Opus, edit list / dOps

Microsoft ADPCM in a WAV file:

    test/vectors/wav_adpcm_ms_stereo_8k.wav
    test/vectors/wav_adpcm_ms_mono_11k.wav

For the ADPCM and the lossless files it prints the length and CRC-32 of
the PCM that ffmpeg decodes from them: that is integer arithmetic, the
library has to produce exactly those bytes, and the tests compare against
these numbers.  For the delay files it prints where the tone starts in
ffmpeg's decode.

Needs ffmpeg with libmp3lame, libvorbis, libopus and libx264: on the PATH, in $FFMPEG, or
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


def make_patches(ff, name, video_args, post=None):
    """`post`: a filter chain applied to the picture before encoding."""
    path = os.path.join(OUT, name)
    graph = PATCHES if post is None else PATCHES[:-3] + "[q];[q]" + post + "[o]"
    subprocess.check_call([
        ff, "-hide_banner", "-loglevel", "error", "-y",
        "-filter_complex", graph, "-map", "[o]"] + list(video_args) +
        ["-an", "-bitexact", "-flags", "+bitexact", path])
    return path


TWO_TONES = ("sine=frequency=440:sample_rate=%(rate)d:duration=%(dur)s[l];"
             "sine=frequency=660:sample_rate=%(rate)d:duration=%(dur)s[r];"
             "[l][r]amerge=inputs=2[a]")


def make_tones(ff, name, rate, seconds, audio_args, mono=False):
    """A tiny picture and the two tones (or the left one alone)."""
    path = os.path.join(OUT, name)
    graph = TWO_TONES % {"rate": rate, "dur": seconds}
    if mono:
        graph = ("sine=frequency=440:sample_rate=%d:duration=%s[a]"
                 % (rate, seconds))
    cmd = [ff, "-hide_banner", "-loglevel", "error", "-y"]
    sound_only = name.endswith(".wav") or name.endswith(".flac")
    if not sound_only:
        cmd += ["-f", "lavfi", "-i",
                "testsrc=size=32x24:rate=5:duration=%s" % seconds]
    cmd += ["-filter_complex", graph]
    if not sound_only:
        cmd += ["-map", "0:v", "-c:v", "mjpeg", "-q:v", "20"]
    cmd += ["-map", "[a]"] + list(audio_args) + [
        "-bitexact", "-flags", "+bitexact", path]
    subprocess.check_call(cmd)
    return path


DELAY_AT = 0.2      # seconds of silence before the tone


def make_delay(ff, name, rate, audio_args):
    path = os.path.join(OUT, name)
    tone = ("aevalsrc='if(lt(t,%s),0,0.25*sin(2*PI*440*(t-%s)))':s=%d:d=0.6"
            % (DELAY_AT, DELAY_AT, rate))
    subprocess.check_call([
        ff, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc=size=32x24:rate=5:duration=0.6",
        "-f", "lavfi", "-i", tone,
        "-c:v", "mjpeg", "-q:v", "20", "-ac", "1"] + list(audio_args) +
        ["-bitexact", "-flags", "+bitexact", path])
    return path


def onset(pcm, threshold=1000):
    """Index of the first 16-bit sample at or above the threshold."""
    n = len(pcm) // 2
    for i, x in enumerate(struct.unpack("<%dh" % n, pcm[:2 * n])):
        if abs(x) >= threshold:
            return i
    return -1


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

    made.append(make_patches(
        ff, "mp4_h264_patches_709.mp4",
        x264 + ["-colorspace", "bt709", "-color_primaries", "bt709",
                "-color_trc", "bt709"],
        post="scale=out_color_matrix=bt709:out_range=tv,format=yuv420p"))
    for fmt in ("422", "444"):
        made.append(make_patches(ff, "avi_mjpeg%s_patches.avi" % fmt,
                                 ["-c:v", "mjpeg", "-q:v", "3",
                                  "-pix_fmt", "yuvj%sp" % fmt]))

    # Lossless audio: every file holds the same 16-bit numbers.
    same = []
    for name, args in (
            ("avi_pcm24.avi", ["-c:a", "pcm_s24le"]),
            ("avi_pcm_f32.avi", ["-c:a", "pcm_f32le"]),
            ("mov_pcm_s16.mov", ["-c:a", "pcm_s16le"]),
            ("mov_pcm24be.mov", ["-c:a", "pcm_s24be"]),
            ("mov_pcm24le.mov", ["-c:a", "pcm_s24le"]),
            ("mov_pcm_f32be.mov", ["-c:a", "pcm_f32be"]),
            ("mp4_ipcm24.mp4", ["-c:a", "pcm_s24be"]),
            ("mkv_pcm32.mkv", ["-c:a", "pcm_s32le"]),
            ("mkv_pcm_s16be.mkv", ["-c:a", "pcm_s16be"]),
            ("mkv_pcm_f32.mkv", ["-c:a", "pcm_f32le"]),
            ("mp4_flac24.mp4", ["-c:a", "flac", "-sample_fmt", "s32",
                                "-bits_per_raw_sample", "24",
                                "-strict", "-2"])):
        p = make_tones(ff, name, 8000, "0.2", args)
        same.append((p, reference_pcm(ff, p)))
        made.append(p)
    for p, pcm in same:
        assert pcm == same[0][1], "%s: ffmpeg decodes other PCM" % p
    refs.append(("the eleven lossless files above", same[0][1]))
    p = make_tones(ff, "mov_pcm_s8.mov", 8000, "0.2", ["-c:a", "pcm_s8"],
                   mono=True)
    refs.append((p, reference_pcm(ff, p)))
    made.append(p)

    # FLAC with 32 bits per sample (RFC 9639): dr_flac stops at 24, the
    # library has to say "unsupported", not "damaged".
    made.append(make_tones(ff, "mp4_flac32.mp4", 8000, "0.2",
                           ["-c:a", "flac", "-sample_fmt", "s32",
                            "-strict", "-2"]))
    made.append(make_tones(ff, "flac32_8k.flac", 8000, "0.2",
                           ["-c:a", "flac", "-sample_fmt", "s32",
                            "-strict", "-2"]))

    # Microsoft ADPCM in a WAV file.
    p = make_tones(ff, "wav_adpcm_ms_stereo_8k.wav", 8000, "0.5",
                   ["-c:a", "adpcm_ms"])
    refs.append((p, reference_pcm(ff, p)))
    made.append(p)
    p = make_tones(ff, "wav_adpcm_ms_mono_11k.wav", 11025, "0.5",
                   ["-c:a", "adpcm_ms"], mono=True)
    refs.append((p, reference_pcm(ff, p)))
    made.append(p)

    # Encoder delay.
    delays = []
    for name, rate, args in (
            ("mp4_mp3_delay.mp4", 44100, ["-c:a", "libmp3lame", "-b:a", "64k"]),
            ("mkv_mp3_delay.mkv", 44100, ["-c:a", "libmp3lame", "-b:a", "64k"]),
            ("mkv_vorbis_delay.mkv", 22050, ["-c:a", "libvorbis", "-q:a", "1"]),
            ("mkv_opus_delay.mkv", 48000, ["-c:a", "libopus", "-b:a", "48k"]),
            ("mp4_opus_delay.mp4", 48000,
             ["-c:a", "libopus", "-b:a", "48k", "-strict", "-2"])):
        p = make_delay(ff, name, rate, args)
        delays.append((p, rate, reference_pcm(ff, p)))
        made.append(p)

    for p in made:
        print("%-44s %6d bytes" % (os.path.relpath(p), os.path.getsize(p)))
    for p, pcm in refs:
        print("%-44s PCM %d bytes, CRC-32 0x%08X" %
              (os.path.relpath(p) if os.path.exists(p) else p, len(pcm),
               zlib.crc32(pcm) & 0xFFFFFFFF))
    for p, rate, pcm in delays:
        print("%-44s tone starts at sample %d of %d (%d expected)" %
              (os.path.relpath(p), onset(pcm), len(pcm) // 2,
               int(DELAY_AT * rate)))


if __name__ == "__main__":
    main()
