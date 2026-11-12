/*
 * hw_jpeg_video.h — STM32H7 JPEG hardware codec → SimpleVideo (MJPEG frames)
 *
 * دیکود نرم‌افزاری MJPEG (picojpeg) گلوگاه FPS ویدیو است. همان
 * سخت‌افزار JPEG که برای عکس‌ها فعال کرده‌اید (hw_jpeg.h) می‌تواند
 * فریم‌های ویدیوی MJPEG را هم دیکود کند — چند برابر سریع‌تر.
 *
 * استفاده (بعد از hw_jpeg.h):
 *
 *   #include "simple_image.h"
 *   #include "hw_jpeg.h"
 *   #include "simple_video.h"
 *   #include "hw_jpeg_video.h"
 *
 *   MX_JPEG_Init();
 *   SimpleImage_UseHwJpeg(&hjpeg);   // عکس‌ها (hw_jpeg.h)
 *   SimpleVideo_UseHwJpeg();         // ویدیوهای MJPEG — همین یک خط
 *
 * از این به بعد Video_DecodeFrame برای هر فریم MJPEG اول سراغ
 * سخت‌افزار می‌رود (HWDecoder_Register). اگر فریمی دیکود نشود
 * (سایز بزرگ‌تر از بافر YCbCr و ...) پلیر خودش خطا را لاگ می‌کند.
 *
 * توجه: این هدر به توابع hw_jpeg.h (si_hjpeg / si_hw_ycbcr /
 * si_jpeg_mcus_to_rgb) تکیه دارد — حتماً 20 را قبل از این include کنید.
 */

#ifndef FM_TOPAPI_HW_JPEG_VIDEO_H
#define FM_TOPAPI_HW_JPEG_VIDEO_H


#ifndef HW_JPEG_VIDEO_H_
#define HW_JPEG_VIDEO_H_

#include "file_manager.h"
#include "fm_hw.h"
#include "fm_image.h"

#ifndef HW_JPEG_H_
#error "include hw_jpeg.h before hw_jpeg_video.h"
#endif

static FM_Result sv_hw_mjpeg_init(void **ctx, const FM_VideoInfo *info)
{
    (void)info;
    if (si_hjpeg == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    *ctx = (void *)1; /* no per-stream state; non-NULL marks "open" */
    return FM_OK;
}

static FM_Result sv_hw_mjpeg_decode(void *ctx, const void *compressed,
                                    size_t size, void *frame_out,
                                    size_t frame_cap)
{
    JPEG_ConfTypeDef cfg;
    size_t ysz = 0;
    FM_Result r;

    (void)ctx;
    /* the same sizing and the same header reading as a still picture:
     * a 4:4:4 or 4:2:2 MJPEG frame gets the buffer it needs */
    r = si_hw_jpeg_to_mcus((const uint8_t *)compressed, size, frame_cap, 0,
                           &cfg, &ysz);
    if (r != FM_OK)
    {
        return r;
    }
    /* pump audio DMA once per frame even on the fast path */
    FM_Idle();
    return si_jpeg_mcus_to_rgb(&cfg, si_hw_ycbcr, ysz, (uint8_t *)frame_out);
}

static void sv_hw_mjpeg_close(void *ctx)
{
    (void)ctx;
}

static uint8_t sv_hw_mjpeg_announced;

static const FM_VideoCodecOps sv_hw_mjpeg_ops = {
    "STM32-JPEG",
    sv_hw_mjpeg_init,
    sv_hw_mjpeg_decode,
    sv_hw_mjpeg_close
};

/* One line: MJPEG video frames decode on the H7 JPEG peripheral. */
static inline FM_Result SimpleVideo_UseHwJpeg(void)
{
    FM_Result r;

    if (si_hjpeg == NULL)
    {
        FM_Log("SimpleVideo: HW JPEG NOT active — call "
               "SimpleImage_UseHwJpeg(&hjpeg) first\n");
        return FM_ERR_NOT_READY;
    }
    if (sv_hw_mjpeg_announced)
    {
        return FM_OK;   /* همان دلیل hw_jpeg.h: لاگ را پر نکن */
    }
    r = HWDecoder_Register(FM_VIDEO_CODEC_MJPEG, &sv_hw_mjpeg_ops);
    /*
     * Say it in words.  This used to be printed as a bare return code,
     * and "hwjpeg vid=0" reads like a failure when 0 is FM_OK -- the
     * accelerator was working the whole time.
     */
    if (r == FM_OK)
    {
        sv_hw_mjpeg_announced = 1u;
        FM_Log("SimpleVideo: HW JPEG active for MJPEG\n");
    }
    else
    {
        FM_Log("SimpleVideo: HW JPEG registration failed: %s (%d)\n",
               File_GetErrorString(r), (int)r);
    }
    return r;
}

#endif /* HW_JPEG_VIDEO_H_ */

#endif /* FM_TOPAPI_HW_JPEG_VIDEO_H */
