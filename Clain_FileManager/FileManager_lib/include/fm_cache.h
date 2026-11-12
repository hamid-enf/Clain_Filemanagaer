/*
 * fm_cache.h — نگهداری کش برای DMA
 * =================================
 *
 * قاعدهٔ این فایل، در یک جمله:
 *
 *     کتابخانه کش را روشن یا خاموش نمی‌کند. فقط می‌فهمد روشن است یا نه،
 *     و بر همان اساس کار درست را می‌کند.
 *
 * روشن‌کردن کش کار شماست، در main.c، با همان دستورهای همیشگی:
 *
 *     SCB_EnableICache();
 *     SCB_EnableDCache();
 *
 * همین. هیچ ماکرویی لازم نیست تعریف کنید، هیچ فایلی را نباید ویرایش
 * کنید، و لازم نیست به کتابخانه خبر بدهید. کتابخانه در زمان اجرا
 * رجیستر SCB->CCR را می‌خواند و می‌فهمد.
 *
 * اگر کش را روشن نکنید، همهٔ توابع این فایل بی‌اثرند و هیچ هزینه‌ای
 * ندارند.
 *
 * ---------------------------------------------------------------------
 * چرا زمان اجرا و نه زمان کامپایل
 * ---------------------------------------------------------------------
 * قبلاً این‌ها به ماکرو وابسته بودند. نتیجه‌اش این بود که اگر کش را در
 * main روشن می‌کردید ولی ماکرو را تعریف نمی‌کردید، درایورها هیچ
 * نگهداری‌ای نمی‌کردند و صدا و تصویر نویز می‌گرفت — بدون هیچ پیام خطایی.
 *
 * خواندن رجیستر چند سیکل است و در برابر خودِ عملیات کش ناچیز. در عوض
 * دیگر هیچ حالتی نیست که کش روشن باشد و کتابخانه نداند.
 *
 * ---------------------------------------------------------------------
 * حافظه
 * ---------------------------------------------------------------------
 * این فایل هیچ آدرسی را نمی‌داند و هیچ ناحیه‌ای را تقسیم نمی‌کند.
 * نقشهٔ حافظه، MPU، و اینکه کدام ناحیه کش‌دار باشد کاملاً دست شماست.
 */
#ifndef FM_CACHE_H
#define FM_CACHE_H

#include <stddef.h>
#include <stdint.h>

/*
 * هدر CMSIS را خودمان می‌آوریم.
 *
 * این مهم است: __DCACHE_PRESENT و SCB از CMSIS می‌آیند، و اگر منتظر
 * بمانیم که فایل صدازننده آن را include کرده باشد، ترتیب include
 * تعیین‌کننده می‌شود. اگر fm_cache.h زودتر بیاید، __DCACHE_PRESENT
 * هنوز تعریف نشده، FM_CACHE_HW صفر می‌شود، و *تمام نگهداری کش بی‌صدا
 * از بیلد حذف می‌شود* — دقیقاً همان چیزی که به صدا و تصویر خش می‌داد.
 *
 * حالا هر ترتیبی درست کار می‌کند.
 */
/*
 * توجه به شکل بررسی: (defined(X) && X) نه فقط defined(X).
 *
 * fm_config.h می‌نویسد «#define FM_HOST_BUILD 0» برای بیلد روی برد.
 * با defined() تنهـا، آن صفر هم «تعریف‌شده» حساب می‌شد، پس هدر CMSIS
 * اصلاً include نمی‌شد، __DCACHE_PRESENT نمی‌آمد، FM_CACHE_HW صفر
 * می‌شد و *تمام نگهداری کش بی‌صدا از بیلد حذف می‌شد* — و محافظِ
 * #error پایین هم که همین شرط را داشت، خفه می‌ماند.
 *
 * روی برد نتیجه‌اش این بود: با کش خاموش همه‌چیز سالم، با کش روشن
 * هر عملیات کارت SD خطای -26 می‌داد.
 */
#if !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
#if defined(FM_CACHE_HAL_HEADER)
#include FM_CACHE_HAL_HEADER
#elif defined(STM32H7) || defined(STM32H743xx) || defined(STM32H750xx) || \
      defined(STM32H723xx) || defined(STM32H747xx) || defined(STM32H745xx)
#include "stm32h7xx_hal.h"
#elif defined(STM32H5) || defined(STM32H503xx) || defined(STM32H523xx) || \
      defined(STM32H533xx) || defined(STM32H562xx) || defined(STM32H563xx) || \
      defined(STM32H573xx)
