/*
 * fm_transfer.h — کپی داده بین هر دو حافظه‌ای، بدون خرابی
 * =======================================================
 *
 * مسئله‌ای که حل می‌کند
 * ---------------------
 * شما SD کارت را مپ کرده‌اید، QSPI را هم مپ کرده‌اید، SDRAM هم دارید و
 * RAM داخلی هم. حالا می‌خواهید داده را بین این‌ها جابه‌جا کنید:
 *
 *     SD  → QSPI      (مثلاً یک ویدیو را روی فلش بریزید)
 *     QSPI → SD
 *     SD  → SDRAM     (بارگذاری برای پخش سریع)
 *     SDRAM → SD      (ذخیرهٔ چیزی که ساخته‌اید)
 *     QSPI → SDRAM ، RAM → هرجا ، ...
 *
 * همهٔ این‌ها با یک تابع انجام می‌شود:
 *
 *     FM_Transfer_Copy("/sd/movie.avi", "/qspi/movie.avi", NULL);
 *
 * چیزی که خودکار رعایت می‌شود
 * ---------------------------
 *  • فلش NOR (QSPI) قبل از نوشتن باید پاک شود — لایهٔ فایل‌سیستم این را
 *    انجام می‌دهد، ولی برای کپی خام (Raw) ما خودمان erase می‌کنیم.
 *  • انسجام کش: اگر بافر میانی در ناحیهٔ کش‌شونده باشد و طرف مقابل DMA،
 *    قبل/بعد از هر تکه Clean/Invalidate می‌شود.
 *  • تراز: بافر میانی همیشه ۳۲-بایت-تراز است.
 *  • اندازهٔ تکه از FM_TRANSFER_CHUNK در fm_board_config.h می‌آید.
 *  • بررسی صحت اختیاری با CRC32 — بعد از کپی، مبدأ و مقصد مقایسه می‌شوند.
 *
 * دو سطح API
 * ----------
 *  ۱) سطح فایل (توصیه‌شده): با مسیر کار می‌کند، فایل‌سیستم همه‌چیز را
 *     مدیریت می‌کند.  FM_Transfer_Copy()
 *  ۲) سطح خام: مستقیم روی FM_Storage، بدون فایل‌سیستم. برای وقتی که
 *     می‌خواهید یک ناحیهٔ خام را آینه کنید.  FM_Transfer_Raw()
 */
#ifndef FM_TRANSFER_H
#define FM_TRANSFER_H

#include "fm_types.h"
#include "fm_storage.h"

FM_BEGIN_DECLS

/* گزارش پیشرفت. هر بار که تکه‌ای منتقل شد صدا زده می‌شود.
 * اگر FM_FALSE برگردانید، انتقال لغو می‌شود (FM_ERR_CANCELLED). */
typedef FM_Bool (*FM_Transfer_ProgressFn)(uint64_t done, uint64_t total,
                                          void *user);

typedef struct
{
    /* بررسی صحت با CRC32 بعد از کپی. کمی کندتر ولی مطمئن.
     * پیش‌فرض (وقتی ساختار را صفر کنید): خاموش. */
    FM_Bool verify;

    /* اگر مقصد از قبل هست، بازنویسی نشود و FM_ERR_EXISTS برگردد.
     * پیش‌فرض (ساختار صفرشده) = بازنویسی می‌کند، هم‌رفتار با File_Copy. */
    FM_Bool no_overwrite;

    /* گزارش پیشرفت — می‌تواند NULL باشد. */
    FM_Transfer_ProgressFn progress;
    void *progress_user;
} FM_TransferOpts;

/* نتیجهٔ تفصیلی یک انتقال. */
typedef struct
{
    uint64_t bytes;        /* چند بایت منتقل شد            */
    uint32_t crc_source;   /* CRC32 مبدأ (اگر verify روشن) */
    uint32_t crc_dest;     /* CRC32 مقصد (اگر verify روشن) */
    FM_Bool  verified;     /* آیا واقعاً بررسی شد           */
} FM_TransferInfo;

/* ------------------------------------------------------------------ */
/* سطح فایل                                                            */
/* ------------------------------------------------------------------ */

/* کپی یک فایل از هر جا به هر جا.
 *
 * مبدأ و مقصد فقط مسیرند — اینکه پشتشان SD است یا QSPI یا SDRAM اصلاً
 * فرقی نمی‌کند، چون فایل‌سیستم انتزاعش کرده.
 *
 *     FM_Transfer_Copy("/sd/clip.avi", "/qspi/clip.avi", NULL);
 *
 * با گزینه‌ها:
 *
 *     FM_TransferOpts o = {0};
 *     o.verify = FM_TRUE;
 *     o.overwrite = FM_TRUE;
 *     FM_Transfer_Copy(src, dst, &o);
 *
 * opts می‌تواند NULL باشد (یعنی همه‌چیز پیش‌فرض).
 */
