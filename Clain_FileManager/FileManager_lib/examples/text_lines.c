/*
 * Example 06 — Text Line Editing
 *
 * APIها:
 *   Text_ReadLine   — خواندن یک خط از فایل باز
 *   Text_InsertLine — درج خط در شماره مشخص
 *   Text_DeleteLine — حذف خط شماره مشخص
 *   Text_Replace    — جایگزینی زیررشته
 *   Text_CountLines — شمارش خطوط
 *   Text_Find       — پیدا کردن زیررشته
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

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

static void show(const char *label, const char *path)
{
    char buf[512];
    printf("--- %s ---\n", label);
    File_ReadText(path, buf, sizeof(buf), NULL);
    printf("%s\n\n", buf);
}

int main(void)
{
    size_t n;

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    File_WriteText("/cfg.txt",
        "host=192.168.1.1\nport=8080\ntimeout=30\ndebug=false\n");
    show("Original", "/cfg.txt");

    Text_Replace("/cfg.txt", "8080", "9090");
    show("Replace 8080->9090", "/cfg.txt");

    Text_InsertLine("/cfg.txt", 3, "retry=3");
    show("InsertLine 3", "/cfg.txt");

    Text_DeleteLine("/cfg.txt", 4);
    show("DeleteLine 4", "/cfg.txt");

    Text_CountLines("/cfg.txt", &n);
    printf("Lines: %lu\n", (unsigned long)n);

    {
        size_t line;
        if (Text_Find("/cfg.txt", "retry", &line) == FM_OK)
            printf("'retry' at line %lu\n", (unsigned long)line);
    }

    {
        FM_File *f = File_Open("/cfg.txt", FM_OPEN_READ);
        char line[128];
        int i = 0;
        printf("\nReadLine:\n");
        while (Text_ReadLine(f, line, sizeof(line), NULL) == FM_OK)
            printf("  %d: %s\n", ++i, line);
        File_Close(f);
    }

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