#include "stm32h5xx_hal.h"
#elif defined(STM32F7) || defined(STM32F746xx) || defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#elif defined(STM32F4) || defined(STM32F407xx) || defined(STM32F429xx) || \
      defined(STM32F411xE) || defined(STM32F446xx)
#include "stm32f4xx_hal.h"
#elif defined(STM32F3)
#include "stm32f3xx_hal.h"
#elif defined(STM32F2)
#include "stm32f2xx_hal.h"
#elif defined(STM32F1) || defined(STM32F103xB) || defined(STM32F103xE)
#include "stm32f1xx_hal.h"
#elif defined(STM32F0)
#include "stm32f0xx_hal.h"
#elif defined(STM32G0)
#include "stm32g0xx_hal.h"
#elif defined(STM32G4)
#include "stm32g4xx_hal.h"
#elif defined(STM32L0)
#include "stm32l0xx_hal.h"
#elif defined(STM32L1)
#include "stm32l1xx_hal.h"
#elif defined(STM32L4)
#include "stm32l4xx_hal.h"
#elif defined(STM32L5)
#include "stm32l5xx_hal.h"
#elif defined(STM32U5)
#include "stm32u5xx_hal.h"
#elif defined(STM32WB)
#include "stm32wbxx_hal.h"
#endif
/* اگر سری تشخیص داده نشد اینجا خطا نمی‌دهیم: شاید هستهٔ بدون کش باشد.
 * در آن حالت __DCACHE_PRESENT تعریف نمی‌شود و همه‌چیز no-op می‌ماند،
 * که رفتار درستی است. */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * روی هاست (تست‌های PC) و روی هسته‌های بدون کش، همه‌چیز no-op است.
 */
#if (defined(FM_HOST_BUILD) && FM_HOST_BUILD) || \
    !defined(__DCACHE_PRESENT) || (__DCACHE_PRESENT != 1U)
#define FM_CACHE_HW 0
#else
#define FM_CACHE_HW 1
#endif

/*
 * محافظ: روی سری‌هایی که کش دارند، FM_CACHE_HW صفر یعنی هدر CMSIS
 * پیدا نشده و *همهٔ نگهداری کش بی‌صدا حذف شده* — که به‌جای خطا، خودش
 * را به‌صورت خش صدا و پرش تصویر نشان می‌دهد. پس همین‌جا خطا می‌دهیم.
 *
 * اگر واقعاً می‌خواهید بدون CMSIS بسازید: FM_HOST_BUILD را تعریف کنید.
 */
#if !FM_CACHE_HW && !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
#if defined(STM32H7) || defined(STM32H743xx) || defined(STM32H750xx) || \
    defined(STM32H723xx) || defined(STM32H747xx) || defined(STM32H745xx) || \
    defined(STM32F7) || defined(STM32F746xx) || defined(STM32F767xx)
#error "هدر CMSIS پیدا نشد — نگهداری کش حذف می‌شد. FM_CACHE_HAL_HEADER را تعریف کنید"
#endif
#endif

/* خط کش روی Cortex-M7 سی‌ودو بایت است. */
#ifndef FM_CACHE_LINE
#define FM_CACHE_LINE 32u
#endif

/* Per-path DMA cache policy (see fm_board_config.h).  Missing defines
 * default to "direct DMA if aligned". */
#ifndef FM_CACHE_SD
#define FM_CACHE_SD 0
#endif
#ifndef FM_CACHE_LCD
#define FM_CACHE_LCD 1
#endif
#ifndef FM_CACHE_AUDIO
#define FM_CACHE_AUDIO 1
#endif
#ifndef FM_CACHE_JPEG
#define FM_CACHE_JPEG 1
#endif
#ifndef FM_CACHE_DMA2D
#define FM_CACHE_DMA2D 1
#endif

