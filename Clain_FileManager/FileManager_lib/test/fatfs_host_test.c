/*
 * Host test: drives the FileManager FatFS adapter (backends/fm_fatfs.c)
 * against the ff_mock simulator, replaying the exact sequence of the
 * on-target example ex_1_SdCard_file_basic() plus mount/unmount cycles,
 * advanced handle ops, directory ops and info queries.
 *
 * Build & run:  see test/build_fatfs_host.sh  (ASan + UBSan)
 */

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

/* exact replica of the on-target ex_1_SdCard_file_basic() body */
static void ex_1_sdcard_file_basic(FM_Result *init_r, FM_Result *mount_r)
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
    printf("== FatFS adapter host test ==\n");

    /* ---------------------------------------------------------- */
    printf("[1] ex_1_SdCard_file_basic replica\n");
    {
        FM_Result ir = FM_OK, mr = FM_OK;
        TEST("FatFS_Init + FS_Mount(/)");
        ex_1_sdcard_file_basic(&ir, &mr);
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
    printf("[3] advanced handle API (ex_3 equivalent)\n");
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
        FS_Unmount("/");
    }

    /* ---------------------------------------------------------- */
    printf("[4] directory API (ex_4 equivalent, '.'/'..' skip)\n");
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

    printf("\n  Results: %d/%d passed, %d failures\n",
           g_tests - g_failures, g_tests, g_failures);
    return g_failures ? 1 : 0;
}
