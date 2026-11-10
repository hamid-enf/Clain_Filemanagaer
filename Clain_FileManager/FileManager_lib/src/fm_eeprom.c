/*
 * FileManager - EEPROM byte-storage adapter
 */

#define FM_DEBUG_TAG "eeprom"
#include "fm_debug.h"
#include "fm_eeprom.h"
#include "fm_internal.h"
#include <string.h>

#if FM_USE_EEPROM

static FM_Result eeprom_range(const FM_EEPROM *eeprom, uint32_t address,
                              uint32_t length)
{
    if (address > eeprom->capacity || length > eeprom->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return FM_OK;
}

FM_Result EEPROM_Init(FM_EEPROM *eeprom, const FM_EEPROMConfig *config)
{
    size_t name_length;

    if (eeprom == NULL || config == NULL || config->name == NULL ||
        config->capacity == 0 || config->page_size == 0 ||
        config->read == NULL || config->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    name_length = strlen(config->name);
    if (name_length == 0 || name_length >= sizeof(eeprom->name))
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(eeprom, 0, sizeof(*eeprom));
    memcpy(eeprom->name, config->name, name_length + 1);
    eeprom->capacity = config->capacity;
    eeprom->page_size = config->page_size;
    eeprom->context = config->context;
    eeprom->read = config->read;
    eeprom->write = config->write;
    eeprom->erase = config->erase;
    eeprom->sync = config->sync;
    return FM_OK;
}

FM_Result EEPROM_Read(FM_EEPROM *eeprom, uint32_t address,
                      void *data, uint32_t length)
{
    FM_Result r;

    if (eeprom == NULL || data == NULL || length == 0 || eeprom->read == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = eeprom_range(eeprom, address, length);
    if (r != FM_OK)
    {
        return r;
    }
    return eeprom->read(eeprom->context, address, data, length);
}

FM_Result EEPROM_Write(FM_EEPROM *eeprom, uint32_t address,
                       const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    FM_Result r;

    if (eeprom == NULL || data == NULL || length == 0 ||
        eeprom->write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = eeprom_range(eeprom, address, length);
    if (r != FM_OK)
    {
        return r;
    }
    while (length > 0)
    {
        uint32_t page_offset = address % eeprom->page_size;
        uint32_t page_remaining = eeprom->page_size - page_offset;
        uint32_t chunk = (length < page_remaining) ? length : page_remaining;

        r = eeprom->write(eeprom->context, address, bytes, chunk);
        if (r != FM_OK)
        {
            return r;
        }
        address += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return FM_OK;
}

FM_Result EEPROM_Update(FM_EEPROM *eeprom, uint32_t address,
                        const void *data, uint32_t length)
{
    const uint8_t *new_bytes = (const uint8_t *)data;
    uint8_t old_bytes[FM_INTERNAL_BUFFER_SIZE];
    FM_Result r;

    if (eeprom == NULL || data == NULL || length == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = eeprom_range(eeprom, address, length);
    if (r != FM_OK)
    {
        return r;
    }
    while (length > 0)
    {
        uint32_t chunk = (length < sizeof(old_bytes))
                             ? length : (uint32_t)sizeof(old_bytes);
        uint32_t i;
        FM_Bool changed = FM_FALSE;

        r = EEPROM_Read(eeprom, address, old_bytes, chunk);
        if (r != FM_OK)
        {
            return r;
        }
        for (i = 0; i < chunk; i++)
        {
            if (old_bytes[i] != new_bytes[i])
            {
                changed = FM_TRUE;
                break;
            }
        }
        if (changed)
        {
            r = EEPROM_Write(eeprom, address, new_bytes, chunk);
            if (r != FM_OK)
            {
                return r;
            }
        }
        address += chunk;
        new_bytes += chunk;
        length -= chunk;
    }
    return FM_OK;
}

FM_Result EEPROM_Erase(FM_EEPROM *eeprom, uint32_t address,
                       uint32_t length)
{
    uint8_t erased[FM_INTERNAL_BUFFER_SIZE];
    FM_Result r;

    if (eeprom == NULL || length == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = eeprom_range(eeprom, address, length);
    if (r != FM_OK)
    {
        return r;
    }
    if (eeprom->erase != NULL)
    {
        return eeprom->erase(eeprom->context, address, length);
    }
    memset(erased, 0xFF, sizeof(erased));
    while (length > 0)
    {
        uint32_t chunk = (length < sizeof(erased))
                             ? length : (uint32_t)sizeof(erased);

        r = EEPROM_Write(eeprom, address, erased, chunk);
        if (r != FM_OK)
        {
            return r;
        }
        address += chunk;
        length -= chunk;
    }
    return FM_OK;
}

FM_Result EEPROM_Sync(FM_EEPROM *eeprom)
{
    if (eeprom == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return (eeprom->sync == NULL) ? FM_OK :
           eeprom->sync(eeprom->context);
}

static FM_Result eeprom_storage_read(FM_Storage *storage, uint64_t offset,
                                      void *data, uint32_t length)
{
    if (offset > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    return EEPROM_Read((FM_EEPROM *)storage->user_data, (uint32_t)offset,
                       data, length);
}

static FM_Result eeprom_storage_write(FM_Storage *storage, uint64_t offset,
                                       const void *data, uint32_t length)
{
    if (offset > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    return EEPROM_Write((FM_EEPROM *)storage->user_data, (uint32_t)offset,
                        data, length);
}

static FM_Result eeprom_storage_erase(FM_Storage *storage, uint64_t offset,
                                       uint32_t length)
{
    if (offset > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    return EEPROM_Erase((FM_EEPROM *)storage->user_data, (uint32_t)offset,
                        length);
}

static FM_Result eeprom_storage_sync(FM_Storage *storage)
{
    return EEPROM_Sync((FM_EEPROM *)storage->user_data);
}

FM_Result EEPROM_StorageInit(FM_EEPROM *eeprom, FM_Storage *storage,
                             FM_StorageType type)
{
    FM_StorageConfig config;

    if (eeprom == NULL || storage == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&config, 0, sizeof(config));
    config.name = eeprom->name;
    config.type = type;
    config.capacity = eeprom->capacity;
    config.block_size = 1;
    config.read_size = 1;
    config.write_size = eeprom->page_size;
    config.erase_size = eeprom->page_size;
    config.present = FM_TRUE;
    config.user_data = eeprom;
    config.read = eeprom_storage_read;
    config.write = eeprom_storage_write;
    config.erase = eeprom_storage_erase;
    config.sync = eeprom_storage_sync;
    return Storage_InitDriver(storage, &config);
}

#endif /* FM_USE_EEPROM */
