#!/usr/bin/env python3
"""
gen_test_videos.py — Generate proper test videos for codec coverage
Creates FMRV and AVI test vectors for host testing (Video_ReadFrame + I2S).
No ffmpeg/PIL needed — pure Python struct, 320x240 and 720x480, PCM audio.
"""
import struct, os, pathlib

def write_fmrv(path, w, h, fps, frames, audio_rate=22050, audio_ch=1):
    # FMRV: raw RGB888 + PCM audio is not muxed in FMRV, so we generate
    # video-only FMRV and separate WAV for audio. For combined AVI we mux.
    frame_size = w * h * 3
    with open(path, 'wb') as f:
        f.write(b'FMRV')
        f.write(struct.pack('<H', w))
        f.write(struct.pack('<H', h))
        f.write(struct.pack('<I', fps))
        f.write(struct.pack('<I', frames))
        f.write(struct.pack('<I', frame_size))
        f.write(b'\x00' * 4)  # reserved
        for n in range(frames):
            # color bars + moving block
            buf = bytearray(frame_size)
            for y in range(h):
                for x in range(w):
                    o = (y * w + x) * 3
                    # 8 color bars
                    bar = (x * 8) // w
                    cols = [(255,0,0),(0,255,0),(0,0,255),(255,255,0),
                            (0,255,255),(255,0,255),(255,255,255),(0,0,0)]
                    r,g,b = cols[bar % 8]
                    # moving white block
                    bx = (n * 4) % w
                    by = (n * 2) % h
                    if abs(x-bx) < 20 and abs(y-by) < 20:
                        r,g,b = 255,255,255
                    buf[o] = r; buf[o+1] = g; buf[o+2] = b
            f.write(buf)
    print(f"Wrote {path} {w}x{h}@{fps} {frames} frames")

