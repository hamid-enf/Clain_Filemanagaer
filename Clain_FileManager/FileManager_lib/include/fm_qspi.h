/*
 * fm_qspi.h — فلش NOR سریال، مستقل از مدل قطعه
 * =============================================
 *
 * شما فقط مدل را اعلام می‌کنید:
 *
 *     #define FM_QSPI_CHIP  FM_QSPI_CHIP_W25Q256
 *
 * و کتابخانه خودش می‌داند این قطعه چند مگابایت است، سکتورش چند بایت
 * است، صفحه‌اش چقدر است، آدرس ۳ بایتی می‌خواهد یا ۴ بایتی، و چه
 * دستوراتی را می‌فهمد.
 *
 * اگر قطعه‌تان در جدول نیست، دو راه دارید:
 *   ۱) FM_QSPI_CHIP_CUSTOM را انتخاب کنید و مشخصاتش را خودتان بدهید.
 *   ۲) FM_QSPI_CHIP_AUTO بگذارید تا از روی JEDEC ID شناسایی شود.
 *
 * ---------------------------------------------------------------------
 * چیزهایی که خودکار رعایت می‌شود (و اگر دستی بنویسید معمولاً یادتان می‌رود)
 * ---------------------------------------------------------------------
 *  • فلش NOR فقط ۱ را به ۰ می‌کند. برای تغییر یک بایت باید کل سکتور
 *    خوانده، در RAM عوض، سکتور پاک، و دوباره نوشته شود. این لایه خودش
 *    انجامش می‌دهد (read-modify-erase-write).
 *  • نوشتن نمی‌تواند از مرز صفحه (معمولاً ۲۵۶ بایت) رد شود — وگرنه داده
 *    به اول همان صفحه می‌پیچد و خراب می‌شود. اینجا تکه‌تکه می‌شود.
 *  • قبل از هر نوشتن/پاک‌کردن باید Write Enable فرستاده شود.
 *  • بعد از هر نوشتن/پاک‌کردن باید منتظر ماند تا بیت busy پاک شود، با
 *    یک timeout که اگر قطعه جواب نداد برنامه قفل نکند.
 *  • قطعات بالای ۱۶ مگابایت آدرس ۴ بایتی می‌خواهند.
 *
 * ---------------------------------------------------------------------
 * چطور به سخت‌افزار وصل می‌شود
 * ---------------------------------------------------------------------
 * این فایل هیچ چیزی از HAL نمی‌داند. شما چهار تابع کوچک می‌دهید که
 * دستور را روی سیم می‌فرستند (FM_QspiIo) و بقیه‌اش اینجاست. برای
 * STM32 یک پیاده‌سازی آماده در examples/qspi_driver.h هست.
 */
#ifndef FM_QSPI_H
#define FM_QSPI_H

#include "fm_types.h"
#include "fm_storage.h"

FM_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* قطعات شناخته‌شده                                                     */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_QSPI_CHIP_AUTO = 0,    /* از روی JEDEC ID تشخیص بده        */
    FM_QSPI_CHIP_CUSTOM,      /* مشخصات را خودم می‌دهم             */

    /* Winbond */
    FM_QSPI_CHIP_W25Q16,      /*   2 MB */
    FM_QSPI_CHIP_W25Q32,      /*   4 MB */
    FM_QSPI_CHIP_W25Q64,      /*   8 MB */
    FM_QSPI_CHIP_W25Q128,     /*  16 MB */
    FM_QSPI_CHIP_W25Q256,     /*  32 MB — آدرس ۴ بایتی */
    FM_QSPI_CHIP_W25Q512,     /*  64 MB — آدرس ۴ بایتی */

    /* Micron / ST */
    FM_QSPI_CHIP_N25Q128,     /*  16 MB */
    FM_QSPI_CHIP_N25Q256,     /*  32 MB */

    /* Macronix */
    FM_QSPI_CHIP_MX25L128,    /*  16 MB */
    FM_QSPI_CHIP_MX25L256,    /*  32 MB */

    /* ISSI */
    FM_QSPI_CHIP_IS25LP128,   /*  16 MB */

    /* Adesto / Dialog */
    FM_QSPI_CHIP_AT25SF128,   /*  16 MB */

    FM_QSPI_CHIP_COUNT
} FM_QspiChip;

