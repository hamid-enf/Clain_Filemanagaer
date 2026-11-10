/*
 * FileManager - FMFS built-in storage filesystem
 *
 * FMFS is a deliberately small, allocation-free filesystem adapter.  It
 * stores a fixed metadata table in the beginning of the bound storage and
 * appends file data after that table.  It is not intended to compete with
 * FAT/LittleFS on very large media; its value is that the same simple
 * high-level file/directory API works immediately on a caller-supplied memory
 * address or on any storage driver implementing FM_Storage callbacks.
 *
 * Two on-storage layouts exist.  Version 2 -- what a format writes
 * whenever the storage is large enough -- keeps two copies of the table
 * and a small move journal, and survives a reset at any moment (see
 * "Version 2" below).  Version 1 volumes, written by older releases,
 * still mount and work as they always did; they are not converted.
 */

#define FM_DEBUG_TAG "fmfs"
#include "fm_debug.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_FMFS

#define FMFS_MAGIC "FMFS"
#define FMFS_VERSION 1u             /* one table, rewritten in place   */
#define FMFS_VERSION_SAFE 2u        /* two tables and a move journal   */
#define FMFS_HEADER_SIZE 32u
#define FMFS_RECORD_SIZE (FM_PATH_MAX + 20u)
#define FMFS_METADATA_SIZE (FMFS_HEADER_SIZE + \
                            (FM_FMFS_MAX_ENTRIES * FMFS_RECORD_SIZE))

/* ------------------------------------------------------------------ */
/* Version 2: the power-fail safe layout                               */
/* ------------------------------------------------------------------ */
/*
 *   0                 table A   (header + FM_FMFS_MAX_ENTRIES records)
 *   slot              table B
 *   2*slot            move journal record A
 *   2*slot + jsize    move journal record B
 *   2*slot + 2*jsize  file data                       (= data_start)
 *
 * `slot` and `jsize` are the sizes of a table and of a journal record
 * rounded up to the block of the storage, so that no two of the four
 * ever share a block.
 *
 * THE TABLE.  A change is never written over the table that is in use.
 * It goes to the OTHER slot with the next sequence number and a CRC
 * over all of it; mounting takes the slot with a good CRC and the
 * highest number.  A reset in the middle of a save leaves a slot with a
 * bad CRC, and the volume is simply what it was before that save.
 *
 * THE DATA.  Two rules keep every table that can be read back true:
 *   1. bytes are only ever written to a place that the table ON THE
 *      STORAGE calls free (or to the very bytes of the file the caller
 *      is overwriting), and
 *   2. the table that points at new bytes is saved after them.
 * So a file grows, moves to the tail or is created/deleted/renamed
 * atomically: after a reset it is entirely the old one or entirely the
 * new one.
 *
 * THE JOURNAL.  Compaction is the one operation that cannot follow rule
 * 1: sliding a file by less than its own length overwrites its old
 * bytes.  Such a move is done in steps that each leave the old bytes of
 * everything not yet moved alone (a step is never longer than the
 * distance moved, or goes through free space above the file first),
 * and the progress is written to the journal before the next step.  The
 * record carries the sequence number of the table it belongs to, so it
 * needs no erasing: the save that ends the move retires it.  Mounting
 * finishes a move that was cut short.
 * A journal record is a write of its own (on flash: an erase cycle of
 * the block it lives in), so a move may only cost FM_FMFS_MOVE_STEPS of
 * them.  A hole that would take more to close stays a hole; files are
 * placed into holes, so it is used all the same.
 *
 * BLOCKS.  A driver for an SD card or a NOR flash rewrites whole blocks
 * (sectors, erase units), and a reset in the middle of that can leave
 * the WHOLE block unreadable -- also the bytes in it that the write was
 * not meant to change.  The four pieces of metadata each have blocks to
 * themselves, and so has every file: files start on block boundaries
 * (fmfs_align; not on RAM, where there is no such thing).  Whatever is
 * lost with a block therefore belongs to the one thing that was being
 * written, and for the metadata and for a file on the move that is
 * exactly what the rules above already cover.  This holds for blocks of
 * up to FMFS2_MAX_UNIT bytes; larger erase units are treated as 1.
 *
 * What this does NOT promise: a File_Write changes the bytes a file
 * already has in place, and a reset can leave those half written (like
 * on every filesystem without data journalling).  On a block device
 * that includes the rest of the block the write was working on -- for
 * an append, the last block of the file as it was.
 */
#define FMFS2_HEADER_SIZE 64u
#define FMFS2_TABLE_SIZE (FMFS2_HEADER_SIZE + \
                          (FM_FMFS_MAX_ENTRIES * FMFS_RECORD_SIZE))
#define FMFS2_JOURNAL_SIZE 64u
#define FMFS2_JOURNAL_MAGIC "FMJ1"
#define FMFS2_MAX_UNIT 4096u

/* header (little endian):  0 magic "FMFS"   4 u16 version (2)
 *    6 u16 used entries     8 u64 data_start   16 u64 data_next
 *   24 u64 capacity        32 u32 sequence     36 u32 slot size
 *   40 u32 journal size    44 u16 entries      46 u16 record size
 *   48 u8  slot (0 = A, 1 = B)                 60 u32 CRC-32 of the
 *   records followed by header bytes 0..59
 *
 * journal record:  0 magic "FMJ1"   4 u32 sequence   8 u32 table sequence
 *   12 u8 state    14 u16 entry    16 u64 from      24 u64 to
 *   32 u64 size    40 u64 done     48 u64 bounce    56 u32 bounce length
 *   60 u32 CRC-32 of bytes 0..59 */

#define FMFS_MOVE_COPY    1u    /* `done` bytes are at the new place    */
#define FMFS_MOVE_BOUNCED 2u    /* ...and the next ones are in `bounce` */

/* Journal records one file move may cost (see FM_FMFS_MOVE_STEPS). */
#define FMFS_MOVE_STEPS_MAX ((uint64_t)(FM_FMFS_MOVE_STEPS))

/* The table is written in pieces of this size (on the stack). */
#ifndef FMFS_TABLE_CHUNK
#define FMFS_TABLE_CHUNK 512u
#endif

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

/* One step of the move journal (version 2). */
typedef struct
{
    uint8_t  state;             /* FMFS_MOVE_*                           */
    uint16_t index;             /* table entry of the file being moved   */
    uint32_t table_seq;         /* the table this move started from      */
    uint32_t bounce_len;
    uint64_t from;              /* old offset of the file                */
    uint64_t to;                /* new offset                            */
    uint64_t size;
    uint64_t done;              /* bytes that are at the new place       */
    uint64_t bounce;            /* where the next bounce_len bytes wait  */
} FMFS_Move;

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
    uint8_t      loaded;        /* a table was read from / written to it */
    uint8_t      safe;          /* version 2: two tables + journal       */
    uint8_t      active;        /* which table (0 = A, 1 = B) is in use  */
    /* a journalled move has begun and is not finished: ctx->move is
     * what the journal on the storage says */
    uint8_t      move_pending;
    uint32_t     seq;           /* sequence number of the table in use   */
    uint32_t     jseq;          /* ...and of the last journal record     */
    uint32_t     slot_size;     /* bytes from table A to table B         */
    uint32_t     journal_size;  /* bytes from journal record A to B      */
    FMFS_Move    move;
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

static uint32_t fmfs_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fmfs_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/* CRC-32 (IEEE, reflected), four bits at a time.  FMFS has its own so
 * that a volume stays readable in a build without the CRC module
 * (FM_USE_CRC=0).  Chains: pass the previous result as `crc`. */
static uint32_t fmfs_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };

    crc = ~crc;
    while (len-- != 0u)
    {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
    }
    return ~crc;
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
static FM_Result fmfs_settle(FMFS_Context *ctx);
static FM_Result fmfs_move_run(FMFS_Context *ctx);
static FM_Bool fmfs_space_free(const FMFS_Context *ctx,
                               const FMFS_Entry *self, uint64_t start,
                               uint64_t end);

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
    FMFS_Entry saved;
    FM_Result r;

    r = fmfs_settle(ctx);
    if (r != FM_OK)
    {
        return r;
    }
    saved = ctx->entries[index];
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

