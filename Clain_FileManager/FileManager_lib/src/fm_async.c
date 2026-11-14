/*
 * FileManager - Cooperative asynchronous operations
 */

#define FM_DEBUG_TAG "async"
#include "fm_debug.h"
#include "fm_async.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_ASYNC

enum
{
    OP_NONE = 0,
    OP_COPY = 1,
    OP_MOVE = 2,
    OP_DELETE = 3,
    OP_SCAN = 4
};

#define FM_ASYNC_MAGIC ((uint32_t)0x464D4153u) /* "FMAS" */

static FM_Result async_release_files(FM_Async *a);

static void async_finish(FM_Async *a, FM_AsyncState state, FM_Result result)
{
    FM_Result cleanup = async_release_files(a);

    if (result == FM_OK && cleanup != FM_OK)
    {
        state = FM_ASYNC_ERROR;
        result = cleanup;
    }
    if (a->temp_owned)
    {
        (void)File_Delete(a->temp_path);
        a->temp_owned = 0;
    }
    a->state = state;
    a->result = result;
    if (a->done_cb != NULL)
    {
        a->done_cb(a, result, a->user_data);
    }
}

static FM_Result async_release_files(FM_Async *a)
{
    FM_Result r = FM_OK;

    if (File_IsOpen(&a->srcf))
    {
        r = File_Close(&a->srcf);
    }
    if (File_IsOpen(&a->dstf))
    {
        FM_Result r2 = File_Close(&a->dstf);

        if (r == FM_OK)
        {
            r = r2;
        }
    }
    if (a->scan_dir.used)
    {
        FM_Result r2 = Directory_Close(&a->scan_dir);

        if (r == FM_OK)
        {
            r = r2;
        }
    }
    return r;
}

void Async_Init(FM_Async *a)
{
    if (a != NULL)
    {
        memset(a, 0, sizeof(*a));
        a->magic = FM_ASYNC_MAGIC;
        a->state = FM_ASYNC_IDLE;
    }
}

FM_Result Async_Reset(FM_Async *a)
{
    if (a == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->magic == FM_ASYNC_MAGIC && a->state == FM_ASYNC_RUNNING)
    {
        return FM_ERR_BUSY;
    }
    if (a->magic == FM_ASYNC_MAGIC)
    {
        (void)async_release_files(a);
    }
    Async_Init(a);
    return FM_OK;
}

FM_Result Async_Cancel(FM_Async *a)
{
    if (a == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->magic != FM_ASYNC_MAGIC)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (a->state == FM_ASYNC_RUNNING)
    {
        a->cancel = FM_TRUE;
    }
    return FM_OK;
}

FM_Bool Async_IsDone(FM_Async *a)
{
    if (a == NULL || a->magic != FM_ASYNC_MAGIC)
    {
        return FM_TRUE;
    }
    return (a->state == FM_ASYNC_DONE || a->state == FM_ASYNC_ERROR ||
            a->state == FM_ASYNC_CANCELLED) ? FM_TRUE : FM_FALSE;
}

FM_AsyncState Async_GetState(const FM_Async *a)
{
    if (a == NULL || a->magic != FM_ASYNC_MAGIC)
    {
        return FM_ASYNC_IDLE;
    }
    return a->state;
}

FM_Result Async_GetResult(const FM_Async *a)
{
    if (a == NULL || a->magic != FM_ASYNC_MAGIC)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return a->result;
}

