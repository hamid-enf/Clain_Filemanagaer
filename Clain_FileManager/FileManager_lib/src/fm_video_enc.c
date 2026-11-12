/*
 * FileManager - saving video: Motion-JPEG in an AVI file
 *
 * VideoFileWriter_* writes an AVI 1.0 file ("MJPG" video, optional 16-bit
 * PCM sound track) that PCs, phones, video editors and this library's
 * own player open.  Frames are given as pixels (encoded here with the
 * JPEG encoder of fm_jpeg.c) or as ready JPEG pictures (a camera in JPEG
 * mode, the JPEG peripheral).
 *
 * Video_Convert reads any video the library can decode and writes it as
 * such a file, with an optional callback that edits every frame.
 *
 * Why MJPEG: every frame is a picture of its own, so the encoder needs no
 * memory of earlier frames, no motion search and no floating point; a
 * Cortex-M can do it.  H.264 / HEVC / VP9 encoders need megabytes of
 * state and far more processor time than an MCU has; there is none here.
 *
 * Memory: one output buffer of FM_VIDEO_ENC_OUT_BYTES plus 512 bytes
 * (FM_Alloc, from Open to Close), whatever the picture size.  The frame
 * index (16 bytes per chunk) is kept in a side file "<name>.idx" next to
 * the video while it is written and moved into the AVI by Close, so a
 * long recording does not grow in RAM.  Two file handles are open.
 *
 * A file that was not closed (power lost) has no index and no lengths in
 * its header: most PC players still play it, this library's does not.
 */

#include "file_manager.h"
#define FM_DEBUG_TAG "videoenc"
#include "fm_debug.h"
#include "fm_video.h"
#include "fm_internal.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_VIDEO

#ifndef FM_VIDEO_ENC_OUT_BYTES
#define FM_VIDEO_ENC_OUT_BYTES 32768u
#endif
#ifndef FM_VIDEO_ENC_QUALITY
#define FM_VIDEO_ENC_QUALITY 80
#endif
/* AVI 1.0 holds up to 4 GB; many players (and FAT32 tools) are only
 * safe below 2 GB.  At this size writes return FM_ERR_TOO_LARGE; the
 * file is complete once closed and a new one can be started. */
#ifndef FM_VIDEO_ENC_MAX_BYTES
#define FM_VIDEO_ENC_MAX_BYTES 0x7F000000u
#endif
#ifndef FM_VIDEO_CONVERT_AUDIO_BYTES
#define FM_VIDEO_CONVERT_AUDIO_BYTES 16384u
#endif

/* Power-loss checkpoint.  Every time the file has grown by this many
 * bytes the header is rewritten with the counts so far and the file is
 * synced (File_Flush: the directory entry gets its size).  A recording
 * that is never closed (reset, power loss, card pulled) is then a valid
 * AVI up to the last checkpoint: it has no index yet, which players make
 * up for by reading the pictures in order, and VideoFile_Repair gives it
 * one.  The cost is one 0.3 KB write at the start of the file and one
 * sync.  0 = never. */
#ifndef FM_VIDEO_ENC_SYNC_BYTES
#define FM_VIDEO_ENC_SYNC_BYTES 1048576u
#endif

#define VW_INDEX_BUF 512u
#define VW_HDR_MAX   336u
/* FM_VideoFileWriter.guard of a writer that Open filled in (a writer is
 * usually a local variable: before the first Open `opened` is whatever
 * was in memory). */
#define VW_GUARD     0x56575231u                 /* "VWR1" */

static void vw_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void vw_wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static FM_Result vw_file_write(FM_File *f, const void *data, uint32_t len)
{
    uint32_t wr = 0;
    FM_Result r = File_Write(f, data, len, &wr);

    if (r == FM_OK && wr != len)
    {
        r = FM_ERR_NO_SPACE;
    }
    return r;
}

static uint64_t vw_pos(const FM_VideoFileWriter *w)
{
    return w->buf_pos + w->buf_len;
}

static void vw_flush(FM_VideoFileWriter *w)
{
    if (w->buf_len != 0u && w->error == FM_OK)
    {
        w->error = vw_file_write(&w->file, w->buf, w->buf_len);
    }
    w->buf_pos += w->buf_len;
    w->buf_len = 0u;
}

static void vw_put(FM_VideoFileWriter *w, const void *data, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)data;

    while (len != 0u)
    {
        uint32_t room = w->buf_cap - w->buf_len;
        uint32_t n = (len < room) ? len : room;

        if (n == 0u)
        {
            vw_flush(w);
            /* a block larger than the buffer goes straight to the card */
            if (len >= w->buf_cap)
            {
                if (w->error == FM_OK)
                {
                    w->error = vw_file_write(&w->file, s, len);
                }
                w->buf_pos += len;
                return;
            }
            continue;
        }
        memcpy(w->buf + w->buf_len, s, n);
        w->buf_len += n;
        s += n;
        len -= n;
    }
}

static FM_Result vw_jpeg_sink(void *ctx, const uint8_t *data, uint32_t len)
{
    FM_VideoFileWriter *w = (FM_VideoFileWriter *)ctx;

    vw_put(w, data, len);
    return w->error;
}

/* Start a chunk: 8 header bytes, kept whole inside the buffer so that
 * the length can usually be filled in without a seek. */
static uint64_t vw_chunk_begin(FM_VideoFileWriter *w, const char *fourcc)
{
    uint8_t h[8];
    uint64_t at;

    if (w->buf_cap - w->buf_len < 8u)
    {
        vw_flush(w);
    }
    at = vw_pos(w);
    memcpy(h, fourcc, 4);
    vw_wr32(h + 4, 0u);
    vw_put(w, h, 8u);
    return at;
}

