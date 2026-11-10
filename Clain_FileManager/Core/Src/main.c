/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "dma2d.h"
#include "fatfs.h"
#include "i2c.h"
#include "i2s.h"
#include "jpeg.h"
#include "mdma.h"
#include "quadspi.h"
#include "sdmmc.h"
#include "spi.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"
#include "fmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "defines.h"
#include "tests.h"
#include "test_tft.h"


/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
//FM_USE_FATFS=1
//FM_USE_AUDIO=1
//FM_USE_MP3=1
//FM_USE_VIDEO=1
//FM_USE_IMAGE=1
//FM_USE_USB_MSC=1
//FM_USE_DEVICE_ADAPTERS=1
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    LcdSpiDma_OnTxCplt(hspi);
}
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    LcdSpiDma_OnError(hspi);
}

/*------------------------------------------------------------------------------------*/
// images

//FM_PIXEL_GRAY8    = 1,
///* 16-bit values are stored little-endian. RGB565 has R in bits 15..11;
// * BGR565 has B in bits 15..11. */
//FM_PIXEL_RGB565   = 2,
//FM_PIXEL_RGB888   = 3,  /* R,G,B */
//FM_PIXEL_RGBA8888 = 4,  /* R,G,B,A */
//FM_PIXEL_BGR565   = 5,
//FM_PIXEL_BGR888   = 6,  /* B,G,R */
//FM_PIXEL_BGRA8888 = 7   /* B,G,R,A */


static const char *fmt_s[] = {
    "GRAY8","RGB565","RGB888","RGBA8888","BGR565","BGR888","BGRA8888"
};

