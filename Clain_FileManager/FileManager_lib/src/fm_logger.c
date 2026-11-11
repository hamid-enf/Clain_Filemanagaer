/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_logger.c - Data Logger plugin (CSV / Binary / JSONL)
 */

#define FM_DEBUG_TAG "logger"
#include "fm_debug.h"
#include "fm_logger.h"
#include "fm_internal.h"
#include "fm_crc.h"
#include <string.h>
#include <stdarg.h>

#if FM_USE_PRINTF
#include <stdio.h>
#include <stdarg.h>
#endif

#if FM_USE_DATA_LOGGER

static void logger_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst == NULL || dst_size == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_size)
    {
        len = dst_size - 1;
    }
    if (len > 0)
    {
        (void)memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static FM_Bool wildcard_match_local(const char *pattern, const char *text)
{
    /* One loop, no recursion.  After a '*' the place is remembered; when
     * the text stops matching, the star takes one more character and the
     * match goes on from there.  Only the last star ever has to be
     * revisited, so the work is at most pattern length x text length.
     * (The recursive form this replaces tried every split for every
     * star: a filter such as "*a*a*a*a*a*b" against a long name of 'a's
     * ran for minutes, and each star cost a stack frame.) */
    const char *star = NULL;        /* the last '*' seen in the pattern */
    const char *retry = NULL;       /* where in the text it began       */

    while (*text != '\0')
    {
        if (*pattern == '*')
        {
            star = pattern++;
            retry = text;
        }
        else if (*pattern == *text)
        {
            pattern++;
            text++;
        }
        else if (star != NULL)
        {
            pattern = star + 1;
            text = ++retry;
        }
        else
        {
            return FM_FALSE;
        }
    }
    while (*pattern == '*')
    {
        pattern++;
    }
    return (FM_Bool)(*pattern == '\0');
}

static const char *logger_ext(FM_LogFormat fmt)
{
    switch (fmt)
    {
    case FM_LOG_FORMAT_CSV:    return "csv";
    case FM_LOG_FORMAT_BINARY: return "bin";
    default:                   return "jsonl";
    }
}

static void u32_to_str(uint32_t v, char *out)
{
    char tmp[12];
    int n = 0;

    if (v == 0)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0)
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    {
        int i;

        for (i = 0; i < n; i++)
        {
            out[i] = tmp[n - 1 - i];
        }
        out[n] = '\0';
    }
}

static void float_to_str(float v, char *out, size_t out_size)
{
    /* Deterministic 3-decimal formatting without libc printf. */
    int64_t scaled;
    uint32_t ip;
    uint32_t fp;
    char int_s[16];
    char frac_s[12];    /* fp < 1000, but u32_to_str can write 11 bytes */
    size_t l;
    size_t i;
    size_t n = 0;

    if (v < 0)
    {
        if (n + 1 < out_size)
        {
            out[n++] = '-';
        }
        v = -v;
    }
    if (v > 9000000.0f)
    {
        v = 9000000.0f;
    }
    scaled = (int64_t)((double)v * 1000.0 + 0.5);
    ip = (uint32_t)(scaled / 1000);
    fp = (uint32_t)(scaled % 1000);

    u32_to_str(ip, int_s);
    l = strlen(int_s);
    for (i = 0; i < l && n + 1 < out_size; i++)
    {
        out[n++] = int_s[i];
    }
    if (n + 1 < out_size)
    {
        out[n++] = '.';
    }
    u32_to_str(fp, frac_s);
    l = strlen(frac_s);
    for (i = l; i < 3 && n + 1 < out_size; i++)
    {
        out[n++] = '0';
    }
    for (i = 0; i < l && n + 1 < out_size; i++)
    {
        out[n++] = frac_s[i];
    }
    if (n < out_size)
    {
        out[n] = '\0';
    }
}

