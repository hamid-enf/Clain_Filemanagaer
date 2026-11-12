/*
 * board_taps_host_test.c — host validation for the PCM-tap block of
 * board/app_audio_hooks.h (audio_record_with_tap.h wiring: pre-tap DSP, post-tap telemetry,
 * automatic 5-second recorder, folder-copy and video-info commands).
 *
 * The tap/pump code is not copied here: the Makefile rule extracts the
 * section between "#define AGC_TARGET_PEAK" and the TAPS-BLOCK-END line
 * from board/app_audio_hooks.h into /tmp/fm_main_taps.h, so what runs is
 * exactly what ships to the board.  HAL/board functions are stubbed;
 * Directory_Copy is the REAL library function.
 *
 *   gcc ... -I/tmp tools/board_taps_host_test.c libfilemanager.a -lm
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "file_manager.h"
#include "fm_filesystem.h"
#include "fm_video.h"
#include "fm_directory.h"

extern FM_Result PosixFS_Init(FM_FS *fs);

/* ---- fake HAL/board environment ---------------------------------- */
static uint32_t fake_tick;
#define HAL_GetTick() fake_tick

typedef void (*TapFn)(void *, int16_t *, size_t, uint32_t, uint16_t);
typedef void (*SimpleVideo_PresentFn)(const void *pixels, uint16_t width,
                                      uint16_t height, FM_PixelFormat fmt,
                                      size_t bytes);

static TapFn stub_pre_tap, stub_post_tap;
static FM_Bool stub_running;
static uint32_t rec_start_rate;
static uint16_t rec_start_ch;
static int rec_start_calls, rec_stop_calls, rec_write_calls;
static size_t rec_written_frames;
static const char *rec_path_ret = "/rec/rec_000001.wav";
static FM_Result rec_start_ret = FM_OK;

static FM_AudioTrackInfo stub_track_info;
static FM_Bool stub_have_track;

/* the extracted block references these; provide host-side fakes */
static void SimpleAudio_SetPrePcmTap(TapFn fn, void *ctx)
{ (void)ctx; stub_pre_tap = fn; }
static void SimpleAudio_SetPcmTap(TapFn fn, void *ctx)
{ (void)ctx; stub_post_tap = fn; }
/* track lifecycle callbacks (this branch); record last values for assertions */
typedef void (*StubTrackFn)(void *ctx, const char *path,
                            const FM_AudioInfo *info);
static StubTrackFn stub_track_start_cb, stub_track_end_cb;
static void SimpleAudio_SetTrackStart(StubTrackFn fn, void *ctx)
{ (void)ctx; stub_track_start_cb = fn; }
static void SimpleAudio_SetTrackEnd(StubTrackFn fn, void *ctx)
{ (void)ctx; stub_track_end_cb = fn; }
static FM_Bool SimpleAudio_IsRunning(void) { return stub_running; }
static FM_Result SimpleAudio_RecStart(const char *dest, uint32_t rate,
                                      uint16_t ch)
{
    (void)dest;
    rec_start_calls++;
    rec_start_rate = rate;
    rec_start_ch = ch;
    return rec_start_ret;
}
static FM_Result SimpleAudio_RecWrite(const int16_t *pcm, size_t frames)
{
    (void)pcm;
    rec_write_calls++;
    rec_written_frames += frames;
    return FM_OK;
}
static FM_Result SimpleAudio_RecStop(void) { rec_stop_calls++; return FM_OK; }
static const char *SimpleAudio_RecPath(void) { return rec_path_ret; }
static FM_Result SimpleVideo_GetAudioInfo(FM_AudioTrackInfo *info)
{
    if (!stub_have_track) return FM_ERR_NOT_READY;
    *info = stub_track_info;
    return FM_OK;
}

/* video/image/SDRAM fakes for the new board commands (vvol2/istart/…) */
static uint8_t  stub_sv_volume;
static int      stub_sv_volume_calls;
static uint8_t  stub_sv_audio_en = 1u, stub_sv_audio_calls;
static SimpleVideo_PresentFn stub_sv_present = (SimpleVideo_PresentFn)1;
static int      stub_present_calls;
static int      stub_img_begin_calls;
static FM_Result stub_img_begin_ret = FM_OK;
static FM_Result SimpleVideo_SetVolume(uint8_t pct)
{ stub_sv_volume = pct; stub_sv_volume_calls++; return FM_OK; }
static void SimpleVideo_EnableAudio(uint8_t en)
{ stub_sv_audio_en = en; stub_sv_audio_calls++; }
static void SimpleVideo_SetPresent(SimpleVideo_PresentFn fn)
{ stub_sv_present = fn; stub_present_calls++; }
static void SimpleImage_SetOutputSize(int w, int h) { (void)w; (void)h; }
static FM_Result SimpleImage_Begin(void)
{ stub_img_begin_calls++; return stub_img_begin_ret; }
static size_t SdramAlloc_FreeBytes(void) { return (size_t)8u * 1024u * 1024u; }
/* the real one lives in board/app_display.h */
void App_LcdBlitVideo(const void *px, uint16_t w, uint16_t h,
                       FM_PixelFormat fmt, size_t bytes)
{ (void)px; (void)w; (void)h; (void)fmt; (void)bytes; }

