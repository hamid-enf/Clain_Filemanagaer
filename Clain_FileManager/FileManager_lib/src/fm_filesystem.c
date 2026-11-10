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
/* ------------------------------------------------------------------ */
#if FM_USE_THREADS
static FM_Mutex g_vfs_lock;
static FM_Bool  g_vfs_lock_init = FM_FALSE;

void FM_Lock(void)
{
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
#else
void FM_Lock(void)   { /* no-op */ }
void FM_Unlock(void) { /* no-op */ }
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

static FM_MountEntry *mount_free_slot(void)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_MOUNTS; i++)
    {
        if (!g_mounts[i].used)
        {
            return &g_mounts[i];
        }
    }
    return NULL;
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
FM_Result FS_Mount(const char *mount_path, FM_FS *fs)
{
    char normalized[FM_PATH_MAX];
    FM_Storage *storage = NULL;
    FM_MountEntry *entry;

    if (mount_path == NULL || fs == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (fs->mounted)
    {
        return FM_ERR_BUSY;
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
    if (!mount_path_ok(normalized))
    {
        return FM_ERR_INVALID_PATH;
    }

    FM_Lock();

    if (mount_find(normalized) != NULL)
    {
        FM_Unlock();
        return FM_ERR_EXISTS;
    }
    entry = mount_free_slot();
    if (entry == NULL)
    {
        FM_Unlock();
        return FM_ERR_NO_MEMORY;
    }

    /* Determine the storage: root mount uses the first mounted storage;
     * named mounts use the storage with the same name (auto-mount). */
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
            FM_Unlock();
            return FM_ERR_NOT_FOUND;
        }
        if (!storage->mounted)
        {
            FM_Result r = Storage_Mount(sname);

            if (r != FM_OK)
            {
                FM_Unlock();
                return r;
            }
        }
    }

    /* Bind and mount */
    fs->storage = storage;
    if (fs->ops.mount != NULL)
    {
        FM_Result r = fs->ops.mount(fs, storage);

        if (r != FM_OK)
        {
            FM_Unlock();
            return r;
        }
    }
    else if (storage == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_READY;
    }
    fs->mounted = FM_TRUE;

    (void)strcpy(entry->mount_path, normalized);
    entry->fs = fs;
    entry->used = FM_TRUE;

    FM_Unlock();
    return FM_OK;
}

FM_Result FS_MountDefault(FM_FS *fs)
{
    return FS_Mount("/", fs);
}

FM_Result FS_Unmount(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;

    if (mount_path == NULL)
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
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_MOUNTED;
    }
    if (FM_File_AnyOpenOnMount(entry) || FM_Dir_AnyOpenOnMount(entry))
    {
        FM_Unlock();
        return FM_ERR_BUSY;
    }

    {
        FM_Storage *storage = entry->fs->storage;
        FM_Bool storage_in_use = FM_FALSE;
        uint32_t i;

        if (entry->fs->ops.unmount != NULL)
        {
            (void)entry->fs->ops.unmount(entry->fs);
        }
        entry->fs->mounted = FM_FALSE;
        entry->fs->storage = NULL;
        entry->used = FM_FALSE;
        entry->fs = NULL;

        for (i = 0; i < FM_MAX_MOUNTS; i++)
        {
            if (g_mounts[i].used && g_mounts[i].fs != NULL &&
                g_mounts[i].fs->storage == storage)
            {
                storage_in_use = FM_TRUE;
                break;
            }
        }
        if (!storage_in_use && storage != NULL)
        {
            (void)Storage_Unmount(storage->name);
        }
    }

    FM_Unlock();
    return FM_OK;
}

FM_Result FS_Format(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;

    if (mount_path == NULL)
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
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_MOUNTED;
    }
    if (entry->fs->ops.format == NULL)
    {
        FM_Unlock();
        return FM_ERR_UNSUPPORTED;
    }
    {
        FM_Result r = entry->fs->ops.format(entry->fs);

        FM_Unlock();
        return r;
    }
}

FM_Result FS_Check(const char *mount_path)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;

    if (mount_path == NULL)
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
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_MOUNTED;
    }
    if (entry->fs->ops.check == NULL)
    {
        FM_Unlock();
        return FM_ERR_UNSUPPORTED;
    }
    {
        FM_Result r = entry->fs->ops.check(entry->fs);

        FM_Unlock();
        return r;
    }
}

FM_Result FS_GetInfo(const char *mount_path, FM_FSInfo *info)
{
    char normalized[FM_PATH_MAX];
    FM_MountEntry *entry;
    uint64_t free = 0;
    uint64_t total = 0;

    if (mount_path == NULL || info == NULL)
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
    entry = mount_find(normalized);
    if (entry == NULL)
    {
        FM_Unlock();
        return FM_ERR_NOT_MOUNTED;
    }

    memset(info, 0, sizeof(*info));
    (void)strcpy(info->name, entry->fs->name);
    if (entry->fs->ops.get_free != NULL)
    {
        FM_Result free_result = entry->fs->ops.get_free(entry->fs, &free,
                                                         &total);
        if (free_result != FM_OK && entry->fs->storage != NULL)
        {
            total = entry->fs->storage->capacity;
            free = entry->fs->storage->free_space;
        }
    }
    else if (entry->fs->storage != NULL)
    {
        total = entry->fs->storage->capacity;
        free = entry->fs->storage->free_space;
    }
    info->capacity = total;
    info->free_space = free;
    if (entry->fs->ops.get_type != NULL)
    {
        (void)entry->fs->ops.get_type(entry->fs, info->type,
                                      sizeof(info->type));
    }
    else
    {
        size_t type_len = strlen(entry->fs->name);

        if (type_len >= sizeof(info->type))
        {
            type_len = sizeof(info->type) - 1;
        }
        memcpy(info->type, entry->fs->name, type_len);
        info->type[type_len] = '\0';
    }
    info->cluster_size = (entry->fs->storage != NULL)
                             ? entry->fs->storage->block_size
                             : 0u;

    /* update free-space hint on the storage */
    if (entry->fs->storage != NULL)
    {
        FM_Storage_SetFreeSpace(entry->fs->storage->name, free);
    }

    FM_Unlock();
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
FM_Result FM_VFS_Resolve(const char *path, FM_MountEntry **mount_out,
                         char *rel_buf, size_t rel_buf_size)
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
    *mount_out = best_mount;
    FM_Unlock();
    return FM_OK;
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
