/*
 * Example 30 — DMA2D (Chrom-Art) برای تبدیل YCbCr → RGB
 *
 * واحد JPEG سخت‌افزاری H7 خروجی YCbCr می‌دهد، نه RGB. تا حالا آن
 * تبدیل را CPU پیکسل‌به‌پیکسل انجام می‌داد و همان بود که وقت می‌برد:
 * برای ۳۲۰×۲۴۰ یعنی ۷۶۸۰۰ پیکسل در هر فریم.
 *
 * DMA2D دقیقاً برای همین ساخته شده — ورودی YCbCr را مستقیم می‌گیرد
 * و با up-sampling کروما به RGB تبدیل می‌کند، بدون دخالت CPU.
 *
 * این فایل روی مثال ۲۰ سوار می‌شود، پس hw_jpeg.h باید
 * قبل از آن include شود (خط FM-REQUIRES پایین همین را به
 * tools/check_examples.sh می‌گوید تا جفتی کامپایل‌شان کند).
 *
 * FM-REQUIRES: simple_video.h hw_jpeg.h
 *
 * استفاده — فقط یک خط:
 *
 *   #include "hw_jpeg.h"
 *   #include "dma2d_ycbcr.h"
 *
 *   MX_DMA2D_Init();          // یا حتی این هم لازم نیست، پایین را ببینید
 *   SimpleImage_UseDma2d();   // ← همین
 *
 * بعد از این، هر جا JPEG سخت‌افزاری خروجی YCbCr بدهد، تبدیل رنگ روی
 * DMA2D می‌رود. اگر DMA2D در دسترس نباشد یا این تابع را صدا نزنید،
 * همان مسیر نرم‌افزاری قبلی کار می‌کند — هیچ چیزی نمی‌شکند.
 *
 * MX_DMA2D_Init() اختیاری است: اگر هندل ندهید (SimpleImage_UseDma2d با
 * NULL یا اصلاً بدون CubeMX)، این فایل خودش یک هندل محلی می‌سازد و
 * کلاک را روشن می‌کند. اگر CubeMX هندل ساخته، آن را بدهید:
 *
 *   SimpleImage_UseDma2dHandle(&hdma2d);
 *
 * محدودیت‌های سخت‌افزار (AN4996) که این فایل خودش هندل می‌کند:
 *   - واحد JPEG همیشه MCU کامل بیرون می‌دهد، پس عرض/ارتفاع به مضرب
 *     ۱۶ (یا ۸ برای 4:4:4) گرد می‌شوند. برش با FGOR انجام می‌شود.
 *   - ارتفاع گردشده اضافه است و صرفاً نادیده گرفته می‌شود.
 *   - grayscale ورودی YCbCr نیست؛ DMA2D آن را نمی‌گیرد و به‌صورت
 *     خودکار به مسیر نرم‌افزاری برمی‌گردد.
 */

#ifndef DMA2D_YCBCR_H_
#define DMA2D_YCBCR_H_

#include <string.h>
#include "fm_board_config.h"
#include "file_manager.h"
#include "fm_image.h"
#include "fm_cache.h"
#include "fm_log.h"

#ifndef SI_DMA2D_TIMEOUT_MS
#define SI_DMA2D_TIMEOUT_MS 100u
#endif

/*
 * هندل DMA2D. یا کاربر می‌دهد (CubeMX) یا خودمان می‌سازیم.
 * si_dma2d_ready وقتی ۱ می‌شود که HAL_DMA2D_Init موفق شده باشد.
 */
static DMA2D_HandleTypeDef *si_dma2d;
static DMA2D_HandleTypeDef si_dma2d_own;
static uint8_t si_dma2d_ready;

