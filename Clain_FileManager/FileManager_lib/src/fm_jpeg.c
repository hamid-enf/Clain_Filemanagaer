/*
 * FileManager - still-image JPEG (picojpeg software decoder + baseline encode)
 *
 * Decode is streamed from the file (or from a memory blob).  The whole
 * JPEG is never malloc'd — that was the STM32 "out of memory (-14)"
 * failure when Image_Load slurped camera photos onto the tiny heap.
 *
 * Hardware JPEG (STM32 JPEG peripheral) still registers over this via
 * Image_RegisterCodec("JPEG", ...).  File load tries hardware only when
 * the native RGB888 image fits the caller buffer *and* FM_Alloc can hold
 * the compressed file; otherwise picojpeg streams and downscales.
 */

#define FM_DEBUG_TAG "jpeg"
#include "fm_debug.h"
#include "fm_image.h"
#include "fm_internal.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_IMAGE && FM_USE_JPEG

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "picojpeg.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct
{
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    FM_File       *file;
} JpegSrc;

static unsigned char jpeg_need_bytes(unsigned char *buf,
                                     unsigned char buf_size,
                                     unsigned char *read,
                                     void *cb_data)
{
    JpegSrc *s = (JpegSrc *)cb_data;

    if (s->file != NULL)
    {
        uint32_t rd = 0;
        FM_Result r = File_Read(s->file, buf, buf_size, &rd);

        if (r != FM_OK && r != FM_ERR_EOF)
        {
            *read = 0;
            return 1;
        }
        *read = (unsigned char)rd;
        return 0;
    }
    {
        size_t n = s->size - s->pos;

        if (n > buf_size)
        {
            n = buf_size;
        }
        if (n > 0)
        {
            memcpy(buf, s->data + s->pos, n);
            s->pos += n;
        }
        *read = (unsigned char)n;
        return 0;
    }
}

static void jpeg_pixel_from_mcu(const pjpeg_image_info_t *info,
                                int mcu_x, int mcu_y, int sx, int sy,
                                uint8_t *rgb)
{
    int lx = sx - mcu_x * info->m_MCUWidth;
    int ly = sy - mcu_y * info->m_MCUHeight;
    int xblock = (lx / 8) * 8;
    int yblock = (ly / 8) * 8;
    int src_ofs = xblock * 8 + yblock * 16 + (ly % 8) * 8 + (lx % 8);
    const uint8_t *src_r = info->m_pMCUBufR + src_ofs;

    if (info->m_scanType == PJPG_GRAYSCALE)
    {
        rgb[0] = rgb[1] = rgb[2] = *src_r;
    }
    else
    {
        rgb[0] = info->m_pMCUBufR[src_ofs];
        rgb[1] = info->m_pMCUBufG[src_ofs];
        rgb[2] = info->m_pMCUBufB[src_ofs];
    }
}

static void jpeg_blit(const pjpeg_image_info_t *info, int mcu_x, int mcu_y,
                      uint8_t *rgb, int img_w, int img_h)
{
    int x;
    int y;

    for (y = 0; y < info->m_MCUHeight; y += 8)
    {
        int by_limit = img_h - (mcu_y * info->m_MCUHeight + y);

        if (by_limit > 8)
        {
            by_limit = 8;
        }
        for (x = 0; x < info->m_MCUWidth; x += 8)
        {
            int src_ofs = (x * 8) + (y * 16);
            const uint8_t *src_r = info->m_pMCUBufR + src_ofs;
            const uint8_t *src_g = info->m_pMCUBufG + src_ofs;
            const uint8_t *src_b = info->m_pMCUBufB + src_ofs;
            int bx_limit = img_w - (mcu_x * info->m_MCUWidth + x);
            int bx;
            int by;

            if (bx_limit > 8)
            {
                bx_limit = 8;
            }
            if (bx_limit <= 0 || by_limit <= 0)
            {
                continue;
            }
            for (by = 0; by < by_limit; by++)
            {
                int px_base = (mcu_y * info->m_MCUHeight + y + by) * img_w +
                              (mcu_x * info->m_MCUWidth + x);

                for (bx = 0; bx < bx_limit; bx++)
                {
                    uint8_t *dst = rgb + (size_t)(px_base + bx) * 3u;

                    if (info->m_scanType == PJPG_GRAYSCALE)
                    {
                        uint8_t g = src_r[bx];

                        dst[0] = g;
                        dst[1] = g;
                        dst[2] = g;
                    }
                    else
                    {
                        dst[0] = src_r[bx];
                        dst[1] = src_g[bx];
                        dst[2] = src_b[bx];
                    }
                }
                src_r += 8;
                src_g += 8;
                src_b += 8;
            }
        }
    }
}

