/*
 * Example 15 — فلش سری روی STM32H7 QUADSPI (تشخیص خودکار قطعه)
 *
 * هدف:
 *   فلش NOR سری (W25Q64/W25Q128/W25Q256 و بقیهٔ جدول fm_qspi.c)
 *   -> sector translation -> FMFS (پیش‌فرض) یا FatFS (اختیاری) -> File_* API
 *
 * مدل قطعه از JEDEC ID خودکار شناسایی می‌شود؛ مثلاً W25Q64 (8MB، آدرس
 * ۳-بایتی) و W25Q256 (32MB، آدرس ۴-بایتی) هر دو بدون هیچ تغییری کار
 * می‌کنند — opcode و ظرفیت و اندازهٔ آدرس از جدول می‌آید.
 *
 * چرا translation لازم است؟
 *   فایل‌سیستم سکتورهای 512 بایتی را چندبار بازنویسی می‌کند، ولی NOR
 *   فقط 1->0 را program می‌کند و برای 0->1 باید sector چهار کیلوبایتی erase
 *   شود. FM_NorBlockDevice این read-modify-erase-program را با یک scratch
 *   چهار کیلوبایتی متعلق به برنامه انجام می‌دهد.
 *
 * مراحل CubeMX:
 *   1) QUADSPI را فعال کنید و MX_QUADSPI_Init() را قبل از
 *      Ex15_QSPI_Begin() صدا بزنید.
 *   2) فایل‌سیستم پیش‌فرض FMFS است: نه diskio می‌خواهد نه VOLUMES، و با
 *      درایو 0 (SD کیوب) هیچ تداخلی ندارد. فقط اگر صریحاً FatFS می‌خواهید
 *      (مثلاً برای USB MSC مثال ۱۶)، قبل از include این را بدهید:
 *        #define EX15_USE_FATFS 1
 *      و در فت‌فس: FF_VOLUMES >= 2 و (مهم!) EX15_QSPI_FATFS_DRIVE=1 —
 *      درایو 0 جای SD است؛ بستن QSPI روی آن یعنی ربودن SD.
 *   3) data تک‌خطی و آدرس ۳/۴ بایتی (بسته به قطعه). به QE bit وابسته
 *      نیستید. بعداً می‌توانید callbackهای پایین را با 1-1-4/1-4-4
 *      سریع‌تر جایگزین کنید، بدون تغییر FileManager.
 *
 *   5) فاز داده به‌صورت DMA محور (MDMA) است:
 *
 *    CubeMX:
 *      - QUADSPI را فعال کنید و در «DMA Settings» یک کانال MDMA با
 *        ریکوئست «QUADSPI_FIFO_TH» اضافه کنید. (روی H7 فقط MDMA به
 *        QUADSPI که در D1 است دسترسی دارد؛ DMA1/DMA2 جایز نیست.)
 *      - در NVIC: وقفهٔ QUADSPI و وقفهٔ همان کانال MDMA را روشن کنید.
 *      - MX_MDMA_Init() باید قبل از Ex15_QSPI_Begin() صدا زده شود
 *        (در board/main.c از قبل هست).
 *
 *    اگر MDMA اضافه نشده باشد، همین کد به‌صورت polling اجرا می‌شود —
 *    و اگر اولین انتقال DMA به هر دلیلی شکست بخورد (مثلاً بافر در
 *    DTCM است که MDMA به آن نمی‌رسد)، یک‌بار برای همیشه به polling
 *    برمی‌گردد؛ هیچ crash ای در کار نیست.
 *
 *    نگهداری کش طبق پروتکل کتابخانه انجام می‌شود:
 *      FM_Cache_CleanForDma قبل از TX و FM_Cache_InvalidateAfterDma
 *      بعد از RX — بافر خارج از DTCM باشد بهترین نتیجه را می‌دهد.
 *
 *    انتقال‌های کوچک‌تر از EX15_DMA_MIN_LEN بایت (وضعیت، JEDEC و ...)
 *    مستقیم polling می‌روند؛ راه‌اندازی DMA برای چند بایت صرف نمی‌کند.
 *
 * استفاده:
 *   if (Ex15_QSPI_Begin() == FM_OK) {
 *       File_WriteText("/qspi/hello.txt", "Hello QSPI");
 *       ... Media examples روی مسیر /qspi/... نیز همان API را دارند ...
 *       Ex15_QSPI_End();
 *   }
 *
 * برای ارائه به PC با USB MSC، g_ex15_qspi_storage را به Example 16 بدهید.
 * قبل از USB حتماً Ex15_QSPI_UnmountFSForUSB() و بعد از قطع USB
 * Ex15_QSPI_RemountFSAfterUSB() را اجرا کنید؛ MCU و PC هم‌زمان نباید FAT را
 * writable mount کنند.
 */

