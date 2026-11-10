/*
 * Example 20 — STM32H7 JPEG hardware codec → Image_Load / SimpleImage
 *
 * CubeMX: JPEG peripheral را فعال کنید (hjpeg). بعد فقط:
 *
 *   #include "simple_image.h"
 *   #include "hw_jpeg.h"
 *
 *   MX_JPEG_Init();
 *   SimpleImage_UseHwJpeg(&hjpeg);   // یک خط — جایگزین picojpeg
 *   SimpleImage_Begin();
 *
 * Image_Load("/photos/x.jpg") اول سخت‌افزار را امتحان می‌کند
 * (YCbCr → RGB888). اگر فایل روی heap جا نشود، یا عکس بزرگ‌تر از
 * بافر TFT باشد، خودش به picojpeg استریم‌شده برمی‌گردد و کوچک می‌کند —
 * دیگر «out of memory (-14)» روی عکس‌های /photos نمی‌دهد.
 *
 * خروجی MCUهای 4:2:0 / 4:2:2 / 4:4:4 و grayscale پشتیبانی می‌شود.
 * فایل فشرده در Image_SetWorkBuffer (SimpleImage: si_file در
 * .sdram_buffers) و YCbCr هم در .sdram_buffers است — heap لمس نمی‌شود.
 */

#ifndef FM_EXAMPLE_20_HW_JPEG_H
#define FM_EXAMPLE_20_HW_JPEG_H


#ifndef HW_JPEG_H_
#define HW_JPEG_H_

#include <string.h>
#include "fm_board_config.h"   /* آدرس‌ها و پرچم‌های مرکزی */
#include "jpeg.h"
#include "file_manager.h"
#include "fm_image.h"
#include "fm_log.h"

#ifndef SIMPLE_IMAGE_HW_JPEG_TIMEOUT_MS
#define SIMPLE_IMAGE_HW_JPEG_TIMEOUT_MS 2000u
#endif
#ifndef SIMPLE_IMAGE_HW_YCBCR_SECTION
#define SIMPLE_IMAGE_HW_YCBCR_SECTION ".sdram_buffers"
#endif
#ifndef SIMPLE_IMAGE_HW_YCBCR_MAX
#define SIMPLE_IMAGE_HW_YCBCR_MAX (800u * 480u * 2u + 384u)
#endif

static JPEG_HandleTypeDef *si_hjpeg;

/*
 * قلاب تبدیل رنگ. اگر شتاب‌دهنده‌ای ثبت شده باشد (مثلاً DMA2D در
 * مثال ۳۰) اول آن امتحان می‌شود؛ هر خطایی برگرداند، بی‌سروصدا به
 * مسیر نرم‌افزاری پایین برمی‌گردیم. ترتیب include مهم نیست.
 */
typedef FM_Result (*si_color_convert_fn)(const JPEG_ConfTypeDef *cfg,
                                         const uint8_t *ycbcr,
                                         size_t ycbcr_bytes, uint8_t *rgb,
                                         FM_PixelFormat out_fmt);
static si_color_convert_fn si_color_hw;

/* تا وضعیت عوض نشده، دوباره چاپ نکن. */
static uint8_t si_hw_jpeg_announced;

__attribute__((section(SIMPLE_IMAGE_HW_YCBCR_SECTION), aligned(32)))
static uint8_t si_hw_ycbcr[SIMPLE_IMAGE_HW_YCBCR_MAX];

