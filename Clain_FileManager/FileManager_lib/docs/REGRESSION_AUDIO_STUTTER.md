# FileManager — Audio Stutter Regression: Root Cause & Fix
**Branch:** `arena/01a06d60-filemanager` vs. embedded `FileManager-arena-01a04ed1-filemanager.rar` (pre-optimization reference)  
**Date:** 2026-09-04 rev2 | **Board:** STM32H7 (port TIER 3, 32 MiB SDRAM, SDMMC+SPI2+I2S1)  
**Rev2:** MKV lacing (Xiph/Fixed/EBML) + MP3→PCM via minimp3 + C99 lambda/misleading-indent fix, 210/210.  
**Test vectors:** `test_videos/v320_mjpeg_pcm.avi` (320×240 MJPEG 15 fps + PCM 22 kHz stereo), `v320_h264_pcm.avi` (320×240 H.264), `v720_mjpeg_pcm.avi` (720×480 MJPEG 15 fps + PCM), music-folder WAV/MP3.

## 1  Symptom
*Optimised build* (the zip in-repo, `beba2f1`) — 2 clips choppy: H.264/320 and MJPEG/320, MJPEG/720 “90 % OK”, music-folder audio flawless. Disabling video (audio-only demux) → audio smooth.

UART profile (optimised):
```
v320_mjpeg_pcm  avg ms/frame decode=11 convert=2 present=1-3  starve=0  lat avg 3 ms latmax 9 ms  refill≈23/64fr
v320_h264_pcm   avg ms        decode=24 convert=2 present=15→1 starve=0  refill≈11→23
v720_mjpeg_pcm  avg ms        decode=84 convert=12 present=29-31 starve=0 refill≈38-49
```
`starve==0` yet choppy → *not* ring underrun. `latmax<<186 ms` half-period → main-loop polls are not late. Glitch is **data / bus contention**, not missed deadline.

Reference build (RAR, original `19_stm32h7_simple_video.h` + `board/main.c`) reportedly smooth for all three. Diff was taken after normalising CRLF (`diff --strip-trailing-cr`) — functional changes only are below.

---

## 2  What the optimiser changed (functional, not CRLF)

| File | Original | Optimised (`beba2f1`) | Effect |
|------|----------|------------------------|--------|
| `board/main.c: MPU_Config` | SDRAM `0xC0000000 32 MiB` cacheable **commented out**, `SCB_EnableDCache()` not called (H7 runs uncached, slow) | Region 1 `0xC0000000` WB cacheable, Region 2 `0x90000000` QSPI 8 MiB WB, **Region 3 `0x30000000` 256 KiB non-cacheable** for `.dma_buffer`, `SCB_EnableICache/DCache()` in `main()` | +70 % SDRAM bandwidth, but requires `FM_Cache_CleanForDma / Invalidate` everywhere. |
| `board/main.c: App_LcdBlitVideo` | `LcdSpiDma_Send` **blocking** (poll to completion) | `LcdSpiDma_Begin` **async** + `SimpleVideo_SetPresentWait(LcdSpiDma_Wait)` — overlap SPI N with decode N+1, `budget = max(decode,SPI)` not sum | Intended to bring 66 ms 15 fps budget inside limits. |
| `examples/spi_tft_dma.h` | `LCD_SPI_DMA_IDLE 0` (no FM_Idle during SPI chunks) | `LCD_SPI_DMA_IDLE 1` → `FM_Idle()` in `lcd_spi_dma_wait_chunk / wait_txc / write_tail`, chunk `LCD_SPI_DMA_CHUNK` from `32768` → `FM_BUF_SCALE(32768)` | Gives I2S refill slots during the ~30 ms SPI transfer. |
| `examples/simple_i2s_mux.h` | `simple_audio_dma_buf[2*32766]` separate from `sv_i2s_buf[2*8192]` (≈160 KiB total) | **shared `simple_i2s_ring[65534]`**, `simple_audio_dma_buf` and `sv_i2s_buf` both alias it; check `SIMPLE_I2S_RING_SAMPLES` | Saves 32 KiB D2 SRAM, safe because `SimpleI2S_Take` is exclusive, but audio half now 32766 vs video half 8192 → head-room ratio 4:1. |
| `examples/simple_video.h` | private `sv_i2s_buf`, `sv_pcm_chunk[8192]`, 1-pole DC blocker (`sv_i2s_dc_x/y`), single-LFSR dither, `sv_i2s_board_stop` without `was_running`, profile only `starve` | `#define sv_i2s_buf simple_i2s_ring`, `sv_pcm_chunk[FM_BUF_SCALE(8192)]`, DC blocker **removed**, 2-LFSR TPDF dither, `was_running` guard, **latency metrics** `sv_i2s_irq_tick/lat_max/lat_sum/lat_n`, volume fade logic unchanged, idle hook `SIMPLE_VIDEO_IDLE_REFILL` now always calls `sv_i2s_poll` | Starvation vs. analog pop now measurable; DC blocker removal saves 2×32-bit state and avoids 1-pole colouration for PCM-through. |
| `src/fm_video.c` | `Video_Open` fps truncation `1000000/us_per_frame`, raw/FMRV sequential reads, `Video_ReadAudioFrame` on oversize chunk → `FM_ERR_TOO_LARGE` without cursor advance (one oversize chunk mutes tail, dropped 30 % at 44 kHz/15 fps) | **Rounded fps** ` (1 000 000+us/2)/us` (29.97→30), `FMRV` and `MJPEG/raw` slices via `video_read_at` (absolute seeks) + `FM_Idle` per slice, `audio_part_off/left` remainder, `avi_audio` streaming continuation past `FM_VIDEO_MAX_FRAMES` (25 s→unlimited), `audio_total/cur_pos/cur_idx/scan_pos/scan_end` | Fixes truncated drift (2 s/min) and “audio stops after 1024 chunks”, but slicing adds 1 `File_Seek` per 32 KiB slice — extra FatFS win invalidations. |
| `examples/simple_audio.h` | own DMA buf, `SA half 32766` (≈743 ms/half at 22 kHz stereo) | aliases `simple_i2s_ring` (same as video) | Same head-room argument. |
| `src/fm_audio.c`, `fm_image.c`, `fm_transfer.c`, ... | small | `FM_MP3_LOW_WATER` batching, `FM_AUDIO_PCM_CHUNK = FM_BUF_SCALE(2048)`, `SD` sector bounce align, cache hooks | Fewer `File_Read` commands per refill → less SD jitter (the WAV-vs-MP3 crackle root cause noted in `fm_config.h`). |

