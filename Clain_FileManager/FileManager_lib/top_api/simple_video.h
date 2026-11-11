/*
 * Example 19 — SimpleVideo: «ویدئو ساده» — آلبوم + فریم TFT + همگام صدا
 *
 * سبک SimpleAudio: دکمه‌ها را صدا می‌زنید، Poll در while(1) فریم را
 * سرِ زمان fps decode می‌کند. صدا و تصویر روی یک ساعت (HAL_GetTick)
 * جلو می‌روند؛ اگر ترک PCM در کانتینر باشد با همان زمان Seek می‌شود.
 *
 *   SimpleVideo_SetPresent(my_lcd_blit);   // DMA: example 21
 *   SimpleVideo_SetOutputSize(320, 240);
 *   SimpleVideo_Begin();
 *   while (1) { LcdSpiDma_Poll(); SimpleVideo_Poll(); }
 *   SimpleVideo_Next(); SimpleVideo_SeekBy(+10);  — اعمال در Poll
 *   SimpleVideo_Pause(); SimpleVideo_Resume();    — Pause مثل صدا
 *   SimpleVideo_SetVolume(50);                    — صدای نرم‌افزاری 0..100
 *
 * FPS — پخش async روی SPI (پیشنهادشده):
 *   اگر blit شما LcdSpiDma_Send بلاک‌شونده است، هر فریم حدود زمان SPI
 *   (برای 320x240@565 حدود 100ms) بلاک می‌شود. برای هم‌پوشانی انتقال
 *   SPI فریم N با decode فریم N+1 دو خط را عوض کنید:
 *
 *     void my_lcd_blit(...) {
 *         ILI9341_Set_Address(...);
 *         HAL_GPIO_WritePin(LCD_DC..., GPIO_PIN_SET);
 *         LcdSpiDma_Begin(px, bytes);      // به‌جای LcdSpiDma_Send
 *     }
 *     SimpleVideo_SetPresent(my_lcd_blit);
 *     SimpleVideo_SetPresentWait(LcdSpiDma_Wait);
 *
 *   آن‌وقت بودجهٔ هر فریم = max(decode+convert, SPI) می‌شود نه جمع‌شان.
 *   بقیهٔ سرعت هم در prescaler ساعت SPI است (پنل‌های ILI9341 معمولاً
 *   تا 40-50MHz رایت را تحمل می‌کنند).
 *
 * مقاوم‌سازی H.264:
 *   یک فریم خراب/جاافتاده زنجیرهٔ مرجع P-frame ها را تا IDR بعدی
 *   می‌شکند. پلاگین (fm_h264.c) آخرین keyframe را کش می‌کند و بعد از
 *   خطا decoder را reset و keyframe را دوباره می‌دهد؛ اینجا هم تا
 *   SIMPLE_VIDEO_H264_FAIL_TOLERANCE فریم خراب در یک فایل تحمل می‌شود
 *   (تصویر لحظه‌ای ثابت می‌ماند و در IDR بعدی وصل می‌شود) — فایل و
 *   پلی‌لیست نمی‌میرند. Begin هم مثل Poll فایل‌های خراب را رد می‌کند.
 *
 * فایل‌های «کندتر از real-time» (مهم!):
 *   دیکد H.264 نرم‌افزاری روی MCU کُند است (H7@-O1 برای 320x240 حدود
 *   300-500ms بر فریم ⇒ ~2-3fps). با SIMPLE_VIDEO_SKIP_SLOW=1 (پیش‌فرض)
 *   بعد از 6 فریم محک، اگر میانگین هزینهٔ decode+convert بیش از
 *   SIMPLE_VIDEO_SLOW_FACTOR× بودجهٔ فریم باشد، آن فایل یک‌بار با لاگ
 *   شفاف رد می‌شود تا پلی‌لیست روان بماند (برای real-time روی MCU:
 *   MJPEG q6 در 320x240؛ با صدا: AVI+PCM). اگر می‌خواهید فایل‌های کند
 *   هم پخش شوند (تصویر واضح ولی آهسته/سینمایی-مکثی)، SKIP_SLOW=0 بگذارید —
 *   فایل‌های بی‌صدا با pacing خودکار در حداکثر سرعتِ ممکن و روان پخش
 *   می‌شوند (هر فریم نمایش داده می‌شود، نه skip)؛ فایل‌های صدادار به
 *   ساعت صدا قفل می‌مانند.
 *
 * فرمت‌هایی که باز می‌شوند (گسترش‌یافته 2026-09):
 *   FMRV     — خام RGB888 (built-in، خواندن+نوشتن)
 *   AVI      — ویدیو MJPEG / H.264 / MPEG-4 (XVID/DIVX/MP4V) / VP8 / VP9 / AV1 / HEVC
 *              + صدای PCM / MP3(0x0055) / ADPCM / aLaw/µLaw / AAC / Vorbis
 *              — PCM 16-bit از بلندگو (I2S) پخش می‌شود، بقیه demux برای پردازش فریم
 *   MP4/MOV  — ویدیو H.264 / MJPEG / HEVC(hvc1/hev1) / AV1(av01) / VP8/VP9 / MPEG-4(mp4v)
 *              + صدای AAC(mp4a) / MP3(.mp3) / Opus / FLAC / Vorbis / ALAC — AAC/MP3
 *              از I2S (با decoder ثبت‌شده) و PCM مستقیم پخش می‌شود
 *   MKV/WebM — (EBML) شناسایی و Video_ReadFrame برای استخراج خام فریم فعال است؛
 *              decode کامل via HWDecoder_Register("VP8"/"VP9"/"AV1"/"HEVC") روی Host/سخت‌افزار
 *   * هر کانتینری که codec آن در بُرد شتاب‌دهنده نداشته باشد، باز هم با
 *     Video_ReadFrame() فریمِ فشرده را می‌دهد (برای پردازش تصویر/Frame extraction)
 *     و Video_GetInfo().codec = UNKNOWN برمی‌گرداند — پخش زنده فقط برای MJPEG/H264/RAW واقعی است
 *
 * صدا از بلندگو (I2S همان hi2s1 موزیک). 17 و 19 را می‌توانید با هم
 * include کنید؛ Begin یکی اگر I2S بخواهد، Stop دیگری را می‌زند:
 *   #define SIMPLE_VIDEO_I2S 1
 *   #include "simple_audio.h"
 *   #include "simple_video.h"
 *
 *   SimpleVideo_SetPresent(my_lcd_blit);
 *   SimpleAudio_Begin();                 — یا SimpleVideo_Begin()
 *   while (1) {
 *       (void)SimpleAudio_Poll();
 *       (void)SimpleVideo_Poll();        — غیرفعال NOT_READY می‌دهد
 *   }
 *
 * اگر SIMPLE_VIDEO_I2S=0 بماند، PCM را با SimpleVideo_AudioRead به
 * DAC خودتان بدهید.
 *
 * پخش خودکار (پیش‌فرض خاموش):
 *   پیش‌فرض: Begin فقط پوشه را می‌خواند و چیزی پخش نمی‌شود؛ شما با
 *   SimpleVideo_Play("/videos/x.avi") یا Start()/Next() شروع می‌کنید و
 *   در پایان فایل (یا پوشه) می‌ایستد. رفتار قدیمیِ «Begin بنواز و
 *   تا ابد کل پوشه را لوپ کن» با یک خط برمی‌گردد:
 *   #define SIMPLE_VIDEO_AUTO_PLAY 1   — یا در زمان اجرا:
 *   SimpleVideo_SetAutoPlay(1);
 *
 * عکس‌گرفتن از فریم:
 *   SimpleVideo_SaveFrame(NULL) یک BMP شماره‌دار در
 *   SIMPLE_VIDEO_SAVE_FOLDER (پیش‌فرض /photos) می‌سازد و مسیر را لاگ
 *   می‌کند؛ با مسیر دلخواه، فرمت از پسوند انتخاب می‌شود.
 *
 * نویسندهٔ built-in: FMRV خام. encode به AVI/MP4 ادعا نمی‌شود.
 *
 * بافر فریم را در SDRAM بگذارید (32MB). H.264 از FM_Alloc هم کار می‌گیرد
 * — allocator را روی SDRAM بگذارید اگر heap داخلی کوچک است.
 *
 *   SIMPLE_VIDEO_FOLDER     "/videos"
 *   SIMPLE_VIDEO_MAX_W/H    320 x 240
 *   SIMPLE_VIDEO_FORMAT     FM_PIXEL_RGB565  (ILI9341: BGR565)
 *   SIMPLE_VIDEO_SWAP16     1 = بایت 565 برای SPI (۰ برای LTDC)
 *   SIMPLE_VIDEO_SECTION    ".sdram_buffers"
 *   SIMPLE_VIDEO_FRAME_COUNT 2  (blit async/DMA روی فریم قبلی امن است)
 *   SIMPLE_VIDEO_H264_FAIL_TOLERANCE 48 (فریم خراب تحمل‌شده در هر فایل)
 *   SIMPLE_VIDEO_AUTO_PLAY   0 (پخش خودکار و لوپ پوشه — بالا را ببینید)
 *   SIMPLE_VIDEO_SAVE_FOLDER "/photos" (پوشهٔ عکس‌گرفتن SaveFrame(NULL))
 *   SIMPLE_VIDEO_FADE_FRAMES 128 (رامپ ضد-خش صدا در start/pause/stop)
 *   SIMPLE_VIDEO_KEEP_I2S_CLOCKS 1 (کلاک I2S در تعویض ویدیو خاموش
 *                              نمی‌شود — پاپِ re-lock آمپلی‌فایر حذف
 *                              می‌شود؛ 0 = رفتار قدیمی برای مقایسه)
 *   SIMPLE_VIDEO_HALF_SAMPLES 8192 (نیمهٔ حلقهٔ I2S؛ اگر لاگ starve>0 شد
 *                              و صداخش داشت، این را بالا ببرید)
 *
 * SPI DMA را داخل FileManager نگذار — example 21 را در main.c include کن.
 */

#ifndef SIMPLE_VIDEO_H_
#define SIMPLE_VIDEO_H_

#include <stddef.h>
#include "fm_board_config.h"
#include "fm_cache.h"          /* نگهداری کش — بی‌اثر اگر کش خاموش باشد */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "fm_log.h"
#include "fm_debug.h"           /* FM_EVENT_LOG used by the I2S stop path */
#include "file_manager.h"
#include "fm_video.h"
#include "fm_image.h"
#include "fm_player.h"
#if FM_USE_H264
#include "fm_h264.h"
#endif

#if !FM_USE_VIDEO
/* ================================================================== */
/* FM_USE_VIDEO == 0                                                   */
/*                                                                     */
/* The video path is compiled out of the library, so nothing of the    */
/* player may exist here either: no frame buffers (sv_rgb / sv_frame   */
/* alone are SIMPLE_VIDEO_MAX_W x MAX_H x 3 + 4 bytes per pixel --     */
/* 11 MB with the default 1000x1000), no player state, no I2S ring.    */
/* The public API keeps its exact signatures so application code that  */
/* calls it still compiles and links; every call reports               */
/* FM_ERR_UNSUPPORTED (or a neutral value) and touches no memory.      */
/*                                                                     */
/* Keep the typedefs below identical to the ones in the real section.  */
/* ================================================================== */
typedef void (*SimpleVideo_PresentFn)(const void *pixels, uint16_t width,
                                      uint16_t height, FM_PixelFormat format,
                                      size_t bytes);
typedef void (*SimpleVideo_TrackStartFn)(void *ctx, const char *path,
                                         uint32_t width, uint32_t height,
                                         uint32_t fps);
typedef void (*SimpleVideo_TrackEndFn)(void *ctx, const char *path);
typedef void (*SimpleVideo_FrameFn)(void *ctx, const void *pixels,
                                    uint16_t width, uint16_t height,
                                    FM_PixelFormat format, size_t bytes);
typedef void (*SimpleVideo_PcmTapFn)(void *ctx, int16_t *pcm, size_t samples,
                                     uint32_t rate, uint16_t channels);
typedef struct
{
    char     name[FM_PATH_MAX];
    char     container[8];
    uint16_t width;
    uint16_t height;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t frame_index;
    double   position;
    double   duration;
    uint8_t  paused;
    uint8_t  has_audio;
    uint8_t  volume_pct;
    uint32_t audio_rate;
    uint16_t audio_channels;
    uint32_t playlist_count;
    uint32_t playlist_index;
    const void *pixels;
    size_t   bytes;
    FM_PixelFormat format;
} SimpleVideoInfo;

