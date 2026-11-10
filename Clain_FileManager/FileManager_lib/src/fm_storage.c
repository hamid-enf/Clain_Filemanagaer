/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_storage.c - Storage (block device) registry and lifecycle
 */

#define FM_DEBUG_TAG "storage"
#include "fm_debug.h"
#include "fm_storage.h"
#include "fm_internal.h"
#include <string.h>

static FM_Storage *g_storages[FM_MAX_STORAGES];
static FM_StorageEventCb g_event_cb = NULL;
static void *g_event_data = NULL;

/* Dedicated registry lock.  It is intentionally separate from the VFS
 * FM_Lock: FS_Mount/FS_GetInfo hold the VFS lock while calling Storage_*
 * helpers, so reusing FM_Lock here would deadlock on non-recursive
 * mutexes.  Lock ordering is always VFS lock -> storage lock. */
#if FM_USE_THREADS
static FM_Mutex g_storage_lock;
static FM_Bool  g_storage_lock_init = FM_FALSE;

static void storage_lock(void)
{
    if (!g_storage_lock_init)
    {
        if (FM_Mutex_Create(&g_storage_lock) == FM_OK)
        {
            g_storage_lock_init = FM_TRUE;
        }
    }
    if (g_storage_lock_init)
    {
        FM_Mutex_Lock(&g_storage_lock);
    }
}

static void storage_unlock(void)
{
    if (g_storage_lock_init)
    {
        FM_Mutex_Unlock(&g_storage_lock);
    }
}
#else
static void storage_lock(void)   { /* no-op */ }
static void storage_unlock(void) { /* no-op */ }
#endif

static FM_Storage **storage_find_slot(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }
    for (i = 0; i < FM_MAX_STORAGES; i++)
    {
        if (g_storages[i] != NULL && strcmp(g_storages[i]->name, name) == 0)
        {
            return &g_storages[i];
        }
    }
    return NULL;
}

static FM_Storage **storage_free_slot(void)
{
    uint32_t i;

    for (i = 0; i < FM_MAX_STORAGES; i++)
    {
        if (g_storages[i] == NULL)
        {
            return &g_storages[i];
        }
    }
    return NULL;
}

