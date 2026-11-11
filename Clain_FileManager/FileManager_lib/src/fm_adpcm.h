/*
 * FileManager - ADPCM block decoders (internal)
 *
 * ADPCM as WAV, AVI and MKV (A_MS/ACM) carry it: a row of blocks of
 * nBlockAlign bytes, each decodable on its own.  Two coders:
 *
 *     0x0011  IMA (DVI) ADPCM
 *     0x0002  Microsoft ADPCM
 *
 * Shared by fm_video.c (audio tracks, both coders) and fm_audio.c (WAV
 * files with Microsoft ADPCM; its IMA reader is older and its own).
 * Everything is static: include it once per file that needs it, and
 * define FM_ADPCM_WANT_IMA before the include for the IMA decoder.
 *
 * Output is interleaved 16-bit little-endian PCM, written bytewise.
 * Mono and stereo only (the callers check).  Both decoders were compared
 * sample for sample with ffmpeg (test/audit_v3_tests.h).
 */
#ifndef FM_ADPCM_H
#define FM_ADPCM_H

#include "fm_types.h"
#include <string.h>

static uint16_t adpcm_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void adpcm_put(uint8_t *dst, size_t frame, uint32_t ch, uint32_t c,
                      int32_t s)
{
    uint8_t *p = dst + (frame * ch + c) * 2u;

    p[0] = (uint8_t)((uint32_t)s & 0xFFu);
    p[1] = (uint8_t)(((uint32_t)s >> 8) & 0xFFu);
}

/* Frames one block of `n` bytes holds. */
static size_t adpcm_block_frames(uint16_t tag, uint32_t ch, size_t n)
{
    if (tag == 0x0002u)
    {
        return (n < 7u * ch) ? 0u : 2u + ((n - 7u * ch) * 2u) / ch;
    }
    return (n < 4u * ch) ? 0u : 1u + 8u * ((n - 4u * ch) / (4u * ch));
}

#ifdef FM_ADPCM_WANT_IMA
static const int16_t adpcm_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t adpcm_index[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };


/* IMA: per channel a 4-byte header (first sample, step index), then
 * 4 bytes = 8 samples for each channel in turn, low nibble first. */
static void adpcm_ima_block(const uint8_t *src, size_t n, uint32_t ch,
                            uint8_t *dst)
{
    int32_t pred[2];
    int32_t idx[2];
    size_t frames = 1u;
    size_t pos = 4u * ch;
    uint32_t c;

    for (c = 0u; c < ch; c++)
    {
        pred[c] = (int16_t)adpcm_rd16(src + 4u * c);
        idx[c] = (src[4u * c + 2u] > 88u) ? 88 : (int32_t)src[4u * c + 2u];
        adpcm_put(dst, 0u, ch, c, pred[c]);
    }
    while (pos + 4u * ch <= n)
    {
        for (c = 0u; c < ch; c++)
        {
            uint32_t k;

            for (k = 0u; k < 8u; k++)
            {
                uint8_t byte = src[pos + 4u * c + (k >> 1)];
                uint32_t nib = ((k & 1u) != 0u) ? ((uint32_t)byte >> 4)
                                                : ((uint32_t)byte & 0x0Fu);
                int32_t step = adpcm_step[idx[c]];
                int32_t diff = step >> 3;

                if ((nib & 1u) != 0u) { diff += step >> 2; }
                if ((nib & 2u) != 0u) { diff += step >> 1; }
                if ((nib & 4u) != 0u) { diff += step; }
                pred[c] = ((nib & 8u) != 0u) ? pred[c] - diff : pred[c] + diff;
                if (pred[c] < -32768) { pred[c] = -32768; }
                if (pred[c] > 32767)  { pred[c] = 32767; }
                idx[c] += adpcm_index[nib];
                if (idx[c] < 0)  { idx[c] = 0; }
                if (idx[c] > 88) { idx[c] = 88; }
                adpcm_put(dst, frames + k, ch, c, pred[c]);
            }
        }
        pos += 4u * ch;
        frames += 8u;
    }
}
#endif /* FM_ADPCM_WANT_IMA */

/* Microsoft ADPCM: per channel a predictor number, a step and two
 * samples; then one nibble per sample, high nibble first, the channels
 * taking turns.  A predictor number outside the standard seven makes
 * the block silence (FM_FALSE). */
static FM_Bool adpcm_ms_block(const uint8_t *src, size_t n, uint32_t ch,
                              uint8_t *dst)
{
    static const int16_t coef1[7] = { 256, 512, 0, 192, 240, 460, 392 };
    static const int16_t coef2[7] = { 0, -256, 0, 64, 0, -208, -232 };
    static const int16_t adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
                                       768, 614, 512, 409, 307, 230, 230, 230 };
    int32_t k1[2];
    int32_t k2[2];
    int32_t delta[2];
    int32_t s1[2];
    int32_t s2[2];
    size_t total = adpcm_block_frames(0x0002u, ch, n);
    size_t frame = 2u;
    size_t pos = 7u * ch;
    uint32_t c;
    uint32_t turn = 0u;

    for (c = 0u; c < ch; c++)
    {
        if (src[c] > 6u)
        {
            memset(dst, 0, total * ch * 2u);
            return FM_FALSE;
        }
        k1[c] = coef1[src[c]];
        k2[c] = coef2[src[c]];
        delta[c] = (int16_t)adpcm_rd16(src + ch + 2u * c);
        s1[c] = (int16_t)adpcm_rd16(src + 3u * ch + 2u * c);
        s2[c] = (int16_t)adpcm_rd16(src + 5u * ch + 2u * c);
        adpcm_put(dst, 0u, ch, c, s2[c]);
        adpcm_put(dst, 1u, ch, c, s1[c]);
    }
    while (frame < total)
    {
        uint8_t byte = src[pos + (turn >> 1)];
        uint32_t nib = ((turn & 1u) != 0u) ? ((uint32_t)byte & 0x0Fu)
                                           : ((uint32_t)byte >> 4);
        int32_t pred;

        c = turn % ch;
        pred = (s1[c] * k1[c] + s2[c] * k2[c]) / 256;
        pred += (((nib & 8u) != 0u) ? (int32_t)nib - 16 : (int32_t)nib) *
                delta[c];
        if (pred < -32768) { pred = -32768; }
        if (pred > 32767)  { pred = 32767; }
        s2[c] = s1[c];
        s1[c] = pred;
        /* the step comes from the file: keep the products inside 32 bits */
        delta[c] = (int32_t)(((int64_t)adapt[nib] * delta[c]) / 256);
        if (delta[c] < 16)      { delta[c] = 16; }
        if (delta[c] > INT32_MAX / 768) { delta[c] = INT32_MAX / 768; }
        adpcm_put(dst, frame, ch, c, pred);
        turn++;
        if (c + 1u == ch)
        {
            frame++;
        }
    }
    return FM_TRUE;
}

#endif /* FM_ADPCM_H */
