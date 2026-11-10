/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_filesystem.c - Filesystem adapter registry + mount table (VFS)
 */

#include "fm_internal.h"
#include <string.h>

static FM_FS *g_fs_adapters[FM_MAX_FS];
static FM_MountEntry g_mounts[FM_MAX_MOUNTS];

/* ------------------------------------------------------------------ */
/* Locking                                                             */
/*                                                                     */
/* Three kinds of lock live here (see the hierarchy in fm_sync.h):     */
/*                                                                     */
/*   g_mount_op_lock  serialises FS_Mount / FS_Unmount as a whole, so  */
/*                    the storage binding of one cannot interleave     */
/*                    with the other.  Outermost.                      */
/*   FM_MountEntry.lock  one per mount point; held around every call   */
/*                    into that filesystem's backend, which is what    */
/*                    makes FatFs / LittleFS / FMFS usable from more   */
/*                    than one task.                                   */
/*   g_vfs_lock (FM_Lock)  the tables: mounts, adapters, handle pools. */
/*                    Short sections only; never held across a backend */
/*                    call, a storage driver call or a user callback.  */
/* ------------------------------------------------------------------ */
#if FM_USE_THREADS
static FM_Mutex g_vfs_lock;
static FM_Bool  g_vfs_lock_init = FM_FALSE;
static FM_Mutex g_mount_op_lock;
static FM_Bool  g_mount_op_lock_init = FM_FALSE;

void FM_Lock(void)
{
    /* Created by FileManager_Init().  The lazy path only exists for
     * applications that skip it, and is safe as long as the first
     * FileManager call is made before a second task is started. */
    if (!g_vfs_lock_init)
    {
        if (FM_Mutex_Create(&g_vfs_lock) == FM_OK)
        {
            g_vfs_lock_init = FM_TRUE;
        }
    }
    if (g_vfs_lock_init)
    {
        FM_Mutex_Lock(&g_vfs_lock);
    }
}

void FM_Unlock(void)
{
    if (g_vfs_lock_init)
    {
        FM_Mutex_Unlock(&g_vfs_lock);
    }
}

static void mount_op_lock(void)
{
    FM_Bool ready;

    FM_Lock();
    if (!g_mount_op_lock_init &&
        FM_Mutex_Create(&g_mount_op_lock) == FM_OK)
    {
        g_mount_op_lock_init = FM_TRUE;
    }
    ready = g_mount_op_lock_init;
    FM_Unlock();
    if (ready)
    {
        FM_Mutex_Lock(&g_mount_op_lock);
    }
}

static void mount_op_unlock(void)
{
    if (g_mount_op_lock_init)
    {
        FM_Mutex_Unlock(&g_mount_op_lock);
    }
}

/* Call with FM_Lock held. */
static FM_Bool mount_lock_prepare(FM_MountEntry *m)
{
    if (!m->lock_ready && FM_Mutex_Create(&m->lock) == FM_OK)
    {
        m->lock_ready = FM_TRUE;
    }
    return m->lock_ready;
}

void FM_Mount_Lock(void *mount)
{
    FM_MountEntry *m = (FM_MountEntry *)mount;

    /* lock_ready is written once, under FM_Lock, before the entry is
     * first published; every caller got `m` from the table after that. */
    if (m != NULL && m->lock_ready)
    {
        FM_Mutex_Lock(&m->lock);
    }
}

void FM_Mount_Unlock(void *mount)
{
    FM_MountEntry *m = (FM_MountEntry *)mount;

    if (m != NULL && m->lock_ready)
    {
        FM_Mutex_Unlock(&m->lock);
    }
}

void FM_VFS_SyncInit(void)
{
    uint32_t i;

    if (!g_vfs_lock_init && FM_Mutex_Create(&g_vfs_lock) == FM_OK)
    {
        g_vfs_lock_init = FM_TRUE;
    }
    FM_Lock();
    if (!g_mount_op_lock_init &&
        FM_Mutex_Create(&g_mount_op_lock) == FM_OK)
    {
        g_mount_op_lock_init = FM_TRUE;
    }
    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        (void)mount_lock_prepare(&g_mounts[i]);
    }
    FM_Unlock();
}

