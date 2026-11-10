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
#if FM_USE_AUDIO
#include "fm_audio.h"
#endif
#if FM_USE_VIDEO
#include "fm_video.h"
#endif
#if FM_USE_H264
#include "fm_h264.h"
#endif
#if FM_USE_MJPEG
#include "fm_mjpeg.h"
#endif
#include "fm_hw.h"
#if FM_USE_AUDIO || FM_USE_VIDEO
#include "fm_player.h"
#endif
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
