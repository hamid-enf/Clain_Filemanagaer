/* Audit v3 FILE3-01, both directions: "delete file" on a directory and
 * "delete directory" on a file, on every backend that can be mounted on
 * the host.  The adapter contract (fm_filesystem.h) is:
 *   delete_file: files only        rmdir: empty dirs only
 * so both calls must be refused with IS_DIRECTORY / NOT_DIRECTORY and
 * must not remove anything.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "file_manager.h"

static uint8_t    g_mem[3][256 * 1024];
static FM_Storage g_ram[3];
static FM_FS      g_fmfs, g_lfs;

static void one(const char *label, const char *mount)
{
    char dir[64], file[64];
    FM_Result rd, rf;
    FM_Bool ex_dir = FM_FALSE, ex_file = FM_FALSE;

    (void)snprintf(dir, sizeof(dir), "%s/delkind_dir", mount);
    (void)snprintf(file, sizeof(file), "%s/delkind_file.txt", mount);

    (void)Directory_Create(dir);
    (void)File_WriteText(file, "abc");

    rf = File_Delete(dir);                    /* file API on a directory */
    rd = Directory_Delete(file);              /* dir API on a file       */
    (void)Directory_Exists(dir, &ex_dir);
    (void)File_Exists(file, &ex_file);

    printf("%-8s File_Delete(dir)  -> %-18s dir survives: %s\n", label,
           FM_ResultString(rf), ex_dir ? "yes" : "NO (!)");
    printf("%-8s Directory_Delete(file) -> %-15s file survives: %s\n", label,
           FM_ResultString(rd), ex_file ? "yes" : "NO (!)");
    printf("%-8s   [want is a directory / not a directory, both survive]\n",
           label);
}

int main(void)
{
    FM_Result r;

    FileManager_Init();
    (void)RamDisk_Init(&g_ram[0], "DK", g_mem[0], sizeof(g_mem[0]), 512);
    (void)Storage_Register(&g_ram[0]);
    (void)Storage_Mount("DK");
    (void)FMFS_Init(&g_fmfs);
    r = FS_Mount("/DK", &g_fmfs);
    if (r != FM_OK) { printf("fmfs mount: %s\n", FM_ResultString(r)); return 2; }
    one("FMFS", "/DK");

    (void)RamDisk_Init(&g_ram[1], "DL", g_mem[1], sizeof(g_mem[1]), 512);
    (void)Storage_Register(&g_ram[1]);
    (void)Storage_Mount("DL");
    (void)LittleFS_Init(&g_lfs);
    r = FS_Mount("/DL", &g_lfs);
    if (r != FM_OK) { printf("lfs mount: %s\n", FM_ResultString(r)); return 2; }
    one("LittleFS", "/DL");

    /* POSIX is mounted at the root of a scratch directory. */
    {
        static FM_FS pf;

        (void)mkdir("/tmp/fm_delkind_root", 0777);
        (void)chdir("/tmp/fm_delkind_root");
        r = PosixFS_Init(&pf);
        if (r == FM_OK) r = FS_Mount("/", &pf);
        if (r != FM_OK) { printf("posix mount: %s\n", FM_ResultString(r)); return 2; }
        one("POSIX", "");
    }
    return 0;
}
