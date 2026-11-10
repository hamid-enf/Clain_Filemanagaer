/*
 * HAL_SD → FM_Storage.  MCU-agnostic: only HAL_SD_* symbols.
 * Cache bounce lives in backends/fm_fatfs.c (disk_read/write).
 */
#define FM_DEBUG_TAG "sdhal"
#include "fm_debug.h"
#include "fm_log.h"
#include "file_manager.h"
#include "fm_sd_hal.h"
#include "fm_device.h"
#include "fm_fatfs.h"
#include "fm_cache.h"

#if FM_USE_FATFS && FM_USE_DEVICE_ADAPTERS && \
    !(defined(FM_HOST_BUILD) && FM_HOST_BUILD) && \
    defined(HAL_SD_MODULE_ENABLED)

#include <string.h>

static SD_HandleTypeDef *g_hsd;
static FM_BlockDevice    g_dev;
static FM_Storage        g_st;

/* ------------------------------------------------------------------ */
/* بافر واسط استاتیک - مسیر خواندن/نوشتن دقیقاً مثل diskio کیوب        */
/* ------------------------------------------------------------------ */
/* چرا؟ دیسک‌آی‌او کیوب (مسیر همهٔ تست‌های سبز) با بافر استاتیک تراز
 * کار می‌کند؛ مسیر مستقیمِ HAL_SD_ReadBlocks با بافر caller در
 * وضعیت USB-فعال روی برد بی‌صدا داده نمی‌نوشت (لاگ r9: بافر تازه =
 * محتوای کهنهٔ استک). الان هر انتقال از این بافر عبور می‌کند و
 * وابسته به تراز/محل/وضعیت بافر caller نیست. */
#ifndef FM_SD_STAGE_SECTORS
#define FM_SD_STAGE_SECTORS 8
#endif
static uint8_t g_sd_stage[FM_SD_STAGE_SECTORS * 512u]
    __attribute__((aligned(32)));

/* Report a failing HAL_SD transfer.  Runs only on an error path, so the
 * extra FM_Log cost is irrelevant.  ErrorCode is a bitmask of
 * HAL_SD_ErrorTypeDef; State is HAL_SD_StateTypeDef. */
static void sd_diag(const char *op, uint64_t lba, uint32_t count,
                    uint32_t bytes)
{
    /* lba is a 32-bit LBA here (FatFS DWORD / this HAL casts to uint32_t);
     * print with %lu to avoid 64-bit format support in the UART log. */
    FM_Log("sdhal: %s FAIL lba=%lu count=%lu bytes=%lu state=%d "
           "err=0x%08lX\n",
           op, (unsigned long)lba, (unsigned long)count,
           (unsigned long)bytes,
           (int)(g_hsd != NULL ? (int)g_hsd->State : -1),
           (unsigned long)(g_hsd != NULL ? g_hsd->ErrorCode : 0u));
}

static FM_Result sd_wait(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();

    while (HAL_SD_GetCardState(g_hsd) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            return FM_ERR_TIMEOUT;
        }
    }
    return FM_OK;
}

