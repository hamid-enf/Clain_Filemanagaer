/*
 * FileManager - LittleFS filesystem adapter
 *
 * Implements the FM_FSOps function table on top of the vendored littlefs.
 * The adapter owns one lfs_t per FM_FS instance and keeps fixed pools of
 * open file/dir objects, indexed by small handles that fit inside the
 * framework's FM_FS_HANDLE_SIZE / FM_FS_DIRHANDLE_SIZE storage.
 */

#include "fm_littlefs.h"

#if FM_USE_LITTLEFS

#include "fm_internal.h"

/* littlefs is vendored upstream code; relax the project's strict warnings
 * for its headers while keeping them for the adapter code itself. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "lfs.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <string.h>

/* ------------------------------------------------------------------ */
/* Context and handle layouts                                          */
/* ------------------------------------------------------------------ */
#if defined(__GNUC__) || defined(__clang__) || defined(__ARMCC_VERSION) || \
    defined(__ICCARM__)
#define LFS_BUF_ALIGN __attribute__((aligned(32)))
#else
#define LFS_BUF_ALIGN
#endif

typedef struct
{
    FM_Storage *storage;
    lfs_t       lfs;
    struct lfs_config cfg;
    FM_Bool     auto_format;
    FM_Bool     mounted;
    FM_Bool     in_use;
    FM_FS      *owner;          /* the FM_FS it is in use by */
    /* The pool (see LittleFS_Init): a mount of this context has begun
     * or is in place; and the order in which contexts were handed out. */
    uint8_t     pool_busy;
    uint32_t    pool_age;

    /* Static buffers handed to littlefs (no heap required).  They go
     * to the storage driver as they are, so they start on a cache line:
     * a DMA driver can then use them directly instead of copying every
     * block through its bounce buffer. */
    LFS_BUF_ALIGN uint8_t read_buf[FM_LFS_CACHE_SIZE];
    LFS_BUF_ALIGN uint8_t prog_buf[FM_LFS_CACHE_SIZE];
    uint8_t     lookahead[FM_LFS_LOOKAHEAD_SIZE];

    /* Open file pool (indexed by the handle stored in FM_FS_HANDLE_SIZE).
     * The lfs_file_config objects must live for the whole lifetime of the
     * open file: littlefs keeps a pointer to them (file->cfg) and reads
     * the cache buffer from it on every flush, so they cannot be stack
     * locals of the open function. */
    lfs_file_t  files[FM_MAX_OPEN_FILES];
    uint8_t     file_used[FM_MAX_OPEN_FILES];
    LFS_BUF_ALIGN uint8_t file_buf[FM_MAX_OPEN_FILES * FM_LFS_CACHE_SIZE];
    struct lfs_file_config file_cfg[FM_MAX_OPEN_FILES];

    /* Open directory pool. */
    lfs_dir_t   dirs[FM_MAX_OPEN_DIRS];
    uint8_t     dir_used[FM_MAX_OPEN_DIRS];
} LittleFS_Ctx;

typedef struct
{
    uint16_t index;
    uint8_t  used;
} LFSFileHandle;

typedef struct
{
    uint16_t index;
    uint8_t  used;
} LFSDirHandle;

typedef char lfs_file_handle_must_fit[
    (sizeof(LFSFileHandle) <= FM_FS_HANDLE_SIZE) ? 1 : -1];
typedef char lfs_dir_handle_must_fit[
    (sizeof(LFSDirHandle) <= FM_FS_DIRHANDLE_SIZE) ? 1 : -1];

/* One context per registered adapter instance (FM_MAX_FS). */
static LittleFS_Ctx g_lfs[FM_MAX_FS];
static uint32_t g_lfs_age;

static LittleFS_Ctx *lfs_ctx(FM_FS *fs)
{
    return (LittleFS_Ctx *)fs->user;
}

/* ------------------------------------------------------------------ */
/* Error mapping                                                       */
/* ------------------------------------------------------------------ */
static FM_Result lfs_map_err(int err)
{
    switch (err)
    {
    case LFS_ERR_OK:       return FM_OK;
    case LFS_ERR_IO:       return FM_ERR_IO;
    case LFS_ERR_CORRUPT:  return FM_ERR_CORRUPTED;
    case LFS_ERR_NOENT:    return FM_ERR_NOT_FOUND;
    case LFS_ERR_EXIST:    return FM_ERR_EXISTS;
    case LFS_ERR_NOTDIR:   return FM_ERR_NOT_DIRECTORY;
    case LFS_ERR_ISDIR:    return FM_ERR_IS_DIRECTORY;
    case LFS_ERR_NOTEMPTY: return FM_ERR_NOT_EMPTY;
    case LFS_ERR_BADF:     return FM_ERR_NOT_OPEN;
    case LFS_ERR_FBIG:     return FM_ERR_TOO_LARGE;
    case LFS_ERR_INVAL:    return FM_ERR_INVALID_ARGUMENT;
    case LFS_ERR_NOSPC:    return FM_ERR_NO_SPACE;
    case LFS_ERR_NOMEM:    return FM_ERR_NO_MEMORY;
    case LFS_ERR_NAMETOOLONG: return FM_ERR_TOO_LARGE;
    default:               return FM_ERR_IO;
    }
}

