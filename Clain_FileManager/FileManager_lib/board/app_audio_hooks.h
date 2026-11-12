/*
 * app_audio_hooks.h — قلاب‌های صدا، ضبط آزمایشی و دستورهای Music_*
 * ================================================================
 *
 *   Music_DataRead / Music_DataReadMono     خواندن PCM خام (دستورهای dtrd / dtrdm)
 *   app_audio_pre_tap            قبل از ولوم: نرمالایزر بلندی (tap1 / tap0)
 *   app_audio_pcm_tap                  بعد از ولوم: پیک زنده + رینگ ضبط
 *   app_audio_track_start/end    شروع و پایان هر ترک
 *   Music_InstallTaps()         نصب همهٔ این‌ها — یک‌بار کافی است (ماندگارند)
 *   Music_*                     دستورهای UART (جدول در top_api/code_cmd.h)
 *   Music_RecPump()             فایل ضبط rectest — در حلقهٔ اصلی
 *
 * فقط از main.c شامل می‌شود (بعد از app_display.h).
 *
 * بلوک بین «#define AGC_TARGET_PEAK» و خط «TAPS-BLOCK-END» را
 * `make tapstest` عیناً بیرون می‌کشد و روی کامپیوتر اجرا می‌کند
 * (tools/board_taps_host_test.c)؛ این دو نشانه را جابه‌جا نکنید.
 */
#ifndef FM_BOARD_APP_AUDIO_HOOKS_H_
#define FM_BOARD_APP_AUDIO_HOOKS_H_

/* -------------------------------------------------------------------------- */
/* خواندن دادهٔ صدا                                                            */
/* -------------------------------------------------------------------------- */
/*
 * اصلاح شد: امضای واقعی این است —
 *
 *     FM_Result SimpleAudio_DataRead(int16_t *buf, size_t frames,
 *                                    size_t *got_frames);
 *
 * قبلاً `&bufm` پاس داده می‌شد که اشاره‌گرِ اشاره‌گر است، و `frames`
 * هرگز مقدار نگرفته بود (صفر بود) و `got` هم یک اشاره‌گر NULL بود.
 * یعنی این دو تابع در عمل یا هیچ کاری نمی‌کردند یا حافظه را خراب
 * می‌کردند. حالا یک بافر واقعی دارند.
 */
#define AUDIO_TAP_FRAMES 256u
static int16_t audio_buf[AUDIO_TAP_FRAMES * 2u];   /* استریو: L,R */

void Music_DataRead(void)
{
    size_t got = 0;
    FM_Result r = SimpleAudio_DataRead(audio_buf, AUDIO_TAP_FRAMES, &got);

    printf("Music_DataRead -> %d, %u frames\n", (int)r, (unsigned)got);
    if (r == FM_OK && got > 0u)
    {
        printf("  first L=%d R=%d\n", audio_buf[0], audio_buf[1]);
    }
}

void Music_DataReadMono(void)
{
    size_t got = 0;
    FM_Result r = SimpleAudio_DataReadMono(audio_buf, AUDIO_TAP_FRAMES,
                                           &got);

    printf("Music_DataReadMono -> %d, %u samples\n", (int)r, (unsigned)got);
}

/* ========================================================================== */
/* قلاب‌های PCM (audio_record_with_tap.h) — کدام تابع، کِی اجرا می‌شود؟                     */
/* ========================================================================== */
/*
 *   مسیر صدا از فایل تا بلندگو (به این ترتیب اجرا می‌شود):
 *
 *      فایل (MP3/WAV/FLAC/...) → دیکد
 *        │
 *        ▼
 *      ۱) app_audio_pre_tap  ←◀ «قبل از پردازش صدا» (قبل از ولوم)
 *        │                      DSP / اکولایزر / نرمالایزر بلندی اینجا
 *        ▼
 *      ۲) ولوم نرم‌افزاری (دستورهای vol50 / vol90)
 *        │
 *        ▼
 *      ۳) app_audio_pcm_tap        ←◀ «بعد از پردازش صدا» (بعد از ولوم)
 *        │                      VU متر / تله‌متری / ضبط — دقیقاً صدای بلندگو
 *        ▼
 *      DMA → I2S → بلندگو
 *
 *   - هر دو بافر قابل‌ویرایش‌اند: نمونه‌ها را درجا تغییر دهید، اثرش
 *     همان لحظه در خروجی می‌آید.
 *   - این توابع داخل مسیر refill صدا زده می‌شوند؛ فقط کار سبک
 *     (حلقهٔ کوتاه / memcpy). باز کردن و نوشتن فایل اینجا ممنوع —
 *     گرسنگی DMA و تق‌تق صدا.
 *   - نصب: Music_InstallTaps()، قبل یا بعد از SimpleAudio_Begin(). کالبک‌ها
 *     ماندگارند و با Stop/Begin پاک نمی‌شوند.
 *   - مسیر: SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER / _CALLBACK / _BOTH)
 *     (دستورهای aout1 / aout2 / aout3).
 */

