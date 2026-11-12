/*
 * usb_sd_presenter.h — معماری جدید: «فقط SD کارت» به PC
 * =====================================================================
 *
 * تصمیم معماری (از این به بعد):
 *   - PC فقط و فقط SD کارت را می‌بیند (درایو FAT32 واقعی، فایل دوطرفه).
 *   - QSPI و فلش SPI هرگز به PC ارائه نمی‌شوند؛ تبادلشان با MCU است
 *     (FM_Transfer_Copy یا TransferDemo_*).
 *   - سوییچ هوشمند مالکیت: هر وقت خواستی SD بده به PC، هر وقت خواستی
 *     برگردان به MCU — unmount/mount و sync خودکار انجام می‌شود.
 *
 * جریان کار هدف:
 *   ۱) MCU داده را از QSPI/فلش به SD می‌آورد (داخل چیپ، سریع):
 *         UsbSd_CopyFromQspi("/qspi/video.avi", "/video.avi");
 *   ۲) SD می‌رود روی PC:
 *         UsbSd_ShowToPc();
 *      فایل را در ویندوز می‌بینی، کپی هم می‌توانی بکنی.
 *   ۳) در ویندوز Eject می‌کنی، بعد:
 *         UsbSd_TakeBack();
 *      SD دوباره مال MCU، مونت و آمادهٔ پخش.
 *
 * ---------------------------------------------------------------------
 * پیش‌نیازها (یک بار)
 * ---------------------------------------------------------------------
 *   - کارت SD در ویندوز FAT32 فرمت شده باشد (دو طرفه = PC و MCU هر دو
 *     همان FAT را می‌بینند؛ دیگر RAW و «فرمت می‌خواهی» در کار نیست).
 *   - CubeMX: همان تنظیمات USB سابق (USB_OTG_FS Device_Only + کلاس MSC
 *     + دقیقاً 48MHz برای USB + NVIC). MX_USB_DEVICE_Init را از لیست
 *     پریفریل‌ها بردار — خود این ماژول مدیریتش می‌کند.
 *   - usbd_storage_if.c: همان ۷ خط پل (مثل usb_present.h / usb_msc.h):
 *         #include "usb_msc.h"
 *         STORAGE_Init_FS            -> UsbMsc_StorageInit(lun)
 *         STORAGE_GetCapacity_FS     -> UsbMsc_StorageGetCapacity(...)
 *         STORAGE_IsReady_FS         -> UsbMsc_StorageIsReady(lun)
 *         STORAGE_IsWriteProtected_FS-> UsbMsc_StorageIsWriteProtected(lun)
 *         STORAGE_Read_FS            -> UsbMsc_StorageRead(...)
 *         STORAGE_Write_FS           -> UsbMsc_StorageWrite(...)
 *         STORAGE_GetMaxLun_FS       -> UsbMsc_StorageGetMaxLun()
 *
 * ---------------------------------------------------------------------
 * سیم‌کشی در main.c (فقط یک TU — همان main)
 * ---------------------------------------------------------------------
 *     در USER CODE 0 (قبل از include ی defines.h):
 *     #define USB_MSC_DEFINE_CONTEXT      <- فقط همین فایل
 *     #define USB_SD_CUBE_USB                <- کنترل USB خودکار
 *     #include "usb_msc.h"
 *     #include "usb_sd_presenter.h"
 *
 * ترتیب بوت:
 *     FM_SD_Hal_Attach(&hsd1);
 *     UsbSd_FatRoot();        <- ریشه = FAT32 کارت (برای دوطرفگی)
 *     ... FileDemo_* / SdDemo_* / SdMediaDemo_* ...   <- همه روی همان ریشه FAT
 *     ...
 *     SimpleVideo_Stop();       <- پخش که SD را نگه می‌دارد، ببند
 *     UsbSd_ShowToPc();       <- SD رفت روی PC
 *     (Eject در ویندوز)
 *     UsbSd_TakeBack();       <- SD برگشت به MCU
 *
 * اختیاری: USB_MSC_FORCE_READONLY=1 قبل از include ی 16 یعنی PC فقط
 * می‌خواند؛ هیچ‌کس نمی‌نویسد، امن‌ترین حالت برای نمایش.
 *
 * نکته: بدون USB_SD_CUBE_USB هم می‌شود (کنترل دستی): آن‌وقت بعد از
 * ShowToPc خودت MX_USB_DEVICE_Init() و قبل از TakeBack خودت
 * USBD_Stop(&hUsbDeviceFS) بزن. با USB_SD_CUBE_USB همه‌ش خودکار است.
 */

