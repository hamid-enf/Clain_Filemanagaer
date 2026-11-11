/*
 * fm_log.h — خروجی متنی کتابخانه، مستقل از سخت‌افزار
 * ===================================================
 *
 * چرا این شکلی
 * ------------
 * کتابخانه لازم دارد پیام چاپ کند، ولی نباید بداند آن پیام کجا می‌رود:
 * UART؟ SWO؟ USB CDC؟ یک فایل؟ صفحهٔ نمایش؟ هیچ‌جا؟
 *
 * پس کتابخانه فقط یک «مقصد» می‌گیرد — یک تابع که بایت‌ها را تحویل
 * می‌گیرد — و بقیه‌اش را خودش انجام می‌دهد. هیچ‌جای کتابخانه به HAL،
 * به huart1، یا به سری خاصی از STM32 وابسته نیست.
 *
 * نتیجه: همین کد روی H7، روی F1، روی لینوکس، یا روی بردی که اصلاً
 * UART ندارد کار می‌کند.
 *
 * ---------------------------------------------------------------------
 * ساده‌ترین راه‌اندازی (سه خط)
 * ---------------------------------------------------------------------
 * در main.c، بعد از MX_USART1_UART_Init():
 *
 *     #include "fm_log.h"
 *     #include "fm_log_uart_hal.h"     // پل آمادهٔ UART
 *
 *     FM_Log_UseHalUart(&huart1);
 *
 * تمام. از این به بعد FM_Log و همهٔ پیام‌های کتابخانه روی همان UART
 * می‌روند.
 *
 * ---------------------------------------------------------------------
 * اگر مقصد دیگری می‌خواهید
 * ---------------------------------------------------------------------
 * یک تابع بنویسید که بایت‌ها را می‌گیرد و ثبتش کنید:
 *
 *     static void my_sink(void *ctx, const char *data, size_t len)
 *     {
 *         (void)ctx;
 *         CDC_Transmit_FS((uint8_t *)data, len);   // مثلاً USB
 *     }
 *
 *     FM_Log_SetSink(my_sink, NULL);
 *
 * برای SWO/ITM، برای نوشتن در فایل، برای نگه‌داشتن در یک رینگ‌بافر و
 * خواندن بعدی — همه با همین یک تابع.
 *
 * ---------------------------------------------------------------------
 * اگر هیچ خروجی نمی‌خواهید
 * ---------------------------------------------------------------------
 * هیچ کاری نکنید. تا وقتی مقصدی ثبت نشده، FM_Log بی‌صدا برمی‌گردد و
 * هیچ چیزی خراب نمی‌شود.
 *
 * و اگر می‌خواهید کدش هم اصلاً کامپایل نشود (صرفه‌جویی فلش):
 *
 *     #define FM_LOG_DISABLE 1     // در fm_board_config.h
 *
 * ---------------------------------------------------------------------
 * نکاتی که در پیاده‌سازی رعایت شده
 * ---------------------------------------------------------------------
 *  • هیچ malloc ای نیست. یک بافر ایستا به اندازهٔ FM_LOG_BUF_SIZE.
 *  • اگر پیام از بافر بلندتر باشد، بریده می‌شود نه اینکه سرریز کند.
 *  • قابل صدا زدن قبل از راه‌اندازی — بی‌صدا رد می‌شود.
 *
 * ---------------------------------------------------------------------
 * Rules for a sink (what a message costs, and what a sink may not do)
 * ---------------------------------------------------------------------
 *  - One message is one call of the sink: prefix, text and the line end
 *    arrive together, so lines of two tasks cannot mix inside a line.
 *
 *  - The sink runs in the context of whoever logged, and that code may
 *    hold a library lock (a storage driver reports an error while the
 *    filesystem above it holds the mount lock).  Whatever time the sink
 *    takes is added to that operation.  The ready-made UART bridge
 *    blocks until the bytes are out: at 115200 baud that is about 87 us
 *    per character, 4 ms for a 45-character line, and up to
 *    FM_LOG_UART_TIMEOUT_MS when nothing drains the UART.  For anything
 *    time-critical register a sink that copies into a ring buffer and
 *    send the bytes from the main loop or a low-priority task.
 *
 *  - A sink must not call back into the library when FM_USE_THREADS=1:
 *    the locks are not recursive and the call can wait for a lock its
 *    own caller holds.  To log into a file, queue the bytes in the sink
 *    and write them from your own task.
 *
 *  - With FM_USE_THREADS=0 a message that arrives while another one is
 *    being formatted or delivered is DROPPED and counted
 *    (FM_Log_Dropped()).  That covers a sink that logs (directly, or by
 *    calling a library function that reports an error), which would
 *    otherwise recurse until the stack is gone, and a message from an
 *    interrupt that lands in the middle of one from the main loop, which
 *    would otherwise overwrite the one format buffer.
 *
 *  - Formatting uses vsnprintf from the C library.  The library's own
 *    messages use only string and int / long conversions -- no floating
 *    point, no 64-bit ("%llu") ones -- so newlib-nano without float
 *    support is enough and no message allocates.
 */
#ifndef FM_LOG_H
#define FM_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* اندازهٔ بافر فرمت. پیام بلندتر از این بریده می‌شود.
 * روی بردهای کم‌حافظه (F1) ۲۵۶ کافی است. */
