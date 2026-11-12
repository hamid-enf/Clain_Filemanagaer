/*
 * examples_save_media.h — مثال‌های «ذخیرهٔ صدا و ویدیو در فایل»
 * ============================================================
 *
 * دو سبک، هر کدام برای صدا و برای ویدیو:
 *
 *   سبک ۱ — ساده: داده را خودتان می‌سازید و در فایل می‌نویسید
 *     SaveDemo_AudioPlain()   بوق ۱ کیلوهرتز، ۲ ثانیه -> /save/tone.wav و tone.flac
 *     SaveDemo_VideoPlain()   ۳۰ فریم ساختگی + بوق     -> /save/bars.avi
 *
 *   سبک ۲ — از کالبک: همان چیزی که پخش‌کننده دیکد می‌کند دوباره ذخیره می‌شود
 *     موزیک  SaveDemo_MusicCallback        کالبک دادهٔ SimpleAudio
 *            SaveDemo_MusicRecStart/Pump/Stop            -> /save/music_cb.wav
 *     ویدیو  SaveDemo_VideoFrameCallback   کالبک فریم SimpleVideo
 *            SaveDemo_VideoSoundCallback   کالبک صدای SimpleVideo
 *            SaveDemo_VideoRecStart/Pump/Stop            -> /save/video_cb.avi
 *     و دو تابع «همه‌چیز در یک‌جا» که کل ترتیب را پشت‌سرهم نشان می‌دهند:
 *            SaveDemo_MusicFromCallback(dest, seconds)
 *            SaveDemo_VideoFromCallback(dest, max_frames)
 *
 * مسیر داده در سبک ۲ (هر پیکان یک تابع همین فایل است):
 *
 *   موزیک:
 *     دیکدر ──> SaveDemo_MusicCallback ──> رینگ (RAM) ──> SaveDemo_MusicRecPump
 *               (فقط کپی، بدون فایل)                      (حلقهٔ اصلی)
 *                                                          └─> SimpleAudio_RecWrite ──> فایل
 *   ویدیو:
 *     فریم ──> SaveDemo_VideoFrameCallback ──> SimpleVideo_RecWritePlayerFrame ──> فایل
 *     صدا  ──> SaveDemo_VideoSoundCallback ──> رینگ ──> SaveDemo_VideoRecPump
 *                                                        └─> SimpleVideo_RecWriteAudio ──> فایل
 *
 * چرا صدا از رینگ رد می‌شود ولی فریم مستقیم نوشته می‌شود؟
 *   - کالبک صدا وسط پُرکردن بافر I2S اجرا می‌شود؛ آن پُرکردن گاهی از داخل
 *     یک کار طولانی دیگر صدا زده می‌شود (FM_Idle وسط دیکد تصویر). پس کالبک
 *     صدا باید فقط کپی کند و برگردد؛ نوشتن روی کارت در حلقهٔ اصلی است.
 *   - کالبک فریم خودش در حلقهٔ اصلی است (داخل SimpleVideo_Poll)، بین دو
 *     فریم؛ همان‌جا فشرده و نوشته می‌شود.
 *
 * سه نکته که این مثال‌ها رعایت می‌کنند (و برنامهٔ شما هم باید):
 *   ۱) کالبک دادهٔ صدا فقط وقتی اجرا می‌شود که خروجی شامل «کالبک» باشد:
 *        SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH)      بلندگو + کالبک
 *        SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH)
 *   ۲) اولین نمونه‌ها و اولین فریم «قبل» از کالبک شروع ترک می‌رسند. پس
 *      ذخیره را قبل از شروع پخش آماده کنید (RecStart همین فایل)، نه داخل
 *      کالبک شروع ترک. فایل با اولین داده باز می‌شود، چون نرخ نمونه‌برداری
 *      و اندازهٔ تصویر همان‌جا معلوم می‌شود.
 *   ۳) Pump را در هر دور حلقهٔ اصلی صدا بزنید، کنار Poll.
 *
 * ویدیو: اگر پخش‌کننده فریم جا بیندازد (دیکد کند)، جای آن فریم‌ها همان
 * تصویر قبلی تکرار می‌شود (SimpleVideo_RecRepeat) تا زمان فایل با صدا جور
 * بماند. هر فایل یک ویدیو است: با رسیدن ویدیوی بعدی (یا برگشت به عقب)
 * فایل بسته می‌شود.
 *
 * حافظهٔ رینگ با FM_Alloc در RecStart گرفته و در RecStop آزاد می‌شود:
 *   SAVE_DEMO_MUSIC_RING_SAMPLES  پیش‌فرض ۷۰۰۰۰ نمونه (۱۴۰ کیلوبایت)
 *   SAVE_DEMO_VIDEO_RING_SAMPLES  پیش‌فرض ۴۰۰۰۰ نمونه (۸۰ کیلوبایت)
 * اندازه باید از دو نیمهٔ بافر I2S بیشتر باشد (پخش‌کننده در شروع هر دو
 * نیمه را پشت‌سرهم پُر می‌کند). اگر رینگ پُر شود آن تکه دور ریخته و در
 * گزارش پایان شمرده می‌شود (dropped).
 *
 * پیش‌نیاز: کارت مونت باشد (SdCard_Begin یا UsbSd_FatRoot).
 *
 * FM-REQUIRES: simple_audio.h simple_video.h
 */
#ifndef FM_TOPAPI_EXAMPLES_SAVE_MEDIA_H
#define FM_TOPAPI_EXAMPLES_SAVE_MEDIA_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "file_manager.h"
#include "fm_alloc.h"
#include "fm_audio.h"
#include "fm_video.h"
#include "fm_directory.h"
#include "fm_log.h"