/*
 * صبر برای پایان انتقال، بدون گرسنه‌گذاشتن صدا.
 *
 * HAL_DMA2D_PollForTransfer یک حلقهٔ بستهٔ مسدودکننده است و در آن
 * FM_Idle() صدا زده نمی‌شود. مسیر نرم‌افزاریِ قبلی یک بار در هر سطر
 * MCU صدا می‌زد و برای همین صدا سالم می‌ماند. روی فریم بزرگ
 * (۷۲۰×۴۸۰) همان چند میلی‌ثانیه کافی است که رینگ I2S ته بکشد —
 * i2s_starve=1 در لاگ برد دقیقاً همین بود.
 *
 * پس با گام‌های کوتاه صبر می‌کنیم و بین گام‌ها به کتابخانه اجازهٔ
 * پُرکردن بافر صدا می‌دهیم. FM_Idle خودش در برابر ورود مجدد محافظت
 * شده است.
 */
#ifndef SI_DMA2D_POLL_SLICE_MS
#define SI_DMA2D_POLL_SLICE_MS 1u
#endif

static FM_Result si_dma2d_wait(void)
{
    uint32_t waited = 0u;

    for (;;)
    {
        if (HAL_DMA2D_PollForTransfer(si_dma2d,
                                      SI_DMA2D_POLL_SLICE_MS) == HAL_OK)
        {
            return FM_OK;
        }
        waited += SI_DMA2D_POLL_SLICE_MS;
        if (waited >= SI_DMA2D_TIMEOUT_MS)
        {
            return FM_ERR_TIMEOUT;
        }
        FM_Idle();   /* صدا را زنده نگه دار */
    }
}


/*
 * پیکربندی DMA2D برای یک فریم.
 *
 * چرا هر بار دوباره config می‌کنیم: CSS و FGOR به ابعاد و نوع
 * زیرنمونه‌برداری همین فریم بستگی دارند، و در یک پخش ممکن است عکس
 * بعدی 4:2:2 باشد در حالی که قبلی 4:2:0 بود. هزینه‌اش چند نوشتن
 * رجیستر است، در برابر ده‌ها میلی‌ثانیه‌ای که صرفه‌جویی می‌شود.
 */
