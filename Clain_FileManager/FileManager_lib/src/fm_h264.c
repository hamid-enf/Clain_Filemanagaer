/*
 * FileManager - H.264 codec plugin (h264bsd backend)
 *
 * Wraps the vendored h264bsd decoder as an FM_VideoCodecOps.  Feeds the
 * decoder with Annex-B NAL units and converts the decoded YUV420p picture
 * to RGB888.
 *
 * Self-healing: h264bsd is a strict reference-chain decoder - one damaged
 * or missing frame (SD hiccup, missed IDR, mid-slice corruption) breaks
 * every P-frame until the next IDR.  The plugin therefore caches the most
 * recent keyframe payload and, when decoding stops producing pictures,
 * resets the decoder and replays the keyframe before retrying the current
 * frame.  Playback survives with at most a brief freeze instead of the
 * file (or the whole playlist) failing.
 *
 * Notes:
 *   - h264bsd allocates its reference-picture buffers with FM_Alloc/FM_Free;
 *     memory use scales with the stream resolution.  On MCUs this needs
 *     external RAM (SDRAM on STM32 H7, PSRAM on ESP32).
 *   - Decoding is single-instance per FM_Video (one decoder context).
 *
 * Usage:
 *     FM_Video v;
 *     Video_Open("/movie.mp4", &v);
 *     Video_RegisterCodec(&v, H264_GetCodecOps());
 *     uint8_t rgb[W*H*3];
 *     Video_DecodeFrame(&v, rgb, sizeof(rgb));
 */

#define FM_DEBUG_TAG "h264"
#include "fm_debug.h"
#include "fm_h264.h"
#include "fm_internal.h"
#include "fm_alloc.h"

#if FM_USE_H264

/* h264bsd_util.h defines MIN/MAX unconditionally; fm_types.h already
 * provided them.  Drop ours first so -Werror builds stay clean (the
 * definitions are semantically identical). */
#undef MIN
#undef MAX
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

#include <string.h>

/* Largest keyframe payload the plugin caches for self-healing replay.
 * 320x240 Baseline IDRs are typically 20-50 KB; the default covers
 * 640x480 as well.  Streams with bigger keyframes simply fall back to
 * "reset and retry" without replay. */
#ifndef FM_H264_KEYFRAME_CACHE_BYTES
#define FM_H264_KEYFRAME_CACHE_BYTES (192u * 1024u)
#endif

/* How many consecutive picture-less frames trigger one self-heal
 * (decoder reset + keyframe replay + retry).  Bounded so a truly dead
 * stream still reports errors to the caller instead of hiding them. */
#ifndef FM_H264_SELFHEAL_ATTEMPTS
#define FM_H264_SELFHEAL_ATTEMPTS 2u
#endif

typedef struct
{
    storage_t *storage;
    uint8_t  *kf_buf;        /* cached keyframe payload (Annex-B)      */
    uint8_t  *kf_scratch;    /* disposable copy fed to the decoder     */
    uint8_t  *frame_copy;    /* pristine copy of the frame being decoded */
    uint32_t  frame_copy_len;
    uint32_t  kf_len;        /* cached keyframe byte count             */
    uint32_t  bad_run;       /* consecutive frames without a picture   */
    uint16_t  out_w;         /* size of the last picture written       */
    uint16_t  out_h;
    uint32_t  resets;        /* self-heal decoder resets so far        */
} H264Ctx;

/* Diagnostics for the last failed decode (FM_ERR_INVALID_FORMAT /
 * FM_ERR_NO_MEMORY).  Filled by h264_decode, readable by applications
 * via H264_GetLastError() to tell decoder rejections apart from
 * corrupted input without a debugger. */
static FM_H264Diag g_h264_diag;

const FM_H264Diag *H264_GetLastError(void)
{
    return &g_h264_diag;
}

static FM_Result h264_init(void **ctx, const FM_VideoInfo *info);
static FM_Result h264_decode(void *ctx, const void *compressed, size_t size,
                             void *frame_out, size_t frame_cap);
static void h264_close(void *ctx);

static const FM_VideoCodecOps g_h264_ops = {
    "H264 (h264bsd)",
    h264_init,
    h264_decode,
    h264_close
};

