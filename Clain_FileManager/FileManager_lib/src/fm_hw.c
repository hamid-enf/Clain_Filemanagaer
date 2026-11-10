/*
 * FileManager - Optional hardware decoder registry (see fm_hw.h)
 */

#define FM_DEBUG_TAG "hw"
#include "fm_debug.h"
#include "fm_hw.h"
#include "fm_internal.h"

#if FM_USE_VIDEO

static const FM_VideoCodecOps *g_hw[FM_VIDEO_CODEC_MPEG4 + 1];

static FM_Bool codec_valid(FM_VideoCodec codec)
{
    return (codec >= FM_VIDEO_CODEC_RAW && codec <= FM_VIDEO_CODEC_MPEG4);
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
    return codec_valid(codec) ? g_hw[codec] : NULL;
}

#endif /* FM_USE_VIDEO */
