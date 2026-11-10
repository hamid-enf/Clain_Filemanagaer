/*
 * fm_eeprom_chips.c — جدول قطعات EEPROM
 */

#define FM_DEBUG_TAG "eeprom"

#include <string.h>

#include "fm_eeprom_chips.h"
#include "fm_debug.h"

/*
 * زمان چرخهٔ نوشتن از بدترین حالت دیتاشیت است. اگر خوش‌بینانه بگیریم،
 * نوشتن بعدی وسط چرخهٔ قبلی می‌افتد و بی‌صدا گم می‌شود.
 */
static const FM_EepromChipInfo g_ee[] = {
    /* name        cap      page  addr i2c   ms  spi */
    { "24LC01",      128u,   8u,  1u, 0x50u,  5u, 0u },
    { "24LC02",      256u,   8u,  1u, 0x50u,  5u, 0u },
    { "24LC04",      512u,  16u,  1u, 0x50u,  5u, 0u },
    { "24LC08",     1024u,  16u,  1u, 0x50u,  5u, 0u },
    { "24LC16",     2048u,  16u,  1u, 0x50u,  5u, 0u },
    { "24LC32",     4096u,  32u,  2u, 0x50u,  5u, 0u },
    { "24LC64",     8192u,  32u,  2u, 0x50u,  5u, 0u },
    { "24LC128",   16384u,  64u,  2u, 0x50u,  5u, 0u },
    { "24LC256",   32768u,  64u,  2u, 0x50u,  5u, 0u },
    { "24LC512",   65536u, 128u,  2u, 0x50u,  5u, 0u },
    { "24LC1025", 131072u, 128u,  2u, 0x50u,  5u, 0u },
    { "M24C64",     8192u,  32u,  2u, 0x50u,  5u, 0u },
    { "M24M01",   131072u, 256u,  2u, 0x50u,  5u, 0u },
    { "25LC256",   32768u,  64u,  2u, 0x00u,  5u, 1u },
    { "25LC512",   65536u, 128u,  2u, 0x00u,  5u, 1u }
};

#define EE_COUNT (sizeof(g_ee) / sizeof(g_ee[0]))

static int ee_index(FM_EepromChip chip)
{
    switch (chip)
    {
    case FM_EEPROM_CHIP_24LC01:   return 0;
    case FM_EEPROM_CHIP_24LC02:   return 1;
    case FM_EEPROM_CHIP_24LC04:   return 2;
    case FM_EEPROM_CHIP_24LC08:   return 3;
    case FM_EEPROM_CHIP_24LC16:   return 4;
    case FM_EEPROM_CHIP_24LC32:   return 5;
    case FM_EEPROM_CHIP_24LC64:   return 6;
    case FM_EEPROM_CHIP_24LC128:  return 7;
    case FM_EEPROM_CHIP_24LC256:  return 8;
    case FM_EEPROM_CHIP_24LC512:  return 9;
    case FM_EEPROM_CHIP_24LC1025: return 10;
    case FM_EEPROM_CHIP_M24C64:   return 11;
    case FM_EEPROM_CHIP_M24M01:   return 12;
    case FM_EEPROM_CHIP_25LC256:  return 13;
    case FM_EEPROM_CHIP_25LC512:  return 14;
    default:                      return -1;
    }
}

FM_Result FM_Eeprom_GetChipInfo(FM_EepromChip chip, FM_EepromChipInfo *out)
{
    int idx;

    FM_CHECK_PTR(out, FM_ERR_INVALID_ARGUMENT);
    idx = ee_index(chip);
    if (idx < 0)
    {
        FM_ERR_LOG("unknown eeprom chip %d", (int)chip);
        return FM_ERR_UNSUPPORTED;
    }
    *out = g_ee[idx];
    return FM_OK;
}

FM_Result FM_Eeprom_ConfigFromChip(FM_EEPROMConfig *cfg, FM_EepromChip chip)
{
    FM_EepromChipInfo info;
    FM_Result r;

    FM_CHECK_PTR(cfg, FM_ERR_INVALID_ARGUMENT);
    r = FM_Eeprom_GetChipInfo(chip, &info);
    if (r != FM_OK)
    {
        return r;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = info.name;
    cfg->capacity = info.capacity;
    cfg->page_size = info.page_size;
    FM_EVENT_LOG("%s: %lu bytes, %lu-byte pages, %u-byte addressing",
                 info.name, (unsigned long)info.capacity,
                 (unsigned long)info.page_size, (unsigned)info.addr_bytes);
    return FM_OK;
}