FM_Result SimpleVideo_Begin(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Poll(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Stop(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Next(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Start(void)
{ return FM_ERR_UNSUPPORTED; }
void SimpleVideo_SetAutoPlay(int8_t on)
{ (void)on; }
FM_Result SimpleVideo_SetPlayMode(FM_PlayMode mode)
{ (void)mode; return FM_ERR_UNSUPPORTED; }
FM_PlayMode SimpleVideo_GetPlayMode(void)
{ return FM_PLAY_MODE_STOP; }
void SimpleVideo_SetRepeatMode(int mode)
{ (void)mode; }
FM_PlayMode SimpleVideo_GetRepeatMode(void)
{ return FM_PLAY_MODE_STOP; }
FM_Result SimpleVideo_SetVolume(uint8_t percent)
{ (void)percent; return FM_ERR_UNSUPPORTED; }
uint8_t SimpleVideo_GetVolume(void)
{ return 0u; }
void SimpleVideo_EnableAudio(uint8_t enable)
{ (void)enable; }
uint8_t SimpleVideo_IsAudioEnabled(void)
{ return 0u; }
void SimpleVideo_SetAudioEnabled(uint8_t enable)
{ (void)enable; }
void SimpleVideo_SetPrePcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetPostPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetAudioPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{ (void)fn; (void)ctx; }
const void *SimpleVideo_GetFrame(uint16_t *w, uint16_t *h)
{ (void)w; (void)h; return NULL; }
FM_Result SimpleVideo_ReadAudio(int16_t *pcm,
        size_t frames,
        size_t *got_frames)
{ (void)pcm; (void)frames; (void)got_frames; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_GetAudioInfo(FM_AudioTrackInfo *info)
{ (void)info; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Prev(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Play(const char *path)
{ (void)path; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Pause(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_Resume(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_TogglePause(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_SeekTo(double seconds)
{ (void)seconds; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_SeekBy(double seconds)
{ (void)seconds; return FM_ERR_UNSUPPORTED; }
void SimpleVideo_SetPresent(SimpleVideo_PresentFn fn)
{ (void)fn; }
void SimpleVideo_SetTrackStartCallback(SimpleVideo_TrackStartFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetTrackEndCallback(SimpleVideo_TrackEndFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetFrameCallback(SimpleVideo_FrameFn fn, void *ctx)
{ (void)fn; (void)ctx; }
void SimpleVideo_SetPresentWait(void (*fn)(void))
{ (void)fn; }
FM_Result SimpleVideo_SetOutputSize(uint16_t width, uint16_t height)
{ (void)width; (void)height; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_SetOutputFormat(FM_PixelFormat format)
{ (void)format; return FM_ERR_UNSUPPORTED; }
void SimpleVideo_SetSwap16(int8_t swap)
{ (void)swap; }
FM_Result SimpleVideo_AudioRead(uint8_t *buf, size_t cap, size_t *got)
{ (void)buf; (void)cap; (void)got; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_SaveFrame(const char *path)
{ (void)path; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_RecStart(const char *path,
        uint16_t width,
        uint16_t height,
        uint32_t fps,
        uint32_t expected_frames)
{ (void)path; (void)width; (void)height; (void)fps; (void)expected_frames; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_RecWrite(const void *rgb888, size_t bytes)
{ (void)rgb888; (void)bytes; return FM_ERR_UNSUPPORTED; }
FM_Result SimpleVideo_RecStop(void)
{ return FM_ERR_UNSUPPORTED; }
const void *SimpleVideo_Frame(uint16_t *width,
        uint16_t *height,
        FM_PixelFormat *format,
        size_t *bytes)
{ (void)width; (void)height; (void)format; (void)bytes; return NULL; }
void SimpleVideo_Info(SimpleVideoInfo *out)
{ if (out != NULL) { memset(out, 0, sizeof(*out)); } }

#else /* FM_USE_VIDEO */

#ifndef SIMPLE_VIDEO_FOLDER
#define SIMPLE_VIDEO_FOLDER "/videos"
#endif
#ifndef SIMPLE_VIDEO_MAX_W
#define SIMPLE_VIDEO_MAX_W 320u
#endif
#ifndef SIMPLE_VIDEO_MAX_H
#define SIMPLE_VIDEO_MAX_H 240u
#endif
#ifndef SIMPLE_VIDEO_FORMAT
#define SIMPLE_VIDEO_FORMAT FM_PIXEL_RGB565
#endif
#ifndef SIMPLE_VIDEO_SWAP16
#define SIMPLE_VIDEO_SWAP16 1
#endif
/* پخش از کش فلش. ۱ = SimpleVideo_Play قبل از پخش، فایل را از راه
 * FM_MediaCache می‌گذراند (اگر راه‌اندازی شده باشد). خاموش‌بودنِ کش
 * بی‌خطر است: همان مسیر اصلی استفاده می‌شود.
 * راه‌اندازی کش: examples/play_from_flash.h */
#ifndef SIMPLE_VIDEO_USE_MEDIACACHE
#define SIMPLE_VIDEO_USE_MEDIACACHE 0
#endif
#if SIMPLE_VIDEO_USE_MEDIACACHE
#include "fm_mediacache.h"
#endif

#ifndef SIMPLE_VIDEO_SECTION
#define SIMPLE_VIDEO_SECTION ".sdram_buffers"
#endif
#ifndef SIMPLE_VIDEO_FRAME_COUNT
#define SIMPLE_VIDEO_FRAME_COUNT 2u
#endif
#if SIMPLE_VIDEO_FRAME_COUNT < 1u || SIMPLE_VIDEO_FRAME_COUNT > 2u
#error "SIMPLE_VIDEO_FRAME_COUNT must be 1 or 2"
#endif
#ifndef SIMPLE_VIDEO_I2S
#define SIMPLE_VIDEO_I2S 0
#endif
#ifndef SIMPLE_VIDEO_HALF_SAMPLES
/* 16384 → 372 ms/half, 744 ms ring (was 8192 → 186 ms).  Doubles the
 * head-room against SD/SPI contention on STM32H7 SPI+TFT panels.
 * Still fits the shared simple_i2s_ring[65534] (2*16384=32768).  With
 * LCD_SPI_DMA_IDLE=1 the 30 ms SPI burst now has 4 FM_Idle slots
 * inside it; together with blocking present (board/app_display.h) the
 * 15 fps budget stays 45 ms sequential and audio no longer contends. */
#define SIMPLE_VIDEO_HALF_SAMPLES 16384u
#endif
/* 1 = refill the I2S ring from FM_Idle DURING decode (gapless audio).
 * 0 = refill only once per SimpleVideo_Poll pass (A/B switch to isolate
 *     picture-noise issues: if noise disappears with 0, the mid-decode
 *     SD reads for audio are interfering with the display path). */
#ifndef SIMPLE_VIDEO_IDLE_REFILL
#define SIMPLE_VIDEO_IDLE_REFILL 1
#endif
/* N > 0: every N frames log average decode/convert/present times (ms)
 * so FPS bottlenecks show up in the UART log.  0 = off. */
#ifndef SIMPLE_VIDEO_PROFILE
#define SIMPLE_VIDEO_PROFILE 64u
#endif
/* Consecutive failed H.264 frames tolerated before the file is given up
 * and the next one starts.  One damaged frame breaks the reference chain
 * until the next IDR (~keyint frames, typically <= 1 s), so the default
 * bridges a full GOP: the picture freezes briefly and then resumes at
 * the next keyframe instead of the playlist jumping files.  0 = old
 * behaviour (first bad frame skips the file). */
#ifndef SIMPLE_VIDEO_H264_FAIL_TOLERANCE
#define SIMPLE_VIDEO_H264_FAIL_TOLERANCE 48u
#endif
/* Decode-cost policy.  SimpleVideo measures the average wall-clock cost
 * of decode+convert over the first SIMPLE_VIDEO_SLOW_PROBE frames of an
 * item.  If it exceeds SIMPLE_VIDEO_SLOW_FACTOR x the frame budget
 * (1000/fps), the item cannot play in real time on this MCU:
 *   SIMPLE_VIDEO_SKIP_SLOW 1 (default): skip the item once with a clear
 *     log line, so slow codecs (e.g. software H.264 on an H7) do not
 *     grind the playlist to a halt.
 *   SIMPLE_VIDEO_SKIP_SLOW 0: keep the item; silent files then play in
 *     "decode-limited" pacing (every frame shown, smooth slow motion),
 *     files with audio stay timeline-locked to the sound. */
#ifndef SIMPLE_VIDEO_SKIP_SLOW
#define SIMPLE_VIDEO_SKIP_SLOW 1
#endif
#ifndef SIMPLE_VIDEO_SLOW_FACTOR
#define SIMPLE_VIDEO_SLOW_FACTOR 2u
#endif
#ifndef SIMPLE_VIDEO_SLOW_PROBE
#define SIMPLE_VIDEO_SLOW_PROBE 6u
#endif
/* Frame rate (fps) that is still considered watchable when the item is
 * slower than real time.  MJPEG and RAW frames are independent, so the
 * player simply drops frames and keeps the timeline (and the audio)
 * correct: a 720x480 MJPEG that decodes in ~380 ms still plays, just at
 * ~2.6 fps instead of 15.  Only give such an item up when even the
 * dropped-frame rate falls below this floor.
 *
 * Without this, SIMPLE_VIDEO_SKIP_SLOW killed every high-resolution
 * clip after SIMPLE_VIDEO_SLOW_PROBE frames — about 3-5 seconds of
 * playback — even though it was playing perfectly well at a reduced
 * frame rate.  H.264 is excluded: P-frames reference their predecessor,
 * so frames cannot be dropped and a slow H.264 item really is
 * unplayable.  Set to 0 to restore the old "skip anything slow"
 * behaviour. */
/* ---------------------------------------------------------------------
 * H.264 on this port: KNOWN LIMITATION (see README / git history)
 *
 * h264bsd decodes all bundled clips correctly on a host build, but on
 * this STM32H7 target the parameter-set lookup fails: the IDR slice
 * header is read as pic_parameter_set_id=2 when the actual value in the
 * bitstream is 0 (verified by decoding the slice header by hand), so
 * h264bsdCheckAccessUnitBoundary rejects it (aub=1/pps2) and the frame
 * comes back as PARAM_SET_ERROR after consuming exactly the 664 bytes of
 * SPS+PPS+SEI that precede the slice.
 *
 * Ruled out: memory (allocfail=0, pool 99% free), parameter-set storage
 * (sps_seen/pps_seen=3), activation (act=0), in-place NAL mutation
 * (fixed, skipnal=0 now), and the bitstream itself.  The remaining
 * suspect is bit-reader behaviour on this target/toolchain, and it is
 * only reachable at -O2: at -O1 the same clips decode, just far too
 * slowly to be usable (~1 fps).
 *
 * Conclusion: H.264 is NOT a supported real-time format on this port.
 * MJPEG and raw formats are.  Set SIMPLE_VIDEO_ENABLE_H264 to 1 if you
 * want the player to attempt H.264 anyway.
 * ------------------------------------------------------------------ */
#ifndef SIMPLE_VIDEO_ENABLE_H264
#define SIMPLE_VIDEO_ENABLE_H264 0
#endif

#ifndef SIMPLE_VIDEO_SLOW_MIN_FPS
#define SIMPLE_VIDEO_SLOW_MIN_FPS 2u
#endif
/* Max frames decoded per SimpleVideo_Poll pass without presenting.
 * Bounds the catch-up work after a stall (pause, seek, slow codec) so
 * one Poll can never block for the length of a whole file. */
#ifndef SIMPLE_VIDEO_MAX_BURST
#define SIMPLE_VIDEO_MAX_BURST 3u
#endif
/* 1 = old behaviour: SimpleVideo_Begin() starts item 0 immediately and
 * every EOF advances to the next file, looping the folder forever.
 * 0 (default) = nothing plays until the app asks for it
 * (SimpleVideo_Play / Start / Next / Prev), and a file stops at its
 * end instead of chaining into the whole folder.  Runtime switch:
 * SimpleVideo_SetAutoPlay(). */
#ifndef SIMPLE_VIDEO_AUTO_PLAY
#define SIMPLE_VIDEO_AUTO_PLAY 0u
#endif
/* Snapshot folder for SimpleVideo_SaveFrame(NULL).  Created on demand;
 * the file is auto-numbered video_000.bmp, video_001.bmp, ...  An
 * explicit path (SimpleVideo_SaveFrame("/logs/x.png")) bypasses this. */
#ifndef SIMPLE_VIDEO_SAVE_FOLDER
#define SIMPLE_VIDEO_SAVE_FOLDER "/photos"
#endif
/* Anti-click ramp length (stereo frames) at I2S start/resume, pause,
 * AND every stop (item end, seek, Next/Prev, too-slow skip): the stop
 * first fades the still-queued samples to silence (via the DMA ring
 * position), so cutting the DMA can no longer click.  0 = off. */
#ifndef SIMPLE_VIDEO_FADE_FRAMES
#define SIMPLE_VIDEO_FADE_FRAMES 128u
#endif
/* 1 (default) = stops never halt the I2S clocks: the stop fades the
 * ring to silence and leaves the DMA circulating, so the DAC/amplifier
 * never loses lock — the residual "خش" at every video change is the
 * clock stop/start (an amp re-lock pop), not the sample data.  The
 * clocks stop only when another player takes hi2s1 or the sample rate
 * really changes.  0 = hard-stop the DMA on every stop (old behaviour,
 * for A/B testing). */
#ifndef SIMPLE_VIDEO_KEEP_I2S_CLOCKS
#define SIMPLE_VIDEO_KEEP_I2S_CLOCKS 1
#endif
#if SIMPLE_VIDEO_I2S
#include "i2s.h"
#include "../top_api/simple_i2s_mux.h"
#include "../top_api/simple_idle_mux.h"
#if (2u * SIMPLE_VIDEO_HALF_SAMPLES) > UINT16_MAX
#error "SIMPLE_VIDEO_HALF_SAMPLES too big for HAL_I2S_Transmit_DMA"
#endif
#if (2u * SIMPLE_VIDEO_HALF_SAMPLES) > SIMPLE_I2S_RING_SAMPLES
#error "SIMPLE_VIDEO_HALF_SAMPLES does not fit the shared simple_i2s_ring"
#endif
extern I2S_HandleTypeDef hi2s1;
FM_Result SimpleVideo_Stop(void);
#endif

typedef void (*SimpleVideo_PresentFn)(const void *pixels, uint16_t width,
                                      uint16_t height, FM_PixelFormat format,
                                      size_t bytes);

/* Track lifecycle callbacks: on_track_start fires once when a video item
 * begins playing; on_track_end fires when that same item has finished
 * (data fully consumed / natural EOF). Both report the item path so the
 * application knows exactly which video the DSP is processing. Install
 * via SimpleVideo_SetTrackStartCallback/SetTrackEndCallback. */
typedef void (*SimpleVideo_TrackStartFn)(void *ctx, const char *path,
                                         uint32_t width, uint32_t height,
                                         uint32_t fps);
typedef void (*SimpleVideo_TrackEndFn)(void *ctx, const char *path);

/* Frame data callback: delivers every decoded frame (already converted to
 * sv.format, ready for present) for DSP/image processing, independent of
 * display. When no SimpleVideo_SetPresent() callback is installed this
 * still fires (data-only mode), so image analysis works without an LCD. */
typedef void (*SimpleVideo_FrameFn)(void *ctx, const void *pixels,
                                    uint16_t width, uint16_t height,
                                    FM_PixelFormat format, size_t bytes);

/* Optional hook, usually LcdSpiDma_Wait() (example 21).  Called at the
 * start of every present so a non-blocking present callback
 * (LcdSpiDma_Begin) can overlap the SPI transfer of frame N with the
 * decode of frame N+1; the hook drains the transfer before frame N+1 is
 * kicked.  Leave NULL (default) for a blocking present callback —
 * behaviour is then identical to before. */
static void (*sv_present_wait)(void);

typedef struct
{
    char     name[FM_PATH_MAX];
    char     container[8];
    uint16_t width;
    uint16_t height;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t frame_index;
    double   position;
    double   duration;
    uint8_t  paused;
    uint8_t  has_audio;
    uint8_t  volume_pct; /* I2S software gain 0..100 (SimpleVideo_SetVolume) */
    uint32_t audio_rate;
    uint16_t audio_channels;
    uint32_t playlist_count;
    uint32_t playlist_index;
    const void *pixels;
    size_t   bytes;
    FM_PixelFormat format;
} SimpleVideoInfo;

#define SV_CMD_NONE      0u
#define SV_CMD_NEXT      1u
#define SV_CMD_PREV      2u
#define SV_CMD_PLAY      3u
#define SV_CMD_SEEK      4u
#define SV_CMD_RESUME    5u
#define SV_CMD_BLIT      6u
#define SV_CMD_SAVE      7u
#define SV_CMD_REC_START 8u
#define SV_CMD_REC_STOP  9u
#define SV_CMD_START     10u

typedef struct
{
    FM_Player player;
    FM_Bool player_ok;
    FM_Bool running;
    FM_Bool paused;
    FM_Bool have_frame;
    uint32_t t0_ms;
    uint32_t elapsed_ms;
    uint32_t shown_frame;
    SimpleVideo_PresentFn present;
    /* Track lifecycle callbacks (start = item begins, end = it finished).
     * See SimpleVideo_SetTrackStartCallback/SetTrackEndCallback. */
    SimpleVideo_TrackStartFn track_start;
    void *track_start_ctx;
    SimpleVideo_TrackEndFn track_end;
    void *track_end_ctx;
    /* Frame data callback for DSP/image processing; fires for every decoded
     * frame even when no present callback is installed (data-only mode). */
    SimpleVideo_FrameFn frame_cb;
    void *frame_cb_ctx;
    /* Snapshot of the current item, so the end callback reports the same
     * path that was reported at start. */
    char cur_path[FM_PATH_MAX];
    uint32_t cur_index;
    uint8_t item_started;
    FM_PixelFormat format;
    uint16_t out_w;
    uint16_t out_h;
    volatile uint8_t pending_cmd;
    char pending_path[FM_PATH_MAX];
    double pending_seek;
    uint16_t pending_w;
    uint16_t pending_h;
    uint32_t pending_fps;
    uint32_t pending_frames;
    uint8_t pause_need_restart;
    FM_Bool has_audio;
    FM_AudioTrackInfo audio;
    FM_Bool audio_pcm; /* 1 = 16-bit PCM that I2S can play */
    FM_VideoWriter rec;
    FM_Bool rec_active;
    uint32_t fail_count;   /* consecutive files that failed to decode */
    uint32_t h264_fail_run; /* consecutive failed H.264 frames (tolerated:
                             * self-heal + next IDR resync instead of a
                             * full file skip)                        */
    uint8_t h264_diag_logged; /* one diag line per failure run        */
    uint8_t size_logged;   /* frame-too-large already logged for file */
    uint8_t out_size_user; /* 1 = SimpleVideo_SetOutputSize() was called */
    uint8_t scale_logged;  /* down-scale reason already explained once */
    uint32_t frame_cost_ema; /* EMA of decode+convert cost (ms/frame)  */
    uint32_t slow_probe;     /* frames measured since the item started */
    uint32_t slow_skip_count; /* consecutive slow-skipped items        */
    /* per-stage timing (ms), averaged over SIMPLE_VIDEO_PROFILE frames */
    uint32_t prof_decode;
    uint32_t prof_convert;
    uint32_t prof_present;
    uint32_t prof_frames;
} SimpleVideoState;

FM_Result SimpleVideo_Stop(void);

static SimpleVideoState sv;
static uint8_t sv_slot;
static uint8_t sv_show;
/* Runtime output pixel format: 0 = use SIMPLE_VIDEO_FORMAT.
 * Set with SimpleVideo_SetOutputFormat() before/after Begin. */
static FM_PixelFormat sv_out_format;
/* Runtime 16-bit byte-swap override: 0/1, -1 = use SIMPLE_VIDEO_SWAP16. */
static int8_t sv_swap16_override = -1;
/* Runtime auto-play override: -1 = use SIMPLE_VIDEO_AUTO_PLAY. */
static int8_t sv_autoplay = -1;
/* Playback persistence — what happens when a video item ends.
 * FM_PLAY_MODE_STOP       : تمام شد → توقف (پاز، نرو بعدی) — پیش‌فرض ویدیو
 * FM_PLAY_MODE_NEXT       : تمام شد → بعدی، آخر لیست → توقف (بدون لوپ) — مناسب پرزنت
 * FM_PLAY_MODE_REPEAT_ONE : تمام شد → همین ویدیو دوباره (تکرار) 
 * FM_PLAY_MODE_REPEAT_ALL : تمام شد → بعدی، آخر→اول (لوپ پوشه) — قدیم با AUTOPLAY=1
 * برای پرزنت: NEXT یا STOP بذار تا خودش نپره؛ برای کیوسک REPEAT_ALL. */
static FM_PlayMode sv_play_mode = (FM_PlayMode)(SIMPLE_VIDEO_AUTO_PLAY ? FM_PLAY_MODE_REPEAT_ALL : FM_PLAY_MODE_STOP);
static uint8_t sv_play_mode_explicit = 0u; /* 1 = app called SetPlayMode, ignore autoplay */
/* Software I2S gain, 0..100.  Lives outside `sv` so Begin() memset
 * does not reset a volume the app already set. */
static uint8_t sv_volume_pct = 100u;
/* Video-audio global enable: 1 = decode+play video audio via I2S, 0 = keep I2S silent
 * and skip Video_ReadAudioFrame in the refill path (saves CPU/SD when you only
 * need the pictures). Music (SimpleAudio) is not affected. */
static uint8_t sv_audio_enabled = 1u;
/* Pre/post PCM taps for video audio — same signature as SimpleAudio.
 * Pre is raw decoded PCM before volume/fade, Post is after volume/fade;
 * both are editable before DMA. NULL = disabled. */
typedef void (*SimpleVideo_PcmTapFn)(void *ctx, int16_t *pcm, size_t samples,
                                     uint32_t rate, uint16_t channels);
static SimpleVideo_PcmTapFn sv_pcm_tap_pre = NULL;
static void *sv_pcm_tap_pre_ctx = NULL;
static SimpleVideo_PcmTapFn sv_pcm_tap_post = NULL;
static void *sv_pcm_tap_post_ctx = NULL;
/* Auto-generated snapshot counter for SaveFrame(NULL). */
static uint32_t sv_save_seq;

static uint8_t sv_autoplay_active(void)
{
    return (sv_autoplay < 0) ? (uint8_t)(SIMPLE_VIDEO_AUTO_PLAY != 0u)
                             : (uint8_t)(sv_autoplay != 0);
}

static FM_PixelFormat sv_active_format(void)
{
    return (sv_out_format != (FM_PixelFormat)0) ? sv_out_format
                                                : SIMPLE_VIDEO_FORMAT;
}

__attribute__((section(SIMPLE_VIDEO_SECTION), aligned(32)))
static uint8_t sv_rgb[(size_t)SIMPLE_VIDEO_MAX_W * SIMPLE_VIDEO_MAX_H * 3u];
__attribute__((section(SIMPLE_VIDEO_SECTION), aligned(32)))
static uint8_t sv_frame[SIMPLE_VIDEO_FRAME_COUNT]
                      [(size_t)SIMPLE_VIDEO_MAX_W * SIMPLE_VIDEO_MAX_H * 4u];

static uint8_t *sv_draw(void)
{
    return sv_frame[sv_slot];
}

static size_t sv_draw_cap(void)
{
    return sizeof(sv_frame[0]);
}

/* Fused RGB888 -> RGB565/BGR565 conversion with optional byte swap in a
 * single pass.  Image_Convert + simple_video_swap16 walked the whole
 * frame twice; with buffers in SDRAM and D-Cache off every pass is
 * expensive, so folding them halves the post-decode cost.  Two pixels
 * per iteration are emitted with one 32-bit store (memcpy emits a single
 * STR on ARM) which measurably cuts the convert stage on H7. */
/*
 * قلاب اختیاری برای تبدیل RGB888 → RGB565 روی سخت‌افزار.
 *
 * مثال ۳۰ (DMA2D) اگر include شود این را پر می‌کند. اگر پر نباشد یا
 * خطا برگرداند، حلقهٔ نرم‌افزاری پایین اجرا می‌شود. بدون تغییر رفتار.
 *
 * px تعداد پیکسل، bgr یعنی جای R و B عوض شود، swap یعنی بایت‌های
 * ۱۶بیتی برعکس شوند (لازمِ SPI).
 */
typedef FM_Result (*sv_rgb565_fn)(const uint8_t *rgb, uint8_t *dst,
                                  uint16_t w, uint16_t h,
                                  uint8_t bgr, uint8_t swap);
static sv_rgb565_fn sv_rgb565_hw;

/*
 * w/h پارامترند، نه متغیر سراسری. با متغیر سراسری یک فراخوانی که
 * یادش می‌رفت آن را تازه کند، ابعاد فریم قبلی را به سخت‌افزار
 * می‌داد، و نگهبانِ «w*h == px» هم جلویش را نمی‌گرفت چون مثلاً
 * ۳۲۰×۲۴۰ و ۲۴۰×۳۲۰ حاصل‌ضرب یکسان دارند.
 */
static void sv_convert_565(const uint8_t *rgb, uint8_t *dst,
                           uint16_t w, uint16_t h,
                           uint8_t bgr, uint8_t swap)
{
    size_t px = (size_t)w * h;
    size_t i = 0;

    if (sv_rgb565_hw != NULL && w != 0u && h != 0u)
    {
        if (sv_rgb565_hw(rgb, dst, w, h, bgr, swap) == FM_OK)
        {
            return;
        }
    }

    if (((uintptr_t)dst & 3u) == 0u)
    {
        while (i + 2u <= px)
        {
            uint32_t r0 = rgb[0];
            uint32_t g0 = rgb[1];
            uint32_t b0 = rgb[2];
            uint32_t r1 = rgb[3];
            uint32_t g1 = rgb[4];
            uint32_t b1 = rgb[5];
            uint16_t v0;
            uint16_t v1;
            uint32_t word;

            if (bgr)
            {
                uint32_t t = r0;

                r0 = b0;
                b0 = t;
                t = r1;
                r1 = b1;
                b1 = t;
            }
            v0 = (uint16_t)(((r0 & 0xF8u) << 8) |
                            ((g0 & 0xFCu) << 3) |
                            (b0 >> 3));
            v1 = (uint16_t)(((r1 & 0xF8u) << 8) |
                            ((g1 & 0xFCu) << 3) |
                            (b1 >> 3));
            if (swap)
            {
                v0 = (uint16_t)((v0 >> 8) | (v0 << 8));
                v1 = (uint16_t)((v1 >> 8) | (v1 << 8));
            }
            word = (uint32_t)v0 | ((uint32_t)v1 << 16);
            memcpy(dst, &word, sizeof(word));
            rgb += 6;
            dst += 4;
            i += 2u;
        }
    }
    for (; i < px; i++)
    {
        uint8_t r = rgb[0];
        uint8_t g = rgb[1];
        uint8_t b = rgb[2];
        uint16_t v;

        if (bgr)
        {
            uint8_t t = r;

            r = b;
            b = t;
        }
        v = (uint16_t)(((uint16_t)(r & 0xF8u) << 8) |
                       ((uint16_t)(g & 0xFCu) << 3) |
                       ((uint16_t)b >> 3));
        if (swap)
        {
            dst[0] = (uint8_t)(v >> 8);
            dst[1] = (uint8_t)v;
        }
        else
        {
            dst[0] = (uint8_t)v;
            dst[1] = (uint8_t)(v >> 8);
        }
        rgb += 3;
        dst += 2;
    }
}

static uint8_t sv_swap16_active(void)
{
    return (sv_swap16_override >= 0) ? (uint8_t)sv_swap16_override
                                     : (uint8_t)(SIMPLE_VIDEO_SWAP16 != 0);
}

static void simple_video_swap16(uint8_t *p, size_t bytes)
{
    size_t i;
    uint8_t do_swap = (sv_swap16_override >= 0)
                          ? (uint8_t)sv_swap16_override
                          : (uint8_t)(SIMPLE_VIDEO_SWAP16 != 0);

    if (!do_swap || p == NULL || (bytes & 1u) != 0u)
    {
        return;
    }
    if (sv.format != FM_PIXEL_RGB565 && sv.format != FM_PIXEL_BGR565)
    {
        return;
    }
    for (i = 0; i < bytes; i += 2u)
    {
        uint8_t t = p[i];

        p[i] = p[i + 1u];
        p[i + 1u] = t;
    }
}

#if SIMPLE_VIDEO_I2S
/* I2S DMA ring — shared with SimpleAudio (simple_i2s_mux.h).  The ring is
 * large enough for the music player; SimpleVideo uses the first
 * 2 * SIMPLE_VIDEO_HALF_SAMPLES samples of it.  Only one player owns
 * hi2s1 at a time (SimpleI2S_Take stops the other), so sharing is safe and
 * removes a separate ~32 KB DMA ring from .dma_buffer. */
#define sv_i2s_buf simple_i2s_ring
/* PCM demux chunk buffer.  Sized by SIMPLE_VIDEO_PCM_CHUNK
 * (fm_board_config.h, default 8 KB) — it used to be a hard-coded 8192
 * so that knob did nothing.  Chunks larger than this are delivered in
 * consecutive pieces by Video_ReadAudioFrame (nothing is dropped). */
#ifndef SIMPLE_VIDEO_PCM_CHUNK
#define SIMPLE_VIDEO_PCM_CHUNK 8192u
#endif
__attribute__((section(".dma_buffer"), aligned(32)))
static uint8_t sv_pcm_chunk[SIMPLE_VIDEO_PCM_CHUNK];
static size_t sv_pcm_len;
static size_t sv_pcm_off;
static volatile uint8_t sv_i2s_refill[2];
static volatile uint8_t sv_i2s_running;
static volatile uint8_t sv_i2s_started;   /* DMA armed for this item    */
static volatile uint8_t sv_i2s_restart_req;
static uint32_t sv_i2s_restarts;
static uint32_t sv_i2s_read_errs;
static uint8_t sv_i2s_logged_aac;
/* Underrun counter: a refill flag that was still set when the DMA came
 * around to that half again — i.e. the main loop (SD/decode) could not
 * service the ring within one ring half (~186 ms).  Each hit replays a
 * stale half and is heard as a crackle/blip.  Reported in the profile
 * line so "خش during playback" becomes measurable. */
static volatile uint32_t sv_i2s_starve;
static uint32_t sv_i2s_starve_logged;
/* Refill-latency diagnostic: HAL_GetTick() at each half/full IRQ, then
 * the time the main loop actually took to service that flag.  If the
 * CPU-side ring never starves (starve=0) but the audio is still
 * choppy, this is what distinguishes "serviced immediately" (latency
 * ~0) from "just made it in time" (latency close to ~186 ms half
 * period).  Reported in the profile line. */
static volatile uint32_t sv_i2s_irq_tick[2];
static uint32_t sv_i2s_lat_max;
static uint32_t sv_i2s_lat_sum;
static uint32_t sv_i2s_lat_n;
/* Anti-click ramps: last played L/R sample (pause fade-out source) and
 * fade-in frames remaining on the next filled half. */
static int16_t sv_i2s_last_l;
static int16_t sv_i2s_last_r;
static uint32_t sv_i2s_fade_in;
static uint8_t sv_i2s_pause_faded[2];

/* Push CPU-written ring data out of the D-cache so the DMA (which
 * bypasses the cache) plays the fresh samples.  No-op when the region
 * is non-cacheable or the D-cache is off — and therefore always safe
 * (same defence ex17 uses for its I2S ring). */
static void sv_i2s_cache_clean(void *p, size_t bytes)
{
    /* بی‌اثر وقتی D-Cache خاموش است؛ آدرس/طول را هم خودش به مرز خط
     * کش گرد می‌کند. */
    FM_Cache_CleanForDma(p, bytes);
}

static FM_Bool sv_i2s_rate_ok(uint32_t rate)
{
    switch (rate)
    {
    case 8000u:
    case 11025u:
    case 16000u:
    case 22050u:
    case 32000u:
    case 44100u:
    case 48000u:
    case 96000u:
        return FM_TRUE;
    default:
        return FM_FALSE;
    }
}

static void sv_i2s_half(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        if (sv_i2s_refill[0] != 0u)
        {
            sv_i2s_starve++;
        }
        sv_i2s_irq_tick[0] = HAL_GetTick();
        sv_i2s_refill[0] = 1u;
    }
}

static void sv_i2s_full(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        if (sv_i2s_refill[1] != 0u)
        {
            sv_i2s_starve++;
        }
        sv_i2s_irq_tick[1] = HAL_GetTick();
        sv_i2s_refill[1] = 1u;
    }
}

static void sv_i2s_err(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        if (sv_i2s_running != 0u)
        {
            sv_i2s_running = 0u;
            /* Audio died mid-item (overrun, DMA bus error...).  Request a
             * re-kick from sv_i2s_poll instead of staying silent for the
             * rest of the video. */
            sv_i2s_restart_req = 1u;
        }
    }
}

static FM_Result sv_i2s_board_stop(void)
{
    FM_EVENT_LOG("i2s_stop: begin\n");
    /* Are WE the current owner of hi2s1?  This must be sampled BEFORE
     * SimpleI2S_Drop() below, because Drop() clears simple_i2s_owner_stop
     * when it matches — testing it afterwards always read NULL and the
     * whole keep-the-clocks-running path was dead code.  That is what
     * made every item change stop and restart the I2S clocks, i.e. the
     * amplifier re-lock "خش" at each video switch. */
    uint8_t we_own = (uint8_t)(simple_i2s_owner_stop == SimpleVideo_Stop);
    uint8_t was_running = sv_i2s_running;

    SimpleI2S_Drop(SimpleVideo_Stop);
    sv_i2s_started = 0u;
    sv_i2s_restart_req = 0u;
    if (!was_running)
    {
        FM_EVENT_LOG("i2s_stop: not running, early return\n");
        return FM_OK;
    }
    FM_EVENT_LOG("i2s_stop: was running, stopping DMA\n");
    sv_i2s_running = 0u;
#if SIMPLE_VIDEO_FADE_FRAMES > 0u
    uint8_t faded = 0u;
    /* Anti-pop stop (this is what removes the "خش" at every item
     * change / seek / stop).  HAL_I2S_DMAStop() cuts the DMA wherever
     * the waveform happens to be; jumping from a full-scale sample to
     * silence is an audible click.  So first overwrite everything the
     * DMA still has to play — from just ahead of its current ring
     * position to the end of the ring AND the wrapped start — with a
     * short fade-down from the last serviced sample plus digital
     * silence.  A few ms later the output is at zero and stopping
     * cannot click, no matter when HAL_I2S_DMAStop() lands. */
    if (hi2s1.hdmatx != NULL && hi2s1.hdmatx->Instance != NULL &&
        hi2s1.State == HAL_I2S_STATE_BUSY_TX &&
        hi2s1.Init.DataFormat == I2S_DATAFORMAT_16B)
    {
        const uint32_t total = 2u * SIMPLE_VIDEO_HALF_SAMPLES;
        const uint32_t fade_el =
            (uint32_t)SIMPLE_VIDEO_FADE_FRAMES * 2u; /* L+R per frame */
        /* The H7 HAL declares DMA_HandleTypeDef.Instance as void *
         * (stream or BDMA channel); I2S always runs on a DMA stream. */
        uint32_t ndtr =
            ((const DMA_Stream_TypeDef *)hi2s1.hdmatx->Instance)->NDTR;
        uint32_t pos;
        uint32_t k;
        uint32_t wait_ms;

        if (ndtr >= 1u && ndtr <= total)
        {
            /* NDTR counts down the whole ring; pos = next element the
             * DMA will send.  Skip 64 elements (~1.5 ms) of margin so
             * the in-flight slot is never rewritten under the DMA. */
            pos = ((total - ndtr) + 64u) % total;
            pos &= ~1u; /* keep L/R pairs aligned */
            for (k = 0u; k < fade_el; k += 2u)
            {
                int32_t g = (int32_t)(fade_el - 2u - k) / 2;
                uint32_t i = (pos + k) % total;

                sv_i2s_buf[i] = (int16_t)(((int32_t)sv_i2s_last_l * g) /
                                          (int32_t)(fade_el / 2u));
                sv_i2s_buf[i + 1u] =
                    (int16_t)(((int32_t)sv_i2s_last_r * g) /
                              (int32_t)(fade_el / 2u));
            }
            /* Silence the rest of the future, including the wrapped
             * start — the ring is circular, so after the fade the DMA
             * would otherwise replay old audio forever. */
            for (k = (pos + fade_el) % total; k != pos; k = (k + 2u) % total)
            {
                sv_i2s_buf[k] = 0;
                sv_i2s_buf[k + 1u] = 0;
            }
            sv_i2s_cache_clean(sv_i2s_buf, sizeof(sv_i2s_buf));
            /* Give the DMA time to play the ramp out before the cut. */
            wait_ms = ((uint64_t)(64u + fade_el) * 1000u) /
                      ((uint32_t)hi2s1.Init.AudioFreq * 2u) + 3u;
            if (wait_ms > 40u)
            {
                wait_ms = 40u;
            }
            {
                uint32_t t0 = HAL_GetTick();

                while ((HAL_GetTick() - t0) < wait_ms)
                {
                    /* busy-wait: board_stop only runs from the main
                     * loop (never from an IRQ), and the present DMA
                     * keeps the panel fed meanwhile. */
                }
            }
            faded = 1u;
        }
    }
#if (!SIMPLE_VIDEO_KEEP_I2S_CLOCKS) && (SIMPLE_VIDEO_FADE_FRAMES > 0u)
    (void)faded; /* only read by the kept-clocks path below */
#endif
#endif /* SIMPLE_VIDEO_FADE_FRAMES > 0u */
#if SIMPLE_VIDEO_KEEP_I2S_CLOCKS
    if (we_own)
    {
        /* Plain stop (item end, seek, Stop()): leave the DMA running
         * over the now-silent ring.  The I2S clocks never stop, so the
         * DAC/amplifier never re-locks — that re-lock was the pop no
         * data fade could remove.  The IRQ hooks are already dropped,
         * so the ring just replays digital silence until the next
         * sv_i2s_start() re-arms it. */
#if SIMPLE_VIDEO_FADE_FRAMES > 0u
        if (faded)
        {
            sv_i2s_refill[0] = sv_i2s_refill[1] = 0u;
            return FM_OK;
        }
#else
        memset(sv_i2s_buf, 0, sizeof(sv_i2s_buf));
        sv_i2s_cache_clean(sv_i2s_buf, sizeof(sv_i2s_buf));
        sv_i2s_refill[0] = sv_i2s_refill[1] = 0u;
        return FM_OK;
#endif
    }
#else
    (void)we_own;
#endif
    if (hi2s1.State == HAL_I2S_STATE_READY)
    {
        FM_EVENT_LOG("i2s_stop: already READY, return\n");
        return FM_OK;
    }
    FM_EVENT_LOG("i2s_stop: calling HAL_I2S_DMAStop (state=%lu)\n",
                 (unsigned long)hi2s1.State);
    HAL_StatusTypeDef st = HAL_I2S_DMAStop(&hi2s1);
    FM_EVENT_LOG("i2s_stop: HAL_I2S_DMAStop returned %lu\n",
                 (unsigned long)st);
    return (st == HAL_OK) ? FM_OK : FM_ERR_IO;
}

static FM_Result sv_i2s_fill_half_inner(uint8_t half)
{
    int16_t *dst = sv_i2s_buf + (size_t)half * SIMPLE_VIDEO_HALF_SAMPLES;
    size_t filled = 0;

    if (!sv.audio_pcm || !sv_audio_enabled)
    {
        memset(dst, 0, SIMPLE_VIDEO_HALF_SAMPLES * sizeof(int16_t));
        return FM_OK;
    }

    while (filled < SIMPLE_VIDEO_HALF_SAMPLES)
    {
        size_t src_left;
        size_t dst_left = SIMPLE_VIDEO_HALF_SAMPLES - filled;
        uint16_t ch = sv.audio.channels;

        if (sv_pcm_off >= sv_pcm_len)
        {
            size_t got = 0;
            FM_Result r = Video_ReadAudioFrame(&sv.player.video, sv_pcm_chunk,
                                              sizeof(sv_pcm_chunk), &got);

            if (r == FM_ERR_EOF)
            {
                memset(dst + filled, 0,
                       dst_left * sizeof(int16_t));
                return FM_OK;
            }
            if (r != FM_OK)
            {
                /* A demux hiccup (oversized chunk, busy SD, transient IO
                 * error) must not starve the whole 371 ms ring half: pad
                 * the remainder with silence and keep the DMA fed.  The
                 * next refill retries the demux cursor. */
                sv_i2s_read_errs++;
                if (sv_i2s_read_errs == 1u ||
                    (sv_i2s_read_errs % 100u) == 0u)
                {
                    FM_Log("SimpleVideo: PCM read %s (%d) #%lu — silence "
                           "padding\n",
                           File_GetErrorString(r), (int)r,
                           (unsigned long)sv_i2s_read_errs);
                }
                memset(dst + filled, 0, dst_left * sizeof(int16_t));
                return FM_OK;
            }
            sv_pcm_len = got;
            sv_pcm_off = 0;
            if (sv_pcm_len < 2u)
            {
                continue;
            }
        }

        src_left = sv_pcm_len - sv_pcm_off;
        if (ch == 2u)
        {
            size_t n = src_left / 2u;

            if (n > dst_left)
            {
                n = dst_left;
            }
            /* Keep L/R pairs aligned.  A chunk holding an odd number of
             * 16-bit samples (or a clamp landing on an odd count) would
             * shift `filled` by one element and swap the two channels
             * for the rest of the stream — audible as a hard stereo
             * flip plus a click at the seam. */
            if ((n & 1u) != 0u && n > 1u)
            {
                n -= 1u;
            }
            memcpy(dst + filled, sv_pcm_chunk + sv_pcm_off, n * 2u);
            filled += n;
            sv_pcm_off += n * 2u;
        }
        else if (ch == 1u)
        {
            size_t frames = src_left / 2u;
            size_t i;

            if (frames > dst_left / 2u)
            {
                frames = dst_left / 2u;
            }
            for (i = 0; i < frames; i++)
            {
                int16_t s;

                memcpy(&s, sv_pcm_chunk + sv_pcm_off + i * 2u, 2u);
                dst[filled + i * 2u] = s;
                dst[filled + i * 2u + 1u] = s;
            }
            filled += frames * 2u;
            sv_pcm_off += frames * 2u;
        }
        else
        {
            memset(dst + filled, 0, dst_left * sizeof(int16_t));
            return FM_OK;
        }
        if (ch == 2u && (sv_pcm_len - sv_pcm_off) < 2u)
        {
            sv_pcm_off = sv_pcm_len;
        }
        if (ch == 1u && (sv_pcm_len - sv_pcm_off) < 2u)
        {
            sv_pcm_off = sv_pcm_len;
        }
    }
    /* Pre-gain tap: raw decoded PCM before volume/fade, editable. */
    if (sv_pcm_tap_pre != NULL && filled > 0u)
    {
        sv_pcm_tap_pre(sv_pcm_tap_pre_ctx, dst, filled,
                       sv.audio.sample_rate, sv.audio.channels);
    }
    if (sv_volume_pct != 100u && filled > 0u)
    {
        uint32_t gain = ((uint32_t)sv_volume_pct * 0x7FFFu) / 100u;
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_FEATURE_DSP)
        /* SIMD: 2 samples per iteration, same TPDF dither */
        size_t i = 0;
        static uint32_t lfsr_a = 0x12345678u;
        static uint32_t lfsr_b = 0x9ABCDEF0u;
        for (; i + 1u < filled; i += 2u) {
            int32_t a0 = (int32_t)dst[i] * (int32_t)gain;
            int32_t a1 = (int32_t)dst[i+1] * (int32_t)gain;
            a0 += 16384; a1 += 16384;
            lfsr_a = lfsr_a * 1664525u + 1013904223u;
            lfsr_b = lfsr_b * 1566083941u + 1u;
            a0 += (int32_t)(((lfsr_a >> 16) & 1u) + ((lfsr_b >> 16) & 1u)) - 1;
            lfsr_a = lfsr_a * 1664525u + 1013904223u;
            lfsr_b = lfsr_b * 1566083941u + 1u;
            a1 += (int32_t)(((lfsr_a >> 16) & 1u) + ((lfsr_b >> 16) & 1u)) - 1;
            a0 >>= 15; a1 >>= 15;
            if (a0 > 32767) a0 = 32767; else if (a0 < -32768) a0 = -32768;
            if (a1 > 32767) a1 = 32767; else if (a1 < -32768) a1 = -32768;
            dst[i] = (int16_t)a0; dst[i+1] = (int16_t)a1;
        }
        for (; i < filled; i++)
        {
            int32_t acc = (int32_t)dst[i] * (int32_t)gain;
            acc += 16384;
            lfsr_a = lfsr_a * 1664525u + 1013904223u;
            lfsr_b = lfsr_b * 1566083941u + 1u;
            acc += (int32_t)(((lfsr_a >> 16) & 1u) + ((lfsr_b >> 16) & 1u)) - 1;
            acc >>= 15;
            if (acc > 32767) acc = 32767;
            else if (acc < -32768) acc = -32768;
            dst[i] = (int16_t)acc;
        }
#else
        size_t i;
        /* TPDF dither: two independent triangular random values.
         * This completely eliminates quantization distortion that would
         * otherwise appear as periodic noise (the "1,2,3,4..." pattern).
         * LFSR provides fast pseudo-random sequence; summing two gives
         * triangular PDF. Range: ±1 LSB peak-to-peak. */
        static uint32_t lfsr_a = 0x12345678u;
        static uint32_t lfsr_b = 0x9ABCDEF0u;

        for (i = 0; i < filled; i++)
        {
            int32_t acc = (int32_t)dst[i] * (int32_t)gain;

            acc += 16384;  /* rounding */
            /* TPDF dither: sum of two uniform random values */
            lfsr_a = lfsr_a * 1664525u + 1013904223u;
            lfsr_b = lfsr_b * 1566083941u + 1u;
            acc += (int32_t)(((lfsr_a >> 16) & 1u) + ((lfsr_b >> 16) & 1u)) - 1;
            acc >>= 15;
            if (acc > 32767)
            {
                acc = 32767;
            }
            else if (acc < -32768)
            {
                acc = -32768;
            }
            dst[i] = (int16_t)acc;
        }
#endif
    }
    /* Linear fade-in over the first frames of a (re)started stream and
     * last-sample bookkeeping for the pause fade-out. */
#if SIMPLE_VIDEO_FADE_FRAMES > 0u
    if (sv_i2s_fade_in != 0u)
    {
        uint32_t n = (sv_i2s_fade_in < (uint32_t)SIMPLE_VIDEO_FADE_FRAMES)
                         ? sv_i2s_fade_in
                         : (uint32_t)SIMPLE_VIDEO_FADE_FRAMES;
        uint32_t ramp = (uint32_t)SIMPLE_VIDEO_FADE_FRAMES;
        uint32_t i;

        for (i = 0u; i < n; i++)
        {
            int32_t g = (int32_t)(i + 1u);

            dst[i * 2u] = (int16_t)(((int32_t)dst[i * 2u] * g) /
                                    (int32_t)ramp);
            dst[i * 2u + 1u] = (int16_t)(((int32_t)dst[i * 2u + 1u] * g) /
                                         (int32_t)ramp);
        }
        sv_i2s_fade_in = 0u;
    }
#endif
    if (filled >= 2u)
    {
        int16_t l;
        int16_t r;

        memcpy(&l, &dst[filled - 2u], sizeof(l));
        memcpy(&r, &dst[filled - 1u], sizeof(r));
        /* Near-zero last sample is noise floor, not music.  Holding it
         * across pause/item fade is the "hiss at the threshold". */
        if (l > -12 && l < 12)
        {
            l = 0;
        }
        if (r > -12 && r < 12)
        {
            r = 0;
        }
        sv_i2s_last_l = l;
        sv_i2s_last_r = r;
    }
    /* Post-gain tap: after volume+fade, exactly what speaker will get, editable. */
    if (sv_pcm_tap_post != NULL && filled > 0u)
    {
        sv_pcm_tap_post(sv_pcm_tap_post_ctx, dst, filled,
                        sv.audio.sample_rate, sv.audio.channels);
    }
    return FM_OK;
}

static FM_Result sv_i2s_fill_half(uint8_t half)
{
    FM_Result r = sv_i2s_fill_half_inner(half);

    sv_i2s_cache_clean(sv_i2s_buf + (size_t)half * SIMPLE_VIDEO_HALF_SAMPLES,
                       SIMPLE_VIDEO_HALF_SAMPLES * sizeof(int16_t));
    return r;
}

/* Pause refill: once per pause, ramp the half down from the last played
 * samples to digital silence (removes the pause pop); afterwards keep
 * the half zeroed. */
static void sv_i2s_pause_fill(uint8_t half)
{
    int16_t *dst = sv_i2s_buf + (size_t)half * SIMPLE_VIDEO_HALF_SAMPLES;

#if SIMPLE_VIDEO_FADE_FRAMES > 0u
    if (!sv_i2s_pause_faded[half])
    {
        uint32_t n = (uint32_t)SIMPLE_VIDEO_FADE_FRAMES;
        uint32_t i;

        for (i = 0u; i < n; i++)
        {
            int32_t g = (int32_t)(n - 1u - i);

            dst[i * 2u] = (int16_t)(((int32_t)sv_i2s_last_l * g) /
                                    (int32_t)n);
            dst[i * 2u + 1u] = (int16_t)(((int32_t)sv_i2s_last_r * g) /
                                         (int32_t)n);
        }
        memset(dst + (size_t)n * 2u, 0,
               (SIMPLE_VIDEO_HALF_SAMPLES - (size_t)n) * sizeof(int16_t));
        sv_i2s_pause_faded[half] = 1u;
    }
    else
#endif
    {
        memset(dst, 0, SIMPLE_VIDEO_HALF_SAMPLES * sizeof(int16_t));
    }
    sv_i2s_cache_clean(dst, SIMPLE_VIDEO_HALF_SAMPLES * sizeof(int16_t));
}

static FM_Result sv_i2s_start(void)
{
    uint32_t rate;
    FM_Result r;

    sv_i2s_refill[0] = sv_i2s_refill[1] = 0;
    sv_i2s_restarts = 0u;
    sv_i2s_read_errs = 0u;
    sv_i2s_starve = 0u;
    sv_i2s_starve_logged = 0u;
    sv_i2s_lat_n = 0u;
    sv_i2s_lat_sum = 0u;
    sv_i2s_lat_max = 0u;
    sv_i2s_restart_req = 0u;
    sv_i2s_pause_faded[0] = sv_i2s_pause_faded[1] = 0u;
#if SIMPLE_VIDEO_FADE_FRAMES > 0u
    /* Ramp up over the first frames of the (re)started stream: kills
     * the pop of a DMA start mid-waveform (pause resume, item change). */
    sv_i2s_fade_in = (uint32_t)SIMPLE_VIDEO_FADE_FRAMES;
#else
    sv_i2s_fade_in = 0u;
#endif
    /* NOTE: sv_pcm_len/off are deliberately NOT reset here — a resume
     * continues exactly at the demux cursor instead of skipping up to
     * one chunk (≈24 ms); simple_video_prime_item clears them for a
     * new item. */
    if (!sv.audio_pcm)
    {
        /* Silent file.  Stopping the DMA lets the I2S data/clock lines
         * float and many amplifier boards turn that into an audible
         * hiss.  Keep (or start) the DMA sending a zeroed ring instead:
         * a continuous digital-silence stream keeps the DAC quiet. */
        if (!sv_i2s_running)
        {
            if (hi2s1.Init.DataFormat != I2S_DATAFORMAT_16B)
            {
                return FM_OK;
            }
#if SIMPLE_VIDEO_KEEP_I2S_CLOCKS
            if (hi2s1.State == HAL_I2S_STATE_BUSY_TX)
            {
                /* The ring is already circulating with the silence a
                 * kept-clocks stop left behind: re-arm the hooks and
                 * keep it playing — nothing to rewrite, no clock
                 * event. */
                SimpleI2S_Take(sv_i2s_half, sv_i2s_full, sv_i2s_err,
                               SimpleVideo_Stop);
                sv_i2s_running = 1u;
                sv_i2s_started = 1u;
                return FM_OK;
            }
#endif
            if (hi2s1.State != HAL_I2S_STATE_READY)
            {
                (void)HAL_I2S_DMAStop(&hi2s1);
            }
            memset(sv_i2s_buf, 0, sizeof(sv_i2s_buf));
            sv_i2s_cache_clean(sv_i2s_buf, sizeof(sv_i2s_buf));
            SimpleI2S_Take(sv_i2s_half, sv_i2s_full, sv_i2s_err,
                           SimpleVideo_Stop);
            if (HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)sv_i2s_buf,
                                     (uint16_t)(2u *
                                                SIMPLE_VIDEO_HALF_SAMPLES)) ==
                HAL_OK)
            {
                sv_i2s_running = 1u;
                sv_i2s_started = 1u;
            }
            else
            {
                SimpleI2S_Drop(SimpleVideo_Stop);
            }
        }
        else
        {
            sv_i2s_started = 1u;
        }
        return FM_OK;
    }
    rate = sv.audio.sample_rate;
    if (!sv_i2s_rate_ok(rate) ||
        (sv.audio.channels != 1u && sv.audio.channels != 2u) ||
        sv.audio.bits_per_sample != 16u)
    {
        FM_Log("SimpleVideo: PCM %lu Hz %uch %ubit not I2S-playable\n",
               (unsigned long)rate, (unsigned)sv.audio.channels,
               (unsigned)sv.audio.bits_per_sample);
        sv.audio_pcm = FM_FALSE;
        return FM_OK;
    }
    if (hi2s1.Init.DataFormat != I2S_DATAFORMAT_16B)
    {
        FM_Log("SimpleVideo: CubeMX I2S DataFormat must be 16-bit\n");
        return FM_ERR_UNSUPPORTED;
    }
#if SIMPLE_VIDEO_KEEP_I2S_CLOCKS
    if (sv_i2s_started == 0u && hi2s1.State == HAL_I2S_STATE_BUSY_TX &&
        hi2s1.Init.AudioFreq == rate)
    {
        /* Clocks were kept running across the previous stop (the ring
         * holds silence): re-arm the hooks and let the normal IRQ
         * refills swap in the new audio with the fade-in.  Overwriting
         * a half the DMA is currently playing would glitch, so nothing
         * is written here — the refill flags target the idle half
         * only.  No clock stop/start means no amp re-lock pop at the
         * item boundary. */
        SimpleI2S_Take(sv_i2s_half, sv_i2s_full, sv_i2s_err,
                       SimpleVideo_Stop);
        sv_i2s_running = 1u;
        sv_i2s_started = 1u;
        FM_Log("SimpleVideo: I2S PCM %lu Hz stereo (clocks kept running, "
               "src %uch)\n",
               (unsigned long)rate, (unsigned)sv.audio.channels);
        return FM_OK;
    }
#endif
    r = sv_i2s_fill_half(0);
    if (r != FM_OK)
    {
        return r;
    }
    r = sv_i2s_fill_half(1);
    if (r != FM_OK)
    {
        return r;
    }
    if (hi2s1.Init.AudioFreq != rate)
    {
        if (hi2s1.State != HAL_I2S_STATE_READY)
        {
            /* The sample rate really changes: the clocks must stop for
             * the re-init below.  This is the one remaining boundary
             * that can make an amp re-lock pop (rare — mixed-rate
             * playlists only). */
            (void)HAL_I2S_DMAStop(&hi2s1);
        }
        if (HAL_I2S_DeInit(&hi2s1) != HAL_OK)
        {
            return FM_ERR_IO;
        }
        hi2s1.Init.AudioFreq = rate;
        if (HAL_I2S_Init(&hi2s1) != HAL_OK)
        {
            return FM_ERR_IO;
        }
    }
    else if (hi2s1.State != HAL_I2S_STATE_READY)
    {
        (void)HAL_I2S_DMAStop(&hi2s1);
    }
    SimpleI2S_Take(sv_i2s_half, sv_i2s_full, sv_i2s_err, SimpleVideo_Stop);
    if (HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)sv_i2s_buf,
                             (uint16_t)(2u * SIMPLE_VIDEO_HALF_SAMPLES)) !=
        HAL_OK)
    {
        SimpleI2S_Drop(SimpleVideo_Stop);
        return FM_ERR_IO;
    }
    sv_i2s_running = 1u;
    sv_i2s_started = 1u;
    FM_Log("SimpleVideo: I2S PCM %lu Hz stereo (src %uch)\n",
           (unsigned long)rate, (unsigned)sv.audio.channels);
    return FM_OK;
}

static FM_Result sv_i2s_poll(void)
{
    uint8_t half;
    FM_Result r;

    /* Paused: the ring is kept silent by the pause branch of
     * SimpleVideo_Poll(); refilling from the demux here would leak
     * audio into a "paused" output and advance the audio cursor. */
    if (sv.paused)
    {
        return FM_OK;
    }
    /* One-shot recovery after an I2S DMA error: stop, re-arm the ring
     * from the demux cursor and restart the transfer.  Bounded so a
     * physically broken I2S cannot spin here forever. */
    if (sv_i2s_restart_req != 0u && sv_i2s_started != 0u &&
        !sv_i2s_running && sv_i2s_restarts < 3u)
    {
        sv_i2s_restart_req = 0u;
        sv_i2s_restarts++;
        (void)HAL_I2S_DMAStop(&hi2s1);
        sv_i2s_refill[0] = sv_i2s_refill[1] = 0;
#if SIMPLE_VIDEO_FADE_FRAMES > 0u
        /* The re-kick jumps into the stream wherever the cursor is: a
         * short ramp keeps that discontinuity inaudible. */
        sv_i2s_fade_in = (uint32_t)SIMPLE_VIDEO_FADE_FRAMES;
#endif
        if (sv_i2s_fill_half(0) == FM_OK && sv_i2s_fill_half(1) == FM_OK &&
            hi2s1.State == HAL_I2S_STATE_READY &&
            HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)sv_i2s_buf,
                                 (uint16_t)(2u *
                                            SIMPLE_VIDEO_HALF_SAMPLES)) ==
                HAL_OK)
        {
            sv_i2s_running = 1u;
            sv_i2s_started = 1u;
            FM_Log("SimpleVideo: I2S restarted after error (%lu/%u)\n",
                   (unsigned long)sv_i2s_restarts, 3u);
        }
    }
    if (!sv_i2s_running)
    {
        return FM_OK;
    }
    /* Report underruns from the IRQs: each hit is a stale half replayed
     * and audible as a crackle — i.e. the measurable part of "خش".
     * Starve > 0 with crackle = the ring is too small for the decode/SD
     * spikes (raise SIMPLE_VIDEO_HALF_SAMPLES); starve = 0 with crackle
     * points at the analog side instead. */
    if (sv_i2s_starve != sv_i2s_starve_logged &&
        (sv_i2s_starve == 1u ||
         sv_i2s_starve >= sv_i2s_starve_logged + 100u))
    {
        sv_i2s_starve_logged = sv_i2s_starve;
        FM_Log("SimpleVideo: I2S ring starved %lu times — audio crackle "
               "(raise SIMPLE_VIDEO_HALF_SAMPLES?)\n",
               (unsigned long)sv_i2s_starve);
    }
    /* Silent file with the DMA kept running (continuous I2S clock so
     * the amplifier stays quiet): keep the ring zeroed.
     * sv_i2s_fill_half() writes silence when !sv.audio_pcm. */
    for (half = 0; half < 2u; half++)
    {
        if (sv_i2s_refill[half] == 0u)
        {
            continue;
        }
        {
            uint32_t t_irq = sv_i2s_irq_tick[half];
            uint32_t t_now = HAL_GetTick();
            uint32_t lat = t_now - t_irq;

            sv_i2s_refill[half] = 0u;
            sv_i2s_lat_sum += lat;
            sv_i2s_lat_n++;
            if (lat > sv_i2s_lat_max)
            {
                sv_i2s_lat_max = lat;
            }
        }
        r = sv_i2s_fill_half(half);
        if (r != FM_OK && r != FM_ERR_EOF)
        {
            return r;
        }
    }
    return FM_OK;
}

/* FM_Idle hook: decoders (MJPEG per MCU row, H.264 per NAL, image
 * loaders) call FM_Idle() during long decodes.  Refilling the I2S
 * halves from here is what keeps video audio gapless — polling only
 * once per main-loop pass starves the DMA whenever one frame takes
 * longer than half the ring buffer (~186 ms), which is exactly the
 * stutter/crackle heard on MJPEG+PCM files. */
static void simple_video_idle(void)
{
#if SIMPLE_VIDEO_IDLE_REFILL
    static volatile uint8_t busy;

    if (busy || !sv_i2s_running)
    {
        return;
    }
    busy = 1u;
    (void)sv_i2s_poll();
    busy = 0u;
#endif
}
#endif /* SIMPLE_VIDEO_I2S */

static uint32_t simple_video_fps(void)
{
    uint32_t fps = sv.player.video.info.fps;

    return (fps == 0u) ? 10u : fps;
}

static double simple_video_duration(void)
{
    uint32_t fps = simple_video_fps();
    uint32_t n = sv.player.video.info.frame_count;

    return (fps == 0u) ? 0.0 : ((double)n / (double)fps);
}

static uint32_t simple_video_now_elapsed(void)
{
    if (sv.paused)
    {
        return sv.elapsed_ms;
    }
    return HAL_GetTick() - sv.t0_ms;
}

#if SIMPLE_VIDEO_PROFILE > 0u
/* Once per profile window, log the per-stage average frame cost. Called by
 * simple_video_present() after every frame (displayed or data-only). */
static void simple_video_profile(void)
{
    sv.prof_frames++;
    if (sv.prof_frames >= (uint32_t)SIMPLE_VIDEO_PROFILE)
    {
        FM_Log("SimpleVideo: avg ms/frame decode=%lu convert=%lu "
               "present=%lu (over %lu frames)"
#if SIMPLE_VIDEO_I2S
               " i2s_starve=%lu i2s_refill=%lu avg=%lums latmax=%lums"
               " restarts=%lu rd_errs=%lu"
#endif
               "\n",
               (unsigned long)(sv.prof_decode / sv.prof_frames),
               (unsigned long)(sv.prof_convert / sv.prof_frames),
               (unsigned long)(sv.prof_present / sv.prof_frames),
               (unsigned long)sv.prof_frames
#if SIMPLE_VIDEO_I2S
               ,
               (unsigned long)sv_i2s_starve,
               (unsigned long)sv_i2s_lat_n,
               (sv_i2s_lat_n != 0u)
                   ? (unsigned long)(sv_i2s_lat_sum / sv_i2s_lat_n)
                   : 0ul,
               (unsigned long)sv_i2s_lat_max,
               (unsigned long)sv_i2s_restarts,
               (unsigned long)sv_i2s_read_errs
#endif
        );
        sv.prof_decode = sv.prof_convert = sv.prof_present = 0u;
#if SIMPLE_VIDEO_I2S
        sv_i2s_read_errs = 0u;   /* شمارش خطای خواندن SD در همین پنجره */
#endif
        sv.prof_frames = 0u;
#if SIMPLE_VIDEO_I2S
        sv_i2s_lat_n = 0u;
        sv_i2s_lat_sum = 0u;
        sv_i2s_lat_max = 0u;
#endif
    }
}
#endif

static void simple_video_present(void)
{
    size_t bytes = 0;
    uint32_t t0;

    if (!sv.have_frame)
    {
        return;
    }
    (void)Image_BufferSize(sv.out_w, sv.out_h, sv.format, &bytes);

    /* Frame data callback for DSP/image processing.  Fires even when no
     * present callback is installed (data-only mode), so an analysis
     * pipeline can consume every decoded frame without an LCD.  The frame
     * buffer was just written by the CPU; a CPU consumer needs no cache
     * clean (that flush is only required before the present DMA). */
    if (sv.frame_cb != NULL)
    {
        sv.frame_cb(sv.frame_cb_ctx, sv_frame[sv_slot],
                    sv.out_w, sv.out_h, sv.format, bytes);
    }

    if (sv.present != NULL)
    {
        t0 = HAL_GetTick();
        /* With the async-present recipe (present = LcdSpiDma_Begin, wait =
         * LcdSpiDma_Wait) this drains the previous frame's SPI DMA here —
         * its transfer time already overlapped the decode/convert stages
         * above, so only the leftover tail shows up in the profile. */
        if (sv_present_wait != NULL)
        {
            sv_present_wait();
        }
        /* CRITICAL: flush CPU writes (D-Cache + write buffer) so the
         * presentation DMA sees the fresh frame data.  Without this, the
         * DMA reads stale SDRAM content while the CPU's new pixels are
         * still in cache or the write buffer — causing visual corruption
         * (noise/artifacts) on the display.  The same write-buffer effect
         * also starves the audio refill if the main loop blocks here,
         * producing simultaneous audio crackle. */
        FM_Cache_CleanForDma(sv_frame[sv_slot], bytes);
        sv.present(sv_frame[sv_slot], sv.out_w, sv.out_h, sv.format, bytes);
        sv.prof_present += HAL_GetTick() - t0;
    }

    /* Advance the ping-pong slot so the next decode does not overwrite the
     * frame we just handed to present/frame_cb. */
    sv_show = sv_slot;
#if SIMPLE_VIDEO_FRAME_COUNT > 1u
    sv_slot ^= 1u;
#endif
#if SIMPLE_VIDEO_PROFILE > 0u
    simple_video_profile();
#endif
}

static FM_Result simple_video_decode_one(void)
{
    FM_Result r;
    size_t rgb_need;
    size_t out_need;
    uint32_t prof_t0;
    uint16_t nw = sv.player.video.info.width;
    uint16_t nh = sv.player.video.info.height;

    if (nw == 0u || nh == 0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    r = Image_BufferSize(nw, nh, FM_PIXEL_RGB888, &rgb_need);
    if (r != FM_OK || rgb_need > sizeof(sv_rgb))
    {
        if (r == FM_OK && !sv.size_logged)
        {
            sv.size_logged = 1u;
            FM_Log("SimpleVideo: %s is %ux%u but decode buffer is max "
                   "%ux%u — re-encode the file smaller or raise "
                   "SIMPLE_VIDEO_MAX_W/H\n",
                   Player_ItemPath(&sv.player, Player_Current(&sv.player)),
                   (unsigned)nw, (unsigned)nh,
                   (unsigned)SIMPLE_VIDEO_MAX_W,
                   (unsigned)SIMPLE_VIDEO_MAX_H);
        }
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }
    {
        uint32_t t0 = HAL_GetTick();

        r = Video_DecodeFrame(&sv.player.video, sv_rgb, sizeof(sv_rgb));
        sv.prof_decode += HAL_GetTick() - t0;
    }
    if (r != FM_OK)
    {
        return r;
    }
    FM_Idle();
    prof_t0 = HAL_GetTick();
    /*
     * پیش‌فرض: خروجی هم‌اندازهٔ خود فایل، یعنی بدون مقیاس‌دهی.
     * مقیاس‌دهی فقط دو حالت دارد: یا خودتان SetOutputSize خواسته‌اید،
     * یا فریم در بافر جا نمی‌شود.
     */
    sv.out_w = (sv.out_w == 0u) ? nw : sv.out_w;
    sv.out_h = (sv.out_h == 0u) ? nh : sv.out_h;
    if (sv.out_w > SIMPLE_VIDEO_MAX_W || sv.out_h > SIMPLE_VIDEO_MAX_H)
    {
        /*
         * بافر کوچک‌تر از فریم است. به‌جای رد کردن فایل، کوچکش
         * می‌کنیم — ولی یک بار می‌گوییم چرا، چون این مقیاس‌دهی گران
         * است و کاربر باید بداند با بزرگ‌کردن بافر از بین می‌رود.
         */
        if (!sv.scale_logged)
        {
            sv.scale_logged = 1u;
            FM_Log("SimpleVideo: %ux%u بزرگ‌تر از بافر %ux%u است — "
                   "کوچک می‌شود (SIMPLE_VIDEO_MAX_W/H را زیاد کنید تا "
                   "بدون مقیاس‌دهی و سریع‌تر پخش شود)\n",
                   (unsigned)nw, (unsigned)nh,
                   (unsigned)SIMPLE_VIDEO_MAX_W,
                   (unsigned)SIMPLE_VIDEO_MAX_H);
        }
        if (sv.out_w > SIMPLE_VIDEO_MAX_W)
        {
            sv.out_w = SIMPLE_VIDEO_MAX_W;
        }
        if (sv.out_h > SIMPLE_VIDEO_MAX_H)
        {
            sv.out_h = SIMPLE_VIDEO_MAX_H;
        }
    }
    r = Image_BufferSize(sv.out_w, sv.out_h, sv.format, &out_need);
    if (r != FM_OK || out_need > sv_draw_cap())
    {
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }
    if (sv.out_w != nw || sv.out_h != nh)
    {
        size_t scaled_rgb = (size_t)sv.out_w * sv.out_h * 3u;

        if (sv.format == FM_PIXEL_RGB565 || sv.format == FM_PIXEL_BGR565)
        {
            /* One pass: nearest-neighbour scale RGB888 → packed 565.
             * Previously: Scale into sv_draw, memcpy back to sv_rgb,
             * then convert — three SDRAM walks.  On 720×480→320×240
             * that was the 80+ ms "convert" line. */
            uint16_t x;
            uint16_t y;
            uint8_t bgr = (sv.format == FM_PIXEL_BGR565) ? 1u : 0u;
            uint8_t swap = sv_swap16_active();
            uint8_t *dst = sv_draw();
            uint16_t xs[SIMPLE_VIDEO_MAX_W];

            if (sv.out_w > SIMPLE_VIDEO_MAX_W)
            {
                return FM_ERR_TOO_LARGE;
            }
            for (x = 0; x < sv.out_w; x++)
            {
                xs[x] = (uint16_t)((uint32_t)x * nw / sv.out_w);
            }
            for (y = 0; y < sv.out_h; y++)
            {
                const uint8_t *srow = sv_rgb +
                    (size_t)((uint32_t)y * nh / sv.out_h) * nw * 3u;
                uint8_t *drow = dst + (size_t)y * sv.out_w * 2u;

                if ((y & 7u) == 0u)
                {
                    FM_Idle();
                }
                for (x = 0; x < sv.out_w; x++)
                {
                    const uint8_t *p = srow + (size_t)xs[x] * 3u;
                    uint32_t r8 = p[0];
                    uint32_t g8 = p[1];
                    uint32_t b8 = p[2];
                    uint16_t v;

                    if (bgr)
                    {
                        uint32_t t = r8;

                        r8 = b8;
                        b8 = t;
                    }
                    v = (uint16_t)(((r8 & 0xF8u) << 8) |
                                   ((g8 & 0xFCu) << 3) |
                                   (b8 >> 3));
                    if (swap)
                    {
                        drow[0] = (uint8_t)(v >> 8);
                        drow[1] = (uint8_t)v;
                    }
                    else
                    {
                        drow[0] = (uint8_t)v;
                        drow[1] = (uint8_t)(v >> 8);
                    }
                    drow += 2;
                }
            }
            sv.prof_convert += HAL_GetTick() - prof_t0;
            sv.have_frame = FM_TRUE;
            sv.shown_frame = sv.player.video.current_frame;
            return FM_OK;
        }
        if (sv.format != FM_PIXEL_RGB888)
        {
            if (scaled_rgb > sv_draw_cap())
            {
                return FM_ERR_TOO_LARGE;
            }
        }
        r = Image_Scale(sv_rgb, nw, nh, FM_PIXEL_RGB888, sv_draw(),
                        sv.out_w, sv.out_h, FM_SCALE_NEAREST);
        if (r != FM_OK)
        {
            return r;
        }
        if (sv.format != FM_PIXEL_RGB888)
        {
            FM_Image src;
            FM_Image dst;

            memset(&src, 0, sizeof(src));
            memset(&dst, 0, sizeof(dst));
            src.width = sv.out_w;
            src.height = sv.out_h;
            src.format = FM_PIXEL_RGB888;
            src.data = sv_draw();
            src.data_capacity = scaled_rgb;
            src.data_size = scaled_rgb;
            memcpy(sv_rgb, sv_draw(),
                   (scaled_rgb < sizeof(sv_rgb)) ? scaled_rgb : sizeof(sv_rgb));
            src.data = sv_rgb;
            dst.data = sv_draw();
            dst.data_capacity = sv_draw_cap();
            r = Image_Convert(&src, &dst, sv.format);
            if (r != FM_OK)
            {
                return r;
            }
        }
    }
    else if (sv.format == FM_PIXEL_RGB888)
    {
        memcpy(sv_draw(), sv_rgb, rgb_need);
    }
    else if (sv.format == FM_PIXEL_RGB565 || sv.format == FM_PIXEL_BGR565)
    {
        /* Fast path: fused convert(+swap) in one pass over the frame. */
        sv_convert_565(sv_rgb, sv_draw(), nw, nh,
                       (sv.format == FM_PIXEL_BGR565) ? 1u : 0u,
                       sv_swap16_active());
        sv.prof_convert += HAL_GetTick() - prof_t0;
        sv.have_frame = FM_TRUE;
        sv.shown_frame = sv.player.video.current_frame;
        return FM_OK;
    }
    else
    {
        FM_Image src;
        FM_Image dst;

        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.width = nw;
        src.height = nh;
        src.format = FM_PIXEL_RGB888;
        src.data = sv_rgb;
        src.data_capacity = rgb_need;
        src.data_size = rgb_need;
        dst.data = sv_draw();
        dst.data_capacity = sv_draw_cap();
        r = Image_Convert(&src, &dst, sv.format);
        if (r != FM_OK)
        {
            return r;
        }
    }
    simple_video_swap16(sv_draw(), out_need);
    sv.prof_convert += HAL_GetTick() - prof_t0;
    sv.have_frame = FM_TRUE;
    sv.shown_frame = sv.player.video.current_frame;
    return FM_OK;
}

static FM_Result simple_video_sync_audio(void)
{
    uint32_t dur_ms;
    uint32_t idx;

    if (!sv.has_audio || sv.audio.sample_count == 0u)
    {
        return FM_OK;
    }
    dur_ms = (uint32_t)(simple_video_duration() * 1000.0);
    if (dur_ms == 0u)
    {
        return FM_OK;
    }
    idx = (simple_video_now_elapsed() * sv.audio.sample_count) / dur_ms;
    if (idx >= sv.audio.sample_count)
    {
        idx = sv.audio.sample_count - 1u;
    }
    return Video_SeekAudio(&sv.player.video, idx);
}

/* Open item `index`, probe its audio track, arm the I2S sink and decode
 * the first frame.  Returns FM_OK only when the item is actually
 * playing (first frame decoded and presented). */
static FM_Result simple_video_prime_item(uint32_t index)
{
    FM_Result r;
    FM_Bool has = FM_FALSE;

    r = Player_Play(&sv.player, index);
    if (r != FM_OK)
    {
        return r;
    }
#if !SIMPLE_VIDEO_ENABLE_H264
    /* Gate on the codec the container actually declares, not on the
     * file extension: an .avi carrying H.264 used to slip through the
     * extension check and get decoded anyway (37–85 ms/frame, i.e. the
     * stutter this switch exists to prevent), while an .mp4 holding
     * MJPEG was skipped although it plays fine. */
    if (sv.player.video.info.codec == FM_VIDEO_CODEC_H264)
    {
        FM_Log("SimpleVideo: %s is H.264 — skipped "
               "(SIMPLE_VIDEO_ENABLE_H264=0; re-encode as MJPEG+PCM AVI)\n",
               Player_ItemPath(&sv.player, index));
        (void)Player_Stop(&sv.player);
        return FM_ERR_UNSUPPORTED;
    }
#endif
    sv.format = sv_active_format();
    sv.have_frame = FM_FALSE;
    sv.paused = FM_FALSE;
    sv.elapsed_ms = 0u;
    sv.t0_ms = HAL_GetTick();
    sv.size_logged = 0u;
    sv.scale_logged = 0u;
    /*
     * اندازهٔ خروجی را برای هر فایل از نو حساب کن.
     *
     * قبلاً out_w/out_h بین فایل‌ها می‌ماند: اگر کلیپ اول ۳۲۰×۲۴۰
     * بود، کلیپ ۷۲۰×۴۸۰ بعدی هم به زور ۳۲۰×۲۴۰ می‌شد — حتی اگر
     * نمایشگر بزرگ‌تر بود و هیچ‌کس مقیاس‌دهی نخواسته بود. حالا فقط
     * وقتی می‌ماند که خود برنامه SetOutputSize را صدا زده باشد.
     */
    if (!sv.out_size_user)
    {
        sv.out_w = 0u;
        sv.out_h = 0u;
    }
    sv.h264_fail_run = 0u;
    sv.h264_diag_logged = 0u;
    sv.frame_cost_ema = 0u;
    sv.slow_probe = 0u;
    sv.has_audio = FM_FALSE;
    sv.audio_pcm = FM_FALSE;
    memset(&sv.audio, 0, sizeof(sv.audio));
    if (sv_audio_enabled && Video_HasAudio(&sv.player.video, &has) == FM_OK && has)
    {
        if (Video_GetAudioInfo(&sv.player.video, &sv.audio) == FM_OK)
        {
            sv.has_audio = FM_TRUE;
            if (sv.audio.codec == FM_VIDEO_AUDIO_PCM &&
                (sv.audio.bits_per_sample == 16u ||
                 sv.audio.bits_per_sample == 8u))
            {
                /* 8-bit PCM is up-converted to 16-bit in fm_video.c
                 * Video_ReadAudioFrame, so I2S can play it. */
                sv.audio_pcm = FM_TRUE;
            }
            else if ((sv.audio.codec == FM_VIDEO_AUDIO_MP3 ||
                      sv.audio.codec == FM_VIDEO_AUDIO_ADPCM ||
                      sv.audio.codec == FM_VIDEO_AUDIO_ALAW ||
                      sv.audio.codec == FM_VIDEO_AUDIO_ULAW) &&
                     sv.audio.bits_per_sample == 16u)
            {
                /* Video_ReadAudioFrame decodes these to 16-bit PCM in
                 * fm_video.c (minimp3 / G.711 / ADPCM) so I2S can play. */
                sv.audio_pcm = FM_TRUE;
            }
#if FM_USE_FLAC || FM_USE_OPUS || FM_USE_VORBIS || FM_USE_AAC
            else if (sv.audio.codec == FM_VIDEO_AUDIO_AAC ||
                     sv.audio.codec == FM_VIDEO_AUDIO_FLAC ||
                     sv.audio.codec == FM_VIDEO_AUDIO_OPUS ||
                     sv.audio.codec == FM_VIDEO_AUDIO_VORBIS)
            {
                const char *need = (sv.audio.codec == FM_VIDEO_AUDIO_AAC) ? "AAC" :
                                   (sv.audio.codec == FM_VIDEO_AUDIO_FLAC) ? "FLAC" :
                                   (sv.audio.codec == FM_VIDEO_AUDIO_OPUS) ? "OPUS" : "VORBIS";
                const FM_AudioDecoderOps *dec = Audio_GetDecoder(need);
                if (dec != NULL)
                {
                    sv.audio_pcm = FM_TRUE;
                }
#if SIMPLE_VIDEO_I2S
                else if (!sv_i2s_logged_aac)
                {
                    sv_i2s_logged_aac = 1u;
                    FM_Log("SimpleVideo: %s track demuxed, no %s decoder "
                           "(speaker silent, extraction still works via "
                           "Video_ReadAudioFrame). Register with "
                           "Audio_RegisterDecoder.\n", need, need);
                }
#endif
            }
#endif
#if SIMPLE_VIDEO_I2S
            else if (sv.audio.codec == FM_VIDEO_AUDIO_AAC &&
                     !sv_i2s_logged_aac)
            {
                sv_i2s_logged_aac = 1u;
                FM_Log("SimpleVideo: AAC track demuxed, no AAC decoder "
                       "(speaker silent). Use AVI+PCM for audio.\n");
            }
#endif
        }
    }
#if SIMPLE_VIDEO_I2S
    (void)sv_i2s_board_stop();
    sv_pcm_len = 0u;
    sv_pcm_off = 0u;
#endif
    r = simple_video_decode_one();
    if (r != FM_OK)
    {
#if FM_USE_H264
        if (sv.player.video.info.codec == FM_VIDEO_CODEC_H264)
        {
            const FM_H264Diag *d = H264_GetLastError();

            FM_Log("SimpleVideo: h264 first-frame diag res=%lu in=%lu "
                   "used=%lu skip=%lu resets=%lu allocfail=%lu act=%lu "
                   "aub=%lu/pps%lu sps_seen=%lu pps_seen=%lu skipnal=%lu "
                   "head=%02X %02X %02X %02X %02X %02X %02X %02X "
                   "fail=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                   (unsigned long)d->bsd_result,
                   (unsigned long)d->input_len,
                   (unsigned long)d->consumed,
                   (unsigned long)d->skipped,
                   (unsigned long)d->resets,
                   (unsigned long)d->alloc_failures,
                   (unsigned long)d->activate_fail,
                   (unsigned long)d->aub_fail,
                   (unsigned long)d->aub_pps_id,
                   (unsigned long)d->sps_seen,
                   (unsigned long)d->pps_seen,
                   (unsigned long)d->skip_nal_type,
                   d->first_bytes[0], d->first_bytes[1],
                   d->first_bytes[2], d->first_bytes[3],
                   d->first_bytes[4], d->first_bytes[5],
                   d->first_bytes[6], d->first_bytes[7],
                   d->fail_bytes[0], d->fail_bytes[1],
                   d->fail_bytes[2], d->fail_bytes[3],
                   d->fail_bytes[4], d->fail_bytes[5],
                   d->fail_bytes[6], d->fail_bytes[7]);
        }
#endif
        return r;
    }
    simple_video_present();
#if SIMPLE_VIDEO_I2S
    /* Start the audio clock AFTER the first picture is on the panel and
     * anchor the video timeline to the same instant — removes the
     * systematic A/V offset of decoding the first frame while the I2S
     * ring was already playing. */
    r = sv_i2s_start();
    if (r != FM_OK)
    {
        FM_Log("SimpleVideo: I2S start %s (%d) — video continues silent\n",
               File_GetErrorString(r), (int)r);
    }
#endif
    sv.t0_ms = HAL_GetTick();
    sv.elapsed_ms = 0u;
    FM_Log("SimpleVideo: %s  %ux%u @ %lu fps  audio=%u\n",
           Player_ItemPath(&sv.player, Player_Current(&sv.player)),
           (unsigned)sv.player.video.info.width,
           (unsigned)sv.player.video.info.height,
           (unsigned long)simple_video_fps(),
           (unsigned)sv.has_audio);
    return FM_OK;
}

/* Case-insensitive compare of a NUL-terminated extension. */
static FM_Bool sv_ext_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + ('a' - 'A')); }
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + ('a' - 'A')); }
        if (ca != cb)
        {
            return FM_FALSE;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? FM_TRUE : FM_FALSE;
}

/* True only for raw H.264 elementary streams, which cannot hold any
 * other codec.  Containers (.avi/.mp4/.m4v) are NOT judged by their
 * extension any more: an .mp4 may carry MJPEG and an .avi may carry
 * H.264, so those are gated on the parsed codec in
 * simple_video_prime_item() after the header has been read. */
__attribute__((unused))   /* only called with SIMPLE_VIDEO_ENABLE_H264=0 */
static FM_Bool sv_path_is_h264(const char *path)
{
    size_t n;

    if (path == NULL)
    {
        return FM_FALSE;
    }
    n = strlen(path);
    if (n >= 4u && sv_ext_eq(path + (n - 4u), ".264"))
    {
        return FM_TRUE;
    }
    if (n >= 5u && sv_ext_eq(path + (n - 5u), ".h264"))
    {
        return FM_TRUE;
    }
    return FM_FALSE;
}

static FM_Result simple_video_start_index(uint32_t index);
static FM_Result simple_video_apply_pending(void);

/* Natural end of an item (EOF, or an item given up as too slow).
 * Behaviour depends on play mode (presentation):
 *  STOP       -> stop here (stay on last frame, click-free fade)
 *  NEXT       -> next file, last -> stop (presentation, no loop)
 *  REPEAT_ONE -> replay same file infinitely
 *  REPEAT_ALL -> next with wrap (historic autoplay=1 loop) */
/* Snapshot the current playback item (path + index) into sv. Used at item
 * start so the end callback can report the same item that was announced. */
static void sv_snapshot_current_item(void)
{
    const char *path = NULL;

    if (sv.player_ok && Player_Count(&sv.player) > 0u)
    {
        path = Player_ItemPath(&sv.player, Player_Current(&sv.player));
        sv.cur_index = Player_Current(&sv.player);
    }
    else
    {
        sv.cur_index = 0u;
    }
    if (path == NULL)
    {
        path = "";
    }
    (void)strncpy(sv.cur_path, path, sizeof(sv.cur_path) - 1u);
    sv.cur_path[sizeof(sv.cur_path) - 1u] = '\0';
}

/* Fire the track-start callback for the current item and remember it. */
static void sv_fire_item_start(void)
{
    sv_snapshot_current_item();
    sv.item_started = 1u;
    if (sv.track_start != NULL)
    {
        sv.track_start(sv.track_start_ctx, sv.cur_path,
                       sv.out_w, sv.out_h, simple_video_fps());
    }
    FM_Log("SimpleVideo: track start %s (%ux%u @ %lu fps)\n",
           sv.cur_path, (unsigned)sv.out_w, (unsigned)sv.out_h,
           (unsigned long)simple_video_fps());
}

/* Fire the track-end callback once for the item that just finished. */
static void sv_fire_item_end(void)
{
    if (!sv.item_started)
    {
        return;
    }
    sv.item_started = 0u;
    if (sv.track_end != NULL)
    {
        sv.track_end(sv.track_end_ctx, sv.cur_path);
    }
    FM_Log("SimpleVideo: track end %s\n", sv.cur_path);
}

static FM_PlayMode sv_effective_play_mode(void)
{
    if (sv_play_mode_explicit) return sv_play_mode;
    return sv_autoplay_active() ? FM_PLAY_MODE_REPEAT_ALL : FM_PLAY_MODE_STOP;
}
static FM_Result sv_playlist_advance(void)
{
    uint32_t count = Player_Count(&sv.player);
    uint32_t cur = Player_Current(&sv.player);
    FM_PlayMode m = sv_effective_play_mode();

    if (count == 0u)
    {
        (void)SimpleVideo_Stop();
        return FM_ERR_NOT_FOUND;
    }
    /* The current item has finished (EOF / given up as too slow): report
     * its end before deciding what plays next. */
    sv_fire_item_end();
    if (m == FM_PLAY_MODE_STOP)
    {
        FM_Log("SimpleVideo: item ended - STOP (stay on last frame)\n");
        (void)SimpleVideo_Stop();
        return FM_OK;
    }
    if (m == FM_PLAY_MODE_REPEAT_ONE)
    {
        FM_Log("SimpleVideo: item ended - REPEAT_ONE -> replay %s\n",
               Player_ItemPath(&sv.player, cur));
        return simple_video_start_index(cur);
    }
    if (m == FM_PLAY_MODE_NEXT)
    {
        if (cur + 1u >= count)
        {
            FM_Log("SimpleVideo: item ended - NEXT at end -> STOP (presentation end)\n");
            (void)SimpleVideo_Stop();
            return FM_OK;
        }
        sv.pending_cmd = SV_CMD_NEXT;
        return simple_video_apply_pending();
    }
    /* REPEAT_ALL: loop forever */
    sv.pending_cmd = SV_CMD_NEXT;
    return simple_video_apply_pending();
}

/* Start playing at `index`.  Items that cannot be opened or whose first
 * frame does not decode are skipped (with a log line) exactly like the
 * SimpleVideo_Poll error path does, so one bad file can neither fail
 * SimpleVideo_Begin() nor kill the playlist. */
static FM_Result simple_video_start_index(uint32_t index)
{
    uint32_t count = Player_Count(&sv.player);
    uint32_t tried;
    FM_Result r = FM_ERR_NOT_FOUND;

    if (count == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
    /* Replacing a playing item (manual Next/Prev/Play): report its end
     * before the new item starts.  Natural EOF does this in
     * sv_playlist_advance() instead (which has already cleared the flag). */
    if (sv.running && sv.item_started)
    {
        sv_fire_item_end();
    }
    for (tried = 0; tried < count; tried++)
    {
        uint32_t idx = (index + tried) % count;

#if !SIMPLE_VIDEO_ENABLE_H264
        /* H.264 is not usable on this port (see the note at the top of
         * this file).  Skip those items up front instead of spending a
         * decode attempt, a decoder reset and a failure message on each
         * one every time the playlist wraps. */
        if (sv_path_is_h264(Player_ItemPath(&sv.player, idx)))
        {
            r = FM_ERR_UNSUPPORTED;
            continue;
        }
#endif
        r = simple_video_prime_item(idx);
        if (r == FM_OK)
        {
            sv.running = FM_TRUE;
            sv_fire_item_start();
            return FM_OK;
        }
        FM_Log("SimpleVideo: %s unplayable at start: %s (%d)%s\n",
               Player_ItemPath(&sv.player, idx),
               File_GetErrorString(r), (int)r,
               (tried + 1u < count) ? " — trying next" : "");
#if FM_USE_H264   /* the diagnosis below is the H.264 decoder's */
        if (r == FM_ERR_NO_MEMORY)
        {
            const FM_H264Diag *d = H264_GetLastError();

            /* h264bsd needs ~800 KB of FM_Alloc for a 320x240 stream
             * (context + DPB reference pictures) and several MB at
             * 720x480.  Only blame the pool when the allocator really
             * refused something: printing "enlarge the pool" at a board
             * whose pool is 99% free sends the user chasing the wrong
             * problem. */
            if (d->alloc_failures != 0u)
            {
                FM_Log("SimpleVideo: decoder out of memory (%lu refused "
                       "allocation(s)) — route FM_Alloc to SDRAM "
                       "(example 22: SdramAlloc_Init() before "
                       "FileManager_Init) or raise SDRAM_ALLOC_BYTES; "
                       "H.264 320x240 needs ~800KB, 720x480 several MB\n",
                       (unsigned long)d->alloc_failures);
            }
            else
            {
                FM_Log("SimpleVideo: decoder reported out of memory but "
                       "no allocation was refused (act=%lu) — the pool is "
                       "fine; this is a parameter-set activation failure. "
                       "Check that h264bsd is built with "
                       "-fno-strict-aliasing and that the SPS/PPS reach "
                       "the decoder intact\n",
                       (unsigned long)d->activate_fail);
            }
        }
#endif /* FM_USE_H264 */
    }
    FM_Log("SimpleVideo: no playable video, stopping\n");
    (void)SimpleVideo_Stop();
    return r;
}

static FM_Result simple_video_apply_pending(void)
{
    uint8_t cmd = sv.pending_cmd;
    uint32_t count;
    uint32_t idx;
    FM_Result r;

    sv.pending_cmd = SV_CMD_NONE;
    count = Player_Count(&sv.player);
    if (cmd == SV_CMD_START)
    {
        return simple_video_start_index(Player_Current(&sv.player));
    }
    if (cmd == SV_CMD_NEXT)
    {
        idx = (count == 0u) ? 0u : (Player_Current(&sv.player) + 1u) % count;
        return simple_video_start_index(idx);
    }
    if (cmd == SV_CMD_PREV)
    {
        idx = (count == 0u) ? 0u
                            : (Player_Current(&sv.player) == 0u)
                                  ? (count - 1u)
                                  : (Player_Current(&sv.player) - 1u);
        return simple_video_start_index(idx);
    }
    if (cmd == SV_CMD_PLAY)
    {
        uint32_t i;

        for (i = 0; i < count; i++)
        {
            if (strcmp(Player_ItemPath(&sv.player, i), sv.pending_path) == 0)
            {
                return simple_video_start_index(i);
            }
        }
        return FM_ERR_NOT_FOUND;
    }
    if (cmd == SV_CMD_SEEK)
    {
        double want = sv.pending_seek;
        double dur = simple_video_duration();

        if (want < 0.0)
        {
            want = 0.0;
        }
        if (dur > 0.0 && want >= dur)
        {
            /* Seeking past the end = the item is over: same policy as a
             * natural EOF (autoplay on → next item, off → stop). */
            return sv_playlist_advance();
        }
        r = Player_Seek(&sv.player, want);
        if (r != FM_OK)
        {
            return r;
        }
#if FM_USE_H264
        if (sv.player.video.info.codec == FM_VIDEO_CODEC_H264)
        {
            /* A jump inside an H.264 stream leaves the decoder pointing
             * at pictures that were never decoded.  Reset it so the
             * next IDR re-primes cleanly (frames until then freeze). */
            (void)Video_ResetCodec(&sv.player.video);
            sv.h264_fail_run = 0u;
            sv.h264_diag_logged = 0u;
        }
#endif
        sv.elapsed_ms = (uint32_t)(want * 1000.0);
        sv.t0_ms = HAL_GetTick() - sv.elapsed_ms;
        sv.paused = FM_FALSE;
        r = simple_video_decode_one();
        if (r == FM_OK)
        {
            (void)simple_video_sync_audio();
#if SIMPLE_VIDEO_I2S
            (void)sv_i2s_board_stop();
            /* Audio cursor moved: drop the stale partial chunk from the
             * old position so the seek restart is sample-clean. */
            sv_pcm_len = 0u;
            sv_pcm_off = 0u;
            r = sv_i2s_start();
            if (r != FM_OK)
            {
                FM_Log("SimpleVideo: I2S seek restart %s (%d)\n",
                       File_GetErrorString(r), (int)r);
                r = FM_OK;
            }
#endif
            simple_video_present();
            FM_Log("SimpleVideo: seek -> %.2f s\n", want);
        }
        return r;
    }
    if (cmd == SV_CMD_RESUME)
    {
        sv.t0_ms = HAL_GetTick() - sv.elapsed_ms;
        sv.paused = FM_FALSE;
        sv.pause_need_restart = 0u;
#if SIMPLE_VIDEO_I2S
        (void)sv_i2s_board_stop();
        r = sv_i2s_start();
        if (r != FM_OK)
        {
            FM_Log("SimpleVideo: I2S resume %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            r = FM_OK;
        }
#endif
        return FM_OK;
    }
    if (cmd == SV_CMD_BLIT)
    {
        simple_video_present();
        return FM_OK;
    }
    if (cmd == SV_CMD_SAVE)
    {
        FM_Image img;
        size_t rgb_need;
        char path[FM_PATH_MAX];
        FM_Result r;

        if (!sv.have_frame)
        {
            return FM_ERR_NOT_READY;
        }
        if (sv.player.video.info.width == 0u ||
            sv.player.video.info.height == 0u)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        rgb_need = (size_t)sv.player.video.info.width *
                   sv.player.video.info.height * 3u;
        if (rgb_need > sizeof(sv_rgb))
        {
            FM_Log("SimpleVideo: snapshot %ux%u exceeds decode buffer "
                   "(SIMPLE_VIDEO_MAX_W/H)\n",
                   (unsigned)sv.player.video.info.width,
                   (unsigned)sv.player.video.info.height);
            return FM_ERR_TOO_LARGE;
        }
        if (sv.pending_path[0] == '\0')
        {
            /* Auto path: <SIMPLE_VIDEO_SAVE_FOLDER>/video_NNN.bmp —
             * bump the counter until a free name is found. */
            (void)Directory_Create(SIMPLE_VIDEO_SAVE_FOLDER);
            for (;;)
            {
                FM_Bool exists = FM_FALSE;

                (void)snprintf(path, sizeof(path), "%s/video_%03lu.bmp",
                               SIMPLE_VIDEO_SAVE_FOLDER,
                               (unsigned long)(sv_save_seq % 1000u));
                if (File_Exists(path, &exists) != FM_OK || !exists)
                {
                    break;
                }
                sv_save_seq++;
            }
        }
        else
        {
            (void)strncpy(path, sv.pending_path, sizeof(path) - 1u);
            path[sizeof(path) - 1u] = '\0';
        }
        memset(&img, 0, sizeof(img));
        img.width = sv.player.video.info.width;
        img.height = sv.player.video.info.height;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3;
        img.bit_depth = 24;
        img.data = sv_rgb;
        img.data_capacity = sizeof(sv_rgb);
        img.data_size = rgb_need;
        r = Image_Save(path, &img);
        if (r == FM_OK)
        {
            sv_save_seq++;
            FM_Log("SimpleVideo: snapshot -> %s\n", path);
        }
        else
        {
            FM_Log("SimpleVideo: snapshot %s failed: %s (%d)\n", path,
                   File_GetErrorString(r), (int)r);
        }
        return r;
    }
    if (cmd == SV_CMD_REC_START)
    {
        uint64_t bytes;
        FM_Result rr;

        if (sv.rec_active)
        {
            return FM_ERR_BUSY;
        }
        bytes = (uint64_t)sv.pending_w * sv.pending_h * 3u;
        if (bytes > UINT32_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        rr = VideoWriter_Create(sv.pending_path, sv.pending_w, sv.pending_h,
                                sv.pending_fps, sv.pending_frames,
                                (uint32_t)bytes, &sv.rec);
        if (rr == FM_OK)
        {
            sv.rec_active = FM_TRUE;
            FM_Log("SimpleVideo: recording FMRV -> %s\n", sv.pending_path);
        }
        return rr;
    }
    if (cmd == SV_CMD_REC_STOP)
    {
        FM_Result rr;

        if (!sv.rec_active)
        {
            return FM_ERR_NOT_READY;
        }
        rr = VideoWriter_Close(&sv.rec);
        sv.rec_active = FM_FALSE;
        return rr;
    }
    return FM_OK;
}

FM_Result SimpleVideo_Begin(void)
{
    FM_Result r;
    SimpleVideo_PresentFn present = sv.present;
    SimpleVideo_TrackStartFn track_start = sv.track_start;
    void *track_start_ctx = sv.track_start_ctx;
    SimpleVideo_TrackEndFn track_end = sv.track_end;
    void *track_end_ctx = sv.track_end_ctx;
    SimpleVideo_FrameFn frame_cb = sv.frame_cb;
    void *frame_cb_ctx = sv.frame_cb_ctx;
    uint16_t out_w = sv.out_w;
    uint16_t out_h = sv.out_h;
    uint8_t out_user = sv.out_size_user;

    memset(&sv, 0, sizeof(sv));
    /* Preserve app-installed hooks across the reset (like present). */
    sv.present = present;
    sv.track_start = track_start;
    sv.track_start_ctx = track_start_ctx;
    sv.track_end = track_end;
    sv.track_end_ctx = track_end_ctx;
    sv.frame_cb = frame_cb;
    sv.frame_cb_ctx = frame_cb_ctx;
    sv.out_w = out_w;
    sv.out_h = out_h;
    sv.out_size_user = out_user;
    sv.format = sv_active_format();
    sv_slot = 0u;
    sv_show = 0u;
    r = Player_Init(&sv.player);
    if (r != FM_OK)
    {
        return r;
    }
    sv.player_ok = FM_TRUE;
    r = Player_ScanFolder(&sv.player, SIMPLE_VIDEO_FOLDER,
                          FM_PLAYER_KIND_VIDEO);
    if (r != FM_OK)
    {
        FM_Log("SimpleVideo: scan %s failed: %s (%d)\n", SIMPLE_VIDEO_FOLDER,
               File_GetErrorString(r), (int)r);
        return r;
    }
    FM_Log("SimpleVideo: playlist %lu in %s\n",
           (unsigned long)Player_Count(&sv.player), SIMPLE_VIDEO_FOLDER);
    if (Player_Count(&sv.player) == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
#if SIMPLE_VIDEO_I2S
    SimpleIdle_Add(simple_video_idle);
#endif
    if (sv_autoplay_active())
    {
        return simple_video_start_index(0);
    }
    FM_Log("SimpleVideo: playlist ready (%lu items, autoplay off) — "
           "SimpleVideo_Play(path) / Start() / Next() to begin\n",
           (unsigned long)Player_Count(&sv.player));
    return FM_OK;
}

FM_Result SimpleVideo_Poll(void)
{
    uint32_t fps;
    uint32_t want;
    uint32_t have;
    FM_Result r;

    if (sv.pending_cmd != SV_CMD_NONE)
    {
        return simple_video_apply_pending();
    }
    if (!sv.running)
    {
        return FM_ERR_NOT_READY;
    }
    if (sv.paused)
    {
#if SIMPLE_VIDEO_I2S
        uint8_t half;

        for (half = 0u; half < 2u; half++)
        {
            if (sv_i2s_refill[half] != 0u)
            {
                sv_i2s_refill[half] = 0u;
                sv_i2s_pause_fill(half);
            }
        }
#endif
        return FM_OK;
    }
#if SIMPLE_VIDEO_I2S
    r = sv_i2s_poll();
    if (r != FM_OK)
    {
        FM_Log("SimpleVideo: I2S refill %s (%d)\n", File_GetErrorString(r),
               (int)r);
    }
#endif

    fps = simple_video_fps();
    want = (simple_video_now_elapsed() * fps) / 1000u;
    /* current_frame is the NEXT frame to decode (already-shown + 1). */
    have = sv.player.video.current_frame;
    if (have > want)
    {
        return FM_OK;
    }
    /* Decode-limited pacing for silent files.  When the decoder cannot
     * sustain the container fps, chasing the wall clock burns CPU on
     * frames nobody ever sees (and EOF-consumes the file in a burst).
     * With no audio to stay in sync with, re-anchor the clock to the
     * decode position instead: every decoded frame is presented at the
     * sustainable rate — smooth slow motion, no storm. */
    if (!sv.has_audio && want > have + 3u)
    {
        sv.t0_ms = HAL_GetTick() -
                   (uint32_t)(((uint64_t)have * 1000u) / fps);
        want = have;
    }
    /* If decode is slower than real time we fall behind the clock.
     * Decoding every stale frame makes it worse (death spiral: each
     * pass is further behind than the last).  MJPEG and RAW frames
     * are independent, so jump the file position to the frame we
     * actually want and decode only that one.  H.264 needs every
     * frame (P-frames reference the previous picture), so there we
     * still decode forward but present only the last frame. */
    if (want > have + 1u &&
        sv.player.video.info.codec != FM_VIDEO_CODEC_H264 &&
        want < sv.player.video.info.frame_count)
    {
        if (Video_Seek(&sv.player.video, want) == FM_OK)
        {
            have = want;
        }
    }
    {
        uint32_t burst = 0u;

        while (sv.player.video.current_frame <= want)
        {
            uint32_t before = sv.player.video.current_frame;
            uint32_t cost_t0 = HAL_GetTick();

            r = simple_video_decode_one();
            if (r == FM_ERR_EOF)
            {
                sv.fail_count = 0u;
                return sv_playlist_advance();
            }
            if (r == FM_ERR_NOT_READY)
            {
                /* The packet held no picture (parameter sets / SEI
                 * only).  It was consumed and nothing is wrong: keep
                 * showing the previous frame and go on. */
                continue;
            }
            if (r != FM_OK)
            {
                uint32_t count = Player_Count(&sv.player);

#if FM_USE_H264
                if (sv.player.video.info.codec == FM_VIDEO_CODEC_H264 &&
                    sv.h264_fail_run <
                        (uint32_t)SIMPLE_VIDEO_H264_FAIL_TOLERANCE)
                {
                    /* Tolerate broken H.264 frames instead of skipping the
                     * file: one damaged frame breaks the reference chain
                     * until the next IDR (<= 1 s with typical GOPs).  Reset
                     * the decoder and step over frames; the self-healing
                     * plugin replays the keyframe, and the picture freezes
                     * briefly rather than the file dying. */
                    const FM_H264Diag *d = H264_GetLastError();

                    sv.h264_fail_run++;
                    if (sv.h264_diag_logged == 0u)
                    {
                        sv.h264_diag_logged = 1u;
                        FM_Log("SimpleVideo: h264 frame %lu failed: %s (%d) "
                               "res=%lu skip=%lu resets=%lu "
                               "head=%02X %02X %02X %02X %02X %02X %02X %02X "
                               "fail=%02X %02X %02X %02X %02X %02X %02X %02X "
                               "— resyncing\n",
                               (unsigned long)before,
                               File_GetErrorString(r), (int)r,
                               (unsigned long)d->bsd_result,
                               (unsigned long)d->skipped,
                               (unsigned long)d->resets,
                               d->first_bytes[0], d->first_bytes[1],
                               d->first_bytes[2], d->first_bytes[3],
                               d->first_bytes[4], d->first_bytes[5],
                               d->first_bytes[6], d->first_bytes[7],
                               d->fail_bytes[0], d->fail_bytes[1],
                               d->fail_bytes[2], d->fail_bytes[3],
                               d->fail_bytes[4], d->fail_bytes[5],
                               d->fail_bytes[6], d->fail_bytes[7]);
                    }
                    /* Leave the plugin state alone here: its self-heal keeps
                     * the cached keyframe and replays it after resets, which
                     * is exactly what re-primes a broken reference chain.
                     * Stepping over frames costs a brief freeze; the picture
                     * resumes at the next IDR instead of the file dying. */
                    if (sv.player.video.current_frame + 1u <
                        sv.player.video.info.frame_count)
                    {
                        (void)Video_Seek(&sv.player.video,
                                         sv.player.video.current_frame + 1u);
                    }
                    else
                    {
                        sv.fail_count = 0u;
                        return sv_playlist_advance();
                    }
                    return FM_OK;
                }
                if (sv.player.video.info.codec == FM_VIDEO_CODEC_H264)
                {
                    FM_Log("SimpleVideo: h264 gave up after %u bad frames\n",
                           (unsigned)sv.h264_fail_run);
                }
#endif
                FM_Log("SimpleVideo: decode %s (%d)\n", File_GetErrorString(r),
                       (int)r);
                /* Unplayable file: skip to the next one instead of retrying
                 * the same frame forever.  If every playlist entry fails,
                 * stop cleanly so the log is not spammed. */
                sv.fail_count++;
                if (count <= 1u || sv.fail_count >= count)
                {
                    FM_Log("SimpleVideo: no playable video, stopping\n");
                    (void)SimpleVideo_Stop();
                    return r;
                }
                /* Same boundary policy as an EOF (autoplay decides). */
                return sv_playlist_advance();
            }
            sv.fail_count = 0u;
            sv.h264_fail_run = 0u;
            sv.h264_diag_logged = 0u;
            /* Decode-cost probe: measure the full per-frame cost and skip
             * items that can never play in real time on this MCU. */
            {
                uint32_t cost = HAL_GetTick() - cost_t0;

                sv.frame_cost_ema = (sv.frame_cost_ema == 0u)
                                        ? cost
                                        : ((sv.frame_cost_ema * 3u + cost) /
                                           4u);
                if (sv.slow_probe < (uint32_t)SIMPLE_VIDEO_SLOW_PROBE)
                {
                    sv.slow_probe++;
                }
                else
                {
                    /* Item survived the probe: it is watchable. */
                    sv.slow_skip_count = 0u;
                }
#if SIMPLE_VIDEO_SKIP_SLOW
                if (sv.slow_probe == (uint32_t)SIMPLE_VIDEO_SLOW_PROBE &&
                    sv.frame_cost_ema >
                        (1000u / fps) * (uint32_t)SIMPLE_VIDEO_SLOW_FACTOR)
                {
                    uint32_t count = Player_Count(&sv.player);
                    uint32_t drop_fps = (sv.frame_cost_ema != 0u)
                                            ? (1000u / sv.frame_cost_ema)
                                            : 0u;
                    /* Frame-droppable codecs stay playable below real
                     * time: keep the item as long as the achievable
                     * rate clears the floor.  Only H.264 (which cannot
                     * drop frames, every P-frame references the last
                     * picture) is still skipped outright. */
                    uint8_t droppable =
                        (uint8_t)(SIMPLE_VIDEO_SLOW_MIN_FPS > 0u &&
                                  sv.player.video.info.codec !=
                                      FM_VIDEO_CODEC_H264 &&
                                  drop_fps >=
                                      (uint32_t)SIMPLE_VIDEO_SLOW_MIN_FPS);

                    if (droppable)
                    {
                        FM_Log("SimpleVideo: %s decodes ~%lums/frame — "
                               "slower than %lufps, playing at ~%lufps "
                               "with dropped frames (audio stays in "
                               "sync)\n",
                               Player_ItemPath(&sv.player,
                                               Player_Current(&sv.player)),
                               (unsigned long)sv.frame_cost_ema,
                               (unsigned long)fps,
                               (unsigned long)drop_fps);
                        /* Mark the item as judged so the probe never
                         * re-runs this check for it. */
                        sv.slow_probe++;
                        sv.slow_skip_count = 0u;
                    }
                    else
                    {
                        FM_Log("SimpleVideo: %s decodes ~%lums/frame vs "
                               "%lums budget @ %lufps — too slow, skipping "
                               "(SIMPLE_VIDEO_SKIP_SLOW=1; MJPEG q6 320x240 "
                               "plays real-time)\n",
                               Player_ItemPath(&sv.player,
                                               Player_Current(&sv.player)),
                               (unsigned long)sv.frame_cost_ema,
                               (unsigned long)((1000u / fps) *
                                   (uint32_t)SIMPLE_VIDEO_SLOW_FACTOR),
                               (unsigned long)fps);
                        sv.slow_skip_count++;
                        if (count <= 1u || sv.slow_skip_count >= count)
                        {
                            FM_Log("SimpleVideo: every item too slow, "
                                   "stopping\n");
                            (void)SimpleVideo_Stop();
                            return FM_ERR_UNSUPPORTED;
                        }
                        /* sv_playlist_advance, not SV_CMD_NEXT: with
                         * autoplay off the item boundary must STOP (this
                         * path used to wrap the folder even with autoplay
                         * disabled). */
                        return sv_playlist_advance();
                    }
                }
#endif
            }
            if (sv.player.video.current_frame == before)
            {
                break;
            }
            /* Drop frames if we are more than one behind the clock, but
             * bound the catch-up burst so one Poll pass can never grind
             * through a whole file (that EOF-consumed slow items). */
            if (sv.player.video.current_frame <= want)
            {
                if (burst < (uint32_t)SIMPLE_VIDEO_MAX_BURST)
                {
                    burst++;
                    continue;
                }
            }
#if !SIMPLE_VIDEO_I2S
            (void)simple_video_sync_audio();
#endif
            simple_video_present();
            break;
        }
    }
    return FM_OK;
}

FM_Result SimpleVideo_Stop(void)
{
    FM_EVENT_LOG("stop: begin\n");
    sv.pending_cmd = SV_CMD_NONE;
    /* If an item was playing, report its end (idempotent: fires once). */
    sv_fire_item_end();
    sv.running = FM_FALSE;
    sv.paused = FM_FALSE;
    /* Drain a possibly still-running present DMA so the SPI bus is idle
     * before the file handles (or the next app screen) go away. */
    if (sv_present_wait != NULL)
    {
        FM_EVENT_LOG("stop: waiting for present DMA\n");
        sv_present_wait();
        FM_EVENT_LOG("stop: present DMA done\n");
    }
#if SIMPLE_VIDEO_I2S
    FM_EVENT_LOG("stop: calling i2s_board_stop\n");
    (void)sv_i2s_board_stop();
    SimpleIdle_Remove(simple_video_idle);
    FM_EVENT_LOG("stop: i2s_board_stop done\n");
#endif
    if (sv.player_ok)
    {
        FM_EVENT_LOG("stop: calling Player_Deinit\n");
        Player_Deinit(&sv.player);
        sv.player_ok = FM_FALSE;
        FM_EVENT_LOG("stop: Player_Deinit done\n");
    }
    /* Clear frame state to prevent further processing */
    sv.have_frame = FM_FALSE;
    sv.shown_frame = 0;
    sv_slot = 0;
    FM_EVENT_LOG("stop: complete\n");
    FM_Log("SimpleVideo: stopped\n");
    return FM_OK;
}

FM_Result SimpleVideo_Next(void)
{
    /* Logged so a device log shows unambiguously who drives the
     * playlist: these lines only appear when the APP calls the API —
     * internal playback never does. */
    FM_Log("SimpleVideo: Next() requested by app\n");
    sv.pending_cmd = SV_CMD_NEXT;
    return FM_OK;
}

/* Start (or resume playback of) the current playlist item.  Works when
 * autoplay is off and Begin() only scanned the folder. */
FM_Result SimpleVideo_Start(void)
{
    if (sv.running)
    {
        return FM_OK;
    }
    FM_Log("SimpleVideo: Start() requested by app\n");
    sv.pending_cmd = SV_CMD_START;
    return FM_OK;
}

/* Runtime auto-play control: 1 = Begin() starts item 0 and EOF loops
 * the folder forever; 0 (default) = play only what is asked for and
 * stop at the end of the folder.  Pass -1 to return to the
 * SIMPLE_VIDEO_AUTO_PLAY compile-time default.  Applies from the next
 * Begin/EOF. */
void SimpleVideo_SetAutoPlay(int8_t on)
{
    FM_Log("SimpleVideo: SetAutoPlay(%d) by app\n", (int)on);
    sv_autoplay = (on < 0) ? -1 : (on ? 1 : 0);
    if (!sv_play_mode_explicit) {
        sv_play_mode = sv_autoplay_active() ? FM_PLAY_MODE_REPEAT_ALL : FM_PLAY_MODE_STOP;
    }
}
/* Playback persistence for video — same as audio.
 * STOP       : ویدیو تمام شد → توقف روی آخرین فریم (پاز)
 * NEXT       : تمام شد → بعدی، آخر → توقف (پرزنت، بدون لوپ)
 * REPEAT_ONE : تمام شد → همین ویدیو دوباره (تکرار یک)
 * REPEAT_ALL : تمام شد → بعدی، آخر→اول (لوپ پوشه) — مناسب کیوسک
 * مثال پرزنت: SimpleVideo_SetPlayMode(FM_PLAY_MODE_NEXT);
 * مثال تکرار یک ویدیو: SimpleVideo_SetPlayMode(FM_PLAY_MODE_REPEAT_ONE);
 */
FM_Result SimpleVideo_SetPlayMode(FM_PlayMode mode)
{
    if ((int)mode < FM_PLAY_MODE_STOP || (int)mode > FM_PLAY_MODE_REPEAT_ALL) return FM_ERR_INVALID_ARGUMENT;
    sv_play_mode = mode;
    sv_play_mode_explicit = 1u;
    /* keep autoplay in sync for legacy checks */
    sv_autoplay = (mode == FM_PLAY_MODE_REPEAT_ALL) ? 1 : (mode == FM_PLAY_MODE_STOP ? 0 : sv_autoplay);
    FM_Log("SimpleVideo: SetPlayMode(%d) %s\n", (int)mode,
           mode==FM_PLAY_MODE_STOP?"STOP":mode==FM_PLAY_MODE_NEXT?"NEXT":mode==FM_PLAY_MODE_REPEAT_ONE?"REPEAT_ONE":"REPEAT_ALL");
    return FM_OK;
}
FM_PlayMode SimpleVideo_GetPlayMode(void)
{
    return sv_effective_play_mode();
}
void SimpleVideo_SetRepeatMode(int mode) { (void)SimpleVideo_SetPlayMode((FM_PlayMode)mode); } /* compat */
FM_PlayMode SimpleVideo_GetRepeatMode(void) { return SimpleVideo_GetPlayMode(); }

/* Software I2S volume 0..100.  Takes effect on the next ring half
 * (~tens of ms).  100 = unchanged PCM.  Same idea as SimpleAudio_SetVolume. */
FM_Result SimpleVideo_SetVolume(uint8_t percent)
{
    /* تا ۲۰۰٪ مجاز: صدای ترک‌های برخی ویدیوها آرام‌تر از موزیک است؛
     * ۱۰۰ = بی‌تغییر، ۱۰۱..۲۰۰ = تقویت با اشباع (تا +۶dB). */
    if (percent > 200u)
    {
        percent = 200u;
    }
    sv_volume_pct = percent;
    return FM_OK;
}

uint8_t SimpleVideo_GetVolume(void)
{
    return sv_volume_pct;
}

/* Global video-audio enable. 1 = video audio is demuxed/decoded and played
 * via I2S (when the clip has an audio track). 0 = I2S for video stays silent
 * and Video_ReadAudioFrame is never called in the refill path — saves
 * SD bandwidth and CPU when you only need pictures. SimpleAudio (music)
 * is not affected. Takes effect on next clip or next half-refill. */
void SimpleVideo_EnableAudio(uint8_t enable)
{
    sv_audio_enabled = (enable != 0u) ? 1u : 0u;
}
uint8_t SimpleVideo_IsAudioEnabled(void)
{
    return sv_audio_enabled;
}
void SimpleVideo_SetAudioEnabled(uint8_t enable) { SimpleVideo_EnableAudio(enable); }

/* Pre/post PCM taps for video audio. Pre is raw decoded PCM before volume/fade,
 * Post is after volume/fade — both editable before DMA. NULL disables.
 * Signature matches SimpleAudio: (ctx, pcm, samples, rate, channels).
 * samples is interleaved int16 count. Keep it very short. */
void SimpleVideo_SetPrePcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{
    sv_pcm_tap_pre = fn;
    sv_pcm_tap_pre_ctx = ctx;
}
void SimpleVideo_SetPostPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{
    sv_pcm_tap_post = fn;
    sv_pcm_tap_post_ctx = ctx;
}
void SimpleVideo_SetPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{
    SimpleVideo_SetPostPcmTap(fn, ctx);
}
void SimpleVideo_SetAudioPcmTap(SimpleVideo_PcmTapFn fn, void *ctx)
{
    SimpleVideo_SetPostPcmTap(fn, ctx);
}

/* Direct access to decoded video frame (RGB888) and to decoded video
 * audio PCM. Both are safe to call from the main loop when
 * SimpleVideo_IsRunning(). For DSP/image pipelines. */
const void *SimpleVideo_GetFrame(uint16_t *w, uint16_t *h)
{
    if (w != NULL) *w = sv.player.video.info.width;
    if (h != NULL) *h = sv.player.video.info.height;
    return sv_rgb;
}
FM_Result SimpleVideo_ReadAudio(int16_t *pcm, size_t frames, size_t *got_frames)
{
    uint8_t *tmp = (uint8_t *)pcm;
    size_t cap = frames * 2u * sizeof(int16_t);
    size_t got = 0;
    FM_Result r;
    if (pcm == NULL || got_frames == NULL) return FM_ERR_INVALID_ARGUMENT;
    *got_frames = 0;
    if (!sv.running || !sv.has_audio) return FM_ERR_NOT_READY;
    if (!sv_audio_enabled) return FM_ERR_NOT_READY;
    r = Video_ReadAudioFrame(&sv.player.video, tmp, cap, &got);
    if (r != FM_OK) return r;
    *got_frames = got / (sv.audio.channels * sizeof(int16_t));
    return FM_OK;
}
FM_Result SimpleVideo_GetAudioInfo(FM_AudioTrackInfo *info)
{
    if (info == NULL) return FM_ERR_INVALID_ARGUMENT;
    if (!sv.has_audio) return FM_ERR_NOT_READY;
    *info = sv.audio;
    return FM_OK;
}

FM_Result SimpleVideo_Prev(void)
{
    FM_Log("SimpleVideo: Prev() requested by app\n");
    sv.pending_cmd = SV_CMD_PREV;
    return FM_OK;
}

FM_Result SimpleVideo_Play(const char *path)
{
    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Log("SimpleVideo: Play(\"%s\") requested by app\n", path);

#if SIMPLE_VIDEO_USE_MEDIACACHE
    /* اگر کش رسانه راه‌اندازی شده باشد، فایل را (یک‌بار) روی فلش سریع
     * می‌برد و از آنجا پخش می‌کند. اگر کش نباشد یا فایل مناسب نباشد،
     * همان مسیر اصلی برمی‌گردد — پس این خط هیچ‌وقت پخش را خراب
     * نمی‌کند، فقط گاهی سریع‌ترش می‌کند. */
    {
        static char cached[FM_PATH_MAX];

        if (FM_MediaCache_Resolve(path, cached, sizeof(cached)) == FM_OK)
        {
            path = cached;
        }
    }
#endif

    (void)strncpy(sv.pending_path, path, FM_PATH_MAX - 1u);
    sv.pending_path[FM_PATH_MAX - 1u] = '\0';
    sv.pending_cmd = SV_CMD_PLAY;
    return FM_OK;
}

FM_Result SimpleVideo_Pause(void)
{
    if (!sv.running)
    {
        return FM_ERR_NOT_READY;
    }
    if (!sv.paused)
    {
        sv.elapsed_ms = simple_video_now_elapsed();
        sv.paused = FM_TRUE;
        sv.pause_need_restart = 0u;
    }
    return FM_OK;
}

FM_Result SimpleVideo_Resume(void)
{
    if (!sv.running)
    {
        return FM_ERR_NOT_READY;
    }
    if (!sv.paused)
    {
        return FM_OK;
    }
    sv.t0_ms = HAL_GetTick() - sv.elapsed_ms;
    sv.paused = FM_FALSE;
    sv.pause_need_restart = 0u;
    return FM_OK;
}

FM_Result SimpleVideo_TogglePause(void)
{
    return sv.paused ? SimpleVideo_Resume() : SimpleVideo_Pause();
}

FM_Result SimpleVideo_SeekTo(double seconds)
{
    if (seconds < 0.0)
    {
        seconds = 0.0;
    }
    sv.pending_seek = seconds;
    sv.pending_cmd = SV_CMD_SEEK;
    return FM_OK;
}

FM_Result SimpleVideo_SeekBy(double seconds)
{
    double pos = (double)simple_video_now_elapsed() / 1000.0;

    return SimpleVideo_SeekTo(pos + seconds);
}

void SimpleVideo_SetPresent(SimpleVideo_PresentFn fn)
{
    sv.present = fn;
}

/* Track lifecycle callbacks.  SetTrackStartCallback also reports the item
 * already playing (if any) so a hook installed after SimpleVideo_Begin()
 * (which reset the player) still learns the current video. */
void SimpleVideo_SetTrackStartCallback(SimpleVideo_TrackStartFn fn, void *ctx)
{
    sv.track_start = fn;
    sv.track_start_ctx = ctx;
    if (fn != NULL && sv.running && sv.player_ok && sv.item_started)
    {
        if (sv.track_start != NULL)
        {
            sv.track_start(sv.track_start_ctx, sv.cur_path,
                           sv.out_w, sv.out_h, simple_video_fps());
        }
    }
}

void SimpleVideo_SetTrackEndCallback(SimpleVideo_TrackEndFn fn, void *ctx)
{
    sv.track_end = fn;
    sv.track_end_ctx = ctx;
}

/* Frame data callback for DSP/image processing.  Fires per decoded frame,
 * even when no present callback is set (data-only mode). */
void SimpleVideo_SetFrameCallback(SimpleVideo_FrameFn fn, void *ctx)
{
    sv.frame_cb = fn;
    sv.frame_cb_ctx = ctx;
}

/* Install the drain hook called before each present (e.g.
 * LcdSpiDma_Wait).  NULL disables it (default). */
void SimpleVideo_SetPresentWait(void (*fn)(void))
{
    sv_present_wait = fn;
}

/*
 * اندازهٔ خروجی را دستی تعیین کن.
 *
 * (0, 0) یعنی «به اندازهٔ خود فایل» — یعنی هیچ مقیاس‌دهی‌ای انجام
 * نشود مگر اینکه فریم از بافر بزرگ‌تر باشد. این حالت پیش‌فرض است.
 */
FM_Result SimpleVideo_SetOutputSize(uint16_t width, uint16_t height)
{
    if (width > SIMPLE_VIDEO_MAX_W || height > SIMPLE_VIDEO_MAX_H)
    {
        return FM_ERR_TOO_LARGE;
    }
    sv.out_w = width;
    sv.out_h = height;
    sv.out_size_user = (width != 0u || height != 0u) ? 1u : 0u;
    return FM_OK;
}

/* Choose the pixel format handed to the present callback at runtime:
 * FM_PIXEL_RGB565 / BGR565 (2 B/px), RGB888 / BGR888 (3 B/px),
 * RGBA8888 / BGRA8888 (4 B/px) or GRAY8 (1 B/px).
 * Takes effect from the next played file (call before Begin, or call
 * Next/Play after changing it).  Pass 0 to return to
 * SIMPLE_VIDEO_FORMAT. */
FM_Result SimpleVideo_SetOutputFormat(FM_PixelFormat format)
{
    if ((int)format != 0 &&
        (format < FM_PIXEL_GRAY8 || format > FM_PIXEL_BGRA8888))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    sv_out_format = format;
    return FM_OK;
}

/* Override the compile-time SIMPLE_VIDEO_SWAP16 at runtime:
 * 1 = swap 565 bytes for SPI panels, 0 = no swap (LTDC / imports),
 * -1 = back to the compile-time default. */
void SimpleVideo_SetSwap16(int8_t swap)
{
    sv_swap16_override = (swap < 0) ? -1 : (swap ? 1 : 0);
}

FM_Result SimpleVideo_AudioRead(uint8_t *buf, size_t cap, size_t *got)
{
    if (!sv.running || !sv.has_audio)
    {
        return FM_ERR_NOT_READY;
    }
    return Video_ReadAudioFrame(&sv.player.video, buf, cap, got);
}

/* Save the most recently decoded frame.  path == NULL stores an
 * auto-numbered BMP in SIMPLE_VIDEO_SAVE_FOLDER (created on demand);
 * an explicit path keeps its extension-chosen format
 * (BMP/PNG/JPEG). */
FM_Result SimpleVideo_SaveFrame(const char *path)
{
    if (path == NULL)
    {
        sv.pending_path[0] = '\0';
    }
    else
    {
        (void)strncpy(sv.pending_path, path, FM_PATH_MAX - 1u);
        sv.pending_path[FM_PATH_MAX - 1u] = '\0';
    }
    sv.pending_cmd = SV_CMD_SAVE;
    return FM_OK;
}

FM_Result SimpleVideo_RecStart(const char *path, uint16_t width,
                               uint16_t height, uint32_t fps,
                               uint32_t expected_frames)
{
    if (path == NULL || width == 0u || height == 0u || fps == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)strncpy(sv.pending_path, path, FM_PATH_MAX - 1u);
    sv.pending_path[FM_PATH_MAX - 1u] = '\0';
    sv.pending_w = width;
    sv.pending_h = height;
    sv.pending_fps = fps;
    sv.pending_frames = expected_frames;
    sv.pending_cmd = SV_CMD_REC_START;
    return FM_OK;
}

FM_Result SimpleVideo_RecWrite(const void *rgb888, size_t bytes)
{
    if (!sv.rec_active)
    {
        return FM_ERR_NOT_READY;
    }
    return VideoWriter_WriteFrame(&sv.rec, rgb888, bytes);
}

FM_Result SimpleVideo_RecStop(void)
{
    sv.pending_cmd = SV_CMD_REC_STOP;
    return FM_OK;
}

const void *SimpleVideo_Frame(uint16_t *width, uint16_t *height,
                              FM_PixelFormat *format, size_t *bytes)
{
    size_t n = 0;

    if (!sv.have_frame)
    {
        if (width != NULL) *width = 0;
        if (height != NULL) *height = 0;
        if (bytes != NULL) *bytes = 0;
        return NULL;
    }
    (void)Image_BufferSize(sv.out_w, sv.out_h, sv.format, &n);
    if (width != NULL) *width = sv.out_w;
    if (height != NULL) *height = sv.out_h;
    if (format != NULL) *format = sv.format;
    if (bytes != NULL) *bytes = n;
    return sv_frame[sv_show];
}

void SimpleVideo_Info(SimpleVideoInfo *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->playlist_count = Player_Count(&sv.player);
    out->playlist_index = Player_Current(&sv.player);
    out->paused = sv.paused ? 1u : 0u;
    out->format = sv.format;
    out->volume_pct = sv_volume_pct;
    if (!sv.running)
    {
        return;
    }
    if (out->playlist_count > 0u)
    {
        (void)strncpy(out->name,
                      Player_ItemPath(&sv.player, out->playlist_index),
                      FM_PATH_MAX - 1u);
    }
    (void)strncpy(out->container, sv.player.video.info.container,
                  sizeof(out->container) - 1u);
    out->width = sv.out_w;
    out->height = sv.out_h;
    out->fps = simple_video_fps();
    out->frame_count = sv.player.video.info.frame_count;
    out->frame_index = sv.player.video.current_frame;
    out->position = (double)simple_video_now_elapsed() / 1000.0;
    out->duration = simple_video_duration();
    out->has_audio = sv.has_audio ? 1u : 0u;
    out->audio_rate = sv.audio.sample_rate;
    out->audio_channels = sv.audio.channels;
    out->pixels = sv.have_frame ? sv_frame[sv_show] : NULL;
    (void)Image_BufferSize(sv.out_w, sv.out_h, sv.format, &out->bytes);
}

#endif /* FM_USE_VIDEO */

#endif /* SIMPLE_VIDEO_H_ */
