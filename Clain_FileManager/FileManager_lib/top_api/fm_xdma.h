/*
 * fm_xdma.h — موتور انتقال DMA پورت‌پذیر برای فاز دادهٔ فلش‌های سری
 * ================================================================
 *
 * یک بار تعریف می‌شود و مثال‌های ۱۵ (W25Q256)، ۲۶ (درایور عمومی
 * QUADSPI) و ۳۳ (فلش روی SPI معمولی) از همان استفاده می‌کنند — هیچ
 * درایوری کد DMA تکراری ندارد و «روشن بودن DMA» خودکار تشخیص داده
 * می‌شود، نه با دو نسخه کدنویسی.
 *
 * فلسفه (روی همهٔ MCUها یکی است):
 *
 *   ۱) اگر موتوری نصب نشده باشد یا طول انتقال بیرون از پنجره باشد،
 *      همه‌چیز polling می‌شود — خطا و کرش در کار نیست.
 *   ۲) اگر اولین انتقال DMA شکست بخورد (بافر در حافظه‌ای که DMA به آن
 *      نمی‌رسد مثل DTCM، وقفهٔ خاموش، پیکربندی غلط DMAMUX و ...)،
 *      یک‌بار برای همیشه به polling برمی‌گردیم و یک خطا لاگ می‌شود.
 *   ۳) نگهداری کش داخل همین لایه است و درایورها چیزی از کش نمی‌دانند:
 *      Tx → FM_Cache_CleanForDma قبل از کیک،
 *      Rx → FM_Cache_InvalidateAfterDma بعد از اتمام.
 *
 * پورت‌پذیری — موتور هر خانواده را همین‌جا نصب می‌کنیم:
 *
 *   STM32H7 : MDMA با ریکوئست QUADSPI_FIFO_TH (QUADSPI در D1 است و
 *             فقط MDMA به آن می‌رسد). کیوب: DMA Settings → MDMA +
 *             QUADSPI_FIFO_TH؛ NVIC → QUADSPI + همان کانال MDMA.
 *   STM32H5 : XSPI/OCTOSPI با GPDMA1 (ریکوئست OCTOSPI1 در DMAMUX).
 *   بقیه    : هیچ — polling. می‌توانید با FM_Xdma_Use موتور خودتان
 *             را بدهید؛ قرارداد سه تابع کوچک بیشتر نمی‌خواهد.
 *
 * استفادهٔ درایور (داخل مثال ۱۵/۲۶):
 *
 *     if (FM_Xdma_Rx(buf, len, timeout_ms)) return FM_OK;   * DMA    *
 *     ... مسیر polling ...                                  * فالبک   *
 *
 * استفادهٔ برنامه برای فلش روی SPI معمولی یا موتور دلخواه:
 *
 *     static const FM_XdmaEngine my_engine = { my_kick_rx, ... };
 *     FM_Xdma_Use(&my_engine);
 */

#ifndef FM_XDMA_H
#define FM_XDMA_H

#include <stdint.h>
#include "fm_types.h"
#include "fm_log.h"
#include "fm_cache.h"

/* پنجرهٔ طول انتقال: کوچک‌تر از حداقل یعنی راه‌اندازی DMA نمی‌ارزد،
 * بزرگ‌تر از حداکثر یعنی سقف امن رجیستر NbData/اندازهٔ بافر. */
#ifndef FM_XDMA_MIN_LEN
#define FM_XDMA_MIN_LEN 64u
#endif
#ifndef FM_XDMA_MAX_LEN
#define FM_XDMA_MAX_LEN 4096u
#endif

/* APIهای اختیاری لایه: اگر برنامه‌ای ازشان استفاده نکند هشدار ندهند. */
#if defined(__GNUC__)
#define FM_XDMA_API __attribute__((unused))
#else
#define FM_XDMA_API
#endif