/* ------------------------------------------------------------------ */
/* Copy / move                                                         */
/* ------------------------------------------------------------------ */
static FM_AsyncState async_poll_copy(FM_Async *a)
{
    if (!File_IsOpen(&a->srcf))
    {
        FM_Result r;
        FM_MountEntry *ms = NULL, *md = NULL;
        char rs[FM_PATH_MAX], rd[FM_PATH_MAX];
        static const char suffix[] = ".fm_tmp";

        /* Same-volume move is a rename, which also allows the FAT adapter
         * to resolve 8.3 aliases without truncating and deleting the source. */
        if (a->op == OP_MOVE && FM_VFS_Pin(a->src, &ms, rs, sizeof(rs)) == FM_OK)
        {
            r = FM_VFS_Resolve(a->dst, &md, rd, sizeof(rd));
            if (r == FM_OK && ms == md && ms->fs->ops.rename != NULL)
            {
                FM_VFS_Unpin(ms);
                r = File_Move(a->src, a->dst, NULL, NULL);
                async_finish(a, r == FM_OK ? FM_ASYNC_DONE : FM_ASYNC_ERROR, r);
                return a->state;
            }
            FM_VFS_Unpin(ms);
        }
        if (strlen(a->dst) + sizeof(suffix) > sizeof(a->temp_path))
        {
            async_finish(a, FM_ASYNC_ERROR, FM_ERR_TOO_LARGE);
            return a->state;
        }
        strcpy(a->temp_path, a->dst);
        strcat(a->temp_path, suffix);
        if (FM_File_SamePath(a->src, a->temp_path))
        {
            async_finish(a, FM_ASYNC_ERROR, FM_ERR_INVALID_ARGUMENT);
            return a->state;
        }
        r = File_OpenEx(&a->srcf, a->src, FM_OPEN_READ);

        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        r = File_OpenEx(&a->dstf, a->temp_path,
                        FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        a->temp_owned = 1;
        r = File_Size(&a->srcf, &a->total);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        a->phase = 1;
    }

    if (a->phase == 1)
    {
        uint32_t rd = 0;
        FM_Result r = File_Read(&a->srcf, a->chunk, FM_ASYNC_CHUNK_SIZE,
                                &rd);

        if (r == FM_ERR_EOF || (r == FM_OK && rd == 0))
        {
            r = File_Flush(&a->dstf);
            if (r != FM_OK)
            {
                async_finish(a, FM_ASYNC_ERROR, r);
                return a->state;
            }
            r = async_release_files(a);
            if (r != FM_OK)
            {
                async_finish(a, FM_ASYNC_ERROR, r);
                return a->state;
            }
            r = File_Rename(a->temp_path, a->dst);
            if (r != FM_OK)
            {
                /* A failed rename may have committed; the old name can
                 * now belong to somebody else. Leave it untouched. */
                a->temp_owned = 0;
                async_finish(a, FM_ASYNC_ERROR, r);
                return a->state;
            }
            a->temp_owned = 0;
            if (a->op == OP_MOVE)
            {
                r = File_Delete(a->src);
                if (r != FM_OK)
                {
                    async_finish(a, FM_ASYNC_ERROR, r);
                    return a->state;
                }
            }
            async_finish(a, FM_ASYNC_DONE, FM_OK);
            return a->state;
        }
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        {
            uint32_t off = 0;

            while (off < rd)
            {
                uint32_t wr = 0;
                uint32_t remaining = rd - off;

                r = File_Write(&a->dstf, a->chunk + off, remaining, &wr);
                if (r != FM_OK)
                {
                    async_finish(a, FM_ASYNC_ERROR, r);
                    return a->state;
                }
                if (wr == 0)
                {
                    async_finish(a, FM_ASYNC_ERROR, FM_ERR_IO);
                    return a->state;
                }
                off += wr;
            }
        }
        a->current += rd;
        if (a->progress_cb != NULL)
        {
            a->progress_cb(a, a->current, a->total, a->user_data);
        }
    }
    return a->state;
}

/* ------------------------------------------------------------------ */
/* Recursive directory traversal                                      */
/* ------------------------------------------------------------------ */
static FM_Result async_rebuild_path(FM_Async *a)
{
    char buf[FM_PATH_MAX];
    uint32_t i;

    if (strlen(a->src) >= sizeof(buf))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(buf, a->src);
    for (i = 0; i < a->depth; i++)
    {
        char next[FM_PATH_MAX];

        if (Path_Join(buf, a->stack[i], next, sizeof(next)) != FM_OK)
        {
            return FM_ERR_TOO_LARGE;
        }
        (void)strcpy(buf, next);
    }
    (void)strcpy(a->scan_path, buf);
    return FM_OK;
}

/* Open the directory represented by depth.  When resume is true, skip
 * entries already consumed before descending into a child directory. */
static FM_Result async_open_dir(FM_Async *a, FM_Bool resume)
{
    FM_Result r;
    uint32_t skip = resume ? a->scan_index[a->depth] : 0;

    if (a->scan_dir.used)
    {
        return FM_ERR_BUSY;
    }
    r = async_rebuild_path(a);
    if (r != FM_OK)
    {
        return r;
    }
    r = Directory_OpenEx(&a->scan_dir, a->scan_path);
    if (r != FM_OK)
    {
        return r;
    }
    while (skip > 0)
    {
        FM_DirEntry ignored;

        r = Directory_Read(&a->scan_dir, &ignored);
        if (r != FM_OK)
        {
            (void)Directory_Close(&a->scan_dir);
            return (r == FM_ERR_EOF) ? FM_ERR_IO : r;
        }
        skip--;
    }
    return FM_OK;
}

static FM_Result async_enter_child(FM_Async *a, const char *name)
{
    if (a->depth >= FM_DIR_MAX_DEPTH)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (a->scan_dir.used)
    {
        FM_Result r = Directory_Close(&a->scan_dir);

        if (r != FM_OK)
        {
            return r;
        }
    }
    (void)strncpy(a->stack[a->depth], name, FM_NAME_MAX - 1);
    a->stack[a->depth][FM_NAME_MAX - 1] = '\0';
    a->depth++;
    a->scan_index[a->depth] = 0;
    return async_open_dir(a, FM_FALSE);
}

static FM_Result async_return_parent(FM_Async *a)
{
    FM_Result r;

    if (a->scan_dir.used)
    {
        r = Directory_Close(&a->scan_dir);
        if (r != FM_OK)
        {
            return r;
        }
    }
    if (a->depth == 0)
    {
        return FM_OK;
    }
    a->depth--;
    return async_open_dir(a, FM_TRUE);
}

/* A delete walk removes every entry it has consumed.  Reopening a parent
 * therefore must start at the beginning; skipping an old entry count would
 * skip live entries because the deleted ones are no longer returned. */
static FM_Result async_return_delete_parent(FM_Async *a)
{
    if (a->depth == 0)
    {
        return FM_OK;
    }
    a->depth--;
    a->scan_index[a->depth] = 0;
    return async_open_dir(a, FM_FALSE);
}

static FM_AsyncState async_poll_delete(FM_Async *a)
{
    FM_Result r;

    if (!a->scan_dir.used)
    {
        r = async_open_dir(a, FM_FALSE);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
    }

    r = Directory_Read(&a->scan_dir, &a->scan_entry);
    if (r == FM_ERR_EOF)
    {
        if (a->scan_dir.used)
        {
            (void)Directory_Close(&a->scan_dir);
        }
        if (a->depth == 0)
        {
            r = Directory_Delete(a->scan_path);
            if (r != FM_OK)
            {
                async_finish(a, FM_ASYNC_ERROR, r);
                return a->state;
            }
            async_finish(a, FM_ASYNC_DONE, FM_OK);
            return a->state;
        }
        r = Directory_Delete(a->scan_path);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        a->current++;
        if (a->progress_cb != NULL)
        {
            a->progress_cb(a, a->current, a->total, a->user_data);
        }
        r = async_return_delete_parent(a);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
        }
        return a->state;
    }
    if (r != FM_OK)
    {
        async_finish(a, FM_ASYNC_ERROR, r);
        return a->state;
    }
    a->scan_index[a->depth]++;
    {
        char full[FM_PATH_MAX];

        if (Path_Join(a->scan_path, a->scan_entry.name, full,
                      sizeof(full)) != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, FM_ERR_TOO_LARGE);
            return a->state;
        }
        if (a->scan_entry.type == FM_ITEM_DIR)
        {
            r = async_enter_child(a, a->scan_entry.name);
        }
        else
        {
            /* Keep the directory handle open while removing a file.  This
             * preserves the iterator position and avoids replaying entries
             * after the directory contents change. */
            r = File_Delete(full);
            if (r == FM_OK)
            {
                a->current++;
                if (a->progress_cb != NULL)
                {
                    a->progress_cb(a, a->current, a->total, a->user_data);
                }
            }
        }
    }
    if (r != FM_OK)
    {
        async_finish(a, FM_ASYNC_ERROR, r);
    }
    return a->state;
}

