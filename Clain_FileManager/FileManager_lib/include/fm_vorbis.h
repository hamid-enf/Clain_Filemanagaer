#ifndef FM_VORBIS_H
#define FM_VORBIS_H
#include "fm_types.h"
#include "fm_audio.h"
FM_BEGIN_DECLS
FM_Result FM_Vorbis_Init(void);
const FM_AudioDecoderOps *FM_Vorbis_GetOps(void);
FM_END_DECLS
#endif