/* ---------------------------------------------------------------
 * قاعده: کجا Clean، کجا Invalidate؟
 * ---------------------------------------------------------------
 *
 * فقط یک سؤال بپرسید: «چه کسی داده را نوشت؟»
 *
 *   CPU نوشت، DMA می‌خواند   →  CleanForDma()        *قبل* از شروع
 *   DMA نوشت، CPU می‌خواند   →  InvalidateAfterDma() *بعد* از پایان
 *   هر دو می‌نویسند          →  Clean قبل، Invalidate بعد
 *
 * مثال‌های واقعی همین پروژه:
 *
 *   پخش صدا (I2S TX)      CPU سمپل را پر می‌کند   → Clean
 *   ضبط صدا (I2S RX)      DMA سمپل را می‌آورد     → Invalidate
 *   نمایشگر SPI / LTDC    CPU فریم را ساخته       → Clean
 *   خواندن کارت SD        DMA سکتور را می‌آورد    → Invalidate
 *   نوشتن روی کارت SD     CPU داده را ساخته       → Clean
 *   DMA2D                 هر دو                   → Clean و Invalidate
 *
 * دو دام:
 *
 *   ۱. Invalidate با دانه‌بندی ۳۲ بایت کار می‌کند. بافر نامتراز یعنی
 *      خط اول و آخر شامل متغیرهای همسایه می‌شود و نوشته‌های
 *      ذخیره‌نشده‌شان دور ریخته می‌شود. بافرهای DMA را با
 *      FM_CACHE_ALIGN تعریف کنید و طول را هم مضرب ۳۲ بگیرید.
 *      FM_Cache_CheckDmaBuffer() همین را بررسی می‌کند.
 *
 *   ۲. نشانهٔ Invalidate جاافتاده «داده صفر یا کهنه» است، نه crash.
 *      روی کارت SD خودش را این‌طور نشان می‌دهد که همه‌چیز خطا
 *      می‌دهد در حالی که با کش خاموش سالم بود.
 *
 * هر دو تابع با کش خاموش هم بی‌خطرند و همچنان بریر لازم را می‌زنند،
 * پس بی‌قید و شرط صدایشان بزنید — شرط نگذارید.
 */

/* --------------------------------------------------------------- */
/* وضعیت — فقط خواندن، هرگز نوشتن                                    */
/* --------------------------------------------------------------- */

/* D-Cache همین الان روشن است؟ از رجیستر می‌خواند، نه از ماکرو. */
static inline int FM_Cache_IsDataEnabled(void)
{
#if FM_CACHE_HW
    return (SCB->CCR & SCB_CCR_DC_Msk) ? 1 : 0;
#else
    return 0;
#endif
}

/* I-Cache همین الان روشن است؟ */
static inline int FM_Cache_IsInstEnabled(void)
{
#if FM_CACHE_HW
    return (SCB->CCR & SCB_CCR_IC_Msk) ? 1 : 0;
#else
    return 0;
#endif
}

static inline int FM_Cache_DmaAligned(const void *addr, size_t bytes)
{
    if (addr == NULL || bytes == 0u)
    {
        return 0;
    }
    return ((((uintptr_t)addr) & ((uintptr_t)FM_CACHE_LINE - 1u)) == 0u &&
            (bytes & ((size_t)FM_CACHE_LINE - 1u)) == 0u) ? 1 : 0;
}

/* 1 = DMA may target `addr` directly.  0 = bounce through an aligned
 * scratch (required for FatFS win[] and stack buffers). */
static inline int FM_Cache_SdDirectOk(const void *addr, size_t bytes)
{
    if (!FM_CACHE_SD)
    {
        return 0;
    }
    if (!FM_Cache_IsDataEnabled())
    {
        return 1;
    }
    return FM_Cache_DmaAligned(addr, bytes);
}

/* --------------------------------------------------------------- */
/* نواحی‌ای که هرگز کش نمی‌شوند                                       */
/* --------------------------------------------------------------- */
/* DTCM و ITCM از کش رد نمی‌شوند، پس عملیات کش رویشان بی‌معناست.
 * DTCM: 0x20000000..0x2001FFFF ، ITCM: 0x00000000..0x0000FFFF */
static inline int fm_cache_addr_is_tcm(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a < 0x00010000u)
    {
        return 1;                       /* ITCM */
    }
    if (a >= 0x20000000u && a < 0x20020000u)
    {
        return 1;                       /* DTCM */
    }
    return 0;
}

/* --------------------------------------------------------------- */
/* نگهداری                                                          */
/* --------------------------------------------------------------- */
/*
 * هر دو تابع:
 *   • اگر D-Cache خاموش باشد، فقط یک DSB می‌زنند و برمی‌گردند.
 *   • آدرس و طول را به مرز خط کش گرد می‌کنند (شرط سخت‌افزار).
 *   • با NULL یا طول صفر بی‌سروصدا برمی‌گردند.
 */

/* قبل از اینکه DMA از حافظه *بخواند*: نوشته‌های CPU را بیرون بده.
 * بدون این، DMA دادهٔ قدیمی RAM را می‌خواند و صدا/تصویر خراب می‌شود. */
