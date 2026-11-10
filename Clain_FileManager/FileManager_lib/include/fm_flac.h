#ifndef FM_FLAC_H
#define FM_FLAC_H
#include "fm_types.h"
#include "fm_audio.h"
FM_BEGIN_DECLS
FM_Result FM_Flac_Init(void);
const FM_AudioDecoderOps *FM_Flac_GetOps(void);
FM_END_DECLS
#endif
