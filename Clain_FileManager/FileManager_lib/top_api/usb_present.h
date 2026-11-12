/*
 * معماری فعلی: «فقط SD به PC» → از usb_sd_presenter.h
 * استفاده کن (سوییچ هوشمند + فایل دوطرفهٔ FAT). این فایل برای
 * نمایش چند-حافظه‌ای (QSPI/فلش SPI) به‌عنوان گزینهٔ پیشرفته می‌ماند.
 *
 * usb_present.h — نمایش SD / QSPI / SPI-فلش به PC با USB MSC
 * ======================================================================
 *
 * هدف: فلش W25Q64 (QSPI یا SPI معمولی) و SD کارت به‌عنوان «درایو» روی
 * کامپیوتر، تا بدون جداکردن قطعه عملیات فایل را روی سیستم انجام دهی.
 * این فایل لایهٔ نازکی روی usb_msc.h (پل STM32Cube USB Device MSC) است.
 *
 * ---------------------------------------------------------------------
 * تنظیمات CubeMX — چک‌لیست دقیق (H743)
 * ---------------------------------------------------------------------
 *   Connectivity -> USB_OTG_FS:
 *     Mode                : Device_Only
 *     Activate_VBUS       : بسته به سیم‌کشی برد (اگر PA9 به VBUS وصل است ON)
 *     پین‌ها              : PA11 = DM ، PA12 = DP (FS روی برد تو همین‌هاست)
 *
 *   Middleware -> USB_DEVICE:
 *     Class For FS IP     : Communication... نه! «Mass Storage Class»
 *     DEVICE_FS           : FS
 *     ( در usbd_storage_if.c تولیدی، همین ۷ خط را وصل کن — پایین فایل )
 *
 *   Clock Tree (مهم‌ترین تله):
 *     USB کلاک ۴۸.۰۰MHz می‌خواهد — دقیقاً. روی H743 معمولاً PLL3 را برای
 *     USB می‌گذارند: HSE/PLL3M * N / Q = 48MHz. بعد از generate، در
 *     SystemClock_Config چک کن PLL3Q = 48. کیوب خودش روی فرکانس‌های
 *     مجاز قفل می‌شود اگر «Solve» بزنی.
 *
 *   NVIC:
 *     OTG_FS_IRQn         : ON  (کیوب با فعال‌کردن کلاس اضافه می‌کند)
 *
 *   پارامترهای کلاس MSC:
 *     MSC_LUN             : 1 (تک-درایو؛ استراتژی این مثال)
 *     MaxLun در USBD_MSC_.c/h همان 1 می‌ماند. (چند-درایو همزمان = LUN
 *     متعدد؛ راه پیشرفته‌اش را پایین همین هدر نوشته‌ام.)
 *
 *   بعد از Generate:
 *     MX_USB_DEVICE_Init() را «بعد از» UsbPresent/UsbMsc آماده‌سازی صدا بزن —
 *     نه قبلش؛ پل باید اول storage را ببندد و callbacks را بدهد.
 *
 *   اتصال callbacks در Core/Src/usbd_storage_if.c (فایل تولیدی):
 *     #include "usb_present.h"
 *     int8_t STORAGE_Init_FS(uint8_t lun)
 *         { return UsbMsc_StorageInit(lun); }
 *     int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *n, uint16_t *s)
 *         { return UsbMsc_StorageGetCapacity(lun, n, s); }
 *     int8_t STORAGE_IsReady_FS(uint8_t lun)
 *         { return UsbMsc_StorageIsReady(lun); }
 *     int8_t STORAGE_IsWriteProtected_FS(uint8_t lun)
 *         { return UsbMsc_StorageIsWriteProtected(lun); }
 *     int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *b, uint32_t l, uint16_t c)
 *         { return UsbMsc_StorageRead(lun, b, l, c); }
 *     int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *b, uint32_t l, uint16_t c)
 *         { return UsbMsc_StorageWrite(lun, b, l, c); }
 *     int8_t STORAGE_GetMaxLun_FS(void)
 *         { return UsbMsc_StorageGetMaxLun(); }
 *
 * ---------------------------------------------------------------------
 * مهم: آنچه PC می‌بیند
 * ---------------------------------------------------------------------
 *   USB MSC «سکتور خام» می‌دهد. برای اینکه Windows آن را درایو ببیند
 *   باید حجم FAT داشته باشد:
 *     - SD کارت: از قبل FAT32 → مستقیم دیده می‌شود.
 *     - فلش سری: بار اول Windows می‌گوید «You need to format the disk»
 *       → با همان دیالوگ ویندوز فرمتش کن (FAT32/exFAT). از آن بعد
 *       فایل‌ها دوطرفه‌اند — همان فایل‌هایی که MCU هم می‌بیند، به شرطی
 *       که فلش روی MCU هم با FatFS فرمت شده باشد:
 *           #define QSPI_W25Q_USE_FATFS 1     (QSPI: درایو 1، VOLUMES>=2)
 *       اگر فلش با FMFS فرمت شده باشد، PC آن را RAW می‌بیند و فرمت
 *       ویندوز همهٔ محتوا را عوض می‌کند — پس یکی را انتخاب کن.
 *
 *   قانون طلایی (usb_msc.h): در هر لحظه فقط MCU یا PC مالک volume است.
 *   قبل از USB: مونت بسته شود (release). بعد از Eject و USBD_Stop:
 *   دوباره مونت (acquire) — هر دو اینجا آماده‌اند.
 *
 * ---------------------------------------------------------------------
 * استفاده در main
 * ---------------------------------------------------------------------
 *     #include "usb_present.h"
 *     ...
 *     بعد از راه‌اندازی QSPI (QspiW25q) و/یا SPI-فلش (SpiFlash):
 *     UsbPresent_ShowQspi();          <- یا PresentSpiFlash/PresentSd
 *     MX_USB_DEVICE_Init();            <- حالا PC درایو را می‌بیند
 *
 *     عوض‌کردن درایو ارائه‌شده (مثلاً با دکمه):
 *     USBD_Stop(&hUsbDeviceFS);
 *     UsbPresent_SwitchTo(USB_PRESENT_MEDIA_SD);
 *     MX_USB_DEVICE_Init();
 *
 *     روی Eject از PC:
 *     USBD_Stop(&hUsbDeviceFS);
 *     UsbMsc_End();                  <- MCU دوباره مالک و مونت می‌شود
 *
 * ---------------------------------------------------------------------
 * دستور العمل دقیق پروژه (هیچ ابهامی نماند)
 * ---------------------------------------------------------------------
 *  ۱) در main.c بالای main (USER CODE 0) — تعریف context فقط همین‌جا:
 *
 *         #define USB_MSC_DEFINE_CONTEXT
 *         #include "usb_msc.h"
 *         #include "usb_present.h"
 *
 *     و در usbd_storage_if.c (فقط همان ۷ خط، بدون define بالا):
 *         #include "usb_msc.h"
 *
 *     (اگر defines.h هم این هدرها را include می‌کند، این دو include را
 *      «قبل از» defines.h در main.c بگذار تا define فقط اینجا اثر کند.)
 *
 *  ۲) ترتیب روشن‌کردن در main:
 *         FM_SD_Hal_Attach(&hsd1);        <- اختیاری، برای LUN سوم
 *         ... SdCard_Begin / QspiFlash_Begin / SpiFlash begin ...
 *         UsbPresent_ShowQspi();         <- درایو اول: فلش QSPI
 *         MX_USB_DEVICE_Init();
 *
 *  ۳) جابه‌جایی بین حافظه‌ها (با دکمه/فرمان UART):
 *         USBD_Stop(&hUsbDeviceFS);
 *         UsbPresent_SwitchTo(USB_PRESENT_MEDIA_SPIFLASH);   <- یا SD/QSPI
 *         MX_USB_DEVICE_Init();
 *     PC برای یک لحظه درایو را «جدا» می‌بیند و درایو جدید را می‌آورد.
 *
 *  ۳ب) دو تلهٔ رایج در main.c واقعی (لاگ برد، دور ۳):
 *     - کیوب MX_USB_DEVICE_Init() را در «لیست پریفریل‌ها» (بالای USER
 *       CODE 2) می‌گذارد ⇒ USB قبل از هر storage ای enumerate می‌شود و
 *       PC وسیلهٔ خراب می‌بیند. آن خط را از لیست پریفریل‌ها بردار و
 *       دقیقاً بعد از UsbPresent_Show*() صدا بزن. (جایگزین مقاوم به
 *       regenerate کیوب: اولین خطِ USER CODE 2 → USBD_Stop(&hUsbDeviceFS);
 *       و انتهای راه‌اندازی → USBD_Start(&hUsbDeviceFS);)
 *     - QspiFlash_End() و QspiW25q_End() سکتور qspi را Unregister
 *       می‌کنند ⇒ UsbPresent_ShowQspi() بعد از آنها NOT_FOUND می‌دهد.
 *       یا در آن بوت end را صدا نزن، یا present را قبل از end انجام بده.
 *
 *  ۴) قواعد امنِ خواندن/نوشتن (پروتکل مالکیت):
 *     - حین اتصال USB، روی volumeِ فعلیِ LUN از MCU فقط Storage_Sync
 *       مجاز است؛ File_* ممنوع تا FAT خراب نشود.
 *     - بقیهٔ حافظه‌ها آزادند: مثلاً LUN=QSPI است، پخش از SD ادامه بده.
 *     - قبل از هر File_* روی volume، USB را خاموش کن:
 *           USBD_Stop(&hUsbDeviceFS); UsbMsc_End();
 *     - کاربر از PC: Eject → USBD_Stop → UsbMsc_End → مالکیت MCU.
 *     - فقط-خواندنی برای PC (بدون تعویض مالکیت، امن‌ترین):
 *           USB_MSC_FORCE_READONLY=1  +  PC فقط می‌خواند، MCU فقط
 *           می‌خواند — هیچ‌کس نمی‌نویسد.
 */

