/*
 * qspi_driver.h — وصل‌کردن fm_qspi به QUADSPI کیوب
 * ==========================================================
 *
 * این فایل پل بین لایهٔ مستقل fm_qspi.c و HAL است. پنج تابع کوچک
 * می‌نویسد و تمام.
 *
 * استفاده:
 *
 *     #include "qspi_driver.h"
 *
 *     static FM_QspiDevice   qspi_dev;
 *     static FM_Storage      qspi_storage;
 *
 *     // در main، بعد از MX_QUADSPI_Init():
 *     QspiDriver_Begin(&qspi_dev, &qspi_storage, &hqspi,
 *                    FM_QSPI_CHIP_AUTO);
 *
 * بعد از این، فلش یک حافظهٔ عادی است: mount می‌شود، فایل‌سیستم می‌گیرد،
 * و FM_Transfer_Copy با آن کار می‌کند.
 *
 * ---------------------------------------------------------------------
 * حالت DMA محور (MDMA)
 * ---------------------------------------------------------------------
 * فاز دادهٔ خواندن/نوشتن با MDMA انجام می‌شود:
 *
 *   CubeMX:
 *     - در «DMA Settings» کیوب: کانال MDMA با ریکوئست «QUADSPI_FIFO_TH»
 *       به QUADSPI اضافه کنید (روی H7 فقط MDMA به QUADSPI می‌رسد).
 *     - در NVIC: وقفهٔ QUADSPI و وقفهٔ همان کانال MDMA روشن باشد.
 *     - MX_MDMA_Init() قبل از QspiDriver_Begin() صدا زده شود.
 *
 *   اگر MDMA اضافه نشده باشد همه‌چیز به‌صورت polling کار می‌کند؛ و اگر
 *   اولین انتقال DMA شکست بخورد (مثلاً بافر در DTCM است که MDMA به آن
 *   نمی‌رسد)، درایور یک‌بار برای همیشه به polling برمی‌گردد.
 *
 *   نگهداری کش طبق پروتکل کتابخانه: FM_Cache_CleanForDma قبل از TX و
 *   FM_Cache_InvalidateAfterDma بعد از RX.
 *
 *   نکتهٔ صادقانه: با روشن‌بودن memory-mapped reads (پیش‌فرض)، خواندن‌ها
 *   اصلاً از مسیر DMA رد نمی‌شوند — نگاشت سریع‌تر است. DMA به نوشتن‌ها و
 *   به حالت command-read (وقتی FM_Qspi_SetMappedReads(dev, FM_FALSE))
 *   و به درایور qspi_w25q.h که نگاشت ندارد سود می‌دهد.
 *
 * ---------------------------------------------------------------------
 * حالت memory-mapped
 * ---------------------------------------------------------------------
 * این فایل هر دو حالت را وصل می‌کند. لایهٔ fm_qspi خودش تصمیم می‌گیرد:
 * موقع خواندن وارد نگاشت می‌شود (سریع)، و قبل از هر نوشتن/پاک‌کردن
 * خارج می‌شود (لازم). شما کاری نمی‌کنید.
 *
 * اگر برنامه‌تان بیشتر می‌نویسد تا بخواند، رفت‌وبرگشت مکرر سربار دارد؛
 * آن‌وقت با FM_Qspi_SetMappedReads(&dev, FM_FALSE) خاموشش کنید.
 */
#ifndef FM_TOPAPI_QSPI_DRIVER_H
#define FM_TOPAPI_QSPI_DRIVER_H

#include "quadspi.h"
#include "fm_qspi.h"
#include "fm_board_config.h"
#include "fm_log.h"
#include "fm_xdma.h"

#ifndef QSPI_DRV_TIMEOUT_MS
#define QSPI_DRV_TIMEOUT_MS 1000u
#endif

/* بافر سکتور برای نوشتن‌های جزئی. باید حداقل به اندازهٔ سکتور قطعه
 * باشد (۴ کیلوبایت برای تقریباً همهٔ قطعات رایج). */
#ifndef QSPI_DRV_SECTOR_BUF_SIZE
#define QSPI_DRV_SECTOR_BUF_SIZE FM_QSPI_SECTOR_SIZE
#endif

__attribute__((aligned(32)))
static uint8_t g_qspi_drv_sector[QSPI_DRV_SECTOR_BUF_SIZE];

/* ------------------------------------------------------------------ */
/* پنج تابع لایهٔ پایین                                                */
/* ------------------------------------------------------------------ */

