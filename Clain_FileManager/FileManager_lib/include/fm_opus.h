#ifndef FM_OPUS_H
#define FM_OPUS_H
#include "fm_types.h"
#include "fm_audio.h"
FM_BEGIN_DECLS
FM_Result FM_Opus_Init(void);
const FM_AudioDecoderOps *FM_Opus_GetOps(void);
FM_END_DECLS
#endif
