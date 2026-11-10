/*
 * fm_port.h — انتخاب معماری/سری، و قابلیت‌هایی که از آن نتیجه می‌شود
 * ==================================================================
 *
 * هدف: هستهٔ کتابخانه به هیچ سری خاصی وابسته نباشد. شما در
 * fm_board_config.h فقط می‌گویید روی چه چیزی هستید:
 *
 *     #define FM_PORT FM_PORT_STM32F1
 *
 * و این فایل بقیه را نتیجه می‌گیرد: چه چیزی هست، چه چیزی نیست، و
 * پیش‌فرض‌های معقول برای آن سخت‌افزار چیست.
 *
 * ---------------------------------------------------------------------
 * چرا این کار لازم است
 * ---------------------------------------------------------------------
 * تا الان کتابخانه پیش‌فرض‌هایی داشت که فقط روی H7 معنا می‌دهند: بافر
 * ۲ مگابایتی تصویر، استخر ۸ مگابایتی، کش، JPEG سخت‌افزاری. روی یک F1 با
 * ۲۰ کیلوبایت RAM این‌ها یعنی لینک نشدن.
 *
 * با انتخاب پورت، همان کد روی F1 هم کامپایل می‌شود — فقط با قابلیت‌های
 * کمتر. مثلاً روی F1 معمولاً فقط کار با فایل می‌خواهید، نه ویدیو.
 *
 * ---------------------------------------------------------------------
 * جدول پورت‌ها
 * ---------------------------------------------------------------------
 *   پورت            RAM نمونه   کش   DMA   SDRAM  QSPI  پیشنهاد
 *   ------------    ---------   ---  ----  -----  ----  ------------------
 *   GENERIC          ؟          -    -     -      -     امن‌ترین پیش‌فرض
 *   HOST             زیاد       -    -     -      -     تست روی PC
 *   STM32F1          20-64K     -    ✓     -      -     فقط فایل
 *   STM32F4          128-192K   -    ✓     ✓      ✓     فایل + صدا
 *   STM32F7          256-512K   ✓    ✓     ✓      ✓     + تصویر
 *   STM32H7          1M         ✓    ✓     ✓      ✓     همه چیز
 *   STM32G0/G4       32-128K    -    ✓     -      ✓     فایل + EEPROM
 *   STM32L4          64-320K    -    ✓     -      ✓     کم‌مصرف
 *
 * اگر پورت شما اینجا نیست، GENERIC را بگذارید و قابلیت‌ها را دستی
 * تنظیم کنید — همه‌شان قابل override هستند.
 */
#ifndef FM_PORT_H
#define FM_PORT_H

/* شناسه‌های پورت */
#define FM_PORT_GENERIC   0
#define FM_PORT_HOST      1
#define FM_PORT_STM32F1   2
#define FM_PORT_STM32F4   3
#define FM_PORT_STM32F7   4
#define FM_PORT_STM32H7   5
#define FM_PORT_STM32G0   6
#define FM_PORT_STM32G4   7
#define FM_PORT_STM32L4   8

/* اگر کاربر چیزی نگفت، سعی کن خودت بفهمی. */
#ifndef FM_PORT
#if defined(STM32H7) || defined(STM32H743xx) || defined(STM32H750xx) || \
    defined(STM32H723xx) || defined(STM32H747xx)
#define FM_PORT FM_PORT_STM32H7
#elif defined(STM32F7) || defined(STM32F746xx) || defined(STM32F767xx)
#define FM_PORT FM_PORT_STM32F7
#elif defined(STM32F4) || defined(STM32F407xx) || defined(STM32F429xx)
#define FM_PORT FM_PORT_STM32F4
#elif defined(STM32F1) || defined(STM32F103xB) || defined(STM32F103xE)
#define FM_PORT FM_PORT_STM32F1
#elif defined(STM32G0)
#define FM_PORT FM_PORT_STM32G0
#elif defined(STM32G4)
#define FM_PORT FM_PORT_STM32G4
#elif defined(STM32L4)
#define FM_PORT FM_PORT_STM32L4
#elif defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#define FM_PORT FM_PORT_HOST
#else
#define FM_PORT FM_PORT_GENERIC
#endif
#endif

/* ------------------------------------------------------------------ */
/* قابلیت‌های سخت‌افزاری که از پورت نتیجه می‌شوند                       */
/* ------------------------------------------------------------------ */
/* هر کدام قابل override است: اگر بردتان استثناست، در
 * fm_board_config.h قبل از include این فایل تعریفش کنید. */

