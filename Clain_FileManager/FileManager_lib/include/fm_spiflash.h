/*
 * fm_spiflash.h — همان فلش، این بار روی SPI معمولی
 * =================================================
 *
 * چرا این فایل
 * ------------
 * قطعات W25Q، MX25، N25Q و امثالشان روی هر دو گذرگاه کار می‌کنند:
 * QUADSPI اختصاصی، یا SPI معمولی. منطق فلش (پاک‌کردن سکتور، مرز صفحه،
 * انتظار busy، جدول قطعات) دقیقاً یکی است — فقط نحوهٔ فرستادن بایت‌ها
 * فرق می‌کند.
 *
 * پس هیچ چیز تکرار نمی‌شود: این فایل همان FM_QspiIo را می‌سازد، ولی با
 * SPI. تمام درایور fm_qspi.c بدون تغییر استفاده می‌شود.
 *
 * یعنی روی یک F1 که QUADSPI ندارد، دقیقاً همان کتابخانه و همان جدول
 * قطعات و همان API کار می‌کند — فقط کندتر.
 *
 * استفاده
 * -------
 *     static FM_SpiFlashPort port = {
 *         .transfer  = my_spi_transfer,   // فرستادن/گرفتن بایت
 *         .cs_select = my_cs_low,
 *         .cs_release= my_cs_high,
 *         .delay_ms  = my_delay,
 *         .ctx       = &hspi1
 *     };
 *
 *     static FM_QspiDevice dev;
 *     static FM_Storage    storage;
 *     static uint8_t       sector[4096];
 *
 *     FM_SpiFlash_Init(&dev, &port, FM_QSPI_CHIP_AUTO,
 *                      sector, sizeof(sector));
 *     FM_Qspi_RegisterStorage(&dev, &storage, "flash");
 *
 * از این به بعد همه‌چیز مثل QSPI است: mount، فایل‌سیستم، FM_Transfer.
 *
 * تفاوت‌ها با QSPI
 * ---------------
 *  • سرعت: SPI تک‌خطی حدود یک‌چهارم QSPI چهارخطی است.
 *  • حالت memory-mapped وجود ندارد. SPI معمولی نمی‌تواند فلش را روی
 *    فضای آدرس بیاورد، پس خواندن همیشه از راه دستور است.
 *  • بقیه چیزها یکسان است.
 */
#ifndef FM_SPIFLASH_H
#define FM_SPIFLASH_H

#include "fm_types.h"
#include "fm_qspi.h"

FM_BEGIN_DECLS

/*
 * چهار تابعی که شما می‌دهید. هیچ‌کدام چیزی از فلش نمی‌دانند — فقط
 * بایت جابه‌جا می‌کنند.
 */
typedef struct
{
    /*
     * انتقال دوطرفهٔ SPI. اگر tx یا rx برابر NULL باشد، آن جهت لازم
     * نیست (فقط خواندن یا فقط نوشتن).
     *
     * توجه: بعضی HALها برای «فقط خواندن» لازم دارند چیزی بفرستید؛
     * در آن حالت 0xFF بفرستید.
     */
    FM_Result (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx,
                          uint32_t len);

    /* پایین/بالا بردن پایهٔ Chip Select. */
    void (*cs_select)(void *ctx);
    void (*cs_release)(void *ctx);

    /* تأخیر میلی‌ثانیه‌ای — می‌تواند NULL باشد. */
    void (*delay_ms)(void *ctx, uint32_t ms);

    void *ctx;
} FM_SpiFlashPort;

/*
 * راه‌اندازی. همان امضای FM_Qspi_Init، فقط با پورت SPI.
 *
 * chip می‌تواند FM_QSPI_CHIP_AUTO باشد تا از JEDEC ID شناسایی شود —
 * همان جدول دوازده‌قطعه‌ای fm_qspi.c استفاده می‌شود.
 *
 * sector_buf برای نوشتن‌های جزئی لازم است (به اندازهٔ یک سکتور،
 * معمولاً ۴ کیلوبایت). اگر NULL بدهید فقط نوشتن هم‌تراز کار می‌کند.
 */
FM_Result FM_SpiFlash_Init(FM_QspiDevice *dev, FM_SpiFlashPort *port,
                           FM_QspiChip chip, uint8_t *sector_buf,
                           uint32_t sector_buf_size);

FM_END_DECLS

#endif /* FM_SPIFLASH_H */
