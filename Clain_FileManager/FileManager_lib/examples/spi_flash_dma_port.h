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
 *     if (Ex33_SpiFlash_Begin(&flash, &spi_port, &hspi1,
 *                             FM_QSPI_CHIP_AUTO,
 *                             sector_buf, sizeof(sector_buf)) == FM_OK)
 *     {
 *         یک‌بار برای فلش نو: Ex33_SpiFlash_Format();
 *         exF_1_spi_flash_smoke();      <- دود: write/read/verify
 *         exF_2_file_ops();             <- متن/باینری/CRC/پوشه
 *         exF_3_sd_to_flash();          <- انتقال SD به فلش با CRC
 *         exF_4_ram_roundtrip();        <- RAM به فلش و برگشت
 *         exF_5_stats();                <- آمار erase/write/پرش
 *         Ex33_SpiFlash_End();          <- اختیاری: بستن
 *     }
 *
 *     نمایش روی USB (مثل QSPI مثال ۳۴):
 *       Ex34_Usb_PresentSpiFlash()  -  release/acquire اش خودش هست
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
 *       #define EX33_CS_PORT  GPIOB
 *       #define EX33_CS_PIN   GPIO_PIN_6
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

#ifndef FM_EXAMPLE_33_STM32_SPI_FLASH_DMA_PORT_H
#define FM_EXAMPLE_33_STM32_SPI_FLASH_DMA_PORT_H

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

#define printf FM_Log

/* گام‌های تست با نام یکتا - با e_step ی هدرهای دیگر (مثل 28) تداخل نکند */
static void ex33_step(const char *what, FM_Result r)
{
    printf("  %s: %s\n", what, (r == FM_OK) ? "ok" : "FAILED");
}

#ifndef EX33_DMA_MIN_LEN
#define EX33_DMA_MIN_LEN 64u    /* کوچک‌تر از این: blocking ارزان‌تر است */
#endif
#ifndef EX33_DMA_MAX_LEN
#define EX33_DMA_MAX_LEN 4096u  /* سقف امن رجیستر اندازهٔ انتقال SPI    */
#endif

#ifndef EX33_SPI_TIMEOUT_MS
#define EX33_SPI_TIMEOUT_MS 500u
#endif

static FM_Bool ex33_dma_ok = FM_TRUE;   /* بعد از اولین شکست: FALSE    */

static FM_Bool ex33_dma_usable(const SPI_HandleTypeDef *h, uint32_t len)
{
    return ex33_dma_ok && h != NULL && h->hdmatx != NULL &&
           h->hdmarx != NULL && len >= EX33_DMA_MIN_LEN &&
           len <= EX33_DMA_MAX_LEN;
}

/* پایان انتقال DMA: HAL در وقفهٔ SPI/DMA وضعیت را READY می‌کند. */
/* بعد از پایان DMA ممکن است SPI هنوز BSY باشد یا بایتی در FIFO مانده
 * باشد — «op بعدی» را شیفت می‌دهد و به‌شکل یک erase گیرکرده/فیکس-خطا
 * دیده می‌شود (لاگ برد: اولین op بعد از page program گیر می‌کرد ولی
 * chip erase در context تمیز 16s سالم تمام می‌شد). */
static void ex33_spi_quiet(SPI_HandleTypeDef *h)
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

static FM_Result ex33_dma_wait(SPI_HandleTypeDef *h)
{
    uint32_t t0 = HAL_GetTick();

    while (h->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > EX33_SPI_TIMEOUT_MS)
        {
            (void)HAL_SPI_Abort(h);   /* باس را برای blocking سالم کن */
            ex33_spi_quiet(h);
            return FM_ERR_TIMEOUT;
        }
    }
    ex33_spi_quiet(h);
    return FM_OK;
}

static void ex33_dma_fallback(const char *what)
{
    ex33_dma_ok = FM_FALSE;
    FM_Log("ex33: %s DMA failed - permanent blocking fallback\n", what);
}