/* Write 4 bytes at an earlier position of the file. */
static void vw_patch32(FM_VideoFileWriter *w, uint64_t at, uint32_t v)
{
    uint8_t b[4];

    vw_wr32(b, v);
    if (at >= w->buf_pos)
    {
        memcpy(w->buf + (size_t)(at - w->buf_pos), b, 4);
        return;
    }
    vw_flush(w);
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, (int64_t)at, FM_SEEK_SET);
    }
    if (w->error == FM_OK)
    {
        w->error = vw_file_write(&w->file, b, 4u);
    }
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, (int64_t)w->buf_pos, FM_SEEK_SET);
    }
}

static void vw_index_flush(FM_VideoFileWriter *w)
{
    if (w->ibuf_len != 0u && w->error == FM_OK)
    {
        w->error = vw_file_write(&w->index, w->ibuf, w->ibuf_len);
    }
    w->ibuf_len = 0u;
}

/* One entry of the index: the chunk at file position `at`, `size` bytes
 * of data. */
static void vw_index_add(FM_VideoFileWriter *w, uint64_t at,
                         const char *fourcc, uint32_t size)
{
    uint8_t e[16];

    memcpy(e, fourcc, 4);
    vw_wr32(e + 4, 0x10u);                       /* AVIIF_KEYFRAME */
    vw_wr32(e + 8, (uint32_t)(at - w->movi_pos));
    vw_wr32(e + 12, size);
    if (w->ibuf_len + 16u > VW_INDEX_BUF)
    {
        vw_index_flush(w);
    }
    memcpy(w->ibuf + w->ibuf_len, e, 16);
    w->ibuf_len += 16u;
    w->index_entries++;
}

/* End a chunk: length, pad byte, index entry.  Returns the length. */
static uint32_t vw_chunk_end(FM_VideoFileWriter *w, uint64_t at,
                             const char *fourcc, uint32_t *largest)
{
    uint32_t size = (uint32_t)(vw_pos(w) - (at + 8u));

    vw_patch32(w, at + 4u, size);
    if ((size & 1u) != 0u)
    {
        uint8_t zero = 0u;

        vw_put(w, &zero, 1u);
    }
    if (size > *largest)
    {
        *largest = size;
    }
    vw_index_add(w, at, fourcc, size);
    return size;
}

/* The whole header, with the values known so far; written with zero
 * counts by Open, with the counts so far by a checkpoint (`indexed`
 * FM_FALSE: the file has no "idx1" yet) and with the final ones by
 * Close.  Returns its size. */
static uint32_t vw_build_header(const FM_VideoFileWriter *w, uint8_t *h,
                                uint64_t file_size, uint64_t movi_end,
                                FM_Bool indexed)
{
    const FM_VideoSaveConfig *c = &w->config;
    uint32_t fps = c->fps;
    uint32_t den = (c->fps_den != 0u) ? c->fps_den : 1u;
    FM_Bool audio = (c->audio_rate != 0u) ? FM_TRUE : FM_FALSE;
    uint32_t hdrl = 4u + 64u + 124u + (audio ? 100u : 0u);
    uint32_t total = 12u + 8u + hdrl + 12u;
    uint32_t block = (uint32_t)c->audio_channels * 2u;
    uint8_t *p = h;
    uint64_t rate64;

    memset(h, 0, total);
    memcpy(p, "RIFF", 4);
    vw_wr32(p + 4, (file_size >= 8u) ? (uint32_t)(file_size - 8u) : 0u);
    memcpy(p + 8, "AVI ", 4);
    p += 12;
    memcpy(p, "LIST", 4);
    vw_wr32(p + 4, hdrl);
    memcpy(p + 8, "hdrl", 4);
    p += 12;

    memcpy(p, "avih", 4);
    vw_wr32(p + 4, 56u);
    vw_wr32(p + 8, (uint32_t)(((uint64_t)1000000u * den + fps / 2u) / fps));
    rate64 = (w->frames != 0u)
                 ? (((movi_end - w->movi_pos) * fps) / ((uint64_t)w->frames * den))
                 : 0u;
    vw_wr32(p + 12, (rate64 > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)rate64);
    /* AVIF_HASINDEX | AVIF_ISINTERLEAVED */
    vw_wr32(p + 20, (indexed ? 0x10u : 0u) | (audio ? 0x100u : 0u));
    vw_wr32(p + 24, w->frames);
    vw_wr32(p + 32, audio ? 2u : 1u);
    vw_wr32(p + 36, w->largest_frame);
    vw_wr32(p + 40, c->width);
    vw_wr32(p + 44, c->height);
    p += 64;

    memcpy(p, "LIST", 4);
    vw_wr32(p + 4, 116u);
    memcpy(p + 8, "strl", 4);
    p += 12;
    memcpy(p, "strh", 4);
    vw_wr32(p + 4, 56u);
    memcpy(p + 8, "vids", 4);
    memcpy(p + 12, "MJPG", 4);
    vw_wr32(p + 28, den);                        /* dwScale  */
    vw_wr32(p + 32, fps);                        /* dwRate   */
    vw_wr32(p + 40, w->frames);                  /* dwLength */
    vw_wr32(p + 44, w->largest_frame);
    vw_wr32(p + 48, 0xFFFFFFFFu);                /* dwQuality: default */
    vw_wr16(p + 60, c->width);
    vw_wr16(p + 62, c->height);
    p += 64;
    memcpy(p, "strf", 4);
    vw_wr32(p + 4, 40u);
    vw_wr32(p + 8, 40u);
    vw_wr32(p + 12, c->width);
    vw_wr32(p + 16, c->height);
    vw_wr16(p + 20, 1u);
    vw_wr16(p + 22, 24u);
    memcpy(p + 24, "MJPG", 4);
    vw_wr32(p + 28, (uint32_t)c->width * c->height * 3u);
    p += 48;

    if (audio)
    {
        memcpy(p, "LIST", 4);
        vw_wr32(p + 4, 92u);
        memcpy(p + 8, "strl", 4);
        p += 12;
        memcpy(p, "strh", 4);
        vw_wr32(p + 4, 56u);
        memcpy(p + 8, "auds", 4);
        vw_wr32(p + 12, 1u);
        vw_wr32(p + 28, block);                              /* dwScale  */
        vw_wr32(p + 32, c->audio_rate * block);              /* dwRate   */
        vw_wr32(p + 40, (uint32_t)(w->audio_bytes / block)); /* dwLength */
        vw_wr32(p + 44, w->largest_audio);
        vw_wr32(p + 48, 0xFFFFFFFFu);
        vw_wr32(p + 52, block);                              /* sample size */
        p += 64;
        memcpy(p, "strf", 4);
        vw_wr32(p + 4, 16u);
        vw_wr16(p + 8, 1u);                                  /* PCM */
        vw_wr16(p + 10, c->audio_channels);
        vw_wr32(p + 12, c->audio_rate);
        vw_wr32(p + 16, c->audio_rate * block);
        vw_wr16(p + 20, block);
        vw_wr16(p + 22, 16u);
        p += 24;
    }

    memcpy(p, "LIST", 4);
    vw_wr32(p + 4, (movi_end > w->movi_pos)
                       ? (uint32_t)(movi_end - w->movi_pos) : 4u);
    memcpy(p + 8, "movi", 4);
    return total;
}

