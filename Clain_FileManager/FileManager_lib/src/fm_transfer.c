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
#include "fm_internal.h"
#include "fm_cache.h"

/* بافر میانی. اندازه از fm_board_config.h می‌آید.
 * تراز ۳۲ بایتی لازم است چون ممکن است طرف مقابل DMA باشد و عملیات کش
 * روی خطوط ۳۲ بایتی انجام می‌شود.  The size is rounded up to a whole
 * number of 32-byte cache lines so that cache maintenance on the buffer
 * can never touch a neighbouring variable. */
#define XFER_BUF_SIZE ((((size_t)FM_TRANSFER_CHUNK) + 31u) & ~(size_t)31u)

#if defined(__GNUC__)
static uint8_t g_xfer_buf[XFER_BUF_SIZE] __attribute__((aligned(32)));
#else
static uint8_t g_xfer_buf[XFER_BUF_SIZE];
#endif

/* F-11 (b): the staging buffer is one static object.  A transfer started
 * while another one is running would overwrite the chunk in flight, so
 * every transfer holds the FM_SCRATCH_XFER claim: a transfer started
 * from the progress callback (nested) gets FM_ERR_BUSY, one started by
 * a second task waits for the first to finish.  (A sync port without
 * thread identity cannot tell the two apart: FM_ERR_BUSY for both.) */
static FM_Bool xfer_acquire(void)
{
    return (FM_Scratch_Enter(FM_SCRATCH_XFER) == FM_OK) ? FM_TRUE : FM_FALSE;
}

static void xfer_release(void)
{
    FM_Scratch_Leave(FM_SCRATCH_XFER);
}

/* F-11 (a): cache maintenance around every chunk (what fm_transfer.h
 * promises).  The transfer layer cannot know whether the driver behind a
 * read()/write() moves the bytes with DMA or with the CPU, so it uses a
 * sequence that is correct for both:
 *
 *   before read : Clean       -- no dirty line is left that could later
 *                                be written back over the DMA's data
 *   after read  : Clean, then Invalidate
 *                   DMA driver : the lines are clean, Clean does nothing,
 *                                Invalidate drops the stale copies
 *                   CPU driver : Clean writes the new bytes to RAM first,
 *                                so Invalidate cannot lose them
 *   before write: Clean       -- a DMA write sees what the CPU sees
 *
 * (A bare Invalidate after a CPU-copy read would throw the data away.)
 * On a core without D-cache, and on the host, the calls compile to
 * nothing.  FM_TRANSFER_CACHE_MAINT=0 removes them entirely;
 * FM_XFER_CACHE_CLEAN / FM_XFER_CACHE_INVALIDATE can be redirected to
 * instrumented functions (the host test suite counts them). */
#ifndef FM_TRANSFER_CACHE_MAINT
#define FM_TRANSFER_CACHE_MAINT 1
#endif

#if FM_TRANSFER_CACHE_MAINT
#ifdef FM_XFER_CACHE_CLEAN
extern void FM_XFER_CACHE_CLEAN(const void *addr, size_t bytes);
#else
#define FM_XFER_CACHE_CLEAN(addr, bytes) FM_Cache_CleanForDma((addr), (bytes))
#endif
#ifdef FM_XFER_CACHE_INVALIDATE
extern void FM_XFER_CACHE_INVALIDATE(void *addr, size_t bytes);
#else
#define FM_XFER_CACHE_INVALIDATE(addr, bytes) \
    FM_Cache_InvalidateAfterDma((addr), (bytes))
#endif

static void xfer_pre_read(void *buf, size_t cap)
{
    FM_XFER_CACHE_CLEAN(buf, cap);
}

static void xfer_post_read(void *buf, size_t got)
{
    if (got != 0u)
    {
        FM_XFER_CACHE_CLEAN(buf, got);
        FM_XFER_CACHE_INVALIDATE(buf, got);
    }
}

static void xfer_pre_write(const void *buf, size_t len)
{
    FM_XFER_CACHE_CLEAN(buf, len);
}
#else
#define xfer_pre_read(buf, cap)   ((void)0)
#define xfer_post_read(buf, got)  ((void)0)
#define xfer_pre_write(buf, len)  ((void)0)
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