/* ------------------------------------------------------------------ */
/* callback یکتای کتابخانه — DMA محور، تک‌مسیره                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* CS نرم‌افزاری — حیاتی: فلش بدون بالا/پایین‌شدن CS جواب نمی‌دهد       */
/* ------------------------------------------------------------------ */
/* پیش‌فرض: PB6. با EX33_CS_PORT/EX33_CS_PIN قبل از include عوض کن،
 * یا در runtime با Ex33_SpiFlash_SetCs(). */
#ifndef EX33_CS_PORT
#define EX33_CS_PORT  GPIOB
#endif
#ifndef EX33_CS_PIN
#define EX33_CS_PIN   GPIO_PIN_6
#endif

static GPIO_TypeDef *ex33_cs_port = EX33_CS_PORT;
static uint16_t      ex33_cs_pin  = EX33_CS_PIN;

/* اگر سیم‌کشی CS متفاوت است، قبل از Begin صدا بزن. */
void Ex33_SpiFlash_SetCs(GPIO_TypeDef *port, uint16_t pin)
{
    ex33_cs_port = port;
    ex33_cs_pin  = pin;
}

/* امضا با FM_SpiFlashPort هم‌خوان است: (void *ctx) - ctx همان port. */
static void ex33_cs_low(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ex33_cs_port, ex33_cs_pin, GPIO_PIN_RESET);
}

static void ex33_cs_high(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ex33_cs_port, ex33_cs_pin, GPIO_PIN_SET);
}


static FM_Result ex33_transfer(void *ctx, const uint8_t *tx, uint8_t *rx,
                               uint32_t len)
{
    SPI_HandleTypeDef *h = (SPI_HandleTypeDef *)ctx;
    HAL_StatusTypeDef st;

    if (len == 0u) return FM_OK;
    if (len > EX33_DMA_MAX_LEN) return FM_ERR_INVALID_ARGUMENT;

    /* CS اینجا کوبیده نمیشود! هر op فلش از دو فراخوانی transfer
     * ساخته میشود (سرایند، بعد داده) و CS باید دور «کل قاب» باشد —
     * این کار را cs_select/cs_release در spif_txn انجام میدهند.
     * CS داخل transfer یعنی هر دستور دو فریم نصفه = فلش از دستور
     * دوم به بعد 0x00 میدهد. */

    /* مسیر DMA — همان شرایط و همان کد برای هر سه حالت جهت.
     * پروتکل کش: TX و RX هر دو قبل از کیک Clean می‌شوند (RX یعنی
     * فلش‌کردن خطوط پوشش‌دهندهٔ بافر؛ Invalidateِ بعدش فقط می‌ریزد). */
    if (ex33_dma_usable(h, len))
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

        if (st == HAL_OK && ex33_dma_wait(h) == FM_OK)
        {
            if (rx != NULL) FM_Cache_InvalidateAfterDma(rx, len);
            return FM_OK;
        }
        ex33_dma_fallback("transfer");
        /* بدون برگشت خطا: ادامه با همان داده از مسیر blocking */
    }

    /* مسیر blocking (و فالبک یک‌بارهٔ DMA) */
    if (tx != NULL && rx != NULL)
    {
        st = HAL_SPI_TransmitReceive(h, (uint8_t *)(uintptr_t)tx, rx,
                                     (uint16_t)len, EX33_SPI_TIMEOUT_MS);
    }
    else if (tx != NULL)
    {
        st = HAL_SPI_Transmit(h, (uint8_t *)(uintptr_t)tx,
                              (uint16_t)len, EX33_SPI_TIMEOUT_MS);
    }
    else
    {
        st = HAL_SPI_Receive(h, rx, (uint16_t)len, EX33_SPI_TIMEOUT_MS);
    }
    return (st == HAL_OK) ? FM_OK : FM_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */

/* پیشرفت پلهٔ ۲۵٪ — مثل exT: بدون اسپم */
static FM_Bool q33_progress(uint64_t done, uint64_t total, void *user)
{
    (void)user;
    if (total != 0u && ((done * 4u) / total) != (((done * 4u) - 1u) / total))
    {
        printf("    %lu%%\r",
               (unsigned long)((done * 100u) / total));
    }
    return FM_TRUE;
}

static FM_QspiDevice   *g_ex33_dev;
static FM_SpiFlashPort *g_ex33_portp;   /* برای عیب‌یابی باس */
static FM_Storage      g_ex33_st;      /* ثبت فلش به‌عنوان حافظهٔ کتابخانه */
static FM_FS          g_ex33_fs;      /* فایل‌سیستم FMFS روی /flash       */
static FM_Bool        g_ex33_mounted;

#ifndef EX33_MOUNT_PATH
#define EX33_MOUNT_PATH "/flash"
#endif
#ifndef EX33_STORAGE_NAME
#define EX33_STORAGE_NAME "flash"
#endif

/*
 * فلش نو/خالی؟ یک‌بار: Ex33_SpiFlash_Format() — بعد از آن FS_Mount
 * درست کار می‌کند. مثل QSPI، فرمت خودکار نیست که داده از بین نرود.
 */
FM_Result Ex33_SpiFlash_Format(void)
{
    FM_Result r;

    if (g_ex33_dev == NULL) return FM_ERR_NOT_READY;
    if (g_ex33_mounted)
    {
        r = FS_Format(EX33_MOUNT_PATH);   /* فرمت از طریق مونت فعلی */
    }
    else
    {
        r = FMFS_Init(&g_ex33_fs);
        if (r == FM_OK) r = g_ex33_fs.ops.format(&g_ex33_fs);
        if (r == FM_OK) r = FS_Mount(EX33_MOUNT_PATH, &g_ex33_fs);
    }
    if (r == FM_OK) g_ex33_mounted = FM_TRUE;
    printf("ex33: format %s\n", (r == FM_OK) ? "OK" : "FAILED");
    return r;
}

/* انتقال مالکیت به PC (USB MSC مثال ۳۴): مونت بسته شود. */
FM_Result Ex33_SpiFlash_ReleaseForUsb(void)
{
    FM_Result r = FM_OK;

    if (g_ex33_mounted)
    {
        r = FS_Unmount(EX33_MOUNT_PATH);
        if (r == FM_OK) g_ex33_mounted = FM_FALSE;
    }
    if (r == FM_OK) r = Storage_Sync(EX33_STORAGE_NAME);
    return r;
}

FM_Result Ex33_SpiFlash_ReturnFromUsb(void)
{
    if (g_ex33_mounted) return FM_OK;
    {
        FM_Result r = FMFS_Init(&g_ex33_fs);

        if (r != FM_OK) return r;
        r = FS_Mount(EX33_MOUNT_PATH, &g_ex33_fs);
        if (r == FM_OK) g_ex33_mounted = FM_TRUE;
        return r;
    }
}

/* برای USB MSC (مثال ۳۴): فلش ثبت‌شده را بده — اگر راه نیفتاده NULL. */
FM_Storage *Ex33_SpiFlash_GetStorage(void)
{
    return (g_ex33_dev != NULL && g_ex33_mounted) ? &g_ex33_st : NULL;
}

void Ex33_SpiFlash_End(void)
{
    if (g_ex33_mounted)
    {
        (void)FS_Unmount(EX33_MOUNT_PATH);
        g_ex33_mounted = FM_FALSE;
    }
    (void)Storage_Unregister(EX33_STORAGE_NAME);
    g_ex33_dev = NULL;
}

/* ------------------------------------------------------------------ */
/* پاک‌سازی کامل قطعه (chip erase) - با پیشرفت زنده                     */
/* ------------------------------------------------------------------ */
/* W25Q64: معمولاً ~20-25s، تا 100s طبق دیتاشیت. این تابع بهترین
 * عیب‌یاب گیرِ erase هم هست: اگر تا آخر پیشرفت کرد و تمام شد یعنی
 * قطعه/باس سالم‌اند و مشکل در مسیر erase-سکتوری بود؛ اگر وسطش WIP
 * هولد شد یعنی مشکل سخت‌افزاری است (کلاک/سیم‌بندی). */
