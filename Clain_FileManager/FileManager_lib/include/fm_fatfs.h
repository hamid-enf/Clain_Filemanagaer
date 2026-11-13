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
 * Who provides disk_read / disk_write / disk_ioctl (FM_FATFS_PROVIDE_DISKIO):
 *   0  the project does (Cube's sd_diskio.c).  This is the default, it
 *      is what FM_SD_DISKIO = CUBEMX in fm_board_config.h means.
 *      FatFS_BindStorage() answers FM_ERR_UNSUPPORTED then: there is
 *      nothing to bind.
 *   1  this adapter does, mapping a FatFS drive number to an FM_Storage
 *      driver bound with FatFS_BindStorage().  Selected by FM_SD_DISKIO =
 *      FILEMANAGER, or by -DFM_FATFS_PROVIDE_DISKIO=1.  The project
 *      On GCC/Clang a weak fallback get_fattime() is provided when FatFS
 *      uses an RTC; the application's strong definition takes precedence.
 *      The fallback is a fixed date, not a clock. Other compilers must
 *      provide their own function unless explicitly enabled below.
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
/* A value given on the command line (-DFM_FATFS_PROVIDE_DISKIO=0/1)
 * stands; it used to be thrown away here.  Without one it follows
 * FM_SD_DISKIO. */
#ifndef FM_FATFS_PROVIDE_DISKIO
#if defined(FM_SD_DISKIO) && (FM_SD_DISKIO == FILEMANAGER)
#define FM_FATFS_PROVIDE_DISKIO 1
#else
#define FM_FATFS_PROVIDE_DISKIO 0
#endif
#endif

/* Override to 0 to require the application's get_fattime, or 1 to supply
 * the fallback even when diskio is external. GCC/Clang use a weak symbol;
 * on other compilers an explicitly enabled fallback is a strong symbol.
 * If the RTC implementation is in a static archive, ensure its object is
 * actually linked: an existing weak definition may not pull it in. */
#ifndef FM_FATFS_PROVIDE_FATTIME
#if defined(__GNUC__) || defined(__clang__)
#define FM_FATFS_PROVIDE_FATTIME FM_FATFS_PROVIDE_DISKIO
#else
#define FM_FATFS_PROVIDE_FATTIME 0
#endif
#endif
/* FAT packed date/time for 2020-01-01 00:00:00; override at build time. */
#ifndef FM_FATFS_FATTIME_VALUE
#define FM_FATFS_FATTIME_VALUE 0x50210000u
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
 * mounted keeps its slot until it is initialised again -- or until all
 * FM_MAX_FS slots are handed out and one more FatFS_Init comes: that
 * one takes over the oldest slot that is not in a mount (such slots
 * used to be lost for good: FM_MAX_FS calls that were never followed by
 * a successful mount, and every later FatFS_Init was
 * FM_ERR_NO_MEMORY).  A mounted volume never loses its slot. */
FM_Result FatFS_Init(FM_FS *fs, uint8_t pdrv);

/* Map a FatFS drive number to an FM_Storage driver.  Call before mounting
 * (or before the first disk access). */
FM_Result FatFS_BindStorage(uint8_t pdrv, FM_Storage *storage);

/* Convenience: init + bind + mount at "/" in one call. */
FM_Result FatFS_MountDefault(FM_FS *fs, uint8_t pdrv, FM_Storage *storage);

#endif /* FM_USE_FATFS */

FM_END_DECLS

#endif /* FM_FATFS_H */
