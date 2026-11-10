/*
 * i2c_eeprom_dma.h — پل I2C-EEPROM با DMA برای STM32H7
 * ================================================================
 *
 * پروتکل کتابخانه برای I2C همانی است که مثال ۲۸ نشان داد: دو تابع
 * read/write می‌نویسید و به EEPROM_examples_begin() می‌دهید. این فایل
 * همان دو تابع را با HAL و به‌صورت DMA محور آماده دارد — دیگر لازم
 * نیست my_ee_read/my_ee_write را دستی بنویسید.
 *
 * استفاده در main:
 *
 *     #include "i2c_eeprom_dma.h"
 *     ...
 *     MX_I2C1_Init();
 *     if (Ex32_I2cEeprom_Begin(FM_EEPROM_CHIP_24LC256, &hi2c1,
 *                              0x50) == FM_OK)
 *     {
 *         exI_1_i2c_dma_smoke();          <- تست دود همین فایل
 *         exE_1_basic();                  <- بقیه از مثال ۲۸
 *         exE_2_settings_struct();
 *         exE_3_wear_aware_counter();
 *         exE_4_as_storage();
 *     }
 *
 * تنظیمات CubeMX — چک‌لیست دقیق
 * ---------------------------------------------------------------------
 *   Connectivity -> I2C1 (یا هر I2C که پین‌هایش روی بردت آزاد است):
 *     Mode            : I2C
 *     Parameter Settings:
 *       Speed Mode    : Standard 100kHz برای اولین راه؛ بعدش Fast
 *                       400kHz (24xx همه پشتیبانی می‌کنند)
 *       TIMING        : خود کیوب با ابزار Resolution حساب می‌کند؛
 *                       کلاک کِرنل I2C را روی PCLK1 بگذار
 *       Addressing    : 7-bit (24xx/M24x همه 7-bit اند)
 *       Clock No Stretch : Disabled  <- مهم! 24xx وسط write-cycle با
 *                       کش‌استرچ جواب می‌دهد؛ روشنش نکن
 *       Rise Time     : 100ns (در 400kHz) / 0 (در 100kHz)
 *     GPIO Configuration:
 *       SDA/SCL       : Open-Drain + پول‌آپ. ترجیح: 4.7K خارجی؛
 *                       پول‌آپ داخلی فقط برای تستِ 100kHz قابل قبول است.
 *     سیم‌کشی قطعه    : WP -> GND (قفل نوشتن باز)، A2/A1/A0 -> GND
 *                       یعنی آدرس پایه 0x50 (در Ex32_I2cEeprom_Begin
 *                       همان 0 بده تا از جدول چیپ برداشته شود).
 *
 *   DMA Settings (حالت DMA - هر دو جهت):
 *     Add -> I2C1_RX و I2C1_TX (از DMAMUX به DMA1/DMA2 می‌روند؛
 *     برخلاف QSPI که روی H7 فقط MDMA می‌خواهد).
 *       Mode          : Normal   <- هرگز Circular؛ HAL اینجا تک‌موج
 *                       کار می‌کند و خودش استریم را می‌بندد
 *       Data Width    : Byte / Byte (هر دو جهت)
 *       Increment     : Peripheral ثابت، Memory افزایشی (HAL ست می‌کند)
 *       Priority      : Low — EEPROM کند است؛ نباید جلوی SD/LCD/صدا
 *                       را بگیرد
 *       FIFO          : پیش‌فرض (Disable) کافی است
 *
 *   NVIC:
 *     I2C1 event interrupt : ON  <- HAL در انتقال DMA به آن نیاز دارد
 *     I2C1 error interrupt : ON
 *     وقفهٔ استریم‌های DMA  : با Addشدن در DMA Settings خودش روشن می‌شود
 *
 *   ترتیب در main: MX_DMA_Init() قبل از MX_I2C1_Init() (کیوب خودش
 *   درست می‌چیند) و هر دو قبل از Ex32_I2cEeprom_Begin().
 *
 *   هشدار حافظه (H7): DMA1/DMA2 به DTCM (0x20000000) نمی‌رسند. اگر
 *   .bss پروژه‌ات در DTCM است، بافرهای EEPROM را در AXI/D2 یا SDRAM
 *   بگذار؛ وگرنه اولین انتقال DMA شکست می‌خورد و پل خودکار و دائمی
 *   blocking می‌شود — درست کار می‌کند، فقط دیگر DMA نمی‌بینی.
 *   صادقانه: انتقال‌های EEPROM کوچک‌اند (صفحهٔ ۳۲ تا ۱۲۸ بایت)؛ سود
 *   DMA بیشتر آزادسازی CPU است نه سرعت. اگر فقط خواندن‌های بزرگ
 *   (دامپ کالیبراسیون) DMA بروند، EX32_DMA_MIN_LEN را بالا ببرید
 *   (مثلاً 256).
 *
 * ---------------------------------------------------------------------
 * رفتار DMA / فالبک
 * ---------------------------------------------------------------------
 *   - اگر DMA اضافه نشده باشد (hi2c->hdmarx/hdmatx == NULL)، پل به‌صورت
 *     blocking کار می‌کند و هیچ تفاوتی از نظر نتیجه نیست.
 *   - اگر اولین انتقال DMA شکست بخورد (مثلاً بافر در DTCM است که
 *     DMA1/DMA2 به آن نمی‌رسد، یا وقفه‌ها روشن نیست)، پل یک‌بار برای
 *     همیشه به مسیر blocking برمی‌گردد؛ crash و hang در کار نیست.
 *   - انتقال‌های کوچک‌تر از EX32_DMA_MIN_LEN بایت مستقیم blocking
 *     می‌روند؛ برای چند بایت راه‌اندازی DMA صرف نمی‌کند.
 *
 *   نگهداری کش طبق پروتکل کتابخانه: FM_Cache_CleanForDma قبل از TX و
 *   FM_Cache_InvalidateAfterDma بعد از RX.
 *
 * ---------------------------------------------------------------------
 * نکتهٔ EEPROM که مثال ۲۸ هم می‌گوید
 * ---------------------------------------------------------------------
 *   بعد از هر نوشتن، قطعه مدت write_cycle_ms مشغول است و جواب نمی‌دهد.
 *   این پل به‌جای تأخیر کورکورانه، ACK-polling می‌کند (IsDeviceReady)
 *   و به‌محض آماده‌شدن برمی‌گردد — سریع‌تر از HAL_Delay ثابت.
 */