FM_Result Storage_Init(FM_Storage *s)
{
    if (s == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(s, 0, sizeof(*s));
    s->present = FM_TRUE;
    s->access_mode = FM_ACCESS_READ;
    return FM_OK;
}

FM_Result Storage_InitDriver(FM_Storage *s, const FM_StorageConfig *config)
{
    size_t name_len;
    FM_Result r;

    if (s == NULL || config == NULL || config->name == NULL ||
        config->capacity == 0 || config->read == NULL ||
        config->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    name_len = strlen(config->name);
    if (name_len == 0 || name_len >= sizeof(s->name))
    {
        return FM_ERR_TOO_LARGE;
    }
    r = Storage_Init(s);
    if (r != FM_OK)
    {
        return r;
    }
    memcpy(s->name, config->name, name_len + 1);
    s->type = config->type;
    s->capacity = config->capacity;
    s->free_space = config->free_space;
    s->block_size = config->block_size;
    s->read_size = config->read_size;
    s->write_size = config->write_size;
    s->erase_size = config->erase_size;
    s->read_only = config->read_only ? FM_TRUE : FM_FALSE;
    s->present = config->present ? FM_TRUE : FM_FALSE;
    s->access_mode = s->read_only ? FM_ACCESS_READ : FM_ACCESS_WRITE;
    s->user_data = config->user_data;
    s->read = config->read;
    s->write = config->write;
    s->erase = config->erase;
    s->sync = config->sync;
    s->on_mount = config->on_mount;
    s->on_unmount = config->on_unmount;
    s->on_format = config->on_format;
    return FM_OK;
}

FM_Result Storage_Register(FM_Storage *s)
{
    FM_Storage **slot;

    if (s == NULL || s->name[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(s->name) >= FM_NAME_MAX || s->capacity == 0 ||
        s->read == NULL || s->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    storage_lock();
    if (storage_find_slot(s->name) != NULL)
    {
        storage_unlock();
        return FM_ERR_EXISTS;
    }
    slot = storage_free_slot();
    if (slot == NULL)
    {
        storage_unlock();
        return FM_ERR_NO_MEMORY;
    }
    s->next = NULL;
    *slot = s;
    storage_unlock();
    return FM_OK;
}

FM_Result Storage_Unregister(const char *name)
{
    FM_Storage **slot;

    storage_lock();
    slot = storage_find_slot(name);
    if (slot == NULL)
    {
        storage_unlock();
        return FM_ERR_NOT_FOUND;
    }
    if ((*slot)->mounted)
    {
        storage_unlock();
        return FM_ERR_BUSY;
    }
    *slot = NULL;
    storage_unlock();
    return FM_OK;
}

FM_Result Storage_Find(const char *name, FM_Storage **out)
{
    FM_Storage **slot;

    if (name == NULL || out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    storage_lock();
    slot = storage_find_slot(name);
    if (slot == NULL)
    {
        storage_unlock();
        return FM_ERR_NOT_FOUND;
    }
    *out = *slot;
    storage_unlock();
    return FM_OK;
}

FM_Result Storage_Mount(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    FM_CHECK_PTR(name, FM_ERR_INVALID_ARGUMENT);
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        FM_ERR_LOG("mount %s: not found (%d)", name, (int)r);
        return r;
    }
    if (!s->present)
    {
        FM_ERR_LOG("mount %s: not present", name);
        return FM_ERR_NOT_READY;
    }
    if (s->mounted)
    {
        return FM_OK; /* idempotent */
    }
    if (s->on_mount != NULL)
    {
        r = s->on_mount(s);
        if (r != FM_OK)
        {
            FM_ERR_LOG("mount %s: on_mount callback failed (%d)", name, (int)r);
            return r;
        }
    }
    s->mounted = FM_TRUE;
    if (s->access_mode == FM_ACCESS_NONE)
    {
        s->access_mode = s->read_only ? FM_ACCESS_READ : FM_ACCESS_WRITE;
    }
    FM_EVENT_LOG("mounted storage %s", name);
    return FM_OK;
}

FM_Result Storage_Unmount(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (!s->mounted)
    {
        return FM_OK;
    }
    if (s->sync != NULL)
    {
        (void)s->sync(s);
    }
    if (s->on_unmount != NULL)
    {
        (void)s->on_unmount(s);
    }
    s->mounted = FM_FALSE;
    return FM_OK;
}

FM_Result Storage_Format(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (s->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    if (!s->present)
    {
        return FM_ERR_NOT_READY;
    }
    if (s->on_format != NULL)
    {
        return s->on_format(s);
    }
    if (s->erase != NULL && s->erase_size > 0)
    {
        uint64_t off;

        if (s->capacity < s->erase_size)
        {
            /* A device smaller than its own erase unit cannot be
             * block-erased; refuse instead of overrunning the device. */
            return FM_ERR_INVALID_ARGUMENT;
        }
        for (off = 0; off + s->erase_size <= s->capacity;
             off += s->erase_size)
        {
            r = s->erase(s, off, s->erase_size);
            if (r != FM_OK)
            {
                return r;
            }
        }
    }
    return FM_OK;
}

FM_Result Storage_GetInfo(const char *name, FM_StorageInfo *info)
{
    FM_Storage *s;
    FM_Result r;

    if (info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    memset(info, 0, sizeof(*info));
    (void)strcpy(info->name, s->name);
    info->type = s->type;
    info->capacity = s->capacity;
    info->block_size = s->block_size;
    info->read_size = s->read_size;
    info->write_size = s->write_size;
    info->erase_size = s->erase_size;
    info->read_only = s->read_only;
    info->present = s->present;
    info->mounted = s->mounted;
    info->access_mode = s->access_mode;
    info->free_space = s->free_space;
    return FM_OK;
}

FM_Result Storage_IsReady(const char *name, FM_Bool *ready)
{
    FM_Storage *s;
    FM_Result r;

    if (ready == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    *ready = (s->present && s->mounted) ? FM_TRUE : FM_FALSE;
    return FM_OK;
}

FM_Result Storage_GetCapacity(const char *name, uint64_t *capacity)
{
    FM_Storage *s;
    FM_Result r;

    if (capacity == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    *capacity = s->capacity;
    return FM_OK;
}

FM_Result Storage_GetFreeSpace(const char *name, uint64_t *free)
{
    FM_Storage *s;
    FM_Result r;

    if (free == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    /* Until a filesystem reports its actual free space (which updates the
     * hint through FM_Storage_SetFreeSpace), claim the full capacity.
     * Once the storage is mounted the hint is meaningful. */
    *free = s->mounted ? s->free_space : s->capacity;
    return FM_OK;
}

FM_Result Storage_SetPresent(FM_Storage *s, FM_Bool present)
{
    if (s == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    s->present = present;
    return FM_OK;
}

FM_Result Storage_Insert(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    s->present = FM_TRUE;
    if (g_event_cb != NULL)
    {
        g_event_cb(name, FM_TRUE, g_event_data);
    }
    return FM_OK;
}

FM_Result Storage_Remove(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (s->mounted)
    {
        (void)Storage_Unmount(name);
    }
    s->present = FM_FALSE;
    if (g_event_cb != NULL)
    {
        g_event_cb(name, FM_FALSE, g_event_data);
    }
    return FM_OK;
}

FM_Result Storage_IsPresent(const char *name, FM_Bool *present)
{
    FM_Storage *s;
    FM_Result r;

    if (present == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    *present = s->present;
    return FM_OK;
}

FM_Result Storage_SetEventCallback(FM_StorageEventCb cb, void *user_data)
{
    storage_lock();
    g_event_cb = cb;
    g_event_data = user_data;
    storage_unlock();
    return FM_OK;
}

FM_Result Storage_ReadBlocks(const char *name, uint64_t offset, void *buf,
                             uint32_t len)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (buf == NULL || len == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!s->present)
    {
        return FM_ERR_NOT_READY;
    }
    if (offset > s->capacity || (uint64_t)len > s->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (s->read == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    return s->read(s, offset, buf, len);
}

FM_Result Storage_WriteBlocks(const char *name, uint64_t offset,
                              const void *buf, uint32_t len)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (buf == NULL || len == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (s->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    if (!s->present)
    {
        return FM_ERR_NOT_READY;
    }
    if (offset > s->capacity || (uint64_t)len > s->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (s->write == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    return s->write(s, offset, buf, len);
}

FM_Result Storage_EraseBlocks(const char *name, uint64_t offset, uint32_t len)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (s->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    if (s->erase == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (!s->present)
    {
        return FM_ERR_NOT_READY;
    }
    if (len == 0 || offset > s->capacity ||
        (uint64_t)len > s->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return s->erase(s, offset, len);
}

FM_Result Storage_Sync(const char *name)
{
    FM_Storage *s;
    FM_Result r;

    r = Storage_Find(name, &s);
    if (r != FM_OK)
    {
        return r;
    }
    if (s->sync == NULL)
    {
        return FM_OK;
    }
    return s->sync(s);
}

/* Internal helper: first mounted storage (for root mounts). */
FM_Storage *FM_Storage_FirstMounted(void)
{
    uint32_t i;
    FM_Storage *found = NULL;

    storage_lock();
    for (i = 0; i < FM_MAX_STORAGES; i++)
    {
        if (g_storages[i] != NULL && g_storages[i]->mounted)
        {
            found = g_storages[i];
            break;
        }
    }
    storage_unlock();
    return found;
}

/* Internal helper used by filesystem layer: update free space hint. */
void FM_Storage_SetFreeSpace(const char *name, uint64_t free)
{
    FM_Storage *s;

    if (Storage_Find(name, &s) == FM_OK)
    {
        s->free_space = free;
    }
}
