/* FM-NOT-FIRMWARE: real FatFS rename-over regression on an in-memory disk. */
#ifndef USE_HAL_DRIVER
/* Reuse the in-memory disk from the standalone real-FatFS time test. */
#define main fatfs_time_unused_main
#include "fatfs_real_time_host.c"
#undef main
/* Include the adapter to exercise its exact internal rename implementation. */
#include "../backends/fm_fatfs.c"

static int put(const char *name, const char *s)
{
    FIL file;
    UINT n;
    size_t len = strlen(s);
    if (f_open(&file, name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 1;
    if (f_write(&file, s, (UINT)len, &n) != FR_OK || n != len) return 1;
    return f_close(&file) != FR_OK;
}
static int same(const char *name, const char *s)
{
    FIL file;
    char buf[64] = {0};
    UINT n = 0;
    if (f_open(&file, name, FA_READ) != FR_OK) return 0;
    if (f_read(&file, buf, sizeof(buf)-1, &n) != FR_OK || f_close(&file) != FR_OK) return 0;
    return n == strlen(s) && strcmp(buf, s) == 0;
}
int main(void)
{
    FATFS volume;
    FatFS_Ctx ctx = {0};
    FM_FS adapter = {0};
    BYTE work[4096];
    adapter.user = &ctx;
    if (f_mkfs("", NULL, work, sizeof work) != FR_OK || f_mount(&volume, "", 1) != FR_OK) return 1;
    if (put("~fmrn.tmp", "sentinel") || put("~fm00.tmp", "occupied") ||
        put("source.txt", "source") || put("target.txt", "target")) return 2;
    if (fatfs_rename(&adapter, "/source.txt", "/target.txt") != FM_OK ||
        !same("~fmrn.tmp", "sentinel") || !same("~fm00.tmp", "occupied") ||
        !same("target.txt", "source")) return 3;
    if (put("target.txt", "target") ||
        fatfs_rename(&adapter, "/~fmrn.tmp", "/target.txt") != FM_OK ||
        !same("target.txt", "sentinel")) return 4;
    /* Mixed-type replacement must not remove either original entry. */
    if (f_mkdir("adir") != FR_OK || put("afile", "valuable")) return 5;
    if (fatfs_rename(&adapter, "/adir", "/afile") != FM_ERR_NOT_DIRECTORY ||
        !same("afile", "valuable")) return 6;
    {
        FILINFO info;
        if (f_stat("adir", &info) != FR_OK || !(info.fattrib & AM_DIR)) return 7;
    }
    if (fatfs_rename(&adapter, "/afile", "/adir") != FM_ERR_IS_DIRECTORY ||
        !same("afile", "valuable")) return 8;
    {
        FILINFO info;
        if (f_stat("adir", &info) != FR_OK || !(info.fattrib & AM_DIR)) return 9;
    }
    if (put("stage.txt", "LOG") || put("final.txt", "RIVAL")) return 10;
    if (fatfs_rename_noreplace(&adapter, "/stage.txt", "/final.txt") != FM_ERR_EXISTS ||
        !same("stage.txt", "LOG") || !same("final.txt", "RIVAL")) return 11;
    if (f_unlink("final.txt") != FR_OK ||
        fatfs_rename_noreplace(&adapter, "/stage.txt", "/final.txt") != FM_OK ||
        !same("final.txt", "LOG")) return 12;
    puts("real FatFS rename collision, no-replace, mixed-type and source preservation: PASS");
    return 0;
}
#endif /* USE_HAL_DRIVER */