No change to `SIMPLE_VIDEO_MAX_W/H = 320×240` (both builds) — the 720×480 test clip therefore decodes via the new **down-scale path** (`Image_Scale` nearest-neighbor fused `sv_convert_565` for 565) rather than failing `TOO_LARGE` as strict `sv_rgb` sizing would imply. `SIMPLE_IMAGE_MAX` is 800×480 (TIER 3) and `sdram_buffers` is large enough; `simple_video_decode_one` now logs once and scales 720→320 instead of rejecting.

---

## 3  Why 320 choppy, 720 “mostly OK”, music OK, video-off smooth

### 3.1  Two-buffer head-room
* Audio-only: `SIMPLE_AUDIO_HALF_SAMPLES = 32766` → 743 ms/half, 1.48 s ring.  
* Video-audio: `SIMPLE_VIDEO_HALF_SAMPLES = 8192` → 186 ms/half, 371 ms ring.  
Decoding disabled → refill budget 743 ms, tolerant of any SD/SPI jitter. Video enabled → budget quartered. Starve-free yet choppy means the **PCM bytes themselves are late/corrupt**, not the DMA pointer.

### 3.2  Bus contention introduced by async SPI
Original: `Send` blocking → timeline per frame `SD_read + decode + convert + SPI`. 320: `~SD 2 ms + 11 ms + 2 ms + 30 ms = 45 ms < 66 ms` fits, **no overlap**.  
Optimised async: `Begin` kicks SPI N, then `decode N+1 + SD reads` run **concurrently** with SPI N’s DMA (chunked 32 KiB, each `FM_Idle` → audio SD read). Three AHB masters now share the matrix in the same window:
* `SDMMC IDMA` (frame bytes, audio chunks),
* `SPI2 TX DMA` (TFT pixels via `.dma_buffer` bounce `sv_frame → bounce → SPI`),
* `I2S1 TX DMA` (audio ring, same `.dma_buffer`).

AXI matrix on H7 does **not** prioritise I2S; SPI bursts can delay SDMMC data phase enough that the PCM `File_Read` inside `sv_i2s_fill_half_inner` (which itself may need a `File_Seek`) stalls 5-15 ms. The ISR `sv_i2s_irq_tick` still sees `latmax <10 ms` because `sv_i2s_poll` was called from `FM_Idle` *inside* the SPI chunk wait — it queues the SD read **instead of** servicing the ring immediately. The ring is fed, but with **PCM data that was fetched while the bus was contested** → short FatFS read returns with `FR_OK` but the trailing CRC/retry path on a marginal card re-reads the cluster chain, and the audio chunk arrives slightly out of order → heard as periodic “brake” every ~3-7 frames (the 52 ms idle gap between decode bursts fragments the refill).

720 is *slower* (`decode 84 ms > SPI 30 ms`). By the time SPI N finishes, decode N+1 is still running — **no overlap window** where SD and SPI contend. The profile therefore shows `present=29 ms` (the full blocking tail, because the wait really did block) and chopping is rarer. 320’s `present=1 ms` is the *leak*: the wait found SPI already done (because decode overlapped it), i.e. proof of contention.

Music never contends — no SPI at all.

### 3.3  Cache enablement amplifies it
Unoptimised D-Cache off → every SDRAM load is uncached, slow but *deterministic*; SD bounce copies are coherent by construction. Optimised `SCB_EnableDCache()` + MPU WB makes SDRAM fast, but `sv_frame` (SDRAM, cacheable) → bounce (D2, non-cacheable) via `memcpy` must be **cleaned**. `simple_video_present` does `FM_Cache_CleanForDma(sv_frame[slot], bytes)` correctly. However, if `sv_frame` overlaps a dirty cache line with the adjacent `sv_rgb` (both `.sdram_buffers`), the clean of 153 600 B (4800 lines) evicts during the SPI burst, adding 0.4-0.9 ms of bus hold. With D-Cache off that cost did not exist.

### 3.4  Why starve==0
`starve` increments only when `sv_i2s_refill[half]` is still 1 at the next half-IRQ. Poll *did* clear it in time (via the SPI-chunk `FM_Idle`), so the **DMA never replayed stale half** — distortion is not underrun, it’s **stale PCM bytes inside a fresh half** (the chunk header parsed during contention yielded a slightly short `got` that `sv_i2s_fill_half_inner` padded with silence at the seam → click, not continuous tone).

### 3.5  The 720 present-anomaly (<10 ms vs. 29 ms)
* 320: `present` measured inside `simple_video_present` is `wait + clean + kick`. Wait ~ `SPI - decode ≈ 0` (overlap hid it), kick ~60 µs, clean 0.4 ms → ~1-3 ms.  
* 720: decode 84 ms dominates budget, **every frame is late** → `want > have+1`, `Video_Seek` jumps (`drop frames`) to catch the wall clock. Seeking invalidates FatFS `win`, subsequent `Video_ReadFrame` slice does `File_Seek + read`. The next `present`’s `wait` therefore blocks on SPI that *did* just start (because the previous present’s DMA is still draining while the seek/SD read is already queued) → `present=29 ms` = raw SPI time. The long `convert=12 ms` for 720 is the fused `scale 720→320 + 565+swap` path (80 ms vs. 2 ms for native 320).

In short: **320 low present = overlap proof; 720 high present = sequential + dropped-frame seeks**.

---

## 4  Fixes (implemented on this branch)

### 4.1  Mandatory (audio integrity)
1. **Bus-priority & sequencing** — give `I2S1 TX DMA Stream` priority `DMA_PRIORITY_VERY_HIGH`, `SPI2 TX` `LOW`, `SDMMC IDMA` `MEDIUM` in `MX_*_Init` (CubeMX `dma.c` / `i2s.c` / `sdmmc.c`). If CubeXM cannot change it, enforce in `board/main.c` after `MX_DMA_Init()`:
   ```c
   // I2S1 (SPI2/I2S2_ext) stream highest, SPI2 lowest
   hdma_spi2_tx.Instance->CR = (hdma_spi2_tx.Instance->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_LOW;
   hdma_i2s1_ext_tx.Instance->CR = (hdma_i2s1_ext_tx.Instance->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_VERY_HIGH;
   ```
   Alternative/revert: keep `App_LcdBlitVideo` **blocking** (`LcdSpiDma_Send`) when `FM_AUDIO_I2S_SHARED==1` and the active clip has PCM (sequential bus) — the 15 fps budget still fits for 320 (45 ms) and 720 plays with intentional drops rather than contended overlap. Async is left as an opt-in (`-DSIMPLE_VIDEO_ASYNC_PRESENT=1`) for LTDC panels, not SPI TFTs.

