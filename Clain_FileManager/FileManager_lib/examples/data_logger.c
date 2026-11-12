/*
 * data_logger.c — Data Logger (CSV)
 *
 * APIها:
 *   Logger_Init/Start/Write/Stop — چرخه کامل لاگ
 *   Logger_GetInfo                — تعداد رکورد، حجم فایل
 *
 * تنظیمات:
 *   max_file_size — بعد از این حجم -> فایل جدید
 *   max_files     — حداکثر فایل (chircular)
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"

static uint8_t g_storage[128 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

int main(void)
{
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_LoggerInfo info;
    float values[3];

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);
    Directory_Create("/logs");

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_file_size = 512;
    cfg.max_files     = 3;
    cfg.flush_records = 5;

    Logger_Init(&lg, "/logs", "sensor", FM_LOG_FORMAT_CSV, &cfg);
    Logger_Start(&lg);

    for (int i = 0; i < 15; i++)
    {
        values[0] = 20.0f + (float)i * 0.5f;
        values[1] = 1000.0f + (float)i;
        values[2] = 50.0f + (float)i * 2.0f;
        Logger_Write(&lg, (uint32_t)(i * 100), values, 3);
    }

    Logger_GetInfo(&lg, &info);
    printf("Files:     %u\n", info.files_created);
    printf("Records:   %llu\n", (unsigned long long)info.record_count);
    printf("Size:      %llu bytes\n", (unsigned long long)info.file_size);
    Logger_Stop(&lg);

    /* نمایش فایل‌ها */
    printf("\nLog files:\n");
    {
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (Directory_Read(d, &e) == FM_OK)
        {
            char fullpath[FM_PATH_MAX];
            Path_Join("/logs", e.name, fullpath, sizeof(fullpath));
            printf("  %s (%llu bytes)\n", e.name, (unsigned long long)e.size);
        }
        Directory_Close(d);
    }

    /* خواندن محتوای اولین فایل */
    {
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (Directory_Read(d, &e) == FM_OK)
        {
            if (e.type == FM_ITEM_FILE)
            {
                char fullpath[FM_PATH_MAX];
                char content[512];

                Path_Join("/logs", e.name, fullpath, sizeof(fullpath));
                printf("\nContent of %s:\n", fullpath);

                if (File_ReadText(fullpath, content, sizeof(content), NULL)
                    == FM_OK)
                {
                    printf("%s\n", content);
                }
                else
                {
                    printf("  (could not read)\n");
                }
                break;
            }
        }
        Directory_Close(d);
    }

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
