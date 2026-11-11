/* Audit v3 / FILE3-04 reproducer: Text_CountLines() and Text_Find() use a
 * line buffer of FM_NAME_MAX*2 bytes (FM_INTERNAL_BUFFER_SIZE in the
 * other helpers), so they stop at a 128-byte line even though
 * Text_ReadLine() itself handles any length given a buffer.
 * Deterministic: no sanitizer needed, exit code 1. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t    g_mem[256 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

int main(void)
{
    FM_Result r;
    const char *p = "/DL/log.txt";
    size_t n = 0, line = 0;
    static char text[320];

    FileManager_Init();
    r = RamDisk_Init(&g_st, "DL", g_mem, sizeof(g_mem), 512u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("DL");
    if (r == FM_OK) r = FMFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/DL", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    /* a 300-character line (one CSV row), then two normal ones */
    memset(text, 'x', 300);
    text[300] = '\n';
    (void)File_WriteAll(p, text, 301);
    (void)File_AppendText(p, "second line\nthird line\n");

    r = Text_CountLines(p, &n);
    printf("Text_CountLines(3 lines, first 300 chars) -> %s, count=%u "
           "(want OK, 3)\n", FM_ResultString(r), (unsigned)n);
    r = Text_Find(p, "third", &line);
    printf("Text_Find(\"third\")                     -> %s, line=%u "
           "(want OK, 3)\n", FM_ResultString(r), (unsigned)line);
    (void)File_Delete(p);
    return (r == FM_OK && line == 3u) ? 0 : 1;
}
