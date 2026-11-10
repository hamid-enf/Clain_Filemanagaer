/*
 * FileManager - MJPEG codec plugin (picojpeg backend)
 *
 * Wraps the vendored picojpeg decoder as an FM_VideoCodecOps.  Feeds the
 * decoder from an in-memory frame buffer and blits the decoded MCUs into a
 * RGB888 output image.
 */

#define FM_DEBUG_TAG "mjpeg"
#include "fm_debug.h"
#include "fm_mjpeg.h"
#include "fm_internal.h"

#if FM_USE_MJPEG

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

#include <string.h>

typedef struct
{
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} MjpegSrc;

static FM_Result mjpeg_init(void **ctx, const FM_VideoInfo *info);
static FM_Result mjpeg_decode(void *ctx, const void *compressed, size_t size,
                              void *frame_out, size_t frame_cap);
static void      mjpeg_close(void *ctx);

static const FM_VideoCodecOps g_mjpeg_ops = {
    "MJPEG (picojpeg)",
    mjpeg_init,
    mjpeg_decode,
    mjpeg_close
};

const FM_VideoCodecOps *MJPEG_GetCodecOps(void)
{
    return &g_mjpeg_ops;
}

static FM_Result mjpeg_init(void **ctx, const FM_VideoInfo *info)
{
    (void)info;
    if (ctx == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* picojpeg keeps its work arrays in static memory; the context pointer
     * is only a non-NULL sentinel for the plugin lifecycle. */
    *ctx = (void *)1;
    return FM_OK;
}

static void mjpeg_close(void *ctx)
{
    (void)ctx;
}

/* picojpeg byte source callback: returns 0 on success (0 bytes read == EOF). */
static unsigned char mjpeg_need_bytes(unsigned char *buf,
                                      unsigned char buf_size,
                                      unsigned char *read,
                                      void *cb_data)
{
    MjpegSrc *s = (MjpegSrc *)cb_data;
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

/* Blit one decoded MCU into the RGB888 output image. */
static void mjpeg_blit(const pjpeg_image_info_t *info, int mcu_x, int mcu_y,
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

static FM_Result mjpeg_decode_run(const void *compressed, size_t size,
                                  void *frame_out, size_t frame_cap);

/* picojpeg's state is file-static and shared with JPEG still decoding
 * (Image_Load): one picture at a time. */
static FM_Result mjpeg_decode(void *ctx, const void *compressed, size_t size,
                              void *frame_out, size_t frame_cap)
{
    FM_Result r;

    (void)ctx;
    if (compressed == NULL || frame_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_CodecScratch_Enter();
    if (r != FM_OK)
    {
        return r;
    }
    r = mjpeg_decode_run(compressed, size, frame_out, frame_cap);
    FM_CodecScratch_Leave();
    return r;
}

static FM_Result mjpeg_decode_run(const void *compressed, size_t size,
                                  void *frame_out, size_t frame_cap)
{
    pjpeg_image_info_t info;
    MjpegSrc src;
    unsigned char status;
    int mcu_x = 0;
    int mcu_y = 0;
    uint8_t *rgb = (uint8_t *)frame_out;

    src.data = (const uint8_t *)compressed;
    src.size = size;
    src.pos = 0;

    status = pjpeg_decode_init(&info, mjpeg_need_bytes, &src, 0);
    if (status != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (info.m_width <= 0 || info.m_height <= 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* 64-bit product: 65535 x 65535 x 3 wraps a 32-bit size_t to a
     * value that passes this check. */
    if ((uint64_t)info.m_width * (uint64_t)info.m_height * 3u >
        (uint64_t)frame_cap)
    {
        return FM_ERR_TOO_LARGE;
    }

    for (;;)
    {
        status = pjpeg_decode_mcu();
        if (status != 0)
        {
            return (status == PJPG_NO_MORE_BLOCKS) ? FM_OK
                                                   : FM_ERR_INVALID_FORMAT;
        }
        if (mcu_y >= info.m_MCUSPerCol)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        mjpeg_blit(&info, mcu_x, mcu_y, rgb, info.m_width, info.m_height);
        mcu_x++;
        if (mcu_x == info.m_MCUSPerRow)
        {
            mcu_x = 0;
            mcu_y++;
            FM_Idle();
        }
    }
}

#endif /* FM_USE_MJPEG */