#ifndef QSPI_W25Q_H_
#define QSPI_W25Q_H_

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include <string.h>
#include "stm32h7xx_hal.h"
#include "quadspi.h"
#include "file_manager.h"
#include "fm_device.h"
#include "fm_fatfs.h"
#include "fm_qspi.h"
#include "fm_log.h"
#include "../top_api/fm_xdma.h"

#ifndef EX15_QSPI_FATFS_DRIVE
#define EX15_QSPI_FATFS_DRIVE 0u
#endif

#ifndef EX15_QSPI_MOUNT_PATH
#define EX15_QSPI_MOUNT_PATH "/qspi"
#endif

#ifndef EX15_QSPI_TIMEOUT_MS
#define EX15_QSPI_TIMEOUT_MS 5000u
#endif

#define EX15_W25Q256_CAPACITY    (32u * 1024u * 1024u)
#define EX15_W25Q256_PAGE_SIZE   256u
#define EX15_W25Q256_SECTOR_SIZE 4096u
#define EX15_W25Q256_JEDEC_ID    0xEF4019u

#define EX15_CMD_READ_JEDEC_ID   0x9Fu
#define EX15_CMD_READ_STATUS_1   0x05u
#define EX15_CMD_WRITE_ENABLE    0x06u
/* قطعات ≤128Mbit (مثل W25Q64) آدرس ۳-بایتی می‌خواهند؛ ≥256Mbit چهارتایی. */
#define EX15_CMD_READ_3BYTE      0x03u
#define EX15_CMD_PROGRAM_3BYTE   0x02u
#define EX15_CMD_ERASE_4K_3BYTE  0x20u
#define EX15_CMD_READ_4BYTE      0x13u
#define EX15_CMD_PROGRAM_4BYTE   0x12u
#define EX15_CMD_ERASE_4K_4BYTE  0x21u

extern QSPI_HandleTypeDef hqspi;

/* ------------------------------------------------------------------ */
/* DMA محور — فاز داده با موتور مشترک fm_xdma.h                        */
/* ------------------------------------------------------------------ */
/*
 * تشخیص/نصب/فالبک/نگهداری کش همه در fm_xdma.h است؛ اینجا فقط پنجرهٔ
 * طول این درایور تنظیم می‌شود و در Ex15_QSPI_Begin موتور پیش‌فرض
 * خانواده نصب می‌شود (روی H7: MDMA اگر کیوب وصلش کرده باشید).
 */
#ifndef EX15_DMA_MIN_LEN
#define EX15_DMA_MIN_LEN 64u    /* کوچک‌تر از این: polling ارزان‌تر است */
#endif
#ifndef EX15_DMA_MAX_LEN
#define EX15_DMA_MAX_LEN 4096u  /* یک سکتور — سقف امن NbData           */
#endif

/* ------------------------------------------------------------------ */
/* شناسایی خودکار قطعه از JEDEC — جدول fm_qspi.c                        */
/* ------------------------------------------------------------------ */
static uint32_t    g_ex15_jedec;
static uint32_t    g_ex15_capacity   = EX15_W25Q256_CAPACITY; /* پیش از شناسایی */
static uint8_t     g_ex15_addr_bytes = 4u;
static const char *g_ex15_chip_name  = "unknown";

static uint8_t ex15_op_read(void)
{
    return (g_ex15_addr_bytes == 4u) ? EX15_CMD_READ_4BYTE
                                     : EX15_CMD_READ_3BYTE;
}

