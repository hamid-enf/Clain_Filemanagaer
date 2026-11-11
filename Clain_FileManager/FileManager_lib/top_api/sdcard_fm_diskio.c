/*
 * Example 12 — SD از diskio کتابخانه (HAL_SD)، نه sd_diskio.c کیوب
 *
 * فلگ:  #define FM_SD_DISKIO FILEMANAGER
 * Cube: FATFS/Target/sd_diskio.c را Exclude from Build کنید.
 *       خود فایل کیوب را کپی/پچ نکنید.
 */
#include "fm_config.h"   /* FM_USE_FATFS may come from fm_board_config.h */
#if defined(FM_USE_FATFS) && FM_USE_FATFS && \
    defined(FM_SD_DISKIO) && (FM_SD_DISKIO == FILEMANAGER) && \
    !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)

#include <stdio.h>
#include "file_manager.h"
#include "fm_sd_hal.h"
#include "fm_fatfs.h"

extern SD_HandleTypeDef hsd1;

static FM_FS g_sd_fs;

FM_Result ex_12_sd_diskio_init(void)
{
    FM_Result r;

    FileManager_Init();
    r = FM_SD_Hal_Attach(&hsd1);
    if (r != FM_OK)
    {
        return r;
    }
    r = FatFS_Init(&g_sd_fs, 0);
    if (r != FM_OK)
    {
        return r;
    }
    return FS_Mount("/", &g_sd_fs);
}

FM_Storage *ex_12_sd_get_storage(void)
{
    return FM_SD_Hal_Storage();
}

void ex_12_sdcard_fm_diskio(void)
{
    FM_Result r;
    char buf[128];

    r = ex_12_sd_diskio_init();
    printf("[init] %s (%d)\n", File_GetErrorString(r), (int)r);
    if (r != FM_OK)
    {
        return;
    }
    r = File_WriteText("/greeting.txt", "Hello from SD Card!");
    printf("[File_WriteText] %s\n", File_GetErrorString(r));
    r = File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("[File_ReadText]  %s -> %s\n", File_GetErrorString(r), buf);
}

#endif