void FM_VFS_SyncDeinit(void)
{
    uint32_t i;

    /* FileManager_Deinit() is called once every task has stopped using
     * the library.  Locks of mount points that are still mounted are
     * kept: their open handles may still be closed afterwards. */
    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        FM_MountEntry *m = &g_mounts[i];

        if (m->lock_ready && !m->used && !m->reserved && m->pins == 0u)
        {
            FM_Mutex_Destroy(&m->lock);
            m->lock_ready = FM_FALSE;
        }
    }
    if (g_mount_op_lock_init)
    {
        FM_Mutex_Destroy(&g_mount_op_lock);
        g_mount_op_lock_init = FM_FALSE;
    }
    if (g_vfs_lock_init)
    {
        FM_Mutex_Destroy(&g_vfs_lock);
        g_vfs_lock_init = FM_FALSE;
    }
}
#else
void FM_Lock(void)   { /* no-op */ }
void FM_Unlock(void) { /* no-op */ }
void FM_VFS_SyncInit(void)   { /* no-op */ }
void FM_VFS_SyncDeinit(void) { /* no-op */ }
#define mount_op_lock()        ((void)0)
#define mount_op_unlock()      ((void)0)
#define mount_lock_prepare(m)  ((void)(m), FM_TRUE)
#endif

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */
FM_Result FS_Register(FM_FS *fs)
{
    uint32_t i;

    if (fs == NULL || fs->name[0] == '\0' || fs->ops.file_open == NULL ||
        fs->ops.file_read == NULL || fs->ops.file_write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    FM_Lock();
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fs_adapters[i] != NULL && strcmp(g_fs_adapters[i]->name,
                                               fs->name) == 0)
        {
            FM_Unlock();
            return FM_ERR_EXISTS;
        }
    }
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fs_adapters[i] == NULL)
        {
            g_fs_adapters[i] = fs;
            fs->next = NULL;
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NO_MEMORY;
}

FM_Result FS_Unregister(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fs_adapters[i] != NULL && strcmp(g_fs_adapters[i]->name,
                                               name) == 0)
        {
            if (g_fs_adapters[i]->mounted)
            {
                FM_Unlock();
                return FM_ERR_BUSY;
            }
            g_fs_adapters[i] = NULL;
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NOT_FOUND;
}

FM_Result FS_Find(const char *name, FM_FS **out)
{
    uint32_t i;

    if (name == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    for (i = 0; i < FM_MAX_FS; i++)
    {
        if (g_fs_adapters[i] != NULL && strcmp(g_fs_adapters[i]->name,
                                               name) == 0)
        {
            *out = g_fs_adapters[i];
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Mount table                                                         */
/* ------------------------------------------------------------------ */
FM_MountEntry *FM_VFS_GetMounts(void)
{
    return g_mounts;
}

uint32_t FM_VFS_GetMountCount(void)
{
    uint32_t i;
    uint32_t used = 0;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used)
        {
            used++;
        }
    }
    return used;
}

static FM_MountEntry *mount_find(const char *mount_path)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && strcmp(g_mounts[i].mount_path,
                                       mount_path) == 0)
        {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* A slot is `reserved` while FS_Mount / FS_Unmount work on it with the
 * VFS lock released: path resolution does not see it (used == 0), but
 * neither its slot nor its mount path can be taken by someone else. */
static FM_MountEntry *mount_find_any(const char *mount_path)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if ((g_mounts[i].used || g_mounts[i].reserved) &&
            strcmp(g_mounts[i].mount_path, mount_path) == 0)
        {
            return &g_mounts[i];
        }
    }
    return NULL;
}

