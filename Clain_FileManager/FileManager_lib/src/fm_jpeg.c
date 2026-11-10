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

        do
        {
            r = jpeg_read_u8(f, &b[0]);
            if (r != FM_OK)
            {
                return FM_ERR_INVALID_FORMAT;
            }
        } while (b[0] != 0xFFu);
        do
        {
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

static FM_Result jpeg_sw_decode_src(JpegSrc *src, FM_Image *img,
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

        if (File_Size(f, &sz) == FM_OK && sz > 0u &&
            sz <= (16u * 1024u * 1024u))
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
                blob = (uint8_t *)FM_Alloc((size_t)sz);
                owned = 1;
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
                        return r;
                    }
                }
                else if (owned)
                {
                    FM_Free(blob);
                }
            }
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
/* Baseline JPEG encode (4:4:4, standard Huffman, quality ~70)         */
/* ------------------------------------------------------------------ */

typedef struct
{
    FM_File *file;
    uint8_t *mem;
    size_t   mem_cap;
    size_t   mem_len;
    uint32_t bits;
    int      nbits;
} JpegOut;

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

static FM_Result jpeg_out_byte(JpegOut *o, uint8_t b)
{
    if (o->file != NULL)
    {
        uint32_t wr = 0;
        FM_Result r = File_Write(o->file, &b, 1, &wr);

        return (r == FM_OK && wr == 1u) ? FM_OK : FM_ERR_IO;
    }
    if (o->mem_len >= o->mem_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    o->mem[o->mem_len++] = b;
    return FM_OK;
}

static FM_Result jpeg_out_bytes(JpegOut *o, const void *p, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i;
    FM_Result r = FM_OK;

    for (i = 0; i < n && r == FM_OK; i++)
    {
        r = jpeg_out_byte(o, s[i]);
    }
    return r;
}

static FM_Result jpeg_put_bits(JpegOut *o, uint32_t bits, int n)
{
    o->bits = (o->bits << n) | (bits & ((n == 32) ? 0xFFFFFFFFu
                                                  : ((1u << n) - 1u)));
    o->nbits += n;
    while (o->nbits >= 8)
    {
        uint8_t b;
        FM_Result r;

        o->nbits -= 8;
        b = (uint8_t)((o->bits >> o->nbits) & 0xFFu);
        r = jpeg_out_byte(o, b);
        if (r != FM_OK)
        {
            return r;
        }
        if (b == 0xFFu)
        {
            r = jpeg_out_byte(o, 0x00);
            if (r != FM_OK)
            {
                return r;
            }
        }
    }
    return FM_OK;
}

static FM_Result jpeg_flush_bits(JpegOut *o)
{
    if (o->nbits > 0)
    {
        FM_Result r = jpeg_put_bits(o, (1u << (8 - o->nbits)) - 1u,
                                    8 - o->nbits);
        o->nbits = 0;
        return r;
    }
    return FM_OK;
}

static int jpeg_clamp_i(int v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > 255)
    {
        return 255;
    }
    return v;
}

static void jpeg_pixel_rgb(const FM_Image *img, uint16_t x, uint16_t y,
                           uint8_t *r, uint8_t *g, uint8_t *b)
{
    size_t index;
    const uint8_t *p;
    uint8_t a = 0xFFu;

    if (x >= img->width)
    {
        x = (uint16_t)(img->width - 1u);
    }
    if (y >= img->height)
    {
        y = (uint16_t)(img->height - 1u);
    }
    index = (size_t)y * img->width + x;
    *r = *g = *b = 0;
    if (img->format == FM_PIXEL_RGB888)
    {
        p = img->data + index * 3u;
        *r = p[0]; *g = p[1]; *b = p[2];
    }
    else if (img->format == FM_PIXEL_BGR888)
    {
        p = img->data + index * 3u;
        *b = p[0]; *g = p[1]; *r = p[2];
    }
    else if (img->format == FM_PIXEL_RGB565 || img->format == FM_PIXEL_BGR565)
    {
        uint16_t v;

        p = img->data + index * 2u;
        v = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
        *g = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        if (img->format == FM_PIXEL_RGB565)
        {
            *r = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
            *b = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        }
        else
        {
            *b = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
            *r = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        }
    }
    else if (img->format == FM_PIXEL_RGBA8888)
    {
        p = img->data + index * 4u;
        *r = p[0]; *g = p[1]; *b = p[2]; (void)a; (void)p[3];
    }
    else if (img->format == FM_PIXEL_BGRA8888)
    {
        p = img->data + index * 4u;
        *b = p[0]; *g = p[1]; *r = p[2];
    }
    else
    {
        uint8_t v = img->data[index];
        *r = *g = *b = v;
    }
}