#ifndef FM_TOPAPI_USB_PRESENT_H
#define FM_TOPAPI_USB_PRESENT_H

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include "file_manager.h"
#include "fm_fatfs.h"
#include "fm_log.h"

#define printf FM_Log
#ifndef puts
#define puts(s) FM_Log("%s\\n", (s))
#endif

#include "../top_api/usb_msc.h"

#ifndef USB_PRESENT_QSPI_MOUNT
#define USB_PRESENT_QSPI_MOUNT "/qspi"
#endif
#ifndef USB_PRESENT_QSPI_NAME
#define USB_PRESENT_QSPI_NAME "qspi"
#endif
#ifndef SPI_FLASH_MOUNT_PATH
#define SPI_FLASH_MOUNT_PATH "/flash"
#endif
#ifndef SPI_FLASH_STORAGE_NAME
#define SPI_FLASH_STORAGE_NAME "flash"
#endif

typedef enum
{
    USB_PRESENT_MEDIA_NONE = 0,
    USB_PRESENT_MEDIA_QSPI,
    USB_PRESENT_MEDIA_SPIFLASH,
    USB_PRESENT_MEDIA_SD
} UsbPresent_Media;

static UsbPresent_Media g_usb_present_current = USB_PRESENT_MEDIA_NONE;