typedef struct
{
    /* کیک دریافت داخل بافر (بعد از صادرشدن دستور روی سیم).
     * FM_TRUE = کیک شد؛ FM_FALSE = این موتور آماده نیست. */
    FM_Bool (*kick_rx)(void *ctx, void *buf, uint32_t len);
    /* کیک ارسال از بافر. */
    FM_Bool (*kick_tx)(void *ctx, const void *buf, uint32_t len);
    /* صبر تا پایان آخرین کیک. خطا = FM_ERR_TIMEOUT/FM_ERR_IO.
     * باید در شکست، کنترلر را Abort کند تا باس سالم بماند. */
    FM_Result (*wait)(void *ctx, uint32_t timeout_ms);
    void *ctx;
} FM_XdmaEngine;

/* وضعیت لایه — درایورهای یک TU همه از این یکی استفاده می‌کنند. */
static const FM_XdmaEngine *fm_xdma_engine;
static FM_Bool              fm_xdma_alive = FM_TRUE;
static uint32_t             fm_xdma_min = FM_XDMA_MIN_LEN;
static uint32_t             fm_xdma_max = FM_XDMA_MAX_LEN;

/* نصب موتور. NULL یعنی «DMA ندارم» — همان حالت پیش‌فرض. */
FM_XDMA_API static void FM_Xdma_Use(const FM_XdmaEngine *engine)
{
    fm_xdma_engine = engine;
    fm_xdma_alive  = FM_TRUE;
}

/* آیا فعلاً DMA در مدار است؟ (برای چاپ وضعیت در لاگ) */
FM_XDMA_API static FM_Bool FM_Xdma_IsActive(void)
{
    return (fm_xdma_alive && fm_xdma_engine != NULL) ? FM_TRUE : FM_FALSE;
}

/* پنجرهٔ طول را درایور تنظیم می‌کند (پیش‌فرض ۶۴ تا ۴۰۹۶ بایت). */
FM_XDMA_API static void FM_Xdma_SetWindow(uint32_t min_len, uint32_t max_len)
{
    fm_xdma_min = min_len;
    fm_xdma_max = max_len;
}