void my_lcd_blit(const void *px, uint16_t w, uint16_t h,
                 FM_PixelFormat fmt, size_t bytes)
{
    (void)fmt;
    if (w == 0u || h == 0u || bytes == 0u)
        return;

    /* SimpleImage_SetOutputSize(320, 240) used to be called from HERE,
     * i.e. inside the present callback.  Once the image is loaded that
     * call queues a SI_CMD_RESIZE, which the next SimpleImage_Poll turns
     * into a bilinear rescale + a second blit — every time.  The output
     * size is already set once in main() before SimpleImage_Begin(). */

    ILI9341_Set_Address(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
    HAL_GPIO_WritePin(tft_dc_GPIO_Port, tft_dc_Pin, GPIO_PIN_SET);
    (void)LcdSpiDma_Send(px, bytes);   /* عکس: blocking */

#if FM_LOG_PER_FRAME
    printf("w:%d * h:%d | fmt:%s  | size:%d \n", w, h, fmt_s[fmt-1], bytes);
#else
    (void)fmt_s;
#endif
}

void my_lcd_blit_video(const void *px, uint16_t w, uint16_t h,
                 FM_PixelFormat fmt, size_t bytes)
{
    (void)fmt;
    if (w == 0u || h == 0u || bytes == 0u)
        return;

    ILI9341_Set_Address(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
    HAL_GPIO_WritePin(tft_dc_GPIO_Port, tft_dc_Pin, GPIO_PIN_SET);
    /* FIX 2026-09: blocking present (sequential bus) restores 320
     * audio.  Async (LcdSpiDma_Begin + Wait) overlapped the 30 ms SPI
     * TX DMA with the audio SD reads inside decode N+1 — with D-Cache
     * on and a 186 ms half, the PCM bytes were fetched under contention
     * and arrived short/corrupt (starve stayed 0 but audio choppy).
     * With the video half now doubled to 372 ms and DMA priorities
     * tuned below, async can be re-enabled by defining
     * SIMPLE_VIDEO_ASYNC_PRESENT=1 for LTDC panels; SPI TFT stays
     * sequential where budget = SD+decode+SPI ≈45 ms for 320@15fps. */
#if defined(SIMPLE_VIDEO_ASYNC_PRESENT) && SIMPLE_VIDEO_ASYNC_PRESENT
    (void)LcdSpiDma_Begin(px, bytes);
#else
    (void)LcdSpiDma_Send(px, bytes);
#endif

#if FM_LOG_PER_FRAME
    printf("w:%d * h:%d | fmt:%s  | size:%d \n", w, h, fmt_s[fmt-1], bytes);
#endif
}



/*----------------------------------------------------------------------------------*/


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

void DataRead(void)
{
    size_t got = 0;
    FM_Result r = SimpleAudio_DataRead(audio_buf, AUDIO_TAP_FRAMES, &got);

    printf("DataRead -> %d, %u frames\n", (int)r, (unsigned)got);
    if (r == FM_OK && got > 0u)
    {
        printf("  first L=%d R=%d\n", audio_buf[0], audio_buf[1]);
    }
}

void DataReadMono(void)
{
    size_t got = 0;
    FM_Result r = SimpleAudio_DataReadMono(audio_buf, AUDIO_TAP_FRAMES,
                                           &got);

    printf("DataReadMono -> %d, %u samples\n", (int)r, (unsigned)got);
}

/* ========================================================================== */
/* قلاب‌های PCM (Example 33) — کدام تابع، کِی اجرا می‌شود؟                     */
/* ========================================================================== */
/*
 *   مسیر صدا از فایل تا بلندگو (به این ترتیب اجرا می‌شود):
 *
 *      فایل (MP3/WAV/FLAC/...) → دیکد
 *        │
 *        ▼
 *      ۱) my_audio_pre_tap  ←◀ «قبل از پردازش صدا» (قبل از ولوم)
 *        │                      DSP / اکولایزر / نرمالایزر بلندی اینجا
 *        ▼
 *      ۲) ولوم نرم‌افزاری (دستورهای vol50 / vol90)
 *        │
 *        ▼
 *      ۳) my_pcm_tap        ←◀ «بعد از پردازش صدا» (بعد از ولوم)
 *        │                      VU متر / تله‌متری / ضبط — دقیقاً صدای بلندگو
 *        ▼
 *      DMA → I2S → بلندگو
 *
 *   - هر دو بافر قابل‌ویرایش‌اند: نمونه‌ها را درجا تغییر دهید، اثرش
 *     همان لحظه در خروجی می‌آید.
 *   - این توابع داخل مسیر refill صدا زده می‌شوند؛ فقط کار سبک
 *     (حلقهٔ کوتاه / memcpy). باز کردن و نوشتن فایل اینجا ممنوع —
 *     گرسنگی DMA و تق‌تق صدا.
 *   - نصب: Music_InstallTaps() بعد از «هر» SimpleAudio_Begin() (چون Begin
 *     کل حالت را پاک می‌کند و قلاب نصب‌شده را می‌اندازد).
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

static void my_audio_pre_tap(void *ctx, int16_t *pcm, size_t samples,
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
 * DataRead بالا استریم را جلو می‌برد، پس برای «شنیدن همان چیزی که پخش
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

static void my_pcm_tap(void *ctx, int16_t *pcm, size_t samples,
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
 * SimpleAudio_SetTrackStart/SetTrackEnd: وقتی یک ترک شروع/تمام می‌شود
 * صدا زده می‌شوند (از حلقهٔ اصلی، نه ISR). همین‌جا است که لایهٔ DSP
 * «کدام موزیک» در حال پردازش است را می‌فهمد؛ اگر پردازش گران است آن را
 * به تپِ PCM بدهید، نه اینجا (اینجا فقط چند چاپ سبک). */
static uint32_t track_seq;                   /* شمارندهٔ ترک‌های پخش‌شده */
static char     last_audio_track[FM_PATH_MAX]; /* برای نمایش «در حال پخش» */

static void my_audio_track_start(void *ctx, const char *path,
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

static void my_audio_track_end(void *ctx, const char *path,
                               const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    printf("audio track END: %s\n", (path != NULL) ? path : "");
}

/* ---- نصب هر دو قلاب (بعد از هر Begin) ------------------------------------ */
static void Music_InstallTaps(void)
{
    agc_peak_env = 0u;              /* تراک جدید: دنباله از صفر */
    agc_gain_q8 = 256u;
    SimpleAudio_SetPrePcmTap(my_audio_pre_tap, NULL);   /* قبل از ولوم */
    SimpleAudio_SetPcmTap(my_pcm_tap, NULL);            /* بعد از ولوم  */
    SimpleAudio_SetTrackStart(my_audio_track_start, NULL);
    SimpleAudio_SetTrackEnd(my_audio_track_end, NULL);
}

/* ---- دستورهای UART جدید (از code_cmd_music.h صدا زده می‌شوند) ------------- */

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

/* EnableAudio: خاموش/روشن صدای ویدیو (تست ایزولهٔ تصویر) */
static void Music_ToggleVideoAudio(void)
{
    static uint8_t ve = 1u;

    ve = (uint8_t)!ve;
    SimpleVideo_EnableAudio(ve);
    printf("EnableAudio: صدای ویدیو %s\n", ve ? "روشن" : "خاموش");
}

/* vnopres: تست A/B کراکل — نمایش LCD ویدیو خاموش/روشن می‌شود؛
 * دیکد و صدا ادامه دارد. اگر بدون LCD صدا تمیز شد ⇒ ریشهٔ کراکل
 * رقابت DMA ارائهٔ LCD با I2S است (نه دکد). */
static void Music_ToggleVideoPresent(void)
{
    static uint8_t vp = 1u;

    vp = (uint8_t)!vp;
    SimpleVideo_SetPresent(vp ? my_lcd_blit_video : NULL);
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




/*------------------------------------------------------------------------------------*/
#define UART_RX_BUF_SIZE    20
#define CMD_BUF_SIZE        20
/* ---- قلاب‌های ویدیو: شروع/پایان ترک + دادهٔ فریم (DSP مستقل از نمایش) ----
 *   - my_video_track_start/end: می‌گوید کدام ویدیو شروع/تمام شد.
 *   - my_video_frame_cb: هر فریم دیکدشده (تبدیل‌شده به sv.format، پیش‌فرض
 *     RGB565) را تحویل می‌دهد؛ حتی وقتی SimpleVideo_SetPresent(NULL) است
 *     (حالت فقط-داده: ۶۰٪ مواقع فقط داده می‌خواهیم، نه LCD). اینجا فقط یک
 *     شمارنده/تله‌متری سبک — پردازش گران را به تپ PCM یا فریم بدهید.
 *   نصب: SimpleVideo_SetTrackStartCallback/SetTrackEndCallback/SetFrameCallback
 *   بعد از هر SimpleVideo_Begin() (Begin حالت را پاک می‌کند). */
static volatile uint32_t video_frames;          /* فریم‌های دیده‌شده */
static volatile uint32_t video_seq;             /* شمارهٔ ویدیوی در حال پخش */
static char last_video_track[FM_PATH_MAX];

static void my_video_track_start(void *ctx, const char *path,
                                 uint32_t w, uint32_t h, uint32_t fps)
{
    (void)ctx;
    video_seq++;
    video_frames = 0u;
    if (path == NULL) path = "";
    (void)strncpy(last_video_track, path, sizeof(last_video_track) - 1u);
    last_video_track[sizeof(last_video_track) - 1u] = '\0';
    printf("video track #%lu START: %s (%lux%lu @ %lu fps)\n",
           (unsigned long)video_seq, path,
           (unsigned long)w, (unsigned long)h, (unsigned long)fps);
}

static void my_video_track_end(void *ctx, const char *path)
{
    (void)ctx;
    printf("video track END: %s (frames=%lu)\n",
           (path != NULL) ? path : "", (unsigned long)video_frames);
}

static void my_video_frame_cb(void *ctx, const void *px, uint16_t w,
                              uint16_t h, FM_PixelFormat fmt, size_t bytes)
{
    (void)ctx; (void)px; (void)w; (void)h; (void)fmt; (void)bytes;
    video_frames++;
    /* اینجا جای پردازش فریم (مثلاً Image_Convert / ماسک / ویژگی‌گیری) است.
     * سبک بماند؛ برای کار سنگین داده را در بافر خودتان کپی کنید چون بافر
     * دوبل است و تا فراخوانی بعدی معتبر است. */
}

/* نصب هر سه قلاب ویدیو (بعد از هر Begin) */
static void Music_InstallVideoHooks(void)
{
    video_frames = 0u;
    SimpleVideo_SetTrackStartCallback(my_video_track_start, NULL);
    SimpleVideo_SetTrackEndCallback(my_video_track_end, NULL);
    SimpleVideo_SetFrameCallback(my_video_frame_cb, NULL);
}

/* vmut: صدای ویدیو را خاموش (مسیر پردازش، نیاز به صدا نیست).
 * vunmut: روشن. اختصارِ دستور: حالا فقط از EnableAudio استفاده شود. */
static void Music_VideoMute(void)
{
    SimpleVideo_EnableAudio(0u);
    printf("vmut: صدای ویدیو خاموش شد (پردازش فقط داده)\n");
}
static void Music_VideoUnmute(void)
{
    SimpleVideo_EnableAudio(1u);
    printf("vunmut: صدای ویدیو روشن شد\n");
}


uint8_t uart_rx_dma_buffer[UART_RX_BUF_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));

static uint8_t data_p[CMD_BUF_SIZE];

volatile uint32_t old_pos = 0;
uint32_t current_pos = 0;
uint32_t length = 0;
uint32_t len_p = 0;


/* -------------------------------------------------------------------------- */
/* Commands                                                                   */
/* -------------------------------------------------------------------------- */

void next(void)
{
    printf("next\n");
    SimpleAudio_Next();
}

void prev(void)
{
    printf("prev\n");
    SimpleAudio_Prev();
}

void np10(void)
{
    printf("np10\n");
//    SimpleAudio_SeekTo(10.0);
    SimpleAudio_SeekBy(+10.0);
}

void pn10(void)
{
    printf("pn10\n");
    SimpleAudio_SeekBy(-10.0);
}



void pauset(void) {
	printf("pauset \n");
	SimpleAudio_TogglePause();
}
void pause(void) {
	printf("pause \n");
	SimpleAudio_Pause();
}
void resume(void) {
	printf("resume \n");
	SimpleAudio_Resume();
}

/*
 * start/stop از داخل HAL_UARTEx_RxEventCallback (وقفهٔ UART) صدا زده
 * می‌شوند. قبلاً همان‌جا SimpleVideo_Stop() + SimpleAudio_Begin() اجرا
 * می‌شد؛ یعنی داخل ISR: بستن فایل ویدیو، fade صدا با busy-wait تا ۴۰ms،
 * اسکن پوشهٔ موزیک، باز کردن فایل و پر کردن دو نیمهٔ DMA از روی SD.
 * خواندن SD منتظر وقفهٔ SDMMC/DMA می‌ماند که با اولویت مساوی/پایین‌تر
 * نمی‌تواند وقفهٔ UART را قطع کند ⇒ همان «هنگ موقع رفتن به موزیک».
 *
 * حالا فقط یک پرچم ست می‌شود و کار واقعی در حلقهٔ اصلی انجام می‌شود
 * (دقیقاً همان الگوی viedo_start). SimpleAudio_Next/Prev/Seek/Pause/
 * Volume خودشان از قبل ISR-safe هستند (فقط پرچم می‌گذارند).
 */
volatile uint8_t music_cmd = 0;   /* 1 = start music, 2 = stop music */

void start(void)
{
    printf("start\n");
    music_cmd = 1;
}

void stop(void)
{
    printf("stop\n");
    music_cmd = 2;
}

SimpleAudioInfo i;
void info(void)
{
    printf("info\n");
    SimpleAudio_Info(&i);
    printf("bit:%d - channels:%d - codec:%s  - duration:%0.2f -  name:%s -  playlist_count:%u -  playlist_index:%u  -sample_rate:%u  - volume_pct:%d  \n",i.bits,i.channels,i.codec,i.duration,
    		i.name,i.playlist_count,i.playlist_index,i.sample_rate,i.volume_pct);
}

void vol50(void)
{
    printf("vol50\n");
    SimpleAudio_SetVolume(50);
//    SimpleAudio_SetVolume(i.volume_pct + 5);
}

void vol90(void)
{
    printf("vol90\n");
    SimpleAudio_SetVolume(90);
//    SimpleAudio_SetVolume(i.volume_pct + 5);
}




/* -------------------------------------------------------------------------- */
/* Command Parser                                                             */
/* -------------------------------------------------------------------------- */

#include "../../FileManager_lib/top_api/code_cmd.h"



static FM_Bool service_button_confirmed = FM_TRUE;
/* و اگر دکمهٔ سرویس داری، جایی که مطمئنی، یک‌بار:
   service_button_confirmed = FM_TRUE;  -- فقط وقتی می‌خواهی فلش فرمت شود! */


/* ---- SPI flash (ex33) ---- */
static FM_SpiFlashPort spi_port;
static FM_QspiDevice  flash;
/* بافر سکتور: حتماً خارج از DTCM تا DMA1/2 برسند + تراز خط کش */
static uint8_t sector_buf[4096] __attribute__((section(".sdram_buffers"), aligned(32)));




// for usb
// in file  usbd_storage_if.c
//#include "usb_msc.h"   /* بدون EX16_USB_DEFINE_CONTEXT! */
//
//int8_t STORAGE_Init_FS(uint8_t lun)
//    { return Ex16_STM32_Init(lun); }
//int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
//    { return Ex16_STM32_GetCapacity(lun, block_num, block_size); }
//int8_t STORAGE_IsReady_FS(uint8_t lun)
//    { return Ex16_STM32_IsReady(lun); }
//int8_t STORAGE_IsWriteProtected_FS(uint8_t lun)
//    { return Ex16_STM32_IsWriteProtected(lun); }
//int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
//    { return Ex16_STM32_Read(lun, buf, blk_addr, blk_len); }
//int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
//    { return Ex16_STM32_Write(lun, buf, blk_addr, blk_len); }
//int8_t STORAGE_GetMaxLun_FS(void)
//    { return Ex16_STM32_GetMaxLun(); }
//

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
//  FM_Cache_Enable();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_MDMA_Init();
  MX_DMA_Init();
  /* FIX 2026-09 (optional): bus priority — I2S VERY_HIGH > SPI LOW.
   * CubeMX leaves all at LOW; the 30 ms SPI burst can stall SDMMC.
   * With blocking Present + 16384 half this is a second-order effect;
   * keep the block but do NOT break build if DMA handles differ.
   * H7 HAL declares Instance as void*, so cast to Stream type.  SDMMC1
   * on H7 uses internal IDMA, not a DMA stream — do not touch it. */
#if 0 /* set to 1 if you still see contention with async Present */
  do {
    extern DMA_HandleTypeDef hdma_spi2_tx;
    extern DMA_HandleTypeDef hdma_i2s1_ext_tx;
    if (hdma_spi2_tx.Instance != NULL) {
      DMA_Stream_TypeDef *st = (DMA_Stream_TypeDef *)hdma_spi2_tx.Instance;
      st->CR = (st->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_LOW;
    }
    if (hdma_i2s1_ext_tx.Instance != NULL) {
      /* I2S1_ext TX is also a DMA stream on H7 (not BDMA) */
      DMA_Stream_TypeDef *st = (DMA_Stream_TypeDef *)hdma_i2s1_ext_tx.Instance;
      st->CR = (st->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_VERY_HIGH;
    }
  } while (0);
#endif
  MX_USART1_UART_Init();
  MX_FMC_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();
  MX_I2S1_Init();
  MX_SPI2_Init();
  MX_JPEG_Init();
  MX_DMA2D_Init();
  MX_I2C2_Init();
  MX_QUADSPI_Init();
  MX_SPI4_Init();
//  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
//  SCB_DisableDCache();
//  USBD_Stop(&hUsbDeviceFS);

  //  MX_LTDC_Init();
//  SimpleVideo_UseLtdc(&hltdc);


  FM_Log_UseHalUart(&huart1);   // به‌جای FM_Log_Init();

  FM_SD_Hal_Attach(&hsd1);
  Ex35_Sd_FatRoot();          /* ریشه = FAT32 - قبل از همهٔ ex_* */


//  SimpleImage_UseDma2d();                /* بدون CubeMX */
  SimpleImage_UseDma2dHandle(&hdma2d);   /* یا با هندل خودتان */


  printf("\n start \n");


  /* وضعیت کش و پورت — اگر dcache=0 دیدید یعنی MPU درست نیست. */
  FM_Cache_Report();
  printf("port=%s  debug=%s\n", FM_PORT_NAME, FM_Debug_LevelName());

  for(int i = 0;i<UART_RX_BUF_SIZE;i++) uart_rx_dma_buffer[i] = 0xff;
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart_rx_dma_buffer, UART_RX_BUF_SIZE);
//  __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT); // inactive callback helf

  /*----------------------------------------*/
	Generate_number();
	start_t = HAL_GetTick();
	SCB_CleanDCache_by_Addr(dataw, size_data*4);
	HAL_SDRAM_Write_DMA(&hsdram1,(uint32_t*)SdRamBaseAddress,dataw,size_data);
// 	HAL_SDRAM_Write_32b(&hsdram1,(uint32_t*)SdRamBaseAddress,dataw,size_data);
	HAL_Delay(10);
	SCB_InvalidateDCache_by_Addr(SdRamBaseAddress, size_data*4);
	ReadSdram();
	HAL_Delay(100);
	LcdSpiDma_Bind(&hspi2, tft_cs_GPIO_Port, tft_cs_Pin);
	init_tft();

	HAL_Delay(100);

	SdramAlloc_Init();
	printf("sdram self-test: %d free=%u\r\n",
	       SdramAlloc_SelfTest(), (unsigned)SdramAlloc_FreeBytes());
	FileManager_Init();

	/* یک بار کافی است. مقدار برگشتی FM_Result است: 0 یعنی FM_OK (فعال
	 * شد)، نه «غیرفعال». خط «hwjpeg img=0 vid=0» در لاگ قبلی یعنی هر دو
	 * موفق بودند — MJPEG با ۹–۱۸ms بر فریم همین را تأیید می‌کند. */
	{
		FM_Result hw_img = SimpleImage_UseHwJpeg(&hjpeg);
		FM_Result hw_vid = SimpleVideo_UseHwJpeg();
		printf("hwjpeg img=%s vid=%s\n",
		       (hw_img == FM_OK) ? "on" : File_GetErrorString(hw_img),
		       (hw_vid == FM_OK) ? "on" : File_GetErrorString(hw_vid));
	}

	/* present mode: blocking by default (sequential bus) — see
	 * my_lcd_blit_video.  Async (Begin+Wait) is opt-in via
	 * -DSIMPLE_VIDEO_ASYNC_PRESENT=1 for LTDC; SPI TFT stays sequential
	 * to avoid SD/SPI contention on 320+PCM. */
#if defined(SIMPLE_VIDEO_ASYNC_PRESENT) && SIMPLE_VIDEO_ASYNC_PRESENT
	SimpleVideo_SetPresentWait(LcdSpiDma_Wait);
#else
	SimpleVideo_SetPresentWait(NULL);
#endif




	/* -------------------------------------------------------------
	 * QSPI (وقتی راه انداختید این بلوک را باز کنید)
	 * -------------------------------------------------------------
	 * مدل قطعه خودکار از JEDEC ID شناسایی می‌شود. اگر فلش نباشد،
	 * تمیز پیام می‌دهد و برنامه ادامه می‌یافت.
	 *
	 *   static FM_QspiDevice qspi_dev;
	 *   static FM_Storage    qspi_st;
	 *   static FM_FS         qspi_fs;
	 *
	 *   if (Ex26_QspiBegin(&qspi_dev, &qspi_st, &hqspi,
	 *                      FM_QSPI_CHIP_AUTO) == FM_OK)
	 *   {
	 *       Storage_Mount("qspi");
	 *       FMFS_Init(&qspi_fs);
	 *       FS_Mount("/qspi", &qspi_fs);
	 *
	 *       // پخش از فلش به‌جای SD (سریع‌تر):
	 *       Ex29_Begin();
	 *   }
	 *
	 * و برای فلشی که روی SPI معمولی است (نه QUADSPI):
	 *
	 *   FM_SpiFlash_Init(&qspi_dev, &my_spi_port, FM_QSPI_CHIP_AUTO,
	 *                    sector_buf, sizeof(sector_buf));
	 *   FM_Qspi_RegisterStorage(&qspi_dev, &qspi_st, "flash");
	 * ------------------------------------------------------------- */

	/*----------------------------------------------------------------------------*/

	{
		printf("\n********************************ex_1_file_basic*******************************\n");
		ex_1_file_basic();
		HAL_Delay(100);
		printf("\n****************************ex_2_file_copy_move_rename***********************************\n");
		ex_2_file_copy_move_rename();
		HAL_Delay(100);
		printf("\n****************************ex_3_file_advance***********************************\n");
		ex_3_file_advance();
		HAL_Delay(100);
		printf("\n***************************ex_4_file_directory************************************\n");
		ex_4_file_directory();
		HAL_Delay(100);
		printf("\n***************************ex_5_file_json************************************\n");
		ex_5_file_json();HAL_Delay(100);
		printf("\n**************************ex_6_file_text_line_edit*************************************\n");
		ex_6_file_text_line_edit();HAL_Delay(100);
		printf("\n*************************ex_7_file_Crash_Safe_Writes**************************************\n");
		ex_7_file_Crash_Safe_Writes();HAL_Delay(100);
		printf("\n**************************ex_8_file_Data_Logger*************************************\n");
		ex_8_file_Data_Logger();HAL_Delay(100);
		printf("\n**************************ex_9_file_crc_cheaksum*************************************\n");
		ex_9_file_crc_cheaksum();HAL_Delay(100);
		printf("\n***************************ex_10_file_BMP_Image_Scale_Convert************************************\n");
		ex_10_file_BMP_Image_Scale_Convert();HAL_Delay(100);
		printf("\n***************************ex_11_audio_codecs************************************\n");
		ex_11_audio_codecs();HAL_Delay(100);
	}
	printf("\n***************************************************************\n");

	/*--------------------------------------------------------------------*/
//	test_sdcard();
	HAL_Delay(100);
	printf("\n****************************SD_examples_begin***********************************\n");
	SD_examples_begin();HAL_Delay(100);
	printf("\n****************************exS_2_file_copy_move_rename***********************************\n");
	exS_2_file_copy_move_rename();HAL_Delay(100);
	printf("\n******************************exS_3_file_advance*********************************\n");
	exS_3_file_advance();HAL_Delay(100);
	printf("\n***************************exS_4_file_directory************************************\n");
	exS_4_file_directory();HAL_Delay(100);
	printf("\n**************************exS_5_file_json*************************************\n");
	exS_5_file_json();HAL_Delay(100);
	printf("\n***************************exS_6_file_text_line_edit************************************\n");
	exS_6_file_text_line_edit();HAL_Delay(100);
	printf("\n****************************exS_7_file_Crash_Safe_Writes***********************************\n");
	exS_7_file_Crash_Safe_Writes();HAL_Delay(100);
	printf("\n*******************************exS_8_file_Data_Logger********************************\n");
	exS_8_file_Data_Logger();HAL_Delay(100);
	printf("\n******************************exS_9_file_crc_cheaksum*********************************\n");
	exS_9_file_crc_cheaksum();HAL_Delay(100);
	printf("\n*****************************exS_10_file_BMP_Image_Scale_Convert**********************************\n");
	exS_10_file_BMP_Image_Scale_Convert();HAL_Delay(100);

	printf("\n*************************exM_1_audio_wav**************************************\n");
	exM_1_audio_wav();HAL_Delay(100);
	printf("\n*************************exM_2_video_fmrv**************************************\n");
	exM_2_video_fmrv();
	printf("\n************************exM_3_copy_song***************************************\n");
	exM_3_copy_song();
	printf("\n************************exM_4_copy_video***************************************\n");
	exM_4_copy_video();
	printf("\n************************exM_5_media_detect***************************************\n");
	exM_5_media_detect();
	printf("\n*************************exP_1_music_controls**************************************\n");
	exP_1_music_controls();
	printf("\n************************exP_2_video_controls***************************************\n");
	exP_2_video_controls();
	printf("\n***********************exP_3_remote_buttons****************************************\n");
	exP_3_remote_buttons();

	printf("\n***********************qspi_transfers****************************************\n");
	if (QSPI_examples_begin() == FM_OK) {
		printf("\n***********************exQ_1_file_basic****************************************\n");
	    exQ_1_file_basic();          /* ۱) فایل — همان ترتیب SD */
		printf("\n***********************exQ_2_file_copy_move_rename****************************************\n");
	    exQ_2_file_copy_move_rename();
	    printf("\n***********************exQ_3_file_advance****************************************\n");
	    exQ_3_file_advance();
	    printf("\n***********************exQ_4_file_directory****************************************\n");
	    exQ_4_file_directory();
	    printf("\n***********************exQ_5_file_json****************************************\n");
	    exQ_5_file_json();
	    printf("\n***********************exQ_6_file_text_line_edit****************************************\n");
	    exQ_6_file_text_line_edit();
	    printf("\n***********************exQ_7_file_Crash_Safe_Writes****************************************\n");
	    exQ_7_file_Crash_Safe_Writes();
	    printf("\n***********************exQ_8_file_Data_Logger****************************************\n");
	    exQ_8_file_Data_Logger();
	    printf("\n***********************exQ_9_file_crc_cheaksum****************************************\n");
	    exQ_9_file_crc_cheaksum();
	    printf("\n***********************exQ_10_file_BMP_Image_Scale_Convert****************************************\n");
	    exQ_10_file_BMP_Image_Scale_Convert();
	    printf("\n***********************exQM_1_audio_detect_process_save****************************************\n");
	    exQM_1_audio_detect_process_save();
	    printf("\n***********************exQM_2_video_detect_to_bgr565****************************************\n");
	    exQM_2_video_detect_to_bgr565();
	    printf("\n***********************exQM_3_image_bgr_effect_resize_save****************************************\n");
	    exQM_3_image_bgr_effect_resize_save();
	    printf("\n***********************exQP_1_music_controls****************************************\n");
	    exQP_1_music_controls();
	    printf("\n***********************exQP_2_video_controls****************************************\n");
	    exQP_2_video_controls();
	    printf("\n***********************exQP_3_remote_buttons****************************************\n");
	    exQP_3_remote_buttons();
	    printf("\n***********************exT_1_sd_to_qspi****************************************\n");
	    exT_1_sd_to_qspi();          /* ۲) انتقال — نیازمند /config.json یا خودش می‌سازد */
	    printf("\n***********************exT_2_qspi_to_sdram****************************************\n");
	    exT_2_qspi_to_sdram();
	    printf("\n***********************exT_3_verify_roundtrip****************************************\n");
	    exT_3_verify_roundtrip();
	    printf("\n***********************exT_4_all_combinations****************************************\n");
	    exT_4_all_combinations();
	    printf("\n***********************QSPI_examples_end****************************************\n");
//	    QSPI_examples_end(); // for usb
	} else if (service_button_confirmed) {
		printf("\n***********************service_button_confirmed****************************************\n");
	    QSPI_examples_format();      /* فقط بار اول، با تأیید خودت */
		printf("\n***********************QSPI_examples_format****************************************\n");

	}




	printf("\n****************************Ex33_SpiFlash_Begin*****************************************************\n");
	Ex33_SpiFlash_SetCs(GPIOE, GPIO_PIN_4);
	if (Ex33_SpiFlash_Begin(&flash, &spi_port, &hspi4, FM_QSPI_CHIP_AUTO, sector_buf, sizeof(sector_buf)) == FM_OK) {
	    /* فلش نو: یک‌بار Ex33_SpiFlash_Format(); */
		printf("\n********************************exF_1_spi_flash_smoke*************************************************\n");
		exF_1_spi_flash_smoke();   /* دود write/read/verify (بافرهای تراز) */
		printf("\n******************************exF_2_file_ops***************************************************\n");
	    exF_2_file_ops();          /* متن + باینری 1K + CRC + پوشه        */
	    printf("\n*****************************exF_3_sd_to_flash****************************************************\n");
	    exF_3_sd_to_flash();       /* SD→فلش با وریفای                    */
	    printf("\n***************************exF_4_ram_roundtrip******************************************************\n");
	    exF_4_ram_roundtrip();     /* RAM→فلش→RAM                        */
	    printf("\n***************************exF_5_stats******************************************************\n");
	    exF_5_stats();             /* erase/write/blank-skip/DMA?          */
	}
	printf("\n*********************************************************************************\n");


	printf("\n*****************************Ex32_I2cEeprom_Begin****************************************************\n");
	if (Ex32_I2cEeprom_Begin(FM_EEPROM_CHIP_24LC02, &hi2c2, 0x50) == FM_OK) {
		printf("\n*****************************exI_1_i2c_dma_smoke****************************************************\n");
		exI_1_i2c_dma_smoke();     /* پروب + بنویس/بخوان/تطبیق */
		printf("\n*****************************exE_1_basic****************************************************\n");
	    exE_1_basic();             /* بقیهٔ تست‌ها از مثال ۲۸   */
	    printf("\n*****************************exE_2_settings_struct****************************************************\n");
	    exE_2_settings_struct();
	    printf("\n*****************************exE_3_wear_aware_counter****************************************************\n");
	    exE_3_wear_aware_counter();
	    printf("\n*****************************exE_4_as_storage****************************************************\n");
	    exE_4_as_storage();
	}



	printf("\n****************************************************************************************\n");




if (music_activated==1){
	printf("\n***********************simple_audio****************************************\n");
	 if (SimpleAudio_Begin() != FM_OK) {
		 printf("SimpleAudio_Begin() != FM_OK \n");
	 }
	 /* بعد از Begin (Begin حالت را memset می‌کند و قلاب قبلی را پاک می‌کند). */
	 Music_InstallTaps();      /* هر دو قلاب: قبل از ولوم + بعد از ولوم */
}



	printf("\n**************************SimpleImage_Begin*************************************\n");


	SimpleImage_SetOutputSize(320, 240);
	SimpleImage_SetPresent(my_lcd_blit);
	SimpleImage_ShowTestPattern();   /* باید ۸ نوار عمودی تمیز باشد */

	HAL_Delay(3000);

	SimpleImage_Begin();



	if(music_activated == 0){
		printf("\n*************************SimpleVideo_Begin**************************************\n");
		SimpleVideo_SetPresent(my_lcd_blit_video);
		SimpleVideo_SetOutputSize(320, 240);

		if(SimpleVideo_Begin() != FM_OK){
			printf(" failed SimpleVideo_Begin ");
		}
		Music_InstallVideoHooks();   /* شروع/پایان ویدیو + دادهٔ فریم */

		SimpleVideo_Start();
		SimpleVideo_SetAutoPlay(1);

}

	printf("\n***************************************************************\n");
	printf("\n***************************************************************\n");





//	printf("\n**************************SD_examples_end*************************************\n");
//	SD_examples_end();HAL_Delay(100); // وقتی داریم آهنگ می خونیم نباید ببندیمیش
	printf("\n***************************************************************\n");



	printf("\n***************************************************************************************\n");
	printf("\n************************USB_present********************************\n");


	printf("\n***************************************************************************************\n");


	SimpleVideo_Stop();
	SimpleImage_Stop();
	SimpleAudio_Stop();

	Ex35_Sd_ShowToPc();         /* SD روی PC */

	printf("Sd For Pc \n");
	while(sd_pc_mcu==1){

		Ex35_UsbPoll();
		/* دستورهای UART در این حلقهٔ انتظار هم باید پردازش شوند:
		 * بعد از انتقال ریشه به PC، فرمان sdmcu باید بلافاصله کار کند
		 * تا TakeBack اجرا شود. هیچ کار فایل/اس‌دی داخل ISR نیست. */
		CmdMusic_ProcessPending();
//		HAL_Delay(1);
//		HAL_Delay(1000);
	}
	/* بعد از Eject در ویندوز (فرمان UART): */
	Ex35_Sd_TakeBack();



	uint32_t last = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

//	  //for test_audio_player.h
//	  if (test_audio_player_is_running())
//	      {
//	          FM_Result audio_poll_result;
//
//	          audio_poll_result = test_audio_player_poll();
//
//	          if (audio_poll_result != FM_OK &&
//	              audio_poll_result != FM_ERR_NOT_READY)
//	          {
//	              printf(
//	                  "Audio poll failed: %s (%d)\n",
//	                  File_GetErrorString(audio_poll_result),
//	                  (int)audio_poll_result);
//	          }
//	      }


	  LcdSpiDma_Poll();

	  /*----------------------------------------------------------*/
	  /* دستورهای start/stop موزیک که از ISR یوآرت فقط پرچم گذاشته‌اند،
	   * اینجا (در حلقهٔ اصلی، نه در وقفه) اجرا می‌شوند. */
	  if (music_cmd != 0u) {
		  uint8_t c = music_cmd;
		  music_cmd = 0u;
		  if (c == 1u) {
			  SimpleVideo_Stop();                 /* I2S را آزاد می‌کند */
			  if (SimpleAudio_Begin() != FM_OK) {
				  /* دابل-start: Begin وقتی پخش فعال است BUSY می‌دهد.
				   * به جای پیام خطا، پخش را تمیز متوقف و دوباره
				   * راه‌اندازی می‌کنیم (رفتار قابل پیش‌بینی). */
				  SimpleAudio_Stop();
				  if (SimpleAudio_Begin() != FM_OK) {
					  printf("SimpleAudio_Begin() failed twice\n");
				  }
			  }
			  /* بعد از Begin: Begin کل حالت sa را memset می‌کند و قلابی که
			   * قبلش نصب شده باشد پاک می‌شود. */
			  Music_InstallTaps();  /* هر دو قلاب: قبل از ولوم + بعد از ولوم */
			  tap_peak = 0;
			  tap_calls = 0;
		  } else if (c == 2u) {
			  SimpleAudio_Stop();
		  }
	  }

	  /*----------------------------------------------------------*/
	  /* بقیهٔ دستورهای UART هم از ISR فقط رشتهٔ فرمان را کپی می‌کنند
	   * (code_cmd_music.h). اینجا در حلقهٔ اصلی اجرا می‌شوند تا هیچ
	   * کار فایل/اس‌دی/توقف I2S داخل وقفهٔ UART انجام نشود. */
	  CmdMusic_ProcessPending();

	  /* پمپ ضبط اتوماتیک (rectest): باز کردن/نوشتن/بستن فایل فقط اینجا،
	   * نه در قلاب صدا — پس هیچ هنگی در مسیر صدا نیست. */
	  Music_RecPump();

	  /*----------------------------------------------------------*/
	 /* هر دو Poll همیشه صدا زده می‌شوند. قبلاً SimpleAudio_Poll پشت
	  * music_activated==1 بود که هیچ‌جا ۱ نمی‌شد؛ بعد از دستور start،
	  * موزیک شروع می‌شد ولی هیچ‌کس ring آن را پر نمی‌کرد ⇒ underrun و
	  * توقف/هنگ. وقتی یکی از دو پخش‌کننده کار نمی‌کند Poll آن بلافاصله
	  * با NOT_READY برمی‌گردد؛ I2S هم از طریق SimpleI2S mux فقط دست
	  * یکی از آن‌هاست. */
	 SimpleAudio_Poll();               // نگه‌داری صدا — حتماً
//	 // ضبط از میکروفون (هر منبعی): data را به تابه بده:
//	 // SimpleAudio_RecStart("/rec", 16000, 1);
//	 // while (mic has data) SimpleAudio_RecWrite(buf, frames);
//	 // SimpleAudio_RecStop();  // -> /rec/rec_000001.wav

	  /*----------------------------------------------------------*/
	 SimpleImage_Poll();
	  /*----------------------------------------------------------*/
	 SimpleVideo_Poll();
//	  SimpleVideo_Pause();
//	  SimpleVideo_SeekBy(+10.0);
//	  SimpleVideo_Next();
//	  SimpleVideo_SaveFrame("/snap.bmp");   /* فریم فعلی */
	  /*----------------------------------------------------------*/
	  /*----------------------------------------------------------*/
	  /*----------------------------------------------------------*/


	  if((HAL_GetTick() - last) >= 2000u){   /* wrap-safe */

		  HAL_GPIO_TogglePin(led_GPIO_Port, led_Pin);
		  /* UseHwJpeg هر ۲ ثانیه حذف شد: یک بار در راه‌اندازی فعال شده و
		   * تکرارش فقط لاگ گمراه‌کنندهٔ «img=0 vid=0» (=FM_OK) می‌داد. */

		  /* بلندی زندهٔ صدا: تابهٔ PCM بعد از ولوم، یعنی دقیقاً همان
		   * چیزی که به I2S می‌رود. اگر این عدد وسط پخش افت‌وخیز شدید
		   * دارد ولی track peak (لاگ start_current) بالاست، مشکل بعد
		   * از بافر است (I2S/تقویت‌کننده)؛ اگر خودش پایین و متغیر است،
		   * فایل/دیکد همین‌قدر آرام است. */
		  if (tap_calls != 0u) {
			  printf("audio live: peak=%d taps=%lu\n",
			         (int)tap_peak, (unsigned long)tap_calls);
			  tap_peak = 0;
			  tap_calls = 0;
		  }

		  last = HAL_GetTick();
	  }

	  /* ↓ بلوک کاربر (sd_pc_mcu / QSPI erase / CmdMusic pending) —
	   * به درخواست صریح کاربر؛ حذف یا جابه‌جا نکنید. */
	  Ex35_UsbPoll();


	  if(viedo_start==1){
		  viedo_start = 2;

	  		SimpleAudio_Stop();
	  		SimpleVideo_SetPresent(my_lcd_blit_video);
	  	    SimpleVideo_SetOutputSize(320, 240);
	  		/* همان نرمالایزر موزیک (tap1) روی صدای ویدیو:
	  		 * بلندی هم‌تراز موزیک، بدون کلیپ. */
	  		SimpleVideo_SetPrePcmTap(my_audio_pre_tap, NULL);
	    	printf("SimpleVideo_Begin \n");
			if(SimpleVideo_Begin() != FM_OK){
				printf(" failed SimpleVideo_Begin ");
			}
			Music_InstallVideoHooks();   /* شروع/پایان ویدیو + دادهٔ فریم */
			SimpleVideo_Start();
			SimpleVideo_SetAutoPlay(1);
	  }

	  /* vstop: دستور UART فقط پرچم viedo_start=3 می‌گذارد (از ISR).
	   * اینجا ویدیو را واقعاً متوقف و I2S را آزاد می‌کنیم. */
	  if(viedo_start==3){
		  viedo_start = 0;
		  printf("SimpleVideo_Stop (vstop) \n");
		  SimpleVideo_Stop();
	  }

	  if(qspi_erase == 1){
		  qspi_erase = 2;
		  printf("QSPI_examples_format \n");
//		  QSPI_examples_format();
		  Ex33_SpiFlash_EraseAll();
	  }

	  if(sd_pc_mcu==1){
			SimpleVideo_Stop();
			SimpleImage_Stop();
			SimpleAudio_Stop();

			Ex35_Sd_ShowToPc();         /* SD روی PC */

			printf("Sd For Pc \n");
			while(sd_pc_mcu==1){

				Ex35_UsbPoll();
				/* دستورهای UART در این حلقهٔ انتظار هم باید پردازش شوند:
				 * بعد از انتقال ریشه به PC، فرمان sdmcu باید بلافاصله کار کند
				 * تا TakeBack اجرا شود. هیچ کار فایل/اس‌دی داخل ISR نیست. */
				CmdMusic_ProcessPending();
		//		HAL_Delay(1);
		//		HAL_Delay(1000);
			}
			sd_pc_mcu = 0;
			/* بعد از Eject در ویندوز (فرمان UART): */
			Ex35_Sd_TakeBack();

	  	  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0xc0000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;


  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  /* ---------------------------------------------------------------- */
  /* ناحیهٔ ۳ — D2 SRAM (0x30000000) بدون کش                          */
  /* ---------------------------------------------------------------- */
  /* تمام بافرهای `__attribute__((section(\".dma_buffer\")))` روی این   */
  /* برد در D2 SRAM می‌نشینند (لاگ SimpleAudio: dma buf=0x3001a020).   */
  /* بدون این ناحیه، MPU آن‌ها را با پیش‌فرضِ cacheable می‌گذارد و حتی  */
  /* با FM_Cache_CleanForDma هم رینگ‌های I2S و بافر bounce در لحظه‌های */
  /* تحویل به DMA می‌توانند stale/خش بدهند؛ دقیقاً حالت «i2s_starve=0   */
  /* ولی صدا بریده‌بریده». دستور زیر D2 را کاملاً non-cacheable و        */
  /* non-bufferable می‌کند.                                            */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x30000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
