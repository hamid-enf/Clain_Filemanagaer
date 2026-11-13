/*
 * Host test: drives the FileManager FatFS adapter (backends/fm_fatfs.c)
 * against the ff_mock simulator, replaying the exact sequence of the
 * on-target example SdCard_FileBasic() plus mount/unmount cycles,
 * advanced handle ops, directory ops and info queries.
 *
 * Build & run:  see test/build_fatfs_host.sh  (ASan + UBSan)
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "file_manager.h"
#include "fm_fatfs.h"
#include "ff.h"

static int g_failures = 0;
static int g_tests = 0;

#define TEST(name) do { g_tests++; printf("  TEST %-50s ... ", name); } while (0)
#define PASS()     do { printf("PASS\n"); } while (0)
#define FAIL(m)    do { g_failures++; printf("FAIL: %s\n", (m)); } while (0)
#define CHECK(r, msg)                                                     \
    do {                                                                  \
        if ((r) != FM_OK) {                                               \
            char eb[160];                                                 \
            snprintf(eb, sizeof(eb), "%s (err=%s)", (msg),                \
                     File_GetErrorString((FM_Result)(r)));                \
            FAIL(eb);                                                     \
        } else PASS();                                                    \
    } while (0)
#define CHECK_TRUE(c, msg) do { if (c) PASS(); else FAIL(msg); } while (0)

static FM_FS      g_sd_fs;
static FM_Storage g_ram_storage;
static FM_FS      g_ram_fs;
static uint8_t    g_ram_buf[32 * 1024];

/* exact replica of the on-target SdCard_FileBasic() body */
static void sdcard_file_basic(FM_Result *init_r, FM_Result *mount_r)
{
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    *init_r = FileManager_Init();

    *init_r = FatFS_Init(&g_sd_fs, 0);
    if (*init_r != FM_OK) {
        printf("FatFS_Init Failed!\n");
        return;
    }

    *mount_r = FS_Mount("/", &g_sd_fs);
    if (*mount_r != FM_OK) {
        printf("FS_Mount Failed! (err=%s)\n",
               File_GetErrorString(*mount_r));
        return;
    }

    File_WriteText("/greeting.txt", "Hello from SD Card!");
    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    File_Exists("/greeting.txt", &exists);
    File_SizeOf("/greeting.txt", &size);
    printf("  read back: '%s' (%llu bytes, exists=%d)\n", buf,
           (unsigned long long)size, (int)exists);

    File_WriteText("/greeting.txt", "New SD Card Content!");
    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("  overwrite: '%s'\n", buf);

    File_Create("/empty.txt");
    File_SizeOf("/empty.txt", &size);
    printf("  empty size: %llu\n", (unsigned long long)size);

    File_Delete("/greeting.txt");
    File_Exists("/greeting.txt", &exists);
    printf("  deleted: exists=%d\n", (int)exists);

    FS_Unmount("/");
    FileManager_Deinit();
}