/* ------------------------------------------------------------------ */
/* Block device callbacks (bridge FM_Storage -> littlefs)              */
/* ------------------------------------------------------------------ */
static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buf, lfs_size_t size)
{
    LittleFS_Ctx *ctx = (LittleFS_Ctx *)c->context;
    uint64_t offset;
    FM_Result r;

    if (ctx->storage == NULL || ctx->storage->read == NULL ||
        size > 0xFFFFFFFFu)
    {
        return LFS_ERR_IO;
    }
    offset = (uint64_t)block * (uint64_t)c->block_size + (uint64_t)off;
    r = ctx->storage->read(ctx->storage, offset, buf, (uint32_t)size);
    if (r == FM_OK)                return LFS_ERR_OK;
    if (r == FM_ERR_CORRUPTED)     return LFS_ERR_CORRUPT;
    return LFS_ERR_IO;
}

static int lfs_bd_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buf, lfs_size_t size)
{
    LittleFS_Ctx *ctx = (LittleFS_Ctx *)c->context;
    uint64_t offset;
    FM_Result r;

    if (ctx->storage == NULL || ctx->storage->write == NULL ||
        size > 0xFFFFFFFFu)
    {
        return LFS_ERR_IO;
    }
    offset = (uint64_t)block * (uint64_t)c->block_size + (uint64_t)off;
    r = ctx->storage->write(ctx->storage, offset, buf, (uint32_t)size);
    if (r == FM_OK)                return LFS_ERR_OK;
    if (r == FM_ERR_CORRUPTED)     return LFS_ERR_CORRUPT;
    return LFS_ERR_IO;
}

