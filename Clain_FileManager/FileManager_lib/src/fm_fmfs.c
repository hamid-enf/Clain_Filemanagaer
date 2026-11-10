/*
 * FileManager - FMFS built-in storage filesystem
 *
 * FMFS is a deliberately small, allocation-free filesystem adapter.  It
 * stores a fixed metadata table in the beginning of the bound storage and
 * appends file data after that table.  It is not intended to compete with
 * FAT/LittleFS on very large media; its value is that the same simple
 * high-level file/directory API works immediately on a caller-supplied memory
 * address or on any storage driver implementing FM_Storage callbacks.
 */

#define FM_DEBUG_TAG "fmfs"
#include "fm_debug.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_FMFS

#define FMFS_MAGIC "FMFS"
#define FMFS_VERSION 1u
#define FMFS_HEADER_SIZE 32u
#define FMFS_RECORD_SIZE (FM_PATH_MAX + 20u)
#define FMFS_METADATA_SIZE (FMFS_HEADER_SIZE + \
                            (FM_FMFS_MAX_ENTRIES * FMFS_RECORD_SIZE))

/* Internal copy/zero buffer kept small so FMFS functions work safely on
 * MCUs with tight stacks (e.g. 2 KB).  These helpers call raw storage
 * callbacks directly and therefore cannot reuse FM_InternalBuffer, which
 * may already be owned by a higher-level caller. */
#ifndef FMFS_COPY_BUF_SIZE
#define FMFS_COPY_BUF_SIZE 256u
#endif

typedef struct
{
    uint8_t  used;
    uint8_t  type;
    char     path[FM_PATH_MAX];
    uint64_t offset;
    uint64_t size;
} FMFS_Entry;

typedef struct
{
    FM_FS       *owner;
    FM_Storage  *storage;
    uint64_t     data_start;
    uint64_t     data_next;
    FMFS_Entry   entries[FM_FMFS_MAX_ENTRIES];
    /* RAM only: bumped whenever a table slot is freed.  An open handle
     * remembers the value it was opened with, so a handle whose file
     * was deleted (or replaced by a rename) can never read or write the
     * unrelated file that reuses the slot later. */
    uint32_t     gen[FM_FMFS_MAX_ENTRIES];
    /* RAM only: the table in RAM may differ from the one on the
     * storage (a write is in progress, or the last save failed). */
    uint8_t      dirty;
} FMFS_Context;

typedef struct
{
    uint16_t index;
    uint8_t  mode;
    uint8_t  used;
    uint32_t gen;
    uint64_t pos;
} FMFS_FileHandle;

typedef struct
{
    char     path[FM_PATH_MAX];
    uint32_t cursor;
    uint8_t  used;
} FMFS_DirHandle;

typedef char fmfs_file_handle_fits[
    (sizeof(FMFS_FileHandle) <= FM_FS_HANDLE_SIZE) ? 1 : -1];
typedef char fmfs_dir_handle_fits[
    (sizeof(FMFS_DirHandle) <= FM_FS_DIRHANDLE_SIZE) ? 1 : -1];

static FMFS_Context g_fmfs[FM_MAX_FS];