/* خاموش‌کردن دستی — مثلاً بعد از سوییچ به memory-mapped خالص. */
FM_XDMA_API static void FM_Xdma_Disable(void)
{
    fm_xdma_alive = FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* مسیر اصلی — درایورها فقط این دو را صدا می‌زنند                      */
/* ------------------------------------------------------------------ */

#define FM_XDMA_DONE    0   /* با DMA تمام شد و کش معتبر است        */
#define FM_XDMA_SKIPPED 1   /* DMA در کار نبود — دستور سالم است؛
                             *  درایور مستقیم polling برود          */
#define FM_XDMA_FAILED  2   /* DMA شکست و کنترلر Abort شد —
                             *  دستور نیمه‌کاره باید از نو صادر شود */

static FM_Bool fm_xdma_pass(uint32_t len)
{
    return (fm_xdma_alive && fm_xdma_engine != NULL && len != 0u &&
            len >= fm_xdma_min && len <= fm_xdma_max) ? FM_TRUE : FM_FALSE;
}

static void fm_xdma_fail(const char *what)
{
    fm_xdma_alive = FM_FALSE;   /* یک‌بار برای همیشه: polling */
    FM_Log("xdma: %s failed - permanent polling fallback\n", what);
}

static int fm_xdma_run(FM_Bool is_rx, void *buf, uint32_t len,
                       uint32_t timeout_ms)
{
    FM_Result r;

    if (!fm_xdma_pass(len)) return FM_XDMA_SKIPPED;

    /*
     * پروتکل کش (بعد از باگ واقعی برد): هر دو جهت قبل از کیک Clean
     * می‌شوند. برای TX یعنی نوشته‌های CPU بیرون برود؛ برای RX یعنی
     * خطوط پوشش‌دهندهٔ بافر (با بایت‌های همسایهٔ لبه‌ها) فلش شوند تا
     * Invalidateِ بعد از انتقال هیچ دادهٔ تازه‌ای را از بین نبرد.
     */
    FM_Cache_CleanForDma(buf, len);

    if (is_rx)
    {
        if (!fm_xdma_engine->kick_rx(fm_xdma_engine->ctx, buf, len))
        {
            fm_xdma_fail("rx kick");
            return FM_XDMA_FAILED;
        }
    }
    else
    {
        if (!fm_xdma_engine->kick_tx(fm_xdma_engine->ctx, buf, len))
        {
            fm_xdma_fail("tx kick");
            return FM_XDMA_FAILED;
        }
    }

    r = fm_xdma_engine->wait(fm_xdma_engine->ctx, timeout_ms);
    if (r != FM_OK)
    {
        fm_xdma_fail(is_rx ? "rx wait" : "tx wait");
        return FM_XDMA_FAILED;
    }

    if (is_rx)
    {
        /* کش را از دادهٔ کهنهٔ DMA خبر کن — پروتکل کتابخانه. */
        FM_Cache_InvalidateAfterDma(buf, len);
    }
    return FM_XDMA_DONE;
}

FM_XDMA_API static int FM_Xdma_RxEx(void *buf, uint32_t len, uint32_t timeout_ms)
{
    return fm_xdma_run(FM_TRUE, buf, len, timeout_ms);
}

FM_XDMA_API static int FM_Xdma_TxEx(const void *buf, uint32_t len, uint32_t timeout_ms)
{
    return fm_xdma_run(FM_FALSE, (void *)(uintptr_t)buf, len, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* موتور پیش‌فرض STM32H7 — QUADSPI + MDMA                              */
/* ------------------------------------------------------------------ */
/*
 * خودکار نصب می‌شود اگر (الف) خانوادهٔ H7 تعریف باشد، (ب) در کیوب
 * کانال MDMA به QUADSPI وصل شده باشد (hmdma != NULL). برای فلش روی
 * SPI معمولی از FM_Xdma_SpiInstall در مثال ۳۳ استفاده کنید.
 */
#if (defined(STM32H743xx) || defined(STM32H750xx) || defined(STM32H723xx) || \
     defined(STM32H725xx) || defined(STM32H730xx) || defined(STM32H735xx) || \
     defined(STM32H747xx) || defined(STM32H757xx) || defined(STM32H7A3xx) || \
     defined(STM32H7B0xx) || defined(STM32H7B3xx)) &&                        \
    !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)

#include "stm32h7xx_hal.h"

#ifndef HAL_QSPI_STATE_READY
#define HAL_QSPI_STATE_READY 1u
#endif

static FM_Bool fm_xdma_h7_rx(void *ctx, void *buf, uint32_t len)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;

    (void)len;
    if (h == NULL || h->hmdma == NULL) return FM_FALSE;
    return (HAL_QSPI_Receive_DMA(h, (uint8_t *)buf) == HAL_OK)
               ? FM_TRUE : FM_FALSE;
}

static FM_Bool fm_xdma_h7_tx(void *ctx, const void *buf, uint32_t len)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;

    (void)len;
    if (h == NULL || h->hmdma == NULL) return FM_FALSE;
    return (HAL_QSPI_Transmit_DMA(h, (uint8_t *)(uintptr_t)buf) == HAL_OK)
               ? FM_TRUE : FM_FALSE;
}

static FM_Result fm_xdma_h7_wait(void *ctx, uint32_t timeout_ms)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    uint32_t t0 = HAL_GetTick();

    while (h->State != HAL_QSPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            (void)HAL_QSPI_Abort(h);   /* باس را برای polling سالم کن */
            return FM_ERR_TIMEOUT;
        }
    }
    return FM_OK;
}