static int lfs_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    LittleFS_Ctx *ctx = (LittleFS_Ctx *)c->context;
    uint64_t offset;
    FM_Result r;

    /* Devices without an explicit erase step (RAM/SD) are fine. */
    if (ctx->storage == NULL || ctx->storage->erase == NULL)
    {
        return LFS_ERR_OK;
    }
    offset = (uint64_t)block * (uint64_t)c->block_size;
    r = ctx->storage->erase(ctx->storage, offset, c->block_size);
    return (r == FM_OK) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int lfs_bd_sync(const struct lfs_config *c)
{
    LittleFS_Ctx *ctx = (LittleFS_Ctx *)c->context;

    if (ctx->storage == NULL || ctx->storage->sync == NULL)
    {
        return LFS_ERR_OK;
    }
    return (ctx->storage->sync(ctx->storage) == FM_OK) ? LFS_ERR_OK
                                                       : LFS_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Configuration helpers                                               */
/* ------------------------------------------------------------------ */

/* Round a (power-of-two) cache size down so it remains a multiple of
 * `prog` and a factor of `block`. */
static lfs_size_t lfs_fit_cache(lfs_size_t cache, lfs_size_t block,
                                lfs_size_t prog)
{
    while (cache > prog && (block % cache != 0 || cache % prog != 0))
    {
        cache >>= 1;
    }
    if (block % cache != 0 || cache % prog != 0)
    {
        cache = prog; /* prog divides block and itself */
    }
    return cache;
}

static FM_Result lfs_build_cfg(LittleFS_Ctx *ctx, FM_Storage *storage)
{
    lfs_size_t block;
    lfs_size_t read_size = FM_LFS_READ_SIZE;
    lfs_size_t prog_size = FM_LFS_PROG_SIZE;
    lfs_size_t cache;
    lfs_size_t lookahead = FM_LFS_LOOKAHEAD_SIZE;

    if (storage == NULL || storage->capacity == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    block = storage->erase_size ? storage->erase_size : storage->block_size;
    if (block == 0)
    {
        block = 512;
    }
    /* littlefs needs blocks of at least 128 bytes (its own metadata does
     * not fit below that; lfs_init() asserts it, and with assertions off
     * it writes outside the block).  A byte-addressed medium -- a RAM
     * disk from RamDisk_Register(), erase unit 1 -- gets the smallest
     * multiple of its unit that reaches 512. */
    if (block < 128u)
    {
        block = ((512u + block - 1u) / block) * block;
    }

    if (read_size == 0 || block % read_size != 0)
    {
        read_size = 1; /* divides any block */
    }
    if (prog_size == 0 || prog_size % read_size != 0)
    {
        prog_size = read_size;
    }
    if (block % prog_size != 0)
    {
        prog_size = read_size;
    }
    cache = lfs_fit_cache(FM_LFS_CACHE_SIZE, block, prog_size);
    if (lookahead < 8 || lookahead % 8 != 0)
    {
        lookahead = 128;
    }

    ctx->storage = storage;
    memset(&ctx->cfg, 0, sizeof(ctx->cfg));
    ctx->cfg.context = ctx;
    ctx->cfg.read = lfs_bd_read;
    ctx->cfg.prog = lfs_bd_prog;
    ctx->cfg.erase = lfs_bd_erase;
    ctx->cfg.sync = lfs_bd_sync;
    ctx->cfg.read_size = read_size;
    ctx->cfg.prog_size = prog_size;
    ctx->cfg.block_size = block;
    if (storage->capacity / block > (uint64_t)UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    ctx->cfg.block_count = (lfs_size_t)(storage->capacity / block);
    ctx->cfg.cache_size = cache;
    ctx->cfg.lookahead_size = lookahead;
    ctx->cfg.block_cycles = FM_LFS_BLOCK_CYCLES;
    ctx->cfg.read_buffer = ctx->read_buf;
    ctx->cfg.prog_buffer = ctx->prog_buf;
    ctx->cfg.lookahead_buffer = ctx->lookahead;

    /* two blocks hold the superblock pair; a volume needs more than
     * that to store anything, but two is what littlefs itself accepts */
    if (ctx->cfg.block_count < 2u)
    {
        return FM_ERR_NO_SPACE;
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Path helpers (framework paths are absolute; littlefs uses "/" root) */
/* ------------------------------------------------------------------ */
static void lfs_path(const char *path, char *out, size_t out_size)
{
    size_t n = 0;

    while (*path == '/')
    {
        path++;
    }
    if (*path == '\0')
    {
        if (out_size > 0)
        {
            out[0] = '/';
            out[1] = '\0';
        }
        return;
    }
    out[0] = '/';
    n = 1;
    while (*path != '\0' && n + 1 < out_size)
    {
        out[n++] = *path++;
    }
    /* strip trailing slash(es) */
    while (n > 1 && out[n - 1] == '/')
    {
        n--;
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Mount / format / check                                              */
/* ------------------------------------------------------------------ */
/* A context goes back to the pool when its volume is unmounted, and
 * when a mount fails (nothing is mounted then, so no unmount would ever
 * return it).  It used to stay taken for good: the fifth LittleFS_Init()
 * of a program -- a card that is taken out and put back often enough --
 * failed with FM_ERR_NO_MEMORY.
 *
 * The FM_FS keeps pointing at its context.  Mounting it again without
 * a new LittleFS_Init() takes the context back when nobody has it, and
 * is refused when another FM_FS does. */
static LittleFS_Ctx *lfs_claim(FM_FS *fs)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);

    if (ctx == NULL)
    {
        return NULL;
    }
    FM_Lock();
    if (!ctx->in_use)
    {
        ctx->in_use = FM_TRUE;
        ctx->owner = fs;
    }
    else if (ctx->owner != fs)
    {
        ctx = NULL;
    }
    if (ctx != NULL)
    {
        ctx->pool_busy = 1u;
    }
    FM_Unlock();
    return ctx;
}

static void lfs_release(LittleFS_Ctx *ctx)
{
    FM_Lock();
    ctx->mounted = FM_FALSE;
    ctx->storage = NULL;
    ctx->owner = NULL;
    ctx->in_use = FM_FALSE;
    ctx->pool_busy = 0u;
    FM_Unlock();
}

static FM_Result lfs_fs_mount(FM_FS *fs, FM_Storage *storage)
{
    LittleFS_Ctx *ctx = lfs_claim(fs);
    FM_Result r;
    int err;

    if (ctx == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    r = lfs_build_cfg(ctx, storage);
    if (r != FM_OK)
    {
        lfs_release(ctx);
        return r;
    }
    err = lfs_mount(&ctx->lfs, &ctx->cfg);
    if (err == LFS_ERR_CORRUPT && ctx->auto_format)
    {
        err = lfs_format(&ctx->lfs, &ctx->cfg);
        if (err == LFS_ERR_OK)
        {
            err = lfs_mount(&ctx->lfs, &ctx->cfg);
        }
    }
    if (err == LFS_ERR_OK)
    {
        ctx->mounted = FM_TRUE;
    }
    else
    {
        lfs_release(ctx);
    }
    return lfs_map_err(err);
}

static FM_Result lfs_fs_unmount(FM_FS *fs)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);

    if (ctx == NULL || ctx->owner != fs)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    if (ctx->mounted)
    {
        (void)lfs_unmount(&ctx->lfs);
    }
    lfs_release(ctx);
    return FM_OK;
}

/* FS_Format() of a mounted volume (the only kind it accepts).
 *
 * lfs_format() writes a new, empty file system and leaves the lfs_t in
 * the state of "not mounted".  This function used to call it on the
 * mounted lfs_t and return: the volume on the medium was new, the
 * lfs_t still described the old one, and every call after the format
 * failed with FM_ERR_CORRUPTED until the next reset.  So: unmount,
 * format, mount again. */
static FM_Result lfs_fs_format(FM_FS *fs)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    FM_Bool was_mounted;
    FM_Result r;
    int err;

    if (ctx == NULL || ctx->owner != fs || fs->storage == NULL)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    was_mounted = ctx->mounted;
    if (was_mounted)
    {
        (void)lfs_unmount(&ctx->lfs);
    }
    r = lfs_build_cfg(ctx, fs->storage);
    err = (r == FM_OK) ? lfs_format(&ctx->lfs, &ctx->cfg) : LFS_ERR_OK;
    if (was_mounted)
    {
        /* whatever the format said, the volume has to be usable again:
         * after a format that failed this mounts what is there */
        int merr = lfs_mount(&ctx->lfs, &ctx->cfg);

        if (merr != LFS_ERR_OK)
        {
            ctx->mounted = FM_FALSE;
            if (err == LFS_ERR_OK)
            {
                err = merr;
            }
        }
    }
    if (r != FM_OK)
    {
        return r;
    }
    return lfs_map_err(err);
}

static int lfs_check_block(void *data, lfs_block_t block)
{
    uint32_t *count = (uint32_t *)data;

    (void)block;
    (*count)++;
    return 0;
}

/* FS_Check(): walk every metadata block of the volume.  littlefs reads
 * each of them from the medium and checks its checksum on the way, so a
 * block that no longer holds what was written ends the walk with
 * LFS_ERR_CORRUPT.  (This used to answer FM_OK for any mounted volume
 * without reading a byte.)  File data is not read: littlefs keeps no
 * checksum of it. */
static FM_Result lfs_fs_check(FM_FS *fs)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    uint32_t blocks = 0u;
    int err;

    if (ctx == NULL || ctx->owner != fs || !ctx->mounted)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    err = lfs_fs_traverse(&ctx->lfs, lfs_check_block, &blocks);
    if (err != LFS_ERR_OK)
    {
        return lfs_map_err(err);
    }
    return (blocks >= 2u) ? FM_OK : FM_ERR_CORRUPTED;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */
static int lfs_flags(uint32_t mode)
{
    int flags = 0;

    if (mode & FM_OPEN_CREATE)   flags |= LFS_O_CREAT;
    if (mode & FM_OPEN_TRUNCATE) flags |= LFS_O_TRUNC;
    if (mode & FM_OPEN_APPEND)   flags |= LFS_O_APPEND;
    if ((mode & (FM_OPEN_READ | FM_OPEN_WRITE)) ==
        (FM_OPEN_READ | FM_OPEN_WRITE))   flags |= LFS_O_RDWR;
    else if (mode & FM_OPEN_WRITE)        flags |= LFS_O_WRONLY;
    else                                  flags |= LFS_O_RDONLY;
    return flags;
}

/* littlefs keeps nothing in common between two handles of one file:
 * each has its own copy of the size and of the block list, taken when it
 * was opened.  A second writer therefore does not see what the first one
 * wrote, and whichever of them is synced last wins -- the other one's
 * data is gone, with every call reporting success.  A reader next to a
 * writer is no better off: the blocks its copy points at are free as
 * far as the allocator is concerned as soon as the writer has replaced
 * them, and are handed out again.
 *
 * So a file that is open for writing has exactly one handle here: the
 * open that would add a second one is refused with FM_ERR_BUSY.  Any
 * number of read-only handles is fine. */
static FM_Bool lfs_same_file(const lfs_file_t *a, const lfs_file_t *b)
{
    if (a->id != b->id)
    {
        return FM_FALSE;
    }
    return ((a->m.pair[0] == b->m.pair[0] && a->m.pair[1] == b->m.pair[1]) ||
            (a->m.pair[0] == b->m.pair[1] && a->m.pair[1] == b->m.pair[0]))
           ? FM_TRUE : FM_FALSE;
}

static FM_Bool lfs_handle_writes(const lfs_file_t *f)
{
    return ((f->flags & LFS_O_WRONLY) == LFS_O_WRONLY ||
            (f->flags & LFS_O_TRUNC) != 0) ? FM_TRUE : FM_FALSE;
}

/* Slot `i` is reserved and not open yet; it is borrowed for a look at
 * the file (read-only, so closing it again changes nothing). */
static FM_Bool lfs_open_conflict(LittleFS_Ctx *ctx, uint32_t i,
                                 const char *lpath, uint32_t mode)
{
    FM_Bool writes = (mode & (FM_OPEN_WRITE | FM_OPEN_TRUNCATE))
                     ? FM_TRUE : FM_FALSE;
    FM_Bool conflict = FM_FALSE;
    FM_Bool candidates = FM_FALSE;
    uint32_t j;

    for (j = 0; j < FM_MAX_OPEN_FILES; j++)
    {
        if (j != i && ctx->file_used[j] &&
            (writes || lfs_handle_writes(&ctx->files[j])))
        {
            candidates = FM_TRUE;
            break;
        }
    }
    if (!candidates)
    {
        return FM_FALSE;
    }
    if (lfs_file_opencfg(&ctx->lfs, &ctx->files[i], lpath, LFS_O_RDONLY,
                         &ctx->file_cfg[i]) != LFS_ERR_OK)
    {
        return FM_FALSE;    /* not there (yet): nobody else has it open */
    }
    for (j = 0; j < FM_MAX_OPEN_FILES; j++)
    {
        if (j != i && ctx->file_used[j] &&
            (writes || lfs_handle_writes(&ctx->files[j])) &&
            lfs_same_file(&ctx->files[i], &ctx->files[j]))
        {
            conflict = FM_TRUE;
            break;
        }
    }
    (void)lfs_file_close(&ctx->lfs, &ctx->files[i]);
    return conflict;
}

static FM_Result lfs_fs_file_open(FM_FS *fs, void *fh, const char *path,
                                  uint32_t mode)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    char lpath[FM_PATH_MAX];
    uint32_t i;
    int err;

    if (fh == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (!ctx->file_used[i])
        {
            break;
        }
    }
    if (i == FM_MAX_OPEN_FILES)
    {
        FM_Unlock();
        return FM_ERR_NO_MEMORY;
    }
    ctx->file_used[i] = 1;
    FM_Unlock();

    memset(&ctx->file_cfg[i], 0, sizeof(ctx->file_cfg[i]));
    ctx->file_cfg[i].buffer = &ctx->file_buf[i * FM_LFS_CACHE_SIZE];

    if (lfs_open_conflict(ctx, i, lpath, mode))
    {
        FM_Lock();
        ctx->file_used[i] = 0;
        FM_Unlock();
        return FM_ERR_BUSY;
    }
    err = lfs_file_opencfg(&ctx->lfs, &ctx->files[i], lpath, lfs_flags(mode),
                           &ctx->file_cfg[i]);
    if (err != LFS_ERR_OK)
    {
        FM_Lock();
        ctx->file_used[i] = 0;
        FM_Unlock();
        return lfs_map_err(err);
    }
    h->index = (uint16_t)i;
    h->used = 1;
    return FM_OK;
}

static FM_Result lfs_fs_file_close(FM_FS *fs, void *fh)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    uint32_t i;
    int err;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    i = h->index;
    if (i >= FM_MAX_OPEN_FILES || !ctx->file_used[i])
    {
        return FM_ERR_NOT_OPEN;
    }
    err = lfs_file_close(&ctx->lfs, &ctx->files[i]);
    FM_Lock();
    ctx->file_used[i] = 0;
    FM_Unlock();
    h->used = 0;
    return lfs_map_err(err);
}

static FM_Result lfs_fs_file_read(FM_FS *fs, void *fh, void *buf,
                                  uint32_t len, uint32_t *out_read)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    lfs_ssize_t r;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if ((ctx->files[h->index].flags & LFS_O_RDONLY) != LFS_O_RDONLY)
    {
        return FM_ERR_ACCESS;      /* littlefs asserts on this */
    }
    r = lfs_file_read(&ctx->lfs, &ctx->files[h->index], buf, len);
    if (r < 0)
    {
        if (out_read) *out_read = 0;
        return lfs_map_err((int)r);
    }
    if (out_read) *out_read = (uint32_t)r;
    return (r == 0) ? FM_ERR_EOF : FM_OK;
}

