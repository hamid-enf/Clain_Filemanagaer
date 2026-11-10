/*
 * fm_log_uart_hal.h — وصل‌کردن خروجی لاگ به UART از طریق HAL
 * ===========================================================
 *
 * این تنها فایل کتابخانه است که HAL را می‌شناسد. اگر UART نمی‌خواهید،
 * اصلاً include اش نکنید و بقیهٔ کتابخانه بدون هیچ وابستگی به STM32
 * کامپایل می‌شود.
 *
 * استفاده — یک خط در main.c بعد از MX_USARTx_UART_Init():
 *
 *     #include "fm_log_uart_hal.h"
 *     ...
 *     FM_Log_UseHalUart(&huart1);
 *
 * تمام. دیگر نه FM_Log_Init لازم است، نه extern کردن huart1، نه
 * ویرایش هیچ فایلی برای عوض کردن سری MCU.
 *
 * ---------------------------------------------------------------------
 * چرا header-only
 * ---------------------------------------------------------------------
 * چون به هدر HAL نیاز دارد و آن هدر بین سری‌ها فرق می‌کند، اگر این را
 * یک .c می‌کردیم باید در بیلد کتابخانه کامپایل می‌شد — و آن‌وقت
 * کتابخانه دیگر روی هاست یا روی یک سری دیگر ساخته نمی‌شد.
 *
 * به این شکل، فقط همان فایلی که include اش می‌کند (main.c شما) به HAL
 * وابسته است، که آنجا از قبل هست.
 *
 * ---------------------------------------------------------------------
 * حالت‌های دیگر
 * ---------------------------------------------------------------------
 * blocking (پیش‌فرض): تا وقتی بایت‌ها بیرون نروند برنمی‌گردد. ساده و
 *   قابل اتکا، ولی با باودریت پایین وقت می‌گیرد — هر ۱۰۰ کاراکتر در
 *   ۱۱۵۲۰۰ باود حدود ۹ میلی‌ثانیه.
 *
 * اگر لاگ سنگین دارید و نمی‌خواهید حلقهٔ اصلی کند شود، به‌جای این از
 * یک sink با رینگ‌بافر و DMA استفاده کنید (نمونه در انتهای فایل).
 */
#ifndef FM_LOG_UART_HAL_H
#define FM_LOG_UART_HAL_H

#include "fm_log.h"

/*
 * انتخاب هدر HAL بر اساس سری. اگر سری شما اینجا نیست، قبل از include
 * این را تعریف کنید:
 *
 *     #define FM_LOG_HAL_HEADER "stm32u5xx_hal.h"
 */
#ifndef FM_LOG_HAL_HEADER
#if   defined(STM32H7) || defined(STM32H743xx) || defined(STM32H750xx) || \
      defined(STM32H723xx) || defined(STM32H747xx) || defined(STM32H745xx)
#define FM_LOG_HAL_HEADER "stm32h7xx_hal.h"
#elif defined(STM32F7) || defined(STM32F746xx) || defined(STM32F767xx)
#define FM_LOG_HAL_HEADER "stm32f7xx_hal.h"
#elif defined(STM32F4) || defined(STM32F407xx) || defined(STM32F429xx) || \
      defined(STM32F411xE) || defined(STM32F446xx)
#define FM_LOG_HAL_HEADER "stm32f4xx_hal.h"
#elif defined(STM32F3)
#define FM_LOG_HAL_HEADER "stm32f3xx_hal.h"
#elif defined(STM32F2)
#define FM_LOG_HAL_HEADER "stm32f2xx_hal.h"
#elif defined(STM32F1) || defined(STM32F103xB) || defined(STM32F103xE)
#define FM_LOG_HAL_HEADER "stm32f1xx_hal.h"
#elif defined(STM32F0)
#define FM_LOG_HAL_HEADER "stm32f0xx_hal.h"
#elif defined(STM32G0)
#define FM_LOG_HAL_HEADER "stm32g0xx_hal.h"
#elif defined(STM32G4)
#define FM_LOG_HAL_HEADER "stm32g4xx_hal.h"
#elif defined(STM32L0)
#define FM_LOG_HAL_HEADER "stm32l0xx_hal.h"
#elif defined(STM32L1)
#define FM_LOG_HAL_HEADER "stm32l1xx_hal.h"
#elif defined(STM32L4)
#define FM_LOG_HAL_HEADER "stm32l4xx_hal.h"
#elif defined(STM32L5)
#define FM_LOG_HAL_HEADER "stm32l5xx_hal.h"
#elif defined(STM32U5)
#define FM_LOG_HAL_HEADER "stm32u5xx_hal.h"
#elif defined(STM32WB)
#define FM_LOG_HAL_HEADER "stm32wbxx_hal.h"
#else
#error "سری MCU تشخیص داده نشد — FM_LOG_HAL_HEADER را خودتان تعریف کنید"
#endif
#endif

