/*
 * Example 02 — Copy, Move, Rename
 *
 * کپی و جابه‌جایی فایل.
 *
 * APIها:
 *   File_Copy   — کپی فایل (progress callback اختیاری)
 *   File_Move   — جابه‌جایی (rename یا copy+delete)
 *   File_Rename — تغییر نام (همان mount)
 */
#include <stdio.h>
#include <stdint.h>
#include "file_manager.h"

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

static void copy_progress(uint64_t done, uint64_t total, void *ud)
{
    (void)ud;
    printf("  copy: %llu / %llu\n",
           (unsigned long long)done, (unsigned long long)total);
}

int main(void)
{
    char buf[128];

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    File_WriteText("/original.txt", "This is the original file content.");

    /* 1. کپی */
    printf("Copy:\n");
    File_Copy("/original.txt", "/copy.txt", copy_progress, NULL);

    File_ReadText("/copy.txt", buf, sizeof(buf), NULL);
    printf("  content: %s\n", buf);

    /* 2. تغییر نام */
    File_Rename("/copy.txt", "/renamed.txt");
    File_ReadText("/renamed.txt", buf, sizeof(buf), NULL);
    printf("Renamed: %s\n", buf);

    /* 3. جابه‌جایی */
    Directory_Create("/backup");
    File_Move("/renamed.txt", "/backup/final.txt", NULL, NULL);
    File_ReadText("/backup/final.txt", buf, sizeof(buf), NULL);
    printf("Moved: %s\n", buf);

    {
        FM_Bool exists;
        File_Exists("/renamed.txt", &exists);
        printf("Original exists? %s\n", exists ? "yes" : "no");
    }

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}
