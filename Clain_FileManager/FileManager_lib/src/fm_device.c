/*
 * FileManager - standard SD/eMMC and Flash device adapters
 */

#include "fm_device.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_DEVICE_ADAPTERS

static FM_Result device_ready(FM_DeviceReadyFn ready, void *context)
{
    FM_Bool is_ready = FM_TRUE;

    if (ready == NULL)
    {
        return FM_OK;
    }
    if (ready(context, &is_ready) != FM_OK)
    {
        return FM_ERR_NOT_READY;
    }
    return is_ready ? FM_OK : FM_ERR_NOT_READY;
}

static FM_Result block_mount(FM_Storage *storage)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;
    FM_Result r = FM_OK;

    if (device->config.init != NULL)
    {
        r = device->config.init(device->config.context);
    }
    if (r == FM_OK)
    {
        r = device_ready(device->config.is_ready, device->config.context);
    }
    if (r == FM_OK)
    {
        device->initialized = FM_TRUE;
    }
    return r;
}

static FM_Result block_unmount(FM_Storage *storage)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;
    FM_Result r = FM_OK;

    if (device->initialized && device->config.deinit != NULL)
    {
        r = device->config.deinit(device->config.context);
    }
    device->initialized = FM_FALSE;
    return r;
}

static FM_Result block_read_bytes(FM_BlockDevice *device, uint64_t offset,
                                  void *data, uint32_t length)
{
    uint8_t *out = (uint8_t *)data;
    uint32_t block_size = device->config.block_size;

    while (length > 0)
    {
        uint64_t lba = offset / block_size;
        uint32_t in_block = (uint32_t)(offset % block_size);
        uint32_t chunk = block_size - in_block;
        FM_Result r;

        if (chunk > length)
        {
            chunk = length;
        }
        if (in_block == 0 && chunk == block_size)
        {
            r = device->config.read_blocks(device->config.context, lba, 1,
                                           out);
        }
        else
        {
            if (block_size > sizeof(device->scratch))
            {
                return FM_ERR_TOO_LARGE;
            }
            r = device->config.read_blocks(device->config.context, lba, 1,
                                           device->scratch);
            if (r == FM_OK)
            {
                memcpy(out, device->scratch + in_block, chunk);
            }
        }
        if (r != FM_OK)
        {
            return r;
        }
        offset += chunk;
        out += chunk;
        length -= chunk;
    }
    return FM_OK;
}

static FM_Result block_write_bytes(FM_BlockDevice *device, uint64_t offset,
                                   const void *data, uint32_t length)
{
    const uint8_t *in = (const uint8_t *)data;
    uint32_t block_size = device->config.block_size;

    while (length > 0)
    {
        uint64_t lba = offset / block_size;
        uint32_t in_block = (uint32_t)(offset % block_size);
        uint32_t chunk = block_size - in_block;
        FM_Result r;

        if (chunk > length)
        {
            chunk = length;
        }
        if (in_block == 0 && chunk == block_size)
        {
            r = device->config.write_blocks(device->config.context, lba, 1,
                                            in);
        }
        else
        {
            if (block_size > sizeof(device->scratch))
            {
                return FM_ERR_TOO_LARGE;
            }
            r = device->config.read_blocks(device->config.context, lba, 1,
                                           device->scratch);
            if (r == FM_OK)
            {
                memcpy(device->scratch + in_block, in, chunk);
                r = device->config.write_blocks(device->config.context, lba, 1,
                                                device->scratch);
            }
        }
        if (r != FM_OK)
        {
            return r;
        }
        offset += chunk;
        in += chunk;
        length -= chunk;
    }
    return FM_OK;
}

static FM_Result block_storage_read(FM_Storage *storage, uint64_t offset,
                                    void *data, uint32_t length)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;

    if (data == NULL || length == 0 || offset > storage->capacity ||
        (uint64_t)length > storage->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return block_read_bytes(device, offset, data, length);
}

