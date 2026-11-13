/* FM-NOT-FIRMWARE: optional real FatFS RTC integration probe. */
#ifndef USE_HAL_DRIVER
#include "diskio.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#define SECTORS 8192
static unsigned char disk[SECTORS * 512];
DSTATUS disk_initialize(BYTE d) { return d ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE d) { return d ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE d, BYTE *p, LBA_t s, UINT n) {
  if (d || s + n > SECTORS)
    return RES_PARERR;
  memcpy(p, disk + s * 512, n * 512);
  return RES_OK;
}
DRESULT disk_write(BYTE d, const BYTE *p, LBA_t s, UINT n) {
  if (d || s + n > SECTORS)
    return RES_PARERR;
  memcpy(disk + s * 512, p, n * 512);
  return RES_OK;
}
DRESULT disk_ioctl(BYTE d, BYTE c, void *p) {
  if (d)
    return RES_PARERR;
  switch (c) {
  case CTRL_SYNC:
    return RES_OK;
  case GET_SECTOR_COUNT:
    *(LBA_t *)p = SECTORS;
    return RES_OK;
  case GET_SECTOR_SIZE:
    *(WORD *)p = 512;
    return RES_OK;
  case GET_BLOCK_SIZE:
    *(DWORD *)p = 1;
    return RES_OK;
  default:
    return RES_PARERR;
  }
}
#ifdef STRONG
DWORD get_fattime(void) { return 0x58210000u; }
#define EXPECT 0x5821u
#else
#define EXPECT 0x5021u
#endif
int main(void) {
  FATFS fs;
  FIL file;
  FILINFO info;
  BYTE work[4096];
  UINT n;
  FRESULT r;
  r = f_mkfs("", NULL, work, sizeof work);
  if (r) {
    printf("mkfs %d\n", r);
    return 1;
  }
  r = f_mount(&fs, "", 1);
  if (r)
    return 2;
  r = f_open(&file, "HELLO.TXT", FA_WRITE | FA_CREATE_ALWAYS);
  if (r)
    return 3;
  if (f_write(&file, "test", 4, &n) || n != 4 || f_close(&file))
    return 4;
  if (f_stat("HELLO.TXT", &info))
    return 5;
  printf("actual FAT date 0x%04x expected 0x%04x\n", info.fdate, EXPECT);
  return info.fdate != EXPECT;
}

#endif /* USE_HAL_DRIVER */
