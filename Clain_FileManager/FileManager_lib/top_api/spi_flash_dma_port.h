/*
 * spi_flash_dma_port.h — فلش سری روی SPI معمولی، با پورت DMA
 * ===================================================================
 *
 * «حالت معمولی» همان چیزی است که همهٔ MCUها دارند: فلش NOR سری روی
 * پورت SPI ساده (نه QUADSPI/OCTOSPI). کتابخانه این مسیر را با
 * FM_SpiFlash_Init باز می‌کند و فقط یک callback از شما می‌خواهد:
 * transfer(ctx, tx, rx, len). این فایل همان callback را می‌نویسد —
 *
 *   - یک مسیر کد: اگر DMA در کیوب به SPI وصل باشد (hspi->hdmatx یا
 *     hdmarx != NULL) و طول در پنجره باشد، انتقال با DMA می‌رود؛
 *     وگرنه همان تابع blocking. هیچ نسخهٔ دومی از کد وجود ندارد.
 *   - اگر اولین انتقال DMA شکست بخورد (وقفهٔ خاموش، بافر در DTCM،
 *     پیکربندی غلط DMA و ...)، یک‌بار برای همیشه به blocking
 *     برمی‌گردد — crash و hang در کار نیست.
 *   - نگهداری کش طبق پروتکل کتابخانه: Clean قبل از TX، Invalidate
 *     بعد از RX (اینجا چون full-duplex است، در صورت دوجهته بودن
 *     هر دو انجام می‌شود).
 *
 * استفاده در main:
 *
 *     #include "spi_flash_dma_port.h"
 *     ...
 *     MX_SPI1_Init();
 *     static FM_SpiFlashPort spi_port;
 *     static FM_QspiDevice   flash;          <- یک حافظهٔ عادی کتابخانه
 *     static uint8_t sector_buf[4096];
 *
 *     if (SpiFlash_Begin(&flash, &spi_port, &hspi1,
 *                             FM_QSPI_CHIP_AUTO,
 *                             sector_buf, sizeof(sector_buf)) == FM_OK)
 *     {
 *         یک‌بار برای فلش نو: SpiFlash_Format();
 *         SpiFlashDemo_Smoke();      <- دود: write/read/verify
 *         SpiFlashDemo_FileOps();             <- متن/باینری/CRC/پوشه
 *         SpiFlashDemo_SdToFlash();          <- انتقال SD به فلش با CRC
 *         SpiFlashDemo_RamRoundtrip();        <- RAM به فلش و برگشت
 *         SpiFlashDemo_Stats();                <- آمار erase/write/پرش
 *         SpiFlash_End();          <- اختیاری: بستن
 *     }
 *
 *     نمایش روی USB (مثل QSPI usb_present.h):
 *       UsbPresent_ShowSpiFlash()  -  release/acquire اش خودش هست
 *
 * قاعدهٔ سوئیت‌های file/transfers روی /flash همان /qspi است؛ تفاوت
 * فقط درایورِ سخت‌افزار زیرش است (SPI معمولی به‌جای QUADSPI).
 *
 * تنظیمات CubeMX (هیچ وابستگی به خانواده — F1 تا H7 یکسان):
 *   - SPIx: Full-Duplex Master، نرخ امن برای اولین راه‌اندازی
 *     (مثلاً prescaler تا زیر نصف حداکثر فلش، ~۱۰ مگاهرتز خوب است)،
 *     CPOL=Low / CPHA=1Edge (حالت ۰، استاندارد اکثر فلش‌ها)،
 *     NSS=Disable (CS نرم‌افزاری با GPIO).
 *   - پایهٔ CS فلش: قبل از include این را به پایهٔ خودت بده (پیش‌فرض PB6):
 *       #define SPI_FLASH_CS_PORT  GPIOB
 *       #define SPI_FLASH_CS_PIN   GPIO_PIN_6
 *     و پین را در کیوب Output Push-Pull + سطح اولیه High بگذار.
 *     CS با cs_select/cs_release دور «کل قاب» هر op کوبیده می‌شود
 *     (فرمان+آدرس+داده = یک فریم — این را spif_txn انجام می‌دهد).
 *     پایهٔ CS حتماً در CubeMX: GPIO_Output / Push-Pull / Level:High —
 *     Begin حالت واقعی MODER را چک می‌کند و اگر INPUT باشد هشدار می‌دهد.
 *     نکتهٔ سخت‌افزاری: WP (پایهٔ 3) و HOLD (پایهٔ 7) قطعه هم باید High
 *     باشند وگرنه DO سه‌حالته می‌ماند و همهٔ خواندن‌ها 0x00 است.
 *     سرعت: W25Q64 روی خواندن تک‌خطی max 50MHz است؛ در bring-up
 *     پری‌اسکیلر SPI4 را جوری بگذار SCK حدود 5 تا 10MHz (حتی کمتر) —
 *     با سیم‌بندی روی برد، کلاک بالا گاهی دستورها را خراب می‌کند و
 *     قطعه در وسط یک erase «گیر» می‌کند (لاگ: status 0x03 = WIP+WEL
 *     هولد شده).
 *   - DMA Settings: ریکوئست SPIx_RX و SPIx_TX (از DMAMUX یا جدول
 *     قدیمی، بسته به خانواده) + NVIC وقفهٔ آن‌ها و SPIx.
 *   - حافظهٔ بافر سکتور: DMA1/DMA2 به DTCM نمی‌رسند. اگر .bss پروژه در
 *     DTCM است، بافر را به ناحیهٔ دسترس‌پذیر ببر (H7: AXI/SDRAM):
 *       static uint8_t sector_buf[4096]
 *           __attribute__((section(".sdram_buffers"), aligned(32)));
 *     وگرنه اولین انتقال DMA شکست می‌خورد و برای همیشه blocking می‌شود
 *     (تست همان جواب می‌دهد، فقط DMA نمی‌بینی).
 *
 * نکته: CS را همین‌جا مدیریت می‌کنیم؛ transfer کل قاب را در بر
 * می‌گیرد (select → انتقال → release) تا درایور کتابخانه از CS بی‌خبر
 * باشد و DMA هم هرگز بین فرمان و CS آزاد، وقفه نخورد.
 */

