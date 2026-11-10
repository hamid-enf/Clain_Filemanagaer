/*
 * fm_sd_bounce.h — DMA امن برای SD وقتی D-Cache روشن است
 *
 * مشکل: FatFS بافر win[] و خیلی از بافرهای File_* روی مرز ۳۲ بایت
 * نیستند. Cube sd_diskio با ENABLE_SD_DMA_CACHE_MAINTENANCE آدرس را
 * به پایین گرد می‌کند و Invalidate خط‌های همسایه را دور می‌ریزد.
 * نتیجه: FR_INT_ERR → FileManager «internal error (-26)».
 *
 * راه حل: اگر بافر ۳۲-بایت-تراز نباشد (یا FM_CACHE_SD=OFF)، DMA فقط
 * روی یک scratch هم‌تراز می‌رود و بعد memcpy.
 *
 * در diskio خود FileManager (FM_FATFS_PROVIDE_DISKIO=1) خودکار است.
 *
 * برای sd_diskio.c کیوب، در SD_read/SD_write به‌جای DMA مستقیم:
 *
 *     #include "fm_sd_bounce.h"
 *     if (!FM_Cache_SdDirectOk(buff, (size_t)count * BLOCKSIZE)) {
 *         return FM_Sd_BounceRead(buff, sector, count, my_dma_read_1);
 *     }
 */
#ifndef FM_SD_BOUNCE_H
#define FM_SD_BOUNCE_H

#include "fm_cache.h"
#include <string.h>

#ifndef FM_SD_BOUNCE_BYTES
#define FM_SD_BOUNCE_BYTES 512u
#endif

typedef int (*FM_Sd_Dma1Fn)(void *aligned_512, uint32_t lba);

static FM_CACHE_ALIGN uint8_t fm_sd_bounce[FM_SD_BOUNCE_BYTES];

/* dma_one: read exactly one 512-byte sector into aligned_512, return 0 on
 * success.  After DMA, this helper invalidates the bounce buffer. */
static inline int FM_Sd_BounceRead(void *buff, uint32_t sector, uint32_t count,
                                   FM_Sd_Dma1Fn dma_one)
{
    uint8_t *out = (uint8_t *)buff;
    uint32_t i;

    if (buff == NULL || dma_one == NULL || count == 0u)
    {
        return -1;
    }
    for (i = 0; i < count; i++)
    {
        if (dma_one(fm_sd_bounce, sector + i) != 0)
        {
            return -1;
        }
        FM_Cache_InvalidateAfterDma(fm_sd_bounce, FM_SD_BOUNCE_BYTES);
        memcpy(out + (size_t)i * FM_SD_BOUNCE_BYTES, fm_sd_bounce,
               FM_SD_BOUNCE_BYTES);
    }
    return 0;
}

static inline int FM_Sd_BounceWrite(const void *buff, uint32_t sector,
                                    uint32_t count, FM_Sd_Dma1Fn dma_one)
{
    const uint8_t *in = (const uint8_t *)buff;
    uint32_t i;

    if (buff == NULL || dma_one == NULL || count == 0u)
    {
        return -1;
    }
    for (i = 0; i < count; i++)
    {
        memcpy(fm_sd_bounce, in + (size_t)i * FM_SD_BOUNCE_BYTES,
               FM_SD_BOUNCE_BYTES);
        FM_Cache_CleanForDma(fm_sd_bounce, FM_SD_BOUNCE_BYTES);
        if (dma_one(fm_sd_bounce, sector + i) != 0)
        {
            return -1;
        }
    }
    return 0;
}

#endif /* FM_SD_BOUNCE_H */