static FM_Result si_dma2d_convert(const JPEG_ConfTypeDef *cfg,
                                  const uint8_t *ycbcr, size_t ycbcr_bytes,
                                  uint8_t *rgb, FM_PixelFormat out_fmt)
{
    uint32_t css;
    uint32_t align;
    uint32_t in_off;
    uint32_t w;
    uint32_t h;
    size_t out_bpp;

    if (si_dma2d == NULL || si_dma2d_ready == 0u ||
        cfg == NULL || ycbcr == NULL || rgb == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    /* grayscale اصلاً YCbCr نیست — DMA2D نمی‌تواند، بگذار نرم‌افزار. */
    if (cfg->ColorSpace == JPEG_GRAYSCALE_COLORSPACE)
    {
        return FM_ERR_UNSUPPORTED;
    }
    w = cfg->ImageWidth;
    h = cfg->ImageHeight;
    if (w == 0u || h == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    /*
     * CSS و گِردکردن عرض. واحد JPEG سطرها را تا مرز MCU پُر می‌کند؛
     * FGOR به DMA2D می‌گوید ته هر سطر چند پیکسل را بپرد، که همان
     * برش تصویر به عرض واقعی است.
     */
    switch (cfg->ChromaSubsampling)
    {
    case JPEG_420_SUBSAMPLING:
        css = DMA2D_CSS_420;
        align = 16u;
        break;
    case JPEG_422_SUBSAMPLING:
        css = DMA2D_CSS_422;
        align = 16u;
        break;
    case JPEG_444_SUBSAMPLING:
        css = DMA2D_NO_CSS;
        align = 8u;
        break;
    default:
        return FM_ERR_UNSUPPORTED;
    }
    in_off = w % align;
    if (in_off != 0u)
    {
        in_off = align - in_off;
    }

    if (out_fmt == FM_PIXEL_RGB565)
    {
        si_dma2d->Init.ColorMode = DMA2D_OUTPUT_RGB565;
        out_bpp = 2u;
    }
    else
    {
        si_dma2d->Init.ColorMode = DMA2D_OUTPUT_RGB888;
        out_bpp = 3u;
    }
    si_dma2d->Init.Mode = DMA2D_M2M_PFC;
    si_dma2d->Init.OutputOffset = 0u;
    si_dma2d->Init.BytesSwap = DMA2D_BYTES_REGULAR;
    si_dma2d->Init.LineOffsetMode = DMA2D_LOM_PIXELS;

    si_dma2d->LayerCfg[1].InputColorMode = DMA2D_INPUT_YCBCR;
    si_dma2d->LayerCfg[1].ChromaSubSampling = css;
    si_dma2d->LayerCfg[1].InputOffset = in_off;
    si_dma2d->LayerCfg[1].AlphaMode = DMA2D_REPLACE_ALPHA;
    si_dma2d->LayerCfg[1].InputAlpha = 0xFFu;
    si_dma2d->LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
    si_dma2d->LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;

    if (HAL_DMA2D_Init(si_dma2d) != HAL_OK ||
        HAL_DMA2D_ConfigLayer(si_dma2d, 1) != HAL_OK)
    {
        return FM_ERR_IO;
    }

    /*
     * DMA2D روی باس دیگری است و بافر YCbCr را CPU/JPEG تازه نوشته.
     * این تابع با کش خاموش هم درست است: خودش فقط __DSB() می‌زند.
     */
    /*
     * فقط همان چیزی که واقعاً بافر است.
     *
     * قبلاً (w + in_off) * h * 2 + 384 حساب می‌شد که برای عرض‌های
     * غیرمضرب ۱۶ از انتهای بافر می‌زد بیرون — با کش روشن یعنی
     * دست‌زدن به خطوط کشِ حافظهٔ همسایه. اندازهٔ واقعی را خود
     * فراخوان می‌داند و حالا پاس می‌دهد.
     */
    {
        size_t span = (size_t)(w + in_off) * h * 2u + 384u;

        if (span > ycbcr_bytes)
        {
            span = ycbcr_bytes;
        }
        FM_Cache_CleanForDma(ycbcr, span);
    }

    if (HAL_DMA2D_Start(si_dma2d, (uint32_t)(uintptr_t)ycbcr,
                        (uint32_t)(uintptr_t)rgb, w, h) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    if (si_dma2d_wait() != FM_OK)
    {
        FM_Log("SimpleImage: DMA2D timeout, falling back to software\n");
        return FM_ERR_TIMEOUT;
    }
    /* حالا CPU می‌خواهد این پیکسل‌ها را بخواند. */
    FM_Cache_InvalidateAfterDma(rgb, (size_t)w * h * out_bpp);
    return FM_OK;
}

/*
 * RGB888 → RGB565 روی DMA2D.
 *
 * این همان چیزی است که در پروفایل با نام convert دیده می‌شود. با
 * DMA2D هم تبدیل و هم جابه‌جایی R/B و هم برعکس‌کردن بایت‌ها (که
 * نمایشگر SPI لازم دارد) در یک گذر و بدون CPU انجام می‌شود.
 */
static FM_Result si_dma2d_rgb565(const uint8_t *rgb, uint8_t *dst,
                                 uint16_t w, uint16_t h,
                                 uint8_t bgr, uint8_t swap)
{
    if (si_dma2d == NULL || si_dma2d_ready == 0u ||
        rgb == NULL || dst == NULL || w == 0u || h == 0u)
    {
        return FM_ERR_NOT_READY;
    }
    si_dma2d->Init.Mode = DMA2D_M2M_PFC;
    si_dma2d->Init.ColorMode = DMA2D_OUTPUT_RGB565;
    si_dma2d->Init.OutputOffset = 0u;
    si_dma2d->Init.BytesSwap = swap ? DMA2D_BYTES_SWAP : DMA2D_BYTES_REGULAR;
    si_dma2d->Init.LineOffsetMode = DMA2D_LOM_PIXELS;

    si_dma2d->LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB888;
    si_dma2d->LayerCfg[1].ChromaSubSampling = DMA2D_NO_CSS;
    si_dma2d->LayerCfg[1].InputOffset = 0u;
    si_dma2d->LayerCfg[1].AlphaMode = DMA2D_REPLACE_ALPHA;
    si_dma2d->LayerCfg[1].InputAlpha = 0xFFu;
    si_dma2d->LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
    si_dma2d->LayerCfg[1].RedBlueSwap = bgr ? DMA2D_RB_SWAP : DMA2D_RB_REGULAR;

    if (HAL_DMA2D_Init(si_dma2d) != HAL_OK ||
        HAL_DMA2D_ConfigLayer(si_dma2d, 1) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    FM_Cache_CleanForDma(rgb, (size_t)w * h * 3u);
    if (HAL_DMA2D_Start(si_dma2d, (uint32_t)(uintptr_t)rgb,
                        (uint32_t)(uintptr_t)dst, w, h) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    if (si_dma2d_wait() != FM_OK)
    {
        return FM_ERR_TIMEOUT;
    }
    FM_Cache_InvalidateAfterDma(dst, (size_t)w * h * 2u);
    return FM_OK;
}

/* هندل CubeMX خودتان را بدهید. */
static inline FM_Result SimpleImage_UseDma2dHandle(DMA2D_HandleTypeDef *h)
{
    if (h == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    si_dma2d = h;
    si_dma2d_ready = 1u;
    si_color_hw = si_dma2d_convert;   /* YCbCr → RGB روی DMA2D */
#ifdef SIMPLE_VIDEO_H_
    sv_rgb565_hw = si_dma2d_rgb565;   /* RGB888 → RGB565 هم روی DMA2D */
#endif
    FM_Log("SimpleImage: DMA2D active for YCbCr conversion\n");
    return FM_OK;
}

/*
 * بدون CubeMX هم کار می‌کند: کلاک را روشن می‌کند و هندل خودش را
 * می‌سازد. اگر DMA2D نبود، خطا برمی‌گرداند و مسیر نرم‌افزاری می‌ماند.
 */
static inline FM_Result SimpleImage_UseDma2d(void)
{
    __HAL_RCC_DMA2D_CLK_ENABLE();
    memset(&si_dma2d_own, 0, sizeof(si_dma2d_own));
    /*
     * Instance یک اشاره‌گر است، نه عدد. قبلاً اینجا یک آدرس خام
     * 0x52001000 ریخته می‌شد که هم هشدار int-to-pointer می‌داد و هم
     * خلاف این قاعدهٔ پروژه بود که آدرس خام در کتابخانه ننویسیم.
     *
     * DMA2D را خود CMSIS تعریف می‌کند. اگر تعریف نشده باشد یعنی این
     * هستهٔ DMA2D ندارد، پس خطا می‌دهیم و مسیر نرم‌افزاری می‌ماند —
     * که دقیقاً رفتار درست است.
     */
#ifdef DMA2D
    si_dma2d_own.Instance = DMA2D;
#else
    return FM_ERR_UNSUPPORTED;
#endif
    si_dma2d_own.Init.Mode = DMA2D_M2M_PFC;
    si_dma2d_own.Init.ColorMode = DMA2D_OUTPUT_RGB888;
    si_dma2d_own.LayerCfg[1].InputColorMode = DMA2D_INPUT_YCBCR;
    if (HAL_DMA2D_Init(&si_dma2d_own) != HAL_OK)
    {
        FM_Log("SimpleImage: DMA2D init failed, using software conversion\n");
        return FM_ERR_IO;
    }
    return SimpleImage_UseDma2dHandle(&si_dma2d_own);
}

#endif /* DMA2D_YCBCR_H_ */
