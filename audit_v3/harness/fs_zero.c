/* Who zeroes the FM_FS object of a mounted LittleFS volume?
 * Sequence: mount FMFS on FA, use it, unmount; mount LittleFS on FB, use it.
 * After every library call the FM_FS is compared with a saved copy.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"
FM_Result LittleFS_Init(FM_FS *fs);

static uint8_t   g_memA[160 * 1024];
static uint8_t   g_memB[160 * 1024];
static FM_Storage g_stA, g_stB;
static FM_FS      g_fsA, g_fsB;
static FM_FS      g_saved;

static int check(const char *what)
{
    if (memcmp(&g_fsB, &g_saved, sizeof(FM_FS)) != 0)
    {
        printf("  !! FM_FS CHANGED after %s  ops.mkdir=%p ops.file_open=%p "
               "user=%p mounted=%d\n", what,
               (void *)g_fsB.ops.mkdir, (void *)g_fsB.ops.file_open,
               g_fsB.user, (int)g_fsB.mounted);
        return 1;
    }
    return 0;
}

int main(void)
{
    FM_Result r;
    char f[256];
    unsigned k;

    FileManager_Init();

    /* A: FMFS */
    memset(g_memA, 0, sizeof(g_memA));
    RamDisk_Init(&g_stA, "FA", g_memA, sizeof(g_memA), 512);
    Storage_Register(&g_stA);
    Storage_Mount("FA");
    FMFS_Init(&g_fsA);
    printf("A mount %s\n", FM_ResultString(FS_Mount("/FA", &g_fsA)));
    printf("A write %s\n", FM_ResultString(File_WriteAll("/FA/a.txt", "x", 1)));
    printf("A unmount %s\n", FM_ResultString(FS_Unmount("/FA")));
    Storage_Unmount("FA");
    Storage_Unregister("FA");

    /* B: LittleFS */
    memset(g_memB, 0xFF, sizeof(g_memB));
    RamDisk_Init(&g_stB, "FB", g_memB, sizeof(g_memB), 4096);
    Storage_Register(&g_stB);
    Storage_Mount("FB");
    LittleFS_Init(&g_fsB);
    printf("B mount %s\n", FM_ResultString(FS_Mount("/FB", &g_fsB)));
    memcpy(&g_saved, &g_fsB, sizeof(FM_FS));
    printf("  ops.mkdir=%p user=%p\n", (void *)g_fsB.ops.mkdir, g_fsB.user);

    r = Directory_Create("/FB/wide");
    printf("B mkdir %s\n", FM_ResultString(r));
    if (check("mkdir")) return 1;

    for (k = 0; k < 12; k++)
    {
        snprintf(f, sizeof(f), "/FB/wide/sub%02u", k);
        r = Directory_Create(f);
        printf("  sub%02u mkdir %s\n", k, FM_ResultString(r));
        if (check("mkdir sub")) return 1;
        snprintf(f, sizeof(f), "/FB/wide/sub%02u/a.bin", k);
        r = File_WriteAll(f, "12345", 5);
        printf("           write %s\n", FM_ResultString(r));
        if (check("write")) return 1;
    }
    return 0;
}