/* ---- ۱) قبل از پردازش صدا (قبل از ولوم) ---------------------------------- */
/*
 * «نرمالایزر بلندی» (AGC) — ریشه‌ی «بعضی آهنگ‌ها کم و زیاد می‌شوند»:
 * پیک چانک را دنبال می‌کند و به‌آرامی گین را به سمت هدف می‌برد؛
 *   - تراک آرام (پیک ۱۰۰۰) → تقویت تا ~۲ برابر (حداکثر +۶dB)
 *   - تراک بلند (پیک ۳۰۰۰۰) → هرگز تقویت نمی‌شود (گین ≥ ۱؛ قطع/کلیپ نداریم)
 * گین در هر چانک حداکثر ۱۲.۵٪ بالا می‌رود (بدون زیپر-نویز) و در صورت
 * بلندشدن ناگهانی ورودی، فوراً پایین می‌آید (کلیپ ممنوع).
 * دستورها:  tap1 = نرمالایزر روشن / tap0 = خاموش.
 * همین تابع روی صدای ویدیو هم نصب می‌شود (پس از SimpleVideo_Begin).
 */
#define AGC_TARGET_PEAK 23000u      /* ~-3dBFS — هدف بلندی */
#define AGC_MAX_GAIN_Q8 512u        /* 2.0x = +6dB سقف تقویت */
#define AGC_ATTACK_Q8     32u       /* +12.5% در هر فراخوانی */
static uint8_t  agc_enabled;        /* tap1/tap0 */
static uint32_t agc_peak_env;       /* دنبال‌کنندهٔ پیک (حمله فوری، رهاسازی آرام) */
static uint32_t agc_gain_q8 = 256u; /* 1.0x */

static void app_audio_pre_tap(void *ctx, int16_t *pcm, size_t samples,
                             uint32_t rate, uint16_t channels)
{
    size_t i;
    uint32_t peak = 0u, target_q8, g;

    (void)ctx; (void)rate; (void)channels;

    if (!agc_enabled || samples == 0u)
    {
        return;                       /* خاموش — هیچ هزینه‌ای ندارد */
    }

    /* حمله فوری / رهاسازی آرام روی پویایی تراک */
    for (i = 0u; i < samples; i++)
    {
        int32_t v = pcm[i];

        if (v < 0) { v = -v; }
        if ((uint32_t)v > peak) { peak = (uint32_t)v; }
    }
    agc_peak_env = (peak > agc_peak_env) ? peak : (agc_peak_env * 15u) / 16u;

    /* هدف: رساندن پیک به AGC_TARGET_PEAK؛ هرگز کمتر از 1.0x (قطع نکن) */
    target_q8 = (agc_peak_env == 0u) ? AGC_MAX_GAIN_Q8
              : (AGC_TARGET_PEAK * 256u) / agc_peak_env;
    if (target_q8 > AGC_MAX_GAIN_Q8) { target_q8 = AGC_MAX_GAIN_Q8; }
    if (target_q8 < 256u)            { target_q8 = 256u; }

    if (target_q8 < agc_gain_q8)
    {
        agc_gain_q8 = target_q8;      /* ورودی بلند شد: فوراً پایین (کلیپ ممنوع) */
    }
    else if (target_q8 > agc_gain_q8 + AGC_ATTACK_Q8)
    {
        agc_gain_q8 += AGC_ATTACK_Q8; /* بالا رفتن نرم */
    }
    else
    {
        agc_gain_q8 = target_q8;
    }

    g = agc_gain_q8;
    if (g == 256u)
    {
        return;                       /* 1.0x — دست نزن */
    }
    for (i = 0u; i < samples; i++)
    {
        int32_t v = ((int32_t)pcm[i] * (int32_t)g + 128) >> 8;

        if (v > 32767)       { v = 32767; }
        else if (v < -32768) { v = -32768; }
        pcm[i] = (int16_t)v;
    }
}

