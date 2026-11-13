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
    if (strcmp(ext, "wave") == 0)  return FM_MEDIA_WAV;
    if (strcmp(ext, "mp3") == 0)   return FM_MEDIA_MP3;
    if (strcmp(ext, "aac") == 0)   return FM_MEDIA_AAC;
    /* .m4a is an MP4 file with AAC in it, not an ADTS stream: by content
     * it is MP4, and by extension it has to be the same. */
    if (strcmp(ext, "m4a") == 0)   return FM_MEDIA_MP4;
    if (strcmp(ext, "flac") == 0)  return FM_MEDIA_FLAC;
    if (strcmp(ext, "ogg") == 0)   return FM_MEDIA_OGG;
    if (strcmp(ext, "oga") == 0)   return FM_MEDIA_OGG;
    if (strcmp(ext, "opus") == 0)  return FM_MEDIA_OGG;
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

/* The `ext` argument of Media_DetectBuffer: a file name ("a.mp3"), or
 * just the extension, with or without its dot ("mp3", ".mp3"). */
static FM_MediaType media_by_hint(const char *ext)
{
    char name[20];
    size_t n = strlen(ext);
    const char *dot = strrchr(ext, '.');

    if (dot != NULL && dot != ext)
    {
        return media_by_ext(ext);
    }
    if (dot == ext)
    {
        ext++;
        n--;
    }
    if (n == 0u || n > sizeof(name) - 3u)
    {
        return FM_MEDIA_UNKNOWN;
    }
    name[0] = 'x';
    name[1] = '.';
    memcpy(name + 2, ext, n + 1u);
    return media_by_ext(name);
}

/* Length in bytes of the MPEG audio frame whose header is at p, 0 when
 * the header is not one. */
static uint32_t media_mpeg_frame_len(const uint8_t *p)
{
    static const uint8_t half_kbps[5][15] = {
        { 0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224 },  /* V1 L1 */
        { 0, 16, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192 },      /* V1 L2 */
        { 0, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160 },       /* V1 L3 */
        { 0, 16, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128 },        /* V2 L1 */
        { 0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80 }             /* V2 L2/3 */
    };
    static const uint16_t rate_v1[3] = { 44100, 48000, 32000 };
    uint32_t version = (p[1] >> 3) & 3u;        /* 3 = 1, 2 = 2, 0 = 2.5 */
    uint32_t layer = 4u - ((p[1] >> 1) & 3u);   /* 1..3 */
    uint32_t br_index = p[2] >> 4;
    uint32_t sr_index = (p[2] >> 2) & 3u;
    uint32_t pad = (p[2] >> 1) & 1u;
    uint32_t row;
    uint32_t bps;
    uint32_t rate;

    if (p[0] != 0xFFu || (p[1] & 0xE0u) != 0xE0u || version == 1u ||
        layer == 4u || br_index == 0u || br_index == 15u || sr_index == 3u)
    {
        return 0u;
    }
    row = (version == 3u) ? layer - 1u : ((layer == 1u) ? 3u : 4u);
    bps = (uint32_t)half_kbps[row][br_index] * 2000u;
    rate = rate_v1[sr_index];
    if (version == 2u) rate /= 2u;
    if (version == 0u) rate /= 4u;
    if (layer == 1u)
    {
        return (12u * bps / rate + pad) * 4u;
    }
    if (layer == 3u && version != 3u)
    {
        return 72u * bps / rate + pad;
    }
    return 144u * bps / rate + pad;
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
        by_ext = media_by_hint(ext);
    }
    if (buf == NULL || len == 0)
    {
        return by_ext;
    }

    /* Magic numbers win over extension. */
    /* An ID3v2 tag may sit in front of any audio stream, not only an MP3
     * one: taggers write it into FLAC, Ogg and WAV files too, and the
     * magic numbers are all behind it.  Every such file was reported as
     * FM_MEDIA_MP3.  Skip whole tags (a chained file may have several),
     * then read what the tag was hiding. */
    while (len >= 10u && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
    {
        size_t taglen = 10u + (((size_t)(p[6] & 0x7Fu) << 21) |
                               ((size_t)(p[7] & 0x7Fu) << 14) |
                               ((size_t)(p[8] & 0x7Fu) << 7) |
                               (size_t)(p[9] & 0x7Fu));

        if ((p[5] & 0x10u) != 0u)
        {
            taglen += 10u;                   /* the tag has a footer */
        }
        if (taglen + 4u > len)
        {
            /* the buffer ends inside the tag: nothing to read behind it */
            return (by_ext != FM_MEDIA_UNKNOWN) ? by_ext : FM_MEDIA_MP3;
        }
        p += taglen;
        len -= taglen;
    }

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
    if (len >= 2 && p[0] == 0xFFu && (p[1] & 0xF6u) == 0xF0u)
    {
        return FM_MEDIA_AAC;   /* ADTS sync */
    }
    if (len >= 4 && media_mpeg_frame_len(p) != 0u)
    {
        /* Eleven set bits and three fields that are not "reserved" is a
         * weak test: the byte-order mark of UTF-16 text (FF FE) followed
         * by an ordinary letter passes it.  A frame is followed by the
         * next one, so when the buffer reaches that far the next header
         * has to be there, same MPEG version and layer.  When it does not
         * reach, the old answer stands, except for the byte-order mark:
         * text is the likelier reading of FF FE. */
        uint32_t flen = media_mpeg_frame_len(p);

        if ((size_t)flen + 4u <= len)
        {
            const uint8_t *q = p + flen;

            if (media_mpeg_frame_len(q) != 0u &&
                ((q[1] ^ p[1]) & 0xFEu) == 0u)
            {
                return FM_MEDIA_MP3;
            }
        }
        else if (p[1] != 0xFEu)
        {
            return FM_MEDIA_MP3;
        }
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
    if ((r == FM_OK || r == FM_ERR_EOF) && rd32 >= 10u &&
        head[0] == 'I' && head[1] == 'D' && head[2] == '3')
    {
        /* An ID3v2 tag in front of the stream: the scan above only
         * reaches into the tag, so every tagged FLAC, Ogg and WAV file
         * was reported as MP3.  Read again from behind the tag. */
        uint64_t skip = 10u + (((uint32_t)(head[6] & 0x7Fu) << 21) |
                               ((uint32_t)(head[7] & 0x7Fu) << 14) |
                               ((uint32_t)(head[8] & 0x7Fu) << 7) |
                               (uint32_t)(head[9] & 0x7Fu));

        if ((head[5] & 0x10u) != 0u)
        {
            skip += 10u;                   /* the tag has a footer */
        }
        if (File_Seek(f, (int64_t)skip, FM_SEEK_SET) == FM_OK)
        {
            rd32 = 0;
            r = File_Read(f, head, sizeof(head), &rd32);
        }
        else
        {
            r = FM_ERR_IO;
        }
    }
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
