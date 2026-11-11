/* Audit v3 -- FILE3-01, narrowed to its worst case.
 *
 * Every backend agrees that a NON-EMPTY directory must be refused by
 * File_Delete() ("delete_file: files only").  LittleFS does not: it removes
 * the directory and everything under it and reports FM_OK.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[1024 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs[2];

static void tree(const char *mnt)
{
    char p[128];
    char q[128];
    FM_Result r;
    FM_Bool exists = FM_TRUE;
    FM_Bool file_exists = FM_TRUE;

    snprintf(p, sizeof(p), "%s/D/sub/deep.txt", mnt);
    snprintf(q, sizeof(q), "%s/D", mnt);
    (void)Directory_DeleteRecursive(q);

    r = Directory_Create(q);
    snprintf(p, sizeof(p), "%s/D/a.txt", mnt);
    if (r == FM_OK) r = File_WriteAll(p, "aaaa", 4);
    snprintf(p, sizeof(p), "%s/D/b.txt", mnt);
    if (r == FM_OK) r = File_WriteAll(p, "bbbb", 4);
    snprintf(p, sizeof(p), "%s/D/sub", mnt);
    if (r == FM_OK) r = Directory_Create(p);
    snprintf(p, sizeof(p), "%s/D/sub/deep.txt", mnt);
    if (r == FM_OK) r = File_WriteAll(p, "deep", 4);
    printf("  built %s/D: 2 files + sub/deep.txt (%s)\n", mnt,
           FM_ResultString(r));

    r = File_Delete(q);
    (void)Directory_Exists(q, &exists);
    snprintf(p, sizeof(p), "%s/D/a.txt", mnt);
    (void)File_Exists(p, &file_exists);
    printf("  File_Delete(\"%s\") -> %-20s directory exists: %-3s  "
           "a.txt exists: %s\n", q, FM_ResultString(r),
           exists ? "yes" : "NO", file_exists ? "yes" : "NO - data gone");

    (void)Directory_DeleteRecursive(q);
}

int main(void)
{
    FM_Result r;

    FileManager_Init();
    (void)mkdir("/tmp/dn_root", 0777);
    (void)chdir("/tmp/dn_root");

    memset(g_mem, 0, sizeof(g_mem));
    r = RamDisk_Init(&g_st, "DN", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("DN");
    if (r == FM_OK) r = FMFS_Init(&g_fs[0]);
    if (r == FM_OK) r = FS_Mount("/DN", &g_fs[0]);
    if (r != FM_OK) { printf("fmfs mount: %s\n", FM_ResultString(r)); return 2; }

    printf("FMFS:\n");
    tree("/DN");
    (void)FS_Unmount("/DN");
    (void)Storage_Unmount("DN");
    (void)Storage_Unregister(&g_st);

    memset(g_mem, 0xFF, sizeof(g_mem));
    r = RamDisk_Init(&g_st, "DN", g_mem, sizeof(g_mem), 4096u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("DN");
    if (r == FM_OK) r = LittleFS_Init(&g_fs[1]);
    if (r == FM_OK) r = FS_Mount("/DN", &g_fs[1]);
    if (r != FM_OK) { printf("lfs mount: %s\n", FM_ResultString(r)); return 2; }

    printf("\nLittleFS:\n");
    tree("/DN");

    printf("\nPOSIX:\n");
    if (PosixFS_Init(&g_fs[0]) == FM_OK && FS_Mount("/", &g_fs[0]) == FM_OK)
        tree("");
    return 0;
}