/* ---- ۲) بعد از پردازش صدا (بعد از ولوم، دقیقاً صدای بلندگو) --------------- */
/*
 * Music_DataRead بالا استریم را جلو می‌برد، پس برای «شنیدن همان چیزی که پخش
 * می‌شود» مناسب نیست. این قلاب بعد از اعمال ولوم و درست قبل از DMA صدا
 * زده می‌شود.
 *
 * کاربرد: ضبط همزمان با پخش (rectest)، VU meter، FFT، یا افکت.
 */
static volatile uint32_t tap_calls;
static volatile int16_t  tap_peak;

/* ---- ضبط اتوماتیک ۵ ثانیه‌ای از صدای بلندگو (دستور rectest) ----
 * قلاب فقط در حافظه کپی می‌کند (سبک)؛ باز کردن/نوشتن فایل در
 * Music_RecPump از حلقهٔ اصلی انجام می‌شود — پس هیچ هنگی نیست. */
#define REC_RING_CAP 16384u
static int16_t rec_ring[REC_RING_CAP];
static volatile size_t rec_ring_len;
static volatile uint8_t  rec_cfg_wait;    /* 1=منتظر rate/ch از قلاب، 2=آماده */
static volatile uint32_t rec_rate;
static volatile uint16_t rec_ch;
static uint8_t  rec_active_demo;          /* ضبط در جریان است */
static uint32_t rec_deadline;             /* زمان RecStop (wrap-safe) */

static void app_audio_pcm_tap(void *ctx, int16_t *pcm, size_t samples,
                       uint32_t rate, uint16_t channels)
{
    size_t k;
    int16_t peak = 0;

    (void)ctx;
    (void)channels;

    /* تله‌متری: پیک همین چانک — چاپ هر ۲ ثانیه از حلقهٔ اصلی */
    for (k = 0; k < samples; k++)
    {
        int16_t v = pcm[k];

        if (v < 0) { v = (int16_t)-v; }
        if (v > peak) { peak = v; }
    }
    tap_peak = peak;
    tap_calls++;

    /* نرخ/کانال فقط همین‌جا در دسترس است؛ فایل در حلقهٔ اصلی باز می‌شود */
    if (rec_cfg_wait == 1u)
    {
        rec_rate = rate;
        rec_ch = (channels == 0u) ? 2u : channels;
        rec_cfg_wait = 2u;
    }

    /* کپی به رینگ ضبط — فقط وقتی حلقه خالی است (تک‌تولیدکننده/تک‌مصرف) */
    if (rec_active_demo != 0u && rec_ring_len == 0u &&
        samples <= REC_RING_CAP)
    {
        memcpy(rec_ring, pcm, samples * sizeof(int16_t));
        rec_ring_len = samples;
    }
}

/* ---- قلاب‌های شروع/پایان ترک (DSP می‌داند چه چیزی پردازش می‌شود) ---------
 * SimpleAudio_SetTrackStartCallback/SetTrackEndCallback: وقتی یک ترک شروع/تمام می‌شود
 * صدا زده می‌شوند (از حلقهٔ اصلی، نه ISR). همین‌جا است که لایهٔ DSP
 * «کدام موزیک» در حال پردازش است را می‌فهمد؛ اگر پردازش گران است آن را
 * به تپِ PCM بدهید، نه اینجا (اینجا فقط چند چاپ سبک). */
static uint32_t track_seq;                   /* شمارندهٔ ترک‌های پخش‌شده */
static char     last_audio_track[FM_PATH_MAX]; /* برای نمایش «در حال پخش» */

static void app_audio_track_start(void *ctx, const char *path,
                                 const FM_AudioInfo *info)
{
    (void)ctx;
    track_seq++;
    if (path == NULL) path = "";
    (void)strncpy(last_audio_track, path, sizeof(last_audio_track) - 1u);
    last_audio_track[sizeof(last_audio_track) - 1u] = '\0';
    printf("audio track #%lu START: %s (%s %luHz %uch)\n",
           (unsigned long)track_seq, path,
           (info != NULL) ? info->codec : "?",
           (unsigned long)((info != NULL) ? info->sample_rate : 0u),
           (unsigned)((info != NULL) ? info->channels : 0u));
}