static void vw_release(FM_VideoFileWriter *w, FM_Bool remove_video,
                       const char *path)
{
    if (File_IsOpen(&w->file))
    {
        (void)File_Close(&w->file);
    }
    if (File_IsOpen(&w->index))
    {
        (void)File_Close(&w->index);
    }
    if (w->index_path[0] != '\0')
    {
        (void)File_Delete(w->index_path);
    }
    if (remove_video && path != NULL)
    {
        (void)File_Delete(path);
    }
    FM_Free(w->buf);
    w->buf = NULL;
    w->ibuf = NULL;
    w->opened = FM_FALSE;
    w->guard = 0u;
}

FM_Result VideoFileWriter_Open(FM_VideoFileWriter *w, const char *path,
                               const FM_VideoSaveConfig *cfg)
{
    uint8_t hdr[VW_HDR_MAX];
    uint32_t hlen;
    size_t plen;
    FM_Result r;

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (w->guard == VW_GUARD && w->opened)
    {
        /* This writer already has a file open.  Opening over it would
         * leave that file without its index and its memory lost. */
        return FM_ERR_BUSY;
    }
    memset(w, 0, sizeof(*w));
    if (path == NULL || cfg == NULL || cfg->width == 0u ||
        cfg->height == 0u || cfg->fps == 0u || cfg->quality > 100u ||
        (cfg->audio_rate != 0u &&
         (cfg->audio_channels < 1u || cfg->audio_channels > 2u)))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    plen = strlen(path);
    if (plen + 5u > sizeof(w->index_path))
    {
        return FM_ERR_TOO_LARGE;
    }
    w->config = *cfg;
    if (w->config.quality == 0u)
    {
        w->config.quality = (uint8_t)FM_VIDEO_ENC_QUALITY;
    }
    if (w->config.audio_rate == 0u)
    {
        w->config.audio_channels = 0u;
    }

    w->buf_cap = (uint32_t)FM_VIDEO_ENC_OUT_BYTES;
    w->buf = (uint8_t *)FM_Alloc((size_t)w->buf_cap + VW_INDEX_BUF);
    if (w->buf == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    w->ibuf = w->buf + w->buf_cap;

    r = File_OpenEx(&w->file, path, FM_OPEN_READ_WRITE | FM_OPEN_CREATE |
                                    FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        FM_Free(w->buf);
        w->buf = NULL;
        return r;
    }
    memcpy(w->index_path, path, plen);
    memcpy(w->index_path + plen, ".idx", 5);
    r = File_OpenEx(&w->index, w->index_path,
                    FM_OPEN_READ_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        w->index_path[0] = '\0';
        vw_release(w, FM_TRUE, path);
        return r;
    }

    hlen = vw_build_header(w, hdr, 0u, 0u, FM_FALSE);
    w->movi_pos = (uint64_t)hlen - 4u;
    vw_put(w, hdr, hlen);
    vw_flush(w);
    if (w->error != FM_OK)
    {
        r = w->error;
        vw_release(w, FM_TRUE, path);
        return r;
    }
    w->synced_pos = vw_pos(w);
    w->opened = FM_TRUE;
    w->guard = VW_GUARD;
    FM_EVENT_LOG("open %s %ux%u %lu/%lu fps q%u audio %lu Hz %uch",
           path, (unsigned)cfg->width, (unsigned)cfg->height,
           (unsigned long)cfg->fps, (unsigned long)cfg->fps_den,
           (unsigned)w->config.quality, (unsigned long)cfg->audio_rate,
           (unsigned)w->config.audio_channels);
    return FM_OK;
}

/* Room for one more chunk of `len` bytes and its index entry? */
static FM_Result vw_room(const FM_VideoFileWriter *w, uint64_t len)
{
    uint64_t end = vw_pos(w) + 8u + len + 1u + 8u +
                   16u * ((uint64_t)w->index_entries + 1u);

    return (end > (uint64_t)FM_VIDEO_ENC_MAX_BYTES) ? FM_ERR_TOO_LARGE
                                                    : FM_OK;
}

static FM_Result vw_ready(const FM_VideoFileWriter *w)
{
    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!w->opened || w->guard != VW_GUARD)
    {
        return FM_ERR_NOT_OPEN;
    }
    return w->error;
}