static FM_Bool mount_has_fs(const FM_FS *fs)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if ((g_mounts[i].used || g_mounts[i].reserved) &&
            g_mounts[i].fs == fs)
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

static FM_MountEntry *mount_free_slot(void)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (!g_mounts[i].used && !g_mounts[i].reserved &&
            g_mounts[i].pins == 0u)
        {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* Pin an active entry found by mount path: the mount cannot go away
 * until FM_VFS_Unpin().  Returns NULL when nothing is mounted there. */
static FM_MountEntry *mount_pin_path(const char *normalized)
{
    FM_MountEntry *entry;

    FM_Lock();
    entry = mount_find(normalized);
    if (entry != NULL)
    {
        entry->pins++;
    }
    FM_Unlock();
    return entry;
}

static FM_Result mount_path_normalize(const char *mount_path, char *out,
                                      size_t out_size)
{
    if (mount_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(mount_path) >= out_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(out, mount_path);
    if (Path_Normalize(out, out_size) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }
    return FM_OK;
}

static FM_Bool mount_path_ok(const char *path)
{
    /* Mount points are "/" or "/name" (single component). */
    if (path == NULL || path[0] != '/')
    {
        return FM_FALSE;
    }
    if (path[1] == '\0')
    {
        return FM_TRUE; /* root */
    }
    if (strchr(path + 1, '/') != NULL)
    {
        return FM_FALSE;
    }
    return FM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Mount API                                                           */
/* ------------------------------------------------------------------ */
/* Storage lookup + backend mount for FS_Mount.  Runs WITHOUT the VFS
 * lock: Storage_Mount() calls the driver's on_mount hook and the backend
 * reads the medium, and neither may stall every other task's file
 * access or run user code under a table lock. */
static FM_Result mount_bind(FM_FS *fs, const char *normalized)
{
    FM_Storage *storage = NULL;

    /* Root mount uses the first mounted storage; named mounts use the
     * storage with the same name (auto-mount). */
    if (normalized[1] == '\0')
    {
        storage = FM_Storage_FirstMounted();
    }
    else
    {
        char sname[FM_NAME_MAX];
        const char *p = normalized + 1;
        size_t len = 0;

        while (p[len] != '\0' && len + 1 < sizeof(sname))
        {
            sname[len] = p[len];
            len++;
        }
        sname[len] = '\0';

        if (Storage_Find(sname, &storage) != FM_OK)
        {
            return FM_ERR_NOT_FOUND;
        }
        if (!FM_Storage_IsMounted(storage))
        {
            FM_Result r = Storage_Mount(sname);

            if (r != FM_OK)
            {
                return r;
            }
        }
    }

    fs->storage = storage;
    if (fs->ops.mount != NULL)
    {
        return fs->ops.mount(fs, storage);
    }
    return (storage == NULL) ? FM_ERR_NOT_READY : FM_OK;
}

FM_Result FS_Mount(const char *mount_path, FM_FS *fs)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    FM_Result r;

    if (mount_path == NULL || fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (fs->mounted)
    {
        return FM_ERR_BUSY;
    }
    r = mount_path_normalize(mount_path, normalized, sizeof(normalized));
    if (r != FM_OK)
    {
        return r;
    }
    if (!mount_path_ok(normalized))
    {
        return FM_ERR_INVALID_PATH;
    }

    mount_op_lock();

    /* 1. reserve the slot and the mount path */
    FM_Lock();
    if (fs->mounted || mount_has_fs(fs))
    {
        r = FM_ERR_BUSY;
    }
    else if (mount_find_any(normalized) != NULL)
    {
        r = FM_ERR_EXISTS;
    }
    else if ((entry = mount_free_slot()) == NULL)
    {
        r = FM_ERR_NO_MEMORY;
    }
    else if (!mount_lock_prepare(entry))
    {
        r = FM_ERR_INTERNAL;
    }
    else
    {
        (void)strcpy(entry->mount_path, normalized);
        entry->fs = fs;
        entry->reserved = FM_TRUE;
    }
    FM_Unlock();
    if (r != FM_OK)
    {
        mount_op_unlock();
        return r;
    }

    /* 2. bind the storage and mount the backend, VFS lock released */
    r = mount_bind(fs, normalized);

    /* 3. publish, or give the slot back */
    FM_Lock();
    if (r == FM_OK)
    {
        fs->mounted = FM_TRUE;
        entry->used = FM_TRUE;
    }
    else
    {
        entry->fs = NULL;
        entry->mount_path[0] = '\0';
    }
    entry->reserved = FM_FALSE;
    FM_Unlock();

    mount_op_unlock();
    return r;
}

FM_Result FS_MountDefault(FM_FS *fs)
{
    return FS_Mount("/", fs);
}

FM_Result FS_Unmount(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    FM_FS *fs;
    FM_Storage *storage;
    FM_Bool storage_in_use = FM_FALSE;
    FM_Result r;
    uint32_t i;

    r = mount_path_normalize(mount_path, normalized, sizeof(normalized));
    if (r != FM_OK)
    {
        return r;
    }

    mount_op_lock();

    /* 1. take the mount out of path resolution.  Open handles and path
     *    operations in flight (pins) keep it alive: FM_ERR_BUSY. */
    FM_Lock();
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        r = FM_ERR_NOT_MOUNTED;
    }
    else if (entry->pins != 0u || FM_File_AnyOpenOnMount(entry) ||
             FM_Dir_AnyOpenOnMount(entry))
    {
        r = FM_ERR_BUSY;
    }
    else
    {
        entry->used = FM_FALSE;
        entry->reserved = FM_TRUE;
    }
    FM_Unlock();
    if (r != FM_OK)
    {
        mount_op_unlock();
        return r;
    }

    /* 2. nobody can reach the backend any more; flush and unmount it
     *    with the VFS lock released */
    fs = entry->fs;
    storage = fs->storage;
    if (fs->ops.unmount != NULL)
    {
        (void)fs->ops.unmount(fs);
    }

    /* 3. release the slot */
    FM_Lock();
    fs->mounted = FM_FALSE;
    fs->storage = NULL;
    entry->fs = NULL;
    entry->mount_path[0] = '\0';
    entry->reserved = FM_FALSE;
    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && g_mounts[i].fs != NULL &&
            g_mounts[i].fs->storage == storage)
        {
            storage_in_use = FM_TRUE;
            break;
        }
    }
    FM_Unlock();

    if (!storage_in_use && storage != NULL)
    {
        (void)Storage_Unmount(storage->name);
    }

    mount_op_unlock();
    return FM_OK;
}