#ifndef FM_LOG_BUF_SIZE
#define FM_LOG_BUF_SIZE 512
#endif
#if FM_LOG_BUF_SIZE < 64
#error "FM_LOG_BUF_SIZE must be at least 64"
#endif

/*
 * مقصد خروجی. `data` تضمینی NUL-terminated نیست؛ فقط `len` بایت را
 * بنویسید.
 *
 * این تابع ممکن است از داخل وقفه صدا زده شود، پس سریع باشید.
 */
typedef void (*FM_LogSinkFn)(void *ctx, const char *data, size_t len);

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */

/* مقصد را ثبت کن. با NULL خروجی خاموش می‌شود. */
void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx);

/* مقصد فعلی (برای زنجیره‌کردن یا موقتاً خاموش‌کردن). */
FM_LogSinkFn FM_Log_GetSink(void *ctx_out);

/* آیا خروجی فعال است؟ */
int FM_Log_IsActive(void);

/* Messages thrown away because another message was in progress (see the
 * rules at the top).  Always 0 with FM_USE_THREADS=1.  A debugging aid:
 * a number that keeps growing means a sink that logs, or interrupts
 * that log while the main loop does. */
unsigned long FM_Log_Dropped(void);

/* ------------------------------------------------------------------ */
/* چاپ                                                                 */
/* ------------------------------------------------------------------ */

/* مثل printf. تعداد بایت نوشته‌شده، یا ۰ اگر مقصدی نیست. */
/* The compiler checks the arguments against the format string, as it does
 * for printf: "%s" given a number is a crash on the target, and nothing
 * else finds it before the message is printed for the first time. */
#if defined(__GNUC__) || defined(__clang__)
/* "__printf__", not "printf": the example headers do
 * "#define printf FM_Log" before this file may be included. */
#define FM_PRINTF_LIKE(fmt_index, first_arg) \
    __attribute__((format(__printf__, fmt_index, first_arg)))
#else
#define FM_PRINTF_LIKE(fmt_index, first_arg)
#endif

int FM_Log(const char *fmt, ...) FM_PRINTF_LIKE(1, 2);

/* نسخهٔ va_list — برای وقتی که خودتان wrapper می‌نویسید. */
int FM_Log_V(const char *fmt, va_list ap);

/*
 * One levelled message, "[<level>/<tag>] <text>\n", in ONE sink call.
 * This is what FM_ERR_LOG / FM_EVENT_LOG / FM_TRACE_LOG expand to
 * (fm_debug.h); `level` is 'E', 'I' or 'T'.  A message longer than the
 * buffer is cut, and still ends with the line break.
 */
int FM_Log_Tagged(int level, const char *tag, const char *fmt, ...)
    FM_PRINTF_LIKE(3, 4);

/* بایت خام، بدون فرمت. برای دادهٔ باینری یا رشتهٔ آماده. */
int FM_Log_Write(const char *buf, size_t len);

/* یک کاراکتر. */
int FM_Log_PutChar(int ch);

/* روی بیلد هاست (تست‌های PC): مقصد را stdout کن. روی میکرو وجود ندارد. */
#if defined(FM_HOST_BUILD) && FM_HOST_BUILD
void FM_Log_UseStdout(void);
#endif

/* ------------------------------------------------------------------ */
/* سازگاری با کد قدیمی                                                 */
/* ------------------------------------------------------------------ */
/*
 * نسخهٔ قبلی FM_Log_Init() داشت که UART را از پیش می‌دانست. حالا کاری
 * نمی‌کند و فقط برای اینکه کد موجود کامپایل شود مانده است.
 *
 * عمداً deprecated علامت خورده: اگر آن را صدا بزنید و مقصد لاگ را ثبت
 * نکرده باشید، خروجی *بی‌صدا* خالی می‌ماند — و پیدا کردن این روی
 * سخت‌افزار وقت‌گیر است. این هشدارِ کامپایلر همان‌جا می‌گوید چه کنید.
 *
 *     FM_Log_Init();                 <-- هشدار می‌دهد، کاری نمی‌کند
 *     FM_Log_UseHalUart(&huart1);    <-- این را بنویسید
 */
#if defined(__GNUC__) && !defined(FM_LOG_ALLOW_INIT)
__attribute__((deprecated("FM_Log_Init() دیگر کاری نمی\u200cکند. به\u200cجایش "
                          "FM_Log_UseHalUart(&huart1) یا FM_Log_SetSink() "
                          "را صدا بزنید، وگرنه خروجی لاگ خالی می\u200cماند.")))
#endif
void FM_Log_Init(void);

#ifndef FM_LOG_DISABLE
#define FM_LOG(...)        FM_Log(__VA_ARGS__)
#define FM_LOG_RAW(b, n)   FM_Log_Write((b), (n))
#define FM_LOG_CHAR(c)     FM_Log_PutChar(c)
#define LOG(...)           FM_Log(__VA_ARGS__)
#else
#define FM_LOG(...)        ((void)0)
#define FM_LOG_RAW(b, n)   ((void)0)
#define FM_LOG_CHAR(c)     ((void)0)
#define LOG(...)           ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* FM_LOG_H */