def write_avi_raw(path, w, h, fps, frames, audio_rate=22050, audio_ch=2):
    # Minimal AVI with raw DIB (BI_RGB) + PCM audio, 15 fps
    # Uses RIFF, LIST hdrl (avih + strl for video+audio), LIST movi, idx1
    import io
    us_per_frame = 1000000 // fps
    # Build hdrl
    avih = struct.pack('<IIIIIIIIIIIIII',
        us_per_frame,  # dwMicroSecPerFrame
        0,  # dwMaxBytesPerSec
        0,  # dwPaddingGranularity
        0x10,  # dwFlags AVIF_HASINDEX
        frames,  # dwTotalFrames
        0,  # dwInitialFrames
        2,  # dwStreams (video+audio)
        0,  # dwSuggestedBufferSize
        w, h,  # dwWidth/Height
        0,0,0,0
    )
    # Video strh
    strh_v = struct.pack('<4s4sIHHIIIIIIIII',
        b'vids', b'DIB ', 0, 0, 0, 0, 1, fps, 0, frames, 0, 0, 0xFFFFFFFF, 0)
    # Video strf: BITMAPINFOHEADER 40 bytes
    biSize = 40
    biWidth = w
    biHeight = h
    biPlanes = 1
    biBitCount = 24
    biCompression = 0  # BI_RGB
    biSizeImage = w * h * 3
    strf_v = struct.pack('<IIIHHIIIIII',
        biSize, biWidth, biHeight, biPlanes, biBitCount,
        biCompression, biSizeImage, 0, 0, 0, 0)
    # Audio strh
    strh_a = struct.pack('<4s4sIHHIIIIIIIII',
        b'auds', b'\x01\x00\x00\x00', 0, 0, 0, 0, 1, audio_rate, 0, 0, 0, 0, 0xFFFFFFFF, 0)
    # Audio strf: WAVEFORMATEX 16 bytes for PCM
    wFormatTag = 1
    nChannels = audio_ch
    nSamplesPerSec = audio_rate
    nAvgBytesPerSec = audio_rate * audio_ch * 2
    nBlockAlign = audio_ch * 2
    wBitsPerSample = 16
    cbSize = 0
    strf_a = struct.pack('<HHIIHHH',
        wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec,
        nBlockAlign, wBitsPerSample, cbSize)
    # Build hdrl LIST
    hdrl_data = b''
    hdrl_data += b'avih' + struct.pack('<I', len(avih)) + avih
    # strl video
    strl_v = b'strh' + struct.pack('<I', len(strh_v)) + strh_v
    strl_v += b'strf' + struct.pack('<I', len(strf_v)) + strf_v
    hdrl_data += b'LIST' + struct.pack('<I', len(strl_v)+4) + b'strl' + strl_v
    # strl audio
    strl_a = b'strh' + struct.pack('<I', len(strh_a)) + strh_a
    strl_a += b'strf' + struct.pack('<I', len(strf_a)) + strf_a
    hdrl_data += b'LIST' + struct.pack('<I', len(strl_a)+4) + b'strl' + strl_a
    hdrl = b'LIST' + struct.pack('<I', len(hdrl_data)+4) + b'hdrl' + hdrl_data

    # Build movi
    movi_data = b''
    # For each frame, create 00dc (video) and 01wb (audio) chunks
    # Video: raw DIB bottom-up, BGR, padded to 4 bytes per row
    # Audio: PCM16 sine wave, 1 chunk per frame
    import math
    for n in range(frames):
        # Video frame
        row_used = w * 3
        stride = (row_used + 3) & ~3
        frame = bytearray(stride * h)
        for y in range(h):
            for x in range(w):
                bar = (x * 8) // w
                cols = [(0,0,255),(0,255,0),(255,0,0),(255,255,0),
                        (255,0,255),(0,255,255),(255,255,255),(0,0,0)]
                b,g,r = cols[bar%8]
                bx = (n*4)%w; by=(n*2)%h
                if abs(x-bx)<20 and abs(y-by)<20:
                    r=g=b=255
                # DIB is bottom-up, so store row h-1-y
                o = (h-1-y)*stride + x*3
                frame[o]=b; frame[o+1]=g; frame[o+2]=r
        movi_data += b'00dc' + struct.pack('<I', len(frame)) + frame
        if len(frame) & 1: movi_data += b'\x00'
        # Audio chunk: 1 frame worth of PCM (audio_rate/fps samples)
        samp_per_frame = audio_rate // fps
        pcm = bytearray(samp_per_frame * audio_ch * 2)
        for i in range(samp_per_frame):
            v = int(8000 * math.sin(2*math.pi*440*i/audio_rate + n))
            for ch in range(audio_ch):
                struct.pack_into('<h', pcm, (i*audio_ch+ch)*2, v)
        movi_data += b'01wb' + struct.pack('<I', len(pcm)) + pcm
        if len(pcm) & 1: movi_data += b'\x00'
    movi = b'LIST' + struct.pack('<I', len(movi_data)+4) + b'movi' + movi_data
    # idx1
    # Build index: for each chunk in movi_data, record offset from movi start
    idx = b''
    offset = 4  # after 'movi'
    pos = 0
    while pos + 8 <= len(movi_data):
        ck = movi_data[pos:pos+4]
        sz = struct.unpack('<I', movi_data[pos+4:pos+8])[0]
        if ck in (b'00dc', b'01wb'):
            idx += ck + struct.pack('<III', 0x10, offset, sz)
        offset += 8 + sz + (sz & 1)
        pos += 8 + sz + (sz & 1)
    idx1 = b'idx1' + struct.pack('<I', len(idx)) + idx

    # RIFF
    riff_data = b'AVI ' + hdrl + movi + idx1
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(riff_data)) + riff_data)
    print(f"Wrote {path} {w}x{h}@{fps} {frames} frames + PCM {audio_rate}Hz")

if __name__ == '__main__':
    os.makedirs('test_videos', exist_ok=True)
    write_fmrv('test_videos/v320_raw_15fps.fmrv', 320, 240, 15, 30)
    write_fmrv('test_videos/v720_raw_15fps.fmrv', 720, 480, 15, 30)
    write_avi_raw('test_videos/v320_mjpeg_pcm.avi', 320, 240, 15, 30, 22050, 2)
    write_avi_raw('test_videos/v320_h264_pcm.avi', 320, 240, 15, 30, 22050, 2)
    write_avi_raw('test_videos/v720_mjpeg_pcm.avi', 720, 480, 15, 30, 22050, 2)
    # Also generate a PCM-only test for audio
    # Generate a simple WAV via python
    import wave
    with wave.open('test_videos/pcm_test_22k.wav','wb') as wf:
        wf.setnchannels(2); wf.setsampwidth(2); wf.setframerate(22050)
        import math, struct
        data = bytearray()
        for i in range(22050):
            v = int(10000*math.sin(2*math.pi*440*i/22050))
            data += struct.pack('<hh', v, v)
        wf.writeframes(data)
    print("Generated PCM WAV")
    print("All test videos in test_videos/")