FM_Result FM_Transfer_Copy(const char *src_path, const char *dst_path,
                           const FM_TransferOpts *opts);

/* مثل بالا ولی با گزارش تفصیلی.
 *
 * مبدأ و مقصدِ یکسان (بعد از نرمال‌سازی مسیر) → FM_ERR_INVALID_ARGUMENT و
 * فایل دست‌نخورده می‌ماند. کپی و verify روی فایل موقتِ انحصاری انجام
 * می‌شوند؛ شکست پیش از rename مقصد قبلی را حفظ می‌کند. وضعیت پس از خطای
 * rename/sync وابسته به backend است و فایل موقت ممکن است باقی بماند؛
 * پیش از پاک‌سازی دستی آن را بررسی کنید. no_overwrite از انتشار بدون جایگزینی
 * استفاده می‌کند؛ adapter سفارشی بدون این عملیات FM_ERR_UNSUPPORTED می‌دهد.
 * در FMFS/LittleFS همهٔ دسترسی‌ها باید از قفل همان mount عبور کنند. */
FM_Result FM_Transfer_CopyEx(const char *src_path, const char *dst_path,
                             const FM_TransferOpts *opts,
                             FM_TransferInfo *out_info);

/* انتقال (کپی + حذف مبدأ در صورت موفقیت). */
FM_Result FM_Transfer_Move(const char *src_path, const char *dst_path,
                           const FM_TransferOpts *opts);

/* ------------------------------------------------------------------ */
/* بارگذاری/ذخیره در بافر حافظه                                        */
/* ------------------------------------------------------------------ */

/* خواندن کل یک فایل در بافری که خودتان داده‌اید.
 * مثلاً از SD بخوان و در SDRAM بگذار:
 *
 *     static uint8_t buf[1024*1024] __attribute__((section(".sdram_buffers")));
 *     size_t n;
 *     FM_Transfer_LoadFile("/sd/clip.avi", buf, sizeof buf, &n);
 *
 * اگر فایل از بافر بزرگ‌تر باشد FM_ERR_TOO_LARGE برمی‌گردد. */
FM_Result FM_Transfer_LoadFile(const char *path, void *dst, size_t dst_cap,
                               size_t *out_len);

/* عکس کار بالا: بافری را در فایل بنویس (هر جا که باشد). */
FM_Result FM_Transfer_SaveFile(const char *path, const void *src, size_t len);

/* ------------------------------------------------------------------ */
/* سطح خام (بدون فایل‌سیستم)                                           */
/* ------------------------------------------------------------------ */

/* کپی مستقیم بین دو storage، بدون فایل‌سیستم.
 *
 * برای فلش NOR، ناحیهٔ مقصد قبل از نوشتن پاک می‌شود.
 *
 *     FM_Transfer_Raw(sd, 0, qspi, 0, 1024*1024, NULL);
 *
 * قرارداد (قبل از اولین erase/write بررسی می‌شود؛ در صورت نقض هیچ بایتی
 * دست نمی‌خورد و FM_ERR_INVALID_ARGUMENT برمی‌گردد):
 *   • [src_off, src_off+bytes) و [dst_off, dst_off+bytes) باید داخل
 *     capacity همان storage باشند.
 *   • اگر مقصد erase دارد (erase_size != 0)، dst_off باید مضرب erase_size
 *     باشد. پاک‌کردن واحد‌به‌واحد انجام می‌شود و آخرین واحد کامل پاک
 *     می‌شود (حتی اگر bytes وسط آن تمام شود) — مثل هر ابزار پروگرام فلش.
 *   • در حین erase، progress با done = 0 صدا زده می‌شود تا بتوانید لغو
 *     کنید؛ درصد از شروع کپی بالا می‌رود.
 *
 * توابع این فایل یک بافر میانی مشترک دارند: اگر از داخل progress (یا از
 * ترد دیگر) دوباره FM_Transfer_* صدا بزنید FM_ERR_BUSY می‌گیرید.
 */
FM_Result FM_Transfer_Raw(FM_Storage *src, uint64_t src_off,
                          FM_Storage *dst, uint64_t dst_off,
                          uint64_t bytes, const FM_TransferOpts *opts);

/* مقایسهٔ دو ناحیه. FM_OK یعنی یکی هستند، FM_ERR_CORRUPTED یعنی فرق دارند.
 * storage بدون read → FM_ERR_UNSUPPORTED؛ بازهٔ خارج از capacity →
 * FM_ERR_INVALID_ARGUMENT. */
FM_Result FM_Transfer_RawVerify(FM_Storage *a, uint64_t a_off,
                                FM_Storage *b, uint64_t b_off,
                                uint64_t bytes);

FM_END_DECLS

#endif /* FM_TRANSFER_H */