/* The copy itself.  The staging buffer is already owned by the caller. */
static FM_Result xfer_copy_locked(const char *src_path, const char *dst_path,
                                  const FM_TransferOpts *o,
                                  FM_TransferInfo *out_info)
{
    FM_File src;
    FM_File dst;
    FM_Result r;
    FM_Result rc;
    uint64_t total = 0;
    uint64_t done = 0;
    uint32_t crc_src = 0;

    /* File_OpenEx returns FM_ERR_BUSY on a handle whose `used` flag is
     * set, and these live on the stack, so clear them first. */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    r = File_OpenEx(&src, src_path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    (void)File_Size(&src, &total);

    r = File_OpenEx(&dst, dst_path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        (void)File_Close(&src);
        return r;
    }

    for (;;)
    {
        uint32_t got = 0;
        uint32_t put = 0;

        xfer_pre_read(g_xfer_buf, sizeof(g_xfer_buf));
        r = File_Read(&src, g_xfer_buf, (uint32_t)sizeof(g_xfer_buf), &got);
        xfer_post_read(g_xfer_buf, got);
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
        xfer_pre_write(g_xfer_buf, got);
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
#if FM_USE_CRC
        if (o->verify)
        {
            crc_src = CRC_Compute32(g_xfer_buf, got, crc_src);
        }
#endif
        done += got;
        if (done >= total)
        {
            /* Same rule as File_Copy (F-05): flush before the report
             * that says "everything is there". */
            r = File_Flush(&dst);
            if (r != FM_OK)
            {
                break;
            }
        }
        if (!xfer_report(o, done, total))
        {
            r = FM_ERR_CANCELLED;
            break;
        }
    }

    rc = File_Close(&dst);
    if (r == FM_OK)
    {
        r = rc;                          /* خطای بستن را گم نکن */
    }
    (void)File_Close(&src);

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

#if FM_USE_CRC
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

            xfer_pre_read(g_xfer_buf, sizeof(g_xfer_buf));
            r = File_Read(&chk, g_xfer_buf, (uint32_t)sizeof(g_xfer_buf),
                          &got);
            xfer_post_read(g_xfer_buf, got);
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
        /* F-10: ALWAYS close what was opened.  `chk` lives on this stack
         * frame and File_OpenEx registered its address in the external
         * handle table; returning without closing leaves that table
         * pointing into a dead frame. */
        rc = File_Close(&chk);
        if (r == FM_OK)
        {
            r = rc;
        }
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
            /* F-10: a failed verify is a failed copy -- do not leave the
             * corrupt destination behind looking like a good file. */
            (void)File_Delete(dst_path);
            return FM_ERR_CORRUPTED;
        }
    }
#else
    (void)o;
#endif /* FM_USE_CRC */
    return FM_OK;
}

