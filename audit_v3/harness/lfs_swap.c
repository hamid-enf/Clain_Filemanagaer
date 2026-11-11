/* Audit v3: FILE3-02 edge cases -- what happens on LittleFS when the
 * swap rename-over does its remove-then-rename and the destination is a
 * DIRECTORY, or when the destination is busy, or the source is a dir. */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t    g_mem[2 * 1024 * 1024];
static FM_Storage g_st;
static FM_FS      g_fs;

static void show(const char *p)
{
    FM_Bool f = FM_FALSE, d = FM_FALSE;
    FM_FileInfo info;

    (void)File_Exists(p, &f);
    (void)Directory_Exists(p, &d);
    if (File_GetInfo(p, &info) == FM_OK)
        printf("    %-24s exists=%d dir=%d type=%d size=%u\n", p, (int)f,
               (int)d, (int)info.type, (unsigned)info.size);
    else
        printf("    %-24s exists=%d dir=%d (no info)\n", p, (int)f, (int)d);
}

int main(void)
{
    FM_Result r;

    FileManager_Init();
    memset(g_mem, 0xFF, sizeof(g_mem));
    r = RamDisk_Init(&g_st, "SW", g_mem, sizeof(g_mem), 4096u);
    if (r == FM_OK) r = Storage_Register(&g_st);
    if (r == FM_OK) r = Storage_Mount("SW");
    if (r == FM_OK) r = LittleFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/SW", &g_fs);
    if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

    /* 1. rename a FILE over a NON-EMPTY DIRECTORY */
    (void)File_WriteAll("/SW/file.txt", "file", 4);
    (void)Directory_Create("/SW/dir");
    (void)File_WriteAll("/SW/dir/inner.txt", "inner", 5);
    printf("[1] rename a file over a non-empty directory\n");
    show("/SW/file.txt");
    show("/SW/dir");
    r = File_Rename("/SW/file.txt", "/SW/dir");
    printf("  File_Rename(file -> non-empty dir) -> %s\n",
           FM_ResultString(r));
    show("/SW/file.txt");
    show("/SW/dir");
    show("/SW/dir/inner.txt");

    /* 2. rename a file over an EMPTY directory */
    (void)File_WriteAll("/SW/f2.txt", "f2", 2);
    (void)Directory_Create("/SW/empty");
    printf("\n[2] rename a file over an empty directory\n");
    r = File_Rename("/SW/f2.txt", "/SW/empty");
    printf("  File_Rename(file -> empty dir) -> %s\n", FM_ResultString(r));
    show("/SW/f2.txt");
    show("/SW/empty");

    /* 3. rename a DIRECTORY over a file */
    (void)Directory_Create("/SW/d3");
    (void)File_WriteAll("/SW/target.txt", "target", 6);
    printf("\n[3] rename a directory over a file\n");
    r = File_Rename("/SW/d3", "/SW/target.txt");
    printf("  File_Rename(dir -> file) -> %s\n", FM_ResultString(r));
    show("/SW/d3");
    show("/SW/target.txt");

    /* 4. rename a directory over a non-empty directory */
    (void)Directory_Create("/SW/d4");
    (void)Directory_Create("/SW/d5");
    (void)File_WriteAll("/SW/d5/keep.txt", "keep", 4);
    printf("\n[4] rename a directory over a non-empty directory\n");
    r = File_Rename("/SW/d4", "/SW/d5");
    printf("  File_Rename(dir -> non-empty dir) -> %s\n", FM_ResultString(r));
    show("/SW/d4");
    show("/SW/d5");
    show("/SW/d5/keep.txt");
    return 0;
}
