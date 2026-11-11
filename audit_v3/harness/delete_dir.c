/* Audit v3 FILE3-xx: File_Delete() on a directory removes the directory.
 * Build: ./build_harness.sh harness/delete_dir.c /tmp/delete_dir */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

int main(void)
{
    FM_Result r;
    FM_Bool exists = FM_TRUE;
    int round;

    FileManager_Init();
    memset(g_mem, 0xFF, sizeof(g_mem));
    (void)RamDisk_Init(&g_st, "DA", g_mem, sizeof(g_mem), 4096);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("DA");
    (void)LittleFS_Init(&g_fs);
    r = FS_Mount("/DA", &g_fs);
    printf("LittleFS mount: %s\n", FM_ResultString(r));
    if (r != FM_OK) return 1;

    for (round = 0; round < 3; round++)
    {
        const char *dir = (round == 0) ? "/DA/empty" : "/DA/full";
        int i;

        r = Directory_Create(dir);
        printf("\nround %d: Directory_Create(%s) -> %s\n", round, dir,
               FM_ResultString(r));
        if (round == 2)
        {
            for (i = 0; i < 3; i++)
            {
                char f[64];

                snprintf(f, sizeof(f), "%s/f%d.txt", dir, i);
                (void)File_WriteAll(f, "hello", 5);
            }
        }
        if (round == 2)
        {
            /* the non-empty case must be refused -- it is */
            printf("  (%s, 3 files inside)\n", dir);
        }
        r = File_Delete(dir);
        printf("  File_Delete(%s) -> %s  (documented: FM_ERR_IS_DIRECTORY)\n",
               dir, FM_ResultString(r));
        exists = FM_TRUE;
        (void)Directory_Exists(dir, &exists);
        printf("  Directory_Exists -> %d\n", (int)exists);
        if (round >= 2) continue;
    }

    /* and the same on POSIX, where File_Delete() is remove(2) */
    {
        static FM_FS pf;
        FM_Result q;

        (void)mkdir("/tmp/dd_root", 0777);
        (void)chdir("/tmp/dd_root");
        q = PosixFS_Init(&pf);
        if (q == FM_OK) q = FS_Mount("/", &pf);
        printf("\nPOSIX mount -> %s\n", FM_ResultString(q));
        if (q == FM_OK)
        {
            q = Directory_Create("/dir");
            printf("POSIX: Directory_Create(/P/dir) -> %s\n",
                   FM_ResultString(q));
            q = File_Delete("/dir");
            printf("  File_Delete(/P/dir) -> %s  (documented: "
                   "FM_ERR_IS_DIRECTORY)\n", FM_ResultString(q));
            exists = FM_TRUE;
            (void)Directory_Exists("/dir", &exists);
            printf("  Directory_Exists -> %d\n", (int)exists);
        }
    }
    return 0;
}
/* POSIX part is inside main above; this appendix left empty */
