/*
 * Example 23 — STM32H7 JPEG hardware codec → SimpleVideo (MJPEG frames)
 *
 * دیکود نرم‌افزاری MJPEG (picojpeg) گلوگاه FPS ویدیو است. همان
 * سخت‌افزار JPEG که برای عکس‌ها فعال کرده‌اید (مثال 20) می‌تواند
 * فریم‌های ویدیوی MJPEG را هم دیکود کند — چند برابر سریع‌تر.
 *
 * استفاده (بعد از مثال 20):
 *
 *   #include "simple_image.h"
 *   #include "hw_jpeg.h"
 *   #include "simple_video.h"
 *   #include "hw_jpeg_video.h"
 *
 *   MX_JPEG_Init();
 *   SimpleImage_UseHwJpeg(&hjpeg);   // عکس‌ها (مثال 20)
 *   SimpleVideo_UseHwJpeg();         // ویدیوهای MJPEG — همین یک خط
 *
 * از این به بعد Video_DecodeFrame برای هر فریم MJPEG اول سراغ
 * سخت‌افزار می‌رود (HWDecoder_Register). اگر فریمی دیکود نشود
 * (سایز بزرگ‌تر از بافر YCbCr و ...) پلیر خودش خطا را لاگ می‌کند.
 *
 * توجه: این هدر به توابع مثال 20 (si_hjpeg / si_hw_ycbcr /
 * si_jpeg_mcus_to_rgb) تکیه دارد — حتماً 20 را قبل از این include کنید.
 */

#ifndef FM_EXAMPLE_23_HW_JPEG_VIDEO_H
#define FM_EXAMPLE_23_HW_JPEG_VIDEO_H


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
    FM_ImageInfo inf;
    size_t ysz;
    HAL_StatusTypeDef hs;
    FM_Result r;

    (void)ctx;
    if (si_hjpeg == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    r = Image_ProbeJPEG(compressed, size, &inf);
    if (r != FM_OK)
    {
        return r;
    }
    if ((size_t)inf.width * inf.height * 3u > frame_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    ysz = (size_t)inf.width * inf.height * 2u + 384u;
    if (ysz > sizeof(si_hw_ycbcr))
    {
        return FM_ERR_TOO_LARGE;
    }
    hs = HAL_JPEG_Decode(si_hjpeg, (uint8_t *)(uintptr_t)compressed,
                         (uint32_t)size, si_hw_ycbcr, (uint32_t)ysz,
                         SIMPLE_IMAGE_HW_JPEG_TIMEOUT_MS);
    if (hs != HAL_OK)
    {
        return FM_ERR_IO;
    }
    memset(&cfg, 0, sizeof(cfg));
    if (HAL_JPEG_GetInfo(si_hjpeg, &cfg) != HAL_OK || cfg.ImageWidth == 0u)
    {
        cfg.ImageWidth = inf.width;
        cfg.ImageHeight = inf.height;
        cfg.ColorSpace = JPEG_YCBCR_COLORSPACE;
        cfg.ChromaSubsampling = JPEG_420_SUBSAMPLING;
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
        return FM_OK;   /* همان دلیل مثال ۲۰: لاگ را پر نکن */
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

#endif /* FM_EXAMPLE_23_HW_JPEG_VIDEO_H */
