/*
 * FileManager - Optional hardware decoder registry (see fm_hw.h)
 */

#define FM_DEBUG_TAG "hw"
#include "fm_debug.h"
#include "fm_hw.h"
#include "fm_internal.h"

#if FM_USE_VIDEO

/* One slot per codec id.  The table used to stop at MPEG4, so HEVC, VP8,
 * VP9, AV1 and Theora -- exactly the codecs that have NO software
 * decoder and therefore need a hardware one -- could not be registered. */
static const FM_VideoCodecOps *g_hw[FM_VIDEO_CODEC_COUNT];

static FM_Bool codec_valid(FM_VideoCodec codec)
{
    return ((int)codec > (int)FM_VIDEO_CODEC_UNKNOWN &&
            (int)codec < (int)FM_VIDEO_CODEC_COUNT) ? FM_TRUE : FM_FALSE;
}

FM_Result HWDecoder_Register(FM_VideoCodec codec, const FM_VideoCodecOps *ops)
{
    if (ops == NULL || ops->decode_frame == NULL || !codec_valid(codec))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    g_hw[codec] = ops;
    FM_Unlock();
    return FM_OK;
}

FM_Result HWDecoder_Unregister(FM_VideoCodec codec)
{
    if (!codec_valid(codec))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    if (g_hw[codec] == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_FOUND;
    }
    g_hw[codec] = NULL;
    FM_Unlock();
    return FM_OK;
}

FM_Bool HWDecoder_Has(FM_VideoCodec codec)
{
    return (codec_valid(codec) && g_hw[codec] != NULL) ? FM_TRUE : FM_FALSE;
}

const FM_VideoCodecOps *HWDecoder_Get(FM_VideoCodec codec)
{
    /* Single pointer load: no lock, so it is safe from the decode path
     * whatever the caller already holds. */
    return codec_valid(codec) ? g_hw[codec] : NULL;
}

#else /* !FM_USE_VIDEO */

/* Video is compiled out: keep the API linkable (application code that
 * registers a hardware decoder unconditionally must still build), with
 * no table and no state behind it. */
FM_Result HWDecoder_Register(FM_VideoCodec codec, const FM_VideoCodecOps *ops)
{
    (void)codec; (void)ops;
    return FM_ERR_UNSUPPORTED;
}

FM_Result HWDecoder_Unregister(FM_VideoCodec codec)
{
    (void)codec;
    return FM_ERR_UNSUPPORTED;
}

FM_Bool HWDecoder_Has(FM_VideoCodec codec)
{
    (void)codec;
    return FM_FALSE;
}

const FM_VideoCodecOps *HWDecoder_Get(FM_VideoCodec codec)
{
    (void)codec;
    return NULL;
}

#endif /* FM_USE_VIDEO */
