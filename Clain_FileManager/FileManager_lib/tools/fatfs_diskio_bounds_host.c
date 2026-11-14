/* FM-NOT-FIRMWARE: test diskio length checks with a fake storage driver. */
#ifndef USE_HAL_DRIVER
#include "../backends/fm_fatfs.c"
#include <stdio.h>
static unsigned calls;
static uint64_t last_offset;
static uint32_t last_size;
static FM_Result probe_read(FM_Storage *s, uint64_t off, void *buf, uint32_t len)
{ (void)s; (void)buf; calls++; last_offset=off; last_size=len; return FM_OK; }
static FM_Result probe_write(FM_Storage *s, uint64_t off, const void *buf, uint32_t len)
{ (void)s; (void)buf; calls++; last_offset=off; last_size=len; return FM_OK; }
static FM_Result probe_erase(FM_Storage *s, uint64_t off, uint32_t len)
{ (void)s; calls++; last_offset=off; last_size=len; return FM_OK; }
int main(void)
{
    FM_Storage storage = {0};
    BYTE buf[1024] = {0};
    storage.block_size = 512;
    storage.read = probe_read;
    storage.write = probe_write;
    storage.erase = probe_erase;
    g_fatfs_storage[0] = &storage;
    if (disk_read(0, buf, 2, 1) != RES_OK || calls != 1 || last_offset != 1024 || last_size != 512) return 1;
    if (disk_write(0, buf, 3, 1) != RES_OK || calls != 2 || last_offset != 1536 || last_size != 512) return 2;
    if (disk_read(0, buf, 0, 0) != RES_PARERR || disk_write(0, buf, 0, 0) != RES_PARERR) return 3;
    if (disk_read(0, buf, 0, 8388608u) != RES_PARERR ||
        disk_write(0, buf, 0, 8388608u) != RES_PARERR || calls != 2) return 4;
    {
        LBA_t range[2] = {2, 3};
        if (disk_ioctl(0, CTRL_TRIM, range) != RES_OK || calls != 3 ||
            last_offset != 1024 || last_size != 1024) return 5;
        range[0] = 4; range[1] = 3;
        if (disk_ioctl(0, CTRL_TRIM, range) != RES_PARERR) return 6;
        range[0] = 0; range[1] = 8388607u;
        if (disk_ioctl(0, CTRL_TRIM, range) != RES_PARERR || calls != 3) return 7;
        if (disk_ioctl(0, CTRL_TRIM, NULL) != RES_PARERR) return 8;
    }
    puts("FatFS diskio length checks: PASS");
    return 0;
}
#endif
