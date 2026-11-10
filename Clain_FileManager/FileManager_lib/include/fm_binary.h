/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_binary.h - Typed binary read/write with endian control
 *
 * Operates on a stream (file or memory).  All multi-byte types support
 * explicit little/big endian selection.
 */

#ifndef FM_BINARY_H
#define FM_BINARY_H

#include "fm_types.h"
#include "fm_stream.h"

FM_BEGIN_DECLS

/* Raw */
FM_Result Binary_Read(FM_Stream *s, void *buf, size_t len, size_t *read);
FM_Result Binary_Write(FM_Stream *s, const void *buf, size_t len,
                       size_t *written);

/* Unsigned */
FM_Result Binary_ReadU8(FM_Stream *s, uint8_t *v);
FM_Result Binary_ReadU16(FM_Stream *s, uint16_t *v, FM_Endian e);
FM_Result Binary_ReadU32(FM_Stream *s, uint32_t *v, FM_Endian e);
FM_Result Binary_ReadU64(FM_Stream *s, uint64_t *v, FM_Endian e);

FM_Result Binary_WriteU8(FM_Stream *s, uint8_t v);
FM_Result Binary_WriteU16(FM_Stream *s, uint16_t v, FM_Endian e);
FM_Result Binary_WriteU32(FM_Stream *s, uint32_t v, FM_Endian e);
FM_Result Binary_WriteU64(FM_Stream *s, uint64_t v, FM_Endian e);

/* Signed (bit-compatible with unsigned) */
FM_Result Binary_ReadI8(FM_Stream *s, int8_t *v);
FM_Result Binary_ReadI16(FM_Stream *s, int16_t *v, FM_Endian e);
FM_Result Binary_ReadI32(FM_Stream *s, int32_t *v, FM_Endian e);
FM_Result Binary_ReadI64(FM_Stream *s, int64_t *v, FM_Endian e);

FM_Result Binary_WriteI8(FM_Stream *s, int8_t v);
FM_Result Binary_WriteI16(FM_Stream *s, int16_t v, FM_Endian e);
FM_Result Binary_WriteI32(FM_Stream *s, int32_t v, FM_Endian e);
FM_Result Binary_WriteI64(FM_Stream *s, int64_t v, FM_Endian e);

/* Float / double (IEEE-754 bit patterns, endian controlled) */
FM_Result Binary_ReadFloat(FM_Stream *s, float *v, FM_Endian e);
FM_Result Binary_ReadDouble(FM_Stream *s, double *v, FM_Endian e);
FM_Result Binary_WriteFloat(FM_Stream *s, float v, FM_Endian e);
FM_Result Binary_WriteDouble(FM_Stream *s, double v, FM_Endian e);

/* Convenience aliases */
#define Binary_ReadLE16(s, v)  Binary_ReadU16((s), (v), FM_ENDIAN_LITTLE)
#define Binary_ReadLE32(s, v)  Binary_ReadU32((s), (v), FM_ENDIAN_LITTLE)
#define Binary_ReadLE64(s, v)  Binary_ReadU64((s), (v), FM_ENDIAN_LITTLE)
#define Binary_ReadBE16(s, v)  Binary_ReadU16((s), (v), FM_ENDIAN_BIG)
#define Binary_ReadBE32(s, v)  Binary_ReadU32((s), (v), FM_ENDIAN_BIG)
#define Binary_ReadBE64(s, v)  Binary_ReadU64((s), (v), FM_ENDIAN_BIG)
#define Binary_WriteLE16(s, v) Binary_WriteU16((s), (v), FM_ENDIAN_LITTLE)
#define Binary_WriteLE32(s, v) Binary_WriteU32((s), (v), FM_ENDIAN_LITTLE)
#define Binary_WriteLE64(s, v) Binary_WriteU64((s), (v), FM_ENDIAN_LITTLE)
#define Binary_WriteBE16(s, v) Binary_WriteU16((s), (v), FM_ENDIAN_BIG)
#define Binary_WriteBE32(s, v) Binary_WriteU32((s), (v), FM_ENDIAN_BIG)
#define Binary_WriteBE64(s, v) Binary_WriteU64((s), (v), FM_ENDIAN_BIG)

FM_END_DECLS

#endif /* FM_BINARY_H */
