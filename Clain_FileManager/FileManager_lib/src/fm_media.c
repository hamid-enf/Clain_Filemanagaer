/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_media.c - Media detection (extension + magic numbers)
 */

#define FM_DEBUG_TAG "media"
#include "fm_debug.h"
#include "fm_media.h"
#include "fm_file.h"
#include "fm_path.h"
#include <string.h>

#if FM_USE_MEDIA_DETECT

static FM_MediaType media_by_ext(const char *path)
{
    char ext[16];

    if (Path_GetExtension(path, ext, sizeof(ext)) != FM_OK)
    {
        return FM_MEDIA_UNKNOWN;
    }
    if (strcmp(ext, "json") == 0)  return FM_MEDIA_JSON;
    if (strcmp(ext, "csv") == 0)   return FM_MEDIA_CSV;
    if (strcmp(ext, "xml") == 0)   return FM_MEDIA_XML;
    if (strcmp(ext, "ini") == 0)   return FM_MEDIA_INI;
    if (strcmp(ext, "txt") == 0)   return FM_MEDIA_TEXT;
    if (strcmp(ext, "log") == 0)   return FM_MEDIA_TEXT;
    if (strcmp(ext, "md") == 0)    return FM_MEDIA_TEXT;
    if (strcmp(ext, "jpg") == 0)   return FM_MEDIA_JPEG;
    if (strcmp(ext, "jpeg") == 0)  return FM_MEDIA_JPEG;
    if (strcmp(ext, "png") == 0)   return FM_MEDIA_PNG;
    if (strcmp(ext, "gif") == 0)   return FM_MEDIA_GIF;
    if (strcmp(ext, "bmp") == 0)   return FM_MEDIA_BMP;
    if (strcmp(ext, "tif") == 0)   return FM_MEDIA_TIFF;
    if (strcmp(ext, "tiff") == 0)  return FM_MEDIA_TIFF;
    if (strcmp(ext, "wav") == 0)   return FM_MEDIA_WAV;
    if (strcmp(ext, "mp3") == 0)   return FM_MEDIA_MP3;
    if (strcmp(ext, "aac") == 0)   return FM_MEDIA_AAC;
    if (strcmp(ext, "m4a") == 0)   return FM_MEDIA_AAC;
    if (strcmp(ext, "flac") == 0)  return FM_MEDIA_FLAC;
    if (strcmp(ext, "ogg") == 0)   return FM_MEDIA_OGG;
    if (strcmp(ext, "avi") == 0)   return FM_MEDIA_AVI;
    if (strcmp(ext, "mp4") == 0)   return FM_MEDIA_MP4;
    if (strcmp(ext, "m4v") == 0)   return FM_MEDIA_MP4;
    if (strcmp(ext, "mov") == 0)   return FM_MEDIA_MP4;
    if (strcmp(ext, "webp") == 0)  return FM_MEDIA_WEBP;
    if (strcmp(ext, "bin") == 0)   return FM_MEDIA_BINARY;
    if (strcmp(ext, "raw") == 0)   return FM_MEDIA_RAW;
    if (strcmp(ext, "fmrv") == 0)  return FM_MEDIA_RAW;
    return FM_MEDIA_UNKNOWN;
}

FM_MediaType Media_DetectByExtension(const char *path)
{
    if (path == NULL)
    {
        return FM_MEDIA_UNKNOWN;
    }
    return media_by_ext(path);
}

