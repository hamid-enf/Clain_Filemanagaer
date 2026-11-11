/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_explorer.c - File explorer / navigation system
 */

#define FM_DEBUG_TAG "explorer"
#include "fm_debug.h"
#include "fm_explorer.h"
#include "fm_path.h"
#include <string.h>

#if FM_USE_EXPLORER

static FM_Bool ext_filter_match(const FM_Explorer *ex, const char *name)
{
    if (ex->filter_ext[0] == '\0')
    {
        return FM_TRUE;
    }
    {
        const char *dot = strrchr(name, '.');
        size_t flen;
        size_t elen;
        size_t i;

        if (dot == NULL)
        {
            return FM_FALSE;
        }
        dot++;
        flen = strlen(ex->filter_ext);
        elen = strlen(dot);
        if (elen != flen)
        {
            return FM_FALSE;
        }
        for (i = 0; i < elen; i++)
        {
            char a = dot[i];
            char b = ex->filter_ext[i];

            if (a >= 'A' && a <= 'Z')
            {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b)
            {
                return FM_FALSE;
            }
        }
    }
    return FM_TRUE;
}

static FM_Result explorer_load(FM_Explorer *ex)
{
    FM_Dir *d;
    FM_DirEntry e;
    FM_Result r;
    uint32_t n = 0;

    ex->count = 0;
    ex->cursor = 0;
    ex->truncated = FM_FALSE;

    d = Directory_Open(ex->cwd);
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
        if (ex->only_files && e.type != FM_ITEM_FILE)
        {
            continue;
        }
        if (ex->only_dirs && e.type != FM_ITEM_DIR)
        {
            continue;
        }
        if (!ext_filter_match(ex, e.name))
        {
            continue;
        }
        if (n >= FM_MAX_EXPLORER_ITEMS)
        {
            /* more than fits: say so (Explorer_IsTruncated) instead of
             * presenting the first part as the whole directory */
            ex->truncated = FM_TRUE;
            break;
        }
        ex->items[n++] = e;
    }
    (void)Directory_Close(d);

    if (ex->sort == FM_EXPLORER_SORT_NAME)
    {
        Directory_SortByName(ex->items, n);
    }
    else if (ex->sort == FM_EXPLORER_SORT_SIZE)
    {
        Directory_SortBySize(ex->items, n);
    }
    ex->count = n;
    return FM_OK;
}

FM_Result Explorer_Open(FM_Explorer *ex, const char *path)
{
    FM_Result r;

    if (ex == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(ex, 0, sizeof(*ex));
    (void)strncpy(ex->cwd, path, sizeof(ex->cwd) - 1);
    ex->cwd[sizeof(ex->cwd) - 1] = '\0';
    if (Path_Normalize(ex->cwd, sizeof(ex->cwd)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }
    r = explorer_load(ex);
    return r;
}

FM_Result Explorer_Refresh(FM_Explorer *ex)
{
    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return explorer_load(ex);
}

FM_Result Explorer_Up(FM_Explorer *ex)
{
    char parent[FM_PATH_MAX];

    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(ex->cwd, "/") == 0)
    {
        return FM_OK; /* already at root */
    }
    if (Path_GetParent(ex->cwd, parent, sizeof(parent)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }
    (void)strcpy(ex->cwd, parent);
    return explorer_load(ex);
}

FM_Result Explorer_Enter(FM_Explorer *ex)
{
    const FM_DirEntry *item;
    char full[FM_PATH_MAX];

    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    item = Explorer_GetCurrentItem(ex);
    if (item == NULL || item->type != FM_ITEM_DIR)
    {
        return FM_ERR_NOT_DIRECTORY;
    }
    if (Path_Join(ex->cwd, item->name, full, sizeof(full)) != FM_OK)
    {
        return FM_ERR_TOO_LARGE;
    }
    /* push history; when it is full the oldest entry goes, so that
     * Explorer_Back still leads to where this step came from (it used
     * to skip the push, and Back then jumped several levels up) */
    if (ex->depth >= FM_EXPLORER_MAX_DEPTH)
    {
        memmove(ex->history[0], ex->history[1],
                sizeof(ex->history) - sizeof(ex->history[0]));
        ex->depth = FM_EXPLORER_MAX_DEPTH - 1u;
    }
    (void)strcpy(ex->history[ex->depth], ex->cwd);
    ex->depth++;
    (void)strcpy(ex->cwd, full);
    return explorer_load(ex);
}

FM_Result Explorer_Back(FM_Explorer *ex)
{
    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (ex->depth == 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    ex->depth--;
    (void)strncpy(ex->cwd, ex->history[ex->depth], sizeof(ex->cwd) - 1);
    return explorer_load(ex);
}

FM_Result Explorer_Next(FM_Explorer *ex)
{
    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (ex->count == 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (ex->cursor + 1 < ex->count)
    {
        ex->cursor++;
    }
    else
    {
        ex->cursor = 0; /* wrap */
    }
    return FM_OK;
}

FM_Result Explorer_Prev(FM_Explorer *ex)
{
    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (ex->count == 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (ex->cursor > 0)
    {
        ex->cursor--;
    }
    else
    {
        ex->cursor = ex->count - 1;
    }
    return FM_OK;
}

FM_Result Explorer_GetCurrentPath(const FM_Explorer *ex, char *out,
                                  size_t out_size)
{
    if (ex == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(ex->cwd) + 1 > out_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strncpy(out, ex->cwd, out_size - 1);
    out[out_size - 1] = '\0';
    return FM_OK;
}

const FM_DirEntry *Explorer_GetCurrentItem(const FM_Explorer *ex)
{
    if (ex == NULL || ex->count == 0 || ex->cursor >= ex->count)
    {
        return NULL;
    }
    return &ex->items[ex->cursor];
}

const FM_DirEntry *Explorer_GetItem(const FM_Explorer *ex, uint32_t index)
{
    if (ex == NULL || index >= ex->count)
    {
        return NULL;
    }
    return &ex->items[index];
}

uint32_t Explorer_GetItemCount(const FM_Explorer *ex)
{
    if (ex == NULL)
    {
        return 0;
    }
    return ex->count;
}

FM_Bool Explorer_IsDir(const FM_Explorer *ex, uint32_t index)
{
    const FM_DirEntry *item = Explorer_GetItem(ex, index);

    if (item == NULL)
    {
        return FM_FALSE;
    }
    return (item->type == FM_ITEM_DIR) ? FM_TRUE : FM_FALSE;
}

FM_Bool Explorer_IsTruncated(const FM_Explorer *ex)
{
    return (ex != NULL && ex->truncated) ? FM_TRUE : FM_FALSE;
}

FM_Result Explorer_SetFilters(FM_Explorer *ex, FM_Bool only_files,
                              FM_Bool only_dirs, const char *ext,
                              FM_ExplorerSort sort)
{
    if (ex == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ex->only_files = only_files;
    ex->only_dirs = only_dirs;
    if (ext != NULL)
    {
        (void)strncpy(ex->filter_ext, ext, sizeof(ex->filter_ext) - 1);
        ex->filter_ext[sizeof(ex->filter_ext) - 1] = '\0';
    }
    else
    {
        ex->filter_ext[0] = '\0';
    }
    ex->sort = sort;
    return explorer_load(ex);
}

FM_Result Explorer_Select(FM_Explorer *ex, uint32_t index)
{
    if (ex == NULL || index >= ex->count)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ex->cursor = index;
    return FM_OK;
}

#endif /* FM_USE_EXPLORER */