static uint8_t ex15_op_program(void)
{
    return (g_ex15_addr_bytes == 4u) ? EX15_CMD_PROGRAM_4BYTE
                                     : EX15_CMD_PROGRAM_3BYTE;
}

static uint8_t ex15_op_erase4k(void)
{
    return (g_ex15_addr_bytes == 4u) ? EX15_CMD_ERASE_4K_4BYTE
                                     : EX15_CMD_ERASE_4K_3BYTE;
}

static FM_NorBlockDevice g_ex15_qspi_nor;
static FM_Storage        g_ex15_qspi_storage;
static FM_FS             g_ex15_qspi_fs;
#if defined(__GNUC__)
static uint8_t g_ex15_qspi_sector[EX15_W25Q256_SECTOR_SIZE]
    __attribute__((aligned(32)));
#else
static uint8_t g_ex15_qspi_sector[EX15_W25Q256_SECTOR_SIZE];
#endif
static FM_Bool g_ex15_qspi_registered;
static FM_Bool g_ex15_qspi_fs_mounted;

static FM_Result ex15_qspi_command(uint8_t instruction, uint32_t address,
                                   uint32_t address_mode,
                                   uint32_t data_mode, uint32_t length)
{
    QSPI_CommandTypeDef command;

    memset(&command, 0, sizeof(command));
    command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    command.Instruction = instruction;
    command.AddressMode = address_mode;
    command.AddressSize = (g_ex15_addr_bytes == 4u)
                              ? QSPI_ADDRESS_32_BITS
                              : QSPI_ADDRESS_24_BITS;
    command.Address = address;
    command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    command.DataMode = data_mode;
    command.DummyCycles = 0;
    command.NbData = length;
    command.DdrMode = QSPI_DDR_MODE_DISABLE;
    command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;

    return (HAL_QSPI_Command(&hqspi, &command, EX15_QSPI_TIMEOUT_MS) == HAL_OK)
               ? FM_OK : FM_ERR_IO;
}

static FM_Result ex15_qspi_wait_ready(void)
{
    QSPI_CommandTypeDef command;
    QSPI_AutoPollingTypeDef poll;

    memset(&command, 0, sizeof(command));
    command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    command.Instruction = EX15_CMD_READ_STATUS_1;
    command.AddressMode = QSPI_ADDRESS_NONE;
    command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    command.DataMode = QSPI_DATA_1_LINE;
    command.DummyCycles = 0;
    command.NbData = 1;
    command.DdrMode = QSPI_DDR_MODE_DISABLE;
    command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;

    memset(&poll, 0, sizeof(poll));
    poll.Match = 0;
    poll.Mask = 0x01u; /* WIP */
    poll.MatchMode = QSPI_MATCH_MODE_AND;
    poll.StatusBytesSize = 1;
    poll.Interval = 0x10u;
    poll.AutomaticStop = QSPI_AUTOMATIC_STOP_ENABLE;

    return (HAL_QSPI_AutoPolling(&hqspi, &command, &poll,
                                 EX15_QSPI_TIMEOUT_MS) == HAL_OK)
               ? FM_OK : FM_ERR_TIMEOUT;
}

static FM_Result ex15_qspi_read_status1(uint8_t *status)
{
    FM_Result r;

    if (status == NULL) return FM_ERR_INVALID_ARGUMENT;
    r = ex15_qspi_command(EX15_CMD_READ_STATUS_1, 0,
                          QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, 1);
    if (r != FM_OK) return r;
    return (HAL_QSPI_Receive(&hqspi, status, EX15_QSPI_TIMEOUT_MS) == HAL_OK)
               ? FM_OK : FM_ERR_IO;
}

static FM_Result ex15_qspi_write_enable(void)
{
    uint8_t status = 0;
    FM_Result r = ex15_qspi_command(EX15_CMD_WRITE_ENABLE, 0,
                                    QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0);

    if (r == FM_OK) r = ex15_qspi_read_status1(&status);
    if (r != FM_OK) return r;
    return (status & 0x02u) ? FM_OK : FM_ERR_ACCESS; /* WEL must be set */
}