#ifndef FM_TOPAPI_SPI_FLASH_DMA_PORT_H
#define FM_TOPAPI_SPI_FLASH_DMA_PORT_H

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "file_manager.h"
#include "fm_spiflash.h"
#include "fm_transfer.h"
#include "fm_crc.h"
#include "fm_cache.h"
#include "fm_log.h"

/* CRC-32 for the round-trip checks below.  With FM_USE_CRC=0 the library
 * has no CRC module, so the example carries the same loop itself
 * (reflected 0xEDB88320, no init / final XOR: identical results to
 * CRC_Compute32) instead of breaking the build. */
#ifndef DEMO_CRC32
#if FM_USE_CRC
#define DEMO_CRC32(data, len, crc) CRC_Compute32((data), (len), (crc))
#else
static uint32_t demo_crc32_local(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    unsigned j;

    for (i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (j = 0; j < 8u; j++)
        {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}
#define DEMO_CRC32(data, len, crc) demo_crc32_local((data), (len), (crc))
#endif
#endif /* DEMO_CRC32 */

#define printf FM_Log

/* گام‌های تست با نام یکتا - با eeprom_demo_step ی هدرهای دیگر (مثل 28) تداخل نکند */
static void spi_flash_demo_step(const char *what, FM_Result r)
{
    printf("  %s: %s\n", what, (r == FM_OK) ? "ok" : "FAILED");
}

#ifndef SPI_FLASH_DMA_MIN_LEN
#define SPI_FLASH_DMA_MIN_LEN 64u    /* کوچک‌تر از این: blocking ارزان‌تر است */
#endif
#ifndef SPI_FLASH_DMA_MAX_LEN
#define SPI_FLASH_DMA_MAX_LEN 4096u  /* سقف امن رجیستر اندازهٔ انتقال SPI    */
#endif

#ifndef SPI_FLASH_TIMEOUT_MS
#define SPI_FLASH_TIMEOUT_MS 500u
#endif

static FM_Bool spi_flash_dma_ok = FM_TRUE;   /* بعد از اولین شکست: FALSE    */

static FM_Bool spi_flash_dma_usable(const SPI_HandleTypeDef *h, uint32_t len)
{
    return spi_flash_dma_ok && h != NULL && h->hdmatx != NULL &&
           h->hdmarx != NULL && len >= SPI_FLASH_DMA_MIN_LEN &&
           len <= SPI_FLASH_DMA_MAX_LEN;
}

/* پایان انتقال DMA: HAL در وقفهٔ SPI/DMA وضعیت را READY می‌کند. */
/* بعد از پایان DMA ممکن است SPI هنوز BSY باشد یا بایتی در FIFO مانده
 * باشد — «op بعدی» را شیفت می‌دهد و به‌شکل یک erase گیرکرده/فیکس-خطا
 * دیده می‌شود (لاگ برد: اولین op بعد از page program گیر می‌کرد ولی
 * chip erase در context تمیز 16s سالم تمام می‌شد). */
static void spi_flash_bus_quiet(SPI_HandleTypeDef *h)
{
    if (h == NULL || h->Instance == NULL)
    {
        return;
    }
#ifdef SPI_SR_BSY
    {
        uint32_t t0 = HAL_GetTick();

        while ((h->Instance->SR & SPI_SR_BSY) != 0u)
        {
            if ((HAL_GetTick() - t0) > 10u)
            {
                break;
            }
        }
    }
#endif
    (void)HAL_SPI_FlushRxFifo(h);
}

static FM_Result spi_flash_dma_wait(SPI_HandleTypeDef *h)
{
    uint32_t t0 = HAL_GetTick();

    while (h->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > SPI_FLASH_TIMEOUT_MS)
        {
            (void)HAL_SPI_Abort(h);   /* باس را برای blocking سالم کن */
            spi_flash_bus_quiet(h);
            return FM_ERR_TIMEOUT;
        }
    }
    spi_flash_bus_quiet(h);
    return FM_OK;
}

static void spi_flash_dma_fallback(const char *what)
{
    spi_flash_dma_ok = FM_FALSE;
    FM_Log("SpiFlash: %s DMA failed - permanent blocking fallback\n", what);
}

/* ------------------------------------------------------------------ */
/* callback یکتای کتابخانه — DMA محور، تک‌مسیره                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* CS نرم‌افزاری — حیاتی: فلش بدون بالا/پایین‌شدن CS جواب نمی‌دهد       */
/* ------------------------------------------------------------------ */
/* پیش‌فرض: PB6. با SPI_FLASH_CS_PORT/SPI_FLASH_CS_PIN قبل از include عوض کن،
 * یا در runtime با SpiFlash_SetCs(). */
#ifndef SPI_FLASH_CS_PORT
#define SPI_FLASH_CS_PORT  GPIOB
#endif
#ifndef SPI_FLASH_CS_PIN
#define SPI_FLASH_CS_PIN   GPIO_PIN_6
#endif

static GPIO_TypeDef *spi_flash_cs_port = SPI_FLASH_CS_PORT;
static uint16_t      spi_flash_cs_pin  = SPI_FLASH_CS_PIN;

/* اگر سیم‌کشی CS متفاوت است، قبل از Begin صدا بزن. */
void SpiFlash_SetCs(GPIO_TypeDef *port, uint16_t pin)
{
    spi_flash_cs_port = port;
    spi_flash_cs_pin  = pin;
}

/* امضا با FM_SpiFlashPort هم‌خوان است: (void *ctx) - ctx همان port. */
static void spi_flash_cs_low(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(spi_flash_cs_port, spi_flash_cs_pin, GPIO_PIN_RESET);
}

static void spi_flash_cs_high(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(spi_flash_cs_port, spi_flash_cs_pin, GPIO_PIN_SET);
}


static FM_Result spi_flash_transfer(void *ctx, const uint8_t *tx, uint8_t *rx,
                               uint32_t len)
{
    SPI_HandleTypeDef *h = (SPI_HandleTypeDef *)ctx;
    HAL_StatusTypeDef st;

    if (len == 0u) return FM_OK;
    if (len > SPI_FLASH_DMA_MAX_LEN) return FM_ERR_INVALID_ARGUMENT;

    /* CS اینجا کوبیده نمیشود! هر op فلش از دو فراخوانی transfer
     * ساخته میشود (سرایند، بعد داده) و CS باید دور «کل قاب» باشد —
     * این کار را cs_select/cs_release در spif_txn انجام میدهند.
     * CS داخل transfer یعنی هر دستور دو فریم نصفه = فلش از دستور
     * دوم به بعد 0x00 میدهد. */

    /* مسیر DMA — همان شرایط و همان کد برای هر سه حالت جهت.
     * پروتکل کش: TX و RX هر دو قبل از کیک Clean می‌شوند (RX یعنی
     * فلش‌کردن خطوط پوشش‌دهندهٔ بافر؛ Invalidateِ بعدش فقط می‌ریزد). */
    if (spi_flash_dma_usable(h, len))
    {
        if (tx != NULL) FM_Cache_CleanForDma(tx, len);
        if (rx != NULL) FM_Cache_CleanForDma(rx, len);

        if (tx != NULL && rx != NULL)
        {
            st = HAL_SPI_TransmitReceive_DMA(h, (uint8_t *)(uintptr_t)tx,
                                             rx, (uint16_t)len);
        }
        else if (tx != NULL)
        {
            st = HAL_SPI_Transmit_DMA(h, (uint8_t *)(uintptr_t)tx,
                                      (uint16_t)len);
        }
        else
        {
            st = HAL_SPI_Receive_DMA(h, rx, (uint16_t)len);
        }

        if (st == HAL_OK && spi_flash_dma_wait(h) == FM_OK)
        {
            if (rx != NULL) FM_Cache_InvalidateAfterDma(rx, len);
            return FM_OK;
        }
        spi_flash_dma_fallback("transfer");
        /* بدون برگشت خطا: ادامه با همان داده از مسیر blocking */
    }

    /* مسیر blocking (و فالبک یک‌بارهٔ DMA) */
    if (tx != NULL && rx != NULL)
    {
        st = HAL_SPI_TransmitReceive(h, (uint8_t *)(uintptr_t)tx, rx,
                                     (uint16_t)len, SPI_FLASH_TIMEOUT_MS);
    }
    else if (tx != NULL)
    {
        st = HAL_SPI_Transmit(h, (uint8_t *)(uintptr_t)tx,
                              (uint16_t)len, SPI_FLASH_TIMEOUT_MS);
    }
    else
    {
        st = HAL_SPI_Receive(h, rx, (uint16_t)len, SPI_FLASH_TIMEOUT_MS);
    }
    return (st == HAL_OK) ? FM_OK : FM_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */

/* پیشرفت پلهٔ ۲۵٪ — مثل exT: بدون اسپم */
static FM_Bool spi_flash_demo_progress(uint64_t done, uint64_t total, void *user)
{
    (void)user;
    if (total != 0u && ((done * 4u) / total) != (((done * 4u) - 1u) / total))
    {
        printf("    %lu%%\r",
               (unsigned long)((done * 100u) / total));
    }
    return FM_TRUE;
}

static FM_QspiDevice   *g_spi_flash_dev;
static FM_SpiFlashPort *g_spi_flash_portp;   /* برای عیب‌یابی باس */
static FM_Storage      g_spi_flash_st;      /* ثبت فلش به‌عنوان حافظهٔ کتابخانه */
static FM_FS          g_spi_flash_fs;      /* فایل‌سیستم FMFS روی /flash       */
static FM_Bool        g_spi_flash_mounted;

#ifndef SPI_FLASH_MOUNT_PATH
#define SPI_FLASH_MOUNT_PATH "/flash"
#endif
#ifndef SPI_FLASH_STORAGE_NAME
#define SPI_FLASH_STORAGE_NAME "flash"
#endif

/*
 * فلش نو/خالی؟ یک‌بار: SpiFlash_Format() — بعد از آن FS_Mount
 * درست کار می‌کند. مثل QSPI، فرمت خودکار نیست که داده از بین نرود.
 */
FM_Result SpiFlash_Format(void)
{
    FM_Result r;

    if (g_spi_flash_dev == NULL) return FM_ERR_NOT_READY;
    if (g_spi_flash_mounted)
    {
        r = FS_Format(SPI_FLASH_MOUNT_PATH);   /* فرمت از طریق مونت فعلی */
    }
    else
    {
        r = FMFS_Init(&g_spi_flash_fs);
        if (r == FM_OK) r = g_spi_flash_fs.ops.format(&g_spi_flash_fs);
        if (r == FM_OK) r = FS_Mount(SPI_FLASH_MOUNT_PATH, &g_spi_flash_fs);
    }
    if (r == FM_OK) g_spi_flash_mounted = FM_TRUE;
    printf("SpiFlash: format %s\n", (r == FM_OK) ? "OK" : "FAILED");
    return r;
}

/* انتقال مالکیت به PC (USB MSC usb_present.h): مونت بسته شود. */
FM_Result SpiFlash_ReleaseForUsb(void)
{
    FM_Result r = FM_OK;

    if (g_spi_flash_mounted)
    {
        r = FS_Unmount(SPI_FLASH_MOUNT_PATH);
        if (r == FM_OK) g_spi_flash_mounted = FM_FALSE;
    }
    if (r == FM_OK) r = Storage_Sync(SPI_FLASH_STORAGE_NAME);
    return r;
}

FM_Result SpiFlash_ReturnFromUsb(void)
{
    if (g_spi_flash_mounted) return FM_OK;
    {
        FM_Result r = FMFS_Init(&g_spi_flash_fs);

        if (r != FM_OK) return r;
        r = FS_Mount(SPI_FLASH_MOUNT_PATH, &g_spi_flash_fs);
        if (r == FM_OK) g_spi_flash_mounted = FM_TRUE;
        return r;
    }
}

/* برای USB MSC (usb_present.h): فلش ثبت‌شده را بده — اگر راه نیفتاده NULL. */
FM_Storage *SpiFlash_GetStorage(void)
{
    return (g_spi_flash_dev != NULL && g_spi_flash_mounted) ? &g_spi_flash_st : NULL;
}

void SpiFlash_End(void)
{
    if (g_spi_flash_mounted)
    {
        (void)FS_Unmount(SPI_FLASH_MOUNT_PATH);
        g_spi_flash_mounted = FM_FALSE;
    }
    (void)Storage_Unregister(SPI_FLASH_STORAGE_NAME);
    g_spi_flash_dev = NULL;
}

/* ------------------------------------------------------------------ */
/* پاک‌سازی کامل قطعه (chip erase) - با پیشرفت زنده                     */
/* ------------------------------------------------------------------ */
/* W25Q64: معمولاً ~20-25s، تا 100s طبق دیتاشیت. این تابع بهترین
 * عیب‌یاب گیرِ erase هم هست: اگر تا آخر پیشرفت کرد و تمام شد یعنی
 * قطعه/باس سالم‌اند و مشکل در مسیر erase-سکتوری بود؛ اگر وسطش WIP
 * هولد شد یعنی مشکل سخت‌افزاری است (کلاک/سیم‌بندی). */
FM_Result SpiFlash_EraseAll(void)
{
    FM_QspiDevice *dev = g_spi_flash_dev;
    FM_Result r;
    uint32_t t0;
    uint32_t last_pct = 0xFFu;

    if (dev == NULL)
    {
        FM_Log("%s\n", ("SpiFlash: aval SpiFlash_Begin()"));
        return FM_ERR_NOT_READY;
    }
    FM_Log("%s\n", ("SpiFlash: chip erase (ta 100s) ..."));
    r = FM_Qspi_EraseChipStart(dev);
    if (r != FM_OK)
    {
        printf("SpiFlash: erase-chip start failed (%d)\n", (int)r);
        return r;
    }
    t0 = HAL_GetTick();
    for (;;)
    {
        uint8_t status = 0;
        FM_Result rs = FM_Qspi_ReadStatus(dev, &status);
        uint32_t elapsed = HAL_GetTick() - t0;
        uint32_t pct;

        if (rs != FM_OK)
        {
            printf("SpiFlash: status read failed (%d)\n", (int)rs);
            return rs;
        }
        if ((status & 0x01u) == 0u)     /* WIP pak shod */
        {
            printf("SpiFlash: chip erase TAMAM dar %lu ms\n",
                   (unsigned long)elapsed);
            return FM_OK;
        }
        pct = (elapsed * 100u) / 25000u;
        if (pct > 100u) pct = 100u;
        if (pct != last_pct && (pct % 5u) == 0u)
        {
            printf("SpiFlash: erase %lu%% (%lu ms)\n",
                   (unsigned long)pct, (unsigned long)elapsed);
            last_pct = pct;
        }
        if (elapsed > 120000u)
        {
            FM_Log("%s\n", ("SpiFlash: !! chip erase ham gir kard - moshkel "
                            "sekt-hardware (SCK ra kam kon / simha)"));
            return FM_ERR_TIMEOUT;
        }
        HAL_Delay(50u);
    }
}

/* عیب‌یابی سیم‌کشی: یک فریم 9F خام با پیش‌پرکردن 0xAA.
 *   خروجی EF 40 17  -> باس سالم است؛ مشکل جای دیگری بود.
 *   0x00 0x00 0x00  -> MISO صفر میدهد: سیم MISO قطع، یا WP(پایه3)/
 *                      HOLD(پایه7) به 3V وصل نیست، یا AF پین‌ها غلط.
 *   0xAA 0xAA 0xAA  -> اصلاً کلاک نرفت: handle SPI یا پین‌های SCK/MOSI. */
void SpiFlash_BusDiag(void)
{
    uint8_t hdr = 0x9F;
    uint8_t tx[3] = { 0xFFu, 0xFFu, 0xFFu };
    uint8_t rx[3];
    SPI_HandleTypeDef *h;

    if (g_spi_flash_portp == NULL)
    {
        FM_Log("%s\n", "SpiFlash diag: aval SpiFlash_Begin()");
        return;
    }
    h = (SPI_HandleTypeDef *)g_spi_flash_portp->ctx;
    memset(rx, 0xAA, sizeof(rx));
    spi_flash_cs_low(NULL);
    (void)spi_flash_transfer(h, &hdr, NULL, 1u);
    (void)spi_flash_transfer(h, tx, rx, 3u);
    spi_flash_cs_high(NULL);
    printf("SpiFlash diag: JEDEC = %02X %02X %02X (salem: EF 40 17)\n",
           rx[0], rx[1], rx[2]);
    if (rx[0] == 0xAAu && rx[1] == 0xAAu && rx[2] == 0xAAu)
    {
        FM_Log("%s\n", "SpiFlash diag: 0xAA -> SPI clock nakard - SCK/MOSI ya handle");
    }
    else if (rx[0] == 0x00u && rx[1] == 0x00u && rx[2] == 0x00u)
    {
        FM_Log("%s\n", "SpiFlash diag: 0x00 -> MISO javab nemidad - sim MISO, ya "
             "WP(pin3)/HOLD(pin7) bayad High bashand");
    }
}

static FM_Result SpiFlash_Begin(FM_QspiDevice *dev,
                                     FM_SpiFlashPort *port,
                                     SPI_HandleTypeDef *hspi,
                                     FM_QspiChip chip,
                                     uint8_t *sector_buf,
                                     uint32_t sector_buf_size)
{
    FM_Result r;
    uint32_t id = 0;

    memset(port, 0, sizeof(*port));
    port->transfer = spi_flash_transfer;
    port->cs_select = spi_flash_cs_low;    /* دور کل قاب هر op - در spif_txn */
    port->cs_release = spi_flash_cs_high;
    port->delay_ms = NULL;
    port->ctx = hspi;

    r = FM_SpiFlash_Init(dev, port, chip, sector_buf, sector_buf_size);
    if (r != FM_OK)
    {
        printf("SpiFlash: spi flash init failed (%d)\n", (int)r);
        return r;
    }
    g_spi_flash_dev = dev;
    g_spi_flash_portp = port;

    r = FM_Qspi_ReadId(dev, &id);
    if (r != FM_OK)
    {
        printf("SpiFlash: flash does not answer (JEDEC read failed)\n");
        return r;
    }
    {
        uint32_t cs_pos = 0;

        while (cs_pos < 16u && ((spi_flash_cs_pin >> cs_pos) & 1u) == 0u)
        {
            cs_pos++;
        }
        if ((spi_flash_cs_port->MODER & (3uL << (cs_pos * 2u))) == 0u)
        {
            FM_Log("%s\n", "SpiFlash: !! CS pin dar halate INPUT ast! dar CubeMX: "
                 "GPIO_Output + Push-Pull + Level:High (bedun-e in CS "
                 "sado nemishavad va flash javab nemidad)");
        }
        else
        {
            printf("SpiFlash: CS port %p pin %u - OUTPUT OK\n",
                   (void *)spi_flash_cs_port, (unsigned)cs_pos);
        }
    }
    printf("SpiFlash: %s ready, JEDEC 0x%06lX, %lu KB - %s\n",
           dev->info.name, (unsigned long)id,
           (unsigned long)(dev->info.size_bytes / 1024u),
           spi_flash_dma_ok ? "DMA-capable port" : "blocking port");
    {
        /* روی سیم‌بندی سیمی، کلاک بالای ~10MHz برای erase/program
         * پرریسک است (خواندن تحمل می‌کند، دستور نوشتن ممکن است خراب
         * لچ شود). MBR در CFG1 بیت‌های 5:3 است: prescaler = 2^(MBR+1).
         * مثلاً SPI4 با کلاک 120MHz و MBR=3 یعنی 7.5MHz - خوب. */
        SPI_HandleTypeDef *h = (SPI_HandleTypeDef *)port->ctx;

        if (h != NULL && h->Instance != NULL)
        {
            uint32_t mbr = ((h->Instance->CFG1 >> 3) & 7u);
            uint32_t div = (1uL << (mbr + 1u));

            if (div <= 8u)
            {
                /* لاگ برد: با /2 (60MHz!) روی سیم‌بندی سیمی، خواندن سالم
                 * است ولی هر erase ~۴ ثانیه گیر می‌کند (فریم دستور خراب
                 * لچ می‌شود). زیر 10MHz برو. */
                printf("SpiFlash: !! SPI prescaler /%lu (=sari'e ziadesh baraye "
                       "sim-basi) - dar CubeMX: SPI4 Prescaler = 64 bezan, "
                       "bad az erase haye ~45ms mibini\n",
                       (unsigned long)div);
            }
            else
            {
                printf("SpiFlash: SPI prescaler /%lu - OK baraye sim-basi\n",
                       (unsigned long)div);
            }
        }
    }

#ifndef SPI_FLASH_NO_SCRATCH_ERASE
    /* عیب‌یابی یک‌خطی پاک‌سازی: آخرین سکتور (به‌ندرت استفاده‌شده) را
     * پاک کن و زمان واقعی را نشان بده. سالم: زیر ~400ms. اگر تا 4
     * ثانیه هم تمام نشد، قطعه یا کند واقعی است یا در حالت گیر.
     * غیرفعال‌سازی: SPI_FLASH_NO_SCRATCH_ERASE قبل از include تعریف کن. */
    {
        uint32_t t0 = HAL_GetTick();
        /* سکتور صفر - دقیقاً همان آدرسی که مسیر mount گیر می‌کند.
         * اگر این هم ۵ms جواب داد ولی mount گیر کرد، مشکل در فریم‌بندی
         * آن مسیر خاص است نه قطعه؛ اگر این هم گیر کرد، وابسته به آدرس
         * است (سخت‌افزار/قطعه). */
        FM_Result re = FM_Qspi_Erase(dev, 0u,
                                     (uint64_t)dev->info.sector_size);

        if (re == FM_OK)
        {
            printf("SpiFlash: scratch erase OK dar %lu ms (salem: <400)\n",
                   (unsigned long)(HAL_GetTick() - t0));
        }
        else
        {
            printf("SpiFlash: !! scratch erase NASHOD (%d) - taghvie ya "
                   "kiep gire\n", (int)re);
        }
    }
#endif

    /* ثبت به‌عنوان حافظهٔ کتابخانه + مونت /flash (FMFS).
     * FMFS پیش‌فرض است تا با SD روی درایو 0 فت‌فس تداخل نکند؛
     * برای USB MSC، PC خودش FAT می‌گذارد یا SPI_FLASH_USE_FATFS بسازید. */
    {
        FM_Result rs = FM_Qspi_RegisterStorage(dev, &g_spi_flash_st,
                                               SPI_FLASH_STORAGE_NAME);

        if (rs != FM_OK && rs != FM_ERR_EXISTS)
        {
            printf("SpiFlash: register storage failed (%d)\n", (int)rs);
            return rs;
        }
        rs = FMFS_Init(&g_spi_flash_fs);
        if (rs != FM_OK)
        {
            printf("SpiFlash: FMFS_Init failed (%d)\n", (int)rs);
            return rs;
        }
        rs = FS_Mount(SPI_FLASH_MOUNT_PATH, &g_spi_flash_fs);
        if (rs == FM_OK)
        {
            g_spi_flash_mounted = FM_TRUE;
            FM_Log("%s\n", "SpiFlash: /flash mounted (FMFS)");
        }
        else
        {
            printf("SpiFlash: /flash khali/new (mount=%d) - yek bar: "
                   "SpiFlash_Format();\n", (int)rs);
        }
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* SpiFlashDemo_Smoke — تست دود: بنویس، بخوان، تطبیق بده                            */
/* ------------------------------------------------------------------ */

void SpiFlashDemo_Smoke(void)
{
    static FM_CACHE_ALIGN uint8_t out[256];   /* تراز خط کش */
    static FM_CACHE_ALIGN uint8_t back[256];
    size_t i;
    FM_Bool same = FM_TRUE;

    FM_Log("%s\n", "\n============== SpiFlashDemo_Smoke ==============");
    if (g_spi_flash_dev == NULL)
    {
        FM_Log("%s\n", "  اول SpiFlash_Begin() را صدا بزنید.");
        return;
    }

    for (i = 0; i < sizeof(out); i++)
    {
        out[i] = (uint8_t)(i * 7u + 3u);
    }

    if (FM_Qspi_Write(g_spi_flash_dev, 0, out, sizeof(out)) != FM_OK)
    {
        FM_Log("%s\n", "  write failed");
        return;
    }
    if (FM_Qspi_Read(g_spi_flash_dev, 0, back, sizeof(back)) != FM_OK)
    {
        FM_Log("%s\n", "  read failed");
        return;
    }
    for (i = 0; i < sizeof(out); i++)
    {
        if (back[i] != out[i]) { same = FM_FALSE; break; }
    }
    printf("  verify: %s  (%s, erases=%lu skipped=%lu)\n",
           same ? "IDENTICAL" : "CORRUPTED",
           spi_flash_dma_ok ? "DMA" : "blocking",
           (unsigned long)g_spi_flash_dev->stat_erases,
           (unsigned long)g_spi_flash_dev->stat_erases_skipped);
}

/* ------------------------------------------------------------------ */
/* SpiFlashDemo_FileOps — عملیات فایل روی /flash: متن، باینری+CRC، پوشه               */
/* ------------------------------------------------------------------ */

void SpiFlashDemo_FileOps(void)
{
    static uint8_t blob[1024];
    static uint8_t back[1024];
    char text[64];
    FM_Dir *d;
    FM_DirEntry e;
    size_t i;
    uint32_t crc_w;
    uint32_t crc_r;

    FM_Log("%s\n", "\n=============== SpiFlashDemo_FileOps ====================");
    if (g_spi_flash_dev == NULL || !g_spi_flash_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }

    /* ۱) متن: بنویس/بخوان/بازنویسی */
    spi_flash_demo_step("write text",
           File_WriteText(SPI_FLASH_MOUNT_PATH "/hello.txt",
                          "Hello from plain-SPI flash!"));
    File_ReadText(SPI_FLASH_MOUNT_PATH "/hello.txt", text, sizeof(text), NULL);
    printf("  read back: %s\n", text);

    /* ۲) باینری ۱KB + CRC رفت‌وبرگشت (از مسیر DMA اگر فعال باشد) */
    for (i = 0; i < sizeof(blob); i++)
    {
        blob[i] = (uint8_t)(i * 13u + 5u);
    }
    crc_w = DEMO_CRC32(blob, sizeof(blob), 0u);
    spi_flash_demo_step("write 1K",
           File_WriteAll(SPI_FLASH_MOUNT_PATH "/blob.bin",
                         blob, (uint32_t)sizeof(blob)));
    spi_flash_demo_step("read 1K",
           File_ReadAll(SPI_FLASH_MOUNT_PATH "/blob.bin",
                        back, sizeof(back), NULL));
    crc_r = DEMO_CRC32(back, sizeof(back), 0u);
    printf("  CRC 1K: %08lX -> %08lX : %s\n",
           (unsigned long)crc_w, (unsigned long)crc_r,
           (crc_w == crc_r) ? "IDENTICAL" : "CORRUPTED");

    /* ۳) پوشه + فهرست */
    (void)Directory_Create(SPI_FLASH_MOUNT_PATH "/cfg");
    spi_flash_demo_step("write cfg",
           File_WriteText(SPI_FLASH_MOUNT_PATH "/cfg/net.txt", "ip=10.0.0.5"));
    FM_Log("%s\n", "  /flash:");
    d = Directory_Open(SPI_FLASH_MOUNT_PATH);
    if (d != NULL)
    {
        while (Directory_Read(d, &e) == FM_OK)
        {
            printf("    [%c] %-16s %lu\n",
                   (e.type == FM_ITEM_DIR) ? 'D' : 'F',
                   e.name, (unsigned long)e.size);
        }
        (void)Directory_Close(d);
    }

    /* ۴) پاک‌سازی فایل‌های دموی همین تست (نه کل فلش) */
    (void)File_Delete(SPI_FLASH_MOUNT_PATH "/hello.txt");
    (void)File_Delete(SPI_FLASH_MOUNT_PATH "/blob.bin");
    (void)File_Delete(SPI_FLASH_MOUNT_PATH "/cfg/net.txt");
    (void)Directory_Delete(SPI_FLASH_MOUNT_PATH "/cfg");
}

/* ------------------------------------------------------------------ */
/* SpiFlashDemo_SdToFlash — SD -> فلش با وریفای CRC (کاربرد: فونت/لوگو بدون کارت)       */
/* ------------------------------------------------------------------ */

void SpiFlashDemo_SdToFlash(void)
{
    FM_TransferOpts o;
    FM_TransferInfo info;
    FM_Result r;
    const char *src = "/config.json";
    FM_Bool exists = FM_FALSE;

    FM_Log("%s\n", "\n=============== SpiFlashDemo_SdToFlash ================");
    if (g_spi_flash_dev == NULL || !g_spi_flash_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }
    if (File_Exists(src, &exists) != FM_OK || !exists)
    {
        src = "/spi_flash_src.txt";
        spi_flash_demo_step("make demo source",
               File_WriteText(src, "SpiFlash transfer demo payload"));
    }

    memset(&o, 0, sizeof(o));
    o.verify = FM_TRUE;
    o.progress = spi_flash_demo_progress;
    r = FM_Transfer_CopyEx(src, SPI_FLASH_MOUNT_PATH "/from_sd.bin",
                           &o, &info);
    if (r == FM_OK)
    {
        printf("  copied %lu bytes from %s, CRC 0x%08lX (verified)\n",
               (unsigned long)info.bytes, src,
               (unsigned long)info.crc_dest);
    }
}

/* ------------------------------------------------------------------ */
/* SpiFlashDemo_RamRoundtrip — RAM -> فلش -> RAM با CRC (رفت‌وبرگشت کامل مسیر DMA)         */
/* ------------------------------------------------------------------ */

void SpiFlashDemo_RamRoundtrip(void)
{
    static FM_CACHE_ALIGN uint8_t src[2048];
    static FM_CACHE_ALIGN uint8_t dst[2048];
    size_t i;
    size_t got = 0;
    uint32_t c1;
    uint32_t c2;

    FM_Log("%s\n", "\n============== SpiFlashDemo_RamRoundtrip ===============");
    if (g_spi_flash_dev == NULL || !g_spi_flash_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }

    for (i = 0; i < sizeof(src); i++)
    {
        src[i] = (uint8_t)((i * 31u) ^ (i >> 3));
    }
    c1 = DEMO_CRC32(src, sizeof(src), 0u);

    spi_flash_demo_step("RAM -> flash",
           FM_Transfer_SaveFile(SPI_FLASH_MOUNT_PATH "/rt.bin",
                                src, sizeof(src)));
    spi_flash_demo_step("flash -> RAM",
           FM_Transfer_LoadFile(SPI_FLASH_MOUNT_PATH "/rt.bin",
                                dst, sizeof(dst), &got));
    c2 = DEMO_CRC32(dst, got, 0u);
    printf("  %lu bytes, CRC %08lX -> %08lX : %s\n",
           (unsigned long)got, (unsigned long)c1, (unsigned long)c2,
           (c1 == c2 && got == sizeof(src)) ? "IDENTICAL" : "CORRUPTED");
    (void)File_Delete(SPI_FLASH_MOUNT_PATH "/rt.bin");
}

/* ------------------------------------------------------------------ */
/* SpiFlashDemo_Stats — آمار: چقدر نوشتیم/پاک کردیم/چند سکتور خالی رد شد            */
/* ------------------------------------------------------------------ */

void SpiFlashDemo_Stats(void)
{
    FM_Log("%s\n", "\n================= SpiFlashDemo_Stats ====================");
    if (g_spi_flash_dev == NULL)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }
    printf("  reads=%lu writes=%lu erases=%lu blank-skipped=%lu "
           "mmap? (SPI: no)\n",
           (unsigned long)g_spi_flash_dev->stat_reads,
           (unsigned long)g_spi_flash_dev->stat_writes,
           (unsigned long)g_spi_flash_dev->stat_erases,
           (unsigned long)g_spi_flash_dev->stat_erases_skipped);
    printf("  data phase: %s\n",
           spi_flash_dma_ok ? "DMA" : "blocking");
}

#endif /* !FM_HOST_BUILD */
#endif /* FM_TOPAPI_SPI_FLASH_DMA_PORT_H */