/* ------------------------------------------------------------------ */
/* Version 1: one table at the start, rewritten in place               */
/* ------------------------------------------------------------------ */
static void fmfs_put_record(const FMFS_Entry *e, uint8_t *record)
{
    memset(record, 0, FMFS_RECORD_SIZE);
    record[0] = e->used;
    record[1] = e->type;
    fmfs_wr64(record + 4, e->offset);
    fmfs_wr64(record + 12, e->size);
    {
        size_t n = strlen(e->path);

        if (n > FM_PATH_MAX - 1u) n = FM_PATH_MAX - 1u;
        memcpy(record + 20, e->path, n);
    }
}

/* FM_OK, or FM_ERR_CORRUPTED for a record no table can hold. */
static FM_Result fmfs_get_record(const FMFS_Context *ctx,
                                 const uint8_t *record, FMFS_Entry *e)
{
    e->used = record[0] ? 1u : 0u;
    e->type = record[1];
    e->offset = fmfs_rd64(record + 4);
    e->size = fmfs_rd64(record + 12);
    memcpy(e->path, record + 20, FM_PATH_MAX);
    e->path[FM_PATH_MAX - 1] = '\0';
    if (e->used && e->type != FM_ITEM_FILE && e->type != FM_ITEM_DIR)
    {
        return FM_ERR_CORRUPTED;
    }
    if (e->used && e->type == FM_ITEM_FILE &&
        (e->offset > ctx->storage->capacity ||
         e->size > ctx->storage->capacity - e->offset))
    {
        return FM_ERR_CORRUPTED;
    }
    return FM_OK;
}

static uint16_t fmfs_used_count(const FMFS_Context *ctx)
{
    uint16_t count = 0;
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        if (ctx->entries[i].used) count++;
    }
    return count;
}

static FM_Result fmfs_save_v1(FMFS_Context *ctx)
{
    uint8_t header[FMFS_HEADER_SIZE];
    uint32_t i;

    if (fmfs_range(ctx, 0, FMFS_METADATA_SIZE) != FM_OK)
    {
        return FM_ERR_NO_SPACE;
    }

    /* Write the entry records first and the header LAST, so an
     * interrupted save never leaves a new header (new data_next) over
     * records that were not written yet.
     *
     * This narrows the window; it does not close it.  The records are
     * rewritten in place, so a reset in the middle leaves some of them
     * new and some old under the old header.  A version 1 volume has
     * one copy of its table and no journal: it is not power-fail safe.
     * Version 2 (below) is. */
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        uint8_t record[FMFS_RECORD_SIZE];
        uint64_t offset = FMFS_HEADER_SIZE +
                          (uint64_t)i * FMFS_RECORD_SIZE;

        fmfs_put_record(&ctx->entries[i], record);
        if (fmfs_write(ctx, offset, record, sizeof(record)) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }

    memset(header, 0, sizeof(header));
    memcpy(header, FMFS_MAGIC, 4);
    fmfs_wr16(header + 4, FMFS_VERSION);
    fmfs_wr16(header + 6, fmfs_used_count(ctx));
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

static FM_Result fmfs_load_v1(FMFS_Context *ctx)
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
    ctx->safe = 0;
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
        if (fmfs_get_record(ctx, record, &ctx->entries[i]) != FM_OK)
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

/* ------------------------------------------------------------------ */
/* Version 2: two tables                                               */
/* ------------------------------------------------------------------ */
static uint32_t fmfs_round_up(uint32_t value, uint32_t unit)
{
    return (unit > 1u) ? ((value + unit - 1u) / unit) * unit : value;
}

/* The block the metadata is laid out in: a table and a journal record
 * each get whole blocks of their own, so a driver that rewrites a whole
 * block (or erase unit) for a partial write cannot take a neighbour of
 * the thing being written down with it. */
static uint32_t fmfs_unit(const FM_Storage *s)
{
    uint32_t unit = 1u;

    if (s->block_size > unit && s->block_size <= FMFS2_MAX_UNIT)
    {
        unit = s->block_size;
    }
    if (s->erase_size > unit && s->erase_size <= FMFS2_MAX_UNIT)
    {
        unit = s->erase_size;
    }
    return unit;
}

/* Tell the driver to put what it was given on the medium before going
 * on.  The order of the writes is what makes the volume safe, so a
 * driver that buffers must not be allowed to reorder across this. */
static FM_Result fmfs_barrier(FMFS_Context *ctx)
{
    FM_Result r;

    if (ctx->storage == NULL || ctx->storage->sync == NULL)
    {
        return FM_OK;
    }
    r = ctx->storage->sync(ctx->storage);
    return (r == FM_ERR_UNSUPPORTED) ? FM_OK : r;
}

/* The block that file data is placed in on a version 2 volume: every
 * file starts on a boundary of it, so no two files have bytes in the
 * same block.  A driver that rewrites a whole block (an SD sector, the
 * erase unit of a NOR flash) for a partial write, and is cut off in the
 * middle, can then only harm the file that was being written.
 * 1 (no alignment, nothing wasted) on a version 1 volume, and on RAM,
 * where a write touches nothing but its own bytes. */
static uint64_t fmfs_align(const FMFS_Context *ctx)
{
    if (!ctx->safe || ctx->storage == NULL ||
        ctx->storage->type == FM_STORAGE_RAM ||
        ctx->storage->type == FM_STORAGE_SDRAM)
    {
        return 1u;
    }
    return fmfs_unit(ctx->storage);
}

static uint64_t fmfs_up(const FMFS_Context *ctx, uint64_t value)
{
    uint64_t a = fmfs_align(ctx);

    return (a > 1u) ? ((value + a - 1u) / a) * a : value;
}

static uint64_t fmfs_down(const FMFS_Context *ctx, uint64_t value)
{
    uint64_t a = fmfs_align(ctx);

    return (a > 1u) ? (value / a) * a : value;
}

static void fmfs_table_header(const FMFS_Context *ctx, uint8_t slot,
                              uint32_t seq, uint8_t *header)
{
    uint8_t record[FMFS_RECORD_SIZE];
    uint32_t crc = 0;
    uint32_t i;

    memset(header, 0, FMFS2_HEADER_SIZE);
    memcpy(header, FMFS_MAGIC, 4);
    fmfs_wr16(header + 4, FMFS_VERSION_SAFE);
    fmfs_wr16(header + 6, fmfs_used_count(ctx));
    fmfs_wr64(header + 8, ctx->data_start);
    fmfs_wr64(header + 16, ctx->data_next);
    fmfs_wr64(header + 24, ctx->storage->capacity);
    fmfs_wr32(header + 32, seq);
    fmfs_wr32(header + 36, ctx->slot_size);
    fmfs_wr32(header + 40, ctx->journal_size);
    fmfs_wr16(header + 44, (uint16_t)FM_FMFS_MAX_ENTRIES);
    fmfs_wr16(header + 46, (uint16_t)FMFS_RECORD_SIZE);
    header[48] = slot;
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        fmfs_put_record(&ctx->entries[i], record);
        crc = fmfs_crc32(crc, record, FMFS_RECORD_SIZE);
    }
    crc = fmfs_crc32(crc, header, FMFS2_HEADER_SIZE - 4u);
    fmfs_wr32(header + 60, crc);
}

/* Bytes [pos, pos + len) of the table as it goes to the storage. */
static void fmfs_table_bytes(const FMFS_Context *ctx, const uint8_t *header,
                             uint32_t pos, uint8_t *out, uint32_t len)
{
    uint8_t record[FMFS_RECORD_SIZE];
    uint32_t end = pos + len;
    uint32_t i;

    memset(out, 0, len);
    if (pos < FMFS2_HEADER_SIZE)
    {
        uint32_t n = FMFS2_HEADER_SIZE - pos;

        if (n > len) n = len;
        memcpy(out, header + pos, n);
    }
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        uint32_t rs = FMFS2_HEADER_SIZE + i * FMFS_RECORD_SIZE;
        uint32_t re = rs + FMFS_RECORD_SIZE;
        uint32_t a = (rs > pos) ? rs : pos;
        uint32_t b = (re < end) ? re : end;

        if (a < b)
        {
            fmfs_put_record(&ctx->entries[i], record);
            memcpy(out + (a - pos), record + (a - rs), b - a);
        }
    }
}

