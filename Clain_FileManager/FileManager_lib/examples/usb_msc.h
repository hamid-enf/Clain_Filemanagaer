/*
 * usb_msc.h — نمایش SD / RAM / QSPI به PC با STM32Cube USB Device MSC
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
 *   #define USB_MSC_DEFINE_CONTEXT
 *   #include "usb_msc.h"
 *
 * و در usbd_storage_if.c همان header را بدون define بالا include کنید.
 *
 * اتصال به usbd_storage_if.c تولیدشده توسط CubeMX:
 *
 *   int8_t STORAGE_Init_FS(uint8_t lun)
 *   { return UsbMsc_StorageInit(lun); }
 *
 *   int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *n, uint16_t *s)
 *   { return UsbMsc_StorageGetCapacity(lun, n, s); }
 *
 *   int8_t STORAGE_IsReady_FS(uint8_t lun)
 *   { return UsbMsc_StorageIsReady(lun); }
 *
 *   int8_t STORAGE_IsWriteProtected_FS(uint8_t lun)
 *   { return UsbMsc_StorageIsWriteProtected(lun); }
 *
 *   int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf,
 *                          uint32_t lba, uint16_t count)
 *   { return UsbMsc_StorageRead(lun, buf, lba, count); }
 *
 *   int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf,
 *                           uint32_t lba, uint16_t count)
 *   { return UsbMsc_StorageWrite(lun, buf, lba, count); }
 *
 *   int8_t STORAGE_GetMaxLun_FS(void)
 *   { return UsbMsc_StorageGetMaxLun(); }
 *
 * ترتیب روشن/خاموش:
 *
 *   // QSPI qspi_w25q.h:
 *   QspiW25q_Begin();
 *   ... File_* ...
 *   UsbMsc_Begin(QspiW25q_GetStorage(),
 *                  usb_msc_release_qspi, usb_msc_acquire_qspi, NULL);
 *   MX_USB_DEVICE_Init();             // حالا PC دیسک را می‌بیند
 *
 *   // بعد از Eject توسط کاربر:
 *   USBD_Stop(&hUsbDeviceFS);
 *   UsbMsc_End();                   // FileManager دوباره mount می‌شود
 *
 *   // SD polling از sdcard_fm_diskio.c:
 *   UsbMsc_Begin(SdDiskio_GetStorage(),
 *                  SdDiskio_ReleaseForUsb,
 *                  SdDiskio_ReturnFromUsb, NULL);
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

#define USB_MSC_OK        ((int8_t)0)
#define USB_MSC_TRUE      ((int8_t)1)
#define USB_MSC_FAIL      ((int8_t)-1)

typedef FM_Result (*UsbMsc_OwnerSwitchFn)(void *context);

typedef struct
{
    FM_USB_MSC msc;
    UsbMsc_OwnerSwitchFn release_from_mcu;
    UsbMsc_OwnerSwitchFn return_to_mcu;
    void *owner_context;
    FM_Bool pc_owns_media;
} UsbMsc_Context;

/* Define USB_MSC_DEFINE_CONTEXT in exactly one .c file (normally main.c).
 * Other users such as usbd_storage_if.c see the same external instance. */
#if defined(USB_MSC_DEFINE_CONTEXT)
UsbMsc_Context g_usb_msc_ctx;
#else
extern UsbMsc_Context g_usb_msc_ctx;
#endif

/* شمارهٔ ساخت - برای اطمینان از تازگی هدرها روی برد. لاگ باید این را
 * داشته باشد؛ اگر نداشت یعنی پروژه، هدرِ کهنه را بیلد کرده است. */
#ifndef USB_MSC_BUILD_ID
#define USB_MSC_BUILD_ID "r13-gaptelem"
#endif

/* آمار ترافیک MSC - برای تشخیص زنده از لاگ */
static volatile uint32_t g_usb_msc_rd, g_usb_msc_wr, g_usb_msc_fail;
/* فاصلهٔ بین دو READ10 پشت‌سرهم (دور ۱۳): خودِ خواندن روی برد t=0ms است؛
 * اگر ویندوزِ بین درخواست‌ها مکث کند اینجا لو می‌رود که کِی و چقدر. */
static uint32_t g_usb_msc_prev_tick = 0xFFFFFFFFu;

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
 *   #define USB_MSC_BOUNCE_SECTION __attribute__((section(".sdram_buffers")))
 */