static FM_Result lfs_fs_file_write(FM_FS *fs, void *fh, const void *buf,
                                   uint32_t len, uint32_t *written)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    lfs_ssize_t r;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if ((ctx->files[h->index].flags & LFS_O_WRONLY) != LFS_O_WRONLY)
    {
        if (written) *written = 0;
        return FM_ERR_ACCESS;      /* littlefs asserts on this */
    }
    r = lfs_file_write(&ctx->lfs, &ctx->files[h->index], buf, len);
    if (r < 0)
    {
        if (written) *written = 0;
        return lfs_map_err((int)r);
    }
    if (written) *written = (uint32_t)r;
    return FM_OK;
}

static FM_Result lfs_fs_file_seek(FM_FS *fs, void *fh, int64_t offset,
                                  FM_SeekOrigin origin)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    int whence;
    lfs_soff_t r;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    switch (origin)
    {
    case FM_SEEK_SET: whence = LFS_SEEK_SET; break;
    case FM_SEEK_CUR: whence = LFS_SEEK_CUR; break;
    case FM_SEEK_END: whence = LFS_SEEK_END; break;
    default: return FM_ERR_INVALID_ARGUMENT;
    }
    r = lfs_file_seek(&ctx->lfs, &ctx->files[h->index], (lfs_soff_t)offset,
                      whence);
    return (r < 0) ? lfs_map_err((int)r) : FM_OK;
}