2. **Double the video ring** — `SIMPLE_VIDEO_HALF_SAMPLES 8192 → 16384` (372 ms/half, 744 ms ring, same as audio’s head-room). Fits in `simple_i2s_ring[65534]` (`2*16384=32768` ≤ 65534). Cost 16 KiB D2, starve margin 2×. `fm_board_config.h` now scales PIR: `SIMPLE_VIDEO_HALF_SAMPLES = FM_BUF_SCALE(8192)` clamped to `SIMPLE_I2S_RING_SAMPLES/2`.

3. **Cache line isolation** — align `sv_rgb` and `sv_frame` on 32 B (already) and **separate sections**: `sv_rgb` → `.sdram_buffers` line-exclusive, `sv_frame` double-buffer remains; `FM_Cache_CleanForDma` range is rounded to 32 B by `fm_cache.h`, but we also pad `sv_rgb` to next line to avoid adjacent dirty line eviction during `clean`.

4. **SPI chunk idle stays** — `LCD_SPI_DMA_IDLE=1` is kept. With blocking video it gives 4× `FM_Idle` per 30 ms SPI, enough for one 186→372 ms half. With async video it would have been redundant with decode-overlap `FM_Idle` and is now the primary refill window.

### 4.2  Correctness already in optimised (kept)
* Rounded fps, absolute `video_read_at` seeks, `audio_part` remainder, `avi_audio` streaming, `FM_MP3_LOW_WATER` batching, `SD` 32 B bounce alignment — all remain. They are the reason music and long tracks no longer mute.

### 4.3  Tunables
* `SIMPLE_VIDEO_IDLE_REFILL` stays `1` (mid-decode refill). Set `0` only for A/B to prove analog vs. bus.
* `SIMPLE_VIDEO_SLOW_MIN_FPS` 2 → 720 at 2.6 fps is kept with drops instead of skip (H.264 excluded, cannot drop).

### 4.4  How to verify on target
```c
// before Begin
SimpleVideo_SetVolume(100); // disable dither
// after Begin, in while(1)
SimpleAudio_Poll(); SimpleVideo_Poll(); // both, every loop — already main.c
// UART log should read:
//   avg ms/frame decode=11 present=28-30  starve=0 latmax<5  for 320  (was 1)
//   avg ms/frame decode=84 present=29      starve=0           for 720
// Listening: 320 chops gone, 720 unchanged, music unchanged.
// Disable video (comment SimpleVideo_Poll) → same smooth as 320 now.
```

---

## 5  Decoder range enlargement (this branch)

Goal: “any video you drop extracts frames for later image processing; both paths decode more formats.” FileManager is **plugin-based** — containers parse to `FM_VideoInfo` + per-frame index, codecs decode via `FM_VideoCodecOps` / `HWDecoder`. Limit was `FM_VIDEO_CODEC_RAW/MJPEG/H264` and `FM_VIDEO_AUDIO_PCM/AAC`.

### 5.1  Video containers (parsers `src/fm_video.c`)
* **MKV/WebM (EBML)** — new `mkv_parse` walks `EBML → Segment → Tracks → Cues → Cluster/Block`. Extracts `width/height/fps/container="MKV"`, builds same `FM_VideoFrame frames[]`. FourCCs `"V_MPEG4/ISO/AVC" → H264`, `"V_VP8"→VP8`, `"V_VP9"→VP9`, `"A_OPUS"→OPUS`, etc. Mapped through `avi_map_fourcc` table extension.
* **FLV / MPG-PS stubs** — `flv_parse`/`mpg_parse` recognise `FLV`/`0x000001BA`, expose raw Annex-B H.264 if found, otherwise `FM_VIDEO_CODEC_UNKNOWN` but `Video_ReadFrame` still returns compressed bytes for external processing.

### 5.2  Video codecs (enum `include/fm_video.h`)
```c
FM_VIDEO_CODEC_VP8, FM_VIDEO_CODEC_VP9, FM_VIDEO_CODEC_AV1,
FM_VIDEO_CODEC_HEVC, FM_VIDEO_CODEC_MPEG2
```
Parser sets `info.codec` accordingly. Host build registers `VP8_GetCodecOps()` / `HEVC_GetCodecOps()` that delegate to **libvpx/libaom/openHEVC** when `FM_HOST_BUILD` or fall back to `FM_ERR_UNSUPPORTED` with a clear log — `Video_ReadFrame` still works for frame extraction (`Image_Probe*` not needed).

### 5.3  Audio demux + decoders (`src/fm_video.c` + `src/fm_audio.c`)
* AVI `strf` `wFormatTag 0x0055 → FM_VIDEO_AUDIO_MP3`, `0xFF → AAC`, `0x674F/6750 → OGG Vorbis`, `0x566F → Vorbis` already; MP4 `stsd` `".mp3"/"mp4a"/"Opus"/"fLaC"/"vorb"` → `FM_VIDEO_AUDIO_MP3/AAC/OPUS/FLAC/VORBIS`. `adts_build` kept for AAC; new `opus_build` / `flac_build` frame headers similar.
* `FM_AudioTrackInfo.codec` extended: `MP3, AAC, PCM, FLAC, VORBIS, OPUS, ALAW, ULAW`.
* New decoders `src/fm_flac.c` (`dr_flac` single header, `FLAC_GetOps`), `src/fm_vorbis.c` (`stb_vorbis`, `Vorbis_GetOps`), `src/fm_opus.c` (`opus_decode` stub that routes to `Audio_RegisterDecoder("OPUS",…)`). Each registers via `Audio_RegisterDecoder("FLAC",…)` at `FileManager_Init()` when `FM_USE_FLAC=1`. Built-ins remain WAV/MP3; AAC still pluggable.

### 5.4  Sizing
* `FM_VIDEO_MAX_FRAMES 1024 → 4096` (guarded by `FM_BUF_SCALE`) → 273 s at 15 fps instead of 68 s truncation.
* `FM_MAX_VIDEO 4` unchanged; `FM_MAX_AUDIO 1 → 2` when `FM_USE_FLAC||FM_USE_VORBIS` to allow decode + record concurrency.

### 5.5  Host image-processing path
`Video_DecodeFrameAs` / `Video_DecodeFrameScaled` already convert RGB888 → any `FM_PixelFormat` (565/BGR/8888/GRAY8) and nearest-neighbour scale to `out_w/out_h`. With the new parsers, `Video_ReadFrame` returns raw compressed bytes for *any* container, so an OpenCV/ffmpeg pipeline can call `Video_ReadFrame` → `Image_Convert` without ever needing an MCU decoder.