static QSPI_CommandTypeDef qspi_drv_base_cmd(uint8_t opcode)
{
    QSPI_CommandTypeDef c;

    memset(&c, 0, sizeof(c));
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c.Instruction       = opcode;
    c.AddressMode       = QSPI_ADDRESS_NONE;
    c.DataMode          = QSPI_DATA_NONE;
    c.DummyCycles       = 0;
    c.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
    c.DdrMode           = QSPI_DDR_MODE_DISABLE;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    return c;
}

static uint32_t qspi_drv_addr_size(uint8_t addr_bytes)
{
    return (addr_bytes == 4u) ? QSPI_ADDRESS_32_BITS : QSPI_ADDRESS_24_BITS;
}

/* ------------------------------------------------------------------ */
/* DMA محور — فاز داده با موتور مشترک fm_xdma.h                        */
/* ------------------------------------------------------------------ */
#ifndef QSPI_DRV_DMA_MIN_LEN
#define QSPI_DRV_DMA_MIN_LEN 64u    /* کوچک‌تر از این: polling ارزان‌تر است */
#endif
#ifndef QSPI_DRV_DMA_MAX_LEN
#define QSPI_DRV_DMA_MAX_LEN 4096u  /* یک سکتور — سقف امن NbData           */
#endif

static void qspi_drv_fill_read_cmd(QSPI_CommandTypeDef *c, uint8_t opcode,
                               uint32_t addr, uint8_t addr_bytes,
                               uint8_t dummy, uint32_t len)
{
    *c = qspi_drv_base_cmd(opcode);
    if (addr_bytes != 0u)
    {
        c->AddressMode = QSPI_ADDRESS_1_LINE;
        c->AddressSize = qspi_drv_addr_size(addr_bytes);
        c->Address     = addr;
    }
    c->DataMode    = QSPI_DATA_1_LINE;
    c->NbData      = len;
    c->DummyCycles = dummy;
}

static void qspi_drv_fill_write_cmd(QSPI_CommandTypeDef *c, uint8_t opcode,
                                uint32_t addr, uint8_t addr_bytes,
                                uint32_t len)
{
    *c = qspi_drv_base_cmd(opcode);
    c->AddressMode = QSPI_ADDRESS_1_LINE;
    c->AddressSize = qspi_drv_addr_size(addr_bytes);
    c->Address     = addr;
    c->DataMode    = QSPI_DATA_1_LINE;
    c->NbData      = len;
}