static int si_jpeg_clamp(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void si_jpeg_ycbcr(int y, int cb, int cr, uint8_t *rgb)
{
    int r = y + ((359 * (cr - 128)) >> 8);
    int g = y - ((88 * (cb - 128) + 183 * (cr - 128)) >> 8);
    int b = y + ((453 * (cb - 128)) >> 8);

    rgb[0] = (uint8_t)si_jpeg_clamp(r);
    rgb[1] = (uint8_t)si_jpeg_clamp(g);
    rgb[2] = (uint8_t)si_jpeg_clamp(b);
}

/* Branch-free saturation table for the fast MCU paths below.
 *
 * The per-pixel path above costs three compares/branches per component.
 * With D-Cache disabled and the frame buffers in SDRAM the conversion
 * loop is the real MJPEG bottleneck (the JPEG peripheral itself decodes
 * a 320x240 frame in a few ms), so the hot paths use a 1 KB lookup
 * table in internal SRAM instead: index = value + 384, covering the
 * full [-384, 639] range the YCbCr maths can produce. */
#define SI_JPEG_CLAMP_BIAS 384
#define SI_JPEG_CLAMP_SIZE 1024u

static uint8_t si_jpeg_clamp_tab[SI_JPEG_CLAMP_SIZE];
static uint8_t si_jpeg_clamp_ready;

static void si_jpeg_clamp_init(void)
{
    unsigned i;

    for (i = 0; i < SI_JPEG_CLAMP_SIZE; i++)
    {
        int v = (int)i - SI_JPEG_CLAMP_BIAS;

        si_jpeg_clamp_tab[i] = (uint8_t)((v < 0) ? 0 : ((v > 255) ? 255 : v));
    }
    si_jpeg_clamp_ready = 1u;
}

#define SI_JPEG_CL(v) \
    si_jpeg_clamp_tab[(unsigned)((v) + SI_JPEG_CLAMP_BIAS) & \
                      (SI_JPEG_CLAMP_SIZE - 1u)]

static FM_Result si_jpeg_mcus_to_rgb_sw(const JPEG_ConfTypeDef *cfg,
                                        const uint8_t *mcu, size_t mcu_bytes,
                                        uint8_t *rgb)
{
    uint32_t w = cfg->ImageWidth;
    uint32_t h = cfg->ImageHeight;
    uint32_t x;
    uint32_t y;

    if (cfg->ColorSpace == JPEG_GRAYSCALE_COLORSPACE)
    {
        size_t i;
        size_t n = (size_t)w * h;

        if (mcu_bytes < n)
        {
            return FM_ERR_TOO_LARGE;
        }
        for (i = 0; i < n; i++)
        {
            rgb[i * 3u] = rgb[i * 3u + 1u] = rgb[i * 3u + 2u] = mcu[i];
        }
        return FM_OK;
    }

    if (cfg->ChromaSubsampling == JPEG_420_SUBSAMPLING)
    {
        uint32_t mbw = (w + 15u) / 16u;
        uint32_t mbh = (h + 15u) / 16u;
        uint32_t mx;
        uint32_t my;
        const uint8_t *p = mcu;

        if (mcu_bytes < (size_t)mbw * mbh * 384u)
        {
            return FM_ERR_TOO_LARGE;
        }
        if (!si_jpeg_clamp_ready)
        {
            si_jpeg_clamp_init();
        }
        /* One chroma sample serves a 2x2 luma quad in 4:2:0, so the
         * colour-difference terms are computed once per four pixels
         * instead of once per pixel, and both rows of the quad are
         * written while the chroma values are still in registers. */
        for (my = 0; my < mbh; my++)
        {
            uint32_t ytop = my * 16u;

            FM_Idle(); /* keep audio DMA fed during the row */
            for (mx = 0; mx < mbw; mx++)
            {
                const uint8_t *y00 = p;
                const uint8_t *y01 = p + 64;
                const uint8_t *y10 = p + 128;
                const uint8_t *y11 = p + 192;
                const uint8_t *cb = p + 256;
                const uint8_t *cr = p + 320;
                uint32_t xleft = mx * 16u;
                uint32_t qy;
                uint32_t qx;

                p += 384;
                for (qy = 0; qy < 8u; qy++)
                {
                    uint32_t by = qy * 2u;
                    const uint8_t *lblk;
                    const uint8_t *rblk;
                    uint8_t *row0;
                    uint8_t *row1;
                    uint32_t have_row1;

                    y = ytop + by;
                    if (y >= h)
                    {
                        break;
                    }
                    have_row1 = ((y + 1u) < h) ? 1u : 0u;
                    lblk = (by < 8u) ? y00 : y10;
                    rblk = (by < 8u) ? y01 : y11;
                    row0 = rgb + ((size_t)y * w + xleft) * 3u;
                    row1 = row0 + (size_t)w * 3u;
                    for (qx = 0; qx < 8u; qx++)
                    {
                        uint32_t bx = qx * 2u;
                        const uint8_t *ys;
                        uint32_t yi;
                        int dcb;
                        int dcr;
                        int rd;
                        int gd;
                        int bd;
                        int yv;
                        uint32_t have_x1;
                        uint8_t *o;

                        x = xleft + bx;
                        if (x >= w)
                        {
                            break;
                        }
                        have_x1 = ((x + 1u) < w) ? 1u : 0u;
                        dcb = (int)cb[qy * 8u + qx] - 128;
                        dcr = (int)cr[qy * 8u + qx] - 128;
                        rd = (359 * dcr) >> 8;
                        gd = -((88 * dcb + 183 * dcr) >> 8);
                        bd = (453 * dcb) >> 8;
                        ys = (bx < 8u) ? lblk : rblk;
                        yi = (by & 7u) * 8u + (bx & 7u);

                        o = row0 + (size_t)bx * 3u;
                        yv = (int)ys[yi];
                        o[0] = SI_JPEG_CL(yv + rd);
                        o[1] = SI_JPEG_CL(yv + gd);
                        o[2] = SI_JPEG_CL(yv + bd);
                        if (have_x1)
                        {
                            yv = (int)ys[yi + 1u];
                            o[3] = SI_JPEG_CL(yv + rd);
                            o[4] = SI_JPEG_CL(yv + gd);
                            o[5] = SI_JPEG_CL(yv + bd);
                        }
                        if (have_row1)
                        {
                            o = row1 + (size_t)bx * 3u;
                            yv = (int)ys[yi + 8u];
                            o[0] = SI_JPEG_CL(yv + rd);
                            o[1] = SI_JPEG_CL(yv + gd);
                            o[2] = SI_JPEG_CL(yv + bd);
                            if (have_x1)
                            {
                                yv = (int)ys[yi + 9u];
                                o[3] = SI_JPEG_CL(yv + rd);
                                o[4] = SI_JPEG_CL(yv + gd);
                                o[5] = SI_JPEG_CL(yv + bd);
                            }
                        }
                    }
                }
            }
        }
        return FM_OK;
    }

    if (cfg->ChromaSubsampling == JPEG_422_SUBSAMPLING)
    {
        uint32_t mbw = (w + 15u) / 16u;
        uint32_t mbh = (h + 7u) / 8u;
        uint32_t mx;
        uint32_t my;
        const uint8_t *p = mcu;

        if (mcu_bytes < (size_t)mbw * mbh * 256u)
        {
            return FM_ERR_TOO_LARGE;
        }
        if (!si_jpeg_clamp_ready)
        {
            si_jpeg_clamp_init();
        }
        /* 4:2:2: one chroma sample per horizontal pair of luma samples. */
        for (my = 0; my < mbh; my++)
        {
            FM_Idle(); /* keep audio DMA fed during the row */
            for (mx = 0; mx < mbw; mx++)
            {
                const uint8_t *y0 = p;
                const uint8_t *y1 = p + 64;
                const uint8_t *cb = p + 128;
                const uint8_t *cr = p + 192;
                uint32_t xleft = mx * 16u;
                uint32_t by;
                uint32_t qx;

                p += 256;
                for (by = 0; by < 8u; by++)
                {
                    uint8_t *row;

                    y = my * 8u + by;
                    if (y >= h)
                    {
                        break;
                    }
                    row = rgb + ((size_t)y * w + xleft) * 3u;
                    for (qx = 0; qx < 8u; qx++)
                    {
                        uint32_t bx = qx * 2u;
                        const uint8_t *ys;
                        uint32_t yi;
                        int dcb;
                        int dcr;
                        int rd;
                        int gd;
                        int bd;
                        int yv;
                        uint8_t *o;

                        x = xleft + bx;
                        if (x >= w)
                        {
                            break;
                        }
                        dcb = (int)cb[by * 8u + qx] - 128;
                        dcr = (int)cr[by * 8u + qx] - 128;
                        rd = (359 * dcr) >> 8;
                        gd = -((88 * dcb + 183 * dcr) >> 8);
                        bd = (453 * dcb) >> 8;
                        ys = (bx < 8u) ? y0 : y1;
                        yi = by * 8u + (bx & 7u);

                        o = row + (size_t)bx * 3u;
                        yv = (int)ys[yi];
                        o[0] = SI_JPEG_CL(yv + rd);
                        o[1] = SI_JPEG_CL(yv + gd);
                        o[2] = SI_JPEG_CL(yv + bd);
                        if ((x + 1u) < w)
                        {
                            yv = (int)ys[yi + 1u];
                            o[3] = SI_JPEG_CL(yv + rd);
                            o[4] = SI_JPEG_CL(yv + gd);
                            o[5] = SI_JPEG_CL(yv + bd);
                        }
                    }
                }
            }
        }
        return FM_OK;
    }

    /* 4:4:4 fallback: packed Y,Cb,Cr per pixel if HAL produced it. */
    if (mcu_bytes >= (size_t)w * h * 3u)
    {
        for (y = 0; y < h; y++)
        {
            for (x = 0; x < w; x++)
            {
                size_t i = (size_t)y * w + x;

                si_jpeg_ycbcr((int)mcu[i * 3u], (int)mcu[i * 3u + 1u],
                              (int)mcu[i * 3u + 2u],
                              rgb + i * 3u);
            }
        }
        return FM_OK;
    }
    (void)mcu_bytes;
    return FM_ERR_UNSUPPORTED;
}

/*
 * تبدیل YCbCr → RGB888. سخت‌افزار اول، نرم‌افزار به‌عنوان پشتیبان.
 *
 * هر شکستی در مسیر سخت‌افزاری (ثبت‌نشده، grayscale، زیرنمونه‌برداری
 * ناشناخته، timeout) فقط یعنی می‌افتیم روی حلقهٔ نرم‌افزاری. تصویر
 * در هر دو حالت درست بیرون می‌آید؛ فقط سرعتش فرق می‌کند.
 */
static FM_Result si_jpeg_mcus_to_rgb(const JPEG_ConfTypeDef *cfg,
                                     const uint8_t *mcu, size_t mcu_bytes,
                                     uint8_t *rgb)
{
    if (si_color_hw != NULL)
    {
        if (si_color_hw(cfg, mcu, mcu_bytes, rgb, FM_PIXEL_RGB888) == FM_OK)
        {
            return FM_OK;
        }
    }
    return si_jpeg_mcus_to_rgb_sw(cfg, mcu, mcu_bytes, rgb);
}

static FM_Result si_hw_jpeg_probe(const uint8_t *data, size_t size,
                                  FM_ImageInfo *info)
{
    return Image_ProbeJPEG(data, size, info);
}

static FM_Result si_hw_jpeg_decode(const uint8_t *data, size_t size,
                                   FM_Image *img, void *pixel_buffer,
                                   size_t capacity)
{
    JPEG_ConfTypeDef cfg;
    FM_ImageInfo inf;
    uint8_t *ycbcr;
    size_t ysz;
    HAL_StatusTypeDef hs;
    FM_Result r;

    if (si_hjpeg == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    r = Image_ProbeJPEG(data, size, &inf);
    if (r != FM_OK)
    {
        return r;
    }
    if ((size_t)inf.width * inf.height * 3u > capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    /* YCbCr 4:2:0 worst-case ≈ 1.5 bytes/pixel, round up to 2. */
    ysz = (size_t)inf.width * inf.height * 2u + 384u;
    if (ysz > sizeof(si_hw_ycbcr))
    {
        return FM_ERR_TOO_LARGE;
    }
    ycbcr = si_hw_ycbcr;
    memset(ycbcr, 0, ysz);
    hs = HAL_JPEG_Decode(si_hjpeg, (uint8_t *)data, (uint32_t)size,
                         ycbcr, (uint32_t)ysz,
                         SIMPLE_IMAGE_HW_JPEG_TIMEOUT_MS);
    if (hs != HAL_OK)
    {
        FM_Log("SimpleImage: HAL_JPEG_Decode %d\n", (int)hs);
        return FM_ERR_IO;
    }
    memset(&cfg, 0, sizeof(cfg));
    if (HAL_JPEG_GetInfo(si_hjpeg, &cfg) != HAL_OK)
    {
        cfg.ImageWidth = inf.width;
        cfg.ImageHeight = inf.height;
        cfg.ColorSpace = JPEG_YCBCR_COLORSPACE;
        cfg.ChromaSubsampling = JPEG_420_SUBSAMPLING;
    }
    r = si_jpeg_mcus_to_rgb(&cfg, ycbcr, ysz, (uint8_t *)pixel_buffer);
    if (r != FM_OK)
    {
        return r;
    }
    memset(img, 0, sizeof(*img));
    img->width = (uint16_t)cfg.ImageWidth;
    img->height = (uint16_t)cfg.ImageHeight;
    if (img->width == 0)
    {
        img->width = inf.width;
        img->height = inf.height;
    }
    img->format = FM_PIXEL_RGB888;
    img->channels = 3;
    img->bit_depth = 24;
    img->data = (uint8_t *)pixel_buffer;
    img->data_capacity = capacity;
    img->data_size = (uint64_t)img->width * img->height * 3u;
    (void)strcpy(img->codec, "JPEG");
    return FM_OK;
}

static const FM_ImageCodecOps si_hw_jpeg_ops = {
    "STM32-JPEG",
    si_hw_jpeg_probe,
    si_hw_jpeg_decode
};

static inline FM_Result SimpleImage_UseHwJpeg(JPEG_HandleTypeDef *hjpeg)
{
    FM_Result r;

    if (hjpeg == NULL)
    {
        FM_Log("SimpleImage: HW JPEG NOT active — hjpeg is NULL "
               "(is MX_JPEG_Init() called?)\n");
        return FM_ERR_INVALID_ARGUMENT;
    }
    /*
     * ثبت دوباره با همان هندل بی‌ضرر است، ولی چاپ دوباره نه: اگر این
     * تابع داخل حلقهٔ برنامه صدا زده شود، لاگ پر می‌شود از یک جمله و
     * پیام‌های مهم گم می‌شوند. پس فقط تغییر وضعیت گزارش می‌شود.
     */
    if (si_hjpeg == hjpeg && si_hw_jpeg_announced)
    {
        return FM_OK;
    }
    si_hjpeg = hjpeg;
    r = Image_RegisterCodec("JPEG", &si_hw_jpeg_ops);
    if (r == FM_OK)
    {
        si_hw_jpeg_announced = 1u;
        FM_Log("SimpleImage: HW JPEG active\n");
    }
    else
    {
        FM_Log("SimpleImage: HW JPEG registration failed: %s (%d)\n",
               File_GetErrorString(r), (int)r);
    }
    return r;
}

#endif /* HW_JPEG_H_ */

#endif /* FM_EXAMPLE_20_HW_JPEG_H */