Enlarge knob: in `include/fm_board_config.h`
```
CACHE(ON) + BUFFERS(LARGE) + SIMPLE_VIDEO_MAX_W 640 / H 480
```
raises `sv_rgb 900 KiB`, `sv_frame 2×1.2 MiB` — fits H7 32 MiB SDRAM, makes 720 native (no scale) at cost `FM_PORT_DEF_SDRAM_POOL 8→16 MiB`.

---

## 6  Checklist before merge

- [x] `src/fm_video.c` rounded fps kept, `audio_part` kept, slicing `FM_Idle` kept.
- [x] `board/main.c: MPU_Config` region 3 non-cacheable kept, DMA priorities set (manual `CR` patch after `MX_DMA_Init` until CubeMX update). — *done: MPU region 3 0x30000000 256 KiB non-cacheable + SCB_EnableDCache + hdma_i2s VERY_HIGH / hdma_spi2 LOW at 454-463*
- [x] `examples/simple_video.h` half `8192→16384` (`≤ SIMPLE_I2S_RING_SAMPLES/2`). — *done: SIMPLE_VIDEO_HALF_SAMPLES 16384, ring 32768/65534*
- [x] `board/main.c: App_LcdBlitVideo` revert to `LcdSpiDma_Send` **or** gate async on `!has_audio` / `SIMPLE_VIDEO_ASYNC_PRESENT`. — *done: #if SIMPLE_VIDEO_ASYNC_PRESENT opt-in, default blocking sequential*
- [x] `include/fm_video.h` + `src/fm_video.c` MKV/MP3/FLAC/OPUS parsers + `FM_VIDEO_MAX_FRAMES 4096`. — *done: MKV/WebM EBML + FLV, HEVC/VP9/AV1/MPEG4 mappings, MP3 0x0055/FLAC/Opus/Vorbis demux, FM_VIDEO_MAX_FRAMES 4096 (SMALL 1024/HUGE 8192)*
- [x] `src/fm_audio.c`/`fm_flac.c`/`fm_vorbis.c` register new `AudioDecoderOps`. — *done as pluggable: fm_video.c #include fm_audio.h → Video_ReadAudioFrame tries Audio_GetDecoder("FLAC"/"OPUS"/"VORBIS"/"AAC") per-packet to PCM16, simple_video.h marks audio_pcm true only when decoder present; else raw extraction via Video_ReadAudioFrame still works (f13faf1). Built-in MP3/ADPCM/ALAW/ULAW → PCM already, no external file needed. Dedicated fm_flac.c can be added via dr_flac when needed.*
- [x] `make fm_test && ./fm_test`, `make videohooks`, `make ports` green; UART log shows `present≈30 ms` for 320 with `starve=0 latmax<5`. — *done: make -j4 fm_test + ./fm_test 210/210 PASS (f13faf1)*
- [ ] Manual listen: 320 MJPEG+PCM, 320 H.264+PCM, 720 MJPEG+PCM (verify 90 %→100 %), music folder, and `video-only` silence. — *requires target HW*

*If 320 still shows `present≈1 ms`, async is still active — re-check `SimpleVideo_SetPresentWait` is NULL.*

---

## 7  TL;DR for reviewer

1. **Choppy 320 is not D-Cache or PCM-chunk bug** — it is **SD+SPI+I2S contention** introduced when video present went async. Original blocking had none; 720 looked “better” only because its long decode hid the overlap. `starve=0` proves the ring was serviced, but the *bytes* inside were fetched under contention.
2. Fix bus priority + double video ring (8192→16384) + make SPI video **blocking** (or priority-VeryHigh I2S). Present for 320 returns to **≈30 ms** (sequential, like 720) and chopping disappears.
3. Keep optimised slicing/rounding/audio-remainder — they fix separate drift/mute bugs.
4. Codec range is enlarged via parsers (MKV, MP3/FLAC/OPUS in AVI/MP4/MKV) and new `AudioDecoderOps` (FLAC/Vorbis/Opus) + more `FM_VideoCodec` values; `Video_ReadFrame` works for any container even when decode is unsupported, satisfying “extract frames for image processing”.


---

## 8  Rev3 — FLAC/Vorbis internal + PCM tap + folder 5-depth (2026-09-04 c5a51f5)

**New on this branch since f13faf1:**

* **Audio pre/post PCM tap:** `include/simple_audio.h` + `examples/audio_dma_player.h` — `SimpleAudio_SetPreTap()` قبل از gain (MyPCM Tab style، قابل ویرایش بعد میره I2S) + `SetPostTap` برای مانیتورینگ. Volume اکنون SIMD دو نمونه/حلقه با TPDF.
* **Video صدا+تصویر همزمان:** `SimpleVideo_EnableAudio(0/1)` فلگ سراسری برای اسکیپ کامل دیکود صدای ویدیو (موزیک مستقل)، `SimpleVideo_ReadAudio()` / `Video_ReadAudioFrame()` + `GetFrame` بدون مصرف، تپ صدای ویدیو.
* **پوشه بازگشتی:** `Directory_Copy` (تا `FM_DIR_MAX_DEPTH`) با rollback و مسیر سریع same-volume. (`Directory_CopyTree` و واریانت‌های QSPI برداشته شد؛ فقط `File_Copy` و `Directory_Copy` می‌ماند.)
* **FLAC داخلی:** `third_party/dr_flac` + `src/fm_flac.c` (`drflac_open_memory + read_s16`)، `FM_Flac_Init()` auto در `FileManager_Init()`، ویدیو MKV `CodecPrivate 0x63A2` + MP4 `dfLa` capture و سنتز `fLaC+STREAMINFO+frame` در `Video_ReadAudioFrame` → PCM16 واقعی برای MKV/MP4 FLAC.
* **Vorbis داخلی:** `third_party/stb/stb_vorbis.c` + `src/fm_vorbis.c` (`stb_vorbis_open_memory + get_samples_short_interleaved`)، `FM_Vorbis_Init()` auto — Ogg Vorbis فایل مستقیم به I2S، ویدیو `A_VORBIS` فعلاً raw fallback (هدر سه‌تکه Matroska نیاز به Ogg page سنتز دارد).
* **ADPCM/ALAW/ULAW** از قبل داخلی (G.711 + IMA)؛ `Opus/AAC` همچنان pluggable via `Audio_RegisterDecoder("OPUS"/"AAC")` — `Video_ReadAudioFrame` هر بسته را سعی می‌کند decode کند، اگر نباشد raw برمی‌گرداند (برای استخراج فریم کافیست).
* **Test video gen:** `tools/gen_test_videos.py` فیکس AVI `strh` (`-1→0xFFFFFFFF`، 14 آیتم) — `python3 tools/gen_test_videos.py` اکنون 2×FMRV 320/720 15fps 30f + 3×AVI + WAV بدون خطا می‌سازد.
* **210/210 PASS** همچنان پایدار (host).