#ifndef FM_I2C_EEPROM_DMA_H
#define FM_I2C_EEPROM_DMA_H

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "stm32h7xx_hal.h"
#include "file_manager.h"
#include "fm_eeprom.h"
#include "fm_eeprom_chips.h"
#include "fm_cache.h"
#include "fm_log.h"

#include "eeprom_simple.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

#ifndef EX32_I2C_TIMEOUT_MS
#define EX32_I2C_TIMEOUT_MS 100u
#endif

#ifndef EX32_DMA_MIN_LEN
#define EX32_DMA_MIN_LEN 32u    /* کوچک‌تر از این: blocking ارزان‌تر است */
#endif

static I2C_HandleTypeDef *g_ex32_i2c;
static uint16_t           g_ex32_dev;      /* آدرس شیفت‌شده (مثل 0xA0) */
static FM_EepromChipInfo  g_ex32_info;
static FM_Bool            ex32_dma_ok = FM_TRUE;
static FM_Bool            ex32_dma_allowed = FM_TRUE;  /* کلید دستی  */

/* کلید دستی: مثلاً برای مقایسهٔ عملکرد با/بدون DMA روی همان سیم.
 * پیش‌فرض: خودکار — فقط به DMA وصل بودن در کیوب نگاه می‌کند. */
void Ex32_I2c_SetDma(FM_Bool enable)
{
    ex32_dma_allowed = enable;
}

static FM_Bool ex32_dma_usable(uint32_t len)
{
    return ex32_dma_allowed && ex32_dma_ok && g_ex32_i2c != NULL &&
           g_ex32_i2c->hdmarx != NULL && g_ex32_i2c->hdmatx != NULL &&
           len >= EX32_DMA_MIN_LEN;
}

/* پایان انتقال DMA: HAL در وقفهٔ I2C/DMA وضعیت را READY می‌کند. */
static FM_Result ex32_dma_wait(void)
{
    uint32_t t0 = HAL_GetTick();

    while (g_ex32_i2c->State != HAL_I2C_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > EX32_I2C_TIMEOUT_MS)
        {
            return FM_ERR_TIMEOUT;
        }
    }
    return FM_OK;
}

static void ex32_dma_fallback(const char *what)
{
    ex32_dma_ok = FM_FALSE;
    FM_Log("ex32: %s DMA failed - permanent blocking fallback\n", what);
}

/* بعد از نوشتن: صبر تا پایان چرخهٔ نوشتن با ACK-polling.
 * چیپ تا آماده‌شدن ACK نمی‌دهد — همین را تبدیل به سنسور آمادگی کردیم.
 * پنجره سخاوتمندانه است (3 برابرِ جدول + 10ms) چون بعضی کلون‌های 24xx
 * کندتر از دیتاشیت‌اند؛ ACK به‌محض آماده‌شدن برمی‌گردد، پس معمولاً
 * خیلی زودتر از سقف خارج می‌شویم. */
