/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_safe.c - Crash-safe / power-loss safe operations
 *
 * Atomic replace: write "<path>.fm_tmp", flush, rename over target.
 * Recovery: File_Recover removes stale temp files.
 */

#define FM_DEBUG_TAG "safe"
#include "fm_debug.h"
#include "fm_safe.h"
#include "fm_file.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_SAFE_WRITE

static FM_Result tmp_path_for(const char *path, char *tmp, size_t tmp_size)
{
    if (path == NULL || tmp == NULL || tmp_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(path) + strlen(".fm_tmp") + 1 > tmp_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(tmp, path);
    (void)strcat(tmp, ".fm_tmp");
    return FM_OK;
}

static FM_Result write_all_through(FM_File *f, const void *data, size_t size)
{
    size_t off = 0;

    while (off < size)
    {
        size_t chunk = size - off;
        uint32_t wr = 0;
        FM_Result r;

        if (chunk > 0xFFFFu)
        {
            chunk = 0xFFFFu;
        }
        r = File_Write(f, (const uint8_t *)data + off, (uint32_t)chunk, &wr);
        if (r != FM_OK)
        {
            return r;
        }
        if (wr == 0)
        {
            return FM_ERR_IO;
        }
        off += wr;
    }
    return FM_OK;
}

FM_Result File_SafeReplace(const char *path, const void *data, size_t size)
{
    char tmp[FM_PATH_MAX];
    FM_File *f;
    FM_Result r;

    if (path == NULL || (data == NULL && size > 0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    r = tmp_path_for(path, tmp, sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }

    /* Clean a stale temp from a previous crash. */
    {
        FM_Bool exists = FM_FALSE;

        if (File_Exists(tmp, &exists) == FM_OK && exists)
        {
            (void)File_Delete(tmp);
        }
    }

    f = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    r = write_all_through(f, data, size);
    if (r == FM_OK)
    {
        r = File_Flush(f);
    }
    if (r == FM_OK)
    {
        r = File_Close(f);
    }
    else
    {
        (void)File_Close(f);
        (void)File_Delete(tmp);
        FM_Core_SetLastError(r);
        return r;
    }

    /* Atomic swap: rename tmp over the target.  Adapters that support
     * rename-over (POSIX, FMFS) replace atomically. */
    r = File_Rename(tmp, path);
    if (r != FM_OK)
    {
        (void)File_Delete(tmp);
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_SafeWriteText(const char *path, const char *text)
{
    if (text == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return File_SafeReplace(path, text, strlen(text));
}

FM_Result File_AtomicWrite(const char *path, const void *data, size_t size)
{
    return File_SafeReplace(path, data, size);
}

FM_Result File_SafeAppend(const char *path, const void *data, size_t size)
{
    /* Crash-safe append without read-modify-write: append directly but
     * flush immediately so a power loss cannot silently lose the record
     * beyond the filesystem's own journaling guarantees. */
    FM_File *f;
    FM_Result r;

    if (path == NULL || (data == NULL && size > 0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    f = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_APPEND);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    r = write_all_through(f, data, size);
    if (r == FM_OK)
    {
        r = File_Flush(f);
    }
    {
        FM_Result c = File_Close(f);

        if (r == FM_OK)
        {
            r = c;
        }
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_Recover(const char *path)
{
    /* Remove a stale temp file, if any.  Also walks up to the parent
     * directory for robustness is not needed: temp name is derived
     * deterministically from the target path. */
    char tmp[FM_PATH_MAX];
    FM_Bool exists = FM_FALSE;
    FM_Result r;

    r = tmp_path_for(path, tmp, sizeof(tmp));
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Exists(tmp, &exists);
    if (r != FM_OK)
    {
        return r;
    }
    if (exists)
    {
        return File_Delete(tmp);
    }
    return FM_OK;
}

FM_Result File_Transaction(const char *path, const void *data, size_t size)
{
    /* Backup the original, safe-replace, restore on failure. */
    char bak[FM_PATH_MAX];
    FM_Bool existed = FM_FALSE;
    FM_Result r;

    r = tmp_path_for(path, bak, sizeof(bak));
    if (r != FM_OK)
    {
        return r;
    }
    /* use a distinct backup suffix */
    if (strlen(bak) + strlen(".bak") + 1 > sizeof(bak))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcat(bak, ".bak");

    r = File_Exists(path, &existed);
    if (r != FM_OK)
    {
        return r;
    }
    if (existed)
    {
        r = File_Copy(path, bak, NULL, NULL);
        if (r != FM_OK)
        {
            return r;
        }
    }

    r = File_SafeReplace(path, data, size);
    if (r != FM_OK)
    {
        if (existed)
        {
            /* restore original from backup */
            (void)File_Copy(bak, path, NULL, NULL);
        }
        (void)File_Delete(bak);
        FM_Core_SetLastError(r);
        return r;
    }

    if (existed)
    {
        (void)File_Delete(bak);
    }
    return FM_OK;
}

#endif /* FM_USE_SAFE_WRITE */