static FM_Result block_storage_write(FM_Storage *storage, uint64_t offset,
                                     const void *data, uint32_t length)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;

    if (data == NULL || length == 0 || storage->read_only ||
        offset > storage->capacity ||
        (uint64_t)length > storage->capacity - offset)
    {
        return storage->read_only ? FM_ERR_READ_ONLY :
               FM_ERR_INVALID_ARGUMENT;
    }
    return block_write_bytes(device, offset, data, length);
}

static FM_Result block_storage_erase(FM_Storage *storage, uint64_t offset,
                                     uint32_t length)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;
    uint32_t erase_size = storage->erase_size;

    if (device->config.erase_blocks == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (length == 0 || erase_size == 0 || offset % erase_size != 0 ||
        length % erase_size != 0 || offset > storage->capacity ||
        (uint64_t)length > storage->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return device->config.erase_blocks(device->config.context,
                                       offset / storage->block_size,
                                       length / storage->block_size);
}

static FM_Result block_storage_sync(FM_Storage *storage)
{
    FM_BlockDevice *device = (FM_BlockDevice *)storage->user_data;

    return (device->config.sync == NULL) ? FM_OK :
           device->config.sync(device->config.context);
}

FM_Result BlockDevice_Init(FM_BlockDevice *device,
                           const FM_BlockDeviceConfig *config)
{
    size_t name_length;

    if (device == NULL || config == NULL || config->name == NULL ||
        config->capacity == 0 || config->block_size == 0 ||
        config->capacity % config->block_size != 0 ||
        (config->erase_size != 0 &&
         config->erase_size % config->block_size != 0) ||
        config->read_blocks == NULL ||
        (!config->read_only && config->write_blocks == NULL))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    name_length = strlen(config->name);
    if (name_length == 0 || name_length >= FM_NAME_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(device, 0, sizeof(*device));
    memcpy(device->name, config->name, name_length + 1);
    device->config = *config;
    device->config.name = device->name;
    device->config.read_size = (config->read_size == 0)
                                   ? config->block_size : config->read_size;
    device->config.write_size = (config->write_size == 0)
                                    ? config->block_size : config->write_size;
    device->config.erase_size = (config->erase_size == 0)
                                    ? config->block_size : config->erase_size;
    return FM_OK;
}

FM_Result Storage_InitBlockDevice(FM_Storage *storage,
                                  FM_BlockDevice *device)
{
    FM_StorageConfig config;
    FM_Result r;

    if (storage == NULL || device == NULL || device->config.name == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&config, 0, sizeof(config));
    config.name = device->config.name;
    config.type = device->config.type;
    config.capacity = device->config.capacity;
    config.block_size = device->config.block_size;
    config.read_size = device->config.read_size;
    config.write_size = device->config.write_size;
    config.erase_size = device->config.erase_size;
    config.read_only = device->config.read_only;
    config.present = device->config.present;
    config.user_data = device;
    config.read = block_storage_read;
    config.write = block_storage_write;
    config.erase = block_storage_erase;
    config.sync = block_storage_sync;
    config.on_mount = block_mount;
    config.on_unmount = block_unmount;
    r = Storage_InitDriver(storage, &config);
    if (r != FM_OK)
    {
        return r;
    }
    return FM_OK;
}

static FM_Result flash_mount(FM_Storage *storage)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;
    FM_Result r = FM_OK;

    if (device->config.init != NULL)
    {
        r = device->config.init(device->config.context);
    }
    if (r == FM_OK)
    {
        r = device_ready(device->config.is_ready, device->config.context);
    }
    if (r == FM_OK)
    {
        device->initialized = FM_TRUE;
    }
    return r;
}

static FM_Result flash_unmount(FM_Storage *storage)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;
    FM_Result r = FM_OK;

    if (device->initialized && device->config.deinit != NULL)
    {
        r = device->config.deinit(device->config.context);
    }
    device->initialized = FM_FALSE;
    return r;
}

