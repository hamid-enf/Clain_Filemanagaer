/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_filesystem.h - Filesystem abstraction (adapter model)
 *
 * A filesystem adapter implements the FM_FSOps function table and is
 * bound to a storage device.  Built-in adapters:
 *
 *   "FMFS"    - tiny flash-style FS on top of any block storage
 *   "POSIX"   - host OS filesystem (desktop testing only)
 *   "FATFS"   - FatFS backend (fm_use_fatfs)
 *   "LITTLEFS"- LittleFS backend (fm_use_littlefs)
 *
 * Mount model:  FS_Mount("/", fs) mounts at the root and binds the
 * first mounted storage;  FS_Mount("/sd", fs) mounts at "/sd" and
 * binds storage "sd" (auto-mounting it if needed).
 */

#ifndef FM_FILESYSTEM_H
#define FM_FILESYSTEM_H

#include "fm_types.h"
#include "fm_storage.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Filesystem info                                                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    char       name[FM_NAME_MAX];      /* adapter name          */
    uint64_t   capacity;               /* total bytes           */
    uint64_t   free_space;             /* free bytes            */
    uint32_t   cluster_size;           /* allocation unit       */
    char       type[16];               /* "FAT32","FMFS",...    */
} FM_FSInfo;

/* ------------------------------------------------------------------ */
/* Adapter function table                                              */
/* ------------------------------------------------------------------ */
typedef struct FM_FS FM_FS;

typedef FM_Result (*FM_FS_MountFn)(FM_FS *fs, FM_Storage *storage);
typedef FM_Result (*FM_FS_UnmountFn)(FM_FS *fs);
typedef FM_Result (*FM_FS_FormatFn)(FM_FS *fs);
typedef FM_Result (*FM_FS_CheckFn)(FM_FS *fs);

typedef FM_Result (*FM_FS_FileOpenFn)(FM_FS *fs, void *fh, const char *path,
                                      uint32_t mode);
typedef FM_Result (*FM_FS_FileCloseFn)(FM_FS *fs, void *fh);
typedef FM_Result (*FM_FS_FileReadFn)(FM_FS *fs, void *fh, void *buf,
                                      uint32_t len, uint32_t *read);
typedef FM_Result (*FM_FS_FileWriteFn)(FM_FS *fs, void *fh, const void *buf,
                                       uint32_t len, uint32_t *written);
typedef FM_Result (*FM_FS_FileSeekFn)(FM_FS *fs, void *fh, int64_t offset,
                                      FM_SeekOrigin origin);
typedef FM_Result (*FM_FS_FileTellFn)(FM_FS *fs, void *fh, uint64_t *pos);
typedef FM_Result (*FM_FS_FileSizeFn)(FM_FS *fs, void *fh, uint64_t *size);
typedef FM_Result (*FM_FS_FileTruncateFn)(FM_FS *fs, void *fh, uint64_t size);
typedef FM_Result (*FM_FS_FileFlushFn)(FM_FS *fs, void *fh);

typedef FM_Result (*FM_FS_StatFn)(FM_FS *fs, const char *path,
                                  FM_FileInfo *info);
typedef FM_Result (*FM_FS_DeleteFn)(FM_FS *fs, const char *path);
typedef FM_Result (*FM_FS_RenameFn)(FM_FS *fs, const char *old_path,
                                    const char *new_path);
typedef FM_Result (*FM_FS_MkdirFn)(FM_FS *fs, const char *path);
typedef FM_Result (*FM_FS_RmdirFn)(FM_FS *fs, const char *path);

typedef FM_Result (*FM_FS_DirOpenFn)(FM_FS *fs, void *dh, const char *path);
typedef FM_Result (*FM_FS_DirCloseFn)(FM_FS *fs, void *dh);
typedef FM_Result (*FM_FS_DirReadFn)(FM_FS *fs, void *dh, FM_DirEntry *entry);

typedef FM_Result (*FM_FS_GetFreeFn)(FM_FS *fs, uint64_t *free_bytes,
                                     uint64_t *total_bytes);
typedef FM_Result (*FM_FS_GetTypeFn)(FM_FS *fs, char *type, size_t type_size);

struct FM_FSOps
{
    FM_FS_MountFn        mount;
    FM_FS_UnmountFn      unmount;
    FM_FS_FormatFn       format;
    FM_FS_CheckFn        check;
    FM_FS_FileOpenFn     file_open;
    FM_FS_FileCloseFn    file_close;
    FM_FS_FileReadFn     file_read;
    FM_FS_FileWriteFn    file_write;
    FM_FS_FileSeekFn     file_seek;
    FM_FS_FileTellFn     file_tell;
    FM_FS_FileSizeFn     file_size;
    FM_FS_FileTruncateFn file_truncate;
    FM_FS_FileFlushFn    file_flush;
    FM_FS_StatFn         stat;
    FM_FS_DeleteFn       delete_file;   /* files only */
    FM_FS_RenameFn       rename;
    FM_FS_MkdirFn        mkdir;
    FM_FS_RmdirFn        rmdir;         /* empty dirs only */
    FM_FS_DirOpenFn      dir_open;
    FM_FS_DirCloseFn     dir_close;
    FM_FS_DirReadFn      dir_read;
    FM_FS_GetFreeFn      get_free;
    FM_FS_GetTypeFn      get_type;
};

struct FM_FS
{
    char            name[FM_NAME_MAX];   /* adapter name        */
    struct FM_FSOps ops;
    void           *user;                /* adapter private     */
    FM_Storage     *storage;             /* bound storage       */
    FM_Bool         mounted;
    struct FM_FS   *next;                /* internal registry   */
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */
FM_Result FS_Register(FM_FS *fs);
FM_Result FS_Unregister(const char *name);
FM_Result FS_Find(const char *name, FM_FS **out);

/* ------------------------------------------------------------------ */
/* Mount management                                                    */
/* ------------------------------------------------------------------ */
FM_Result FS_Mount(const char *mount_path, FM_FS *fs);
FM_Result FS_Unmount(const char *mount_path);
FM_Result FS_Format(const char *mount_path);
FM_Result FS_Check(const char *mount_path);
FM_Result FS_GetInfo(const char *mount_path, FM_FSInfo *info);
FM_Result FS_GetCapacity(const char *mount_path, uint64_t *capacity);
FM_Result FS_GetFreeSpace(const char *mount_path, uint64_t *free);
FM_Result FS_IsMounted(const char *mount_path, FM_Bool *mounted);

/* Default root mount convenience: mount fs at "/" using the first
 * mounted storage.  Returns FM_ERR_NOT_READY if no storage mounted. */
FM_Result FS_MountDefault(FM_FS *fs);

/* Host adapter entry point (available when FM_USE_POSIX is enabled). */
FM_Result PosixFS_Init(FM_FS *fs);

/* Built-in small filesystem for pointer-backed/flash storage.  It keeps
 * metadata in the storage itself and uses the storage callback table for
 * data, so the same high-level File_* API works on RAM, SDRAM, NOR/QSPI
 * adapters and custom SD drivers. */
FM_Result FMFS_Init(FM_FS *fs);

/* Compact the FMFS data area: relocate live files contiguously from the
 * metadata region, recovering space consumed by deleted files.  All open
 * file handles on this FS become invalid after a successful repack. */
FM_Result FMFS_Repack(FM_FS *fs);

FM_END_DECLS

#endif /* FM_FILESYSTEM_H */
