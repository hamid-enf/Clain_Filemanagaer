/*
 * Example 00 — Setup
 *
 * راه‌اندازی اولیه FileManager روی RAM.
 * تمام مثال‌های دیگه از همین الگو برای setup استفاده می‌کنن.
 *
 * ورودی:  یک بافر RAM + ظرفیت
 * خروجی: آماده برای File/Directory
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdint.h>
#include "file_manager.h"

static uint8_t g_storage[64 * 1024];   /* 64 KB */
static FM_Storage g_block;
static FM_FS      g_fs;

int main(void)
{
    FM_Result r;
    char buf[64];

    r = FileManager_Init();
    if (r != FM_OK) return 1;

    r = FileManager_MountMemory(&g_block, &g_fs, "RAM",
                                g_storage, sizeof(g_storage),
                                1, FM_FALSE);
    if (r != FM_OK) return 1;

    printf("OK - FileManager mounted (%u KB RAM)\n",
           (unsigned)(sizeof(g_storage) / 1024));

    File_WriteText("/hello.txt", "FileManager is ready!");

    File_ReadText("/hello.txt", buf, sizeof(buf), NULL);
    printf("Read: %s\n", buf);

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
