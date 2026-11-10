/*
 * FileManager - Vorbis decoder via stb_vorbis (single file)
 * FM_USE_VORBIS must be 1. Auto-registers via FM_Vorbis_Init().
 */
#define FM_DEBUG_TAG "vorbis"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_VORBIS

/* stb_vorbis is a single-file library: the implementation normally gets
 * compiled wherever the .c is included with STB_VORBIS_IMPLEMENTATION.
 * Here the *only* implementation translation unit is the vendored
 * third_party/stb/stb_vorbis.c itself, which build systems compile as a
 * standalone object (STM32CubeIDE compiles every .c under the project;
 * the host Makefile and CMake list it in OBJS too).  Including it here
 * in header-only mode gives us the public API declarations without any
 * definitions -- compiling the implementation in both places yields
 * "multiple definition of `stb_vorbis_*'" link errors. */
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb/stb_vorbis.c"

typedef struct {
    FM_AudioInfo info;
} VorbisCtx;

static FM_Result fm_vorbis_do_init(void **ctx, const FM_AudioInfo *info)
{
    VorbisCtx *c;
    if (ctx == NULL || info == NULL) return FM_ERR_INVALID_ARGUMENT;
    c = (VorbisCtx *)FM_Alloc(sizeof(VorbisCtx));
    if (c == NULL) return FM_ERR_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->info = *info;
    *ctx = c;
    return FM_OK;
}

static FM_Result fm_vorbis_do_decode(void *ctx, const uint8_t *in, size_t in_size,
                               int16_t *out, size_t out_count, size_t *out_samples)
{
    VorbisCtx *c = (VorbisCtx *)ctx;
    int error = 0;
    stb_vorbis *v = NULL;
    int channels = 0, sample_rate = 0;
    int frames = 0;
    (void)c;

    if (out == NULL || out_samples == NULL) return FM_ERR_INVALID_ARGUMENT;
    *out_samples = 0;
    if (in == NULL || in_size < 32u) return FM_ERR_CORRUPTED;
    if (out_count == 0u) return FM_ERR_TOO_LARGE;

    v = stb_vorbis_open_memory(in, (int)in_size, &error, NULL);
    if (v == NULL) return FM_ERR_CORRUPTED;

    {
        stb_vorbis_info info = stb_vorbis_get_info(v);
        channels = info.channels;
        sample_rate = info.sample_rate;
        (void)sample_rate;
        if (channels <= 0 || channels > 2) {
            stb_vorbis_close(v);
            return FM_ERR_UNSUPPORTED;
        }
        int max_frames = (int)(out_count / (size_t)channels);
        if (max_frames <= 0) {
            stb_vorbis_close(v);
            return FM_ERR_TOO_LARGE;
        }
        frames = stb_vorbis_get_samples_short_interleaved(v, channels, out, max_frames);
        if (frames < 0) frames = 0;
        *out_samples = (size_t)(frames * channels);
        stb_vorbis_close(v);
        if (*out_samples == 0) return FM_ERR_EOF;
        return FM_OK;
    }
}

static void fm_vorbis_do_close(void *ctx)
{
    if (ctx != NULL) FM_Free(ctx);
}

static const FM_AudioDecoderOps g_vorbis_ops = {
    "VORBIS",
    fm_vorbis_do_init,
    fm_vorbis_do_decode,
    fm_vorbis_do_close
};

FM_Result FM_Vorbis_Init(void)
{
    return Audio_RegisterDecoder("VORBIS", &g_vorbis_ops);
}

const FM_AudioDecoderOps *FM_Vorbis_GetOps(void)
{
    return &g_vorbis_ops;
}

#else
FM_Result FM_Vorbis_Init(void) { return FM_ERR_UNSUPPORTED; }
const FM_AudioDecoderOps *FM_Vorbis_GetOps(void) { return NULL; }
#endif