#include FM_LOG_HAL_HEADER

/* مهلت ارسال (میلی‌ثانیه). اگر UART جواب ندهد، لاگ بی‌صدا رد می‌شود
 * و برنامه قفل نمی‌کند. */
#ifndef FM_LOG_UART_TIMEOUT_MS
#define FM_LOG_UART_TIMEOUT_MS 100u
#endif

#ifdef __cplusplus
extern "C" {
#endif

static void fm_log_hal_uart_sink(void *ctx, const char *data, size_t len)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;

    if (h == NULL || data == NULL || len == 0u)
    {
        return;
    }
    /* نتیجه عمداً نادیده گرفته می‌شود: یک لاگ ناموفق نباید مسیر
     * فراخوان را تغییر دهد. */
    (void)HAL_UART_Transmit(h, (uint8_t *)(void *)(size_t)data,
                            (uint16_t)len, FM_LOG_UART_TIMEOUT_MS);
}

/*
 * خروجی لاگ را به این UART وصل کن.
 *
 *     FM_Log_UseHalUart(&huart1);
 *
 * با NULL خروجی خاموش می‌شود.
 */
static inline void FM_Log_UseHalUart(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        FM_Log_SetSink(NULL, NULL);
        return;
    }
    FM_Log_SetSink(fm_log_hal_uart_sink, huart);
}

#ifdef __cplusplus
}
#endif

/*
 * ---------------------------------------------------------------------
 * اگر لاگ زیاد دارید: نسخهٔ غیرمسدودکننده
 * ---------------------------------------------------------------------
 * ارسال blocking با باودریت پایین می‌تواند بودجهٔ فریم ویدیو را بخورد.
 * الگوی زیر پیام‌ها را در رینگ‌بافر می‌ریزد و در حلقهٔ اصلی با DMA
 * می‌فرستد:
 *
 *   #define RING 4096
 *   static char     ring[RING];
 *   static volatile size_t rd, wr;
 *   static volatile uint8_t busy;
 *
 *   static void ring_sink(void *ctx, const char *d, size_t n)
 *   {
 *       (void)ctx;
 *       for (size_t i = 0; i < n; i++)
 *       {
 *           size_t next = (wr + 1u) % RING;
 *           if (next == rd) { break; }      // پر شد: بقیه را دور بریز
 *           ring[wr] = d[i];
 *           wr = next;
 *       }
 *   }
 *
 *   void log_poll(void)                     // در حلقهٔ اصلی
 *   {
 *       if (busy || rd == wr) { return; }
 *       size_t end = (wr > rd) ? wr : RING;
 *       busy = 1;
 *       HAL_UART_Transmit_DMA(&huart1, (uint8_t *)&ring[rd], end - rd);
 *   }
 *
 *   void HAL_UART_TxCpltCallback(UART_HandleTypeDef *h)
 *   {
 *       (void)h;
 *       rd = (rd + last_len) % RING;        // last_len را نگه دارید
 *       busy = 0;
 *   }
 *
 *   // در main:
 *   FM_Log_SetSink(ring_sink, NULL);
 *
 * نکتهٔ کش: اگر D-Cache روشن است، بافر DMA را قبل از ارسال Clean کنید
 * یا در ناحیهٔ non-cacheable بگذارید (examples/cache.h).
 * ---------------------------------------------------------------------
 */

#endif /* FM_LOG_UART_HAL_H */
