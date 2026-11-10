/*
 * Example 16 — نمایش SD / RAM / QSPI به PC با STM32Cube USB Device MSC
 *
 * نکتهٔ اصلی:
 *   USB MSC «فایل» صادر نمی‌کند؛ sectorهای خام یک دیسک را صادر می‌کند.
 *   پس bytes حافظه باید FAT12/16/32 باشند تا Windows/Linux/macOS آن را مثل
 *   یک drive ببینند. FMFS و LittleFS مستقیماً برای PC قابل mount نیستند.
 *
 * قانون جلوگیری از خرابی FAT:
 *   در هر لحظه فقط یکی از MCU یا PC مالک writable volume باشد.
 *
 *   MCU mode:
 *     FS_Mount -> File_* مجاز، USB MSC غیرفعال
 *
 *   PC mode:
 *     همهٔ Fileها بسته -> FS_Unmount -> Storage_Sync -> USB فعال
 *     File_* روی همان volume ممنوع
 *
 *   بازگشت به MCU:
 *     host Eject/Safely Remove -> USBD_Stop -> USB sync -> FS_Mount
 *
 * این header در main.c و usbd_storage_if.c استفاده می‌شود. context باید فقط
 * در یک translation unit ساخته شود؛ در main.c قبل از include بنویسید:
 *
 *   #define EX16_USB_DEFINE_CONTEXT
 *   #include "usb_msc.h"
 *
 * و در usbd_storage_if.c همان header را بدون define بالا include کنید.
 *
 * اتصال به usbd_storage_if.c تولیدشده توسط CubeMX:
 *
 *   int8_t STORAGE_Init_FS(uint8_t lun)
 *   { return Ex16_STM32_Init(lun); }
 *
 *   int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *n, uint16_t *s)
 *   { return Ex16_STM32_GetCapacity(lun, n, s); }
 *
 *   int8_t STORAGE_IsReady_FS(uint8_t lun)
 *   { return Ex16_STM32_IsReady(lun); }
 *
 *   int8_t STORAGE_IsWriteProtected_FS(uint8_t lun)
 *   { return Ex16_STM32_IsWriteProtected(lun); }
 *
 *   int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf,
 *                          uint32_t lba, uint16_t count)
 *   { return Ex16_STM32_Read(lun, buf, lba, count); }
 *
 *   int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf,
 *                           uint32_t lba, uint16_t count)
 *   { return Ex16_STM32_Write(lun, buf, lba, count); }
 *
 *   int8_t STORAGE_GetMaxLun_FS(void)
 *   { return Ex16_STM32_GetMaxLun(); }
 *
 * ترتیب روشن/خاموش:
 *
 *   // QSPI Example 15:
 *   Ex15_QSPI_Begin();
 *   ... File_* ...
 *   Ex16_USB_Begin(Ex15_QSPI_GetStorage(),
 *                  ex16_release_qspi, ex16_acquire_qspi, NULL);
 *   MX_USB_DEVICE_Init();             // حالا PC دیسک را می‌بیند
 *
 *   // بعد از Eject توسط کاربر:
 *   USBD_Stop(&hUsbDeviceFS);
 *   Ex16_USB_End();                   // FileManager دوباره mount می‌شود
 *
 *   // SD polling از Example 12:
 *   Ex16_USB_Begin(ex_12_sd_get_storage(),
 *                  ex_12_sd_release_for_usb,
 *                  ex_12_sd_return_from_usb, NULL);
 *
 * release/acquire آمادهٔ QSPI پایین همین فایل قرار دارد و RAM-FAT نیز در
 * انتهای فایل نمونه دارد. خود bridge به نوع حافظه وابسته نیست.
 */

#ifndef USB_MSC_H_
#define USB_MSC_H_

#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_log.h"
#include "fm_cache.h"
#include "fm_usb_msc.h"
#include "fm_fatfs.h"

#define EX16_USB_OK        ((int8_t)0)
#define EX16_USB_TRUE      ((int8_t)1)
#define EX16_USB_FAIL      ((int8_t)-1)

typedef FM_Result (*Ex16_OwnerSwitchFn)(void *context);

typedef struct
{
    FM_USB_MSC msc;
    Ex16_OwnerSwitchFn release_from_mcu;
    Ex16_OwnerSwitchFn return_to_mcu;
    void *owner_context;
    FM_Bool pc_owns_media;
} Ex16_USBContext;

