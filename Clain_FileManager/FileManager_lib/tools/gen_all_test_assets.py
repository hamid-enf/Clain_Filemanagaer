#!/usr/bin/env python3
"""
gen_all_test_assets.py — Generate comprehensive test assets for all codecs
- Images: BMP, PNG, JPEG via Pillow
- Audio: WAV PCM 8/16, ALAW, ULAW, ADPCM, FLAC, OGG Vorbis, Opus via ffmpeg/soundfile
- Video: AVI/MKV/WebM/FLV with H264/MJPEG/HEVC/VP9/AV1 via ffmpeg
Uses imageio-ffmpeg binary if system ffmpeg not found. All clips are tiny (1-2 sec, 320x240)
so they fit in repo and decode quickly on MCU.
"""
import os, pathlib, struct, math, subprocess, sys, wave
from pathlib import Path

ROOT = Path(__file__).parent.parent
OUT_VIDEO = ROOT / "test_videos"
OUT_AUDIO = ROOT / "audio_test"
OUT_IMAGE = ROOT / "images"
OUT_VECTORS = ROOT / "test" / "vectors"
FFMPEG = None

def find_ffmpeg():
    global FFMPEG
    # try system
    import shutil
    f = shutil.which("ffmpeg")
    if f:
        FFMPEG = f
        return f
    # try imageio-ffmpeg
    try:
        import imageio_ffmpeg
        f = imageio_ffmpeg.get_ffmpeg_exe()
        if Path(f).exists():
            FFMPEG = f
            return f
    except:
        pass
    # fallback to known path from pip
    cand = Path("/usr/local/lib/python3.11/dist-packages/imageio_ffmpeg/binaries/ffmpeg-linux-x86_64-v7.0.2")
    if cand.exists():
        FFMPEG = str(cand)
        return str(cand)
    return None

def run(cmd):
    print(">", " ".join(cmd))
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print(r.stderr[:1000])
    return r.returncode == 0