/* ---------------------------------------------------------------------- */
/* تنظیم‌ها                                                                */
/* ---------------------------------------------------------------------- */
#define SAVE_DEMO_DIR         "/save"
#define SAVE_DEMO_TONE_WAV    "/save/tone.wav"
#define SAVE_DEMO_TONE_FLAC   "/save/tone.flac"
#define SAVE_DEMO_BARS_AVI    "/save/bars.avi"
#define SAVE_DEMO_MUSIC_COPY  "/save/music_cb.wav"
#define SAVE_DEMO_VIDEO_COPY  "/save/video_cb.avi"

#ifndef SAVE_DEMO_MUSIC_RING_SAMPLES
#define SAVE_DEMO_MUSIC_RING_SAMPLES 70000u
#endif
#ifndef SAVE_DEMO_VIDEO_RING_SAMPLES
#define SAVE_DEMO_VIDEO_RING_SAMPLES 40000u
#endif

/* ---------------------------------------------------------------------- */
/* ابزار مشترک                                                             */
/* ---------------------------------------------------------------------- */

/* یک خط برای هر مرحله: «ok» یا متن خطا. نتیجه را برمی‌گرداند. */
static FM_Result save_demo_step(const char *what, FM_Result r)
{
    if (r == FM_OK)
    {
        FM_Log("  ok  %s\n", what);
    }
    else
    {
        FM_Log("  !!  %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
    }
    return r;
}

static void save_demo_make_dir(void)
{
    FM_Result r = Directory_Create(SAVE_DEMO_DIR);

    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        (void)save_demo_step("Directory_Create " SAVE_DEMO_DIR, r);
    }
}

/* یک دورهٔ سینوس در ۱۶ نمونه: با ۱۶۰۰۰ نمونه در ثانیه می‌شود ۱ کیلوهرتز. */
static const int16_t save_demo_sine16[16] = {
        0,   4592,   8485,  11087,  12000,  11087,   8485,   4592,
        0,  -4592,  -8485, -11087, -12000, -11087,  -8485,  -4592
};

/* `frames` نمونهٔ مونو از بوق؛ `phase` بین فراخوانی‌ها حفظ می‌شود تا بوق
 * در مرز بلوک‌ها تق نزند. */
static void save_demo_tone(int16_t *pcm, size_t frames, uint32_t *phase)
{
    size_t i;

    for (i = 0u; i < frames; i++)
    {
        pcm[i] = save_demo_sine16[*phase & 15u];
        (*phase)++;
    }
}

/* ---- رینگ نمونه‌ها: یک نویسنده (کالبک)، یک خواننده (Pump) ----------------
 * head را فقط کالبک جلو می‌برد و tail را فقط Pump؛ برای همین اگر کالبک
 * وسط کار Pump صدا زده شود هم چیزی خراب نمی‌شود. یک خانه همیشه خالی
 * می‌ماند تا «پُر» از «خالی» تشخیص داده شود. */
typedef struct
{
    int16_t        *data;       /* FM_Alloc در RecStart */
    size_t          cap;        /* ظرفیت به نمونه (زوج) */
    volatile size_t head;       /* جای نوشتن بعدی */
    volatile size_t tail;       /* جای خواندن بعدی */
    uint32_t        dropped;    /* نمونه‌هایی که جا نشدند */
} SaveDemoRing;

