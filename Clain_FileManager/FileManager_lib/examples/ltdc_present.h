/*
 * ltdc_present.h — نمایش ویدیو/عکس روی LTDC (نمایشگر RGB موازی)
 *
 * چرا این فایل وجود دارد:
 *
 * با نمایشگر SPI، هر فریم باید بایت‌به‌بایت از حافظه به نمایشگر
 * فرستاده شود. برای ۳۲۰×۲۴۰ در RGB565 این ۱۵۳۶۰۰ بایت است و روی
 * SPI حدود ۳۷ میلی‌ثانیه طول می‌کشد — یعنی سقف حدود ۲۷ فریم بر
 * ثانیه، هر چقدر هم که رمزگشایی سریع باشد. برای ۸۰۰×۴۸۰ همین عدد
 * می‌شود ~۱۸۵ms یعنی ۵ فریم بر ثانیه. این محدودیت پهنای‌باند SPI
 * است، نه پردازنده.
 *
 * LTDC اصلاً چیزی «نمی‌فرستد». خودش مستقیم از SDRAM می‌خواند و پیکسل
 * را به پنل می‌دهد، مستقل از پردازنده. پس نمایش یک فریم یعنی فقط
 * گفتن آدرس بافر جدید به LTDC — یک نوشتن رجیستر. زمان present عملاً
 * صفر می‌شود و آنچه می‌ماند فقط رمزگشایی است.
 *
 * استفاده — همان یک خط همیشگی:
 *
 *   #include "simple_video.h"
 *   #include "ltdc_present.h"
 *
 *   MX_LTDC_Init();
 *   SimpleVideo_UseLtdc(&hltdc);      // ← همین
 *   SimpleVideo_Begin();
 *
 * برای عکس هم همان‌طور:
 *
 *   SimpleImage_SetPresent(Ltdc_Present);
 *
 * FM-REQUIRES: simple_video.h
 *
 * -------------------------------------------------------------------
 * نکتهٔ مهم دربارهٔ اندازه
 * -------------------------------------------------------------------
 * با LTDC معمولاً نمایشگر بزرگ‌تری دارید. یادتان باشد
 * SIMPLE_VIDEO_MAX_W/H را در fm_board_config.h به اندازهٔ پنل زیاد
 * کنید، وگرنه کتابخانه مجبور می‌شود فریم را کوچک کند و مقیاس‌دهی
 * روی CPU انجام می‌شود و گران است. اگر بافر به‌اندازه باشد،
 * مقیاس‌دهی کلاً حذف می‌شود.
 *
 * حافظهٔ لازم = MAX_W * MAX_H * 2 * SIMPLE_VIDEO_FRAME_COUNT بایت
 * برای RGB565. مثلاً ۸۰۰×۴۸۰ با دو بافر = حدود ۱.۵ مگابایت.
 *
 * -------------------------------------------------------------------
 * پارگی تصویر (tearing)
 * -------------------------------------------------------------------
 * چون بافر در همان لحظه‌ای عوض می‌شود که LTDC دارد می‌خواند، اگر
 * وسط یک فریم عوض شود نصف تصویر قدیم و نصف جدید دیده می‌شود. برای
 * همین آدرس با HAL_LTDC_Reload(..., LTDC_RELOAD_VERTICAL_BLANKING)
 * اعمال می‌شود، یعنی سخت‌افزار صبر می‌کند تا بین دو فریم برسد. این
 * کار منتظر نمی‌ماند و پردازنده را بلوکه نمی‌کند.
 */

#ifndef LTDC_PRESENT_H_
#define LTDC_PRESENT_H_

#include <string.h>
#include "fm_board_config.h"
#include "file_manager.h"
#include "fm_image.h"
#include "fm_cache.h"
#include "fm_log.h"

/*
 * LTDC مدام از بافری که آدرسش را داده‌ایم می‌خواند — بر خلاف SPI که
 * یک‌بار می‌فرستد و تمام. اگر فقط یک بافر باشد، رمزگشا همان بافری را
 * بازنویسی می‌کند که همان لحظه روی پنل است و تصویر دائماً پاره
 * می‌شود. پس دو بافر اینجا اختیاری نیست.
 */
#if defined(SIMPLE_VIDEO_FRAME_COUNT) && (SIMPLE_VIDEO_FRAME_COUNT < 2u)
#error "LTDC نیاز به SIMPLE_VIDEO_FRAME_COUNT = 2 دارد (تک‌بافر پاره می‌شود)"
#endif

static LTDC_HandleTypeDef *sv_ltdc;
static uint32_t sv_ltdc_layer = LTDC_LAYER_1;
static uint8_t sv_ltdc_announced;

