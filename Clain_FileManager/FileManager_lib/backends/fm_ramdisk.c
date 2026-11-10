/*
 * FileManager - pointer-backed memory storage
 *
 * This is the canonical RAM/SDRAM/memory-mapped storage adapter.  It is
 * deliberately small: the user owns the memory and the FM_Storage object;
 * the framework only adds bounds checking and the common storage lifecycle.
 *
 * A command-based SD/NOR driver should implement FM_Storage callbacks
 * directly because such devices have alignment, erase and program rules
 * that a raw memcpy adapter cannot infer.
 */

#include "fm_storage.h"
#include <string.h>

#if FM_USE_RAMDISK

typedef struct
{
    uint8_t  *memory;
    uint64_t  capacity;
    uint32_t  block_size;
    FM_Bool   read_only;
} MemoryStorageCtx;

typedef char memory_storage_context_fits[
    (sizeof(MemoryStorageCtx) <= FM_STORAGE_DRIVER_DATA_SIZE) ? 1 : -1];
/* The context is stored in the byte array FM_Storage.reserved_data and
 * cast back to MemoryStorageCtx*; the member offset must keep its
 * pointers naturally aligned. */
typedef char memory_storage_context_aligned[
    ((offsetof(FM_Storage, reserved_data) % 4) == 0) ? 1 : -1];

static MemoryStorageCtx *memory_ctx(FM_Storage *s)
{
    return (MemoryStorageCtx *)(void *)s->reserved_data;
}

static FM_Result memory_range_ok(const MemoryStorageCtx *ctx,
                                 uint64_t offset, uint32_t len)
{
    if (offset > ctx->capacity || (uint64_t)len > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return FM_OK;
}

static FM_Result memory_read(FM_Storage *s, uint64_t offset, void *buf,
                             uint32_t len)
{
    MemoryStorageCtx *ctx = memory_ctx(s);
    FM_Result r;

    if (buf == NULL || len == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = memory_range_ok(ctx, offset, len);
    if (r != FM_OK)
    {
        return r;
    }
    memcpy(buf, ctx->memory + (size_t)offset, len);
    return FM_OK;
}

static FM_Result memory_write(FM_Storage *s, uint64_t offset,
                              const void *buf, uint32_t len)
{
    MemoryStorageCtx *ctx = memory_ctx(s);
    FM_Result r;

    if (buf == NULL || len == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (ctx->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    r = memory_range_ok(ctx, offset, len);
    if (r != FM_OK)
    {
        return r;
    }
    memcpy(ctx->memory + (size_t)offset, buf, len);
    return FM_OK;
}

static FM_Result memory_erase(FM_Storage *s, uint64_t offset, uint32_t len)
{
    MemoryStorageCtx *ctx = memory_ctx(s);
    FM_Result r;

    if (len == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (ctx->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    r = memory_range_ok(ctx, offset, len);
    if (r != FM_OK)
    {
        return r;
    }
    memset(ctx->memory + (size_t)offset, 0xFF, len);
    return FM_OK;
}

static FM_Result memory_sync(FM_Storage *s)
{
    (void)s;
    return FM_OK;
}

static FM_Result memory_mount(FM_Storage *s)
{
    (void)s;
    return FM_OK;
}

static FM_Result memory_unmount(FM_Storage *s)
{
    (void)s;
    return FM_OK;
}

static FM_Result memory_format(FM_Storage *s)
{
    MemoryStorageCtx *ctx = memory_ctx(s);

    if (ctx->read_only)
    {
        return FM_ERR_READ_ONLY;
    }
    memset(ctx->memory, 0xFF, (size_t)ctx->capacity);
    return FM_OK;
}

static FM_Result memory_storage_init(FM_Storage *s, const char *name,
                                     FM_StorageType type, void *address,
                                     uint64_t capacity, uint32_t block_size,
                                     FM_Bool read_only)
{
    MemoryStorageCtx *ctx;
    size_t name_len;

    if (s == NULL || name == NULL || address == NULL || capacity == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (capacity > (uint64_t)SIZE_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    name_len = strlen(name);
    if (name_len == 0 || name_len >= sizeof(s->name))
    {
        return FM_ERR_TOO_LARGE;
    }
    if (block_size == 0)
    {
        block_size = 1;
    }

    (void)Storage_Init(s);
    memcpy(s->name, name, name_len + 1);
    s->type = type;
    s->capacity = capacity;
    s->free_space = capacity;
    s->block_size = block_size;
    s->read_size = block_size;
    s->write_size = block_size;
    s->erase_size = block_size;
    s->read_only = read_only ? FM_TRUE : FM_FALSE;
    s->present = FM_TRUE;
    s->access_mode = read_only ? FM_ACCESS_READ : FM_ACCESS_WRITE;

    s->read = memory_read;
    s->write = memory_write;
    s->erase = memory_erase;
    s->sync = memory_sync;
    s->on_mount = memory_mount;
    s->on_unmount = memory_unmount;
    s->on_format = memory_format;

    ctx = memory_ctx(s);
    memset(ctx, 0, sizeof(*ctx));
    ctx->memory = (uint8_t *)address;
    ctx->capacity = capacity;
    ctx->block_size = block_size;
    ctx->read_only = s->read_only;
    s->user_data = ctx;
    return FM_OK;
}

FM_Result MemoryStorage_InitTyped(FM_Storage *s, const char *name,
                                  FM_StorageType type, void *address,
                                  uint64_t capacity, uint32_t block_size,
                                  FM_Bool read_only)
{
    if (type == FM_STORAGE_UNKNOWN)
    {
        type = FM_STORAGE_RAM;
    }
    return memory_storage_init(s, name, type, address, capacity, block_size,
                               read_only);
}

FM_Result MemoryStorage_Init(FM_Storage *s, const char *name, void *address,
                             uint64_t capacity, uint32_t block_size,
                             FM_Bool read_only)
{
    return MemoryStorage_InitTyped(s, name, FM_STORAGE_RAM, address, capacity,
                                   block_size, read_only);
}

FM_Result RamDisk_Init(FM_Storage *s, const char *name, void *buffer,
                       uint64_t capacity, uint32_t block_size)
{
    return MemoryStorage_Init(s, name, buffer, capacity, block_size,
                              FM_FALSE);
}

FM_Result RamDisk_Register(FM_Storage *s, const char *name, void *buffer,
                           uint64_t capacity)
{
    FM_Result r = RamDisk_Init(s, name, buffer, capacity, 1);

    if (r != FM_OK)
    {
        return r;
    }
    return Storage_Register(s);
}

#endif /* FM_USE_RAMDISK */
