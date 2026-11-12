/*
 * simple_audio.h — SimpleAudio: پخش موزیک، دادهٔ صدا و ضبط در یک فایل
 *
 * این header را یک‌بار و فقط در main.c include کنید. با SimpleVideo
 * (simple_video.h) هم‌زمان می‌تواند باشد: هر دو از همان I2S استفاده می‌کنند و
 * هر کدام شروع شود دیگری را متوقف می‌کند.
 *
 * ── پخش ────────────────────────────────────────────────────────────────
 *   SimpleAudio_Begin()               اسکن پوشه و شروع پخش اولین فایل
 *   SimpleAudio_Poll()                داخل while(1) — همهٔ کار اینجاست
 *   SimpleAudio_Stop()                توقف کامل
 *   SimpleAudio_Start()               ادامه اگر Pause، شروع اگر متوقف
 *   SimpleAudio_Play("/music/a.mp3")  پخش یک فایل مشخص
 *   SimpleAudio_Next() / Prev()       ترک بعدی / قبلی
 *   SimpleAudio_Pause() / Resume() / TogglePause()
 *   SimpleAudio_SeekTo(30.0)          پرش به ثانیهٔ ۳۰
 *   SimpleAudio_SeekBy(-10.0)         ده ثانیه عقب (مثبت = جلو)
 *   SimpleAudio_SetVolume(80)         ولوم ۰ تا ۲۰۰ (۱۰۰ = بدون تغییر)
 *   SimpleAudio_GetVolume()
 *   SimpleAudio_SetPlayMode(mode)     آخر ترک: توقف / بعدی / تکرار یکی / تکرار همه
 *   SimpleAudio_GetPlayMode()
 *   SimpleAudio_IsRunning() / IsPaused()
 *   SimpleAudio_Info(&info)           نام، کدک، نرخ، طول، موقعیت، ولوم
 *
 * ── صدا کجا برود؟ ───────────────────────────────────────────────────────
 *
 *   فایل → دیکد → [کالبک داده] → ولوم → [کالبک مانیتور] → I2S
 *
 *   SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER)    فقط بلندگو
 *   SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK)   فقط کالبک شما (I2S ساکت)
 *   SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH)       هر دو (پیش‌فرض)
 *
 *   SimpleAudio_SetDataCallback(fn, ctx)      دادهٔ دیکدشده، قبل از ولوم؛
 *                                             هر تغییری بدهید شنیده می‌شود
 *   SimpleAudio_SetMonitorCallback(fn, ctx)   بعد از ولوم: دقیقاً همان که
 *                                             به بلندگو می‌رود (VU، ضبط)
 *   SimpleAudio_SetTrackStartCallback(fn, ctx) / SetTrackEndCallback(fn, ctx)
 *
 *   همهٔ این تنظیم‌ها (خروجی، کالبک‌ها، ولوم، حالت پخش) ماندگارند: یک‌بار،
 *   قبل یا بعد از Begin؛ با Stop و Begin دوباره پاک نمی‌شوند.
 *
 *   static void my_data(void *ctx, int16_t *pcm, size_t samples,
 *                       uint32_t rate, uint16_t channels)
 *   {
 *       // samples = تعداد int16 (استریو: L,R,L,R). فقط کار سبک.
 *   }
 *   SimpleAudio_SetDataCallback(my_data, NULL);
 *
 * ── خواندن مستقیم (بدون کالبک) ─────────────────────────────────────────
 *   SimpleAudio_DataRead(buf, frames, &got)      استریو (L,R)
 *   SimpleAudio_DataReadMono(buf, n, &got)       مونو
 *   این دو از همان دیکدرِ در حال پخش می‌خوانند: آنچه بخوانید دیگر پخش
 *   نمی‌شود. برای دادهٔ هم‌زمان با پخش، کالبک بالا را استفاده کنید.
 *
 * ── ضبط WAV ─────────────────────────────────────────────────────────────
 *   SimpleAudio_RecStart("/rec", rate, channels)   یا مسیر کامل یک .wav
 *   SimpleAudio_RecWrite(pcm, frames)
 *   SimpleAudio_RecStop()
 *   SimpleAudio_RecPath() / RecActive()
 *
 * نمونه:
 *
 *   #include "simple_audio.h"
 *
 *   SdCard_Begin();                       // کارت باید مونت باشد
 *   SimpleAudio_SetVolume(80);
 *   SimpleAudio_Begin();                  // پوشهٔ /music
 *   while (1)
 *   {
 *       SimpleAudio_Poll();
 *       if (Key_Pressed(KEY_NEXT)) SimpleAudio_Next();
 *   }
 *
 * بافر DMA: 2 * SIMPLE_AUDIO_HALF_SAMPLES * 2 بایت (پیش‌فرض حدود 128KB) —
 * سکشن .dma_buffer را در linker به RAM قابل دسترسی DMA (AXI-SRAM/
 * SRAM1/2) بگذارید.
 *
 * تنظیمات (پیش از include یا در preprocessor کل پروژه):
 *   SIMPLE_AUDIO_FOLDER           پوشهٔ لیست پخش (پیش‌فرض "/music")
 *   SIMPLE_AUDIO_HALF_SAMPLES     N (پیش‌فرض 32766؛ زوج و 4-byte-aligned)
 *   SIMPLE_AUDIO_LOG_STATS        1/0 چاپ آمار refill (پیش‌فرض 1)
 *   SIMPLE_AUDIO_STATS_EVERY      هر چند refill (پیش‌فرض 64)
 *   SIMPLE_AUDIO_MAX_RESTARTS     حد خودکارِ restart بعد از underrun
 *   SIMPLE_AUDIO_I2S_REC          1/0 فعال‌کردن RX از I2S1 با DMA
 *                                 (mic روی خط I2S؛ پیش‌فرض 0 = داده را
 *                                 خودتان با RecWrite بدهید)
 */

#ifndef SIMPLE_AUDIO_H_
#define SIMPLE_AUDIO_H_

#include <stddef.h>
#include "fm_board_config.h"
#include "fm_cache.h"          /* نگهداری کش — بی‌اثر اگر کش خاموش باشد */
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "i2s.h"
#include "simple_i2s_mux.h"
#include "simple_idle_mux.h"

#include "fm_log.h"
#include "file_manager.h"
#include "fm_audio.h"

/* log مراحل تعویض ترک را هم ببینیم (کجا دقیقاً می‌ماند؟). با define
 * نگه‌داشتن، هزینهٔ صفر دارد و برای بقیهٔ مصرف‌کننده‌های audio_dma_player.h
 * فعال نمی‌شود. */
#define AUDIO_DMA_DEBUG_LOG(...) FM_Log("AudioDma: " __VA_ARGS__)

#include "audio_dma_player.h"

/* ------------------------------------------------------------------------ */
/* تنظیمات                                                                  */
/* ------------------------------------------------------------------------ */

#ifndef SIMPLE_AUDIO_FOLDER
#define SIMPLE_AUDIO_FOLDER "/music"
#endif

/* سقف سخت HAL: HAL_I2S_Transmit_DMA عدد را uint16_t می‌گیرد => 2N <= 65535.
 * N must be even so half 1 starts 4-byte aligned (N=32767 puts half 1
 * at byte 65534, which is only 2-byte aligned — SDMMC IDMA on H7 can
 * hang there). 32766 ≈ 341 ms/half at 48 kHz stereo. */
#ifndef SIMPLE_AUDIO_HALF_SAMPLES
#define SIMPLE_AUDIO_HALF_SAMPLES (32766u)
#endif

#if (2u * SIMPLE_AUDIO_HALF_SAMPLES) > UINT16_MAX
#error "SIMPLE_AUDIO_HALF_SAMPLES too big: HAL_I2S_Transmit_DMA takes a uint16_t count (max 32767 samples per half)"
#endif
#if (2u * SIMPLE_AUDIO_HALF_SAMPLES) > SIMPLE_I2S_RING_SAMPLES
#error "SIMPLE_AUDIO_HALF_SAMPLES does not fit the shared simple_i2s_ring"
#endif

#ifndef SIMPLE_AUDIO_LOG_STATS
#define SIMPLE_AUDIO_LOG_STATS 1
#endif
#ifndef SIMPLE_AUDIO_STATS_EVERY
#define SIMPLE_AUDIO_STATS_EVERY 64u
#endif
#ifndef SIMPLE_AUDIO_MAX_RESTARTS
#define SIMPLE_AUDIO_MAX_RESTARTS 3u
#endif