int main(void)
{
    /* Unbuffered: if the adapter crashes, the last line names the test
     * that was running. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== FatFS adapter host test ==\n");

    /* ---------------------------------------------------------- */
    printf("[1] SdCard_FileBasic replica\n");
    {
        FM_Result ir = FM_OK, mr = FM_OK;
        TEST("FatFS_Init + FS_Mount(/)");
        sdcard_file_basic(&ir, &mr);
        CHECK(ir, "FatFS_Init");
        CHECK(mr, "FS_Mount");
    }

    /* verify with a second mount that the write really happened once */
    printf("[2] persistence + overwrite + delete through adapter\n");
    {
        FM_Result r;
        char buf[128];
        FM_Bool exists = FM_FALSE;
        uint64_t size = 0;

        TEST("re-mount after unmount");
        r = FatFS_Init(&g_sd_fs, 0);
        CHECK(r, "FatFS_Init #2");
        r = FS_Mount("/", &g_sd_fs);
        CHECK(r, "FS_Mount #2");

        TEST("deleted file is gone");
        File_Exists("/greeting.txt", &exists);
        CHECK_TRUE(!exists, "greeting.txt should be deleted");

        TEST("write + read text");
        r = File_WriteText("/greeting.txt", "Hello from SD Card!");
        CHECK(r, "File_WriteText");
        r = File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
        CHECK(r, "File_ReadText");
        CHECK_TRUE(strcmp(buf, "Hello from SD Card!") == 0,
                   "content mismatch");

        TEST("overwrite truncates (CREATE_ALWAYS semantics)");
        r = File_WriteText("/greeting.txt", "short");
        CHECK(r, "File_WriteText overwrite");
        r = File_SizeOf("/greeting.txt", &size);
        CHECK(r, "File_SizeOf");
        CHECK_TRUE(size == 5, "overwrite must truncate to 5 bytes");

        TEST("append keeps content");
        r = File_AppendAll("/greeting.txt", "!", 1);
        CHECK(r, "File_AppendAll");
        r = File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
        CHECK_TRUE(strcmp(buf, "short!") == 0, "append mismatch");

        (void)File_Delete("/greeting.txt");
        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[2b] FS_Format (f_mkfs version dispatch)\n");
    {
        FM_Result r;

        FatFS_Init(&g_sd_fs, 0);
        r = FS_Mount("/", &g_sd_fs);
        CHECK(r, "FS_Mount for format");

        TEST("FS_Format(\"/\")");
        r = FS_Format("/");
        CHECK(r, "FS_Format");

        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[3] advanced handle API (FileDemo_Advanced equivalent)\n");
    {
        FM_Result r;
        FM_File *f;
        uint64_t pos = 0, sz = 0;
        char b4[8];
        uint32_t got = 0;

        FatFS_Init(&g_sd_fs, 0);
        FS_Mount("/", &g_sd_fs);

        f = File_Open("/adv.bin", FM_OPEN_READ | FM_OPEN_WRITE |
                                  FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
        TEST("File_Open READ|WRITE|CREATE|TRUNCATE");
        CHECK_TRUE(f != NULL, "open failed");

        r = File_Write(f, "AAAABBBBCCCC", 12, NULL);
        TEST("File_Write 12 bytes");
        CHECK(r, "File_Write");

        r = File_Tell(f, &pos);
        TEST("File_Tell after write == 12");
        CHECK(r, "File_Tell");
        CHECK_TRUE(pos == 12, "pos != 12");

        r = File_Seek(f, 4, FM_SEEK_SET);
        r = File_Read(f, b4, 4, &got);
        TEST("seek+read at offset 4");
        CHECK(r, "File_Read");
        CHECK_TRUE(got == 4 && memcmp(b4, "BBBB", 4) == 0, "wrong bytes");

        r = File_Truncate(f, 6);
        TEST("File_Truncate to 6");
        CHECK(r, "File_Truncate");

        r = File_Size(f, &sz);
        CHECK(r, "File_Size");
        CHECK_TRUE(sz == 6, "size != 6");

        r = File_Append(f, "DD", 2, NULL);
        CHECK(r, "File_Append");

        r = File_Close(f);
        CHECK(r, "File_Close");

        {
            FM_File *fr2 = File_Open("/adv.bin", FM_OPEN_READ);
            TEST("read back truncated file");
            CHECK_TRUE(fr2 != NULL, "reopen failed");
            got = 0;
            r = File_Read(fr2, b4, 8, &got);
            CHECK(r, "File_Read");
            CHECK_TRUE(got == 8 && memcmp(b4, "AAAABBDD", 8) == 0,
                       "content mismatch after truncate+append");
            File_Close(fr2);
        }

        File_Delete("/adv.bin");

        /* External audit v2, FILE-03: truncating must not move the file
         * position (the adapter has to move it for f_truncate, and used
         * to leave it at the cut). */
        f = File_Open("/pos.bin", FM_OPEN_READ | FM_OPEN_WRITE |
                                  FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
        TEST("File_Truncate keeps the position (below the cut)");
        CHECK_TRUE(f != NULL, "open failed");
        r = File_Write(f, "AAAABBBBCCCC", 12, NULL);
        if (r == FM_OK) r = File_Seek(f, 2, FM_SEEK_SET);
        if (r == FM_OK) r = File_Truncate(f, 8);
        if (r == FM_OK) r = File_Tell(f, &pos);
        CHECK(r, "write/seek/truncate/tell");
        CHECK_TRUE(pos == 2, "position moved to the cut");
        got = 0;
        r = File_Read(f, b4, 4, &got);
        CHECK(r, "File_Read");
        CHECK_TRUE(got == 4 && memcmp(b4, "AABB", 4) == 0,
                   "read after truncate came from the wrong offset");

        TEST("File_Truncate pulls a position behind the cut back to it");
        r = File_Seek(f, 7, FM_SEEK_SET);
        if (r == FM_OK) r = File_Truncate(f, 5);
        if (r == FM_OK) r = File_Tell(f, &pos);
        CHECK(r, "seek/truncate/tell");
        CHECK_TRUE(pos == 5, "position not at the new end");
        r = File_Size(f, &sz);
        CHECK(r, "File_Size");
        CHECK_TRUE(sz == 5, "size != 5");
        File_Close(f);
        File_Delete("/pos.bin");
        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[4] directory API (FileDemo_Directory equivalent, '.'/'..' skip)\n");
    {
        FM_Result r;
        FM_Dir *d;
        FM_DirEntry e;
        int seen = 0, saw_dot = 0;

        FatFS_Init(&g_sd_fs, 0);
        FS_Mount("/", &g_sd_fs);

        TEST("mkdir /logs");
        r = Directory_Create("/logs");
        CHECK(r, "Directory_Create");

        File_WriteText("/logs/a.csv", "1,2,3");
        File_WriteText("/logs/b.csv", "4,5,6");

        TEST("opendir /logs");
        d = Directory_Open("/logs");
        CHECK_TRUE(d != NULL, "Directory_Open failed");
        while (Directory_Read(d, &e) == FM_OK) {
            if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
                saw_dot = 1;
            seen++;
        }
        Directory_Close(d);
        CHECK_TRUE(seen == 2, "expected exactly 2 entries");
        CHECK_TRUE(!saw_dot, "'.'/'..' must be skipped by adapter");

        TEST("rmdir non-empty denied");
        r = Directory_Delete("/logs");
        CHECK_TRUE(r != FM_OK, "rmdir should fail on non-empty dir");

        File_Delete("/logs/a.csv");
        File_Delete("/logs/b.csv");
        r = Directory_Delete("/logs");
        CHECK(r, "rmdir after emptying");

        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[5] FS_GetInfo (get_free / get_type)\n");
    {
        FM_Result r;
        FM_FSInfo info;

        TEST("FatFS_Init #5 (slot recycling from [1]-[4])");
        r = FatFS_Init(&g_sd_fs, 0);
        CHECK(r, "FatFS_Init");

        TEST("FS_Mount");
        r = FS_Mount("/", &g_sd_fs);
        CHECK(r, "FS_Mount");

        TEST("FS_GetInfo on FatFS mount");
        r = FS_GetInfo("/", &info);
        CHECK(r, "FS_GetInfo");
        CHECK_TRUE(info.free_space > 0, "free_space must be > 0");
        CHECK_TRUE(strcmp(info.type, "FAT32") == 0, "type must be FAT32");
        printf("     type=%s free=%llu capacity=%llu\n", info.type,
               (unsigned long long)info.free_space,
               (unsigned long long)info.capacity);

        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[5b] FatFS_Init repeated without unmount (slot reclaim)\n");
    {
        int i;
        FM_Result r = FM_OK;
        TEST("10 x FatFS_Init without unmount");
        for (i = 0; i < 10; i++)
        {
            r = FatFS_Init(&g_sd_fs, 0);
            if (r != FM_OK) break;
        }
        CHECK(r, "FatFS_Init must reclaim the previous slot");
    }

    /* ---------------------------------------------------------- */
    printf("[6] RAM FS beside FatFS at '/' (mount isolation)\n");
    {
        FM_Result r;
        char buf[64];

        r = FileManager_MountMemory(&g_ram_storage, &g_ram_fs, "RAM",
                                    g_ram_buf, sizeof(g_ram_buf), 1,
                                    FM_FALSE);
        TEST("mount RAM fs at / (after sd unmount)");
        CHECK(r, "FileManager_MountMemory");

        r = File_WriteText("/ram.txt", "ram-works");
        CHECK(r, "write via RAM mount");
        r = File_ReadText("/ram.txt", buf, sizeof(buf), NULL);
        CHECK_TRUE(strcmp(buf, "ram-works") == 0, "ram read mismatch");

        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[7] mount/unmount cycles (g_fatfs slot recycling)\n");
    {
        int cycle;
        FM_Result r = FM_OK;
        TEST("10 x (FatFS_Init + FS_Mount + write + FS_Unmount)");
        for (cycle = 1; cycle <= 10; cycle++) {
            ff_mock_reset();
            r = FatFS_Init(&g_sd_fs, 0);
            if (r != FM_OK) break;
            r = FS_Mount("/", &g_sd_fs);
            if (r != FM_OK) break;
            r = File_WriteText("/cyc.txt", "cycle");
            if (r != FM_OK) break;
            r = FS_Unmount("/");
            if (r != FM_OK) break;
        }
        if (cycle > 10) PASS(); else {
            char eb[96];
            snprintf(eb, sizeof(eb),
                     "cycle %d failed: %s", cycle,
                     File_GetErrorString(r));
            FAIL(eb);
        }
    }

    /* A mount at "/name" wants a storage called "name"; FatFS does its
     * own disk I/O here, so two small RAM blocks stand in for them. */
    {
        static FM_Storage sd_st, usb_st;
        static uint8_t sd_mem[2048], usb_mem[2048];
        FM_Result r;

        TEST("storages 'sd' and 'usb' for the named mounts");
        r = RamDisk_Init(&sd_st, "sd", sd_mem, sizeof(sd_mem), 512);
        if (r == FM_OK) r = Storage_Register(&sd_st);
        if (r == FM_OK) r = RamDisk_Init(&usb_st, "usb", usb_mem,
                                         sizeof(usb_mem), 512);
        if (r == FM_OK) r = Storage_Register(&usb_st);
        CHECK(r, "RamDisk_Init / Storage_Register");
    }

#if FF_VOLUMES >= 2
    /* ---------------------------------------------------------- */
    printf("[8] two drives: SD on 0:, USB on 1:\n");
    {
        /* Both are initialised before either is mounted -- the normal
         * start-up order.  The adapter used to think a slot free until
         * its volume had been touched, so the second Init took the slot
         * of the first: both FM_FS then shared one context, with the
         * drive number of the last one. */
        static FM_FS usb_fs;
        FM_Result r;
        FM_Result r2;
        FM_File *fh = NULL;
        char buf[32];
        uint32_t got = 0;

        ff_mock_reset();
        r = FatFS_Init(&g_sd_fs, 0);
        r2 = FatFS_Init(&usb_fs, 1);
        TEST("two FatFS_Init before the first mount");
        CHECK_TRUE(r == FM_OK && r2 == FM_OK, "FatFS_Init");

        r = FS_Mount("/sd", &g_sd_fs);
        r2 = FS_Mount("/usb", &usb_fs);
        TEST("both drives mount");
        CHECK_TRUE(r == FM_OK && r2 == FM_OK, "FS_Mount");

        r = File_WriteText("/sd/on_sd.txt", "sd");
        r2 = File_WriteText("/usb/on_usb.txt", "usb");
        TEST("a file written to /sd is on drive 0, one to /usb on 1");
        if (r != FM_OK || r2 != FM_OK) FAIL("File_WriteText");
        else if (ff_mock_file_count_on(0) != 1 ||
                 ff_mock_file_count_on(1) != 1)
        {
            char eb[96];
            snprintf(eb, sizeof(eb), "drive 0 has %d files, drive 1 has %d",
                     ff_mock_file_count_on(0), ff_mock_file_count_on(1));
            FAIL(eb);
        }
        else PASS();

        TEST("unmounting /usb leaves a file open on /sd alone");
        fh = File_Open("/sd/on_sd.txt", FM_OPEN_READ);
        r = (fh != NULL) ? FM_OK : FM_ERR_NOT_FOUND;
        r2 = FS_Unmount("/usb");
        memset(buf, 0, sizeof(buf));
        if (r == FM_OK) r = File_Read(fh, buf, 2, &got);
        if (fh != NULL) (void)File_Close(fh);
        CHECK_TRUE(r == FM_OK && r2 == FM_OK && got == 2 &&
                   memcmp(buf, "sd", 2) == 0, "read through the handle");

        TEST("/usb mounts again without a new FatFS_Init, on drive 1");
        r = FS_Mount("/usb", &usb_fs);
        if (r == FM_OK) r = File_ReadText("/usb/on_usb.txt", buf,
                                          sizeof(buf), NULL);
        CHECK_TRUE(r == FM_OK && strcmp(buf, "usb") == 0, "remount");

        (void)FS_Unmount("/usb");
        (void)FS_Unmount("/sd");
    }
#endif

    /* ---------------------------------------------------------- */
    printf("[9] an FM_FS whose slot went to another one\n");
    {
        /* `old` is unmounted and never initialised again; `cur` then
         * gets the slot `old` had.  Mounting `old` must be refused, not
         * run on the context that is now `cur`'s. */
        static FM_FS old_fs;
        static FM_FS cur_fs[FM_MAX_FS];
        FM_Result r;
        FM_Result r2 = FM_OK;
        FM_Bool exists = FM_FALSE;
        int i;

        ff_mock_reset();
        r = FatFS_Init(&old_fs, 0);
        if (r == FM_OK) r = FS_Mount("/sd", &old_fs);
        if (r == FM_OK) r = FS_Unmount("/sd");
        /* every slot there is, so the one `old` had is among them */
        for (i = 0; i < FM_MAX_FS && r == FM_OK; i++)
        {
            r = FatFS_Init(&cur_fs[i], 0);
        }
        if (r == FM_OK) r = FS_Mount("/sd", &cur_fs[0]);
        if (r == FM_OK) r = File_WriteText("/sd/keep.txt", "keep");
        TEST("set-up: every adapter slot taken, one mounted");
        CHECK(r, "set-up");

        TEST("mounting the stale FM_FS is refused");
        r2 = FS_Mount("/usb", &old_fs);
        if (r2 == FM_OK) (void)FS_Unmount("/usb");
        if (r2 == FM_ERR_NOT_READY) PASS(); else {
            char eb[96];
            snprintf(eb, sizeof(eb), "expected 'not ready', got '%s'",
                     File_GetErrorString(r2));
            FAIL(eb);
        }

        TEST("...and the live mount is untouched");
        r = File_Exists("/sd/keep.txt", &exists);
        CHECK_TRUE(r == FM_OK && exists, "keep.txt");

        (void)FS_Unmount("/sd");
        /* give the slots back: mount + unmount each */
        r = FM_OK;
        for (i = 0; i < FM_MAX_FS && r == FM_OK; i++)
        {
            r = FS_Mount("/sd", &cur_fs[i]);
            if (r == FM_OK) r = FS_Unmount("/sd");
        }
        TEST("each of them mounts and unmounts");
        CHECK(r, "mount cycle");
    }

    /* ---------------------------------------------------------- */
    printf("[10] card out, card in: mount again without FatFS_Init\n");
    {
        FM_Result r;
        char buf[32];
        int cycle;

        ff_mock_reset();
        TEST("FatFS_Init once, then 10 x (mount, write, read, unmount)");
        r = FatFS_Init(&g_sd_fs, 0);
        for (cycle = 1; cycle <= 10 && r == FM_OK; cycle++)
        {
            r = FS_Mount("/", &g_sd_fs);
            if (r != FM_OK) break;
            r = File_WriteText("/cyc.txt", "cycle");
            if (r == FM_OK) r = File_ReadText("/cyc.txt", buf,
                                              sizeof(buf), NULL);
            if (r == FM_OK && strcmp(buf, "cycle") != 0) r = FM_ERR_CRC;
            (void)FS_Unmount("/");
        }
        if (r == FM_OK) PASS(); else {
            char eb[96];
            snprintf(eb, sizeof(eb), "cycle %d failed: %s", cycle,
                     File_GetErrorString(r));
            FAIL(eb);
        }

        TEST("a mount that fails gives the slot back");
        /* (the mock is told to fail f_mount, as a forced mount of a
         *  card that does not answer would) */
        {
            static FM_FS bad_fs[FM_MAX_FS + 2];
            static FM_FS good_fs;
            int i;

            r = FM_OK;
            for (i = 0; i < FM_MAX_FS + 2 && r == FM_OK; i++)
            {
                r = FatFS_Init(&bad_fs[i], 0);
                if (r != FM_OK) break;
                ff_mock_fail_mounts(1);
                if (FS_Mount("/", &bad_fs[i]) == FM_OK)
                {
                    (void)FS_Unmount("/");
                    r = FM_ERR_INTERNAL;
                }
                ff_mock_fail_mounts(0);
            }
            if (r == FM_OK) r = FatFS_Init(&good_fs, 0);
            if (r == FM_OK) r = FS_Mount("/", &good_fs);
            if (r == FM_OK) r = FS_Unmount("/");
        }
        CHECK(r, "after failed mounts");

        TEST("FatFS_MountDefault that stops at the bind keeps no slot");
        /* (this build has no diskio of its own, so the bind is refused) */
        {
            static FM_FS half_fs[FM_MAX_FS + 2];
            static FM_FS good_fs;
            int i;

            r = FM_OK;
            for (i = 0; i < FM_MAX_FS + 2 && r == FM_OK; i++)
            {
                if (FatFS_MountDefault(&half_fs[i], 0, NULL) !=
                    FM_ERR_UNSUPPORTED)
                {
                    (void)FS_Unmount("/");
                    r = FM_ERR_INTERNAL;
                }
            }
            if (r == FM_OK) r = FatFS_Init(&good_fs, 0);
            if (r == FM_OK) r = FS_Mount("/", &good_fs);
            if (r == FM_OK) r = FS_Unmount("/");
        }
        CHECK(r, "after FatFS_MountDefault");
    }

    /* ---------------------------------------------------------- */
    printf("[11] audit V5: what a real FatFS does and the mock did not\n");
    {
        static FM_FS fs;
        FM_Result r;
        FM_FileInfo info;
        FM_Bool exists = FM_FALSE;
        FM_File *f;
        char buf[32];

        ff_mock_reset();
        r = FatFS_Init(&fs, 0);
        if (r == FM_OK) r = FS_Mount("/sd", &fs);
        TEST("set-up");
        CHECK(r, "mount");

        /* HIGH-01: 4 bytes were asked for, 6 were written */
        TEST("HIGH-01: get_type into 4 bytes is refused, not overrun");
        {
            char *small = (char *)malloc(4);    /* ASan sees one byte too many */
            char type6[6];
            FM_Result r4 = FM_ERR_INTERNAL;
            FM_Result r5 = FM_ERR_INTERNAL;
            FM_Result r6 = FM_ERR_INTERNAL;

            (void)File_WriteText("/sd/t.txt", "x");     /* fs_type is set */
            if (small != NULL)
            {
                r4 = fs.ops.get_type(&fs, small, 4);
                free(small);
            }
            r5 = fs.ops.get_type(&fs, type6, 5);
            r6 = fs.ops.get_type(&fs, type6, 6);
            CHECK_TRUE(r4 == FM_ERR_TOO_LARGE && r5 == FM_ERR_TOO_LARGE &&
                       r6 == FM_OK && strcmp(type6, "FAT32") == 0,
                       "want -16, -16, 0 and \"FAT32\"");
        }

        /* MED-02: f_stat("0:/") is FR_INVALID_NAME on a real volume */
        TEST("MED-02: File_GetInfo of the mount point is a directory");
        memset(&info, 0, sizeof(info));
        r = File_GetInfo("/sd", &info);
        CHECK_TRUE(r == FM_OK && info.type == FM_ITEM_DIR, "File_GetInfo(/sd)");
        TEST("MED-02: Directory_Exists of the mount point");
        r = Directory_Exists("/sd", &exists);
        CHECK_TRUE(r == FM_OK && exists, "Directory_Exists(/sd)");

        /* MED-03 */
        TEST("MED-03: WRITE|TRUNCATE without CREATE does not make a file");
        f = File_Open("/sd/none.txt", FM_OPEN_WRITE | FM_OPEN_TRUNCATE);
        r = File_GetLastError();
        if (f != NULL) (void)File_Close(f);
        exists = FM_TRUE;
        (void)File_Exists("/sd/none.txt", &exists);
        CHECK_TRUE(f == NULL && r == FM_ERR_NOT_FOUND && !exists,
                   "opened or created a file that was not there");
        TEST("MED-03: ...and empties one that is there");
        r = File_WriteText("/sd/full.txt", "twelve bytes");
        f = (r == FM_OK) ? File_Open("/sd/full.txt",
                                     FM_OPEN_WRITE | FM_OPEN_TRUNCATE) : NULL;
        if (f == NULL) r = (r == FM_OK) ? File_GetLastError() : r;
        if (r == FM_OK) r = File_Write(f, "ab", 2u, NULL);
        if (f != NULL) (void)File_Close(f);
        memset(buf, 0, sizeof(buf));
        if (r == FM_OK) r = File_ReadText("/sd/full.txt", buf, sizeof(buf), NULL);
        CHECK_TRUE(r == FM_OK && strcmp(buf, "ab") == 0, "content after truncate");

        /* HIGH-02: the table of open objects */
        TEST("HIGH-02: Directory_Copy of a folder with a file (needs 3 objects open)");
        r = Directory_Create("/sd/src");
        if (r == FM_OK) r = File_WriteText("/sd/src/a.txt", "copy me");
        if (r == FM_OK) r = Directory_Copy("/sd/src", "/sd/dst", NULL, NULL);
        memset(buf, 0, sizeof(buf));
        if (r == FM_OK) r = File_ReadText("/sd/dst/a.txt", buf, sizeof(buf), NULL);
        CHECK_TRUE(r == FM_OK && strcmp(buf, "copy me") == 0, "Directory_Copy");

        TEST("HIGH-02: every file handle the library has can be open at once");
        {
            FM_File *h[FM_MAX_OPEN_FILES];
            int i;
            int opened = 0;

            for (i = 0; i < FM_MAX_OPEN_FILES; i++)
            {
                char name[24];

                snprintf(name, sizeof(name), "/sd/h%d.bin", i);
                h[i] = File_Open(name, FM_OPEN_WRITE | FM_OPEN_CREATE);
                if (h[i] != NULL) opened++;
            }
            for (i = 0; i < FM_MAX_OPEN_FILES; i++)
            {
                if (h[i] != NULL) (void)File_Close(h[i]);
            }
            CHECK_TRUE(opened == FM_MAX_OPEN_FILES, "an open was refused");
        }

        /* MED-08: one entry, two spellings */
        TEST("MED-08: renaming a file to its own name in another case keeps it");
        r = File_WriteText("/sd/name.txt", "still here");
        if (r == FM_OK) r = File_Rename("/sd/name.txt", "/sd/NAME.TXT");
        memset(buf, 0, sizeof(buf));
        if (r == FM_OK) r = File_ReadText("/sd/NAME.TXT", buf, sizeof(buf), NULL);
        CHECK_TRUE(r == FM_OK && strcmp(buf, "still here") == 0,
                   "the file is gone or the rename failed");

        TEST("MED-08: a rename onto another file replaces it, the source is gone");
        r = File_WriteText("/sd/one.txt", "one");
        if (r == FM_OK) r = File_WriteText("/sd/two.txt", "two");
        if (r == FM_OK) r = File_Rename("/sd/one.txt", "/sd/two.txt");
        memset(buf, 0, sizeof(buf));
        if (r == FM_OK) r = File_ReadText("/sd/two.txt", buf, sizeof(buf), NULL);
        exists = FM_TRUE;
        (void)File_Exists("/sd/one.txt", &exists);
        {
            FM_Bool left = FM_TRUE;

            (void)File_Exists("/sd/~fmrn.tmp", &left);
            CHECK_TRUE(r == FM_OK && strcmp(buf, "one") == 0 && !exists && !left,
                       "wrong content, source still there, or a temp file left");
        }

        TEST("MED-08: copying a file onto itself in another case is refused");
        r = File_WriteText("/sd/self.txt", "self");
        if (r == FM_OK)
        {
            FM_Result rc = File_Copy("/sd/self.txt", "/sd/SELF.TXT", NULL, NULL);

            memset(buf, 0, sizeof(buf));
            r = File_ReadText("/sd/self.txt", buf, sizeof(buf), NULL);
            CHECK_TRUE(rc != FM_OK && r == FM_OK && strcmp(buf, "self") == 0,
                       "copied onto itself, or the file was emptied");
        }
        else
        {
            FAIL("set-up");
        }

        TEST("MED-08: a folder cannot be moved into itself under another case");
        r = Directory_Create("/sd/Tree");
        if (r == FM_OK)
        {
            FM_Result rm = Directory_Move("/sd/Tree", "/sd/tree/sub", NULL, NULL);

            CHECK_TRUE(rm == FM_ERR_INVALID_ARGUMENT, "not refused");
        }
        else
        {
            FAIL("set-up");
        }

        /* The move-aside of MED-08 must not let a folder with content
         * be replaced: aside it went, and stayed there as "~fmrn.tmp". */
        TEST("MED-08: a rename onto a folder that has entries is refused, nothing moves");
        {
            FM_Result a = Directory_Create("/sd/ra");
            FM_Result b = Directory_Create("/sd/rb");
            FM_Result rr;
            FM_Bool ea = FM_FALSE;
            FM_Bool eb = FM_FALSE;
            FM_Bool et = FM_TRUE;

            if (a == FM_OK) a = File_WriteText("/sd/ra/a.txt", "from a");
            if (b == FM_OK) b = File_WriteText("/sd/rb/b.txt", "in b");
            rr = Directory_Rename("/sd/ra", "/sd/rb");
            (void)File_Exists("/sd/ra/a.txt", &ea);
            (void)File_Exists("/sd/rb/b.txt", &eb);
            (void)Directory_Exists("/sd/~fmrn.tmp", &et);
            if (a != FM_OK || b != FM_OK || rr != FM_ERR_NOT_EMPTY || !ea ||
                !eb || et)
            {
                printf("[set-up %d %d, rename %d, a.txt %d, b.txt %d, "
                       "~fmrn.tmp %d] ", (int)a, (int)b, (int)rr, (int)ea,
                       (int)eb, (int)et);
                FAIL("wrong");
            }
            else { PASS(); }
        }

        TEST("MED-08: a rename onto an empty folder replaces it");
        {
            FM_Result c = Directory_Create("/sd/rc");
            FM_Result rr = Directory_Rename("/sd/ra", "/sd/rc");
            FM_Bool ea = FM_TRUE;
            FM_Bool ec = FM_FALSE;
            FM_Bool et = FM_TRUE;

            (void)Directory_Exists("/sd/ra", &ea);
            (void)File_Exists("/sd/rc/a.txt", &ec);
            (void)Directory_Exists("/sd/~fmrn.tmp", &et);
            if (c != FM_OK || rr != FM_OK || ea || !ec || et)
            {
                printf("[set-up %d, rename %d, ra %d, rc/a.txt %d, "
                       "~fmrn.tmp %d] ", (int)c, (int)rr, (int)ea, (int)ec,
                       (int)et);
                FAIL("wrong");
            }
            else { PASS(); }
        }

        TEST("MED-08: a folder renamed to its own name in another case keeps its files");
        {
            FM_Result rr = Directory_Rename("/sd/rc", "/sd/RC");
            FM_Bool ec = FM_FALSE;
            FM_Bool et = FM_TRUE;

            (void)File_Exists("/sd/RC/a.txt", &ec);
            (void)Directory_Exists("/sd/~fmrn.tmp", &et);
            if (rr != FM_OK || !ec || et)
            {
                printf("[rename %d, RC/a.txt %d, ~fmrn.tmp %d] ", (int)rr,
                       (int)ec, (int)et);
                FAIL("wrong");
            }
            else { PASS(); }
        }

        (void)FS_Unmount("/sd");

        /* CRIT-02: FM_MAX_FS FatFS_Init calls that were never mounted
         * used up the adapter for good */
        TEST("CRIT-02: FatFS_Init after FM_MAX_FS unmounted ones still works");
        {
            static FM_FS lost[FM_MAX_FS];
            static FM_FS late;
            FM_Result rl = FM_OK;
            FM_Result rm = FM_ERR_INTERNAL;
            int robbed = 0;
            int kept = 0;
            int k;

            for (k = 0; k < (int)FM_MAX_FS && rl == FM_OK; k++)
            {
                rl = FatFS_Init(&lost[k], 0);
            }
            if (rl == FM_OK) rl = FatFS_Init(&late, 0);
            if (rl == FM_OK) rm = FS_Mount("/sd", &late);
            if (rm == FM_OK) rm = File_WriteText("/sd/late.txt", "late");
            (void)FS_Unmount("/sd");
            /* exactly one FM_FS lost its slot; it is told so at its
             * mount, the others mount as if nothing had happened */
            for (k = 0; k < (int)FM_MAX_FS; k++)
            {
                FM_Result ro = FS_Mount("/sd", &lost[k]);

                if (ro == FM_OK) { kept++; (void)FS_Unmount("/sd"); }
                else if (ro == FM_ERR_NOT_READY) robbed++;
            }
            if (rl != FM_OK || rm != FM_OK || robbed != 1 ||
                kept != (int)FM_MAX_FS - 1)
            {
                printf("[init %d, mount %d, %d robbed, %d kept] ", (int)rl,
                       (int)rm, robbed, kept);
                FAIL("wrong");
            }
            else { PASS(); }
        }
    }

    printf("\n  Results: %d/%d passed, %d failures\n",
           g_tests - g_failures, g_tests, g_failures);
    return g_failures ? 1 : 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
