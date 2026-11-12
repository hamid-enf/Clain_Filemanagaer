/*
 * crc_example.c — CRC & Checksum
 *
 * APIها:
 *   CRC_Calculate16      — CRC16-CCITT
 *   CRC_Compute16Modbus  — CRC16-MODBUS
 *   CRC_Calculate32      — CRC32 (IEEE/zlib)
 *   CRC_Checksum8        — XOR
 *   CRC_Checksum32       — sum
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
    const char *data = "123456789";
    size_t len = strlen(data);

    printf("Data: \"%s\" (%lu bytes)\n\n", data, (unsigned long)len);

    printf("CRC16-CCITT  = 0x%04X\n", CRC_Calculate16(data, len));
    printf("CRC16-MODBUS = 0x%04X\n", CRC_Compute16Modbus(data, len, 0xFFFF));
    printf("CRC32        = 0x%08X  (check: 0xCBF43926)\n",
           CRC_Calculate32(data, len));
    printf("Checksum8    = 0x%02X\n", CRC_Checksum8(data, len));
    printf("Checksum32   = 0x%08X\n", CRC_Checksum32(data, len));

    /* CRC فایل */
    {
        char text[256];
        size_t rd;

        FileManager_Init();
        FileManager_MountMemory(&g_block, &g_fs, "RAM",
                                g_storage, sizeof(g_storage), 1, FM_FALSE);

        File_WriteText("/data.bin", "important sensor data");
        File_ReadAll("/data.bin", text, sizeof(text), &rd);
        printf("\nFile CRC32 = 0x%08X (%lu bytes)\n",
               CRC_Calculate32(text, rd), (unsigned long)rd);

        FS_Unmount("/");
        FileManager_Deinit();
    }

    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