static FM_Result save_demo_ring_open(SaveDemoRing *q, size_t samples)
{
    memset(q, 0, sizeof(*q));
    samples &= ~(size_t)1u;
    q->data = (int16_t *)FM_Alloc(samples * sizeof(int16_t));
    if (q->data == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    q->cap = samples;
    return FM_OK;
}

static void save_demo_ring_close(SaveDemoRing *q)
{
    if (q->data != NULL)
    {
        FM_Free(q->data);
    }
    q->data = NULL;
    q->cap = 0u;
}

/* کالبک: همهٔ تکه یا هیچ (نصفه نوشتن جفت چپ/راست را به‌هم می‌زند). */
static void save_demo_ring_put(SaveDemoRing *q, const int16_t *pcm, size_t n)
{
    size_t head = q->head;
    size_t used = (head + q->cap - q->tail) % q->cap;
    size_t first;

    if (n > q->cap - 1u - used)
    {
        q->dropped += (uint32_t)n;
        return;
    }
    first = q->cap - head;                  /* تا ته آرایه */
    if (first > n)
    {
        first = n;
    }
    memcpy(q->data + head, pcm, first * sizeof(int16_t));
    memcpy(q->data, pcm + first, (n - first) * sizeof(int16_t));
    q->head = (head + n) % q->cap;
}

/* Pump: تکهٔ پیوستهٔ آماده (ممکن است تا ته آرایه باشد؛ بقیه در دور بعد). */
static size_t save_demo_ring_peek(const SaveDemoRing *q, const int16_t **pcm)
{
    size_t head = q->head;
    size_t tail = q->tail;

    *pcm = q->data + tail;
    return (head >= tail) ? (head - tail) : (q->cap - tail);
}

static void save_demo_ring_skip(SaveDemoRing *q, size_t n)
{
    q->tail = (q->tail + n) % q->cap;
}

/* ====================================================================== */
/* سبک ۱ — ساده: صدا                                                       */
/* ====================================================================== */
#if FM_USE_AUDIO

/* بعد از بستن، فایل دوباره باز می‌شود تا ببینید واقعاً چه چیزی نوشته شده. */
static void save_demo_show_audio(const char *path)
{
    static FM_Audio a;
    FM_AudioInfo info;
    uint64_t bytes = 0u;
    FM_Result r;

    memset(&info, 0, sizeof(info));
    r = Audio_Open(path, &a);
    if (r != FM_OK)
    {
        (void)save_demo_step("Audio_Open (check)", r);
        return;
    }
    (void)Audio_GetInfo(&a, &info);
    (void)Audio_Close(&a);
    (void)File_SizeOf(path, &bytes);
    FM_Log("  -> %s: %s, %lu Hz, %u ch, %lu frames (%lu ms), %lu bytes\n",
           path, info.codec, (unsigned long)info.sample_rate,
           (unsigned)info.channels, (unsigned long)info.total_frames,
           (unsigned long)(info.sample_rate != 0u
               ? info.total_frames * 1000u / info.sample_rate : 0u),
           (unsigned long)bytes);
}

/* ۲ ثانیه بوق مونو ۱۶ کیلوهرتز در `path`. فرمت از پسوند: ‎.wav‎ یا ‎.flac‎. */
static FM_Result save_demo_tone_file(const char *path)
{
    static int16_t block[1600];             /* ۰٫۱ ثانیه */
    uint32_t phase = 0u;
    uint32_t n;
    FM_Result r;

    /* ۱) باز کردن: مشخصات داده‌ای که می‌خواهید بدهید */
    r = save_demo_step("SimpleAudio_RecStart",
                       SimpleAudio_RecStart(path, 16000u, 1u));
    if (r != FM_OK)
    {
        return r;
    }

    /* ۲) داده: هر بار یک بلوک؛ در برنامهٔ واقعی این بلوک از میکروفون،
     *    ADC یا محاسبهٔ خودتان می‌آید */
    for (n = 0u; n < 20u && r == FM_OK; n++)
    {
        save_demo_tone(block, 1600u, &phase);
        r = SimpleAudio_RecWrite(block, 1600u);
    }
    (void)save_demo_step("SimpleAudio_RecWrite x20", r);

    /* ۳) بستن: الزامی؛ طول فایل در سرصفحه همین‌جا نوشته می‌شود */
    {
        FM_Result rs = save_demo_step("SimpleAudio_RecStop",
                                      SimpleAudio_RecStop());

        if (r == FM_OK)
        {
            r = rs;
        }
    }
    if (r == FM_OK)
    {
        save_demo_show_audio(path);
    }
    return r;
}

FM_Result SaveDemo_AudioPlain(void)
{
    FM_Result r;

    FM_Log("SaveDemo_AudioPlain: 1 kHz tone, 2 s, 16 kHz mono\n");
    save_demo_make_dir();
    r = save_demo_tone_file(SAVE_DEMO_TONE_WAV);        /* PCM ساده */
    if (r == FM_OK)
    {
        r = save_demo_tone_file(SAVE_DEMO_TONE_FLAC);   /* همان، فشردهٔ بی‌افت */
    }
    return r;
}

#else  /* FM_USE_AUDIO */
FM_Result SaveDemo_AudioPlain(void)
{
    FM_Log("SaveDemo_AudioPlain: FM_USE_AUDIO is 0\n");
    return FM_ERR_UNSUPPORTED;
}
#endif /* FM_USE_AUDIO */

/* ====================================================================== */
/* سبک ۱ — ساده: ویدیو                                                     */
/* ====================================================================== */
#if FM_USE_VIDEO

#define SAVE_DEMO_VW   160u
#define SAVE_DEMO_VH   120u
#define SAVE_DEMO_VFPS 10u
#define SAVE_DEMO_VN   30u                  /* ۳ ثانیه */

static void save_demo_show_video(const char *path)
{
    static FM_Video v;
    FM_VideoInfo info;
    uint64_t bytes = 0u;
    FM_Result r;

    memset(&info, 0, sizeof(info));
    r = Video_Open(path, &v);
    if (r != FM_OK)
    {
        (void)save_demo_step("Video_Open (check)", r);
        return;
    }
    (void)Video_GetInfo(&v, &info);
    (void)Video_Close(&v);
    (void)File_SizeOf(path, &bytes);
    FM_Log("  -> %s: %s, %ux%u, %lu fps, %lu frames, %lu bytes\n", path,
           info.container, (unsigned)info.width, (unsigned)info.height,
           (unsigned long)info.fps, (unsigned long)info.frame_count,
           (unsigned long)bytes);
}

/* فریم شمارهٔ n: هشت نوار رنگی و یک مربع سفید که هر فریم جلو می‌رود. */
static void save_demo_draw(uint16_t *fb, uint32_t n)
{
    static const uint16_t bars[8] = {
        0xFFFFu, 0xFFE0u, 0x07FFu, 0x07E0u, 0xF81Fu, 0xF800u, 0x001Fu, 0x0000u
    };
    uint32_t x;
    uint32_t y;
    uint32_t box = (n * 4u) % (SAVE_DEMO_VW - 24u);

    for (y = 0u; y < SAVE_DEMO_VH; y++)
    {
        for (x = 0u; x < SAVE_DEMO_VW; x++)
        {
            uint16_t c = bars[x * 8u / SAVE_DEMO_VW];

            if (x >= box && x < box + 24u && y >= 48u && y < 72u)
            {
                c = 0xFFFFu;
            }
            fb[y * SAVE_DEMO_VW + x] = c;
        }
    }
}

FM_Result SaveDemo_VideoPlain(void)
{
    static uint16_t fb[SAVE_DEMO_VW * SAVE_DEMO_VH];    /* RGB565 */
    static int16_t snd[16000u / SAVE_DEMO_VFPS];        /* صدای یک فریم */
    uint32_t phase = 0u;
    uint32_t n;
    FM_Result r;

    FM_Log("SaveDemo_VideoPlain: %ux%u, %u fps, %u frames, 16 kHz mono\n",
           (unsigned)SAVE_DEMO_VW, (unsigned)SAVE_DEMO_VH,
           (unsigned)SAVE_DEMO_VFPS, (unsigned)SAVE_DEMO_VN);
    save_demo_make_dir();

    /* ۱) تنظیم‌های ماندگار، قبل از RecStart */
    (void)SimpleVideo_RecSetQuality(80u);       /* JPEG هر فریم، ۱ تا ۱۰۰ */
    (void)SimpleVideo_RecSetAudio(16000u, 1u);  /* تراک صدا؛ (0, 0) = بی‌صدا */

    /* ۲) باز کردن: پسوند ‎.avi‎ یعنی MJPEG در AVI */
    r = save_demo_step("SimpleVideo_RecStart",
                       SimpleVideo_RecStart(SAVE_DEMO_BARS_AVI,
                                            (uint16_t)SAVE_DEMO_VW,
                                            (uint16_t)SAVE_DEMO_VH,
                                            SAVE_DEMO_VFPS, 0u));
    if (r == FM_OK)
    {
        /* ۳) هر فریم: تصویر، بعد صدای همان بازه (نرخ / fps نمونه) */
        for (n = 0u; n < SAVE_DEMO_VN && r == FM_OK; n++)
        {
            save_demo_draw(fb, n);
            r = SimpleVideo_RecWritePixels(fb, FM_PIXEL_RGB565);
            if (r == FM_OK)
            {
                save_demo_tone(snd, 16000u / SAVE_DEMO_VFPS, &phase);
                r = SimpleVideo_RecWriteAudio(snd, 16000u / SAVE_DEMO_VFPS);
            }
        }
        (void)save_demo_step("RecWritePixels + RecWriteAudio x30", r);

        /* ۴) بستن: الزامی؛ فهرست فریم‌ها (ایندکس) همین‌جا نوشته می‌شود */
        {
            FM_Result rs = save_demo_step("SimpleVideo_RecStop",
                                          SimpleVideo_RecStop());

            if (r == FM_OK)
            {
                r = rs;
            }
        }
    }
    (void)SimpleVideo_RecSetAudio(0u, 0u);      /* ذخیرهٔ بعدی: پیش‌فرض بی‌صدا */
    if (r == FM_OK)
    {
        save_demo_show_video(SAVE_DEMO_BARS_AVI);
    }
    return r;
}

#else  /* FM_USE_VIDEO */
FM_Result SaveDemo_VideoPlain(void)
{
    FM_Log("SaveDemo_VideoPlain: FM_USE_VIDEO is 0\n");
    return FM_ERR_UNSUPPORTED;
}
#endif /* FM_USE_VIDEO */

/* ====================================================================== */
/* سبک ۲ — از کالبک: موزیکی که پخش می‌شود                                  */
/* ====================================================================== */
#if FM_USE_AUDIO

static struct
{
    uint8_t      armed;         /* RecStart صدا زده شده، منتظر یا در حال ذخیره */
    uint8_t      open;          /* فایل باز است */
    uint8_t      finish;        /* کالبک خواسته Pump فایل را ببندد */
    char         dest[FM_PATH_MAX];
    uint32_t     rate;          /* از اولین تکهٔ کالبک */
    uint16_t     channels;
    uint32_t     frames;        /* نوشته‌شده در فایل */
    SaveDemoRing ring;
} save_demo_music;

/*
 * کالبک دادهٔ موزیک (FM_AudioDataFn). نصب:
 *     SimpleAudio_SetDataCallback(SaveDemo_MusicCallback, NULL);
 * یا از داخل کالبک خودتان، بعد از پردازش، همین را صدا بزنید؛ هر چه در pcm
 * باشد همان ذخیره می‌شود.
 *
 * samples تعداد نمونه‌هاست (چپ و راست جدا شمرده می‌شوند)، channels تعداد
 * کانال؛ پس samples / channels فریم. اینجا فقط کپی می‌شود.
 */
void SaveDemo_MusicCallback(void *ctx, int16_t *pcm, size_t samples,
                            uint32_t rate, uint16_t channels)
{
    (void)ctx;
    if (!save_demo_music.armed || save_demo_music.finish || samples == 0u)
    {
        return;
    }
    if (save_demo_music.rate == 0u)
    {
        /* اولین تکه: مشخصات صدا همین‌جا معلوم می‌شود */
        save_demo_music.rate = rate;
        save_demo_music.channels = channels;
    }
    else if (rate != save_demo_music.rate ||
             channels != save_demo_music.channels)
    {
        /* ترک بعدی مشخصات دیگری دارد؛ یک فایل فقط یک نرخ دارد */
        save_demo_music.finish = 1u;
        return;
    }
    save_demo_ring_put(&save_demo_music.ring, pcm, samples);
}

/* آماده‌باش: از اولین تکه‌ای که کالبک بگیرد ذخیره شروع می‌شود. قبل از
 * شروع پخش صدا بزنید تا اول آهنگ هم در فایل باشد؛ وسط پخش هم می‌شود. */
FM_Result SaveDemo_MusicRecStart(const char *dest)
{
    FM_Result r;

    if (dest == NULL || strlen(dest) >= sizeof(save_demo_music.dest))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (save_demo_music.armed)
    {
        return FM_ERR_BUSY;
    }
    save_demo_make_dir();
    r = save_demo_ring_open(&save_demo_music.ring,
                            SAVE_DEMO_MUSIC_RING_SAMPLES);
    if (r != FM_OK)
    {
        return r;
    }
    (void)strcpy(save_demo_music.dest, dest);
    save_demo_music.rate = 0u;
    save_demo_music.channels = 0u;
    save_demo_music.frames = 0u;
    save_demo_music.open = 0u;
    save_demo_music.finish = 0u;
    save_demo_music.armed = 1u;             /* آخر از همه: کالبک فعال شد */
    FM_Log("SaveDemo music: waiting for the data callback -> %s\n", dest);
    return FM_OK;
}

FM_Bool SaveDemo_MusicRecActive(void)
{
    return save_demo_music.armed ? FM_TRUE : FM_FALSE;
}

/* هر چه در رینگ هست در فایل نوشته می‌شود. */
static FM_Result save_demo_music_drain(void)
{
    FM_Result r = FM_OK;
    const int16_t *pcm;
    size_t n;

    while (r == FM_OK &&
           (n = save_demo_ring_peek(&save_demo_music.ring, &pcm)) != 0u)
    {
        r = SimpleAudio_RecWrite(pcm, n / save_demo_music.channels);
        save_demo_ring_skip(&save_demo_music.ring, n);
        save_demo_music.frames += (uint32_t)(n / save_demo_music.channels);
    }
    return r;
}

/* پایان: باقی‌ماندهٔ رینگ نوشته، فایل بسته و حافظهٔ رینگ آزاد می‌شود. */
FM_Result SaveDemo_MusicRecStop(void)
{
    FM_Result r = FM_OK;

    if (!save_demo_music.armed)
    {
        return FM_ERR_NOT_READY;
    }
    save_demo_music.armed = 0u;             /* اول از همه: کالبک دیگر نمی‌نویسد */
    if (save_demo_music.open)
    {
        FM_Result rs;

        r = save_demo_music_drain();
        rs = SimpleAudio_RecStop();
        if (r == FM_OK)
        {
            r = rs;
        }
        save_demo_music.open = 0u;
        FM_Log("SaveDemo music: %s, %lu frames (%lu s), dropped %lu "
               "samples: %s\n", save_demo_music.dest,
               (unsigned long)save_demo_music.frames,
               (unsigned long)(save_demo_music.frames /
                               save_demo_music.rate),
               (unsigned long)save_demo_music.ring.dropped,
               File_GetErrorString(r));
    }
    else
    {
        FM_Log("SaveDemo music: stopped, no sound came through the "
               "callback (is the output BOTH or CALLBACK?)\n");
    }
    save_demo_ring_close(&save_demo_music.ring);
    return r;
}

/* در هر دور حلقهٔ اصلی. وقتی ذخیره‌ای در کار نیست فوری برمی‌گردد. */
FM_Result SaveDemo_MusicRecPump(void)
{
    FM_Result r = FM_OK;

    if (!save_demo_music.armed)
    {
        return FM_OK;
    }
    /* ۱) اولین داده رسیده -> فایل با همان مشخصات باز می‌شود */
    if (!save_demo_music.open && save_demo_music.rate != 0u)
    {
        r = SimpleAudio_RecStart(save_demo_music.dest, save_demo_music.rate,
                                 save_demo_music.channels);
        if (r != FM_OK)
        {
            (void)save_demo_step("SaveDemo music: SimpleAudio_RecStart", r);
            (void)SaveDemo_MusicRecStop();
            return r;
        }
        save_demo_music.open = 1u;
        FM_Log("SaveDemo music: saving %lu Hz %u ch -> %s\n",
               (unsigned long)save_demo_music.rate,
               (unsigned)save_demo_music.channels, save_demo_music.dest);
    }
    /* ۲) رینگ -> فایل */
    if (save_demo_music.open)
    {
        r = save_demo_music_drain();
    }
    /* ۳) خطای نوشتن، یا کالبک پایان را خواسته -> بستن */
    if (r != FM_OK || save_demo_music.finish)
    {
        FM_Result rs = SaveDemo_MusicRecStop();

        if (r == FM_OK)
        {
            r = rs;
        }
    }
    return r;
}

/*
 * همه‌چیز در یک‌جا: پوشهٔ موزیک پخش می‌شود و `seconds` ثانیهٔ اول آن (یا
 * تا پایان پخش) از کالبک داده در `dest` ذخیره می‌شود. تا پایان برنمی‌گردد.
 *
 * این تابع کالبک داده و خروجی SimpleAudio را عوض می‌کند و در پایان پخش را
 * می‌بندد؛ اگر برنامهٔ شما کالبک خودش را دارد، به‌جای این تابع
 * SaveDemo_MusicCallback را از کالبک خودتان صدا بزنید و فقط
 * RecStart / Pump / RecStop را به کار ببرید.
 */
FM_Result SaveDemo_MusicFromCallback(const char *dest, uint32_t seconds)
{
    FM_AudioOutput old_out = SimpleAudio_GetOutput();
    FM_Result r;

    FM_Log("SaveDemo_MusicFromCallback: %lu s of the music -> %s\n",
           (unsigned long)seconds, dest);

    /* ۱) کالبک داده روشن (بلندگو هم می‌ماند) */
    (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
    SimpleAudio_SetDataCallback(SaveDemo_MusicCallback, NULL);

    /* ۲) ذخیره آماده، قبل از شروع پخش */
    r = save_demo_step("SaveDemo_MusicRecStart",
                       SaveDemo_MusicRecStart(dest));

    /* ۳) پخش */
    if (r == FM_OK)
    {
        r = save_demo_step("SimpleAudio_Begin", SimpleAudio_Begin());
    }

    /* ۴) حلقه: همان دو خطی که در while (1) برنامه می‌گذارید */
    while (r == FM_OK && SaveDemo_MusicRecActive() &&
           SimpleAudio_IsRunning() &&
           (save_demo_music.rate == 0u ||
            save_demo_music.frames < seconds * save_demo_music.rate))
    {
        (void)SimpleAudio_Poll();
        r = SaveDemo_MusicRecPump();
    }

    /* ۵) پایان */
    if (SaveDemo_MusicRecActive())
    {
        FM_Result rs = SaveDemo_MusicRecStop();

        if (r == FM_OK)
        {
            r = rs;
        }
    }
    (void)SimpleAudio_Stop();
    SimpleAudio_SetDataCallback(NULL, NULL);
    (void)SimpleAudio_SetOutput(old_out);
    if (r == FM_OK)
    {
        save_demo_show_audio(dest);
    }
    return r;
}

#else  /* FM_USE_AUDIO */
void SaveDemo_MusicCallback(void *ctx, int16_t *pcm, size_t samples,
                            uint32_t rate, uint16_t channels)
{ (void)ctx; (void)pcm; (void)samples; (void)rate; (void)channels; }
FM_Result SaveDemo_MusicRecStart(const char *dest)
{ (void)dest; return FM_ERR_UNSUPPORTED; }
FM_Bool SaveDemo_MusicRecActive(void)
{ return FM_FALSE; }
FM_Result SaveDemo_MusicRecStop(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SaveDemo_MusicRecPump(void)
{ return FM_OK; }
FM_Result SaveDemo_MusicFromCallback(const char *dest, uint32_t seconds)
{ (void)dest; (void)seconds; return FM_ERR_UNSUPPORTED; }
#endif /* FM_USE_AUDIO */

/* ====================================================================== */
/* سبک ۲ — از کالبک: ویدیویی که پخش می‌شود                                 */
/* ====================================================================== */
#if FM_USE_VIDEO

static struct
{
    uint8_t      armed;         /* RecStart صدا زده شده */
    uint8_t      open;          /* فایل باز است */
    uint8_t      finish;        /* کالبک خواسته Pump فایل را ببندد */
    char         dest[FM_PATH_MAX];
    uint16_t     w;             /* اندازهٔ فریم‌های این فایل */
    uint16_t     h;
    uint32_t     item;          /* شمارهٔ این ویدیو در فهرست پخش */
    uint32_t     first_src;     /* شمارهٔ اولین فریم ذخیره‌شده در ویدیوی اصلی */
    uint32_t     last_src;      /* شمارهٔ آخرین فریم ذخیره‌شده */
    uint32_t     repeated;      /* فریم‌هایی که با تکرار پُر شدند */
    uint32_t     sound_rate;    /* 0 = فایل تراک صدا ندارد */
    uint32_t     sound_frames;
    FM_Result    error;
    SaveDemoRing ring;
} save_demo_video;

/*
 * کالبک فریم (SimpleVideo_FrameFn). نصب:
 *     SimpleVideo_SetFrameCallback(SaveDemo_VideoFrameCallback, NULL);
 *
 * برای هر فریمی که پخش‌کننده نشان می‌دهد یک بار، در حلقهٔ اصلی؛ pixels به
 * اندازه و فرمت خروجی پخش‌کننده است (پیش‌فرض RGB565) و فقط تا برگشتن از
 * همین تابع معتبر است.
 */
void SaveDemo_VideoFrameCallback(void *ctx, const void *pixels, uint16_t w,
                                 uint16_t h, FM_PixelFormat format,
                                 size_t bytes)
{
    SimpleVideoInfo info;
    uint32_t src;
    uint32_t want;
    uint32_t have;
    FM_Result r;

    (void)ctx;
    (void)bytes;
    if (!save_demo_video.armed || save_demo_video.finish)
    {
        return;
    }

    /* این فریم چندمین فریم ویدیوی اصلی است؟ frame_index «فریم بعدی» را
     * می‌گوید، پس فریمِ الان یکی قبل از آن است. */
    SimpleVideo_Info(&info);
    src = (info.frame_index > 0u) ? info.frame_index - 1u : 0u;

    if (!save_demo_video.open)
    {
        /* اولین فریم: اندازه، fps و صدا معلوم شد -> فایل باز می‌شود.
         * صدا همیشه استریو می‌رسد (ویدیوی مونو دو بار کپی شده). */
        save_demo_video.sound_rate =
            (info.has_audio &&
             ((unsigned)SimpleVideo_GetAudioOutput() &
              (unsigned)FM_AUDIO_OUT_CALLBACK) != 0u) ? info.audio_rate : 0u;
        (void)SimpleVideo_RecSetAudio(save_demo_video.sound_rate, 2u);
        r = SimpleVideo_RecStart(save_demo_video.dest, w, h, info.fps, 0u);
        if (r != FM_OK)
        {
            save_demo_video.error = r;
            save_demo_video.finish = 1u;
            return;
        }
        save_demo_video.open = 1u;
        save_demo_video.w = w;
        save_demo_video.h = h;
        save_demo_video.item = info.playlist_index;
        save_demo_video.first_src = src;
        FM_Log("SaveDemo video: saving %ux%u %lu fps, sound %lu Hz -> %s\n",
               (unsigned)w, (unsigned)h, (unsigned long)info.fps,
               (unsigned long)save_demo_video.sound_rate,
               save_demo_video.dest);
    }
    else if (info.playlist_index != save_demo_video.item ||
             w != save_demo_video.w || h != save_demo_video.h ||
             src < save_demo_video.last_src)
    {
        /* ویدیوی بعدی، تکرار از اول، یا پرش به عقب: این فایل تمام است */
        save_demo_video.finish = 1u;
        return;
    }
    else if (src == save_demo_video.last_src)
    {
        return;                             /* همان فریم دوباره نشان داده شد */
    }

    /* فریم‌های جاافتاده: فایل باید قبل از این فریم (src - first_src) فریم
     * داشته باشد؛ کمبود با تکرار تصویر قبلی پُر می‌شود. */
    want = src - save_demo_video.first_src;
    have = SimpleVideo_RecFrames();
    r = FM_OK;
    if (want > have)
    {
        r = SimpleVideo_RecRepeat(want - have);
        save_demo_video.repeated += want - have;
    }
    if (r == FM_OK)
    {
        /* فریم پخش‌کننده: RecWritePlayerFrame، نه RecWritePixels؛ خروجی ۱۶
         * بیتی پخش‌کننده برای LCD بایت‌جابه‌جا است و این تابع آن را می‌داند */
        r = SimpleVideo_RecWritePlayerFrame(pixels, format);
    }
    save_demo_video.last_src = src;
    if (r != FM_OK)
    {
        save_demo_video.error = r;
        save_demo_video.finish = 1u;
    }
}

/*
 * کالبک دادهٔ صدای ویدیو (FM_AudioDataFn). نصب:
 *     SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
 *     SimpleVideo_SetAudioDataCallback(SaveDemo_VideoSoundCallback, NULL);
 * همیشه چپ/راست پشت‌سرهم (channels = 2). فقط کپی.
 */
void SaveDemo_VideoSoundCallback(void *ctx, int16_t *pcm, size_t samples,
                                 uint32_t rate, uint16_t channels)
{
    (void)ctx;
    (void)rate;
    (void)channels;
    if (!save_demo_video.armed || save_demo_video.finish)
    {
        return;
    }
    save_demo_ring_put(&save_demo_video.ring, pcm, samples);
}

/* آماده‌باش: از اولین فریمی که کالبک بگیرد ذخیره شروع می‌شود. قبل از
 * شروع ویدیو صدا بزنید تا صدا و تصویر از یک نقطه شروع شوند؛ اگر وسط پخش
 * صدا بزنید صدا تا حدود یک بافر I2S از تصویر جلوتر می‌افتد (پخش‌کننده صدا
 * را زودتر از لحظهٔ پخش دیکد می‌کند). */
FM_Result SaveDemo_VideoRecStart(const char *dest)
{
    FM_Result r;

    if (dest == NULL || strlen(dest) >= sizeof(save_demo_video.dest))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (save_demo_video.armed || SimpleVideo_RecActive())
    {
        return FM_ERR_BUSY;
    }
    save_demo_make_dir();
    r = save_demo_ring_open(&save_demo_video.ring,
                            SAVE_DEMO_VIDEO_RING_SAMPLES);
    if (r != FM_OK)
    {
        return r;
    }
    (void)strcpy(save_demo_video.dest, dest);
    save_demo_video.open = 0u;
    save_demo_video.finish = 0u;
    save_demo_video.first_src = 0u;
    save_demo_video.last_src = 0u;
    save_demo_video.repeated = 0u;
    save_demo_video.sound_rate = 0u;
    save_demo_video.sound_frames = 0u;
    save_demo_video.error = FM_OK;
    save_demo_video.armed = 1u;             /* آخر از همه: کالبک‌ها فعال شدند */
    FM_Log("SaveDemo video: waiting for the frame callback -> %s\n", dest);
    return FM_OK;
}

FM_Bool SaveDemo_VideoRecActive(void)
{
    return save_demo_video.armed ? FM_TRUE : FM_FALSE;
}

/* صدای رینگ -> تراک صدای فایل (اگر فایل تراک صدا ندارد، دور ریخته می‌شود). */
static FM_Result save_demo_video_drain(void)
{
    FM_Result r = FM_OK;
    const int16_t *pcm;
    size_t n;

    while (r == FM_OK &&
           (n = save_demo_ring_peek(&save_demo_video.ring, &pcm)) != 0u)
    {
        if (save_demo_video.sound_rate != 0u)
        {
            r = SimpleVideo_RecWriteAudio(pcm, n / 2u);
            save_demo_video.sound_frames += (uint32_t)(n / 2u);
        }
        save_demo_ring_skip(&save_demo_video.ring, n);
    }
    return r;
}

/* پایان: باقی‌ماندهٔ صدا نوشته، فایل بسته (ایندکس) و رینگ آزاد می‌شود. */
FM_Result SaveDemo_VideoRecStop(void)
{
    FM_Result r = save_demo_video.error;

    if (!save_demo_video.armed)
    {
        return FM_ERR_NOT_READY;
    }
    save_demo_video.armed = 0u;             /* اول از همه: کالبک‌ها بی‌اثر */
    if (save_demo_video.open)
    {
        uint32_t frames = SimpleVideo_RecFrames();
        FM_Result rd = save_demo_video_drain();
        FM_Result rs = SimpleVideo_RecStop();

        if (r == FM_OK)
        {
            r = (rd != FM_OK) ? rd : rs;
        }
        save_demo_video.open = 0u;
        FM_Log("SaveDemo video: %s, %lu frames (%lu repeated), sound %lu "
               "frames, dropped %lu samples: %s\n", save_demo_video.dest,
               (unsigned long)frames,
               (unsigned long)save_demo_video.repeated,
               (unsigned long)save_demo_video.sound_frames,
               (unsigned long)save_demo_video.ring.dropped,
               File_GetErrorString(r));
    }
    else if (r != FM_OK)
    {
        (void)save_demo_step("SaveDemo video: SimpleVideo_RecStart", r);
    }
    else
    {
        FM_Log("SaveDemo video: stopped, no frame came through the "
               "callback\n");
    }
    (void)SimpleVideo_RecSetAudio(0u, 0u);  /* ذخیرهٔ بعدی: پیش‌فرض بی‌صدا */
    save_demo_ring_close(&save_demo_video.ring);
    return r;
}

/* در هر دور حلقهٔ اصلی. وقتی ذخیره‌ای در کار نیست فوری برمی‌گردد. */
FM_Result SaveDemo_VideoRecPump(void)
{
    FM_Result r = FM_OK;

    if (!save_demo_video.armed)
    {
        return FM_OK;
    }
    /* ۱) صدا: رینگ -> فایل (تا فایل باز نشده، در رینگ می‌ماند) */
    if (save_demo_video.open)
    {
        r = save_demo_video_drain();
    }
    /* ۲) ویدیو به آخر رسیده؟ بعد از آخرین فریم دیگر کالبکی نمی‌آید، پس
     *    اینجا از خود پخش‌کننده پرسیده می‌شود. */
    if (r == FM_OK && save_demo_video.open && !save_demo_video.finish)
    {
        SimpleVideoInfo info;

        SimpleVideo_Info(&info);
        if (!info.running)
        {
            save_demo_video.finish = 1u;    /* SimpleVideo_Stop یا پایان فهرست */
        }
        else if (info.playlist_index == save_demo_video.item &&
                 info.frame_count != 0u &&
                 info.frame_index >= info.frame_count)
        {
            /* همهٔ فریم‌های ویدیو مصرف شده‌اند. اگر چند فریم آخر جا افتاده
             * باشند، با تصویر آخر پُر می‌شوند تا طول تصویر و صدا یکی شود. */
            uint32_t want = info.frame_count - save_demo_video.first_src;
            uint32_t have = SimpleVideo_RecFrames();

            if (want > have)
            {
                r = SimpleVideo_RecRepeat(want - have);
                save_demo_video.repeated += want - have;
            }
            save_demo_video.finish = 1u;
        }
    }
    /* ۳) خطا، یا پایان -> بستن */
    if (r != FM_OK || save_demo_video.finish)
    {
        FM_Result rs;

        if (save_demo_video.error == FM_OK)
        {
            save_demo_video.error = r;
        }
        rs = SaveDemo_VideoRecStop();
        if (r == FM_OK)
        {
            r = rs;
        }
    }
    return r;
}

/*
 * همه‌چیز در یک‌جا: اولین ویدیوی پوشهٔ ویدیو پخش می‌شود و تصویر و صدایش
 * از کالبک‌ها در `dest` ذخیره می‌شود؛ تا پایان ویدیو یا `max_frames` فریم
 * (0 = بدون سقف). تا پایان برنمی‌گردد.
 *
 * این تابع کالبک فریم، کالبک صدا و خروجی صدای SimpleVideo را عوض می‌کند و
 * در پایان پخش را می‌بندد؛ اگر برنامهٔ شما کالبک‌های خودش را دارد، دو
 * کالبک این فایل را از داخل کالبک‌های خودتان صدا بزنید.
 */
FM_Result SaveDemo_VideoFromCallback(const char *dest, uint32_t max_frames)
{
    FM_AudioOutput old_out = SimpleVideo_GetAudioOutput();
    FM_Result r;

    FM_Log("SaveDemo_VideoFromCallback: the playing video -> %s\n", dest);

    /* ۱) کالبک‌ها: فریم، و صدا (خروجی باید شامل کالبک باشد) */
    (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    SimpleVideo_SetAudioDataCallback(SaveDemo_VideoSoundCallback, NULL);
    SimpleVideo_SetFrameCallback(SaveDemo_VideoFrameCallback, NULL);

    /* ۲) ذخیره آماده، قبل از شروع پخش */
    r = save_demo_step("SaveDemo_VideoRecStart",
                       SaveDemo_VideoRecStart(dest));

    /* ۳) پخش: فهرست پوشه، بعد اولین ویدیو */
    if (r == FM_OK)
    {
        r = save_demo_step("SimpleVideo_Begin", SimpleVideo_Begin());
    }
    if (r == FM_OK)
    {
        (void)SimpleVideo_Start();
    }

    /* ۴) حلقه: همان دو خطی که در while (1) برنامه می‌گذارید */
    while (r == FM_OK && SaveDemo_VideoRecActive() &&
           (max_frames == 0u || SimpleVideo_RecFrames() < max_frames))
    {
        (void)SimpleVideo_Poll();
        r = SaveDemo_VideoRecPump();
        if (!SimpleVideo_IsRunning())
        {
            break;                          /* پخش شروع نشد یا تمام شد */
        }
    }

    /* ۵) پایان */
    if (SaveDemo_VideoRecActive())
    {
        FM_Result rs = SaveDemo_VideoRecStop();

        if (r == FM_OK)
        {
            r = rs;
        }
    }
    (void)SimpleVideo_Stop();
    SimpleVideo_SetFrameCallback(NULL, NULL);
    SimpleVideo_SetAudioDataCallback(NULL, NULL);
    (void)SimpleVideo_SetAudioOutput(old_out);
    if (r == FM_OK)
    {
        save_demo_show_video(dest);
    }
    return r;
}

#else  /* FM_USE_VIDEO */
void SaveDemo_VideoFrameCallback(void *ctx, const void *pixels, uint16_t w,
                                 uint16_t h, FM_PixelFormat format,
                                 size_t bytes)
{ (void)ctx; (void)pixels; (void)w; (void)h; (void)format; (void)bytes; }
void SaveDemo_VideoSoundCallback(void *ctx, int16_t *pcm, size_t samples,
                                 uint32_t rate, uint16_t channels)
{ (void)ctx; (void)pcm; (void)samples; (void)rate; (void)channels; }
FM_Result SaveDemo_VideoRecStart(const char *dest)
{ (void)dest; return FM_ERR_UNSUPPORTED; }
FM_Bool SaveDemo_VideoRecActive(void)
{ return FM_FALSE; }
FM_Result SaveDemo_VideoRecStop(void)
{ return FM_ERR_UNSUPPORTED; }
FM_Result SaveDemo_VideoRecPump(void)
{ return FM_OK; }
FM_Result SaveDemo_VideoFromCallback(const char *dest, uint32_t max_frames)
{ (void)dest; (void)max_frames; return FM_ERR_UNSUPPORTED; }
#endif /* FM_USE_VIDEO */

#endif /* FM_TOPAPI_EXAMPLES_SAVE_MEDIA_H */
