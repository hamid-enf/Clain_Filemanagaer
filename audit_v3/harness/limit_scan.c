/* Audit v3: the exact line length at which Text_CountLines and Text_Find
 * give up, on FMFS.  Zero is fine for the disk itself. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;
static char       g_line[1024];

int main(void)
{
    FM_Result r;
    const char *p = "/LS/f.txt";
    unsigned len;
    unsigned count_limit = 0, find_limit = 0;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "LS", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("LS");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/LS", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    for (len = 1; len <= 600; len++)
    {
        size_t n = 0, line = 0;

        memset(g_line, 'a', len);
        g_line[len] = '\n';
        g_line[len + 1] = 0;
        (void)File_WriteAll(p, g_line, len + 1);
        (void)File_AppendText(p, "needle\n");
        if (Text_CountLines(p, &n) == FM_OK) count_limit = len;
        if (Text_Find(p, "needle", &line) == FM_OK && line == 2u)
            find_limit = len;
    }
    printf("longest line Text_CountLines can count: %u (it uses FM_NAME_MAX*2 "
           "= %d)\n", count_limit, (int)(FM_NAME_MAX * 2));
    printf("longest line Text_Find can look through: %u (FM_INTERNAL_BUFFER_"
           "SIZE = %d)\n", find_limit, (int)FM_INTERNAL_BUFFER_SIZE);
    (void)File_Delete(p);
    return 0;
}
