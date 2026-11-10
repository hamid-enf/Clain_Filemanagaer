/*
 * FileManager - FatFS filesystem adapter
 *
 * Glue between the FileManager FM_FSOps table and the FatFS library that
 * STM32CubeMX generates (Middlewares/Third_Party/FatFs).  This file does
 * NOT contain FatFS itself; add ff.c/ff.h/diskio.h to your build and
 * include paths as usual.
 *
 * Build the adapter with:  -DFM_USE_FATFS=1  (and your FatFS include path)
 *
 * The adapter provides the disk_read/disk_write/disk_ioctl/... bridge that
 * maps a FatFS drive number (pdrv) to an FM_Storage driver.  If you already
 * have your own diskio (e.g. Cube's user_diskio.c), build with
 * -DFM_FATFS_PROVIDE_DISKIO=0 and bind storages with FatFS_BindStorage().
 *
 * Usage:
 *     FM_Storage storage;
 *     FM_FS      fs;
 *
 *     // ... register + mount an SD/eMMC storage driver ...
 *     FatFS_Init(&fs, 0);         // drive 0
 *     FatFS_BindStorage(0, &storage);
 *     FS_MountDefault(&fs);
 *     File_WriteText("/boot.txt", "ok");
 */

#ifndef FM_FATFS_H
#define FM_FATFS_H

#include "fm_types.h"
#include "fm_filesystem.h"

FM_BEGIN_DECLS

#if FM_USE_FATFS

#include "fm_switches.h"
/* CUBEMX (پیش‌فرض): sd_diskio.c کیوب در بیلد می‌ماند؛ diskio نمی‌دهیم.
 * FILEMANAGER: diskio کتابخانه — فقط اگر خودتان sd_diskio را از بیلد
 * بردارید؛ لازم نیست. */
#if defined(FM_SD_DISKIO) && (FM_SD_DISKIO == FILEMANAGER)
#undef FM_FATFS_PROVIDE_DISKIO
#define FM_FATFS_PROVIDE_DISKIO 1
#else
#undef FM_FATFS_PROVIDE_DISKIO
#define FM_FATFS_PROVIDE_DISKIO 0
#endif

/* Initialize the adapter for the given FatFS drive number (0..FF_VOLUMES-1).
 * Must be called before the first FS_Mount of `fs`.
 *
 * The adapter has FM_MAX_FS slots.  FatFS_Init takes one for `fs`; an
 * unmount, and a mount that fails, give it back.  After that the same
 * FM_FS may be mounted again without a new FatFS_Init (card out, card
 * in) for as long as no other FM_FS has been given its slot in between;
 * once that happened FS_Mount answers FM_ERR_NOT_READY and FatFS_Init
 * has to be called again.  An FM_FS that is initialised and then never
 * mounted keeps its slot until it is initialised again. */
FM_Result FatFS_Init(FM_FS *fs, uint8_t pdrv);

/* Map a FatFS drive number to an FM_Storage driver.  Call before mounting
 * (or before the first disk access). */
FM_Result FatFS_BindStorage(uint8_t pdrv, FM_Storage *storage);

/* Convenience: init + bind + mount at "/" in one call. */
FM_Result FatFS_MountDefault(FM_FS *fs, uint8_t pdrv, FM_Storage *storage);

#endif /* FM_USE_FATFS */

FM_END_DECLS

#endif /* FM_FATFS_H */