FM_Result Ex33_SpiFlash_EraseAll(void)
{
    FM_QspiDevice *dev = g_ex33_dev;
    FM_Result r;
    uint32_t t0;
    uint32_t last_pct = 0xFFu;

    if (dev == NULL)
    {
        FM_Log("%s\n", ("ex33: aval Ex33_SpiFlash_Begin()"));
        return FM_ERR_NOT_READY;
    }
    FM_Log("%s\n", ("ex33: chip erase (ta 100s) ..."));
    r = FM_Qspi_EraseChipStart(dev);
    if (r != FM_OK)
    {
        printf("ex33: erase-chip start failed (%d)\n", (int)r);
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
            printf("ex33: status read failed (%d)\n", (int)rs);
            return rs;
        }
        if ((status & 0x01u) == 0u)     /* WIP pak shod */
        {
            printf("ex33: chip erase TAMAM dar %lu ms\n",
                   (unsigned long)elapsed);
            return FM_OK;
        }
        pct = (elapsed * 100u) / 25000u;
        if (pct > 100u) pct = 100u;
        if (pct != last_pct && (pct % 5u) == 0u)
        {
            printf("ex33: erase %lu%% (%lu ms)\n",
                   (unsigned long)pct, (unsigned long)elapsed);
            last_pct = pct;
        }
        if (elapsed > 120000u)
        {
            FM_Log("%s\n", ("ex33: !! chip erase ham gir kard - moshkel "
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
void Ex33_SpiFlash_BusDiag(void)
{
    uint8_t hdr = 0x9F;
    uint8_t tx[3] = { 0xFFu, 0xFFu, 0xFFu };
    uint8_t rx[3];
    SPI_HandleTypeDef *h;

    if (g_ex33_portp == NULL)
    {
        FM_Log("%s\n", "ex33 diag: aval Ex33_SpiFlash_Begin()");
        return;
    }
    h = (SPI_HandleTypeDef *)g_ex33_portp->ctx;
    memset(rx, 0xAA, sizeof(rx));
    ex33_cs_low(NULL);
    (void)ex33_transfer(h, &hdr, NULL, 1u);
    (void)ex33_transfer(h, tx, rx, 3u);
    ex33_cs_high(NULL);
    printf("ex33 diag: JEDEC = %02X %02X %02X (salem: EF 40 17)\n",
           rx[0], rx[1], rx[2]);
    if (rx[0] == 0xAAu && rx[1] == 0xAAu && rx[2] == 0xAAu)
    {
        FM_Log("%s\n", "ex33 diag: 0xAA -> SPI clock nakard - SCK/MOSI ya handle");
    }
    else if (rx[0] == 0x00u && rx[1] == 0x00u && rx[2] == 0x00u)
    {
        FM_Log("%s\n", "ex33 diag: 0x00 -> MISO javab nemidad - sim MISO, ya "
             "WP(pin3)/HOLD(pin7) bayad High bashand");
    }
}

static FM_Result Ex33_SpiFlash_Begin(FM_QspiDevice *dev,
                                     FM_SpiFlashPort *port,
                                     SPI_HandleTypeDef *hspi,
                                     FM_QspiChip chip,
                                     uint8_t *sector_buf,
                                     uint32_t sector_buf_size)
{
    FM_Result r;
    uint32_t id = 0;

    memset(port, 0, sizeof(*port));
    port->transfer = ex33_transfer;
    port->cs_select = ex33_cs_low;    /* دور کل قاب هر op - در spif_txn */
    port->cs_release = ex33_cs_high;
    port->delay_ms = NULL;
    port->ctx = hspi;

    r = FM_SpiFlash_Init(dev, port, chip, sector_buf, sector_buf_size);
    if (r != FM_OK)
    {
        printf("ex33: spi flash init failed (%d)\n", (int)r);
        return r;
    }
    g_ex33_dev = dev;
    g_ex33_portp = port;

    r = FM_Qspi_ReadId(dev, &id);
    if (r != FM_OK)
    {
        printf("ex33: flash does not answer (JEDEC read failed)\n");
        return r;
    }
    {
        uint32_t cs_pos = 0;

        while (cs_pos < 16u && ((ex33_cs_pin >> cs_pos) & 1u) == 0u)
        {
            cs_pos++;
        }
        if ((ex33_cs_port->MODER & (3uL << (cs_pos * 2u))) == 0u)
        {
            FM_Log("%s\n", "ex33: !! CS pin dar halate INPUT ast! dar CubeMX: "
                 "GPIO_Output + Push-Pull + Level:High (bedun-e in CS "
                 "sado nemishavad va flash javab nemidad)");
        }
        else
        {
            printf("ex33: CS port %p pin %u - OUTPUT OK\n",
                   (void *)ex33_cs_port, (unsigned)cs_pos);
        }
    }
    printf("ex33: %s ready, JEDEC 0x%06lX, %lu KB - %s\n",
           dev->info.name, (unsigned long)id,
           (unsigned long)(dev->info.size_bytes / 1024u),
           ex33_dma_ok ? "DMA-capable port" : "blocking port");
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
                printf("ex33: !! SPI prescaler /%lu (=sari'e ziadesh baraye "
                       "sim-basi) - dar CubeMX: SPI4 Prescaler = 64 bezan, "
                       "bad az erase haye ~45ms mibini\n",
                       (unsigned long)div);
            }
            else
            {
                printf("ex33: SPI prescaler /%lu - OK baraye sim-basi\n",
                       (unsigned long)div);
            }
        }
    }

#ifndef EX33_NO_SCRATCH_ERASE
    /* عیب‌یابی یک‌خطی پاک‌سازی: آخرین سکتور (به‌ندرت استفاده‌شده) را
     * پاک کن و زمان واقعی را نشان بده. سالم: زیر ~400ms. اگر تا 4
     * ثانیه هم تمام نشد، قطعه یا کند واقعی است یا در حالت گیر.
     * غیرفعال‌سازی: EX33_NO_SCRATCH_ERASE قبل از include تعریف کن. */
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
            printf("ex33: scratch erase OK dar %lu ms (salem: <400)\n",
                   (unsigned long)(HAL_GetTick() - t0));
        }
        else
        {
            printf("ex33: !! scratch erase NASHOD (%d) - taghvie ya "
                   "kiep gire\n", (int)re);
        }
    }
