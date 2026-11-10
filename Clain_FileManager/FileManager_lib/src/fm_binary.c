/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_binary.c - Typed binary read/write with endian control
 */

#define FM_DEBUG_TAG "binary"
#include "fm_debug.h"
#include "fm_binary.h"
#include <string.h>

#if FM_USE_BINARY && FM_USE_STREAM

FM_Result Binary_Read(FM_Stream *s, void *buf, size_t len, size_t *read)
{
    return Stream_Read(s, buf, len, read);
}

FM_Result Binary_Write(FM_Stream *s, const void *buf, size_t len,
                       size_t *written)
{
    return Stream_Write(s, buf, len, written);
}

FM_Result Binary_ReadU8(FM_Stream *s, uint8_t *v)
{
    size_t rd = 0;
    FM_Result r = Stream_Read(s, v, 1, &rd);

    if (r == FM_OK && rd != 1)
    {
        return FM_ERR_EOF;
    }
    return r;
}

FM_Result Binary_ReadU16(FM_Stream *s, uint16_t *v, FM_Endian e)
{
    uint8_t b[2];
    size_t rd = 0;
    FM_Result r = Stream_Read(s, b, sizeof(b), &rd);

    if (r == FM_OK && rd != sizeof(b))
    {
        return FM_ERR_EOF;
    }
    if (r != FM_OK)
    {
        return r;
    }
    if (e == FM_ENDIAN_BIG)
    {
        *v = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    }
    else
    {
        *v = (uint16_t)(((uint16_t)b[1] << 8) | b[0]);
    }
    return FM_OK;
}

FM_Result Binary_ReadU32(FM_Stream *s, uint32_t *v, FM_Endian e)
{
    uint8_t b[4];
    size_t rd = 0;
    FM_Result r = Stream_Read(s, b, sizeof(b), &rd);

    if (r == FM_OK && rd != sizeof(b))
    {
        return FM_ERR_EOF;
    }
    if (r != FM_OK)
    {
        return r;
    }
    if (e == FM_ENDIAN_BIG)
    {
        *v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
             ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    }
    else
    {
        *v = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
             ((uint32_t)b[1] << 8) | (uint32_t)b[0];
    }
    return FM_OK;
}

FM_Result Binary_ReadU64(FM_Stream *s, uint64_t *v, FM_Endian e)
{
    uint8_t b[8];
    size_t rd = 0;
    FM_Result r = Stream_Read(s, b, sizeof(b), &rd);
    uint32_t hi;
    uint32_t lo;

    if (r == FM_OK && rd != sizeof(b))
    {
        return FM_ERR_EOF;
    }
    if (r != FM_OK)
    {
        return r;
    }
    if (e == FM_ENDIAN_BIG)
    {
        hi = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
             ((uint32_t)b[2] << 8) | (uint32_t)b[3];
        lo = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
             ((uint32_t)b[6] << 8) | (uint32_t)b[7];
    }
    else
    {
        lo = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
             ((uint32_t)b[1] << 8) | (uint32_t)b[0];
        hi = ((uint32_t)b[7] << 24) | ((uint32_t)b[6] << 16) |
             ((uint32_t)b[5] << 8) | (uint32_t)b[4];
    }
    *v = ((uint64_t)hi << 32) | lo;
    return FM_OK;
}

FM_Result Binary_WriteU8(FM_Stream *s, uint8_t v)
{
    size_t wr = 0;
    FM_Result r = Stream_Write(s, &v, 1, &wr);

    if (r == FM_OK && wr != 1)
    {
        return FM_ERR_IO;
    }
    return r;
}

FM_Result Binary_WriteU16(FM_Stream *s, uint16_t v, FM_Endian e)
{
    uint8_t b[2];

    if (e == FM_ENDIAN_BIG)
    {
        b[0] = (uint8_t)(v >> 8);
        b[1] = (uint8_t)(v & 0xFFu);
    }
    else
    {
        b[0] = (uint8_t)(v & 0xFFu);
        b[1] = (uint8_t)(v >> 8);
    }
    {
        size_t wr = 0;
        FM_Result r = Stream_Write(s, b, sizeof(b), &wr);

        if (r == FM_OK && wr != sizeof(b))
        {
            return FM_ERR_IO;
        }
        return r;
    }
}

FM_Result Binary_WriteU32(FM_Stream *s, uint32_t v, FM_Endian e)
{
    uint8_t b[4];

    if (e == FM_ENDIAN_BIG)
    {
        b[0] = (uint8_t)(v >> 24);
        b[1] = (uint8_t)((v >> 16) & 0xFFu);
        b[2] = (uint8_t)((v >> 8) & 0xFFu);
        b[3] = (uint8_t)(v & 0xFFu);
    }
    else
    {
        b[0] = (uint8_t)(v & 0xFFu);
        b[1] = (uint8_t)((v >> 8) & 0xFFu);
        b[2] = (uint8_t)((v >> 16) & 0xFFu);
        b[3] = (uint8_t)(v >> 24);
    }
    {
        size_t wr = 0;
        FM_Result r = Stream_Write(s, b, sizeof(b), &wr);

        if (r == FM_OK && wr != sizeof(b))
        {
            return FM_ERR_IO;
        }
        return r;
    }
}

