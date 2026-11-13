/*
 * FileManager - Unified Embedded File & Storage Framework
 * file_manager.h - Master include (Simple API front door)
 *
 * #include "file_manager.h"
 *
 *   FileManager_Init();
 *   Storage_Mount("SD");
 *   FS_MountDefault(&fatfs_fs);
 *   File_WriteText("/hello.txt", "Hello STM32!");
 *
 *   char buffer[128];
 *   File_ReadText("/hello.txt", buffer, sizeof(buffer));
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "fm_config.h"
#include "fm_types.h"
#include "fm_sync.h"
#include "fm_alloc.h"
#include "fm_path.h"
#include "fm_storage.h"
#include "fm_filesystem.h"
#include "fm_file.h"
#include "fm_directory.h"
#if FM_USE_STREAM || FM_USE_BINARY || FM_USE_IMAGE || FM_USE_VIDEO
#include "fm_stream.h"
#endif

#if FM_USE_CRC
#include "fm_crc.h"
#endif
#if FM_USE_TEXT
#include "fm_text.h"
#endif
#if FM_USE_BINARY
#include "fm_binary.h"
#endif
#if FM_USE_JSON
#include "fm_json.h"
#endif
#if FM_USE_DATA_LOGGER
#include "fm_logger.h"
#endif
#if FM_USE_EXPLORER
#include "fm_explorer.h"
#endif
#if FM_USE_MEDIA_DETECT
#include "fm_media.h"
#endif
#if FM_USE_SAFE_WRITE
#include "fm_safe.h"
#endif
#if FM_USE_ASYNC
#include "fm_async.h"
#endif
#if FM_USE_IMAGE
#include "fm_image.h"
#endif
/* Audio, video and the player are declared whatever the switches say:
 * with a subsystem compiled out its functions are still defined (they
 * answer FM_ERR_UNSUPPORTED), so code that calls them needs the
 * prototypes and the types too. */
#include "fm_audio.h"
#include "fm_video.h"
#if FM_USE_H264
#include "fm_h264.h"
#endif
#if FM_USE_MJPEG
#include "fm_mjpeg.h"
#endif
#include "fm_hw.h"
#include "fm_player.h"
#if FM_USE_EEPROM
#include "fm_eeprom.h"
#endif
#if FM_USE_USB_MSC
#include "fm_usb_msc.h"
#endif
#if FM_USE_DEVICE_ADAPTERS
#include "fm_device.h"
#endif
#if FM_USE_FATFS
#include "fm_fatfs.h"
#include "fm_sd_hal.h"
#endif
#if FM_USE_LITTLEFS
#include "fm_littlefs.h"
#endif

FM_BEGIN_DECLS

/* Initialize / deinitialize the framework (registries, locks). */
FM_Result FileManager_Init(void);
void      FileManager_Deinit(void);

/* FileManager_Init() with a check that the caller and the library were
 * compiled with the same options.  FM_File, FM_Dir, FM_Storage, FM_FS
 * and FM_DirEntry are allocated by the application and filled in by the
 * library, and their sizes follow the configuration (FM_USE_THREADS,
 * FM_PATH_MAX, FM_NAME_MAX, the buffer sizes...).  A libfilemanager.a
 * built with one set of -D options and linked to an application built
 * with another reads and writes past the end of those objects, silently.
 * With different sizes this returns FM_ERR_INTERNAL, prints which type
 * differs (FM_DEBUG_LEVEL >= 1) and initializes nothing.
 *
 * What it does not see: an option that changes none of these five.  The
 * objects of the optional modules (FM_Logger, FM_Json, FM_Video, ...)
 * are not compared one by one; they follow the five through FM_File,
 * FM_PATH_MAX and FM_NAME_MAX, but an option of their own
 * (FM_LOGGER_BUFFER_SIZE in FM_Logger, for one) can differ unnoticed.
 * The rule stays: one set of -D options for the library and for
 * everything that includes its headers.
 *
 * FileManager_Init() in application code IS this call: the macro below
 * passes the sizes the application sees.  -DFM_NO_ABI_CHECK=1 removes
 * the macro (for code that needs the plain function, e.g. takes its
 * address; `(FileManager_Init)()` calls it as well). */
FM_Result FileManager_InitChecked(size_t file_size, size_t dir_size,
                                  size_t storage_size, size_t fs_size,
                                  size_t entry_size);
#if !defined(FM_NO_ABI_CHECK) || !FM_NO_ABI_CHECK
#define FileManager_Init() \
    FileManager_InitChecked(sizeof(FM_File), sizeof(FM_Dir), \
                            sizeof(FM_Storage), sizeof(FM_FS), \
                            sizeof(FM_DirEntry))
#endif

/* Optional super-loop hook.  Image/PNG/JPEG decode calls FM_Idle()
 * between scanlines so I2S audio can refill instead of underrunning.
 * SimpleAudio_Begin installs SimpleAudio_Poll here.  Re-entrant calls
 * are ignored.  Pass NULL to clear. */
void FM_SetIdle(void (*fn)(void));
void FM_Idle(void);

/* Pause a real-time sink (I2S) around a long blocking call such as an
 * SPI TFT blit, then resume.  SimpleAudio_Begin installs Pause/Resume.
 * Nested enter/leave are counted.  No-ops if no hooks are registered. */
void FM_SetBusyHooks(void (*enter)(void), void (*leave)(void));
void FM_BusyEnter(void);
void FM_BusyLeave(void);

/* Version information */
uint32_t  FileManager_GetVersion(void);   /* 0xMMNNPP */
const char *FileManager_GetVersionString(void);

/* The debug level the LIBRARY was compiled with (0 off, 1 low, 2 medium,
 * 3 high; see fm_debug.h).  FM_DEBUG_LEVEL in an application file only
 * says what that file was compiled with; when the library is built
 * separately (a static library, another build configuration) the two can
 * differ, and this is the one that decides what the library prints. */
int FileManager_GetDebugLevel(void);

/* One-call setup for the common pointer-backed case.  After this call
 * the normal high-level file and directory APIs operate at "/" without
 * exposing storage blocks, filesystem handles or mount details. */
FM_Result FileManager_MountMemory(FM_Storage *storage, FM_FS *fs,
                                  const char *name, void *address,
                                  uint64_t capacity, uint32_t block_size,
                                  FM_Bool read_only);
/* Register, mount and attach the built-in FMFS to an already initialized
 * custom storage driver. */
FM_Result FileManager_MountStorage(FM_Storage *storage, FM_FS *fs);

FM_END_DECLS

#endif /* FILE_MANAGER_H */
