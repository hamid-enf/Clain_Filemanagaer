/*
 * examples_sdcard_media.h — مثال‌های موسیقی/صدا و ویدئو روی SD
 *
 * پنج مثال ساده و واضح:
 *   exM_1_audio_wav    : ساخت یک فایل WAV (سینوس 440Hz) با AudioWriter،
 *                        خواندن اطلاعات، پخش (خواندن نمونه‌ها) و Seek
 *   exM_2_video_fmrv   : ساخت یک ویدئوی خام FMRV (کانتینر داخلی کتابخانه)
 *                        با VideoWriter، خواندن فریم‌ها و بررسی صحت
 *   exM_3_copy_song    : (سناریوی شما) آهنگ از SD → ذخیره با اسم جدید +
 *                        مقایسه‌ی CRC32 دو فایل (تست سالم بودن SD)
 *   exM_4_copy_video   : (سناریوی شما) ویدئو از SD → ذخیره با اسم جدید +
 *                        مقایسه‌ی CRC32
 *   exM_5_media_detect : تشخیص خودکار نوع رسانه از magic فایل‌ها
 *
 * پیش‌نیاز: SD_examples_begin() از examples_sdcard.h قبلاً صدا
 * شده باشد و SD_examples_end() فقط بعد از آخرین exM_* اجرا شود:
 *
 *   if (SD_examples_begin() == FM_OK) {
 *       exM_1_audio_wav();
 *       exM_2_video_fmrv();
 *       exM_3_copy_song();
 *       exM_4_copy_video();
 *       exM_5_media_detect();
 *       SD_examples_end();
 *   }
 *
 * همه‌ی printfها با %lu/%d/%s هستند (سازگار با mini-printf).
 */

#ifndef EXAMPLES_FILE_SDCARD_MEDIA_H_
#define EXAMPLES_FILE_SDCARD_MEDIA_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "file_manager.h"
#include "fm_audio.h"
#include "fm_video.h"
#include "fm_crc.h"
#include "fm_media.h"
#include "fm_log.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

/*----------------------------------------------------------------------*/
/* تنظیم‌ها — مسیر فایل‌های واقعی روی کارت خودتان                        */
/*----------------------------------------------------------------------*/

/* اگر مسیر مستقیم وجود نداشت، اولین فایلِ پوشه به‌طور خودکار انتخاب می‌شود */
#define EXM_SONG_PATH    "/music/song.mp3"
#define EXM_SONG_DIR     "/music"
#define EXM_VIDEO_PATH   "/clip_fast.avi"     /* روی کارت شما موجود است */
#define EXM_VIDEO_DIR    "/frames"

/*----------------------------------------------------------------------*/
/* ابزارهای کمکی                                                        */
/*----------------------------------------------------------------------*/