FM_Result FS_Format(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    FM_Result r;

    r = mount_path_normalize(mount_path, normalized, sizeof(normalized));
    if (r != FM_OK)
    {
        return r;
    }

    FM_Lock();
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        r = FM_ERR_NOT_MOUNTED;
    }
    else if (entry->fs->ops.format == NULL)
    {
        r = FM_ERR_UNSUPPORTED;
    }
    else if (FM_File_AnyOpenOnMount(entry) || FM_Dir_AnyOpenOnMount(entry))
    {
        /* Formatting under an open handle leaves that handle pointing
         * into a filesystem that no longer exists. */
        r = FM_ERR_BUSY;
    }
    else
    {
        entry->pins++;
    }
    FM_Unlock();
    if (r != FM_OK)
    {
        return r;
    }

    FM_Mount_Lock(entry);
    r = entry->fs->ops.format(entry->fs);
    FM_Mount_Unlock(entry);
    FM_VFS_Unpin(entry);
    return r;
}

FM_Result FS_Check(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    FM_Result r;

    r = mount_path_normalize(mount_path, normalized, sizeof(normalized));
    if (r != FM_OK)
    {
        return r;
    }
    entry = mount_pin_path(normalized);
    if (entry == NULL)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    if (entry->fs->ops.check == NULL)
    {
        r = FM_ERR_UNSUPPORTED;
    }
    else
    {
        FM_Mount_Lock(entry);
        r = entry->fs->ops.check(entry->fs);
        FM_Mount_Unlock(entry);
    }
    FM_VFS_Unpin(entry);
    return r;
}