/* فقط اگر SCB_EnableDCache() را در main فعال کرده باشید 1 کنید. */
/*
 * RX از I2S1 با DMA: اگر میکروفون روی خط I2S است و CubeMX RX+DMA را
 * فعال کرده، 1 کنید؛ callbackهای RX خودکار داده را به «تابهٔ ضبط»
 * منتقل می‌کنند. در غیر این صورت 0 بگذارید و داده را دستی با
 * SimpleAudio_RecWrite() بدهید (منبع: هر چه باشد).
 *
 * نکته: RX و TX یک I2S روی یک rate مشترکند؛ برای ضبط در rate متفاوت از
 * پخش، اول SimpleAudio_Stop() بزنید.
 */
/* پخش از کش فلش. ۱ = SimpleAudio_Play فایل را از راه FM_MediaCache
 * می‌گذراند. خاموش‌بودن کش بی‌خطر است.
 * راه‌اندازی: examples/play_from_flash.h */
#ifndef SIMPLE_AUDIO_USE_MEDIACACHE
#define SIMPLE_AUDIO_USE_MEDIACACHE 0
#endif
#if SIMPLE_AUDIO_USE_MEDIACACHE
#include "fm_mediacache.h"
#endif

#ifndef SIMPLE_AUDIO_I2S_REC
#define SIMPLE_AUDIO_I2S_REC 0
#endif
#ifndef SIMPLE_AUDIO_RX_HALF_SAMPLES
#define SIMPLE_AUDIO_RX_HALF_SAMPLES (4096u)
#endif

/* ------------------------------------------------------------------------ */
/* بخش برد (BOARD) — فقط اگر I2S دیگری دارید، این بلوک را عوض کنید        */
/* ------------------------------------------------------------------------ */

extern I2S_HandleTypeDef hi2s1; /* توسط CubeMX در i2s.c */

static FM_Bool simple_audio_rate_supported(uint32_t rate)
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
    case 192000u:
        return FM_TRUE;
    default:
        return FM_FALSE;
    }
}

/* شروع/بازشروع DMA پخش برای rate جدید. فقط وقتی صدا زده می‌شود که DMA
 * متوقف باشد (ابتدای پخش، تعویض ترک، seek). */
