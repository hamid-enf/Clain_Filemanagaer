/* Audit v3 FILE3-06: Text_Replace / Text_InsertLine / Text_DeleteLine
 * rewrite the file line by line through Text_ReadLine + Text_WriteLine,
 * and Text_ReadLine drops the CR of every CRLF line end.  So an edit
 * turns a CRLF file into an LF file: every line ending of every line
 * changes, in a call that was asked to change one word.  The CR of a
 * lone CR is kept (FILE-04, fixed); this is the other half.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_text.h"

static uint8_t g_mem[256 * 1024];
static FM_Storage g_st;
static FM_FS g_fs, g_pf;

static void dump(const char *what, const char *p)
{
    unsigned char buf[512];
    size_t got = 0;
    size_t i;
    FM_Result r = File_ReadAll(p, buf, sizeof(buf), &got);

    printf("  %-28s %-20s %3u bytes:", what, FM_ResultString(r), (unsigned)got);
    for (i = 0; i < got && i < 32; i++) printf(" %02X", buf[i]);
    printf("%s\n", got > 32 ? " ..." : "");
}

int main(void)
{
    const char *p = "/LE/f.csv";
    FM_Result r;

    FileManager_Init();
    (void)RamDisk_Init(&g_st, "LE", g_mem, sizeof(g_mem), 512);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("LE");
    (void)FMFS_Init(&g_fs);
    r = FS_Mount("/LE", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    (void)File_WriteAll(p, "id,name\r\n1,alpha\r\n2,beta\r\n", 26);
    dump("written (CRLF, 26 bytes)", p);

    r = Text_Replace(p, "alpha", "ALPHA");
    printf("  Text_Replace(alpha -> ALPHA) -> %s\n", FM_ResultString(r));
    dump("after", p);
    printf("  [the two untouched lines changed their line ending]\n");

    (void)File_WriteAll(p, "a,b\r\nc,d\r\n", 10);
    dump("written again", p);
    r = Text_DeleteLine(p, 1);
    printf("  Text_DeleteLine(1) -> %s\n", FM_ResultString(r));
    dump("after", p);

    /* the same file through Text_ReadLine only: nothing is written, so
     * nothing changes -- the loss comes from the rewrite path */
    (void)File_WriteAll(p, "a,b\r\nc,d\r\n", 10);
    {
        FM_File *f = File_Open(p, FM_OPEN_READ);
        char line[64];

        if (f)
        {
            while (Text_ReadLine(f, line, sizeof(line), NULL) != FM_ERR_EOF)
                printf("  read line: '%s'\n", line);
            (void)File_Close(f);
        }
    }
    dump("after reading only", p);
    return 0;
}