/* Called after every chunk.  When the file has grown by
 * FM_VIDEO_ENC_SYNC_BYTES since the last time: everything buffered goes
 * to the card, the header gets the counts so far and the file is synced.
 * What is on the card is then a complete AVI without an index. */
static void vw_checkpoint(FM_VideoFileWriter *w)
{
#if FM_VIDEO_ENC_SYNC_BYTES > 0u
    uint8_t hdr[VW_HDR_MAX];
    uint32_t hlen;
    uint64_t end;

    if (w->error != FM_OK ||
        vw_pos(w) - w->synced_pos < (uint64_t)FM_VIDEO_ENC_SYNC_BYTES)
    {
        return;
    }
    vw_flush(w);
    end = vw_pos(w);
    hlen = vw_build_header(w, hdr, end, end, FM_FALSE);
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, 0, FM_SEEK_SET);
    }
    if (w->error == FM_OK)
    {
        w->error = vw_file_write(&w->file, hdr, hlen);
    }
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, (int64_t)end, FM_SEEK_SET);
    }
    if (w->error == FM_OK)
    {
        w->error = File_Flush(&w->file);
    }
    w->synced_pos = end;
#else
    (void)w;
#endif
}

FM_Result VideoFileWriter_WriteJpeg(FM_VideoFileWriter *w, const void *jpeg,
                                    size_t size)
{
    const uint8_t *j = (const uint8_t *)jpeg;
    uint64_t at;
    FM_Result r = vw_ready(w);

    if (r != FM_OK)
    {
        return r;
    }
    if (jpeg == NULL || size < 4u || size > 0x7FFFFFFFu)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (j[0] != 0xFFu || j[1] != 0xD8u)
    {
        return FM_ERR_INVALID_FORMAT;            /* not a JPEG picture */
    }
    r = vw_room(w, size);
    if (r != FM_OK)
    {
        return r;
    }
    at = vw_chunk_begin(w, "00dc");
    vw_put(w, jpeg, (uint32_t)size);
    (void)vw_chunk_end(w, at, "00dc", &w->largest_frame);
    if (w->error == FM_OK)
    {
        w->frames++;
        w->last_at = at;
        w->last_size = (uint32_t)size;
    }
    vw_checkpoint(w);
    return w->error;
}

