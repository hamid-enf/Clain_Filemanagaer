/*
 * FileManager - EEPROM abstraction
 *
 * EEPROMs are byte-addressable devices with implementation-specific
 * page-write and write-cycle behavior.  The adapter splits writes at page
 * boundaries and can also expose the device as an FM_Storage for FMFS.
 */

#ifndef FM_EEPROM_H
#define FM_EEPROM_H

#include "fm_storage.h"

FM_BEGIN_DECLS

typedef FM_Result (*FM_EEPROM_ReadFn)(void *context, uint32_t address,
                                      void *data, uint32_t length);
typedef FM_Result (*FM_EEPROM_WriteFn)(void *context, uint32_t address,
                                       const void *data, uint32_t length);
typedef FM_Result (*FM_EEPROM_EraseFn)(void *context, uint32_t address,
                                       uint32_t length);
typedef FM_Result (*FM_EEPROM_SyncFn)(void *context);

typedef struct
{
    const char          *name;
    uint32_t             capacity;
    uint32_t             page_size;
    void                *context;
    FM_EEPROM_ReadFn     read;
    FM_EEPROM_WriteFn    write;
    FM_EEPROM_EraseFn    erase;       /* optional; fallback writes 0xFF */
    FM_EEPROM_SyncFn     sync;        /* optional */
} FM_EEPROMConfig;

typedef struct
{
    char                 name[FM_NAME_MAX];
    uint32_t             capacity;
    uint32_t             page_size;
    void                *context;
    FM_EEPROM_ReadFn     read;
    FM_EEPROM_WriteFn    write;
    FM_EEPROM_EraseFn    erase;
    FM_EEPROM_SyncFn     sync;
} FM_EEPROM;

FM_Result EEPROM_Init(FM_EEPROM *eeprom, const FM_EEPROMConfig *config);
FM_Result EEPROM_Read(FM_EEPROM *eeprom, uint32_t address,
                      void *data, uint32_t length);
FM_Result EEPROM_Write(FM_EEPROM *eeprom, uint32_t address,
                       const void *data, uint32_t length);
FM_Result EEPROM_Update(FM_EEPROM *eeprom, uint32_t address,
                        const void *data, uint32_t length);
FM_Result EEPROM_Erase(FM_EEPROM *eeprom, uint32_t address,
                       uint32_t length);
FM_Result EEPROM_Sync(FM_EEPROM *eeprom);

/* Make the EEPROM usable by FMFS and the high-level File_* API. */
FM_Result EEPROM_StorageInit(FM_EEPROM *eeprom, FM_Storage *storage,
                             FM_StorageType type);

FM_END_DECLS

#endif /* FM_EEPROM_H */