def gen_images():
    OUT_IMAGE.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image, ImageDraw, ImageFont
        # BMP 320x240
        im = Image.new("RGB", (320,240), (0,0,0))
        draw = ImageDraw.Draw(im)
        for x in range(0,320,40):
            col = (x*255//320, 255-x*255//320, 128)
            draw.rectangle([x,0,x+40,240], fill=col)
        draw.ellipse([100,80,220,160], fill=(255,255,255), outline=(255,0,0), width=3)
        draw.text((10,10), "FileManager BMP 320x240", fill=(255,255,255))
        im.save(OUT_IMAGE / "test.bmp")
        print(f"Wrote {OUT_IMAGE/'test.bmp'} {im.size}")
        # PNG with gradient + text
        im2 = Image.new("RGB", (320,240), (0,0,64))
        draw2 = ImageDraw.Draw(im2)
        for y in range(240):
            c = int(255*y/240)
            draw2.line([(0,y),(320,y)], fill=(c,c//2,255-c))
        draw2.text((10,10), "PNG test 320x240", fill=(255,255,0))
        draw2.rectangle([50,50,270,190], outline=(255,255,255), width=2)
        im2.save(OUT_IMAGE / "test.png", "PNG")
        print(f"Wrote {OUT_IMAGE/'test.png'}")
        # JPEG
        im3 = Image.new("RGB", (320,240), (64,0,0))
        draw3 = ImageDraw.Draw(im3)
        for i in range(8):
            col = [(255,0,0),(0,255,0),(0,0,255),(255,255,0),(0,255,255),(255,0,255),(255,255,255),(0,0,0)][i]
            draw3.rectangle([i*40,0,(i+1)*40,240], fill=col)
        draw3.ellipse([80,60,240,180], fill=(255,255,255))
        draw3.text((10,10), "JPEG q85 320x240", fill=(0,0,0))
        im3.save(OUT_IMAGE / "test.jpg", "JPEG", quality=85)
        print(f"Wrote {OUT_IMAGE/'test.jpg'}")
        # Tiny vector for test/vectors
        OUT_VECTORS.mkdir(parents=True, exist_ok=True)
        im_small = Image.new("RGB", (64,64), (255,0,0))
        ImageDraw.Draw(im_small).text((5,25), "64", fill=(255,255,255))
        im_small.save(OUT_VECTORS / "tiny64.png", "PNG")
        # Also create a 64x64 jpeg for mjpeg
        im_small.save(OUT_VECTORS / "tiny64.jpg", "JPEG", quality=75)
        # BMP vector
        im_small.save(OUT_VECTORS / "tiny64.bmp", "BMP")
        print("Wrote tiny vectors")
        return True
    except Exception as e:
        print(f"Pillow failed: {e}")
        # fallback: generate BMP manually
        w,h=320,240
        row = w*3
        stride = (row+3)&~3
        hdr = struct.pack('<2sIHHI', b'BM', 54+stride*h, 0,0,54) + struct.pack('<IIIHHIIIIII',40,w,h,1,24,0,stride*h,0,0,0,0)
        with open(OUT_IMAGE/"test.bmp","wb") as f:
            f.write(hdr)
            for y in range(h):
                f.write(b'\x00'*stride)
        print("fallback BMP")
        return False

def gen_wav_pcm(path, sr=22050, ch=1, bits=16, duration=1.0, freq=440):
    n = int(sr*duration)
    with wave.open(str(path),'wb') as wf:
        wf.setnchannels(ch)
        wf.setsampwidth(bits//8)
        wf.setframerate(sr)
        import struct, math
        for i in range(n):
            v = int(8000*math.sin(2*math.pi*freq*i/sr))
            if bits==8:
                v8 = (v//256)+128
                if ch==1:
                    wf.writeframes(struct.pack('B', v8 & 0xFF))
                else:
                    wf.writeframes(struct.pack('BB', v8 & 0xFF, v8 & 0xFF))
            else:
                if ch==1:
                    wf.writeframes(struct.pack('<h', v))
                else:
                    wf.writeframes(struct.pack('<hh', v, v))

def gen_audio():
    OUT_AUDIO.mkdir(parents=True, exist_ok=True)
    OUT_VECTORS.mkdir(parents=True, exist_ok=True)
    # pure WAV PCM variants via wave
    gen_wav_pcm(OUT_AUDIO/"pcm_s16_mono_22k.wav", sr=22050, ch=1, bits=16, duration=1)
    gen_wav_pcm(OUT_AUDIO/"pcm_s16_stereo_22k.wav", sr=22050, ch=2, bits=16, duration=1)
    gen_wav_pcm(OUT_AUDIO/"pcm_u8_mono_8k.wav", sr=8000, ch=1, bits=8, duration=1)
    gen_wav_pcm(OUT_AUDIO/"pcm_s16_mono_44k.wav", sr=44100, ch=1, bits=16, duration=1)
    print("Wrote WAV PCM variants")
    # Use ffmpeg for compressed variants if available
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("ffmpeg not found, skipping compressed audio generation")
        return
    # Generate a 1 sec sine wav as source
    src = OUT_AUDIO/"_src_440.wav"
    gen_wav_pcm(src, sr=22050, ch=1, bits=16, duration=1)
    def ffmpeg_encode(src, dst, acodec, extra=[]):
        cmd = [ffmpeg, "-y", "-i", str(src)] + extra + ["-c:a", acodec, str(dst)]
        # for alaw/mulaw/adpcm need wav container
        if dst.suffix==".wav":
            cmd = [ffmpeg, "-y", "-i", str(src), "-c:a", acodec, str(dst)]
        return run(cmd)
    # alaw, mulaw, adpcm
    ffmpeg_encode(src, OUT_AUDIO/"wav_alaw_8k.wav", "pcm_alaw")
    ffmpeg_encode(src, OUT_AUDIO/"wav_mulaw_8k.wav", "pcm_mulaw")
    ffmpeg_encode(src, OUT_AUDIO/"wav_adpcm_ima_22k.wav", "adpcm_ima_wav")
    # flac
    run([ffmpeg,"-y","-i",str(src),"-c:a","flac",str(OUT_AUDIO/"test_flac.flac")])
    run([ffmpeg,"-y","-i",str(src),"-c:a","libvorbis","-q:a","3",str(OUT_AUDIO/"test_vorbis.ogg")])
    run([ffmpeg,"-y","-i",str(src),"-c:a","libopus","-b:a","32k",str(OUT_AUDIO/"test_opus.ogg")])
    # also copy to vectors for CI (tiny)
    for f in ["test_flac.flac","test_vorbis.ogg","test_opus.ogg","wav_alaw_8k.wav","wav_mulaw_8k.wav","wav_adpcm_ima_22k.wav"]:
        s = OUT_AUDIO/f
        if s.exists():
            import shutil
            shutil.copy(s, OUT_VECTORS/f)
            print(f"Copied {f} to vectors")
    # cleanup src
    try: src.unlink()
    except: pass

def gen_video():
    OUT_VIDEO.mkdir(parents=True, exist_ok=True)
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("ffmpeg not found, skipping video generation")
        return
    # create a 1 sec testsrc via lavfi
    # Use ffmpeg lavfi testsrc + sine audio
    def make_video(dst, vcodec, acodec=None, fmt="mp4", size="320x240", dur="1"):
        # Use lavfi for video and anullsrc for audio
        # If acodec is None, no audio
        if acodec:
            cmd = [ffmpeg,"-y","-f","lavfi","-i",f"testsrc=size={size}:rate=15:duration={dur}",
                   "-f","lavfi","-i",f"sine=frequency=440:duration={dur}:sample_rate=22050",
                   "-c:v",vcodec,"-pix_fmt","yuv420p","-t",dur,"-c:a",acodec]
            if vcodec=="libx264":
                cmd += ["-profile:v","baseline","-x264-params","keyint=15:bframes=0:cabac=0"]
            cmd += [str(dst)]
        else:
            cmd = [ffmpeg,"-y","-f","lavfi","-i",f"testsrc=size={size}:rate=15:duration={dur}",
                   "-c:v",vcodec,"-pix_fmt","yuv420p","-t",dur]
            if vcodec=="libx264":
                cmd += ["-profile:v","baseline","-x264-params","keyint=15:bframes=0:cabac=0"]
            cmd += [str(dst)]
        return run(cmd)
    # H264 MP4 already exists but create small variants
    make_video(OUT_VIDEO/"v320_mkv_h264.mkv", "libx264", "pcm_s16le", fmt="mkv")
    make_video(OUT_VIDEO/"v320_webm_vp9.webm", "libvpx-vp9", "libopus", fmt="webm")
    # AV1
    make_video(OUT_VIDEO/"v320_webm_av1.webm", "libaom-av1", "libopus", fmt="webm")
    # FLV with H264 + mp3
    make_video(OUT_VIDEO/"v320_flv_h264.flv", "libx264", "libmp3lame", fmt="flv")
    # HEVC MP4
    make_video(OUT_VIDEO/"v320_hevc.mp4", "libx265", None, fmt="mp4")
    # MJPEG AVI
    make_video(OUT_VIDEO/"v320_mjpeg.avi", "mjpeg", "pcm_s16le", fmt="avi")
    print("Video generation done")
    # list
    for p in OUT_VIDEO.iterdir():
        print(f"{p.name} {p.stat().st_size} bytes")

if __name__=="__main__":
    print("=== Images ===")
    gen_images()
    print("\n=== Audio ===")
    gen_audio()
    print("\n=== Video ===")
    gen_video()
    print("\nAll assets done")