static void exm_step(const char *what, FM_Result r)
{
    if (r != FM_OK)
        printf("  !! %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
}

/* Media examples مالک mount نیستند. این guard جلوی ادامه‌دادن با handleها
 * و ساختارهای نامعتبر را می‌گیرد اگر caller زودتر SD_examples_end() زده باشد. */
static FM_Bool exm_require_mounted(const char *example_name)
{
    FM_Bool mounted = FM_FALSE;
    FM_Result r = FS_IsMounted("/", &mounted);

    if (r != FM_OK || !mounted)
    {
        printf("  !! %s requires mounted SD: call SD_examples_begin() before "
               "media and SD_examples_end() after it\n", example_name);
        return FM_FALSE;
    }
    return FM_TRUE;
}

/* CRC32 یک فایل به‌صورت تکه‌تکه (برای فایل‌های بزرگ) */
static FM_Result exm_crc32_file(const char *path, uint32_t *crc_out)
{
    FM_File *f;
    uint8_t  buf[512];
    uint32_t crc = 0;
    FM_Result r;

    f = File_Open(path, FM_OPEN_READ);
    if (f == NULL) return File_GetLastError();

    for (;;)
    {
        uint32_t got = 0;
        r = File_Read(f, buf, sizeof(buf), &got);
        if (r != FM_OK && r != FM_ERR_EOF) { File_Close(f); return r; }
        if (got == 0) break;
        crc = CRC_Compute32(buf, got, crc);
        if (r == FM_ERR_EOF) break;
    }
    (void)File_Close(f);
    *crc_out = crc;
    return FM_OK;
}

/* /music/song.mp3 -> /music/song_copy.mp3 */
static void exm_make_copy_name(const char *src, char *out, size_t out_size)
{
    const char *dot = strrchr(src, '.');
    size_t base;

    if (dot != NULL && dot > src)
        base = (size_t)(dot - src);
    else
        base = strlen(src);

    if (base >= out_size - 6) base = out_size - 6;
    memcpy(out, src, base);
    memcpy(out + base, "_copy", 5);
    out[base + 5] = '\0';
    if (dot != NULL && dot > src)
        strncat(out, dot, out_size - strlen(out) - 1);
}

/* اگر path موجود نبود، اولین فایلِ folder را پیدا کن */
static FM_Result exm_pick_file(const char *path, const char *folder,
                               char *out, size_t out_size)
{
    FM_Bool exists = FM_FALSE;

    if (File_Exists(path, &exists) == FM_OK && exists)
    {
        snprintf(out, out_size, "%s", path);
        return FM_OK;
    }

    {
        FM_Dir *d = Directory_Open(folder);
        FM_DirEntry e;
        while (d != NULL && Directory_Read(d, &e) == FM_OK)
        {
            if (e.type == FM_ITEM_FILE)
            {
                Path_Join(folder, e.name, out, out_size);
                Directory_Close(d);
                return FM_OK;
            }
        }
        if (d != NULL) Directory_Close(d);
    }
    return FM_ERR_NOT_FOUND;
}

/* پیشرفت کپی — فقط هر ۲۵٪ یک خط چاپ می‌کند تا خروجی شلوغ نشود */
typedef struct { uint32_t last_pct; } ExmProgress;
static void exm_copy_progress(uint64_t done, uint64_t total, void *ud)
{
    ExmProgress *p = (ExmProgress *)ud;
    uint32_t pct = (total != 0) ? (uint32_t)(done * 100u / total) : 100u;
    if (pct != p->last_pct && (pct % 25u) == 0u)
    {
        p->last_pct = pct;
        printf("  %lu%% (%lu / %lu bytes)\n",
               (unsigned long)pct, (unsigned long)done, (unsigned long)total);
    }
}

/* هسته‌ی مشترک سناریوهای «کپی با اسم جدید + تأیید» */
static FM_Result exm_copy_and_verify(const char *title, const char *src)
{
    char dst[FM_PATH_MAX];
    uint64_t s1 = 0, s2 = 0;
    uint32_t c1 = 0, c2 = 0;
    FM_MediaType type;
    FM_Result r;

    printf("\n--- %s ---\n", title);
    printf("source: %s\n", src);

    type = File_DetectType(src);
    printf("type:   %s\n", Media_TypeName(type));

    exm_make_copy_name(src, dst, sizeof(dst));
    printf("dest:   %s\n", dst);

    {
        ExmProgress p;

        p.last_pct = 0;
        r = File_Copy(src, dst, exm_copy_progress, &p);
    }
    if (r != FM_OK)
    {
        exm_step("File_Copy", r);
        return r;
    }

    r = File_SizeOf(src, &s1);
    if (r == FM_OK) r = File_SizeOf(dst, &s2);
    if (r != FM_OK)
    {
        exm_step("File_SizeOf", r);
        return r;
    }
    printf("size:   %lu -> %lu bytes\n",
           (unsigned long)s1, (unsigned long)s2);
    if (s1 != s2)
    {
        printf("  !! copied size mismatch\n");
        return FM_ERR_IO;
    }

    r = exm_crc32_file(src, &c1);
    if (r == FM_OK) r = exm_crc32_file(dst, &c2);
    if (r != FM_OK)
    {
        exm_step("CRC32 copied file", r);
        return r;
    }

    printf("crc32:  0x%08lX -> 0x%08lX  %s\n",
           (unsigned long)c1, (unsigned long)c2,
           (c1 == c2) ? "[IDENTICAL - copy is perfect]" : "[MISMATCH!]");
    return (c1 == c2) ? FM_OK : FM_ERR_CRC;
}

/*----------------------------------------------------------------------*/
/* exM_1 — صدا: ساخت WAV سینوسی + خواندن + Seek                          */
/*----------------------------------------------------------------------*/

void exM_1_audio_wav(void)
{
    FM_AudioWriter w;
    FM_Audio a;
    FM_AudioInfo info;
    FM_Result r;
    int16_t chunk[512];
    size_t i, written_frames = 0, total_samples = 0;

    printf("\n--- make /tone.wav (440 Hz, 0.5 s) ---\n");
    if (!exm_require_mounted("exM_1_audio_wav")) return;

    memset(&w, 0, sizeof(w));
    memset(&a, 0, sizeof(a));
    memset(&info, 0, sizeof(info));

    /* ۱. ساخت: 8000 Hz، مونو، 16-bit، 4000 نمونه = 0.5 ثانیه */
    r = AudioWriter_Create("/tone.wav", 8000, 1, 16, &w);
    if (r != FM_OK)
    {
        exm_step("AudioWriter_Create", r);
        return;
    }

    for (i = 0; i < 4000u / 512u + 1u; i++)
    {
        size_t n;
        size_t first = i * 512u;
        size_t written = 0;
        size_t k;

        if (first >= 4000u) break;
        n = (4000u - first < 512u) ? (4000u - first) : 512u;
        for (k = 0; k < n; k++)
        {
            /* سینوس 440Hz با فرکانس نمونه‌برداری 8000Hz */
            double t = (double)(first + k) / 8000.0;
            double phase = 2.0 * 3.14159265358979 * 440.0 * t;
            int v = (int)(6000.0 * sin(phase));
            chunk[k] = (int16_t)v;
        }

        r = AudioWriter_Write(&w, chunk, n, &written);
        if (r != FM_OK || written != n)
        {
            exm_step("AudioWriter_Write",
                     (r != FM_OK) ? r : FM_ERR_IO);
            (void)AudioWriter_Close(&w);
            return;
        }
        written_frames += written;
    }

    r = AudioWriter_Close(&w);
    if (r != FM_OK)
    {
        exm_step("AudioWriter_Close", r);
        return;
    }
    printf("wrote %lu samples (%.2f s)\n",
           (unsigned long)written_frames, (double)written_frames / 8000.0);

    /* ۲. خواندن اطلاعات */
    r = Audio_Open("/tone.wav", &a);
    if (r != FM_OK)
    {
        exm_step("Audio_Open", r);
        return;
    }

    r = Audio_GetInfo(&a, &info);
    if (r != FM_OK)
    {
        exm_step("Audio_GetInfo", r);
        (void)Audio_Close(&a);
        return;
    }
    printf("info: %lu Hz, %u ch, %u-bit, codec=%s, %lu bytes PCM\n",
           (unsigned long)info.sample_rate, (int)info.channels,
           (int)info.bits_per_sample, info.codec,
           (unsigned long)info.data_size);

    /* ۳. «پخش»: خواندن همه‌ی نمونه‌ها تکه‌تکه و شمارششان */
    for (;;)
    {
        size_t got = 0;

        r = Audio_Read(&a, chunk, 256, &got);
        if (r != FM_OK && r != FM_ERR_EOF)
        {
            exm_step("Audio_Read", r);
            (void)Audio_Close(&a);
            return;
        }
        total_samples += got;
        if (r == FM_ERR_EOF || got == 0) break;
    }
    printf("played back %lu samples\n", (unsigned long)total_samples);

    /* ۴. Seek به 0.25 ثانیه و خواندن یک فریم */
    r = Audio_Seek(&a, 0.25);
    if (r == FM_OK)
    {
        size_t got = 0;

        r = Audio_ReadFrame(&a, chunk, &got);
        if (r == FM_OK)
            printf("after seek 0.25s: %lu channel-sample(s)\n",
                   (unsigned long)got);
        else
            exm_step("Audio_ReadFrame", r);
    }
    else
    {
        exm_step("Audio_Seek", r);
    }

    r = Audio_Close(&a);
    if (r != FM_OK) exm_step("Audio_Close", r);
}

/*----------------------------------------------------------------------*/
/* exM_2 — ویدئو: ساخت FMRV + خواندن فریم‌ها                             */
/*----------------------------------------------------------------------*/

#define EXM_VW 16
#define EXM_VH 16
#define EXM_VFRAMES 8

static void exm_fill_frame(uint8_t *px, uint8_t frame_no)
{
    for (int y = 0; y < EXM_VH; y++)
        for (int x = 0; x < EXM_VW; x++)
        {
            int i = (y * EXM_VW + x) * 3;
            px[i + 0] = (uint8_t)(x * 16);
            px[i + 1] = (uint8_t)(y * 16);
            px[i + 2] = frame_no;
        }
}

void exM_2_video_fmrv(void)
{
    FM_VideoWriter vw;
    FM_Video v;
    FM_VideoInfo info;
    FM_Result r;
    uint8_t frame[EXM_VW * EXM_VH * 3];
    uint8_t readback[EXM_VW * EXM_VH * 3];
    uint32_t fr;

    printf("\n--- make /clip.fmrv (%ux%u, %u frames, 10 fps) ---\n",
           (unsigned long)EXM_VW, (unsigned long)EXM_VH,
           (unsigned long)EXM_VFRAMES);
    if (!exm_require_mounted("exM_2_video_fmrv")) return;

    memset(&vw, 0, sizeof(vw));
    memset(&v, 0, sizeof(v));
    memset(&info, 0, sizeof(info));

    /* ۱. ساخت ویدئوی خام (هر فریم = یک بافر RGB888 کامل) */
    r = VideoWriter_Create("/clip.fmrv", EXM_VW, EXM_VH, 10,
                           EXM_VFRAMES, sizeof(frame), &vw);
    if (r != FM_OK)
    {
        exm_step("VideoWriter_Create", r);
        return;
    }

    for (fr = 0; fr < EXM_VFRAMES; fr++)
    {
        exm_fill_frame(frame, (uint8_t)fr);
        r = VideoWriter_WriteFrame(&vw, frame, sizeof(frame));
        if (r != FM_OK)
        {
            exm_step("VideoWriter_WriteFrame", r);
            (void)VideoWriter_Close(&vw);
            return;
        }
    }

    r = VideoWriter_Close(&vw);
    if (r != FM_OK)
    {
        exm_step("VideoWriter_Close", r);
        return;
    }

    /* ۲. باز کردن و خواندن اطلاعات */
    r = Video_Open("/clip.fmrv", &v);
    if (r != FM_OK)
    {
        exm_step("Video_Open", r);
        return;
    }

    r = Video_GetInfo(&v, &info);
    if (r != FM_OK)
    {
        exm_step("Video_GetInfo", r);
        (void)Video_Close(&v);
        return;
    }
    printf("info: %lux%lu, %lu fps, %lu frames, %lu B/frame, codec=%ld, container=%s\n",
           (unsigned long)info.width, (unsigned long)info.height,
           (unsigned long)info.fps, (unsigned long)info.frame_count,
           (unsigned long)info.frame_size, (long)info.codec, info.container);

    /* ۳. خواندن فریم ۰ و مقایسه‌ی بایت‌به‌بایت */
    {
        size_t got = 0;

        r = Video_ReadFrame(&v, readback, sizeof(readback), &got);
        if (r != FM_OK)
        {
            exm_step("Video_ReadFrame", r);
            (void)Video_Close(&v);
            return;
        }
        exm_fill_frame(frame, 0);
        printf("frame 0: %lu bytes -> %s\n", (unsigned long)got,
               (got == sizeof(frame) && memcmp(frame, readback, got) == 0)
                   ? "content OK" : "CONTENT MISMATCH!");
    }

    /* ۴. خواندن بقیه‌ی فریم‌ها تا EOF */
    {
        uint32_t n = 1;
        size_t got2 = 0;

        while ((r = Video_ReadFrame(&v, readback, sizeof(readback), &got2))
               == FM_OK)
        {
            n++;
        }
        if (r != FM_ERR_EOF)
        {
            exm_step("Video_ReadFrame", r);
            (void)Video_Close(&v);
            return;
        }
        printf("read %lu frames until EOF\n", (unsigned long)n);
    }

    r = Video_Close(&v);
    if (r != FM_OK) exm_step("Video_Close", r);
}

/*----------------------------------------------------------------------*/
/* exM_3 — سناریوی ۱: آهنگ را از SD بخوان و با اسم دیگری ذخیره کن        */
/*----------------------------------------------------------------------*/

void exM_3_copy_song(void)
{
    char src[FM_PATH_MAX];

    printf("\n=== exM_3: copy a song from SD card ===\n");
    if (!exm_require_mounted("exM_3_copy_song")) return;

    if (exm_pick_file(EXM_SONG_PATH, EXM_SONG_DIR, src, sizeof(src)) != FM_OK)
    {
        printf("no song found (put one in %s or set EXM_SONG_PATH)\n",
               EXM_SONG_DIR);
        return;
    }
    if (exm_copy_and_verify("song copy", src) != FM_OK) return;

    /* برای فایل‌های WAV/MP3 می‌شود همین‌طور اطلاعات صوتی هم خواند */
    {
        FM_Audio a;
        memset(&a, 0, sizeof(a));
        FM_Result r = Audio_Open(src, &a);

        if (r == FM_OK)
        {
            FM_AudioInfo info;

            r = Audio_GetInfo(&a, &info);
            if (r == FM_OK)
                printf("audio info: %lu Hz, %u ch, %u-bit, codec=%s\n",
                       (unsigned long)info.sample_rate, (int)info.channels,
                       (int)info.bits_per_sample, info.codec);
            else
                exm_step("Audio_GetInfo", r);
            (void)Audio_Close(&a);
        }
        else
        {
            printf("audio info unavailable: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
        }
    }
}

/*----------------------------------------------------------------------*/
/* exM_4 — سناریوی ۲: ویدئو را از SD بخوان و با اسم دیگری ذخیره کن       */
/*----------------------------------------------------------------------*/

void exM_4_copy_video(void)
{
    char src[FM_PATH_MAX];

    printf("\n=== exM_4: copy a video from SD card ===\n");
    if (!exm_require_mounted("exM_4_copy_video")) return;

    if (exm_pick_file(EXM_VIDEO_PATH, EXM_VIDEO_DIR, src, sizeof(src)) != FM_OK)
    {
        printf("no video found (put one in %s or set EXM_VIDEO_PATH)\n",
               EXM_VIDEO_DIR);
        return;
    }
    if (exm_copy_and_verify("video copy", src) != FM_OK) return;

    /* FMRV و AVI/MP4 معتبر توسط Video_Open parse می‌شوند. بازشدن container
     * به معنی قابل decode بودن codec آن روی هر build/برد نیست. */
    {
        FM_Video v;
        memset(&v, 0, sizeof(v));
        FM_Result r = Video_Open(src, &v);

        if (r == FM_OK)
        {
            FM_VideoInfo info;

            r = Video_GetInfo(&v, &info);
            if (r == FM_OK)
                printf("video info: %lux%lu, %lu fps, %lu frames, container=%s\n",
                       (unsigned long)info.width, (unsigned long)info.height,
                       (unsigned long)info.fps,
                       (unsigned long)info.frame_count, info.container);
            else
                exm_step("Video_GetInfo", r);
            (void)Video_Close(&v);
        }
        else
        {
            printf("video info unavailable: %s (%d); copy is still valid\n",
                   File_GetErrorString(r), (int)r);
        }
    }
}

/*----------------------------------------------------------------------*/
/* exM_5 — تشخیص خودکار نوع رسانه                                         */
/*----------------------------------------------------------------------*/

void exM_5_media_detect(void)
{
    const char *files[] = { "/tone.wav", "/clip.fmrv",
                            EXM_SONG_PATH, EXM_VIDEO_PATH };
    uint32_t i;

    printf("\n--- media detection (magic bytes) ---\n");
    if (!exm_require_mounted("exM_5_media_detect")) return;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        FM_MediaType t = File_DetectType(files[i]);
        printf("  %-16s -> %s (%s)\n", files[i], Media_TypeName(t),
               Media_MimeType(t));
    }
}

#endif /* EXAMPLES_FILE_SDCARD_MEDIA_H_ */
