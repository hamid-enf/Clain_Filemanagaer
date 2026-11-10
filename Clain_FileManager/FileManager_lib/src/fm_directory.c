/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_directory.c - Directory management & scanning
 */

#define FM_DEBUG_TAG "dir"
#include "fm_debug.h"
#include "fm_internal.h"
#include <string.h>

/* The adapter directory handles live in the byte array FM_Dir.fh; the
 * member offset must keep them naturally aligned (see fm_file.c). */
typedef char fm_dir_fh_aligned[
    ((offsetof(FM_Dir, fh) % 4) == 0) ? 1 : -1];

static FM_Dir g_dir_pool[FM_MAX_OPEN_DIRS];
static FM_Dir *g_external_dirs[FM_DIR_MAX_DEPTH + 1];

/* Backend calls on an open directory handle, made with the mount's
 * backend lock held (see fm_file.c / FM_Mount_Lock).  The handle keeps
 * its mount alive, so d->mount / d->fs are stable. */
static FM_Result dop_close(FM_Dir *d)
{
    FM_Result r;

    FM_Mount_Lock(d->mount);
    r = d->fs->ops.dir_close(d->fs, d->fh);
    FM_Mount_Unlock(d->mount);
    return r;
}

static FM_Result dop_read(FM_Dir *d, FM_DirEntry *entry)
{
    FM_Result r;

    FM_Mount_Lock(d->mount);
    r = d->fs->ops.dir_read(d->fs, d->fh, entry);
    FM_Mount_Unlock(d->mount);
    return r;
}

static FM_Result dop_open(FM_Dir *d, const char *rel)
{
    FM_Result r;

    FM_Mount_Lock(d->mount);
    r = d->fs->ops.dir_open(d->fs, d->fh, rel);
    FM_Mount_Unlock(d->mount);
    return r;
}

void Directory_Init(FM_Dir *d)
{
    if (d != NULL)
    {
        memset(d, 0, sizeof(*d));
    }
}

static void external_dir_remove(FM_Dir *d)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_DIR_MAX_DEPTH + 1u; i++)
    {
        if (g_external_dirs[i] == d)
        {
            g_external_dirs[i] = NULL;
            FM_Unlock();
            return;
        }
    }
    FM_Unlock();
}

static FM_Bool dir_is_pool(const FM_Dir *d)
{
    uintptr_t p;
    uintptr_t first;
    uintptr_t last;

    if (d == NULL)
    {
        return FM_FALSE;
    }
    p = (uintptr_t)(const void *)d;
    first = (uintptr_t)(const void *)&g_dir_pool[0];
    last = (uintptr_t)(const void *)&g_dir_pool[FM_MAX_OPEN_DIRS];
    return p >= first && p < last;
}

/* Release a pool slot under the VFS lock so a concurrent Directory_Open
 * can never observe a half-released handle. */
static void dir_release(FM_Dir *d)
{
    if (d == NULL)
    {
        return;
    }
    FM_Lock();
    if (d->used)
    {
        d->used = 0;
    }
    FM_Unlock();
}

/* ------------------------------------------------------------------ */
/* Pool helpers                                                        */
/* ------------------------------------------------------------------ */
static FM_Dir *dir_acquire(void)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_DIRS; i++)
    {
        if (!g_dir_pool[i].used)
        {
            memset(&g_dir_pool[i], 0, sizeof(FM_Dir));
            g_dir_pool[i].used = 1; /* claim the slot while opening */
            FM_Unlock();
            return &g_dir_pool[i];
        }
    }
    FM_Unlock();
    return NULL;
}

