/*
 * fm_sd_hal.h — diskio استاندارد روی HAL_SD (بدون کپی sd_diskio.c کیوب)
 *
 * با FM_SD_DISKIO FILEMANAGER در fm_board_config.h:
 *   - کتابخانه disk_read/write را می‌دهد (bounce کش داخل fm_fatfs.c)
 *   - این فایل فقط HAL_SD را به FM_Storage وصل می‌کند
 *
 * در Cube فقط FATFS/Target/sd_diskio.c را Exclude کنید.
 * فایل کیوب را ویرایش یا جایگزین نکنید.
 *
 *     MX_SDMMC1_SD_Init();
 *     FileManager_Init();
 *     FM_SD_Hal_Attach(&hsd1);
 *     SdCard_Begin();   // FatFS_Init + FS_Mount
 */
#ifndef FM_SD_HAL_H
#define FM_SD_HAL_H

#include "fm_types.h"
#include "fm_storage.h"

FM_BEGIN_DECLS

FM_Result FM_SD_Hal_Attach(void *sd_handle);
FM_Storage *FM_SD_Hal_Storage(void);

FM_END_DECLS

#endif
