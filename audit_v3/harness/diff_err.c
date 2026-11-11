/* Audit v3: the same failing path-operation on all three backends; the
 * point is the ERROR CODE, not the effect (nothing must move). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[3];

static void setup(void)
{
    FM_Result r;
    int k;

    for (k = 0; k < 2; k++)
    {
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], k ? "EB" : "EA", g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(k ? "EB" : "EA");
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[k]) : LittleFS_Init(&g_fs[k]);
        if (r == FM_OK) r = FS_Mount(k ? "/EB" : "/EA", &g_fs[k]);
    }
    (void)PosixFS_Init(&g_fs[2]);
    (void)FS_Mount("/", &g_fs[2]);
    (void)mkdir("/tmp/de_root", 0777);
    (void)chdir("/tmp/de_root");
}

static void row(const char *what, const char *mnt, FM_Result r, const char *ops)
{
    printf("  %-8s %-34s %-22s %s\n", mnt, what, FM_ResultString(r), ops);
}

static void run(const char *mnt)
{
    char f[64], d[64], e[64];
    FM_Result r;

    snprintf(f, sizeof(f), "%s/file.txt", mnt);
    snprintf(d, sizeof(d), "%s/dir", mnt);
    snprintf(e, sizeof(e), "%s/empty", mnt);
    (void)Directory_DeleteRecursive(f);
    (void)Directory_DeleteRecursive(d);
    (void)Directory_DeleteRecursive(e);
    (void)File_WriteAll(f, "x", 1);
    (void)Directory_Create(d);
    (void)File_WriteAll((snprintf(f + strlen(f), 8, ""), f), "x", 1);

    r = File_Move(f, d, NULL, NULL);
    row("File_Move(file, existing dir)", mnt, r, "");
    (void)File_WriteAll(f, "x", 1);
    r = File_Move(f, d, NULL, NULL);
    row(" (again)", mnt, r, "");
    (void)File_Delete(f);
    (void)Directory_DeleteRecursive(d);
}

int main(void)
{
    setup();
    printf("%-8s %-34s %s\n", "backend", "operation", "result");
    run("/EA");
    run("/EB");
    run("");
    return 0;
}