FM_Result Binary_WriteU64(FM_Stream *s, uint64_t v, FM_Endian e)
{
    uint8_t b[8];

    if (e == FM_ENDIAN_BIG)
    {
        b[0] = (uint8_t)(v >> 56);
        b[1] = (uint8_t)((v >> 48) & 0xFFu);
        b[2] = (uint8_t)((v >> 40) & 0xFFu);
        b[3] = (uint8_t)((v >> 32) & 0xFFu);
        b[4] = (uint8_t)((v >> 24) & 0xFFu);
        b[5] = (uint8_t)((v >> 16) & 0xFFu);
        b[6] = (uint8_t)((v >> 8) & 0xFFu);
        b[7] = (uint8_t)(v & 0xFFu);
    }
    else
    {
        b[0] = (uint8_t)(v & 0xFFu);
        b[1] = (uint8_t)((v >> 8) & 0xFFu);
        b[2] = (uint8_t)((v >> 16) & 0xFFu);
        b[3] = (uint8_t)((v >> 24) & 0xFFu);
        b[4] = (uint8_t)((v >> 32) & 0xFFu);
        b[5] = (uint8_t)((v >> 40) & 0xFFu);
        b[6] = (uint8_t)((v >> 48) & 0xFFu);
        b[7] = (uint8_t)(v >> 56);
    }
    {
        size_t wr = 0;
        FM_Result r = Stream_Write(s, b, sizeof(b), &wr);

        if (r == FM_OK && wr != sizeof(b))
        {
            return FM_ERR_IO;
        }
        return r;
    }
}

FM_Result Binary_ReadI8(FM_Stream *s, int8_t *v)
{
    uint8_t u;
    FM_Result r = Binary_ReadU8(s, &u);

    if (r == FM_OK)
    {
        *v = (int8_t)u;
    }
    return r;
}

FM_Result Binary_ReadI16(FM_Stream *s, int16_t *v, FM_Endian e)
{
    uint16_t u;
    FM_Result r = Binary_ReadU16(s, &u, e);

    if (r == FM_OK)
    {
        *v = (int16_t)u;
    }
    return r;
}

FM_Result Binary_ReadI32(FM_Stream *s, int32_t *v, FM_Endian e)
{
    uint32_t u;
    FM_Result r = Binary_ReadU32(s, &u, e);

    if (r == FM_OK)
    {
        *v = (int32_t)u;
    }
    return r;
}

FM_Result Binary_ReadI64(FM_Stream *s, int64_t *v, FM_Endian e)
{
    uint64_t u;
    FM_Result r = Binary_ReadU64(s, &u, e);

    if (r == FM_OK)
    {
        *v = (int64_t)u;
    }
    return r;
}

FM_Result Binary_WriteI8(FM_Stream *s, int8_t v)
{
    return Binary_WriteU8(s, (uint8_t)v);
}

FM_Result Binary_WriteI16(FM_Stream *s, int16_t v, FM_Endian e)
{
    return Binary_WriteU16(s, (uint16_t)v, e);
}

FM_Result Binary_WriteI32(FM_Stream *s, int32_t v, FM_Endian e)
{
    return Binary_WriteU32(s, (uint32_t)v, e);
}

FM_Result Binary_WriteI64(FM_Stream *s, int64_t v, FM_Endian e)
{
    return Binary_WriteU64(s, (uint64_t)v, e);
}

FM_Result Binary_ReadFloat(FM_Stream *s, float *v, FM_Endian e)
{
    uint32_t u;
    FM_Result r = Binary_ReadU32(s, &u, e);

    if (r == FM_OK)
    {
        memcpy(v, &u, sizeof(u));
    }
    return r;
}

FM_Result Binary_ReadDouble(FM_Stream *s, double *v, FM_Endian e)
{
    uint64_t u;
    FM_Result r = Binary_ReadU64(s, &u, e);

    if (r == FM_OK)
    {
        memcpy(v, &u, sizeof(u));
    }
    return r;
}

FM_Result Binary_WriteFloat(FM_Stream *s, float v, FM_Endian e)
{
    uint32_t u;

    memcpy(&u, &v, sizeof(v));
    return Binary_WriteU32(s, u, e);
}

FM_Result Binary_WriteDouble(FM_Stream *s, double v, FM_Endian e)
{
    uint64_t u;

    memcpy(&u, &v, sizeof(v));
    return Binary_WriteU64(s, u, e);
}

#endif /* FM_USE_BINARY && FM_USE_STREAM */