static FM_Result ex15_qspi_read_id(uint32_t *id)
{
    uint8_t bytes[3];
    FM_Result r;

    if (id == NULL) return FM_ERR_INVALID_ARGUMENT;
    r = ex15_qspi_command(EX15_CMD_READ_JEDEC_ID, 0,
                          QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, 3);
    if (r != FM_OK) return r;
    if (HAL_QSPI_Receive(&hqspi, bytes, EX15_QSPI_TIMEOUT_MS) != HAL_OK)
    {
        return FM_ERR_IO;
    }
    *id = ((uint32_t)bytes[0] << 16) |
          ((uint32_t)bytes[1] << 8) | bytes[2];
    return FM_OK;
}

/*
 * شناسایی کامل: JEDEC را می‌خواند و اگر قطعه در جدول باشد (W25Q64 هم
 * هست)، ظرفیت/آدرس/opcodeها را برای همهٔ callbackها تنظیم می‌کند.
 * اگر ناشناخته باشد با پیام تمیز رد می‌شود.
 */
static FM_Result ex15_detect(void)
{
    FM_QspiChipInfo chip;
    uint32_t id = 0;
    FM_Result r;

    r = ex15_qspi_wait_ready();
    if (r == FM_OK) r = ex15_qspi_read_id(&id);
    if (r != FM_OK) return r;

    if (FM_Qspi_IdentifyById(id, &chip) != FM_OK)
    {
        FM_Log("ex15: JEDEC 0x%06lX nashnakht - add it to fm_qspi.c "
               "or use FM_QSPI_CHIP_CUSTOM\n", (unsigned long)id);
        return FM_ERR_NOT_READY;
    }
    g_ex15_jedec      = id;
    g_ex15_capacity   = (uint32_t)chip.size_bytes;
    g_ex15_addr_bytes = chip.addr_bytes;
    g_ex15_chip_name  = chip.name;
    FM_Log("ex15: %s, %lu KB, %u-byte addressing\n", chip.name,
           (unsigned long)(chip.size_bytes / 1024u),
           (unsigned)chip.addr_bytes);
    return FM_OK;
}

/* مرحلهٔ ثبت (دوباره بعد از detect صدا زده می‌شود) — فقط همان قطعه؟ */
static FM_Result ex15_flash_init(void *context)
{
    uint32_t id = 0;
    FM_Result r;

    (void)context;
    r = ex15_qspi_wait_ready();
    if (r == FM_OK) r = ex15_qspi_read_id(&id);
    if (r != FM_OK) return r;
    return (id == g_ex15_jedec) ? FM_OK : FM_ERR_NOT_READY;
}

static FM_Result ex15_flash_ready(void *context, FM_Bool *ready)
{
    (void)context;
    if (ready == NULL) return FM_ERR_INVALID_ARGUMENT;
    *ready = (ex15_qspi_wait_ready() == FM_OK) ? FM_TRUE : FM_FALSE;
    return *ready ? FM_OK : FM_ERR_NOT_READY;
}

