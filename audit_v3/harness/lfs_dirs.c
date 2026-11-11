/* LittleFS directory listing / nesting consistency.
 * Build: ./build_harness.sh harness/lfs_dirs.c /tmp/lfs_dirs */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t   g_mem[192 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

static void list(const char *path)
{
    FM_Dir *d = Directory_Open(path);
    FM_DirEntry e;
    unsigned n = 0;

    if (d == NULL)
    {
        printf("  list %-16s -> open failed: %s\n", path,
               FM_ResultString(File_GetLastError()));
        return;
    }
    printf("  list %-16s ->", path);
    for (;;)
    {
        FM_Result r = Directory_Read(d, &e);

        if (r != FM_OK)
        {
            if (r != FM_ERR_EOF) printf(" [read %s]", FM_ResultString(r));
            break;
        }
        if (n < 12) printf(" %s%s", e.name, e.type == FM_ITEM_DIR ? "/" : "");
        n++;
    }
    printf("   (%u entries)\n", n);
    (void)Directory_Close(d);
}

int main(void)
{
    FM_Result r;
    unsigned k;

    memset(g_mem, 0xFF, sizeof(g_mem));
    FileManager_Init();
    (void)RamDisk_Init(&g_st, "FB", g_mem, sizeof(g_mem), 4096);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("FB");
    (void)LittleFS_Init(&g_fs);
    r = FS_Mount("/FB", &g_fs);
    printf("mount %s\n", FM_ResultString(r));

    printf("\n[1] files directly in the root\n");
    for (k = 0; k < 5; k++)
    {
        char f[64];

        snprintf(f, sizeof(f), "/FB/f%u.txt", k);
        r = File_WriteAll(f, "x", 1);
        if (r != FM_OK) printf("  write %s -> %s\n", f, FM_ResultString(r));
    }
    list("/FB");

    printf("\n[2] two subdirectories, each with a file\n");
    r = Directory_Create("/FB/d1");
    printf("  mkdir d1 -> %s\n", FM_ResultString(r));
    r = Directory_Create("/FB/d2");
    printf("  mkdir d2 -> %s\n", FM_ResultString(r));
    list("/FB");
    r = File_WriteAll("/FB/d1/a.txt", "aaa", 3);
    printf("  write d1/a.txt -> %s\n", FM_ResultString(r));
    r = File_WriteAll("/FB/d2/b.txt", "bbb", 3);
    printf("  write d2/b.txt -> %s\n", FM_ResultString(r));
    list("/FB/d1");
    list("/FB/d2");
    list("/FB");

    printf("\n[3] 8 subdirectories created up front, then filled\n");
    for (k = 0; k < 8; k++)
    {
        char d[64];

        snprintf(d, sizeof(d), "/FB/s%u", k);
        r = Directory_Create(d);
        printf("  mkdir s%u -> %s\n", k, FM_ResultString(r));
    }
    list("/FB");
    for (k = 0; k < 8; k++)
    {
        char f[64];

        snprintf(f, sizeof(f), "/FB/s%u/a.bin", k);
        r = File_WriteAll(f, "12345", 5);
        printf("  write s%u/a.bin -> %s\n", k, FM_ResultString(r));
    }
    list("/FB/s0");
    printf("  size of /FB: ");
    {
        uint64_t sz = 0;
        r = Directory_Size("/FB", &sz);
        printf("%s, %u bytes\n", FM_ResultString(r), (unsigned)sz);
    }
    (void)FS_Unmount("/FB");
    (void)Storage_Unmount("FB");
    return 0;
}
