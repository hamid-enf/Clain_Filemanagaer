/*
 * Shared I2S IRQ mux for SimpleAudio (17) and SimpleVideo (19).
 *
 * Both headers may be included in the same main.c: HAL_I2S Tx callbacks
 * are emitted once. Begin() of one player takes the sink and Stop()s the
 * other if it currently owns hi2s1. Poll both every loop — the inactive
 * one returns FM_ERR_NOT_READY.
 *
 * Included from 17 / 19; do not add this file to the CubeMX sources list.
 */

#ifndef EXAMPLES_SIMPLE_I2S_MUX_H_
#define EXAMPLES_SIMPLE_I2S_MUX_H_

#include "i2s.h"
#include "fm_types.h"
#include "fm_board_config.h"

/* These examples drive a single shared hi2s1.  If the board config is not
 * loaded for some reason (e.g. include path mismatch), the default ONE
 * shared channel keeps the examples building and correct. */
#ifndef FM_AUDIO_I2S_SHARED
#define FM_AUDIO_I2S_SHARED 1
#endif

/*
 * دو حالت کار (با FM_AUDIO_I2S_SHARED در fm_board_config.h):
 *
 *  SHARED = 1  (پیش‌فرض)
 *      موسیقی و صدای ویدیو یک I2S مشترک دارند. هرکدام Begin() شود
 *      دیگری Stop() می‌شود. روی هر بردی کار می‌کند.
 *
 *  SHARED = 0
 *      هر کدام I2S خودش را دارد و می‌توانند همزمان پخش شوند. لازم است
 *      بردتان دو I2S داشته باشد و هر دو در کیوب فعال باشند. تشخیص
 *      اینکه یک وقفه مال کدام است، از روی handle انجام می‌شود.
 */

typedef void (*SimpleI2S_IrqFn)(I2S_HandleTypeDef *hi2s);
typedef FM_Result (*SimpleI2S_StopFn)(void);

/*
 * Shared I2S DMA ring for the two players that use hi2s1.
 *
 * SimpleAudio needs 2 * SIMPLE_AUDIO_HALF_SAMPLES (~128 KB) and SimpleVideo
 * needs 2 * SIMPLE_VIDEO_HALF_SAMPLES (~32 KB).  Both on this board sit in
 * .dma_buffer (RAM_D2, which is also where the LCD SPI bounce lives), so
 * declaring two separate rings wasted 32 KB of the same DMA SRAM even
 * though the two players are mutually exclusive (SimpleI2S_Take stops the
 * other).  A single ring sized for the largest consumer is shared by both:
 * SimpleAudio passes the whole ring to Ex13_AudioDma, SimpleVideo uses only
 * its smaller 32 KB prefix.  The HAL only hands the pointer/count to the
 * DMA engine, and the ring contents are never touched by both owners at the
 * same time, so this is safe.
 */
#ifndef SIMPLE_I2S_RING_SAMPLES
/* 2 * 32767 = max uint16_t HAL_I2S_Transmit_DMA count (half-samples). */
#define SIMPLE_I2S_RING_SAMPLES 65534u
#endif

__attribute__((section(".dma_buffer"), aligned(32)))
static int16_t simple_i2s_ring[SIMPLE_I2S_RING_SAMPLES];

#if FM_AUDIO_I2S_SHARED
/* یک مجموعه قلاب — یک صاحب در هر لحظه. */
static SimpleI2S_IrqFn simple_i2s_half_fn;
static SimpleI2S_IrqFn simple_i2s_full_fn;
static SimpleI2S_IrqFn simple_i2s_err_fn;
static SimpleI2S_StopFn simple_i2s_owner_stop;
static uint8_t simple_i2s_take_depth;
#else
/* دو مجموعه قلاب — یکی برای هر I2S. اندیس ۰ موسیقی، ۱ ویدیو. */
static SimpleI2S_IrqFn simple_i2s_half_fn2[2];
static SimpleI2S_IrqFn simple_i2s_full_fn2[2];
static SimpleI2S_IrqFn simple_i2s_err_fn2[2];
static SimpleI2S_StopFn simple_i2s_owner_stop2[2];

/* کدام handle به کدام کانال تعلق دارد. */
extern I2S_HandleTypeDef FM_AUDIO_I2S_MUSIC;
extern I2S_HandleTypeDef FM_AUDIO_I2S_VIDEO;

static inline int simple_i2s_slot(I2S_HandleTypeDef *h)
{
    if (h == &FM_AUDIO_I2S_VIDEO) { return 1; }
    return 0;                        /* پیش‌فرض: موسیقی */
}

/* کانالی که فراخوان الان دارد ادعا می‌کند. Begin() هر پخش‌کننده
 * قبل از Take آن را تعیین می‌کند. */
static int simple_i2s_active_slot;
#endif

#if FM_AUDIO_I2S_SHARED
static inline void SimpleI2S_Hook(SimpleI2S_IrqFn half, SimpleI2S_IrqFn full,
                                  SimpleI2S_IrqFn err)
{
    simple_i2s_half_fn = half;
    simple_i2s_full_fn = full;
    simple_i2s_err_fn = err;
}
#else
static inline void SimpleI2S_Hook(SimpleI2S_IrqFn half, SimpleI2S_IrqFn full,
                                  SimpleI2S_IrqFn err)
{
    int k = simple_i2s_active_slot;

    simple_i2s_half_fn2[k] = half;
    simple_i2s_full_fn2[k] = full;
    simple_i2s_err_fn2[k] = err;
}

/* پخش‌کننده‌ها قبل از Take این را صدا می‌زنند تا بگویند کدام کانال‌اند.
 * 0 = موسیقی، 1 = ویدیو. */