static FM_Result flash_storage_read(FM_Storage *storage, uint64_t offset,
                                    void *data, uint32_t length)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;

    if (data == NULL || length == 0 || offset > storage->capacity ||
        (uint64_t)length > storage->capacity - offset || offset > UINT32_MAX)
    {
        return (offset > UINT32_MAX) ? FM_ERR_TOO_LARGE :
               FM_ERR_INVALID_ARGUMENT;
    }
    return device->config.read(device->config.context, (uint32_t)offset,
                               data, length);
}

static FM_Result flash_storage_write(FM_Storage *storage, uint64_t offset,
                                     const void *data, uint32_t length)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t page_size = device->config.page_size;

    if (data == NULL || length == 0 || storage->read_only)
    {
        return storage->read_only ? FM_ERR_READ_ONLY : FM_ERR_INVALID_ARGUMENT;
    }
    if (offset > storage->capacity || (uint64_t)length >
        storage->capacity - offset || offset > UINT32_MAX)
    {
        return (offset > UINT32_MAX) ? FM_ERR_TOO_LARGE :
               FM_ERR_INVALID_ARGUMENT;
    }
    while (length > 0)
    {
        uint32_t page_offset = (uint32_t)offset % page_size;
        uint32_t chunk = page_size - page_offset;
        FM_Result r;

        if (chunk > length) chunk = length;
        r = device->config.program(device->config.context, (uint32_t)offset,
                                   bytes, chunk);
        if (r != FM_OK) return r;
        offset += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return FM_OK;
}

static FM_Result flash_storage_erase(FM_Storage *storage, uint64_t offset,
                                     uint32_t length)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;

    if (device->config.erase == NULL || storage->erase_size == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (length == 0 || offset % storage->erase_size != 0 ||
        length % storage->erase_size != 0 || offset > storage->capacity ||
        (uint64_t)length > storage->capacity - offset || offset > UINT32_MAX)
    {
        return (offset > UINT32_MAX) ? FM_ERR_TOO_LARGE :
               FM_ERR_INVALID_ARGUMENT;
    }
    return device->config.erase(device->config.context, (uint32_t)offset,
                                length);
}

static FM_Result flash_storage_sync(FM_Storage *storage)
{
    FM_FlashDevice *device = (FM_FlashDevice *)storage->user_data;

    return (device->config.sync == NULL) ? FM_OK :
           device->config.sync(device->config.context);
}

FM_Result FlashDevice_Init(FM_FlashDevice *device,
                           const FM_FlashDeviceConfig *config)
{
    size_t name_length;

    if (device == NULL || config == NULL || config->name == NULL ||
        config->capacity == 0 || config->capacity > UINT32_MAX ||
        config->page_size == 0 || config->read == NULL ||
        (!config->read_only && config->program == NULL))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (config->erase_size != 0 && config->capacity % config->erase_size != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    name_length = strlen(config->name);
    if (name_length == 0 || name_length >= FM_NAME_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(device, 0, sizeof(*device));
    memcpy(device->name, config->name, name_length + 1);
    device->config = *config;
    device->config.name = device->name;
    if (device->config.erase_size == 0)
    {
        device->config.erase_size = device->config.page_size;
    }
    return FM_OK;
}

FM_Result Storage_InitFlashDevice(FM_Storage *storage,
                                  FM_FlashDevice *device)
{
    FM_StorageConfig config;

    if (storage == NULL || device == NULL || device->config.name == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&config, 0, sizeof(config));
    config.name = device->config.name;
    config.type = device->config.type;
    config.capacity = device->config.capacity;
    config.block_size = 1;
    config.read_size = 1;
    config.write_size = device->config.page_size;
    config.erase_size = device->config.erase_size;
    config.read_only = device->config.read_only;
    config.present = device->config.present;
    config.user_data = device;
    config.read = flash_storage_read;
    config.write = flash_storage_write;
    config.erase = flash_storage_erase;
    config.sync = flash_storage_sync;
    config.on_mount = flash_mount;
    config.on_unmount = flash_unmount;
    return Storage_InitDriver(storage, &config);
}

/* ------------------------------------------------------------------ */
/* NOR flash -> logical block adapter                                  */
/* ------------------------------------------------------------------ */
static FM_Result nor_init(void *context)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;
    FM_Result r = FM_OK;

    if (device->flash.init != NULL)
    {
        r = device->flash.init(device->flash.context);
    }
    if (r == FM_OK)
    {
        r = device_ready(device->flash.is_ready, device->flash.context);
    }
    return r;
}

static FM_Result nor_deinit(void *context)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;

    return (device->flash.deinit == NULL) ? FM_OK :
           device->flash.deinit(device->flash.context);
}

