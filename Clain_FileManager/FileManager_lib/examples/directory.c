/*
 * Example 04 — Directories
 *
 * APIها:
 *   Directory_Create          — ایجاد پوشه
 *   Directory_Open/Read/Close — لیست کردن
 *   Directory_Delete          — حذف پوشه خالی
 *   Directory_DeleteRecursive — حذف پوشه + محتوا
 *   Directory_ScanRecursive   — اسکن بازگشتی با callback
 *   Directory_Count           — شمارش
 *   Directory_Size            — حجم کل
 */
#include <stdio.h>
#include <stdint.h>
#include "file_manager.h"

static uint8_t g_storage[64 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

static FM_Result print_item(const FM_FileInfo *info, void *ud)
{
    (void)ud;
    printf("  [%s] %-18s %llu bytes\n",
           info->type == FM_ITEM_DIR ? "D" : "F",
           info->name, (unsigned long long)info->size);
    return FM_OK;
}

int main(void)
{
    FM_Dir *d;
    FM_DirEntry entry;
    uint32_t count;
    uint64_t total;

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    Directory_Create("/logs");
    Directory_Create("/logs/2026");
    Directory_Create("/config");
    File_WriteText("/readme.txt", "root readme");
    File_WriteText("/config/app.txt", "key=value");
    File_WriteText("/logs/2026/jan.csv", "1,2,3");
    File_WriteText("/logs/2026/feb.csv", "4,5,6");

    /* 1. لیست */
    printf("Contents of /:\n");
    d = Directory_Open("/");
    while (Directory_Read(d, &entry) == FM_OK)
        printf("  %s %-18s %llu\n",
               entry.type == FM_ITEM_DIR ? "[D]" : "[F]",
               entry.name, (unsigned long long)entry.size);
    Directory_Close(d);

    /* 2. اسکن بازگشتی */
    printf("\nRecursive scan:\n");
    Directory_ScanRecursive("/", print_item, NULL, NULL, NULL);

    /* 3. شمارش */
    Directory_Count("/logs", FM_ITEM_FILE, &count);
    printf("\nFiles in /logs: %u\n", count);

    /* 4. حجم */
    Directory_Size("/", &total);
    printf("Total size: %llu bytes\n", (unsigned long long)total);

    /* 5. حذف */
    Directory_DeleteRecursive("/logs");
    printf("\nAfter cleanup:\n");
    d = Directory_Open("/");
    while (Directory_Read(d, &entry) == FM_OK)
        printf("  %s\n", entry.name);
    Directory_Close(d);

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}
