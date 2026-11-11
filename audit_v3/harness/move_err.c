/* File_Move(file -> existing directory): FMFS says "not a directory",
 * LittleFS/POSIX say "is a directory".  Also: what is left behind. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t g_memA[512 * 1024], g_memB[512 * 1024];
static FM_Storage g_sa, g_sb;
static FM_FS g_fa, g_fb, g_px;

static void trial(const char *mnt, int kind)
{
    char src[64], dst[64], inside[64];
    FM_Result r;
    FM_Bool ex = FM_FALSE;

    snprintf(src, sizeof(src), "%s/x.txt", mnt);
    snprintf(dst, sizeof(dst), "%s/d", mnt);
    snprintf(inside, sizeof(inside), "%s/d/x.txt", mnt);
    (void)File_Delete(src);
    (void)Directory_DeleteRecursive(dst);
    (void)File_WriteAll(src, "hello", 5);
    (void)Directory_Create(dst);
    r = File_Move(src, dst, NULL, NULL);
    printf("%-10s File_Move(file, existing dir) -> %-18s", 
           kind == 0 ? "FMFS" : (kind == 1 ? "LittleFS" : "POSIX"),
           FM_ResultString(r));
    (void)File_Exists(src, &ex);
    printf("source=%d ", (int)ex);
    (void)File_Exists(inside, &ex);
    printf("moved-inside=%d", (int)ex);
    (void)File_Exists(dst, &ex);
    printf(" dst-exists=%d\n", (int)ex);
}

int main(void)
{
    FileManager_Init();

    memset(g_memA, 0, sizeof(g_memA));
    (void)RamDisk_Init(&g_sa, "MA", g_memA, sizeof(g_memA), 512);
    (void)Storage_Register(&g_sa);
    (void)Storage_Mount("MA");
    (void)FMFS_Init(&g_fa);
    printf("FMFS mount %s\n", FM_ResultString(FS_Mount("/MA", &g_fa)));

    memset(g_memB, 0xFF, sizeof(g_memB));
    (void)RamDisk_Init(&g_sb, "MB", g_memB, sizeof(g_memB), 4096);
    (void)Storage_Register(&g_sb);
    (void)Storage_Mount("MB");
    (void)LittleFS_Init(&g_fb);
    printf("lfs  mount %s\n", FM_ResultString(FS_Mount("/MB", &g_fb)));

    (void)mkdir("/tmp/mv_root", 0777);
    (void)chdir("/tmp/mv_root");
    (void)PosixFS_Init(&g_px);
    printf("posix mount %s\n", FM_ResultString(FS_Mount("/", &g_px)));

    trial("/MA", 0);
    trial("/MB", 1);
    trial("", 2);
    return 0;
}