/* ------------------------------------------------------------------ */
/* release/acquire — قانون مالکیت برای هر حافظه                        */
/* ------------------------------------------------------------------ */

/* QSPI (qspi_w25q.h — FMFS روی /qspi) */
static FM_Result usb_present_release_qspi(void *ctx)
{
    (void)ctx;
    (void)FS_Unmount(USB_PRESENT_QSPI_MOUNT);
    return Storage_Sync(USB_PRESENT_QSPI_NAME);
}

static FM_Result usb_present_acquire_qspi(void *ctx)
{
    FM_FS fs;
    FM_Result r;

    (void)ctx;
    r = FMFS_Init(&fs);
    if (r != FM_OK) return r;
    return FS_Mount(USB_PRESENT_QSPI_MOUNT, &fs);
}

/* SPI-فلش (spi_flash_dma_port.h — FMFS روی /flash؛ توابعش خودش دارد) */
static FM_Result usb_present_release_spiflash(void *ctx)
{
    (void)ctx;
    return SpiFlash_ReleaseForUsb();
}

static FM_Result usb_present_acquire_spiflash(void *ctx)
{
    (void)ctx;
    return SpiFlash_ReturnFromUsb();
}

/*
 * SD (در پروژهٔ تو: FatFS درایو 0 کیوب که با SdCard_Begin روی "/"
 * مونت شده). اسم storage بسته به راه‌اندازی‌ات "sd" یا مشابه است؛
 * اگر مطمئن نیستی، همانی را بده که به FS_Mount اول وصل شد.
 */