static void jpeg_blit_scaled(const pjpeg_image_info_t *info, int mcu_x,
                             int mcu_y, uint8_t *rgb, int src_w, int src_h,
                             int dst_w, int dst_h)
{
    int x0 = mcu_x * info->m_MCUWidth;
    int y0 = mcu_y * info->m_MCUHeight;
    int x1 = x0 + info->m_MCUWidth;
    int y1 = y0 + info->m_MCUHeight;
    uint32_t dy;
    uint32_t dy0;
    uint32_t dy1;
    uint32_t dx0;
    uint32_t dx1;
    uint32_t sh = (uint32_t)src_h;
    uint32_t sw = (uint32_t)src_w;
    uint32_t dh = (uint32_t)dst_h;
    uint32_t dw = (uint32_t)dst_w;

    if (x1 > src_w)
    {
        x1 = src_w;
    }
    if (y1 > src_h)
    {
        y1 = src_h;
    }
    if (x0 >= src_w || y0 >= src_h || dw == 0u || dh == 0u)
    {
        return;
    }
    dy0 = (uint32_t)(((uint64_t)(uint32_t)y0 * dh) / sh);
    dy1 = (uint32_t)(((uint64_t)(uint32_t)y1 * dh + sh - 1u) / sh);
    dx0 = (uint32_t)(((uint64_t)(uint32_t)x0 * dw) / sw);
    dx1 = (uint32_t)(((uint64_t)(uint32_t)x1 * dw + sw - 1u) / sw);
    if (dy1 > dh)
    {
        dy1 = dh;
    }
    if (dx1 > dw)
    {
        dx1 = dw;
    }
    for (dy = dy0; dy < dy1; dy++)
    {
        uint32_t sy = (uint32_t)((uint64_t)dy * sh / dh);
        uint32_t dx;

        if (sy < (uint32_t)y0 || sy >= (uint32_t)y1)
        {
            continue;
        }
        for (dx = dx0; dx < dx1; dx++)
        {
            uint32_t sx = (uint32_t)((uint64_t)dx * sw / dw);
            uint8_t *dst;

            if (sx < (uint32_t)x0 || sx >= (uint32_t)x1)
            {
                continue;
            }
            dst = rgb + ((size_t)dy * dw + dx) * 3u;
            jpeg_pixel_from_mcu(info, mcu_x, mcu_y, (int)sx, (int)sy, dst);
        }
    }
}

