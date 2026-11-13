#!/usr/bin/env python3
"""Regenerate every audio vector used by the AUD5 reproducers."""
import os, struct, subprocess, imageio_ffmpeg, numpy as np
FF = imageio_ffmpeg.get_ffmpeg_exe()
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "files")
os.makedirs(D, exist_ok=True)

def f(*a):
    subprocess.run([FF, "-y", "-loglevel", "error"] + list(a), check=True)

def tone(rate, ch, secs, hz=440.0, amp=0.5):
    n = int(rate * secs)
    t = np.arange(n) / rate
    x = (amp * 32767 * np.sin(2 * np.pi * hz * t)).astype("<i2")
    return np.repeat(x[:, None], ch, axis=1) if ch > 1 else x

def wav(path, x, rate, fmt=1, bits=16):
    ch = 1 if x.ndim == 1 else x.shape[1]
    data = x.tobytes()
    ba = ch * bits // 8
    hdr = (b"WAVEfmt " + struct.pack("<IHHIIHH", 16, fmt, ch, rate,
                                     rate * ba, ba, bits) +
           b"data" + struct.pack("<I", len(data)))
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(hdr) + len(data)) + hdr + data)

wav(f"{D}/a_mono16.wav", tone(44100, 1, 2), 44100)
wav(f"{D}/a_st16.wav", tone(44100, 2, 2, 440, 0.4), 44100)
wav(f"{D}/a_u8.wav", ((tone(8000, 1, 2) >> 8) + 128).astype("u1"), 8000, bits=8)
wav(f"{D}/short.wav", tone(8000, 1, 1), 8000)
for name, fmt in (("a_alaw.wav", "alaw"), ("a_ulaw.wav", "mulaw")):
    wav(f"{D}/_t.wav", tone(8000, 1, 2), 8000)
    f("-i", f"{D}/_t.wav", "-c:a", f"pcm_{fmt}", f"{D}/{name}")
for name, codec in (("a_ima_mono.wav", "adpcm_ima_wav"),
                    ("a_ima_st.wav", "adpcm_ima_wav"),
                    ("a_ms_mono.wav", "adpcm_ms"),
                    ("a_ms_st.wav", "adpcm_ms")):
    src = tone(22050, 1 if "mono" in name else 2, 2, 440, 0.4)
    wav(f"{D}/_t.wav", src, 22050)
    f("-i", f"{D}/_t.wav", "-c:a", codec, f"{D}/{name}")
os.path.exists(f"{D}/_t.wav") and os.remove(f"{D}/_t.wav")

wav(f"{D}/_t.wav", tone(44100, 2, 5, 440, 0.4), 44100)
f("-i", f"{D}/_t.wav", f"{D}/f_st.flac")
f("-i", f"{D}/_t.wav", "-c:a", "libvorbis", "-q:a", "5", f"{D}/v_st.ogg")
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "128k", f"{D}/m_cbr128.mp3")
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-q:a", "4", f"{D}/m_vbr.mp3")
wav(f"{D}/_t.wav", tone(22050, 1, 3, 330, 0.4), 22050)
f("-i", f"{D}/_t.wav", f"{D}/f_mono.flac")
f("-i", f"{D}/_t.wav", "-c:a", "libvorbis", "-q:a", "1", f"{D}/v_mono_q10.ogg")
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "64k", f"{D}/m_mono.mp3")
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "64k", "-ar", "8000", f"{D}/m_8k.mp3")
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "320k", f"{D}/m_320.mp3")
os.remove(f"{D}/_t.wav")

# 24-bit / 8-bit / 8 kHz / 6-channel FLAC
wav(f"{D}/_t.wav", tone(44100, 2, 4, 440, 0.4), 44100)
f("-i", f"{D}/_t.wav", "-c:a", "flac", "-sample_fmt", "s32",
      "-bits_per_raw_sample", "24", f"{D}/f24.flac")
f("-i", f"{D}/_t.wav", "-c:a", "flac", "-sample_fmt", "s16",
      "-bits_per_raw_sample", "8", f"{D}/f8bit.flac")
f("-i", f"{D}/_t.wav", "-ar", "8000", f"{D}/f_8k.flac")
f("-f", "lavfi", "-i", "sine=frequency=440:duration=2:sample_rate=44100", "-ac", "6", f"{D}/f6ch.flac")
os.remove(f"{D}/_t.wav")

# joined streams: two rates, two channel counts
f("-i", f"{D}/m_cbr128.mp3", "-t", "1", "-c", "copy", f"{D}/p44.mp3")
wav(f"{D}/_t.wav", tone(22050, 2, 1, 440, 0.4), 22050)
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "64k", f"{D}/p22.mp3")
os.remove(f"{D}/_t.wav")
open(f"{D}/joined_rate.mp3", "wb").write(open(f"{D}/p44.mp3", "rb").read() +
                                         open(f"{D}/p22.mp3", "rb").read())
wav(f"{D}/_t.wav", tone(44100, 1, 1, 440, 0.4), 44100)
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-b:a", "128k", f"{D}/_m.mp3")
os.remove(f"{D}/_t.wav")
open(f"{D}/joined_chan.mp3", "wb").write(open(f"{D}/m_cbr128.mp3", "rb").read() +
                                         open(f"{D}/_m.mp3", "rb").read())
os.remove(f"{D}/_m.mp3")

# noise VBR: a lag search needs a non-periodic signal
rng = np.random.default_rng(7)
wav(f"{D}/_t.wav", (rng.standard_normal(44100 * 6) * 3000).astype("<i2"), 44100)
f("-i", f"{D}/_t.wav", "-c:a", "libmp3lame", "-q:a", "4", f"{D}/n_vbr.mp3")
os.remove(f"{D}/_t.wav")

# ID3v2.4 tag in front of four containers
def id3(payload_len, pad=0):
    n = payload_len
    size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])
    return b"ID3\x04\x00\x00" + size + (b"TIT2" + struct.pack(">I", payload_len) +
        b"\x00\x00" + b"A" * (payload_len - 10) if payload_len >= 10 else b"\x00" * payload_len)

T = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tagtest")
os.makedirs(T, exist_ok=True)
for src, dst in (("f_st.flac", "flac"), ("v_st.ogg", "ogg"),
                 ("a_mono16.wav", "wav"), ("m_8k.mp3", "mp3")):
    body = open(f"{D}/{src}", "rb").read()
    open(f"{T}/{dst}_id3.bin", "wb").write(id3(90) + body)
    open(f"{T}/{dst}_id3_big.bin", "wb").write(id3(200000) + body)
    # a tag whose declared length runs past the end of the file
    open(f"{T}/{dst}_id3_past.bin", "wb").write(
        b"ID3\x04\x00\x00" + bytes([0x7F, 0x7F, 0x7F, 0x70]) + body)
print("vectors:", len(os.listdir(D)), "tagtest:", len(os.listdir(T)))
