/* Audit v3 / FILE3-04b: what the streaming rewrites do to a line that is
 * longer than their internal buffer (FM_INTERNAL_BUFFER_SIZE).
 * A line that does not fit must survive; only the needle inside it may
 * stay unreplaced, and that must be documented behaviour, not silent loss.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[512 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;
static char       g_long[2048];
static char       g_back[4096];

static void show(const char *what, const char *p)
{
    size_t got = 0;
    FM_Result r = File_ReadAll(p, g_back, sizeof(g_back), &got);

    printf("  %-34s %-20s %5u bytes", what, FM_ResultString(r), (unsigned)got);
    if (got > 0)
    {
        /* a cheap fingerprint: length + how the tail ends */
        printf("  first=%02X last=%02X tail='%.4s'",
               (unsigned char)g_back[0],
               (unsigned char)g_back[got - 1],
               g_back + (got > 4 ? got - 4 : 0));
    }
    printf("\n");
}

int main(void)
{
    FM_Result r;
    const char *p = "/LW/f.txt";
    unsigned i;

    FileManager_Init();
    r = RamDisk_Init(&g_st, "LW", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("LW");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/LW", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    printf("FM_INTERNAL_BUFFER_SIZE = %d\n", (int)FM_INTERNAL_BUFFER_SIZE);

    /* a 1500-character line, then a short one that needs replacing */
    memset(g_long, 'A', 1500);
    g_long[1500] = '\n';
    memcpy(g_long + 1501, "name=old\n", 9);
    (void)File_WriteAll(p, g_long, 1510);
    show("written: 1500 x 'A' + name=old", p);

    r = Text_Replace(p, "old", "new");
    printf("  Text_Replace(old -> new) -> %s\n", FM_ResultString(r));
    show("after", p);
    printf("  (the 1500-char line must still be 1500 chars)\n");

    /* a file whose ONLY line is too long, replaced / inserted / deleted */
    (void)File_WriteAll(p, g_long, 1501);
    r = Text_Replace(p, "A", "B");
    printf("  Text_Replace on a too-long line only -> %s\n", FM_ResultString(r));
    show("after", p);

    (void)File_WriteAll(p, g_long, 1501);
    r = Text_InsertLine(p, 1, "first");
    printf("  Text_InsertLine(1) -> %s\n", FM_ResultString(r));
    show("after", p);

    (void)File_WriteAll(p, g_long, 1501);
    r = Text_DeleteLine(p, 1);
    printf("  Text_DeleteLine(1) on the only (long) line -> %s\n",
           FM_ResultString(r));
    show("after (0 bytes expected)", p);

    /* two long lines: delete the FIRST, the second must stay intact */
    {
        static char two[4096];
        size_t n = 0;

        for (i = 0; i < 700; i++) two[n++] = 'a';
        two[n++] = '\n';
        for (i = 0; i < 700; i++) two[n++] = 'b';
        two[n++] = '\n';
        (void)File_WriteAll(p, two, n);
        r = Text_DeleteLine(p, 1);
        printf("  Text_DeleteLine(1) of two 700-char lines -> %s\n",
               FM_ResultString(r));
        show("after (701 bytes expected)", p);
    }
    (void)File_Delete(p);
    return 0;
}
