/*
 * FileManager - Opus decoder
 * FM_USE_OPUS must be 1. Auto-registers via FM_Opus_Init().
 *
 * Two implementations behind one FM_AudioDecoderOps vtable:
 *
 *  1) FM_OPUS_USE_LIBOPUS=1 (default on-target: CubeIDE compiles the
 *     vendored libopus tree under third_party/opus for us).  Real decode
 *     via opus_decode() at 48 kHz, including PLC (packet loss
 *     concealment) when a packet is missing.  The caller (Audio/Video)
 *     resamples if the track rate differs; this layer always outputs
 *     the native 48 kHz frames.
 *
 *  2) Otherwise (host unit tests, tier-1 ports): a tiny dependency-free
 *     fallback synthesises silence with the exact RFC 6716 frame
 *     duration derived from the TOC byte.  Audio_GetDecoder("OPUS")
 *     still succeeds and Video_ReadAudioFrame still returns valid PCM
 *     (silence) instead of feeding raw compressed bytes to I2S as noise.
 *
 * Memory (FM_OPUS_USE_LIBOPUS): one OpusDecoder state is heap allocated
 * through the library allocator (opus uses malloc internally; route it
 * to SDRAM via FM_Allocator_Set / your _sbrk if the default newlib heap
 * is small).  opus_decoder_get_size(2) is ~20 KiB on the vendored build.
 */

#define FM_DEBUG_TAG "opus"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_alloc.h"
#include <string.h>
#include <stdint.h>

#if FM_USE_OPUS

#if FM_OPUS_USE_LIBOPUS
/* Relative include: no extra -I flag needed in any build system
 * (same convention as the dr_flac/stb includes in fm_flac/fm_vorbis). */
#include "../third_party/opus/include/opus.h"
#endif

/* ------------------------------------------------------------------ */
/* Common context                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    FM_AudioInfo info;         /* sanitized stream info (ch, rate)   */
    uint8_t      toc;          /* last packet TOC, for debug         */
#if FM_OPUS_USE_LIBOPUS
    OpusDecoder *dec;          /* 48 kHz libopus decoder             */
#endif
} OpusCtx;

/* ------------------------------------------------------------------ */
/* Opus packet duration (RFC 6716 section 3.1)                        */
/*                                                                    */
/* TOC byte: configuration = toc>>3 (codec + audio bandwidth + frame  */
/*           duration), code = toc&3 (frame count / framing mode).    */
/* Duration index inside each codec band:                             */
/*   SILK   (cfg 0..11): cfg&3        -> 10/20/40/60 ms               */
/*   Hybrid (cfg 12..15): cfg&1       -> 10/20 ms                     */
/*   CELT   (cfg 16..31): (cfg-16)&3  -> 2.5/5/10/20 ms               */
/* Returns samples per channel at 48 kHz for the whole packet.        */
/* ------------------------------------------------------------------ */
static size_t opus_packet_samples_48k(const uint8_t *data, size_t len)
{
    static const uint16_t silk_48[4] = { 480, 960, 1920, 2880 };   /* 10..60ms  */
    static const uint16_t hybr_48[2] = { 480, 960 };               /* 10/20ms   */
    static const uint16_t celt_48[4] = { 120, 240, 480, 960 };     /* 2.5..20ms */
    size_t per_frame;
    size_t frames;
    unsigned config;
    unsigned code;

    if (data == NULL || len == 0) return 960; /* 20 ms default */

    config = (unsigned)(data[0] >> 3) & 0x1Fu;
    code   = (unsigned)(data[0]) & 0x3u;

    if (config < 12u) {                 /* SILK NB/MB/WB                */
        per_frame = silk_48[config & 3u];
    } else if (config < 16u) {          /* Hybrid SWB/FB                */
        per_frame = hybr_48[config & 1u];
    } else {                            /* CELT NB/WB/SWB/FB            */
        per_frame = celt_48[(config - 16u) & 3u];
    }

    switch (code) {
    case 0:  frames = 1u; break;
    case 1:  frames = 2u; break;
    case 2:  frames = 2u; break;
    default: /* code 3: frame count is bits 0..5 of the second byte   */
        frames = (len >= 2u) ? (size_t)(data[1] & 0x3Fu) : 1u;
        if (frames == 0u) frames = 1u;
        break;
    }

    return per_frame * frames;
}