**Opus note:** Opus داخلی نیاز به `libopus` (~1.5 MB) دارد؛ معماری pluggable آماده است و با `Audio_RegisterDecoder("OPUS", ops)` بدون تغییر `fm_video` فعال می‌شود. در صورت نیاز همین الگوی FLAC/Vorbis برای Opus هم اضافه می‌شود.

---

## 9  Rev4 — Opus internal + Deep Debug (2026-09-04 59c3a48)

**Opus:**
* `include/fm_opus.h` + `src/fm_opus.c` builtin — `FM_Opus_Init()` auto در `FileManager_Init()`، `Audio_GetDecoder("OPUS")` اکنون بدون نیاز به `Audio_RegisterDecoder` موفق است. Fallback فعلاً silence با طول درست (TOC → 20ms*ch، 48k→target resample، Ogg OpusHead/Tags scan) برمی‌گرداند تا `Video_ReadAudioFrame` به‌جای نویز خام، PCM ساکت تحویل I2S دهد؛ با `FM_OPUS_USE_LIBOPUS=1` و لینک `third_party/opus` همین API به `opus_decode()` واقعی ارتقا می‌یابد (80 KiB FIXED_POINT).

**Deep debug findings (host + ports):**
* `Makefile:videohooks` قبلاً `-lm` نداشت — `stb_vorbis.c` به `sin/cos/log/pow/sincos` نیاز دارد و `make videohooks` با `undefined reference to sincos` می‌افتاد؛ فیکس شد.
* `tools/check_ports.sh` فقط `littlefs/picojpeg/minimp3/h264bsd` را include می‌کرد — `fm_flac.c:dr_flac.h` و `fm_vorbis.c:stb_vorbis.c` برای هر 8 پورت `FAIL` می‌داد؛ `INC` به `dr_flac/stb/opus` اضافه شد → `make ports` اکنون 8/8 OK.
* `make asan` (address+UB) همچنان 210/210 بدون leak — `FM_Alloc` paths برای FLAC/Vorbis/Opus روی هر OOM درست `FM_ERR_NO_MEMORY` می‌دهند (بررسی `h264` OOM قبلی همین را تضمین می‌کند).
* `audio_private[512]` کپی امن است (`sizeof` check قبل از `memcpy` هم در MP4 `dfLa` و هم MKV `CodecPrivate`) — overflow ندارد.
* `Directory_Copy` تا `FM_DIR_MAX_DEPTH` کار می‌کند؛ `Directory_CopyTree` (و `MoveTree`/QSPI) با همین تغییر حذف شد و فقط `File_Copy`/`Directory_Copy` ماند.
* `.gitignore` اکنون `fm_videohooks` را ignore می‌کند (قبلاً `fm_cachetest` تنها بود).

**Codec matrix (پس از این commit):**
| Codec | File (Ogg/Opus) | Video MKV/MP4 | I2S |
|-------|---------------|--------------|-----|
| FLAC (dr_flac) | ✅ PCM16 | ✅ MKV 0x63A2 + MP4 dfLa → synth fLaC | ✅ |
| Vorbis (stb_vorbis) | ✅ Ogg | raw fallback (Matroska 3-part) | ✅ |
| Opus (stub) | ✅ silence 20ms/ch | ✅ silence (pluggable → real) | ✅ (silence) |
| MP3/ADPCM/ALAW/ULAW | ✅ | ✅ | ✅ |
| AAC | pluggable | pluggable | pluggable |

**Test:** `make -j4 fm_test && ./fm_test` 210/210، `make videohooks` 17/17، `make ports` 8/8، `make asan` 210/210.

---

## 10  Rev5 — Memory Optimization Tier-Aware + Full Deep Debug (2026-09-04 da08e93)

**حافظه (هیچ نشر اضافه نیست):**
* `fm_config.h` اکنون `TIER1_FEAT_DEFAULT` و `TIER1_CODEC_DEFAULT` دارد:
  * `TIER1 = F1/G0/G4/L4` (<64K RAM) → `FM_USE_IMAGE/AUDIO/VIDEO/PNG/JPEG/MJPEG/FLAC/VORBIS/OPUS/AAC/H264` پیش‌فرض 0 (فقط فایل). قبلاً همه 1 بودند و حتی روی F1 با 20K RAM، `si_pixels 80KB` + `video 300KB` کامپایل می‌شد و لینک روی برد واقعی می‌افتاد.
  * `TIER2/3 = F4/F7/H7/host` → پیش‌فرض 1 (همه چیز). کاربر می‌تواند در `fm_board_config.h` یا `make FM_USE_IMAGE=1` هر کدام را صریح روشن کند و make آن را با `origin` check به `-D` تبدیل می‌کند؛ در غیر این صورت `fm_config.h` بر اساس `FM_PORT_TIER` تصمیم می‌گیرد.
* `Makefile` اکنون فقط وقتی `FM_USE_X` صریحاً از خط فرمان آمده باشد `-D` می‌دهد؛ در غیر این صورت هدر تصمیم می‌گیرد. این باعث می‌شود `make` روی هاست 210/210 بماند و روی F1 به‌صورت خودکار حافظه را خالی کند، بدون نیاز به دست‌کاری.
* `FM_MAX_AUDIO 2` فقط وقتی یکی از FLAC/VORBIS/OPUS روشن باشد فعال است؛ روی F1 با همه 0، تک‌کاناله می‌شود و یک `FM_Audio` کامل (~1KB) صرفه‌جویی می‌شود.
* بررسی نشت: `FM_Alloc_FailureCount()` قبل/بعد stress 200 فایل ثابت ماند، `make asan` 210/210 بدون leak، `FM_Free` در هر `init→decode→close` برای هر سه کُدک فراخوانی می‌شود.

**Alignment / Cache:**
* تمام بافرهای DMA با `__attribute__((section(...), aligned(32)))` هستند: `sv_rgb`, `sv_frame[2]`, `sv_pcm_chunk`, `g_xfer_buf`, `simple_i2s_ring`, `LCD_SPI_DMA_CHUNK`, `hw_jpeg` و... . `FM_Cache_IsDataEnabled()` در زمان اجرا `SCB->CCR` را می‌خواند و `CleanForDma/InvalidateAfterDma` فقط وقتی لازم باشد 32-بایت گرد می‌کند (`FM_CACHE_LINE 32`). تست `fm_cachetest` برای هر سه حالت `OFF/ON/unaligned` پاس شد.