#if FM_PORT == FM_PORT_STM32H7
  #define FM_PORT_NAME          "STM32H7"
  #define FM_PORT_HAS_DCACHE    1
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     1
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   1
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          3      /* 3 = پرتوان */

#elif FM_PORT == FM_PORT_STM32F7
  #define FM_PORT_NAME          "STM32F7"
  #define FM_PORT_HAS_DCACHE    1
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     1
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          2

#elif FM_PORT == FM_PORT_STM32F4
  #define FM_PORT_NAME          "STM32F4"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     1
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          2

#elif FM_PORT == FM_PORT_STM32F1
  #define FM_PORT_NAME          "STM32F1"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      0
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          1      /* 1 = کم‌توان */

#elif FM_PORT == FM_PORT_STM32G0
  #define FM_PORT_NAME          "STM32G0"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          1

#elif FM_PORT == FM_PORT_STM32G4
  #define FM_PORT_NAME          "STM32G4"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          1

#elif FM_PORT == FM_PORT_STM32L4
  #define FM_PORT_NAME          "STM32L4"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       1
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      1
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       1
  #define FM_PORT_TIER          1

#elif FM_PORT == FM_PORT_HOST
  #define FM_PORT_NAME          "host"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       0
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      0
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       0
  #define FM_PORT_TIER          3

#else /* FM_PORT_GENERIC — هیچ فرضی نکن */
  #define FM_PORT_NAME          "generic"
  #define FM_PORT_HAS_DCACHE    0
  #define FM_PORT_HAS_DMA       0
  #define FM_PORT_HAS_SDRAM     0
  #define FM_PORT_HAS_QSPI      0
  #define FM_PORT_HAS_HW_JPEG   0
  #define FM_PORT_HAS_I2S       0
  #define FM_PORT_TIER          1
#endif

/* ------------------------------------------------------------------ */
/* پیش‌فرض اندازهٔ بافرها بر اساس توان برد                              */
/* ------------------------------------------------------------------ */
/*
 * این‌ها فقط پیش‌فرض‌اند. هر کدام را در fm_board_config.h می‌توانید
 * عوض کنید — آنجا اولویت دارد چون قبل از این فایل خوانده می‌شود.
 *
 * TIER 1 (F1/G0/L4): حافظه بسیار کم. فقط فایل.
 * TIER 2 (F4/F7):    حافظهٔ متوسط، معمولاً با SDRAM.
 * TIER 3 (H7/host):  همه چیز.
 */
#if FM_PORT_TIER == 1
  #define FM_PORT_DEF_TRANSFER_CHUNK   512u
  #define FM_PORT_DEF_IMAGE_MAX_W      160u
  #define FM_PORT_DEF_IMAGE_MAX_H      128u
  #define FM_PORT_DEF_IMAGE_FILE_MAX   (16u * 1024u)
  #define FM_PORT_DEF_VIDEO_MAX_W      0u    /* ویدیو خاموش */
  #define FM_PORT_DEF_VIDEO_MAX_H      0u
  #define FM_PORT_DEF_SDRAM_POOL       0u
#elif FM_PORT_TIER == 2
  #define FM_PORT_DEF_TRANSFER_CHUNK   2048u
  #define FM_PORT_DEF_IMAGE_MAX_W      480u
  #define FM_PORT_DEF_IMAGE_MAX_H      320u
  #define FM_PORT_DEF_IMAGE_FILE_MAX   (512u * 1024u)
  #define FM_PORT_DEF_VIDEO_MAX_W      320u
  #define FM_PORT_DEF_VIDEO_MAX_H      240u
  #define FM_PORT_DEF_SDRAM_POOL       (2u * 1024u * 1024u)
#else
  #define FM_PORT_DEF_TRANSFER_CHUNK   4096u
  #define FM_PORT_DEF_IMAGE_MAX_W      800u
  #define FM_PORT_DEF_IMAGE_MAX_H      480u
  #define FM_PORT_DEF_IMAGE_FILE_MAX   (2u * 1024u * 1024u)
  #define FM_PORT_DEF_VIDEO_MAX_W      320u
  #define FM_PORT_DEF_VIDEO_MAX_H      240u
  #define FM_PORT_DEF_SDRAM_POOL       (8u * 1024u * 1024u)
#endif

#endif /* FM_PORT_H */
