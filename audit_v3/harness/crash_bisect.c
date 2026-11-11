/* Minimal bisect of the SEGV seen in walk.c:
 *   FMFS: build a 17-level tree, let DeleteRecursive fail with TOO_LARGE,
 *   then keep using the volume.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"

static uint8_t mem[160 * 1024];
static FM_Storage st;
static FM_FS fs;

int main(void)
{
    char path[FM_PATH_MAX];
    char f[FM_PATH_MAX];
    FM_Result r;
    unsigned k;

    memset(mem, 0, sizeof(mem));
    FileManager_Init();
    RamDisk_Init(&st, "FA", mem, sizeof(mem), 512);
    Storage_Register(&st);
    Storage_Mount("FA");
    FMFS_Init(&fs);
    printf("mount %s\n", FM_ResultString(FS_Mount("/FA", &fs)));

    /* 17 levels: one more than FM_DIR_MAX_DEPTH */
    strcpy(path, "/FA/dp");
    printf("mkdir %s\n", FM_ResultString(Directory_Create(path)));
    for (k = 0; k < FM_DIR_MAX_DEPTH + 1u; k++)
    {
        strcat(path, "/d");
        r = Directory_Create(path);
        printf("  mkdir level %u -> %s\n", k + 1, FM_ResultString(r));
        if (r != FM_OK) break;
    }
    strcat(path, "/f.txt");
    printf("write deep file -> %s\n", FM_ResultString(File_WriteAll(path, "x", 1)));
    {
        FM_Bool ex = FM_TRUE;

        printf("Exists(/FA/wide) before -> %s\n",
               FM_ResultString(Directory_Exists("/FA/wide", &ex)));
        printf("Create(/FA/wide) before -> %s\n",
               FM_ResultString(Directory_Create("/FA/wide")));
        {
            FM_FileInfo info;

            memset(&info, 0, sizeof(info));
            printf("GetInfo(/FA/wide) 1 -> %s type=%d\n",
                   FM_ResultString(File_GetInfo("/FA/wide", &info)),
                   (int)info.type);
        }
        {
            FM_Dir *d = Directory_Open("/FA/wide");
            FM_DirEntry e;

            printf("Open(/FA/wide) -> %p\n", (void *)d);
            if (d)
            {
                FM_Result q = Directory_Read(d, &e);

                printf("  Read -> %s name='%s'\n", FM_ResultString(q), e.name);
                (void)Directory_Close(d);
            }
        }
        printf("Create(/FA/dp) -> %s\n",
               FM_ResultString(Directory_Create("/FA/dp")));
        printf("DeleteRecursive(dp) -> %s\n",
               FM_ResultString(Directory_DeleteRecursive("/FA/dp")));
        printf("GetInfo(/FA/wide) -> ");
        {
            FM_FileInfo info;

            memset(&info, 0, sizeof(info));
            printf("%s type=%d\n",
                   FM_ResultString(File_GetInfo("/FA/wide", &info)),
                   (int)info.type);
        }
    }
    fflush(stdout);

    /* now ordinary work on the same volume */
    printf("create wide -> %s\n", FM_ResultString(Directory_Create("/FA/wide")));
    for (k = 0; k < 40; k++)
    {
        snprintf(f, sizeof(f), "/FA/wide/sub%02u", k);
        r = Directory_Create(f);
        printf("  mkdir %u -> %s\n", k, FM_ResultString(r));
        snprintf(f, sizeof(f), "/FA/wide/sub%02u/a.bin", k);
        fflush(stdout);
        r = File_WriteAll(f, "12345", 5);
        printf("    write -> %s\n", FM_ResultString(r));
        fflush(stdout);
    }
    return 0;
}