static FM_AsyncState async_poll_scan(FM_Async *a)
{
    FM_Result r;

    if (!a->scan_dir.used)
    {
        r = async_open_dir(a, FM_FALSE);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
    }

    r = Directory_Read(&a->scan_dir, &a->scan_entry);
    if (r == FM_ERR_EOF)
    {
        r = async_return_parent(a);
        if (r != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, r);
            return a->state;
        }
        if (a->depth == 0 && !a->scan_dir.used)
        {
            async_finish(a, FM_ASYNC_DONE, FM_OK);
        }
        return a->state;
    }
    if (r != FM_OK)
    {
        async_finish(a, FM_ASYNC_ERROR, r);
        return a->state;
    }
    a->scan_index[a->depth]++;
    {
        FM_FileInfo info;

        memset(&info, 0, sizeof(info));
        (void)strncpy(info.name, a->scan_entry.name, sizeof(info.name) - 1);
        info.size = a->scan_entry.size;
        info.type = a->scan_entry.type;
        info.attributes = a->scan_entry.attributes;
        info.modified = a->scan_entry.timestamp;
        if (Path_Join(a->scan_path, a->scan_entry.name, info.path,
                      sizeof(info.path)) != FM_OK)
        {
            async_finish(a, FM_ASYNC_ERROR, FM_ERR_TOO_LARGE);
            return a->state;
        }
        if (a->item_cb != NULL)
        {
            r = a->item_cb(&info, a->user_data);
            if (r != FM_OK)
            {
                async_finish(a,
                             (r == FM_ERR_CANCELLED) ? FM_ASYNC_CANCELLED
                                                      : FM_ASYNC_ERROR,
                             r);
                return a->state;
            }
        }
        if (info.type == FM_ITEM_DIR)
        {
            r = async_enter_child(a, a->scan_entry.name);
            if (r != FM_OK)
            {
                async_finish(a, FM_ASYNC_ERROR, r);
            }
            return a->state;
        }
    }
    a->current++;
    if (a->progress_cb != NULL)
    {
        a->progress_cb(a, a->current, a->total, a->user_data);
    }
    return a->state;
}

