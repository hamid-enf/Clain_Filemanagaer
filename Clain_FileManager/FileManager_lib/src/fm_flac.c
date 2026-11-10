/*
 * FileManager - FLAC decoder via dr_flac (single header)
 * FM_USE_FLAC must be 1. Auto-registers at startup via FM_Flac_Init().
 */
#define FM_DEBUG_TAG "flac"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_FLAC

#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac/dr_flac.h"

typedef struct {
    FM_AudioInfo info;
} FlacCtx;

static FM_Result flac_init(void **ctx, const FM_AudioInfo *info)
{
    FlacCtx *c;
    if (ctx == NULL || info == NULL) return FM_ERR_INVALID_ARGUMENT;
    c = (FlacCtx *)FM_Alloc(sizeof(FlacCtx));
    if (c == NULL) return FM_ERR_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->info = *info;
    *ctx = c;
    return FM_OK;
}

static FM_Result flac_decode(void *ctx, const uint8_t *in, size_t in_size,
                             int16_t *out, size_t out_count, size_t *out_samples)
{
    FlacCtx *c = (FlacCtx *)ctx;
    drflac *p = NULL;
    drflac_uint64 frames_read = 0;

    if (out == NULL || out_samples == NULL) return FM_ERR_INVALID_ARGUMENT;
    *out_samples = 0;
    if (in == NULL || in_size < 42u) return FM_ERR_CORRUPTED;
    if (out_count == 0u) return FM_ERR_TOO_LARGE;

    p = drflac_open_memory(in, in_size, NULL);
    if (p == NULL) return FM_ERR_CORRUPTED;

    if (c != NULL && c->info.channels != 0 && p->channels != c->info.channels) {
        FM_ERR_LOG("flac: channel mismatch info %u vs stream %u", (unsigned)c->info.channels, (unsigned)p->channels);
    }

    {
        unsigned ch = p->channels ? p->channels : 1;
        drflac_uint64 max_frames = out_count / ch;
        if (max_frames == 0) {
            drflac_close(p);
            return FM_ERR_TOO_LARGE;
        }
        frames_read = drflac_read_pcm_frames_s16(p, max_frames, out);
        *out_samples = (size_t)(frames_read * ch);
    }
    drflac_close(p);
    if (*out_samples == 0) return FM_ERR_EOF;
    return FM_OK;
}

static void flac_close(void *ctx)
{
    if (ctx != NULL) FM_Free(ctx);
}

static const FM_AudioDecoderOps g_flac_ops = {
    "FLAC",
    flac_init,
    flac_decode,
    flac_close
};

FM_Result FM_Flac_Init(void)
{
    return Audio_RegisterDecoder("FLAC", &g_flac_ops);
}

const FM_AudioDecoderOps *FM_Flac_GetOps(void)
{
    return &g_flac_ops;
}

#else
FM_Result FM_Flac_Init(void) { return FM_ERR_UNSUPPORTED; }
const FM_AudioDecoderOps *FM_Flac_GetOps(void) { return NULL; }
#endif
