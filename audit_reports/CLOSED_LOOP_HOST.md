# Closed-loop host media audit

Date: 2026-11-10. This is a synthetic host simulation, not a board test.

## Reproduction

From `Clain_FileManager/FileManager_lib`, install Python NumPy and provide FFmpeg with PNG, FLAC, MJPEG, libmp3lame, libvorbis, libopus and libx264 encoders, plus the libopus decoder. A C compiler and make are required. For example, an FFmpeg binary can be obtained through the Python `imageio-ffmpeg` package.

```sh
python3 tools/closed_loop.py /path/to/ffmpeg
python3 tools/closed_loop.py /path/to/ffmpeg --asan
sh tools/check_cubeide.sh
```

The script forces a rebuild with `FM_USE_OPUS_LIB=1`; sanitizer mode adds ASan and UBSan to both library and harness, with leak detection enabled. It modifies build artifacts in the library directory: use an isolated checkout and rebuild/clean before switching build flags. Temporary media are deleted after each run.

## Results

Three deterministic inputs (seeds 0, 3, 17), both regular and freshly rebuilt ASan/UBSan/LeakSanitizer runs: **PASS**; CubeIDE host compatibility check: **PASS**. The images are 96×64, videos 12 frames at 12 fps, and input audio one second mono at 12 kHz. Opus is resampled to 48 kHz. Results do not establish general codec or hardware correctness.

- PNG and BMP, in both directions, were pixel-exact. JPEG, in both directions, used mean absolute RGB error (MAE) ≤ 8, not an exact or per-pixel bound.
- WAV and FLAC, both directions, and PCM in MJPEG AVI, library-to-FFmpeg, FFmpeg-to-library and library-to-library, were sample-exact. MJPEG frames used MAE ≤ 8; FFmpeg-generated baseline H.264 MP4 used MAE ≤ 12. Count and size checks also apply.
- FFmpeg-generated Vorbis and Opus were decoded by the library and compared to FFmpeg output: equal sample counts and maximum sample difference ≤ 2. Opus comparison explicitly uses FFmpeg's libopus decoder; the library also uses libopus, so this tests container handling and integration, **not an independent Opus decoder**. FFmpeg's native Opus decoder differed substantially from libopus on this synthetic signal (seed 0: mean absolute difference 68, max 9157 on 16-bit PCM); this difference alone does not establish which implementation is wrong.
- MP3 was compared with FFmpeg after correlation-based alignment: 1105 leading and 143 trailing samples in these one-second examples, aligned MAE ≤ 0.002. This confirms the already-known missing gapless trimming; alignment masks that issue for the waveform comparison. `tools/check_mp3_gapless.py` provides another reproducible example.
- Text replace, insert and delete were checked against exact expected bytes. These are not filesystem stress or power-loss tests.

The thresholds for lossy images/video are mean-error thresholds and can miss localized defects; reported maxima are diagnostic, not enforced. The test does not cover all sizes, formats, codecs, damaged inputs, seeking, streaming timing, concurrency or real filesystem behavior. No ARM execution, firmware link, physical board, SD/I2S/USB or real-time timing was tested. See `AUDIO_FOLLOWUP.md` for previously observed open issues and the scope of earlier tests.
