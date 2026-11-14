/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_file.c - Unified File API (Simple + Advanced)
 */

#define FM_DEBUG_TAG "file"
#include "fm_debug.h"
#include "fm_log.h"          /* FM_Log is called even at FM_DEBUG_LEVEL 0 */
#include "fm_internal.h"
#include "fm_cache.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Backend calls on an open handle                                     */
/*                                                                     */
/* Every call into the filesystem adapter is made with the mount's     */
/* backend lock held (FM_Mount_Lock): FatFs, LittleFS and FMFS keep    */
/* per-volume state that two tasks must not enter at once.  The lock   */
/* is held for exactly one backend call, so it is a leaf below the     */
/* read-cache and bounce-buffer locks and is never held across a user  */
/* callback.  An open handle keeps its mount alive (FS_Unmount returns */
/* FM_ERR_BUSY), so f->mount / f->fs are stable here.                  */
/* ------------------------------------------------------------------ */
static FM_Result fop_read(FM_File *f, void *buf, uint32_t len, uint32_t *got)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_read(f->fs, f->fh, buf, len, got);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_write(FM_File *f, const void *buf, uint32_t len,
                           uint32_t *put)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_write(f->fs, f->fh, buf, len, put);
    FM_Mount_Unlock(f->mount);
    return r;
}

/* Append: ask the backend where the file ends, go there and write, all
 * under one hold of the mount lock.  The handle's own f->size is only a
 * snapshot -- another handle (a second task, the logger, File_AppendAll)
 * may have grown the file since, and writing at the remembered offset
 * would overwrite that data and still report FM_OK.  *end receives the
 * offset the bytes were written at. */
static FM_Result fop_write_end(FM_File *f, const void *buf, uint32_t len,
                               uint32_t *put, uint64_t *end)
{
    FM_Result r;
    uint64_t size = f->size;

    FM_Mount_Lock(f->mount);
    r = FM_OK;
    if (f->fs->ops.file_size != NULL)
    {
        r = f->fs->ops.file_size(f->fs, f->fh, &size);
    }
    if (r == FM_OK && f->fs->ops.file_seek != NULL)
    {
        r = f->fs->ops.file_seek(f->fs, f->fh, (int64_t)size, FM_SEEK_SET);
    }
    if (r == FM_OK)
    {
        r = f->fs->ops.file_write(f->fs, f->fh, buf, len, put);
    }
    FM_Mount_Unlock(f->mount);
    *end = size;
    return r;
}

static FM_Result fop_seek(FM_File *f, int64_t offset, FM_SeekOrigin origin)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_seek(f->fs, f->fh, offset, origin);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_tell(FM_File *f, uint64_t *pos)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_tell(f->fs, f->fh, pos);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_size(FM_File *f, uint64_t *size)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_size(f->fs, f->fh, size);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_truncate(FM_File *f, uint64_t size)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_truncate(f->fs, f->fh, size);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_flush(FM_File *f)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_flush(f->fs, f->fh);
    FM_Mount_Unlock(f->mount);
    return r;
}

static FM_Result fop_close(FM_File *f)
{
    FM_Result r;

    FM_Mount_Lock(f->mount);
    r = f->fs->ops.file_close(f->fs, f->fh);
    FM_Mount_Unlock(f->mount);
    return r;
}

/* ------------------------------------------------------------------ */
/* DMA bounce buffer                                                   */
/*                                                                     */
/* On a target, a caller buffer that the SD/DMA path cannot use        */
/* directly (unaligned, on the stack, ...) is copied through one       */
/* cache-line-aligned 512-byte block.  That block is shared by every   */
/* open file, so in a threaded build each chunk is moved under its own */
/* mutex -- without it two tasks reading different files swap sectors. */
/*                                                                     */
/* Host builds talk to the adapter directly; -DFM_FILE_FORCE_BOUNCE    */
/* routes them through the bounce path as well so it can be tested.    */
/* ------------------------------------------------------------------ */
#if !(defined(FM_HOST_BUILD) && FM_HOST_BUILD) || defined(FM_FILE_FORCE_BOUNCE)
#define FM_FILE_BOUNCE 1
#else
#define FM_FILE_BOUNCE 0
#endif

#if FM_FILE_BOUNCE
static uint8_t g_file_bounce[512] FM_DMA_ALIGNED;

#ifdef FM_FILE_FORCE_BOUNCE
#define file_direct_ok(buf, len) ((void)(buf), (void)(len), 0)
#else
#define file_direct_ok(buf, len) FM_Cache_SdDirectOk((buf), (len))
#endif

#if FM_USE_THREADS
static FM_Mutex g_bounce_lock;
static FM_Bool  g_bounce_lock_init = FM_FALSE;

static void bounce_lock(void)
{
    FM_Bool ready;

    FM_Lock();
    if (!g_bounce_lock_init && FM_Mutex_Create(&g_bounce_lock) == FM_OK)
    {
        g_bounce_lock_init = FM_TRUE;
    }
    ready = g_bounce_lock_init;
    FM_Unlock();
    if (ready)
    {
        FM_Mutex_Lock(&g_bounce_lock);
    }
}

static void bounce_unlock(void)
{
    if (g_bounce_lock_init)
    {
        FM_Mutex_Unlock(&g_bounce_lock);
    }
}
#else
#define bounce_lock()   ((void)0)
#define bounce_unlock() ((void)0)
#endif