FM_Result FM_Transfer_CopyEx(const char *src_path, const char *dst_path,
                             const FM_TransferOpts *opts,
                             FM_TransferInfo *out_info)
{
    FM_TransferOpts tmp;
    const FM_TransferOpts *o = xfer_opts(opts, &tmp);
    FM_Result r;

    if (src_path == NULL || dst_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (out_info != NULL)
    {
        memset(out_info, 0, sizeof(*out_info));
    }
#if !FM_USE_CRC
    /* verify re-reads the destination and compares CRC-32s; without the
     * CRC module there is nothing to compare with.  Refuse up front --
     * before anything is opened or truncated -- instead of copying and
     * then reporting a check that never ran as passed. */
    if (o->verify)
    {
        return FM_ERR_UNSUPPORTED;
    }
#endif
    /* F-09: copying a file onto itself would open it for reading and then
     * truncate it through the second handle -- the file ends up empty and
     * the call reports success (and FM_Transfer_Move would then delete
     * it).  Same rule as File_Copy: compare the RESOLVED paths, so
     * "/a/b" and "/a/./b" are recognised as one file. */
    if (FM_File_SamePath(src_path, dst_path))
    {
        return FM_ERR_INVALID_ARGUMENT;
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

    if (!xfer_acquire())
    {
        return FM_ERR_BUSY;
    }
    r = xfer_copy_locked(src_path, dst_path, o, out_info);
    xfer_release();
    return r;
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

/* FM_TRUE when [off, off + bytes) lies inside the device.  Subtraction
 * form: off + bytes may wrap around uint64_t. */
static FM_Bool xfer_range_ok(const FM_Storage *s, uint64_t off,
                             uint64_t bytes)
{
    return (off <= s->capacity && bytes <= s->capacity - off) ? FM_TRUE
                                                              : FM_FALSE;
}

/* F-07: erase the destination range one erase unit at a time.
 *
 *  - every request is exactly one aligned unit (the adapters in
 *    fm_device.c reject anything else);
 *  - the 64-bit length is never squeezed through a uint32_t
 *    (a multiple of 4 GiB used to become "erase 0 bytes");
 *  - the progress callback runs before every unit, so a long erase can
 *    be cancelled.  It is called with done = 0: nothing has been copied
 *    yet, and the bar must not run to 100 % twice.
 *
 * The caller has verified that dst_off is unit-aligned.  The END of the
 * range is rounded up: the last unit is erased whole, as every flash
 * programming tool does. */
static FM_Result xfer_erase_range(FM_Storage *dst, uint64_t dst_off,
                                  uint64_t bytes, const FM_TransferOpts *o)
{
    const uint32_t unit = dst->erase_size;
    uint64_t at = dst_off;
    uint64_t end = dst_off + bytes;      /* in range: checked by caller */

    if (unit != 0u)
    {
        uint64_t tail = end % unit;

        if (tail != 0u)
        {
            end += (uint64_t)unit - tail;
        }
        if (end > dst->capacity)
        {
            end = dst->capacity;
        }
    }
    while (at < end)
    {
        /* no erase unit declared: hand the range over in pieces that
         * fit the 32-bit length of the driver callback */
        const uint32_t step = (unit != 0u) ? unit : 0x40000000u;
        uint64_t left = end - at;
        uint32_t n = (left > (uint64_t)step) ? step : (uint32_t)left;
        FM_Result r;

        if (!xfer_report(o, 0u, bytes))
        {
            return FM_ERR_CANCELLED;
        }
        r = dst->erase(dst, at, n);
        if (r == FM_ERR_UNSUPPORTED)
        {
            break;                       /* driver erases on write */
        }
        if (r != FM_OK)
        {
            FM_ERR_LOG("erase at %lu failed: %d", (unsigned long)at, (int)r);
            return r;
        }
        at += n;
    }
    return FM_OK;
}

static FM_Result xfer_raw_verify_locked(FM_Storage *a, uint64_t a_off,
                                        FM_Storage *b, uint64_t b_off,
                                        uint64_t bytes)
{
    uint64_t done = 0;
    /* بافر را نصف می‌کنیم تا هر دو طرف در همان بافر ایستا جا شوند.
     * Each half is a whole number of cache lines. */
    const uint32_t half = (uint32_t)((sizeof(g_xfer_buf) / 2u) & ~(size_t)31u);
    uint8_t *ba = g_xfer_buf;
    uint8_t *bb = g_xfer_buf + half;

    while (done < bytes)
    {
        uint64_t left = bytes - done;
        uint32_t n = (left > (uint64_t)half) ? half : (uint32_t)left;
        FM_Result r;

        xfer_pre_read(ba, n);
        r = a->read(a, a_off + done, ba, n);
        xfer_post_read(ba, n);
        if (r != FM_OK)
        {
            return r;
        }
        xfer_pre_read(bb, n);
        r = b->read(b, b_off + done, bb, n);
        xfer_post_read(bb, n);
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

static FM_Result xfer_raw_locked(FM_Storage *src, uint64_t src_off,
                                 FM_Storage *dst, uint64_t dst_off,
                                 uint64_t bytes, const FM_TransferOpts *o)
{
    uint64_t done = 0;
    FM_Result r;
    uint32_t chunk = (uint32_t)sizeof(g_xfer_buf);
    uint32_t unit_mask = 0u;
    FM_Bool synced = FM_FALSE;

    /* فلش NOR فقط بعد از erase قابل نوشتن است. اگر مقصد erase دارد،
     * ناحیه را پاک کن. */
    if (dst->erase != NULL && bytes > 0u)
    {
        r = xfer_erase_range(dst, dst_off, bytes, o);
        if (r != FM_OK)
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

        xfer_pre_read(g_xfer_buf, n);
        r = src->read(src, src_off + done, g_xfer_buf, n);
        xfer_post_read(g_xfer_buf, n);
        if (r != FM_OK)
        {
            FM_ERR_LOG("read at %lu failed: %d",
                       (unsigned long)(src_off + done), (int)r);
            return r;
        }
        xfer_pre_write(g_xfer_buf, n);
        r = dst->write(dst, dst_off + done, g_xfer_buf, n);
        if (r != FM_OK)
        {
            FM_ERR_LOG("write at %lu failed: %d",
                       (unsigned long)(dst_off + done), (int)r);
            return r;
        }
        done += n;
        if (done == bytes && dst->sync != NULL)
        {
            /* durable before the 100 % report; a failed sync is a
             * failed write */
            r = dst->sync(dst);
            synced = FM_TRUE;
            if (r != FM_OK && r != FM_ERR_UNSUPPORTED)
            {
                FM_ERR_LOG("sync failed: %d", (int)r);
                return r;
            }
        }
        if (!xfer_report(o, done, bytes))
        {
            return FM_ERR_CANCELLED;
        }
    }
    if (!synced && dst->sync != NULL)
    {
        r = dst->sync(dst);
        if (r != FM_OK && r != FM_ERR_UNSUPPORTED)
        {
            return r;
        }
    }
    if (o->verify)
    {
        return xfer_raw_verify_locked(src, src_off, dst, dst_off, bytes);
    }
    return FM_OK;
}

FM_Result FM_Transfer_Raw(FM_Storage *src, uint64_t src_off,
                          FM_Storage *dst, uint64_t dst_off,
                          uint64_t bytes, const FM_TransferOpts *opts)
{
    FM_TransferOpts tmp;
    const FM_TransferOpts *o = xfer_opts(opts, &tmp);
    FM_Result r;

    FM_CHECK_PTR(src, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(dst, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(src->read != NULL, FM_ERR_INVALID_ARGUMENT,
             "source has no read function");
    FM_CHECK(dst->write != NULL, FM_ERR_INVALID_ARGUMENT,
             "destination has no write function");
    FM_CHECK(!o->verify || dst->read != NULL, FM_ERR_UNSUPPORTED,
             "verify needs a destination read function");

    /* F-06: the driver callbacks are thin and trust their caller (the
     * Storage_*Blocks wrappers do the same check).  Everything below is
     * decided BEFORE the first erase or write. */
    if (bytes > 0u)
    {
        FM_CHECK(xfer_range_ok(src, src_off, bytes), FM_ERR_INVALID_ARGUMENT,
                 "source range past capacity");
        FM_CHECK(xfer_range_ok(dst, dst_off, bytes), FM_ERR_INVALID_ARGUMENT,
                 "destination range past capacity");
        /* F-07: one policy for every adapter.  A start offset inside an
         * erase unit would mean erasing bytes in front of the requested
         * range (what the QSPI adapter silently did) or failing half-way
         * (what the block adapters did).  Neither: refuse it here. */
        FM_CHECK(dst->erase == NULL || dst->erase_size == 0u ||
                 (dst_off % dst->erase_size) == 0u,
                 FM_ERR_INVALID_ARGUMENT,
                 "destination offset is not aligned to the erase unit");
    }

    if (!xfer_acquire())
    {
        return FM_ERR_BUSY;
    }
    r = xfer_raw_locked(src, src_off, dst, dst_off, bytes, o);
    xfer_release();
    return r;
}

FM_Result FM_Transfer_RawVerify(FM_Storage *a, uint64_t a_off,
                                FM_Storage *b, uint64_t b_off,
                                uint64_t bytes)
{
    FM_Result r;

    if (a == NULL || b == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* F-08: same guards as FM_Transfer_Raw -- a storage without read()
     * is an error, not a NULL call. */
    FM_CHECK(a->read != NULL, FM_ERR_UNSUPPORTED,
             "source has no read function");
    FM_CHECK(b->read != NULL, FM_ERR_UNSUPPORTED,
             "target has no read function");
    if (bytes > 0u)
    {
        FM_CHECK(xfer_range_ok(a, a_off, bytes), FM_ERR_INVALID_ARGUMENT,
                 "verify range past source capacity");
        FM_CHECK(xfer_range_ok(b, b_off, bytes), FM_ERR_INVALID_ARGUMENT,
                 "verify range past target capacity");
    }
    if (!xfer_acquire())
    {
        return FM_ERR_BUSY;
    }
    r = xfer_raw_verify_locked(a, a_off, b, b_off, bytes);
    xfer_release();
    return r;
}
