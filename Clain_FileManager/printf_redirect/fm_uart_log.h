///*
// * fm_uart_log.h
// *
// * یه لاگر حرفه‌ای برای STM32 که printf رو با HAL_UART جایگزین می‌کنه.
// * ویژگی‌ها:
// *   - 100% بدون heap (بافر stack)
// *   - Thread-safe با لاک HAL
// *   - بدون نیاز به newlib nano/standard
// *   - سازگار با Cortex-M0/M3/M4/M7
// *
// * طرز استفاده:
// *   1) این فایل + fm_uart_log.c رو به پروژه اضافه کنید
// *   2) syscalls.c و printf_redirect.c رو از build exclude کنید
// *   3) در main.c بنویسید:
// *        #include "fm_uart_log.h"
// *        FM_LOG_INIT();
// *        FM_LOG("Hello %d!\n", 42);
// *   4) یا اگه می‌خواهید printf کار کنه:
// *        #define printf FM_LOG
// *      (همه printfهای FileManager هم درست می‌شن)
// */
//
//#ifndef FM_UART_LOG_H
//#define FM_UART_LOG_H
//
//#include <stdarg.h>
//#include <stdint.h>
//#include <stddef.h>
//
//
///* ------------------------------------------------------------------ */
///* Configuration                                                       */
///* ------------------------------------------------------------------ */
//
///* اگه می‌خواهید بافر بزرگ‌تر یا کوچیک‌تر باشه، قبل از include این رو
// * تعریف کنید. حداقل 64. پیش‌فرض 256. */
//#ifndef FM_LOG_BUF_SIZE
//#define FM_LOG_BUF_SIZE 1024
//#endif
//
///* Timeout برای HAL_UART_Transmit (ms) */
//#ifndef FM_LOG_TIMEOUT_MS
//#define FM_LOG_TIMEOUT_MS 100
//#endif
//
///* اگه می‌خواهید لاگ کلاً غیرفعال بشه (برای release build) */
///* #define FM_LOG_DISABLE 1 */
//
///* ------------------------------------------------------------------ */
///* Public API                                                          */
///* ------------------------------------------------------------------ */
//
///* یک‌بار در main بعد از MX_USART1_UART_Init() صدا بزنید */
//void FM_Log_Init(void);
//
///* ارسال فرمت‌شده — مثل printf ولی بدون heap */
////int  FM_Log(const char *fmt, ...);
//
///* ارسال بافر خام */
//int  FM_Log_Write(const char *buf, size_t len);
//
///* ارسال یه کاراکتر */
//int  FM_Log_PutChar(int ch);
//
//#ifndef FM_LOG_DISABLE
//#define FM_LOG(...)         FM_Log(__VA_ARGS__)
//#define FM_LOG_RAW(buf, n)  FM_Log_Write((buf), (n))
//#define FM_LOG_CHAR(ch)     FM_Log_PutChar(ch)
//#else
//#define FM_LOG(...)         ((void)0)
//#define FM_LOG_RAW(buf, n)  ((void)0)
//#define FM_LOG_CHAR(ch)     ((void)0)
//#endif
//
///* اگه می‌خواهید printf سازگار باشه، در main.c بنویسید:
// *   #define printf FM_LOG
// *   #define puts(s)   FM_Log("%s\n", (s))
// */
//
///* alias کوتاه */
//#ifndef FM_LOG_DISABLE
//#define LOG(...) FM_Log(__VA_ARGS__)
//#else
//#define LOG(...) ((void)0)
//#endif
//
//
//
//#endif /* FM_UART_LOG_H */