static FM_Result ex15_flash_read(void *context, uint32_t address,
                                 void *data, uint32_t length)
{
    FM_Result r;

    (void)context;
    if (data == NULL || length == 0 ||
        address > g_ex15_capacity ||
        length > g_ex15_capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = ex15_qspi_command(ex15_op_read(), address,
                          QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, length);
    if (r != FM_OK) return r;

    /* DMA محور: انتقال بزرگ با موتور fm_xdma؛ نتیجه سه‌حالته است. */
    if (FM_Xdma_RxEx(data, length, EX15_QSPI_TIMEOUT_MS) == FM_XDMA_FAILED)
    {
        /* کنترلر Abort شده و دستور نیمه‌کاره مانده؛ از نو صادرش کن. */
        r = ex15_qspi_command(ex15_op_read(), address,
                              QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, length);
        if (r != FM_OK) return r;
    }

    return (HAL_QSPI_Receive(&hqspi, (uint8_t *)data,
                             EX15_QSPI_TIMEOUT_MS) == HAL_OK)
               ? FM_OK : FM_ERR_IO;
}

static FM_Result ex15_flash_program(void *context, uint32_t address,
                                    const void *data, uint32_t length)
{
    FM_Result r;

    (void)context;
    if (data == NULL || length == 0 || length > EX15_W25Q256_PAGE_SIZE ||
        address / EX15_W25Q256_PAGE_SIZE !=
            (address + length - 1u) / EX15_W25Q256_PAGE_SIZE ||
        address > g_ex15_capacity ||
        length > g_ex15_capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = ex15_qspi_write_enable();
    if (r == FM_OK)
    {
        r = ex15_qspi_command(ex15_op_program(), address,
                              QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, length);
    }
    if (r != FM_OK) return r;

    /* DMA محور: نوشتن صفحه با موتور fm_xdma. */
    if (FM_Xdma_TxEx(data, length, EX15_QSPI_TIMEOUT_MS) == FM_XDMA_FAILED)
    {
        /* کنترلر Abort شده؛ WREN + PROGRAM را از نو بزن. */
        r = ex15_qspi_write_enable();
        if (r == FM_OK)
        {
            r = ex15_qspi_command(ex15_op_program(), address,
                                  QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE,
                                  length);
        }
        if (r != FM_OK) return r;
    }

    if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)data,
                          EX15_QSPI_TIMEOUT_MS) != HAL_OK)
    {
        r = FM_ERR_IO;
    }
    if (r == FM_OK) r = ex15_qspi_wait_ready();
    return r;
}