/* Define EX16_USB_DEFINE_CONTEXT in exactly one .c file (normally main.c).
 * Other users such as usbd_storage_if.c see the same external instance. */
#if defined(EX16_USB_DEFINE_CONTEXT)
Ex16_USBContext g_ex16_usb;
#else
extern Ex16_USBContext g_ex16_usb;
#endif

/* شمارهٔ ساخت - برای اطمینان از تازگی هدرها روی برد. لاگ باید این را
 * داشته باشد؛ اگر نداشت یعنی پروژه، هدرِ کهنه را بیلد کرده است. */
#ifndef EX16_BUILD_ID
#define EX16_BUILD_ID "r13-gaptelem"
#endif

/* آمار ترافیک MSC - برای تشخیص زنده از لاگ */
static volatile uint32_t g_ex16_rd, g_ex16_wr, g_ex16_fail;
/* فاصلهٔ بین دو READ10 پشت‌سرهم (دور ۱۳): خودِ خواندن روی برد t=0ms است؛
 * اگر ویندوزِ بین درخواست‌ها مکث کند اینجا لو می‌رود که کِی و چقدر. */
static uint32_t g_ex16_prev_tick = 0xFFFFFFFFu;

/* ------------------------------------------------------------------ */
/* بافر واسط خودمان - مسیر MSC مستقل از وضعیت D-Cache                  */
/* ------------------------------------------------------------------ */
/* چرا؟ بافر میانهٔ پشتهٔ USB کیوب (bot_data) را ما کنترل نمی‌کنیم:
 * تراز/جایش دست کیوب است و در عمل روی برد (کش روشن) داده در آن مسیر
 * صفر می‌رسید. حالا هر پاکت از دو مرحلهٔ «کاملاً معین» رد می‌شود:
 *   ۱) DMA درایور (SD/QSPI) داخل بافرِ ۳۲بایت‌ترازِ خودمان می‌نویسد
 *      (درایور خودش Invalidate دارد) -> CPU از RAM واقعی می‌خواند.
 *   ۲) memcpy با CPU به بافر کیوب -> CleanForDma -> هستهٔ USB با DMA
 *      از RAM واقعی می‌خواند. هیچ فرضی به MPU/کش/تراز بافر کیوب نیست.
 * لازم شد بافر را در ناحیهٔ DMA-پسند بگذار (مثلاً SDRAM)، قبل از
 * include یعنی 16 این را تعریف کن:
 *   #define EX16_BOUNCE_SECTION __attribute__((section(".sdram_buffers")))
 */
#ifndef EX16_BOUNCE_SECTORS
#define EX16_BOUNCE_SECTORS 8      /* 4KB - پنجرهٔ هر انتقال */
#endif
#ifndef EX16_BOUNCE_SECTION
#define EX16_BOUNCE_SECTION
#endif
static uint8_t g_ex16_bounce[EX16_BOUNCE_SECTORS * 512u]
    __attribute__((aligned(32))) EX16_BOUNCE_SECTION;

/* `storage` must represent a FAT-formatted logical block volume. Begin first
 * asks the application to close/unmount its local filesystem, then enables
 * sector callbacks. Start the Cube USB device only after this returns FM_OK. */