FM_Bool FM_Dir_AnyOpenOnMount(const FM_MountEntry *mount)
{
    uint32_t i;

    /* Called while the VFS lock is held by FS_Unmount. */
    for (i = 0; i < FM_MAX_OPEN_DIRS; i++)
    {
        if (g_dir_pool[i].used && g_dir_pool[i].mount == (void *)mount)
        {
            return FM_TRUE;
        }
    }
    for (i = 0; i < FM_DIR_MAX_DEPTH + 1u; i++)
    {
        if (g_external_dirs[i] != NULL && g_external_dirs[i]->used &&
            g_external_dirs[i]->mount == (void *)mount)
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* Basic operations                                                    */
/* ------------------------------------------------------------------ */
FM_Result Directory_Create(const char *path)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_Result r;

    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
        return r;
    }
    if (mount->fs->ops.mkdir == NULL)
    {
        FM_VFS_Unpin(mount);
        return FM_ERR_UNSUPPORTED;
    }

    /* Fast path: try the full path first. */
    FM_FS_CALL(mount, r, mkdir(mount->fs, rel));
    if (r == FM_OK || r != FM_ERR_NOT_FOUND)
    {
        FM_VFS_Unpin(mount);
        FM_Core_SetLastError(r == FM_ERR_EXISTS ? FM_OK : r);
        return (r == FM_ERR_EXISTS) ? FM_OK : r;
    }

    /* Recursive: create each path component. */
    {
        size_t i = 0;

        while (rel[i] != '\0')
        {
            while (rel[i] == '/') i++;
            if (rel[i] == '\0') break;

            /* Advance to next separator or end. */
            while (rel[i] != '\0' && rel[i] != '/') i++;

            /* Build prefix up to position i. */
            {
                char prefix[FM_PATH_MAX];
                size_t k;

                for (k = 0; k < i && k + 1 < sizeof(prefix); k++)
                {
                    prefix[k] = rel[k];
                }
                prefix[k] = '\0';

                FM_FS_CALL(mount, r, mkdir(mount->fs, prefix));
                if (r != FM_OK && r != FM_ERR_EXISTS)
                {
                    FM_VFS_Unpin(mount);
                    FM_Core_SetLastError(r);
                    return r;
                }
            }
        }
    }

    FM_VFS_Unpin(mount);
    FM_Core_SetLastError(FM_OK);
    return FM_OK;
}

