/*
 * diskio SD — بدون فورک sd_diskio.c کیوب
 *
 * در fm_board_config.h:
 *
 *     #define FM_SD_DISKIO FILEMANAGER
 *
 * در Cube: FATFS/Target/sd_diskio.c → Exclude from Build
 * در main، بعد از MX_SDMMC1_SD_Init و FileManager_Init:
 *
 *     FM_SD_Hal_Attach(&hsd1);
 *
 * فایل کیوب را کپی یا ویرایش نکنید؛ روی F4/F7/H7 همان HAL_SD است.
 */
#ifndef FM_SD_DISKIO_CACHE_PATCH_H
#define FM_SD_DISKIO_CACHE_PATCH_H

#include "fm_sd_hal.h"

#endif