/* Integer 8-point DCT (scaled, Loeffler-style constants >> 10). */
static void jpeg_dct8(const int *src, int *dst)
{
    int i;
    int tmp[8];

    for (i = 0; i < 8; i++)
    {
        int s07 = src[0] + src[7];
        int s16 = src[1] + src[6];
        int s25 = src[2] + src[5];
        int s34 = src[3] + src[4];
        int d07 = src[0] - src[7];
        int d16 = src[1] - src[6];
        int d25 = src[2] - src[5];
        int d34 = src[3] - src[4];
        int s0734 = s07 + s34;
        int s1625 = s16 + s25;
        int d0734 = s07 - s34;
        int d1625 = s16 - s25;

        tmp[0] = s0734 + s1625;
        tmp[4] = s0734 - s1625;
        tmp[2] = (d0734 * 1420 + d1625 * 588) >> 10;   /* cos(2pi/16),sin */
        tmp[6] = (d0734 * 588 - d1625 * 1420) >> 10;
        tmp[1] = (d07 * 1448 + d16 * 1892 + d25 * 784 - d34 * 400) >> 10;
        tmp[3] = (d07 * 784 - d16 * 400 - d25 * 1892 + d34 * 1448) >> 10;
        tmp[5] = (d07 * 400 - d16 * 784 + d25 * 1448 + d34 * 1892) >> 10;
        tmp[7] = (d07 * 1892 - d16 * 1448 + d25 * 400 + d34 * 784) >> 10;
        src += 8;
        for (s07 = 0; s07 < 8; s07++)
        {
            dst[s07 * 8 + i] = tmp[s07];
        }
    }
}

static void jpeg_fdct(const int block[64], int coeff[64])
{
    int tmp[64];
    int i;

    jpeg_dct8(block, tmp);
    jpeg_dct8(tmp, coeff);
    for (i = 0; i < 64; i++)
    {
        coeff[i] = (coeff[i] + 4) >> 3;
    }
}

static int jpeg_category(int v)
{
    int a = (v < 0) ? -v : v;
    int n = 0;

    while (a != 0)
    {
        a >>= 1;
        n++;
    }
    return n;
}

static FM_Result jpeg_write_block(JpegOut *o, const int *zz, const uint8_t *q,
                                  JpegHuff *dc, JpegHuff *ac, int *pred)
{
    int dc_val = zz[0] / (int)q[0];
    int d;
    int cat;
    int i;
    int run = 0;
    FM_Result r;

    if (dc_val > 1023)
    {
        dc_val = 1023;
    }
    if (dc_val < -1023)
    {
        dc_val = -1023;
    }
    d = dc_val - *pred;
    *pred = dc_val;
    cat = jpeg_category(d);
    r = jpeg_put_bits(o, dc->code[cat], dc->size[cat]);
    if (r != FM_OK)
    {
        return r;
    }
    if (cat != 0)
    {
        uint32_t bits = (d < 0) ? (uint32_t)(d + ((1 << cat) - 1))
                                : (uint32_t)d;

        r = jpeg_put_bits(o, bits, cat);
        if (r != FM_OK)
        {
            return r;
        }
    }
    for (i = 1; i < 64; i++)
    {
        int v = zz[i] / (int)q[i];

        if (v > 1023)
        {
            v = 1023;
        }
        if (v < -1023)
        {
            v = -1023;
        }
        if (v == 0)
        {
            run++;
            continue;
        }
        while (run >= 16)
        {
            r = jpeg_put_bits(o, ac->code[0xF0], ac->size[0xF0]);
            if (r != FM_OK)
            {
                return r;
            }
            run -= 16;
        }
        cat = jpeg_category(v);
        r = jpeg_put_bits(o, ac->code[(run << 4) | cat],
                          ac->size[(run << 4) | cat]);
        if (r != FM_OK)
        {
            return r;
        }
        {
            uint32_t bits = (v < 0) ? (uint32_t)(v + ((1 << cat) - 1))
                                    : (uint32_t)v;

            r = jpeg_put_bits(o, bits, cat);
            if (r != FM_OK)
            {
                return r;
            }
        }
        run = 0;
    }
    if (run > 0)
    {
        r = jpeg_put_bits(o, ac->code[0x00], ac->size[0x00]);
    }
    return r;
}

