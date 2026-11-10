///*
// * fm_uart_log.c — پیاده‌سازی لاگر UART برای STM32
// *
// * این فایل printf رو با HAL_UART جایگزین می‌کنه، بدون نیاز به
// * newlib nano/standard و بدون heap allocation.
// *
// * استفاده:
// *   1) این فایل + fm_uart_log.h رو به پروژه اضافه کنید
// *   2) در main.c بعد از MX_USART1_UART_Init() بنویسید:
// *        FM_Log_Init();
// *   3) از FM_LOG یا FM_Log() استفاده کنید
// *   4) (اختیاری) برای استفاده از printf:
// *        #define printf FM_Log
// */
//
//#include "fm_uart_log.h"
//#include "stm32h7xx_hal.h"   /* ← اگه MCU دیگه‌ای دارید عوض کنید */
//#include <string.h>
//
///* UART هندل — از main.c شما */
//extern UART_HandleTypeDef huart1;
//
///* ------------------------------------------------------------------ */
///* Configuration                                                       */
///* ------------------------------------------------------------------ */
//
///* Thread-safety: اگه RTOS دارید (FreeRTOS)، 1 بذارید.
// * اگه bare-metal هستید، 0 بذارید (سریع‌تر). */
//#ifndef FM_LOG_USE_MUTEX
//#define FM_LOG_USE_MUTEX 0
//#endif
//
//#if FM_LOG_USE_MUTEX
//#include "cmsis_os2.h"   /* اگه CMSIS-RTOS2 دارید */
//static osMutexId_t s_log_mutex = NULL;
//#endif
//
///* ------------------------------------------------------------------ */
///* State                                                               */
///* ------------------------------------------------------------------ */
//
//static uint8_t s_log_buf[FM_LOG_BUF_SIZE];
//static uint8_t s_inited = 0;
//
//void FM_Log_Init(void)
//{
//    if (s_inited) return;
//    s_inited = 1;
//#if FM_LOG_USE_MUTEX
//    if (s_log_mutex == NULL)
//    {
//        const osMutexAttr_t attr = {
//            .name      = "FM_LOG",
//            .attr_bits = osMutexRecursive | osMutexPrioInherit,
//            .cb_mem    = NULL,
//            .cb_size   = 0U
//        };
//        s_log_mutex = osMutexNew(&attr);
//    }
//#endif
//}
//
///* ------------------------------------------------------------------ */
///* Low-level output                                                    */
///* ------------------------------------------------------------------ */
//
//int FM_Log_Write(const char *buf, size_t len)
//{
//    if (!s_inited || buf == NULL || len == 0)
//    {
//        return 0;
//    }
//
//#if FM_LOG_USE_MUTEX
//    if (s_log_mutex != NULL) (void)osMutexAcquire(s_log_mutex, osWaitForever);
//#endif
//
//    /* HAL_UART_Transmit مستقیم — blocking با timeout */
//    HAL_StatusTypeDef status = HAL_UART_Transmit(&huart1,
//                                                 (const uint8_t *)buf,
//                                                 (uint16_t)len,
//                                                 FM_LOG_TIMEOUT_MS);
//
//#if FM_LOG_USE_MUTEX
//    if (s_log_mutex != NULL) (void)osMutexRelease(s_log_mutex);
//#endif
//
//    return (status == HAL_OK) ? (int)len : -1;
//}
//
//int FM_Log_PutChar(int ch)
//{
//    uint8_t c = (uint8_t)(ch & 0xFF);
//    return FM_Log_Write((const char *)&c, 1);
//}
//
///* ------------------------------------------------------------------ */
///* vsnprintf — مثل قبل ولی با بافر داخلی                             */
///* ------------------------------------------------------------------ */
//
///* Forward declaration از libc — ما فقط صدا می‌زنیم، نمی‌سازیم.
// * vsnprintf در newlib موجوده و از heap استفاده نمی‌کنه (با -O2). */
//extern int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
//
//int FM_Log(const char *fmt, ...)
//{
//    va_list args;
//    int n;
//
//    if (!s_inited || fmt == NULL)
//    {
//        return 0;
//    }
//
//    va_start(args, fmt);
//    n = vsnprintf((char *)s_log_buf, sizeof(s_log_buf), fmt, args);
//    va_end(args);
//
//    if (n < 0)
//    {
//        return -1;  /* encoding error */
//    }
//    if (n >= (int)sizeof(s_log_buf))
//    {
//        n = (int)sizeof(s_log_buf) - 1;  /* truncated */
//    }
//
//    return FM_Log_Write((const char *)s_log_buf, (size_t)n);
//}
