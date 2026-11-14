/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_text.c - Text file API (line based, streaming)
 */

#define FM_DEBUG_TAG "text"
#include "fm_debug.h"
#include "fm_text.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_TEXT

/* ------------------------------------------------------------------ */
/* Line helpers                                                        */
/* ------------------------------------------------------------------ */
FM_Result Text_ReadLine(FM_File *f, char *line, size_t line_size,
                        size_t *read)
{
    size_t n = 0;
    FM_Bool overflow = FM_FALSE;
    FM_Bool had_data = FM_FALSE;
    size_t pending_cr = 0;

    if (f == NULL || line == NULL || line_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (read != NULL)
    {
        *read = 0;
    }

    for (;;)
    {
        uint32_t rd = 0;
        char c = 0;
        FM_Result r = File_Read(f, &c, 1, &rd);

        if (r == FM_ERR_EOF || (r == FM_OK && rd == 0))
        {
            if (!had_data)
            {
                line[0] = '\0';
                return FM_ERR_EOF;
            }
            break;
        }
        if (r != FM_OK)
        {
            line[0] = '\0';
            return r;
        }
        had_data = FM_TRUE;
        if (c == '\n')
        {
            break;
        }
        if (c == '\r')
        {
            /* Only the CR of a CRLF line end (or one that is the last
             * byte of the file) is dropped.  A CR anywhere else is data:
             * dropping it changed the text, and Text_Replace /
             * Text_InsertLine / Text_DeleteLine, which copy the file
             * line by line, deleted it from the file for good. */
            pending_cr++;
            continue;
        }
        while (pending_cr > 0)
        {
            pending_cr--;
            if (n + 1 < line_size)
            {
                line[n++] = '\r';
            }
            else
            {
                overflow = FM_TRUE;
            }
        }
        if (n + 1 < line_size)
        {
            line[n++] = c;
        }
        else
        {
            overflow = FM_TRUE;
        }
    }
    /* the line ended: one CR belongs to the line end, the others before
     * it are text */
    while (pending_cr > 1)
    {
        pending_cr--;
        if (n + 1 < line_size)
        {
            line[n++] = '\r';
        }
        else
        {
            overflow = FM_TRUE;
        }
    }
    line[n] = '\0';
    if (read != NULL)
    {
        *read = n;
    }
    return overflow ? FM_ERR_TOO_LARGE : FM_OK;
}

FM_Result Text_WriteLine(FM_File *f, const char *line)
{
    size_t len;
    FM_Result r;
    uint32_t wr = 0;

    if (f == NULL || line == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    len = strlen(line);
    while (len > 0)
    {
        uint32_t chunk = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
        r = File_Write(f, line, chunk, &wr);
        if (r != FM_OK) return r;
        if (wr == 0 || wr > chunk) return FM_ERR_IO;
        line += wr;
        len -= wr;
    }
    r = File_Write(f, "\n", 1, &wr);
    if (r != FM_OK) return r;
    return wr == 1u ? FM_OK : FM_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Whole-file operations                                               */
/* ------------------------------------------------------------------ */
FM_Result Text_ReadAll(const char *path, char *buffer, size_t buffer_size,
                       size_t *read)
{
    return File_ReadText(path, buffer, buffer_size, read);
}

FM_Result Text_WriteAll(const char *path, const char *text)
{
    return File_WriteText(path, text);
}

FM_Result Text_AppendLine(const char *path, const char *line)
{
    char buf[FM_INTERNAL_BUFFER_SIZE];
    size_t len;
    FM_Result r;

    if (path == NULL || line == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    len = strlen(line);
    if (len + 2 > sizeof(buf))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strncpy(buf, line, sizeof(buf) - 2);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    r = File_AppendText(path, buf);
    return r;
}

FM_Result Text_CountLines(const char *path, size_t *count)
{
    FM_File *f;
    FM_Result r;
    size_t n = 0;

    if (path == NULL || count == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    f = File_Open(path, FM_OPEN_READ);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    for (;;)
    {
        char line[FM_NAME_MAX * 2];

        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
    }
    (void)File_Close(f);
    *count = n;
    return FM_OK;
}

FM_Result Text_Find(const char *path, const char *needle, size_t *line_out)
{
    FM_File *f;
    FM_Result r;
    size_t n = 0;

    if (path == NULL || needle == NULL || line_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    f = File_Open(path, FM_OPEN_READ);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    for (;;)
    {
        char line[FM_INTERNAL_BUFFER_SIZE];

        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
        if (strstr(line, needle) != NULL)
        {
            (void)File_Close(f);
            *line_out = n;
            return FM_OK;
        }
    }
    (void)File_Close(f);
    return FM_ERR_NOT_FOUND;
}

/* "<path>.fm_tmp", the file the three editing functions below build the
 * new content in before it replaces `path`.  The name has to fit as a
 * whole.  It used to be cut to the buffer: for a path of 153 to 159
 * characters the "temporary" name was then a prefix of ".fm_tmp" short
 * of the real one -- at 159 characters `path` itself, so the function
 * truncated the file it was reading and then deleted it; a few
 * characters shorter it was the name of whatever other file is called
 * "<path>." or "<path>.fm", which was overwritten and deleted. */
static FM_Result text_tmp_name(const char *path, char *tmp, size_t tmp_size)
{
    static const char suffix[] = ".fm_tmp";
    size_t len = strlen(path);

    if (len + sizeof(suffix) > tmp_size)
    {
        FM_Core_SetLastError(FM_ERR_TOO_LARGE);
        return FM_ERR_TOO_LARGE;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, suffix, sizeof(suffix));
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Streaming replace                                                   */
/* ------------------------------------------------------------------ */
FM_Result Text_Replace(const char *path, const char *old_sub,
                       const char *new_sub)
{
    char tmp[FM_PATH_MAX];
    char line[FM_INTERNAL_BUFFER_SIZE];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    size_t old_len;
    FM_Result r;

    if (path == NULL || old_sub == NULL || new_sub == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (old_sub[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    old_len = strlen(old_sub);

    r = text_tmp_name(path, tmp, sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }

    src = File_Open(path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        return r;
    }

    for (;;)
    {
        size_t line_len = 0;
        size_t pos = 0;

        r = Text_ReadLine(src, line, sizeof(line), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }

        /* replace all occurrences within this line (in buffer) */
        while (pos + old_len <= line_len)
        {
            if (strncmp(line + pos, old_sub, old_len) == 0)
            {
                size_t new_len = strlen(new_sub);
                size_t rem = line_len - (pos + old_len);
                size_t new_total = pos + new_len + rem;

                if (new_total + 1 > sizeof(line))
                {
                    r = FM_ERR_TOO_LARGE;
                    goto fail;
                }
                memmove(line + pos + new_len, line + pos + old_len, rem);
                memcpy(line + pos, new_sub, new_len);
                line_len = new_total;
                line[line_len] = '\0';
                pos += new_len;
            }
            else
            {
                pos++;
            }
        }

        r = Text_WriteLine(dst, line);
        if (r != FM_OK)
        {
            goto fail;
        }
    }

    r = File_Close(dst);
    dst = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Close(src);
    src = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Rename(tmp, path);
    /* Do not delete after ambiguous rename failure: the name may be new. */
    FM_Core_SetLastError(r);
    return r;

fail:
    if (dst != NULL)
    {
        (void)File_Close(dst);
    }
    if (src != NULL)
    {
        (void)File_Close(src);
    }
    (void)File_Delete(tmp);
    FM_Core_SetLastError(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Line insert / delete                                                */
/* ------------------------------------------------------------------ */
FM_Result Text_InsertLine(const char *path, size_t at_line, const char *line)
{
    char tmp[FM_PATH_MAX];
    char buf[FM_INTERNAL_BUFFER_SIZE];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    size_t n = 0;
    FM_Result r;

    if (path == NULL || line == NULL || at_line == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    r = text_tmp_name(path, tmp, sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }

    src = File_Open(path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        return r;
    }

    for (;;)
    {
        size_t line_len = 0;

        r = Text_ReadLine(src, buf, sizeof(buf), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == at_line)
        {
            r = Text_WriteLine(dst, line);
            if (r != FM_OK)
            {
                goto fail;
            }
        }
        r = Text_WriteLine(dst, buf);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
    if (n < at_line)
    {
        /* append at end */
        r = Text_WriteLine(dst, line);
        if (r != FM_OK)
        {
            goto fail;
        }
    }

    r = File_Close(dst);
    dst = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Close(src);
    src = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Rename(tmp, path);
    /* Do not delete after ambiguous rename failure: the name may be new. */
    FM_Core_SetLastError(r);
    return r;

fail:
    if (dst != NULL)
    {
        (void)File_Close(dst);
    }
    if (src != NULL)
    {
        (void)File_Close(src);
    }
    (void)File_Delete(tmp);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result Text_DeleteLine(const char *path, size_t line)
{
    char tmp[FM_PATH_MAX];
    char buf[FM_INTERNAL_BUFFER_SIZE];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    size_t n = 0;
    FM_Result r;

    if (path == NULL || line == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    r = text_tmp_name(path, tmp, sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }

    src = File_Open(path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        return r;
    }

    for (;;)
    {
        size_t line_len = 0;

        r = Text_ReadLine(src, buf, sizeof(buf), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == line)
        {
            continue; /* skip */
        }
        r = Text_WriteLine(dst, buf);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
    if (n < line)
    {
        r = FM_ERR_NOT_FOUND;
        goto fail;
    }

    r = File_Close(dst);
    dst = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Close(src);
    src = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Rename(tmp, path);
    /* Do not delete after ambiguous rename failure: the name may be new. */
    FM_Core_SetLastError(r);
    return r;

fail:
    if (dst != NULL)
    {
        (void)File_Close(dst);
    }
    if (src != NULL)
    {
        (void)File_Close(src);
    }
    (void)File_Delete(tmp);
    FM_Core_SetLastError(r);
    return r;
}

#endif /* FM_USE_TEXT */