static FM_Result nor_ready(void *context, FM_Bool *ready)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;

    if (ready == NULL) return FM_ERR_INVALID_ARGUMENT;
    if (device->flash.is_ready == NULL)
    {
        *ready = FM_TRUE;
        return FM_OK;
    }
    return device->flash.is_ready(device->flash.context, ready);
}

static FM_Result nor_sync(void *context)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;

    return (device->flash.sync == NULL) ? FM_OK :
           device->flash.sync(device->flash.context);
}

static FM_Result nor_program_range(FM_NorBlockDevice *device,
                                   uint32_t address, const uint8_t *data,
                                   uint32_t length)
{
    while (length > 0)
    {
        uint32_t in_page = address % device->flash.page_size;
        uint32_t chunk = device->flash.page_size - in_page;
        FM_Result r;

        if (chunk > length) chunk = length;
        r = device->flash.program(device->flash.context, address, data, chunk);
        if (r != FM_OK) return r;
        address += chunk;
        data += chunk;
        length -= chunk;
    }
    return FM_OK;
}

static FM_Result nor_read_blocks(void *context, uint64_t lba,
                                 uint32_t block_count, void *data)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;
    uint64_t offset;
    uint64_t length;

    if (data == NULL || block_count == 0 ||
        lba > UINT64_MAX / device->logical_block_size)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    offset = lba * device->logical_block_size;
    length = (uint64_t)block_count * device->logical_block_size;
    if (length > UINT32_MAX || offset > device->flash.capacity ||
        length > device->flash.capacity - offset || offset > UINT32_MAX)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return device->flash.read(device->flash.context, (uint32_t)offset, data,
                              (uint32_t)length);
}

static FM_Result nor_write_blocks(void *context, uint64_t lba,
                                  uint32_t block_count, const void *data)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;
    const uint8_t *input = (const uint8_t *)data;
    uint64_t offset64;
    uint64_t length64;
    uint32_t offset;
    uint32_t remaining;

    if (data == NULL || block_count == 0 || device->flash.read_only ||
        lba > UINT64_MAX / device->logical_block_size)
    {
        return device->flash.read_only ? FM_ERR_READ_ONLY :
               FM_ERR_INVALID_ARGUMENT;
    }
    offset64 = lba * device->logical_block_size;
    length64 = (uint64_t)block_count * device->logical_block_size;
    if (length64 > UINT32_MAX || offset64 > device->flash.capacity ||
        length64 > device->flash.capacity - offset64 || offset64 > UINT32_MAX)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    offset = (uint32_t)offset64;
    remaining = (uint32_t)length64;

    while (remaining > 0)
    {
        uint32_t sector = offset - offset % device->flash.erase_size;
        uint32_t in_sector = offset - sector;
        uint32_t chunk = device->flash.erase_size - in_sector;
        FM_Bool changed = FM_FALSE;
        FM_Bool needs_erase = FM_FALSE;
        uint32_t i;
        FM_Result r;

        if (chunk > remaining) chunk = remaining;
        r = device->flash.read(device->flash.context, sector,
                               device->erase_scratch,
                               device->flash.erase_size);
        if (r != FM_OK) return r;

        for (i = 0; i < chunk; i++)
        {
            uint8_t old_byte = device->erase_scratch[in_sector + i];
            uint8_t new_byte = input[i];

            if (old_byte != new_byte) changed = FM_TRUE;
            if ((old_byte & new_byte) != new_byte) needs_erase = FM_TRUE;
            device->erase_scratch[in_sector + i] = new_byte;
        }
        if (changed && needs_erase)
        {
            uint32_t page;

            r = device->flash.erase(device->flash.context, sector,
                                    device->flash.erase_size);
            if (r != FM_OK) return r;
            for (page = 0; page < device->flash.erase_size;
                 page += device->flash.page_size)
            {
                uint32_t page_length = device->flash.page_size;
                FM_Bool all_erased = FM_TRUE;

                if (page_length > device->flash.erase_size - page)
                {
                    page_length = device->flash.erase_size - page;
                }
                for (i = 0; i < page_length; i++)
                {
                    if (device->erase_scratch[page + i] != 0xFFu)
                    {
                        all_erased = FM_FALSE;
                        break;
                    }
                }
                if (!all_erased)
                {
                    r = nor_program_range(device, sector + page,
                                         device->erase_scratch + page,
                                         page_length);
                    if (r != FM_OK) return r;
                }
            }
        }
        else if (changed)
        {
            r = nor_program_range(device, offset, input, chunk);
            if (r != FM_OK) return r;
        }
        offset += chunk;
        input += chunk;
        remaining -= chunk;
    }
    return FM_OK;
}

