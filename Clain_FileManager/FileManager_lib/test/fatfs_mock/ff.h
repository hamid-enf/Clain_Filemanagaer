/*
 * Minimal FatFS R0.13a-compatible mock for HOST testing of the
 * FileManager FatFS adapter (backends/fm_fatfs.c).
 *
 * This is NOT a real FAT implementation - it simulates the f_* API at the
 * file level with FatFS-accurate semantics (open modes, error codes,
 * "." / ".." from f_readdir, fs_type lifecycle after mount/unmount, ...)
 * so the adapter can be validated (with ASan) without an SD card.
 */

#ifndef FF_MOCK_H
#define FF_MOCK_H

#include <stdint.h>

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef uint32_t LBA_t;      /* R0.13a default (FF_LBA64 == 0) */
typedef uint64_t FSIZE_t;
typedef char     TCHAR;

/* Two header generations are simulated:
 *   default            : STM32Cube middleware style (R0.12c .. R0.13c,
 *                        _FFCONF 68300, 5-argument f_mkfs, no FF_DEFINED)
 *   MOCK_FATFS_MODERN=1: R0.15 style (FF_DEFINED 15100, MKFS_PARM,
 *                        4-argument f_mkfs) */
#ifdef MOCK_FATFS_MODERN
#define FF_DEFINED  15100
#elif defined(MOCK_FATFS_LEGACY)
/* no revision macro at all, R0.12a-style 4-argument f_mkfs */
#else
#define _FATFS      68300
#define _FFCONF     68300
#endif

/* The table of open objects (files and directories together), as the
 * real ffconf.h has it.  16 is what the firmware project ships;
 * -DMOCK_FS_LOCK=2 gives what CubeMX generates. */
#ifndef MOCK_FS_LOCK
#define MOCK_FS_LOCK 16
#endif
#ifdef MOCK_FATFS_MODERN
#define FF_FS_LOCK  MOCK_FS_LOCK
#else
#define _FS_LOCK    MOCK_FS_LOCK
#endif

#define FF_MAX_SS   512
#define FF_MAX_LFN  255
/* One volume, like a CubeMX project with just the SD card.  Build with
 * -DMOCK_FF_VOLUMES=2 for the tests that need a second drive ("1:"). */
#ifdef MOCK_FF_VOLUMES
#define FF_VOLUMES  MOCK_FF_VOLUMES
#else
#define FF_VOLUMES  1
#endif

#ifdef MOCK_FATFS_MODERN
#define FM_FAT      0x01
#define FM_FAT32    0x02
#define FM_EXFAT    0x04
#define FM_ANY      (FM_FAT | FM_FAT32 | FM_EXFAT)
#define FM_SFD      0x08
#endif

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE,
    FR_NO_PATH, FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED, FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

/* File access mode (R0.13a values) */
#define FA_READ             0x01
#define FA_WRITE            0x02
#define FA_OPEN_EXISTING    0x00
#define FA_CREATE_NEW       0x04
#define FA_CREATE_ALWAYS    0x08
#define FA_OPEN_ALWAYS      0x10
#define FA_OPEN_APPEND      0x30

/* File attribute (R0.13a values) */
#define AM_RDO  0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10
#define AM_ARC  0x20

/* FAT type codes */
#define FS_FAT12  1
#define FS_FAT16  2
#define FS_FAT32  3
#define FS_EXFAT  4

typedef struct {
    BYTE  fs_type;      /* 0 = not mounted/valid */
    BYTE  n_fats;
    WORD  n_rootdir;
    WORD  csize;        /* sectors per cluster */
    DWORD n_fatent;     /* number of FAT entries (clusters + 2) */
    DWORD obj_size;
} FATFS;

typedef struct {
    FATFS  *obj_fs;     /* FatFS object this file belongs to */
    int     mo_file;    /* mock file table index */
    FSIZE_t fptr;       /* read/write pointer */
    BYTE    fmode;      /* FA_* flags */
    BYTE    is_open;
} FIL;

typedef struct {
    char   path[256];
    int    vol;         /* the volume it was opened on */
    int    cursor;      /* iteration cursor, -1 = fresh */
    BYTE   is_open;
} DIR;

typedef struct {
    FSIZE_t fsize;
    WORD    fdate;
    WORD    ftime;
    BYTE    fattrib;
    TCHAR   fname[FF_MAX_LFN + 1];
} FILINFO;

FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt);
FRESULT f_open  (FIL* fp, const TCHAR* path, BYTE mode);
FRESULT f_close (FIL* fp);
FRESULT f_read  (FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write (FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);
FRESULT f_truncate (FIL* fp);
FRESULT f_sync   (FIL* fp);
FRESULT f_stat   (const TCHAR* path, FILINFO* fno);
FRESULT f_unlink (const TCHAR* path);
FRESULT f_rename (const TCHAR* path_old, const TCHAR* path_new);
FRESULT f_mkdir  (const TCHAR* path);
FRESULT f_rmdir  (const TCHAR* path);
FRESULT f_opendir(DIR* dp, const TCHAR* path);
FRESULT f_readdir(DIR* dp, FILINFO* fno);
FRESULT f_closedir(DIR* dp);
FRESULT f_getfree(const TCHAR* path, DWORD* nclst, FATFS** fatfs);
#if defined(MOCK_FATFS_MODERN)
typedef struct {
    BYTE fmt;        /* Format option (FM_FAT/FM_FAT32/FM_EXFAT/FM_ANY) */
    BYTE n_fat;      /* Number of FATs */
    BYTE align;      /* Alignment of volume start (2^n) */
    UINT n_root;     /* Number of root directory entries */
    UINT au_size;    /* Cluster size (byte) */
} MKFS_PARM;
FRESULT f_mkfs   (const TCHAR* path, const MKFS_PARM* opt, void* work, UINT len);
#elif defined(MOCK_FATFS_LEGACY)
FRESULT f_mkfs   (const TCHAR* path, BYTE opt, DWORD au, void* work);
#else
FRESULT f_mkfs   (const TCHAR* path, BYTE opt, DWORD au, void* work, UINT len);
#endif

FSIZE_t f_size (FIL* fp);
FSIZE_t f_tell (FIL* fp);

/* mock-only helpers */
void ff_mock_reset(void);
int  ff_mock_file_count(void);
int  ff_mock_file_count_on(int vol);    /* files + directories on a drive */
void ff_mock_fail_rename_temps(int on); /* f_rename to temp -> FR_EXIST */
void ff_mock_fail_mounts(int on);       /* f_mount(fs != NULL) -> FR_DISK_ERR */

#endif /* FF_MOCK_H */