static FM_Result lfs_fs_file_tell(FM_FS *fs, void *fh, uint64_t *pos)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    lfs_soff_t p;

    if (fh == NULL || !h->used || pos == NULL)
    {
        return FM_ERR_NOT_OPEN;
    }
    p = lfs_file_tell(&ctx->lfs, &ctx->files[h->index]);
    if (p < 0)
    {
        return lfs_map_err((int)p);
    }
    *pos = (uint64_t)p;
    return FM_OK;
}

static FM_Result lfs_fs_file_size(FM_FS *fs, void *fh, uint64_t *size)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    lfs_soff_t s;

    if (fh == NULL || !h->used || size == NULL)
    {
        return FM_ERR_NOT_OPEN;
    }
    s = lfs_file_size(&ctx->lfs, &ctx->files[h->index]);
    if (s < 0)
    {
        return lfs_map_err((int)s);
    }
    *size = (uint64_t)s;
    return FM_OK;
}

static FM_Result lfs_fs_file_truncate(FM_FS *fs, void *fh, uint64_t size)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;
    int err;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (size > (uint64_t)UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    /* littlefs asserts on this (lfs.c, lfs_file_truncate_) */
    if ((ctx->files[h->index].flags & LFS_O_WRONLY) != LFS_O_WRONLY)
    {
        return FM_ERR_ACCESS;
    }
    err = lfs_file_truncate(&ctx->lfs, &ctx->files[h->index], (lfs_off_t)size);
    return lfs_map_err(err);
}