static inline void FM_Cache_CleanForDma(const void *addr, size_t bytes)
{
#if FM_CACHE_HW
    if (addr != NULL && bytes != 0u && FM_Cache_IsDataEnabled() &&
        !fm_cache_addr_is_tcm(addr))
    {
        /* به مرز خط گرد می‌شود، وگرنه CMSIS بخشی از داده را جا
         * می‌گذارد. */
        uintptr_t start = (uintptr_t)addr &
                          ~((uintptr_t)FM_CACHE_LINE - 1u);
        uintptr_t end   = ((uintptr_t)addr + bytes + FM_CACHE_LINE - 1u) &
                          ~((uintptr_t)FM_CACHE_LINE - 1u);

        SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    }
    /*
     * DSB بیرون از شرط است و این عمدی است.
     *
     * حتی با کش کاملاً خاموش، نوشته‌های CPU می‌توانند در write buffer
     * معلق بمانند. DMA از باس دیگری می‌خواند و آن بافر را نمی‌بیند، پس
     * بدون این حصار دادهٔ کهنه می‌خواند — خش صدا و پرش تصویر، در حالی
     * که هیچ کشی روشن نیست.
     *
     * یک بار این را داخل شرط بردم و دقیقاً همین اتفاق افتاد.
     */
    __DSB();
#else
    (void)addr;
    (void)bytes;
#endif
}

/* بعد از اینکه DMA در حافظه *نوشت*: نسخهٔ کهنهٔ کش را دور بریز.
 * بدون این، CPU دادهٔ قدیمی را از کش می‌خواند. */
/*
 * نسخهٔ «فقط Invalidate» — این تابع دیگر هیچ Cleanی انجام نمی‌دهد.
 *
 * چرا؟ باگ واقعی روی برد: برای بافر ناهم‌تراز، Cleanِ خطِ لبه بعد از
 * DMA محتوای کهنهٔ کش را روی دادهٔ تازهٔ DMA می‌نویسد — در لاگ تست
 * به‌صورت «آخرین ۸ بایت خواندن I2C صفر» ظاهر شد (کش روشن). Cleanِ
 * لبه‌ها مسئولیتِ «قبل از کیک DMA» است؛ برای RX هم همین‌طور (تمام
 * خطوط پوشش‌دهندهٔ بافر قبل از انتقال Clean می‌شوند تا نوشته‌های
 * معلق همسایه‌ها از بین نروند — DMA بعداً فقط بایت‌های خودش را
 * بازنویسی می‌کند و اینجا همهٔ خطوط پوشش‌دهنده Invalidate می‌شوند).
 *
 * ترتیب صحیح RX (رعایت‌شده در fm_xdma.h و i2c_eeprom_dma.h / spi_flash_dma_port.h):
 *     FM_Cache_CleanForDma(buf, len);        ۱) قبل از کیک
 *     kick DMA; wait;                        ۲) انتقال
 *     FM_Cache_InvalidateAfterDma(buf, len); ۳) فقط Invalidate
 *
 * و باز هم بهترین راه: بافر را FM_CACHE_ALIGN و طول را مضرب ۳۲
 * بگیرید — آن‌وقت نه لبه‌ای هست نه همین بحث.
 */
static inline void FM_Cache_InvalidateAfterDma(void *addr, size_t bytes)
{
#if FM_CACHE_HW
    if (addr != NULL && bytes != 0u && FM_Cache_IsDataEnabled() &&
        !fm_cache_addr_is_tcm(addr))
    {
        uintptr_t raw_start = (uintptr_t)addr;
        uintptr_t raw_end   = raw_start + bytes;
        uintptr_t start     = raw_start &
                              ~((uintptr_t)FM_CACHE_LINE - 1u);
        uintptr_t end       = (raw_end + FM_CACHE_LINE - 1u) &
                              ~((uintptr_t)FM_CACHE_LINE - 1u);

        SCB_InvalidateDCache_by_Addr((uint32_t *)start,
                                     (int32_t)(end - start));
    }
    __DSB();      /* همیشه — به دلیل بالا */
#else
    (void)addr;
    (void)bytes;
#endif
}

/* برای بافری که در هر دو جهت استفاده می‌شود. */
static inline void FM_Cache_CleanInvalidate(void *addr, size_t bytes)
{
    FM_Cache_CleanForDma(addr, bytes);
    FM_Cache_InvalidateAfterDma(addr, bytes);
}