#endif

    /* ثبت به‌عنوان حافظهٔ کتابخانه + مونت /flash (FMFS).
     * FMFS پیش‌فرض است تا با SD روی درایو 0 فت‌فس تداخل نکند؛
     * برای USB MSC، PC خودش FAT می‌گذارد یا EX33_USE_FATFS بسازید. */
    {
        FM_Result rs = FM_Qspi_RegisterStorage(dev, &g_ex33_st,
                                               EX33_STORAGE_NAME);

        if (rs != FM_OK && rs != FM_ERR_EXISTS)
        {
            printf("ex33: register storage failed (%d)\n", (int)rs);
            return rs;
        }
        rs = FMFS_Init(&g_ex33_fs);
        if (rs != FM_OK)
        {
            printf("ex33: FMFS_Init failed (%d)\n", (int)rs);
            return rs;
        }
        rs = FS_Mount(EX33_MOUNT_PATH, &g_ex33_fs);
        if (rs == FM_OK)
        {
            g_ex33_mounted = FM_TRUE;
            FM_Log("%s\n", "ex33: /flash mounted (FMFS)");
        }
        else
        {
            printf("ex33: /flash khali/new (mount=%d) - yek bar: "
                   "Ex33_SpiFlash_Format();\n", (int)rs);
        }
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* exF_1 — تست دود: بنویس، بخوان، تطبیق بده                            */
/* ------------------------------------------------------------------ */

void exF_1_spi_flash_smoke(void)
{
    static FM_CACHE_ALIGN uint8_t out[256];   /* تراز خط کش */
    static FM_CACHE_ALIGN uint8_t back[256];
    size_t i;
    FM_Bool same = FM_TRUE;

    FM_Log("%s\n", "\n============== exF_1_spi_flash_smoke ==============");
    if (g_ex33_dev == NULL)
    {
        FM_Log("%s\n", "  اول Ex33_SpiFlash_Begin() را صدا بزنید.");
        return;
    }

    for (i = 0; i < sizeof(out); i++)
    {
        out[i] = (uint8_t)(i * 7u + 3u);
    }

    if (FM_Qspi_Write(g_ex33_dev, 0, out, sizeof(out)) != FM_OK)
    {
        FM_Log("%s\n", "  write failed");
        return;
    }
    if (FM_Qspi_Read(g_ex33_dev, 0, back, sizeof(back)) != FM_OK)
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
           ex33_dma_ok ? "DMA" : "blocking",
           (unsigned long)g_ex33_dev->stat_erases,
           (unsigned long)g_ex33_dev->stat_erases_skipped);
}

