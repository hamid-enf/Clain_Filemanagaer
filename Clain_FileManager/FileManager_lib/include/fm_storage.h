/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_storage.h - Storage (block device) abstraction
 *
 * A "storage" is a raw memory/block device: SD card, QSPI flash,
 * SDRAM, RAM disk, eMMC, ...  Storages are NOT filesystems.  A
 * filesystem (see fm_filesystem.h) is mounted on top of a storage.
 *
 * RAM/SDRAM is represented the same way: register a RAM disk storage
 * and mount a filesystem (e.g. the built-in FMFS) on it.
 *
 * Registration model (no allocation): the user owns a static
 * FM_Storage object, fills it (initializer or Storage_Init), and
 * registers it with Storage_Register().  The core keeps a small
 * pointer table.
 */

#ifndef FM_STORAGE_H
#define FM_STORAGE_H

#include "fm_types.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_STORAGE_UNKNOWN       = 0,
    FM_STORAGE_RAM           = 1,
    FM_STORAGE_SD            = 2,
    FM_STORAGE_EMMC          = 3,
    FM_STORAGE_NAND          = 4,
    FM_STORAGE_NOR           = 5,
    FM_STORAGE_QSPI          = 6,
    FM_STORAGE_SDRAM         = 7,
    FM_STORAGE_INTERNAL_FLASH= 8,
    FM_STORAGE_USB_MSC       = 9,
    FM_STORAGE_CUSTOM        = 255
} FM_StorageType;

typedef enum
{
    FM_ACCESS_NONE  = 0,
    FM_ACCESS_READ  = 1,
    FM_ACCESS_WRITE = 2
} FM_AccessMode;

typedef struct FM_Storage FM_Storage;

typedef FM_Result (*FM_Storage_ReadFn)(FM_Storage *s, uint64_t offset,
                                       void *buf, uint32_t len);
typedef FM_Result (*FM_Storage_WriteFn)(FM_Storage *s, uint64_t offset,
                                        const void *buf, uint32_t len);
typedef FM_Result (*FM_Storage_EraseFn)(FM_Storage *s, uint64_t offset,
                                        uint32_t len);
typedef FM_Result (*FM_Storage_SyncFn)(FM_Storage *s);

struct FM_Storage
{
    char               name[FM_NAME_MAX];  /* e.g. "SD", "RAM", "QSPI" */
    FM_StorageType     type;
    uint64_t           capacity;           /* total bytes              */
    uint64_t           free_space;         /* cached free bytes        */
    uint32_t           block_size;         /* logical block size       */
    uint32_t           read_size;          /* optimal read granularity */
    uint32_t           write_size;         /* optimal write granularity*/
    uint32_t           erase_size;         /* erase unit (0 = n/a)     */
    uint8_t            read_only;
    uint8_t            present;            /* hardware present flag    */
    uint8_t            mounted;            /* set by Storage_Mount     */
    FM_AccessMode      access_mode;
    void              *user_data;          /* optional driver data     */
    uint8_t            reserved_data[FM_STORAGE_DRIVER_DATA_SIZE];

    FM_Storage_ReadFn  read;
    FM_Storage_WriteFn write;
    FM_Storage_EraseFn erase;              /* may be NULL              */
    FM_Storage_SyncFn  sync;               /* may be NULL              */

    /* Optional driver hooks, may be NULL */
    FM_Result (*on_mount)(FM_Storage *s);
    FM_Result (*on_unmount)(FM_Storage *s);
    FM_Result (*on_format)(FM_Storage *s);

    struct FM_Storage  *next;              /* internal registry link   */
};

typedef struct
{
    char          name[FM_NAME_MAX];
    FM_StorageType type;
    uint64_t      capacity;
    uint64_t      free_space;      /* FM_ERR_UNSUPPORTED if unknown */
    uint32_t      block_size;
    uint32_t      read_size;
    uint32_t      write_size;
    uint32_t      erase_size;
    uint8_t       read_only;
    uint8_t       present;
    uint8_t       mounted;
    FM_AccessMode access_mode;
} FM_StorageInfo;