/* ------------------------------------------------------------------ */
/* Poll and start helpers                                              */
/* ------------------------------------------------------------------ */
FM_AsyncState Async_Poll(FM_Async *a)
{
    if (a == NULL || a->magic != FM_ASYNC_MAGIC)
    {
        return FM_ASYNC_IDLE;
    }
    if (a->state == FM_ASYNC_IDLE || a->state != FM_ASYNC_RUNNING)
    {
        return a->state;
    }
    if (a->cancel)
    {
        async_finish(a, FM_ASYNC_CANCELLED, FM_ERR_CANCELLED);
        return a->state;
    }

    switch (a->op)
    {
    case OP_COPY:
    case OP_MOVE:
        return async_poll_copy(a);
    case OP_DELETE:
        return async_poll_delete(a);
    case OP_SCAN:
        return async_poll_scan(a);
    default:
        async_finish(a, FM_ASYNC_ERROR, FM_ERR_INTERNAL);
        return a->state;
    }
}

static FM_Result async_set_path(char *dst, const char *src)
{
    if (dst == NULL || src == NULL || strlen(src) >= FM_PATH_MAX)
    {
        return FM_ERR_INVALID_PATH;
    }
    (void)strcpy(dst, src);
    if (Path_Normalize(dst, FM_PATH_MAX) != FM_OK || !Path_IsAbsolute(dst))
    {
        return FM_ERR_INVALID_PATH;
    }
    return FM_OK;
}

static FM_Result async_start(FM_Async *a, uint8_t op, const char *src,
                             const char *dst, FM_AsyncDoneCb done,
                             FM_AsyncProgressCb progress, void *user_data)
{
    FM_Result r;

    if (a == NULL || src == NULL ||
        ((op == OP_COPY || op == OP_MOVE) && dst == NULL))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Async_Reset(a);
    if (r != FM_OK)
    {
        return r;
    }
    a->op = op;
    r = async_set_path(a->src, src);
    if (r != FM_OK)
    {
        return r;
    }
    if (op == OP_DELETE && strcmp(a->src, "/") == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (dst != NULL)
    {
        r = async_set_path(a->dst, dst);
        if (r != FM_OK)
        {
            return r;
        }
        if ((op == OP_COPY || op == OP_MOVE) &&
            (strcmp(a->src, a->dst) == 0 || FM_File_SamePath(a->src, a->dst)))
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
    }
    a->done_cb = done;
    a->progress_cb = progress;
    a->user_data = user_data;
    a->state = FM_ASYNC_RUNNING;
    a->result = FM_OK;
    a->cancel = FM_FALSE;
    return FM_OK;
}

FM_Result Async_StartCopy(FM_Async *a, const char *src, const char *dst,
                          FM_AsyncDoneCb done, FM_AsyncProgressCb progress,
                          void *user_data)
{
    return async_start(a, OP_COPY, src, dst, done, progress, user_data);
}

FM_Result Async_StartMove(FM_Async *a, const char *src, const char *dst,
                          FM_AsyncDoneCb done, FM_AsyncProgressCb progress,
                          void *user_data)
{
    return async_start(a, OP_MOVE, src, dst, done, progress, user_data);
}

FM_Result Async_StartDeleteRecursive(FM_Async *a, const char *path,
                                     FM_AsyncDoneCb done,
                                     FM_AsyncProgressCb progress,
                                     void *user_data)
{
    return async_start(a, OP_DELETE, path, NULL, done, progress, user_data);
}

FM_Result Async_StartScan(FM_Async *a, const char *path,
                          FM_ItemCallback item_cb, FM_AsyncDoneCb done,
                          void *user_data)
{
    FM_Result r = async_start(a, OP_SCAN, path, NULL, done, NULL, user_data);

    if (r == FM_OK)
    {
        a->item_cb = item_cb;
    }
    return r;
}

#endif /* FM_USE_ASYNC */