static FM_Result usb_present_release_sd(void *ctx)
{
    (void)ctx;
    (void)FS_Unmount("/");
    return Storage_Sync("sd");
}

static FM_Result usb_present_acquire_sd(void *ctx)
{
    FM_FS fs;
    FM_Result r;

    (void)ctx;
    r = FatFS_Init(&fs, 0);
    if (r != FM_OK) return r;
    return FS_Mount("/", &fs);
}

/* ------------------------------------------------------------------ */
/* ارائه‌کردن یک حافظه — همان UsbMsc با سوئیچ آماده                      */
/* ------------------------------------------------------------------ */

static FM_Result UsbPresent_Show(FM_Storage *storage,
                                  FM_Result (*release)(void *),
                                  FM_Result (*acquire)(void *))
{
    FM_Result r;

    if (storage == NULL)
    {
        printf("UsbPresent: storage NULL - Present* ra BAD AZ begin-e hamun "
               "hafeze seda bezan (FM_SD_Hal_Attach? name='SD'?)\n");
        return FM_ERR_NOT_FOUND;
    }
    r = UsbMsc_Begin(storage, release, acquire, NULL);
    if (r != FM_OK)
    {
        printf("UsbPresent: USB begin failed (%d)\\n", (int)r);
        return r;
    }
    puts("UsbPresent: USB MSC ready - call MX_USB_DEVICE_Init() now");
    return FM_OK;
}

FM_Result UsbPresent_ShowQspi(void)
{
    extern FM_Storage *QspiW25q_GetStorage(void);

    g_usb_present_current = USB_PRESENT_MEDIA_QSPI;
    return UsbPresent_Show(QspiW25q_GetStorage(),
                            usb_present_release_qspi, usb_present_acquire_qspi);
}

FM_Result UsbPresent_ShowSpiFlash(void)
{
    extern FM_Storage *SpiFlash_GetStorage(void);

    g_usb_present_current = USB_PRESENT_MEDIA_SPIFLASH;
    return UsbPresent_Show(SpiFlash_GetStorage(),
                            usb_present_release_spiflash, usb_present_acquire_spiflash);
}

/*
 * پیش‌نیاز SD-over-USB: در main صدا زده شده باشد:
 *     FM_SD_Hal_Attach(&hsd1);     <- اسم storage: "SD" (بزرگ!)
 * (درایو diskio کیوب بهتنهایی storage ای به کتابخانه ثبت نمی‌کند.)
 */
FM_Result UsbPresent_ShowSd(void)
{
    FM_Storage *sd = NULL;

    g_usb_present_current = USB_PRESENT_MEDIA_SD;
    (void)Storage_Find("SD", &sd);       /* FM_SD_Hal_Attach: نام "SD" */
    return UsbPresent_Show(sd, usb_present_release_sd, usb_present_acquire_sd);
}

/*
 * عوض‌کردن درایوِ ارائه‌شده در حین اتصال USB (ساده و امن):
 *     USBD_Stop(&hUsbDeviceFS);
 *     UsbPresent_SwitchTo(USB_PRESENT_MEDIA_SD);   <- unmount/mount داخلی
 *     MX_USB_DEVICE_Init();                   <- دوباره روی باس
 */
void UsbPresent_SwitchTo(UsbPresent_Media media)
{
    if (g_usb_present_current == media) return;
    g_usb_present_current = media;

    switch (media)
    {
    case USB_PRESENT_MEDIA_QSPI:      (void)UsbPresent_ShowQspi();     break;
    case USB_PRESENT_MEDIA_SPIFLASH:  (void)UsbPresent_ShowSpiFlash(); break;
    case USB_PRESENT_MEDIA_SD:        (void)UsbPresent_ShowSd();       break;
    default:                       (void)UsbMsc_End();             break;
    }
}

#endif /* !FM_HOST_BUILD */
#endif /* FM_TOPAPI_USB_PRESENT_H */