/* FM_PixelFormat → ثابت LTDC. صفر یعنی این فرمت را LTDC ندارد. */
static uint32_t sv_ltdc_pixfmt(FM_PixelFormat f, uint32_t *ok)
{
    *ok = 1u;
    switch (f)
    {
    case FM_PIXEL_RGB565:
        return LTDC_PIXEL_FORMAT_RGB565;
    case FM_PIXEL_RGB888:
        return LTDC_PIXEL_FORMAT_RGB888;
    case FM_PIXEL_RGBA8888:
        return LTDC_PIXEL_FORMAT_ARGB8888;
    default:
        break;
    }
    *ok = 0u;
    return LTDC_PIXEL_FORMAT_RGB565;
}

/*
 * همان امضای SimpleVideo_PresentFn و SimpleImage_PresentFn.
 *
 * هیچ کپی‌ای انجام نمی‌شود: بافری که رمزگشا پر کرده همان بافری است
 * که LTDC می‌خواند. کل کار، دادن آدرسش به سخت‌افزار است.
 */
static void Ltdc_Present(const void *pixels, uint16_t width, uint16_t height,
                         FM_PixelFormat format, size_t bytes)
{
    uint32_t fmt;
    uint32_t ok = 0u;

    if (sv_ltdc == NULL || pixels == NULL || width == 0u || height == 0u)
    {
        return;
    }
    fmt = sv_ltdc_pixfmt(format, &ok);
    if (!ok)
    {
        /*
         * فرمتی که LTDC نمی‌فهمد. بی‌صدا چیز غلط نشان نمی‌دهیم؛
         * یک بار می‌گوییم و رد می‌شویم. با SIMPLE_VIDEO_FORMAT =
         * FM_PIXEL_RGB565 (پیش‌فرض) اینجا هرگز نمی‌رسیم.
         */
        FM_Log("Ltdc: pixel format %d not supported by LTDC — "
               "set SIMPLE_VIDEO_FORMAT to FM_PIXEL_RGB565\n", (int)format);
        return;
    }

    /*
     * پیکسل‌ها را CPU (یا DMA2D) تازه نوشته و LTDC از باس دیگری
     * می‌خواند. با کش خاموش این فقط یک بریر است، که لازم است.
     */
    FM_Cache_CleanForDma(pixels, bytes);

    (void)HAL_LTDC_SetPixelFormat(sv_ltdc, fmt, sv_ltdc_layer);
    (void)HAL_LTDC_SetAddress(sv_ltdc, (uint32_t)(uintptr_t)pixels,
                              sv_ltdc_layer);
    /*
     * اعمال در بازهٔ خاموشی عمودی: سخت‌افزار خودش لحظهٔ درست را
     * انتخاب می‌کند تا تصویر پاره نشود. این تابع منتظر نمی‌ماند.
     */
    (void)HAL_LTDC_Reload(sv_ltdc, LTDC_RELOAD_VERTICAL_BLANKING);
}

/*
 * یک خط برای وصل‌کردن LTDC به پخش‌کنندهٔ ویدیو.
 *
 * چیزی برای انتظار ثبت نمی‌کنیم: بر خلاف SPI که باید تا پایان انتقال
 * صبر کرد، اینجا انتقالی در کار نیست.
 */
static inline FM_Result SimpleVideo_UseLtdc(LTDC_HandleTypeDef *h)
{
    if (h == NULL)
    {
        FM_Log("SimpleVideo: LTDC handle is NULL (is MX_LTDC_Init() "
               "called?)\n");
        return FM_ERR_INVALID_ARGUMENT;
    }
    sv_ltdc = h;
#ifdef SIMPLE_VIDEO_H_
    SimpleVideo_SetPresent(Ltdc_Present);
    SimpleVideo_SetPresentWait(NULL);   /* چیزی برای drain کردن نیست */
    /*
     * جابه‌جایی بایت‌های ۱۶بیتی را خاموش کن.
     *
     * SIMPLE_VIDEO_SWAP16 پیش‌فرض ۱ است چون نمایشگرهای SPI بایت
     * بالا را اول می‌خواهند. LTDC مستقیم از حافظه می‌خواند و ترتیب
     * طبیعی little-endian را می‌خواهد؛ اگر جابه‌جا بماند رنگ‌ها
     * به‌هم می‌ریزند. این دامی است که کاربر نباید خودش کشف کند، پس
     * همین‌جا درستش می‌کنیم.
     *
     * اگر عمداً چیز دیگری می‌خواهید، بعد از این تابع
     * SimpleVideo_SetSwap16() را صدا بزنید.
     */
    SimpleVideo_SetSwap16(0);
#endif
    if (!sv_ltdc_announced)
    {
        sv_ltdc_announced = 1u;
        FM_Log("SimpleVideo: LTDC present active (no per-frame copy)\n");
    }
    return FM_OK;
}

/* اگر لایهٔ ۲ را می‌خواهید (پیش‌فرض لایهٔ ۱ است). */
static inline void SimpleVideo_SetLtdcLayer(uint32_t layer)
{
    sv_ltdc_layer = layer;
}

#endif /* LTDC_PRESENT_H_ */
