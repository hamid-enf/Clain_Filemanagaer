/* Audit v3: fm_text.c line editing at the edges: the first line, the last
 * line, a line longer than the buffer, a file without a final newline,
 * CRLF, lone CR, an empty file.  Every rewrite is read back and compared
 * with the expected bytes.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_text.h"

static char g_back[4096];
static size_t n_lines;
static int g_bad;

static void put(const char *path, const char *text, size_t n)
{
    (void)File_WriteAll(path, text, n);
}

static void expect(const char *what, const char *path, const char *want,
                   size_t wn)
{
    (void)wn;
    size_t got = 0;
    FM_Result r;

    memset(g_back, 0, sizeof(g_back));
    wn = strlen(want);
    r = File_ReadAll(path, g_back, sizeof(g_back) - 1, &got);
    if (r != FM_OK || got != wn || memcmp(g_back, want, wn) != 0)
    {
        size_t i;

        printf("FAIL %s: %s, %u bytes (want %u)\n", what, FM_ResultString(r),
               (unsigned)got, (unsigned)wn);
        printf("  got :");
        for (i = 0; i < got && i < 60; i++)
            printf(" %02X", (unsigned char)g_back[i]);
        printf("\n  want:");
        for (i = 0; i < wn && i < 60; i++)
            printf(" %02X", (unsigned char)want[i]);
        printf("\n");
        g_bad = 1;
    }
}

static void read_lines(const char *what, const char *path, size_t bufsize,
                       unsigned maxlines)
{
    FM_File *f = File_Open(path, FM_OPEN_READ);
    char line[256];
    unsigned i;
    size_t n;

    printf("%-42s", what);
    if (f == NULL)
    {
        printf(" open failed: %s\n", FM_ResultString(File_GetLastError()));
        g_bad = 1;
        return;
    }
    for (i = 0; i < maxlines; i++)
    {
        FM_Result r = Text_ReadLine(f, line, bufsize, &n);

        if (r != FM_OK)
        {
            printf(" [%s]", FM_ResultString(r));
            break;
        }
        printf(" '%s'", line);
    }
    printf("\n");
    (void)File_Close(f);
}

static uint8_t    g_mem[1024 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

static int mount_ramdisk(void)
{
    FM_Result r;

    memset(g_mem, 0, sizeof(g_mem));
    r = RamDisk_Init(&g_st, "TA", g_mem, sizeof(g_mem), 512u);
    printf("RamDisk_Init %s\n", FM_ResultString(r));
    if (r == FM_OK) r = Storage_Register(&g_st);
    printf("Storage_Register %s\n", FM_ResultString(r));
    if (r == FM_OK) r = Storage_Mount("TA");
    printf("Storage_Mount %s\n", FM_ResultString(r));
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    printf("FMFS_Init %s\n", FM_ResultString(r));
    if (r == FM_OK) r = FS_Mount("/TA", &g_fs);
    printf("FS_Mount %s\n", FM_ResultString(r));
    return (r == FM_OK) ? 0 : 1;
}

int main(void)
{
    const char *p = "/TA/txt_audit.txt";

    FileManager_Init();
    if (mount_ramdisk() != 0) return 2;

    /* 1. lines, CRLF, lone CR, no final newline, empty */
    put(p, "a\nb\n", 4);
    read_lines("'a\\nb\\n'", p, 64, 5);
    put(p, "a\r\nb\r\n", 6);
    read_lines("'a\\r\\nb\\r\\n' (CRLF)", p, 64, 5);
    put(p, "a\rb\r", 4);
    read_lines("'a\\rb\\r' (lone CR, classic Mac)", p, 64, 5);
    put(p, "abc", 3);
    read_lines("'abc' (no final newline)", p, 64, 5);
    put(p, "", 0);
    read_lines("empty file", p, 64, 5);
    put(p, "\n", 1);
    read_lines("a single newline", p, 64, 5);
    put(p, "\r\n", 2);
    read_lines("a single CRLF", p, 64, 5);
    put(p, "0123456789\n", 11);
    read_lines("buffer smaller than the line", p, 5, 3);
    put(p, "a\n\n\nb\n", 6);
    read_lines("empty lines in the middle", p, 64, 6);

    /* 2. rewriting: a needle that does not occur must not change the file */
    {
        static const char orig[] = "one\r\ntwo\rXthree\nfour\r\n";
        size_t n = sizeof(orig) - 1;
        FM_Result r;

        put(p, orig, n);
        r = Text_Replace(p, "ZZZ", "Y");
        printf("Text_Replace(no match) -> %s\n", FM_ResultString(r));
        expect("Text_Replace with no match keeps the file", p, orig, n);
    }

    /* 3. first / last / only line */
    put(p, "one\ntwo\nthree\n", 14);
    printf("Text_Replace(first line) -> %s\n",
           FM_ResultString(Text_Replace(p, "one", "ONE")));
    expect("first line replaced", p, "ONE\ntwo\nthree\n", 14);
    printf("Text_Replace(last line) -> %s\n",
           FM_ResultString(Text_Replace(p, "three", "THREE")));
    expect("last line replaced", p, "ONE\ntwo\nTHREE\n", 14);
    put(p, "only", 4);
    printf("Text_Replace(only line, no newline) -> %s\n",
           FM_ResultString(Text_Replace(p, "only", "ONLY")));
    expect("a file without a final newline", p, "ONLY", 4);

    /* 4. line insert / delete at both ends */
    put(p, "a\nb\n", 4);
    printf("Text_InsertLine(0) -> %s (lines are 1-based, so 0 is rejected)\n",
           FM_ResultString(Text_InsertLine(p, 0, "first")));
    expect("a rejected insert leaves the file alone", p, "a\nb\n", 0);
    printf("Text_InsertLine(1) -> %s\n",
           FM_ResultString(Text_InsertLine(p, 1, "first")));
    expect("insert as the first line", p, "first\na\nb\n", 0);
    printf("Text_InsertLine(99) -> %s\n",
           FM_ResultString(Text_InsertLine(p, 99, "last")));
    expect("insert past the end", p, "first\na\nb\nlast\n", 0);
    printf("Text_DeleteLine(0) -> %s\n",
           FM_ResultString(Text_DeleteLine(p, 0)));
    expect("a rejected delete leaves the file alone", p,
           "first\na\nb\nlast\n", 0);
    printf("Text_DeleteLine(1) -> %s\n",
           FM_ResultString(Text_DeleteLine(p, 1)));
    expect("delete the first line", p, "a\nb\nlast\n", 0);
    printf("Text_DeleteLine(3) -> %s\n",
           FM_ResultString(Text_DeleteLine(p, 3)));
    expect("delete the last line", p, "a\nb\n", 4);
    printf("Text_DeleteLine(99) -> %s\n",
           FM_ResultString(Text_DeleteLine(p, 99)));
    expect("delete past the end", p, "a\nb\n", 4);

    /* 5. what the other helpers do with line endings */
    put(p, "one\r\ntwo\r\n", 10);
    {
        FM_Result r = Text_CountLines(p, &n_lines);

        printf("Text_CountLines('one\\r\\ntwo\\r\\n') -> %s, %u lines\n",
               FM_ResultString(r), (unsigned)n_lines);
    }
    put(p, "abc", 3);
    {
        FM_Result r = Text_CountLines(p, &n_lines);

        printf("Text_CountLines('abc') -> %s, %u lines\n",
               FM_ResultString(r), (unsigned)n_lines);
    }
    printf("Text_AppendLine(no final newline) -> %s\n",
           FM_ResultString(Text_AppendLine(p, "def")));
    expect("AppendLine after a line with no newline", p, "abcdef\n", 7);

    (void)File_Delete(p);
    printf("\n%s\n", g_bad ? "FAIL" : "all text cases as expected");
    return g_bad;
}