/*
 * هم‌ترازی توصیه‌شده برای بافرهای DMA وقتی کش روشن است.
 *
 *     FM_CACHE_ALIGN static uint8_t buf[4096];
 *
 * چرا: نگهداری کش با دانه‌بندی ۳۲ بایت انجام می‌شود. اگر بافر هم‌تراز
 * نباشد، یک invalidate می‌تواند متغیر همسایه را هم دور بیندازد.
 */
#define FM_CACHE_ALIGN __attribute__((aligned(FM_CACHE_LINE)))

/* گزارش وضعیت — از رجیستر می‌خواند، نه از ماکرو. */
static inline void FM_Cache_AutoInit(void);

static inline void FM_Cache_Report(void)
{
    extern int FM_Log(const char *fmt, ...);

    /* اگر کاربر Report را قبل از FileManager_Init صدا بزند (کار
     * رایجی است) فلگ باید تا اینجا اعمال شده باشد، وگرنه گزارش
     * وضعیتی را نشان می‌دهد که یک خط بعد عوض می‌شود. */
    FM_Cache_AutoInit();

#if !FM_CACHE_HW
    /*
     * سکوت اینجا بدترین حالت بود: کاربر FM_Cache_Report() را صدا
     * می‌زد و *هیچ* خطی نمی‌دید، و همین نبودنِ خط تنها نشانهٔ این بود
     * که تمام نگهداری کش بی‌صدا حذف شده. حالا صریح می‌گوییم.
     */
    (void)FM_Log("cache: maintenance COMPILED OUT (no CMSIS __DCACHE_PRESENT) "
                 "— if this core has a cache, define FM_CACHE_HAL_HEADER\n");
#else
    (void)FM_Log("cache: icache=%u dcache=%u\n",
                 (unsigned)FM_Cache_IsInstEnabled(),
                 (unsigned)FM_Cache_IsDataEnabled());

    /*
     * با D-Cache روشن، هر DMAیی که *در* حافظه می‌نویسد باید بعدش
     * invalidate شود، وگرنه CPU نسخهٔ کهنهٔ کش را می‌خواند. این شامل
     * درایورهای بیرون این کتابخانه هم می‌شود — مهم‌ترینشان SDMMC زیر
     * FatFs. علامتش این است: هر عملیات فایل خطا می‌دهد یا داده صفر
     * می‌آید، در حالی که با کش خاموش همه‌چیز سالم بود.
     *
     * این را می‌گوییم چون تشخیصش از روی نشانه تقریباً غیرممکن است.
     */
    if (FM_Cache_IsDataEnabled())
    {
        (void)FM_Log(
            "cache: D-Cache is ON — every DMA that WRITES into memory "
            "must call FM_Cache_InvalidateAfterDma() afterwards "
            "(SD/SDMMC under FatFs included), or the CPU reads stale "
            "data. See FM_Cache_CheckDmaBuffer().\n");
    }
#endif
}

/*
 * آیا این بافر برای DMA با کش روشن امن است؟
 *
 * دو شرط دارد و هر دو سخت‌افزاری‌اند:
 *   • آدرس باید روی مرز خط کش (۳۲ بایت) باشد.
 *   • طول هم باید مضرب خط کش باشد.
 *
 * اگر نباشد، invalidate ناچار خط‌های لبه را هم شامل می‌شود و
 * متغیرهای همسایه را دور می‌ریزد. FM_Cache_InvalidateAfterDma خودش
 * لبه‌ها را اول Clean می‌کند تا این اتفاق نیفتد، ولی بهتر است بافر
 * از اول درست تراز باشد.
 *
 * با کش خاموش همیشه FM_TRUE برمی‌گرداند — هیچ‌کدام از این‌ها
 * اهمیتی ندارد.
 *
 * برمی‌گرداند: FM_TRUE اگر امن باشد.
 */
static inline int FM_Cache_CheckDmaBuffer(const void *addr, size_t bytes,
                                          const char *name)
{
#if FM_CACHE_HW
    extern int FM_Log(const char *fmt, ...);

    if (!FM_Cache_IsDataEnabled() || addr == NULL || bytes == 0u ||
        fm_cache_addr_is_tcm(addr))
    {
        return 1;
    }
    if ((((uintptr_t)addr) & ((uintptr_t)FM_CACHE_LINE - 1u)) != 0u ||
        (bytes & ((size_t)FM_CACHE_LINE - 1u)) != 0u)
    {
        (void)FM_Log("cache: %s at %p size %u is not %u-byte aligned — "
                     "align it (FM_CACHE_ALIGN) or a neighbouring "
                     "variable can be discarded by invalidate\n",
                     (name != NULL) ? name : "DMA buffer",
                     addr, (unsigned)bytes, (unsigned)FM_CACHE_LINE);
        return 0;
    }
    return 1;
#else
    (void)addr; (void)bytes; (void)name;
    return 1;
#endif
}