static FM_Result vw_write_frame(FM_VideoFileWriter *w, const void *pixels,
                                FM_PixelFormat format, uint8_t swap16)
{
    FM_Image img;
    FM_JpegEncodeOptions opt;
    uint64_t at;
    uint32_t size;
    size_t bpp = Image_BytesPerPixel(format);
    FM_Result r = vw_ready(w);

    if (r != FM_OK)
    {
        return r;
    }
    if (pixels == NULL || bpp == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* a frame never needs more than its raw size and a header */
    r = vw_room(w, (uint64_t)w->config.width * w->config.height * 3u / 2u);
    if (r != FM_OK)
    {
        return r;
    }
    memset(&img, 0, sizeof(img));
    img.width = w->config.width;
    img.height = w->config.height;
    img.format = format;
    img.data = (uint8_t *)(uintptr_t)pixels;
    img.data_capacity = (size_t)img.width * img.height * bpp;
    img.data_size = img.data_capacity;
    opt.quality = w->config.quality;
    opt.subsample = (uint8_t)((w->config.chroma_full != 0u) ? 0u : 1u);
    opt.swap16 = swap16;

    at = vw_chunk_begin(w, "00dc");
    r = FM_JPEG_EncodeSink(&img, &opt, vw_jpeg_sink, w);
    if (r != FM_OK && w->error == FM_OK)
    {
        /* the encoder refused the picture (or JPEG is compiled out):
         * the chunk header is in the file, so the writer is finished */
        w->error = r;
    }
    size = vw_chunk_end(w, at, "00dc", &w->largest_frame);
    if (w->error == FM_OK)
    {
        w->frames++;
        w->last_at = at;
        w->last_size = size;
    }
    vw_checkpoint(w);
    return w->error;
}

FM_Result VideoFileWriter_WriteFrame(FM_VideoFileWriter *w,
                                     const void *pixels,
                                     FM_PixelFormat format)
{
    return vw_write_frame(w, pixels, format, 0u);
}

FM_Result VideoFileWriter_WriteFrameSwap16(FM_VideoFileWriter *w,
                                           const void *pixels,
                                           FM_PixelFormat format)
{
    return vw_write_frame(w, pixels, format, 1u);
}

/* The previous picture once more.  Its bytes are read back from the file
 * (the handle is read / write) straight into the output buffer, so no
 * memory is needed for it and nothing is encoded. */
FM_Result VideoFileWriter_RepeatFrame(FM_VideoFileWriter *w, uint32_t count)
{
    FM_Result r = vw_ready(w);

    if (r != FM_OK)
    {
        return r;
    }
    if (count == 0u)
    {
        return FM_OK;
    }
    if (w->last_size == 0u)
    {
        return FM_ERR_NOT_READY;                 /* no picture yet */
    }
    while (count != 0u)
    {
        uint64_t src = w->last_at + 8u;
        uint32_t left = w->last_size;
        uint64_t at;

        r = vw_room(w, left);
        if (r != FM_OK)
        {
            return r;
        }
        vw_flush(w);                 /* the picture is on the card now */
        at = vw_chunk_begin(w, "00dc");
        while (left != 0u && w->error == FM_OK)
        {
            uint32_t room;
            uint32_t n;
            uint32_t got = 0;

            if (w->buf_len == w->buf_cap)
            {
                vw_flush(w);
                if (w->error != FM_OK)
                {
                    break;
                }
            }
            room = w->buf_cap - w->buf_len;
            n = (left < room) ? left : room;
            w->error = File_Seek(&w->file, (int64_t)src, FM_SEEK_SET);
            if (w->error == FM_OK)
            {
                FM_Result rr = File_Read(&w->file, w->buf + w->buf_len, n,
                                         &got);

                /* the picture may end exactly where the file ends */
                w->error = (got == n) ? FM_OK
                                      : ((rr != FM_OK) ? rr : FM_ERR_IO);
            }
            if (w->error == FM_OK)
            {
                /* back to the end: that is where the buffer goes */
                w->error = File_Seek(&w->file, (int64_t)w->buf_pos,
                                     FM_SEEK_SET);
            }
            if (w->error == FM_OK)
            {
                w->buf_len += n;
                src += n;
                left -= n;
            }
        }
        (void)vw_chunk_end(w, at, "00dc", &w->largest_frame);
        if (w->error != FM_OK)
        {
            return w->error;
        }
        w->frames++;
        w->last_at = at;
        count--;
    }
    vw_checkpoint(w);
    return w->error;
}

FM_Result VideoFileWriter_WriteAudio(FM_VideoFileWriter *w,
                                     const int16_t *pcm, size_t frames)
{
    uint64_t at;
    uint64_t bytes;
    FM_Result r = vw_ready(w);

    if (r != FM_OK)
    {
        return r;
    }
    if (w->config.audio_rate == 0u)
    {
        return FM_ERR_UNSUPPORTED;               /* opened without sound */
    }
    if (pcm == NULL || frames == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bytes = (uint64_t)frames * w->config.audio_channels * 2u;
    if (bytes > 0x7FFFFFFFu)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = vw_room(w, bytes);
    if (r != FM_OK)
    {
        return r;
    }
    at = vw_chunk_begin(w, "01wb");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    {
        size_t i;
        size_t n = frames * w->config.audio_channels;

        for (i = 0; i < n; i++)
        {
            uint8_t b[2];

            vw_wr16(b, (uint16_t)pcm[i]);
            vw_put(w, b, 2u);
        }
    }
#else
    vw_put(w, pcm, (uint32_t)bytes);
#endif
    (void)vw_chunk_end(w, at, "01wb", &w->largest_audio);
    if (w->error == FM_OK)
    {
        w->audio_bytes += bytes;
    }
    vw_checkpoint(w);
    return w->error;
}

/* Index, counts and lengths: what turns a row of chunks into a finished
 * file.  The writer stands right after the last chunk and everything
 * before that is on the card. */
static FM_Result vw_finish(FM_VideoFileWriter *w)
{
    uint8_t hdr[VW_HDR_MAX];
    uint64_t movi_end = vw_pos(w);
    uint64_t end;
    uint32_t hlen;
    uint32_t left;
    FM_Bool indexed = FM_TRUE;
    FM_Result r = FM_OK;

    vw_index_flush(w);
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->index, 0, FM_SEEK_SET);
    }
    {
        uint8_t h[8];

        memcpy(h, "idx1", 4);
        vw_wr32(h + 4, w->index_entries * 16u);
        vw_put(w, h, 8u);
    }
    left = w->index_entries * 16u;
    while (left != 0u && w->error == FM_OK)
    {
        uint32_t n = (left < VW_INDEX_BUF) ? left : VW_INDEX_BUF;
        uint32_t got = 0;

        w->error = File_Read(&w->index, w->ibuf, n, &got);
        if (w->error == FM_OK && got != n)
        {
            w->error = FM_ERR_IO;
        }
        if (w->error == FM_OK)
        {
            vw_put(w, w->ibuf, n);
            left -= n;
        }
    }
    vw_flush(w);
    end = vw_pos(w);
    if (w->error != FM_OK)
    {
        /* The index did not fit (card full) or could not be read back.
         * The file is completed without one: the header says so and
         * counts only the chunks, which players then read in order. */
        r = w->error;
        w->error = FM_OK;
        indexed = FM_FALSE;
        end = movi_end;
        (void)File_Truncate(&w->file, movi_end);         /* best effort */
    }

    hlen = vw_build_header(w, hdr, end, movi_end, indexed);
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, 0, FM_SEEK_SET);
    }
    if (w->error == FM_OK)
    {
        w->error = vw_file_write(&w->file, hdr, hlen);
    }
    if (w->error == FM_OK)
    {
        w->error = File_Flush(&w->file);
    }
    return (r != FM_OK) ? r : w->error;
}

