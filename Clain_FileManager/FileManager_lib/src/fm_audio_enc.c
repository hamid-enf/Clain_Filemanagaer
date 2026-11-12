/*
 * FileManager - audio encoders and file-to-file conversion
 *
 * The saving side of fm_audio.c.  Everything here takes signed 16-bit
 * interleaved PCM and writes a finished file:
 *
 *   WAV   16-bit PCM            (always built in)
 *   WAV   IMA ADPCM, 4:1        (always built in; decoded by Audio_Open)
 *   FLAC  lossless, about 2:1   (built in when FM_USE_FLAC is 1)
 *   MP3 / Ogg Vorbis / Opus     a registered FM_AudioEncoderOps plugin
 *
 * Speed, the two things that matter on a microcontroller:
 *
 *   1. The card.  Encoded bytes are collected in one buffer aligned for
 *      DMA and written only in whole buffers of FM_AUDIO_ENC_OUT_BYTES
 *      (a multiple of the sector size).  After the first buffer every
 *      write starts on a sector boundary, so the filesystem hands the
 *      block straight to the driver as one multi-sector command: no
 *      copying through its sector window, no single-sector writes.
 *   2. The coder.  FLAC uses the fixed predictors (order 0..4) with
 *      partitioned Rice coding and the four stereo layouts, all chosen
 *      from one statistics pass of adds, subtracts and shifts: no
 *      multiply, no divide, no floating point.  ADPCM is the usual
 *      three compares per sample.  Both run many times faster than real
 *      time on a Cortex-M7, and on a Cortex-M4 too.
 *
 * Memory comes from FM_Alloc (see fm_alloc.h): the output buffer for
 * every format, plus one block of samples for FLAC and ADPCM.  FLAC
 * stereo with the default block of 4096 frames takes about 66 KB.
 */

#define FM_DEBUG_TAG "audioenc"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_internal.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_AUDIO

/* Bytes written to the file in one go.  A multiple of 512.  When that
 * much memory is not there the writer halves it, down to one sector. */
#ifndef FM_AUDIO_ENC_OUT_BYTES
#define FM_AUDIO_ENC_OUT_BYTES 16384u
#endif

/* Sample frames in one FLAC block: 256 .. 4608 (the streamable subset
 * for rates up to 48 kHz).  Memory is about 16 bytes per frame for
 * stereo. */
#ifndef FM_FLAC_ENC_BLOCK
#define FM_FLAC_ENC_BLOCK 4096u
#endif

#if (FM_AUDIO_ENC_OUT_BYTES < 512u) || ((FM_AUDIO_ENC_OUT_BYTES % 512u) != 0u)
#error "FM_AUDIO_ENC_OUT_BYTES must be a multiple of 512"
#endif
#if (FM_FLAC_ENC_BLOCK < 256u) || (FM_FLAC_ENC_BLOCK > 4608u)
#error "FM_FLAC_ENC_BLOCK must be 256 .. 4608"
#endif

#define ENC_ALIGN 32u

