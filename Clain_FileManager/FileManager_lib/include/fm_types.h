/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_types.h - Common types, error model, info structures
 *
 * Error model:
 *   Every API returns an FM_Result.  FM_OK (0) means success, all
 *   negative values are error codes.  The convenient FILE_OK /
 *   FILE_ERROR_* aliases are provided for compatibility with the
 *   Simple API style.
 */

#ifndef FM_TYPES_H
#define FM_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "fm_config.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Result / error codes                                                */
/* ------------------------------------------------------------------ */
typedef int32_t FM_Result;

enum
{
    FM_OK                        =  0,

    FM_ERR_INVALID_ARGUMENT      = -1,
    FM_ERR_INVALID_PATH          = -2,
    FM_ERR_NOT_FOUND             = -3,
    FM_ERR_ACCESS                = -4,
    FM_ERR_NO_SPACE              = -5,
    FM_ERR_IO                    = -6,
    FM_ERR_TIMEOUT               = -7,
    FM_ERR_BUSY                  = -8,
    FM_ERR_EXISTS                = -9,
    FM_ERR_NOT_EMPTY             = -10,
    FM_ERR_UNSUPPORTED           = -11,
    FM_ERR_CORRUPTED             = -12,
    FM_ERR_NOT_MOUNTED           = -13,
    FM_ERR_NO_MEMORY             = -14,
    FM_ERR_EOF                   = -15,
    FM_ERR_TOO_LARGE             = -16,
    FM_ERR_INVALID_FORMAT        = -17,
    FM_ERR_CRC                   = -18,
    FM_ERR_ABORTED               = -19,
    FM_ERR_READ_ONLY             = -20,
    FM_ERR_NOT_OPEN              = -21,
    FM_ERR_IS_DIRECTORY          = -22,
    FM_ERR_NOT_DIRECTORY         = -23,
    FM_ERR_CANCELLED             = -24,
    FM_ERR_NOT_READY             = -25,
    FM_ERR_INTERNAL              = -26,
    FM_ERR_CLEANUP               = -27 /* committed; post-commit cleanup failed */
};

/* Simple-API style aliases (see spec: FILE_OK, FILE_ERROR_*) */
#define FILE_OK                       FM_OK
#define FILE_ERROR_INVALID_ARGUMENT   FM_ERR_INVALID_ARGUMENT
#define FILE_ERROR_INVALID_PATH       FM_ERR_INVALID_PATH
#define FILE_ERROR_NOT_FOUND          FM_ERR_NOT_FOUND
#define FILE_ERROR_ACCESS             FM_ERR_ACCESS
#define FILE_ERROR_NO_SPACE           FM_ERR_NO_SPACE
#define FILE_ERROR_IO                 FM_ERR_IO
#define FILE_ERROR_TIMEOUT            FM_ERR_TIMEOUT
#define FILE_ERROR_BUSY               FM_ERR_BUSY
#define FILE_ERROR_EXISTS             FM_ERR_EXISTS
#define FILE_ERROR_NOT_EMPTY          FM_ERR_NOT_EMPTY
#define FILE_ERROR_UNSUPPORTED        FM_ERR_UNSUPPORTED
#define FILE_ERROR_CORRUPTED          FM_ERR_CORRUPTED
#define FILE_ERROR_NOT_MOUNTED        FM_ERR_NOT_MOUNTED
#define FILE_ERROR_NO_MEMORY          FM_ERR_NO_MEMORY
#define FILE_ERROR_READ_ONLY          FM_ERR_READ_ONLY
#define FILE_ERROR_CANCELLED          FM_ERR_CANCELLED

/* ------------------------------------------------------------------ */
/* Boolean                                                             */
/* ------------------------------------------------------------------ */
typedef uint8_t FM_Bool;
#define FM_FALSE ((FM_Bool)0)
#define FM_TRUE  ((FM_Bool)1)

/* ------------------------------------------------------------------ */
/* Item types & attributes                                             */
/* ------------------------------------------------------------------ */
typedef uint8_t FM_ItemType;
enum
{
    FM_ITEM_UNKNOWN = 0,
    FM_ITEM_FILE    = 1,
    FM_ITEM_DIR     = 2,
    FM_ITEM_VOLUME  = 3
};

typedef uint8_t FM_Attribute;
enum
{
    FM_ATTR_NONE      = 0x00,
    FM_ATTR_READ_ONLY = 0x01,
    FM_ATTR_HIDDEN    = 0x02,
    FM_ATTR_SYSTEM    = 0x04,
    FM_ATTR_ARCHIVE   = 0x08
};

/* ------------------------------------------------------------------ */
/* File info                                                           */
/* ------------------------------------------------------------------ */
typedef struct
{
    char     name[FM_NAME_MAX];    /* entry name (no path)            */
    char     path[FM_PATH_MAX];    /* full path of the entry          */
    uint64_t size;                 /* size in bytes                   */
    uint8_t  type;                 /* FM_ItemType                     */
    uint8_t  attributes;           /* FM_Attribute bitmask            */
    uint64_t created;              /* creation time (fs dependent)    */
    uint64_t modified;             /* last modification time          */
    uint64_t accessed;             /* last access time                */
} FM_FileInfo;

/* ------------------------------------------------------------------ */
/* Directory entry (short form used by listing)                        */
/* ------------------------------------------------------------------ */
typedef struct
{
    char     name[FM_NAME_MAX];
    uint64_t size;
    uint8_t  type;                 /* FM_ItemType                     */
    uint8_t  attributes;           /* FM_Attribute bitmask            */
    uint32_t timestamp;            /* modified (fs dependent)         */
} FM_DirEntry;

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */
typedef void (*FM_ProgressCallback)(uint64_t current, uint64_t total,
                                    void *user_data);
/* Return FM_OK to continue, FM_ERR_CANCELLED to abort the scan. */
typedef FM_Result (*FM_ItemCallback)(const FM_FileInfo *info, void *user_data);

/* ------------------------------------------------------------------ */
/* Enumerations used by several modules                                */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_SEEK_SET = 0,
    FM_SEEK_CUR = 1,
    FM_SEEK_END = 2
} FM_SeekOrigin;

typedef enum
{
    FM_ENDIAN_LITTLE = 0,
    FM_ENDIAN_BIG    = 1
} FM_Endian;

typedef enum
{
    FM_SCALE_NEAREST  = 0,
    FM_SCALE_BILINEAR = 1
} FM_ScaleMode;

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_LEN
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#if FM_DEBUG
#include <assert.h>
#define FM_ASSERT(x) assert(x)
#else
#define FM_ASSERT(x) ((void)0)
#endif

FM_END_DECLS

#endif /* FM_TYPES_H */
