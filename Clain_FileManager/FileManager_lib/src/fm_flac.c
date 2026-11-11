/*
 * FileManager - FLAC decoder via dr_flac (single header)
 * FM_USE_FLAC must be 1. Auto-registers at startup via FM_Flac_Init().
 */
#define FM_DEBUG_TAG "flac"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_flac.h"
#include "fm_alloc.h"
#include <string.h>

uint32_t FM_Flac_Bits(const uint8_t *data, size_t size)
{
    /* "fLaC", a 4-byte block header (type 0 = STREAMINFO), then 34 bytes:
     * block sizes (4), frame sizes (6), 20 bits rate, 3 bits channels - 1,
     * 5 bits width - 1, ... */
    if (data == NULL || size < 22u || memcmp(data, "fLaC", 4) != 0 ||
        (data[4] & 0x7Fu) != 0u)
    {
        return 0u;
    }
    return ((((uint32_t)data[20] & 1u) << 4) | ((uint32_t)data[21] >> 4)) + 1u;
}

#if FM_USE_AUDIO && FM_USE_FLAC

/* dr_flac lives in this file only: no file I/O of its own (the library
 * reads through its backends), and every function static, so nothing
 * named drflac_* is exported (an application that links dr_flac itself
 * used to get duplicate symbols). */
#define DR_FLAC_NO_STDIO
#define DRFLAC_API static
#define DR_FLAC_IMPLEMENTATION
/* Static means the part of the dr_flac API this file does not call is
 * "defined but not used".  Silenced here, around the include only, so
 * that it does not depend on a per-file compiler flag: STM32CubeIDE and
 * tools/check_ports.sh compile this file with the project-wide flags. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/dr_flac/dr_flac.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* dr_flac's own memory comes from the library allocator (FM_Allocator_Set).
 * No realloc hook: dr_flac then grows a block by allocate + copy + free,
 * which works with an allocator that has no realloc. */
static void *fm_flac_malloc(size_t size, void *user)
{
    (void)user;
    return FM_Alloc(size);
}

static void fm_flac_free(void *p, void *user)
{
    (void)user;
    FM_Free(p);
}

static const drflac_allocation_callbacks g_fm_flac_alloc = {
    NULL, fm_flac_malloc, NULL, fm_flac_free
};

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

    if (FM_Flac_Bits(in, in_size) > FM_FLAC_MAX_BITS)
    {
        return FM_ERR_UNSUPPORTED;       /* valid, wider than dr_flac does */
    }
    p = drflac_open_memory(in, in_size, &g_fm_flac_alloc);
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

void *FM_Flac_OpenMemory(const uint8_t *data, size_t size,
                         uint32_t *sample_rate, uint32_t *channels,
                         uint64_t *total_frames)
{
    drflac *p = drflac_open_memory(data, size, &g_fm_flac_alloc);

    if (p != NULL)
    {
        if (sample_rate != NULL)  { *sample_rate = p->sampleRate; }
        if (channels != NULL)     { *channels = p->channels; }
        if (total_frames != NULL) { *total_frames = p->totalPCMFrameCount; }
    }
    return p;
}

uint64_t FM_Flac_ReadS16(void *flac, uint64_t frames, int16_t *out)
{
    return (flac != NULL)
               ? drflac_read_pcm_frames_s16((drflac *)flac, frames, out) : 0u;
}

void FM_Flac_Close(void *flac)
{
    if (flac != NULL)
    {
        drflac_close((drflac *)flac);
    }
}

const FM_AudioDecoderOps *FM_Flac_GetOps(void)
{
    return &g_flac_ops;
}

#else
FM_Result FM_Flac_Init(void) { return FM_ERR_UNSUPPORTED; }
const FM_AudioDecoderOps *FM_Flac_GetOps(void) { return NULL; }
void *FM_Flac_OpenMemory(const uint8_t *data, size_t size,
                         uint32_t *sample_rate, uint32_t *channels,
                         uint64_t *total_frames)
{
    (void)data; (void)size; (void)sample_rate; (void)channels;
    (void)total_frames;
    return NULL;
}
uint64_t FM_Flac_ReadS16(void *flac, uint64_t frames, int16_t *out)
{
    (void)flac; (void)frames; (void)out;
    return 0u;
}
void FM_Flac_Close(void *flac) { (void)flac; }
#endif