static inline FM_Result Ex16_USB_Begin(FM_Storage *storage,
                                       Ex16_OwnerSwitchFn release_from_mcu,
                                       Ex16_OwnerSwitchFn return_to_mcu,
                                       void *owner_context)
{
    FM_Result r;

    if (storage == NULL || release_from_mcu == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (g_ex16_usb.pc_owns_media)
    {
        return FM_ERR_BUSY;
    }
    r = release_from_mcu(owner_context);
    if (r != FM_OK) return r;
    if (storage->sync != NULL)
    {
        r = storage->sync(storage);
        if (r != FM_OK)
        {
            if (return_to_mcu != NULL) (void)return_to_mcu(owner_context);
            return r;
        }
    }
    memset(&g_ex16_usb, 0, sizeof(g_ex16_usb));
    r = USB_MSC_Init(&g_ex16_usb.msc, storage, 512);
    if (r != FM_OK)
    {
        if (return_to_mcu != NULL) (void)return_to_mcu(owner_context);
        return r;
    }
    g_ex16_usb.release_from_mcu = release_from_mcu;
    g_ex16_usb.return_to_mcu = return_to_mcu;
    g_ex16_usb.owner_context = owner_context;
    g_ex16_usb.pc_owns_media = FM_TRUE;
    return FM_OK;
}

/* Call only after the USB device has stopped and the host has ejected the
 * volume. It syncs host writes before remounting for FileManager. */
static inline FM_Result Ex16_USB_End(void)
{
    FM_Result r;
    FM_Result remount = FM_OK;

    if (!g_ex16_usb.pc_owns_media)
    {
        return FM_ERR_NOT_READY;
    }
    r = USB_MSC_Sync(&g_ex16_usb.msc);
    g_ex16_usb.pc_owns_media = FM_FALSE;
    if (g_ex16_usb.return_to_mcu != NULL)
    {
        remount = g_ex16_usb.return_to_mcu(g_ex16_usb.owner_context);
    }
    memset(&g_ex16_usb, 0, sizeof(g_ex16_usb));
    return (r != FM_OK) ? r : remount;
}

/* آمار ترافیک PC - هرجا خواستی وضعیت زنده را لاگ کن */
static inline void Ex16_USB_GetStats(uint32_t *rd, uint32_t *wr,
                                     uint32_t *fail)
{
    if (rd)   *rd   = g_ex16_rd;
    if (wr)   *wr   = g_ex16_wr;
    if (fail) *fail = g_ex16_fail;
}

static inline FM_Bool Ex16_USB_PCIsOwner(void)
{
    return g_ex16_usb.pc_owns_media;
}

/* ---- Adapters called from CubeMX usbd_storage_if.c --------------------- */
static inline int8_t Ex16_STM32_Init(uint8_t lun)
{
    (void)lun;
    return g_ex16_usb.pc_owns_media ? EX16_USB_OK : EX16_USB_FAIL;
}

static inline int8_t Ex16_STM32_GetCapacity(uint8_t lun,
                                            uint32_t *block_count,
                                            uint16_t *block_size)
{
    uint64_t count;
    uint32_t size;
    FM_Result r;

    (void)lun;
    if (!g_ex16_usb.pc_owns_media || block_count == NULL || block_size == NULL)
    {
        return EX16_USB_FAIL;
    }
    r = USB_MSC_GetCapacity(&g_ex16_usb.msc, &count, &size);
    if (r != FM_OK || count > UINT32_MAX || size > UINT16_MAX)
    {
        return EX16_USB_FAIL;
    }
    *block_count = (uint32_t)count;
    *block_size = (uint16_t)size;
    return EX16_USB_OK;
}

static inline int8_t Ex16_STM32_IsReady(uint8_t lun)
{
    (void)lun;
    return (g_ex16_usb.pc_owns_media &&
            g_ex16_usb.msc.storage != NULL &&
            g_ex16_usb.msc.storage->present)
               ? EX16_USB_OK : EX16_USB_FAIL;
}

/*
 * گزینهٔ امن: EX16_USB_FORCE_READONLY=1 بده تا PC درایو را فقط-خواندنی
 * ببیند (Write-Protected). در این حالت ویندوز نمی‌تواند FAT را تغییر
 * دهد — بهترین حالت وقتی است که نمی‌خواهی مالکیت را عوض کنی و فقط
 * می‌خواهی PC فایل‌ها را بخواند. همچنان قانون مالکیت را رعایت کن:
 * PC فقط بخواند و MCU هم روی همان volume فقط بخواند.
 */
#ifndef EX16_USB_FORCE_READONLY
#define EX16_USB_FORCE_READONLY 0
#endif

static inline int8_t Ex16_STM32_IsWriteProtected(uint8_t lun)
{
    (void)lun;
#if EX16_USB_FORCE_READONLY
    return EX16_USB_TRUE;
#else
    return (g_ex16_usb.msc.storage != NULL &&
            g_ex16_usb.msc.storage->read_only)
               ? EX16_USB_TRUE : EX16_USB_OK;
#endif
}

static inline int8_t Ex16_STM32_Read(uint8_t lun, uint8_t *buffer,
                                     uint32_t lba, uint16_t block_count)
{
    (void)lun;
    if (!g_ex16_usb.pc_owns_media)
    {
        return EX16_USB_FAIL;
    }
    {
        uint32_t done = 0;
        uint32_t t0 = HAL_GetTick();

        if (g_ex16_prev_tick != 0xFFFFFFFFu &&
            (t0 - g_ex16_prev_tick) > 250u)
        {
            FM_Log("ex16: gap %lums bein-e READ10-ha (ghabl-e lba=%lu)\n",
                   (unsigned long)(t0 - g_ex16_prev_tick),
                   (unsigned long)lba);
        }
        g_ex16_prev_tick = t0;

        while (done < (uint32_t)block_count)
        {
            uint32_t n = (uint32_t)block_count - done;
            size_t bytes;
            FM_Result r;

            if (n > (uint32_t)EX16_BOUNCE_SECTORS)
            {
                n = (uint32_t)EX16_BOUNCE_SECTORS;
            }
            bytes = (size_t)n * 512u;

            /* مرحله ۱: درایور -> بافر واسط خودمان، با «پروتکل کامل»
             * خودمان (نه تکیه بر درایور): Clean قبل از کیک (خطوط کثیفِ
             * احتمالی بیرون بریزد) -> DMA درایور -> Clean+Invalidate
             * بعد از آن => memcpy تضمینی از RAM واقعی می‌خواند. */
            FM_Cache_CleanForDma(g_ex16_bounce, bytes);
            r = USB_MSC_ReadBlocks(&g_ex16_usb.msc, lba + done, n,
                                   g_ex16_bounce);
            if (r != FM_OK)
            {
                g_ex16_fail++;
                FM_Log("ex16: !! READ fail lba=%lu n=%u (fail=%lu)\n",
                       (unsigned long)(lba + done), (unsigned)n,
                       (unsigned long)g_ex16_fail);
                return EX16_USB_FAIL;
            }
            FM_Cache_CleanForDma(g_ex16_bounce, bytes);
            FM_Cache_InvalidateAfterDma(g_ex16_bounce, bytes);

            if (lba + done == 0u && g_ex16_rd < 9u)
            {
                uint8_t s1a = g_ex16_bounce[510], s1b = g_ex16_bounce[511];

                if (s1a == 0x55u && s1b == 0xAAu)
                {
                    FM_Log("ex16: lba0 OK (sig 55 AA, rd=%lu)\n",
                           (unsigned long)g_ex16_rd);
                }
                else
                {
                    /* یک Invalidate صریح دیگر + نگاه دوباره: اگر این بار
                     * 55AA آمد یعنی داده در RAM بود و خطوط کش کهنه بودند؛
                     * اگر باز صفر بود یعنی DMA درایور واقعا ننوشته
                     * (مسیر ISR/SDMMC) - لاگ را بفرست تا سراغش برویم. */
                    SCB_InvalidateDCache_by_Addr(
                        (uint32_t *)(uintptr_t)g_ex16_bounce,
                        (int32_t)bytes);
                    __DSB();
                    if (g_ex16_bounce[510] == 0x55u &&
                        g_ex16_bounce[511] == 0xAAu)
                    {
                        FM_Log("%s\n", "ex16: lba0 ba invalide doomm salam "
                               "shod (cache line - protocol fixed it)");
                    }
                    else
                    {
                        /* خواندن دوباره با بافر تازهٔ جدایbounce: اگر این
                         * هم صفر بود یعنی خود پای درایور در این context
                         * داده نمی‌دهد (نه کش، نه آدرس بافر) */
                        uint8_t probe[512];
                        uint8_t via_fm[512];
                        FM_Result r1, r2;
                        int i;

                        /* پروب ۱: همان مسیر، بافر تازه */
                        memset(probe, 0x5Au, sizeof(probe));
                        r1 = USB_MSC_ReadBlocks(&g_ex16_usb.msc, 0u, 1u,
                                                probe);
                        FM_Cache_CleanForDma(probe, sizeof(probe));
                        FM_Cache_InvalidateAfterDma(probe, sizeof(probe));
                        /* پروب ۲: مستقیم از درایور (بدون لایهٔ USB-MSC) */
                        memset(via_fm, 0x5Au, sizeof(via_fm));
                        r2 = Storage_ReadBlocks("SD", 0u, via_fm,
                                                sizeof(via_fm));
                        FM_Cache_CleanForDma(via_fm, sizeof(via_fm));
                        FM_Cache_InvalidateAfterDma(via_fm,
                                                    sizeof(via_fm));
                        FM_Log("ex16: lba0 ZERO! r1=%d r2=%d\n",
                               (int)r1, (int)r2);
                        FM_Log("ex16: usb16:");
                        for (i = 0; i < 16; i++)
                        {
                            FM_Log(" %02X", probe[i]);
                        }
                        FM_Log("%s\n", "");
                        FM_Log("ex16: sd--16:");
                        for (i = 0; i < 16; i++)
                        {
                            FM_Log(" %02X", via_fm[i]);
                        }
                        FM_Log("%s\n", "");
                    }
                }
            }

            /* مرحله ۲: کپی CPU به بافر کیوب + Clean => هستهٔ USB با DMA
             * از RAM واقعی می‌خواند. مستقل از کش/MPU/تراز کیوب. */
            memcpy(buffer + (size_t)done * 512u, g_ex16_bounce, bytes);
            FM_Cache_CleanForDma(buffer + (size_t)done * 512u, bytes);

            done += n;
            g_ex16_rd += n;
            if ((g_ex16_rd % 128u) < (uint32_t)n)
            {
                FM_Log("ex16: rd=%lu fail=%lu (lba=%lu) t=%lums\n",
                       (unsigned long)g_ex16_rd,
                       (unsigned long)g_ex16_fail,
                       (unsigned long)(lba + done),
                       (unsigned long)(HAL_GetTick() - t0));
            }
        }
        if ((HAL_GetTick() - t0) > 2000u)
        {
            /* یک درخواست خواندن که بیش از ۲ ثانیه کشید = کارت مکث/گیر
             * کرده؛ دقیقاً همین لحظه‌هاست که اکسپلورر قفل می‌زند. */
            FM_Log("ex16: !! RD SLOW n=%u lba=%lu ms=%lu\n",
                   (unsigned)block_count, (unsigned long)lba,
                   (unsigned long)(HAL_GetTick() - t0));
        }
    }
    return EX16_USB_OK;
}

static inline int8_t Ex16_STM32_Write(uint8_t lun, const uint8_t *buffer,
                                      uint32_t lba, uint16_t block_count)
{
    FM_Result r;

    (void)lun;
    if (!g_ex16_usb.pc_owns_media)
    {
        return EX16_USB_FAIL;
    }
    {
        uint32_t done = 0;
        uint32_t t0 = HAL_GetTick();

        while (done < (uint32_t)block_count)
        {
            uint32_t n = (uint32_t)block_count - done;
            size_t bytes = (size_t)n * 512u;

            /* هستهٔ USB تازه با DMA در بافر کیوب نوشته؛ CPU باید دادهٔ
             * تازهٔ RAM را ببیند: Clean (خطوط کثیفِ همسایه بیرون بریزد)
             * بعد Invalidate. سپس کپی CPU به بافر واسط خودمان و Clean
             * آن برای DMA خواندنِ درایور. */
            FM_Cache_CleanForDma(buffer + (size_t)done * 512u, bytes);
            FM_Cache_InvalidateAfterDma((void *)(uintptr_t)(buffer +
                                        (size_t)done * 512u), bytes);
            memcpy(g_ex16_bounce, buffer + (size_t)done * 512u, bytes);
            FM_Cache_CleanForDma(g_ex16_bounce, bytes);

            r = USB_MSC_WriteBlocks(&g_ex16_usb.msc, lba + done, n,
                                    g_ex16_bounce);
            g_ex16_wr += n;
            if (r != FM_OK)
            {
                g_ex16_fail++;
                FM_Log("ex16: !! WRITE fail lba=%lu n=%u (fail=%lu)\n",
                       (unsigned long)(lba + done), (unsigned)n,
                       (unsigned long)g_ex16_fail);
                return EX16_USB_FAIL;
            }
            if ((g_ex16_wr % 32u) < (uint32_t)n)
            {
                FM_Log("ex16: wr=%lu fail=%lu (lba=%lu) t=%lums\n",
                       (unsigned long)g_ex16_wr,
                       (unsigned long)g_ex16_fail,
                       (unsigned long)(lba + done),
                       (unsigned long)(HAL_GetTick() - t0));
            }
            done += n;
        }
        if ((HAL_GetTick() - t0) > 2000u)
        {
            FM_Log("ex16: !! WR SLOW n=%u lba=%lu ms=%lu\n",
                   (unsigned)block_count, (unsigned long)lba,
                   (unsigned long)(HAL_GetTick() - t0));
        }
    }
    return EX16_USB_OK;
}

static inline int8_t Ex16_STM32_GetMaxLun(void)
{
    return 0; /* one logical unit */
}

/* Ready-made ownership callbacks for Example 15 QSPI. Define
 * EXAMPLES_15... first (normally by including its header) before using these. */
#if defined(QSPI_W25Q_H_)
static inline FM_Result Ex16_ReleaseQSPI(void *context)
{
    (void)context;
    return Ex15_QSPI_UnmountFSForUSB();
}

static inline FM_Result Ex16_ReturnQSPI(void *context)
{
    (void)context;
    return Ex15_QSPI_RemountFSAfterUSB();
}
#endif

/* ------------------------------------------------------------------------ */
/* Optional FAT RAM-disk example                                            */
/* ------------------------------------------------------------------------ */
#if FM_USE_FATFS
typedef struct
{
    FM_Storage storage;
    FM_FS fs;
    const char *name;
    const char *mount_path;
    uint8_t drive;
    FM_Bool registered;
    FM_Bool mounted;
} Ex16_RAMFAT;

/* The RAM buffer must stay alive and its size must be a multiple of 512.
 * A blank buffer must be formatted once with Ex16_RAMFAT_Format(). */
static inline FM_Result Ex16_RAMFAT_Begin(Ex16_RAMFAT *disk,
                                          void *buffer, size_t size,
                                          uint8_t fatfs_drive,
                                          const char *mount_path)
{
    FM_Result r;

    if (disk == NULL || buffer == NULL || size < 4096 ||
        size % 512u != 0 || mount_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(disk, 0, sizeof(*disk));
    disk->name = "usbram";
    disk->mount_path = mount_path;
    disk->drive = fatfs_drive;
    r = RamDisk_Init(&disk->storage, disk->name, buffer, size, 512);
    if (r != FM_OK) return r;
    r = Storage_Register(&disk->storage);
    if (r != FM_OK) return r;
    disk->registered = FM_TRUE;
    r = Storage_Mount(disk->name);
    if (r != FM_OK) return r;
    r = FatFS_BindStorage(fatfs_drive, &disk->storage);
    if (r != FM_OK) return r;
    r = FatFS_Init(&disk->fs, fatfs_drive);
    if (r != FM_OK) return r;
    r = FS_Mount(mount_path, &disk->fs);
    if (r == FM_OK) disk->mounted = FM_TRUE;
    return r;
}

/* Explicitly format a blank/corrupt RAM buffer, then mount it. This remains
 * callable when Ex16_RAMFAT_Begin completed initialization but its first
 * FS_Mount failed because the buffer did not contain a FAT volume yet. */
static inline FM_Result Ex16_RAMFAT_Format(Ex16_RAMFAT *disk)
{
    FM_Result r;

    if (disk == NULL || !disk->registered || disk->fs.ops.format == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    if (disk->mounted)
    {
        /* Keep the initialized FatFS adapter context of the lazy mount. */
        return FS_Format(disk->mount_path);
    }
    r = disk->fs.ops.format(&disk->fs);
    if (r != FM_OK) return r;
    r = FS_Mount(disk->mount_path, &disk->fs);
    if (r == FM_OK) disk->mounted = FM_TRUE;
    return r;
}

static inline FM_Result Ex16_ReleaseRAM(void *context)
{
    Ex16_RAMFAT *disk = (Ex16_RAMFAT *)context;
    FM_Result r;

    if (disk == NULL || !disk->mounted) return FM_ERR_NOT_MOUNTED;
    r = FS_Unmount(disk->mount_path);
    if (r == FM_OK) disk->mounted = FM_FALSE;
    return r;
}

static inline FM_Result Ex16_ReturnRAM(void *context)
{
    Ex16_RAMFAT *disk = (Ex16_RAMFAT *)context;
    FM_Result r;

    if (disk == NULL) return FM_ERR_INVALID_ARGUMENT;
    if (disk->mounted) return FM_OK;
    r = FatFS_Init(&disk->fs, disk->drive);
    if (r != FM_OK) return r;
    r = FatFS_BindStorage(disk->drive, &disk->storage);
    if (r != FM_OK) return r;
    r = FS_Mount(disk->mount_path, &disk->fs);
    if (r == FM_OK) disk->mounted = FM_TRUE;
    return r;
}

static inline void Ex16_RAMFAT_End(Ex16_RAMFAT *disk)
{
    if (disk == NULL) return;
    if (disk->mounted)
    {
        (void)FS_Unmount(disk->mount_path);
        disk->mounted = FM_FALSE;
    }
    if (disk->registered)
    {
        (void)Storage_Unmount(disk->name);
        (void)Storage_Unregister(disk->name);
        disk->registered = FM_FALSE;
    }
}
#endif /* FM_USE_FATFS */

#endif /* USB_MSC_H_ */