/* ------------------------------------------------------------------ */
/* exF_2 — عملیات فایل روی /flash: متن، باینری+CRC، پوشه               */
/* ------------------------------------------------------------------ */

void exF_2_file_ops(void)
{
    static uint8_t blob[1024];
    static uint8_t back[1024];
    char text[64];
    FM_Dir *d;
    FM_DirEntry e;
    size_t i;
    uint32_t crc_w;
    uint32_t crc_r;

    FM_Log("%s\n", "\n=============== exF_2_file_ops ====================");
    if (g_ex33_dev == NULL || !g_ex33_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }

    /* ۱) متن: بنویس/بخوان/بازنویسی */
    ex33_step("write text",
           File_WriteText(EX33_MOUNT_PATH "/hello.txt",
                          "Hello from plain-SPI flash!"));
    File_ReadText(EX33_MOUNT_PATH "/hello.txt", text, sizeof(text), NULL);
    printf("  read back: %s\n", text);

    /* ۲) باینری ۱KB + CRC رفت‌وبرگشت (از مسیر DMA اگر فعال باشد) */
    for (i = 0; i < sizeof(blob); i++)
    {
        blob[i] = (uint8_t)(i * 13u + 5u);
    }
    crc_w = CRC_Compute32(blob, sizeof(blob), 0u);
    ex33_step("write 1K",
           File_WriteAll(EX33_MOUNT_PATH "/blob.bin",
                         blob, (uint32_t)sizeof(blob)));
    ex33_step("read 1K",
           File_ReadAll(EX33_MOUNT_PATH "/blob.bin",
                        back, sizeof(back), NULL));
    crc_r = CRC_Compute32(back, sizeof(back), 0u);
    printf("  CRC 1K: %08lX -> %08lX : %s\n",
           (unsigned long)crc_w, (unsigned long)crc_r,
           (crc_w == crc_r) ? "IDENTICAL" : "CORRUPTED");

    /* ۳) پوشه + فهرست */
    (void)Directory_Create(EX33_MOUNT_PATH "/cfg");
    ex33_step("write cfg",
           File_WriteText(EX33_MOUNT_PATH "/cfg/net.txt", "ip=10.0.0.5"));
    FM_Log("%s\n", "  /flash:");
    d = Directory_Open(EX33_MOUNT_PATH);
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
    (void)File_Delete(EX33_MOUNT_PATH "/hello.txt");
    (void)File_Delete(EX33_MOUNT_PATH "/blob.bin");
    (void)File_Delete(EX33_MOUNT_PATH "/cfg/net.txt");
    (void)Directory_Delete(EX33_MOUNT_PATH "/cfg");
}