static FM_Result ex15_flash_erase(void *context, uint32_t address,
                                  uint32_t length)
{
    FM_Result r = FM_OK;

    (void)context;
    if (length == 0 || address % EX15_W25Q256_SECTOR_SIZE != 0 ||
        length % EX15_W25Q256_SECTOR_SIZE != 0 ||
        address > g_ex15_capacity ||
        length > g_ex15_capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    while (length > 0 && r == FM_OK)
    {
        r = ex15_qspi_write_enable();
        if (r == FM_OK)
        {
            r = ex15_qspi_command(ex15_op_erase4k(), address,
                                  QSPI_ADDRESS_1_LINE, QSPI_DATA_NONE, 0);
        }
        if (r == FM_OK) r = ex15_qspi_wait_ready();
        address += EX15_W25Q256_SECTOR_SIZE;
        length -= EX15_W25Q256_SECTOR_SIZE;
    }
    return r;
}

/*
 * فایل‌سیستم روی فلش — پیش‌فرض FMFS:
 *   نه diskio می‌خواهد نه VOLUMES، و با درایو 0 (SD کیوب) تداخلی ندارد.
 *   برای FatFS (مثلاً USB MSC مثال ۱۶): قبل از include بدهید
 *   EX15_USE_FATFS=1 و در فت‌فس VOLUMES>=2 و EX15_QSPI_FATFS_DRIVE=1.
 */
static FM_Result ex15_qspi_mount_fs(void)
{
    FM_Result r;

#if defined(EX15_USE_FATFS) && EX15_USE_FATFS
    r = FatFS_BindStorage((uint8_t)EX15_QSPI_FATFS_DRIVE,
                          &g_ex15_qspi_storage);
    if (r != FM_OK) return r;
    r = FatFS_Init(&g_ex15_qspi_fs, (uint8_t)EX15_QSPI_FATFS_DRIVE);
    if (r != FM_OK) return r;
#else
    r = FMFS_Init(&g_ex15_qspi_fs);
    if (r != FM_OK) return r;
#endif
    r = FS_Mount(EX15_QSPI_MOUNT_PATH, &g_ex15_qspi_fs);
    if (r == FM_OK) g_ex15_qspi_fs_mounted = FM_TRUE;
    return r;
}

FM_Result Ex15_QSPI_Begin(void)
{
    FM_FlashDeviceConfig flash;
    FM_Result r;

    r = FileManager_Init();
    if (r != FM_OK) return r;

    /* موتور DMA پیش‌فرض خانواده (روی H7: MDMA اگر وصل باشد).
     * اگر برنامه خودش FM_Xdma_Use زده، دست نمی‌زنیم. */
    FM_Xdma_SetWindow(EX15_DMA_MIN_LEN, EX15_DMA_MAX_LEN);
    FM_Xdma_AutoInstallQspi(&hqspi);

    /* اول قطعه را بشناس (W25Q64/128/256 و ...) تا ظرفیت و آدرس
     * درست به ثبت برسد. ناشناخته؟ پیام تمیز و رد. */
    r = ex15_detect();
    if (r != FM_OK) return r;

    memset(&flash, 0, sizeof(flash));
    flash.name = "qspi";
    flash.type = FM_STORAGE_QSPI;
    flash.capacity = g_ex15_capacity;   /* از شناسایی خودکار */
    flash.page_size = EX15_W25Q256_PAGE_SIZE;
    flash.erase_size = EX15_W25Q256_SECTOR_SIZE;
    flash.present = FM_TRUE;
    flash.init = ex15_flash_init;
    flash.is_ready = ex15_flash_ready;
    flash.read = ex15_flash_read;
    flash.program = ex15_flash_program;
    flash.erase = ex15_flash_erase;

    r = NorBlockDevice_Init(&g_ex15_qspi_nor, &flash, 512,
                            g_ex15_qspi_sector,
                            sizeof(g_ex15_qspi_sector));
    if (r != FM_OK) return r;
    r = Storage_InitNorBlockDevice(&g_ex15_qspi_storage,
                                   &g_ex15_qspi_nor);
    if (r != FM_OK) return r;
    r = Storage_Register(&g_ex15_qspi_storage);
    if (r == FM_ERR_EXISTS)
    {
        FM_Storage *existing = NULL;

        r = Storage_Find("qspi", &existing);
        if (r != FM_OK || existing != &g_ex15_qspi_storage)
        {
            return FM_ERR_EXISTS;
        }
    }
    else if (r != FM_OK)
    {
        return r;
    }
    g_ex15_qspi_registered = FM_TRUE;
    r = Storage_Mount("qspi");
    if (r != FM_OK) return r;
    return ex15_qspi_mount_fs();
}

FM_Storage *Ex15_QSPI_GetStorage(void)
{
    return g_ex15_qspi_registered ? &g_ex15_qspi_storage : NULL;
}

/* Explicit destructive operation for a blank/corrupt flash. This works even
 * when Ex15_QSPI_Begin initialized the block device but its first FS_Mount
 * failed because no FAT volume existed yet. On success it mounts /qspi. */
FM_Result Ex15_QSPI_FormatAndMount(void)
{
    FM_Result r;

    if (!g_ex15_qspi_registered || g_ex15_qspi_fs.ops.format == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    if (g_ex15_qspi_fs_mounted)
    {
        /* Preserve FatFS's initialized adapter context; FS_Format is the
         * supported operation for an already registered lazy mount. */
        return FS_Format(EX15_QSPI_MOUNT_PATH);
    }
    r = g_ex15_qspi_fs.ops.format(&g_ex15_qspi_fs);
    if (r != FM_OK) return r;
    return ex15_qspi_mount_fs();
}

FM_Result Ex15_QSPI_UnmountFSForUSB(void)
{
    FM_Result r = FM_OK;

    if (g_ex15_qspi_fs_mounted)
    {
        r = FS_Unmount(EX15_QSPI_MOUNT_PATH);
        if (r == FM_OK) g_ex15_qspi_fs_mounted = FM_FALSE;
    }
    if (r == FM_OK) r = Storage_Sync("qspi");
    return r;
}

FM_Result Ex15_QSPI_RemountFSAfterUSB(void)
{
    if (g_ex15_qspi_fs_mounted) return FM_OK;
    return ex15_qspi_mount_fs();
}

void Ex15_QSPI_End(void)
{
    if (g_ex15_qspi_fs_mounted)
    {
        (void)FS_Unmount(EX15_QSPI_MOUNT_PATH);
        g_ex15_qspi_fs_mounted = FM_FALSE;
    }
    (void)Storage_Unmount("qspi");
    if (g_ex15_qspi_registered)
    {
        (void)Storage_Unregister("qspi");
        g_ex15_qspi_registered = FM_FALSE;
    }
}

#endif /* !FM_HOST_BUILD */
#endif /* QSPI_W25Q_H_ */
