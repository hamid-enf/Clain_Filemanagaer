/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_logger.h - Data Logger plugin (optional)
 *
 * CSV / Binary / JSONL logging with timestamps, sequence numbers,
 * automatic file rotation, circular (keep-last-N) operation and
 * crash-safe close.
 */

#ifndef FM_LOGGER_H
#define FM_LOGGER_H

#include "fm_types.h"
#include "fm_file.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_LOG_FORMAT_CSV = 0,
    FM_LOG_FORMAT_BINARY = 1,
    FM_LOG_FORMAT_JSONL = 2
} FM_LogFormat;

typedef struct
{
    uint64_t max_file_size;    /* rotate after N bytes (0 = unlimited) */
    uint32_t max_records;      /* rotate after N records (0 = unlimited)*/
    uint32_t max_files;        /* circular: keep at most N files        */
    uint32_t flush_records;    /* flush every N records (0 = never)     */
    FM_Bool  crash_safe;       /* write via temp file + rename          */
    uint32_t (*now_cb)(void);  /* timestamp source (NULL = 0)           */
} FM_LoggerConfig;

typedef struct
{
    uint32_t record_count;     /* records in current file               */
    uint32_t file_index;       /* current file sequence number          */
    uint64_t file_size;        /* current file size                     */
    char     file_name[FM_PATH_MAX];  /* current file name              */
    uint32_t files_created;    /* total files created this session      */
} FM_LoggerInfo;

typedef struct
{
    FM_File     file;
    char        dir[FM_PATH_MAX];
    char        base[FM_NAME_MAX];
    FM_LogFormat format;
    FM_LoggerConfig cfg;
    FM_LoggerInfo info;
    uint64_t    sequence;
    uint8_t     active;
    uint8_t     header_written;
    uint8_t     buffer[FM_LOGGER_BUFFER_SIZE];
    uint32_t    buffer_len;
    uint32_t    since_flush;
} FM_Logger;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
FM_Result Logger_Init(FM_Logger *lg, const char *dir, const char *base_name,
                      FM_LogFormat format, const FM_LoggerConfig *cfg);
FM_Result Logger_Start(FM_Logger *lg);
FM_Result Logger_Stop(FM_Logger *lg);        /* flush + close current file */
FM_Result Logger_Close(FM_Logger *lg);       /* alias for Stop             */
FM_Result Logger_Flush(FM_Logger *lg);
FM_Result Logger_GetInfo(FM_Logger *lg, FM_LoggerInfo *info);

/* CSV / JSONL: one record of N float values with a timestamp, three
 * decimals each.  A value beyond +-9000000 is written as +-9000000.000.
 * NaN and +-infinity are written as nan / inf / -inf in CSV and as null
 * in JSONL. */
FM_Result Logger_Write(FM_Logger *lg, uint32_t timestamp,
                       const float *values, uint32_t value_count);

/* Binary: one record with a fixed user layout.  The record is prefixed
 * with a 4-byte length + 4-byte CRC32 so corrupt tails can be
 * detected/skipped on recovery. */
FM_Result Logger_WriteRecord(FM_Logger *lg, const void *data, uint32_t size);

/* Raw bytes (e.g. a pre-formatted CSV line, including '\n'). */
FM_Result Logger_WriteRaw(FM_Logger *lg, const void *data, uint32_t size);

/* Log a CSV line from printf-style formatting. */
FM_Result Logger_Printf(FM_Logger *lg, const char *fmt, ...);

/* Recovery: remove stale temp files left after power loss. */
FM_Result Logger_Recover(const char *dir, const char *base_name);

FM_END_DECLS

#endif /* FM_LOGGER_H */