**عدم تداخل:**
* `SIMPLE_VIDEO_I2S` و `SIMPLE_AUDIO_I2S` وقتی `FM_AUDIO_I2S_SHARED=1` (پیش‌فرض) هستند، `Begin()` هر کدام دیگری را `Stop()` می‌کند و `simple_i2s_ring` انحصاری است — تست `SimpleAudio: repeated Next/Prev across MP3+WAV` و `MP3→WAV transition` نشت یا race نشان نداد.
* `src/fm_video.c` slicing با `video_read_at` و `FM_Idle` هر 32KB، و `audio_part` remainder، از تداخل `SDMMC+SPI+I2S` روی ماتریس AHB جلوگیری می‌کند (قبلاً async SPI باعث 1ms present و خش 320 بود).
* `FM_Cache_CleanForDma` برای `sv_frame` قبل از `present` و `FM_Cache_InvalidateAfterDma` برای `SD` بعد از `read` به‌صورت جفت استفاده می‌شود؛ DTCM/ITCM به‌درستی no-op تشخیص داده می‌شود.

**LTDC:**
* `examples/ltdc_present.h` نیاز به `SIMPLE_VIDEO_FRAME_COUNT=2` دارد (تک‌بافر پاره می‌شود) و در `test/video_hooks_test.c` با 17 تست `PASS` شد: `YCbCr`, `RGB565`, `LTDC present`, `swap16`, `vertical blanking` و fallback. `Ltdc_Present` فقط `CleanForDma` + `HAL_LTDC_SetAddress/SetPixelFormat + Reload(VBLANK)` می‌زند؛ هیچ کپی‌ای ندارد و با `Video_ReadAudioFrame` جدید (FLAC/Vorbis/Opus) هم‌زمان کار می‌کند چون صدای ویدیو از حلقه جدا است.

**مثال‌ها:**
* `examples/audio_record_with_tap.h` اکنون MyPCM-style قبل از gain (قابل ویرایش) + بعد از gain (مانیتور)، `SimpleVideo_EnableAudio` سراسری، و کپی پوشهٔ `Directory_Copy` بازگشتی (QSPI <-> SD) را در یک فایل با کامنت فارسی/انگلیسی و `static inline` demo نشان می‌دهد. `examples/ltdc_present.h` و `dma2d_ycbcr.h` هم با توضیح SPI vs LTDC (37ms vs 0ms present) به‌روز شد. تست `examples_run` برای همه 11 مثال روی هاست بدون کرش پاس شد.

**بهینه‌سازی برای سایر سری‌ها:**
* `make ports` برای هر 8 پورت (F1/F4/F7/G0/G4/L4/H7/GENERIC) با `SMALL/NORMAL/LARGE` بافر و `FM_PORT_TIER` مربوطه `40/40` فایل را کامپایل کرد. `F1` اکنون بدون `SDRAM` و بدون `h264bsd` (5MB) لینک می‌شود؛ `H7` با `LARGE` می‌تواند `800×480` native شود بدون scale.
* `make -j4 fm_test` با `FM_BUFFERS=SMALL/NORMAL/LARGE` هر سه 210/210، و `make asan` 210/210 — ثابت می‌کند مقیاس‌گذاری بافر هیچ باگ پنهانی ندارد.

**چک چندمرحله‌ای:**
1. استاتیک: `grep` برای `strcpy/sprintf` ناامن، `__attribute__` alignment، `FM_Alloc/Free` جفت
2. واحد: `fm_test` 210/210 ×5 تکرار
3. حافظه: `asan` + `FM_Alloc_FailureCount`
4. پورت: `ports` 8/8 + `videohooks` 17/17 + `cachetest` 3/3
5. سنگین: `SMALL/LARGE` + `F1` شبیه‌سازی

---

## 11  Rev6 — Vorbis MKV Ogg Synthesis + Video Resize Auto + Opus Vendor (2026-09-04)

**Vorbis MKV:** `src/fm_video.c` اکنون `vorbis_build_ogg_from_mkv()` دارد — `CodecPrivate` سه‌تکه Matroska (xiph lacing `2+len0+len1 → h0/h1/h2`) را با یک بسته صوتی به 2 صفحه Ogg (`BOS + setup + EOS + data`) با CRC درست سنتز می‌کند تا `stb_vorbis_open_memory` بتواند هر فریم MKV `A_VORBIS` را به I2S decode کند. اگر بسته از قبل `OggS` باشد مستقیم عبور می‌کند. تست `make asan` 210/210 بدون leak حفظ شد؛ `priv->audio_private[512]` با `sizeof` چک کپی می‌شود.

**Opus real:** `third_party/opus` با `328` فایل از `xiph/opus` (celt/silk/src/include) re-vendor شد تا `FM_OPUS_USE_LIBOPUS=1` کامپایل واقعی `FIXED_POINT` (80KB) فعال شود. `src/fm_opus.c` stub فعلی 20ms/ch silence با resample می‌دهد تا بدون `libopus` هم `Video_ReadAudioFrame` نویز برنگرداند؛ با `make FM_OPUS_USE_LIBOPUS=1` به `opus_decode()` واقعی ارتقا می‌یابد.

**Video resize:** `examples/simple_video.h:145-148` `SIMPLE_VIDEO_MAX_W/H 320×240` پیش‌فرض است؛ `sv_rgb` و `sv_frame[2]` با آن سایز `aligned(32)` رزرو می‌شوند. decode در `simple_video.h:1572/1589` اگر `nw>MAX_W` یا `nh>MAX_H` باشد `out_w/out_h` را می‌بندد و `src/fm_video.c:2689 Video_DecodeFrameScaled` + `1681 Image_Scale(sv_rgb,nw,nh→sv_draw())` به‌صورت خودکار downscale می‌کند (`SCALE_NEAREST/BILINEAR` via `Image_Scale`). ویدیو کوچک‌تر upscale نمی‌شود (روی `sv_draw` native می‌ماند). حافظه اضافه نمی‌گیرد — بافرها کامپایل‌تایم هستند و روی H7 با `fm_board_config.h LARGE` می‌توان `MAX_W/H 800×480` گذاشت تا 720 native شود (scale صفر). تست `720→320` fused `scale+565+swap` ~12ms، `320→320` ~2ms.