static uint16_t jpeg_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static FM_Result jpeg_probe_bytes(const uint8_t *p, size_t jpeg_size,
                                  FM_ImageInfo *info)
{
    size_t i;

    if (p == NULL || info == NULL || jpeg_size < 4u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (p[0] != 0xFFu || p[1] != 0xD8u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    memset(info, 0, sizeof(*info));
    i = 2;
    while (i + 4u < jpeg_size)
    {
        uint8_t marker;
        uint16_t len;

        if (p[i] != 0xFFu)
        {
            i++;
            continue;
        }
        while (i < jpeg_size && p[i] == 0xFFu)
        {
            i++;
        }
        if (i >= jpeg_size)
        {
            break;
        }
        marker = p[i++];
        if (marker == 0xD9u || marker == 0xDAu)
        {
            break;
        }
        if (marker == 0xD8u || marker == 0x01u ||
            (marker >= 0xD0u && marker <= 0xD7u))
        {
            continue;
        }
        if (i + 2u > jpeg_size)
        {
            break;
        }
        len = jpeg_be16(p + i);
        if (len < 2u || i + len > jpeg_size)
        {
            break;
        }
        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u)
        {
            if (len < 8u)
            {
                return FM_ERR_CORRUPTED;
            }
            info->height = jpeg_be16(p + i + 3);
            info->width = jpeg_be16(p + i + 5);
            info->bit_depth = 24;
            info->channels = 3;
            info->data_size = (uint64_t)info->width * info->height * 3u;
            (void)strcpy(info->codec, "JPEG");
            return (info->width != 0 && info->height != 0)
                       ? FM_OK : FM_ERR_INVALID_FORMAT;
        }
        i += len;
    }
    return FM_ERR_INVALID_FORMAT;
}

FM_Result Image_ProbeJPEG(const void *jpeg, size_t jpeg_size,
                          FM_ImageInfo *info)
{
    return jpeg_probe_bytes((const uint8_t *)jpeg, jpeg_size, info);
}

static FM_Result jpeg_read_u8(FM_File *f, uint8_t *v)
{
    return FM_Image_ReadExact(f, v, 1);
}

FM_Result FM_JPEG_ProbeFile(FM_File *f, FM_ImageInfo *info)
{
    uint8_t b[2];
    FM_Result r;
    uint32_t budget = (uint32_t)FM_JPEG_PROBE_BUDGET;

    if (f == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_Image_ReadExact(f, b, 2);
    if (r != FM_OK)
    {
        return r;
    }
    if (b[0] != 0xFFu || b[1] != 0xD8u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    memset(info, 0, sizeof(*info));
    for (;;)
    {
        uint8_t marker;
        uint8_t lenb[2];
        uint16_t len;

        /* Every byte read one at a time and every segment costs budget:
         * a file of 0xFF fill, or of millions of 4-byte segments, is not
         * a JPEG worth scanning to the end (on an SD card that is
         * seconds of blocked UI per megabyte). */
        if (budget < 16u)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        budget -= 16u;
        FM_Idle();
        do
        {
            if (budget == 0u)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            budget--;
            r = jpeg_read_u8(f, &b[0]);
            if (r != FM_OK)
            {
                return FM_ERR_INVALID_FORMAT;
            }
        } while (b[0] != 0xFFu);
        do
        {
            if (budget == 0u)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            budget--;
            r = jpeg_read_u8(f, &marker);
            if (r != FM_OK)
            {
                return FM_ERR_INVALID_FORMAT;
            }
        } while (marker == 0xFFu);
        if (marker == 0xD9u || marker == 0xDAu)
        {
            break;
        }
        if (marker == 0xD8u || marker == 0x01u ||
            (marker >= 0xD0u && marker <= 0xD7u))
        {
            continue;
        }
        r = FM_Image_ReadExact(f, lenb, 2);
        if (r != FM_OK)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        len = jpeg_be16(lenb);
        if (len < 2u)
        {
            return FM_ERR_CORRUPTED;
        }
        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u)
        {
            uint8_t sof[6];

            if (len < 8u)
            {
                return FM_ERR_CORRUPTED;
            }
            r = FM_Image_ReadExact(f, sof, 6);
            if (r != FM_OK)
            {
                return r;
            }
            info->height = jpeg_be16(sof + 1);
            info->width = jpeg_be16(sof + 3);
            info->bit_depth = 24;
            info->channels = 3;
            info->data_size = (uint64_t)info->width * info->height * 3u;
            (void)strcpy(info->codec, "JPEG");
            return (info->width != 0 && info->height != 0)
                       ? FM_OK : FM_ERR_INVALID_FORMAT;
        }
        if (File_Seek(f, (int64_t)len - 2, FM_SEEK_CUR) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    return FM_ERR_INVALID_FORMAT;
}

static FM_Result jpeg_sw_decode_run(JpegSrc *src, FM_Image *img,
                                    void *pixel_buffer, size_t capacity,
                                    uint16_t max_w, uint16_t max_h);

/* picojpeg keeps its whole decoder state in file statics, and MJPEG
 * video frames go through the same decoder: one picture at a time. */
static FM_Result jpeg_sw_decode_src(JpegSrc *src, FM_Image *img,
                                    void *pixel_buffer, size_t capacity,
                                    uint16_t max_w, uint16_t max_h)
{
    FM_Result r = FM_CodecScratch_Enter();

    if (r != FM_OK)
    {
        return r;
    }
    r = jpeg_sw_decode_run(src, img, pixel_buffer, capacity, max_w, max_h);
    FM_CodecScratch_Leave();
    return r;
}

static FM_Result jpeg_sw_decode_run(JpegSrc *src, FM_Image *img,
                                    void *pixel_buffer, size_t capacity,
                                    uint16_t max_w, uint16_t max_h)
{
    pjpeg_image_info_t pj;
    unsigned char status;
    int mcu_x = 0;
    int mcu_y = 0;
    uint8_t *rgb = (uint8_t *)pixel_buffer;
    uint16_t dw;
    uint16_t dh;
    FM_Result r;

    status = pjpeg_decode_init(&pj, jpeg_need_bytes, src, 0);
    if (status != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (pj.m_width <= 0 || pj.m_height <= 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    r = FM_Image_FitSize((uint16_t)pj.m_width, (uint16_t)pj.m_height,
                         capacity, 3u, max_w, max_h, &dw, &dh);
    if (r != FM_OK)
    {
        return r;
    }
    for (;;)
    {
        status = pjpeg_decode_mcu();
        if (status != 0)
        {
            if (status != PJPG_NO_MORE_BLOCKS)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            break;
        }
        if (mcu_y >= pj.m_MCUSPerCol)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        if (dw == (uint16_t)pj.m_width && dh == (uint16_t)pj.m_height)
        {
            jpeg_blit(&pj, mcu_x, mcu_y, rgb, pj.m_width, pj.m_height);
        }
        else
        {
            jpeg_blit_scaled(&pj, mcu_x, mcu_y, rgb, pj.m_width, pj.m_height,
                             (int)dw, (int)dh);
        }
        mcu_x++;
        if (mcu_x == pj.m_MCUSPerRow)
        {
            mcu_x = 0;
            mcu_y++;
            FM_Idle();
        }
    }
    memset(img, 0, sizeof(*img));
    img->width = dw;
    img->height = dh;
    img->format = FM_PIXEL_RGB888;
    img->channels = 3;
    img->bit_depth = 24;
    img->data = rgb;
    img->data_capacity = capacity;
    img->data_size = (uint64_t)dw * (uint64_t)dh * 3u;
    (void)strcpy(img->codec, "JPEG");
    return FM_OK;
}

static FM_Result jpeg_sw_decode(const uint8_t *data, size_t size,
                                FM_Image *img, void *pixel_buffer,
                                size_t capacity)
{
    JpegSrc src;

    src.data = data;
    src.size = size;
    src.pos = 0;
    src.file = NULL;
    return jpeg_sw_decode_src(&src, img, pixel_buffer, capacity, 0, 0);
}

FM_Result Image_DecodeJPEG(const void *jpeg, size_t jpeg_size,
                           FM_Image *img, void *pixel_buffer, size_t capacity)
{
    const FM_ImageCodecOps *hw;
    FM_ImageInfo inf;
    uint16_t dw;
    uint16_t dh;
    FM_Result r;

    if (jpeg == NULL || img == NULL || pixel_buffer == NULL || jpeg_size < 4u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_ProbeJPEG(jpeg, jpeg_size, &inf);
    if (r == FM_OK)
    {
        r = FM_Image_FitSize(inf.width, inf.height, capacity, 3u, 0, 0,
                             &dw, &dh);
        if (r != FM_OK)
        {
            return r;
        }
        hw = Image_GetCodec("JPEG");
        if (hw != NULL && hw->decode != NULL &&
            dw == inf.width && dh == inf.height)
        {
            return hw->decode((const uint8_t *)jpeg, jpeg_size, img,
                              pixel_buffer, capacity);
        }
    }
    return jpeg_sw_decode((const uint8_t *)jpeg, jpeg_size, img,
                          pixel_buffer, capacity);
}

FM_Result FM_JPEG_LoadFile(FM_File *f, FM_Image *img, void *pixel_buffer,
                           size_t capacity, uint16_t max_w, uint16_t max_h)
{
    const FM_ImageCodecOps *hw;
    FM_ImageInfo inf;
    uint16_t dw;
    uint16_t dh;
    JpegSrc src;
    FM_Result r;
    uint64_t pos = 0;

    if (f == NULL || img == NULL || pixel_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)File_Tell(f, &pos);
    r = FM_JPEG_ProbeFile(f, &inf);
    if (r != FM_OK)
    {
        return r;
    }
    r = FM_Image_FitSize(inf.width, inf.height, capacity, 3u, max_w, max_h,
                         &dw, &dh);
    if (r != FM_OK)
    {
        return r;
    }
    hw = Image_GetCodec("JPEG");
    if (hw != NULL && hw->decode != NULL &&
        dw == inf.width && dh == inf.height)
    {
        uint64_t sz = 0;
        uint8_t *blob;

        /* The work buffer is one shared block: hold the codec claim
         * while the file sits in it.  When the claim is not available
         * (nested call) the hardware path is skipped and the software
         * decoder below reports FM_ERR_BUSY. */
        if (File_Size(f, &sz) == FM_OK && sz > 0u &&
            sz <= (uint64_t)FM_JPEG_HW_FILE_MAX &&
            (uint64_t)sz <= (uint64_t)FM_IMAGE_MAX_BYTES &&
            FM_CodecScratch_Enter() == FM_OK)
        {
            size_t work_cap = 0;
            uint8_t *work = (uint8_t *)Image_GetWorkBuffer(&work_cap);
            int owned = 0;

            if (work != NULL && (size_t)sz <= work_cap)
            {
                blob = work;
            }
            else
            {
#if FM_JPEG_HW_HEAP
                blob = (uint8_t *)FM_Alloc((size_t)sz);
                owned = 1;
#else
                blob = NULL;    /* no work buffer: software decode below */
#endif
            }
            if (blob != NULL)
            {
                if (File_Seek(f, 0, FM_SEEK_SET) == FM_OK &&
                    FM_Image_ReadExact(f, blob, (uint32_t)sz) == FM_OK)
                {
                    r = hw->decode(blob, (size_t)sz, img, pixel_buffer,
                                   capacity);
                    if (owned)
                    {
                        FM_Free(blob);
                    }
                    if (r == FM_OK)
                    {
                        FM_CodecScratch_Leave();
                        return r;
                    }
                }
                else if (owned)
                {
                    FM_Free(blob);
                }
            }
            FM_CodecScratch_Leave();
        }
    }
    if (File_Seek(f, (int64_t)pos, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    src.data = NULL;
    src.size = 0;
    src.pos = 0;
    src.file = f;
    return jpeg_sw_decode_src(&src, img, pixel_buffer, capacity, max_w, max_h);
}

/* ------------------------------------------------------------------ */
/* Baseline JPEG encode                                                 */
/*                                                                      */
/* JFIF (full-range BT.601 YCbCr), standard Huffman tables, quality     */
/* 1..100 (libjpeg scale), 4:4:4 or 4:2:0 chroma.  The same encoder     */
/* writes still images (Image_Save / Image_EncodeJPEG) and the frames   */
/* of saved video (MJPEG in AVI, fm_video_enc.c).                       */
/* ------------------------------------------------------------------ */

#define JPEG_OUT_BUF 256u

typedef struct
{
    FM_File *file;
    FM_JpegSinkFn sink;
    void    *sink_ctx;
    uint8_t *mem;
    size_t   mem_cap;
    size_t   mem_len;
    uint32_t bits;
    int      nbits;
    FM_Result err;
    uint32_t n;
    uint8_t  buf[JPEG_OUT_BUF];
} JpegOut;

static void jpeg_out_init(JpegOut *o)
{
    o->file = NULL;
    o->sink = NULL;
    o->sink_ctx = NULL;
    o->mem = NULL;
    o->mem_cap = 0u;
    o->mem_len = 0u;
    o->bits = 0u;
    o->nbits = 0;
    o->err = FM_OK;
    o->n = 0u;
}

static const uint8_t k_zigzag[64] = {
    0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t k_q_lum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77, 24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101, 72, 92, 95, 98,112,100,103, 99
};

static const uint8_t k_q_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

/* JPEG Annex K bits/values for DC/AC luminance and chrominance. */
static const uint8_t k_dc_lum_bits[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t k_dc_lum_val[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const uint8_t k_dc_chr_bits[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const uint8_t k_dc_chr_val[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const uint8_t k_ac_lum_bits[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const uint8_t k_ac_lum_val[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};
static const uint8_t k_ac_chr_bits[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const uint8_t k_ac_chr_val[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

typedef struct
{
    uint16_t code[256];
    uint8_t  size[256];
} JpegHuff;

static void jpeg_huff_build(JpegHuff *h, const uint8_t *bits,
                            const uint8_t *val, int nval)
{
    int k = 0;
    uint16_t code = 0;
    int si;

    memset(h, 0, sizeof(*h));
    for (si = 1; si <= 16; si++)
    {
        int n = bits[si];
        int j;

        for (j = 0; j < n && k < nval; j++, k++)
        {
            uint8_t v = val[k];

            h->code[v] = code;
            h->size[v] = (uint8_t)si;
            code++;
        }
        code = (uint16_t)(code << 1);
    }
}

/* The four standard tables, expanded once (3 KB of .bss, no stack). */
static JpegHuff jpeg_h_dc_y;
static JpegHuff jpeg_h_dc_c;
static JpegHuff jpeg_h_ac_y;
static JpegHuff jpeg_h_ac_c;
static uint8_t  jpeg_h_ready;

static void jpeg_huff_init(void)
{
    if (jpeg_h_ready == 0u)
    {
        jpeg_huff_build(&jpeg_h_dc_y, k_dc_lum_bits, k_dc_lum_val, 12);
        jpeg_huff_build(&jpeg_h_dc_c, k_dc_chr_bits, k_dc_chr_val, 12);
        jpeg_huff_build(&jpeg_h_ac_y, k_ac_lum_bits, k_ac_lum_val, 162);
        jpeg_huff_build(&jpeg_h_ac_c, k_ac_chr_bits, k_ac_chr_val, 162);
        jpeg_h_ready = 1u;
    }
}

static void jpeg_out_flush(JpegOut *o)
{
    if (o->n == 0u || o->err != FM_OK)
    {
        o->n = 0u;
        return;
    }
    if (o->sink != NULL)
    {
        o->err = o->sink(o->sink_ctx, o->buf, o->n);
    }
    else if (o->file != NULL)
    {
        uint32_t wr = 0;
        FM_Result r = File_Write(o->file, o->buf, o->n, &wr);

        if (r != FM_OK)
        {
            o->err = r;
        }
        else if (wr != o->n)
        {
            o->err = FM_ERR_IO;
        }
    }
    else if (o->n > o->mem_cap - o->mem_len)
    {
        o->err = FM_ERR_TOO_LARGE;
    }
    else
    {
        memcpy(o->mem + o->mem_len, o->buf, o->n);
        o->mem_len += o->n;
    }
    o->n = 0u;
}

static void jpeg_out_byte(JpegOut *o, uint8_t b)
{
    if (o->n >= JPEG_OUT_BUF)
    {
        jpeg_out_flush(o);
    }
    o->buf[o->n++] = b;
}

static void jpeg_out_bytes(JpegOut *o, const void *p, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        jpeg_out_byte(o, s[i]);
    }
}

/* n is 1..16; at most 7 bits are pending, so 32 bits never overflow. */
static void jpeg_put_bits(JpegOut *o, uint32_t bits, int n)
{
    o->bits = (o->bits << n) | (bits & ((1u << n) - 1u));
    o->nbits += n;
    while (o->nbits >= 8)
    {
        uint8_t b;

        o->nbits -= 8;
        b = (uint8_t)((o->bits >> o->nbits) & 0xFFu);
        jpeg_out_byte(o, b);
        if (b == 0xFFu)
        {
            jpeg_out_byte(o, 0x00);
        }
    }
}

static void jpeg_flush_bits(JpegOut *o)
{
    if (o->nbits > 0)
    {
        jpeg_put_bits(o, (1u << (8 - o->nbits)) - 1u, 8 - o->nbits);
        o->nbits = 0;
    }
}

static void jpeg_write_marker(JpegOut *o, uint8_t m, const uint8_t *data,
                              uint16_t n)
{
    uint8_t h[4];

    h[0] = 0xFF;
    h[1] = m;
    h[2] = (uint8_t)((n + 2u) >> 8);
    h[3] = (uint8_t)((n + 2u) & 0xFFu);
    jpeg_out_bytes(o, h, 4);
    jpeg_out_bytes(o, data, n);
}

/*
 * Forward DCT, the accurate integer one of the IJG library (jfdctint,
 * Loeffler / Ligtenberg / Moschytz, 12 multiplies per row).  Input: 64
 * samples centred on zero.  Output: the coefficients times 8.
 */
#define JF_CONST_BITS 13
#define JF_PASS1_BITS 2
#define JF_DESCALE(x, n) (((x) + ((int32_t)1 << ((n) - 1))) >> (n))

static void jpeg_fdct(int32_t *d)
{
    int32_t *p = d;
    int i;

    for (i = 0; i < 8; i++, p += 8)
    {
        int32_t t0 = p[0] + p[7], t7 = p[0] - p[7];
        int32_t t1 = p[1] + p[6], t6 = p[1] - p[6];
        int32_t t2 = p[2] + p[5], t5 = p[2] - p[5];
        int32_t t3 = p[3] + p[4], t4 = p[3] - p[4];
        int32_t t10 = t0 + t3, t13 = t0 - t3;
        int32_t t11 = t1 + t2, t12 = t1 - t2;
        int32_t z1, z2, z3, z4, z5;

        p[0] = (t10 + t11) * (1 << JF_PASS1_BITS);
        p[4] = (t10 - t11) * (1 << JF_PASS1_BITS);
        z1 = (t12 + t13) * 4433;
        p[2] = JF_DESCALE(z1 + t13 * 6270, JF_CONST_BITS - JF_PASS1_BITS);
        p[6] = JF_DESCALE(z1 - t12 * 15137, JF_CONST_BITS - JF_PASS1_BITS);

        z1 = t4 + t7;
        z2 = t5 + t6;
        z3 = t4 + t6;
        z4 = t5 + t7;
        z5 = (z3 + z4) * 9633;
        t4 *= 2446;
        t5 *= 16819;
        t6 *= 25172;
        t7 *= 12299;
        z1 *= -7373;
        z2 *= -20995;
        z3 = z3 * -16069 + z5;
        z4 = z4 * -3196 + z5;
        p[7] = JF_DESCALE(t4 + z1 + z3, JF_CONST_BITS - JF_PASS1_BITS);
        p[5] = JF_DESCALE(t5 + z2 + z4, JF_CONST_BITS - JF_PASS1_BITS);
        p[3] = JF_DESCALE(t6 + z2 + z3, JF_CONST_BITS - JF_PASS1_BITS);
        p[1] = JF_DESCALE(t7 + z1 + z4, JF_CONST_BITS - JF_PASS1_BITS);
    }

    p = d;
    for (i = 0; i < 8; i++, p++)
    {
        int32_t t0 = p[0] + p[56], t7 = p[0] - p[56];
        int32_t t1 = p[8] + p[48], t6 = p[8] - p[48];
        int32_t t2 = p[16] + p[40], t5 = p[16] - p[40];
        int32_t t3 = p[24] + p[32], t4 = p[24] - p[32];
        int32_t t10 = t0 + t3, t13 = t0 - t3;
        int32_t t11 = t1 + t2, t12 = t1 - t2;
        int32_t z1, z2, z3, z4, z5;

        p[0] = JF_DESCALE(t10 + t11, JF_PASS1_BITS);
        p[32] = JF_DESCALE(t10 - t11, JF_PASS1_BITS);
        z1 = (t12 + t13) * 4433;
        p[16] = JF_DESCALE(z1 + t13 * 6270, JF_CONST_BITS + JF_PASS1_BITS);
        p[48] = JF_DESCALE(z1 - t12 * 15137, JF_CONST_BITS + JF_PASS1_BITS);

        z1 = t4 + t7;
        z2 = t5 + t6;
        z3 = t4 + t6;
        z4 = t5 + t7;
        z5 = (z3 + z4) * 9633;
        t4 *= 2446;
        t5 *= 16819;
        t6 *= 25172;
        t7 *= 12299;
        z1 *= -7373;
        z2 *= -20995;
        z3 = z3 * -16069 + z5;
        z4 = z4 * -3196 + z5;
        p[56] = JF_DESCALE(t4 + z1 + z3, JF_CONST_BITS + JF_PASS1_BITS);
        p[40] = JF_DESCALE(t5 + z2 + z4, JF_CONST_BITS + JF_PASS1_BITS);
        p[24] = JF_DESCALE(t6 + z2 + z3, JF_CONST_BITS + JF_PASS1_BITS);
        p[8] = JF_DESCALE(t7 + z1 + z4, JF_CONST_BITS + JF_PASS1_BITS);
    }
}

/* DCT, quantise and entropy-code one 8x8 block.  `q8` holds the
 * quantiser steps times 8, in natural (row) order. */
static void jpeg_write_block(JpegOut *o, int32_t *blk, const uint16_t *q8,
                             const JpegHuff *dc, const JpegHuff *ac,
                             int *pred)
{
    int run = 0;
    int i;
    int v;
    int a;
    int cat;

    jpeg_fdct(blk);

    for (i = 0; i < 64; i++)
    {
        int32_t c = blk[k_zigzag[i]];
        int32_t q = (int32_t)q8[k_zigzag[i]];

        v = (c < 0) ? -(int)((-c + (q >> 1)) / q) : (int)((c + (q >> 1)) / q);

        if (i == 0)
        {
            int diff = v - *pred;

            *pred = v;
            v = diff;
        }
        else if (v == 0)
        {
            run++;
            continue;
        }

        a = (v < 0) ? -v : v;
        cat = 0;
        while (a != 0)
        {
            a >>= 1;
            cat++;
        }
        if (i == 0)
        {
            jpeg_put_bits(o, dc->code[cat], dc->size[cat]);
        }
        else
        {
            while (run >= 16)
            {
                jpeg_put_bits(o, ac->code[0xF0], ac->size[0xF0]);
                run -= 16;
            }
            jpeg_put_bits(o, ac->code[(run << 4) | cat],
                          ac->size[(run << 4) | cat]);
            run = 0;
        }
        if (cat > 0)
        {
            jpeg_put_bits(o, (uint32_t)((v < 0) ? v + ((1 << cat) - 1) : v),
                          cat);
        }
    }
    if (run > 0)
    {
        jpeg_put_bits(o, ac->code[0x00], ac->size[0x00]);
    }
}

/* Reads `n` pixels of row `y` starting at column `x` and converts them
 * to full-range YCbCr (JFIF), centred on zero.  Columns and rows past
 * the picture repeat the last one, which is what a decoder expects of
 * the padding of an incomplete block. */
static void jpeg_row_ycc(const FM_Image *img, uint32_t x, uint32_t y,
                         uint32_t n, int32_t *yy, int32_t *cb, int32_t *cr)
{
    const uint8_t *p;
    uint32_t have;
    uint32_t i;
    size_t bpp = Image_BytesPerPixel(img->format);
    uint8_t rgb[16][3];

    if (y >= img->height)
    {
        y = (uint32_t)img->height - 1u;
    }
    if (x >= img->width)
    {
        x = (uint32_t)img->width - 1u;
    }
    have = (uint32_t)img->width - x;
    if (have > n)
    {
        have = n;
    }
    p = img->data + ((size_t)y * img->width + x) * bpp;

    switch (img->format)
    {
    case FM_PIXEL_RGB888:
    case FM_PIXEL_RGBA8888:
        for (i = 0; i < have; i++, p += bpp)
        {
            rgb[i][0] = p[0]; rgb[i][1] = p[1]; rgb[i][2] = p[2];
        }
        break;
    case FM_PIXEL_BGR888:
    case FM_PIXEL_BGRA8888:
        for (i = 0; i < have; i++, p += bpp)
        {
            rgb[i][0] = p[2]; rgb[i][1] = p[1]; rgb[i][2] = p[0];
        }
        break;
    case FM_PIXEL_RGB565:
    case FM_PIXEL_BGR565:
        for (i = 0; i < have; i++, p += 2)
        {
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            uint8_t hi = (uint8_t)(((v >> 11) << 3) | (v >> 13));
            uint8_t lo = (uint8_t)(((v & 0x1Fu) << 3) | ((v & 0x1Fu) >> 2));

            rgb[i][1] = (uint8_t)((((v >> 5) & 0x3Fu) << 2) |
                                  (((v >> 5) & 0x3Fu) >> 4));
            if (img->format == FM_PIXEL_RGB565)
            {
                rgb[i][0] = hi; rgb[i][2] = lo;
            }
            else
            {
                rgb[i][0] = lo; rgb[i][2] = hi;
            }
        }
        break;
    default:                                    /* GRAY8 */
        for (i = 0; i < have; i++, p++)
        {
            rgb[i][0] = rgb[i][1] = rgb[i][2] = p[0];
        }
        break;
    }

    for (i = 0; i < have; i++)
    {
        int32_t r = rgb[i][0];
        int32_t g = rgb[i][1];
        int32_t b = rgb[i][2];

        yy[i] = ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128;
        cb[i] = (-11059 * r - 21709 * g + 32768 * b + 32768) >> 16;
        cr[i] = (32768 * r - 27439 * g - 5329 * b + 32768) >> 16;
        if (cb[i] > 127) cb[i] = 127;
        if (cr[i] > 127) cr[i] = 127;
    }
    for (; i < n; i++)
    {
        yy[i] = yy[have - 1u];
        cb[i] = cb[have - 1u];
        cr[i] = cr[have - 1u];
    }
}

static void jpeg_encode(JpegOut *o, const FM_Image *img, int quality,
                        int subsample)
{
    uint16_t qy8[64];
    uint16_t qc8[64];
    uint8_t seg[1 + 16 + 162];
    int pred_y = 0;
    int pred_cb = 0;
    int pred_cr = 0;
    int scale;
    uint32_t step = subsample ? 16u : 8u;
    uint32_t mx;
    uint32_t my;
    uint32_t i;

    jpeg_huff_init();
    if (quality < 1)
    {
        quality = FM_JPEG_QUALITY;
    }
    if (quality > 100)
    {
        quality = 100;
    }
    scale = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);

    jpeg_out_byte(o, 0xFF);
    jpeg_out_byte(o, 0xD8);
    {
        static const uint8_t app0[14] = {
            'J','F','I','F',0, 1, 1, 0, 0, 1, 0, 1, 0, 0
        };

        jpeg_write_marker(o, 0xE0, app0, 14);
    }
    for (i = 0; i < 2u; i++)
    {
        const uint8_t *base = (i == 0u) ? k_q_lum : k_q_chr;
        uint16_t *q8 = (i == 0u) ? qy8 : qc8;
        uint32_t k;

        seg[0] = (uint8_t)i;
        for (k = 0; k < 64u; k++)
        {
            int v = ((int)base[k_zigzag[k]] * scale + 50) / 100;

            if (v < 1) v = 1;
            if (v > 255) v = 255;
            seg[1u + k] = (uint8_t)v;
            q8[k_zigzag[k]] = (uint16_t)(v * 8);
        }
        jpeg_write_marker(o, 0xDB, seg, 65);
    }
    seg[0] = 8;
    seg[1] = (uint8_t)(img->height >> 8);
    seg[2] = (uint8_t)(img->height & 0xFFu);
    seg[3] = (uint8_t)(img->width >> 8);
    seg[4] = (uint8_t)(img->width & 0xFFu);
    seg[5] = 3;
    seg[6] = 1; seg[7] = subsample ? 0x22 : 0x11; seg[8] = 0;
    seg[9] = 2; seg[10] = 0x11; seg[11] = 1;
    seg[12] = 3; seg[13] = 0x11; seg[14] = 1;
    jpeg_write_marker(o, 0xC0, seg, 15);

    seg[0] = 0x00;
    memcpy(seg + 1, k_dc_lum_bits + 1, 16);
    memcpy(seg + 17, k_dc_lum_val, 12);
    jpeg_write_marker(o, 0xC4, seg, 29);
    seg[0] = 0x10;
    memcpy(seg + 1, k_ac_lum_bits + 1, 16);
    memcpy(seg + 17, k_ac_lum_val, 162);
    jpeg_write_marker(o, 0xC4, seg, 179);
    seg[0] = 0x01;
    memcpy(seg + 1, k_dc_chr_bits + 1, 16);
    memcpy(seg + 17, k_dc_chr_val, 12);
    jpeg_write_marker(o, 0xC4, seg, 29);
    seg[0] = 0x11;
    memcpy(seg + 1, k_ac_chr_bits + 1, 16);
    memcpy(seg + 17, k_ac_chr_val, 162);
    jpeg_write_marker(o, 0xC4, seg, 179);
    {
        static const uint8_t sos[10] = {
            3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0
        };

        jpeg_write_marker(o, 0xDA, sos, 10);
    }

    for (my = 0; my < img->height && o->err == FM_OK; my += step)
    {
        for (mx = 0; mx < img->width; mx += step)
        {
            if (!subsample)
            {
                int32_t by[64];
                int32_t bcb[64];
                int32_t bcr[64];

                for (i = 0; i < 8u; i++)
                {
                    jpeg_row_ycc(img, mx, my + i, 8u, by + 8u * i,
                                 bcb + 8u * i, bcr + 8u * i);
                }
                jpeg_write_block(o, by, qy8, &jpeg_h_dc_y, &jpeg_h_ac_y,
                                 &pred_y);
                jpeg_write_block(o, bcb, qc8, &jpeg_h_dc_c, &jpeg_h_ac_c,
                                 &pred_cb);
                jpeg_write_block(o, bcr, qc8, &jpeg_h_dc_c, &jpeg_h_ac_c,
                                 &pred_cr);
            }
            else
            {
                int32_t by[4][64];
                int32_t bcb[64];
                int32_t bcr[64];
                int32_t ry[16];
                int32_t rcb[2][16];
                int32_t rcr[2][16];
                uint32_t row;
                uint32_t k;

                for (row = 0; row < 16u; row++)
                {
                    int32_t *left = by[(row / 8u) * 2u] + 8u * (row % 8u);
                    int32_t *right = by[(row / 8u) * 2u + 1u] +
                                     8u * (row % 8u);

                    jpeg_row_ycc(img, mx, my + row, 16u, ry, rcb[row & 1u],
                                 rcr[row & 1u]);
                    for (k = 0; k < 8u; k++)
                    {
                        left[k] = ry[k];
                        right[k] = ry[8u + k];
                    }
                    if ((row & 1u) != 0u)
                    {
                        int32_t *ocb = bcb + 8u * (row / 2u);
                        int32_t *ocr = bcr + 8u * (row / 2u);

                        for (k = 0; k < 8u; k++)
                        {
                            ocb[k] = (rcb[0][2u * k] + rcb[0][2u * k + 1u] +
                                      rcb[1][2u * k] + rcb[1][2u * k + 1u] +
                                      2) >> 2;
                            ocr[k] = (rcr[0][2u * k] + rcr[0][2u * k + 1u] +
                                      rcr[1][2u * k] + rcr[1][2u * k + 1u] +
                                      2) >> 2;
                        }
                    }
                }
                for (k = 0; k < 4u; k++)
                {
                    jpeg_write_block(o, by[k], qy8, &jpeg_h_dc_y,
                                     &jpeg_h_ac_y, &pred_y);
                }
                jpeg_write_block(o, bcb, qc8, &jpeg_h_dc_c, &jpeg_h_ac_c,
                                 &pred_cb);
                jpeg_write_block(o, bcr, qc8, &jpeg_h_dc_c, &jpeg_h_ac_c,
                                 &pred_cr);
            }
        }
    }
    jpeg_flush_bits(o);
    jpeg_out_byte(o, 0xFF);
    jpeg_out_byte(o, 0xD9);
    jpeg_out_flush(o);
}

static FM_Result jpeg_encode_common(const FM_Image *img, JpegOut *o,
                                    const FM_JpegEncodeOptions *opt)
{
    FM_Result r;

    if (img == NULL || img->data == NULL || img->width == 0 ||
        img->height == 0 || Image_BytesPerPixel(img->format) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_Image_CheckSource(img, NULL);   /* capacity AND data_size */
    if (r != FM_OK)
    {
        return r;
    }
    jpeg_encode(o, img, (opt != NULL) ? (int)opt->quality : 0,
                (opt != NULL && opt->subsample != 0u) ? 1 : 0);
    return o->err;
}

FM_Result FM_JPEG_EncodeFile(FM_File *f, const FM_Image *img)
{
    JpegOut o;

    jpeg_out_init(&o);
    o.file = f;
    return jpeg_encode_common(img, &o, NULL);
}

FM_Result FM_JPEG_EncodeSink(const FM_Image *img,
                             const FM_JpegEncodeOptions *opt,
                             FM_JpegSinkFn sink, void *ctx)
{
    JpegOut o;

    if (sink == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    jpeg_out_init(&o);
    o.sink = sink;
    o.sink_ctx = ctx;
    return jpeg_encode_common(img, &o, opt);
}

FM_Result Image_EncodeJPEGEx(const FM_Image *img,
                             const FM_JpegEncodeOptions *opt, void *out,
                             size_t out_cap, size_t *out_len)
{
    JpegOut o;
    FM_Result r;

    if (out == NULL || out_len == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    jpeg_out_init(&o);
    o.mem = (uint8_t *)out;
    o.mem_cap = out_cap;
    r = jpeg_encode_common(img, &o, opt);
    if (r == FM_OK)
    {
        *out_len = o.mem_len;
    }
    return r;
}

FM_Result Image_EncodeJPEG(const FM_Image *img, void *out, size_t out_cap,
                           size_t *out_len)
{
    return Image_EncodeJPEGEx(img, NULL, out, out_cap, out_len);
}

#else /* !FM_USE_JPEG */

FM_Result Image_ProbeJPEG(const void *jpeg, size_t jpeg_size,
                          FM_ImageInfo *info)
{
    (void)jpeg;
    (void)jpeg_size;
    (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_DecodeJPEG(const void *jpeg, size_t jpeg_size,
                           FM_Image *img, void *pixel_buffer, size_t capacity)
{
    (void)jpeg;
    (void)jpeg_size;
    (void)img;
    (void)pixel_buffer;
    (void)capacity;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_EncodeJPEG(const FM_Image *img, void *out, size_t out_cap,
                           size_t *out_len)
{
    (void)img;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_EncodeJPEGEx(const FM_Image *img,
                             const FM_JpegEncodeOptions *opt, void *out,
                             size_t out_cap, size_t *out_len)
{
    (void)img;
    (void)opt;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return FM_ERR_UNSUPPORTED;
}

FM_Result FM_JPEG_EncodeSink(const FM_Image *img,
                             const FM_JpegEncodeOptions *opt,
                             FM_JpegSinkFn sink, void *ctx)
{
    (void)img;
    (void)opt;
    (void)sink;
    (void)ctx;
    return FM_ERR_UNSUPPORTED;
}

#endif