/* ==================================================================
 * FM_Cache_Apply — پشت فلگ FM_CACHE
 * ==================================================================
 *
 * تنها جایی در کل کتابخانه که کش را روشن یا خاموش می‌کند، و فقط
 * وقتی که خود شما در main() صریحاً خواسته باشید. هیچ مسیر دیگری
 * این تابع را صدا نمی‌زند.
 *
 * روشن‌کردن: SCB_EnableICache/DCache همان کاری را می‌کنند که باید،
 * و خودشان invalidate اولیه را انجام می‌دهند.
 *
 * خاموش‌کردن: SCB_DisableDCache قبل از خاموشی کش را Clean می‌کند،
 * پس داده‌ای که هنوز در کش مانده به حافظه نوشته می‌شود و از دست
 * نمی‌رود. ترتیب هم مهم است: اول داده، بعد دستور.
 *
 * MPU دست‌نخورده می‌ماند. اگر ناحیه‌ای Cacheable نباشد این تابع
 * رویش اثری ندارد، که درست است — چیدمان MPU کار شماست.
 */
static inline void FM_Cache_Apply(int enable)
{
    extern int FM_Log(const char *fmt, ...);
#if FM_CACHE_HW
    if (enable)
    {
        SCB_EnableICache();
        SCB_EnableDCache();
    }
    else
    {
        /* Disable خودش Clean می‌کند؛ داده از دست نمی‌رود. */
        SCB_DisableDCache();
        SCB_DisableICache();
    }
    __DSB();
    __ISB();
    (void)FM_Log("cache: %s -> icache=%u dcache=%u\n",
                 enable ? "ON" : "OFF",
                 (unsigned)FM_Cache_IsInstEnabled(),
                 (unsigned)FM_Cache_IsDataEnabled());
#else
    /*
     * کش سخت‌افزاری در دسترس نیست. اگر کاربر CACHE(ON) خواسته این
     * را بلند بگوییم، چون یعنی یا هستهٔ بدون کش است یا هدر CMSIS
     * پیدا نشده — و حالت دوم همان تلهٔ بی‌صداست.
     */
    if (enable)
    {
        (void)FM_Log("cache: CACHE(ON) خواسته شد ولی نگهداری کش در این "
                     "بیلد نیست (CMSIS __DCACHE_PRESENT نبود)\n");
    }
#endif
}

/* ==================================================================
 * اعمال خودکار فلگ FM_CACHE
 * ==================================================================
 *
 * با «#define FM_CACHE ON» در fm_board_config.h، کش خودکار روشن
 * می‌شود. هیچ تابعی صدا نمی‌زنید.
 *
 * چرا constructor نه:
 *
 *   نسخهٔ اول این کار را با __attribute__((constructor)) می‌کرد، که
 *   *قبل از main* اجرا می‌شود — یعنی قبل از MPU_Config(). روشن‌کردن
 *   کش پیش از پیکربندی MPU یعنی کش با ویژگی‌های حافظهٔ پیش‌فرض شروع
 *   به کار می‌کند، نه آنچه شما تعریف کرده‌اید. غلط بود.
 *
 * حالا: FM_Cache_AutoInit() در اولین صدازدن هر API کتابخانه اجرا
 * می‌شود (FileManager_Init، FM_Cache_Report و غیره) — یعنی مطمئناً
 * بعد از MPU_Config()، چون آن اولین چیز در main است.
 *
 * فقط یک بار اثر می‌کند. اگر خودتان زودتر SCB_EnableDCache() زده
 * باشید هم مشکلی نیست: می‌بیند روشن است و کاری نمی‌کند.
 */
static inline void FM_Cache_AutoInit(void)
{
#if defined(FM_CACHE) && FM_CACHE && FM_CACHE_HW
    static unsigned char done = 0u;

    if (done) {
        return;
    }
    done = 1u;

    /* اگر کاربر خودش قبلاً روشنش کرده، دوباره invalidate نکنیم. */
    if (!FM_Cache_IsDataEnabled()) {
        FM_Cache_Apply(1);
    }
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* FM_CACHE_H */