#ifndef USB_MSC_BOUNCE_SECTORS
#define USB_MSC_BOUNCE_SECTORS 8      /* 4KB - پنجرهٔ هر انتقال */
#endif
#ifndef USB_MSC_BOUNCE_SECTION
#define USB_MSC_BOUNCE_SECTION
#endif
#if (USB_MSC_BOUNCE_SECTORS) < 1
#error "USB_MSC_BOUNCE_SECTORS must be at least 1"
#endif
static uint8_t g_usb_msc_bounce[USB_MSC_BOUNCE_SECTORS * 512u]
    __attribute__((aligned(32))) USB_MSC_BOUNCE_SECTION;

/* `storage` must represent a FAT-formatted logical block volume. Begin first
 * asks the application to close/unmount its local filesystem, then enables
 * sector callbacks. Start the Cube USB device only after this returns FM_OK. */
static inline FM_Result UsbMsc_Begin(FM_Storage *storage,
                                       UsbMsc_OwnerSwitchFn release_from_mcu,
                                       UsbMsc_OwnerSwitchFn return_to_mcu,
                                       void *owner_context)
{
    FM_Result r;

    if (storage == NULL || release_from_mcu == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (g_usb_msc_ctx.pc_owns_media)
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
    memset(&g_usb_msc_ctx, 0, sizeof(g_usb_msc_ctx));
    r = USB_MSC_Init(&g_usb_msc_ctx.msc, storage, 512);
    if (r != FM_OK)
    {
        if (return_to_mcu != NULL) (void)return_to_mcu(owner_context);
        return r;
    }
    g_usb_msc_ctx.release_from_mcu = release_from_mcu;
    g_usb_msc_ctx.return_to_mcu = return_to_mcu;
    g_usb_msc_ctx.owner_context = owner_context;
    g_usb_msc_ctx.pc_owns_media = FM_TRUE;
    return FM_OK;
}

/* Call only after the USB device has stopped and the host has ejected the
 * volume. It syncs host writes before remounting for FileManager. */
static inline FM_Result UsbMsc_End(void)
{
    FM_Result r;
    FM_Result remount = FM_OK;

    if (!g_usb_msc_ctx.pc_owns_media)
    {
        return FM_ERR_NOT_READY;
    }
    r = USB_MSC_Sync(&g_usb_msc_ctx.msc);
    g_usb_msc_ctx.pc_owns_media = FM_FALSE;
    if (g_usb_msc_ctx.return_to_mcu != NULL)
    {
        remount = g_usb_msc_ctx.return_to_mcu(g_usb_msc_ctx.owner_context);
    }
    memset(&g_usb_msc_ctx, 0, sizeof(g_usb_msc_ctx));
    return (r != FM_OK) ? r : remount;
}

/* آمار ترافیک PC - هرجا خواستی وضعیت زنده را لاگ کن */
static inline void UsbMsc_GetStats(uint32_t *rd, uint32_t *wr,
                                     uint32_t *fail)
{
    if (rd)   *rd   = g_usb_msc_rd;
    if (wr)   *wr   = g_usb_msc_wr;
    if (fail) *fail = g_usb_msc_fail;
}

static inline FM_Bool UsbMsc_PcIsOwner(void)
{
    return g_usb_msc_ctx.pc_owns_media;
}

/* ---- Adapters called from CubeMX usbd_storage_if.c --------------------- */
static inline int8_t UsbMsc_StorageInit(uint8_t lun)
{
    (void)lun;
    return g_usb_msc_ctx.pc_owns_media ? USB_MSC_OK : USB_MSC_FAIL;
}

static inline int8_t UsbMsc_StorageGetCapacity(uint8_t lun,
                                            uint32_t *block_count,
                                            uint16_t *block_size)
{
    uint64_t count;
    uint32_t size;
    FM_Result r;

    (void)lun;
    if (!g_usb_msc_ctx.pc_owns_media || block_count == NULL || block_size == NULL)
    {
        return USB_MSC_FAIL;
    }
    r = USB_MSC_GetCapacity(&g_usb_msc_ctx.msc, &count, &size);
    if (r != FM_OK || count > UINT32_MAX || size > UINT16_MAX)
    {
        return USB_MSC_FAIL;
    }
    *block_count = (uint32_t)count;
    *block_size = (uint16_t)size;
    return USB_MSC_OK;
}

static inline int8_t UsbMsc_StorageIsReady(uint8_t lun)
{
    (void)lun;
    return (g_usb_msc_ctx.pc_owns_media &&
            g_usb_msc_ctx.msc.storage != NULL &&
            g_usb_msc_ctx.msc.storage->present)
               ? USB_MSC_OK : USB_MSC_FAIL;
}

/*
 * گزینهٔ امن: USB_MSC_FORCE_READONLY=1 بده تا PC درایو را فقط-خواندنی
 * ببیند (Write-Protected). در این حالت ویندوز نمی‌تواند FAT را تغییر
 * دهد — بهترین حالت وقتی است که نمی‌خواهی مالکیت را عوض کنی و فقط
 * می‌خواهی PC فایل‌ها را بخواند. همچنان قانون مالکیت را رعایت کن:
 * PC فقط بخواند و MCU هم روی همان volume فقط بخواند.
 */
#ifndef USB_MSC_FORCE_READONLY
#define USB_MSC_FORCE_READONLY 0
#endif

static inline int8_t UsbMsc_StorageIsWriteProtected(uint8_t lun)
{
    (void)lun;
#if USB_MSC_FORCE_READONLY
    return USB_MSC_TRUE;
#else
    return (g_usb_msc_ctx.msc.storage != NULL &&
            g_usb_msc_ctx.msc.storage->read_only)
               ? USB_MSC_TRUE : USB_MSC_OK;
#endif
}

/* عیب‌یابی: سکتور صفر بدون امضای 55 AA خوانده شد. دوباره با دو بافر
 * جدا می‌خواند و ۱۶ بایت اول هر دو را لاگ می‌کند.
 *
 * این دو بافر ۵۱۲ بایتی قبلاً متغیر محلیِ UsbMsc_StorageRead بودند: کامپایلر
 * جای‌شان را در ابتدای تابع می‌گیرد، یعنی هر READ(10) -- که داخل وقفهٔ
 * USB و روی پشتهٔ اصلی (MSP) اجرا می‌شود -- بیش از ۱ کیلوبایت پشته
 * می‌برد، برای شاخه‌ای که تقریباً هیچ‌وقت اجرا نمی‌شود. حالا static
 * هستند (۱ کیلوبایت RAM ثابت، صفر بایت پشته). تابع بازورودی نیست؛ لازم
 * هم نیست: فقط از یک جا (وقفهٔ USB یا UsbSd_Poll) صدا زده می‌شود. */
static void UsbMsc_Lba0Probe(void)
{
    static uint8_t probe[512];
    static uint8_t via_fm[512];
    FM_Result r1, r2;
    int i;

    /* پروب ۱: همان مسیر، بافر تازه */
    memset(probe, 0x5Au, sizeof(probe));
    r1 = USB_MSC_ReadBlocks(&g_usb_msc_ctx.msc, 0u, 1u, probe);
    FM_Cache_CleanForDma(probe, sizeof(probe));
    FM_Cache_InvalidateAfterDma(probe, sizeof(probe));
    /* پروب ۲: مستقیم از درایور (بدون لایهٔ USB-MSC) */
    memset(via_fm, 0x5Au, sizeof(via_fm));
    r2 = Storage_ReadBlocks("SD", 0u, via_fm, sizeof(via_fm));
    FM_Cache_CleanForDma(via_fm, sizeof(via_fm));
    FM_Cache_InvalidateAfterDma(via_fm, sizeof(via_fm));
    FM_Log("UsbMsc: lba0 ZERO! r1=%d r2=%d\n", (int)r1, (int)r2);
    FM_Log("UsbMsc: usb16:");
    for (i = 0; i < 16; i++)
    {
        FM_Log(" %02X", probe[i]);
    }
    FM_Log("%s\n", "");
    FM_Log("UsbMsc: sd--16:");
    for (i = 0; i < 16; i++)
    {
        FM_Log(" %02X", via_fm[i]);
    }
    FM_Log("%s\n", "");
}

static inline int8_t UsbMsc_StorageRead(uint8_t lun, uint8_t *buffer,
                                     uint32_t lba, uint16_t block_count)
{
    (void)lun;
    if (!g_usb_msc_ctx.pc_owns_media)
    {
        return USB_MSC_FAIL;
    }
    {
        uint32_t done = 0;
        uint32_t t0 = HAL_GetTick();

        if (g_usb_msc_prev_tick != 0xFFFFFFFFu &&
            (t0 - g_usb_msc_prev_tick) > 250u)
        {
            FM_Log("UsbMsc: gap %lums bein-e READ10-ha (ghabl-e lba=%lu)\n",
                   (unsigned long)(t0 - g_usb_msc_prev_tick),
                   (unsigned long)lba);
        }
        g_usb_msc_prev_tick = t0;

        while (done < (uint32_t)block_count)
        {
            uint32_t n = (uint32_t)block_count - done;
            size_t bytes;
            FM_Result r;

            if (n > (uint32_t)USB_MSC_BOUNCE_SECTORS)
            {
                n = (uint32_t)USB_MSC_BOUNCE_SECTORS;
            }
            bytes = (size_t)n * 512u;

            /* مرحله ۱: درایور -> بافر واسط خودمان، با «پروتکل کامل»
             * خودمان (نه تکیه بر درایور): Clean قبل از کیک (خطوط کثیفِ
             * احتمالی بیرون بریزد) -> DMA درایور -> Clean+Invalidate
             * بعد از آن => memcpy تضمینی از RAM واقعی می‌خواند. */
            FM_Cache_CleanForDma(g_usb_msc_bounce, bytes);
            r = USB_MSC_ReadBlocks(&g_usb_msc_ctx.msc, lba + done, n,
                                   g_usb_msc_bounce);
            if (r != FM_OK)
            {
                g_usb_msc_fail++;
                FM_Log("UsbMsc: !! READ fail lba=%lu n=%u (fail=%lu)\n",
                       (unsigned long)(lba + done), (unsigned)n,
                       (unsigned long)g_usb_msc_fail);
                return USB_MSC_FAIL;
            }
            FM_Cache_CleanForDma(g_usb_msc_bounce, bytes);
            FM_Cache_InvalidateAfterDma(g_usb_msc_bounce, bytes);

            if (lba + done == 0u && g_usb_msc_rd < 9u)
            {
                uint8_t s1a = g_usb_msc_bounce[510], s1b = g_usb_msc_bounce[511];

                if (s1a == 0x55u && s1b == 0xAAu)
                {
                    FM_Log("UsbMsc: lba0 OK (sig 55 AA, rd=%lu)\n",
                           (unsigned long)g_usb_msc_rd);
                }
                else
                {
                    /* یک Invalidate صریح دیگر + نگاه دوباره: اگر این بار
                     * 55AA آمد یعنی داده در RAM بود و خطوط کش کهنه بودند؛
                     * اگر باز صفر بود یعنی DMA درایور واقعا ننوشته
                     * (مسیر ISR/SDMMC) - لاگ را بفرست تا سراغش برویم. */
                    SCB_InvalidateDCache_by_Addr(
                        (uint32_t *)(uintptr_t)g_usb_msc_bounce,
                        (int32_t)bytes);
                    __DSB();
                    if (g_usb_msc_bounce[510] == 0x55u &&
                        g_usb_msc_bounce[511] == 0xAAu)
                    {
                        FM_Log("%s\n", "UsbMsc: lba0 ba invalide doomm salam "
                               "shod (cache line - protocol fixed it)");
                    }
                    else
                    {
                        UsbMsc_Lba0Probe();
                    }
                }
            }

            /* مرحله ۲: کپی CPU به بافر کیوب + Clean => هستهٔ USB با DMA
             * از RAM واقعی می‌خواند. مستقل از کش/MPU/تراز کیوب. */
            memcpy(buffer + (size_t)done * 512u, g_usb_msc_bounce, bytes);
            FM_Cache_CleanForDma(buffer + (size_t)done * 512u, bytes);

            done += n;
            g_usb_msc_rd += n;
            if ((g_usb_msc_rd % 128u) < (uint32_t)n)
            {
                FM_Log("UsbMsc: rd=%lu fail=%lu (lba=%lu) t=%lums\n",
                       (unsigned long)g_usb_msc_rd,
                       (unsigned long)g_usb_msc_fail,
                       (unsigned long)(lba + done),
                       (unsigned long)(HAL_GetTick() - t0));
            }
        }
        if ((HAL_GetTick() - t0) > 2000u)
        {
            /* یک درخواست خواندن که بیش از ۲ ثانیه کشید = کارت مکث/گیر
             * کرده؛ دقیقاً همین لحظه‌هاست که اکسپلورر قفل می‌زند. */
            FM_Log("UsbMsc: !! RD SLOW n=%u lba=%lu ms=%lu\n",
                   (unsigned)block_count, (unsigned long)lba,
                   (unsigned long)(HAL_GetTick() - t0));
        }
    }
    return USB_MSC_OK;
}

static inline int8_t UsbMsc_StorageWrite(uint8_t lun, const uint8_t *buffer,
                                      uint32_t lba, uint16_t block_count)
{
    FM_Result r;

    (void)lun;
    if (!g_usb_msc_ctx.pc_owns_media)
    {
        return USB_MSC_FAIL;
    }
    {
        uint32_t done = 0;
        uint32_t t0 = HAL_GetTick();

        while (done < (uint32_t)block_count)
        {
            uint32_t n = (uint32_t)block_count - done;
            size_t bytes;

            /* One window of the bounce buffer per pass, like the read
             * path.  Without this limit a request of more than
             * USB_MSC_BOUNCE_SECTORS blocks copied block_count * 512
             * bytes into the 4 KB buffer (the Cube stack hands over 1
             * block per call with MSC_MEDIA_PACKET 512, 64 with 32768). */
            if (n > (uint32_t)USB_MSC_BOUNCE_SECTORS)
            {
                n = (uint32_t)USB_MSC_BOUNCE_SECTORS;
            }
            bytes = (size_t)n * 512u;

            /* هستهٔ USB تازه با DMA در بافر کیوب نوشته؛ CPU باید دادهٔ
             * تازهٔ RAM را ببیند: Clean (خطوط کثیفِ همسایه بیرون بریزد)
             * بعد Invalidate. سپس کپی CPU به بافر واسط خودمان و Clean
             * آن برای DMA خواندنِ درایور. */
            FM_Cache_CleanForDma(buffer + (size_t)done * 512u, bytes);
            FM_Cache_InvalidateAfterDma((void *)(uintptr_t)(buffer +
                                        (size_t)done * 512u), bytes);
            memcpy(g_usb_msc_bounce, buffer + (size_t)done * 512u, bytes);
            FM_Cache_CleanForDma(g_usb_msc_bounce, bytes);

            r = USB_MSC_WriteBlocks(&g_usb_msc_ctx.msc, lba + done, n,
                                    g_usb_msc_bounce);
            g_usb_msc_wr += n;
            if (r != FM_OK)
            {
                g_usb_msc_fail++;
                FM_Log("UsbMsc: !! WRITE fail lba=%lu n=%u (fail=%lu)\n",
                       (unsigned long)(lba + done), (unsigned)n,
                       (unsigned long)g_usb_msc_fail);
                return USB_MSC_FAIL;
            }
            if ((g_usb_msc_wr % 32u) < (uint32_t)n)
            {
                FM_Log("UsbMsc: wr=%lu fail=%lu (lba=%lu) t=%lums\n",
                       (unsigned long)g_usb_msc_wr,
                       (unsigned long)g_usb_msc_fail,
                       (unsigned long)(lba + done),
                       (unsigned long)(HAL_GetTick() - t0));
            }
            done += n;
        }
        if ((HAL_GetTick() - t0) > 2000u)
        {
            FM_Log("UsbMsc: !! WR SLOW n=%u lba=%lu ms=%lu\n",
                   (unsigned)block_count, (unsigned long)lba,
                   (unsigned long)(HAL_GetTick() - t0));
        }
    }
    return USB_MSC_OK;
}

static inline int8_t UsbMsc_StorageGetMaxLun(void)
{
    return 0; /* one logical unit */
}

/* Ready-made ownership callbacks for qspi_w25q.h QSPI. Define
 * EXAMPLES_15... first (normally by including its header) before using these. */
#if defined(QSPI_W25Q_H_)
static inline FM_Result UsbMsc_ReleaseQspi(void *context)
{
    (void)context;
    return QspiW25q_UnmountForUsb();
}

static inline FM_Result UsbMsc_ReturnQspi(void *context)
{
    (void)context;
    return QspiW25q_RemountAfterUsb();
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
} UsbMsc_RamFat;

/* The RAM buffer must stay alive and its size must be a multiple of 512.
 * A blank buffer must be formatted once with UsbMsc_RamFatFormat(). */
static inline FM_Result UsbMsc_RamFatBegin(UsbMsc_RamFat *disk,
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
 * callable when UsbMsc_RamFatBegin completed initialization but its first
 * FS_Mount failed because the buffer did not contain a FAT volume yet. */
static inline FM_Result UsbMsc_RamFatFormat(UsbMsc_RamFat *disk)
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

static inline FM_Result UsbMsc_ReleaseRam(void *context)
{
    UsbMsc_RamFat *disk = (UsbMsc_RamFat *)context;
    FM_Result r;

    if (disk == NULL || !disk->mounted) return FM_ERR_NOT_MOUNTED;
    r = FS_Unmount(disk->mount_path);
    if (r == FM_OK) disk->mounted = FM_FALSE;
    return r;
}

static inline FM_Result UsbMsc_ReturnRam(void *context)
{
    UsbMsc_RamFat *disk = (UsbMsc_RamFat *)context;
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

static inline void UsbMsc_RamFatEnd(UsbMsc_RamFat *disk)
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
