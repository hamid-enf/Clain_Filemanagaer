/* Audit v3: the LittleFS "one writer or many readers" rule, and whether it
 * can be bypassed through rename / delete / copy / truncate-by-path.
 * Build: ./build_lfs.sh ; run: /tmp/lfs_conflict */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"

static uint8_t g_mem[192 * 1024];
static FM_Storage g_st;
static FM_FS g_fs;

static void dump(const char *path)
{
    char buf[256];
    size_t got = 0;
    FM_Result r = File_ReadAll(path, buf, sizeof(buf), &got);

    printf("  %-14s -> %s, %u bytes:", path, FM_ResultString(r), (unsigned)got);
    for (size_t i = 0; i < got; i++)
    {
        printf(" %02X", (unsigned char)buf[i]);
    }
    printf("  \"%.*s\"\n", (int)got, buf);
}

int main(void)
{
    FM_Result r;
    FM_File *w1;
    FM_File *w2;
    uint32_t wr = 0;
    uint64_t size = 0;

    memset(g_mem, 0xFF, sizeof(g_mem));
    if (FileManager_Init() != FM_OK) return 1;
    r = RamDisk_Init(&g_st, "LFS", g_mem, sizeof(g_mem), 4096);
    printf("RamDisk_Init %s\n", FM_ResultString(r));
    r = Storage_Register(&g_st);
    printf("Storage_Register %s\n", FM_ResultString(r));
    r = Storage_Mount("LFS");
    printf("Storage_Mount %s\n", FM_ResultString(r));
    r = LittleFS_Init(&g_fs);
    printf("LittleFS_Init %s\n", FM_ResultString(r));
    r = FS_Mount("/LFS", &g_fs);
    printf("FS_Mount %s\n", FM_ResultString(r));
    if (r != FM_OK) return 1;

    /* baseline: two writers on one path -- must be refused */
    printf("\n[baseline] two writers on the same path\n");
    (void)File_WriteAll("/LFS/a.txt", "AAAA", 4);
    w1 = File_Open("/LFS/a.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w1 = %s\n", w1 ? "open" : FM_ResultString(File_GetLastError()));
    w2 = File_Open("/LFS/a.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w2 = %s\n", w2 ? "open" : FM_ResultString(File_GetLastError()));
    if (w2) (void)File_Close(w2);

    /* bypass 1: rename the file under the open writer */
    printf("\n[bypass 1] rename the file while w1 is open for writing\n");
    r = File_Rename("/LFS/a.txt", "/LFS/b.txt");
    printf("  File_Rename -> %s\n", FM_ResultString(r));
    w2 = File_Open("/LFS/b.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w2 on the new name = %s\n", w2 ? "open (!)" : FM_ResultString(File_GetLastError()));
    if (w2 != NULL)
    {
        r = File_Write(w1, "BBBBBBBB", 8, &wr);
        printf("  write through w1 -> %s, %u bytes\n", FM_ResultString(r), (unsigned)wr);
        r = File_Write(w2, "CCCCCCCC", 8, &wr);
        printf("  write through w2 -> %s, %u bytes\n", FM_ResultString(r), (unsigned)wr);
        (void)File_Flush(w1);
        (void)File_Flush(w2);
        printf("  w1 close -> %s\n", FM_ResultString(File_Close(w1)));
        printf("  w2 close -> %s\n", FM_ResultString(File_Close(w2)));
        dump("/LFS/b.txt");
        (void)File_SizeOf("/LFS/b.txt", &size);
        printf("  size = %u (16 bytes were written)\n", (unsigned)size);
    }
    else
    {
        (void)File_Close(w1);
    }

    /* bypass 2: reader that was opened before the writer took over */
    printf("\n[bypass 2] open a reader, then a writer, then use both\n");
    (void)File_Delete("/LFS/c.txt");
    (void)File_WriteAll("/LFS/c.txt", "0123456789", 10);
    w1 = File_Open("/LFS/c.txt", FM_OPEN_READ);
    printf("  r1 = %s\n", w1 ? "open" : FM_ResultString(File_GetLastError()));
    w2 = File_Open("/LFS/c.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w2 = %s\n", w2 ? "open" : FM_ResultString(File_GetLastError()));
    if (w2) (void)File_Close(w2);
    if (w1) (void)File_Close(w1);

    /* bypass 3: rename + delete: the old handle keeps writing to a file
     * that no longer has a name, and the name is reused */
    printf("\n[bypass 3] writer open, file renamed away, name recreated\n");
    (void)File_Delete("/LFS/d.txt");
    (void)File_Delete("/LFS/e.txt");
    (void)File_WriteAll("/LFS/d.txt", "DDDD", 4);
    w1 = File_Open("/LFS/d.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w1 = %s\n", w1 ? "open" : FM_ResultString(File_GetLastError()));
    printf("  rename d->e: %s\n", FM_ResultString(File_Rename("/LFS/d.txt", "/LFS/e.txt")));
    w2 = File_Open("/LFS/d.txt", FM_OPEN_WRITE | FM_OPEN_CREATE);
    printf("  w2 on the recreated name = %s\n", w2 ? "open" : FM_ResultString(File_GetLastError()));
    if (w1 && w2)
    {
        r = File_Write(w1, "1111", 4, &wr);
        printf("  write w1 -> %s\n", FM_ResultString(r));
        r = File_Write(w2, "2222", 4, &wr);
        printf("  write w2 -> %s\n", FM_ResultString(r));
        printf("  w1 close -> %s\n", FM_ResultString(File_Close(w1)));
        printf("  w2 close -> %s\n", FM_ResultString(File_Close(w2)));
        dump("/LFS/e.txt");
        dump("/LFS/d.txt");
    }
    else
    {
        if (w1) (void)File_Close(w1);
        if (w2) (void)File_Close(w2);
    }

    /* bypass 4: File_Copy onto the file that is open for writing */
    printf("\n[bypass 4] File_Copy onto the file that is open for writing\n");
    (void)File_Delete("/LFS/f.txt");
    (void)File_Delete("/LFS/src.txt");
    (void)File_WriteAll("/LFS/f.txt", "FFFFFFFF", 8);
    (void)File_WriteAll("/LFS/src.txt", "S", 1);
    w1 = File_Open("/LFS/f.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    printf("  w1 = %s\n", w1 ? "open" : FM_ResultString(File_GetLastError()));
    r = File_Copy("/LFS/src.txt", "/LFS/f.txt", NULL, NULL);
    printf("  File_Copy -> %s\n", FM_ResultString(r));
    if (w1)
    {
        r = File_Write(w1, "XX", 2, &wr);
        printf("  write w1 after the copy -> %s, %u bytes\n", FM_ResultString(r), (unsigned)wr);
        printf("  w1 close -> %s\n", FM_ResultString(File_Close(w1)));
    }
    dump("/LFS/f.txt");

    /* bypass 5: truncate by path while a writer holds the file */
    printf("\n[bypass 5] truncate-by-path while a writer holds the file\n");
    (void)File_Delete("/LFS/g.txt");
    (void)File_WriteAll("/LFS/g.txt", "0123456789", 10);
    w1 = File_Open("/LFS/g.txt", FM_OPEN_WRITE | FM_OPEN_APPEND);
    r = File_WriteAll("/LFS/g.txt", "zz", 2);
    printf("  File_WriteAll (truncating open) -> %s\n", FM_ResultString(r));
    if (w1)
    {
        r = File_Write(w1, "YY", 2, &wr);
        printf("  write w1 -> %s, %u bytes\n", FM_ResultString(r), (unsigned)wr);
        printf("  w1 close -> %s\n", FM_ResultString(File_Close(w1)));
    }
    dump("/LFS/g.txt");

    (void)FS_Unmount("/LFS");
    Storage_Unmount("LFS");
    return 0;
}