static FM_Result fmfs_save_safe(FMFS_Context *ctx)
{
    uint8_t header[FMFS2_HEADER_SIZE];
    uint8_t chunk[FMFS_TABLE_CHUNK];
    uint8_t slot = (uint8_t)(ctx->active ? 0 : 1);  /* the one NOT in use */
    uint64_t base = (uint64_t)slot * ctx->slot_size;
    uint32_t seq = ctx->seq + 1u;
    uint32_t pos;
    uint32_t n;

    if (fmfs_range(ctx, base, FMFS2_TABLE_SIZE) != FM_OK)
    {
        return FM_ERR_NO_SPACE;
    }
    fmfs_table_header(ctx, slot, seq, header);

    /* Everything but the first piece, then the first piece -- the one
     * with the header.  The CRC alone decides whether a slot is good,
     * so the order is not what makes this safe; it only means that a
     * save cut short usually does not even leave a header behind. */
    for (pos = FMFS_TABLE_CHUNK; pos < FMFS2_TABLE_SIZE;
         pos += FMFS_TABLE_CHUNK)
    {
        n = FMFS2_TABLE_SIZE - pos;
        if (n > FMFS_TABLE_CHUNK) n = FMFS_TABLE_CHUNK;
        fmfs_table_bytes(ctx, header, pos, chunk, n);
        if (fmfs_write(ctx, base + pos, chunk, n) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    /* the data this table points at, and the rest of the table, have
     * to be on the medium before the piece that makes it count */
    if (fmfs_barrier(ctx) != FM_OK)
    {
        return FM_ERR_IO;
    }
    n = (FMFS2_TABLE_SIZE < FMFS_TABLE_CHUNK) ? FMFS2_TABLE_SIZE
                                               : FMFS_TABLE_CHUNK;
    fmfs_table_bytes(ctx, header, 0, chunk, n);
    if (fmfs_write(ctx, base, chunk, n) != FM_OK ||
        fmfs_barrier(ctx) != FM_OK)
    {
        return FM_ERR_IO;
    }
    ctx->active = slot;
    ctx->seq = seq;
    ctx->dirty = 0;
    return FM_OK;
}

typedef struct
{
    uint32_t seq;
    uint32_t slot_size;
    uint32_t journal_size;
    uint64_t data_start;
    uint64_t data_next;
} FMFS_TableInfo;

/* Read and check the table at `base`.
 *   FM_OK            a complete table (and, with `load`, now the one in
 *                    RAM)
 *   FM_ERR_NOT_FOUND no FMFS header there
 *   FM_ERR_CORRUPTED a header, but not a table that can be used
 *   FM_ERR_IO        the storage could not be read
 * Without `load` nothing in the context is touched. */
static FM_Result fmfs_table_read(FMFS_Context *ctx, uint8_t slot,
                                 uint64_t base, FM_Bool load,
                                 FMFS_TableInfo *info)
{
    uint8_t header[FMFS2_HEADER_SIZE];
    FMFS_TableInfo t;
    uint32_t crc = 0;
    uint32_t i;

    if (fmfs_range(ctx, base, FMFS2_TABLE_SIZE) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (fmfs_read(ctx, base, header, sizeof(header)) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (memcmp(header, FMFS_MAGIC, 4) != 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    t.data_start = fmfs_rd64(header + 8);
    t.data_next = fmfs_rd64(header + 16);
    t.seq = fmfs_rd32(header + 32);
    t.slot_size = fmfs_rd32(header + 36);
    t.journal_size = fmfs_rd32(header + 40);
    if (fmfs_rd16(header + 4) != FMFS_VERSION_SAFE ||
        fmfs_rd64(header + 24) != ctx->storage->capacity ||
        fmfs_rd16(header + 44) != FM_FMFS_MAX_ENTRIES ||
        fmfs_rd16(header + 46) != FMFS_RECORD_SIZE ||
        header[48] != slot ||
        t.slot_size < FMFS2_TABLE_SIZE ||
        t.journal_size < FMFS2_JOURNAL_SIZE ||
        base != (uint64_t)slot * t.slot_size ||
        t.data_start != 2u * (uint64_t)t.slot_size +
                        2u * (uint64_t)t.journal_size ||
        t.data_start > ctx->storage->capacity ||
        t.data_next < t.data_start ||
        t.data_next > ctx->storage->capacity)
    {
        return FM_ERR_CORRUPTED;
    }
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        uint8_t record[FMFS_RECORD_SIZE];
        FMFS_Entry scratch;
        FMFS_Entry *e = load ? &ctx->entries[i] : &scratch;

        if (fmfs_read(ctx, base + FMFS2_HEADER_SIZE +
                               (uint64_t)i * FMFS_RECORD_SIZE,
                      record, sizeof(record)) != FM_OK)
        {
            return FM_ERR_IO;
        }
        crc = fmfs_crc32(crc, record, FMFS_RECORD_SIZE);
        if (fmfs_get_record(ctx, record, e) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        if (i == 0u && (!e->used || e->type != FM_ITEM_DIR ||
                        e->path[0] != '\0'))
        {
            return FM_ERR_CORRUPTED;
        }
    }
    crc = fmfs_crc32(crc, header, FMFS2_HEADER_SIZE - 4u);
    if (crc != fmfs_rd32(header + 60))
    {
        return FM_ERR_CORRUPTED;
    }
    if (load)
    {
        ctx->safe = 1;
        ctx->active = slot;
        ctx->seq = t.seq;
        ctx->slot_size = t.slot_size;
        ctx->journal_size = t.journal_size;
        ctx->data_start = t.data_start;
        ctx->data_next = t.data_next;
    }
    if (info != NULL)
    {
        *info = t;
    }
    return FM_OK;
}

/* Where table B can be when table A cannot say (its header is the
 * thing that was being written): the place this storage gives it now,
 * then every other block size a volume can have been formatted with. */
static uint32_t fmfs_slot_guess(const FMFS_Context *ctx, uint32_t n)
{
    static const uint16_t units[] = { 1u, 128u, 256u, 512u, 1024u, 2048u,
                                      4096u };

    if (n == 0u)
    {
        return fmfs_round_up(FMFS2_TABLE_SIZE, fmfs_unit(ctx->storage));
    }
    if (n <= sizeof(units) / sizeof(units[0]))
    {
        return fmfs_round_up(FMFS2_TABLE_SIZE, units[n - 1u]);
    }
    return 0;
}

static FM_Result fmfs_load_safe(FMFS_Context *ctx)
{
    FMFS_TableInfo a;
    FMFS_TableInfo b;
    FM_Bool have_b = FM_FALSE;
    FM_Bool header_seen;
    uint64_t b_base = 0;
    uint8_t slot;
    FM_Result ra;
    FM_Result r;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    ra = fmfs_table_read(ctx, 0, 0, FM_FALSE, &a);
    if (ra == FM_ERR_IO)
    {
        return ra;
    }
    header_seen = (ra != FM_ERR_NOT_FOUND) ? FM_TRUE : FM_FALSE;
    if (ra == FM_OK)
    {
        r = fmfs_table_read(ctx, 1, a.slot_size, FM_FALSE, &b);
        if (r == FM_ERR_IO)
        {
            return r;
        }
        if (r == FM_OK)
        {
            have_b = FM_TRUE;
            b_base = a.slot_size;
        }
    }
    else
    {
        uint32_t n;

        for (n = 0; !have_b; n++)
        {
            uint32_t guess = fmfs_slot_guess(ctx, n);

            if (guess == 0u)
            {
                break;
            }
            r = fmfs_table_read(ctx, 1, guess, FM_FALSE, &b);
            if (r == FM_ERR_IO)
            {
                return r;
            }
            if (r == FM_OK)
            {
                have_b = FM_TRUE;
                b_base = guess;
            }
            else if (r == FM_ERR_CORRUPTED)
            {
                header_seen = FM_TRUE;
            }
        }
    }
    if (ra == FM_OK && have_b)
    {
        /* the later one; the numbers wrap, the difference does not */
        slot = (uint8_t)(((int32_t)(b.seq - a.seq) > 0) ? 1u : 0u);
    }
    else if (ra == FM_OK)
    {
        slot = 0;
    }
    else if (have_b)
    {
        slot = 1;
    }
    else
    {
        /* nothing that looks like FMFS at all: a blank storage */
        return header_seen ? FM_ERR_CORRUPTED : FM_ERR_NOT_FOUND;
    }
    r = fmfs_table_read(ctx, slot, slot ? b_base : 0u, FM_TRUE, NULL);
    if (r != FM_OK)
    {
        return (r == FM_ERR_IO) ? r : FM_ERR_CORRUPTED;
    }
    return FM_OK;
}

static FM_Result fmfs_load(FMFS_Context *ctx)
{
    uint8_t head[8];

    if (fmfs_read(ctx, 0, head, sizeof(head)) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (memcmp(head, FMFS_MAGIC, 4) == 0 &&
        fmfs_rd16(head + 4) == FMFS_VERSION)
    {
        return fmfs_load_v1(ctx);
    }
    if (ctx->storage->capacity < FMFS2_TABLE_SIZE)
    {
        return (memcmp(head, FMFS_MAGIC, 4) == 0) ? FM_ERR_CORRUPTED
                                                   : FM_ERR_NOT_FOUND;
    }
    return fmfs_load_safe(ctx);
}

/* Write the table.  Version 2: to the slot that is not in use. */
static FM_Result fmfs_save(FMFS_Context *ctx)
{
    ctx->dirty = 1;
    if (ctx->move_pending)
    {
        /* A file is half way between two places and the journal on
         * the storage says where.  A new table would retire that
         * record; the move has to be finished first (fmfs_settle). */
        return FM_ERR_IO;
    }
    return ctx->safe ? fmfs_save_safe(ctx) : fmfs_save_v1(ctx);
}

/* ------------------------------------------------------------------ */
/* Version 2: the move journal                                         */
/* ------------------------------------------------------------------ */
/* Make `next` the state on the storage; only then the state in RAM. */
static FM_Result fmfs_journal_write(FMFS_Context *ctx, const FMFS_Move *next)
{
    uint8_t rec[FMFS2_JOURNAL_SIZE];
    uint32_t jseq = ctx->jseq + 1u;
    /* alternate between the two records: the one written now is never
     * the one that describes the state before it */
    uint64_t at = 2u * (uint64_t)ctx->slot_size +
                  (uint64_t)(jseq & 1u) * ctx->journal_size;

    memset(rec, 0, sizeof(rec));
    memcpy(rec, FMFS2_JOURNAL_MAGIC, 4);
    fmfs_wr32(rec + 4, jseq);
    fmfs_wr32(rec + 8, next->table_seq);
    rec[12] = next->state;
    fmfs_wr16(rec + 14, next->index);
    fmfs_wr64(rec + 16, next->from);
    fmfs_wr64(rec + 24, next->to);
    fmfs_wr64(rec + 32, next->size);
    fmfs_wr64(rec + 40, next->done);
    fmfs_wr64(rec + 48, next->bounce);
    fmfs_wr32(rec + 56, next->bounce_len);
    fmfs_wr32(rec + 60, fmfs_crc32(0, rec, sizeof(rec) - 4u));

    /* the bytes this record vouches for first, then the record, then
     * nothing else until the record is down */
    if (fmfs_barrier(ctx) != FM_OK ||
        fmfs_write(ctx, at, rec, sizeof(rec)) != FM_OK ||
        fmfs_barrier(ctx) != FM_OK)
    {
        return FM_ERR_IO;
    }
    ctx->jseq = jseq;
    ctx->move = *next;
    return FM_OK;
}

/* After mounting a version 2 volume: finish the move that a reset cut
 * short, if there is one. */
static FM_Result fmfs_journal_recover(FMFS_Context *ctx)
{
    FMFS_Move m;
    FM_Bool have = FM_FALSE;
    uint32_t best = 0;
    uint32_t i;

    memset(&m, 0, sizeof(m));
    ctx->jseq = 0;
    ctx->move_pending = 0;
    for (i = 0; i < 2u; i++)
    {
        uint8_t rec[FMFS2_JOURNAL_SIZE];
        uint32_t jseq;

        if (fmfs_read(ctx, 2u * (uint64_t)ctx->slot_size +
                               (uint64_t)i * ctx->journal_size,
                      rec, sizeof(rec)) != FM_OK)
        {
            return FM_ERR_IO;
        }
        if (memcmp(rec, FMFS2_JOURNAL_MAGIC, 4) != 0 ||
            fmfs_crc32(0, rec, sizeof(rec) - 4u) != fmfs_rd32(rec + 60))
        {
            continue;               /* never written, or cut short */
        }
        jseq = fmfs_rd32(rec + 4);
        if (have && (int32_t)(jseq - best) <= 0)
        {
            continue;
        }
        have = FM_TRUE;
        best = jseq;
        m.table_seq = fmfs_rd32(rec + 8);
        m.state = rec[12];
        m.index = fmfs_rd16(rec + 14);
        m.from = fmfs_rd64(rec + 16);
        m.to = fmfs_rd64(rec + 24);
        m.size = fmfs_rd64(rec + 32);
        m.done = fmfs_rd64(rec + 40);
        m.bounce = fmfs_rd64(rec + 48);
        m.bounce_len = fmfs_rd32(rec + 56);
    }
    if (!have)
    {
        return FM_OK;
    }
    ctx->jseq = best;
    if (m.table_seq != ctx->seq)
    {
        /* The table was saved after this move began: the move is
         * finished (that save ended it) or older still. */
        return FM_OK;
    }
    /* The record belongs to the table in use, so the move in it did
     * not reach its end.  It must describe that table exactly. */
    if (m.index == 0u || m.index >= FM_FMFS_MAX_ENTRIES ||
        !ctx->entries[m.index].used ||
        ctx->entries[m.index].type != FM_ITEM_FILE ||
        ctx->entries[m.index].offset != m.from ||
        ctx->entries[m.index].size != m.size ||
        m.size == 0u || m.done > m.size || m.from == m.to ||
        m.from < ctx->data_start || m.to < ctx->data_start ||
        fmfs_range(ctx, m.from, m.size) != FM_OK ||
        fmfs_range(ctx, m.to, m.size) != FM_OK ||
        (m.state != FMFS_MOVE_COPY && m.state != FMFS_MOVE_BOUNCED) ||
        (m.state == FMFS_MOVE_BOUNCED &&
         (m.to > m.from || m.bounce_len == 0u ||
          m.bounce_len > m.size - m.done ||
          m.bounce < m.from + m.size ||
          !fmfs_space_free(ctx, NULL, m.bounce,
                           m.bounce + m.bounce_len))))
    {
        return FM_ERR_CORRUPTED;
    }
    FM_EVENT_LOG("finishing an interrupted move of entry %u (%lu of %lu)",
                 (unsigned)m.index, (unsigned long)m.done,
                 (unsigned long)m.size);
    ctx->move = m;
    ctx->move_pending = 1;
    return fmfs_move_run(ctx);
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */
/* The layout a fresh format gives this storage: FM_TRUE for version 2. */
static FM_Bool fmfs_pick_layout(const FM_Storage *s, uint32_t *slot_size,
                                uint32_t *journal_size)
{
#if FM_FMFS_POWER_SAFE
    uint32_t unit = fmfs_unit(s);
    uint32_t ss = fmfs_round_up(FMFS2_TABLE_SIZE, unit);
    uint32_t js = fmfs_round_up(FMFS2_JOURNAL_SIZE, unit);
    uint64_t need = 2u * (uint64_t)ss + 2u * (uint64_t)js;

    *slot_size = ss;
    *journal_size = js;
#if FM_FMFS_POWER_SAFE >= 2
    return (need < s->capacity) ? FM_TRUE : FM_FALSE;
#else
    /* not when the second table would take more than half of a small
     * storage away from the files */
    return (need <= s->capacity / 2u) ? FM_TRUE : FM_FALSE;
#endif
#else
    (void)s;
    *slot_size = 0;
    *journal_size = 0;
    return FM_FALSE;
#endif
}

/* Remove everything a later mount could take for a volume: the header
 * at the start (version 1 or table A), a table B wherever one can be,
 * and the journal of the layout about to be written. */
static FM_Result fmfs_erase_marks(FMFS_Context *ctx, FM_Bool safe)
{
    uint8_t zeros[FMFS2_HEADER_SIZE];
    uint32_t n;

    memset(zeros, 0, sizeof(zeros));
    if (fmfs_range(ctx, 0, sizeof(zeros)) == FM_OK &&
        fmfs_write(ctx, 0, zeros, sizeof(zeros)) != FM_OK)
    {
        return FM_ERR_IO;
    }
    for (n = 0;; n++)
    {
        uint32_t guess = fmfs_slot_guess(ctx, n);

        if (guess == 0u)
        {
            break;
        }
        if (fmfs_range(ctx, guess, sizeof(zeros)) == FM_OK &&
            fmfs_write(ctx, guess, zeros, sizeof(zeros)) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    if (safe)
    {
        for (n = 0; n < 2u; n++)
        {
            if (fmfs_write(ctx, 2u * (uint64_t)ctx->slot_size +
                                    (uint64_t)n * ctx->journal_size,
                           zeros, FMFS2_JOURNAL_SIZE) != FM_OK)
            {
                return FM_ERR_IO;
            }
        }
    }
    return fmfs_barrier(ctx);
}

static FM_Result fmfs_reset(FMFS_Context *ctx)
{
    uint32_t slot_size = 0;
    uint32_t journal_size = 0;
    FM_Result r;

    memset(ctx->entries, 0, sizeof(ctx->entries));
    fmfs_bump_all(ctx);
    ctx->entries[0].used = 1;
    ctx->entries[0].type = FM_ITEM_DIR;
    ctx->entries[0].path[0] = '\0';
    ctx->move_pending = 0;

    if (ctx->loaded && ctx->safe)
    {
        /* A mounted version 2 volume is emptied like any other change:
         * one new table.  A reset in the middle leaves the old volume,
         * whole.  (The journal needs nothing: its record names a table
         * that is no longer the one in use.) */
        ctx->data_next = ctx->data_start;
        return fmfs_save(ctx);
    }

    ctx->loaded = 0;
    ctx->safe = fmfs_pick_layout(ctx->storage, &slot_size, &journal_size)
                    ? 1u : 0u;
    if (ctx->safe)
    {
        ctx->slot_size = slot_size;
        ctx->journal_size = journal_size;
        ctx->data_start = 2u * (uint64_t)slot_size +
                          2u * (uint64_t)journal_size;
        ctx->active = 1;            /* the first table goes to slot A */
        ctx->seq = 0;
        ctx->jseq = 0;
    }
    else
    {
#if FM_FMFS_POWER_SAFE
        FM_EVENT_LOG("storage of %lu bytes is too small for two tables: "
                     "formatted without power-fail safety",
                     (unsigned long)ctx->storage->capacity);
#endif
        ctx->data_start = FMFS_METADATA_SIZE;
    }
    ctx->data_next = ctx->data_start;
    r = fmfs_erase_marks(ctx, ctx->safe ? FM_TRUE : FM_FALSE);
    if (r == FM_OK)
    {
        r = fmfs_save(ctx);
    }
    if (r == FM_OK)
    {
        ctx->loaded = 1;
    }
    return r;
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
    {
        FM_Result rs = fmfs_settle(ctx);

        if (rs != FM_OK)
        {
            return rs;
        }
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

/* Copy from the lowest byte up: right for separate ranges and for a
 * destination below the source. */
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

/* FM_TRUE when [start, end) lies in the data area and no file other
 * than `self` has bytes in it. */
static FM_Bool fmfs_space_free(const FMFS_Context *ctx,
                               const FMFS_Entry *self, uint64_t start,
                               uint64_t end)
{
    uint32_t i;

    if (end < start || start < ctx->data_start ||
        end > ctx->storage->capacity)
    {
        return FM_FALSE;
    }
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        const FMFS_Entry *e = &ctx->entries[i];

        if (e != self && e->used && e->type == FM_ITEM_FILE &&
            e->size != 0u && e->offset < end && start < e->offset + e->size)
        {
            return FM_FALSE;
        }
    }
    return FM_TRUE;
}

/* End of the highest file: where the free tail really begins. */
static uint64_t fmfs_live_end(const FMFS_Context *ctx)
{
    uint64_t end = ctx->data_start;
    uint32_t i;

    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        const FMFS_Entry *e = &ctx->entries[i];

        if (e->used && e->type == FM_ITEM_FILE && e->size != 0u &&
            e->offset + e->size > end)
        {
            end = e->offset + e->size;
        }
    }
    return end;
}

/* A run of free bytes inside [lo, hi): the largest one, or -- with
 * `need` -- the smallest one that holds `need` bytes.  Returns its
 * length (0: there is none) and where it starts, which is a block
 * boundary (fmfs_align). */
static uint64_t fmfs_free_run(const FMFS_Context *ctx, uint64_t lo,
                              uint64_t hi, uint64_t need, uint64_t *at)
{
    uint64_t best = 0;
    uint32_t i;

    *at = lo;
    for (i = 0; i <= FM_FMFS_MAX_ENTRIES; i++)
    {
        /* a run starts at `lo` or behind a file */
        uint64_t start = fmfs_up(ctx, lo);
        uint64_t end = hi;
        FM_Bool inside = FM_FALSE;
        uint32_t k;

        if (i < FM_FMFS_MAX_ENTRIES)
        {
            const FMFS_Entry *e = &ctx->entries[i];

            if (!e->used || e->type != FM_ITEM_FILE || e->size == 0u)
            {
                continue;
            }
            if (e->offset + e->size <= lo)
            {
                continue;
            }
            start = fmfs_up(ctx, e->offset + e->size);
        }
        if (start >= hi)
        {
            continue;
        }
        for (k = 0; k < FM_FMFS_MAX_ENTRIES; k++)
        {
            const FMFS_Entry *f = &ctx->entries[k];

            if (!f->used || f->type != FM_ITEM_FILE || f->size == 0u)
            {
                continue;
            }
            if (f->offset <= start && start < f->offset + f->size)
            {
                inside = FM_TRUE;
            }
            else if (f->offset >= start && f->offset < end)
            {
                end = f->offset;
            }
        }
        if (inside)
        {
            continue;
        }
        if ((need == 0u) ? (end - start > best)
                         : (end - start >= need &&
                            (best == 0u || end - start < best)))
        {
            best = end - start;
            *at = start;
        }
    }
    return best;
}

/* The next step of a file on its way from `from` to `to`, `left` of its
 * `size` bytes still to go.  Returns how many bytes the step moves and
 * says how: *bounce is 0 for a direct copy, or the place in free space
 * the bytes are parked at first.  `cost` gets the journal records the
 * rest of the move takes.
 *
 * Directly, a step is as long as the distance.  Through a free run
 * above the file it is as long as that run, at two records a step --
 * worth it when the file moves by little and there is room elsewhere.
 *
 * The driver may rewrite the blocks a step writes to as a whole
 * (fmfs_align).  Steps therefore begin and end on block boundaries
 * where it matters: no block that a step writes to holds a byte of the
 * file that is not yet safe at its new place (or parked). */
static uint64_t fmfs_move_step(const FMFS_Context *ctx, uint64_t from,
                               uint64_t to, uint64_t size, uint64_t left,
                               uint64_t *bounce, uint64_t *cost)
{
    uint64_t a = fmfs_align(ctx);
    uint64_t gap = (to < from) ? from - to : to - from;
    uint64_t part = size % a;       /* the last, partial block */
    uint64_t direct = (left + gap - 1u) / gap;
    uint64_t n;
    uint64_t run;
    uint64_t at;

    *bounce = 0;
    if (to > from)
    {
        /* Moving up, the last bytes go first: the partial block, then
         * whole steps down to the start. */
        n = (left < gap) ? left : gap;
        if (left == size && part != 0u && part < n)
        {
            n = part;
            direct = 1u + (left - part + gap - 1u) / gap;
        }
        if (cost != NULL) *cost = direct - 1u;
        return n;
    }
    /* Moving down, steps start at the start of the file: each ends on
     * a block boundary but the last, and the block that one ends in
     * only holds bytes from less than `gap` before the end of the file
     * -- bytes of this very step's predecessors. */
    n = (left < gap) ? left : gap;
    if (cost != NULL) *cost = direct - 1u;

    run = fmfs_free_run(ctx, from + size, ctx->storage->capacity, 0, &at);
    if (run < left)
    {
        run = fmfs_down(ctx, run);  /* not the last step: whole blocks */
    }
    if (run > 0x40000000u)
    {
        run = 0x40000000u;          /* bounce_len is 32 bits */
    }
    if (run > gap)
    {
        uint64_t steps = (left + run - 1u) / run;

        if (2u * steps < direct)
        {
            *bounce = at;
            if (cost != NULL) *cost = 2u * steps - 1u;
            return (left < run) ? left : run;
        }
    }
    return n;
}

/* FM_TRUE when moving `e` to `to` stays within the journal records one
 * move may cost.  A hole that is tiny next to the file behind it is
 * not worth thousands of journal writes (and, on flash, as many erase
 * cycles of the one block the journal lives in): it stays a hole. */
static FM_Bool fmfs_move_affordable(const FMFS_Context *ctx,
                                    const FMFS_Entry *e, uint64_t to)
{
    uint64_t gap = (to < e->offset) ? e->offset - to : to - e->offset;
    uint64_t at;
    uint64_t cost;

    if (!ctx->safe || gap == 0u || e->size <= gap)
    {
        return FM_TRUE;         /* not journalled at all */
    }
    (void)fmfs_move_step(ctx, e->offset, to, e->size, e->size, &at, &cost);
    return (cost <= FMFS_MOVE_STEPS_MAX) ? FM_TRUE : FM_FALSE;
}

/* Copy `size` bytes from src to dst like memmove(): the ranges may
 * overlap in either direction. */
static FM_Result fmfs_shift_data(FMFS_Context *ctx, uint64_t dst,
                                 uint64_t src, uint64_t size)
{
    uint8_t buffer[FMFS_COPY_BUF_SIZE];
    uint64_t left = size;

    if (dst <= src)
    {
        return fmfs_copy_data(ctx, dst, src, size);
    }
    while (left > 0u)
    {
        uint32_t chunk = (uint32_t)((left < sizeof(buffer))
                                        ? left : sizeof(buffer));
        FM_Result r;

        left -= chunk;
        r = fmfs_read(ctx, src + left, buffer, chunk);
        if (r != FM_OK) return r;
        r = fmfs_write(ctx, dst + left, buffer, chunk);
        if (r != FM_OK) return r;
    }
    return FM_OK;
}

/* Carry the move in ctx->move through to its end: the file at its new
 * place and a table on the storage that says so.
 *
 * A step (fmfs_move_step) copies bytes whose destination holds nothing
 * that is still needed:
 *   - directly, at most as many bytes as the file moves by: source and
 *     destination of the step do not overlap, and the destination only
 *     covers source bytes of steps already done;
 *   - or through free space above the file ("bounce"): the bytes are
 *     parked there first, the journal says so, and only then are they
 *     written over their own source.  Worth it when the file moves by
 *     little and there is room elsewhere -- a step is then as long as
 *     that room.
 * Before a step may destroy the source of the one before it, the
 * journal has that one down as done.  The record on the storage is
 * therefore always enough to go on from, and that is all a mount after
 * a reset -- or a second call after an I/O error -- does. */
static FM_Result fmfs_move_run(FMFS_Context *ctx)
{
    FMFS_Move *m = &ctx->move;
    FM_Bool down = (m->to < m->from) ? FM_TRUE : FM_FALSE;
    FM_Result r;

    for (;;)
    {
        uint64_t progress;
        FMFS_Move next = *m;

        if (m->state == FMFS_MOVE_BOUNCED)
        {
            r = fmfs_copy_data(ctx, m->to + m->done, m->bounce,
                               m->bounce_len);
            if (r != FM_OK) return r;
            progress = m->done + m->bounce_len;
        }
        else
        {
            uint64_t left = m->size - m->done;
            uint64_t at;
            uint64_t n;

            if (left == 0u)
            {
                break;
            }
            n = fmfs_move_step(ctx, m->from, m->to, m->size, left, &at,
                               NULL);
            if (at != 0u)
            {
                r = fmfs_copy_data(ctx, at, m->from + m->done, n);
                if (r != FM_OK) return r;
                next.state = FMFS_MOVE_BOUNCED;
                next.bounce = at;
                next.bounce_len = (uint32_t)n;
                r = fmfs_journal_write(ctx, &next);
                if (r != FM_OK) return r;
                continue;
            }
            /* moving up, the last bytes go first */
            r = fmfs_copy_data(ctx, m->to + (down ? m->done : left - n),
                               m->from + (down ? m->done : left - n), n);
            if (r != FM_OK) return r;
            progress = m->done + n;
        }
        if (progress == m->size)
        {
            break;      /* nothing is written between here and the save */
        }
        next.state = FMFS_MOVE_COPY;
        next.done = progress;
        next.bounce = 0;
        next.bounce_len = 0;
        r = fmfs_journal_write(ctx, &next);
        if (r != FM_OK) return r;
    }
    ctx->entries[m->index].offset = m->to;
    ctx->move_pending = 0;
    return fmfs_save(ctx);
}

/* Before anything changes the volume: finish a move that an I/O error
 * stopped half way. */
static FM_Result fmfs_settle(FMFS_Context *ctx)
{
    return ctx->move_pending ? fmfs_move_run(ctx) : FM_OK;
}

/* Move the data of one file to `to`.
 *
 * Version 2, and the old and new place overlap: journalled, and the
 * table is saved when the file has arrived.  Otherwise (the new place
 * is free space, or the volume is version 1) the bytes are copied and
 * only the table in RAM changes -- the caller saves it. */
static FM_Result fmfs_move(FMFS_Context *ctx, uint32_t index, uint64_t to)
{
    FMFS_Entry *e = &ctx->entries[index];
    uint64_t from = e->offset;
    uint64_t gap = (to < from) ? from - to : to - from;
    FM_Result r;

    if (gap == 0u)
    {
        return FM_OK;
    }
    if (!ctx->safe || e->size <= gap)
    {
        r = fmfs_shift_data(ctx, to, from, e->size);
        if (r != FM_OK) return r;
        e->offset = to;
        ctx->dirty = 1;
        return FM_OK;
    }
    if (ctx->dirty)
    {
        /* the journal names the table on the storage */
        r = fmfs_save(ctx);
        if (r != FM_OK) return r;
    }
    memset(&ctx->move, 0, sizeof(ctx->move));
    ctx->move.state = FMFS_MOVE_COPY;
    ctx->move.index = (uint16_t)index;
    ctx->move.table_seq = ctx->seq;
    ctx->move.from = from;
    ctx->move.to = to;
    ctx->move.size = e->size;
    ctx->move_pending = 1;
    return fmfs_move_run(ctx);
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
 * not a storage offset.
 *
 * Version 2: safe against a reset at any point, and the table is saved
 * on return.  A file that drops into free space is copied; the table
 * that says so is saved, at the latest, before anything is written over
 * the place it came from.  A file that slides over itself is journalled
 * (fmfs_move) -- unless that would take more journal records than a
 * move may cost (FM_FMFS_MOVE_STEPS): then the hole in front of that
 * file stays, and the files above are packed against it.
 * Version 1: the caller saves the table. */
static FM_Result fmfs_repack(FMFS_Context *ctx)
{
    uint64_t dest = ctx->data_start;
    uint64_t last_end = ctx->data_start;    /* end of the highest file */
    uint64_t floor = 0;             /* offsets below this are done */
    /* lowest old place of a file whose move is not saved yet */
    uint64_t unsaved_low = UINT64_MAX;
    FM_Result r;

    if (ctx->safe && ctx->dirty)
    {
        r = fmfs_save(ctx);
        if (r != FM_OK) return r;
    }
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
        /* never upwards (a file that does not start on a block
         * boundary, on a volume something else wrote) */
        if (next->offset < dest ||
            (next->offset != dest && !fmfs_move_affordable(ctx, next, dest)))
        {
            dest = next->offset;
        }
        if (next->offset != dest)
        {
            uint64_t old = next->offset;

            if (ctx->safe && ctx->dirty && dest + next->size > unsaved_low)
            {
                r = fmfs_save(ctx);
                if (r != FM_OK) return r;
            }
            if (!ctx->dirty)
            {
                unsaved_low = UINT64_MAX;
            }
            r = fmfs_move(ctx, (uint32_t)(next - ctx->entries), dest);
            if (r != FM_OK)
            {
                return r;
            }
            if (!ctx->dirty)
            {
                unsaved_low = UINT64_MAX;   /* fmfs_move saved it all */
            }
            else if (old < unsaved_low)
            {
                unsaved_low = old;
            }
        }
        last_end = dest + next->size;
        dest = fmfs_up(ctx, last_end);
    }
    if (last_end < ctx->data_next)
    {
        uint64_t old_next = ctx->data_next;

        ctx->data_next = last_end;
        ctx->dirty = 1;
        if (ctx->safe)
        {
            /* what is about to be wiped still belongs to files in the
             * table on the storage */
            r = fmfs_save(ctx);
            if (r != FM_OK) return r;
        }
        /* wipe the freed tail so stale data doesn't linger -- from the
         * next block on: the one the last file ends in is left alone */
        if (dest < old_next)
        {
            r = fmfs_zero_data(ctx, dest, old_next - dest);
            if (r != FM_OK)
            {
                return r;
            }
        }
    }
    if (ctx->safe && ctx->dirty)
    {
        return fmfs_save(ctx);
    }
    return FM_OK;
}

/* Grow `entry` to new_size without moving any other file: where it is,
 * when the bytes behind it are free; else as a copy in free space --
 * the hole a deleted or moved file left, or the tail.  The new bytes up
 * to zero_to read back as zeros (the caller writes the rest).  Only the
 * table in RAM changes. */
static FM_Result fmfs_grow_simple(FMFS_Context *ctx, FMFS_Entry *entry,
                                  uint64_t new_size, uint64_t zero_to)
{
    uint64_t old = entry->size;
    uint64_t at;
    FM_Result r;

    if (old != 0u && fmfs_space_free(ctx, entry, entry->offset + old,
                                     entry->offset + new_size))
    {
        /* The last file grows into the tail, and any file grows into
         * the space a deleted or moved neighbour left behind it.
         * (Copying it to the tail instead, every time, made N appends
         * cost O(N^2) bytes of flash.) */
        if (zero_to > old)
        {
            r = fmfs_zero_data(ctx, entry->offset + old, zero_to - old);
            if (r != FM_OK) return r;
        }
        entry->size = new_size;
        if (entry->offset + new_size > ctx->data_next)
        {
            ctx->data_next = entry->offset + new_size;
        }
        ctx->dirty = 1;
        return FM_OK;
    }
    /* A file that has bytes is on its way up: give it the largest run
     * there is, so that it can go on growing where it lands.  New
     * content goes into the smallest hole that holds it, and leaves the
     * large ones whole.  (Handing out only the tail, as this used to,
     * forced a compaction as soon as the tail was used up -- however
     * much room the deleted files had left.) */
    if (fmfs_free_run(ctx, ctx->data_start, ctx->storage->capacity,
                      (old != 0u) ? 0u : new_size, &at) < new_size)
    {
        return FM_ERR_NO_SPACE;
    }
    if (old != 0u)
    {
        r = fmfs_copy_data(ctx, at, entry->offset, old);
        if (r != FM_OK) return r;
    }
    if (zero_to > old)
    {
        r = fmfs_zero_data(ctx, at + old, zero_to - old);
        if (r != FM_OK) return r;
    }
    entry->offset = at;
    entry->size = new_size;
    ctx->data_next = fmfs_live_end(ctx);
    ctx->dirty = 1;
    return FM_OK;
}

/* `entry` is not the last file.  Push every file stored above it up
 * against the end of the storage -- highest first -- so that the free
 * space becomes a hole right behind `entry`, reaching at least to
 * `need_end`.  All of them or none: FM_ERR_NO_SPACE, with nothing
 * moved, when the hole would be too short or a file would cost too
 * many journal records (FM_FMFS_MOVE_STEPS). */
static FM_Result fmfs_open_hole(FMFS_Context *ctx, FMFS_Entry *entry,
                                uint64_t need_end)
{
    uint32_t pass;
    FM_Result r;

    for (pass = 0; pass < 2u; pass++)   /* 0: look, 1: move */
    {
        uint64_t top = ctx->storage->capacity;
        uint64_t ceiling = UINT64_MAX;  /* old offsets at or above: done */

        for (;;)
        {
            FMFS_Entry *next = NULL;
            uint64_t old;
            uint32_t i;

            for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
            {
                FMFS_Entry *e = &ctx->entries[i];

                if (e != entry && e->used && e->type == FM_ITEM_FILE &&
                    e->size != 0u && e->offset > entry->offset &&
                    e->offset < ceiling &&
                    (next == NULL || e->offset > next->offset))
                {
                    next = e;
                }
            }
            if (next == NULL)
            {
                break;
            }
            old = next->offset;
            ceiling = old;
            top = fmfs_down(ctx, top - next->size);
            if (top < old)
            {
                top = old;              /* never downwards */
            }
            if (pass == 0u)
            {
                if (!fmfs_move_affordable(ctx, next, top))
                {
                    return FM_ERR_NO_SPACE;
                }
                continue;
            }
            if (old != top)
            {
                /* the files now reach the end of the storage: every
                 * table saved from here on has to say so */
                if (ctx->data_next != ctx->storage->capacity)
                {
                    ctx->data_next = ctx->storage->capacity;
                    ctx->dirty = 1;
                }
                r = fmfs_move(ctx, (uint32_t)(next - ctx->entries), top);
                if (r != FM_OK) return r;
                if (ctx->safe && ctx->dirty)
                {
                    /* the next file moves into the place this one left */
                    r = fmfs_save(ctx);
                    if (r != FM_OK) return r;
                }
            }
        }
        if (pass == 0u && top < need_end)
        {
            return FM_ERR_NO_SPACE;
        }
    }
    return FM_OK;
}

/* Give `entry` room for new_size bytes; the new bytes up to zero_to
 * read back as zeros.
 *
 * "No space" at the tail usually only means that dead space sits
 * between the files.  Before reporting FM_ERR_NO_SPACE the data area is
 * compacted, and when even then there is no room for a second copy of
 * the file, the free space is brought to where the file is.  On a
 * version 1 volume a write therefore fails for lack of space only when
 * the live data really does not fit.  On a version 2 volume a hole can
 * stay where it is because closing it would cost too many journal
 * records (FM_FMFS_MOVE_STEPS), so the last percent or two of a nearly
 * full volume may be out of reach.
 *
 * The table in RAM is left changed for the caller to save, after it has
 * written its data: a reset before that save leaves the file as it was. */
static FM_Result fmfs_grow(FMFS_Context *ctx, FMFS_Entry *entry,
                           uint64_t new_size, uint64_t zero_to)
{
    uint64_t room = ctx->storage->capacity - ctx->data_start;
    uint64_t others = 0;
    uint32_t i;
    FM_Result r;

    if (new_size > room)
    {
        return FM_ERR_NO_SPACE;
    }
    if (ctx->dirty)
    {
        /* "Free" has to mean free in the table on the storage, or a
         * reset would bring back a file whose bytes were just reused. */
        r = fmfs_save(ctx);
        if (r != FM_OK) return r;
    }
    r = fmfs_grow_simple(ctx, entry, new_size, zero_to);
    if (r != FM_ERR_NO_SPACE)
    {
        return r;
    }
    for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
    {
        const FMFS_Entry *e = &ctx->entries[i];

        if (e != entry && e->used && e->type == FM_ITEM_FILE)
        {
            others += fmfs_up(ctx, e->size);    /* whole blocks */
        }
    }
    if (others > room || new_size > room - others)
    {
        return FM_ERR_NO_SPACE;
    }
    r = fmfs_repack(ctx);
    if (r != FM_OK)
    {
        return r;
    }
    if (entry->size != 0u &&
        entry->offset + entry->size != ctx->data_next)
    {
        uint64_t at;
        uint64_t run = fmfs_free_run(ctx, ctx->data_start,
                                     ctx->storage->capacity, 0, &at);
        /* the tail, from the first block no file has bytes in */
        uint64_t tail_at = fmfs_up(ctx, ctx->data_next);
        uint64_t tail = (tail_at < ctx->storage->capacity)
                            ? ctx->storage->capacity - tail_at : 0u;

        if (run >= new_size)
        {
            /* room for the grown file in one piece: nothing to arrange */
        }
        else if (tail >= entry->size)
        {
            /* Room at the tail for a copy of the file as it is, not
             * for the grown one: put the copy there and close the hole
             * it leaves.  The file is then the last one and grows
             * where it is.  Not when a file above would not follow
             * into that hole (FM_FMFS_MOVE_STEPS): the copy would be
             * for nothing. */
            for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
            {
                const FMFS_Entry *e = &ctx->entries[i];

                if (e != entry && e->used && e->type == FM_ITEM_FILE &&
                    e->size != 0u && e->offset > entry->offset &&
                    !fmfs_move_affordable(ctx, e, e->offset - entry->size))
                {
                    return FM_ERR_NO_SPACE;
                }
            }
            r = fmfs_copy_data(ctx, tail_at, entry->offset, entry->size);
            if (r != FM_OK) return r;
            entry->offset = tail_at;
            ctx->data_next = tail_at + entry->size;
            ctx->dirty = 1;
            r = fmfs_repack(ctx);
            if (r != FM_OK) return r;
        }
        else
        {
            /* NO_SPACE: holes that had to stay */
            r = fmfs_open_hole(ctx, entry, entry->offset + new_size);
            if (r != FM_OK) return r;
        }
    }
    return fmfs_grow_simple(ctx, entry, new_size, zero_to);
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
    /* Only a gap left by a seek past the end has to read back as
     * zeros; the rest of the extension is about to be written by the
     * caller.  (Zero-filling all of it wrote every appended byte to the
     * flash twice.) */
    return fmfs_grow(ctx, entry, end, pos);
}

/* The context of `fs`, if it is still its own.  An unmount -- and a
 * mount that failed -- gives the context back to the pool; the FM_FS
 * keeps pointing at it.  Mounting that FM_FS again without a new
 * FMFS_Init() takes the context back when nobody has it, and is
 * refused when another FM_FS does: two mounts never share one. */
static FMFS_Context *fmfs_claim(FM_FS *fs)
{
    FMFS_Context *ctx = fmfs_ctx(fs);

    if (ctx == NULL)
    {
        return NULL;
    }
    FM_Lock();
    if (ctx->owner == NULL)
    {
        ctx->owner = fs;
    }
    else if (ctx->owner != fs)
    {
        ctx = NULL;
    }
    FM_Unlock();
    return ctx;
}

static void fmfs_release(FMFS_Context *ctx)
{
    FM_Lock();
    ctx->storage = NULL;
    ctx->owner = NULL;
    FM_Unlock();
}

static FM_Result fmfs_mount(FM_FS *fs, FM_Storage *storage)
{
    FMFS_Context *ctx = fmfs_claim(fs);
    FM_Result r;

    if (ctx == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    if (storage == NULL || storage->capacity < FMFS_METADATA_SIZE)
    {
        fmfs_release(ctx);
        return FM_ERR_NO_SPACE;
    }
    ctx->storage = storage;
    ctx->loaded = 0;
    ctx->safe = 0;
    ctx->dirty = 0;
    ctx->move_pending = 0;
    ctx->data_start = FMFS_METADATA_SIZE;
    r = fmfs_load(ctx);
    if (r == FM_OK)
    {
        ctx->loaded = 1;
        if (ctx->safe)
        {
            r = fmfs_journal_recover(ctx);
        }
    }
    if (r == FM_ERR_NOT_FOUND)
    {
        r = fmfs_reset(ctx);
    }
    else if (r == FM_ERR_CORRUPTED &&
             (storage->type == FM_STORAGE_RAM ||
              storage->type == FM_STORAGE_SDRAM))
    {
        /* Volatile media (RAM disk, external SDRAM): a metadata image
         * that cannot be used - plain stale bytes in SDRAM that
         * survives soft resets, or a version 1 table torn by a reset
         * in the middle of a save - has no recoverable content by
         * definition.  Re-format instead of failing the mount, which
         * would silently kill every later file call in the
         * application.  Non-volatile media keeps the FM_ERR_CORRUPTED
         * error so real data is never erased. */
        ctx->loaded = 0;
        r = fmfs_reset(ctx);
    }
    if (r != FM_OK)
    {
        /* nothing is mounted, so no unmount will ever give the context
         * back: FM_MAX_FS failed mounts would leave none for anybody */
        fmfs_release(ctx);
    }
    return r;
}

static FM_Result fmfs_unmount(FM_FS *fs)
{
    FMFS_Context *ctx = fmfs_ctx(fs);
    FM_Result r = fmfs_settle(ctx);

    /* Version 2 saves every change when it is made; another table
     * here would only wear the flash.  Version 1 keeps writing its
     * table on unmount as it always did. */
    if (r == FM_OK && (ctx->dirty || !ctx->safe))
    {
        r = fmfs_save(ctx);
    }
    fmfs_release(ctx);
    return r;
}

static FM_Result fmfs_format(FM_FS *fs)
{
    return fmfs_reset(fmfs_ctx(fs));
}

static FM_Result fmfs_check(FM_FS *fs)
{
    FMFS_Context *ctx = fmfs_ctx(fs);

    if (ctx->storage == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    if (ctx->safe)
    {
        /* the table in use still reads back whole; RAM is not touched */
        FM_Result r = fmfs_table_read(ctx, ctx->active,
                                      (uint64_t)ctx->active * ctx->slot_size,
                                      FM_FALSE, NULL);

        return (r == FM_ERR_NOT_FOUND) ? FM_ERR_CORRUPTED : r;
    }
    return fmfs_load(ctx);
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
        uint64_t old_size;
        uint64_t old_offset;

        r = fmfs_settle(ctx);
        if (r != FM_OK)
        {
            handle->used = 0;
            return r;
        }
        old_size = ctx->entries[index].size;
        old_offset = ctx->entries[index].offset;
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
    if (ctx->move_pending && ctx->move.index == handle->index)
    {
        /* this file is half way between two places */
        r = fmfs_settle(ctx);
        if (r != FM_OK)
        {
            return r;
        }
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
    /* No data goes to the storage while its table is behind the one
     * in RAM (a move an I/O error stopped, a save that failed): what is
     * written next has to be judged against the table a reset would
     * bring back. */
    r = fmfs_settle(ctx);
    if (r == FM_OK && ctx->dirty)
    {
        r = fmfs_save(ctx);
    }
    if (r != FM_OK)
    {
        return r;
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
     * table that points at the old places.
     *
     * The data was written first: until this save a reset leaves the
     * file exactly as long as it was. */
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
    r = fmfs_settle(ctx);
    if (r != FM_OK)
    {
        return r;
    }
    if (size < entry->size)
    {
        entry->size = size;
        ctx->dirty = 1;
    }
    else if (size > entry->size)
    {
        r = fmfs_grow(ctx, entry, size, size);
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
    if (!fmfs_ctx(fs)->dirty && !fmfs_ctx(fs)->move_pending)
    {
        return FM_OK;
    }
    {
        FM_Result r = fmfs_settle(fmfs_ctx(fs));

        if (r == FM_OK && fmfs_ctx(fs)->dirty)
        {
            r = fmfs_save(fmfs_ctx(fs));
        }
        return r;
    }
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

    r = fmfs_settle(ctx);
    if (r != FM_OK) return r;

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
         * tail compacts the data area and reuses it (fmfs_grow).  Where
         * files start on block boundaries, a file counts with the
         * blocks it has bytes in. */
        uint64_t live = ctx->data_start;
        uint32_t i;

        for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
        {
            if (ctx->entries[i].used &&
                ctx->entries[i].type == FM_ITEM_FILE)
            {
                live += fmfs_up(ctx, ctx->entries[i].size);
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
 * On a version 2 volume a reset in the middle loses nothing: the next
 * mount finishes the move that was under way.
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
    if (ctx->storage == NULL || ctx->owner != fs)
    {
        r = FM_ERR_NOT_READY;   /* not mounted (any more) */
    }
    else
    {
        r = fmfs_settle(ctx);
        if (r == FM_OK)
        {
            r = fmfs_repack(ctx);
        }
        if (r == FM_OK && ctx->dirty)
        {
            r = fmfs_save(ctx);
        }
    }
    FM_VFS_LeaveFS(mount);
    return r;
}

/* FM_TRUE when the mounted volume has the version 2 layout. */
FM_Bool FMFS_IsPowerSafe(FM_FS *fs)
{
    FMFS_Context *ctx;
    FM_MountEntry *mount;
    FM_Bool safe;

    if (fs == NULL)
    {
        return FM_FALSE;
    }
    ctx = fmfs_ctx(fs);
    if (ctx == NULL)
    {
        return FM_FALSE;
    }
    mount = FM_VFS_EnterFS(fs);
    safe = (ctx->storage != NULL && ctx->owner == fs && ctx->loaded &&
            ctx->safe) ? FM_TRUE : FM_FALSE;
    FM_VFS_LeaveFS(mount);
    return safe;
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
    /* An FM_FS that is initialised again without having been mounted
     * in between keeps the context it has. */
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fmfs[i].owner == fs)
        {
            ctx = &g_fmfs[i];
            break;
        }
    }
    for (i = 0; ctx == NULL && i < FM_MAX_FS; i++)
    {
        if (g_fmfs[i].owner == NULL)
        {
            ctx = &g_fmfs[i];
        }
    }
    if (ctx != NULL)
    {
        memset(ctx, 0, sizeof(*ctx));
        ctx->owner = fs;
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