/* مشخصات یک قطعه. برای قطعات جدول‌شده خودکار پر می‌شود. */
typedef struct
{
    const char *name;
    uint32_t jedec_id;        /* manufacturer<<16 | type<<8 | capacity */
    uint64_t size_bytes;
    uint32_t sector_size;     /* کوچک‌ترین واحد پاک‌شدنی، معمولاً 4096  */
    uint32_t page_size;       /* بزرگ‌ترین نوشتن پیوسته، معمولاً 256    */
    uint32_t block_size;      /* پاک‌کردن بلوکی، معمولاً 65536          */
    uint8_t  addr_bytes;      /* 3 یا 4                                */
    uint32_t erase_sector_ms; /* بیشترین زمان پاک‌کردن یک سکتور        */
    uint32_t erase_chip_ms;   /* بیشترین زمان پاک‌کردن کل قطعه         */
    uint32_t page_program_ms; /* بیشترین زمان نوشتن یک صفحه            */
} FM_QspiChipInfo;

/* مشخصات یک قطعهٔ شناخته‌شده را بردار (برای CUSTOM هم می‌شود پرش کرد). */
FM_Result FM_Qspi_GetChipInfo(FM_QspiChip chip, FM_QspiChipInfo *out);

/* از روی JEDEC ID مدل را پیدا کن. */
FM_Result FM_Qspi_IdentifyById(uint32_t jedec_id, FM_QspiChipInfo *out);

/* ------------------------------------------------------------------ */
/* لایهٔ سخت‌افزار — این چهار تابع را شما می‌دهید                        */
/* ------------------------------------------------------------------ */
/*
 * فقط انتقال بایت‌ها روی سیم. هیچ منطق فلشی اینجا لازم نیست.
 */
typedef struct
{
    /* یک دستور بدون داده (مثل Write Enable, Chip Erase). */
    FM_Result (*cmd)(void *ctx, uint8_t opcode);

    /* دستور + آدرس، بدون داده (مثل Sector Erase). */
    FM_Result (*cmd_addr)(void *ctx, uint8_t opcode, uint32_t addr,
                          uint8_t addr_bytes);

    /* دستور (+آدرس اختیاری) و خواندن n بایت.
     * اگر addr_bytes صفر باشد یعنی بدون آدرس (مثل Read JEDEC ID). */
    FM_Result (*read)(void *ctx, uint8_t opcode, uint32_t addr,
                      uint8_t addr_bytes, uint8_t dummy_cycles,
                      void *buf, uint32_t len);

    /* دستور + آدرس و نوشتن n بایت (حداکثر یک صفحه). */
    FM_Result (*write)(void *ctx, uint8_t opcode, uint32_t addr,
                       uint8_t addr_bytes, const void *buf, uint32_t len);

    /* میلی‌ثانیه بخواب — برای انتظار busy. می‌تواند NULL باشد. */
    void (*delay_ms)(void *ctx, uint32_t ms);

    /* ---------------- حالت memory-mapped (اختیاری) ----------------
     *
     * در این حالت کنترلر QSPI فلش را روی فضای آدرس CPU نگاشت می‌کند و
     * خواندن تبدیل می‌شود به یک دسترسی حافظه‌ای ساده — بدون فرستادن
     * دستور، بدون سربار. برای خواندن‌های پیاپی (پخش ویدیو از فلش،
     * اجرای کد، خواندن فونت) خیلی سریع‌تر است.
     *
     * قیمتش: تا وقتی فعال است فلش فقط خواندنی است. هیچ دستور نوشتن،
     * پاک‌کردن یا حتی خواندن رجیستر وضعیت کار نمی‌کند. برای نوشتن باید
     * اول از این حالت خارج شد.
     *
     * این لایه خودش مدیریتش می‌کند: موقع خواندن وارد می‌شود، و به‌محض
     * اینکه نوشتن یا پاک‌کردن لازم شود خارج می‌شود. شما فقط این دو
     * تابع و آدرس پایه را می‌دهید.
     *
     * اگر NULL بگذارید، همه‌چیز از راه دستورات معمولی انجام می‌شود.
     */
    FM_Result (*mmap_enter)(void *ctx);
    FM_Result (*mmap_exit)(void *ctx);
    /* آدرسی که فلش در آن دیده می‌شود (مثلاً 0x90000000). */
    const volatile uint8_t *mmap_base;

    void *ctx;
} FM_QspiIo;

