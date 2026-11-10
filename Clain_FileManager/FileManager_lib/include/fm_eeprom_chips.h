/*
 * fm_eeprom_chips.h — EEPROM با انتخاب مدل، مثل QSPI
 * ===================================================
 *
 * شما مدل را می‌گویید، بقیه‌اش خودکار است:
 *
 *     FM_EepromChipInfo info;
 *     FM_Eeprom_GetChipInfo(FM_EEPROM_CHIP_24LC256, &info);
 *
 * و کتابخانه می‌داند ظرفیتش چقدر است، صفحه‌اش چند بایت است، آدرسش ۱
 * بایتی است یا ۲ بایتی، و بعد از نوشتن چقدر باید صبر کرد.
 *
 * ---------------------------------------------------------------------
 * چیزهایی که در EEPROM اشتباه می‌شود و اینجا حل شده
 * ---------------------------------------------------------------------
 *  • نوشتن نمی‌تواند از مرز صفحه رد شود. اگر رد شود، داده به اول همان
 *    صفحه می‌پیچد — دقیقاً مثل فلش NOR ولی با صفحات خیلی کوچک‌تر
 *    (۸ تا ۲۵۶ بایت). این شایع‌ترین باگ کار با EEPROM است.
 *
 *  • بعد از هر نوشتن، قطعه چند میلی‌ثانیه مشغول است و به هیچ دستوری
 *    جواب نمی‌دهد. باید یا صبر کرد یا با ACK polling منتظر ماند.
 *
 *  • قطعات ۱۶ کیلوبیت و کمتر آدرس ۱ بایتی دارند و بیت‌های بالای آدرس
 *    داخل آدرس I2C کدگذاری می‌شود. بزرگ‌ترها آدرس ۲ بایتی دارند.
 *    اشتباه‌گرفتن این دو یعنی نوشتن روی جای غلط.
 *
 *  • عمر نوشتن حدود یک میلیون بار برای هر سلول است. EEPROM_Update
 *    اول می‌خواند و اگر همان مقدار بود اصلاً نمی‌نویسد.
 */
#ifndef FM_EEPROM_CHIPS_H
#define FM_EEPROM_CHIPS_H

#include "fm_types.h"
#include "fm_eeprom.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_EEPROM_CHIP_CUSTOM = 0,

    /* Microchip / Atmel 24xx — آدرس ۱ بایتی */
    FM_EEPROM_CHIP_24LC01,     /*   128 B */
    FM_EEPROM_CHIP_24LC02,     /*   256 B */
    FM_EEPROM_CHIP_24LC04,     /*   512 B */
    FM_EEPROM_CHIP_24LC08,     /*  1 KB   */
    FM_EEPROM_CHIP_24LC16,     /*  2 KB   */

    /* آدرس ۲ بایتی */
    FM_EEPROM_CHIP_24LC32,     /*  4 KB   */
    FM_EEPROM_CHIP_24LC64,     /*  8 KB   */
    FM_EEPROM_CHIP_24LC128,    /* 16 KB   */
    FM_EEPROM_CHIP_24LC256,    /* 32 KB   */
    FM_EEPROM_CHIP_24LC512,    /* 64 KB   */
    FM_EEPROM_CHIP_24LC1025,   /* 128 KB  */

    /* ST M24xx */
    FM_EEPROM_CHIP_M24C64,     /*  8 KB   */
    FM_EEPROM_CHIP_M24M01,     /* 128 KB  */

    /* SPI 25xx */
    FM_EEPROM_CHIP_25LC256,    /* 32 KB   */
    FM_EEPROM_CHIP_25LC512,    /* 64 KB   */

    FM_EEPROM_CHIP_COUNT
} FM_EepromChip;

typedef struct
{
    const char *name;
    uint32_t capacity;         /* بایت                                  */
    uint32_t page_size;        /* بزرگ‌ترین نوشتن پیوسته                */
    uint8_t  addr_bytes;       /* 1 یا 2                                */
    uint8_t  i2c_addr;         /* آدرس پایهٔ I2C (0 برای SPI)           */
    uint16_t write_cycle_ms;   /* زمان لازم بعد از هر نوشتن             */
    uint8_t  is_spi;           /* 1 = SPI، 0 = I2C                      */
} FM_EepromChipInfo;

/* مشخصات یک مدل را بردار. */
FM_Result FM_Eeprom_GetChipInfo(FM_EepromChip chip, FM_EepromChipInfo *out);

/*
 * پیکربندی FM_EEPROMConfig را از روی مدل پر کن.
 *
 * فقط ظرفیت و اندازهٔ صفحه را می‌گذارد؛ read/write/context را خودتان
 * می‌دهید چون به درایور I2C/SPI شما بستگی دارد.
 *
 *     FM_EEPROMConfig cfg;
 *     FM_Eeprom_ConfigFromChip(&cfg, FM_EEPROM_CHIP_24LC256);
 *     cfg.read = my_i2c_read;
 *     cfg.write = my_i2c_write;
 *     cfg.context = &hi2c1;
 *     EEPROM_Init(&eeprom, &cfg);
 */
FM_Result FM_Eeprom_ConfigFromChip(FM_EEPROMConfig *cfg, FM_EepromChip chip);

FM_END_DECLS

#endif /* FM_EEPROM_CHIPS_H */
