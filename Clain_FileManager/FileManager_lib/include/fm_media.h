/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_media.h - Media detection (optional)
 *
 * Detects file type from extension first, then confirms/refines with
 * magic numbers read from the beginning of the file.
 */

#ifndef FM_MEDIA_H
#define FM_MEDIA_H

#include "fm_types.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_MEDIA_UNKNOWN = 0,
    FM_MEDIA_TEXT,
    FM_MEDIA_JSON,
    FM_MEDIA_CSV,
    FM_MEDIA_XML,
    FM_MEDIA_INI,
    FM_MEDIA_BINARY,
    FM_MEDIA_JPEG,
    FM_MEDIA_PNG,
    FM_MEDIA_GIF,
    FM_MEDIA_BMP,
    FM_MEDIA_TIFF,
    FM_MEDIA_WAV,
    FM_MEDIA_MP3,
    FM_MEDIA_AAC,
    FM_MEDIA_FLAC,
    FM_MEDIA_OGG,
    FM_MEDIA_AVI,
    FM_MEDIA_MP4,
    FM_MEDIA_WEBP,
    FM_MEDIA_RAW,
    FM_MEDIA_IMAGE_MAX
} FM_MediaType;

/* Detect from a path: reads FM_MEDIA_MAGIC_SCAN bytes, then reads behind
 * one leading ID3v2 tag if present. */
FM_MediaType File_DetectType(const char *path);

/* Detect from an in-memory buffer; ext may be NULL. For an incomplete
 * leading ID3v2 tag, return the extension type or FM_MEDIA_UNKNOWN; the
 * tag alone cannot establish which audio format follows it. */
FM_MediaType Media_DetectBuffer(const void *buf, size_t len,
                                const char *ext);

/* Ext -> type without touching the file. */
FM_MediaType Media_DetectByExtension(const char *path);

/* Save bytes that are already encoded/compressed (for example a complete MP3
 * received over a network). Magic bytes are checked against `expected` and
 * the destination extension. Use FM_MEDIA_UNKNOWN to accept the detected
 * type. This function does not encode raw PCM/pixels; use the corresponding
 * Audio/Image/Video writer for that job. */
FM_Result Media_SaveEncoded(const char *path, const void *encoded,
                            size_t encoded_size, FM_MediaType expected,
                            FM_MediaType *detected);

const char *Media_TypeName(FM_MediaType type);
const char *Media_MimeType(FM_MediaType type);

FM_END_DECLS

#endif /* FM_MEDIA_H */