/* ------------------------------------------------------------------ */
/* دستگاه فلش                                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_QspiIo io;
    FM_QspiChipInfo info;
    FM_Bool ready;

    /* بافر read-modify-write برای نوشتن‌های کوچک‌تر از یک سکتور.
     * کاربر می‌دهد تا این لایه چیزی allocate نکند. */
    uint8_t *sector_buf;
    uint32_t sector_buf_size;

    /* آمار — برای دیباگ و سنجش فرسودگی. */
    uint32_t stat_erases;
    uint32_t stat_erases_skipped; /* سکتورهای خالی که پاک نشدند (بهینه) */
    uint32_t stat_writes;
    uint32_t stat_reads;
    uint32_t stat_mmap_reads;   /* خواندن‌هایی که از راه نگاشت رفتند */
    uint32_t stat_mmap_switch;  /* چند بار حالت عوض شد               */

    /* وضعیت فعلی حالت نگاشت. مستقیم دستش نزنید. */
    FM_Bool mmap_active;
    /* اجازهٔ استفاده از حالت نگاشت (پیش‌فرض: بله اگر io داشته باشد). */
    FM_Bool mmap_allowed;
} FM_QspiDevice;

/*
 * راه‌اندازی.
 *
 * chip:        مدل، یا FM_QSPI_CHIP_AUTO برای تشخیص خودکار.
 * sector_buf:  بافری به اندازهٔ یک سکتور (معمولاً ۴ کیلوبایت). برای
 *              نوشتنِ جزئی روی سکتوری که دادهٔ دیگری هم دارد لازم است
 *              (خواندن، پاک‌کردن، بازنویسی). بدون آن (NULL) این‌ها کار
 *              می‌کنند: نوشتنِ یک سکتور کامل، و نوشتن روی بایت‌هایی که
 *              هنوز 0xFF هستند (مثل اضافه‌کردن به ته یک صفحه؛ LittleFS
 *              همین‌طور می‌نویسد). بقیه FM_ERR_UNSUPPORTED می‌دهد.
 *              قاعدهٔ کامل: FM_Qspi_Write.
 *
 * اگر قطعه جواب ندهد یا ID ناشناخته باشد، خطا برمی‌گرداند — کرش نمی‌کند.
 */
FM_Result FM_Qspi_Init(FM_QspiDevice *dev, const FM_QspiIo *io,
                       FM_QspiChip chip, uint8_t *sector_buf,
                       uint32_t sector_buf_size);

/* خواندن — هر آدرس، هر طول. */
FM_Result FM_Qspi_Read(FM_QspiDevice *dev, uint64_t addr, void *buf,
                       uint32_t len);

/* نوشتن — مرز صفحه و پاک‌کردن سکتور خودکار رعایت می‌شود.
 *
 * برای هر سکتوری که نوشته به آن می‌خورد، ارزان‌ترین راهِ درست انتخاب
 * می‌شود:
 *   ۱) بایت‌های مقصد همین حالا همان داده‌اند  → هیچ کاری (نه پاک، نه نوشتن)
 *   ۲) بایت‌های مقصد 0xFF هستند (یا برابر)    → فقط برنامه‌ریزی، بدون پاک‌کردن
 *   ۳) کل سکتور نوشته می‌شود                  → پاک‌کردن + نوشتن
 *   ۴) وگرنه                                   → خواندن سکتور در sector_buf،
 *                                                 پاک‌کردن، نوشتنِ دوباره
 *      (بدون sector_buf: FM_ERR_UNSUPPORTED، و چیزی عوض نمی‌شود)
 * قبلاً هر نوشتهٔ جزئی راه ۴ را می‌رفت: هر «append» چند ده بایتی یک
 * پاک‌کردن ۴ کیلوبایتی بود (فرسایش و ۴۵ تا ۴۰۰ میلی‌ثانیه)، و بدون
 * sector_buf اصلاً LittleFS روی QSPI سوار نمی‌شد. */
FM_Result FM_Qspi_Write(FM_QspiDevice *dev, uint64_t addr, const void *buf,
                        uint32_t len);

/*
 * پاک‌کردن. addr و len به سکتور گرد می‌شوند.
 *
 * بهینه‌سازی: سکتوری که از قبل خالی باشد (همهٔ بایت‌ها 0xFF) پاک
 * نمی‌شود — چون پاک‌کردنش دقیقاً همان حالت را می‌دهد، فقط فرسایش و
 * ۴۵ تا ۴۰۰ میلی‌ثانیه زمان اضافه دارد. خواندنِ چک سریع‌تر است و از
 * مسیر memory-mapped هم می‌گذرد. با FM_QSPI_ERASE_SKIP_BLANK=0 خاموش
 * می‌شود. تعداد پرش‌ها: dev->stat_erases_skipped.
 */
FM_Result FM_Qspi_Erase(FM_QspiDevice *dev, uint64_t addr, uint64_t len);

/* پاک‌کردن کل قطعه — چند ثانیه طول می‌کشد. */
FM_Result FM_Qspi_EraseChip(FM_QspiDevice *dev);