static FM_Result qspi_drv_cmd(void *ctx, uint8_t opcode)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    QSPI_CommandTypeDef c = qspi_drv_base_cmd(opcode);

    if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result qspi_drv_cmd_addr(void *ctx, uint8_t opcode, uint32_t addr,
                               uint8_t addr_bytes)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    QSPI_CommandTypeDef c = qspi_drv_base_cmd(opcode);

    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.AddressSize = qspi_drv_addr_size(addr_bytes);
    c.Address     = addr;

    if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result qspi_drv_read(void *ctx, uint8_t opcode, uint32_t addr,
                           uint8_t addr_bytes, uint8_t dummy,
                           void *buf, uint32_t len)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    QSPI_CommandTypeDef c;

    /* DMA محور: انتقال بزرگ با موتور fm_xdma؛ نتیجه سه‌حالته است. */
    if (FM_Xdma_RxEx(buf, len, QSPI_DRV_TIMEOUT_MS) == FM_XDMA_FAILED)
    {
        /* کنترلر Abort شده و دستور نیمه‌کاره مانده؛ از نو صادرش کن. */
        qspi_drv_fill_read_cmd(&c, opcode, addr, addr_bytes, dummy, len);
        if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
        {
            return FM_ERR_IO;
        }
    }

    /* مسیر polling (SKIPPED) یا ادامهٔ همان فرمانِ دوباره‌صادرشده */
    qspi_drv_fill_read_cmd(&c, opcode, addr, addr_bytes, dummy, len);

    if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    if (HAL_QSPI_Receive(h, (uint8_t *)buf, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result qspi_drv_write(void *ctx, uint8_t opcode, uint32_t addr,
                            uint8_t addr_bytes, const void *buf,
                            uint32_t len)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    QSPI_CommandTypeDef c;

    /* DMA محور: نوشتن صفحه با موتور fm_xdma. */
    if (FM_Xdma_TxEx(buf, len, QSPI_DRV_TIMEOUT_MS) == FM_XDMA_FAILED)
    {
        /* کنترلر Abort شده؛ فرمان نوشتن را از نو صادر کن. */
        qspi_drv_fill_write_cmd(&c, opcode, addr, addr_bytes, len);
        if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
        {
            return FM_ERR_IO;
        }
    }

    /* مسیر polling (SKIPPED) یا ادامهٔ همان فرمانِ دوباره‌صادرشده */
    qspi_drv_fill_write_cmd(&c, opcode, addr, addr_bytes, len);

    if (HAL_QSPI_Command(h, &c, QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    if (HAL_QSPI_Transmit(h, (uint8_t *)(uintptr_t)buf,
                          QSPI_DRV_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static void qspi_drv_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    HAL_Delay(ms);
}

/* ------------------------------------------------------------------ */
/* حالت memory-mapped                                                  */
/* ------------------------------------------------------------------ */
/*
 * وارد شدن: کنترلر را در حالت نگاشت بگذار. از این لحظه فلش در آدرس
 * 0x90000000 مثل حافظهٔ معمولی خوانده می‌شود و هیچ دستوری قبول نمی‌کند.
 *
 * خارج شدن: HAL_QSPI_Abort کنترلر را به حالت دستوری برمی‌گرداند.
 */

static FM_Result qspi_drv_mmap_enter(void *ctx)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;
    QSPI_CommandTypeDef c;
    QSPI_MemoryMappedTypeDef mm;

    memset(&c, 0, sizeof(c));
    memset(&mm, 0, sizeof(mm));

    /* Fast Read با ۸ سیکل dummy — سازگار با تقریباً همهٔ قطعات.
     * اگر قطعهٔ شما Quad Output را پشتیبانی می‌کند، 0x6B با
     * QSPI_DATA_4_LINES سریع‌تر است. */
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c.Instruction       = 0x0Bu;              /* Fast Read */
    c.AddressMode       = QSPI_ADDRESS_1_LINE;
    c.AddressSize       = QSPI_ADDRESS_24_BITS;
    c.DataMode          = QSPI_DATA_1_LINE;
    c.DummyCycles       = 8u;
    c.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
    c.DdrMode           = QSPI_DDR_MODE_DISABLE;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;

    mm.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
    mm.TimeOutPeriod     = 0;

    if (HAL_QSPI_MemoryMapped(h, &c, &mm) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result qspi_drv_mmap_exit(void *ctx)
{
    QSPI_HandleTypeDef *h = (QSPI_HandleTypeDef *)ctx;

    /* Abort تنها راه خروج از حالت نگاشت است. */
    if (HAL_QSPI_Abort(h) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی یکجا                                                     */
/* ------------------------------------------------------------------ */
/*
 * chip: مدل قطعه، یا FM_QSPI_CHIP_AUTO تا از JEDEC ID شناسایی شود.
 *
 * اگر فلش جواب ندهد، خطا برمی‌گرداند و چیزی ثبت نمی‌شود — برنامه
 * سالم ادامه می‌دهد.
 */
static FM_Result QspiDriver_Begin(FM_QspiDevice *dev, FM_Storage *storage,
                                QSPI_HandleTypeDef *hqspi,
                                FM_QspiChip chip)
{
    FM_QspiIo io;
    FM_Result r;

    /* موتور DMA پیش‌فرض خانواده (H7: MDMA / H5: GPDMA+OCTOSPI).
     * اگر برنامه خودش FM_Xdma_Use زده باشد، دست نمی‌زنیم. */
    FM_Xdma_SetWindow(QSPI_DRV_DMA_MIN_LEN, QSPI_DRV_DMA_MAX_LEN);
    FM_Xdma_AutoInstallQspi(hqspi);

    memset(&io, 0, sizeof(io));
    io.cmd        = qspi_drv_cmd;
    io.cmd_addr   = qspi_drv_cmd_addr;
    io.read       = qspi_drv_read;
    io.write      = qspi_drv_write;
    io.delay_ms   = qspi_drv_delay;
    io.ctx        = hqspi;

#if FM_QSPI_USE_MEMORY_MAPPED
    io.mmap_enter = qspi_drv_mmap_enter;
    io.mmap_exit  = qspi_drv_mmap_exit;
    io.mmap_base  = (const volatile uint8_t *)FM_MEM_QSPI_BASE;
#endif

    r = FM_Qspi_Init(dev, &io, chip, g_qspi_drv_sector,
                     (uint32_t)sizeof(g_qspi_drv_sector));
    if (r != FM_OK)
    {
        FM_Log("QSPI: init failed (%d) — flash not available\n", (int)r);
        return r;
    }

    r = FM_Qspi_RegisterStorage(dev, storage, "qspi");
    if (r != FM_OK)
    {
        FM_Log("QSPI: storage registration failed (%d)\n", (int)r);
        return r;
    }

    FM_Log("QSPI: %s ready, %lu MB%s\n", dev->info.name,
           (unsigned long)(dev->info.size_bytes / (1024u * 1024u)),
#if FM_QSPI_USE_MEMORY_MAPPED
           ", memory-mapped reads on");
#else
           "");
#endif
    return FM_OK;
}

#endif /* FM_TOPAPI_QSPI_DRIVER_H */
