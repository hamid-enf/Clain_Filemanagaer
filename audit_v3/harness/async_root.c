/* Audit v3: is Async_StartScan("/<mount>") the same as scanning the root?
 * And a scan of a directory whose name is exactly at the path limit. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_async.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[1024 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;
static unsigned g_n;

static FM_Result cb(const FM_FileInfo *i, void *u)
{
    (void)u;
    printf("    %s\n", i->path);
    g_n++;
    return FM_OK;
}

int main(void)
{
    FM_Result r;
    FM_Async a;
    unsigned polls = 0;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "RA", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("RA");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/RA", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    (void)Directory_Create("/RA/d");
    (void)File_WriteAll("/RA/a.txt", "a", 1);
    (void)File_WriteAll("/RA/d/b.txt", "b", 1);

    g_n = 0;
    printf("Directory_ScanRecursive(\"/RA\"):\n");
    r = Directory_ScanRecursive("/RA", cb, NULL, NULL, NULL);
    printf("  -> %s, %u entries\n", FM_ResultString(r), g_n);

    g_n = 0;
    printf("Directory_List(\"/RA\"):\n");
    r = Directory_List("/RA", NULL, cb, NULL);
    printf("  -> %s, %u entries\n", FM_ResultString(r), g_n);

    g_n = 0;
    printf("Async_StartScan(\"/RA\"):\n");
    Async_Init(&a);
    r = Async_StartScan(&a, "/RA", cb, NULL, NULL);
    while (r == FM_OK && Async_Poll(&a) == FM_ASYNC_RUNNING && polls++ < 1000) {}
    printf("  -> start %s, state %d, result %s, %u entries\n",
           FM_ResultString(r), (int)a.state, FM_ResultString(Async_GetResult(&a)),
           g_n);
    (void)Directory_DeleteRecursive("/RA/d");
    (void)File_Delete("/RA/a.txt");
    return 0;
}