static FM_Result lfs_fs_file_flush(FM_FS *fs, void *fh)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSFileHandle *h = (LFSFileHandle *)fh;

    if (fh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    return lfs_map_err(lfs_file_sync(&ctx->lfs, &ctx->files[h->index]));
}

/* ------------------------------------------------------------------ */
/* Path / metadata operations                                          */
/* ------------------------------------------------------------------ */
static FM_Result lfs_ops_stat(FM_FS *fs, const char *path, FM_FileInfo *info)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    struct lfs_info li;
    char lpath[FM_PATH_MAX];
    int err;

    if (path == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));
    err = lfs_stat(&ctx->lfs, lpath, &li);
    if (err != LFS_ERR_OK)
    {
        return lfs_map_err(err);
    }
    memset(info, 0, sizeof(*info));
    if (strlen(li.name) >= sizeof(info->name))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(info->name, li.name);
    info->type = (li.type == LFS_TYPE_DIR) ? FM_ITEM_DIR : FM_ITEM_FILE;
    info->size = (info->type == FM_ITEM_DIR) ? 0u : (uint64_t)li.size;
    return FM_OK;
}

static FM_Result lfs_fs_delete(FM_FS *fs, const char *path)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    char lpath[FM_PATH_MAX];

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));
    return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
}

static FM_Result lfs_fs_rename(FM_FS *fs, const char *old_path,
                               const char *new_path)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    char oldp[FM_PATH_MAX];
    char newp[FM_PATH_MAX];
    int err;

    if (old_path == NULL || new_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(old_path, oldp, sizeof(oldp));
    lfs_path(new_path, newp, sizeof(newp));

    if (strcmp(oldp, newp) == 0)
    {
        return FM_OK; /* renaming onto itself is a no-op */
    }

    err = lfs_rename(&ctx->lfs, oldp, newp);
    if (err == LFS_ERR_EXIST)
    {
        /* littlefs does not replace an existing destination.  Emulate
         * POSIX rename-over: remove the destination first.  lfs_remove
         * fails with LFS_ERR_NOTEMPTY for non-empty directories, which
         * matches the expected semantics. */
        err = lfs_remove(&ctx->lfs, newp);
        if (err == LFS_ERR_OK)
        {
            err = lfs_rename(&ctx->lfs, oldp, newp);
        }
    }
    return lfs_map_err(err);
}

static FM_Result lfs_fs_mkdir(FM_FS *fs, const char *path)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    char lpath[FM_PATH_MAX];

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));
    return lfs_map_err(lfs_mkdir(&ctx->lfs, lpath));
}

static FM_Result lfs_fs_rmdir(FM_FS *fs, const char *path)
{
    /* littlefs lfs_remove removes empty dirs and files alike. */
    return lfs_fs_delete(fs, path);
}

/* ------------------------------------------------------------------ */
/* Directory operations                                                */
/* ------------------------------------------------------------------ */
static FM_Result lfs_fs_dir_open(FM_FS *fs, void *dh, const char *path)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSDirHandle *h = (LFSDirHandle *)dh;
    char lpath[FM_PATH_MAX];
    uint32_t i;
    int err;

    if (dh == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_DIRS; i++)
    {
        if (!ctx->dir_used[i])
        {
            break;
        }
    }
    if (i == FM_MAX_OPEN_DIRS)
    {
        FM_Unlock();
        return FM_ERR_NO_MEMORY;
    }
    ctx->dir_used[i] = 1;
    FM_Unlock();

    err = lfs_dir_open(&ctx->lfs, &ctx->dirs[i], lpath);
    if (err != LFS_ERR_OK)
    {
        FM_Lock();
        ctx->dir_used[i] = 0;
        FM_Unlock();
        return lfs_map_err(err);
    }
    h->index = (uint16_t)i;
    h->used = 1;
    return FM_OK;
}