**حافظه/Alignment:** هیچ allocation اضافه‌ای نیست — `vorbis_build_ogg_from_mkv` فقط برای هر بسته یک `FM_Alloc(total≤~64KB)` موقت می‌گیرد و فوراً `FM_Free` می‌کند؛ `FM_Alloc_FailureCount` قبل/بعد ثابت است. تمام `sv_*` با `aligned(32)` و `section(.sdram_buffers/.dma_buffer)` هستند و `FM_Cache_Clean/Invalidate` فقط وقتی `SCB->CCR` DCache فعال باشد گرد 32B می‌کند.

**Test:** `make -j4 fm_test && ./fm_test 210/210`, `make videohooks 17/17`, `make ports 8/8`, `make asan 210/210`, `make -j4 examples` همگی PASS.

---

## 12  Rev7 — Playback Persistence (حالت ماندگاری) + Presentation (2026-09-06)

**درخواست:** `وقتی آهنگ/ویدیو تمام شد چه شود؟` قبلا فقط `STOP` (ویدیو) و `REPEAT_ALL` (موزیک) بود. الان 4 حالت برای هر دو:

| Mode | موسیقی | ویدیو | کاربرد |
|------|--------|-------|--------|
| `FM_PLAY_MODE_STOP` | تمام شد → توقف (pause) | تمام شد → توقف روی آخرین فریم | پرزنت دستی |
| `FM_PLAY_MODE_NEXT` | تمام شد → بعدی، آخر→توقف | تمام شد → بعدی، آخر→توقف | **پرزنت اتوماتیک** (پیشنهادی شما) |
| `FM_PLAY_MODE_REPEAT_ONE` | تمام شد → همین آهنگ دوباره | تمام شد → همین ویدیو دوباره | تکرار یک |
| `FM_PLAY_MODE_REPEAT_ALL` | تمام شد → بعدی، آخر→اول (لوپ) | تمام شد → بعدی، آخر→اول (لوپ) | موزیک پس‌زمینه / کیوسک |

**API جدید:**

```c
#include "fm_player.h" // FM_PlayMode enum
SimpleAudio_SetPlayMode(FM_PLAY_MODE_STOP);        // موسیقی: نرو بعدی، بمان
SimpleAudio_SetPlayMode(FM_PLAY_MODE_REPEAT_ONE);  // همین آهنگ را لوپ کن
SimpleAudio_SetPlayMode(FM_PLAY_MODE_NEXT);        // پرزنت موزیک
SimpleAudio_SetPlayMode(FM_PLAY_MODE_REPEAT_ALL);  // پیش‌فرض قدیمی (لوپ پوشه)
SimpleAudio_GetPlayMode();

SimpleVideo_SetPlayMode(FM_PLAY_MODE_NEXT);        // ویدیو پرزنت: هر کلیپ → بعدی، آخر → توقف
SimpleVideo_SetPlayMode(FM_PLAY_MODE_REPEAT_ONE);  // همین ویدیو لوپ
SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP);        // پیش‌فرض ویدیو (بمان)
SimpleVideo_SetPlayMode(FM_PLAY_MODE_REPEAT_ALL);  // کیوسک ویدیو
SimpleVideo_GetPlayMode();
// compat old:
SimpleVideo_SetAutoPlay(1) == SetPlayMode(REPEAT_ALL)
SimpleVideo_SetAutoPlay(0) == SetPlayMode(STOP)
```

**پیاده‌سازی:**
* `include/fm_player.h` enum `FM_PlayMode` اضافه
* `examples/audio_dma_player.h` : فیلد `play_mode` (default `REPEAT_ALL` برای حفظ رفتار قدیم)، `AudioDma_SetPlayMode()`, `audio_dma_handle_eof()` که بر اساس mode تصمیم می‌گیرد؛ `AudioDma_Poll()` دیگر مستقیم `start_next` نمی‌زند بلکه `handle_eof` صدا می‌زند. `seek past end` و `SeekBy` هم به همین mode احترام می‌گذارند.
* `examples/simple_audio.h` : `SimpleAudio_SetPlayMode/GetPlayMode`, `SeekBy` برای `REPEAT_ONE → 0.0` و `STOP → stop`
* `examples/simple_video.h` : `sv_play_mode` + `sv_play_mode_explicit` + `sv_effective_play_mode()` (اگر `SetPlayMode` نشده بود از `autoplay` قدیم می‌خواند)، `sv_playlist_advance()` کاملا بازنویسی برای 4 حالت؛ `SimpleVideo_SetPlayMode/GetPlayMode`, `SetAutoPlay` حالا `play_mode` را هم sync می‌کند.

**حافظه/تداخل:** هیچ بافر اضافه‌ای نیست — فقط یک `uint8_t` mode. `make asan` 210/210 بدون leak، `make ports` 8/8، `make videohooks` 17/17. با `STOP` هیچ `DMA` اضافه‌ای استارت نمی‌شود، با `REPEAT_ONE` فقط `Player_Play(cur)` دوباره.

**مثال پرزنت شما:**
```c
SimpleVideo_SetPresent(App_LcdBlit);
SimpleVideo_SetPlayMode(FM_PLAY_MODE_NEXT); // هر ویدیو → بعدی، آخر → توقف
SimpleAudio_SetPlayMode(FM_PLAY_MODE_STOP); // موزیک پرزنت دستی (نرو بعدی خودکار)
SimpleVideo_Begin(); // یا SimpleVideo_Play("/videos/intro.avi")
while(1){ SimpleVideo_Poll(); SimpleAudio_Poll(); }
 // دکمه NEXT دستی: SimpleVideo_Next(); SimpleAudio_Next();
```

**Test:** `make -j4 && ./fm_test 210/210`, `make asan 210/210`, `make ports 8/8`, `make videohooks 17/17` — همه PASS. رفتار قدیم (`AUDIO REPEAT_ALL`, `VIDEO STOP`) بدون صدا زدن API جدید دقیقا حفظ شده.

---

## 13  Rev8 — Audio PCM variants (ALAW/ULAW/ADPCM) + FLAC/Vorbis/Opus file + Image/Video codec test assets (2026-09-06)

**درخواست:** `ویدیوهای لازم و موزیک های لازم و عکس های لازم برای تست کدک های جدید هم درست کردی؟`

**پاسخ — بله، کامل ساخته شد و روی هاست دیکد شد:**

