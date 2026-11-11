/* Audit v3: does Text_ReadLine resynchronise after FM_ERR_TOO_LARGE?
 * A 3000-character line followed by "needle" must not make Text_Find
 * report a line number that depends on the buffer size of an internal
 * helper. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;
static char       g_long[4096];

int main(void)
{
    FM_Result r;
    const char *p = "/TR/f.txt";
    size_t n = 0;
    size_t which = 0;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "TR", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("TR");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/TR", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    memset(g_long, 'a', 3000);
    g_long[3000] = '\n';
    memcpy(g_long + 3001, "needle here\n", 12);
    (void)File_WriteAll(p, g_long, 3013);

    printf("file: one 3000-char line, then \"needle here\"\n");
    r = Text_CountLines(p, &n);
    printf("Text_CountLines -> %s, %u (want 2)\n", FM_ResultString(r),
           (unsigned)n);
    r = Text_Find(p, "needle", &which);
    printf("Text_Find(\"needle\") -> %s, line %u (want 2)\n",
           FM_ResultString(r), (unsigned)which);

    /* read the file line by line the way an application would */
    {
        FM_File *f = File_Open(p, FM_OPEN_READ);
        char line[600];
        unsigned calls = 0;
        size_t got = 0;

        printf("line-by-line with a 600-byte buffer:\n");
        while (calls < 20)
        {
            r = Text_ReadLine(f, line, sizeof(line), &got);
            if (r == FM_ERR_EOF) break;
            printf("  call %u: %-14s %4u chars\n", calls, FM_ResultString(r),
                   (unsigned)got);
            calls++;
        }
        (void)File_Close(f);
    }
    (void)File_Delete(p);
    return 0;
}