static FM_Result nor_erase_blocks(void *context, uint64_t lba,
                                  uint32_t block_count)
{
    FM_NorBlockDevice *device = (FM_NorBlockDevice *)context;
    uint64_t offset;
    uint64_t length;

    if (block_count == 0 || lba > UINT64_MAX / device->logical_block_size)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    offset = lba * device->logical_block_size;
    length = (uint64_t)block_count * device->logical_block_size;
    if (offset > UINT32_MAX || length > UINT32_MAX ||
        offset % device->flash.erase_size != 0 ||
        length % device->flash.erase_size != 0 ||
        offset > device->flash.capacity ||
        length > device->flash.capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return device->flash.erase(device->flash.context, (uint32_t)offset,
                               (uint32_t)length);
}

FM_Result NorBlockDevice_Init(FM_NorBlockDevice *device,
                              const FM_FlashDeviceConfig *flash,
                              uint32_t logical_block_size,
                              void *erase_scratch,
                              size_t erase_scratch_size)
{
    FM_BlockDeviceConfig block;

    if (device == NULL || flash == NULL || flash->name == NULL ||
        flash->capacity == 0 || flash->capacity > UINT32_MAX ||
        flash->page_size == 0 || flash->erase_size == 0 ||
        logical_block_size == 0 ||
        flash->capacity % logical_block_size != 0 ||
        flash->erase_size % logical_block_size != 0 ||
        flash->erase_size % flash->page_size != 0 ||
        erase_scratch == NULL || erase_scratch_size < flash->erase_size ||
        flash->read == NULL || flash->program == NULL || flash->erase == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(device, 0, sizeof(*device));
    device->flash = *flash;
    device->erase_scratch = (uint8_t *)erase_scratch;
    device->erase_scratch_size = erase_scratch_size;
    device->logical_block_size = logical_block_size;

    memset(&block, 0, sizeof(block));
    block.name = flash->name;
    block.type = flash->type;
    block.capacity = flash->capacity;
    block.block_size = logical_block_size;
    block.read_size = logical_block_size;
    block.write_size = logical_block_size;
    block.erase_size = flash->erase_size;
    block.read_only = flash->read_only;
    block.present = flash->present;
    block.context = device;
    block.init = nor_init;
    block.deinit = nor_deinit;
    block.is_ready = nor_ready;
    block.sync = nor_sync;
    block.read_blocks = nor_read_blocks;
    block.write_blocks = nor_write_blocks;
    block.erase_blocks = nor_erase_blocks;
    return BlockDevice_Init(&device->block, &block);
}

FM_Result Storage_InitNorBlockDevice(FM_Storage *storage,
                                     FM_NorBlockDevice *device)
{
    if (storage == NULL || device == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Storage_InitBlockDevice(storage, &device->block);
}

#endif /* FM_USE_DEVICE_ADAPTERS */