#ifndef FM_TOPAPI_USB_SD_PRESENTER_H
#define FM_TOPAPI_USB_SD_PRESENTER_H

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include "file_manager.h"
#include "fm_fatfs.h"
#include "fm_log.h"
#include "fm_transfer.h"

#define printf FM_Log


#include "../top_api/usb_msc.h"

/* کنترل مستقیم پشتهٔ USB کیوب - فقط روی برد */
#if defined(USB_SD_CUBE_USB)
#include "stm32h7xx_hal.h"
#include "usbd_core.h"      /* USBD_Start/USBD_Stop */
extern USBD_HandleTypeDef hUsbDeviceFS;
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;   /* usb_device.c ی کیوب */
extern void MX_USB_DEVICE_Init(void);

/* ------------------------------------------------------------------ */
/* USB در «حلقهٔ اصلی» به‌جای وقفه - فیکس ریشه‌ای درایو خالی با کش روشن  */
/* ------------------------------------------------------------------ */
/* لاگ برد (r8): خواندن SD از داخل context وقفهٔ USB با کش روشن، FM_OK
 * با محتوای صفر برمی‌گرداند؛ همان خواندن در thread اصلی سالم است (تمام
 * SdDemo_* سبز). پس کل پردازش USB را به حلقهٔ اصلی می‌آوریم:
 *   ۱) ShowToPc خودش وقفهٔ OTG_FS را خاموش می‌کند
 *   ۲) تو UsbSd_Poll() را در while(1) (و هر حلقهٔ منتظر) صدا بزن:
 *          while(1){ ...; UsbSd_Poll(); }
 *   و اگر حلقهٔ انتظار داری:
 *          while(sd_pc_mcu==1){ UsbSd_Poll(); HAL_Delay(1); }
 *      (HAL_Delay(1000) داخل انتظار یعنی USB یک‌ثانیه‌ای گرسنه است!) */
/* اسم CMSIS: OTG_FS_IRQn (روی هدر واقعی و استاب هر دو هست) */
FM_Result UsbSd_ThreadMode(void)
{
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    FM_Log("%s\n", ("UsbSd: USB dar hal-ye asli - UsbSd_Poll() ra dar "
                    "while(1) bezan"));
    return FM_OK;
}

static uint32_t usb_sd_hb_tick;
static uint32_t usb_sd_hb_rd, usb_sd_hb_wr, usb_sd_hb_fail;
static uint32_t usb_sd_hb_mark_rd = 0xFFFFFFFFu;

void UsbSd_Poll(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);

    /* تله‌متری سشن USB (دور ۱۱): هر ۵ ثانیه اگر خواندن/نوشتن PC در حال
     * پیشرفت است یک خط «زنده»؛ و در لحظهٔ از-کار-افتادن جریان، فقط یک
     * خط «متوقف شد» — تا در لاگ بعدی دقیقاً معلوم شود آخرین فعالیتِ
     * ویندوز کجا بوده (نقطهٔ گیرِ اکسپلورر). */
    {
        uint32_t now = HAL_GetTick();

        if ((uint32_t)(now - usb_sd_hb_tick) < 5000u)
        {
            return;
        }
        usb_sd_hb_tick = now;
        if (g_usb_msc_rd != usb_sd_hb_rd || g_usb_msc_wr != usb_sd_hb_wr ||
            g_usb_msc_fail != usb_sd_hb_fail)
        {
            FM_Log("UsbSd: usb active rd=%lu wr=%lu fail=%lu\n",
                   (unsigned long)g_usb_msc_rd, (unsigned long)g_usb_msc_wr,
                   (unsigned long)g_usb_msc_fail);
            usb_sd_hb_rd = g_usb_msc_rd;
            usb_sd_hb_wr = g_usb_msc_wr;
            usb_sd_hb_fail = g_usb_msc_fail;
        }
        else if ((usb_sd_hb_rd != 0u || usb_sd_hb_wr != 0u) &&
                 usb_sd_hb_mark_rd != usb_sd_hb_rd)
        {
            FM_Log("UsbSd: usb activity stopped (rd=%lu wr=%lu fail=%lu)\n",
                   (unsigned long)usb_sd_hb_rd, (unsigned long)usb_sd_hb_wr,
                   (unsigned long)usb_sd_hb_fail);
            usb_sd_hb_mark_rd = usb_sd_hb_rd;
        }
    }
}
#endif