static uint16_t fmfs_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint64_t fmfs_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < 8; i++)
    {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static void fmfs_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void fmfs_wr64(uint8_t *p, uint64_t v)
{
    uint32_t i;

    for (i = 0; i < 8; i++)
    {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static FM_Result fmfs_read(FMFS_Context *ctx, uint64_t offset, void *data,
                           uint32_t size)
{
    if (size == 0)
    {
        return FM_OK;
    }
    if (ctx->storage == NULL || ctx->storage->read == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    return ctx->storage->read(ctx->storage, offset, data, size);
}

static FM_Result fmfs_write(FMFS_Context *ctx, uint64_t offset,
                            const void *data, uint32_t size)
{
    if (size == 0)
    {
        return FM_OK;
    }
    if (ctx->storage == NULL || ctx->storage->write == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    return ctx->storage->write(ctx->storage, offset, data, size);
}

static FM_Result fmfs_range(FMFS_Context *ctx, uint64_t offset,
                            uint64_t size)
{
    if (offset > ctx->storage->capacity ||
        size > ctx->storage->capacity - offset)
    {
        return FM_ERR_NO_SPACE;
    }
    return FM_OK;
}

static FMFS_Context *fmfs_ctx(FM_FS *fs)
{
    return (FMFS_Context *)fs->user;
}

static FM_Result fmfs_save(FMFS_Context *ctx);

static void fmfs_bump_all(FMFS_Context *ctx)
{
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        ctx->gen[i]++;
    }
}

/* The table entry behind an open handle, or NULL when the file is gone. */
static FMFS_Entry *fmfs_handle_entry(FMFS_Context *ctx,
                                     const FMFS_FileHandle *handle)
{
    FMFS_Entry *entry;

    if (handle->index >= FM_FMFS_MAX_ENTRIES)
    {
        return NULL;
    }
    entry = &ctx->entries[handle->index];
    if (!entry->used || entry->type != FM_ITEM_FILE ||
        ctx->gen[handle->index] != handle->gen)
    {
        return NULL;
    }
    return entry;
}

/* Free one slot and persist; on a failed save the slot is put back so
 * the table in RAM keeps describing what is actually on the storage. */
static FM_Result fmfs_remove_entry(FMFS_Context *ctx, int index)
{
    FMFS_Entry saved = ctx->entries[index];
    FM_Result r;

    memset(&ctx->entries[index], 0, sizeof(ctx->entries[index]));
    r = fmfs_save(ctx);
    if (r != FM_OK)
    {
        ctx->entries[index] = saved;
        return r;
    }
    ctx->gen[index]++;
    return FM_OK;
}

static int fmfs_find(FMFS_Context *ctx, const char *path)
{
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (ctx->entries[i].used && strcmp(ctx->entries[i].path, path) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static FM_Bool fmfs_is_child_of(const char *parent, const char *path)
{
    size_t plen = strlen(parent);

    if (path[0] == '\0')
    {
        return FM_FALSE;
    }
    if (plen == 0)
    {
        return strchr(path, '/') == NULL ? FM_TRUE : FM_FALSE;
    }
    return strncmp(path, parent, plen) == 0 && path[plen] == '/' &&
           path[plen + 1] != '\0' && strchr(path + plen + 1, '/') == NULL;
}

static FM_Bool fmfs_has_child(FMFS_Context *ctx, const char *parent)
{
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (ctx->entries[i].used && fmfs_is_child_of(parent,
                                                       ctx->entries[i].path))
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

static FM_Result fmfs_normalize_rel(const char *path, char *out,
                                    size_t out_size)
{
    char temp[FM_PATH_MAX];

    if (path == NULL || out == NULL || out_size == 0 ||
        strlen(path) >= sizeof(temp))
    {
        return FM_ERR_INVALID_PATH;
    }
    (void)strcpy(temp, path);
    while (temp[0] == '/')
    {
        (void)memmove(temp, temp + 1, strlen(temp));
    }
    if (temp[0] == '\0')
    {
        if (out_size < 1) return FM_ERR_TOO_LARGE;
        out[0] = '\0';
        return FM_OK;
    }
    if (Path_Normalize(temp, sizeof(temp)) != FM_OK ||
        strcmp(temp, ".") == 0 || Path_IsAbsolute(temp))
    {
        return FM_ERR_INVALID_PATH;
    }
    if (strlen(temp) + 1 > out_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(out, temp);
    return FM_OK;
}

static FM_Result fmfs_parent_path(const char *path, char *parent,
                                  size_t parent_size)
{
    const char *slash;
    size_t len;

    slash = strrchr(path, '/');
    if (slash == NULL)
    {
        if (parent_size == 0) return FM_ERR_TOO_LARGE;
        parent[0] = '\0';
        return FM_OK;
    }
    len = (size_t)(slash - path);
    if (len + 1 > parent_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    return FM_OK;
}

static FM_Result fmfs_save(FMFS_Context *ctx)
{
    uint8_t header[FMFS_HEADER_SIZE];
    uint32_t i;
    uint16_t count = 0;

    ctx->dirty = 1;
    if (fmfs_range(ctx, 0, FMFS_METADATA_SIZE) != FM_OK)
    {
        return FM_ERR_NO_SPACE;
    }

    /* Write the entry records first and the header LAST: if power or a
     * reset interrupts the save, the storage still holds the previous
     * (self-consistent) header describing older-but-valid metadata,
     * instead of a new header pointing at half-written records. */
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        uint8_t record[FMFS_RECORD_SIZE];
        uint64_t offset = FMFS_HEADER_SIZE +
                          (uint64_t)i * FMFS_RECORD_SIZE;

        memset(record, 0, sizeof(record));
        record[0] = ctx->entries[i].used;
        record[1] = ctx->entries[i].type;
        fmfs_wr64(record + 4, ctx->entries[i].offset);
        fmfs_wr64(record + 12, ctx->entries[i].size);
        (void)strncpy((char *)(record + 20), ctx->entries[i].path,
                      FM_PATH_MAX - 1);
        if (fmfs_write(ctx, offset, record, sizeof(record)) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }

    memset(header, 0, sizeof(header));
    memcpy(header, FMFS_MAGIC, 4);
    fmfs_wr16(header + 4, FMFS_VERSION);
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (ctx->entries[i].used) count++;
    }
    fmfs_wr16(header + 6, count);
    fmfs_wr64(header + 8, ctx->data_start);
    fmfs_wr64(header + 16, ctx->data_next);
    fmfs_wr64(header + 24, ctx->storage->capacity);
    if (fmfs_write(ctx, 0, header, sizeof(header)) != FM_OK)
    {
        return FM_ERR_IO;
    }
    ctx->dirty = 0;
    return FM_OK;
}

static FM_Result fmfs_reset(FMFS_Context *ctx)
{
    memset(ctx->entries, 0, sizeof(ctx->entries));
    fmfs_bump_all(ctx);
    ctx->data_start = FMFS_METADATA_SIZE;
    ctx->data_next = ctx->data_start;
    ctx->entries[0].used = 1;
    ctx->entries[0].type = FM_ITEM_DIR;
    ctx->entries[0].path[0] = '\0';
    return fmfs_save(ctx);
}

static FM_Result fmfs_load(FMFS_Context *ctx)
{
    uint8_t header[FMFS_HEADER_SIZE];
    uint32_t i;

    if (fmfs_read(ctx, 0, header, sizeof(header)) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (memcmp(header, FMFS_MAGIC, 4) != 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (fmfs_rd16(header + 4) != FMFS_VERSION ||
        fmfs_rd64(header + 8) != FMFS_METADATA_SIZE ||
        fmfs_rd64(header + 24) != ctx->storage->capacity)
    {
        return FM_ERR_CORRUPTED;
    }
    ctx->data_start = fmfs_rd64(header + 8);
    ctx->data_next = fmfs_rd64(header + 16);
    if (ctx->data_next < ctx->data_start ||
        ctx->data_next > ctx->storage->capacity)
    {
        return FM_ERR_CORRUPTED;
    }
    memset(ctx->entries, 0, sizeof(ctx->entries));
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        uint8_t record[FMFS_RECORD_SIZE];
        uint64_t offset = FMFS_HEADER_SIZE +
                          (uint64_t)i * FMFS_RECORD_SIZE;

        if (fmfs_read(ctx, offset, record, sizeof(record)) != FM_OK)
        {
            return FM_ERR_IO;
        }
        ctx->entries[i].used = record[0] ? 1u : 0u;
        ctx->entries[i].type = record[1];
        ctx->entries[i].offset = fmfs_rd64(record + 4);
        ctx->entries[i].size = fmfs_rd64(record + 12);
        memcpy(ctx->entries[i].path, record + 20, FM_PATH_MAX);
        ctx->entries[i].path[FM_PATH_MAX - 1] = '\0';
        if (ctx->entries[i].used &&
            (ctx->entries[i].type != FM_ITEM_FILE &&
             ctx->entries[i].type != FM_ITEM_DIR))
        {
            return FM_ERR_CORRUPTED;
        }
        if (ctx->entries[i].used && ctx->entries[i].type == FM_ITEM_FILE &&
            (ctx->entries[i].offset > ctx->storage->capacity ||
             ctx->entries[i].size > ctx->storage->capacity -
                                    ctx->entries[i].offset))
        {
            return FM_ERR_CORRUPTED;
        }
    }
    if (!ctx->entries[0].used || ctx->entries[0].type != FM_ITEM_DIR ||
        ctx->entries[0].path[0] != '\0')
    {
        return FM_ERR_CORRUPTED;
    }
    return FM_OK;
}

static FM_Result fmfs_require_parent(FMFS_Context *ctx, const char *path)
{
    char parent[FM_PATH_MAX];
    int index;
    FM_Result r = fmfs_parent_path(path, parent, sizeof(parent));

    if (r != FM_OK)
    {
        return r;
    }
    index = fmfs_find(ctx, parent);
    if (index < 0 || ctx->entries[index].type != FM_ITEM_DIR)
    {
        return FM_ERR_NOT_FOUND;
    }
    return FM_OK;
}

static FM_Result fmfs_add(FMFS_Context *ctx, const char *path, uint8_t type,
                          int *index_out)
{
    uint32_t i;

    if (strlen(path) >= FM_PATH_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (fmfs_find(ctx, path) >= 0)
    {
        return FM_ERR_EXISTS;
    }
    if (fmfs_require_parent(ctx, path) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    for (i = 1; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (!ctx->entries[i].used)
        {
            memset(&ctx->entries[i], 0, sizeof(ctx->entries[i]));
            ctx->entries[i].used = 1;
            ctx->entries[i].type = type;
            (void)strcpy(ctx->entries[i].path, path);
            if (index_out != NULL) *index_out = (int)i;
            {
                FM_Result r = fmfs_save(ctx);

                if (r != FM_OK)
                {
                    /* not on the storage: not in the table either */
                    memset(&ctx->entries[i], 0, sizeof(ctx->entries[i]));
                }
                return r;
            }
        }
    }
    return FM_ERR_NO_MEMORY;
}

static FM_Result fmfs_copy_data(FMFS_Context *ctx, uint64_t dst,
                                uint64_t src, uint64_t size)
{
    uint8_t buffer[FMFS_COPY_BUF_SIZE];
    uint64_t done = 0;

    while (done < size)
    {
        uint32_t chunk = (uint32_t)((size - done < sizeof(buffer))
                                        ? size - done : sizeof(buffer));
        FM_Result r = fmfs_read(ctx, src + done, buffer, chunk);

        if (r != FM_OK) return r;
        r = fmfs_write(ctx, dst + done, buffer, chunk);
        if (r != FM_OK) return r;
        done += chunk;
    }
    return FM_OK;
}

static FM_Result fmfs_zero_data(FMFS_Context *ctx, uint64_t offset,
                                uint64_t size)
{
    uint8_t zeros[FMFS_COPY_BUF_SIZE];
    uint64_t done = 0;

    memset(zeros, 0, sizeof(zeros));
    while (done < size)
    {
        uint32_t chunk = (uint32_t)((size - done < sizeof(zeros))
                                        ? size - done : sizeof(zeros));
        FM_Result r = fmfs_write(ctx, offset + done, zeros, chunk);

        if (r != FM_OK) return r;
        done += chunk;
    }
    return FM_OK;
}

/* Compact the data area in place: slide every live file down so they sit
 * back to back from data_start, and pull data_next back to the end of
 * the last one.  The space of deleted, truncated and relocated files is
 * recovered.
 *
 * Files are moved in ASCENDING OFFSET order, never in entry-table order.
 * A slot's index says nothing about where its data lives (slots are
 * reused, and a file that grows is relocated to the end of the data
 * area), so walking the table could slide a file from high up down onto
 * a live file that has not been moved yet -- and destroy it.  In offset
 * order every move goes to an address at or below the file's own, and
 * everything below that address has already been moved.
 *
 * Open handles stay valid: they hold the entry index and a position,
 * not a storage offset.  Does not write the metadata (the caller saves
 * it together with its own change). */
static FM_Result fmfs_repack(FMFS_Context *ctx)
{
    uint64_t dest = ctx->data_start;
    uint64_t floor = 0;             /* offsets below this are done */
    FM_Result r;

    ctx->dirty = 1;

    for (;;)
    {
        FMFS_Entry *next = NULL;
        uint32_t i;

        for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
        {
            FMFS_Entry *e = &ctx->entries[i];

            if (e->used && e->type == FM_ITEM_FILE && e->size != 0u &&
                e->offset >= floor &&
                (next == NULL || e->offset < next->offset))
            {
                next = e;
            }
        }
        if (next == NULL)
        {
            break;
        }
        floor = next->offset + next->size;
        if (next->offset != dest)
        {
            /* dest < offset: a forward, chunk-by-chunk copy is safe even
             * when the two ranges overlap */
            r = fmfs_copy_data(ctx, dest, next->offset, next->size);
            if (r != FM_OK)
            {
                return r;
            }
            next->offset = dest;
        }
        dest += next->size;
    }
    /* wipe the freed tail so stale data doesn't linger */
    if (dest < ctx->data_next)
    {
        r = fmfs_zero_data(ctx, dest, ctx->data_next - dest);
        if (r != FM_OK)
        {
            return r;
        }
    }
    ctx->data_next = dest;
    return FM_OK;
}

static FM_Result fmfs_relocate_once(FMFS_Context *ctx, FMFS_Entry *entry,
                                    uint64_t new_size);

/* Exchange two equally long, non-overlapping byte ranges. */
static FM_Result fmfs_swap_data(FMFS_Context *ctx, uint64_t a, uint64_t b,
                                uint64_t size)
{
    uint8_t ba[FMFS_COPY_BUF_SIZE];
    uint8_t bb[FMFS_COPY_BUF_SIZE];
    uint64_t done = 0;

    while (done < size)
    {
        uint32_t chunk = (uint32_t)((size - done < sizeof(ba))
                                        ? size - done : sizeof(ba));
        FM_Result r = fmfs_read(ctx, a + done, ba, chunk);

        if (r == FM_OK) r = fmfs_read(ctx, b + done, bb, chunk);
        if (r == FM_OK) r = fmfs_write(ctx, a + done, bb, chunk);
        if (r == FM_OK) r = fmfs_write(ctx, b + done, ba, chunk);
        if (r != FM_OK) return r;
        done += chunk;
    }
    return FM_OK;
}

/* Turn the adjacent ranges [A: a_len][B: b_len] starting at `base` into
 * [B][A] without any spare room (block-swap rotation: exchange the
 * shorter range with the matching part of the longer one, which puts
 * the shorter one in its final place, and repeat on what is left). */
static FM_Result fmfs_rotate_data(FMFS_Context *ctx, uint64_t base,
                                  uint64_t a_len, uint64_t b_len)
{
    while (a_len != 0u && b_len != 0u)
    {
        FM_Result r;

        if (a_len <= b_len)
        {
            /* [A][B1][B2] -> [B1][A][B2]; B1 is final */
            r = fmfs_swap_data(ctx, base, base + a_len, a_len);
            base += a_len;
            b_len -= a_len;
        }
        else
        {
            /* [A1][A2][B] -> [B][A2][A1]; B is final */
            r = fmfs_swap_data(ctx, base, base + a_len, b_len);
            base += b_len;
            a_len -= b_len;
        }
        if (r != FM_OK)
        {
            return r;
        }
    }
    return FM_OK;
}

/* Make `entry` the last file of a freshly compacted data area, so it
 * can grow into the free tail.  Everything stored behind it moves down
 * by its size. */
static FM_Result fmfs_move_last(FMFS_Context *ctx, FMFS_Entry *entry)
{
    uint64_t start = entry->offset;
    uint64_t size = entry->size;
    uint64_t behind = ctx->data_next - (start + size);
    uint32_t i;
    FM_Result r;

    ctx->dirty = 1;
    r = fmfs_rotate_data(ctx, start, size, behind);
    if (r != FM_OK)
    {
        return r;
    }
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        FMFS_Entry *e = &ctx->entries[i];

        if (e->used && e->type == FM_ITEM_FILE && e != entry &&
            e->size != 0u && e->offset > start)
        {
            e->offset -= size;
        }
    }
    entry->offset = ctx->data_next - size;
    return FM_OK;
}

/* Give `entry` room for new_size bytes.  The data area is a bump
 * allocator, so "no space" at the tail usually only means that dead
 * space sits between the files.  Before reporting FM_ERR_NO_SPACE:
 * compact; then either move the file to the (now larger) tail, or --
 * when even that does not hold a second copy of it -- rotate it to the
 * end of the packed data and let it grow in place.  A write therefore
 * fails for lack of space only when the live data really does not fit. */
static FM_Result fmfs_relocate(FMFS_Context *ctx, FMFS_Entry *entry,
                               uint64_t new_size)
{
    FM_Result r = fmfs_relocate_once(ctx, entry, new_size);

    if (r == FM_ERR_NO_SPACE && fmfs_repack(ctx) == FM_OK)
    {
        FM_Bool last = (entry->size != 0u &&
                        entry->offset + entry->size == ctx->data_next)
                           ? FM_TRUE : FM_FALSE;

        if (!last && fmfs_range(ctx, ctx->data_next, new_size) == FM_OK)
        {
            return fmfs_relocate_once(ctx, entry, new_size);
        }
        if (!last && entry->size != 0u && new_size > entry->size &&
            fmfs_range(ctx, ctx->data_next,
                       new_size - entry->size) == FM_OK)
        {
            r = fmfs_move_last(ctx, entry);
            if (r != FM_OK)
            {
                return r;
            }
            last = FM_TRUE;
        }
        r = FM_ERR_NO_SPACE;
        if (last && new_size > entry->size &&
            fmfs_range(ctx, entry->offset, new_size) == FM_OK)
        {
            r = fmfs_zero_data(ctx, entry->offset + entry->size,
                               new_size - entry->size);
            if (r == FM_OK)
            {
                ctx->data_next = entry->offset + new_size;
                entry->size = new_size;
            }
        }
    }
    return r;
}

static FM_Result fmfs_relocate_once(FMFS_Context *ctx, FMFS_Entry *entry,
                                    uint64_t new_size)
{
    uint64_t new_offset;
    FM_Result r;

    if (new_size == 0)
    {
        ctx->dirty = 1;
        entry->size = 0;
        return FM_OK;
    }
    if (new_size > UINT64_MAX - ctx->data_next)
    {
        return FM_ERR_TOO_LARGE;
    }
    r = fmfs_range(ctx, ctx->data_next, new_size);
    if (r != FM_OK)
    {
        return r;
    }
    ctx->dirty = 1;
    new_offset = ctx->data_next;
    ctx->data_next += new_size;
    if (entry->size > 0)
    {
        r = fmfs_copy_data(ctx, new_offset, entry->offset, entry->size);
        if (r != FM_OK) return r;
    }
    if (new_size > entry->size)
    {
        r = fmfs_zero_data(ctx, new_offset + entry->size,
                           new_size - entry->size);
        if (r != FM_OK) return r;
    }
    entry->offset = new_offset;
    entry->size = new_size;
    return FM_OK;
}

static FM_Result fmfs_prepare_write(FMFS_Context *ctx, FMFS_Entry *entry,
                                    uint64_t pos, uint32_t size)
{
    uint64_t end;

    if ((uint64_t)size > UINT64_MAX - pos)
    {
        return FM_ERR_TOO_LARGE;
    }
    end = pos + size;
    if (end <= entry->size)
    {
        return FM_OK;
    }

    /* Fast path: appending to the last allocated region extends it in place.
     * Without this, every append relocates the whole file (copy-on-write) and
     * advances data_next by the new total size, so N appends consume O(n^2)
     * space and exhaust the device quickly. */
    if (entry->offset + entry->size == ctx->data_next)
    {
        FM_Result r = fmfs_range(ctx, entry->offset, end);

        if (r == FM_OK)
        {
            /* Only a gap left by a seek past the end has to read back
             * as zeros; the rest of the extension is about to be
             * written by the caller.  (Zero-filling all of it wrote
             * every appended byte to the flash twice.) */
            if (pos > entry->size)
            {
                r = fmfs_zero_data(ctx, entry->offset + entry->size,
                                   pos - entry->size);
                if (r != FM_OK)
                {
                    return r;
                }
            }
            ctx->dirty = 1;
            ctx->data_next = entry->offset + end;
            entry->size = end;
            return FM_OK;
        }
    }
    return fmfs_relocate(ctx, entry, end);
}

static FM_Result fmfs_mount(FM_FS *fs, FM_Storage *storage)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FM_Result r;

    if (storage == NULL || storage->capacity < FMFS_METADATA_SIZE)
    {
        return FM_ERR_NO_SPACE;
    }
    ctx->storage = storage;
    ctx->data_start = FMFS_METADATA_SIZE;
    r = fmfs_load(ctx);
    if (r == FM_ERR_NOT_FOUND)
    {
        r = fmfs_reset(ctx);
    }
    else if (r == FM_ERR_CORRUPTED &&
             (storage->type == FM_STORAGE_RAM ||
              storage->type == FM_STORAGE_SDRAM))
    {
        /* Volatile media (RAM disk, external SDRAM): a torn metadata
         * image - e.g. a reset in the middle of fmfs_save(), or plain
         * stale bytes in SDRAM that survives soft resets - has no
         * recoverable content by definition.  Re-format instead of
         * failing the mount, which would silently kill every later
         * file call in the application.  Non-volatile media keeps the
         * FM_ERR_CORRUPTED error so real data is never erased. */
        r = fmfs_reset(ctx);
    }
    return r;
}

static FM_Result fmfs_unmount(FM_FS *fs)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FM_Result r = fmfs_save(ctx);

    ctx->storage = NULL;
    ctx->owner = NULL;
    return r;
}

static FM_Result fmfs_format(FM_FS *fs)
{
    return fmfs_reset(fmfs_ctx(fs));
}

static FM_Result fmfs_check(FM_FS *fs)
{
    FMFS_Context *ctx = fmfs_ctx(fs);

    return (ctx->storage == NULL) ? FM_ERR_NOT_READY : fmfs_load(ctx);
}

static FM_Result fmfs_file_open(FM_FS *fs, void *fh, const char *path,
                                uint32_t mode)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;
    char clean[FM_PATH_MAX];
    int index;
    FM_Result r;

    if (fh == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK || clean[0] == '\0')
    {
        return FM_ERR_IS_DIRECTORY;
    }
    index = fmfs_find(ctx, clean);
    if (index < 0)
    {
        if (!(mode & FM_OPEN_CREATE))
        {
            return FM_ERR_NOT_FOUND;
        }
        r = fmfs_add(ctx, clean, FM_ITEM_FILE, &index);
        if (r != FM_OK) return r;
    }
    if (ctx->entries[index].type != FM_ITEM_FILE)
    {
        return FM_ERR_IS_DIRECTORY;
    }
    memset(handle, 0, FM_FS_HANDLE_SIZE);
    handle->index = (uint16_t)index;
    handle->mode = (uint8_t)mode;
    handle->gen = ctx->gen[index];
    handle->used = 1;
    if (mode & FM_OPEN_TRUNCATE)
    {
        uint64_t old_size = ctx->entries[index].size;
        uint64_t old_offset = ctx->entries[index].offset;

        ctx->entries[index].size = 0;
        ctx->entries[index].offset = ctx->data_next;
        r = fmfs_save(ctx);
        if (r != FM_OK)
        {
            ctx->entries[index].size = old_size;
            ctx->entries[index].offset = old_offset;
            handle->used = 0;
            return r;
        }
    }
    if (mode & FM_OPEN_APPEND)
    {
        handle->pos = ctx->entries[index].size;
    }
    return FM_OK;
}

static FM_Result fmfs_file_close(FM_FS *fs, void *fh)
{
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;

    (void)fs;
    if (handle == NULL || !handle->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    handle->used = 0;
    return FM_OK;
}

static FM_Result fmfs_file_read(FM_FS *fs, void *fh, void *data,
                                uint32_t len, uint32_t *read)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;
    FMFS_Entry *entry;
    uint64_t available;
    uint32_t n;
    FM_Result r;

    if (handle == NULL || !handle->used || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (read != NULL) *read = 0;
    entry = fmfs_handle_entry(ctx, handle);
    if (entry == NULL)
    {
        return FM_ERR_NOT_FOUND;        /* deleted while open */
    }
    if (handle->pos >= entry->size)
    {
        return FM_ERR_EOF;
    }
    available = entry->size - handle->pos;
    n = (uint32_t)((available < len) ? available : len);
    r = fmfs_read(ctx, entry->offset + handle->pos, data, n);
    if (r == FM_OK)
    {
        handle->pos += n;
        if (read != NULL) *read = n;
    }
    return r;
}

static FM_Result fmfs_file_write(FM_FS *fs, void *fh, const void *data,
                                 uint32_t len, uint32_t *written)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;
    FMFS_Entry *entry;
    uint64_t old_size;
    FM_Result r;

    if (handle == NULL || !handle->used || data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (written != NULL) *written = 0;
    if (!(handle->mode & FM_OPEN_WRITE))
    {
        return FM_ERR_ACCESS;
    }
    entry = fmfs_handle_entry(ctx, handle);
    if (entry == NULL)
    {
        return FM_ERR_NOT_FOUND;        /* deleted while open */
    }
    old_size = entry->size;
    r = fmfs_prepare_write(ctx, entry, handle->pos, len);
    if (r == FM_OK)
    {
        r = fmfs_write(ctx, entry->offset + handle->pos, data, len);
        if (r != FM_OK && entry->size > old_size)
        {
            /* the extension never received its data: do not let the
             * file grow by whatever was lying in that space */
            entry->size = old_size;
            ctx->dirty = 1;
        }
    }
    if (r == FM_OK)
    {
        handle->pos += len;
    }
    /* The metadata only changes when the file grew or data moved; an
     * overwrite in place leaves it alone (it used to cost a rewrite of
     * the whole table per File_Write).  It IS saved after a failure
     * that compacted the area on the way, so the storage never keeps a
     * table that points at the old places. */
    if (ctx->dirty)
    {
        FM_Result rs = fmfs_save(ctx);

        if (r == FM_OK) r = rs;
    }
    if (r == FM_OK && written != NULL) *written = len;
    return r;
}

static FM_Result fmfs_file_seek(FM_FS *fs, void *fh, int64_t offset,
                                FM_SeekOrigin origin)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;
    int64_t base;
    int64_t result;

    if (handle == NULL || !handle->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (fmfs_handle_entry(ctx, handle) == NULL)
    {
        return FM_ERR_NOT_FOUND;        /* deleted while open */
    }
    base = (origin == FM_SEEK_SET) ? 0 :
           (origin == FM_SEEK_CUR ? (int64_t)handle->pos :
                                     (int64_t)ctx->entries[handle->index].size);
    if (origin != FM_SEEK_SET && origin != FM_SEEK_CUR && origin != FM_SEEK_END)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
    {
        return FM_ERR_TOO_LARGE;
    }
    result = base + offset;
    if (result < 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    handle->pos = (uint64_t)result;
    return FM_OK;
}

static FM_Result fmfs_file_tell(FM_FS *fs, void *fh, uint64_t *pos)
{
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;

    (void)fs;
    if (handle == NULL || !handle->used || pos == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *pos = handle->pos;
    return FM_OK;
}

static FM_Result fmfs_file_size(FM_FS *fs, void *fh, uint64_t *size)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;

    if (handle == NULL || !handle->used || size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (fmfs_handle_entry(ctx, handle) == NULL)
    {
        return FM_ERR_NOT_FOUND;        /* deleted while open */
    }
    *size = ctx->entries[handle->index].size;
    return FM_OK;
}

static FM_Result fmfs_file_truncate(FM_FS *fs, void *fh, uint64_t size)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;
    FMFS_Entry *entry;
    FM_Result r;

    if (handle == NULL || !handle->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (!(handle->mode & FM_OPEN_WRITE))
    {
        return FM_ERR_ACCESS;
    }
    entry = fmfs_handle_entry(ctx, handle);
    if (entry == NULL)
    {
        return FM_ERR_NOT_FOUND;        /* deleted while open */
    }
    r = FM_OK;
    if (size < entry->size)
    {
        entry->size = size;
        ctx->dirty = 1;
    }
    else if (size > entry->size)
    {
        r = fmfs_relocate(ctx, entry, size);
    }
    if (r == FM_OK && handle->pos > size)
    {
        handle->pos = size;
    }
    if (ctx->dirty)
    {
        FM_Result rs = fmfs_save(ctx);

        if (r == FM_OK) r = rs;
    }
    return r;
}

static FM_Result fmfs_file_flush(FM_FS *fs, void *fh)
{
    FMFS_FileHandle *handle = (FMFS_FileHandle *)fh;

    if (handle == NULL || !handle->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    /* Every change is saved when it is made, so there is normally
     * nothing left to do.  Saving unconditionally rewrote the whole
     * metadata table on every flush and every close -- including the
     * close of a file that was only read -- which on NOR flash is pure
     * wear. */
    if (!fmfs_ctx(fs)->dirty)
    {
        return FM_OK;
    }
    return fmfs_save(fmfs_ctx(fs));
}

static FM_Result fmfs_stat(FM_FS *fs, const char *path, FM_FileInfo *info)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    char clean[FM_PATH_MAX];
    int index;
    FM_Result r;

    if (info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK) return r;
    index = fmfs_find(ctx, clean);
    if (index < 0) return FM_ERR_NOT_FOUND;
    memset(info, 0, sizeof(*info));
    (void)Path_GetFilename(clean, info->name, sizeof(info->name));
    (void)strcpy(info->path, clean);
    info->size = ctx->entries[index].size;
    info->type = ctx->entries[index].type;
    return FM_OK;
}

static FM_Result fmfs_delete(FM_FS *fs, const char *path)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    char clean[FM_PATH_MAX];
    int index;
    FM_Result r;

    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK) return r;
    index = fmfs_find(ctx, clean);
    if (index < 0) return FM_ERR_NOT_FOUND;
    if (ctx->entries[index].type != FM_ITEM_FILE)
    {
        return FM_ERR_IS_DIRECTORY;
    }
    return fmfs_remove_entry(ctx, index);
}

/* Give `from` and everything below it the prefix `to`.  The caller has
 * checked that every resulting path fits. */
static void fmfs_rename_prefix(FMFS_Context *ctx, const char *from,
                               const char *to)
{
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        FMFS_Entry *e = &ctx->entries[i];

        if (e->used &&
            (strcmp(e->path, from) == 0 ||
             (strncmp(e->path, from, from_len) == 0 &&
              e->path[from_len] == '/')))
        {
            size_t tail = strlen(e->path + from_len);

            if (to_len + tail < sizeof(e->path))
            {
                memmove(e->path + to_len, e->path + from_len, tail + 1u);
                memcpy(e->path, to, to_len);
            }
        }
    }
}

static FM_Result fmfs_rename(FM_FS *fs, const char *old_path,
                             const char *new_path)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    char old_clean[FM_PATH_MAX];
    char new_clean[FM_PATH_MAX];
    char parent[FM_PATH_MAX];
    FMFS_Entry saved_dst;
    uint32_t i;
    size_t old_len;
    size_t new_len;
    FM_Result r;
    int old_index;
    int new_index;

    memset(&saved_dst, 0, sizeof(saved_dst));
    r = fmfs_normalize_rel(old_path, old_clean, sizeof(old_clean));
    if (r != FM_OK) return r;
    r = fmfs_normalize_rel(new_path, new_clean, sizeof(new_clean));
    if (r != FM_OK) return r;

    old_index = fmfs_find(ctx, old_clean);
    if (old_index < 0) return FM_ERR_NOT_FOUND;
    if (strcmp(old_clean, new_clean) == 0)
    {
        return FM_OK; /* renaming onto itself is a no-op */
    }
    if (ctx->entries[old_index].type == FM_ITEM_DIR &&
        strncmp(new_clean, old_clean, strlen(old_clean)) == 0 &&
        new_clean[strlen(old_clean)] == '/')
    {
        return FM_ERR_INVALID_ARGUMENT; /* dir cannot move into itself */
    }

    /* Rename-over semantics (like POSIX rename): an existing destination
     * is replaced, but only if it is a file or an empty directory.
     *
     * Everything that can refuse the rename is checked BEFORE the table
     * is touched.  It used to be checked on the way: a path that became
     * too long was noticed after the destination had been dropped and
     * part of the tree renamed, and the function returned the error with
     * the table left like that. */
    new_index = fmfs_find(ctx, new_clean);
    if (new_index >= 0)
    {
        if (ctx->entries[new_index].type == FM_ITEM_DIR)
        {
            if (fmfs_has_child(ctx, new_clean))
            {
                return FM_ERR_NOT_EMPTY;
            }
            if (ctx->entries[old_index].type != FM_ITEM_DIR)
            {
                return FM_ERR_NOT_DIRECTORY;
            }
        }
        else
        {
            if (ctx->entries[old_index].type == FM_ITEM_DIR)
            {
                return FM_ERR_NOT_DIRECTORY;
            }
        }
    }

    r = fmfs_parent_path(new_clean, parent, sizeof(parent));
    if (r != FM_OK) return r;
    {
        int parent_index = fmfs_find(ctx, parent);

        if (parent_index < 0)
        {
            return FM_ERR_NOT_FOUND;
        }
        if (ctx->entries[parent_index].type != FM_ITEM_DIR)
        {
            return FM_ERR_NOT_DIRECTORY;
        }
    }

    old_len = strlen(old_clean);
    new_len = strlen(new_clean);
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (ctx->entries[i].used &&
            (strcmp(ctx->entries[i].path, old_clean) == 0 ||
             (strncmp(ctx->entries[i].path, old_clean, old_len) == 0 &&
              ctx->entries[i].path[old_len] == '/')) &&
            new_len + strlen(ctx->entries[i].path + old_len) >= FM_PATH_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
    }

    /* Now change the table... */
    if (new_index >= 0)
    {
        saved_dst = ctx->entries[new_index];
        memset(&ctx->entries[new_index], 0, sizeof(ctx->entries[new_index]));
    }
    fmfs_rename_prefix(ctx, old_clean, new_clean);

    /* ...and put it back when it cannot be written: the caller is told
     * the rename failed, so the old names (and the file that was to be
     * replaced) must still be there.  They used to be gone from the
     * mounted volume while the storage still held the old table. */
    r = fmfs_save(ctx);
    if (r != FM_OK)
    {
        fmfs_rename_prefix(ctx, new_clean, old_clean);
        if (new_index >= 0)
        {
            ctx->entries[new_index] = saved_dst;
        }
        return r;
    }
    if (new_index >= 0)
    {
        ctx->gen[new_index]++;      /* handles of the replaced file */
    }
    return FM_OK;
}

static FM_Result fmfs_mkdir(FM_FS *fs, const char *path)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    char clean[FM_PATH_MAX];
    FM_Result r;

    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK) return r;
    return fmfs_add(ctx, clean, FM_ITEM_DIR, NULL);
}

static FM_Result fmfs_rmdir(FM_FS *fs, const char *path)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    char clean[FM_PATH_MAX];
    int index;
    FM_Result r;

    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK) return r;
    if (clean[0] == '\0') return FM_ERR_BUSY;
    index = fmfs_find(ctx, clean);
    if (index < 0) return FM_ERR_NOT_FOUND;
    if (ctx->entries[index].type != FM_ITEM_DIR) return FM_ERR_NOT_DIRECTORY;
    if (fmfs_has_child(ctx, clean)) return FM_ERR_NOT_EMPTY;
    return fmfs_remove_entry(ctx, index);
}

static FM_Result fmfs_dir_open(FM_FS *fs, void *dh, const char *path)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_DirHandle *handle = (FMFS_DirHandle *)dh;
    char clean[FM_PATH_MAX];
    int index;
    FM_Result r;

    if (handle == NULL) return FM_ERR_INVALID_ARGUMENT;
    r = fmfs_normalize_rel(path, clean, sizeof(clean));
    if (r != FM_OK) return r;
    index = fmfs_find(ctx, clean);
    if (index < 0) return FM_ERR_NOT_FOUND;
    if (ctx->entries[index].type != FM_ITEM_DIR) return FM_ERR_NOT_DIRECTORY;
    memset(handle, 0, FM_FS_DIRHANDLE_SIZE);
    (void)strcpy(handle->path, clean);
    handle->used = 1;
    return FM_OK;
}

static FM_Result fmfs_dir_close(FM_FS *fs, void *dh)
{
    FMFS_DirHandle *handle = (FMFS_DirHandle *)dh;

    (void)fs;
    if (handle == NULL || !handle->used) return FM_ERR_NOT_OPEN;
    handle->used = 0;
    return FM_OK;
}

static FM_Result fmfs_dir_read(FM_FS *fs, void *dh, FM_DirEntry *entry)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FMFS_DirHandle *handle = (FMFS_DirHandle *)dh;

    if (handle == NULL || !handle->used || entry == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    while (handle->cursor < FM_FMFS_MAX_ENTRIES)
    {
        FMFS_Entry *candidate = &ctx->entries[handle->cursor++];
        const char *name;

        if (!candidate->used || !fmfs_is_child_of(handle->path,
                                                   candidate->path))
        {
            continue;
        }
        name = strrchr(candidate->path, '/');
        name = (name == NULL) ? candidate->path : name + 1;
        memset(entry, 0, sizeof(*entry));
        (void)strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->size = candidate->size;
        entry->type = candidate->type;
        return FM_OK;
    }
    return FM_ERR_EOF;
}

static FM_Result fmfs_get_free(FM_FS *fs, uint64_t *free_bytes,
                               uint64_t *total_bytes)
{
    FMFS_Context *ctx = fmfs_ctx(fs);

    if (free_bytes == NULL || total_bytes == NULL || ctx->storage == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *total_bytes = ctx->storage->capacity;
    {
        /* Free = capacity minus metadata minus live file data.  The dead
         * space between files counts as free: a write that runs out of
         * tail compacts the data area and reuses it (fmfs_relocate). */
        uint64_t live = ctx->data_start;
        uint32_t i;

        for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
        {
            if (ctx->entries[i].used &&
                ctx->entries[i].type == FM_ITEM_FILE)
            {
                live += ctx->entries[i].size;
            }
        }
        *free_bytes = (live <= ctx->storage->capacity)
                          ? ctx->storage->capacity - live : 0;
    }
    return FM_OK;
}

static FM_Result fmfs_get_type(FM_FS *fs, char *type, size_t type_size)
{
    (void)fs;
    if (type == NULL || type_size < sizeof("FMFS"))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(type, "FMFS");
    return FM_OK;
}

/* Compact the data area: relocate every live file contiguously from
 * data_start, so that space consumed by deleted files is recovered.
 * Open file handles stay valid.  The filesystem does this by itself
 * when a write would otherwise fail with FM_ERR_NO_SPACE; call it
 * explicitly to do the work at a convenient time instead.
 *
 * Takes the mount's backend lock, so it is safe next to file activity
 * of other tasks on the same FMFS. */
FM_Result FMFS_Repack(FM_FS *fs)
{
    FMFS_Context *ctx;
    FM_MountEntry *mount;
    FM_Result r;

    if (fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ctx = fmfs_ctx(fs);
    if (ctx == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    mount = FM_VFS_EnterFS(fs);         /* NULL: not mounted through VFS */
    if (ctx->storage == NULL)
    {
        r = FM_ERR_NOT_READY;
    }
    else
    {
        r = fmfs_repack(ctx);
        if (r == FM_OK)
        {
            r = fmfs_save(ctx);
        }
    }
    FM_VFS_LeaveFS(mount);
    return r;
}

FM_Result FMFS_Init(FM_FS *fs)
{
    FMFS_Context *ctx = NULL;
    uint32_t i;

    if (fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(fs, 0, sizeof(*fs));
    FM_Lock();
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fmfs[i].owner == NULL)
        {
            ctx = &g_fmfs[i];
            memset(ctx, 0, sizeof(*ctx));
            ctx->owner = fs;
            break;
        }
    }
    FM_Unlock();
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    (void)strcpy(fs->name, "FMFS");
    fs->user = ctx;
    fs->ops.mount = fmfs_mount;
    fs->ops.unmount = fmfs_unmount;
    fs->ops.format = fmfs_format;
    fs->ops.check = fmfs_check;
    fs->ops.file_open = fmfs_file_open;
    fs->ops.file_close = fmfs_file_close;
    fs->ops.file_read = fmfs_file_read;
    fs->ops.file_write = fmfs_file_write;
    fs->ops.file_seek = fmfs_file_seek;
    fs->ops.file_tell = fmfs_file_tell;
    fs->ops.file_size = fmfs_file_size;
    fs->ops.file_truncate = fmfs_file_truncate;
    fs->ops.file_flush = fmfs_file_flush;
    fs->ops.stat = fmfs_stat;
    fs->ops.delete_file = fmfs_delete;
    fs->ops.rename = fmfs_rename;
    fs->ops.mkdir = fmfs_mkdir;
    fs->ops.rmdir = fmfs_rmdir;
    fs->ops.dir_open = fmfs_dir_open;
    fs->ops.dir_close = fmfs_dir_close;
    fs->ops.dir_read = fmfs_dir_read;
    fs->ops.get_free = fmfs_get_free;
    fs->ops.get_type = fmfs_get_type;
    return FM_OK;
}

#endif /* FM_USE_FMFS */
