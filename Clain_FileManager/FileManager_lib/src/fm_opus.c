/*
 * FileManager - Opus decoder
 * FM_USE_OPUS must be 1. Auto-registers via FM_Opus_Init().
 *
 * Real decode needs the vendored libopus (third_party/opus):
 *
 *  - FM_OPUS_USE_LIBOPUS=1 (default on-target: CubeIDE compiles the
 *    vendored tree for us; host: `make FM_USE_OPUS_LIB=1`).  Decode via
 *    opus_decode() at 48 kHz, including PLC (packet loss concealment)
 *    when a packet is missing.  The caller (Audio/Video) resamples if
 *    the track rate differs; this layer always outputs native 48 kHz.
 *
 *  - FM_OPUS_USE_LIBOPUS=0: there is NO decoder.  FM_Opus_GetOps()
 *    returns NULL and FM_Opus_Init() returns FM_ERR_UNSUPPORTED, so
 *    Audio_Open() on an Opus file and Video_ReadAudioFrame() on an Opus
 *    track fail loudly with FM_ERR_UNSUPPORTED.  (An earlier revision
 *    shipped a "silence fallback" here that returned FM_OK with all-zero
 *    PCM: playback looked successful and was mute.  It also kept a
 *    decoder table in ROM for a codec that could not decode.)
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

#if FM_USE_AUDIO && FM_USE_OPUS && FM_OPUS_USE_LIBOPUS

/* Relative include: no extra -I flag needed in any build system
 * (same convention as the dr_flac/stb includes in fm_flac/fm_vorbis). */
#include "../third_party/opus/include/opus.h"

typedef struct {
    FM_AudioInfo info;         /* sanitized stream info (ch, rate)   */
    uint8_t      toc;          /* last packet TOC, for debug         */
    OpusDecoder *dec;          /* 48 kHz libopus decoder             */
} OpusCtx;

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
}

static void fm_opus_do_close(void *ctx)
{
    if (ctx == NULL) return;
    {
        OpusCtx *c = (OpusCtx *)ctx;
        if (c->dec != NULL) opus_decoder_destroy(c->dec);
    }
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

#else /* no audio, no Opus, or no libopus: no decoder and no static state */
const FM_AudioDecoderOps *FM_Opus_GetOps(void) { return NULL; }
FM_Result FM_Opus_Init(void) { return FM_ERR_UNSUPPORTED; }
#endif
