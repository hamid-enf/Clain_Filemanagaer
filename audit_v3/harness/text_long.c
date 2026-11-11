/* Audit v3: text helpers on ordinary files whose lines are longer than the
 * internal name-sized buffer (a 200-byte line is not exotic). */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

int main(void)
{
    FM_Result r;
    const char *p = "/TO/f.txt";
    static char big[610];
    size_t n = 0;
    unsigned i;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "TO", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("TO");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/TO", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    printf("FM_NAME_MAX=%d FM_INTERNAL_BUFFER_SIZE=%d\n", (int)FM_NAME_MAX,
           (int)FM_INTERNAL_BUFFER_SIZE);

    /* three lines of 200 characters each */
    for (i = 0; i < 200; i++) big[i] = 'x';
    big[200] = '\n';
    for (i = 201; i < 401; i++) big[i] = 'y';
    big[401] = '\n';
    for (i = 402; i < 602; i++) big[i] = 'z';
    big[602] = '\n';
    (void)File_WriteAll(p, big, 603);

    r = Text_CountLines(p, &n);
    printf("Text_CountLines(3 lines of 200 chars) -> %s, %u\n",
           FM_ResultString(r), (unsigned)n);
    {
        size_t which = 0;

        r = Text_Find(p, "yyy", &which);
        printf("Text_Find(\"yyy\") on a 200-char line -> %s, line %u\n",
               FM_ResultString(r), (unsigned)which);
    }
    {
        char line[64];
        FM_File *f = File_Open(p, FM_OPEN_READ);
        size_t got = 0;

        r = Text_ReadLine(f, line, 51, &got);      /* a 64-byte caller buffer */
        printf("Text_ReadLine(line_size=51) on a 200-char line -> %s, %u "
               "chars\n", FM_ResultString(r), (unsigned)got);
        r = Text_ReadLine(f, line, sizeof(line), &got);
        printf("  (remaining of the same line) -> %s, %u chars\n",
               FM_ResultString(r), (unsigned)got);
        (void)File_Close(f);
    }
    {
        /* Text_Replace on such a file */
        r = Text_Replace(p, "yyy", "Y");
        printf("Text_Replace on 200-char lines -> %s\n", FM_ResultString(r));
        r = File_SizeOf(p, (uint64_t *)&n);
        printf("  file still there: %s, %u bytes\n", FM_ResultString(r),
               (unsigned)n);
    }
    (void)File_Delete(p);
    return 0;
}
