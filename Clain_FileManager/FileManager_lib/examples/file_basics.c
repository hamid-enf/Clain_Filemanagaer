/*
 * Example 01 — File Basics
 *
 * عملیات پایه روی فایل: نوشتن، خواندن، وجود داشتن، حذف.
 *
 * APIها:
 *   File_WriteText  — نوشتن متن (ایجاد می‌کنه اگه نبود)
 *   File_ReadText   — خواندن متن
 *   File_Exists     — بررسی وجود فایل
 *   File_Delete     — حذف فایل
 *   File_Create     — ایجاد فایل خالی
 *   File_SizeOf     — حجم فایل
 */
#include <stdio.h>
#include <stdint.h>
#include "file_manager.h"

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

int main(void)
{
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    /* 1. نوشتن */
    File_WriteText("/greeting.txt", "Hello, FileManager!");
    printf("Written: /greeting.txt\n");

    /* 2. خواندن */
    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("Read:    %s\n", buf);

    /* 3. بررسی وجود */
    File_Exists("/greeting.txt", &exists);
    printf("Exists?  %s\n", exists ? "yes" : "no");

    File_Exists("/no-such-file.txt", &exists);
    printf("NoFile?  %s\n", exists ? "yes" : "no");

    /* 4. حجم */
    File_SizeOf("/greeting.txt", &size);
    printf("Size:    %llu bytes\n", (unsigned long long)size);

    /* 5. بازنویسی */
    File_WriteText("/greeting.txt", "New content!");
    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("Overwrite: %s\n", buf);

    /* 6. فایل خالی */
    File_Create("/empty.txt");
    File_SizeOf("/empty.txt", &size);
    printf("Empty size: %llu\n", (unsigned long long)size);

    /* 7. حذف */
    File_Delete("/greeting.txt");
    File_Exists("/greeting.txt", &exists);
    printf("Deleted? %s\n", exists ? "no" : "yes");

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}