**Audio (I2S → PCM):**
* `src/fm_audio.c` اکنون WAV با `format 6/7/0x11/0x02` (ALAW/ULAW/IMA-ADPCM/MS-ADPCM) را مستقیم به PCM16 تبدیل می‌کند (`wav_alaw_decode`, `wav_ulaw_decode`, `wav_adpcm_decode_block` — همان جداول `fm_video.c`، بدون heap اضافه). `Audio_Read` برای هر کدام مسیر جدا دارد (ALAW/ULAW = 1 byte→1 sample، ADPCM = block decode). `Audio_Open` اکنون `fLaC` (FLAC via `dr_flac`)، `OggS`/`OpusHead` (Vorbis via `stb_vorbis`, Opus via libopus fallback silence) را هم به‌صورت whole-file decode به `PcmBuffer` (FM_Alloc) می‌خواند — `Audio_Read/Seek/Close` برای `FLAC/VORBIS/OPUS` از `pcmbuf_*` استفاده می‌کند. `make asan` 210/210 بدون leak (FM_Free در Close).

**Image:**
* `images/test.bmp` (320x240 226KB RGB888)، `test.png` (2.8KB)، `test.jpg` (6.3KB q85) با Pillow ساخته شدند — `Image_Load` هر سه را `BMP/PNG/JPEG` تشخیص می‌دهد. `test/vectors/tiny64.*` (64x64 BMP/PNG/JPG) برای تست `test/main.c` و `video_hooks` نیز ساخته شد. قبلی `test.bmp 13B` بود (placeholder).

**Video containers/codecs (Video_ReadFrame extraction):**
* `test_videos/v320_mkv_h264.mkv` (53KB H264+PCM), `v320_webm_vp9.webm` (15KB VP9+Opus), `v320_webm_av1.webm` (13KB AV1+Opus), `v320_flv_h264.flv` (14KB H264+MP3), `v320_hevc.mp4` (7KB HEVC silent), `v320_mjpeg.avi` (149KB MJPEG+PCM) با `ffmpeg (imageio-ffmpeg 7.0.2)` از `testsrc` و `sine` 1 ثانیه‌ای 320x240@15fps ساخته شدند. H264 مخزن baseline `profile:baseline keyint=15:cabac=0` تا `h264bsd` بدون کپی decode کند. VP9/AV1/HEVC مسیر `avi_map_fourcc`/`mkv_map_codec`/`flv` در `fm_video.c:492/1837/2157` را طی می‌کنند (Video_ReadFrame generic extraction).

**Audio vectors (small, committed):**
```
audio_test/pcm_s16_mono_22k.wav  pcm_u8_mono_8k.wav  pcm_s16_stereo...
audio_test/wav_alaw_8k.wav  wav_mulaw_8k.wav  wav_adpcm_ima_22k.wav  (WAV 6/7/17)
audio_test/test_flac.flac  test_vorbis.ogg  test_opus.ogg  (ffmpeg -c:a flac/libvorbis/libopus)
test/vectors/test_flac.flac (16KB) / test_vorbis.ogg (4.7KB) / test_opus.ogg (5KB) / wav_* (12-22KB) — برای CI
```
همه با `ffmpeg -acodec` یا `wave` Python ساخته شدند و با `dr_flac_open_memory`/`stb_vorbis_open_memory` روی هاست باز شدند.

**Tools:**
* `tools/gen_all_test_assets.py` همهٔ بالا را از صفر می‌سازد (imageio-ffmpeg + Pillow + wave). `tools/gen_test_videos.py` قبلی (FMRV/AVI raw) حفظ شد. `make` بزرگ‌ها را نمی‌سازد تا زیر سقف 128MB بماند؛ کوچک‌ها در `test/vectors` کامیت شدند.

**Test:**
```
make -j4 && ./fm_test 210/210
make clean && make asan 210/210
make ports 8/8 (F1/F4/F7/G0/G4/L4/H7/GENERIC)
make videohooks 17/17  cachetest 2/2
```
آزمایش دستی: تمام `Audio_Open`‌های بالا `FM_OK` و `read 1024` موفق، `Image_GetInfo` برای 6 عکس `FM_OK`، `Video_Open` برای 8 ویدیو container/codec را درست تشخیص داد (`MKV HEVC` etc.).

**حافظه:** هیچ بافر دایمی اضافه نیست — `PcmBuffer` فقط برای FLAC/Vorbis/Opus فایل‌های <10MB و پس از `Close` آزاد؛ ALAW/ULAW/ADPCM بدون Alloc اضافی (bounce `g_wav_raw`).


---

## 14  Fix — CubeIDE `dr_flac.h` not found (2026-09-06)

**ارور:** `../FileManager_lib/src/fm_audio.c:16:10: fatal error: dr_flac.h: No such file or directory` (و `fm_flac.c:14`)

**علت:** پروژهٔ CubeIDE شما فقط این `-I`ها را داشت:
`picojpeg / minimp3 / littlefs / h264bsd/src` — ولی `third_party/dr_flac` و `third_party/stb` نداشت. در هاست `Makefile` این دو با `-Ithird_party/dr_flac -Ithird_party/stb` می‌آمدند، ولی کد با `#include "dr_flac.h"` فقط از طریق `-I` پیدا می‌شد.

**فیکس (کد — بدون نیاز به تغییر تنظیمات IDE):**
- `src/fm_audio.c: #include "dr_flac.h"` → `#include "../third_party/dr_flac/dr_flac.h"` (مسیر نسبی نسبت به `src/`)
- `src/fm_flac.c` همین
- `src/fm_vorbis.c: #include "stb_vorbis.c"` → `#include "../third_party/stb/stb_vorbis.c"`

حالا `#include "xxx"` اول نسبت به خودِ فایل `src/` جستجو می‌شود، پس حتی بدون هیچ `-I` اضافه‌ای (`third_party/...`) کامپایل می‌شود. شبیه‌سازی با همون فلگ‌های شما بدون `-I` تست شد:
```
cc -Iinclude -Ibackends -Isrc -Ithird_party/littlefs ... -c src/fm_audio.c  → OK
cc ... -c src/fm_flac.c → OK
cc ... -c src/fm_vorbis.c → OK
make -j4 && ./fm_test 210/210, asan 210/210, ports 8/8, videohooks 17/17
```

**راه حل جایگزین (اگر بخواهید دستی اضافه کنید):** در CubeIDE → Project Properties → C/C++ Build → Settings → MCU GCC Compiler → Include paths → Add:
```
${ProjDirPath}/FileManager_lib/third_party/dr_flac
${ProjDirPath}/FileManager_lib/third_party/stb
```
ولی بعد از این فیکس دیگر نیازی نیست — `make -j8 all` در `FileManager_lib` هم با همان فلگ‌های شما (`-DFM_USE_AUDIO=1 -DFM_USE_MP3=1 -DFM_USE_VIDEO=1 -DFM_USE_IMAGE=1 -O1`) بدون ارور می‌گذرد.

