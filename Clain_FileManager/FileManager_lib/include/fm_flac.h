#ifndef FM_FLAC_H
#define FM_FLAC_H
#include "fm_types.h"
#include "fm_audio.h"
FM_BEGIN_DECLS
FM_Result FM_Flac_Init(void);
const FM_AudioDecoderOps *FM_Flac_GetOps(void);

/* The widest sample the bundled decoder (dr_flac) does.  FLAC allows 32
 * (RFC 9639); such a file is FM_ERR_UNSUPPORTED, not "damaged". */
#define FM_FLAC_MAX_BITS 24u

/* Used by the library itself (Audio_Open of a .flac file): dr_flac is
 * compiled into fm_flac.c with every one of its functions static, so the
 * application can link its own copy of dr_flac next to this library.
 *
 * FM_Flac_OpenMemory: NULL when the block is not a FLAC stream.  `data`
 * must stay valid until FM_Flac_Close.  total_frames 0 = not declared. */
void *FM_Flac_OpenMemory(const uint8_t *data, size_t size,
                         uint32_t *sample_rate, uint32_t *channels,
                         uint64_t *total_frames);
/* Interleaved frames, returns how many were written (0 = the end). */
uint64_t FM_Flac_ReadS16(void *flac, uint64_t frames, int16_t *out);
void FM_Flac_Close(void *flac);
/* Bits per sample of a stream that starts with "fLaC" + STREAMINFO;
 * 0 when that cannot be told from the first `size` bytes. */
uint32_t FM_Flac_Bits(const uint8_t *data, size_t size);
FM_END_DECLS
#endif
