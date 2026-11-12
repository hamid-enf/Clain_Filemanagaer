/*
 * safe_write.c — Safe / Crash-Safe Writes
 *
 * APIها:
 *   File_SafeReplace   — نوشتن امن (temp + rename)
 *   File_SafeWriteText — نسخه متنی SafeReplace
 *   File_SafeAppend    — append امن با flush
 *   File_Transaction   — backup + safe-replace + rollback on failure
 *   File_Recover       — حذف فایل temp باقی‌مانده
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

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

static void show(const char *label)
{
    char buf[256];
    File_ReadText("/settings.txt", buf, sizeof(buf), NULL);
    printf("  %s => \"%s\"\n", label, buf);
}

int main(void)
{
    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    File_WriteText("/settings.txt", "version=1");
    show("Initial");

    File_SafeReplace("/settings.txt", "version=2", 9);
    show("SafeReplace");

    File_SafeWriteText("/settings.txt", "version=3");
    show("SafeWriteText");

    File_SafeAppend("/settings.txt", "\nextra=data", 11);
    show("SafeAppend");

    File_Transaction("/settings.txt", "version=FINAL", 14);
    show("Transaction");

    File_Recover("/settings.txt");
    printf("  Recover done (no stale .fm_tmp).\n");

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
