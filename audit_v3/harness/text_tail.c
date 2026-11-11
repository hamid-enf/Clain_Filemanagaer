/* Audit v3 / FILE-04 follow-up: what a line rewrite does to the end of a
 * file.  Text_ReadLine documents "a '\r' anywhere else [than before a
 * '\n'] is part of the line and is kept", and nothing documents that a
 * missing final newline is added.  Three backends.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2][1024 * 1024];
static FM_Storage g_st[2];
static FM_FS      g_fs[3];
static char       g_back[256];

static void hex(const char *what, const char *path)
{
    size_t got = 0;
    FM_Result r = File_ReadAll(path, g_back, sizeof(g_back), &got);
    size_t i;

    printf("  %-34s %2u bytes:", what, (unsigned)got);
    for (i = 0; i < got; i++) printf(" %02X", (unsigned char)g_back[i]);
    printf("  (%s)\n", FM_ResultString(r));
}

static void one(const char *mnt)
{
    char p[64];
    FM_File *f;
    char line[64];
    size_t n = 0;
    FM_Result r;

    snprintf(p, sizeof(p), "%s/a.txt", mnt);
    printf("%s\n", mnt);

    /* 1. no final newline */
    (void)File_WriteAll(p, "abc", 3);
    hex("written: 'abc'", p);
    f = File_Open(p, FM_OPEN_READ);
    r = Text_ReadLine(f, line, sizeof(line), &n);
    printf("  ReadLine -> %s '%s'\n", FM_ResultString(r), line);
    (void)File_Close(f);
    r = Text_Replace(p, "NOT-PRESENT", "x");
    printf("  Text_Replace(no match) -> %s\n", FM_ResultString(r));
    hex("after", p);

    /* 2. a file that ends in a lone CR (classic Mac line end) */
    (void)File_WriteAll(p, "abc\r", 4);
    hex("written: 'abc\\r'", p);
    f = File_Open(p, FM_OPEN_READ);
    r = Text_ReadLine(f, line, sizeof(line), &n);
    printf("  ReadLine -> %s '%s' (%u chars)\n", FM_ResultString(r), line,
           (unsigned)n);
    (void)File_Close(f);
    r = Text_Replace(p, "NOT-PRESENT", "x");
    printf("  Text_Replace(no match) -> %s\n", FM_ResultString(r));
    hex("after", p);

    /* 3. a whole classic-Mac file, three lines separated by lone CR */
    (void)File_WriteAll(p, "one\rtwo\rthree\r", 14);
    hex("written: 'one\\rtwo\\rthree\\r'", p);
    r = Text_Replace(p, "NOT-PRESENT", "x");
    printf("  Text_Replace(no match) -> %s\n", FM_ResultString(r));
    hex("after", p);

    /* 4. and CRLF, for reference: this normalisation IS documented */
    (void)File_WriteAll(p, "one\r\ntwo\r\n", 10);
    hex("written: 'one\\r\\ntwo\\r\\n'", p);
    (void)Text_Replace(p, "NOT-PRESENT", "x");
    hex("after", p);

    /* 5. Text_InsertLine keeps a 1-byte file 1 byte long? */
    (void)File_WriteAll(p, "x", 1);
    r = Text_InsertLine(p, 1, "y");
    printf("  InsertLine(1,\"y\") on 'x' -> %s\n", FM_ResultString(r));
    hex("after", p);

    (void)File_Delete(p);
    printf("\n");
}

int main(void)
{
    int k;
    FM_Result r;

    FileManager_Init();
    (void)mkdir("/tmp/tt_root", 0777);
    (void)chdir("/tmp/tt_root");

    for (k = 0; k < 2; k++)
    {
        memset(g_mem[k], k ? 0xFF : 0, sizeof(g_mem[k]));
        r = RamDisk_Init(&g_st[k], k ? "LB" : "LA", g_mem[k], sizeof(g_mem[k]),
                         k ? 4096u : 512u);
        if (r == FM_OK) r = Storage_Register(&g_st[k]);
        if (r == FM_OK) r = Storage_Mount(k ? "LB" : "LA");
        if (r == FM_OK) r = (k == 0) ? FMFS_Init(&g_fs[0]) : LittleFS_Init(&g_fs[1]);
        if (r == FM_OK) r = FS_Mount(k ? "/LB" : "/LA", &g_fs[k]);
        if (r != FM_OK) printf("mount %d: %s\n", k, FM_ResultString(r));
    }
    if (PosixFS_Init(&g_fs[2]) == FM_OK && FS_Mount("/", &g_fs[2]) != FM_OK)
        printf("posix mount failed\n");

    one("/LA");
    one("/LB");
    one("");
    return 0;
}
