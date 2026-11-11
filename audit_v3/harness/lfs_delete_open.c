/* Audit v3 FILE3-xx: does delete/rename-over of a file that is OPEN for
 * writing destroy the data the open handle still owes the caller?
 *
 * The LittleFS adapter holds one writer per file (FILE-02 rule).  The rule
 * is keyed on the file's *name* at open time (lfs_dir_find), so any path
 * operation that moves or removes that name takes the file out of the
 * table while the handle stays.
 *
 * Build: ./build_del.sh ; run: /tmp/lfs_delete_open
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t g_mem[192 * 1024];
static FM_Storage g_st;
static FM_FS g_fs;

static void dump(const char *path)
{
    static char buf[512];
    size_t got = 0;
    FM_Result r;
    size_t i;

    memset(buf, 0, sizeof(buf));
    r = File_ReadAll(path, buf, sizeof(buf) - 1u, &got);
    printf("  %-18s -> %-16s %3u bytes:", path, FM_ResultString(r),
           (unsigned)got);
    for (i = 0; i < got && i < 24u; i++)
    {
        printf(" %02X", (unsigned char)buf[i]);
    }
    printf("\n");
}

int main(void)
{
    FM_Result r;
    FM_File *w;
    FM_File *rd;
    uint32_t wr = 0;
    uint32_t got = 0;
    char buf[128];
    FM_Bool exists = FM_TRUE;

    memset(g_mem, 0xFF, sizeof(g_mem));
    if (FileManager_Init() != FM_OK) return 1;
    (void)RamDisk_Init(&g_st, "LFS", g_mem, sizeof(g_mem), 4096);
    (void)Storage_Register(&g_st);
    (void)Storage_Mount("LFS");
    r = LittleFS_Init(&g_fs);
    if (r == FM_OK) r = FS_Mount("/LFS", &g_fs);
    printf("mount: %s\n", FM_ResultString(r));

    /* ---- case A: delete the file while a writer has it open ---- */
    printf("\n[A] delete a file that is open for writing\n");
    (void)File_WriteAll("/LFS/A.txt", "AAAA", 4);
    w = File_Open("/LFS/A.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w = %s\n", w ? "open" : FM_ResultString(File_GetLastError()));
    r = File_Delete("/LFS/A.txt");
    printf("  File_Delete -> %s\n", FM_ResultString(r));
    r = File_Exists("/LFS/A.txt", &exists);
    printf("  File_Exists -> %s exists=%d\n", FM_ResultString(r), (int)exists);
    if (w != NULL)
    {
        r = File_Write(w, "NEWDATA", 7, &wr);
        printf("  File_Write through the open handle -> %s, %u bytes\n",
               FM_ResultString(r), (unsigned)wr);
        r = File_Close(w);
        printf("  File_Close -> %s\n", FM_ResultString(r));
    }
    dump("/LFS/A.txt");

    /* ---- case B: rename-over an empty file that is open for writing ---- */
    printf("\n[B] rename over a file that is open for writing\n");
    (void)File_Delete("/LFS/B.txt");
    (void)File_Delete("/LFS/B2.txt");
    (void)File_WriteAll("/LFS/B.txt", "BBBB", 4);
    (void)File_WriteAll("/LFS/B2.txt", "ZZ", 2);
    w = File_Open("/LFS/B.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w = %s\n", w ? "open" : FM_ResultString(File_GetLastError()));
    r = File_Rename("/LFS/B2.txt", "/LFS/B.txt");   /* POSIX: replaces B */
    printf("  File_Rename(B2 -> B) -> %s\n", FM_ResultString(r));
    if (w != NULL)
    {
        r = File_Write(w, "LOST", 4, &wr);
        printf("  write through the old handle -> %s, %u bytes\n",
               FM_ResultString(r), (unsigned)wr);
        r = File_Close(w);
        printf("  close -> %s\n", FM_ResultString(r));
    }
    dump("/LFS/B.txt");

    /* ---- case C: a reader open across a delete ---- */
    printf("\n[C] delete a file that is open for reading\n");
    (void)File_WriteAll("/LFS/C.txt", "0123456789", 10);
    rd = File_Open("/LFS/C.txt", FM_OPEN_READ);
    r = File_Read(rd, buf, 4, &got);
    printf("  read 4 -> %s, %u bytes\n", FM_ResultString(r), (unsigned)got);
    printf("  File_Delete -> %s\n", FM_ResultString(File_Delete("/LFS/C.txt")));
    r = File_Read(rd, buf, 4, &got);
    printf("  read 4 more -> %s, %u bytes\n", FM_ResultString(r), (unsigned)got);
    { uint64_t sz = 0; r = File_Size(rd, &sz); printf("  File_Size -> %s, %u\n", FM_ResultString(r), (unsigned)sz); }
    printf("  File_Size\n");
    (void)File_Close(rd);
    dump("/LFS/C.txt");

    /* ---- case D: the same for a directory entry that is open ---- */
    printf("\n[D] File_Copy onto a file that is open for writing (data sizes)\n");
    (void)File_WriteAll("/LFS/D.txt", "0123456789", 10);
    (void)File_WriteAll("/LFS/Ds.txt", "SSS", 3);
    w = File_Open("/LFS/D.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w = %s\n", w ? "open" : FM_ResultString(File_GetLastError()));
    r = File_Copy("/LFS/Ds.txt", "/LFS/D.txt", NULL, NULL);
    printf("  File_Copy -> %s\n", FM_ResultString(r));
    if (w != NULL)
    {
        r = File_Write(w, "XY", 2, &wr);
        printf("  write through w -> %s, %u bytes\n", FM_ResultString(r),
               (unsigned)wr);
        (void)File_Close(w);
    }
    dump("/LFS/D.txt");

    (void)FS_Unmount("/LFS");
    (void)Storage_Unmount("LFS");
    return 0;
}