static void app_audio_track_end(void *ctx, const char *path,
                               const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    printf("audio track END: %s\n", (path != NULL) ? path : "");
}

/* ---- نصب کالبک‌های موزیک ---------------------------------------------------
 * یک‌بار کافی است؛ با Stop/Begin پاک نمی‌شوند. فراخوانی دوباره فقط
 * نرمالایزر را از صفر شروع می‌کند. */
static void Music_InstallTaps(void)
{
    agc_peak_env = 0u;              /* تراک جدید: دنباله از صفر */
    agc_gain_q8 = 256u;
    SimpleAudio_SetDataCallback(app_audio_pre_tap, NULL);    /* قبل از ولوم */
    SimpleAudio_SetMonitorCallback(app_audio_pcm_tap, NULL); /* بعد از ولوم */
    SimpleAudio_SetTrackStartCallback(app_audio_track_start, NULL);
    SimpleAudio_SetTrackEndCallback(app_audio_track_end, NULL);
}

/* ---- دستورهای UART جدید (از top_api/code_cmd.h صدا زده می‌شوند) ------------- */

/* tap1: نرمالایزر روشن / tap0: خاموش */
static void Music_TapBoostOn(void)
{
    agc_enabled = 1u;
    printf("pre-DSP: نرمالایزر روشن (هدف ~-3dBFS، سقف +6dB؛ tap0 = خاموش)\n");
}
static void Music_TapBoostOff(void)
{
    agc_enabled = 0u;
    agc_peak_env = 0u;
    agc_gain_q8 = 256u;
    printf("pre-DSP: نرمالایزر خاموش\n");
}

/* rectest: ضبط ۵ ثانیه از «همان چیزی که بلندگو می‌شنود» → /rec/rec_XXXXXX.wav */
static void Music_RecTest(void)
{
    if (!SimpleAudio_IsRunning())
    {
        printf("rectest: اول موزیک را start کن، بعد rectest بزن\n");
        return;
    }
    if (rec_active_demo != 0u || rec_cfg_wait != 0u)
    {
        printf("rectest: ضبط در جریان است...\n");
        return;
    }
    rec_ring_len = 0u;
    rec_cfg_wait = 1u;        /* قلاب بعدی نرخ/کانال را تحویل می‌دهد */
    printf("rectest: ضبط ۵ ثانیه شروع می‌شود (پایان: خودکار)\n");
}

/* pools: دیاگ خطای -14 (NO_MEMORY) — چند هندل فایل باز است و توسط کی */
static void Music_PoolDump(void)
{
    printf("pool: %lu/8 هندل | FM_Alloc fail=%lu | SDRAM free=%luK\n",
           (unsigned long)File_PoolUsed(),
           (unsigned long)FM_Alloc_FailureCount(),
           (unsigned long)(SdramAlloc_FreeBytes() / 1024u));
    File_PoolDump();
    /* fail>0  ⇒ -14 از کمبود/فرگمانت SDRAM است (نه نشت هندل).
     * fail=0 و pool<8 و باز -14 ⇒ خطا از داخل FatFs:
     *   ffconf.h → FF_USE_LFN==3 (بافر اسم بلند با ff_memalloc روی
     *   heap ۳۲KB لینکر) یا FF_FS_LOCK>0 (سهمیهٔ فایل هم‌زمان). */
}

/* vvol2: ولوم ویدیو تا ۲۰۰٪ (ولوم نرم‌افزاری، اجازهٔ بالای ۱۰۰) */
static void Music_VideoVol200(void)
{
    FM_Result r = SimpleVideo_SetVolume(200u);

    if (r != FM_OK)
    {
        printf("vvol2: خطا %d (%s)\n", (int)r, File_GetErrorString(r));
    }
    else
    {
        printf("vvol2: ولوم ویدیو = 200%%\n");
    }
}

/* istart: اجرای اسلایدشو تصویر از حلقهٔ اصلی (همان SimpleImage_Begin
 * بوت، ولی بدون ری‌ست برد) — دستور «istart» */
static void Music_ImageStart(void)
{
    printf("istart: SimpleImage_Begin\n");
    SimpleImage_SetOutputSize(320, 240);
    if (SimpleImage_Begin() != FM_OK)
    {
        printf("istart: SimpleImage_Begin failed\n");
    }
}