/* ------------------------------------------------------------------ */
/* exF_3 — SD -> فلش با وریفای CRC (کاربرد: فونت/لوگو بدون کارت)       */
/* ------------------------------------------------------------------ */

void exF_3_sd_to_flash(void)
{
    FM_TransferOpts o;
    FM_TransferInfo info;
    FM_Result r;
    const char *src = "/config.json";
    FM_Bool exists = FM_FALSE;

    FM_Log("%s\n", "\n=============== exF_3_sd_to_flash ================");
    if (g_ex33_dev == NULL || !g_ex33_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }
    if (File_Exists(src, &exists) != FM_OK || !exists)
    {
        src = "/ex33_src.txt";
        ex33_step("make demo source",
               File_WriteText(src, "ex33 transfer demo payload"));
    }

    memset(&o, 0, sizeof(o));
    o.verify = FM_TRUE;
    o.progress = q33_progress;
    r = FM_Transfer_CopyEx(src, EX33_MOUNT_PATH "/from_sd.bin",
                           &o, &info);
    if (r == FM_OK)
    {
        printf("  copied %lu bytes from %s, CRC 0x%08lX (verified)\n",
               (unsigned long)info.bytes, src,
               (unsigned long)info.crc_dest);
    }
}

/* ------------------------------------------------------------------ */
/* exF_4 — RAM -> فلش -> RAM با CRC (رفت‌وبرگشت کامل مسیر DMA)         */
/* ------------------------------------------------------------------ */

void exF_4_ram_roundtrip(void)
{
    static FM_CACHE_ALIGN uint8_t src[2048];
    static FM_CACHE_ALIGN uint8_t dst[2048];
    size_t i;
    size_t got = 0;
    uint32_t c1;
    uint32_t c2;

    FM_Log("%s\n", "\n============== exF_4_ram_roundtrip ===============");
    if (g_ex33_dev == NULL || !g_ex33_mounted)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }

    for (i = 0; i < sizeof(src); i++)
    {
        src[i] = (uint8_t)((i * 31u) ^ (i >> 3));
    }
    c1 = CRC_Compute32(src, sizeof(src), 0u);

    ex33_step("RAM -> flash",
           FM_Transfer_SaveFile(EX33_MOUNT_PATH "/rt.bin",
                                src, sizeof(src)));
    ex33_step("flash -> RAM",
           FM_Transfer_LoadFile(EX33_MOUNT_PATH "/rt.bin",
                                dst, sizeof(dst), &got));
    c2 = CRC_Compute32(dst, got, 0u);
    printf("  %lu bytes, CRC %08lX -> %08lX : %s\n",
           (unsigned long)got, (unsigned long)c1, (unsigned long)c2,
           (c1 == c2 && got == sizeof(src)) ? "IDENTICAL" : "CORRUPTED");
    (void)File_Delete(EX33_MOUNT_PATH "/rt.bin");
}

/* ------------------------------------------------------------------ */
/* exF_5 — آمار: چقدر نوشتیم/پاک کردیم/چند سکتور خالی رد شد            */
/* ------------------------------------------------------------------ */

void exF_5_stats(void)
{
    FM_Log("%s\n", "\n================= exF_5_stats ====================");
    if (g_ex33_dev == NULL)
    {
        FM_Log("%s\n", "  (فلش آماده نیست — رد شد)");
        return;
    }
    printf("  reads=%lu writes=%lu erases=%lu blank-skipped=%lu "
           "mmap? (SPI: no)\n",
           (unsigned long)g_ex33_dev->stat_reads,
           (unsigned long)g_ex33_dev->stat_writes,
           (unsigned long)g_ex33_dev->stat_erases,
           (unsigned long)g_ex33_dev->stat_erases_skipped);
    printf("  data phase: %s\n",
           ex33_dma_ok ? "DMA" : "blocking");
}

#endif /* !FM_HOST_BUILD */
#endif /* FM_EXAMPLE_33_STM32_SPI_FLASH_DMA_PORT_H */