static void enc_wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void enc_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static FM_Result enc_file_write(FM_File *f, const uint8_t *p, uint32_t size)
{
    uint32_t done = 0u;

    while (done < size)
    {
        uint32_t wr = 0u;
        FM_Result r = File_Write(f, p + done, size - done, &wr);

        if (r != FM_OK)
        {
            return r;
        }
        if (wr == 0u)
        {
            return FM_ERR_NO_SPACE;
        }
        done += wr;
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Output buffer: aligned, written in whole buffers                    */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t *raw;      /* the allocation                         */
    uint8_t *buf;      /* ENC_ALIGN-aligned start inside `raw`   */
    uint32_t cap;      /* a multiple of 512                      */
    uint32_t len;
} EncOut;

static FM_Result encout_init(EncOut *o)
{
    uint32_t cap = (uint32_t)FM_AUDIO_ENC_OUT_BYTES;

    memset(o, 0, sizeof(*o));
    for (;;)
    {
        o->raw = (uint8_t *)FM_Alloc((size_t)cap + ENC_ALIGN);
        if (o->raw != NULL)
        {
            break;
        }
        if (cap <= 512u)
        {
            return FM_ERR_NO_MEMORY;
        }
        cap /= 2u;
        cap -= cap % 512u;
        if (cap < 512u)
        {
            cap = 512u;
        }
    }
    o->buf = o->raw +
             ((ENC_ALIGN - ((uintptr_t)o->raw & (ENC_ALIGN - 1u))) &
              (ENC_ALIGN - 1u));
    o->cap = cap;
    return FM_OK;
}

static void encout_free(EncOut *o)
{
    if (o->raw != NULL)
    {
        FM_Free(o->raw);
    }
    memset(o, 0, sizeof(*o));
}

static FM_Result encout_put(EncOut *o, FM_File *f, const void *data,
                            size_t size)
{
    const uint8_t *p = (const uint8_t *)data;

    while (size > 0u)
    {
        size_t room = (size_t)(o->cap - o->len);
        size_t n = (size < room) ? size : room;

        memcpy(o->buf + o->len, p, n);
        o->len += (uint32_t)n;
        p += n;
        size -= n;
        if (o->len == o->cap)
        {
            FM_Result r = enc_file_write(f, o->buf, o->cap);

            if (r != FM_OK)
            {
                return r;
            }
            o->len = 0u;
        }
    }
    return FM_OK;
}

static FM_Result encout_flush(EncOut *o, FM_File *f)
{
    FM_Result r = FM_OK;

    if (o->len > 0u)
    {
        r = enc_file_write(f, o->buf, o->len);
        o->len = 0u;
    }
    return r;
}

/* Rewrite `size` bytes at `offset` (a header field, once the length is
 * known).  The buffer must have been flushed. */
static FM_Result enc_patch(FM_File *f, uint32_t offset, const uint8_t *p,
                           uint32_t size)
{
    FM_Result r = File_Seek(f, (int64_t)offset, FM_SEEK_SET);

    if (r != FM_OK)
    {
        return r;
    }
    return enc_file_write(f, p, size);
}

/* ------------------------------------------------------------------ */
/* WAV, 16-bit PCM                                                     */
/* ------------------------------------------------------------------ */
#define WAV_PCM_HEADER 44u
#define WAV_MAX_DATA   (0xFFFFFFFFu - 64u)

typedef struct
{
    EncOut    out;
    uint32_t  rate;
    uint32_t  ch;
    uint64_t  data_bytes;
    FM_Bool   header_done;
    FM_Result err;
} PcmEnc;

static FM_Result pcm_header(PcmEnc *e, FM_File *f)
{
    uint8_t h[WAV_PCM_HEADER];

    memcpy(h, "RIFF", 4);
    enc_wr32(h + 4, 36u);
    memcpy(h + 8, "WAVEfmt ", 8);
    enc_wr32(h + 16, 16u);
    enc_wr16(h + 20, 1u);
    enc_wr16(h + 22, e->ch);
    enc_wr32(h + 24, e->rate);
    enc_wr32(h + 28, e->rate * e->ch * 2u);
    enc_wr16(h + 32, e->ch * 2u);
    enc_wr16(h + 34, 16u);
    memcpy(h + 36, "data", 4);
    enc_wr32(h + 40, 0u);
    e->header_done = FM_TRUE;
    return encout_put(&e->out, f, h, sizeof(h));
}

static FM_Result pcm_init(void **ctx, const FM_AudioEncodeConfig *config)
{
    PcmEnc *e;
    FM_Result r;

    if (config->sample_rate > 0xFFFFFFFFu / (config->channels * 2u))
    {
        return FM_ERR_TOO_LARGE;
    }
    e = (PcmEnc *)FM_Calloc(1u, sizeof(*e));
    if (e == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = encout_init(&e->out);
    if (r != FM_OK)
    {
        FM_Free(e);
        return r;
    }
    e->rate = config->sample_rate;
    e->ch = config->channels;
    *ctx = e;
    return FM_OK;
}

static FM_Result pcm_write(void *ctx, FM_File *f, const int16_t *pcm,
                           size_t frame_count, size_t *frames_consumed)
{
    PcmEnc *e = (PcmEnc *)ctx;
    uint64_t bytes = (uint64_t)frame_count * e->ch * 2u;

    if (e->err != FM_OK)
    {
        return e->err;
    }
    if (bytes > (uint64_t)WAV_MAX_DATA - e->data_bytes)
    {
        return FM_ERR_TOO_LARGE;      /* a RIFF size field is 32 bits */
    }
    if (!e->header_done)
    {
        e->err = pcm_header(e, f);
    }
    if (e->err == FM_OK)
    {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        /* The samples already are the file bytes. */
        e->err = encout_put(&e->out, f, pcm, (size_t)bytes);
#else
        size_t left = frame_count * e->ch;

        while (left > 0u && e->err == FM_OK)
        {
            uint8_t tmp[256];
            size_t n = (left < sizeof(tmp) / 2u) ? left : sizeof(tmp) / 2u;
            size_t i;

            for (i = 0u; i < n; i++)
            {
                enc_wr16(tmp + 2u * i, (uint16_t)pcm[i]);
            }
            e->err = encout_put(&e->out, f, tmp, n * 2u);
            pcm += n;
            left -= n;
        }
#endif
    }
    if (e->err != FM_OK)
    {
        return e->err;
    }
    e->data_bytes += bytes;
    if (frames_consumed != NULL)
    {
        *frames_consumed = frame_count;
    }
    return FM_OK;
}

static FM_Result pcm_finish(void *ctx, FM_File *f)
{
    PcmEnc *e = (PcmEnc *)ctx;
    uint8_t v[4];
    FM_Result r = e->err;

    if (r == FM_OK && !e->header_done)
    {
        r = pcm_header(e, f);
    }
    if (r == FM_OK)
    {
        r = encout_flush(&e->out, f);
    }
    if (r == FM_OK)
    {
        enc_wr32(v, 36u + (uint32_t)e->data_bytes);
        r = enc_patch(f, 4u, v, 4u);
    }
    if (r == FM_OK)
    {
        enc_wr32(v, (uint32_t)e->data_bytes);
        r = enc_patch(f, 40u, v, 4u);
    }
    return r;
}

static void pcm_close(void *ctx)
{
    PcmEnc *e = (PcmEnc *)ctx;

    if (e != NULL)
    {
        encout_free(&e->out);
        FM_Free(e);
    }
}

static const FM_AudioEncoderOps g_pcm_ops = {
    "wav-pcm16", pcm_init, pcm_write, pcm_finish, pcm_close
};

/* ------------------------------------------------------------------ */
/* WAV, IMA ADPCM (format 0x11): 4 bits per sample                      */
/* ------------------------------------------------------------------ */
#define WAV_ADPCM_HEADER 60u

static const int16_t g_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t g_ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct
{
    EncOut    out;
    uint32_t  rate;
    uint32_t  ch;
    uint32_t  block_align;     /* bytes of one full block           */
    uint32_t  block_frames;    /* sample frames of one full block   */
    uint32_t  fill;            /* frames waiting in `blk`           */
    uint64_t  total;           /* frames handed in                  */
    uint64_t  data_bytes;
    int32_t   pred[2];
    int32_t   idx[2];
    FM_Bool   header_done;
    FM_Bool   started;
    FM_Result err;
    int16_t  *blk;             /* block_frames * ch samples         */
    uint8_t  *enc;             /* block_align bytes                 */
} AdpcmEnc;

static uint32_t adpcm_nibble(int32_t *pred, int32_t *idx, int32_t sample)
{
    int32_t step = g_ima_step[*idx];
    int32_t diff = sample - *pred;
    int32_t delta = step >> 3;
    uint32_t nib = 0u;

    if (diff < 0)
    {
        nib = 8u;
        diff = -diff;
    }
    if (diff >= step)
    {
        nib |= 4u;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step)
    {
        nib |= 2u;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step)
    {
        nib |= 1u;
        delta += step;
    }
    /* exactly what the decoder will rebuild, so the two stay in step */
    *pred += (nib & 8u) ? -delta : delta;
    if (*pred > 32767)
    {
        *pred = 32767;
    }
    else if (*pred < -32768)
    {
        *pred = -32768;
    }
    *idx += g_ima_index[nib & 7u];
    if (*idx < 0)
    {
        *idx = 0;
    }
    else if (*idx > 88)
    {
        *idx = 88;
    }
    return nib;
}

static FM_Result adpcm_header(AdpcmEnc *e, FM_File *f)
{
    uint8_t h[WAV_ADPCM_HEADER];

    memcpy(h, "RIFF", 4);
    enc_wr32(h + 4, WAV_ADPCM_HEADER - 8u);
    memcpy(h + 8, "WAVEfmt ", 8);
    enc_wr32(h + 16, 20u);
    enc_wr16(h + 20, 0x0011u);
    enc_wr16(h + 22, e->ch);
    enc_wr32(h + 24, e->rate);
    enc_wr32(h + 28, (uint32_t)(((uint64_t)e->rate * e->block_align) /
                                e->block_frames));
    enc_wr16(h + 32, e->block_align);
    enc_wr16(h + 34, 4u);
    enc_wr16(h + 36, 2u);
    enc_wr16(h + 38, e->block_frames);
    memcpy(h + 40, "fact", 4);
    enc_wr32(h + 44, 4u);
    enc_wr32(h + 48, 0u);
    memcpy(h + 52, "data", 4);
    enc_wr32(h + 56, 0u);
    e->header_done = FM_TRUE;
    return encout_put(&e->out, f, h, sizeof(h));
}

/* Encode `frames` frames of e->blk (1 + 2m for mono, 1 + 8m for stereo)
 * into e->enc; returns the bytes used. */
static uint32_t adpcm_block(AdpcmEnc *e, uint32_t frames)
{
    const int16_t *s = e->blk;
    uint8_t *d = e->enc;
    uint32_t ch = e->ch;
    uint32_t c;
    uint32_t i;

    for (c = 0u; c < ch; c++)
    {
        e->pred[c] = s[c];
        if (!e->started)
        {
            /* First block: start near the first step of the signal
             * instead of climbing up from the smallest one. */
            int32_t diff = (frames > 1u) ? ((int32_t)s[ch + c] - s[c]) : 0;
            int32_t k = 0;

            if (diff < 0)
            {
                diff = -diff;
            }
            while (k < 88 && g_ima_step[k] < diff)
            {
                k++;
            }
            e->idx[c] = k;
        }
        enc_wr16(d + 4u * c, (uint16_t)(int16_t)e->pred[c]);
        d[4u * c + 2u] = (uint8_t)e->idx[c];
        d[4u * c + 3u] = 0u;
    }
    e->started = FM_TRUE;

    if (ch == 1u)
    {
        uint32_t pos = 4u;

        for (i = 1u; i + 1u < frames; i += 2u)
        {
            uint32_t lo = adpcm_nibble(&e->pred[0], &e->idx[0], s[i]);
            uint32_t hi = adpcm_nibble(&e->pred[0], &e->idx[0], s[i + 1u]);

            d[pos++] = (uint8_t)(lo | (hi << 4));
        }
        return pos;
    }
    else
    {
        uint32_t pos = 8u;

        for (i = 1u; i + 7u < frames; i += 8u)
        {
            for (c = 0u; c < 2u; c++)
            {
                uint32_t j;

                for (j = 0u; j < 4u; j++)
                {
                    uint32_t lo = adpcm_nibble(&e->pred[c], &e->idx[c],
                                               s[(i + 2u * j) * 2u + c]);
                    uint32_t hi = adpcm_nibble(&e->pred[c], &e->idx[c],
                                               s[(i + 2u * j + 1u) * 2u + c]);

                    d[pos + 4u * c + j] = (uint8_t)(lo | (hi << 4));
                }
            }
            pos += 8u;
        }
        return pos;
    }
}

static FM_Result adpcm_init(void **ctx, const FM_AudioEncodeConfig *config)
{
    AdpcmEnc *e;
    uint32_t ch = config->channels;
    uint32_t mult = 1u;
    uint32_t ba;
    uint32_t bf;
    FM_Result r;

    /* The usual block: 256 bytes per channel at 11 kHz, twice that at
     * 22 kHz, four times from 44 kHz up. */
    if (config->sample_rate >= 44100u)
    {
        mult = 4u;
    }
    else if (config->sample_rate >= 22050u)
    {
        mult = 2u;
    }
    ba = 256u * ch * mult;
    bf = 1u + ((ba / ch) - 4u) * 2u;

    e = (AdpcmEnc *)FM_Calloc(1u, sizeof(*e) + (size_t)bf * ch * 2u + ba);
    if (e == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = encout_init(&e->out);
    if (r != FM_OK)
    {
        FM_Free(e);
        return r;
    }
    e->rate = config->sample_rate;
    e->ch = ch;
    e->block_align = ba;
    e->block_frames = bf;
    e->blk = (int16_t *)(void *)(e + 1);
    e->enc = (uint8_t *)(e->blk + (size_t)bf * ch);
    *ctx = e;
    return FM_OK;
}

static FM_Result adpcm_emit(AdpcmEnc *e, FM_File *f, uint32_t frames)
{
    uint32_t bytes;

    if ((uint64_t)e->block_align > (uint64_t)WAV_MAX_DATA - e->data_bytes)
    {
        return FM_ERR_TOO_LARGE;
    }
    bytes = adpcm_block(e, frames);
    e->data_bytes += bytes;
    return encout_put(&e->out, f, e->enc, bytes);
}

static FM_Result adpcm_write(void *ctx, FM_File *f, const int16_t *pcm,
                             size_t frame_count, size_t *frames_consumed)
{
    AdpcmEnc *e = (AdpcmEnc *)ctx;
    size_t left = frame_count;

    if (e->err != FM_OK)
    {
        return e->err;
    }
    if (!e->header_done)
    {
        e->err = adpcm_header(e, f);
    }
    while (left > 0u && e->err == FM_OK)
    {
        size_t n = (size_t)(e->block_frames - e->fill);

        if (n > left)
        {
            n = left;
        }
        memcpy(e->blk + (size_t)e->fill * e->ch, pcm,
               n * e->ch * sizeof(int16_t));
        e->fill += (uint32_t)n;
        pcm += n * e->ch;
        left -= n;
        if (e->fill == e->block_frames)
        {
            e->err = adpcm_emit(e, f, e->block_frames);
            e->fill = 0u;
        }
    }
    if (e->err != FM_OK)
    {
        return e->err;
    }
    e->total += frame_count;
    if (frames_consumed != NULL)
    {
        *frames_consumed = frame_count;
    }
    return FM_OK;
}

static FM_Result adpcm_finish(void *ctx, FM_File *f)
{
    AdpcmEnc *e = (AdpcmEnc *)ctx;
    uint8_t v[4];
    FM_Result r = e->err;

    if (r == FM_OK && !e->header_done)
    {
        r = adpcm_header(e, f);
    }
    if (r == FM_OK && e->fill > 0u)
    {
        /* The last block is filled up with a repeat of the last frame:
         * every reader expects whole blocks, and some play a short one
         * as noise.  So an ADPCM file is up to one block (46 ms at
         * 44.1 kHz) longer than what was handed in, as with any ADPCM
         * encoder; the exact length is in the "fact" chunk for the
         * readers that use it. */
        uint32_t frames = e->block_frames;
        uint32_t i;
        uint32_t c;

        for (i = e->fill; i < frames; i++)
        {
            for (c = 0u; c < e->ch; c++)
            {
                e->blk[(size_t)i * e->ch + c] =
                    e->blk[(size_t)(e->fill - 1u) * e->ch + c];
            }
        }
        r = adpcm_emit(e, f, frames);
        e->fill = 0u;
    }
    if (r == FM_OK)
    {
        r = encout_flush(&e->out, f);
    }
    if (r == FM_OK)
    {
        enc_wr32(v, (WAV_ADPCM_HEADER - 8u) + (uint32_t)e->data_bytes);
        r = enc_patch(f, 4u, v, 4u);
    }
    if (r == FM_OK)
    {
        enc_wr32(v, (e->total > 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                             : (uint32_t)e->total);
        r = enc_patch(f, 48u, v, 4u);
    }
    if (r == FM_OK)
    {
        enc_wr32(v, (uint32_t)e->data_bytes);
        r = enc_patch(f, 56u, v, 4u);
    }
    return r;
}

static void adpcm_close(void *ctx)
{
    AdpcmEnc *e = (AdpcmEnc *)ctx;

    if (e != NULL)
    {
        encout_free(&e->out);
        FM_Free(e);
    }
}

static const FM_AudioEncoderOps g_adpcm_ops = {
    "wav-ima-adpcm", adpcm_init, adpcm_write, adpcm_finish, adpcm_close
};

/* ------------------------------------------------------------------ */
/* FLAC (lossless)                                                     */
/* ------------------------------------------------------------------ */
#if FM_USE_FLAC

#define FLAC_BPS          16u
#define FLAC_MAX_PART     16u      /* partitions of one subframe (2^4)  */
#define FLAC_MIN_RICE_N   32u      /* shorter blocks are stored as-is   */
#define FLAC_HEADER_BYTES 65u      /* "fLaC" + STREAMINFO + vendor tag  */
#define FLAC_VENDOR       "FileManager"

static const uint8_t g_flac_crc8[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31,
    0x24, 0x23, 0x2A, 0x2D, 0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
    0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D, 0xE0, 0xE7, 0xEE, 0xE9,
    0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1,
    0xB4, 0xB3, 0xBA, 0xBD, 0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
    0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA, 0xB7, 0xB0, 0xB9, 0xBE,
    0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16,
    0x03, 0x04, 0x0D, 0x0A, 0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
    0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A, 0x89, 0x8E, 0x87, 0x80,
    0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8,
    0xDD, 0xDA, 0xD3, 0xD4, 0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
    0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44, 0x19, 0x1E, 0x17, 0x10,
    0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F,
    0x6A, 0x6D, 0x64, 0x63, 0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
    0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13, 0xAE, 0xA9, 0xA0, 0xA7,
    0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF,
    0xFA, 0xFD, 0xF4, 0xF3
};

static const uint16_t g_flac_crc16[256] = {
    0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
    0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
    0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
    0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
    0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
    0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
    0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
    0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
    0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
    0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
    0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
    0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
    0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
    0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
    0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
    0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
    0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202
};

typedef struct
{
    EncOut    out;
    uint32_t  rate;
    uint32_t  ch;
    uint32_t  fill;            /* frames waiting in x[]              */
    uint32_t  frame_no;
    uint64_t  total;           /* frames encoded                     */
    uint32_t  min_frame;       /* smallest / largest frame, bytes    */
    uint32_t  max_frame;
    FM_Bool   header_done;
    FM_Result err;
    int32_t  *x[2];            /* one block per channel              */
    uint8_t  *fbuf;            /* one encoded frame                  */
    uint32_t  fcap;
    uint32_t  flen;
    uint64_t  acc;             /* bit writer: low `nbits` bits valid */
    uint32_t  nbits;
} FlacEnc;

/* What one statistics pass learns about one channel layout. */
typedef struct
{
    uint64_t part[5][FLAC_MAX_PART];  /* sum |residual|: order, partition */
    uint32_t order;                   /* the predictor that won           */
    uint32_t bits;                    /* size of the subframe, an upper
                                       * bound (0 = not computed)         */
    FM_Bool  constant;
    FM_Bool  verbatim;
} FlacStat;

/* ---- bit writer ---------------------------------------------------- */

/* Append the low `n` bits of `v` (1..32; v < 2^n), most significant
 * first. */
static void flac_put(FlacEnc *e, uint32_t v, uint32_t n)
{
    uint64_t a = (e->acc << n) | v;
    uint32_t bits = e->nbits + n;

    if (bits >= 32u)
    {
        uint32_t w;
        uint8_t *p;

        if (e->flen + 4u > e->fcap)
        {
            /* Cannot happen: a subframe is only Rice coded when its
             * upper bound is below the stored size the buffer is made
             * for.  Checked all the same, so that a mistake there shows
             * as an error and not as a write past the buffer. */
            e->err = FM_ERR_INTERNAL;
            return;
        }
        bits -= 32u;
        w = (uint32_t)(a >> bits);
        p = e->fbuf + e->flen;
        p[0] = (uint8_t)(w >> 24);
        p[1] = (uint8_t)(w >> 16);
        p[2] = (uint8_t)(w >> 8);
        p[3] = (uint8_t)w;
        e->flen += 4u;
    }
    e->acc = a;
    e->nbits = bits;
}

/* Pad to a byte with zero bits and move what is left into the frame. */
static void flac_align(FlacEnc *e)
{
    if ((e->nbits & 7u) != 0u)
    {
        flac_put(e, 0u, 8u - (e->nbits & 7u));
    }
    while (e->nbits >= 8u)
    {
        if (e->flen + 1u > e->fcap)
        {
            e->err = FM_ERR_INTERNAL;
            return;
        }
        e->nbits -= 8u;
        e->fbuf[e->flen++] = (uint8_t)(e->acc >> e->nbits);
    }
}

/* ---- statistics ---------------------------------------------------- */

static uint32_t flac_abs(int32_t v)
{
    return (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
}

/* Sum of |residual| of the five fixed predictors, per partition.  The
 * residual of order k is the k-th difference of the signal, so one
 * sample costs four subtractions.  EXPR gives sample `i`. */
#define FLAC_STATS_LOOP(EXPR)                                              \
    do {                                                                   \
        int32_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;                            \
        uint32_t i = 0u;                                                   \
        uint32_t sp_;                                                      \
                                                                           \
        for (sp_ = 0u; sp_ < parts; sp_++)                                  \
        {                                                                  \
            uint32_t end = (sp_ + 1u) * psize;                             \
                                                                           \
            while (i < end)                                                \
            {                                                              \
                uint32_t stop = (end - i > 256u) ? i + 256u : end;         \
                uint32_t s0 = 0u, s1 = 0u, s2 = 0u, s3 = 0u, s4 = 0u;      \
                                                                           \
                for (; i < stop; i++)                                      \
                {                                                          \
                    int32_t d0 = (EXPR);                                   \
                    int32_t d1 = d0 - p0;                                  \
                    int32_t d2 = d1 - p1;                                  \
                    int32_t d3 = d2 - p2;                                  \
                    int32_t d4 = d3 - p3;                                  \
                                                                           \
                    p0 = d0; p1 = d1; p2 = d2; p3 = d3;                    \
                    if (i >= 4u)                                           \
                    {                                                      \
                        s0 += flac_abs(d0); s1 += flac_abs(d1);            \
                        s2 += flac_abs(d2); s3 += flac_abs(d3);            \
                        s4 += flac_abs(d4);                                \
                    }                                                      \
                    else                                                   \
                    {                                                      \
                        /* the first samples: order k has a residual   */  \
                        /* only from sample k on                       */  \
                        s0 += flac_abs(d0);                                \
                        if (i >= 1u) s1 += flac_abs(d1);                   \
                        if (i >= 2u) s2 += flac_abs(d2);                   \
                        if (i >= 3u) s3 += flac_abs(d3);                   \
                    }                                                      \
                }                                                          \
                st->part[0][sp_] += s0; st->part[1][sp_] += s1;            \
                st->part[2][sp_] += s2; st->part[3][sp_] += s3;            \
                st->part[4][sp_] += s4;                                    \
            }                                                              \
        }                                                                  \
    } while (0)

/* Bits of `count` residuals whose |r| add up to `sum`, coded with Rice
 * parameter k: never less than the real size (zigzag(r) <= 2|r|, and a
 * sum of floors is at most the floor of the sum). */
static uint64_t flac_rice_bits(uint64_t sum, uint32_t count, uint32_t k)
{
    return (uint64_t)count * (k + 1u) + ((sum << 1) >> k);
}

static uint32_t flac_best_k(uint64_t sum, uint32_t count, uint64_t *bits)
{
    uint32_t best = 0u;
    uint64_t best_bits = flac_rice_bits(sum, count, 0u);
    uint32_t k;

    for (k = 1u; k <= 14u; k++)
    {
        uint64_t b = flac_rice_bits(sum, count, k);

        if (b < best_bits)
        {
            best_bits = b;
            best = k;
        }
    }
    if (bits != NULL)
    {
        *bits = best_bits;
    }
    return best;
}

/* mode 0: a[]   1: mid = (a + b) >> 1   2: side = a - b */
static void flac_stats(const int32_t *a, const int32_t *b, int mode,
                       uint32_t n, uint32_t parts, uint32_t bps,
                       FlacStat *st)
{
    uint32_t psize = n / parts;
    uint32_t order;
    uint32_t part;
    uint64_t best_total = 0u;
    uint64_t bits;
    uint64_t total1 = 0u;
    uint64_t stored = 8u + (uint64_t)n * bps;

    memset(st, 0, sizeof(*st));
    if (mode == 0)
    {
        FLAC_STATS_LOOP(a[i]);
    }
    else if (mode == 1)
    {
        FLAC_STATS_LOOP((a[i] + b[i]) >> 1);
    }
    else
    {
        FLAC_STATS_LOOP(a[i] - b[i]);
    }

    for (order = 0u; order <= 4u; order++)
    {
        uint64_t total = 0u;

        for (part = 0u; part < parts; part++)
        {
            total += st->part[order][part];
        }
        if (order == 1u)
        {
            total1 = total;
        }
        if (order == 0u || total < best_total)
        {
            best_total = total;
            st->order = order;
        }
    }
    if (total1 == 0u)
    {
        /* every first difference is zero: one value for the block */
        st->constant = FM_TRUE;
        st->bits = 8u + bps;
        return;
    }

    bits = 8u + (uint64_t)st->order * bps + 6u;
    for (part = 0u; part < parts; part++)
    {
        uint64_t pb = 0u;
        uint32_t count = psize - ((part == 0u) ? st->order : 0u);

        (void)flac_best_k(st->part[st->order][part], count, &pb);
        bits += 4u + pb;
    }
    if (bits >= stored)
    {
        st->verbatim = FM_TRUE;      /* noise: predicting only costs */
        bits = stored;
    }
    st->bits = (uint32_t)bits;
}

/* ---- subframes ------------------------------------------------------ */

static void flac_put_sample(FlacEnc *e, int32_t v, uint32_t bps)
{
    flac_put(e, (uint32_t)v & ((1u << bps) - 1u), bps);
}

#define FLAC_RICE_LOOP(EXPR)                                               \
    for (; i < end; i++)                                                   \
    {                                                                      \
        int32_t r = (EXPR);                                                \
        uint32_t u = ((uint32_t)r << 1) ^ ((r < 0) ? 0xFFFFFFFFu : 0u);    \
        uint32_t q = u >> k;                                               \
                                                                           \
        if (q + k < 32u)                                                   \
        {                                                                  \
            /* q zero bits, a one, then the k low bits: one write */       \
            flac_put(e, (1u << k) | (u & mask), q + k + 1u);               \
        }                                                                  \
        else                                                               \
        {                                                                  \
            while (q >= 32u)                                               \
            {                                                              \
                flac_put(e, 0u, 32u);                                      \
                q -= 32u;                                                  \
            }                                                              \
            flac_put(e, 1u, q + 1u);                                       \
            if (k != 0u)                                                   \
            {                                                              \
                flac_put(e, u & mask, k);                                  \
            }                                                              \
        }                                                                  \
    }

static void flac_subframe(FlacEnc *e, const int32_t *x, uint32_t n,
                          uint32_t parts, uint32_t part_order,
                          uint32_t bps, const FlacStat *st)
{
    uint32_t psize;
    uint32_t part;
    uint32_t i;

    if (st->constant)
    {
        flac_put(e, 0x00u, 8u);
        flac_put_sample(e, x[0], bps);
        return;
    }
    if (st->verbatim)
    {
        flac_put(e, 0x02u, 8u);
        for (i = 0u; i < n; i++)
        {
            flac_put_sample(e, x[i], bps);
        }
        return;
    }

    /* 0 | 001 order | 0 */
    flac_put(e, (0x08u | st->order) << 1, 8u);
    for (i = 0u; i < st->order; i++)
    {
        flac_put_sample(e, x[i], bps);
    }
    flac_put(e, 0u, 2u);              /* Rice, 4-bit parameters */
    flac_put(e, part_order, 4u);

    psize = n / parts;
    i = st->order;
    for (part = 0u; part < parts; part++)
    {
        uint32_t end = (part + 1u) * psize;
        uint32_t count = psize - ((part == 0u) ? st->order : 0u);
        uint32_t k = flac_best_k(st->part[st->order][part], count, NULL);
        uint32_t mask = (1u << k) - 1u;

        flac_put(e, k, 4u);
        switch (st->order)
        {
        case 0u:
            FLAC_RICE_LOOP(x[i])
            break;
        case 1u:
            FLAC_RICE_LOOP(x[i] - x[i - 1u])
            break;
        case 2u:
            FLAC_RICE_LOOP(x[i] - 2 * x[i - 1u] + x[i - 2u])
            break;
        case 3u:
            FLAC_RICE_LOOP(x[i] - 3 * (x[i - 1u] - x[i - 2u]) - x[i - 3u])
            break;
        default:
            FLAC_RICE_LOOP(x[i] - 4 * (x[i - 1u] + x[i - 3u]) +
                           6 * x[i - 2u] + x[i - 4u])
            break;
        }
    }
}

/* ---- frames --------------------------------------------------------- */

static uint32_t flac_block_code(uint32_t n)
{
    uint32_t c;

    if (n == 192u)
    {
        return 1u;
    }
    for (c = 2u; c <= 5u; c++)
    {
        if (n == (576u << (c - 2u)))
        {
            return c;
        }
    }
    for (c = 8u; c <= 15u; c++)
    {
        if (n == (256u << (c - 8u)))
        {
            return c;
        }
    }
    return (n <= 256u) ? 6u : 7u;     /* n - 1 follows, 8 or 16 bits */
}

static uint32_t flac_rate_code(uint32_t rate)
{
    switch (rate)
    {
    case 88200u:  return 1u;
    case 176400u: return 2u;
    case 192000u: return 3u;
    case 8000u:   return 4u;
    case 16000u:  return 5u;
    case 22050u:  return 6u;
    case 24000u:  return 7u;
    case 32000u:  return 8u;
    case 44100u:  return 9u;
    case 48000u:  return 10u;
    case 96000u:  return 11u;
    default:      break;
    }
    if ((rate % 1000u) == 0u && rate <= 255000u)
    {
        return 12u;                   /* kHz in 8 bits */
    }
    if (rate <= 65535u)
    {
        return 13u;                   /* Hz in 16 bits */
    }
    if ((rate % 10u) == 0u && rate <= 655350u)
    {
        return 14u;                   /* tens of Hz in 16 bits */
    }
    return 0u;                        /* "see STREAMINFO" */
}

static uint32_t flac_frame_header(FlacEnc *e, uint32_t n, uint32_t layout)
{
    uint8_t *h = e->fbuf;
    uint32_t len = 0u;
    uint32_t bc = flac_block_code(n);
    uint32_t rc = flac_rate_code(e->rate);
    uint32_t v = e->frame_no;
    uint8_t crc = 0u;
    uint32_t i;

    h[len++] = 0xFFu;
    h[len++] = 0xF8u;                 /* sync, fixed block size */
    h[len++] = (uint8_t)((bc << 4) | rc);
    h[len++] = (uint8_t)((layout << 4) | (4u << 1));     /* 16 bits */

    /* the frame number, coded the way UTF-8 codes a character */
    if (v < 0x80u)
    {
        h[len++] = (uint8_t)v;
    }
    else
    {
        uint32_t extra = (v < 0x800u) ? 1u
                       : (v < 0x10000u) ? 2u
                       : (v < 0x200000u) ? 3u
                       : (v < 0x4000000u) ? 4u
                       : (v < 0x80000000u) ? 5u : 6u;
        uint32_t lead = (0xFFu << (7u - extra)) & 0xFFu;
        uint32_t top = (extra == 6u) ? 0u : (v >> (6u * extra));

        h[len++] = (uint8_t)(lead | top);
        for (i = extra; i > 0u; i--)
        {
            h[len++] = (uint8_t)(0x80u | ((v >> (6u * (i - 1u))) & 0x3Fu));
        }
    }
    if (bc == 6u)
    {
        h[len++] = (uint8_t)(n - 1u);
    }
    else if (bc == 7u)
    {
        h[len++] = (uint8_t)((n - 1u) >> 8);
        h[len++] = (uint8_t)(n - 1u);
    }
    if (rc == 12u)
    {
        h[len++] = (uint8_t)(e->rate / 1000u);
    }
    else if (rc == 13u || rc == 14u)
    {
        uint32_t r = (rc == 13u) ? e->rate : e->rate / 10u;

        h[len++] = (uint8_t)(r >> 8);
        h[len++] = (uint8_t)r;
    }
    for (i = 0u; i < len; i++)
    {
        crc = g_flac_crc8[crc ^ h[i]];
    }
    h[len++] = crc;
    return len;
}

/* Encode the `n` frames waiting in e->x[] as one FLAC frame. */
static FM_Result flac_encode_block(FlacEnc *e, FM_File *f, uint32_t n)
{
    FlacStat st[4];                   /* left, right, mid, side */
    const FlacStat *s0;
    const FlacStat *s1 = NULL;
    int32_t *l = e->x[0];
    int32_t *r = e->x[1];
    uint32_t layout = 0u;             /* channel assignment code */
    uint32_t bps1 = FLAC_BPS;         /* bits of the second channel */
    uint32_t bps0 = FLAC_BPS;
    uint32_t part_order = 0u;
    uint32_t parts;
    uint32_t i;
    uint16_t crc = 0u;

    /* As many partitions as divide the block evenly and stay 64 samples
     * or longer; each gets its own Rice parameter. */
    while (part_order < 4u && (n % (2u << part_order)) == 0u &&
           (n >> (part_order + 1u)) >= 64u)
    {
        part_order++;
    }
    parts = 1u << part_order;

    if (n < FLAC_MIN_RICE_N)
    {
        /* a handful of samples (the tail of the stream): stored */
        memset(st, 0, sizeof(st));
        st[0].verbatim = FM_TRUE;
        st[1].verbatim = FM_TRUE;
        s0 = &st[0];
        if (e->ch == 2u)
        {
            s1 = &st[1];
            layout = 1u;
        }
    }
    else if (e->ch == 1u)
    {
        flac_stats(l, NULL, 0, n, parts, FLAC_BPS, &st[0]);
        s0 = &st[0];
    }
    else
    {
        uint32_t lr;
        uint32_t ls;
        uint32_t sr;
        uint32_t ms;
        uint32_t best;

        flac_stats(l, NULL, 0, n, parts, FLAC_BPS, &st[0]);
        flac_stats(r, NULL, 0, n, parts, FLAC_BPS, &st[1]);
        flac_stats(l, r, 1, n, parts, FLAC_BPS, &st[2]);
        flac_stats(l, r, 2, n, parts, FLAC_BPS + 1u, &st[3]);
        lr = st[0].bits + st[1].bits;
        ls = st[0].bits + st[3].bits;
        sr = st[3].bits + st[1].bits;
        ms = st[2].bits + st[3].bits;
        best = lr;
        layout = 1u;                              /* left, right */
        s0 = &st[0];
        s1 = &st[1];
        if (ls < best)
        {
            best = ls;
            layout = 8u;                          /* left, side */
            s0 = &st[0];
            s1 = &st[3];
        }
        if (sr < best)
        {
            best = sr;
            layout = 9u;                          /* side, right */
            s0 = &st[3];
            s1 = &st[1];
        }
        if (ms < best)
        {
            layout = 10u;                         /* mid, side */
            s0 = &st[2];
            s1 = &st[3];
        }
        /* turn the block into the two channels that won, in place */
        if (layout == 8u)
        {
            for (i = 0u; i < n; i++)
            {
                r[i] = l[i] - r[i];
            }
            bps1 = FLAC_BPS + 1u;
        }
        else if (layout == 9u)
        {
            for (i = 0u; i < n; i++)
            {
                l[i] = l[i] - r[i];
            }
            bps0 = FLAC_BPS + 1u;
        }
        else if (layout == 10u)
        {
            for (i = 0u; i < n; i++)
            {
                int32_t side = l[i] - r[i];

                l[i] = (l[i] + r[i]) >> 1;
                r[i] = side;
            }
            bps1 = FLAC_BPS + 1u;
        }
    }

    e->flen = flac_frame_header(e, n, layout);
    e->acc = 0u;
    e->nbits = 0u;
    flac_subframe(e, l, n, parts, part_order, bps0, s0);
    if (s1 != NULL)
    {
        flac_subframe(e, r, n, parts, part_order, bps1, s1);
    }
    flac_align(e);
    if (e->err != FM_OK)
    {
        return e->err;
    }
    for (i = 0u; i < e->flen; i++)
    {
        crc = (uint16_t)((uint16_t)(crc << 8) ^
                         g_flac_crc16[(crc >> 8) ^ e->fbuf[i]]);
    }
    e->fbuf[e->flen++] = (uint8_t)(crc >> 8);
    e->fbuf[e->flen++] = (uint8_t)crc;

    if (e->min_frame == 0u || e->flen < e->min_frame)
    {
        e->min_frame = e->flen;
    }
    if (e->flen > e->max_frame)
    {
        e->max_frame = e->flen;
    }
    e->frame_no++;
    e->total += n;
    return encout_put(&e->out, f, e->fbuf, e->flen);
}

static void flac_streaminfo(const FlacEnc *e, uint8_t *s)
{
    uint64_t total = (e->total > 0xFFFFFFFFFull) ? 0u : e->total;

    memset(s, 0, 34);
    s[0] = (uint8_t)(FM_FLAC_ENC_BLOCK >> 8);
    s[1] = (uint8_t)(FM_FLAC_ENC_BLOCK);
    s[2] = s[0];
    s[3] = s[1];
    s[4] = (uint8_t)(e->min_frame >> 16);
    s[5] = (uint8_t)(e->min_frame >> 8);
    s[6] = (uint8_t)(e->min_frame);
    s[7] = (uint8_t)(e->max_frame >> 16);
    s[8] = (uint8_t)(e->max_frame >> 8);
    s[9] = (uint8_t)(e->max_frame);
    s[10] = (uint8_t)(e->rate >> 12);
    s[11] = (uint8_t)(e->rate >> 4);
    s[12] = (uint8_t)(((e->rate & 0x0Fu) << 4) | ((e->ch - 1u) << 1) |
                      ((FLAC_BPS - 1u) >> 4));
    s[13] = (uint8_t)((((FLAC_BPS - 1u) & 0x0Fu) << 4) |
                      (uint32_t)((total >> 32) & 0x0Fu));
    s[14] = (uint8_t)(total >> 24);
    s[15] = (uint8_t)(total >> 16);
    s[16] = (uint8_t)(total >> 8);
    s[17] = (uint8_t)(total);
    /* s[18..33]: the MD5 of the samples, left at zero = "not computed" */
}

static FM_Result flac_header(FlacEnc *e, FM_File *f)
{
    uint8_t h[FLAC_HEADER_BYTES];
    uint32_t vendor = (uint32_t)(sizeof(FLAC_VENDOR) - 1u);

    memcpy(h, "fLaC", 4);
    h[4] = 0x00u;                     /* STREAMINFO, 34 bytes */
    h[5] = 0u;
    h[6] = 0u;
    h[7] = 34u;
    flac_streaminfo(e, h + 8);
    h[42] = 0x84u;                    /* last block: VORBIS_COMMENT */
    h[43] = 0u;
    h[44] = 0u;
    h[45] = (uint8_t)(8u + vendor);
    enc_wr32(h + 46, vendor);
    memcpy(h + 50, FLAC_VENDOR, vendor);
    enc_wr32(h + 50u + vendor, 0u);
    e->header_done = FM_TRUE;
    return encout_put(&e->out, f, h, sizeof(h));
}

static FM_Result flac_init(void **ctx, const FM_AudioEncodeConfig *config)
{
    FlacEnc *e;
    uint32_t ch = config->channels;
    size_t samples = (size_t)FM_FLAC_ENC_BLOCK * ch * sizeof(int32_t);
    /* the largest frame: a header and every sample stored, the side
     * channel being one bit wider */
    uint32_t fcap = 32u + ch * (2u + (FM_FLAC_ENC_BLOCK *
                                      (FLAC_BPS + 1u) + 7u) / 8u);
    FM_Result r;

    if (config->sample_rate > 655350u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    e = (FlacEnc *)FM_Calloc(1u, sizeof(*e) + samples + fcap);
    if (e == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = encout_init(&e->out);
    if (r != FM_OK)
    {
        FM_Free(e);
        return r;
    }
    e->rate = config->sample_rate;
    e->ch = ch;
    e->x[0] = (int32_t *)(void *)(e + 1);
    e->x[1] = e->x[0] + ((ch == 2u) ? FM_FLAC_ENC_BLOCK : 0u);
    e->fbuf = (uint8_t *)(e->x[0] + (size_t)FM_FLAC_ENC_BLOCK * ch);
    e->fcap = fcap;
    *ctx = e;
    return FM_OK;
}

static FM_Result flac_write(void *ctx, FM_File *f, const int16_t *pcm,
                            size_t frame_count, size_t *frames_consumed)
{
    FlacEnc *e = (FlacEnc *)ctx;
    size_t left = frame_count;

    if (e->err != FM_OK)
    {
        return e->err;
    }
    if (!e->header_done)
    {
        e->err = flac_header(e, f);
    }
    while (left > 0u && e->err == FM_OK)
    {
        size_t n = (size_t)(FM_FLAC_ENC_BLOCK - e->fill);
        size_t i;

        if (n > left)
        {
            n = left;
        }
        if (e->ch == 2u)
        {
            int32_t *l = e->x[0] + e->fill;
            int32_t *r = e->x[1] + e->fill;

            for (i = 0u; i < n; i++)
            {
                l[i] = pcm[2u * i];
                r[i] = pcm[2u * i + 1u];
            }
        }
        else
        {
            int32_t *m = e->x[0] + e->fill;

            for (i = 0u; i < n; i++)
            {
                m[i] = pcm[i];
            }
        }
        e->fill += (uint32_t)n;
        pcm += n * e->ch;
        left -= n;
        if (e->fill == FM_FLAC_ENC_BLOCK)
        {
            e->err = flac_encode_block(e, f, e->fill);
            e->fill = 0u;
        }
    }
    if (e->err != FM_OK)
    {
        return e->err;
    }
    if (frames_consumed != NULL)
    {
        *frames_consumed = frame_count;
    }
    return FM_OK;
}

static FM_Result flac_finish(void *ctx, FM_File *f)
{
    FlacEnc *e = (FlacEnc *)ctx;
    uint8_t s[34];
    FM_Result r = e->err;

    if (r == FM_OK && !e->header_done)
    {
        r = flac_header(e, f);
    }
    if (r == FM_OK && e->fill > 0u)
    {
        r = flac_encode_block(e, f, e->fill);
        e->fill = 0u;
    }
    if (r == FM_OK)
    {
        r = encout_flush(&e->out, f);
    }
    if (r == FM_OK)
    {
        flac_streaminfo(e, s);        /* now with the length and sizes */
        r = enc_patch(f, 8u, s, sizeof(s));
    }
    return r;
}

static void flac_close(void *ctx)
{
    FlacEnc *e = (FlacEnc *)ctx;

    if (e != NULL)
    {
        encout_free(&e->out);
        FM_Free(e);
    }
}

static const FM_AudioEncoderOps g_flac_ops = {
    "flac", flac_init, flac_write, flac_finish, flac_close
};

#endif /* FM_USE_FLAC */

/* ------------------------------------------------------------------ */
/* Formats and the encoder registry                                    */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_AudioFileFormat format;
    const FM_AudioEncoderOps *ops;
} FM_AudioEncoderEntry;

static FM_AudioEncoderEntry g_aenc[FM_AUDIO_FILE_COUNT];

static FM_Bool audio_ext_equal(const char *text, const char *extension)
{
    while (*text != '\0' && *extension != '\0')
    {
        char a = *text++;
        char b = *extension++;

        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return FM_FALSE;
    }
    return (*text == '\0' && *extension == '\0') ? FM_TRUE : FM_FALSE;
}

FM_AudioFileFormat Audio_FormatFromPath(const char *path)
{
    const char *dot = NULL;
    const char *p;

    if (path == NULL)
    {
        return FM_AUDIO_FILE_AUTO;
    }
    for (p = path; *p != '\0'; p++)
    {
        if (*p == '.') dot = p;
        if (*p == '/' || *p == '\\') dot = NULL;
    }
    if (dot == NULL)
    {
        return FM_AUDIO_FILE_AUTO;
    }
    if (audio_ext_equal(dot, ".wav") || audio_ext_equal(dot, ".wave"))
    {
        return FM_AUDIO_FILE_WAV;
    }
    if (audio_ext_equal(dot, ".flac"))
    {
        return FM_AUDIO_FILE_FLAC;
    }
    if (audio_ext_equal(dot, ".mp3"))
    {
        return FM_AUDIO_FILE_MP3;
    }
    if (audio_ext_equal(dot, ".ogg") || audio_ext_equal(dot, ".oga"))
    {
        return FM_AUDIO_FILE_VORBIS;
    }
    if (audio_ext_equal(dot, ".opus"))
    {
        return FM_AUDIO_FILE_OPUS;
    }
    return FM_AUDIO_FILE_AUTO;
}

const char *Audio_FormatExtension(FM_AudioFileFormat format)
{
    switch (format)
    {
    case FM_AUDIO_FILE_WAV:    return ".wav";
    case FM_AUDIO_FILE_ADPCM:  return ".wav";
    case FM_AUDIO_FILE_FLAC:   return ".flac";
    case FM_AUDIO_FILE_MP3:    return ".mp3";
    case FM_AUDIO_FILE_VORBIS: return ".ogg";
    case FM_AUDIO_FILE_OPUS:   return ".opus";
    default:                   break;
    }
    return "";
}

static const FM_AudioEncoderOps *audio_builtin_encoder(
    FM_AudioFileFormat format)
{
    switch (format)
    {
    case FM_AUDIO_FILE_WAV:   return &g_pcm_ops;
    case FM_AUDIO_FILE_ADPCM: return &g_adpcm_ops;
#if FM_USE_FLAC
    case FM_AUDIO_FILE_FLAC:  return &g_flac_ops;
#endif
    default:                  break;
    }
    return NULL;
}

FM_Result Audio_RegisterEncoder(FM_AudioFileFormat format,
                                const FM_AudioEncoderOps *ops)
{
    if (format <= FM_AUDIO_FILE_AUTO || format >= FM_AUDIO_FILE_COUNT ||
        format == FM_AUDIO_FILE_WAV ||
        ops == NULL || ops->name == NULL || ops->init == NULL ||
        ops->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    g_aenc[format].format = format;
    g_aenc[format].ops = ops;
    return FM_OK;
}

FM_Result Audio_UnregisterEncoder(FM_AudioFileFormat format)
{
    if (format <= FM_AUDIO_FILE_AUTO || format >= FM_AUDIO_FILE_COUNT ||
        g_aenc[format].ops == NULL)
    {
        return FM_ERR_NOT_FOUND;
    }
    memset(&g_aenc[format], 0, sizeof(g_aenc[format]));
    return FM_OK;
}

const FM_AudioEncoderOps *Audio_GetEncoder(FM_AudioFileFormat format)
{
    if (format <= FM_AUDIO_FILE_AUTO || format >= FM_AUDIO_FILE_COUNT)
    {
        return NULL;
    }
    /* a registered encoder comes first, so a hardware or a better one
     * can take the place of a built-in */
    if (g_aenc[format].ops != NULL)
    {
        return g_aenc[format].ops;
    }
    return audio_builtin_encoder(format);
}

FM_Bool Audio_CanEncode(FM_AudioFileFormat format)
{
    return (Audio_GetEncoder(format) != NULL) ? FM_TRUE : FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* File writer: PCM16 in, a finished file out                          */
/* ------------------------------------------------------------------ */
static FM_Result writer_encode(FM_AudioFileWriter *writer,
                               const int16_t *pcm, size_t frames)
{
    while (frames > 0u)
    {
        size_t used = 0u;
        FM_Result r = writer->encoder->write(writer->encoder_ctx,
                                             &writer->file, pcm, frames,
                                             &used);

        if (r != FM_OK)
        {
            return r;
        }
        if (used == 0u || used > frames)
        {
            return FM_ERR_IO;        /* an encoder that makes no progress */
        }
        pcm += used * writer->out_channels;
        frames -= used;
    }
    return FM_OK;
}

FM_Result AudioFileWriter_Open(FM_AudioFileWriter *writer, const char *path,
                               FM_AudioFileFormat format,
                               const FM_AudioEncodeConfig *config)
{
    FM_AudioEncodeConfig out;
    const FM_AudioEncoderOps *ops;
    void *ctx = NULL;
    FM_Result r;

    if (writer == NULL || path == NULL || config == NULL ||
        config->sample_rate == 0 ||
        (config->channels != 1 && config->channels != 2) ||
        config->bits_per_sample != 16)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (format == FM_AUDIO_FILE_AUTO)
    {
        format = Audio_FormatFromPath(path);
    }
    ops = Audio_GetEncoder(format);
    if (ops == NULL)
    {
        /* Nothing is created: no WAV bytes under an .mp3 name, and no
         * empty file left behind. */
        return FM_ERR_UNSUPPORTED;
    }

    out = *config;
    if (config->out_channels == 1u || config->out_channels == 2u)
    {
        out.channels = config->out_channels;
    }
    out.out_channels = out.channels;

    r = ops->init(&ctx, &out);
    if (r != FM_OK)
    {
        return r;
    }
    memset(writer, 0, sizeof(*writer));
    File_Init(&writer->file);
    r = File_OpenEx(&writer->file, path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        if (ops->close != NULL)
        {
            ops->close(ctx);
        }
        memset(writer, 0, sizeof(*writer));
        return r;
    }
    writer->format = format;
    writer->config = *config;
    writer->out_channels = out.channels;
    writer->encoder = ops;
    writer->encoder_ctx = ctx;
    writer->opened = FM_TRUE;
    return FM_OK;
}

FM_Result AudioFileWriter_WriteFrames(FM_AudioFileWriter *writer,
                                      const int16_t *pcm,
                                      size_t frame_count,
                                      size_t *frames_written)
{
    FM_Result r = FM_OK;

    if (frames_written != NULL)
    {
        *frames_written = 0;
    }
    if (writer == NULL || !writer->opened || pcm == NULL || frame_count == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (writer->error != FM_OK)
    {
        return writer->error;        /* the file is already incomplete */
    }

    if (writer->out_channels == writer->config.channels)
    {
        r = writer_encode(writer, pcm, frame_count);
    }
    else
    {
        int16_t tmp[512];
        size_t left = frame_count;

        while (left > 0u && r == FM_OK)
        {
            size_t n = (left < 256u) ? left : 256u;
            size_t i;

            if (writer->out_channels == 1u)
            {
                for (i = 0u; i < n; i++)
                {
                    tmp[i] = (int16_t)(((int32_t)pcm[2u * i] +
                                        (int32_t)pcm[2u * i + 1u]) >> 1);
                }
            }
            else
            {
                for (i = 0u; i < n; i++)
                {
                    tmp[2u * i] = pcm[i];
                    tmp[2u * i + 1u] = pcm[i];
                }
            }
            r = writer_encode(writer, tmp, n);
            pcm += n * writer->config.channels;
            left -= n;
        }
    }
    if (r != FM_OK)
    {
        writer->error = r;
        return r;
    }
    writer->frames += frame_count;
    if (frames_written != NULL)
    {
        *frames_written = frame_count;
    }
    return FM_OK;
}

FM_Result AudioFileWriter_Close(FM_AudioFileWriter *writer)
{
    FM_Result r;
    FM_Result c;

    if (writer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!writer->opened)
    {
        return FM_ERR_NOT_OPEN;
    }
    r = writer->error;
    if (r == FM_OK && writer->encoder->finish != NULL)
    {
        r = writer->encoder->finish(writer->encoder_ctx, &writer->file);
    }
    if (r == FM_OK)
    {
        r = File_Flush(&writer->file);
    }
    c = File_Close(&writer->file);
    if (r == FM_OK)
    {
        r = c;
    }
    if (writer->encoder->close != NULL)
    {
        writer->encoder->close(writer->encoder_ctx);
    }
    memset(writer, 0, sizeof(*writer));
    return r;
}

/* ------------------------------------------------------------------ */
/* File to file: decode -> your processing -> encode                   */
/* ------------------------------------------------------------------ */
#ifndef FM_AUDIO_CONVERT_SAMPLES
#define FM_AUDIO_CONVERT_SAMPLES 4096u
#endif

static FM_Bool convert_same_file(const char *a, const char *b)
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

FM_Result Audio_Convert(const char *src, const char *dst,
                        const FM_AudioConvertConfig *config,
                        uint64_t *frames_out)
{
    static const FM_AudioConvertConfig defaults;
    FM_Audio in;
    FM_AudioFileWriter out;
    FM_AudioEncodeConfig enc;
    int16_t small[256];
    int16_t *buf;
    int16_t *heap;
    size_t cap;
    uint64_t done = 0u;
    uint32_t ch;
    FM_Result r;

    if (frames_out != NULL)
    {
        *frames_out = 0u;
    }
    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (config == NULL)
    {
        config = &defaults;
    }
    if (convert_same_file(src, dst))
    {
        return FM_ERR_INVALID_ARGUMENT;   /* it would be cut to nothing */
    }

    memset(&in, 0, sizeof(in));
    r = Audio_Open(src, &in);
    if (r != FM_OK)
    {
        return r;
    }
    ch = in.info.channels;
    if ((ch != 1u && ch != 2u) || in.info.sample_rate == 0u)
    {
        (void)Audio_Close(&in);
        return FM_ERR_UNSUPPORTED;
    }

    memset(&enc, 0, sizeof(enc));
    enc.sample_rate = in.info.sample_rate;
    enc.channels = (uint16_t)ch;
    enc.bits_per_sample = 16u;
    enc.bitrate_kbps = config->bitrate_kbps;
    enc.out_channels = config->out_channels;
    r = AudioFileWriter_Open(&out, dst, config->format, &enc);
    if (r != FM_OK)
    {
        (void)Audio_Close(&in);
        return r;
    }

    heap = (int16_t *)FM_Alloc((size_t)FM_AUDIO_CONVERT_SAMPLES *
                               sizeof(int16_t));
    buf = (heap != NULL) ? heap : small;
    cap = (heap != NULL) ? (size_t)FM_AUDIO_CONVERT_SAMPLES
                         : sizeof(small) / sizeof(small[0]);

    for (;;)
    {
        size_t got = 0u;

        r = Audio_Read(&in, buf, cap, &got);
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            if (got == 0u)
            {
                break;
            }
        }
        if (r != FM_OK || got == 0u)
        {
            break;
        }
        if (config->process != NULL)
        {
            config->process(config->process_ctx, buf, got,
                            in.info.sample_rate, (uint16_t)ch);
        }
        r = AudioFileWriter_WriteFrames(&out, buf, got / ch, NULL);
        if (r != FM_OK)
        {
            break;
        }
        done += got / ch;
        if (config->progress != NULL &&
            !config->progress(config->progress_ctx, done,
                              in.info.total_frames))
        {
            r = FM_ERR_CANCELLED;
            break;
        }
    }

    if (heap != NULL)
    {
        FM_Free(heap);
    }
    (void)Audio_Close(&in);
    {
        FM_Result c = AudioFileWriter_Close(&out);

        if (r == FM_OK)
        {
            r = c;
        }
    }
    if (r != FM_OK)
    {
        (void)File_Delete(dst);      /* never leave half a file */
        return r;
    }
    if (frames_out != NULL)
    {
        *frames_out = done;
    }
    return FM_OK;
}

#else /* FM_USE_AUDIO == 0 */

FM_AudioFileFormat Audio_FormatFromPath(const char *path)
{
    (void)path;
    return FM_AUDIO_FILE_AUTO;
}

const char *Audio_FormatExtension(FM_AudioFileFormat format)
{
    (void)format;
    return "";
}

FM_Bool Audio_CanEncode(FM_AudioFileFormat format)
{
    (void)format;
    return FM_FALSE;
}

FM_Result Audio_RegisterEncoder(FM_AudioFileFormat format,
                                const FM_AudioEncoderOps *ops)
{
    (void)format; (void)ops;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_UnregisterEncoder(FM_AudioFileFormat format)
{
    (void)format;
    return FM_ERR_UNSUPPORTED;
}

const FM_AudioEncoderOps *Audio_GetEncoder(FM_AudioFileFormat format)
{
    (void)format;
    return NULL;
}

FM_Result AudioFileWriter_Open(FM_AudioFileWriter *writer, const char *path,
                               FM_AudioFileFormat format,
                               const FM_AudioEncodeConfig *config)
{
    (void)writer; (void)path; (void)format; (void)config;
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioFileWriter_WriteFrames(FM_AudioFileWriter *writer,
                                      const int16_t *pcm,
                                      size_t frame_count,
                                      size_t *frames_written)
{
    (void)writer; (void)pcm; (void)frame_count;
    if (frames_written != NULL)
    {
        *frames_written = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioFileWriter_Close(FM_AudioFileWriter *writer)
{
    (void)writer;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Convert(const char *src, const char *dst,
                        const FM_AudioConvertConfig *config,
                        uint64_t *frames_out)
{
    (void)src; (void)dst; (void)config;
    if (frames_out != NULL)
    {
        *frames_out = 0u;
    }
    return FM_ERR_UNSUPPORTED;
}

#endif /* FM_USE_AUDIO */