static FM_Result jpeg_write_marker(JpegOut *o, uint8_t m,
                                   const uint8_t *data, uint16_t n)
{
    uint8_t h[4];
    FM_Result r;

    h[0] = 0xFF;
    h[1] = m;
    r = jpeg_out_bytes(o, h, 2);
    if (n > 0u || m == 0xC0u || m == 0xC4u || m == 0xDBu || m == 0xDAu ||
        m == 0xE0u)
    {
        uint16_t len = (uint16_t)(n + 2u);

        h[0] = (uint8_t)(len >> 8);
        h[1] = (uint8_t)(len & 0xFFu);
        r = (r == FM_OK) ? jpeg_out_bytes(o, h, 2) : r;
        if (r == FM_OK && n > 0u)
        {
            r = jpeg_out_bytes(o, data, n);
        }
    }
    return r;
}

static FM_Result jpeg_encode(JpegOut *o, const FM_Image *img)
{
    JpegHuff dc_y;
    JpegHuff dc_c;
    JpegHuff ac_y;
    JpegHuff ac_c;
    uint8_t qy[64];
    uint8_t qc[64];
    int pred_y = 0;
    int pred_cb = 0;
    int pred_cr = 0;
    uint16_t mx;
    uint16_t my;
    uint16_t mcu_w;
    uint16_t mcu_h;
    FM_Result r;
    uint32_t i;

    jpeg_huff_build(&dc_y, k_dc_lum_bits, k_dc_lum_val, 12);
    jpeg_huff_build(&dc_c, k_dc_chr_bits, k_dc_chr_val, 12);
    jpeg_huff_build(&ac_y, k_ac_lum_bits, k_ac_lum_val, 162);
    jpeg_huff_build(&ac_c, k_ac_chr_bits, k_ac_chr_val, 162);
    for (i = 0; i < 64u; i++)
    {
        int v = ((int)k_q_lum[k_zigzag[i]] * 50 + 50) / 100;
        int c = ((int)k_q_chr[k_zigzag[i]] * 50 + 50) / 100;

        if (v < 1) v = 1;
        if (v > 255) v = 255;
        if (c < 1) c = 1;
        if (c > 255) c = 255;
        qy[i] = (uint8_t)v;
        qc[i] = (uint8_t)c;
    }

    r = jpeg_out_byte(o, 0xFF);
    if (r == FM_OK) r = jpeg_out_byte(o, 0xD8);
    {
        uint8_t app0[14] = {
            'J','F','I','F',0, 1, 1, 0, 0, 1, 0, 1, 0, 0
        };

        if (r == FM_OK) r = jpeg_write_marker(o, 0xE0, app0, 14);
    }
    {
        uint8_t dqt[65];

        dqt[0] = 0x00;
        memcpy(dqt + 1, qy, 64);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xDB, dqt, 65);
        dqt[0] = 0x01;
        memcpy(dqt + 1, qc, 64);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xDB, dqt, 65);
    }
    {
        uint8_t sof[15];

        sof[0] = 8;
        sof[1] = (uint8_t)(img->height >> 8);
        sof[2] = (uint8_t)(img->height & 0xFFu);
        sof[3] = (uint8_t)(img->width >> 8);
        sof[4] = (uint8_t)(img->width & 0xFFu);
        sof[5] = 3;
        sof[6] = 1; sof[7] = 0x11; sof[8] = 0;
        sof[9] = 2; sof[10] = 0x11; sof[11] = 1;
        sof[12] = 3; sof[13] = 0x11; sof[14] = 1;
        if (r == FM_OK) r = jpeg_write_marker(o, 0xC0, sof, 15);
    }
    {
        uint8_t dht[1 + 16 + 162];
        uint32_t n;

        dht[0] = 0x00;
        memcpy(dht + 1, k_dc_lum_bits + 1, 16);
        memcpy(dht + 17, k_dc_lum_val, 12);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xC4, dht, 29);
        dht[0] = 0x10;
        memcpy(dht + 1, k_ac_lum_bits + 1, 16);
        memcpy(dht + 17, k_ac_lum_val, 162);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xC4, dht, 179);
        dht[0] = 0x01;
        memcpy(dht + 1, k_dc_chr_bits + 1, 16);
        memcpy(dht + 17, k_dc_chr_val, 12);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xC4, dht, 29);
        dht[0] = 0x11;
        memcpy(dht + 1, k_ac_chr_bits + 1, 16);
        memcpy(dht + 17, k_ac_chr_val, 162);
        if (r == FM_OK) r = jpeg_write_marker(o, 0xC4, dht, 179);
        (void)n;
    }
    {
        uint8_t sos[10] = {
            3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0
        };

        if (r == FM_OK) r = jpeg_write_marker(o, 0xDA, sos, 10);
    }

    mcu_w = (uint16_t)((img->width + 7u) / 8u);
    mcu_h = (uint16_t)((img->height + 7u) / 8u);
    for (my = 0; r == FM_OK && my < mcu_h; my++)
    {
        for (mx = 0; r == FM_OK && mx < mcu_w; mx++)
        {
            int by[64], bcb[64], bcr[64];
            int cy[64], ccb[64], ccr[64];
            int zz[64];
            int y;
            int x;

            for (y = 0; y < 8; y++)
            {
                for (x = 0; x < 8; x++)
                {
                    uint8_t R, G, B;
                    int Yv, Cb, Cr;
                    uint16_t px = (uint16_t)(mx * 8u + (uint16_t)x);
                    uint16_t py = (uint16_t)(my * 8u + (uint16_t)y);

                    jpeg_pixel_rgb(img, px, py, &R, &G, &B);
                    Yv = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
                    Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                    Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
                    by[y * 8 + x] = jpeg_clamp_i(Yv) - 128;
                    bcb[y * 8 + x] = jpeg_clamp_i(Cb) - 128;
                    bcr[y * 8 + x] = jpeg_clamp_i(Cr) - 128;
                }
            }
            jpeg_fdct(by, cy);
            jpeg_fdct(bcb, ccb);
            jpeg_fdct(bcr, ccr);
            for (i = 0; i < 64u; i++)
            {
                zz[i] = cy[k_zigzag[i]];
            }
            r = jpeg_write_block(o, zz, qy, &dc_y, &ac_y, &pred_y);
            if (r == FM_OK)
            {
                for (i = 0; i < 64u; i++)
                {
                    zz[i] = ccb[k_zigzag[i]];
                }
                r = jpeg_write_block(o, zz, qc, &dc_c, &ac_c, &pred_cb);
            }
            if (r == FM_OK)
            {
                for (i = 0; i < 64u; i++)
                {
                    zz[i] = ccr[k_zigzag[i]];
                }
                r = jpeg_write_block(o, zz, qc, &dc_c, &ac_c, &pred_cr);
            }
        }
    }
    if (r == FM_OK)
    {
        r = jpeg_flush_bits(o);
    }
    if (r == FM_OK)
    {
        r = jpeg_out_byte(o, 0xFF);
    }
    if (r == FM_OK)
    {
        r = jpeg_out_byte(o, 0xD9);
    }
    return r;
}

static FM_Result jpeg_encode_common(const FM_Image *img, JpegOut *o)
{
    uint64_t need;

    if (img == NULL || img->data == NULL || img->width == 0 ||
        img->height == 0 || Image_BytesPerPixel(img->format) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    need = (uint64_t)img->width * img->height * Image_BytesPerPixel(img->format);
    if (need > img->data_capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    o->bits = 0;
    o->nbits = 0;
    return jpeg_encode(o, img);
}

FM_Result FM_JPEG_EncodeFile(FM_File *f, const FM_Image *img)
{
    JpegOut o;

    memset(&o, 0, sizeof(o));
    o.file = f;
    return jpeg_encode_common(img, &o);
}

FM_Result Image_EncodeJPEG(const FM_Image *img, void *out, size_t out_cap,
                           size_t *out_len)
{
    JpegOut o;
    FM_Result r;

    if (out == NULL || out_len == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&o, 0, sizeof(o));
    o.mem = (uint8_t *)out;
    o.mem_cap = out_cap;
    r = jpeg_encode_common(img, &o);
    if (r == FM_OK)
    {
        *out_len = o.mem_len;
    }
    return r;
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

#endif
