/* Audit v3 FILE3-06, the rest of the matrix: insert into a CRLF file,
 * a file whose last line has no newline, a mixed CRLF/LF file, an empty
 * file, a file with a lone CR inside a line -- every edit must give back
 * byte-for-byte what it did not change.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_text.h"

static uint8_t g_mem[256 * 1024];
static FM_Storage g_st;
static FM_FS g_fs;
static int g_bad;

static void put(const char *p, const char *t, size_t n) { (void)File_WriteAll(p, t, n); }

static void check(const char *what, const char *p, const char *want, size_t wn)
{
    unsigned char buf[512];
    size_t got = 0;
    size_t i;

    memset(buf, 0, sizeof(buf));
    (void)File_ReadAll(p, buf, sizeof(buf), &got);
    if (got == wn && memcmp(buf, want, wn) == 0)
    {
        printf("  ok   %-46s %3u bytes\n", what, (unsigned)got);
        return;
    }
    g_bad = 1;
    printf("  FAIL %-46s %3u bytes (want %u)\n", what, (unsigned)got,
           (unsigned)wn);
    printf("       got :"); for (i = 0; i < got; i++) printf(" %02X", buf[i]);
    printf("\n       want:"); for (i = 0; i < wn; i++) printf(" %02X", (unsigned char)want[i]);
    printf("\n");
}

int main(void)
{
    const char *p = "/LE2/f.txt";
    FM_Result r;

    FileManager_Init();
    (void)RamDisk_Init(&g_st, "LE2", g_mem, sizeof(g_mem), 512);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("LE2");
    (void)FMFS_Init(&g_fs);
    r = FS_Mount("/LE2", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    /* insert into a CRLF file */
    put(p, "a\r\nb\r\n", 6);
    r = Text_InsertLine(p, 2, "NEW");
    printf("  Text_InsertLine(2) into CRLF -> %s\n", FM_ResultString(r));
    check("CRLF file, a line inserted", p, "a\r\nNEW\r\nb\r\n", 11);

    /* insert at the very end of a CRLF file */
    put(p, "a\r\nb\r\n", 6);
    r = Text_InsertLine(p, 99, "END");
    printf("  Text_InsertLine(99) into CRLF -> %s\n", FM_ResultString(r));
    check("CRLF file, appended at the end", p, "a\r\nb\r\nEND\r\n", 11);

    /* the last line without a newline keeps having none */
    put(p, "one\ntwo", 7);
    r = Text_Replace(p, "one", "ONE");
    printf("  Text_Replace on a file without a final newline -> %s\n",
           FM_ResultString(r));
    check("no final newline stays absent", p, "ONE\ntwo", 7);

    /* mixed endings: each line keeps its own */
    put(p, "a\r\nb\nc\r\n", 8);
    r = Text_Replace(p, "b", "B");
    printf("  Text_Replace on a mixed file -> %s\n", FM_ResultString(r));
    check("mixed CRLF/LF is preserved per line", p, "a\r\nB\nc\r\n", 8);

    /* a lone CR inside a line is data (FILE-04) and survives an edit */
    put(p, "a\rb\nc\n", 6);
    r = Text_Replace(p, "c", "C");
    printf("  Text_Replace with a lone CR in the file -> %s\n",
           FM_ResultString(r));
    check("a lone CR inside a line is kept", p, "a\rb\nC\n", 6);

    /* an empty file gets a line, with an LF */
    put(p, "", 0);
    r = Text_InsertLine(p, 1, "first");
    printf("  Text_InsertLine(1) into an empty file -> %s\n",
           FM_ResultString(r));
    check("empty file, one line inserted", p, "first\n", 6);

    /* deleting the last line of a CRLF file leaves no stray ending */
    put(p, "a\r\nb\r\n", 6);
    r = Text_DeleteLine(p, 2);
    printf("  Text_DeleteLine(2) of a CRLF file -> %s\n", FM_ResultString(r));
    check("CRLF file, last line deleted", p, "a\r\n", 3);

    printf("\nline_end2: %s\n", g_bad ? "FAIL" : "PASS");
    return g_bad;
}
