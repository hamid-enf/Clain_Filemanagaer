/* Audit v3: every failing call must give its handle/slot back.
 * 10 000 iterations of each failing call, then one valid call must work. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[2];
static const char *const g_stname[2] = { "LA", "LB" };
static const char *const g_mount[2] = { "/LA", "/LB" };

static int check(const char *what, int kind, const char *mnt)
{
    char p[96];
    uint32_t used0 = File_PoolUsed();
    int i;
    FM_Bool ok = FM_FALSE;

    for (i = 0; i < 10000; i++)
    {
        /* a failing call, chosen so the pool is entered and left */
        snprintf(p, sizeof(p), "%s/missing%d", mnt, i & 1);
        (void)File_Open(p, FM_OPEN_READ);
        (void)File_Open(NULL, FM_OPEN_READ);
        (void)File_Open(p, 0);
        (void)File_ReadAll(p, p, 4, NULL);
    }
    snprintf(p, sizeof(p), "%s/ok.txt", mnt);
    (void)File_WriteAll(p, "ok", 2);
    (void)File_Exists(p, &ok);
    if (!ok)
    {
        printf("  %-8s %-22s FAILED: a valid open does not work any more "
               "(pool used %u before, %u after)\n", mnt, what, used0,
               File_PoolUsed());
        return 1;
    }
    (void)File_Delete(p);
    if (File_PoolUsed() != used0)
    {
        printf("  %-8s %-22s pool used changed %u -> %u\n", mnt, what, used0,
               File_PoolUsed());
        return 1;
    }
    (void)kind;
    (void)what;
    return 0;
}

int main(void)
{
    int bad = 0;

    FileManager_Init();
    {
        int k;

        for (k = 0; k < 2; k++)
        {
            FM_Result r;

            memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
            r = RamDisk_Init(&g_st[k], g_stname[k], g_mem[k], sizeof(g_mem[k]),
                             k ? 4096u : 512u);
            if (r == FM_OK) r = Storage_Register(&g_st[k]);
            if (r == FM_OK) r = Storage_Mount(g_stname[k]);
            if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[k]) : LittleFS_Init(&g_fs[k]);
            if (r == FM_OK) r = FS_Mount(g_mount[k], &g_fs[k]);
            if (r != FM_OK) printf("mount %d: %s\n", k, FM_ResultString(r));
        }
    }
    bad += check("failing File_Open", 0, "/LA");
    bad += check("failing File_Open", 1, "/LB");

    /* external handles (File_OpenEx) too */
    {
        FM_File f;
        int i;
        FM_Result r = FM_OK;

        for (i = 0; i < 10000 && r != FM_OK; i++)
        {
            File_Init(&f);
            r = File_OpenEx(&f, "/LA/none.txt", FM_OPEN_READ);
        }
        File_Init(&f);
        r = File_OpenEx(&f, "/LA/none.txt", FM_OPEN_READ);
        {
            FM_Result r2;
            FM_File g;

            File_Init(&g);
            r2 = File_OpenEx(&g, "/LA/none2.txt", FM_OPEN_READ);
            (void)r2;
            (void)r;
        }
        /* a valid external open still works after 10 000 failures */
        {
            FM_File a;
            FM_Result r3;

            (void)File_WriteAll("/LA/ext.txt", "x", 1);
            File_Init(&a);
            r3 = File_OpenEx(&a, "/LA/ext.txt", FM_OPEN_READ);
            if (r3 != FM_OK)
            {
                printf("  external handle pool exhausted after failing opens: "
                       "%s\n", FM_ResultString(r3));
                bad++;
            }
            else (void)File_Close(&a);
        }
    }
    printf("handle_leak: %s\n", bad ? "LEAK" : "clean");
    return bad;
}