FM_Result FS_GetInfo(const char *mount_path, FM_FSInfo *info)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    FM_FS *fs;
    FM_Storage *storage;
    uint64_t free = 0;
    uint64_t total = 0;
    FM_Result r;

    if (mount_path == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = mount_path_normalize(mount_path, normalized, sizeof(normalized));
    if (r != FM_OK)
    {
        return r;
    }
    entry = mount_pin_path(normalized);
    if (entry == NULL)
    {
        return FM_ERR_NOT_MOUNTED;
    }
    fs = entry->fs;
    storage = fs->storage;

    memset(info, 0, sizeof(*info));
    (void)strcpy(info->name, fs->name);

    FM_Mount_Lock(entry);
    if (fs->ops.get_free != NULL)
    {
        FM_Result free_result = fs->ops.get_free(fs, &free, &total);

        if (free_result != FM_OK && storage != NULL)
        {
            total = storage->capacity;
            free = FM_Storage_FreeSpaceHint(storage);
        }
    }
    else if (storage != NULL)
    {
        total = storage->capacity;
        free = FM_Storage_FreeSpaceHint(storage);
    }
    info->capacity = total;
    info->free_space = free;
    if (fs->ops.get_type != NULL)
    {
        (void)fs->ops.get_type(fs, info->type, sizeof(info->type));
    }
    else
    {
        size_t type_len = strlen(fs->name);

        if (type_len >= sizeof(info->type))
        {
            type_len = sizeof(info->type) - 1;
        }
        memcpy(info->type, fs->name, type_len);
        info->type[type_len] = '\0';
    }
    FM_Mount_Unlock(entry);
    info->cluster_size = (storage != NULL) ? storage->block_size : 0u;

    /* update free-space hint on the storage */
    if (storage != NULL)
    {
        FM_Storage_SetFreeSpace(storage->name, free);
    }

    FM_VFS_Unpin(entry);
    return FM_OK;
}