/* EnableAudio: خاموش/روشن صدای ویدیو (تست ایزولهٔ تصویر).
 * خاموش = FM_AUDIO_OUT_NONE: از کلیپ بعدی ترک صدا اصلاً خوانده نمی‌شود. */
static void Music_ToggleVideoAudio(void)
{
    static uint8_t ve = 1u;

    ve = (uint8_t)!ve;
    (void)SimpleVideo_SetAudioOutput(ve ? FM_AUDIO_OUT_BOTH
                                        : FM_AUDIO_OUT_NONE);
    printf("EnableAudio: صدای ویدیو %s\n", ve ? "روشن" : "خاموش");
}

/* vnopres: تست A/B کراکل — نمایش LCD ویدیو خاموش/روشن می‌شود؛
 * دیکد و صدا ادامه دارد. اگر بدون LCD صدا تمیز شد ⇒ ریشهٔ کراکل
 * رقابت DMA ارائهٔ LCD با I2S است (نه دکد). */
static void Music_ToggleVideoPresent(void)
{
    static uint8_t vp = 1u;

    vp = (uint8_t)!vp;
    SimpleVideo_SetPresent(vp ? App_LcdBlitVideo : NULL);
    printf("vnopres: نمایش LCD ویدیو %s\n", vp ? "روشن" : "خاموش");
}

/* vinfo: مشخصات ترک صوتی ویدیوی در حال پخش */
static void Music_VideoInfo(void)
{
    FM_AudioTrackInfo ai;

    if (SimpleVideo_GetAudioInfo(&ai) == FM_OK)
    {
        printf("video audio: %lu Hz %u ch %u bit, %lu smp\n",
               (unsigned long)ai.sample_rate, (unsigned)ai.channels,
               (unsigned)ai.bits_per_sample,
               (unsigned long)ai.sample_count);
    }
    else
    {
        printf("vinfo: ویدیو فعال نیست (اول vstrt بزنید)\n");
    }
}

/* پمپ ضبط — هر دورِ while(1) صدا زده می‌شود (فایل I/O فقط اینجا) */
static void Music_RecPump(void)
{
    if (rec_cfg_wait != 0u || rec_active_demo != 0u)
    {
        /* مرحلهٔ ۱: نرخ/کانال از قلاب رسیده → باز کردن فایل */
        if (rec_cfg_wait == 2u)
        {
            FM_Result rr;

            rec_cfg_wait = 0u;
            rr = SimpleAudio_RecStart("/rec", rec_rate, rec_ch);
            if (rr == FM_OK)
            {
                rec_active_demo = 1u;
                rec_deadline = HAL_GetTick() + 5000u;
                printf("rectest: %lu Hz %u ch → ضبط...\n",
                       (unsigned long)rec_rate, (unsigned)rec_ch);
            }
            else
            {
                printf("rectest: RecStart خطا %d (%s) pool=%lu/8 "
                       "alloc_fail=%lu sdram_free=%luK\n",
                       (int)rr, File_GetErrorString(rr),
                       (unsigned long)File_PoolUsed(),
                       (unsigned long)FM_Alloc_FailureCount(),
                       (unsigned long)(SdramAlloc_FreeBytes() / 1024u));
                File_PoolDump();
            }
        }

        /* مرحلهٔ ۲: خالی‌کردن رینگ در فایل */
        if (rec_active_demo != 0u)
        {
            if (rec_ring_len != 0u)
            {
                (void)SimpleAudio_RecWrite(rec_ring,
                                           rec_ring_len / rec_ch);
                rec_ring_len = 0u;
            }

            /* مرحلهٔ ۳: پایان خودکار بعد از ۵ ثانیه (wrap-safe) */
            if ((int32_t)(HAL_GetTick() - rec_deadline) >= 0)
            {
                rec_active_demo = 0u;
                if (SimpleAudio_RecStop() == FM_OK)
                {
                    printf("rectest: ذخیره شد → %s\n",
                           SimpleAudio_RecPath());
                }
                else
                {
                    printf("rectest: RecStop خطا داد\n");
                }
            }
        }
    }
}

/* TAPS-BLOCK-END (make tapstest تا همین خط را برمی‌دارد) */

#endif /* FM_BOARD_APP_AUDIO_HOOKS_H_ */