/* ------------------------------------------------------------------ */
/* Init / decode / close                                              */
/* ------------------------------------------------------------------ */
static FM_Result fm_opus_do_init(void **ctx, const FM_AudioInfo *info)
{
    OpusCtx *c;

    if (ctx == NULL || info == NULL) return FM_ERR_INVALID_ARGUMENT;
    c = (OpusCtx *)FM_Alloc(sizeof(OpusCtx));
    if (c == NULL) return FM_ERR_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->info = *info;
    /* Sanitize channels: Opus supports 1..255 but our I2S path is 1..2 */
    if (c->info.channels == 0) c->info.channels = 1;
    if (c->info.channels > 2) c->info.channels = 2;
    if (c->info.sample_rate == 0) c->info.sample_rate = 48000;

#if FM_OPUS_USE_LIBOPUS
    {
        int err = OPUS_INVALID_STATE;
        /* Opus always decodes to 48 kHz internally (input_sample_rate
         * is only a hint for the encoder API family we do not use). */
        c->dec = opus_decoder_create(48000, (opus_int32)c->info.channels, &err);
        if (c->dec == NULL || err != OPUS_OK) {
            FM_Free(c);
            FM_ERR_LOG("opus_decoder_create failed: %s",
                       err != OPUS_OK ? opus_strerror(err) : "null state");
            return FM_ERR_NO_MEMORY;
        }
    }
#endif

    *ctx = c;
    return FM_OK;
}

static FM_Result fm_opus_do_decode(void *ctx, const uint8_t *in, size_t in_size,
                                   int16_t *out, size_t out_count, size_t *out_samples)
{
    OpusCtx *c = (OpusCtx *)ctx;
    int      channels;
    size_t   need;

    if (out == NULL || out_samples == NULL) return FM_ERR_INVALID_ARGUMENT;
    *out_samples = 0;
    if (c == NULL) return FM_ERR_INVALID_ARGUMENT;
    channels = (int)c->info.channels;
    if (channels <= 0) channels = 1;
    if (channels > 2) channels = 2;

#if FM_OPUS_USE_LIBOPUS
    {
        opus_int32 frame_capacity = (opus_int32)(out_count / (size_t)channels);
        int n;

        if (frame_capacity <= 0) return FM_ERR_TOO_LARGE;
        if (in_size > (size_t)0x7FFFFFFFu) return FM_ERR_TOO_LARGE;

        /* in == NULL / in_size == 0 asks the decoder for PLC: libopus
         * then synthesises a concealed frame instead of hard silence. */
        n = opus_decode(c->dec, in, (opus_int32)in_size,
                        out, frame_capacity, 0);
        if (n < 0) {
            FM_ERR_LOG("opus_decode: %s", opus_strerror(n));
            return (n == OPUS_BUFFER_TOO_SMALL) ? FM_ERR_TOO_LARGE
                                                : FM_ERR_CORRUPTED;
        }
        if (in != NULL && in_size > 0u) c->toc = in[0];
        need = (size_t)n * (size_t)channels;   /* n <= frame_capacity */
        *out_samples = need;
        return FM_OK;
    }
#else
    /* -------- silence fallback (host tests / tier-1 ports) -------- */
    {
        size_t frame48;
        int    target_rate;
        size_t i;

        if (in == NULL || in_size == 0) {
            frame48 = 960; /* PLC: 20 ms */
        } else {
            frame48 = opus_packet_samples_48k(in, in_size);
            /* Clamp: Opus allows 120..5760 at 48k per packet group. */
            if (frame48 < 120u) frame48 = 120u;
            if (frame48 > 5760u) frame48 = 5760u;
            /* If the track is played at another rate, scale the sample
             * count so durations stay correct without a resampler. */
            target_rate = (int)c->info.sample_rate;
            if (target_rate != 48000 && target_rate > 0) {
                frame48 = (size_t)(((int64_t)frame48 * target_rate) / 48000);
                if (frame48 < 1u) frame48 = 1u;
            }
        }
        need = frame48 * (size_t)channels;
        if (need > out_count) return FM_ERR_TOO_LARGE;
        if (need == 0u) return FM_ERR_EOF;
        for (i = 0; i < need; i++) out[i] = 0;
        if (in != NULL && in_size > 0u) c->toc = in[0];
        *out_samples = need;
        return FM_OK;
    }
#endif
}

static void fm_opus_do_close(void *ctx)
{
    if (ctx == NULL) return;
#if FM_OPUS_USE_LIBOPUS
    {
        OpusCtx *c = (OpusCtx *)ctx;
        if (c->dec != NULL) opus_decoder_destroy(c->dec);
    }
#endif
    FM_Free(ctx);
}

static const FM_AudioDecoderOps g_opus_ops = {
    "OPUS",
    fm_opus_do_init,
    fm_opus_do_decode,
    fm_opus_do_close
};

const FM_AudioDecoderOps *FM_Opus_GetOps(void)
{
    return &g_opus_ops;
}

FM_Result FM_Opus_Init(void)
{
    /* Register "OPUS" so Audio_GetDecoder("OPUS") and Video_ReadAudioFrame
     * succeed without the caller having to provide an external decoder. */
    FM_Result r = Audio_RegisterDecoder("OPUS", &g_opus_ops);
    if (r != FM_OK && r != FM_ERR_EXISTS) return r;
    return FM_OK;
}

#else /* FM_USE_OPUS == 0 */
const FM_AudioDecoderOps *FM_Opus_GetOps(void) { return NULL; }
FM_Result FM_Opus_Init(void) { return FM_ERR_UNSUPPORTED; }
#endif