FM_Result VideoFileWriter_Close(FM_VideoFileWriter *w)
{
    FM_Result r;
    FM_Result c;

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!w->opened || w->guard != VW_GUARD)
    {
        return FM_ERR_NOT_OPEN;
    }

    r = w->error;
    if (r == FM_OK)
    {
        vw_flush(w);                 /* the last chunks */
        r = w->error;
    }
    if (r == FM_OK)
    {
        r = vw_finish(w);
        c = File_Close(&w->file);
        if (r == FM_OK)
        {
            r = c;
        }
        FM_EVENT_LOG("close: %lu frames, %lu bytes, r=%d",
               (unsigned long)w->frames, (unsigned long)vw_pos(w), (int)r);
        vw_release(w, FM_FALSE, NULL);
        return r;
    }

    /* A write failed (card full, I/O error): the counts and the index
     * kept in memory describe chunks that may not be on the card.  The
     * file is completed from what is really there, exactly as for a
     * recording that was never closed: every whole chunk is kept, the
     * one that was cut is dropped.  The error stays the answer. */
    {
        char *path = w->index_path;
        size_t n = strlen(path);

        (void)File_Close(&w->file);
        (void)File_Close(&w->index);
        FM_Free(w->buf);
        w->buf = NULL;
        w->ibuf = NULL;
        w->opened = FM_FALSE;
        w->guard = 0u;
        FM_EVENT_LOG("close after an error (r=%d): completing what is on "
                     "the card", (int)r);
        if (n > 4u)
        {
            path[n - 4u] = '\0';     /* "<name>.idx" -> "<name>" */
            (void)VideoFile_Repair(path, NULL);
        }
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* VideoFile_Repair                                                    */
/* ------------------------------------------------------------------ */
static uint32_t vw_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static FM_Result vw_read_at(FM_File *f, uint64_t at, uint8_t *dst,
                            uint32_t len)
{
    uint32_t got = 0;
    FM_Result r = File_Seek(f, (int64_t)at, FM_SEEK_SET);

    if (r == FM_OK)
    {
        r = File_Read(f, dst, len, &got);
    }
    if (r == FM_OK && got != len)
    {
        r = FM_ERR_EOF;
    }
    return r;
}

/* The header of a file this writer made -> the configuration it was
 * opened with.  FM_ERR_INVALID_FORMAT for anything else. */
static FM_Result vw_parse_header(const uint8_t *h, uint32_t have,
                                 FM_VideoSaveConfig *c, uint32_t *hlen)
{
    uint32_t streams;
    uint32_t at;

    memset(c, 0, sizeof(*c));
    if (have < 224u || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "AVI LIST", 8) != 0 ||
        memcmp(h + 20, "hdrlavih", 8) != 0 ||
        memcmp(h + 100, "strh", 4) != 0 ||
        memcmp(h + 108, "vidsMJPG", 8) != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    streams = vw_rd32(h + 24 + 32);
    c->width = (uint16_t)vw_rd32(h + 24 + 40);
    c->height = (uint16_t)vw_rd32(h + 24 + 44);
    c->fps_den = vw_rd32(h + 100 + 28);
    c->fps = vw_rd32(h + 100 + 32);
    at = 212u;
    if (streams == 2u)
    {
        if (have < 324u || memcmp(h + 224, "strh", 4) != 0 ||
            memcmp(h + 232, "auds", 4) != 0)
        {
            return FM_ERR_INVALID_FORMAT;
        }
        c->audio_channels = (uint16_t)(h[288 + 10] | (h[288 + 11] << 8));
        c->audio_rate = vw_rd32(h + 288 + 12);
        at = 312u;
    }
    else if (streams != 1u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (memcmp(h + at, "LIST", 4) != 0 || memcmp(h + at + 8u, "movi", 4) != 0 ||
        c->width == 0u || c->height == 0u || c->fps == 0u ||
        (streams == 2u && (c->audio_rate == 0u || c->audio_channels < 1u ||
                           c->audio_channels > 2u)))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    *hlen = at + 12u;
    return FM_OK;
}

FM_Result VideoFile_Repair(const char *path, uint32_t *frames)
{
    FM_VideoFileWriter *w;
    uint8_t hdr[VW_HDR_MAX];
    uint64_t size = 0;
    uint64_t pos;
    uint32_t hlen = 0;
    uint32_t got = 0;
    size_t plen;
    FM_Bool there = FM_FALSE;
    FM_Result r;

    if (frames != NULL)
    {
        *frames = 0u;
    }
    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    plen = strlen(path);
    if (plen + 5u > sizeof(w->index_path))
    {
        return FM_ERR_TOO_LARGE;
    }
    w = (FM_VideoFileWriter *)FM_Calloc(1u, sizeof(*w));
    if (w == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    memcpy(w->index_path, path, plen);
    memcpy(w->index_path + plen, ".idx", 5);

    /* The side index is there only while a recording is open: no such
     * file = the video was closed properly (or never existed). */
    r = File_Exists(w->index_path, &there);
    if (r == FM_OK && !there)
    {
        r = FM_ERR_NOT_FOUND;
    }
    if (r == FM_OK)
    {
        r = File_Exists(path, &there);
        if (r == FM_OK && !there)
        {
            (void)File_Delete(w->index_path);    /* an index of nothing */
            r = FM_ERR_NOT_FOUND;
        }
    }
    if (r != FM_OK)
    {
        FM_Free(w);
        return r;
    }

    w->buf_cap = (uint32_t)FM_VIDEO_ENC_OUT_BYTES;
    w->buf = (uint8_t *)FM_Alloc((size_t)w->buf_cap + VW_INDEX_BUF);
    if (w->buf == NULL)
    {
        FM_Free(w);
        return FM_ERR_NO_MEMORY;
    }
    w->ibuf = w->buf + w->buf_cap;

    r = File_OpenEx(&w->file, path, FM_OPEN_READ_WRITE);
    if (r == FM_OK)
    {
        r = File_Size(&w->file, &size);
    }
    if (r == FM_OK)
    {
        r = File_Read(&w->file, hdr, sizeof(hdr), &got);
    }
    if (r == FM_OK)
    {
        r = vw_parse_header(hdr, got, &w->config, &hlen);
    }
    if (r != FM_OK)
    {
        /* Not a file of this writer, or it cannot be read: both files
         * are left exactly as they are. */
        if (File_IsOpen(&w->file))
        {
            (void)File_Close(&w->file);
        }
        FM_Free(w->buf);
        FM_Free(w);
        return r;
    }
    r = File_OpenEx(&w->index, w->index_path,
                    FM_OPEN_READ_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        (void)File_Close(&w->file);
        FM_Free(w->buf);
        FM_Free(w);
        return r;
    }
    w->movi_pos = (uint64_t)hlen - 4u;

    /* Walk the chunks that are whole; the index is made from them. */
    pos = hlen;
    while (pos + 8u <= size && w->error == FM_OK)
    {
        uint8_t ch[8];
        uint32_t len;
        FM_Bool picture;

        if (vw_read_at(&w->file, pos, ch, 8u) != FM_OK)
        {
            break;
        }
        len = vw_rd32(ch + 4);
        picture = (memcmp(ch, "00dc", 4) == 0) ? FM_TRUE : FM_FALSE;
        if ((!picture && (memcmp(ch, "01wb", 4) != 0 ||
                          w->config.audio_rate == 0u)) ||
            len == 0u || len > 0x7FFFFFFFu ||
            pos + 8u + (uint64_t)len + (len & 1u) > size)
        {
            break;                    /* the chunk that was cut */
        }
        if (picture)
        {
            w->frames++;
            w->last_at = pos;
            w->last_size = len;
            if (len > w->largest_frame)
            {
                w->largest_frame = len;
            }
        }
        else
        {
            w->audio_bytes += len;
            if (len > w->largest_audio)
            {
                w->largest_audio = len;
            }
        }
        vw_index_add(w, pos, picture ? "00dc" : "01wb", len);
        pos += 8u + (uint64_t)len + (len & 1u);
    }
    /* Continue as a writer that stands right after the last whole chunk
     * and finish it: index, counts and lengths are written as by Close
     * (without the index when even that does not fit on the card). */
    if (pos < size)
    {
        (void)File_Truncate(&w->file, pos);      /* best effort */
    }
    if (w->error == FM_OK)
    {
        w->error = File_Seek(&w->file, (int64_t)pos, FM_SEEK_SET);
    }
    w->buf_pos = pos;
    if (frames != NULL)
    {
        *frames = w->frames;
    }
    FM_EVENT_LOG("repair %s: %lu frames, %lu bytes kept of %lu", path,
           (unsigned long)w->frames, (unsigned long)pos,
           (unsigned long)size);
    r = vw_finish(w);
    {
        FM_Result c = File_Close(&w->file);

        if (r == FM_OK)
        {
            r = c;
        }
    }
    vw_release(w, FM_FALSE, NULL);   /* closes and removes "<path>.idx" */
    FM_Free(w);
    return r;
}

/* ------------------------------------------------------------------ */
/* Video_Convert                                                       */
/* ------------------------------------------------------------------ */

/* The same file, also when only the letter case differs (FAT does not
 * tell "A.AVI" from "a.avi"): writing it would destroy the source. */
static FM_Bool vw_same_file(const char *a, const char *b)
{
    char pa[FM_PATH_MAX];
    char pb[FM_PATH_MAX];
    size_t i;

    if (strlen(a) >= sizeof(pa) || strlen(b) >= sizeof(pb))
    {
        return FM_FALSE;
    }
    (void)strcpy(pa, a);
    (void)strcpy(pb, b);
    (void)Path_Normalize(pa, sizeof(pa));
    (void)Path_Normalize(pb, sizeof(pb));
    for (i = 0u; pa[i] != '\0' && pb[i] != '\0'; i++)
    {
        char x = pa[i];
        char y = pb[i];

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
        {
            return FM_FALSE;
        }
    }
    return (pa[i] == pb[i]) ? FM_TRUE : FM_FALSE;
}

FM_Result Video_Convert(const char *src, const char *dst,
                        const FM_VideoConvertConfig *cfg,
                        uint32_t *frames_out)
{
    static const FM_VideoConvertConfig none;
    FM_Video v;
    FM_VideoInfo info;
    FM_AudioTrackInfo ai;
    FM_VideoSaveConfig sc;
    FM_VideoFileWriter *w = NULL;
    uint8_t *rgb = NULL;
    uint8_t *abuf = NULL;
    size_t rgb_cap;
    uint64_t audio_frames = 0u;
    uint32_t frame = 0u;
    uint16_t out_w;
    uint16_t out_h;
    FM_Bool audio = FM_FALSE;
    FM_Bool audio_done = FM_FALSE;
    FM_Bool writer_open = FM_FALSE;
    FM_Result r;

    if (frames_out != NULL)
    {
        *frames_out = 0u;
    }
    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (cfg == NULL)
    {
        cfg = &none;
    }
    if ((cfg->width == 0u) != (cfg->height == 0u))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (vw_same_file(src, dst))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    r = Video_Open(src, &v);
    if (r != FM_OK)
    {
        return r;
    }
    (void)Video_GetInfo(&v, &info);
    memset(&ai, 0, sizeof(ai));
    if (!cfg->no_audio && Video_GetAudioInfo(&v, &ai) == FM_OK &&
        ai.codec != FM_VIDEO_AUDIO_NONE && ai.decodable &&
        ai.sample_rate != 0u && (ai.channels == 1u || ai.channels == 2u))
    {
        audio = FM_TRUE;
    }

    out_w = (cfg->width != 0u) ? cfg->width : info.width;
    out_h = (cfg->height != 0u) ? cfg->height : info.height;
    if (info.width == 0u || info.height == 0u || info.fps == 0u)
    {
        r = FM_ERR_UNSUPPORTED;
        goto done;
    }
    rgb_cap = (size_t)out_w * out_h * 3u;
    if (rgb_cap < (size_t)info.width * info.height * 3u && cfg->width == 0u)
    {
        rgb_cap = (size_t)info.width * info.height * 3u;
    }
    rgb = (uint8_t *)FM_Alloc(rgb_cap);
    w = (FM_VideoFileWriter *)FM_Alloc(sizeof(*w));
    if (audio)
    {
        abuf = (uint8_t *)FM_Alloc((size_t)FM_VIDEO_CONVERT_AUDIO_BYTES);
    }
    if (rgb == NULL || w == NULL || (audio && abuf == NULL))
    {
        r = FM_ERR_NO_MEMORY;
        goto done;
    }

    for (;;)
    {
        if (cfg->width != 0u)
        {
            r = Video_DecodeFrameScaled(&v, rgb, rgb_cap, out_w, out_h,
                                        FM_SCALE_BILINEAR);
        }
        else
        {
            r = Video_DecodeFrame(&v, rgb, rgb_cap);
        }
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            break;
        }
        if (r == FM_ERR_NOT_READY)
        {
            continue;                    /* a packet without a picture */
        }
        if (r != FM_OK)
        {
            break;
        }

        if (!writer_open)
        {
            /* Some decoders (H.264) only know the real picture size
             * after the first frame: open the file now. */
            if (cfg->width == 0u)
            {
                (void)Video_GetInfo(&v, &info);
                if ((size_t)info.width * info.height * 3u > rgb_cap)
                {
                    r = FM_ERR_TOO_LARGE;
                    break;
                }
                out_w = info.width;
                out_h = info.height;
            }
            memset(&sc, 0, sizeof(sc));
            sc.width = out_w;
            sc.height = out_h;
            sc.fps = info.fps;
            sc.fps_den = 1u;
            sc.quality = cfg->quality;
            sc.chroma_full = cfg->chroma_full;
            if (audio)
            {
                sc.audio_rate = ai.sample_rate;
                sc.audio_channels = ai.channels;
            }
            r = VideoFileWriter_Open(w, dst, &sc);
            if (r != FM_OK)
            {
                break;
            }
            writer_open = FM_TRUE;
        }

        if (cfg->process != NULL)
        {
            cfg->process(cfg->process_ctx, rgb, out_w, out_h, frame);
        }
        r = VideoFileWriter_WriteFrame(w, rgb, FM_PIXEL_RGB888);
        if (r != FM_OK)
        {
            break;
        }
        frame++;

        /* the sound that belongs to the pictures written so far */
        while (audio && !audio_done &&
               audio_frames < ((uint64_t)frame * ai.sample_rate) / info.fps)
        {
            size_t got = 0;

            r = Video_ReadAudioFrame(&v, abuf,
                                     (size_t)FM_VIDEO_CONVERT_AUDIO_BYTES,
                                     &got);
            if (r == FM_ERR_CORRUPTED)
            {
                continue;                /* passed over; next packet */
            }
            if (r == FM_ERR_EOF || r == FM_ERR_UNSUPPORTED)
            {
                audio_done = FM_TRUE;
                break;
            }
            if (r != FM_OK)
            {
                goto done;
            }
            got /= (size_t)ai.channels * 2u;
            if (got == 0u)
            {
                continue;
            }
            if (cfg->audio_process != NULL)
            {
                cfg->audio_process(cfg->audio_ctx, (int16_t *)(void *)abuf,
                                   got * ai.channels, ai.sample_rate,
                                   ai.channels);
            }
            r = VideoFileWriter_WriteAudio(w, (const int16_t *)(void *)abuf,
                                           got);
            if (r != FM_OK)
            {
                goto done;
            }
            audio_frames += got;
        }
        r = FM_OK;

        if (cfg->progress != NULL &&
            !cfg->progress(cfg->progress_ctx, frame, info.frame_count))
        {
            r = FM_ERR_CANCELLED;
            break;
        }
    }
    if (r == FM_OK && !writer_open)
    {
        r = FM_ERR_INVALID_FORMAT;       /* not one picture in the file */
    }

done:
    if (writer_open)
    {
        FM_Result c = VideoFileWriter_Close(w);

        if (r == FM_OK)
        {
            r = c;
        }
        if (r != FM_OK)
        {
            (void)File_Delete(dst);
        }
    }
    (void)Video_Close(&v);
    FM_Free(abuf);
    FM_Free(w);
    FM_Free(rgb);
    if (r == FM_OK && frames_out != NULL)
    {
        *frames_out = frame;
    }
    return r;
}

#else /* FM_USE_VIDEO == 0 */

FM_Result VideoFileWriter_Open(FM_VideoFileWriter *w, const char *path,
                               const FM_VideoSaveConfig *cfg)
{
    (void)w; (void)path; (void)cfg;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_WriteFrame(FM_VideoFileWriter *w,
                                     const void *pixels,
                                     FM_PixelFormat format)
{
    (void)w; (void)pixels; (void)format;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_WriteFrameSwap16(FM_VideoFileWriter *w,
                                           const void *pixels,
                                           FM_PixelFormat format)
{
    (void)w; (void)pixels; (void)format;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_WriteJpeg(FM_VideoFileWriter *w, const void *jpeg,
                                    size_t size)
{
    (void)w; (void)jpeg; (void)size;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_RepeatFrame(FM_VideoFileWriter *w, uint32_t count)
{
    (void)w; (void)count;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_WriteAudio(FM_VideoFileWriter *w,
                                     const int16_t *pcm, size_t frames)
{
    (void)w; (void)pcm; (void)frames;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFileWriter_Close(FM_VideoFileWriter *w)
{
    (void)w;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoFile_Repair(const char *path, uint32_t *frames)
{
    (void)path;
    if (frames != NULL)
    {
        *frames = 0u;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_Convert(const char *src, const char *dst,
                        const FM_VideoConvertConfig *cfg,
                        uint32_t *frames_out)
{
    (void)src; (void)dst; (void)cfg; (void)frames_out;
    return FM_ERR_UNSUPPORTED;
}

#endif /* FM_USE_VIDEO */