static FM_Result simple_audio_board_start(void *ctx, int16_t *samples,
                                          size_t count, uint32_t rate,
                                          uint16_t channels)
{
    I2S_HandleTypeDef *hi2s = (I2S_HandleTypeDef *)ctx;

    /* ابتدا همهٔ ورودی‌ها را چاپ کن تا خطا «قابل تشخیص» باشد */
    FM_Log("SimpleAudio: board_start rate=%lu Hz ch=%u count=%lu "
           "fmt=%d hi2s=%p\n",
           (unsigned long)rate, (unsigned)channels,
           (unsigned long)count,
           (int)(hi2s != NULL ? (int)hi2s->Init.DataFormat : -1),
           (void *)hi2s);

    if (hi2s == NULL || samples == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (count == 0u || (count & 1u) != 0u)
    {
        FM_Log("SimpleAudio: bad sample count %lu (odd or zero)\n",
               (unsigned long)count);
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (channels != 2u)
    {
        FM_Log("SimpleAudio: I2S sink is stereo — got %u channels\n",
               (unsigned)channels);
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!simple_audio_rate_supported(rate))
    {
        FM_Log("SimpleAudio: unsupported sample rate %lu Hz\n",
               (unsigned long)rate);
        return FM_ERR_UNSUPPORTED;
    }
    if (hi2s->Init.DataFormat != I2S_DATAFORMAT_16B)
    {
        FM_Log("SimpleAudio: CubeMX I2S DataFormat must be 16-bit "
               "(got %d)\n", (int)hi2s->Init.DataFormat);
        return FM_ERR_UNSUPPORTED;
    }

    if (hi2s->Init.AudioFreq != rate)
    {
        if (HAL_I2S_DeInit(hi2s) != HAL_OK)
        {
            return FM_ERR_IO;
        }
        hi2s->Init.AudioFreq = rate;
        if (HAL_I2S_Init(hi2s) != HAL_OK)
        {
            return FM_ERR_IO;
        }
    }
    else if (hi2s->State != HAL_I2S_STATE_READY)
    {
        /* Seek/Next can race auto-restart: I2S may already be BUSY.
         * Stop first so Transmit_DMA cannot fail with HAL_ERROR (-6). */
        (void)HAL_I2S_DMAStop(hi2s);
    }

    if (HAL_I2S_Transmit_DMA(hi2s, (uint16_t *)samples,
                             (uint16_t)count) != HAL_OK)
    {
        return FM_ERR_IO;
    }

    /*
     * مدرک عیب‌یابی: آیا اصلاً داده در بافر است؟ + آیا HAL در حالت
     * BUSY (DMA فعال) قرار گرفته؟ max|s| صفر = بافر سکوت است (مشکل
     * در decode/fill)؛ بزرگ = داده هست و مشکل در مسیر I2S/callback.
     */
    {
        int32_t maxabs = 0;
        size_t k;

        for (k = 0u; k < count; k++)
        {
            int32_t v = (int32_t)samples[k];

            if (v < 0)
            {
                v = -v;
            }
            if (v > maxabs)
            {
                maxabs = v;
            }
        }
        FM_Log("SimpleAudio: buf check: max|s|=%ld, i2s state=%d\n",
               (long)maxabs, (int)hi2s->State);
    }
    FM_Log("SimpleAudio: I2S start rate=%lu Hz ch=%u N=%lu\n",
           (unsigned long)rate, (unsigned)channels, (unsigned long)count);
    return FM_OK;
}

static FM_Result simple_audio_board_stop(void *ctx)
{
    I2S_HandleTypeDef *hi2s = (I2S_HandleTypeDef *)ctx;

    if (hi2s == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* Already idle: HAL_I2S_DMAStop() errors on some Cube versions when
     * called twice, and Next/Prev stop from the UART ISR then Poll
     * starts the new track. Treat READY as success. */
    if (hi2s->State == HAL_I2S_STATE_READY)
    {
        return FM_OK;
    }
    if (HAL_I2S_DMAStop(hi2s) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

/* همیشه ثبت می‌شود. اگر D-Cache خاموش باشد خودِ FM_Cache_CleanForDma
 * بی‌اثر است، پس نیازی به ماکروی روشن/خاموش نیست — و مهم‌تر، حالتی
 * وجود ندارد که کش روشن باشد و این صدا زده نشود. آن حالت دقیقاً همان
 * چیزی بود که به صدا نویز می‌داد. */
static void simple_audio_clean_cache(void *ctx, const void *p, size_t bytes)
{
    (void)ctx;
    FM_Cache_CleanForDma(p, bytes);
}

#if SIMPLE_AUDIO_I2S_REC
/* بافر RX — 2 * RXN * 2 بایت (پیش‌فرض 16KB). سکشن .dma_buffer. */
__attribute__((section(".dma_buffer"), aligned(32)))
static int16_t sa_rx_buf[2u * SIMPLE_AUDIO_RX_HALF_SAMPLES];
static volatile uint8_t sa_rx_free[2];

/* شروع DMA دریافت (RX) — فقط وقتی DMA پخش متوقف است یا rate یکی است. */
static FM_Result simple_audio_board_rx_start(uint32_t rate,
                                             uint16_t channels)
{
    if (channels != 2u || !simple_audio_rate_supported(rate))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (hi2s1.Init.AudioFreq != rate)
    {
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
    if (HAL_I2S_Receive_DMA(&hi2s1, (uint16_t *)sa_rx_buf,
                            (uint16_t)(2u * SIMPLE_AUDIO_RX_HALF_SAMPLES)) !=
        HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result simple_audio_board_rx_stop(void)
{
    return (HAL_I2S_DMAStop(&hi2s1) == HAL_OK) ? FM_OK : FM_ERR_IO;
}
#endif

static const AudioDma_Port simple_audio_port =
{
    simple_audio_board_start,
    simple_audio_board_stop,
    simple_audio_clean_cache
};

/* ------------------------------------------------------------------------ */
/* وضعیت                                                                    */
/* ------------------------------------------------------------------------ */

/* بافر پخش — از حلقهٔ I2S مشترک simple_i2s_ring استفاده می‌کند (همان
 * بافری که SimpleVideo برای صوت ویدیو به‌کار می‌برد). چون فقط یکی از
 * دو پخش‌کننده با SimpleI2S_Take صاحب hi2s1 است، این تداخل ندارد و
 * بافر ۳۲KB ویدیو را اضافه نگه نمی‌داریم. پیاده‌سازی آن در
 * simple_i2s_mux.h است. */
#define simple_audio_dma_buf simple_i2s_ring

typedef struct
{
    AudioDma dma;        /* موتور پخش (audio_dma_player.h)          */

    uint32_t restart_count;   /* underrun‌های خودکارِ restart    */

    /* آمار refill (بدون اسپم کنسول) */
    uint32_t refill_count;
    uint32_t refill_min_ms;
    uint32_t refill_max_ms;
    uint64_t refill_sum_ms;

    /* «تابهٔ ضبط»: داده می‌دهی، خودش به آدرس ذخیره می‌کند */
    FM_AudioFileWriter rec;
    FM_Bool rec_active;
    char rec_path[FM_PATH_MAX];
    uint32_t rec_rate;
    uint16_t rec_channels;
    uint64_t rec_frames;

    /* Track-change commands issued from a UART/key ISR must not open
     * files or fill DMA halves in that ISR: a 64 KB WAV File_Read from
     * inside HAL_UARTEx_RxEventCallback waits for SDMMC completion that
     * cannot preempt the UART IRQ, so the board hangs on MP3↔WAV. The
     * ISR only stops I2S and records the request; Poll() does the I/O. */
    volatile uint8_t pending_cmd;
    char pending_path[FM_PATH_MAX];
    double pending_seek;
    volatile uint8_t paused;
    volatile uint8_t pause_need_restart;
    double pause_playhead;
} SimpleAudioState;

#define SA_CMD_NONE    0u
#define SA_CMD_NEXT    1u
#define SA_CMD_PREV    2u
#define SA_CMD_PLAY    3u
#define SA_CMD_SEEK    4u
#define SA_CMD_RESUME  5u

static SimpleAudioState sa;

static void simple_audio_i2s_half(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        AudioDma_HalfCallback(&sa.dma);
    }
}

static void simple_audio_i2s_full(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        AudioDma_FullCallback(&sa.dma);
    }
}

static void simple_audio_i2s_err(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1 && sa.dma.running)
    {
        sa.dma.underrun = 1u;
    }
}

/* عیب‌یابی: چند refill اولیه را همان لحظه چاپ کند (callbackهای I2S
 * واقعاً می‌آیند؟) — جدا از آمارِ هر-۶۴-tایی. */
static volatile uint8_t sa_refill_log_left = 0u;

/* پیش‌اعلان (Begin/mux قبل از تعریف اصلی از این‌ها استفاده می‌کنند) */
FM_Result SimpleAudio_Stop(void);
FM_Result SimpleAudio_Poll(void);
FM_Bool   SimpleAudio_IsRunning(void);
FM_Result SimpleAudio_RecWrite(const int16_t *pcm, size_t frames);

/* Keep I2S alive while PNG/JPEG decode blocks the super-loop. */
static void simple_audio_idle(void)
{
    if (sa.dma.running)
    {
        (void)SimpleAudio_Poll();
    }
}

/* Optional: pause I2S around a *long* blocking call. SimpleImage no
 * longer uses this — DMAPause/Resume clicks the speaker on each photo. */
static uint8_t sa_busy_held;

__attribute__((unused))
static void simple_audio_busy_enter(void)
{
    if (sa_busy_held != 0u || sa.paused != 0u || !sa.dma.running)
    {
        return;
    }
    if (hi2s1.State != HAL_I2S_STATE_READY &&
        HAL_I2S_DMAPause(&hi2s1) == HAL_OK)
    {
        sa.pause_need_restart = 0u;
        sa.paused = 1u;
        sa_busy_held = 1u;
    }
}

__attribute__((unused))
static void simple_audio_busy_leave(void)
{
    if (sa_busy_held == 0u)
    {
        return;
    }
    sa_busy_held = 0u;
    if (HAL_I2S_DMAResume(&hi2s1) == HAL_OK)
    {
        sa.paused = 0u;
        return;
    }
    sa.pause_need_restart = 1u;
    sa.pending_cmd = SA_CMD_RESUME;
}

/* ------------------------------------------------------------------------ */
/* ساختار Info — «اسم موزیک و تنظیم صدا در دست شما»                        */
/* ------------------------------------------------------------------------ */

typedef struct
{
    char     name[FM_PATH_MAX]; /* آدرس فایل در حال پخش            */
    char     codec[8];          /* "MP3" / "WAV" / ...               */
    uint32_t sample_rate;       /* فرکانس نمونه‌برداری فایل         */
    uint16_t channels;          /* کانال‌های اصلی فایل (1 یا 2)     */
    uint16_t bits;              /* همیشه 16 (خروجی decode)          */
    double   duration;          /* طول کل (ثانیه؛ 0 = ناشناخته/VBR) */
    double   position;          /* موقعیت فعلی (ثانیه)              */
    uint8_t  volume_pct;        /* ولوم فعلی (0..200؛ 100 = بی‌تغییر) */
    uint32_t playlist_count;    /* تعداد فایل‌های پوشه             */
    uint32_t playlist_index;    /* شاخص ترک فعلی در لیست            */
    uint8_t  paused;            /* 1 = Pause (صدا قطع، ترک باز)     */
    uint8_t  running;           /* 1 = در حال پخش یا Pause          */
    FM_PlayMode    play_mode;   /* رفتار آخر ترک                    */
    FM_AudioOutput output;      /* بلندگو / کالبک / هر دو           */
} SimpleAudioInfo;

/* ------------------------------------------------------------------------ */
/* صدا کجا برود؟ خروجی و کالبک‌ها                                           */
/* ------------------------------------------------------------------------ */
/*
 *   فایل → دیکد → [کالبک داده] → ولوم → [کالبک مانیتور] → I2S
 *
 * SimpleAudio_SetOutput() تنها سوئیچ مسیر است:
 *
 *   FM_AUDIO_OUT_SPEAKER    فقط بلندگو؛ کالبک‌ها صدا زده نمی‌شوند
 *   FM_AUDIO_OUT_CALLBACK   فقط کالبک‌ها؛ I2S سکوت پخش می‌کند. انتقال DMA
 *                           ادامه دارد، پس داده با همان سرعت واقعی پخش
 *                           به دست شما می‌رسد
 *   FM_AUDIO_OUT_BOTH       کالبک‌ها و بعد بلندگو (پیش‌فرض). اگر کالبکی
 *                           نصب نشده باشد همان «فقط بلندگو» است
 *
 * کالبک داده برای پردازش است (افکت، اکولایزر، FFT)؛ بافر قابل نوشتن است.
 * کالبک مانیتور برای دیدن خروجی نهایی است (VU متر، ضبط هم‌زمان با پخش).
 *
 * هر دو روی مسیر پرکردن بافر DMA اجرا می‌شوند: فقط کپی یا محاسبهٔ سبک.
 * بازکردن و نوشتن فایل یا printf داخلشان باعث تق‌تق صدا می‌شود؛ داده را
 * در بافر خودتان کپی کنید و فایل را در حلقهٔ اصلی بنویسید.
 *
 * این تنظیم‌ها بیرون از حالت پخش نگه‌داری می‌شوند: قبل یا بعد از Begin
 * قابل‌تغییرند و با Stop()/Begin() از بین نمی‌روند.
 */
static AudioDma_Config sa_cfg;
static uint8_t sa_cfg_ready;
static uint8_t sa_volume_pct = 100u;

static AudioDma_Config *simple_audio_cfg(void)
{
    if (sa_cfg_ready == 0u)
    {
        AudioDma_ConfigDefaults(&sa_cfg);
        sa_cfg.force_stereo = FM_TRUE;   /* mono -> استریو، خودکار */
        sa_cfg_ready = 1u;
    }
    return &sa_cfg;
}

FM_Result SimpleAudio_SetOutput(FM_AudioOutput output)
{
    if ((unsigned)output > (unsigned)FM_AUDIO_OUT_BOTH)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    simple_audio_cfg()->output = output;
    AudioDma_SetOutput(&sa.dma, output);
    return FM_OK;
}

FM_AudioOutput SimpleAudio_GetOutput(void)
{
    return simple_audio_cfg()->output;
}

/* دادهٔ دیکدشده، قبل از ولوم. NULL = برداشتن کالبک. */
void SimpleAudio_SetDataCallback(FM_AudioDataFn fn, void *ctx)
{
    AudioDma_Config *cfg = simple_audio_cfg();

    cfg->data_ctx = ctx;
    cfg->data_cb = fn;
    AudioDma_SetDataCallback(&sa.dma, fn, ctx);
}

/* بعد از ولوم: همان نمونه‌هایی که به بلندگو می‌رود. NULL = برداشتن. */
void SimpleAudio_SetMonitorCallback(FM_AudioDataFn fn, void *ctx)
{
    AudioDma_Config *cfg = simple_audio_cfg();

    cfg->monitor_ctx = ctx;
    cfg->monitor_cb = fn;
    AudioDma_SetMonitorCallback(&sa.dma, fn, ctx);
}

typedef AudioDma_TrackFn SimpleAudio_TrackFn;

/* شروع و پایان هر ترک: مسیر فایل و مشخصات آن (FM_AudioInfo). از حلقهٔ
 * اصلی صدا زده می‌شوند، نه از وقفه. اگر هنگام نصبِ «شروع» ترکی در حال
 * پخش باشد، همان لحظه گزارش می‌شود. */
void SimpleAudio_SetTrackStartCallback(SimpleAudio_TrackFn fn, void *ctx)
{
    AudioDma_Config *cfg = simple_audio_cfg();

    cfg->track_start_cb = fn;
    cfg->track_start_ctx = ctx;
    AudioDma_SetTrackStartCallback(&sa.dma, fn, ctx);
}

void SimpleAudio_SetTrackEndCallback(SimpleAudio_TrackFn fn, void *ctx)
{
    AudioDma_Config *cfg = simple_audio_cfg();

    cfg->track_end_cb = fn;
    cfg->track_end_ctx = ctx;
    AudioDma_SetTrackEndCallback(&sa.dma, fn, ctx);
}

/* ------------------------------------------------------------------------ */
/* پخش                                                                        */
/* ------------------------------------------------------------------------ */

/*
 * شروع: اسکن پوشه، پخش اولین فایل قابل‌پخش، پرکردن بافرها، شروع DMA.
 * WAV/MP3 و mono/stereo — همه خودکار؛ فایل‌های خراب/ناپشتیبانی رد می‌شوند.
 * خروجی، کالبک‌ها، ولوم و حالت پخشِ تنظیم‌شده از همان اولین بافر اعمال
 * می‌شوند.
 */
FM_Result SimpleAudio_Begin(void)
{
    FM_Result r;

    if (sa.dma.running || sa.rec_active)
    {
        return FM_ERR_BUSY;
    }

    SimpleI2S_Take(simple_audio_i2s_half, simple_audio_i2s_full,
                   simple_audio_i2s_err, SimpleAudio_Stop);

    memset(&sa, 0, sizeof(sa));
    sa_refill_log_left = 3u;

    /* دیباگ: آدرس/آلایمن بافر DMA — اگر start با invalid-argument رد
     * شود، این خط نشان می‌دهد بافر کجا قرار گرفته است. */
    FM_Log("SimpleAudio: dma buf=%p (4B-aligned=%d) N=%lu folder=%s\n",
           (void *)simple_audio_dma_buf,
           (((uintptr_t)simple_audio_dma_buf & 0x3u) == 0u) ? 1 : 0,
           (unsigned long)SIMPLE_AUDIO_HALF_SAMPLES, SIMPLE_AUDIO_FOLDER);

    r = AudioDma_StartConfig(
        &sa.dma, SIMPLE_AUDIO_FOLDER, &simple_audio_port, &hi2s1,
        simple_audio_dma_buf, SIMPLE_AUDIO_HALF_SAMPLES, simple_audio_cfg());
    if (r != FM_OK)
    {
        FM_Log("SimpleAudio: start failed: %s (%d)\n",
               File_GetErrorString(r), (int)r);
        if (r == FM_ERR_NOT_FOUND)
        {
            FM_Log("SimpleAudio: no audio file in %s\n",
                   SIMPLE_AUDIO_FOLDER);
        }
        SimpleI2S_Drop(SimpleAudio_Stop);
        SimpleIdle_Remove(simple_audio_idle);
        return r;
    }

    FM_Log("SimpleAudio: playing %s (%s %lu Hz %uch) — playlist %lu\n",
           Player_ItemPath(&sa.dma.player, Player_Current(&sa.dma.player)),
           sa.dma.player.audio.info.codec,
           (unsigned long)sa.dma.sample_rate,
           (unsigned)sa.dma.player.audio.info.channels,
           (unsigned long)Player_Count(&sa.dma.player));
    SimpleIdle_Add(simple_audio_idle);
    /* Do not FM_SetBusyHooks: DMAPause/Resume clicks the speaker on every
     * photo. JPEG/PNG already call FM_Idle; SPI blit is shorter than one
     * I2S half-buffer. */
    return FM_OK;
}

/* Seconds currently sitting in the two DMA halves (decoder is this far
 * ahead of the speaker). 0 if the sink is not configured yet. */
static double simple_audio_buffered_seconds(void)
{
    if (sa.dma.sample_rate == 0u || sa.dma.channels == 0u)
    {
        return 0.0;
    }
    return ((double)SIMPLE_AUDIO_HALF_SAMPLES * 2.0) /
           ((double)sa.dma.sample_rate * (double)sa.dma.channels);
}

/* Playhead the listener hears. Decoder position includes the prefilled
 * DMA window; SeekBy must use this or -10 s jumps from 10.68 instead of
 * 10.00 (and stacked seeks from the UART ISR fight each other). */
static double simple_audio_playhead(void)
{
    double pos;
    double buf;

    if (sa.pending_cmd == SA_CMD_SEEK)
    {
        pos = sa.pending_seek;
        return (pos < 0.0) ? 0.0 : pos;
    }
    pos = Player_GetPosition(&sa.dma.player);
    buf = simple_audio_buffered_seconds();
    if (buf > 0.0 && pos >= buf)
    {
        pos -= buf;
    }
    else if (buf > 0.0)
    {
        pos = 0.0;
    }
    return pos;
}

/* Apply a Next/Prev/Play/Seek that was recorded from an ISR (or from
 * the super-loop). File I/O and both DMA-half prefills happen here,
 * never inside HAL_UARTEx_RxEventCallback. */
static FM_Result simple_audio_apply_pending(void)
{
    uint8_t cmd = sa.pending_cmd;
    FM_Result r;

    sa.pending_cmd = SA_CMD_NONE;
    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    if (cmd == SA_CMD_NONE)
    {
        return FM_OK;
    }

    if (cmd == SA_CMD_RESUME)
    {
        double want = sa.pause_playhead;

        if (want < 0.0)
        {
            want = 0.0;
        }
        r = Player_Seek(&sa.dma.player, want);
        if (r != FM_OK && r != FM_ERR_EOF)
        {
            FM_Log("SimpleAudio: resume seek failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            return r;
        }
        sa.dma.running = FM_FALSE;
        (void)simple_audio_board_stop(&hi2s1);
        r = audio_dma_start_current(&sa.dma);
        if (r == FM_OK)
        {
            FM_Log("SimpleAudio: resumed at %.2f s\n", want);
        }
        else
        {
            FM_Log("SimpleAudio: resume failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
        }
        return r;
    }

    if (cmd == SA_CMD_NEXT)
    {
        r = audio_dma_start_next(&sa.dma);
        if (r == FM_OK)
        {
            FM_Log("SimpleAudio: next -> %s\n",
                   Player_ItemPath(&sa.dma.player,
                                   Player_Current(&sa.dma.player)));
        }
        else
        {
            FM_Log("SimpleAudio: next failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
        }
        return r;
    }

    if (cmd == SA_CMD_PREV)
    {
        uint32_t count = Player_Count(&sa.dma.player);
        uint32_t idx;

        if (count == 0u)
        {
            FM_Log("SimpleAudio: prev: player is not running\n");
            return FM_ERR_NOT_READY;
        }
        idx = (Player_Current(&sa.dma.player) == 0u)
                  ? (count - 1u)
                  : (Player_Current(&sa.dma.player) - 1u);
        r = audio_dma_play_from(&sa.dma, idx);
        if (r == FM_OK)
        {
            FM_Log("SimpleAudio: prev -> %s\n",
                   Player_ItemPath(&sa.dma.player,
                                   Player_Current(&sa.dma.player)));
        }
        else
        {
            FM_Log("SimpleAudio: prev failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
        }
        return r;
    }

    if (cmd == SA_CMD_SEEK)
    {
        double want = sa.pending_seek;

        if (want < 0.0)
        {
            want = 0.0;
        }
        r = Player_Seek(&sa.dma.player, want);
        if (r == FM_ERR_EOF)
        {
            FM_Log("SimpleAudio: seek past end (mode %d) -> handle EOF\n", (int)sa.dma.play_mode);
            sa.dma.running = FM_FALSE;
            (void)simple_audio_board_stop(&hi2s1);
            r = audio_dma_handle_eof(&sa.dma);
            if (r == FM_OK && sa.dma.running)
            {
                FM_Log("SimpleAudio: -> %s\n",
                       Player_ItemPath(&sa.dma.player,
                                       Player_Current(&sa.dma.player)));
            }
            else if (r == FM_OK)
            {
                FM_Log("SimpleAudio: stopped (STOP/NEXT end)\n");
            }
            return r;
        }
        if (r != FM_OK)
        {
            FM_Log("SimpleAudio: seek failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            sa.dma.running = FM_FALSE;
            (void)simple_audio_board_stop(&hi2s1);
            r = audio_dma_start_current(&sa.dma);
            if (r != FM_OK)
            {
                FM_Log("SimpleAudio: seek restore failed: %s (%d)\n",
                       File_GetErrorString(r), (int)r);
            }
            return r;
        }
        sa.dma.running = FM_FALSE;
        (void)simple_audio_board_stop(&hi2s1);
        r = audio_dma_start_current(&sa.dma);
        if (r == FM_ERR_EOF)
        {
            FM_Log("SimpleAudio: seek past end (mode %d) -> handle EOF\n", (int)sa.dma.play_mode);
            r = audio_dma_handle_eof(&sa.dma);
            if (r == FM_OK && sa.dma.running)
            {
                FM_Log("SimpleAudio: -> %s\n",
                       Player_ItemPath(&sa.dma.player,
                                       Player_Current(&sa.dma.player)));
            }
            return r;
        }
        if (r == FM_OK)
        {
            FM_Log("SimpleAudio: seek -> %.2f s\n", want);
        }
        else
        {
            FM_Log("SimpleAudio: seek restart failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
        }
        return r;
    }

    if (cmd == SA_CMD_PLAY)
    {
        uint32_t i;
        uint32_t count;
        const char *path = sa.pending_path;

        if (!sa.dma.player_initialized)
        {
            FM_Log("SimpleAudio: play: player is not initialized "
                   "(Begin first)\n");
            return FM_ERR_NOT_READY;
        }

        count = Player_Count(&sa.dma.player);
        for (i = 0u; i < count; i++)
        {
            if (strcmp(Player_ItemPath(&sa.dma.player, i), path) == 0)
            {
                r = audio_dma_play_from(&sa.dma, i);
                if (r == FM_OK)
                {
                    FM_Log("SimpleAudio: play -> %s\n", path);
                }
                else
                {
                    FM_Log("SimpleAudio: play failed: %s (%d)\n",
                           File_GetErrorString(r), (int)r);
                }
                return r;
            }
        }

        r = Player_ScanFolder(&sa.dma.player, SIMPLE_AUDIO_FOLDER,
                              FM_PLAYER_KIND_AUDIO);
        if (r != FM_OK)
        {
            FM_Log("SimpleAudio: play: rescan failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            return r;
        }
        count = Player_Count(&sa.dma.player);
        for (i = 0u; i < count; i++)
        {
            if (strcmp(Player_ItemPath(&sa.dma.player, i), path) == 0)
            {
                r = audio_dma_play_from(&sa.dma, i);
                if (r == FM_OK)
                {
                    FM_Log("SimpleAudio: play -> %s\n", path);
                }
                else
                {
                    FM_Log("SimpleAudio: play failed: %s (%d)\n",
                           File_GetErrorString(r), (int)r);
                }
                return r;
            }
        }
        FM_Log("SimpleAudio: play: %s not in %s\n", path, SIMPLE_AUDIO_FOLDER);
        return FM_ERR_NOT_FOUND;
    }

    return FM_OK;
}

#if SIMPLE_AUDIO_I2S_REC
/* خالی‌کردن نیمه‌های RX مصرف‌شده به تابهٔ ضبط (خارج از IRQ). */
static void simple_audio_rx_drain(void)
{
    uint8_t half;

    for (half = 0u; half < 2u; half++)
    {
        if (sa_rx_free[half] != 0u)
        {
            int16_t *chunk = sa_rx_buf + (size_t)half *
                             (size_t)SIMPLE_AUDIO_RX_HALF_SAMPLES;
            size_t chunk_bytes = (size_t)SIMPLE_AUDIO_RX_HALF_SAMPLES *
                                 sizeof(int16_t);

            sa_rx_free[half] = 0u;
            /*
             * این نیمه را DMA پر کرده، نه CPU. بدون invalidate، CPU
             * نسخهٔ کهنهٔ کش را می‌خواند و ضبط سکوت یا صدای قدیمی
             * می‌شود. مسیر پخش clean دارد ولی مسیر ضبط جهت مخالف
             * است و invalidate می‌خواهد.
             *
             * با کش خاموش این فقط یک بریر است.
             */
            FM_Cache_InvalidateAfterDma(chunk, chunk_bytes);
            if (sa.rec_active)
            {
                /* RX_HALF channel samples = RX_HALF/2 stereo frames
                 * (the RX path records 16-bit stereo slots). */
                (void)SimpleAudio_RecWrite(
                    chunk, (size_t)SIMPLE_AUDIO_RX_HALF_SAMPLES / 2u);
            }
        }
    }
}
#endif

/*
 * نگه‌دارندهٔ صدا — داخل while(1) صدا بزنید:
 *   refill نیمه‌های مصرف‌شده، تعویض خودکار ترک در پایان،
 *   restart محدود بعد از underrun، آمار، و خالی‌کردن RX (اگر فعال).
 *
 * معمولاً لازم نیست مقدار برگشتی را بررسی کنید؛ خطاها خودکار مدیریت
 * می‌شوند و با FM_Log چاپ می‌شوند.
 */
FM_Result SimpleAudio_Poll(void)
{
    FM_Result r;

#if SIMPLE_AUDIO_I2S_REC
    simple_audio_rx_drain();
#endif

    if (sa.pending_cmd != SA_CMD_NONE)
    {
        r = simple_audio_apply_pending();
        if (r != FM_OK)
        {
            return r;
        }
        /* A freshly started track has both halves prefilled; skip the
         * refill pass this tick so we do not race the first DMA IRQ. */
        return FM_OK;
    }

    if (sa.paused)
    {
        uint8_t half;

        /* Keep I2S DMA running and feed silence. DMAPause/DMAStop hiss
         * when SPI TFT blits, and leave the DAC floating. */
        if (sa.dma.running)
        {
            if (sa.pause_need_restart == 0u)
            {
                audio_dma_silence_half(&sa.dma, 0u);
                audio_dma_silence_half(&sa.dma, 1u);
                sa.dma.refill[0] = sa.dma.refill[1] = 0u;
                sa.pause_need_restart = 1u;
            }
            else
            {
                for (half = 0u; half < 2u; half++)
                {
                    if (sa.dma.refill[half] != 0u)
                    {
                        sa.dma.refill[half] = 0u;
                        audio_dma_silence_half(&sa.dma, half);
                    }
                }
            }
        }
        return FM_OK;
    }

    if (!sa.dma.running)
    {
        return FM_ERR_NOT_READY;
    }

    {
        uint32_t started = HAL_GetTick();
        uint32_t elapsed;
        FM_Bool was_draining = (sa.dma.draining != 0u) ? FM_TRUE : FM_FALSE;
        FM_Bool had_refill =
            (sa.dma.refill[0] != 0u || sa.dma.refill[1] != 0u)
                ? FM_TRUE : FM_FALSE;

        r = AudioDma_Poll(&sa.dma);
        elapsed = HAL_GetTick() - started;

        /* UART Next/Seek sets running=FALSE while Poll is inside the
         * refill. Treat that as "command pending", not a crash: the old
         * auto-restart started I2S again, then apply_pending started it
         * a second time (HAL -6) and the player died. */
        if (sa.pending_cmd != SA_CMD_NONE)
        {
            return simple_audio_apply_pending();
        }
        if (r == FM_ERR_NOT_READY)
        {
            return r;
        }

#if SIMPLE_AUDIO_LOG_STATS
        if (had_refill && !was_draining)
        {
            if (sa_refill_log_left != 0u)
            {
                /* مدارک عیب‌یابی: callbackهای I2S می‌آیند یا نه؟ */
                FM_Log("SimpleAudio: first refill ok (%lu ms)\n",
                       (unsigned long)elapsed);
                sa_refill_log_left--;
            }
            if (elapsed < sa.refill_min_ms)
            {
                sa.refill_min_ms = elapsed;
            }
            if (elapsed > sa.refill_max_ms)
            {
                sa.refill_max_ms = elapsed;
            }
            sa.refill_sum_ms += (uint64_t)elapsed;
            sa.refill_count++;

            if (sa.refill_count >= SIMPLE_AUDIO_STATS_EVERY)
            {
                uint32_t avg = (uint32_t)(sa.refill_sum_ms /
                                          (uint64_t)sa.refill_count);
                uint32_t half_ms =
                    sa.dma.sample_rate != 0u && sa.dma.channels != 0u
                        ? (uint32_t)(
                              ((uint64_t)SIMPLE_AUDIO_HALF_SAMPLES * 1000u) /
                              ((uint64_t)sa.dma.sample_rate *
                               (uint64_t)sa.dma.channels))
                        : 0u;

                FM_Log("SimpleAudio refill (last %u): avg=%lu ms, "
                       "min=%lu, max=%lu, half=%lu ms\n",
                       (unsigned)sa.refill_count, (unsigned long)avg,
                       (unsigned long)sa.refill_min_ms,
                       (unsigned long)sa.refill_max_ms,
                       (unsigned long)half_ms);
                sa.refill_count = 0u;
                sa.refill_min_ms = 0xFFFFFFFFu;
                sa.refill_max_ms = 0u;
                sa.refill_sum_ms = 0u;
            }
        }
#endif

        if (r != FM_OK)
        {
            /*
             * موتور خودش را متوقف کرده است: یا underrun (یک refill از
             * مرز زمانی دیر آمد — FM_ERR_TIMEOUT) یا خطای refill
             * (خطای SD/read روی فایل). پیش از این فقط TIMEOUT مدیریت
             * می‌شد؛ خطاهای دیگر پلیر را به‌صورت «بی‌صدا» خاموش
             * می‌کردند (همان «گیرکردن روی آهنگ سوم» روی برد).
             *
             * حالا هر خطایی تا SIMPLE_AUDIO_MAX_RESTARTS بار ترک فعلی
             * را از اول restart می‌کند تا اجرا از کار نیفتد؛
             * audio_dma_play_from همچنین ترک‌هایی که باز/decode نمی‌شوند
             * را رد می‌کند، پس حتی یک فایل خراب به‌تنهایی پلیر را
             * نمی‌کُشد.
             */
            sa.restart_count++;
            FM_Log("SimpleAudio: playback error (%s, code %d) — "
                   "auto-restart %u/%u\n",
                   File_GetErrorString(r), (int)r,
                   (unsigned)sa.restart_count,
                   (unsigned)SIMPLE_AUDIO_MAX_RESTARTS);

            if (sa.restart_count <= SIMPLE_AUDIO_MAX_RESTARTS)
            {
                FM_Result r2 = audio_dma_play_from(
                    &sa.dma, Player_Current(&sa.dma.player));

                if (r2 == FM_OK)
                {
                    FM_Log("SimpleAudio: restarted -> %s\n",
                           Player_ItemPath(&sa.dma.player,
                                           Player_Current(&sa.dma.player)));
                    return FM_OK;
                }
                r = r2;
            }
            FM_Log("SimpleAudio: restarts exhausted — player stopped\n");
            return r;
        }
    }

    if (r == FM_OK)
    {
        sa.restart_count = 0u;
    }
    return r;
}

/* شروع یا ادامه: اگر Pause است ادامه می‌دهد، اگر متوقف است Begin می‌کند،
 * اگر در حال پخش است کاری نمی‌کند. همتای SimpleVideo_Start. */
FM_Result SimpleAudio_Resume(void);

FM_Result SimpleAudio_Start(void)
{
    if (sa.paused != 0u)
    {
        return SimpleAudio_Resume();
    }
    if (AudioDma_IsRunning(&sa.dma))
    {
        return FM_OK;
    }
    return SimpleAudio_Begin();
}

/* توقف کامل (DMA + لیست پخش). برای شروع دوباره: SimpleAudio_Begin() یا
 * SimpleAudio_Start(). تنظیم‌ها (خروجی، کالبک‌ها، ولوم) می‌مانند. */
FM_Result SimpleAudio_Stop(void)
{
    FM_Result r;

    sa.pending_cmd = SA_CMD_NONE;
    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    SimpleI2S_Drop(SimpleAudio_Stop);
    SimpleIdle_Remove(simple_audio_idle);
    r = AudioDma_Stop(&sa.dma);

    if (r == FM_OK)
    {
        FM_Log("SimpleAudio: stopped\n");
    }
    return r;
}

FM_Bool SimpleAudio_IsRunning(void)
{
    if (sa.paused)
    {
        return FM_TRUE;
    }
    return AudioDma_IsRunning(&sa.dma);
}

FM_Bool SimpleAudio_IsPaused(void)
{
    return (sa.paused != 0u) ? FM_TRUE : FM_FALSE;
}

/* UART-ISR safe: only a flag. Poll feeds silence into the still-running
 * I2S DMA so SPI TFT cannot glitch a paused peripheral. */
FM_Result SimpleAudio_Pause(void)
{
    if (sa.paused)
    {
        return FM_OK;
    }
    if (!sa.dma.running && sa.pending_cmd == SA_CMD_NONE)
    {
        return FM_ERR_NOT_READY;
    }
    sa.pause_playhead = simple_audio_playhead();
    sa.paused = 1u;
    sa.pause_need_restart = 0u;
    return FM_OK;
}

FM_Result SimpleAudio_Resume(void)
{
    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    return FM_OK;
}

FM_Result SimpleAudio_TogglePause(void)
{
    return sa.paused ? SimpleAudio_Resume() : SimpleAudio_Pause();
}

/* ⏭ ترک بعدی — فوری از نظر صدا (I2S قطع می‌شود)؛ بازکردن فایل و پر
 * کردن بافر در SimpleAudio_Poll انجام می‌شود تا از IRQ UART امن باشد. */
FM_Result SimpleAudio_Next(void)
{
    if (!sa.dma.running && !sa.paused && sa.pending_cmd == SA_CMD_NONE)
    {
        FM_Log("SimpleAudio: next: player is not running\n");
        return FM_ERR_NOT_READY;
    }
    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    if (sa.dma.running || hi2s1.State != HAL_I2S_STATE_READY)
    {
        sa.dma.running = FM_FALSE;
        if (simple_audio_board_stop(&hi2s1) != FM_OK)
        {
            FM_Log("SimpleAudio: next failed: I2S stop\n");
            return FM_ERR_IO;
        }
    }
    sa.pending_cmd = SA_CMD_NEXT;
    return FM_OK;
}

/* ⏮ ترک قبلی — فوری از نظر صدا؛ I/O در Poll. */
FM_Result SimpleAudio_Prev(void)
{
    if (!sa.dma.running && !sa.paused && sa.pending_cmd == SA_CMD_NONE)
    {
        FM_Log("SimpleAudio: prev: player is not running\n");
        return FM_ERR_NOT_READY;
    }
    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    if (sa.dma.running || hi2s1.State != HAL_I2S_STATE_READY)
    {
        sa.dma.running = FM_FALSE;
        if (simple_audio_board_stop(&hi2s1) != FM_OK)
        {
            FM_Log("SimpleAudio: prev failed: I2S stop\n");
            return FM_ERR_IO;
        }
    }
    sa.pending_cmd = SA_CMD_PREV;
    return FM_OK;
}

/*
 * پخش یک فایل مشخص (مثل "/music/023-....mp3"). فایل باید در درخت
 * پوشهٔ SIMPLE_AUDIO_FOLDER باشد؛ اگر در لیست فعلی نیست، پوشه دوباره
 * اسکن می‌شود. اگر پخش می‌کرد، فوری روی فایل جدید عوض می‌شود.
 */
FM_Result SimpleAudio_Play(const char *path)
{
    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!sa.dma.player_initialized)
    {
        FM_Log("SimpleAudio: play: player is not initialized "
               "(Begin first)\n");
        return FM_ERR_NOT_READY;
    }

#if SIMPLE_AUDIO_USE_MEDIACACHE
    /* اگر کش رسانه راه‌اندازی شده باشد، آهنگ را (یک‌بار) روی فلش سریع
     * می‌برد و از آنجا پخش می‌کند. اگر کش نباشد، همان مسیر اصلی
     * برمی‌گردد — پس این خط هرگز پخش را خراب نمی‌کند. */
    {
        static char sa_cached[FM_PATH_MAX];

        if (FM_MediaCache_Resolve(path, sa_cached,
                                  sizeof(sa_cached)) == FM_OK)
        {
            path = sa_cached;
        }
    }
#endif

    sa.paused = 0u;
    sa.pause_need_restart = 0u;
    if (sa.dma.running || hi2s1.State != HAL_I2S_STATE_READY)
    {
        sa.dma.running = FM_FALSE;
        if (simple_audio_board_stop(&hi2s1) != FM_OK)
        {
            FM_Log("SimpleAudio: play failed: I2S stop\n");
            return FM_ERR_IO;
        }
    }
    (void)strncpy(sa.pending_path, path, FM_PATH_MAX - 1u);
    sa.pending_path[FM_PATH_MAX - 1u] = '\0';
    sa.pending_cmd = SA_CMD_PLAY;
    return FM_OK;
}

FM_Result SimpleAudio_SeekTo(double seconds)
{
    if (!sa.dma.player_initialized)
    {
        FM_Log("SimpleAudio: seek: player is not running\n");
        return FM_ERR_NOT_READY;
    }
    if (seconds < 0.0)
    {
        seconds = 0.0;
    }
    /* Keep I2S running until Poll applies the byte-seek. */
    sa.pending_seek = seconds;
    sa.pending_cmd = SA_CMD_SEEK;
    return FM_OK;
}

/* ⏪/⏩ جلو یا عقب. پرش از انتهای ترک = آهنگ بعدی. */
FM_Result SimpleAudio_SeekBy(double seconds)
{
    double target = simple_audio_playhead() + seconds;
    double duration = Player_GetDuration(&sa.dma.player);
    FM_PlayMode m = sa.dma.play_mode;

    if (target < 0.0)
    {
        target = 0.0;
    }
    if (duration > 0.0 && target >= duration)
    {
        /* پایان ترک via seek — مثل EOF واقعی، بر اساس play mode */
        if (m == FM_PLAY_MODE_REPEAT_ONE)
        {
            return SimpleAudio_SeekTo(0.0);
        }
        if (m == FM_PLAY_MODE_STOP)
        {
            sa.paused = 0u;
            sa.pause_need_restart = 0u;
            if (sa.dma.running || hi2s1.State != HAL_I2S_STATE_READY)
            {
                sa.dma.running = FM_FALSE;
                (void)simple_audio_board_stop(&hi2s1);
            }
            /* بگذار Poll تصمیم بگیرد: stop */
            return FM_OK;
        }
        /* NEXT و REPEAT_ALL → برو بعدی */
        sa.paused = 0u;
        sa.pause_need_restart = 0u;
        if (sa.dma.running || hi2s1.State != HAL_I2S_STATE_READY)
        {
            sa.dma.running = FM_FALSE;
            if (simple_audio_board_stop(&hi2s1) != FM_OK)
            {
                return FM_ERR_IO;
            }
        }
        sa.pending_cmd = SA_CMD_NEXT;
        return FM_OK;
    }
    return SimpleAudio_SeekTo(target);
}

/* ولوم نرم‌افزاری 0..200: 100 = بدون تغییر، بالاتر = تقویت با اشباع (تا
 * +6dB)، 0 = سکوت. از بافر بعدی اعمال می‌شود. همان بازهٔ
 * SimpleVideo_SetVolume. */
FM_Result SimpleAudio_SetVolume(uint8_t percent)
{
    int32_t gain;

    if (percent > 200u)
    {
        percent = 200u;
    }
    gain = (int32_t)(((uint32_t)percent * 0x7FFFu) / 100u);
    sa_volume_pct = percent;
    simple_audio_cfg()->volume = gain;
    AudioDma_SetVolume(&sa.dma, gain);
    return FM_OK;
}

uint8_t SimpleAudio_GetVolume(void)
{
    return sa_volume_pct;
}

/* حالت ماندگاری پخش — چی شد وقتی آهنگ تمام شد؟
 * FM_PLAY_MODE_STOP       : تمام شد → توقف (pause، نرو بعدی)
 * FM_PLAY_MODE_NEXT       : تمام شد → بعدی، آخر لیست → توقف
 * FM_PLAY_MODE_REPEAT_ONE : تمام شد → همین آهنگ دوباره (تکرار یک)
 * FM_PLAY_MODE_REPEAT_ALL : تمام شد → بعدی، آخر → اول (لوپ پوشه) — پیش‌فرض
 * برای پرزنت: STOP یا NEXT بذار تا خودش نپره؛ برای موزیک پس‌زمینه REPEAT_ALL. */
FM_Result SimpleAudio_SetPlayMode(FM_PlayMode mode)
{
    if ((int)mode < FM_PLAY_MODE_STOP || (int)mode > FM_PLAY_MODE_REPEAT_ALL) return FM_ERR_INVALID_ARGUMENT;
    simple_audio_cfg()->play_mode = mode;
    AudioDma_SetPlayMode(&sa.dma, mode);
    FM_Log("SimpleAudio: SetPlayMode(%d) %s\n", (int)mode,
           mode==FM_PLAY_MODE_STOP?"STOP":mode==FM_PLAY_MODE_NEXT?"NEXT":mode==FM_PLAY_MODE_REPEAT_ONE?"REPEAT_ONE":"REPEAT_ALL");
    return FM_OK;
}
FM_PlayMode SimpleAudio_GetPlayMode(void)
{
    return simple_audio_cfg()->play_mode;
}

/*
 * «اسم موزیک و تنظیم صدا» — همهٔ اطلاعات استریم فعلی در یک struct.
 * (وقتی پخش نیست، فیلدها صفر/خالی برمی‌گردند.)
 */
void SimpleAudio_Info(SimpleAudioInfo *out)
{
    FM_AudioInfo info;

    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->bits = 16;
    out->volume_pct = sa_volume_pct;
    out->paused = sa.paused;
    out->running = (sa.dma.running || sa.paused) ? 1u : 0u;
    out->play_mode = simple_audio_cfg()->play_mode;
    out->output = simple_audio_cfg()->output;

    if (!sa.dma.running && !sa.paused)
    {
        return;
    }
    if (Player_Count(&sa.dma.player) > 0u)
    {
        (void)strncpy(out->name,
                      Player_ItemPath(&sa.dma.player,
                                      Player_Current(&sa.dma.player)),
                      FM_PATH_MAX - 1u);
        out->playlist_count = Player_Count(&sa.dma.player);
        out->playlist_index = Player_Current(&sa.dma.player);
    }
    if (Audio_GetInfo(&sa.dma.player.audio, &info) == FM_OK)
    {
        out->sample_rate = info.sample_rate;
        out->channels = info.channels;
        (void)strncpy(out->codec, info.codec, sizeof(out->codec) - 1u);
    }
    out->duration = Player_GetDuration(&sa.dma.player);
    out->position = simple_audio_playhead();
}

/*
 * خواندن مستقیم از دیکدرِ ترکِ در حال پخش — استریو (جفت L,R).
 *
 * این تابع استریم پخش را جلو می‌برد: آنچه اینجا بخوانید دیگر به بلندگو
 * نمی‌رسد. برای «همان صدایی که پخش می‌شود» SimpleAudio_SetDataCallback را
 * استفاده کنید. برای خواندن یک فایل بدون پخش، خودتان بازش کنید:
 *
 *     FM_Audio a;
 *     Audio_Open("/music/x.mp3", &a);
 *     Audio_ReadStereo(&a, buf, frames, &got);   // یا Audio_ReadMono
 *     Audio_Close(&a);
 */
FM_Result SimpleAudio_DataRead(int16_t *buf, size_t frames,
                               size_t *got_frames)
{
    if (!sa.dma.running)
    {
        return FM_ERR_NOT_READY;
    }
    return Audio_ReadStereo(&sa.dma.player.audio, buf, frames, got_frames);
}

/* همان بالا ولی مونو: استریو به میانگین L/R تبدیل می‌شود. */
FM_Result SimpleAudio_DataReadMono(int16_t *buf, size_t count, size_t *got)
{
    if (!sa.dma.running)
    {
        return FM_ERR_NOT_READY;
    }
    return Audio_ReadMono(&sa.dma.player.audio, buf, count, got);
}

/* ------------------------------------------------------------------------ */
/* ضبط — «تابهٔ صدا»: داده بده، خودش می‌فهمد و به آدرس ذخیره می‌کند         */
/* ------------------------------------------------------------------------ */

/* آیا string با .wav/.mp3 (بی‌توجه به حروف بزرگ/کوچک) تمام می‌شود؟ */
static FM_Bool simple_audio_has_audio_ext(const char *p)
{
    static const char *exts[2] = {".wav", ".mp3"};
    size_t len = strlen(p);
    uint8_t i;

    for (i = 0u; i < 2u; i++)
    {
        size_t elen = strlen(exts[i]);
        size_t k;

        if (len < elen)
        {
            continue;
        }
        for (k = 0u; k < elen; k++)
        {
            char a = p[len - elen + k];
            char b = exts[i][k];

            if (a >= 'A' && a <= 'Z')
            {
                a = (char)(a - 'A' + 'a');
            }
            if (a != b)
            {
                break;
            }
        }
        if (k == elen)
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

/* در حالت «پوشه» (بدون پسوند): rec_000001.wav، rec_000002.wav، ... */
static FM_Result simple_audio_make_rec_path(const char *folder, char *path,
                                            size_t cap)
{
    static uint32_t counter;
    size_t flen;
    size_t off;
    uint32_t n;
    uint32_t tries;
    FM_Result r;

    if (folder == NULL || path == NULL || cap == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    flen = strlen(folder);
    if (flen == 0u || flen + 1 + 4 + 6 + 4 + 1 > cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    while (flen > 1u && folder[flen - 1u] == '/')
    {
        flen--;
    }

    r = Directory_Create(folder);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        return r;
    }

    for (tries = 0u; tries < 10000u; tries++)
    {
        FM_Bool exists = FM_FALSE;

        counter++;
        if (counter > 999999u)
        {
            counter = 1u;
        }
        memcpy(path, folder, flen);
        off = flen;
        path[off++] = '/';
        memcpy(path + off, "rec_", 4u);
        off += 4u;
        for (n = 100000u; n > 0u; n /= 10u)
        {
            path[off++] = (char)('0' + (counter / n) % 10u);
        }
        memcpy(path + off, ".wav", 4u);
        off += 4u;
        path[off] = '\0';

        if (File_Exists(path, &exists) != FM_OK || !exists)
        {
            return FM_OK;
        }
    }
    return FM_ERR_EXISTS;
}

/*
 * شروع ضبط:
 *   dest = آدرس دقیق با پسوند  -> همان فایل: "/rec/take1.wav"
 *                                (فرمت از پسوند: .wav داخلی / .mp3 اگر
 *                                encoder ثبت‌شده باشد)
 *   dest = پوشه (بدون پسوند)   -> نام خودکار: "/rec" -> rec_000001.wav
 *
 * rate/channels: ویژگی‌های دادهٔ خامی که می‌دهید (مثلاً 16000 Hz mono
 * از میکروفون). هدر فایل (WAV) خودکار درست نوشته/رفع می‌شود.
 */
FM_Result SimpleAudio_RecStart(const char *dest, uint32_t rate,
                               uint16_t channels)
{
    FM_AudioEncodeConfig cfg;
    FM_Result r;

    if (dest == NULL || rate == 0u || (channels != 1u && channels != 2u))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (sa.rec_active)
    {
        return FM_ERR_BUSY;
    }
#if SIMPLE_AUDIO_I2S_REC
    if (sa.dma.running && rate != sa.dma.sample_rate)
    {
        FM_Log("SimpleAudio: RX rate != playback rate — stop playback "
               "first (one I2S clock)\n");
        return FM_ERR_BUSY;
    }
#endif

    memset(&sa.rec, 0, sizeof(sa.rec));
    cfg.sample_rate = rate;
    cfg.channels = channels;
    cfg.bits_per_sample = 16;
    cfg.bitrate_kbps = 0u;

    if (simple_audio_has_audio_ext(dest))
    {
        (void)strncpy(sa.rec_path, dest, FM_PATH_MAX - 1u);
        sa.rec_path[FM_PATH_MAX - 1u] = '\0';
        r = AudioFileWriter_Open(&sa.rec, sa.rec_path,
                                 FM_AUDIO_FILE_AUTO, &cfg);
    }
    else
    {
        r = simple_audio_make_rec_path(dest, sa.rec_path,
                                       sizeof(sa.rec_path));
        if (r == FM_OK)
        {
            r = AudioFileWriter_Open(&sa.rec, sa.rec_path,
                                     FM_AUDIO_FILE_AUTO, &cfg);
        }
    }
    if (r != FM_OK)
    {
        FM_Log("SimpleAudio: rec start failed (%s): %s (%d)\n", dest,
               File_GetErrorString(r), (int)r);
        return r;
    }

    sa.rec_active = FM_TRUE;
    sa.rec_rate = rate;
    sa.rec_channels = channels;
    sa.rec_frames = 0u;

#if SIMPLE_AUDIO_I2S_REC
    r = simple_audio_board_rx_start(rate, channels);
    if (r != FM_OK)
    {
        (void)AudioFileWriter_Close(&sa.rec);
        sa.rec_active = FM_FALSE;
        return r;
    }
#endif
    FM_Log("SimpleAudio: recording -> %s (%lu Hz %uch)\n", sa.rec_path,
           (unsigned long)rate, (unsigned)channels);
    return FM_OK;
}

/*
 * دادن داده به تابه: `frames` فریم (هر فریم = channels نمونهٔ int16،
 * به‌هم‌ریز). هر چقدر data از منبع (DMA/IRQ/چرخهٔ اصلی) آمد، اینجا بده.
 */
FM_Result SimpleAudio_RecWrite(const int16_t *pcm, size_t frames)
{
    size_t written = 0u;
    FM_Result r;

    if (!sa.rec_active)
    {
        return FM_ERR_NOT_READY;
    }
    if (pcm == NULL || frames == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = AudioFileWriter_WriteFrames(&sa.rec, pcm, frames, &written);
    sa.rec_frames += written;
    return r;
}

/* پایان ضبط: هدر WAV رفع و فایل بسته می‌شود. آدرس نهایی: RecPath(). */
FM_Result SimpleAudio_RecStop(void)
{
    FM_Result r;

    if (!sa.rec_active)
    {
        return FM_ERR_NOT_READY;
    }
#if SIMPLE_AUDIO_I2S_REC
    (void)simple_audio_board_rx_stop();
#endif
    r = AudioFileWriter_Close(&sa.rec);
    sa.rec_active = FM_FALSE;
    if (r == FM_OK)
    {
        FM_Log("SimpleAudio: recorded %s (%.2f s, %lu frames)\n",
               sa.rec_path,
               (double)sa.rec_frames / (double)sa.rec_rate,
               (unsigned long)sa.rec_frames);
    }
    return r;
}

/* آدرس فایل آخرین ضبط (معتبر بعد از RecStart تا بعد از RecStop). */
const char *SimpleAudio_RecPath(void)
{
    return sa.rec_path;
}

FM_Bool SimpleAudio_RecActive(void)
{
    return sa.rec_active;
}

/* ------------------------------------------------------------------------ */
/* HAL I2S callbacks (IRQ) — فقط flag؛ هیچ فایل/decode/لاگ در IRQ            */
/* ------------------------------------------------------------------------ */

/* HAL_I2S Tx callbacks live in simple_i2s_mux.h (shared with SimpleVideo). */

#if SIMPLE_AUDIO_I2S_REC
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        sa_rx_free[0] = 1u;
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s == &hi2s1)
    {
        sa_rx_free[1] = 1u;
    }
}
#endif

#endif /* SIMPLE_AUDIO_H_ */