FM_Result Directory_Delete(const char *path)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_Result r;

    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        return r;
    }
    if (rel[0] == '\0')
    {
        /* Deleting the mount root itself is never meaningful. */
        FM_VFS_Unpin(mount);
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (mount->fs->ops.rmdir == NULL)
    {
        FM_VFS_Unpin(mount);
        return FM_ERR_UNSUPPORTED;
    }
    FM_FS_CALL(mount, r, rmdir(mount->fs, rel));
    FM_VFS_Unpin(mount);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result Directory_Exists(const char *path, FM_Bool *exists)
{
    FM_FileInfo info;
    FM_Result r;

    if (exists == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_GetInfo(path, &info);
    if (r == FM_OK)
    {
        *exists = (info.type == FM_ITEM_DIR) ? FM_TRUE : FM_FALSE;
        return FM_OK;
    }
    if (r == FM_ERR_NOT_FOUND)
    {
        *exists = FM_FALSE;
        return FM_OK;
    }
    return r;
}

FM_Result Directory_Rename(const char *old_path, const char *new_path)
{
    FM_MountEntry *m1 = NULL;
    FM_MountEntry *m2 = NULL;
    char r1[FM_PATH_MAX];
    char r2[FM_PATH_MAX];
    FM_Result r;

    r = FM_VFS_Pin(old_path, &m1, r1, sizeof(r1));
    if (r != FM_OK)
    {
        return r;
    }
    r = FM_VFS_Resolve(new_path, &m2, r2, sizeof(r2));
    if (r != FM_OK)
    {
        FM_VFS_Unpin(m1);
        return r;
    }
    if (m1 != m2 || m1->fs->ops.rename == NULL)
    {
        FM_VFS_Unpin(m1);
        return FM_ERR_UNSUPPORTED;
    }
    FM_FS_CALL(m1, r, rename(m1->fs, r1, r2));
    FM_VFS_Unpin(m1);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result Directory_GetInfo(const char *path, FM_FileInfo *info)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_Result r;

    if (info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        return r;
    }
    if (mount->fs->ops.stat == NULL)
    {
        FM_VFS_Unpin(mount);
        return FM_ERR_UNSUPPORTED;
    }
    memset(info, 0, sizeof(*info));
    FM_FS_CALL(mount, r, stat(mount->fs, rel, info));
    FM_VFS_Unpin(mount);
    if (r == FM_OK)
    {
        char normalized[FM_PATH_MAX];

        if (strlen(path) < sizeof(normalized))
        {
            (void)strcpy(normalized, path);
            if (Path_Normalize(normalized, sizeof(normalized)) == FM_OK)
            {
                (void)strcpy(info->path, normalized);
            }
        }
    }
    FM_Core_SetLastError(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Listing                                                             */
/* ------------------------------------------------------------------ */
FM_Result Directory_OpenEx(FM_Dir *d, const char *path)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    char normalized[FM_PATH_MAX];
    FM_Result r;
    FM_Bool from_pool;

    if (d == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    from_pool = dir_is_pool(d);
    if (!from_pool)
    {
        uint32_t i;
        FM_Bool registered = FM_FALSE;

        if (d->used)
        {
            return FM_ERR_BUSY;
        }
        memset(d, 0, sizeof(*d));

        /* Register the user handle BEFORE the backend open: the registry
         * is what FS_Unmount consults, so a handle entered only after
         * its directory is open could lose the filesystem in between.
         * Pool slots are already claimed (used=1) by dir_acquire. */
        FM_Lock();
        for (i = 0; i < FM_DIR_MAX_DEPTH + 1u; i++)
        {
            if (g_external_dirs[i] == NULL)
            {
                g_external_dirs[i] = d;
                d->used = 1;
                registered = FM_TRUE;
                break;
            }
        }
        FM_Unlock();
        if (!registered)
        {
            return FM_ERR_NO_MEMORY;
        }
    }

    /* The mount stays pinned from the lookup until the handle itself
     * keeps it alive (FM_Dir_AnyOpenOnMount). */
    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r == FM_OK)
    {
        if (mount->fs->ops.dir_open == NULL)
        {
            r = FM_ERR_UNSUPPORTED;
        }
        else if (strlen(path) >= sizeof(normalized))
        {
            r = FM_ERR_TOO_LARGE;
        }
        else
        {
            (void)strcpy(normalized, path);
            if (Path_Normalize(normalized, sizeof(normalized)) != FM_OK)
            {
                r = FM_ERR_INVALID_PATH;
            }
        }
        if (r == FM_OK)
        {
            FM_Lock();
            d->mount = (void *)mount;
            d->fs = mount->fs;
            FM_Unlock();
            r = dop_open(d, rel);
        }
        FM_VFS_Unpin(mount);
    }
    if (r != FM_OK)
    {
        if (from_pool)
        {
            dir_release(d);
        }
        else
        {
            external_dir_remove(d);
            memset(d, 0, sizeof(*d));
        }
        return r;
    }
    (void)strcpy(d->path, normalized);
    (void)strcpy(d->resolved, rel);
    return FM_OK;
}

FM_Dir *Directory_Open(const char *path)
{
    FM_Dir *d;
    FM_Result r;

    /* Validate BEFORE claiming a pool slot: Directory_OpenEx rejects a
     * NULL path without releasing the handle it was given, so every
     * Directory_Open(NULL) -- and every Directory_List / Count / Size
     * (NULL, ...) built on it -- used to leak one of the
     * FM_MAX_OPEN_DIRS slots for good. */
    if (path == NULL)
    {
        FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    d = dir_acquire();
    if (d == NULL)
    {
        FM_Core_SetLastError(FM_ERR_NO_MEMORY);
        return NULL;
    }
    r = Directory_OpenEx(d, path);
    if (r != FM_OK)
    {
        /* Directory_OpenEx already released the slot */
        FM_Core_SetLastError(r);
        return NULL;
    }
    FM_Core_SetLastError(FM_OK);
    return d;
}

FM_Result Directory_Close(FM_Dir *d)
{
    FM_Result r = FM_OK;

    if (d == NULL || !d->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (d->fs != NULL && d->fs->ops.dir_close != NULL)
    {
        r = dop_close(d);
    }
    if (dir_is_pool(d))
    {
        dir_release(d);
    }
    else
    {
        external_dir_remove(d);
        memset(d, 0, sizeof(*d));
    }
    return r;
}

FM_Result Directory_Read(FM_Dir *d, FM_DirEntry *entry)
{
    if (d == NULL || !d->used || entry == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (d->fs->ops.dir_read == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    memset(entry, 0, sizeof(*entry));
    return dop_read(d, entry);
}

FM_Result Directory_Rewind(FM_Dir *d)
{
    if (d == NULL || !d->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* Re-open the directory on the same handle. */
    {
        FM_Result r;

        if (d->fs->ops.dir_close != NULL)
        {
            (void)dop_close(d);
        }
        r = dop_open(d, d->resolved);
        if (r != FM_OK)
        {
            if (dir_is_pool(d))
            {
                dir_release(d);
            }
            else
            {
                external_dir_remove(d);
                memset(d, 0, sizeof(*d));
            }
        }
        return r;
    }
}

FM_Result Directory_Count(const char *path, uint8_t type_filter,
                          uint32_t *count)
{
    FM_Dir *d;
    FM_DirEntry e;
    FM_Result r;
    uint32_t n = 0;

    if (count == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    d = Directory_Open(path);
    if (d == NULL)
    {
        return File_GetLastError();
    }
    for (;;)
    {
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
        if (type_filter != 0 && e.type != type_filter)
        {
            continue;
        }
        n++;
    }
    (void)Directory_Close(d);
    *count = n;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Wildcard matching ('*' only)                                        */
/* ------------------------------------------------------------------ */
static FM_Bool wildcard_match(const char *pattern, const char *text)
{
    while (*pattern != '\0')
    {
        if (*pattern == '*')
        {
            /* collapse consecutive stars */
            while (pattern[1] == '*')
            {
                pattern++;
            }
            if (pattern[1] == '\0')
            {
                return FM_TRUE;
            }
            {
                const char *t = text;

                while (*t != '\0')
                {
                    if (wildcard_match(pattern + 1, t))
                    {
                        return FM_TRUE;
                    }
                    t++;
                }
            }
            return wildcard_match(pattern + 1, text);
        }
        if (*text == '\0' || *text != *pattern)
        {
            return FM_FALSE;
        }
        pattern++;
        text++;
    }
    return *text == '\0';
}

/* ------------------------------------------------------------------ */
/* Listing with filter                                                 */
/* ------------------------------------------------------------------ */
typedef struct
{
    const char *filter;
    FM_ItemCallback callback;
    void *user_data;
} DirectoryListCtx;

static FM_Result directory_list_cb(const FM_FileInfo *info, void *user_data)
{
    DirectoryListCtx *ctx = (DirectoryListCtx *)user_data;

    if (ctx->filter != NULL && ctx->filter[0] != '\0' &&
        !wildcard_match(ctx->filter, info->name))
    {
        return FM_OK;
    }
    return ctx->callback(info, ctx->user_data);
}

FM_Result Directory_List(const char *path, const char *filter,
                         FM_ItemCallback callback, void *user_data)
{
    DirectoryListCtx ctx;

    if (path == NULL || callback == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ctx.filter = filter;
    ctx.callback = callback;
    ctx.user_data = user_data;
    return Directory_ScanRecursive(path, directory_list_cb, NULL, &ctx, NULL);
}

/* ------------------------------------------------------------------ */
/* Recursive scan                                                      */
/* ------------------------------------------------------------------ */
static FM_Result scan_recursive(const char *path, FM_ItemCallback callback,
                                FM_ProgressCallback progress,
                                void *user_data, volatile FM_Bool *cancel,
                                uint32_t depth, uint64_t *processed)
{
    FM_Dir d;
    FM_DirEntry e;
    FM_Result r;

    if (depth > FM_DIR_MAX_DEPTH)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (cancel != NULL && *cancel)
    {
        return FM_ERR_CANCELLED;
    }

    Directory_Init(&d);
    r = Directory_OpenEx(&d, path);
    if (r != FM_OK)
    {
        return r;
    }
    for (;;)
    {
        FM_FileInfo info;

        memset(&e, 0, sizeof(e));
        r = Directory_Read(&d, &e);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)Directory_Close(&d);
            return r;
        }
        if (cancel != NULL && *cancel)
        {
            (void)Directory_Close(&d);
            return FM_ERR_CANCELLED;
        }

        memset(&info, 0, sizeof(info));
        (void)strcpy(info.name, e.name);
        info.size = e.size;
        info.type = e.type;
        info.attributes = e.attributes;
        info.modified = e.timestamp;
        {
            char full[FM_PATH_MAX];

            if (Path_Join(path, e.name, full, sizeof(full)) != FM_OK)
            {
                (void)Directory_Close(&d);
                return FM_ERR_TOO_LARGE;
            }
            (void)strcpy(info.path, full);
        }

        if (e.type == FM_ITEM_DIR)
        {
            r = callback(&info, user_data);
            if (r != FM_OK)
            {
                (void)Directory_Close(&d);
                return r;
            }
            r = scan_recursive(info.path, callback, progress, user_data,
                               cancel, depth + 1, processed);
            if (r != FM_OK)
            {
                (void)Directory_Close(&d);
                return r;
            }
        }
        else
        {
            r = callback(&info, user_data);
            if (r != FM_OK)
            {
                (void)Directory_Close(&d);
                return r;
            }
            if (processed != NULL)
            {
                *processed += e.size;
            }
            if (progress != NULL)
            {
                progress(*processed, 0, user_data);
            }
        }
    }
    (void)Directory_Close(&d);
    return FM_OK;
}

FM_Result Directory_ScanRecursive(const char *path, FM_ItemCallback callback,
                                  FM_ProgressCallback progress,
                                  void *user_data,
                                  volatile FM_Bool *cancel)
{
    FM_FileInfo root;
    FM_Result r;
    uint64_t processed = 0;

    if (callback == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_GetInfo(path, &root);
    if (r == FM_OK && root.type != FM_ITEM_DIR)
    {
        return FM_ERR_NOT_DIRECTORY;
    }
    if (r != FM_OK)
    {
        return r;
    }
    return scan_recursive(path, callback, progress, user_data, cancel, 0,
                          &processed);
}

/* ------------------------------------------------------------------ */
/* Recursive delete                                                    */
/* ------------------------------------------------------------------ */
static FM_Result delete_recursive(const char *path, uint32_t depth)
{
    FM_Dir d;
    FM_DirEntry e;
    FM_Result r;

    if (depth > FM_DIR_MAX_DEPTH)
    {
        return FM_ERR_TOO_LARGE;
    }
    Directory_Init(&d);
    r = Directory_OpenEx(&d, path);
    if (r != FM_OK)
    {
        return r;
    }
    for (;;)
    {
        r = Directory_Read(&d, &e);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)Directory_Close(&d);
            return r;
        }
        {
            char full[FM_PATH_MAX];

            if (Path_Join(path, e.name, full, sizeof(full)) != FM_OK)
            {
                (void)Directory_Close(&d);
                return FM_ERR_TOO_LARGE;
            }
            if (e.type == FM_ITEM_DIR)
            {
                r = delete_recursive(full, depth + 1);
            }
            else
            {
                r = File_Delete(full);
            }
            if (r != FM_OK)
            {
                (void)Directory_Close(&d);
                return r;
            }
        }
    }
    (void)Directory_Close(&d);
    return Directory_Delete(path);
}

FM_Result Directory_DeleteRecursive(const char *path)
{
    char normalized[FM_PATH_MAX];

    if (path == NULL || strlen(path) >= sizeof(normalized))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)strcpy(normalized, path);
    if (Path_Normalize(normalized, sizeof(normalized)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }
    if (strcmp(normalized, "/") == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return delete_recursive(normalized, 0);
}

/* ------------------------------------------------------------------ */
/* Copy / move trees                                                   */
/* ------------------------------------------------------------------ */
/* Each batch keeps the stack footprint small while still copying large
 * directories in a single pass. */
#define DIR_COPY_BATCH 8

/* Copy a directory tree.  A naive recursive copy opens the source
 * directory and keeps it open while recursing into every subfolder, so a
 * tree nested deeper than the directory-handle pool ("FM_MAX_OPEN_DIRS",
 * default 4) exhausts the backends' open-directory slots and fails with
 * FM_ERR_NO_MEMORY.  Instead we read the entries of one level in small
 * batches, close the source handle, copy the batch (recursing for
 * subfolders — the handle is already closed, so recursion pinpoints no
 * extra slot), then reopen and skip the already-copied entries for the
 * next batch.  At any moment at most one directory handle is held, so the
 * copy works for any depth up to FM_DIR_MAX_DEPTH. */
static FM_Result dir_copy_recursive(const char *src, const char *dst,
                                    FM_ProgressCallback progress,
                                    void *user_data, uint32_t depth)
{
    FM_Dir d;
    FM_Result r;

    if (depth > FM_DIR_MAX_DEPTH)
    {
        return FM_ERR_TOO_LARGE;
    }
    /* src را اول باز کن: اگر مبدأ وجود ندارد، پوشهٔ مقصد نسازیم و
     * نیمه‌کاره جا نگذاریم (قبلاً dst ساخته می‌شد و بعد خطا می‌آمد). */
    Directory_Init(&d);
    r = Directory_OpenEx(&d, src);
    if (r != FM_OK)
    {
        return r;
    }
    (void)Directory_Close(&d);

    r = Directory_Create(dst);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        return r;
    }

    {
        uint32_t processed = 0;
        FM_Bool done = FM_FALSE;

        while (!done)
        {
            FM_DirEntry batch[DIR_COPY_BATCH];
            uint32_t n = 0;
            uint32_t i;
            uint32_t skipped;

            Directory_Init(&d);
            r = Directory_OpenEx(&d, src);
            if (r != FM_OK)
            {
                return r;
            }

            /* Skip the entries already copied in earlier batches. */
            skipped = 0;
            while (skipped < processed)
            {
                FM_DirEntry tmp;

                r = Directory_Read(&d, &tmp);
                if (r == FM_ERR_EOF)
                {
                    break; /* fewer entries than expected */
                }
                if (r != FM_OK)
                {
                    (void)Directory_Close(&d);
                    return r;
                }
                skipped++;
            }

            /* Collect the next batch. */
            while (n < DIR_COPY_BATCH)
            {
                r = Directory_Read(&d, &batch[n]);
                if (r == FM_ERR_EOF)
                {
                    done = FM_TRUE;
                    break;
                }
                if (r != FM_OK)
                {
                    (void)Directory_Close(&d);
                    return r;
                }
                n++;
            }

            /* Close the handle before processing so the recursive call for
             * a subfolder never holds an ancestor directory handle. */
            (void)Directory_Close(&d);

            for (i = 0; i < n; i++)
            {
                char sfull[FM_PATH_MAX];
                char dfull[FM_PATH_MAX];

                if (Path_Join(src, batch[i].name, sfull, sizeof(sfull)) != FM_OK ||
                    Path_Join(dst, batch[i].name, dfull, sizeof(dfull)) != FM_OK)
                {
                    return FM_ERR_TOO_LARGE;
                }
                if (batch[i].type == FM_ITEM_DIR)
                {
                    r = dir_copy_recursive(sfull, dfull, progress, user_data,
                                           depth + 1);
                }
                else
                {
                    r = File_Copy(sfull, dfull, progress, user_data);
                }
                if (r != FM_OK)
                {
                    return r;
                }
                processed++;
            }
        }
    }
    return FM_OK;
}

/* How `b` relates to directory `a`: 0 unrelated, 1 the same path,
 * 2 `b` lies inside `a`.  Both are normalised first, so "/x/", "/x/."
 * and "/x" compare equal. */
static FM_Result dir_relation(const char *a, const char *b, int *rel)
{
    char na[FM_PATH_MAX];
    char nb[FM_PATH_MAX];
    size_t la;

    *rel = 0;
    if (a == NULL || b == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(a) >= sizeof(na) || strlen(b) >= sizeof(nb))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(na, a);
    (void)strcpy(nb, b);
    if (Path_Normalize(na, sizeof(na)) != FM_OK ||
        Path_Normalize(nb, sizeof(nb)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }
    la = strlen(na);
    while (la > 1u && na[la - 1u] == '/')
    {
        na[--la] = '\0';
    }
    {
        size_t lb = strlen(nb);

        while (lb > 1u && nb[lb - 1u] == '/')
        {
            nb[--lb] = '\0';
        }
    }
    if (strcmp(na, nb) == 0)
    {
        *rel = 1;
    }
    else if (strcmp(na, "/") == 0 ||
             (strncmp(nb, na, la) == 0 && nb[la] == '/'))
    {
        *rel = 2;
    }
    return FM_OK;
}

FM_Result Directory_Copy(const char *src, const char *dst,
                         FM_ProgressCallback progress, void *user_data)
{
    FM_Bool existed = FM_FALSE;
    int rel = 0;
    FM_Result r;

    /* A tree copied into itself never ends: every pass finds the folder
     * the previous pass created, until the path limit or the disk stops
     * it. */
    r = dir_relation(src, dst, &rel);
    if (r == FM_OK && rel != 0)
    {
        r = FM_ERR_INVALID_ARGUMENT;
    }
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
        return r;
    }
    if (Directory_Exists(dst, &existed) != FM_OK)
    {
        existed = FM_TRUE;              /* unknown: never clean up */
    }
    r = dir_copy_recursive(src, dst, progress, user_data, 0);
    if (r != FM_OK && !existed)
    {
        /* The destination did not exist before, so everything in it is
         * the unfinished copy: do not leave half a tree behind.  (When
         * copying INTO an existing folder nothing is removed -- there
         * is no telling which files were already there.) */
        (void)Directory_DeleteRecursive(dst);
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result Directory_Move(const char *src, const char *dst,
                         FM_ProgressCallback progress, void *user_data)
{
    FM_MountEntry *m1 = NULL;
    FM_MountEntry *m2 = NULL;
    char r1[FM_PATH_MAX];
    char r2[FM_PATH_MAX];
    FM_Result r;
    int rel = 0;

    r = dir_relation(src, dst, &rel);
    if (r == FM_OK && rel == 2)
    {
        r = FM_ERR_INVALID_ARGUMENT;    /* a folder cannot move into itself */
    }
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
        return r;
    }
    if (rel == 1)
    {
        /* onto itself: nothing to do, as long as it exists */
        FM_Bool there = FM_FALSE;

        r = Directory_Exists(src, &there);
        if (r == FM_OK && !there)
        {
            r = FM_ERR_NOT_FOUND;
        }
        FM_Core_SetLastError(r);
        return r;
    }
    if (FM_VFS_Pin(src, &m1, r1, sizeof(r1)) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (FM_VFS_Resolve(dst, &m2, r2, sizeof(r2)) == FM_OK && m1 == m2 &&
        m1->fs->ops.rename != NULL)
    {
        FM_FS_CALL(m1, r, rename(m1->fs, r1, r2));
        FM_VFS_Unpin(m1);
        FM_Core_SetLastError(r);
        return r;
    }
    FM_VFS_Unpin(m1);
    r = Directory_Copy(src, dst, progress, user_data);
    if (r != FM_OK)
    {
        return r;
    }
    /* The copy is complete.  If removing the source now fails part way,
     * the destination is the only place that still has everything, so
     * it must stay: the old "rollback" deleted it, and with it every
     * file that had already been removed from the source.  The caller
     * gets the error; the source may be partly gone, the destination is
     * whole. */
    r = Directory_DeleteRecursive(src);
    FM_Core_SetLastError(r);
    return r;
}

typedef struct
{
    uint64_t total;
} FM_SizeCtx;

static FM_Result size_cb(const FM_FileInfo *info, void *user_data)
{
    FM_SizeCtx *ctx = (FM_SizeCtx *)user_data;

    if (info->type == FM_ITEM_FILE)
    {
        ctx->total += info->size;
    }
    return FM_OK;
}

FM_Result Directory_Size(const char *path, uint64_t *size)
{
    FM_SizeCtx ctx;
    FM_Result r;

    if (size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ctx.total = 0;
    r = scan_recursive(path, size_cb, NULL, &ctx, NULL, 0, NULL);
    if (r != FM_OK)
    {
        return r;
    }
    *size = ctx.total;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */
static int name_cmp(const FM_DirEntry *a, const FM_DirEntry *b)
{
    return strcmp(a->name, b->name);
}

void Directory_SortByName(FM_DirEntry *entries, uint32_t count)
{
    uint32_t i;

    for (i = 1; i < count; i++)
    {
        FM_DirEntry key = entries[i];
        uint32_t j = i;

        while (j > 0 && name_cmp(&entries[j - 1], &key) > 0)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

void Directory_SortBySize(FM_DirEntry *entries, uint32_t count)
{
    uint32_t i;

    for (i = 1; i < count; i++)
    {
        FM_DirEntry key = entries[i];
        uint32_t j = i;

        while (j > 0 && entries[j - 1].size > key.size)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}