FM_MediaType Media_DetectBuffer(const void *buf, size_t len,
                                const char *ext)
{
    const uint8_t *p = (const uint8_t *)buf;
    FM_MediaType by_ext = FM_MEDIA_UNKNOWN;

    if (ext != NULL)
    {
        by_ext = media_by_ext(ext);
    }
    if (buf == NULL || len == 0)
    {
        return by_ext;
    }

    /* Magic numbers win over extension. */
    if (len >= 2 && p[0] == 0xFFu && p[1] == 0xD8u)
    {
        return FM_MEDIA_JPEG;
    }
    if (len >= 8 && p[0] == 0x89u && p[1] == 'P' && p[2] == 'N' &&
        p[3] == 'G' && p[4] == 0x0Du && p[5] == 0x0Au && p[6] == 0x1Au &&
        p[7] == 0x0Au)
    {
        return FM_MEDIA_PNG;
    }
    if (len >= 6 && (p[0] == 'G' && p[1] == 'I' && p[2] == 'F' &&
                     (p[3] == '8' && (p[4] == '7' || p[4] == '9') &&
                      p[5] == 'a')))
    {
        return FM_MEDIA_GIF;
    }
    if (len >= 2 && p[0] == 'B' && p[1] == 'M')
    {
        return FM_MEDIA_BMP;
    }
    if (len >= 4 && p[0] == 'I' && p[1] == 'I' && p[2] == 0x2Au &&
        p[3] == 0x00u)
    {
        return FM_MEDIA_TIFF;
    }
    if (len >= 4 && p[0] == 'M' && p[1] == 'M' && p[2] == 0x00u &&
        p[3] == 0x2Au)
    {
        return FM_MEDIA_TIFF;
    }
    if (len >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' &&
        p[3] == 'F' && p[8] == 'W' && p[9] == 'A' && p[10] == 'V' &&
        p[11] == 'E')
    {
        return FM_MEDIA_WAV;
    }
    if (len >= 3 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
    {
        return FM_MEDIA_MP3;
    }
    if (len >= 2 && p[0] == 0xFFu && (p[1] & 0xF6u) == 0xF0u)
    {
        return FM_MEDIA_AAC;   /* ADTS sync */
    }
    if (len >= 4 && p[0] == 0xFFu && (p[1] & 0xE0u) == 0xE0u &&
        (p[1] & 0x18u) != 0x08u && /* MPEG version is not reserved */
        (p[1] & 0x06u) != 0x00u && /* layer is not reserved */
        (p[2] & 0xF0u) != 0x00u && (p[2] & 0xF0u) != 0xF0u &&
        (p[2] & 0x0Cu) != 0x0Cu)
    {
        return FM_MEDIA_MP3;
    }
    if (len >= 4 && p[0] == 'f' && p[1] == 'L' && p[2] == 'a' &&
        p[3] == 'C')
    {
        return FM_MEDIA_FLAC;
    }
    if (len >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' &&
        p[3] == 'S')
    {
        return FM_MEDIA_OGG;
    }
    if (len >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' &&
        p[3] == 'F' && p[8] == 'A' && p[9] == 'V' && p[10] == 'I' &&
        p[11] == ' ')
    {
        return FM_MEDIA_AVI;
    }
    if (len >= 12 && p[4] == 'f' && p[5] == 't' && p[6] == 'y' &&
        p[7] == 'p')
    {
        return FM_MEDIA_MP4;
    }
    if (len >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' &&
        p[3] == 'F' && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' &&
        p[11] == 'P')
    {
        return FM_MEDIA_WEBP;
    }
    if (len >= 4 && p[0] == 'F' && p[1] == 'M' && p[2] == 'R' &&
        p[3] == 'V')
    {
        return FM_MEDIA_RAW;
    }

    /* JSON / text detection */
    if (by_ext == FM_MEDIA_JSON || by_ext == FM_MEDIA_CSV ||
        by_ext == FM_MEDIA_XML || by_ext == FM_MEDIA_INI ||
        by_ext == FM_MEDIA_TEXT)
    {
        size_t i;

        for (i = 0; i < len; i++)
        {
            uint8_t c = p[i];

            if (c == 0x00 || c == 0xFF)
            {
                return FM_MEDIA_BINARY;
            }
        }
        return by_ext;
    }

    if (by_ext != FM_MEDIA_UNKNOWN)
    {
        return by_ext;
    }
    return FM_MEDIA_BINARY;
}

FM_MediaType File_DetectType(const char *path)
{
    uint8_t head[FM_MEDIA_MAGIC_SCAN];
    uint32_t rd32 = 0;
    FM_File *f;
    FM_Result r;

    if (path == NULL)
    {
        return FM_MEDIA_UNKNOWN;
    }
    f = File_Open(path, FM_OPEN_READ);
    if (f == NULL)
    {
        return Media_DetectByExtension(path);
    }
    r = File_Read(f, head, sizeof(head), &rd32);
    (void)File_Close(f);
    if (r != FM_OK && r != FM_ERR_EOF)
    {
        return Media_DetectByExtension(path);
    }
    return Media_DetectBuffer(head, rd32, path);
}

FM_Result Media_SaveEncoded(const char *path, const void *encoded,
                            size_t encoded_size, FM_MediaType expected,
                            FM_MediaType *detected)
{
    FM_MediaType actual;
    FM_MediaType extension;

    if (path == NULL || encoded == NULL || encoded_size == 0 ||
        expected < FM_MEDIA_UNKNOWN || expected >= FM_MEDIA_IMAGE_MAX)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    actual = Media_DetectBuffer(encoded, encoded_size, NULL);
    extension = Media_DetectByExtension(path);
    if (detected != NULL) *detected = actual;

    if (actual == FM_MEDIA_UNKNOWN || actual == FM_MEDIA_BINARY ||
        (expected != FM_MEDIA_UNKNOWN && actual != expected))
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (extension != FM_MEDIA_UNKNOWN && extension != FM_MEDIA_BINARY &&
        extension != actual)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    return File_WriteAll(path, encoded, encoded_size);
}

const char *Media_TypeName(FM_MediaType type)
{
    switch (type)
    {
    case FM_MEDIA_TEXT:    return "TEXT";
    case FM_MEDIA_JSON:    return "JSON";
    case FM_MEDIA_CSV:     return "CSV";
    case FM_MEDIA_XML:     return "XML";
    case FM_MEDIA_INI:     return "INI";
    case FM_MEDIA_BINARY:  return "BINARY";
    case FM_MEDIA_JPEG:    return "JPEG";
    case FM_MEDIA_PNG:     return "PNG";
    case FM_MEDIA_GIF:     return "GIF";
    case FM_MEDIA_BMP:     return "BMP";
    case FM_MEDIA_TIFF:    return "TIFF";
    case FM_MEDIA_WAV:     return "WAV";
    case FM_MEDIA_MP3:     return "MP3";
    case FM_MEDIA_AAC:     return "AAC";
    case FM_MEDIA_FLAC:    return "FLAC";
    case FM_MEDIA_OGG:     return "OGG";
    case FM_MEDIA_AVI:     return "AVI";
    case FM_MEDIA_MP4:     return "MP4";
    case FM_MEDIA_WEBP:    return "WEBP";
    case FM_MEDIA_RAW:     return "RAW";
    default:               return "UNKNOWN";
    }
}

const char *Media_MimeType(FM_MediaType type)
{
    switch (type)
    {
    case FM_MEDIA_TEXT:    return "text/plain";
    case FM_MEDIA_JSON:    return "application/json";
    case FM_MEDIA_CSV:     return "text/csv";
    case FM_MEDIA_XML:     return "application/xml";
    case FM_MEDIA_INI:     return "text/plain";
    case FM_MEDIA_BINARY:  return "application/octet-stream";
    case FM_MEDIA_JPEG:    return "image/jpeg";
    case FM_MEDIA_PNG:     return "image/png";
    case FM_MEDIA_GIF:     return "image/gif";
    case FM_MEDIA_BMP:     return "image/bmp";
    case FM_MEDIA_TIFF:    return "image/tiff";
    case FM_MEDIA_WAV:     return "audio/wav";
    case FM_MEDIA_MP3:     return "audio/mpeg";
    case FM_MEDIA_AAC:     return "audio/aac";
    case FM_MEDIA_FLAC:    return "audio/flac";
    case FM_MEDIA_OGG:     return "audio/ogg";
    case FM_MEDIA_AVI:     return "video/x-msvideo";
    case FM_MEDIA_MP4:     return "video/mp4";
    case FM_MEDIA_WEBP:    return "image/webp";
    case FM_MEDIA_RAW:     return "video/x-raw";
    default:               return "application/octet-stream";
    }
}

#endif /* FM_USE_MEDIA_DETECT */