static FM_Result ex32_wait_write_cycle(void)
{
    uint32_t window = ((uint32_t)g_ex32_info.write_cycle_ms * 3u) + 10u;
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) <= window)
    {
        if (HAL_I2C_IsDeviceReady(g_ex32_i2c, g_ex32_dev, 1,
                                  2) == HAL_OK)
        {
            return FM_OK;
        }
        HAL_Delay(1);
    }
    return FM_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* دو تابع پروتکل FM_EEPROM — این‌ها کل پل سخت‌افزاری‌اند              */
/* ------------------------------------------------------------------ */

static FM_Result Ex32_ee_read(void *ctx, uint32_t address, void *data,
                              uint32_t length)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)ctx;
    uint16_t msize = (g_ex32_info.addr_bytes == 2u)
                         ? I2C_MEMADD_SIZE_16BIT
                         : I2C_MEMADD_SIZE_8BIT;

    if (ex32_dma_usable(length))
    {
        /* پروتکل کش: قبل از کیک، خطوط پوشش‌دهندهٔ بافر RX فلش شوند. */
        FM_Cache_CleanForDma(data, length);
        if (HAL_I2C_Mem_Read_DMA(hi2c, g_ex32_dev, (uint16_t)address,
                                 msize, (uint8_t *)data,
                                 (uint16_t)length) == HAL_OK)
        {
            if (ex32_dma_wait() == FM_OK &&
                hi2c->ErrorCode == HAL_I2C_ERROR_NONE)
            {
                /* کش را از دادهٔ کهنهٔ DMA خبر کن — پروتکل کتابخانه. */
                FM_Cache_InvalidateAfterDma(data, length);
                return FM_OK;
            }
        }
        ex32_dma_fallback("rx");
    }

    if (HAL_I2C_Mem_Read(hi2c, g_ex32_dev, (uint16_t)address, msize,
                         (uint8_t *)data, (uint16_t)length,
                         EX32_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result Ex32_ee_write(void *ctx, uint32_t address,
                               const void *data, uint32_t length)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)ctx;
    uint16_t msize = (g_ex32_info.addr_bytes == 2u)
                         ? I2C_MEMADD_SIZE_16BIT
                         : I2C_MEMADD_SIZE_8BIT;
    FM_Result r = FM_OK;

    if (ex32_dma_usable(length))
    {
        /* قبل از اینکه DMA از RAM بخواند، نوشته‌های CPU را بیرون بده. */
        FM_Cache_CleanForDma(data, length);

        if (HAL_I2C_Mem_Write_DMA(hi2c, g_ex32_dev, (uint16_t)address,
                                  msize, (uint8_t *)(uintptr_t)data,
                                  (uint16_t)length) == HAL_OK)
        {
            r = ex32_dma_wait();
            if (r == FM_OK && hi2c->ErrorCode == HAL_I2C_ERROR_NONE)
            {
                return ex32_wait_write_cycle();
            }
        }
        ex32_dma_fallback("tx");
    }

    if (HAL_I2C_Mem_Write(hi2c, g_ex32_dev, (uint16_t)address, msize,
                          (uint8_t *)(uintptr_t)data, (uint16_t)length,
                          EX32_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return ex32_wait_write_cycle();
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی یک‌جا                                                    */
/* ------------------------------------------------------------------ */
/*
 * chip          مدل EEPROM (FM_EEPROM_CHIP_24LC256 و ...)
 * hi2c          هندل I2C کیوب (&hi2c1)
 * dev_addr_7bit آدرس ۷بیتی قطعه؛ 0 بدهید تا پایهٔ جدول چیپ (0x50) شود.
 *               A2/A1 روی VCC؟ مقدارش را جمع بزنید: 0x50|A2<<2|A1<<1|A0
 */
static FM_Result Ex32_I2cEeprom_Begin(FM_EepromChip chip,
                                      I2C_HandleTypeDef *hi2c,
                                      uint8_t dev_addr_7bit)
{
    FM_Result r;

    g_ex32_i2c = hi2c;
    r = FM_Eeprom_GetChipInfo(chip, &g_ex32_info);
    if (r != FM_OK)
    {
        puts("  ex32: مدل EEPROM ناشناخته است.");
        return r;
    }
    if (g_ex32_info.is_spi)
    {
        puts("  ex32: این مدل SPI است — پل I2C به آن نمی‌خورد.");
        return FM_ERR_INVALID_ARGUMENT;
    }

    g_ex32_dev = (uint16_t)
        ((dev_addr_7bit != 0u ? dev_addr_7bit : g_ex32_info.i2c_addr) << 1);

    printf("  ex32: %s @ 0x%02X, page %lu B, write-cycle %u ms - %s\n",
           g_ex32_info.name, (unsigned)(g_ex32_dev >> 1),
           (unsigned long)g_ex32_info.page_size,
           (unsigned)g_ex32_info.write_cycle_ms,
           (hi2c->hdmarx != NULL && hi2c->hdmatx != NULL)
               ? "DMA mode" : "blocking mode");

    /* بقیهٔ راه‌اندازی و تست‌ها از مثال ۲۸ می‌آیند. */
    return EEPROM_examples_begin(chip, Ex32_ee_read, Ex32_ee_write, hi2c);
}

/* ------------------------------------------------------------------ */
/* exI_1 — تست دود: پروب، بنویس، بخوان، تطبیق بده                     */
/* ------------------------------------------------------------------ */

void exI_1_i2c_dma_smoke(void)
{
    /* تراز خط کش: با بافر تراز، بحث خطِ لبه و همسایه کلاً حذف می‌شود. */
    static FM_CACHE_ALIGN uint8_t out[64];
    static FM_CACHE_ALIGN uint8_t back[64];
    static FM_CACHE_ALIGN uint8_t back2[64];   /* خواندن مسیر معکوس */
    size_t i;
    FM_Bool same = FM_TRUE;
    FM_Bool same2 = FM_TRUE;
    FM_Bool was_dma;

    puts("\n============== exI_1_i2c_dma_smoke ===============");
    E_REQUIRE();

    if (HAL_I2C_IsDeviceReady(g_ex32_i2c, g_ex32_dev, 3,
                              EX32_I2C_TIMEOUT_MS) != HAL_OK)
    {
        puts("  دستگاه روی باس جواب نداد — سیم‌کشی/پول‌آپ/آدرس را چک کن.");
        return;
    }
    puts("  probe: ACK گرفت.");

    for (i = 0; i < sizeof(out); i++)
    {
        out[i] = (uint8_t)(i * 5u + 1u);
    }

    e_step("write 64B",
           EEPROM_Write(&g_ee, 0u, out, (uint32_t)sizeof(out)));

    was_dma = ex32_dma_ok;
    e_step("read 64B (path A)",
           EEPROM_Read(&g_ee, 0u, back, (uint32_t)sizeof(back)));

    /* خواندن دوم در مسیر معکوس — عیب‌یابی: اگر A خراب و B درست بود،
     * مشکل مسیر DMA است؛ اگر هر دو خراب بودند، نوشتن نکشیده. */
    Ex32_I2c_SetDma(was_dma ? FM_FALSE : FM_TRUE);
    e_step("read 64B (path B)",
           EEPROM_Read(&g_ee, 0u, back2, (uint32_t)sizeof(back2)));
    Ex32_I2c_SetDma(FM_TRUE);      /* برگرد به حالت خودکار */

    for (i = 0; i < sizeof(out); i++)
    {
        if (back[i] != out[i]) { same = FM_FALSE; }
        if (back2[i] != out[i]) { same2 = FM_FALSE; }
    }

    if (!same || !same2)
    {
        /* چهار ناهمخوان اول با مقدار واقعی — برای تشخیص الگو */
        uint32_t shown = 0;

        for (i = 0; i < sizeof(out) && shown < 4u; i++)
        {
            if (back[i] != out[i])
            {
                printf("  [%02lu] want %02X got A=%02X B=%02X\n",
                       (unsigned long)i, out[i], back[i], back2[i]);
                shown++;
            }
            else if (back2[i] != out[i])
            {
                printf("  [%02lu] want %02X got B=%02X\n",
                       (unsigned long)i, out[i], back2[i]);
                shown++;
            }
        }
    }

    printf("  verify: A=%s B=%s  (%s)\n",
           same ? "OK" : "BAD", same2 ? "OK" : "BAD",
           was_dma ? "A=DMA B=blocking" : "A=blocking B=DMA");

    if (same && same2)
    {
        puts("  هر دو مسیر درست — حمل‌ونقل سالم است.");
    }
    else if (!same && same2)
    {
        puts("  فقط مسیر A خراب است — لاگ بالا را برای الگو بفرست.");
    }
    else if (same && !same2)
    {
        puts("  فقط مسیر B خراب است — لاگ بالا را برای الگو بفرست.");
    }
    else
    {
        puts("  هر دو مسیر خراب — نوشتن نکشیده (تأمین تغذیه/پول‌آپ/WP؟)");
    }

    /* نشستن روی آدرس 0 بی‌ضرر است: exE_2 در 256 و exE_3 در 512+ اند. */
}

#endif /* !FM_HOST_BUILD */
#endif /* FM_I2C_EEPROM_DMA_H */
