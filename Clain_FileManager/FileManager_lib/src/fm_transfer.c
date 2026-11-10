/*
 * fm_transfer.c — پیاده‌سازی کپی بین حافظه‌ها
 *
 * نکتهٔ طراحی: هیچ‌جا از malloc استفاده نمی‌شود. یک بافر ایستا با تراز
 * ۳۲ بایت داریم که هم برای DMA امن است و هم برای عملیات کش.
 */

#include <string.h>

#define FM_DEBUG_TAG "transfer"

#include "fm_transfer.h"
#include "fm_file.h"
#include "fm_crc.h"
#include "fm_board_config.h"
#include "fm_debug.h"

/* بافر میانی. اندازه از fm_board_config.h می‌آید.
 * تراز ۳۲ بایتی لازم است چون ممکن است طرف مقابل DMA باشد و عملیات کش
 * روی خطوط ۳۲ بایتی انجام می‌شود. */
#if defined(__GNUC__)
static uint8_t g_xfer_buf[FM_TRANSFER_CHUNK] __attribute__((aligned(32)));
#else
static uint8_t g_xfer_buf[FM_TRANSFER_CHUNK];
#endif

/* ------------------------------------------------------------------ */
/* کمکی                                                                */
/* ------------------------------------------------------------------ */

static const FM_TransferOpts *xfer_opts(const FM_TransferOpts *o,
                                        FM_TransferOpts *tmp)
{
    if (o != NULL)
    {
        return o;
    }
    memset(tmp, 0, sizeof(*tmp));
    return tmp;
}

static FM_Bool xfer_report(const FM_TransferOpts *o, uint64_t done,
                           uint64_t total)
{
    if (o->progress == NULL)
    {
        return FM_TRUE;
    }
    return o->progress(done, total, o->progress_user);
}

/* ------------------------------------------------------------------ */
/* سطح فایل                                                            */
/* ------------------------------------------------------------------ */

