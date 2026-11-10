/*
 * FileManager - standard hardware-device adapters
 *
 * SD/eMMC/USB block devices and NOR/QSPI/internal Flash have different
 * vendor HALs, but their storage contracts are stable.  These structures
 * provide that stable boundary and translate block/page operations into
 * FM_Storage callbacks used by FMFS and the high-level File_* API.
 */

#ifndef FM_DEVICE_H
#define FM_DEVICE_H

#include "fm_storage.h"

FM_BEGIN_DECLS

typedef FM_Result (*FM_DeviceInitFn)(void *context);
typedef FM_Result (*FM_DeviceDeinitFn)(void *context);
typedef FM_Result (*FM_DeviceReadyFn)(void *context, FM_Bool *ready);
typedef FM_Result (*FM_DeviceSyncFn)(void *context);

/* ------------------------------------------------------------------ */
/* SD / eMMC / block-device contract                                  */
/* ------------------------------------------------------------------ */
typedef FM_Result (*FM_BlockReadFn)(void *context, uint64_t lba,
                                    uint32_t block_count, void *data);
typedef FM_Result (*FM_BlockWriteFn)(void *context, uint64_t lba,
                                     uint32_t block_count, const void *data);
typedef FM_Result (*FM_BlockEraseFn)(void *context, uint64_t lba,
                                     uint32_t block_count);

typedef struct
{
    const char        *name;
    FM_StorageType     type;          /* SD, eMMC, USB MSC, ... */
    uint64_t           capacity;      /* bytes                   */
    uint32_t           block_size;    /* normally 512            */
    uint32_t           read_size;
    uint32_t           write_size;
    uint32_t           erase_size;
    FM_Bool            read_only;
    FM_Bool            present;
    void              *context;
    FM_DeviceInitFn    init;
    FM_DeviceDeinitFn  deinit;
    FM_DeviceReadyFn   is_ready;
    FM_DeviceSyncFn    sync;
    FM_BlockReadFn     read_blocks;
    FM_BlockWriteFn    write_blocks;
    FM_BlockEraseFn    erase_blocks;
} FM_BlockDeviceConfig;

typedef struct
{
    char                  name[FM_NAME_MAX];
    FM_BlockDeviceConfig  config;
    uint8_t               scratch[FM_INTERNAL_BUFFER_SIZE];
    FM_Bool               initialized;
} FM_BlockDevice;

FM_Result BlockDevice_Init(FM_BlockDevice *device,
                           const FM_BlockDeviceConfig *config);
FM_Result Storage_InitBlockDevice(FM_Storage *storage,
                                  FM_BlockDevice *device);

/* ------------------------------------------------------------------ */
/* NOR / QSPI / internal Flash contract                                */
/* ------------------------------------------------------------------ */
typedef FM_Result (*FM_FlashReadFn)(void *context, uint32_t address,
                                    void *data, uint32_t length);
typedef FM_Result (*FM_FlashProgramFn)(void *context, uint32_t address,
                                       const void *data, uint32_t length);
typedef FM_Result (*FM_FlashEraseFn)(void *context, uint32_t address,
                                     uint32_t length);

typedef struct
{
    const char        *name;
    FM_StorageType     type;          /* NOR, QSPI, internal flash */
    uint64_t           capacity;      /* bytes                     */
    uint32_t           page_size;
    uint32_t           erase_size;
    FM_Bool            read_only;
    FM_Bool            present;
    void              *context;
    FM_DeviceInitFn    init;
    FM_DeviceDeinitFn  deinit;
    FM_DeviceReadyFn   is_ready;
    FM_DeviceSyncFn    sync;
    FM_FlashReadFn     read;
    FM_FlashProgramFn  program;
    FM_FlashEraseFn    erase;
} FM_FlashDeviceConfig;

typedef struct
{
    char                  name[FM_NAME_MAX];
    FM_FlashDeviceConfig  config;
    FM_Bool               initialized;
} FM_FlashDevice;

FM_Result FlashDevice_Init(FM_FlashDevice *device,
                           const FM_FlashDeviceConfig *config);
FM_Result Storage_InitFlashDevice(FM_Storage *storage,
                                  FM_FlashDevice *device);

/* ------------------------------------------------------------------ */
/* NOR flash -> logical block adapter (FatFS / USB MSC)               */
/* ------------------------------------------------------------------ */
/* Raw NOR cannot safely accept arbitrary 512-byte rewrites because bits only
 * program 1->0 and erase operates on a larger sector. This adapter performs
 * read-modify-erase-program with a caller-owned erase-sector scratch buffer.
 * It is intentionally predictable (one erase_size buffer, no heap). For high
 * write-rate products use a dedicated FTL; this adapter is aimed at embedded
 * configuration/media volumes and examples. */
typedef struct
{
    FM_FlashDeviceConfig flash;
    FM_BlockDevice       block;
    uint8_t             *erase_scratch;
    size_t               erase_scratch_size;
    uint32_t             logical_block_size;
} FM_NorBlockDevice;

FM_Result NorBlockDevice_Init(FM_NorBlockDevice *device,
                              const FM_FlashDeviceConfig *flash,
                              uint32_t logical_block_size,
                              void *erase_scratch,
                              size_t erase_scratch_size);
FM_Result Storage_InitNorBlockDevice(FM_Storage *storage,
                                     FM_NorBlockDevice *device);

FM_END_DECLS

#endif /* FM_DEVICE_H */