const FM_VideoCodecOps *H264_GetCodecOps(void)
{
    return &g_h264_ops;
}

FM_Result H264_GetOutputSize(void *ctx, uint16_t *width, uint16_t *height)
{
    const H264Ctx *h = (const H264Ctx *)ctx;

    if (h == NULL || width == NULL || height == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (h->out_w == 0u || h->out_h == 0u)
    {
        return FM_ERR_NOT_READY;
    }
    *width = h->out_w;
    *height = h->out_h;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* YUV420p -> RGB888 (BT.601, integer math)                             */
/*                                                                      */
/* H.264 luma is 16..235 and chroma 16..240 ("video range") unless the  */
/* stream's VUI sets video_full_range_flag; a stream without VUI is     */
/* video range.  Converting such a picture with the full-range matrix   */
/* (as this file did for every stream) gives black at 16 and white at   */
/* 235: a flat, grey picture, about 25 dB away from the reference       */
/* decoder's output.                                                    */
/* ------------------------------------------------------------------ */
static int clamp8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* Converts the crop_w x crop_h window whose top-left corner is at
 * (left, top) of a w x h picture.  The caller guarantees
 * left + crop_w <= w and top + crop_h <= h.
 *
 * The window origin used to be ignored (conversion always started at
 * 0,0): a stream cropped on the left/top came out shifted by the crop
 * offset, with the padding the encoder wanted hidden shown instead. */
static void yuv420_to_rgb888(const uint8_t *yuv, uint32_t w, uint32_t h,
                             uint32_t left, uint32_t top,
                             uint32_t crop_w, uint32_t crop_h,
                             FM_Bool full_range, uint8_t *rgb)
{
    const uint8_t *Y = yuv;
    const uint8_t *U = yuv + (size_t)w * h;
    const uint8_t *V = yuv + (size_t)w * h + ((size_t)w * h) / 4u;
    uint32_t y;
    uint32_t x;

    for (y = 0; y < crop_h; y++)
    {
        const size_t sy = (size_t)top + y;
        const uint8_t *yrow = Y + sy * w;
        const uint8_t *urow = U + (sy / 2u) * (w / 2u);
        const uint8_t *vrow = V + (sy / 2u) * (w / 2u);

        for (x = 0; x < crop_w; x++)
        {
            const size_t sx = (size_t)left + x;
            int yy = yrow[sx];
            int uu = urow[sx / 2u] - 128;
            int vv = vrow[sx / 2u] - 128;
            int r;
            int g;
            int b;
            uint8_t *p = rgb + ((size_t)y * crop_w + x) * 3u;

            if (full_range)
            {
                r = yy + ((359 * vv) >> 8);
                g = yy - ((88 * uu + 183 * vv) >> 8);
                b = yy + ((454 * uu) >> 8);
            }
            else
            {
                /* 255/219 on luma, 255/224 on chroma, rounded */
                const int c = 298 * (yy - 16) + 128;

                r = (c + 409 * vv) >> 8;
                g = (c - 100 * uu - 208 * vv) >> 8;
                b = (c + 516 * uu) >> 8;
            }

            p[0] = (uint8_t)clamp8(r);
            p[1] = (uint8_t)clamp8(g);
            p[2] = (uint8_t)clamp8(b);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Plugin callbacks                                                    */
/* ------------------------------------------------------------------ */
static FM_Result h264_init(void **ctx, const FM_VideoInfo *info)
{
    H264Ctx *h;

    (void)info;
    if (ctx == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    h = (H264Ctx *)FM_Calloc(1, sizeof(H264Ctx));
    if (h == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    h->storage = h264bsdAlloc();
    if (h->storage == NULL)
    {
        FM_Free(h);
        return FM_ERR_NO_MEMORY;
    }
    if (h264bsdInit(h->storage, HANTRO_FALSE) != HANTRO_OK)
    {
        h264bsdFree(h->storage);
        FM_Free(h);
        return FM_ERR_NO_MEMORY;
    }
    *ctx = h;
    return FM_OK;
}

static void h264_close(void *ctx)
{
    H264Ctx *h = (H264Ctx *)ctx;

    if (h == NULL)
    {
        return;
    }
    if (h->storage != NULL)
    {
        h264bsdShutdown(h->storage);
        h264bsdFree(h->storage);
    }
    if (h->kf_buf != NULL)
    {
        FM_Free(h->kf_buf);
    }
    if (h->kf_scratch != NULL)
    {
        FM_Free(h->kf_scratch);
    }
    if (h->frame_copy != NULL)
    {
        FM_Free(h->frame_copy);
    }
    FM_Free(h);
}

/* Tear down and re-create the h264bsd decoder state. */
static FM_Result h264_reset_decoder(H264Ctx *h)
{
    if (h->storage != NULL)
    {
        h264bsdShutdown(h->storage);
        h264bsdFree(h->storage);
        h->storage = NULL;
    }
    h->storage = h264bsdAlloc();
    if (h->storage == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    if (h264bsdInit(h->storage, HANTRO_FALSE) != HANTRO_OK)
    {
        h264bsdFree(h->storage);
        h->storage = NULL;
        return FM_ERR_NO_MEMORY;
    }
    return FM_OK;
}

/* Walk the Annex-B start codes of `s` and call back with each NAL type.
 * Returns 1 as soon as `want_a` and `want_b` have both been seen (pass
 * want_b < 0 to look for a single type). */
static int h264_blob_scan_nals(const uint8_t *s, size_t n, int want_a,
                               int want_b)
{
    size_t i = 0;
    int seen_a = 0;
    int seen_b = (want_b < 0) ? 1 : 0;

    while (i + 4 <= n)
    {
        size_t sc;
        size_t j;
        int type;

        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
        {
            sc = 3;
        }
        else if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 0 && s[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            i++;
            continue;
        }
        type = s[i + sc] & 0x1F;
        if (type == want_a)
        {
            seen_a = 1;
        }
        if (want_b >= 0 && type == want_b)
        {
            seen_b = 1;
        }
        if (seen_a && seen_b)
        {
            return 1;
        }
        /* find the next start code */
        j = i + sc;
        while (j + 3 <= n)
        {
            if (s[j] == 0 && s[j + 1] == 0 &&
                (s[j + 2] == 1 ||
                 (s[j + 2] == 0 && s[j + 3] == 1)))
            {
                break;
            }
            j++;
        }
        i = j;
    }
    return 0;
}

/* True if the blob carries both a sequence (7) and picture (8) parameter
 * set, i.e. everything the decoder needs to activate them. */
static int h264_blob_has_param_sets(const uint8_t *s, size_t n)
{
    return h264_blob_scan_nals(s, n, 7, 8);
}

/* True if the Annex-B blob contains an IDR (NAL type 5): the frame a
 * decoder restart can safely re-sync on. */
static int h264_blob_is_keyframe(const uint8_t *s, size_t n)
{
    size_t i = 0;

    while (i + 4 <= n)
    {
        size_t sc;
        size_t j;
        int type;

        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
        {
            sc = 3;
        }
        else if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 0 && s[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            i++;
            continue;
        }
        type = s[i + sc] & 0x1F;
        if (type == 5)
        {
            return 1;
        }
        /* find the next start code */
        j = i + sc;
        while (j + 3 <= n)
        {
            if (s[j] == 0 && s[j + 1] == 0 &&
                (s[j + 2] == 1 ||
                 (s[j + 2] == 0 && s[j + 3] == 1)))
            {
                break;
            }
            j++;
        }
        i = j;
    }
    return 0;
}

/* Cache the payload of the latest keyframe (lazily allocating the
 * buffer through FM_Alloc, i.e. SDRAM when redirected). */
static void h264_maybe_cache_keyframe(H264Ctx *h, const uint8_t *s,
                                      size_t n)
{
    if (n == 0u || n > (size_t)FM_H264_KEYFRAME_CACHE_BYTES ||
        !h264_blob_is_keyframe(s, n))
    {
        return;
    }
    if (h->kf_buf == NULL)
    {
        h->kf_buf = (uint8_t *)FM_Alloc(FM_H264_KEYFRAME_CACHE_BYTES);
        if (h->kf_buf == NULL)
        {
            /* Out of memory for the self-heal cache.  The allocation
             * failure is recorded by FM_Alloc, so h264_decode still
             * reports NO_MEMORY rather than letting the subsequent
             * decode fail as a bogus "invalid format": without the
             * cached keyframe the self-heal retry has nothing to
             * re-prime the decoder with and every later frame breaks. */
            h->kf_len = 0u;
            return;
        }
    }
    memcpy(h->kf_buf, s, n);
    h->kf_len = (uint32_t)n;
}

/* One bounded decode pass over a single frame payload.  Returns FM_OK
 * when a picture was produced, or an FM_ERR_* describing the failure.
 * This is the historical error-resilient loop; the self-healing wrapper
 * around it lives in h264_decode(). */
static FM_Result h264_decode_attempt(H264Ctx *h, const uint8_t *stream,
                                     uint32_t size, uint32_t in_len,
                                     void *frame_out, size_t frame_cap)
{
    const uint8_t *compressed_base = stream;
    uint32_t len = size;
    uint32_t no_progress = 0;
    unsigned long alloc_fail_start = FM_Alloc_FailureCount();
    /* Did the input carry picture data at all?  A packet of parameter
     * sets / SEI / AUD only legitimately yields no picture; that must
     * not be reported like a damaged frame (see the end of the loop). */
    FM_Bool saw_slice = FM_FALSE;

    while (len > 0)
    {
        uint32_t read_bytes = 0;
        uint32_t res;

        /* Count parameter-set NALs as they are handed over, so the
         * board log shows whether the SPS/PPS ever reached h264bsd at
         * all -- the difference between a demuxer that drops them and a
         * decoder that refuses to store them. */
        if (len > 4u && stream[0] == 0u && stream[1] == 0u)
        {
            const uint8_t *nal = NULL;

            if (stream[2] == 1u)
            {
                nal = stream + 3;
            }
            else if (len > 5u && stream[2] == 0u && stream[3] == 1u)
            {
                nal = stream + 4;
            }
            if (nal != NULL)
            {
                uint32_t t = (uint32_t)(*nal & 0x1Fu);

                if (t >= 1u && t <= 5u)
                {
                    saw_slice = FM_TRUE;   /* coded slice (VCL) NAL */
                }
                if (t == 7u)
                {
                    g_h264_diag.sps_seen++;
                }
                else if (t == 8u)
                {
                    g_h264_diag.pps_seen++;
                }
            }
        }
        res = h264bsdDecode(h->storage, (uint8_t *)stream, len, 0,
                            &read_bytes);

        if (res == H264BSD_PIC_RDY)
        {
            uint32_t pic_id;
            uint32_t is_idr;
            uint32_t num_err_mbs;
            uint32_t crop_flag;
            uint32_t left;
            uint32_t top;
            uint32_t cw;
            uint32_t ch;
            uint32_t buf_w = h264bsdPicWidth(h->storage) * 16u;
            uint32_t buf_h = h264bsdPicHeight(h->storage) * 16u;
            uint8_t *pic = h264bsdNextOutputPicture(h->storage, &pic_id,
                                                    &is_idr, &num_err_mbs);

            (void)pic_id;
            (void)is_idr;
            if (pic == NULL)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            h264bsdCroppingParams(h->storage, &crop_flag, &left, &cw,
                                  &top, &ch);
            if (!crop_flag)
            {
                left = 0u;
                top = 0u;
                cw = buf_w;
                ch = buf_h;
            }
            /* The cropping rectangle comes from the bitstream: one that
             * does not lie inside the decoded picture would read past
             * the picture buffer.  Fall back to the whole picture. */
            if (cw == 0u || ch == 0u || left > buf_w || top > buf_h ||
                cw > buf_w - left || ch > buf_h - top)
            {
                left = 0u;
                top = 0u;
                cw = buf_w;
                ch = buf_h;
            }
            if (cw > 0xFFFFu || ch > 0xFFFFu ||
                (uint64_t)cw * ch * 3u > frame_cap)
            {
                return FM_ERR_TOO_LARGE;
            }
            yuv420_to_rgb888(pic, buf_w, buf_h, left, top, cw, ch,
                             (FM_Bool)(h264bsdVideoRange(h->storage) != 0u),
                             (uint8_t *)frame_out);
            h->out_w = (uint16_t)cw;
            h->out_h = (uint16_t)ch;
            return FM_OK;
        }
        if (res == H264BSD_ERROR)
        {
            /* h264bsd flags stray/damaged NAL units (or SEI payloads it
             * cannot parse) as H264BSD_ERROR but stays in sync: skip the
             * consumed bytes and keep decoding the remaining NALs of the
             * frame instead of rejecting the whole file.  Bail out only
             * when the decoder makes no forward progress. */
            g_h264_diag.bsd_result = res;
            g_h264_diag.consumed = in_len - len;
            memcpy(g_h264_diag.fail_bytes, stream,
                   (len < sizeof(g_h264_diag.fail_bytes))
                       ? len : sizeof(g_h264_diag.fail_bytes));
            if (g_h264_diag.skipped == 0u)
            {
                /* Remember what h264bsd threw away first: a skipped
                 * SPS/PPS (type 7/8) is fatal -- every later slice then
                 * fails parameter-set lookup -- while a skipped SEI
                 * (type 6) is harmless. */
                const uint8_t *nal = NULL;

                if (len > 4u && stream[0] == 0u && stream[1] == 0u)
                {
                    if (stream[2] == 1u)
                    {
                        nal = stream + 3;
                    }
                    else if (len > 5u && stream[2] == 0u && stream[3] == 1u)
                    {
                        nal = stream + 4;
                    }
                }
                g_h264_diag.skip_nal_type =
                    (nal != NULL) ? (uint32_t)(*nal & 0x1Fu) : 0xFFu;
            }
            g_h264_diag.skipped++;
            if (read_bytes == 0 || read_bytes > len)
            {
                /* re-sync manually: find the next start code */
                uint32_t k;

                for (k = 1; k + 2 < len; k++)
                {
                    if (stream[k] == 0 && stream[k + 1] == 0 &&
                        (stream[k + 2] == 1 ||
                         (k + 3 < len && stream[k + 2] == 0 &&
                          stream[k + 3] == 1)))
                    {
                        break;
                    }
                }
                if (k + 2 >= len)
                {
                    /* The NAL h264bsd choked on was the last one.  If
                     * no slice was involved it was trailing SEI/filler:
                     * nothing is wrong with the reference chain. */
                    return saw_slice ? FM_ERR_INVALID_FORMAT
                                     : FM_ERR_NOT_READY;
                }
                read_bytes = k;
            }
            if (g_h264_diag.skipped > 32u)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            len -= read_bytes;
            stream += read_bytes;
            continue;
        }
        if (res == H264BSD_PARAM_SET_ERROR)
        {
            g_h264_diag.bsd_result = res;
            g_h264_diag.consumed = in_len - len;
            g_h264_diag.activate_fail = h264bsdLastActivateFail;
            g_h264_diag.aub_fail = h264bsdLastAubFail;
            g_h264_diag.aub_pps_id = h264bsdLastAubPpsId;
            memcpy(g_h264_diag.fail_bytes, stream,
                   (len < sizeof(g_h264_diag.fail_bytes))
                       ? len : sizeof(g_h264_diag.fail_bytes));
            /* A param-set error on a frame that *does* carry a valid
             * SPS+PPS means the sets were parsed but never activated.
             * Only report that as out-of-memory when the allocator
             * actually refused a request during this decode.
             *
             * Guessing from the blob alone (the original heuristic) is
             * wrong whenever the real cause is a miscompiled decoder:
             * an aliasing-optimised h264bsd fails activation on a
             * perfectly valid stream with a perfectly healthy pool, and
             * the player then printed "out of memory" and told the user
             * to enlarge an 8 MB pool that was already 99% free. */
            if (FM_Alloc_FailureCount() != alloc_fail_start &&
                h264_blob_has_param_sets((const uint8_t *)compressed_base,
                                         in_len))
            {
                return FM_ERR_NO_MEMORY;
            }
            return FM_ERR_INVALID_FORMAT;
        }
        if (res == H264BSD_MEMALLOC_ERROR)
        {
            g_h264_diag.bsd_result = res;
            g_h264_diag.consumed = in_len - len;
            return FM_ERR_NO_MEMORY;
        }
        /* H264BSD_RDY or H264BSD_HDRS_RDY.  HDRS_RDY is emitted after the
         * parameter sets are decoded and may not consume any input, so
         * retry (bounded) instead of treating zero progress as an error. */
        if (read_bytes == 0)
        {
            no_progress++;
            if (no_progress > 4)
            {
                g_h264_diag.bsd_result = res;
                g_h264_diag.consumed = in_len - len;
                return FM_ERR_INVALID_FORMAT;
            }
            continue;
        }
        no_progress = 0;
        len -= read_bytes;
        stream += read_bytes;
        /* Long decode: let the application pump audio DMA etc. */
        FM_Idle();
    }
    /* No picture became ready from this input. */
    g_h264_diag.bsd_result = 0xFFFFFFFFu; /* consumed all, no picture */
    g_h264_diag.consumed = in_len;
    /* Input with no slice NAL in it (parameter sets, SEI, AUD) cannot
     * produce a picture: that is "nothing to show yet", not a damaged
     * frame.  Reporting it as INVALID_FORMAT made h264_decode() take it
     * for a broken reference chain and reset the decoder + replay the
     * keyframe -- on a perfectly healthy stream. */
    return saw_slice ? FM_ERR_INVALID_FORMAT : FM_ERR_NOT_READY;
}

static FM_Result h264_decode_claimed(void *ctx, const void *compressed,
                                     size_t size, void *frame_out,
                                     size_t frame_cap);

/* One H.264 access unit at a time, library-wide.  The decoder instance
 * (H264Ctx) is per video, but a decode also runs on process-wide state:
 * the last-error diagnostics (g_h264_diag, which the retry / NO_MEMORY
 * classification below reads back), h264bsd's own failure latches
 * (h264bsdLastActivateFail ...) and the allocator failure counter.  Two
 * tasks decoding two clips would mix them up -- e.g. report one clip's
 * exhausted pool as the other's FM_ERR_NO_MEMORY.  A second task waits;
 * a nested decode from the FM_SetIdle hook gets FM_ERR_BUSY. */
static FM_Result h264_decode(void *ctx, const void *compressed, size_t size,
                             void *frame_out, size_t frame_cap)
{
    FM_Result r = FM_Scratch_Enter(FM_SCRATCH_H264);

    if (r != FM_OK)
    {
        return r;
    }
    r = h264_decode_claimed(ctx, compressed, size, frame_out, frame_cap);
    FM_Scratch_Leave(FM_SCRATCH_H264);
    return r;
}

static FM_Result h264_decode_claimed(void *ctx, const void *compressed,
                                     size_t size, void *frame_out,
                                     size_t frame_cap)
{
    H264Ctx *h = (H264Ctx *)ctx;
    const uint8_t *stream = (const uint8_t *)compressed;
    unsigned long alloc_fail_before;
    FM_Result r;

    if (h == NULL || h->storage == NULL || compressed == NULL ||
        frame_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (size > 0xFFFFFFFFu)
    {
        return FM_ERR_TOO_LARGE;
    }
    /* Arm the diagnostics with this frame's identity up front. */
    memset(&g_h264_diag, 0, sizeof(g_h264_diag));
    g_h264_diag.input_len = (uint32_t)size;
    memcpy(g_h264_diag.first_bytes, stream,
           (size < sizeof(g_h264_diag.first_bytes))
               ? size : sizeof(g_h264_diag.first_bytes));

    /* Watch the global allocator: h264bsd swallows many ALLOCATE
     * failures and reports them as ordinary bitstream errors deep
     * inside a NAL parser (see the H264BSD_MEMALLOC_ERROR patches in
     * h264bsd_decoder.c).  Any decode that fails while the allocator
     * also handed back NULL is an out-of-memory condition, not a
     * corrupt file, and must say so — otherwise an FM_Alloc pool that
     * is merely too small looks like nine unplayable clips. */
    alloc_fail_before = FM_Alloc_FailureCount();
    h264bsdLastActivateFail = 0u;
    h264bsdLastAubFail = 0u;
    h264bsdLastAubPpsId = 0u;

    h264_maybe_cache_keyframe(h, stream, size);

    /* h264bsdExtractNalUnit strips emulation-prevention bytes IN PLACE,
     * so the decoder destroys the caller's frame while reading it: every
     * 0x000003 collapses to 0x0000 and the NAL shrinks.  A retry that
     * re-reads the same buffer therefore parses rubbish -- the SPS is
     * rejected (skipnal=7) and the slice that follows cannot find its
     * parameter set (res=4, used=664).  Keep a pristine copy so the
     * self-heal retry below replays the ORIGINAL bytes. */
    h->frame_copy_len = 0u;
    if (size <= (size_t)FM_H264_KEYFRAME_CACHE_BYTES)
    {
        if (h->frame_copy == NULL)
        {
            h->frame_copy =
                (uint8_t *)FM_Alloc(FM_H264_KEYFRAME_CACHE_BYTES);
        }
        if (h->frame_copy != NULL)
        {
            memcpy(h->frame_copy, stream, size);
            h->frame_copy_len = (uint32_t)size;
        }
    }

    r = h264_decode_attempt(h, stream, (uint32_t)size, (uint32_t)size,
                            frame_out, frame_cap);
    g_h264_diag.alloc_failures =
        (uint32_t)(FM_Alloc_FailureCount() - alloc_fail_before);
    if (r == FM_OK)
    {
        h->bad_run = 0u;
        return FM_OK;
    }
    if (g_h264_diag.alloc_failures != 0u)
    {
        return FM_ERR_NO_MEMORY;
    }
    /* Non-recoverable or resource errors: do not "heal" over them.
     * FM_ERR_NOT_READY (packet held no picture) is not an error at all:
     * it neither counts towards bad_run nor resets the decoder. */
    if (r != FM_ERR_INVALID_FORMAT)
    {
        return r;
    }
    /* Self-heal: a broken reference chain (one damaged/missed frame)
     * makes every following P-frame fail.  Reset the decoder, replay the
     * cached keyframe to re-prime it, then retry this frame once. */
    h->bad_run++;
    if (h->bad_run > (uint32_t)FM_H264_SELFHEAL_ATTEMPTS)
    {
        return r;
    }
    {
        FM_Result r2;

        g_h264_diag.resets++;
        h->resets++;
        if (h264_reset_decoder(h) != FM_OK)
        {
            return FM_ERR_NO_MEMORY;
        }
        if (h->kf_len != 0u)
        {
            /* h264bsdExtractNalUnit strips emulation-prevention bytes
             * IN PLACE, so handing it h->kf_buf directly destroys the
             * cache on first use: the 0x000003 sequences inside the
             * cached SPS collapse to 0x0000, and every later replay
             * feeds a corrupted SPS that h264bsd rejects (skipnal=7).
             * The slice that follows then cannot find its parameter set
             * and the whole clip is declared unplayable.  Replay from a
             * disposable copy and keep the cache pristine. */
            if (h->kf_scratch == NULL)
            {
                h->kf_scratch =
                    (uint8_t *)FM_Alloc(FM_H264_KEYFRAME_CACHE_BYTES);
            }
            if (h->kf_scratch != NULL)
            {
                memcpy(h->kf_scratch, h->kf_buf, h->kf_len);
                (void)h264_decode_attempt(h, h->kf_scratch, h->kf_len,
                                          h->kf_len, frame_out, frame_cap);
            }
        }
        /* Replay the untouched copy: `stream` was rewritten in place by
         * the failed attempt above. */
        if (h->frame_copy_len == (uint32_t)size)
        {
            r2 = h264_decode_attempt(h, h->frame_copy, (uint32_t)size,
                                     (uint32_t)size, frame_out, frame_cap);
        }
        else
        {
            r2 = h264_decode_attempt(h, stream, (uint32_t)size,
                                     (uint32_t)size, frame_out, frame_cap);
        }
        if (r2 == FM_OK)
        {
            h->bad_run = 0u;
            return FM_OK;
        }
        /* Any allocation refused anywhere in this decode -- including
         * inside the decoder reset and the keyframe replay above --
         * means the failure is a resource problem, not a bad file. */
        g_h264_diag.alloc_failures =
            (uint32_t)(FM_Alloc_FailureCount() - alloc_fail_before);
        if (r2 != FM_OK && g_h264_diag.alloc_failures != 0u)
        {
            return FM_ERR_NO_MEMORY;
        }
        return r2;
    }
}

#endif /* FM_USE_H264 */