static FM_Result logger_append(char *out, size_t cap, size_t *pos,
                               const char *text)
{
    size_t len;

    if (out == NULL || pos == NULL || text == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    len = strlen(text);
    if (*pos > cap || len + 1 > cap - *pos)
    {
        return FM_ERR_TOO_LARGE;
    }
    memcpy(out + *pos, text, len);
    *pos += len;
    out[*pos] = '\0';
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* File lifecycle                                                      */
/* ------------------------------------------------------------------ */
static FM_Result logger_make_name(const FM_Logger *lg, char *out,
                                  size_t out_size, char *tmp_out,
                                  size_t tmp_size)
{
    char seq[16];
    char ts[16];
    const char *ext = logger_ext(lg->format);
    size_t n = 0;
    FM_Result r;

    u32_to_str(lg->info.file_index, seq);
    if (lg->cfg.now_cb != NULL)
    {
        u32_to_str(lg->cfg.now_cb(), ts);
    }
    else
    {
        ts[0] = '\0';
    }

    out[0] = '\0';
    r = logger_append(out, out_size, &n, lg->dir);
    if (r == FM_OK) r = logger_append(out, out_size, &n, "/");
    if (r == FM_OK) r = logger_append(out, out_size, &n, lg->base);
    if (r == FM_OK) r = logger_append(out, out_size, &n, "_");
    if (r == FM_OK) r = logger_append(out, out_size, &n, seq);
    if (r == FM_OK && ts[0] != '\0') r = logger_append(out, out_size, &n, "_");
    if (r == FM_OK && ts[0] != '\0') r = logger_append(out, out_size, &n, ts);
    if (r == FM_OK) r = logger_append(out, out_size, &n, ".");
    if (r == FM_OK) r = logger_append(out, out_size, &n, ext);
    if (r != FM_OK)
    {
        return r;
    }

    if (tmp_out != NULL)
    {
        if (tmp_size == 0 || strlen(out) + strlen(".fm_tmp") + 1 > tmp_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        (void)strcpy(tmp_out, out);
        (void)strcat(tmp_out, ".fm_tmp");
    }
    return FM_OK;
}

static FM_Result logger_open_file(FM_Logger *lg)
{
    char name[FM_PATH_MAX];
    char tmp[FM_PATH_MAX];
    const char *open_name;
    FM_Result r;
    FM_Bool exists = FM_FALSE;

    lg->info.file_index++;
    r = logger_make_name(lg, name, sizeof(name),
                         (lg->cfg.crash_safe) ? tmp : NULL,
                         sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }

    if (lg->cfg.crash_safe)
    {
        open_name = tmp;
    }
    else
    {
        open_name = name;
    }

    /* If the target exists, bump the index until free. */
    while (File_Exists(open_name, &exists) == FM_OK && exists)
    {
        lg->info.file_index++;
        r = logger_make_name(lg, name, sizeof(name),
                             (lg->cfg.crash_safe) ? tmp : NULL,
                             sizeof(tmp));
        if (r != FM_OK)
        {
            return r;
        }
        if (lg->cfg.crash_safe)
        {
            open_name = tmp;
        }
        else
        {
            open_name = name;
        }
    }

    r = File_OpenEx(&lg->file, open_name,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        return r;
    }

    logger_copy_string(lg->info.file_name, sizeof(lg->info.file_name), name);
    lg->info.file_size = 0;
    lg->info.record_count = 0;
    lg->header_written = 0;
    return FM_OK;
}

static FM_Result logger_finalize_file(FM_Logger *lg)
{
    FM_Result r;

    if (!lg->active)
    {
        return FM_OK;
    }
    r = Logger_Flush(lg);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Close(&lg->file);
    lg->active = 0;
    if (r != FM_OK)
    {
        return r;
    }
    if (lg->cfg.crash_safe)
    {
        char final_name[FM_PATH_MAX];
        char tmp_name[FM_PATH_MAX];
        size_t flen = strlen(lg->info.file_name);

        logger_copy_string(final_name, sizeof(final_name), lg->info.file_name);
        logger_copy_string(tmp_name, sizeof(tmp_name), final_name);
        if (flen + strlen(".fm_tmp") + 1 > sizeof(tmp_name))
        {
            return FM_ERR_TOO_LARGE;
        }
        (void)strcat(tmp_name, ".fm_tmp");
        r = File_Rename(tmp_name, final_name);
        if (r != FM_OK)
        {
            return r;
        }
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Circular rotation cleanup                                           */
/* ------------------------------------------------------------------ */
static FM_Bool logger_parse_sequence(const FM_Logger *lg,
                                      const char *name, uint32_t *sequence)
{
    char prefix[FM_NAME_MAX + 2];
    const char *p;
    const char *ext = logger_ext(lg->format);
    size_t prefix_len;
    size_t ext_len = strlen(ext);
    uint32_t value = 0;
    size_t n = 0;

    if (strlen(lg->base) + 2 > sizeof(prefix))
    {
        return FM_FALSE;
    }
    (void)strcpy(prefix, lg->base);
    (void)strcat(prefix, "_");
    prefix_len = strlen(prefix);
    if (strncmp(name, prefix, prefix_len) != 0)
    {
        return FM_FALSE;
    }
    p = name + prefix_len;
    while (p[n] >= '0' && p[n] <= '9')
    {
        if (value > (UINT32_MAX - (uint32_t)(p[n] - '0')) / 10u)
        {
            return FM_FALSE;
        }
        value = value * 10u + (uint32_t)(p[n] - '0');
        n++;
    }
    if (n == 0 || (p[n] != '.' && p[n] != '_'))
    {
        return FM_FALSE;
    }
    if (strlen(name) < ext_len + 1 ||
        strcmp(name + strlen(name) - ext_len, ext) != 0)
    {
        return FM_FALSE;
    }
    *sequence = value;
    return FM_TRUE;
}

static FM_Result logger_circular_cleanup(FM_Logger *lg)
{
    FM_Dir *d;
    FM_DirEntry e;
    FM_Result r;
    uint32_t to_delete;
    uint32_t threshold;

    if (lg->cfg.max_files == 0 || lg->info.files_created <= lg->cfg.max_files)
    {
        return FM_OK;
    }
    to_delete = lg->info.files_created - lg->cfg.max_files;
    threshold = (lg->info.file_index > lg->cfg.max_files)
                    ? lg->info.file_index - lg->cfg.max_files
                    : 0;
    d = Directory_Open(lg->dir);
    if (d == NULL)
    {
        return File_GetLastError();
    }
    while (to_delete > 0)
    {
        FM_Bool found = FM_FALSE;

        for (;;)
        {
            uint32_t sequence;
            char full[FM_PATH_MAX];

            r = Directory_Read(d, &e);
            if (r == FM_ERR_EOF)
            {
                break;
            }
            if (r != FM_OK)
            {
                (void)Directory_Close(d);
                return r;
            }
            if (e.type != FM_ITEM_FILE ||
                !logger_parse_sequence(lg, e.name, &sequence) ||
                sequence > threshold)
            {
                continue;
            }
            if (Path_Join(lg->dir, e.name, full, sizeof(full)) == FM_OK)
            {
                r = File_Delete(full);
                if (r != FM_OK)
                {
                    (void)Directory_Close(d);
                    return r;
                }
                found = FM_TRUE;
                to_delete--;
                break;
            }
        }
        if (!found)
        {
            break;
        }
        (void)Directory_Rewind(d);
    }
    (void)Directory_Close(d);
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
FM_Result Logger_Init(FM_Logger *lg, const char *dir, const char *base_name,
                      FM_LogFormat format, const FM_LoggerConfig *cfg)
{
    if (lg == NULL || dir == NULL || base_name == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(dir) >= sizeof(lg->dir) || strlen(base_name) >= sizeof(lg->base))
    {
        return FM_ERR_TOO_LARGE;
    }
    if (format != FM_LOG_FORMAT_CSV && format != FM_LOG_FORMAT_BINARY &&
        format != FM_LOG_FORMAT_JSONL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(lg, 0, sizeof(*lg));
    (void)strcpy(lg->dir, dir);
    (void)strcpy(lg->base, base_name);
    lg->format = format;
    if (cfg != NULL)
    {
        lg->cfg = *cfg;
    }
    else
    {
        lg->cfg.max_file_size = 0;
        lg->cfg.max_records = 0;
        lg->cfg.max_files = 0;
        lg->cfg.flush_records = 0;
        lg->cfg.crash_safe = FM_FALSE;
        lg->cfg.now_cb = NULL;
    }
    return FM_OK;
}

FM_Result Logger_Start(FM_Logger *lg)
{
    FM_Result r;

    if (lg == NULL || lg->dir[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (lg->active)
    {
        return FM_ERR_BUSY;
    }

    /* Ensure the log directory exists. */
    {
        FM_Bool exists = FM_FALSE;

        if (Directory_Exists(lg->dir, &exists) == FM_OK && !exists)
        {
            r = Directory_Create(lg->dir);
            if (r != FM_OK)
            {
                return r;
            }
        }
    }

    r = logger_open_file(lg);
    if (r != FM_OK)
    {
        return r;
    }
    lg->active = 1;
    lg->buffer_len = 0;
    lg->since_flush = 0;
    lg->info.files_created++;
    return FM_OK;
}

FM_Result Logger_Stop(FM_Logger *lg)
{
    if (lg == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return logger_finalize_file(lg);
}

FM_Result Logger_Close(FM_Logger *lg)
{
    return Logger_Stop(lg);
}

FM_Result Logger_Flush(FM_Logger *lg)
{
    FM_Result r;

    if (lg == NULL || !lg->active)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (lg->buffer_len > 0)
    {
        uint32_t wr = 0;

        r = File_Write(&lg->file, lg->buffer, lg->buffer_len, &wr);
        if (r != FM_OK)
        {
            return r;
        }
        if (wr != lg->buffer_len)
        {
            return FM_ERR_IO;
        }
        lg->buffer_len = 0;
    }
    r = File_Flush(&lg->file);
    if (r == FM_OK)
    {
        uint64_t size = 0;

        if (File_Size(&lg->file, &size) == FM_OK)
        {
            lg->info.file_size = size;
        }
        lg->since_flush = 0;
    }
    return r;
}

FM_Result Logger_GetInfo(FM_Logger *lg, FM_LoggerInfo *info)
{
    if (lg == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *info = lg->info;
    return FM_OK;
}

/* Rotate when limits are reached. */
static FM_Result logger_maybe_rotate(FM_Logger *lg)
{
    FM_Result r;

    if (lg->cfg.max_file_size > 0 &&
        lg->info.file_size >= lg->cfg.max_file_size)
    {
        r = logger_finalize_file(lg);
        if (r != FM_OK)
        {
            return r;
        }
        r = logger_open_file(lg);
        if (r != FM_OK)
        {
            return r;
        }
        lg->active = 1;
        lg->buffer_len = 0;
        lg->since_flush = 0;
        lg->info.files_created++;
        r = logger_circular_cleanup(lg);
        if (r != FM_OK)
        {
            return r;
        }
    }
    else if (lg->cfg.max_records > 0 &&
             lg->info.record_count >= lg->cfg.max_records)
    {
        r = logger_finalize_file(lg);
        if (r != FM_OK)
        {
            return r;
        }
        r = logger_open_file(lg);
        if (r != FM_OK)
        {
            return r;
        }
        lg->active = 1;
        lg->buffer_len = 0;
        lg->since_flush = 0;
        lg->info.files_created++;
        r = logger_circular_cleanup(lg);
        if (r != FM_OK)
        {
            return r;
        }
    }
    return FM_OK;
}

static FM_Result logger_write_buffered(FM_Logger *lg, const void *data,
                                       uint32_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    FM_Result r;

    while (size > 0)
    {
        uint32_t space = (uint32_t)sizeof(lg->buffer) - lg->buffer_len;
        uint32_t chunk = (size < space) ? size : space;

        if (space == 0)
        {
            r = Logger_Flush(lg);
            if (r != FM_OK)
            {
                return r;
            }
            continue;
        }
        if (UINT64_MAX - lg->info.file_size < chunk)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(lg->buffer + lg->buffer_len, p, chunk);
        lg->buffer_len += chunk;
        lg->info.file_size += chunk;
        p += chunk;
        size -= chunk;
    }
    return FM_OK;
}

FM_Result Logger_WriteRaw(FM_Logger *lg, const void *data, uint32_t size)
{
    FM_Result r;

    if (lg == NULL || (data == NULL && size > 0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!lg->active)
    {
        return FM_ERR_NOT_OPEN;
    }
    r = logger_write_buffered(lg, data, size);
    if (r != FM_OK)
    {
        return r;
    }
    lg->since_flush++;
    if (lg->cfg.flush_records > 0 && lg->since_flush >= lg->cfg.flush_records)
    {
        r = Logger_Flush(lg);
        if (r != FM_OK)
        {
            return r;
        }
    }
    r = logger_maybe_rotate(lg);
    return r;
}

FM_Result Logger_Write(FM_Logger *lg, uint32_t timestamp,
                       const float *values, uint32_t value_count)
{
    char line[FM_LOGGER_BUFFER_SIZE];
    char ts[16];
    char seq[16];
    size_t n = 0;
    uint32_t i;
    FM_Result r;

    if (lg == NULL || (values == NULL && value_count > 0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    line[0] = '\0';
    u32_to_str(timestamp, ts);
    u32_to_str((uint32_t)lg->sequence, seq);

    if (lg->format == FM_LOG_FORMAT_CSV)
    {
        r = logger_append(line, sizeof(line), &n, ts);
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, ",");
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, seq);
        for (i = 0; r == FM_OK && i < value_count; i++)
        {
            char fv[32];

            float_to_str(values[i], fv, sizeof(fv));
            r = logger_append(line, sizeof(line), &n, ",");
            if (r == FM_OK) r = logger_append(line, sizeof(line), &n, fv);
        }
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, "\n");
    }
    else if (lg->format == FM_LOG_FORMAT_JSONL)
    {
        r = logger_append(line, sizeof(line), &n, "{\"ts\":");
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, ts);
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, ",\"seq\":");
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, seq);
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, ",\"v\":[");
        for (i = 0; r == FM_OK && i < value_count; i++)
        {
            char fv[32];

            float_to_str(values[i], fv, sizeof(fv));
            if (i > 0) r = logger_append(line, sizeof(line), &n, ",");
            if (r == FM_OK) r = logger_append(line, sizeof(line), &n, fv);
        }
        if (r == FM_OK) r = logger_append(line, sizeof(line), &n, "]}\n");
    }
    else
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (r != FM_OK)
    {
        return r;
    }
    r = Logger_WriteRaw(lg, line, (uint32_t)n);
    if (r != FM_OK)
    {
        return r;
    }
    lg->sequence++;
    lg->info.record_count++;
    return FM_OK;
}

FM_Result Logger_WriteRecord(FM_Logger *lg, const void *data, uint32_t size)
{
#if !FM_USE_CRC
    (void)lg; (void)data; (void)size;
    return FM_ERR_UNSUPPORTED;
#else
    uint8_t hdr[8];
    uint32_t crc;
    FM_Result r;

    if (lg == NULL || (data == NULL && size > 0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!lg->active)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (lg->format != FM_LOG_FORMAT_BINARY)
    {
        return FM_ERR_INVALID_ARGUMENT; /* records are binary-only */
    }

    crc = CRC_Calculate32(data, size);
    hdr[0] = (uint8_t)(size & 0xFFu);
    hdr[1] = (uint8_t)((size >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((size >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((size >> 24) & 0xFFu);
    hdr[4] = (uint8_t)(crc & 0xFFu);
    hdr[5] = (uint8_t)((crc >> 8) & 0xFFu);
    hdr[6] = (uint8_t)((crc >> 16) & 0xFFu);
    hdr[7] = (uint8_t)((crc >> 24) & 0xFFu);

    r = logger_write_buffered(lg, hdr, sizeof(hdr));
    if (r != FM_OK)
    {
        return r;
    }
    r = logger_write_buffered(lg, data, size);
    if (r != FM_OK)
    {
        return r;
    }
    lg->since_flush++;
    if (lg->cfg.flush_records > 0 && lg->since_flush >= lg->cfg.flush_records)
    {
        r = Logger_Flush(lg);
        if (r != FM_OK)
        {
            return r;
        }
    }
    lg->sequence++;
    lg->info.record_count++;
    return logger_maybe_rotate(lg);
#endif /* FM_USE_CRC */
}

#if FM_USE_PRINTF
FM_Result Logger_Printf(FM_Logger *lg, const char *fmt, ...)
{
    char line[FM_LOGGER_BUFFER_SIZE];
    va_list ap;
    int n;

    if (lg == NULL || fmt == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)n >= sizeof(line))
    {
        return FM_ERR_TOO_LARGE;
    }
    return Logger_WriteRaw(lg, line, (uint32_t)n);
}
#else
FM_Result Logger_Printf(FM_Logger *lg, const char *fmt, ...)
{
    (void)lg;
    (void)fmt;
    return FM_ERR_UNSUPPORTED;
}
#endif

FM_Result Logger_Recover(const char *dir, const char *base_name)
{
    /* Delete stale "<base>*..fm_tmp" files after a power loss. */
    FM_Dir *d;
    FM_DirEntry e;
    FM_Result r;
    char pat[FM_NAME_MAX + 16];

    if (dir == NULL || base_name == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        size_t n = 0;
        pat[0] = '\0';
        r = logger_append(pat, sizeof(pat), &n, base_name);
        if (r == FM_OK) r = logger_append(pat, sizeof(pat), &n, "*.fm_tmp");
        if (r != FM_OK)
        {
            return r;
        }
    }

    d = Directory_Open(dir);
    if (d == NULL)
    {
        return File_GetLastError();
    }
    for (;;)
    {
        char full[FM_PATH_MAX];

        r = Directory_Read(d, &e);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)Directory_Close(d);
            return r;
        }
        if (e.type != FM_ITEM_FILE)
        {
            continue;
        }
        if (!wildcard_match_local(pat, e.name))
        {
            continue;
        }
        if (Path_Join(dir, e.name, full, sizeof(full)) == FM_OK)
        {
            (void)File_Delete(full);
        }
    }
    (void)Directory_Close(d);
    return FM_OK;
}

#endif /* FM_USE_DATA_LOGGER */
