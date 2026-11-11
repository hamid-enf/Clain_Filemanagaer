/*
 * Example 03 — Advanced File Handle
 *
 * FM_File handle برای عملیات پیشرفته:
 * Open, Read, Write, Seek, Tell, Size, Truncate, Flush, Close, EOF.
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

int main(void)
{
    FM_File *f;
    uint64_t pos, size;
    char buf[32];
    uint32_t n;

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    /* 1. نوشتن تکه‌ای */
    f = File_Open("/data.bin", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (!f) return 1;
    File_Write(f, "AAAA", 4, &n);
    File_Write(f, "BBBB", 4, &n);
    File_Write(f, "CCCC", 4, &n);
    File_Tell(f, &pos);
    printf("After write, pos = %llu\n", (unsigned long long)pos);
    File_Close(f);

    /* 2. خواندن با seek */
    f = File_Open("/data.bin", FM_OPEN_READ);

    File_Seek(f, 4, FM_SEEK_SET);
    File_Read(f, buf, 4, &n);
    buf[n] = '\0';
    printf("At offset 4: %s\n", buf);

    File_Seek(f, -4, FM_SEEK_END);
    File_Read(f, buf, 4, &n);
    buf[n] = '\0';
    printf("Last 4 bytes: %s\n", buf);

    File_Size(f, &size);
    printf("Size: %llu\n", (unsigned long long)size);
    File_Close(f);

    /* 3. Truncate */
    f = File_Open("/data.bin", FM_OPEN_READ | FM_OPEN_WRITE);
    File_Truncate(f, 6);
    File_Size(f, &size);
    printf("After truncate: size = %llu\n", (unsigned long long)size);

    File_Seek(f, 0, FM_SEEK_SET);
    File_Read(f, buf, 6, &n);
    buf[n] = '\0';
    printf("Content: %s\n", buf);

    File_Seek(f, 0, FM_SEEK_END);
    File_Write(f, "DDDD", 4, &n);
    File_Tell(f, &pos);
    printf("After append, pos = %llu\n", (unsigned long long)pos);
    File_Close(f);

    /* 4. EOF */
    f = File_Open("/data.bin", FM_OPEN_READ);
    File_Seek(f, 0, FM_SEEK_END);
    printf("EOF? %s\n", File_EOF(f) ? "yes" : "no");
    File_Close(f);

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