static FM_FS   g_usb_sd_sd_fs;
static FM_Bool g_usb_sd_fat_root;   /* ریشه را خودم FAT کردم */
static FM_Bool g_usb_sd_usb_up;     /* MX یک‌بار انجام شده */

/* ------------------------------------------------------------------ */
/* ریشه = FAT32 کارت - یک بار در ابتدای بوت                            */
/* ------------------------------------------------------------------ */
/* اگر ریشه از قبل مونت باشد (مثلاً FMFS خودکار)، اول بسته می‌شود تا
 * هیچ‌وقت دو فایل‌سیستم روی یک volume نداشته باشیم. از این به بعد
 * فایل‌های MCU همان فایل‌هایی هستند که ویندوز می‌بیند. */
FM_Result UsbSd_FatRoot(void)
{
    FM_Result r;
    FM_Bool mounted = FM_FALSE;
    FM_FSInfo info;

    (void)FS_IsMounted("/", &mounted);
    if (mounted)
    {
        r = FS_Unmount("/");
        if (r != FM_OK)
        {
            printf("UsbSd: FS_Unmount(/) failed (%d)\n", (int)r);
            return r;
        }
    }

    r = FatFS_Init(&g_usb_sd_sd_fs, 0);      /* درایو 0 = SD */
    if (r != FM_OK)
    {
        printf("UsbSd: FatFS_Init(0) failed (%d)\n", (int)r);
        return r;
    }
    r = FS_Mount("/", &g_usb_sd_sd_fs);
    if (r != FM_OK)
    {
        printf("UsbSd: / mont nashod (%d) - kart ra dar PC FAT32 format "
               "kon\n", (int)r);
        return r;
    }
    g_usb_sd_fat_root = FM_TRUE;

    FM_Log("%s\n", ("UsbSd build: " USB_MSC_BUILD_ID));
    if (FS_GetInfo("/", &info) == FM_OK)
    {
        printf("UsbSd: root = %s, free %lu KB (PC-o MCU yek FAT mibinand)"
               "\n", info.type,
               (unsigned long)(info.free_space / 1024u));
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* سوییچ مالکیت - unmount/mount خودکار                                 */
/* ------------------------------------------------------------------ */

/* SD می‌رود دست PC: بستن مونت MCU + sync */
static FM_Result usb_sd_release(void *ctx)
{
    FM_Bool mounted = FM_FALSE;

    (void)ctx;
    (void)FS_IsMounted("/", &mounted);
    if (mounted)
    {
        (void)FS_Unmount("/");
    }
    return Storage_Sync("SD");
}

/* SD برمی‌گردد دست MCU: دوباره همان FAT را مونت کن */
static FM_Result usb_sd_acquire(void *ctx)
{
    (void)ctx;
    if (g_usb_sd_fat_root)
    {
        FM_Result r = FatFS_Init(&g_usb_sd_sd_fs, 0);

        if (r != FM_OK) return r;
        return FS_Mount("/", &g_usb_sd_sd_fs);
    }
    {
        FM_FS fs;
        FM_Result r = FMFS_Init(&fs);

        if (r != FM_OK) return r;
        return FS_Mount("/", &fs);
    }
}

/* SD -> PC (کاملاً خودکار با USB_SD_CUBE_USB) */
FM_Result UsbSd_ShowToPc(void)
{
    FM_Storage *sd = NULL;
    FM_Result r;

    if (UsbMsc_PcIsOwner())
    {
        FM_Log("%s\n", "UsbSd: SD alan dast PC ast - kari nakhastam");
        return FM_ERR_BUSY;
    }
    /* قبل از unmount، هر پخش‌کننده‌ای که فایل را روی SD باز دارد باید
     * بسته شود؛ وگرنه UsbSd_ShowToPc در میانهٔ پخش، mount را از زیر
     * دست FileManager می‌کشد و اجرا گیر/کرش می‌کند. اگر هدر این
     * پخش‌کننده‌ها include نشده باشد، این بلوک بی‌اثر است. */
#if defined(SIMPLE_VIDEO_H_)
    (void)SimpleVideo_Stop();
#endif
#if defined(SIMPLE_AUDIO_H_)
    (void)SimpleAudio_Stop();
#endif
#if defined(SIMPLE_IMAGE_H_)
    (void)SimpleImage_Stop();
#endif
    (void)Storage_Find("SD", &sd);
    if (sd == NULL)
    {
        FM_Log("%s\n", "UsbSd: storage 'SD' nabood - FM_SD_Hal_Attach(&hsd1)?");
        return FM_ERR_NOT_FOUND;
    }
    r = UsbMsc_Begin(sd, usb_sd_release, usb_sd_acquire, NULL);
    if (r != FM_OK)
    {
        printf("UsbSd: USB begin failed (%d)\n", (int)r);
        return r;
    }
#if defined(USB_SD_CUBE_USB)
    if (!g_usb_sd_usb_up)
    {
        MX_USB_DEVICE_Init();          /* فقط بار اول */
        g_usb_sd_usb_up = FM_TRUE;
    }
    else
    {
        (void)USBD_Start(&hUsbDeviceFS);
    }
    (void)UsbSd_ThreadMode();       /* وقفه خاموش - پول در while(1) */
    FM_Log("%s\n", "UsbSd: SD raft ro PC - bad az Eject: UsbSd_TakeBack()");
#else
    FM_Log("%s\n", "UsbSd: bridge amade - hala MX_USB_DEVICE_Init() bezan");
#endif
    return FM_OK;
}

/* PC -> MCU (بعد از Eject در ویندوز) */
FM_Result UsbSd_TakeBack(void)
{
    FM_Result r;

    if (!UsbMsc_PcIsOwner())
    {
        FM_Log("%s\n", "UsbSd: SD dast PC nist ke bargardonam");
        return FM_ERR_NOT_READY;
    }
#if defined(USB_SD_CUBE_USB)
    (void)USBD_Stop(&hUsbDeviceFS);
#else
    FM_Log("%s\n", "UsbSd: aval USBD_Stop(&hUsbDeviceFS) bezan (ya USB_SD_CUBE_USB)");
#endif
    r = UsbMsc_End();                /* sync + remount خودکار */
    printf("UsbSd: SD bargasht be MCU (%d)\n", (int)r);
    return r;
}

/* مالک کیست؟ 1 = PC ، 0 = MCU */
FM_Bool UsbSd_IsWithPc(void)
{
    return UsbMsc_PcIsOwner();
}

/* ------------------------------------------------------------------ */
/* آماده‌سازی داده برای PC - تبادل MCU-داخلی SD <-> QSPI                */
/* ------------------------------------------------------------------ */
/* قانون: این کار فقط وقتی مجاز است که SD دست MCU باشد. */
FM_Result UsbSd_CopyFromQspi(const char *src, const char *dst)
{
    if (UsbMsc_PcIsOwner())
    {
        FM_Log("%s\n", "UsbSd: SD dast PC ast - aval UsbSd_TakeBack()");
        return FM_ERR_BUSY;
    }
    return FM_Transfer_Copy(src, dst, NULL);
}

#endif /* !FM_HOST_BUILD */
#endif /* FM_TOPAPI_USB_SD_PRESENTER_H */