static FM_Result lfs_fs_dir_close(FM_FS *fs, void *dh)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSDirHandle *h = (LFSDirHandle *)dh;
    uint32_t i;
    int err;

    if (dh == NULL || !h->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    i = h->index;
    if (i >= FM_MAX_OPEN_DIRS || !ctx->dir_used[i])
    {
        return FM_ERR_NOT_OPEN;
    }
    err = lfs_dir_close(&ctx->lfs, &ctx->dirs[i]);
    FM_Lock();
    ctx->dir_used[i] = 0;
    FM_Unlock();
    h->used = 0;
    return lfs_map_err(err);
}

static FM_Result lfs_fs_dir_read(FM_FS *fs, void *dh, FM_DirEntry *entry)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    LFSDirHandle *h = (LFSDirHandle *)dh;
    struct lfs_info li;
    int err;

    if (dh == NULL || !h->used || entry == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (;;)
    {
        err = lfs_dir_read(&ctx->lfs, &ctx->dirs[h->index], &li);
        if (err <= 0)
        {
            return (err == 0) ? FM_ERR_EOF : lfs_map_err(err);
        }
        if (strcmp(li.name, ".") == 0 || strcmp(li.name, "..") == 0)
        {
            continue;
        }
        if (strlen(li.name) >= sizeof(entry->name))
        {
            return FM_ERR_TOO_LARGE;
        }
        memset(entry, 0, sizeof(*entry));
        (void)strcpy(entry->name, li.name);
        entry->type = (li.type == LFS_TYPE_DIR) ? FM_ITEM_DIR : FM_ITEM_FILE;
        entry->size = (entry->type == FM_ITEM_DIR) ? 0u : (uint64_t)li.size;
        return FM_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Space / type                                                        */
/* ------------------------------------------------------------------ */
static FM_Result lfs_fs_get_free(FM_FS *fs, uint64_t *free, uint64_t *total)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    lfs_ssize_t used;

    if (free == NULL || total == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!ctx->mounted || ctx->storage == NULL)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    used = lfs_fs_size(&ctx->lfs);
    *total = (uint64_t)ctx->cfg.block_count * (uint64_t)ctx->cfg.block_size;
    if (used < 0)
    {
        *free = 0;
        return lfs_map_err((int)used);
    }
    {
        uint64_t used_bytes = (uint64_t)used * (uint64_t)ctx->cfg.block_size;
        *free = (used_bytes < *total) ? *total - used_bytes : 0u;
    }
    return FM_OK;
}

static FM_Result lfs_fs_get_type(FM_FS *fs, char *type, size_t type_size)
{
    if (type == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (type_size < sizeof("LITTLEFS"))
    {
        return FM_ERR_TOO_LARGE;    /* the code every backend gives */
    }
    (void)fs;
    (void)strcpy(type, "LITTLEFS");
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
FM_Result LittleFS_Init(FM_FS *fs)
{
    LittleFS_Ctx *ctx = NULL;
    uint32_t i;

    if (fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(fs, 0, sizeof(*fs));

    FM_Lock();
    /* an FM_FS that is initialised again keeps the context it has */
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_lfs[i].in_use && g_lfs[i].owner == fs)
        {
            ctx = &g_lfs[i];
            break;
        }
    }
    for (i = 0; ctx == NULL && i < FM_MAX_FS; i++)
    {
        if (!g_lfs[i].in_use)
        {
            ctx = &g_lfs[i];
        }
    }
    /* None free.  A context that was handed out by an earlier
     * LittleFS_Init() and never got as far as a mount is not in use by
     * anybody: its FS_Mount() was refused before the driver was asked
     * (no such storage, mount path taken, mount table full), or was
     * never called.  Nothing gave such a context back, so FM_MAX_FS of
     * them ended every later LittleFS_Init() with FM_ERR_NO_MEMORY.
     * The oldest one is taken over.  Its FM_FS can no longer be mounted
     * (FM_ERR_NOT_READY) until it is initialised again, which is the
     * rule anyway: LittleFS_Init() before each FS_Mount(). */
    if (ctx == NULL)
    {
        for (i = 0; i < FM_MAX_FS; i++)
        {
            if (!g_lfs[i].pool_busy &&
                (ctx == NULL || g_lfs[i].pool_age < ctx->pool_age))
            {
                ctx = &g_lfs[i];
            }
        }
    }
    if (ctx != NULL)
    {
        memset(ctx, 0, sizeof(*ctx));
        ctx->auto_format = FM_TRUE;
        ctx->in_use = FM_TRUE;
        ctx->owner = fs;
        ctx->pool_age = ++g_lfs_age;
    }
    FM_Unlock();
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }

    (void)strncpy(fs->name, "LITTLEFS", sizeof(fs->name) - 1);
    fs->user = ctx;
    fs->ops.mount = lfs_fs_mount;
    fs->ops.unmount = lfs_fs_unmount;
    fs->ops.format = lfs_fs_format;
    fs->ops.check = lfs_fs_check;
    fs->ops.file_open = lfs_fs_file_open;
    fs->ops.file_close = lfs_fs_file_close;
    fs->ops.file_read = lfs_fs_file_read;
    fs->ops.file_write = lfs_fs_file_write;
    fs->ops.file_seek = lfs_fs_file_seek;
    fs->ops.file_tell = lfs_fs_file_tell;
    fs->ops.file_size = lfs_fs_file_size;
    fs->ops.file_truncate = lfs_fs_file_truncate;
    fs->ops.file_flush = lfs_fs_file_flush;
    fs->ops.stat = lfs_ops_stat;
    fs->ops.delete_file = lfs_fs_delete;
    fs->ops.rename = lfs_fs_rename;
    fs->ops.mkdir = lfs_fs_mkdir;
    fs->ops.rmdir = lfs_fs_rmdir;
    fs->ops.dir_open = lfs_fs_dir_open;
    fs->ops.dir_close = lfs_fs_dir_close;
    fs->ops.dir_read = lfs_fs_dir_read;
    fs->ops.get_free = lfs_fs_get_free;
    fs->ops.get_type = lfs_fs_get_type;
    return FM_OK;
}

/* Write a new, empty LittleFS to a storage no file system is mounted on.
 *
 * This is the format for a volume that cannot be mounted: with
 * LittleFS_SetAutoFormat(fs, FM_FALSE) a blank or damaged medium fails
 * at FS_Mount(), and FS_Format() only takes a mount path.
 *
 *   FM_ERR_BUSY       a mount uses the storage (use FS_Format there)
 *   FM_ERR_NO_MEMORY  all FM_MAX_FS LittleFS contexts are mounted
 *
 * The storage is mounted (Storage_Mount) when it was not, as FS_Mount()
 * does, and stays mounted.
 *
 * The work needs the buffers of one context.  One that is not mounted is
 * borrowed and handed back as it was: an FM_FS that LittleFS_Init() has
 * prepared, or one whose mount has just failed, keeps its context and
 * its auto-format setting, and can be mounted right after this call.
 * (While the format runs, a second task that mounts that very FM_FS gets
 * FM_ERR_NOT_READY.) */
FM_Result LittleFS_FormatStorage(FM_Storage *storage)
{
    static FM_FS borrower;      /* only its address is used */
    LittleFS_Ctx *ctx = NULL;
    FM_Bool was_in_use = FM_FALSE;
    FM_FS *was_owner = NULL;
    FM_Result r;
    int err = LFS_ERR_OK;
    uint32_t i;

    if (storage == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (FM_VFS_StorageInUse(storage))
    {
        return FM_ERR_BUSY;
    }
    if (!FM_Storage_IsMounted(storage))
    {
        r = Storage_Mount(storage->name);
        if (r != FM_OK)
        {
            return r;
        }
    }

    FM_Lock();
    for (i = 0; i < FM_MAX_FS; i++)
    {
        /* not in a mount; a free one rather than one an FM_FS holds */
        if (!g_lfs[i].pool_busy &&
            (ctx == NULL || (ctx->in_use && !g_lfs[i].in_use)))
        {
            ctx = &g_lfs[i];
        }
    }
    if (ctx != NULL)
    {
        was_in_use = ctx->in_use;
        was_owner = ctx->owner;
        ctx->in_use = FM_TRUE;
        ctx->owner = &borrower;
        ctx->pool_busy = 1u;
    }
    FM_Unlock();
    if (ctx == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }

    r = lfs_build_cfg(ctx, storage);
    if (r == FM_OK)
    {
        err = lfs_format(&ctx->lfs, &ctx->cfg);
    }

    FM_Lock();
    ctx->mounted = FM_FALSE;
    ctx->storage = NULL;
    ctx->in_use = was_in_use;
    ctx->owner = was_owner;
    ctx->pool_busy = 0u;
    FM_Unlock();

    return (r != FM_OK) ? r : lfs_map_err(err);
}

void LittleFS_SetAutoFormat(FM_FS *fs, FM_Bool auto_format)
{
    LittleFS_Ctx *ctx;

    if (fs == NULL)
    {
        return;
    }
    ctx = lfs_ctx(fs);
    if (ctx != NULL && ctx->owner == fs)
    {
        ctx->auto_format = auto_format;
    }
}

#endif /* FM_USE_LITTLEFS */