static FM_XdmaEngine fm_xdma_h7qspi_engine =   /* ctx در نصب پر می‌شود */
{
    fm_xdma_h7_rx, fm_xdma_h7_tx, fm_xdma_h7_wait, NULL
};

/* اگر MDMA وصل است نصب کن؛ وگرنه بی‌صدا polling بمان. */
static void FM_Xdma_AutoInstallQspi(QSPI_HandleTypeDef *h)
{
    if (!FM_Xdma_IsActive() && h != NULL && h->hmdma != NULL)
    {
        fm_xdma_h7qspi_engine.ctx = h;
        FM_Xdma_Use(&fm_xdma_h7qspi_engine);
        FM_Log("xdma: MDMA engine active (QUADSPI_FIFO_TH)\n");
    }
}

#else /* خانوادهٔ غیر H7: پیش‌فرض polling — هیچ هال‌خاصی کامپایل نمی‌شود */

typedef struct { int fm_xdma_no_qspi; } FM_Xdma_NoQspiStub;
static void FM_Xdma_AutoInstallQspi(void *h)
{
    (void)h;   /* موتوری برای این خانواده از پیش تعریف نشده — polling */
}

#endif /* STM32H7 */

/* ------------------------------------------------------------------ */
/* موتور پیش‌فرض STM32H5 — XSPI/OCTOSPI + GPDMA1                       */
/* ------------------------------------------------------------------ */
#if (defined(STM32H503xx) || defined(STM32H562xx) || defined(STM32H563xx) || \
     defined(STM32H573xx)) &&                                                \
    !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)

#include "stm32h5xx_hal.h"

static FM_Bool fm_xdma_h5_rx(void *ctx, void *buf, uint32_t len)
{
    OSPI_HandleTypeDef *h = (OSPI_HandleTypeDef *)ctx;

    (void)len;
    if (h == NULL || h->hdmarx == NULL) return FM_FALSE;
    return (HAL_OSPI_Receive_DMA(h, (uint8_t *)buf) == HAL_OK)
               ? FM_TRUE : FM_FALSE;
}

static FM_Bool fm_xdma_h5_tx(void *ctx, const void *buf, uint32_t len)
{
    OSPI_HandleTypeDef *h = (OSPI_HandleTypeDef *)ctx;

    (void)len;
    if (h == NULL || h->hdmatx == NULL) return FM_FALSE;
    return (HAL_OSPI_Transmit_DMA(h, (uint8_t *)(uintptr_t)buf) == HAL_OK)
               ? FM_TRUE : FM_FALSE;
}

static FM_Result fm_xdma_h5_wait(void *ctx, uint32_t timeout_ms)
{
    OSPI_HandleTypeDef *h = (OSPI_HandleTypeDef *)ctx;
    uint32_t t0 = HAL_GetTick();

    while (h->State != HAL_OSPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            (void)HAL_OSPI_Abort(h);
            return FM_ERR_TIMEOUT;
        }
    }
    return FM_OK;
}

static FM_XdmaEngine fm_xdma_h5ospi_engine =   /* ctx در نصب پر می‌شود */
{
    fm_xdma_h5_rx, fm_xdma_h5_tx, fm_xdma_h5_wait, NULL
};

/* فلش روی OCTOSPI/XSPIِ H5: کیوب → DMA Settings → GPDMA1 با
 * ریکوئست OCTOSPI1_RX/TX، و NVIC وقفهٔ OCTOSPI + GPDMA1. */
static void FM_Xdma_AutoInstallOspi(OSPI_HandleTypeDef *h)
{
    if (!FM_Xdma_IsActive() && h != NULL &&
        (h->hdmarx != NULL || h->hdmatx != NULL))
    {
        fm_xdma_h5ospi_engine.ctx = h;
        FM_Xdma_Use(&fm_xdma_h5ospi_engine);
        FM_Log("xdma: GPDMA engine active (OCTOSPI)\n");
    }
}

#endif /* STM32H5 */

#endif /* FM_XDMA_H */