FM_Result FS_GetCapacity(const char *mount_path, uint64_t *capacity)
{
    FM_FSInfo info;
    FM_Result r;

    if (capacity == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FS_GetInfo(mount_path, &info);
    if (r != FM_OK)
    {
        return r;
    }
    *capacity = info.capacity;
    return FM_OK;
}

FM_Result FS_GetFreeSpace(const char *mount_path, uint64_t *free)
{
    FM_FSInfo info;
    FM_Result r;

    if (free == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FS_GetInfo(mount_path, &info);
    if (r != FM_OK)
    {
        return r;
    }
    *free = info.free_space;
    return FM_OK;
}

FM_Result FS_IsMounted(const char *mount_path, FM_Bool *mounted)
{
    char normalized[FM_PATH_MAX];

    if (mount_path == NULL || mounted == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(mount_path) >= sizeof(normalized))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(normalized, mount_path);
    if (Path_Normalize(normalized, sizeof(normalized)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }

    FM_Lock();
    *mounted = (mount_find(normalized) != NULL) ? FM_TRUE : FM_FALSE;
    FM_Unlock();
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */
static FM_Result vfs_resolve(const char *path, FM_MountEntry **mount_out,
                             char *rel_buf, size_t rel_buf_size,
                             FM_Bool pin)
{
    char normalized[FM_PATH_MAX];
    size_t best = 0;
    uint32_t i;
    FM_MountEntry *best_mount = NULL;
    const char *rel_ptr;
    size_t rel_len;

    if (path == NULL || mount_out == NULL || rel_buf == NULL ||
        rel_buf_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (path[0] != '/')
    {
        return FM_ERR_INVALID_PATH;
    }
    if (strlen(path) >= sizeof(normalized))
    {
        return FM_ERR_TOO_LARGE;
    }

    (void)strcpy(normalized, path);
    if (Path_Normalize(normalized, sizeof(normalized)) != FM_OK)
    {
        return FM_ERR_INVALID_PATH;
    }

    FM_Lock();
    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        FM_MountEntry *m = &g_mounts[i];
        size_t mlen;

        if (!m->used)
        {
            continue;
        }
        mlen = strlen(m->mount_path);
        if (mlen == 1)
        {
            if (best_mount == NULL)
            {
                best_mount = m;
                best = 0;
            }
            continue;
        }
        if (strncmp(normalized, m->mount_path, mlen) == 0 &&
            (normalized[mlen] == '\0' || normalized[mlen] == '/') &&
            mlen > best)
        {
            best = mlen;
            best_mount = m;
        }
    }

    if (best_mount == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_MOUNTED;
    }

    {
        size_t mlen = strlen(best_mount->mount_path);

        /* For an exact named mount (/sd), the relative path is empty.
         * Never read one byte past the normalized string. */
        if (mlen == 1)
        {
            rel_ptr = normalized + 1;
        }
        else if (normalized[mlen] == '\0')
        {
            rel_ptr = normalized + mlen;
        }
        else
        {
            rel_ptr = normalized + mlen + 1;
        }
    }
    rel_len = strlen(rel_ptr);
    if (rel_len + 1 > rel_buf_size)
    {
        FM_Unlock();
        return FM_ERR_TOO_LARGE;
    }
    memcpy(rel_buf, rel_ptr, rel_len + 1);
    if (pin)
    {
        best_mount->pins++;
    }
    *mount_out = best_mount;
    FM_Unlock();
    return FM_OK;
}

/* Lookup only.  The entry may be unmounted the moment this returns, so
 * the result is good for comparing mounts, not for calling the backend:
 * use FM_VFS_Pin() for that. */
FM_Result FM_VFS_Resolve(const char *path, FM_MountEntry **mount_out,
                         char *rel_buf, size_t rel_buf_size)
{
    return vfs_resolve(path, mount_out, rel_buf, rel_buf_size, FM_FALSE);
}

FM_Result FM_VFS_Pin(const char *path, FM_MountEntry **mount_out,
                     char *rel_buf, size_t rel_buf_size)
{
    return vfs_resolve(path, mount_out, rel_buf, rel_buf_size, FM_TRUE);
}

FM_MountEntry *FM_VFS_EnterFS(const FM_FS *fs)
{
    FM_MountEntry *found = NULL;
    uint32_t i;

    if (fs == NULL)
    {
        return NULL;
    }
    FM_Lock();
    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && g_mounts[i].fs == fs)
        {
            found = &g_mounts[i];
            found->pins++;
            break;
        }
    }
    FM_Unlock();
    if (found != NULL)
    {
        FM_Mount_Lock(found);
    }
    return found;
}

void FM_VFS_LeaveFS(FM_MountEntry *mount)
{
    if (mount != NULL)
    {
        FM_Mount_Unlock(mount);
        FM_VFS_Unpin(mount);
    }
}

void FM_VFS_Unpin(FM_MountEntry *mount)
{
    if (mount == NULL)
    {
        return;
    }
    FM_Lock();
    if (mount->pins != 0u)
    {
        mount->pins--;
    }
    FM_Unlock();
}

FM_Result FM_VFS_ResolveToFS(const char *path, FM_FS **fs,
                             char *rel_buf, size_t rel_buf_size)
{
    FM_MountEntry *mount;

    if (fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        FM_Result r = FM_VFS_Resolve(path, &mount, rel_buf, rel_buf_size);

        if (r != FM_OK)
        {
            return r;
        }
    }
    *fs = mount->fs;
    return FM_OK;
}