/* شروع chip erase بدون انتظار (Write Enable + دستور پاک‌سازی کل) —
 * برای رابط‌هایی که می‌خواهند خودشان با FM_Qspi_ReadStatus پیشرفت
 * را نشان بدهند. بعدش با FM_Qspi_ReadStatus تا پاک شدن WIP صبر کن. */
FM_Result FM_Qspi_EraseChipStart(FM_QspiDevice *dev);

/* خواندن Status-1 (خروج از mmap خودکار است). بیت 0 = WIP. */
FM_Result FM_Qspi_ReadStatus(FM_QspiDevice *dev, uint8_t *out_status);

/* منتظر بمان تا عملیات قبلی تمام شود. */
FM_Result FM_Qspi_WaitReady(FM_QspiDevice *dev, uint32_t timeout_ms);

/* خواندن JEDEC ID خام. */
FM_Result FM_Qspi_ReadId(FM_QspiDevice *dev, uint32_t *out_id);

/* ------------------------------------------------------------------ */
/* حالت memory-mapped                                                  */
/* ------------------------------------------------------------------ */
/*
 * سیاست ورود/خروج حالت نگاشت (خودکار — «هوشمند»):
 *
 *   وارد: قبل از هر خواندنی اگر امکان نگاشت باشد؛ و بعد از آن در حالت
 *         نگاشت «می‌مانیم» تا خواندن بعدی هم ارزان باشد (sticky).
 *   خارج: قبل از هر Write Enable (یعنی هر نوشتن و پاک‌کردن)، قبل از
 *         هر خواندن رجیستر وضعیت (WaitReady) و قبل از ReadId — چون
 *         در حالت نگاشت هیچ دستوری به قطعه نمی‌رسد. اگر خروج شکست
 *         بخورد خطا برگردانده می‌شود، چون نوشتنِ در حالت نگاشت بی‌صدا
 *         گم می‌شود.
 *   پس شمارش ورود/خروج در stat_mmap_switch دیده می‌شود؛ در کار عادی
 *         (فقط خواندن) یک بار وارد می‌شوید و همان‌جا می‌مانید.
 *
 * به‌طور عادی لازم نیست این‌ها را صدا بزنید: FM_Qspi_Read خودش وارد
 * حالت نگاشت می‌شود و FM_Qspi_Write/Erase خودشان خارج می‌شوند.
 *
 * ولی اگر می‌خواهید مستقیم از حافظه بخوانید (مثلاً memcpy از فلش، یا
 * دادن اشاره‌گر به یک دیکودر) این‌ها لازم می‌شوند:
 *
 *     const uint8_t *p;
 *     if (FM_Qspi_MapForRead(&dev, 0, &p) == FM_OK)
 *     {
 *         // p یک اشاره‌گر معمولی به محتوای فلش است
 *         memcpy(dst, p, len);
 *     }
 *
 * هشدار: اشاره‌گر فقط تا وقتی معتبر است که کسی چیزی ننویسد. اولین
 * نوشتن، حالت نگاشت را می‌بندد و اشاره‌گر بی‌اعتبار می‌شود.
 */

/* وارد حالت نگاشت شو و اشاره‌گر به آدرس داده‌شده را برگردان. */
FM_Result FM_Qspi_MapForRead(FM_QspiDevice *dev, uint64_t addr,
                             const uint8_t **out_ptr);

/* از حالت نگاشت خارج شو (تا نوشتن ممکن شود). */
FM_Result FM_Qspi_Unmap(FM_QspiDevice *dev);

/* آیا الان در حالت نگاشت هستیم؟ */
FM_Bool FM_Qspi_IsMapped(const FM_QspiDevice *dev);

/* حالت نگاشت را کلاً مجاز/غیرمجاز کن. اگر برنامه‌تان زیاد می‌نویسد،
 * غیرفعال‌کردنش از رفت‌وبرگشت مکرر جلوگیری می‌کند. */
FM_Result FM_Qspi_SetMappedReads(FM_QspiDevice *dev, FM_Bool allowed);

/* ------------------------------------------------------------------ */
/* اتصال به لایهٔ Storage                                              */
/* ------------------------------------------------------------------ */
/*
 * بعد از این، فلش مثل هر حافظهٔ دیگری است: فایل‌سیستم رویش mount
 * می‌شود و FM_Transfer_Copy با آن کار می‌کند.
 *
 *     static FM_Storage qspi_storage;
 *     FM_Qspi_RegisterStorage(&dev, &qspi_storage, "qspi");
 *     Storage_Mount("qspi");
 */
FM_Result FM_Qspi_RegisterStorage(FM_QspiDevice *dev, FM_Storage *storage,
                                  const char *name);

FM_END_DECLS

#endif /* FM_QSPI_H */
