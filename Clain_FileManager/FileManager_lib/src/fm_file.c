/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_file.c - Unified File API (Simple + Advanced)
 */

#define FM_DEBUG_TAG "file"
#include "fm_debug.h"
#include "fm_internal.h"
#include "fm_cache.h"
#include <string.h>

#if !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
static uint8_t g_file_bounce[512] FM_DMA_ALIGNED;

static FM_Result file_read_backend(FM_File *f, void *buf, uint32_t len,
                                   uint32_t *got)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t total = 0;
    FM_Result r = FM_OK;

    if (FM_Cache_SdDirectOk(buf, len))
    {
        return f->fs->ops.file_read(f->fs, f->fh, buf, len, got);
    }
    while (total < len)
    {
        uint32_t chunk = len - total;
        uint32_t rd = 0;

        if (chunk > sizeof(g_file_bounce))
        {
            chunk = (uint32_t)sizeof(g_file_bounce);
        }
        r = f->fs->ops.file_read(f->fs, f->fh, g_file_bounce, chunk, &rd);
        if (rd > 0u)
        {
            memcpy(out + total, g_file_bounce, rd);
            total += rd;
        }
        if (r != FM_OK && r != FM_ERR_EOF)
        {
            break;
        }
        if (rd == 0u)
        {
            break;
        }
    }
    *got = total;
    if (total > 0u && r == FM_ERR_EOF)
    {
        return FM_OK;
    }
    return (total > 0u) ? FM_OK : r;
}

static FM_Result file_write_backend(FM_File *f, const void *buf, uint32_t len,
                                    uint32_t *put)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t total = 0;
    FM_Result r = FM_OK;

    if (FM_Cache_SdDirectOk(buf, len))
    {
        return f->fs->ops.file_write(f->fs, f->fh, buf, len, put);
    }
    while (total < len)
    {
        uint32_t chunk = len - total;
        uint32_t wr = 0;

        if (chunk > sizeof(g_file_bounce))
        {
            chunk = (uint32_t)sizeof(g_file_bounce);
        }
        memcpy(g_file_bounce, in + total, chunk);
        r = f->fs->ops.file_write(f->fs, f->fh, g_file_bounce, chunk, &wr);
        total += wr;
        if (r != FM_OK || wr == 0u)
        {
            break;
        }
    }
    *put = total;
    return r;
}
#else
#define file_read_backend(f, buf, len, got) \
    ((f)->fs->ops.file_read((f)->fs, (f)->fh, (buf), (len), (got)))
#define file_write_backend(f, buf, len, put) \
    ((f)->fs->ops.file_write((f)->fs, (f)->fh, (buf), (len), (put)))
#endif

/* The adapter file handles (e.g. POSIX `int fd`) are stored in the byte
 * array FM_File.fh; the member offset must keep them naturally aligned. */
typedef char fm_file_fh_aligned[
    ((offsetof(FM_File, fh) % 4) == 0) ? 1 : -1];

static FM_File g_pool[FM_MAX_OPEN_FILES];
static FM_File *g_external_files[FM_MAX_OPEN_FILES];

/* ------------------------------------------------------------------ */
/* Optional file-level read cache (FM_USE_CACHE)                       */
/*                                                                     */
/* A small set of fixed-size cache lines keyed by (file, offset).  File_Read
 * consults the cache before hitting the filesystem backend, so small reads
 * and repeated scans avoid backend round-trips.  The cache is read-only:
 * File_Write / File_Truncate / File_Close invalidate the affected lines. */
/* ------------------------------------------------------------------ */
#if FM_USE_CACHE

typedef struct
{
    FM_File  *file;                    /* owning handle (NULL = free)  */
    uint64_t  offset;                  /* file offset of the block     */
    uint32_t  len;                     /* valid bytes in data          */
    uint8_t   valid;
    uint8_t   data[FM_CACHE_LINE_SIZE] FM_DMA_ALIGNED;
} FM_CacheLine;

static FM_CacheLine g_cache[FM_CACHE_LINES];
static FM_Bool      g_cache_enabled = FM_TRUE;
static uint32_t     g_cache_evict_pos;
static uint32_t     g_cache_hits;
static uint32_t     g_cache_misses;

/* The cache is shared by every open file, so threaded builds protect it
 * with a dedicated mutex.  cache_find / cache_fill / cache_invalidate
 * must be called with the lock already held; cache_fill keeps it held
 * across the backend read/seek calls, which serializes cache misses but
 * keeps the line ownership race-free.  Backend file ops never take this
 * lock, so there is no lock-ordering cycle. */
#if FM_USE_THREADS
static FM_Mutex g_cache_lock;
static FM_Bool  g_cache_lock_init = FM_FALSE;

static void cache_lock(void)
{
    if (!g_cache_lock_init)
    {
        if (FM_Mutex_Create(&g_cache_lock) == FM_OK)
        {
            g_cache_lock_init = FM_TRUE;
        }
    }
    if (g_cache_lock_init)
    {
        FM_Mutex_Lock(&g_cache_lock);
    }
}