static FM_Result sd_read_blocks(void *ctx, uint64_t lba, uint32_t count,
                                void *data)
{
    uint32_t bytes;

    (void)ctx;
    if (g_hsd == NULL || data == NULL || count == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bytes = count * 512u;
    if (count <= (uint32_t)FM_SD_STAGE_SECTORS)
    {
        /* تک‌مرحله‌ای: در بافر واسط بخوان، بعد کپی کن.
         *
         * HAL_SD_ReadBlocks() (با timeout، بدون پسوند _DMA/_IT) انتقال
         * POLLING است: خود CPU سکتور را از FIFO می‌خواند و در g_sd_stage
         * می‌نویسد. پس اینجا «CPU می‌نویسد، CPU می‌خواند»، نه DMA.
         *
         * ترتیب cache پس از این transfer polling باید این باشد:
         *   Clean بعد از خواندن (فلاش داده‌ی CPUنوشته به DRAM)،
         *   سپس Invalidate (ریختن خطوط کشِ الآن پاک، تا CPU از RAMِ واقعی
         *   بخواند). اگر فقط Invalidate بزنیم، خطوط dirtyِ داده‌ی تازه
         *   بدون Write-Back دور ریخته می‌شوند و memcpy صفر/کهنه می‌خواند —
         *   همان باگ «LBA0 همه صفر» روی برد.
         *
         * (Cleanِ قبل از kick برای حالت DMA-فعال است؛ اینجا بی‌ضرر است و
         * برایِ پوششِ HALهای DMA محور نگهش می‌داریم.) */
        FM_Cache_CleanForDma(g_sd_stage, bytes);
        if (HAL_SD_ReadBlocks(g_hsd, g_sd_stage, (uint32_t)lba, count,
                              2000u) != HAL_OK)
        {
            sd_diag("READ", lba, count, bytes);
            return FM_ERR_IO;
        }
        if (sd_wait(2000u) != FM_OK)
        {
            sd_diag("READwait", lba, count, bytes);
            return FM_ERR_TIMEOUT;
        }
        /*
         * HAL_SD_ReadBlocks() is the POLLING API: the CPU reads the SDMMC
         * FIFO and stores the sector into g_sd_stage.  Those stores land in
         * the D-Cache (write-back).  A bare InvalidateAfterDma() here would
         * DISCARD the freshly-written lines before they reach DRAM, so the
         * memcpy below reads stale/zero memory.  Clean AFTER the transfer
         * flushes the CPU-written data to DRAM first; we may then invalidate
         * so the CPU re-reads the real RAM.  (For a DMA-capable HAL the
         * Clean-before-kick above already covers it and this Clean is a
         * harmless no-op on clean lines.) */
        FM_Cache_CleanForDma(g_sd_stage, bytes);
        FM_Cache_InvalidateAfterDma(g_sd_stage, bytes);
        memcpy(data, g_sd_stage, bytes);
        /*
         * The caller buffer `data` was filled by a CPU memcpy (not a DMA
         * write).  Generic callers follow a Storage_ReadBlocks()/read()
         * with FM_Cache_InvalidateAfterDma(), which is correct for a DMA
         * writer but would DISCARD these CPU-written dirty lines before
         * they reach DRAM (FatFS disk_read does exactly that).  Flushing
         * `data` here makes that Invalidate harmless: the clean lines are
         * dropped and re-fetched from real RAM, which now holds the sect.
         */
        FM_Cache_CleanForDma(data, bytes);
        return FM_OK;
    }
    {
        /* چندتکه: پنجرهٔ ۸ سکتوری */
        uint32_t done = 0;

        while (done < count)
        {
            uint32_t n = count - done;

            if (n > (uint32_t)FM_SD_STAGE_SECTORS)
            {
                n = (uint32_t)FM_SD_STAGE_SECTORS;
            }
            /* Clean قبل از kick: برای حالت DMA-فعال خطوط کهنهٔ CPU را
             * بیرون می‌ریزد. اینجا انتقال polling است، پس این Clean پیش‌دست
             * بی‌ضرر است و همان‌طور که در مسیر تک‌سکتوری توضیح داده شد،
             * Cleanِ بعد از read (پایین‌تر) مسئولیت اصلی را دارد. */
            FM_Cache_CleanForDma(g_sd_stage, (size_t)n * 512u);
            if (HAL_SD_ReadBlocks(g_hsd, g_sd_stage,
                                  (uint32_t)(lba + done), n,
                                  2000u) != HAL_OK)
            {
                sd_diag("READchunk", lba + done, n, (uint32_t)n * 512u);
                return FM_ERR_IO;
            }
            if (sd_wait(2000u) != FM_OK)
            {
                sd_diag("READwait", lba + done, n, (uint32_t)n * 512u);
                return FM_ERR_TIMEOUT;
            }
            /* Polling RX: flush CPU-written data to DRAM before invalidating
             * (see the comment above the single-sector path). */
            FM_Cache_CleanForDma(g_sd_stage, (size_t)n * 512u);
            FM_Cache_InvalidateAfterDma(g_sd_stage, (size_t)n * 512u);
            memcpy((uint8_t *)data + (size_t)done * 512u, g_sd_stage,
                   (size_t)n * 512u);
            FM_Cache_CleanForDma((uint8_t *)data + (size_t)done * 512u,
                                 (size_t)n * 512u);
            done += n;
        }
        return FM_OK;
    }
}

static FM_Result sd_write_blocks(void *ctx, uint64_t lba, uint32_t count,
                                 const void *data)
{
    uint32_t bytes;

    (void)ctx;
    if (g_hsd == NULL || data == NULL || count == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bytes = count * 512u;
    if (count <= (uint32_t)FM_SD_STAGE_SECTORS)
    {
        memcpy(g_sd_stage, data, bytes);
        FM_Cache_CleanForDma(g_sd_stage, bytes);
        if (HAL_SD_WriteBlocks(g_hsd, g_sd_stage, (uint32_t)lba, count,
                               2000u) != HAL_OK)
        {
            sd_diag("WRITE", lba, count, bytes);
            return FM_ERR_IO;
        }
        if (sd_wait(2000u) != FM_OK)
        {
            sd_diag("WRITEwait", lba, count, bytes);
            return FM_ERR_TIMEOUT;
        }
        return FM_OK;
    }
    {
        uint32_t done = 0;

        while (done < count)
        {
            uint32_t n = count - done;

            if (n > (uint32_t)FM_SD_STAGE_SECTORS)
            {
                n = (uint32_t)FM_SD_STAGE_SECTORS;
            }
            memcpy(g_sd_stage, (const uint8_t *)data + (size_t)done * 512u,
                   (size_t)n * 512u);
            FM_Cache_CleanForDma(g_sd_stage, (size_t)n * 512u);
            if (HAL_SD_WriteBlocks(g_hsd, g_sd_stage,
                                   (uint32_t)(lba + done), n,
                                   2000u) != HAL_OK)
            {
                sd_diag("WRITEchunk", lba + done, n, (uint32_t)n * 512u);
                return FM_ERR_IO;
            }
            if (sd_wait(2000u) != FM_OK)
            {
                sd_diag("WRITEwait", lba + done, n, (uint32_t)n * 512u);
                return FM_ERR_TIMEOUT;
            }
            done += n;
        }
        return FM_OK;
    }
}

FM_Result FM_SD_Hal_Attach(void *sd_handle)
{
    FM_BlockDeviceConfig cfg;
    HAL_SD_CardInfoTypeDef ci;
    FM_Result r;

    (void)FileManager_Init();

    if (sd_handle == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    g_hsd = (SD_HandleTypeDef *)sd_handle;
    if (HAL_SD_GetCardInfo(g_hsd, &ci) != HAL_OK)
    {
        return FM_ERR_NOT_READY;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "SD";
    cfg.type = FM_STORAGE_SD;
    cfg.capacity = (uint64_t)ci.LogBlockNbr * ci.LogBlockSize;
    cfg.block_size = ci.LogBlockSize ? ci.LogBlockSize : 512u;
    cfg.present = FM_TRUE;
    cfg.read_blocks = sd_read_blocks;
    cfg.write_blocks = sd_write_blocks;

    r = BlockDevice_Init(&g_dev, &cfg);
    if (r != FM_OK)
    {
        return r;
    }
    r = Storage_InitBlockDevice(&g_st, &g_dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = Storage_Register(&g_st);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        return r;
    }
    r = Storage_Mount("SD");
    if (r != FM_OK)
    {
        return r;
    }
    return FatFS_BindStorage(0, &g_st);
}

FM_Storage *FM_SD_Hal_Storage(void)
{
    return &g_st;
}

#else /* not FILEMANAGER HAL path */

FM_Result FM_SD_Hal_Attach(void *sd_handle)
{
    (void)sd_handle;
    return FM_ERR_UNSUPPORTED;
}

FM_Storage *FM_SD_Hal_Storage(void)
{
    return NULL;
}

#endif
