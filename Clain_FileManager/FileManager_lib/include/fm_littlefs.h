/*
 * FileManager - LittleFS filesystem adapter
 *
 * Mounts the vendored littlefs (third_party/littlefs) on top of any
 * FM_Storage driver.  All RAM used by the adapter is statically sized; no
 * heap is required by the adapter itself (littlefs is given static buffers
 * for its read/program/lookahead caches and per-file caches).
 *
 * Usage:
 *     FM_Storage storage;
 *     FM_FS      fs;
 *
 *     MemoryStorage_Init(&storage, "QSPI", flash_addr, size, 4096, FM_FALSE);
 *     Storage_Register(&storage);
 *     Storage_Mount("QSPI");
 *
 *     LittleFS_Init(&fs);
 *     FS_MountDefault(&fs);      // auto-formats on first use
 *     File_WriteText("/app/status.txt", "ok");
 */

#ifndef FM_LITTLEFS_H
#define FM_LITTLEFS_H

#include "fm_types.h"
#include "fm_filesystem.h"

FM_BEGIN_DECLS

#if FM_USE_LITTLEFS

/* ------------------------------------------------------------------ */
/* Tuning (override before including this header or with -D flags)     */
/* ------------------------------------------------------------------ */
#ifndef FM_LFS_READ_SIZE
#define FM_LFS_READ_SIZE 16      /* minimum read granularity          */
#endif
#ifndef FM_LFS_PROG_SIZE
#define FM_LFS_PROG_SIZE 16      /* minimum program granularity       */
#endif
#ifndef FM_LFS_CACHE_SIZE
#define FM_LFS_CACHE_SIZE 512    /* read/prog/file cache size (bytes) */
#endif
#ifndef FM_LFS_LOOKAHEAD_SIZE
#define FM_LFS_LOOKAHEAD_SIZE 128 /* block allocator bitmap (bytes)   */
#endif
#ifndef FM_LFS_BLOCK_CYCLES
#define FM_LFS_BLOCK_CYCLES 500  /* wear-leveling erase cycles        */
#endif

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialize the adapter object.  Must be called before FS_Mount. */
FM_Result LittleFS_Init(FM_FS *fs);

/* When auto_format is FM_TRUE (default) an empty or corrupted device is
 * formatted automatically during mount, mirroring FMFS behaviour.  Set to
 * FM_FALSE to return FM_ERR_CORRUPTED instead and let the application call
 * FS_Format() explicitly. */
void LittleFS_SetAutoFormat(FM_FS *fs, FM_Bool auto_format);

#endif /* FM_USE_LITTLEFS */

FM_END_DECLS

#endif /* FM_LITTLEFS_H */