static FM_Result file_read_backend(FM_File *f, void *buf, uint32_t len,
                                   uint32_t *got)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t total = 0;
    FM_Result r = FM_OK;

    if (file_direct_ok(buf, len))
    {
        return fop_read(f, buf, len, got);
    }
    while (total < len)
    {
        uint32_t chunk = len - total;
        uint32_t rd = 0;

        if (chunk > sizeof(g_file_bounce))
        {
            chunk = (uint32_t)sizeof(g_file_bounce);
        }
        bounce_lock();
        r = fop_read(f, g_file_bounce, chunk, &rd);
        if (rd > 0u)
        {
            memcpy(out + total, g_file_bounce, rd);
            total += rd;
        }
        bounce_unlock();
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
    /* F-14: only EOF may be folded into a short read.  A hard error
     * (IO / timeout / corrupted) is reported even when some bytes were
     * already delivered; *got still tells the caller how many. */
    return r;
}

/* at_end != NULL: every chunk is appended (see fop_write_end) and
 * *at_end receives the offset just behind the last byte written. */
static FM_Result file_write_backend(FM_File *f, const void *buf, uint32_t len,
                                    uint32_t *put, uint64_t *at_end)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t total = 0;
    FM_Result r = FM_OK;

    if (file_direct_ok(buf, len))
    {
        if (at_end != NULL)
        {
            r = fop_write_end(f, buf, len, put, at_end);
            *at_end += *put;
            return r;
        }
        return fop_write(f, buf, len, put);
    }
    while (total < len)
    {
        uint32_t chunk = len - total;
        uint32_t wr = 0;

        if (chunk > sizeof(g_file_bounce))
        {
            chunk = (uint32_t)sizeof(g_file_bounce);
        }
        bounce_lock();
        memcpy(g_file_bounce, in + total, chunk);
        if (at_end != NULL)
        {
            r = fop_write_end(f, g_file_bounce, chunk, &wr, at_end);
            *at_end += wr;
        }
        else
        {
            r = fop_write(f, g_file_bounce, chunk, &wr);
        }
        bounce_unlock();
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
#define file_read_backend(f, buf, len, got)  fop_read((f), (buf), (len), (got))
static FM_Result file_write_backend(FM_File *f, const void *buf, uint32_t len,
                                    uint32_t *put, uint64_t *at_end)
{
    FM_Result r;

    if (at_end != NULL)
    {
        r = fop_write_end(f, buf, len, put, at_end);
        *at_end += *put;
        return r;
    }
    return fop_write(f, buf, len, put);
}
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
    /* Created by FileManager_Init() (FM_File_SyncInit).  The lazy path
     * serves applications that skip it; creating under the VFS lock
     * keeps two first-time callers from each making a mutex. */
    if (!g_cache_lock_init)
    {
        FM_Lock();
        if (!g_cache_lock_init && FM_Mutex_Create(&g_cache_lock) == FM_OK)
        {
            g_cache_lock_init = FM_TRUE;
        }
        FM_Unlock();
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

/* F-13: a cache line belongs to one handle, but the bytes belong to the
 * file.  A write / truncate / delete / rename must therefore drop the
 * lines of EVERY handle that is open on the same file, otherwise a second
 * handle keeps serving the old content.
 *
 * Identity is (mount, normalized path).  The comparison ignores ASCII
 * case: FAT is case-insensitive, and on a case-sensitive filesystem the
 * only cost of a false match is one extra cache miss. */
static FM_Bool cache_name_equal(const char *a, const char *b)
{
    for (;;)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= (unsigned char)'A' && ca <= (unsigned char)'Z')
        {
            ca = (unsigned char)(ca + ((unsigned char)'a' - (unsigned char)'A'));
        }
        if (cb >= (unsigned char)'A' && cb <= (unsigned char)'Z')
        {
            cb = (unsigned char)(cb + ((unsigned char)'a' - (unsigned char)'A'));
        }
        if (ca != cb)
        {
            return FM_FALSE;
        }
        if (ca == 0u)
        {
            return FM_TRUE;
        }
    }
}

/* Relative path of an open handle inside its mount -- the same slice of
 * the normalized path that FM_VFS_Resolve() hands to the backend. */
static const char *cache_file_rel(const FM_File *f)
{
    const FM_MountEntry *m = (const FM_MountEntry *)f->mount;
    size_t mlen = (m != NULL) ? strlen(m->mount_path) : 1u;
    size_t plen = strlen(f->path);

    if (mlen <= 1u)
    {
        return (plen > 0u) ? f->path + 1 : f->path;
    }
    if (plen <= mlen)
    {
        return f->path + plen;          /* "" */
    }
    return f->path + mlen + 1u;
}

/* Drop every line that caches (mount, rel).  Cache lock must be held. */
static void cache_invalidate_path(const void *mount, const char *rel)
{
    uint32_t i;

    for (i = 0; i < FM_CACHE_LINES; i++)
    {
        const FM_File *owner = g_cache[i].file;

        if (owner != NULL && owner->mount == mount &&
            cache_name_equal(cache_file_rel(owner), rel))
        {
            g_cache[i].valid = 0;
            g_cache[i].file = NULL;
        }
    }
}

/* Drop the lines of every handle open on the same file as `f`
 * (including f itself).  Cache lock must be held. */