static void cache_unlock(void)
{
    if (g_cache_lock_init)
    {
        FM_Mutex_Unlock(&g_cache_lock);
    }
}
#else
#define cache_lock()   ((void)0)
#define cache_unlock() ((void)0)
#endif

static void cache_invalidate_file(FM_File *f)
{
    uint32_t i;

    for (i = 0; i < FM_CACHE_LINES; i++)
    {
        if (g_cache[i].file == f)
        {
            g_cache[i].valid = 0;
            g_cache[i].file = NULL;
        }
    }
}

static FM_CacheLine *cache_find(FM_File *f, uint64_t off)
{
    uint32_t i;

    for (i = 0; i < FM_CACHE_LINES; i++)
    {
        FM_CacheLine *l = &g_cache[i];

        if (l->valid && l->file == f && l->len > 0 &&
            off >= l->offset && off < l->offset + l->len)
        {
            return l;
        }
    }
    return NULL;
}

static FM_CacheLine *cache_evict(void)
{
    FM_CacheLine *l = &g_cache[g_cache_evict_pos];

    g_cache_evict_pos = (g_cache_evict_pos + 1) % FM_CACHE_LINES;
    l->valid = 0;
    l->file = NULL;
    l->len = 0;
    return l;
}

/* Read the aligned block covering `block_off` into a cache line, then
 * restore the file position.  On success *out points at the filled line.
 * Must be called with the cache lock held (see cache_lock above). */
static FM_Result cache_fill(FM_File *f, uint64_t block_off, FM_CacheLine **out)
{
    uint64_t save = f->pos;
    FM_CacheLine *line = cache_evict();
    uint32_t got = 0;
    FM_Result r = FM_OK;

    line->file = f;
    line->offset = block_off;
    line->len = 0;
    line->valid = 0;

    if (f->fs->ops.file_seek != NULL)
    {
        r = f->fs->ops.file_seek(f->fs, f->fh, (int64_t)block_off,
                                 FM_SEEK_SET);
        if (r != FM_OK)
        {
            line->valid = 0;
            return r;
        }
    }
    while (got < FM_CACHE_LINE_SIZE)
    {
        uint32_t rd = 0;

        r = f->fs->ops.file_read(f->fs, f->fh, line->data + got,
                                 FM_CACHE_LINE_SIZE - got, &rd);
        if (r == FM_OK)
        {
            got += rd;
            if (rd == 0)
            {
                break;
            }
        }
        else if (r == FM_ERR_EOF)
        {
            got += rd;
            r = FM_OK; /* EOF during block fill is not an error here */
            break;
        }
        else
        {
            line->valid = 0;
            break;
        }
    }
    if (got == 0)
    {
        /* EOF: leave the line invalid so an empty block never evicts a
         * useful one and never matches a later read */
        line->valid = 0;
        line->file = NULL;
    }
    else
    {
        line->len = got;
        line->valid = 1;
    }

    /* restore the logical file position */
    if (f->fs->ops.file_seek != NULL)
    {
        (void)f->fs->ops.file_seek(f->fs, f->fh, (int64_t)save, FM_SEEK_SET);
    }
    f->pos = save;
    *out = line;
    return r;
}

void File_SetCacheEnabled(FM_Bool enabled)
{
    cache_lock();
    g_cache_enabled = enabled ? FM_TRUE : FM_FALSE;
    cache_unlock();
}

FM_Bool File_GetCacheEnabled(void)
{
    FM_Bool enabled;

    cache_lock();
    enabled = g_cache_enabled;
    cache_unlock();
    return enabled;
}

void File_CacheGetStats(uint32_t *hits, uint32_t *misses)
{
    cache_lock();
    if (hits != NULL)    *hits = g_cache_hits;
    if (misses != NULL)  *misses = g_cache_misses;
    cache_unlock();
}

void File_CacheResetStats(void)
{
    cache_lock();
    g_cache_hits = 0;
    g_cache_misses = 0;
    cache_unlock();
}

#endif /* FM_USE_CACHE */

void File_Init(FM_File *f)
{
    if (f != NULL)
    {
        memset(f, 0, sizeof(*f));
    }
}

static void external_file_remove(FM_File *f)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (g_external_files[i] == f)
        {
            g_external_files[i] = NULL;
            FM_Unlock();
            return;
        }
    }
    FM_Unlock();
}

/* ------------------------------------------------------------------ */
/* Pool helpers                                                        */
/* ------------------------------------------------------------------ */
static FM_File *file_acquire(void)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (!g_pool[i].used)
        {
            memset(&g_pool[i], 0, sizeof(FM_File));
            g_pool[i].used = 1;
            FM_Unlock();
            return &g_pool[i];
        }
    }
    FM_Unlock();
    return NULL;
}

static void file_release(FM_File *f)
{
    FM_Lock();
    if (f != NULL && f->used)
    {
        f->used = 0;
    }
    FM_Unlock();
}

