/* Audit v3: the mount/storage registry under stress.
 *
 *  1. every way FS_Mount can fail (bad path, bad FS, unknown storage,
 *     a backend whose mount fails, the same FS twice) must give the slot
 *     back: after 1000 failed attempts a normal mount must still work.
 *  2. the same for a failed Storage_Mount and Storage_Register.
 *  3. FS_Mount with the backend's mount() failing.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[4][256 * 1024];
static FM_Storage g_st[4];
static FM_FS      g_fs[8];
static int        g_bad;

static FM_Result failing_mount(FM_FS *fs, FM_Storage *st)
{
    (void)fs;
    (void)st;
    return FM_ERR_CORRUPTED;
}

static void register_store(int k, const char *name)
{
    FM_Result r;

    memset(g_mem[k], k == 0 ? 0 : 0xFF, sizeof(g_mem[k]));
    r = RamDisk_Init(&g_st[k], name, g_mem[k], sizeof(g_mem[k]),
                     k == 0 ? 512u : 4096u);
    if (r == FM_OK) r = Storage_Register(&g_st[k]);
    if (r == FM_OK) r = Storage_Mount(name);
    if (r != FM_OK) printf("  setup %s: %s\n", name, FM_ResultString(r));
}

int main(void)
{
    FM_Result r;
    int i;

    FileManager_Init();
    register_store(0, "S0");
    register_store(1, "S1");
    register_store(2, "S2");
    register_store(3, "S3");

    /* ---- 1000 failing FS_Mount calls ---- */
    for (i = 0; i < 1000; i++)
    {
        (void)FS_Mount(NULL, &g_fs[0]);
        (void)FS_Mount("/no_such_storage", &g_fs[0]);
        (void)FS_Mount("/S0/sub", &g_fs[0]);
        (void)FS_Mount("relative", &g_fs[0]);
        (void)FS_Mount("/S0", NULL);
        (void)FS_Mount("", &g_fs[0]);
        (void)FS_Mount("/S0", &g_fs[0]);      /* real, then unmount below */
        (void)FS_Unmount("/S0");
    }
    {
        /* a backend whose mount() fails must not eat a mount slot */
        for (i = 0; i < 200; i++)
        {
            FM_FS broken;

            memset(&broken, 0, sizeof(broken));
            broken.ops.mount = failing_mount;
            r = FS_Mount("/S0", &broken);
            if (r != FM_ERR_CORRUPTED && i == 0)
                printf("  broken backend mount -> %s (want corrupted)\n",
                       FM_ResultString(r));
        }
    }
    /* a normal mount must still work */
    r = FMFS_Init(&g_fs[0]);
    if (r == FM_OK) r = FS_Mount("/S0", &g_fs[0]);
    printf("mount after 1200 failed attempts: %s\n", FM_ResultString(r));
    if (r != FM_OK) g_bad = 1;

    /* ---- the same FS twice, and two FS on one path ---- */
    if (g_bad == 0)
    {
        r = FS_Mount("/S1", &g_fs[0]);
        printf("mount the same FS on a second path: %s (want busy)\n",
               FM_ResultString(r));
        if (r != FM_ERR_BUSY) g_bad = 1;

        r = FMFS_Init(&g_fs[1]);
        if (r == FM_OK) r = FS_Mount("/S0", &g_fs[1]);
        printf("mount a second FS on the same path: %s (want exists)\n",
               FM_ResultString(r));
        if (r != FM_ERR_EXISTS) g_bad = 1;
    }

    /* ---- fill every mount slot, then overrun ---- */
    if (g_bad == 0)
    {
        (void)FS_Unmount("/S0");
        (void)FS_Unmount("/");
        r = FMFS_Init(&g_fs[0]);
        if (r == FM_OK) r = FS_Mount("/S0", &g_fs[0]);
        r = FMFS_Init(&g_fs[1]);
        if (r == FM_OK) r = FS_Mount("/S1", &g_fs[1]);
        r = FMFS_Init(&g_fs[2]);
        if (r == FM_OK) r = FS_Mount("/S2", &g_fs[2]);
        r = FMFS_Init(&g_fs[3]);
        if (r == FM_OK) r = FS_Mount("/S3", &g_fs[3]);
        printf("four mounts: %s\n", FM_ResultString(r));
        {
            static uint8_t extra_mem[64 * 1024];
            static FM_Storage extra_st;
            FM_FS extra;

            memset(extra_mem, 0, sizeof(extra_mem));
            (void)RamDisk_Init(&extra_st, "SX", extra_mem, sizeof(extra_mem),
                               512u);
            (void)Storage_Register(&extra_st);
            memset(&extra, 0, sizeof(extra));
            (void)FMFS_Init(&extra);
            r = FS_Mount("/SX", &extra);
            printf("a fifth mount point: %s (want no memory)\n",
                   FM_ResultString(r));
            if (r != FM_ERR_NO_MEMORY) g_bad = 1;
        }
        /* and everything still works after the refusal */
        r = File_WriteAll("/S3/last.txt", "x", 1);
        printf("write on the last mount: %s\n", FM_ResultString(r));
        if (r != FM_OK) g_bad = 1;
        (void)FS_Unmount("/S1");
        (void)FS_Unmount("/S2");
        (void)FS_Unmount("/S3");
        (void)FS_Unmount("/S0");
    }

    /* ---- Storage_Register over and over ---- */
    for (i = 0; i < 1000; i++)
    {
        (void)Storage_Register(&g_st[0]);        /* duplicate name */
        (void)Storage_Register(NULL);
        (void)Storage_Mount("nope");
    }
    r = FMFS_Init(&g_fs[4]);
    if (r == FM_OK) r = FS_Mount("/S0", &g_fs[4]);
    printf("mount after 3000 failed registry calls: %s\n",
           FM_ResultString(r));
    if (r != FM_OK) g_bad = 1;
    (void)FS_Unmount("/S0");

    printf("\nfs_slots: %s\n", g_bad ? "PROBLEM" : "registry intact");
    return g_bad;
}