static void cache_invalidate_same_file(const FM_File *f)
{
    uint32_t i;

    for (i = 0; i < FM_CACHE_LINES; i++)
    {
        const FM_File *owner = g_cache[i].file;

        if (owner == NULL)
        {
            continue;
        }
        if (owner == f ||
            (owner->mount == f->mount &&
             cache_name_equal(owner->path, f->path)))
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
        r = fop_seek(f, (int64_t)block_off,
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

        r = fop_read(f, line->data + got,
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
    if (got == 0 || r != FM_OK)
    {
        /* EOF: leave the line invalid so an empty block never evicts a
         * useful one and never matches a later read.  Same for a hard
         * read error (F-14): a half-filled block must not be served as
         * if it were the file's content. */
        line->valid = 0;
        line->file = NULL;
        line->len = 0;
    }
    else
    {
        line->len = got;
        line->valid = 1;
    }

    /* restore the logical file position */
    if (f->fs->ops.file_seek != NULL)
    {
        (void)fop_seek(f, (int64_t)save, FM_SEEK_SET);
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

void FM_File_SyncInit(void)
{
#if FM_USE_THREADS
    FM_Lock();
#if FM_USE_CACHE
    if (!g_cache_lock_init && FM_Mutex_Create(&g_cache_lock) == FM_OK)
    {
        g_cache_lock_init = FM_TRUE;
    }
#endif
#if FM_FILE_BOUNCE
    if (!g_bounce_lock_init && FM_Mutex_Create(&g_bounce_lock) == FM_OK)
    {
        g_bounce_lock_init = FM_TRUE;
    }
#endif
    FM_Unlock();
#endif
}

void FM_File_SyncDeinit(void)
{
#if FM_USE_THREADS
#if FM_USE_CACHE
    if (g_cache_lock_init)
    {
        FM_Mutex_Destroy(&g_cache_lock);
        g_cache_lock_init = FM_FALSE;
    }
#endif
#if FM_FILE_BOUNCE
    if (g_bounce_lock_init)
    {
        FM_Mutex_Destroy(&g_bounce_lock);
        g_bounce_lock_init = FM_FALSE;
    }
#endif
#endif
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

/* One handle against a path of its mount.  A handle keeps the whole
 * normalised path; `rel` is the part after the mount point, without a
 * leading '/'.  With `tree`, a handle below the directory `rel` matches
 * too.  FAT does not tell upper from lower case, so there (and only
 * there) the comparison does not either. */
static FM_Bool file_handle_on_path(const FM_File *f,
                                   const FM_MountEntry *mount,
                                   const char *rel, FM_Bool tree,
                                   FM_Bool nocase)
{
    const char *h;
    size_t i;

    if (!f->used || f->mount != (const void *)mount)
    {
        return FM_FALSE;
    }
    h = f->resolved;
    i = strlen(mount->mount_path);
    if (strlen(h) < i)
    {
        return FM_FALSE;
    }
    h += i;
    while (*h == '/')
    {
        h++;
    }
    for (i = 0u; rel[i] != '\0'; i++)
    {
        char a = h[i];
        char b = rel[i];

        if (nocase)
        {
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
            return FM_FALSE;
        }
    }
    if (h[i] == '\0')
    {
        return FM_TRUE;
    }
    return (tree && h[i] == '/') ? FM_TRUE : FM_FALSE;
}

/* FM_TRUE while a handle is open on `rel` of this mount (with `tree`:
 * on it or below it).  Deleting or renaming such a path is refused: the
 * backends disagree on what the handle does afterwards, and one of them
 * went on accepting writes for a file that no longer existed.
 *
 * Call it with the mount's backend lock held.  File_Open publishes its
 * handle (under FM_Lock) before it asks the backend, and it asks the
 * backend under that same backend lock, so an open that has not been
 * seen here cannot reach the file before the caller is done with it. */
/* FM_TRUE when names on this mount compare without case (FAT). */
FM_Bool FM_Mount_NoCase(const FM_MountEntry *mount)
{
    return (mount != NULL && mount->fs != NULL &&
            strcmp(mount->fs->name, "FATFS") == 0) ? FM_TRUE : FM_FALSE;
}

FM_Bool FM_File_OpenOnPath(const FM_MountEntry *mount, const char *rel,
                           FM_Bool tree)
{
    FM_Bool nocase = FM_Mount_NoCase(mount);
    FM_Bool found = FM_FALSE;
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_OPEN_FILES && !found; i++)
    {
        if (file_handle_on_path(&g_pool[i], mount, rel, tree, nocase))
        {
            found = FM_TRUE;
        }
        else if (g_external_files[i] != NULL &&
                 file_handle_on_path(g_external_files[i], mount, rel, tree,
                                     nocase))
        {
            found = FM_TRUE;
        }
    }
    FM_Unlock();
    return found;
}

void File_PoolUsedSplit(uint32_t *internal, uint32_t *external)
{
    uint32_t i;
    uint32_t used = 0u;
    uint32_t ext = 0u;

    FM_Lock();
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
    FM_Unlock();
    if (internal != NULL)
    {
        *internal = used;
    }
    if (external != NULL)
    {
        *external = ext;
    }
}

uint32_t File_PoolUsed(void)
{
    uint32_t used = 0u;
    uint32_t ext = 0u;

    File_PoolUsedSplit(&used, &ext);
    return used + ext;
}

/* F-03: the pool (File_Open) and the external registry (File_OpenEx) are
 * two independent resources of FM_MAX_OPEN_FILES entries each.  Print
 * them separately -- telling them apart is exactly the evidence needed
 * to find which kind of handle is leaking. */
void File_PoolDump(void)
{
    uint32_t i;
    uint32_t used = 0u;
    uint32_t ext = 0u;

    File_PoolUsedSplit(&used, &ext);
    FM_Log("file pool: %lu internal / %lu external (slots %u)\n",
           (unsigned long)used, (unsigned long)ext,
           (unsigned)FM_MAX_OPEN_FILES);
    for (i = 0; i < FM_MAX_OPEN_FILES; i++)
    {
        char path[FM_PATH_MAX];
        FM_Bool have;

        /* Copy the name out under the VFS lock and print without it: a
         * log sink is user code and must not run under a table lock. */
        FM_Lock();
        have = g_pool[i].used ? FM_TRUE : FM_FALSE;
        if (have)
        {
            (void)strncpy(path, g_pool[i].path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
        FM_Unlock();
        if (have)
        {
            FM_Log("  [i%lu] %s\n", (unsigned long)i, path);
        }

        FM_Lock();
        have = (g_external_files[i] != NULL && g_external_files[i]->used)
                   ? FM_TRUE : FM_FALSE;
        if (have)
        {
            (void)strncpy(path, g_external_files[i]->path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
        FM_Unlock();
        if (have)
        {
            FM_Log("  [x%lu] %s\n", (unsigned long)i, path);
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

    /* The caller has prepared f and marked it busy under FM_Lock (pool
     * slots by file_acquire, user handles by File_OpenEx), so the slot
     * cannot be grabbed by another thread while the backend opens the
     * file.  f->used is not written here: an unlocked store would race
     * with file_acquire() scanning the pool.
     *
     * The mount is pinned from the lookup until the handle itself keeps
     * it alive, so FS_Unmount cannot pull the filesystem away between
     * "path resolved" and "file open". */
    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        file_release(f);
        return r;
    }
    if (rel[0] == '\0')
    {
        /* Opening the mount root as a file is never meaningful and the
         * adapters disagree on it (POSIX would happily open "."). */
        FM_VFS_Unpin(mount);
        file_release(f);
        return FM_ERR_IS_DIRECTORY;
    }
    if ((mode & FM_OPEN_CREATE) != 0u && FM_Path_NameTooLong(rel, FM_FALSE))
    {
        /* a file this call would create under a name no listing can
         * return (see FM_Path_NameTooLong) */
        FM_VFS_Unpin(mount);
        file_release(f);
        return FM_ERR_TOO_LARGE;
    }
    fs = mount->fs;
    if ((mode & FM_OPEN_EXCLUSIVE) &&
        (((mode & (FM_OPEN_CREATE | FM_OPEN_WRITE)) != (FM_OPEN_CREATE | FM_OPEN_WRITE)) ||
         !(fs->capabilities & FM_FS_CAP_EXCLUSIVE_CREATE)))
    {
        FM_VFS_Unpin(mount);
        file_release(f);
        return FM_ERR_UNSUPPORTED;
    }

    /* Normalize directly into f->path instead of an extra stack buffer:
     * this function sits at the deepest point of the File_* call chain
     * (FM_VFS_Resolve's normalized[] + this rel[] already cost
     * 2 * FM_PATH_MAX of stack; backends add their own path buffers on
     * top, e.g. FatFS builds a drive-qualified copy). */
    (void)strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';
    if (Path_Normalize(f->path, sizeof(f->path)) != FM_OK)
    {
        FM_VFS_Unpin(mount);
        file_release(f);
        return FM_ERR_INVALID_PATH;
    }
    (void)strcpy(f->resolved, f->path);

    /* Publish the mount on the handle before the backend call: from here
     * on FM_File_AnyOpenOnMount() sees it (it scans under FM_Lock). */
    FM_Lock();
    f->mount = (void *)mount;
    f->fs = fs;
    FM_Unlock();

    FM_FS_CALL(mount, r, file_open(fs, f->fh, rel, mode));
    if (r != FM_OK)
    {
        file_release(f);
        FM_VFS_Unpin(mount);
        return r;
    }
    FM_VFS_Unpin(mount);

    f->mode = (uint8_t)mode;
#if FM_USE_CACHE
    if (mode & FM_OPEN_TRUNCATE)
    {
        /* the content other handles cached is gone (F-13) */
        cache_lock();
        cache_invalidate_same_file(f);
        cache_unlock();
    }
#endif

    /* refresh cached size / position */
    if (fs->ops.file_size != NULL)
    {
        (void)fop_size(f, &f->size);
    }
    if (fs->ops.file_tell != NULL)
    {
        (void)fop_tell(f, &f->pos);
    }

    if (mode & FM_OPEN_APPEND)
    {
        if (fs->ops.file_seek != NULL)
        {
            (void)fop_seek(f, (int64_t)f->size,
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
    if ((mode & FM_OPEN_TRUNCATE) && !(mode & FM_OPEN_WRITE))
    {
        FM_Core_SetLastError(FM_ERR_ACCESS);
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
        /* file_open_internal already released the slot.  "Open it if it
         * is there" is normal use, so this is an event, not an error. */
        FM_EVENT_LOG("open %s failed (%d)", path, (int)r);
        FM_Core_SetLastError(r);
        return NULL;
    }
    FM_EVENT_LOG("opened %s (mode 0x%lx)", path, (unsigned long)mode);
    FM_Core_SetLastError(FM_OK);
    return f;
}

FM_Result File_OpenEx(FM_File *f, const char *path, uint32_t mode)
{
    FM_Result r;

    FM_CHECK_PTR(f, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    if ((mode & FM_OPEN_TRUNCATE) && !(mode & FM_OPEN_WRITE))
    {
        FM_Core_SetLastError(FM_ERR_ACCESS);
        return FM_ERR_ACCESS;
    }
    {
        uint32_t i;
        FM_Bool registered = FM_FALSE;

        /* Register BEFORE the backend open.  The registry is what
         * FS_Unmount consults; a handle that is entered only after its
         * file is open leaves a window in which the filesystem can be
         * unmounted underneath it.
         *
         * "Is it in use" and "it is mine now" are one step under the
         * lock.  The test and the memset used to come before it: two
         * tasks opening the same FM_File both passed the test, and the
         * second memset wiped a handle the first had registered. */
        FM_Lock();
        if (f->used)
        {
            FM_Unlock();
            FM_ERR_LOG("File_OpenEx: handle already in use");
            return FM_ERR_BUSY;
        }
        memset(f, 0, sizeof(*f));
        for (i = 0; i < FM_MAX_OPEN_FILES; i++)
        {
            if (g_external_files[i] == NULL)
            {
                g_external_files[i] = f;
                f->used = 1;
                registered = FM_TRUE;
                break;
            }
        }
        FM_Unlock();
        if (!registered)
        {
            FM_Core_SetLastError(FM_ERR_NO_MEMORY);
            return FM_ERR_NO_MEMORY;
        }
    }
    r = file_open_internal(f, path, mode);
    if (r == FM_OK)
    {
        FM_EVENT_LOG("opened %s (mode 0x%lx)", path, (unsigned long)mode);
    }
    else
    {
        FM_EVENT_LOG("open %s failed (%d)", path, (int)r);
        external_file_remove(f);
        file_release(f);
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
        r = fop_flush(f);
    }
    if (f->fs != NULL && f->fs->ops.file_close != NULL)
    {
        FM_Result close_result = fop_close(f);

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
    /* Before the handle (and the path in it) is given back. */
    if (r != FM_OK)
    {
        /* The one place a lost write shows up when the caller ignores
         * the result of File_Close, which most callers do. */
        FM_ERR_LOG("close %s failed (%d): data may not be on the medium",
                   f->path, (int)r);
    }
    else
    {
        FM_EVENT_LOG("closed %s", f->path);
    }
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

                if (f->size != 0 && block_off >= f->size &&
                    f->fs->ops.file_size != NULL)
                {
                    /* f->size is this handle's snapshot; another handle
                     * may have appended since (F-13).  Ask the backend
                     * before declaring EOF. */
                    (void)fop_size(f, &f->size);
                }
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
            (void)fop_seek(f, (int64_t)f->pos,
                                       FM_SEEK_SET);
        }
        /* F-14: a short read is a success only at end of file.  A hard
         * error (IO / timeout / corrupted) is returned even when some
         * bytes were already served; *read reports how many. */
        if (r == FM_ERR_EOF && total > 0)
        {
            r = FM_OK;
        }
        return r;
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

/* at_end: write behind the last byte the file has now, wherever the
 * handle's position is.  That is File_Append, and every File_Write on a
 * handle opened with FM_OPEN_APPEND (what O_APPEND and LFS_O_APPEND do
 * on their own; FMFS and FatFs only positioned the handle at open, so
 * the same program wrote to different places on different backends). */
static FM_Result file_write_common(FM_File *f, const void *buf, uint32_t len,
                                   uint32_t *written, FM_Bool at_end)
{
    FM_Result r;
    uint32_t local_written = 0;
    uint32_t *written_ptr = (written != NULL) ? written : &local_written;
    uint64_t end = 0;

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
    *written_ptr = 0;
#if FM_USE_CACHE
    cache_lock();
    cache_invalidate_same_file(f);
    cache_unlock();
#endif
    if (at_end)
    {
        r = file_write_backend(f, buf, len, written_ptr, &end);
        if (r == FM_OK || *written_ptr != 0u)
        {
            f->pos = end;
            if (f->pos > f->size)
            {
                f->size = f->pos;
            }
        }
        return r;
    }
    r = file_write_backend(f, buf, len, written_ptr, NULL);
    if (r == FM_OK)
    {
        f->pos += *written_ptr;
        if (f->pos > f->size)
        {
            f->size = f->pos;
        }
    }
    return r;
}

FM_Result File_Write(FM_File *f, const void *buf, uint32_t len,
                     uint32_t *written)
{
    FM_Bool at_end = FM_FALSE;

    if (f != NULL && f->used && (f->mode & FM_OPEN_APPEND))
    {
        at_end = FM_TRUE;
    }
    return file_write_common(f, buf, len, written, at_end);
}

FM_Result File_Append(FM_File *f, const void *buf, uint32_t len,
                      uint32_t *written)
{
    return file_write_common(f, buf, len, written, FM_TRUE);
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
    r = fop_seek(f, offset, origin);
    if (r == FM_OK && f->fs->ops.file_tell != NULL)
    {
        (void)fop_tell(f, &f->pos);
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
        FM_Result r = fop_tell(f, pos);

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
        FM_Result r = fop_size(f, size);

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
    /* Truncating is a write.  Without this check the adapters disagreed
     * on a read-only handle: FMFS FM_ERR_ACCESS, POSIX FM_ERR_IO, and
     * littlefs an LFS_ASSERT -- that is abort(), a reset on a target. */
    if (!(f->mode & FM_OPEN_WRITE))
    {
        return FM_ERR_ACCESS;
    }
    if (f->fs->ops.file_truncate == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (f->pos > size && (f->fs->ops.file_seek == NULL || size > INT64_MAX))
        return FM_ERR_UNSUPPORTED;
    r = fop_truncate(f, size);
    if (r == FM_OK)
    {
#if FM_USE_CACHE
        cache_lock();
        cache_invalidate_same_file(f);
        cache_unlock();
#endif
        f->size = size;
        if (f->pos > size)
        {
            /* Truncate may already be committed if this seek fails. */
            r = fop_seek(f, (int64_t)size, FM_SEEK_SET);
            if (r == FM_OK) f->pos = size;
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
        return fop_flush(f);
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

        (void)fop_size(f, &size);
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
    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        FM_ERR_LOG("delete %s: resolve failed (%d)", path, (int)r);
        FM_Core_SetLastError(r);
        return r;
    }
    if (rel[0] == '\0')
    {
        FM_VFS_Unpin(mount);
        FM_ERR_LOG("delete: cannot delete mount root");
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (mount->fs->ops.delete_file == NULL)
    {
        FM_VFS_Unpin(mount);
        return FM_ERR_UNSUPPORTED;
    }
#if FM_USE_CACHE
    cache_lock();
    cache_invalidate_path(mount, rel);
    cache_unlock();
#endif
    /* One answer on every backend (they used to give three):
     *   - a file that is open is not deleted: FM_ERR_BUSY;
     *   - a directory is not a file: FM_ERR_IS_DIRECTORY, also when it
     *     is empty (two adapters removed it and answered FM_OK). */
    FM_Mount_Lock(mount);
    if (FM_File_OpenOnPath(mount, rel, FM_FALSE))
    {
        r = FM_ERR_BUSY;
    }
    else
    {
        FM_FileInfo info;

        r = FM_OK;
        if (mount->fs->ops.stat != NULL &&
            mount->fs->ops.stat(mount->fs, rel, &info) == FM_OK &&
            info.type == FM_ITEM_DIR)
        {
            r = FM_ERR_IS_DIRECTORY;
        }
        if (r == FM_OK)
        {
            r = mount->fs->ops.delete_file(mount->fs, rel);
        }
    }
    FM_Mount_Unlock(mount);
    FM_VFS_Unpin(mount);
    if (r == FM_OK)
    {
        FM_EVENT_LOG("deleted %s", path);
    }
    else if (r == FM_ERR_NOT_FOUND)
    {
        /* "Delete it if it is there" is the usual way to call this, so
         * a missing file is an answer, not a fault: an error line for
         * each one buried the real errors at the default level. */
        FM_EVENT_LOG("delete %s: not there", path);
    }
    else
    {
        FM_ERR_LOG("delete %s: backend returned %d", path, (int)r);
    }
    FM_Core_SetLastError(r);
    return r;
}

/* Same-mount rename on already-resolved relative paths.  Shared by
 * File_Rename and File_Move so both apply one policy (F-16):
 * renaming an entry onto itself -- however the two paths were spelled
 * ("/a/b" vs "/a/./b") -- is a no-op that never reaches the backend, so
 * an adapter that implements rename as "delete target, then move" cannot
 * lose the file.  A missing source is still reported.
 * `m` must be pinned by the caller (FM_VFS_Pin). */
/* A directory entry is handed out in FM_DirEntry.name, FM_NAME_MAX
 * bytes.  A longer name cannot be listed: Directory_Read stops there
 * with FM_ERR_TOO_LARGE, and with it every walk -- a folder holding
 * such a name could be filled but no longer copied or deleted.  The
 * backends take longer names (LittleFS and FAT up to 255), so the
 * library has to refuse to MAKE them.  Names that are already on a
 * card (written by a PC) can still be opened and deleted by path; to
 * list them, build with a larger FM_NAME_MAX.
 *
 * `all`: look at every component of `rel`, not only the last. */
FM_Bool FM_Path_NameTooLong(const char *rel, FM_Bool all)
{
    size_t len = 0u;
    size_t i;

    for (i = 0u;; i++)
    {
        if (rel[i] == '/' || rel[i] == '\0')
        {
            if (len >= (size_t)FM_NAME_MAX && (all || rel[i] == '\0'))
            {
                return FM_TRUE;
            }
            if (rel[i] == '\0')
            {
                return FM_FALSE;
            }
            len = 0u;
        }
        else
        {
            len++;
        }
    }
}

FM_Result FM_File_RenameResolved(FM_MountEntry *m, const char *from,
                                 const char *to)
{
    FM_Result r;

    if (m->fs->ops.rename == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (FM_Path_NameTooLong(to, FM_FALSE))
    {
        return FM_ERR_TOO_LARGE;
    }
    if (strcmp(from, to) == 0)
    {
        if (m->fs->ops.stat != NULL)
        {
            FM_FileInfo info;

            FM_FS_CALL(m, r, stat(m->fs, from, &info));
            return r;
        }
        return FM_OK;
    }
#if FM_USE_CACHE
    cache_lock();
    cache_invalidate_path(m, from);
    cache_invalidate_path(m, to);
    cache_unlock();
#endif
    /* Neither the old name (a file, or a directory with an open file
     * below it) nor the name that would be replaced may be open: the
     * handle would go on writing into a file nobody can find, and one
     * backend answered FM_OK for every such write. */
    FM_Mount_Lock(m);
    if (FM_File_OpenOnPath(m, from, FM_TRUE) ||
        FM_File_OpenOnPath(m, to, FM_TRUE))
    {
        r = FM_ERR_BUSY;
    }
    else
    {
        r = m->fs->ops.rename(m->fs, from, to);
    }
    FM_Mount_Unlock(m);
    return r;
}

FM_Result File_Rename(const char *old_path, const char *new_path)
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
    if (m1 != m2)
    {
        FM_VFS_Unpin(m1);
        return FM_ERR_UNSUPPORTED; /* cross-mount rename: use Move */
    }
    r = FM_File_RenameResolved(m1, r1, r2);
    FM_VFS_Unpin(m1);
    FM_Core_SetLastError(r);
    return r;
}

/* Publish a file only if its destination is absent. Backends either use a
 * native exclusive primitive, or serialize lookup+rename under the same
 * mount lock. A custom adapter must explicitly provide this operation. */
FM_Result FM_File_RenameNoReplace(const char *old_path, const char *new_path)
{
    FM_MountEntry *m1 = NULL, *m2 = NULL;
    char r1[FM_PATH_MAX], r2[FM_PATH_MAX];
    FM_Result r = FM_VFS_Pin(old_path, &m1, r1, sizeof(r1));
    if (r != FM_OK) return r;
    r = FM_VFS_Resolve(new_path, &m2, r2, sizeof(r2));
    if (r == FM_OK && m1 != m2) r = FM_ERR_UNSUPPORTED;
    if (r == FM_OK && (r1[0] == '\0' || r2[0] == '\0')) r = FM_ERR_INVALID_ARGUMENT;
    if (r == FM_OK && FM_Path_NameTooLong(r2, FM_FALSE)) r = FM_ERR_TOO_LARGE;
    if (r == FM_OK && m1->fs->ops.rename_noreplace == NULL) r = FM_ERR_UNSUPPORTED;
    if (r == FM_OK)
    {
#if FM_USE_CACHE
        cache_lock();
        cache_invalidate_path(m1, r1);
        cache_invalidate_path(m1, r2);
        cache_unlock();
#endif
        FM_Mount_Lock(m1);
        if (FM_File_OpenOnPath(m1, r1, FM_TRUE) || FM_File_OpenOnPath(m1, r2, FM_TRUE))
            r = FM_ERR_BUSY;
        else
            r = m1->fs->ops.rename_noreplace(m1->fs, r1, r2);
        FM_Mount_Unlock(m1);
    }
    FM_VFS_Unpin(m1);
    FM_Core_SetLastError(r);
    return r;
}

/* Two resolved paths of one mount: the same entry?  "/sd/a.txt" and
 * "/sd/A.TXT" are one file on a FAT volume.  (A long name and its 8.3
 * alias are one file too; that cannot be seen from here, only the
 * volume knows its aliases.) */
static FM_Bool file_rel_same(const FM_MountEntry *m, const char *a,
                             const char *b)
{
    size_t i;

    if (!FM_Mount_NoCase(m))
    {
        return (strcmp(a, b) == 0) ? FM_TRUE : FM_FALSE;
    }
    for (i = 0u;; i++)
    {
        char x = a[i];
        char y = b[i];

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return FM_FALSE;
        if (x == '\0') return FM_TRUE;
    }
}

/* FM_TRUE when both paths name the same entry of the same mount after
 * normalization.  Paths that do not resolve are never "the same": the
 * caller's own open/resolve reports the real error. */
FM_Bool FM_File_SamePath(const char *a, const char *b)
{
    FM_MountEntry *ma = NULL;
    FM_MountEntry *mb = NULL;
    char ra[FM_PATH_MAX];
    char rb[FM_PATH_MAX];

    if (a == NULL || b == NULL)
    {
        return FM_FALSE;
    }
    if (FM_VFS_Resolve(a, &ma, ra, sizeof(ra)) != FM_OK ||
        FM_VFS_Resolve(b, &mb, rb, sizeof(rb)) != FM_OK)
    {
        return FM_FALSE;
    }
    return (ma == mb && file_rel_same(ma, ra, rb)) ? FM_TRUE : FM_FALSE;
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
    r = FM_VFS_Pin(path, &mount, rel, sizeof(rel));
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
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
    size_t buf_size = 0;
    uint64_t total = 0;
    uint64_t done = 0;
    FM_Result r;

    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (FM_InternalBufferSize() == 0u)
    {
        return FM_ERR_NO_MEMORY;
    }
    /* F-11: one scratch buffer serves File_Copy, Directory_Copy and the
     * File_Insert family.  A progress callback that starts another of
     * them would overwrite the chunk in flight, so the nested call is
     * refused instead of silently corrupting the copy. */
    buf = FM_InternalBufferAcquire(&buf_size);
    if (buf == NULL)
    {
        return FM_ERR_BUSY;
    }

    r = File_Size(src, &total);
    while (r == FM_OK)
    {
        uint32_t rd = 0;
        uint32_t off = 0;

        r = File_Read(src, buf, (uint32_t)buf_size, &rd);
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            if (rd == 0)
            {
                break;
            }
        }
        if (r != FM_OK || rd == 0)
        {
            break;
        }
        while (off < rd)
        {
            uint32_t wr = 0;

            r = File_Write(dst, buf + off, rd - off, &wr);
            if (r != FM_OK)
            {
                break;
            }
            if (wr == 0)
            {
                /* accepted nothing without an error: the volume is full */
                r = FM_ERR_NO_SPACE;
                break;
            }
            off += wr;
        }
        if (r != FM_OK)
        {
            break;
        }
        done += rd;
        /* F-05: 100 % is reported only once, below, after the flush. */
        if (progress != NULL && done < total)
        {
            progress(done, total, user_data);
        }
    }
    FM_InternalBufferRelease();

    /* F-05: make the data durable BEFORE telling the caller it is all
     * there.  Reporting done == total on a buffered write is the classic
     * "progress bar completes, file is short after a power cut". */
    if (r == FM_OK)
    {
        r = File_Flush(dst);
    }
    if (r == FM_OK && progress != NULL)
    {
        progress(done, (done > total) ? done : total, user_data);
    }
    return r;
}

#define FM_FILE_TMP_SUFFIX ".fm_tmp"

FM_Result File_Copy(const char *src_path, const char *dst_path,
                    FM_ProgressCallback progress, void *user_data)
{
    FM_MountEntry *src_mount = NULL;
    FM_MountEntry *dst_mount = NULL;
    char tmp[FM_PATH_MAX];
    const char *work_path;
    FM_Bool staged = FM_FALSE;
    FM_Bool dst_can_rename = FM_FALSE;
    FM_File *src = NULL;
    FM_File *dst = NULL;
    FM_Result r;
    FM_Result rc;

    if (src_path == NULL || dst_path == NULL)
    {
        FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        char src_rel[FM_PATH_MAX];
        char dst_rel[FM_PATH_MAX];

        r = FM_VFS_Resolve(src_path, &src_mount, src_rel, sizeof(src_rel));
        if (r == FM_OK)
        {
            /* pinned only long enough to look at the adapter */
            r = FM_VFS_Pin(dst_path, &dst_mount, dst_rel, sizeof(dst_rel));
        }
        if (r != FM_OK)
        {
            FM_Core_SetLastError(r);
            return r;
        }
        dst_can_rename = (dst_mount->fs->ops.rename != NULL) ? FM_TRUE
                                                             : FM_FALSE;
        FM_VFS_Unpin(dst_mount);
        if (src_mount == dst_mount &&
            file_rel_same(src_mount, src_rel, dst_rel))
        {
            FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
            return FM_ERR_INVALID_ARGUMENT;
        }
    }

    /* Require a rename-capable destination and atomically claim the temp.
     * Never fall back to truncating the destination on failure. */
    if (!dst_can_rename)
    {
        FM_Core_SetLastError(FM_ERR_UNSUPPORTED);
        return FM_ERR_UNSUPPORTED;
    }
    if (strlen(dst_path) + sizeof(FM_FILE_TMP_SUFFIX) > sizeof(tmp))
    {
        FM_Core_SetLastError(FM_ERR_TOO_LARGE);
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(tmp, dst_path);
    (void)strcat(tmp, FM_FILE_TMP_SUFFIX);
    if (FM_File_SamePath(src_path, tmp))
    {
        FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
        return FM_ERR_INVALID_ARGUMENT;
    }
    staged = FM_TRUE;
    work_path = tmp;

    src = File_Open(src_path, FM_OPEN_READ);
    if (src == NULL)
    {
        return File_GetLastError();
    }
    dst = File_Open(work_path, FM_OPEN_WRITE | FM_OPEN_CREATE |
                               FM_OPEN_EXCLUSIVE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        FM_Core_SetLastError(r);
        return r;
    }

    r = FM_File_CopyStream(src, dst, progress, user_data);
    rc = File_Close(dst);
    if (r == FM_OK)
    {
        r = rc;
    }
    rc = File_Close(src);
    if (r == FM_OK)
    {
        r = rc;
    }
    if (r == FM_OK && staged)
    {
        /* A failed rename may already have committed. Do not delete this
         * path afterward: another actor could have recreated it. */
        r = File_Rename(tmp, dst_path);
    }
    else if (r != FM_OK)
    {
        (void)File_Delete(work_path); /* pre-publication cleanup */
    }
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

    /* F-01: reject NULL here and report the resolver's own verdict for
     * BOTH paths (invalid path / too long / not mounted) instead of
     * folding every cause into FM_ERR_NOT_FOUND. */
    if (src_path == NULL || dst_path == NULL)
    {
        FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_VFS_Pin(src_path, &m1, r1, sizeof(r1));
    if (r == FM_OK)
    {
        r = FM_VFS_Resolve(dst_path, &m2, r2, sizeof(r2));
        if (r != FM_OK)
        {
            FM_VFS_Unpin(m1);
        }
    }
    if (r != FM_OK)
    {
        FM_Core_SetLastError(r);
        return r;
    }

    /* Same-mount move is a rename (atomic and fast). */
    if (m1 == m2 && m1->fs->ops.rename != NULL)
    {
        r = FM_File_RenameResolved(m1, r1, r2);
        FM_VFS_Unpin(m1);
        FM_Core_SetLastError(r);
        return r;
    }
    FM_VFS_Unpin(m1);

    /* Cross-mount: publish the copy, then delete the source. If source
     * deletion fails, never delete the published destination: an external
     * writer could already have replaced that name, and removing it would
     * destroy unrelated data. The error means both names may remain. */
    r = File_Copy(src_path, dst_path, progress, user_data);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Delete(src_path);
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
    size_t buf_size = 0;
    uint64_t size = 0;
    uint64_t pos = 0;
    FM_Result r;

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* F-15: the temp name must fit completely.  A silently truncated
     * "<path>.fm_tmp" can be the original path itself (or another
     * file's temp) -- the edit would then replace the wrong entry. */
    if (strlen(path) + sizeof(FM_FILE_TMP_SUFFIX) > sizeof(tmp))
    {
        FM_Core_SetLastError(FM_ERR_TOO_LARGE);
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(tmp, path);
    (void)strcat(tmp, FM_FILE_TMP_SUFFIX);

    if (FM_InternalBufferSize() == 0u)
    {
        return FM_ERR_NO_MEMORY;
    }
    /* F-11: refuse nested use of the shared scratch buffer (e.g. from a
     * File_Copy progress callback) instead of corrupting both users. */
    buf = FM_InternalBufferAcquire(&buf_size);
    if (buf == NULL)
    {
        FM_Core_SetLastError(FM_ERR_BUSY);
        return FM_ERR_BUSY;
    }

    src = File_Open(path, FM_OPEN_READ);
    if (src == NULL)
    {
        FM_InternalBufferRelease();
        return File_GetLastError();
    }
    dst = File_Open(tmp, FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_EXCLUSIVE);
    if (dst == NULL)
    {
        r = File_GetLastError();
        (void)File_Close(src);
        FM_InternalBufferRelease();
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

    FM_InternalBufferRelease();
    buf = NULL;

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
    /* On rename error the commit is unknown; preserve any name now there. */
    FM_Core_SetLastError(r);
    return r;

fail:
    if (buf != NULL)
    {
        FM_InternalBufferRelease();
    }
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
