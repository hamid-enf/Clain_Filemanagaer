/* FM-NOT-FIRMWARE: real FatFS LFN/8.3 async-move regression on RAM disk. */
#ifndef USE_HAL_DRIVER
#define main fatfs_time_unused_main
#include "fatfs_real_time_host.c"
#undef main
#include "file_manager.h"
#include "fm_fatfs.h"
#include "fm_async.h"
#include "fm_storage.h"
#include "fm_filesystem.h"

static unsigned char dummy[4096];
static FM_Storage st;
static FM_FS fs;
static FM_Async job;
int main(void)
{
    BYTE work[4096];
    DIR dir;
    FILINFO info;
    char alias[FM_PATH_MAX];
    char data[32];
    FM_Result r;
    const char *original = "/sd/VeryLongTrackName.dat";
    if (f_mkfs("", NULL, work, sizeof(work)) != FR_OK) return 1;
    if (FileManager_Init() != FM_OK || RamDisk_Init(&st, "sd", dummy, sizeof(dummy), 512) != FM_OK ||
        Storage_Register(&st) != FM_OK || FatFS_Init(&fs, 0) != FM_OK || FS_Mount("/sd", &fs) != FM_OK) return 2;
    if (File_WriteText(original, "valuable-data") != FM_OK) return 3;
    if (f_opendir(&dir, "0:/") != FR_OK || f_readdir(&dir, &info) != FR_OK || f_closedir(&dir) != FR_OK) return 4;
    if (info.altname[0] == '\0') return 5;
    snprintf(alias, sizeof(alias), "/sd/%s", info.altname);
    if (File_ReadText(alias, data, sizeof(data), NULL) != FM_OK || strcmp(data, "valuable-data") != 0) return 6;
    Async_Init(&job);
    r = Async_StartMove(&job, original, alias, NULL, NULL, NULL);
    if (r != FM_OK) return 7;
    while (Async_Poll(&job) == FM_ASYNC_RUNNING) { }
    if (Async_GetState(&job) != FM_ASYNC_DONE || Async_GetResult(&job) != FM_OK) return 8;
    if (File_ReadText(alias, data, sizeof(data), NULL) != FM_OK || strcmp(data, "valuable-data") != 0) return 9;
    printf("FatFS real LFN to 8.3 async move preserved data: %s PASS\n", info.altname);
    return 0;
}
#endif