FM_Result FM_Transfer_CopyEx(const char *src_path, const char *dst_path,
                             const FM_TransferOpts *opts,
                             FM_TransferInfo *out_info)
{
    FM_TransferOpts tmp;
    const FM_TransferOpts *o = xfer_opts(opts, &tmp);
    FM_File src;
    FM_File dst;
    FM_Result r;
    FM_Result rc;
    uint64_t total = 0;
    uint64_t done = 0;
    uint32_t crc_src = 0;
    FM_Bool src_open = FM_FALSE;
    FM_Bool dst_open = FM_FALSE;

    if (src_path == NULL || dst_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (out_info != NULL)
    {
        memset(out_info, 0, sizeof(*out_info));
    }

    /* مقصد از قبل هست؟ */
    if (o->no_overwrite)
    {
        FM_Bool exists = FM_FALSE;

        if (File_Exists(dst_path, &exists) == FM_OK && exists)
        {
            return FM_ERR_EXISTS;
        }
    }

    /* File_OpenEx returns FM_ERR_BUSY on a handle whose `used` flag is
     * set, and these live on the stack, so clear them first. */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    r = File_OpenEx(&src, src_path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    src_open = FM_TRUE;
    (void)File_Size(&src, &total);

    r = File_OpenEx(&dst, dst_path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        (void)File_Close(&src);
        return r;
    }
    dst_open = FM_TRUE;

    for (;;)
    {
        uint32_t got = 0;
        uint32_t put = 0;

        r = File_Read(&src, g_xfer_buf, (uint32_t)sizeof(g_xfer_buf), &got);
        /* File_Read reports EOF as FM_ERR_EOF, not as a zero-length
         * read.  Treat both as a clean end of input. */
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            break;
        }
        if (r != FM_OK)
        {
            break;
        }
        if (got == 0u)
        {
            break;
        }
        r = File_Write(&dst, g_xfer_buf, got, &put);
        if (r != FM_OK)
        {
            break;
        }
        if (put != got)
        {
            /* مقصد پر شده — روی فلش یعنی جا نیست. */
            r = FM_ERR_NO_SPACE;
            break;
        }
        if (o->verify)
        {
            crc_src = CRC_Compute32(g_xfer_buf, got, crc_src);
        }
        done += got;
        if (!xfer_report(o, done, total))
        {
            r = FM_ERR_CANCELLED;
            break;
        }
    }

    if (dst_open)
    {
        rc = File_Close(&dst);
        if (r == FM_OK)
        {
            r = rc;                      /* خطای بستن را گم نکن */
        }
    }
    if (src_open)
    {
        (void)File_Close(&src);
    }

    if (r != FM_OK)
    {
        /* کپی ناقص را جا نگذار. */
        (void)File_Delete(dst_path);
        return r;
    }

    if (out_info != NULL)
    {
        out_info->bytes = done;
        out_info->crc_source = crc_src;
    }

    /* بررسی صحت: مقصد را دوباره بخوان و CRC بگیر. */
    if (o->verify)
    {
        FM_File chk;
        uint32_t crc_dst = 0;

        memset(&chk, 0, sizeof(chk));
        r = File_OpenEx(&chk, dst_path, FM_OPEN_READ);
        if (r != FM_OK)
        {
            return r;
        }
        for (;;)
        {
            uint32_t got = 0;

            r = File_Read(&chk, g_xfer_buf, (uint32_t)sizeof(g_xfer_buf),
                          &got);
            if (r == FM_ERR_EOF)
            {
                r = FM_OK;
                break;
            }
            if (r != FM_OK || got == 0u)
            {
                break;
            }
            crc_dst = CRC_Compute32(g_xfer_buf, got, crc_dst);
        }
        (void)File_Close(&chk);
        if (r != FM_OK)
        {
            return r;
        }
        if (out_info != NULL)
        {
            out_info->crc_dest = crc_dst;
            out_info->verified = FM_TRUE;
        }
        if (crc_dst != crc_src)
        {
            return FM_ERR_CORRUPTED;
        }
    }
    return FM_OK;
}

FM_Result FM_Transfer_Copy(const char *src_path, const char *dst_path,
                           const FM_TransferOpts *opts)
{
    return FM_Transfer_CopyEx(src_path, dst_path, opts, NULL);
}

FM_Result FM_Transfer_Move(const char *src_path, const char *dst_path,
                           const FM_TransferOpts *opts)
{
    FM_Result r = FM_Transfer_CopyEx(src_path, dst_path, opts, NULL);

    if (r != FM_OK)
    {
        return r;
    }
    return File_Delete(src_path);
}

/* ------------------------------------------------------------------ */
/* بافر <-> فایل                                                       */
/* ------------------------------------------------------------------ */

FM_Result FM_Transfer_LoadFile(const char *path, void *dst, size_t dst_cap,
                               size_t *out_len)
{
    FM_File f;
    FM_Result r;
    uint64_t size = 0;
    size_t done = 0;
    uint8_t *p = (uint8_t *)dst;

    if (path == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (out_len != NULL)
    {
        *out_len = 0;
    }
    memset(&f, 0, sizeof(f));
    r = File_OpenEx(&f, path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    (void)File_Size(&f, &size);
    if (size > (uint64_t)dst_cap)
    {
        (void)File_Close(&f);
        return FM_ERR_TOO_LARGE;
    }
    for (;;)
    {
        uint32_t want;
        uint32_t got = 0;
        size_t left = dst_cap - done;

        if (left == 0u)
        {
            break;
        }
        want = (left > 0xFFFF0000u) ? 0xFFFF0000u : (uint32_t)left;
        r = File_Read(&f, p + done, want, &got);
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            break;
        }
        if (r != FM_OK)
        {
            break;
        }
        if (got == 0u)
        {
            break;
        }
        done += got;
    }
    (void)File_Close(&f);
    if (r != FM_OK)
    {
        return r;
    }
    if (out_len != NULL)
    {
        *out_len = done;
    }
    return FM_OK;
}

FM_Result FM_Transfer_SaveFile(const char *path, const void *src, size_t len)
{
    if (path == NULL || (src == NULL && len != 0u))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return File_WriteAll(path, src, len);
}

/* ------------------------------------------------------------------ */
/* سطح خام                                                             */
/* ------------------------------------------------------------------ */

FM_Result FM_Transfer_Raw(FM_Storage *src, uint64_t src_off,
                          FM_Storage *dst, uint64_t dst_off,
                          uint64_t bytes, const FM_TransferOpts *opts)
{
    FM_TransferOpts tmp;
    const FM_TransferOpts *o = xfer_opts(opts, &tmp);
    uint64_t done = 0;
    FM_Result r;
    uint32_t chunk = (uint32_t)sizeof(g_xfer_buf);
    uint32_t unit_mask = 0u;

    FM_CHECK_PTR(src, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(dst, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(src->read != NULL, FM_ERR_INVALID_ARGUMENT,
             "source has no read function");
    FM_CHECK(dst->write != NULL, FM_ERR_INVALID_ARGUMENT,
             "destination has no write function");

    /* فلش NOR فقط بعد از erase قابل نوشتن است. اگر مقصد erase دارد،
     * ناحیه را پاک کن. */
    if (dst->erase != NULL && bytes > 0u)
    {
        r = dst->erase(dst, dst_off, (uint32_t)bytes);
        if (r != FM_OK && r != FM_ERR_UNSUPPORTED)
        {
            return r;
        }
    }

    /* Chunk size: as large as the staging buffer allows, but a whole
     * number of destination write units.  On NOR flash a write that
     * straddles a sector boundary forces a read-modify-erase-write of
     * two sectors instead of one, which is both slower and twice the
     * wear.  With a 4KB sector and a 4KB buffer this makes each chunk
     * land on exactly one sector. */
    {
        uint32_t unit = dst->erase_size;

        if (unit == 0u)
        {
            unit = dst->write_size;
        }
        if (unit != 0u && unit <= chunk)
        {
            uint32_t aligned = chunk - (chunk % unit);

            if (aligned != 0u)
            {
                chunk = aligned;
            }
        }
        /* Power-of-two erase units let us clamp to the next boundary
         * cheaply; anything else just uses the plain chunk size. */
        if (unit != 0u && (unit & (unit - 1u)) == 0u)
        {
            unit_mask = unit - 1u;
        }
        FM_TRACE_LOG("raw copy %lu bytes, chunk %lu (dst unit %lu)",
                     (unsigned long)bytes, (unsigned long)chunk,
                     (unsigned long)unit);
    }

    while (done < bytes)
    {
        uint64_t left = bytes - done;
        uint32_t n = (left > (uint64_t)chunk) ? chunk : (uint32_t)left;

        /* Keep each chunk inside one destination erase unit even when
         * the starting offset is not itself aligned. */
        if (unit_mask != 0u)
        {
            uint64_t pos = dst_off + done;
            uint32_t to_edge = (uint32_t)(unit_mask + 1u -
                                          (pos & unit_mask));

            if (n > to_edge)
            {
                n = to_edge;
            }
        }

        r = src->read(src, src_off + done, g_xfer_buf, n);
        if (r != FM_OK)
        {
            FM_ERR_LOG("read at %lu failed: %d",
                       (unsigned long)(src_off + done), (int)r);
            return r;
        }
        r = dst->write(dst, dst_off + done, g_xfer_buf, n);
        if (r != FM_OK)
        {
            FM_ERR_LOG("write at %lu failed: %d",
                       (unsigned long)(dst_off + done), (int)r);
            return r;
        }
        done += n;
        if (!xfer_report(o, done, bytes))
        {
            return FM_ERR_CANCELLED;
        }
    }
    if (dst->sync != NULL)
    {
        (void)dst->sync(dst);
    }
    if (o->verify)
    {
        return FM_Transfer_RawVerify(src, src_off, dst, dst_off, bytes);
    }
    return FM_OK;
}

FM_Result FM_Transfer_RawVerify(FM_Storage *a, uint64_t a_off,
                                FM_Storage *b, uint64_t b_off,
                                uint64_t bytes)
{
    uint64_t done = 0;
    /* بافر را نصف می‌کنیم تا هر دو طرف در همان بافر ایستا جا شوند. */
    const uint32_t half = (uint32_t)(sizeof(g_xfer_buf) / 2u);
    uint8_t *ba = g_xfer_buf;
    uint8_t *bb = g_xfer_buf + half;

    if (a == NULL || b == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    while (done < bytes)
    {
        uint64_t left = bytes - done;
        uint32_t n = (left > (uint64_t)half) ? half : (uint32_t)left;
        FM_Result r;

        r = a->read(a, a_off + done, ba, n);
        if (r != FM_OK)
        {
            return r;
        }
        r = b->read(b, b_off + done, bb, n);
        if (r != FM_OK)
        {
            return r;
        }
        if (memcmp(ba, bb, n) != 0)
        {
            return FM_ERR_CORRUPTED;
        }
        done += n;
    }
    return FM_OK;
}