static FM_Bool file_is_pool(const FM_File *f)
{
    uintptr_t p;
    uintptr_t first;
    uintptr_t last;

    if (f == NULL)
    {
        return FM_FALSE;
    }
    p = (uintptr_t)(const void *)f;
    first = (uintptr_t)(const void *)&g_pool[0];
    last = (uintptr_t)(const void *)&g_pool[FM_MAX_OPEN_FILES];
    return p >= first && p < last;
}

FM_Bool FM_File_AnyOpenOnMount(const FM_MountEntry *mount)
{
    uint32_t i;

    /* Called while the VFS lock is held by FS_Unmount. */
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (g_pool[i].used && g_pool[i].mount == (void *)mount)
        {
            return FM_TRUE;
        }
        if (g_external_files[i] != NULL && g_external_files[i]->used &&
            g_external_files[i]->mount == (void *)mount)
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

uint32_t File_PoolUsed(void)
{
    uint32_t i;
    uint32_t used = 0u;
    uint32_t ext = 0u;

    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (g_pool[i].used)
        {
            used++;
        }
        if (g_external_files[i] != NULL && g_external_files[i]->used)
        {
            ext++;
        }
    }
    return used + ext;
}

void File_PoolDump(void)
{
    uint32_t i;

    FM_Log("file pool: %lu/%u internal",
           (unsigned long)File_PoolUsed(), (unsigned)FM_MAX_OPEN_FILES);
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        if (g_pool[i].used)
        {
            FM_Log("  [%lu] %s", (unsigned long)i, g_pool[i].path);
        }
        if (g_external_files[i] != NULL && g_external_files[i]->used)
        {
            FM_Log("  [x%lu] %s", (unsigned long)i,
                   g_external_files[i]->path);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */
static FM_Result file_open_internal(FM_File *f, const char *path,
                                    uint32_t mode)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_FS *fs;
    FM_Result r;

    if (f == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (mode == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if ((mode & (FM_OPEN_READ | FM_OPEN_WRITE)) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    /* The caller has prepared f (pool slots are claimed under FM_Lock by
     * file_acquire; user handles are memset by File_OpenEx).  Mark the
     * handle busy BEFORE the backend open so the slot cannot be grabbed
     * by another thread while the filesystem call is in progress. */
    f->used = 1;

    r = FM_VFS_Resolve(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        file_release(f);
        return r;
    }
    if (rel[0] == '\0')
    {
        /* Opening the mount root as a file is never meaningful and the
         * adapters disagree on it (POSIX would happily open "."). */
        file_release(f);
        return FM_ERR_IS_DIRECTORY;
    }
    fs = mount->fs;

    /* Normalize directly into f->path instead of an extra stack buffer:
     * this function sits at the deepest point of the File_* call chain
     * (FM_VFS_Resolve's normalized[] + this rel[] already cost
     * 2 * FM_PATH_MAX of stack; backends add their own path buffers on
     * top, e.g. FatFS builds a drive-qualified copy). */
    (void)strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';
    if (Path_Normalize(f->path, sizeof(f->path)) != FM_OK)
    {
        file_release(f);
        return FM_ERR_INVALID_PATH;
    }
    (void)strcpy(f->resolved, f->path);

    r = fs->ops.file_open(fs, f->fh, rel, mode);
    if (r != FM_OK)
    {
        file_release(f);
        return r;
    }

    f->mode = (uint8_t)mode;
    f->mount = (void *)mount;
    f->fs = fs;

    /* refresh cached size / position */
    if (fs->ops.file_size != NULL)
    {
        (void)fs->ops.file_size(fs, f->fh, &f->size);
    }
    if (fs->ops.file_tell != NULL)
    {
        (void)fs->ops.file_tell(fs, f->fh, &f->pos);
    }

    if (mode & FM_OPEN_APPEND)
    {
        if (fs->ops.file_seek != NULL)
        {
            (void)fs->ops.file_seek(fs, f->fh, (int64_t)f->size,
                                    FM_SEEK_SET);
            f->pos = f->size;
        }
    }
    return FM_OK;
}

FM_File *File_Open(const char *path, uint32_t mode)
{
    FM_File *f;
    FM_Result r;

    /* Validate BEFORE claiming a pool slot.  file_open_internal() rejects
     * these too, but on that early path it returns without releasing the
     * slot, so every File_Open(NULL, ...) / File_Open(p, 0) -- and every
     * File_ReadAll/WriteAll/ReadText(NULL, ...) built on it -- leaked one
     * of the FM_MAX_OPEN_FILES handles for good. */
    if (path == NULL || (mode & (FM_OPEN_READ | FM_OPEN_WRITE)) == 0u)
    {
        FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    f = file_acquire();
    if (f == NULL)
    {
        FM_Core_SetLastError(FM_ERR_NO_MEMORY);
        return NULL;
    }
    r = file_open_internal(f, path, mode);
    if (r != FM_OK)
    {
        /* file_open_internal already released the slot */
        FM_Core_SetLastError(r);
        return NULL;
    }
    FM_Core_SetLastError(FM_OK);
    return f;
}

FM_Result File_OpenEx(FM_File *f, const char *path, uint32_t mode)
{
    FM_Result r;

    FM_CHECK_PTR(f, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    if (f->used)
    {
        FM_ERR_LOG("File_OpenEx: handle already in use");
        return FM_ERR_BUSY;
    }
    memset(f, 0, sizeof(*f));
    r = file_open_internal(f, path, mode);
    if (r == FM_OK)
    {
        uint32_t i;
        FM_EVENT_LOG("opened %s (mode 0x%lx)", path, (unsigned long)mode);
        FM_Bool registered = FM_FALSE;

        FM_Lock();
        for (i = 0; i < FM_MAX_OPEN_FILES; i++)
        {
            if (g_external_files[i] == NULL)
            {
                g_external_files[i] = f;
                registered = FM_TRUE;
                break;
            }
        }
        FM_Unlock();
        if (!registered)
        {
            (void)File_Close(f);
            r = FM_ERR_NO_MEMORY;
        }
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_Close(FM_File *f)
{
    FM_Result r = FM_OK;

    if (f == NULL || !f->used)
    {
        return FM_ERR_NOT_OPEN;
    }
    if (f->fs != NULL && f->fs->ops.file_flush != NULL)
    {
        r = f->fs->ops.file_flush(f->fs, f->fh);
    }
    if (f->fs != NULL && f->fs->ops.file_close != NULL)
    {
        FM_Result close_result = f->fs->ops.file_close(f->fs, f->fh);

        if (r == FM_OK)
        {
            r = close_result;
        }
    }
#if FM_USE_CACHE
    cache_lock();
    cache_invalidate_file(f);
    cache_unlock();
#endif
    if (file_is_pool(f))
    {
        file_release(f);
    }
    else
    {
        external_file_remove(f);
        memset(f, 0, sizeof(*f));
    }
    return r;
}

FM_Bool File_IsOpen(const FM_File *f)
{
    return (f != NULL && f->used) ? FM_TRUE : FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* Read / write / seek                                                 */
/* ------------------------------------------------------------------ */
FM_Result File_Read(FM_File *f, void *buf, uint32_t len, uint32_t *read)
{
    FM_Result r;

    if (f == NULL || !f->used || buf == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!(f->mode & FM_OPEN_READ))
    {
        return FM_ERR_ACCESS;
    }
    if (f->fs->ops.file_read == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (read != NULL)
    {
        *read = 0;
    }
#if FM_USE_CACHE
    /* The fixed 512-byte cache is intended for small/repeated reads. Routing
     * a multi-kilobyte media read through it turns one FatFS multi-sector
     * transfer into many single-sector reads and seeks, which can stall a
     * real-time audio refill for hundreds of milliseconds. Large sequential
     * reads therefore go directly to the filesystem backend. The backend
     * position is already kept synchronized after cached reads and seeks. */
    if (g_cache_enabled &&
        len >= (uint32_t)FM_CACHE_LINE_SIZE * 2u)
    {
        uint32_t local_read = 0;
        uint32_t *read_ptr = (read != NULL) ? read : &local_read;

        r = file_read_backend(f, buf, len, read_ptr);
        if (r == FM_OK || (r == FM_ERR_EOF && *read_ptr > 0u))
        {
            f->pos += *read_ptr;
        }
        return (*read_ptr > 0u && r == FM_ERR_EOF) ? FM_OK : r;
    }
    if (g_cache_enabled)
    {
        uint8_t *out = (uint8_t *)buf;
        uint32_t total = 0;
        uint32_t remaining = len;

        r = FM_OK;
        while (remaining > 0)
        {
            FM_CacheLine *line;
            FM_Bool served = FM_FALSE;

            /* Look up + copy under the cache lock so a concurrent
             * eviction can never reuse the line mid-copy. */
            cache_lock();
            line = cache_find(f, f->pos);
            if (line != NULL)
            {
                uint64_t off = f->pos - line->offset;
                uint32_t avail = line->len - (uint32_t)off;
                uint32_t n = (remaining < avail) ? remaining : avail;

                g_cache_hits++;
                memcpy(out, line->data + off, n);
                out += n;
                total += n;
                remaining -= n;
                f->pos += n;
                served = FM_TRUE;
                cache_unlock();
            }
            else
            {
                uint64_t block_off =
                    (f->pos / FM_CACHE_LINE_SIZE) * FM_CACHE_LINE_SIZE;

                if (f->size != 0 && block_off >= f->size)
                {
                    cache_unlock();
                    r = FM_ERR_EOF;
                    break;
                }
                g_cache_misses++;
                r = cache_fill(f, block_off, &line);
                if (r == FM_OK && line->len > 0 &&
                    f->pos < line->offset + line->len)
                {
                    uint64_t off = f->pos - line->offset;
                    uint32_t avail = line->len - (uint32_t)off;
                    uint32_t n = (remaining < avail) ? remaining : avail;

                    memcpy(out, line->data + off, n);
                    out += n;
                    total += n;
                    remaining -= n;
                    f->pos += n;
                    served = FM_TRUE;
                }
                cache_unlock();
                if (r != FM_OK)
                {
                    break;
                }
                if (!served)
                {
                    r = FM_ERR_EOF;
                    break;
                }
            }
        }
        if (read != NULL)
        {
            *read = total;
        }
        if (total > 0 && f->fs->ops.file_seek != NULL)
        {
            /* keep the backend position in sync with f->pos so File_Tell
             * still reflects the logical position after cached reads */
            (void)f->fs->ops.file_seek(f->fs, f->fh, (int64_t)f->pos,
                                       FM_SEEK_SET);
        }
        return (total > 0) ? FM_OK : r;
    }
#endif
    {
        uint32_t local_read = 0;
        uint32_t *read_ptr = (read != NULL) ? read : &local_read;

        r = file_read_backend(f, buf, len, read_ptr);
        if (r == FM_OK)
        {
            f->pos += *read_ptr;
        }
    }
    return r;
}

FM_Result File_Write(FM_File *f, const void *buf, uint32_t len,
                     uint32_t *written)
{
    FM_Result r;

    if (f == NULL || !f->used || buf == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!(f->mode & FM_OPEN_WRITE))
    {
        return FM_ERR_ACCESS;
    }
    if (f->fs->ops.file_write == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (written != NULL)
    {
        *written = 0;
    }
#if FM_USE_CACHE
    cache_lock();
    cache_invalidate_file(f);
    cache_unlock();
#endif
    {
        uint32_t local_written = 0;
        uint32_t *written_ptr = (written != NULL) ? written : &local_written;

        r = file_write_backend(f, buf, len, written_ptr);
        if (r == FM_OK)
        {
            f->pos += *written_ptr;
            if (f->pos > f->size)
            {
                f->size = f->pos;
            }
        }
    }
    return r;
}

FM_Result File_Append(FM_File *f, const void *buf, uint32_t len,
                      uint32_t *written)
{
    FM_Result r;

    if (f == NULL || !f->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!(f->mode & FM_OPEN_WRITE))
    {
        return FM_ERR_ACCESS;
    }
    if (f->fs->ops.file_seek != NULL)
    {
        r = f->fs->ops.file_seek(f->fs, f->fh, (int64_t)f->size,
                                 FM_SEEK_SET);
        if (r != FM_OK)
        {
            return r;
        }
        f->pos = f->size;
    }
    return File_Write(f, buf, len, written);
}

FM_Result File_Seek(FM_File *f, int64_t offset, FM_SeekOrigin origin)
{
    FM_Result r;

    if (f == NULL || !f->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (f->fs->ops.file_seek == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    r = f->fs->ops.file_seek(f->fs, f->fh, offset, origin);
    if (r == FM_OK && f->fs->ops.file_tell != NULL)
    {
        (void)f->fs->ops.file_tell(f->fs, f->fh, &f->pos);
    }
    return r;
}

FM_Result File_Tell(FM_File *f, uint64_t *pos)
{
    if (f == NULL || !f->used || pos == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (f->fs->ops.file_tell != NULL)
    {
        FM_Result r = f->fs->ops.file_tell(f->fs, f->fh, pos);

        if (r == FM_OK)
        {
            f->pos = *pos;
        }
        return r;
    }
    *pos = f->pos;
    return FM_OK;
}

FM_Result File_Size(FM_File *f, uint64_t *size)
{
    if (f == NULL || !f->used || size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (f->fs->ops.file_size != NULL)
    {
        FM_Result r = f->fs->ops.file_size(f->fs, f->fh, size);

        if (r == FM_OK)
        {
            f->size = *size;
        }
        return r;
    }
    *size = f->size;
    return FM_OK;
}

FM_Result File_Truncate(FM_File *f, uint64_t size)
{
    FM_Result r;

    if (f == NULL || !f->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (f->fs->ops.file_truncate == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    r = f->fs->ops.file_truncate(f->fs, f->fh, size);
    if (r == FM_OK)
    {
#if FM_USE_CACHE
        cache_lock();
        cache_invalidate_file(f);
        cache_unlock();
#endif
        f->size = size;
        if (f->pos > size)
        {
            f->pos = size;
        }
    }
    return r;
}

FM_Result File_Flush(FM_File *f)
{
    if (f == NULL || !f->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (f->fs->ops.file_flush != NULL)
    {
        return f->fs->ops.file_flush(f->fs, f->fh);
    }
    return FM_OK;
}

FM_Bool File_EOF(FM_File *f)
{
    if (f == NULL || !f->used)
    {
        return FM_TRUE;
    }
    if (f->fs->ops.file_size != NULL)
    {
        uint64_t size = 0;

        (void)f->fs->ops.file_size(f->fs, f->fh, &size);
        f->size = size;
    }
    return (f->pos >= f->size) ? FM_TRUE : FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* Simple API: whole-file operations                                   */
/* ------------------------------------------------------------------ */
FM_Result File_Create(const char *path)
{
    FM_File *f = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE |
                                  FM_OPEN_TRUNCATE);

    if (f == NULL)
    {
        return File_GetLastError();
    }
    return File_Close(f);
}

FM_Result File_Delete(const char *path)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_Result r;

    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    r = FM_VFS_Resolve(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        FM_ERR_LOG("delete %s: resolve failed (%d)", path, (int)r);
        FM_Core_SetLastError(r);
        return r;
    }
    if (rel[0] == '\0')
    {
        FM_ERR_LOG("delete: cannot delete mount root");
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (mount->fs->ops.delete_file == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    r = mount->fs->ops.delete_file(mount->fs, rel);
    if (r == FM_OK)
    {
        FM_EVENT_LOG("deleted %s", path);
    }
    else
    {
        FM_ERR_LOG("delete %s: backend returned %d", path, (int)r);
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_Rename(const char *old_path, const char *new_path)
{
    FM_MountEntry *m1 = NULL;
    FM_MountEntry *m2 = NULL;
    char r1[FM_PATH_MAX];
    char r2[FM_PATH_MAX];
    FM_Result r;

    r = FM_VFS_Resolve(old_path, &m1, r1, sizeof(r1));
    if (r != FM_OK)
    {
        return r;
    }
    r = FM_VFS_Resolve(new_path, &m2, r2, sizeof(r2));
    if (r != FM_OK)
    {
        return r;
    }
    if (m1 != m2)
    {
        return FM_ERR_UNSUPPORTED; /* cross-mount rename: use Move */
    }
    if (m1->fs->ops.rename == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    r = m1->fs->ops.rename(m1->fs, r1, r2);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_Exists(const char *path, FM_Bool *exists)
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
        *exists = FM_TRUE;
        return FM_OK;
    }
    if (r == FM_ERR_NOT_FOUND)
    {
        *exists = FM_FALSE;
        return FM_OK;
    }
    return r;
}

FM_Result File_GetInfo(const char *path, FM_FileInfo *info)
{
    FM_MountEntry *mount = NULL;
    char rel[FM_PATH_MAX];
    FM_Result r;

    if (info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_VFS_Resolve(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
        return r;
    }
    if (mount->fs->ops.stat == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    memset(info, 0, sizeof(*info));
    r = mount->fs->ops.stat(mount->fs, rel, info);
    if (r == FM_OK)
    {
        char norm[FM_PATH_MAX];

        (void)strncpy(norm, path, sizeof(norm) - 1);
        norm[sizeof(norm) - 1] = '\0';
        (void)Path_Normalize(norm, sizeof(norm));
        (void)strcpy(info->path, norm);
    }
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_SizeOf(const char *path, uint64_t *size)
{
    FM_FileInfo info;
    FM_Result r;

    if (size == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_GetInfo(path, &info);
    if (r != FM_OK)
    {
        return r;
    }
    *size = info.size;
    return FM_OK;
}

FM_Result File_ReadAll(const char *path, void *buffer, size_t buffer_size,
                       size_t *read)
{
    FM_File *f;
    FM_Result r;
    uint64_t size = 0;
    uint32_t rd = 0;

    if (buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (read != NULL)
    {
        *read = 0;
    }

    f = File_Open(path, FM_OPEN_READ);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    r = File_Size(f, &size);
    if (r != FM_OK)
    {
        (void)File_Close(f);
        return r;
    }
    if (size > buffer_size || size > UINT32_MAX)
    {
        (void)File_Close(f);
        FM_Core_SetLastError(FM_ERR_TOO_LARGE);
        return FM_ERR_TOO_LARGE;
    }
    if (size == 0)
    {
        (void)File_Close(f);
        FM_Core_SetLastError(FM_OK);
        return FM_OK;
    }
    r = File_Read(f, buffer, (uint32_t)size, &rd);
    if (r == FM_OK)
    {
        if (read != NULL)
        {
            *read = rd;
        }
    }
    else if (r == FM_ERR_EOF)
    {
        /* fewer bytes than expected: still deliver what we got */
        if (read != NULL)
        {
            *read = rd;
        }
        r = FM_OK;
    }
    (void)File_Close(f);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_WriteAll(const char *path, const void *data, size_t size)
{
    FM_File *f;
    FM_Result r;
    uint32_t wr = 0;
    size_t off = 0;

    if (data == NULL && size > 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    f = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    while (off < size)
    {
        size_t chunk = size - off;
        uint32_t c;

        if (chunk > 0xFFFFu)
        {
            chunk = 0xFFFFu;
        }
        r = File_Write(f, (const uint8_t *)data + off, (uint32_t)chunk, &c);
        if (r != FM_OK)
        {
            (void)File_Close(f);
            FM_Core_SetLastError(r);
            return r;
        }
        if (c == 0)
        {
            (void)File_Close(f);
            FM_Core_SetLastError(FM_ERR_IO);
            return FM_ERR_IO;
        }
        off += c;
        wr += c;
    }
    (void)wr;
    r = File_Close(f);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_AppendAll(const char *path, const void *data, size_t size)
{
    FM_File *f;
    FM_Result r;

    if (data == NULL && size > 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    f = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_APPEND);
    if (f == NULL)
    {
        return File_GetLastError();
    }
    {
        size_t off = 0;

        while (off < size)
        {
            size_t chunk = size - off;
            uint32_t c;

            if (chunk > 0xFFFFu)
            {
                chunk = 0xFFFFu;
            }
            r = File_Write(f, (const uint8_t *)data + off, (uint32_t)chunk,
                           &c);
            if (r != FM_OK)
            {
                (void)File_Close(f);
                FM_Core_SetLastError(r);
                return r;
            }
            if (c == 0)
            {
                (void)File_Close(f);
                FM_Core_SetLastError(FM_ERR_IO);
                return FM_ERR_IO;
            }
            off += c;
        }
    }
    r = File_Close(f);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_ReadText(const char *path, char *buffer, size_t buffer_size,
                        size_t *read)
{
    FM_Result r;
    size_t rd = 0;

    if (buffer == NULL || buffer_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = File_ReadAll(path, buffer,
                     (buffer_size > 0) ? buffer_size - 1 : 0, &rd);
    if (r != FM_OK)
    {
        return r;
    }
    buffer[rd] = '\0';
    if (read != NULL)
    {
        *read = rd;
    }
    return FM_OK;
}

FM_Result File_WriteText(const char *path, const char *text)
{
    if (text == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return File_WriteAll(path, text, strlen(text));
}

FM_Result File_AppendText(const char *path, const char *text)
{
    if (text == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return File_AppendAll(path, text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* Copy / move with progress                                           */
/* ------------------------------------------------------------------ */
FM_Result FM_File_CopyStream(FM_File *src, FM_File *dst,
                             FM_ProgressCallback progress, void *user_data)
{
    uint8_t *buf;
    size_t buf_size;
    uint64_t total = 0;
    uint64_t done = 0;
    FM_Result r;

    buf = FM_InternalBuffer();
    buf_size = FM_InternalBufferSize();
    if (buf == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }

    r = File_Size(src, &total);
    if (r != FM_OK)
    {
        return r;
    }

    for (;;)
    {
        uint32_t rd = 0;
        uint32_t wr = 0;
        size_t chunk = buf_size;

        r = File_Read(src, buf, (uint32_t)chunk, &rd);
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            break;
        }
        if (r != FM_OK)
        {
            return r;
        }
        if (rd == 0)
        {
            break;
        }
        {
            uint32_t off = 0;

            while (off < rd)
            {
                uint32_t remaining = rd - off;

                r = File_Write(dst, buf + off, remaining, &wr);
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
        }
        done += rd;
        if (progress != NULL)
        {
            progress(done, total, user_data);
        }
    }
    if (progress != NULL)
    {
        progress(done, total, user_data);
    }
    return FM_OK;
}

FM_Result File_Copy(const char *src_path, const char *dst_path,
                    FM_ProgressCallback progress, void *user_data)
{
    FM_File *src = NULL;

    if (src_path == NULL || dst_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(src_path, dst_path) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_MountEntry *src_mount = NULL;
        FM_MountEntry *dst_mount = NULL;
        char src_rel[FM_PATH_MAX];
        char dst_rel[FM_PATH_MAX];

        if (FM_VFS_Resolve(src_path, &src_mount, src_rel, sizeof(src_rel)) == FM_OK &&
            FM_VFS_Resolve(dst_path, &dst_mount, dst_rel, sizeof(dst_rel)) == FM_OK &&
            src_mount == dst_mount && strcmp(src_rel, dst_rel) == 0)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
    }
    FM_File *dst = NULL;
    FM_Result r;

    src = File_Open(src_path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(dst_path, FM_OPEN_WRITE | FM_OPEN_CREATE |
                             FM_OPEN_TRUNCATE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        return r;
    }

    r = FM_File_CopyStream(src, dst, progress, user_data);
    if (r != FM_OK)
    {
        (void)File_Close(dst);
        (void)File_Close(src);
        FM_Core_SetLastError(r);
        return r;
    }
    r = File_Close(dst);
    if (r != FM_OK)
    {
        (void)File_Close(src);
        FM_Core_SetLastError(r);
        return r;
    }
    r = File_Close(src);
    FM_Core_SetLastError(r);
    return r;
}

FM_Result File_Move(const char *src_path, const char *dst_path,
                    FM_ProgressCallback progress, void *user_data)
{
    FM_MountEntry *m1 = NULL;
    FM_MountEntry *m2 = NULL;
    char r1[FM_PATH_MAX];
    char r2[FM_PATH_MAX];
    FM_Result r;

    /* Same-mount move is a rename (atomic and fast). */
    if (FM_VFS_Resolve(src_path, &m1, r1, sizeof(r1)) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (FM_VFS_Resolve(dst_path, &m2, r2, sizeof(r2)) == FM_OK && m1 == m2 &&
        m1->fs->ops.rename != NULL)
    {
        r = m1->fs->ops.rename(m1->fs, r1, r2);
        FM_Core_SetLastError(r);
        return r;
    }

    /* Cross-mount: copy then delete.  If the source delete fails after a
     * successful copy, remove the destination copy so the operation stays
     * atomic from the caller's perspective. */
    r = File_Copy(src_path, dst_path, progress, user_data);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Delete(src_path);
    if (r != FM_OK)
    {
        (void)File_Delete(dst_path); /* rollback: undo the copy */
    }
    FM_Core_SetLastError(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* File editing (streaming via temp file)                              */
/* ------------------------------------------------------------------ */
static FM_Result file_edit(const char *path, uint64_t ins_offset,
                           uint64_t del_len, const void *ins_data,
                           size_t ins_size)
{
    char tmp[FM_PATH_MAX];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    uint8_t *buf;
    size_t buf_size;
    uint64_t size = 0;
    uint64_t pos = 0;
    FM_Result r;

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    buf = FM_InternalBuffer();
    buf_size = FM_InternalBufferSize();
    if (buf == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }

    (void)strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    (void)strncat(tmp, ".fm_tmp", sizeof(tmp) - strlen(tmp) - 1);

    src = File_Open(path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        return r;
    }

    r = File_Size(src, &size);
    if (r != FM_OK)
    {
        goto fail;
    }
    if (ins_offset > size)
    {
        ins_offset = size;
    }
    if (del_len > size - ins_offset)
    {
        del_len = size - ins_offset;
    }

    /* copy head [0, ins_offset) */
    if (ins_offset > 0)
    {
        uint64_t remaining = ins_offset;

        if (File_Seek(src, 0, FM_SEEK_SET) != FM_OK)
        {
            r = FM_ERR_IO;
            goto fail;
        }
        while (remaining > 0)
        {
            size_t chunk = (remaining < buf_size) ? (size_t)remaining
                                                  : buf_size;
            uint32_t rd = 0;
            uint32_t wr = 0;

            r = File_Read(src, buf, (uint32_t)chunk, &rd);
            if (r != FM_OK || rd == 0)
            {
                r = FM_ERR_IO;
                goto fail;
            }
            r = File_Write(dst, buf, rd, &wr);
            if (r != FM_OK || wr != rd)
            {
                r = FM_ERR_IO;
                goto fail;
            }
            remaining -= rd;
        }
        pos = ins_offset;
    }

    /* insert new data */
    if (ins_size > 0)
    {
        size_t off = 0;

        while (off < ins_size)
        {
            uint32_t wr = 0;
            size_t chunk = ins_size - off;

            if (chunk > buf_size)
            {
                chunk = buf_size;
            }
            r = File_Write(dst, (const uint8_t *)ins_data + off,
                           (uint32_t)chunk, &wr);
            if (r != FM_OK || wr != chunk)
            {
                r = FM_ERR_IO;
                goto fail;
            }
            off += chunk;
        }
    }

    /* skip deleted region and copy the tail */
    {
        uint64_t tail_start = ins_offset + del_len;
        uint64_t remaining = size - tail_start;

        if (remaining > 0)
        {
            if (File_Seek(src, (int64_t)tail_start, FM_SEEK_SET) != FM_OK)
            {
                r = FM_ERR_IO;
                goto fail;
            }
            while (remaining > 0)
            {
                size_t chunk = (remaining < buf_size) ? (size_t)remaining
                                                      : buf_size;
                uint32_t rd = 0;
                uint32_t wr = 0;

                r = File_Read(src, buf, (uint32_t)chunk, &rd);
                if (r != FM_OK || rd == 0)
                {
                    r = FM_ERR_IO;
                    goto fail;
                }
                r = File_Write(dst, buf, rd, &wr);
                if (r != FM_OK || wr != rd)
                {
                    r = FM_ERR_IO;
                    goto fail;
                }
                remaining -= rd;
            }
        }
        (void)pos;
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

    /* Replace original with temp (safe rename). */
    r = File_Rename(tmp, path);
    if (r != FM_OK)
    {
        (void)File_Delete(tmp);
    }
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

FM_Result File_Insert(const char *path, uint64_t offset, const void *data,
                      size_t size)
{
    if (data == NULL && size > 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return file_edit(path, offset, 0, data, size);
}

FM_Result File_DeleteRange(const char *path, uint64_t offset, uint64_t len)
{
    return file_edit(path, offset, len, NULL, 0);
}

FM_Result File_ReplaceRange(const char *path, uint64_t offset, uint64_t len,
                            const void *data, size_t size)
{
    if (data == NULL && size > 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return file_edit(path, offset, len, data, size);
}
