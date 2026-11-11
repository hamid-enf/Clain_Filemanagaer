/* Audit v3 FILE3-05: FS_Mount() accepts an FM_FS that was never passed to
 * its adapter's *_Init() (every op is NULL) and reports success; the next
 * File_Open() then calls a NULL function pointer.
 *
 * Build: build_harness.sh harness/uninit_fs.c /tmp/uninit_fs
 * Run  : /tmp/uninit_fs
 *   before the fix: "FS_Mount(all-zero FM_FS) -> 0 (OK)" and the call
 *   after it would jump through a NULL pointer (see the ASan SEGV with
 *   -DFM_UNINIT_CRASH=1);
 *   after the fix: invalid argument, and a real adapter still mounts.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "file_manager.h"

static uint8_t g_mem[3][64 * 1024];
static FM_Storage g_st[3];

static FM_Result one_storage(int i, const char *name)
{
    FM_Result r;

    memset(g_mem[i], 0, sizeof(g_mem[i]));
    r = RamDisk_Init(&g_st[i], name, g_mem[i], sizeof(g_mem[i]), 512);
    if (r == FM_OK) r = Storage_Register(&g_st[i]);
    if (r == FM_OK) r = Storage_Mount(name);
    return r;
}

int main(void)
{
    static FM_FS zero;         /* never passed to any *_Init()  */
    static FM_FS half;
    static FM_FS good;
    FM_Result r;
    int bad = 0;

    FileManager_Init();

    /* 1. an all-zero FM_FS on a working storage */
    if (one_storage(0, "FA") != FM_OK) { printf("setup FA failed\n"); return 2; }
    printf("FM_FS.ops.file_open = %p, ops.mount = %p\n",
           (void *)zero.ops.file_open, (void *)zero.ops.mount);
    r = FS_Mount("/FA", &zero);
    printf("FS_Mount(all-zero FM_FS)   -> %-18s [want invalid argument]\n",
           FM_ResultString(r));
    if (r == FM_OK)
    {
        bad = 1;
        printf("  ...published; the next call would jump through NULL\n");
    }
#ifdef FM_UNINIT_CRASH
    if (r == FM_OK)
    {
        printf("now File_Open(\"/FA/x\", ...)\n");
        fflush(stdout);
        (void)File_Open("/FA/x", FM_OPEN_WRITE | FM_OPEN_CREATE);
        printf("File_Open returned\n");
    }
#endif

    /* 2. the registry has always refused the same table */
    r = FS_Register(&zero);
    printf("FS_Register(all-zero)      -> %-18s [want invalid argument]\n",
           FM_ResultString(r));
    if (r != FM_ERR_INVALID_ARGUMENT) bad = 1;

    /* 3. a table with only file_open filled in */
    if (one_storage(1, "FB") != FM_OK) { printf("setup FB failed\n"); return 2; }
    memset(&half, 0, sizeof(half));
    (void)strcpy(half.name, "HALF");
    half.ops.file_open = zero.ops.file_open;   /* still NULL */
    half.ops.file_open = (FM_FS_FileOpenFn)RamDisk_Init;
    r = FS_Mount("/FB", &half);
    printf("FS_Mount(half-filled)      -> %-18s [want invalid argument]\n",
           FM_ResultString(r));
    if (r != FM_ERR_INVALID_ARGUMENT) bad = 1;

    /* 4. an initialised adapter still mounts and its volume works */
    if (one_storage(2, "FC") != FM_OK) { printf("setup FC failed\n"); return 2; }
    r = FMFS_Init(&good);
    if (r == FM_OK) r = FS_Mount("/FC", &good);
    printf("FS_Mount(FMFS_Init'ed)     -> %-18s [want OK]\n",
           FM_ResultString(r));
    if (r != FM_OK)
    {
        bad = 1;
    }
    else
    {
        FM_File *f = File_Open("/FC/x.txt", FM_OPEN_WRITE | FM_OPEN_CREATE);

        printf("  File_Open on it          -> %s\n", f ? "OK" : "NULL");
        if (f == NULL) bad = 1;
        else
        {
            uint32_t wr = 0;

            if (File_Write(f, "abc", 3, &wr) == FM_OK && wr == 3)
                printf("  File_Write               -> OK, 3 bytes\n");
            else { printf("  File_Write               -> FAIL\n"); bad = 1; }
            (void)File_Close(f);
        }
    }

    printf("\nuninit_fs: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