/* ---- the REAL board code, extracted from board/app_audio_hooks.h ---- */
#include "fm_main_taps.h"

/* ---- tiny framework ----------------------------------------------- */
static int fails;
static void ok(const char *what, int cond)
{
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void)
{
    FM_FS fs;
    FM_Bool exists = FM_FALSE;

    printf("board/app_audio_hooks.h PCM-tap block (audio_record_with_tap.h) host test\n");
    printf("==================================================\n");

    /* 1) install: pre = قبل از ولوم، post = بعد از ولوم */
    Music_InstallTaps();
    ok("InstallTaps: pre-tap (قبل از ولوم) نصب شد", stub_pre_tap != NULL);
    ok("InstallTaps: post-tap (بعد از ولوم) نصب شد",
       stub_post_tap != NULL && stub_post_tap != stub_pre_tap);

    /* 2) pre-tap DSP = نرمالایزر بلندی (AGC) — tap1/tap0 */
    {
        int16_t q[2] = {1000, -1000};
        int i;

        /* 2a) خاموش (پیش‌فرض): دست‌نخورده */
        Music_TapBoostOff();
        q[0] = 1234;
        app_audio_pre_tap(NULL, q, 1, 48000, 2);
        ok("tap0: خاموش — نمونه دست‌نخورده", q[0] == 1234);

        /* 2b) تراک آرام (پیک ۱۰۰۰): پله‌های نرم +12.5٪ تا ×2
         * (هر چانک «تازه» است — مثل دکدر واقعی؛ اگر همان بافر را دوباره
         * بدهیم AGC خروجیِ خودش را دنبال می‌کند و تست بی‌معنا می‌شود) */
        Music_TapBoostOn();
        q[0] = 1000; q[1] = -1000;
        app_audio_pre_tap(NULL, q, 2, 48000, 2);          /* گام ۱: 1.125 */
        ok("tap1 گام اول: 1000 → 1125 (گین 288/256)",
           q[0] == 1125 && q[1] == -1125);
        for (i = 0; i < 8; i++)                            /* ادامهٔ پله‌ها */
        {
            q[0] = 1000; q[1] = -1000;                     /* چانک تازه */
            app_audio_pre_tap(NULL, q, 2, 48000, 2);
        }
        ok("tap1 بعد از ~۱۰ چانک: گین سقف ×2 (1000 → 2000)",
           q[0] == 2000 && q[1] == -2000);

        /* 2c) ورودی بلند ناگهانی (پیک ۳۰۰۰۰): بدون کلیپ، فوری پایین */
        q[0] = 30000; q[1] = -20000;
        app_audio_pre_tap(NULL, q, 2, 48000, 2);
        ok("tap1 تراک بلند: 30000/−20000 دست‌نخورده (بدون کلیپ)",
           q[0] == 30000 && q[1] == -20000);

        Music_TapBoostOff();
    }

    /* 2d) دستورهای جدید ویدیو/تصویر: vvol2 / vnopres / EnableAudio / istart */
    {
        Music_VideoVol200();
        ok("vvol2: SimpleVideo_SetVolume(200)",
           stub_sv_volume_calls == 1 && stub_sv_volume == 200u);

        Music_ToggleVideoPresent();
        ok("vnopres #1: نمایش خاموش (SetPresent(NULL))",
           stub_present_calls == 1 && stub_sv_present == NULL);
        Music_ToggleVideoPresent();
        ok("vnopres #2: نمایش روشن (App_LcdBlitVideo)",
           stub_present_calls == 2 && stub_sv_present == App_LcdBlitVideo);

        Music_ToggleVideoAudio();
        Music_ToggleVideoAudio();
        ok("EnableAudio ×2: خاموش/روشن",
           stub_sv_audio_calls == 2 && stub_sv_audio_en == 1u);

        Music_ImageStart();
        ok("istart: SimpleImage_Begin صدا زده شد",
           stub_img_begin_calls == 1);
    }

    /* 3) rectest بدون پخش → پیام خطا، بدون تغییر حالت */
    stub_running = FM_FALSE;
    rec_cfg_wait = 0u;
    Music_RecTest();
    ok("rectest بدون موزیک: رد شد", rec_cfg_wait == 0u);

    /* 4) جریان کامل ضبط: cfg از قلاب → RecStart → پمپ → RecStop */
    stub_running = FM_TRUE;
    rec_write_calls = 0;
    rec_written_frames = 0u;
    Music_RecTest();
    ok("rectest: منتظر cfg از قلاب", rec_cfg_wait == 1u);

    {
        int16_t chunk[6] = {1000, -1000, 2000, -2000, 3000, -3000};

        app_audio_pcm_tap(NULL, chunk, 6, 48000, 2);      /* تله‌متری + تحویل cfg */
        ok("قلاب: نرخ/کانال تحویل شد (48k/2)",
           rec_cfg_wait == 2u && rec_rate == 48000u && rec_ch == 2u);
        ok("قلاب: تله‌متری پیک", tap_calls == 1u && tap_peak == 3000);

        fake_tick = 1000u;
        Music_RecPump();                            /* RecStart + ددلاین */
        ok("پمپ: RecStart(48000, 2)",
           rec_start_calls == 1 && rec_start_rate == 48000u &&
           rec_start_ch == 2u && rec_active_demo == 1u);

        app_audio_pcm_tap(NULL, chunk, 6, 48000, 2);      /* کپی به رینگ */
        ok("قلاب: رینگ ضبط پر شد (۶ نمونه)", rec_ring_len == 6u);

        fake_tick = 2000u;
        Music_RecPump();                            /* تخلیهٔ رینگ */
        ok("پمپ: RecWrite(۳ فریم استریو)",
           rec_write_calls == 1 && rec_written_frames == 3u &&
           rec_ring_len == 0u);

        fake_tick = 6500u;                          /* از ددلاین گذشت */
        Music_RecPump();
        ok("پمپ: RecStop خودکار بعد از ۵ ثانیه",
           rec_stop_calls == 1 && rec_active_demo == 0u);
    }

    /* 5) پوشهٔ کاری تمیز + FileManager بوت (برای کپی/مونت واقعی) */
    (void)system("rm -rf /tmp/fm_taps_ws && mkdir -p /tmp/fm_taps_ws");
    (void)chdir("/tmp/fm_taps_ws");
    if (FileManager_Init() != FM_OK ||
        PosixFS_Init(&fs) != FM_OK ||
        FS_Mount("/", &fs) != FM_OK)
    {
        printf("FATAL: fs setup\n");
        return 1;
    }

    /* 5a) دیاگ pool فایل (خطای -14 برد): شمارش/بستن */
    {
        FM_File *a = File_Open("/a.txt", FM_OPEN_WRITE | FM_OPEN_CREATE);
        FM_File *b = File_Open("/b.txt", FM_OPEN_WRITE | FM_OPEN_CREATE);

        ok("pool: دو فایل باز → File_PoolUsed()==2",
           a != NULL && b != NULL && File_PoolUsed() == 2u);
        (void)File_Close(a);
        (void)File_Close(b);
        ok("pool: بعد از Close → File_PoolUsed()==0",
           File_PoolUsed() == 0u);
        File_PoolDump();          /* باید خالی چاپ کند — crash نمی‌کند */
    }

    /* 5b) Directory_Copy واقعی — اول بدون /music (خطا)، بعد با */
    (void)Directory_DeleteRecursive("/music");
    (void)Directory_DeleteRecursive("/backup_music");
    ok("Directory_Copy بدون /music: خطا و بدون کپی",
       Directory_Copy("/music", "/backup_music", NULL, NULL) != FM_OK);
    (void)File_Exists("/backup_music", &exists);
    ok("no copy happened when source missing", !exists);

    (void)Directory_Create("/music");
    (void)File_WriteText("/music/a.txt", "salam");
    ok("Directory_Copy with /music: success",
       Directory_Copy("/music", "/backup_music", NULL, NULL) == FM_OK);
    {
        char txt[32];
        FM_Result r = File_ReadText("/backup_music/a.txt", txt,
                                    sizeof(txt), NULL);
        ok("Directory_Copy واقعی شد",
           r == FM_OK && strcmp(txt, "salam") == 0);
    }
    (void)Directory_DeleteRecursive("/music");
    (void)Directory_DeleteRecursive("/backup_music");

    /* 6) vinfo: بدون ویدیو → پیام؛ با ترک → اعداد */
    Music_VideoInfo();
    stub_have_track = FM_TRUE;
    stub_track_info.sample_rate = 44100;
    stub_track_info.channels = 2;
    stub_track_info.bits_per_sample = 16;
    stub_track_info.sample_count = 123456;
    Music_VideoInfo();
    ok("vinfo: هر دو شاخه اجرا شد (پیام + اعداد)", 1);

    printf("\nResults: %s\n", fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