static inline void SimpleI2S_SelectChannel(int slot)
{
    simple_i2s_active_slot = (slot != 0) ? 1 : 0;
}
#endif

#if FM_AUDIO_I2S_SHARED
static inline void SimpleI2S_Drop(SimpleI2S_StopFn stop)
{
    if (simple_i2s_owner_stop == stop)
    {
        SimpleI2S_Hook(NULL, NULL, NULL);
        simple_i2s_owner_stop = NULL;
    }
}
#else
static inline void SimpleI2S_Drop(SimpleI2S_StopFn stop)
{
    int k;

    for (k = 0; k < 2; k++)
    {
        if (simple_i2s_owner_stop2[k] == stop)
        {
            simple_i2s_half_fn2[k] = NULL;
            simple_i2s_full_fn2[k] = NULL;
            simple_i2s_err_fn2[k] = NULL;
            simple_i2s_owner_stop2[k] = NULL;
        }
    }
}
#endif

#if FM_AUDIO_I2S_SHARED
/* اولویت DMA ارسال صدا را به بالاترین سطح ببر (یک‌بار).
 *
 * چرا: استریم DMA مربوط به I2S با استریم DMA مربوط به LCD/SPI (و MDMA/
 * DMA2D هنگام MJPEG) روی همان کنترلر DMA/بس تزاحم دارند. FIFO ای‌دبلیو‌اس
 * کوچک است؛ اگر استریم LCD (پرش‌های بزرگ) اولویت برابر/بالاتر داشته باشد
 * سرویس I2S دیر می‌شود → TX underrun → «خش‌خش/بریده‌بریده» حتی وقتی
 * حلقهٔ نرم‌افزاری پر است (starve=0). با اولویت VERY_HIGH برای ارسال I2S
 * (و LOW برای LCD در spi_tft_dma.h) این تزاحم از بین می‌رود.
 * روی هاست/بدون HAL بی‌اثر است. */
static inline void SimpleI2S_BoostDmaPriority(void)
{
#if defined(DMA_PRIORITY_VERY_HIGH) && !defined(FM_HOST_BUILD)
    static uint8_t boosted;
    DMA_HandleTypeDef *d;

    if (boosted != 0u)
    {
        return;
    }
    boosted = 1u;
    d = hi2s1.hdmatx;
    if (d != NULL && d->Init.Priority != DMA_PRIORITY_VERY_HIGH)
    {
        d->Init.Priority = DMA_PRIORITY_VERY_HIGH;
        (void)HAL_DMA_DeInit(d);
        if (HAL_DMA_Init(d) != HAL_OK)
        {
            FM_Log("SimpleI2S: DMA priority boost failed\n");
        }
        else
        {
            FM_Log("SimpleI2S: TX DMA priority = VERY_HIGH\n");
        }
    }
#else
    /* هاست/هال‌استاب: کاری نکن */
#endif
}

/* Claim hi2s1. If another player owns it, its Stop() runs first. */
static inline void SimpleI2S_Take(SimpleI2S_IrqFn half, SimpleI2S_IrqFn full,
                                  SimpleI2S_IrqFn err, SimpleI2S_StopFn stop)
{
    SimpleI2S_StopFn prev = simple_i2s_owner_stop;

    SimpleI2S_BoostDmaPriority();
    if (prev == stop)
    {
        SimpleI2S_Hook(half, full, err);
        return;
    }
    simple_i2s_owner_stop = stop;
    SimpleI2S_Hook(half, full, err);
    if (prev != NULL && simple_i2s_take_depth == 0u)
    {
        simple_i2s_take_depth = 1u;
        (void)prev();
        simple_i2s_take_depth = 0u;
    }
}
#else
/* هر کانال صاحب خودش را دارد؛ گرفتن یکی دیگری را متوقف نمی‌کند، پس
 * موسیقی و ویدیو می‌توانند همزمان صدا بدهند. */
static inline void SimpleI2S_Take(SimpleI2S_IrqFn half, SimpleI2S_IrqFn full,
                                  SimpleI2S_IrqFn err, SimpleI2S_StopFn stop)
{
    int k = simple_i2s_active_slot;
    SimpleI2S_StopFn prev = simple_i2s_owner_stop2[k];

    simple_i2s_owner_stop2[k] = stop;
    SimpleI2S_Hook(half, full, err);
    /* فقط اگر همان کانال صاحب دیگری داشت، آن را متوقف کن. */
    if (prev != NULL && prev != stop)
    {
        (void)prev();
    }
}
#endif

#ifndef SIMPLE_I2S_HAL_DEFINED
#define SIMPLE_I2S_HAL_DEFINED
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
#if FM_AUDIO_I2S_SHARED
    if (simple_i2s_half_fn != NULL)
    {
        simple_i2s_half_fn(hi2s);
    }
#else
    int k = simple_i2s_slot(hi2s);

    if (simple_i2s_half_fn2[k] != NULL)
    {
        simple_i2s_half_fn2[k](hi2s);
    }
#endif
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
#if FM_AUDIO_I2S_SHARED
    if (simple_i2s_full_fn != NULL)
    {
        simple_i2s_full_fn(hi2s);
    }
#else
    int k = simple_i2s_slot(hi2s);

    if (simple_i2s_full_fn2[k] != NULL)
    {
        simple_i2s_full_fn2[k](hi2s);
    }
#endif
}

void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
#if FM_AUDIO_I2S_SHARED
    if (simple_i2s_err_fn != NULL)
    {
        simple_i2s_err_fn(hi2s);
    }
#else
    int k = simple_i2s_slot(hi2s);

    if (simple_i2s_err_fn2[k] != NULL)
    {
        simple_i2s_err_fn2[k](hi2s);
    }
#endif
}
#endif

#endif /* EXAMPLES_SIMPLE_I2S_MUX_H_ */