typedef void (*FM_StorageEventCb)(const char *name, FM_Bool inserted,
                                  void *user_data);

/* Compact driver description used by Storage_InitDriver.  The caller
 * still owns all callback state and the FM_Storage object. */
typedef struct
{
    const char            *name;
    FM_StorageType         type;
    uint64_t               capacity;
    uint64_t               free_space;
    uint32_t               block_size;
    uint32_t               read_size;
    uint32_t               write_size;
    uint32_t               erase_size;
    FM_Bool                read_only;
    FM_Bool                present;
    void                  *user_data;
    FM_Storage_ReadFn      read;
    FM_Storage_WriteFn     write;
    FM_Storage_EraseFn     erase;
    FM_Storage_SyncFn      sync;
    FM_Result (*on_mount)(FM_Storage *s);
    FM_Result (*on_unmount)(FM_Storage *s);
    FM_Result (*on_format)(FM_Storage *s);
} FM_StorageConfig;

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
FM_Result Storage_Init(FM_Storage *s);   /* zero + defaults */
FM_Result Storage_InitDriver(FM_Storage *s, const FM_StorageConfig *config);
FM_Result Storage_Register(FM_Storage *s);
FM_Result Storage_Unregister(const char *name);
FM_Result Storage_Find(const char *name, FM_Storage **out);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
FM_Result Storage_Mount(const char *name);
FM_Result Storage_Unmount(const char *name);
FM_Result Storage_Format(const char *name);  /* driver-level format */
FM_Result Storage_GetInfo(const char *name, FM_StorageInfo *info);
FM_Result Storage_IsReady(const char *name, FM_Bool *ready);
FM_Result Storage_GetCapacity(const char *name, uint64_t *capacity);
FM_Result Storage_GetFreeSpace(const char *name, uint64_t *free);

/* ------------------------------------------------------------------ */
/* Hot plug                                                            */
/* ------------------------------------------------------------------ */
FM_Result Storage_Insert(const char *name);
FM_Result Storage_Remove(const char *name);
FM_Result Storage_IsPresent(const char *name, FM_Bool *present);
FM_Result Storage_SetEventCallback(FM_StorageEventCb cb, void *user_data);

/* Raw block access (advanced use; bypasses filesystem) */
FM_Result Storage_ReadBlocks(const char *name, uint64_t offset, void *buf,
                             uint32_t len);
FM_Result Storage_WriteBlocks(const char *name, uint64_t offset,
                              const void *buf, uint32_t len);
FM_Result Storage_EraseBlocks(const char *name, uint64_t offset, uint32_t len);
FM_Result Storage_Sync(const char *name);

/* Helpers for driver authors */
FM_Result Storage_SetPresent(FM_Storage *s, FM_Bool present);

/* ------------------------------------------------------------------ */
/* Ready-made pointer-backed storage                                   */
/* ------------------------------------------------------------------ */
/*
 * These helpers expose a byte-addressable memory region as a storage
 * device.  They are useful for RAM, SDRAM, memory-mapped NOR/QSPI and
 * host test buffers.  The address must remain valid for the lifetime of
 * the storage object.  Real SD cards or command-based flash devices
 * should use the callback table above so erase/program rules stay in
 * their driver.
 */
FM_Result MemoryStorage_InitTyped(FM_Storage *s, const char *name,
                                  FM_StorageType type, void *address,
                                  uint64_t capacity, uint32_t block_size,
                                  FM_Bool read_only);
FM_Result MemoryStorage_Init(FM_Storage *s, const char *name, void *address,
                             uint64_t capacity, uint32_t block_size,
                             FM_Bool read_only);
FM_Result RamDisk_Init(FM_Storage *s, const char *name, void *buffer,
                       uint64_t capacity, uint32_t block_size);
FM_Result RamDisk_Register(FM_Storage *s, const char *name, void *buffer,
                           uint64_t capacity);

FM_END_DECLS

#endif /* FM_STORAGE_H */
