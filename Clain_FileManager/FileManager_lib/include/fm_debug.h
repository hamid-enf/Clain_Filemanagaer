/*
 * fm_debug.h — سیستم دیباگ چهار حالته
 * ====================================
 *
 * چهار حالت، در fm_board_config.h انتخاب می‌شود:
 *
 *   FM_DEBUG_OFF    (0) — هیچ چیز. کد لاگ اصلاً کامپایل نمی‌شود، پس نه
 *                         فلش می‌گیرد نه یک سیکل CPU.
 *   FM_DEBUG_LOW    (1) — فقط خطاها. چیزی که واقعاً غلط پیش رفته.
 *   FM_DEBUG_MEDIUM (2) — خطا + رویدادهای مهم: mount، باز/بسته شدن فایل،
 *                         شروع/پایان پخش، تعویض قطعه.
 *   FM_DEBUG_HIGH   (3) — همه چیز: ورود/خروج توابع، هر تخصیص حافظه، هر
 *                         تراکنش storage، هر فریم. برای وقتی که باید
 *                         بفهمید دقیقاً کجا خراب می‌شود.
 *
 * استفاده:
 *
 *     FM_ERR_LOG("mount failed: %d", r);      // در LOW و بالاتر
 *     FM_EVENT_LOG("playing %s", path);       // در MEDIUM و بالاتر
 *     FM_TRACE_LOG("frame %u decoded", n);    // فقط در HIGH
 *
 * هر پیام برچسب زیرسیستم می‌خورد تا در خروجی شلوغ گم نشود:
 *
 *     [E/video] decode failed: -17
 *     [I/audio] playing /music/a.mp3
 *     [T/alloc] 4096 bytes @ 0xC0001000
 *
 * ---------------------------------------------------------------------
 * فلسفهٔ «هیچ خطایی نباید برنامه را بخواباند»
 * ---------------------------------------------------------------------
 * این فایل دو ابزار برای همین دارد:
 *
 *   FM_CHECK(cond, ret, msg)  — اگر شرط برقرار نبود، لاگ بزن و مقدار
 *                               خطا را برگردان. جای assert که برنامه را
 *                               می‌کشد.
 *   FM_CHECK_PTR(p, ret)      — کوتاه‌شدهٔ همان برای بررسی NULL.
 *
 * قاعده در کل کتابخانه: هیچ‌جا assert نداریم، هیچ‌جا NULL را بدون بررسی
 * dereference نمی‌کنیم، و هر تابع عمومی ورودی‌اش را اعتبارسنجی می‌کند.
 */
#ifndef FM_DEBUG_H
#define FM_DEBUG_H

#include "fm_board_config.h"

/* سطوح — اعداد ثابت‌اند، در مقایسه‌ها استفاده می‌شوند. */
#define FM_DEBUG_OFF    0
#define FM_DEBUG_LOW    1
#define FM_DEBUG_MEDIUM 2
#define FM_DEBUG_HIGH   3

#ifndef FM_DEBUG_LEVEL
#define FM_DEBUG_LEVEL FM_DEBUG_LOW
#endif

/* خروجی لاگ. پیش‌فرض FM_Log است (UART). اگر جای دیگری می‌خواهید
 * بفرستید، FM_DEBUG_PRINTF را در fm_board_config.h عوض کنید. */
#ifndef FM_DEBUG_PRINTF
#if FM_DEBUG_LEVEL > FM_DEBUG_OFF
/* FM_Log routes to whatever sink the application registered (UART, SWO,
 * USB, a ring buffer, nothing).  The library never touches hardware
 * itself -- see fm_log.h. */
#include "fm_log.h"
#define FM_DEBUG_PRINTF FM_Log
#else
#define FM_DEBUG_PRINTF(...) ((void)0)
#endif
#endif

/* ------------------------------------------------------------------ */
/* سه ماکروی اصلی                                                      */
/* ------------------------------------------------------------------ */
/*
 * هر کدام یک برچسب زیرسیستم می‌گیرند. FM_DEBUG_TAG را بالای هر فایل .c
 * تعریف کنید:
 *
 *     #define FM_DEBUG_TAG "video"
 *     #include "fm_debug.h"
 */
#ifndef FM_DEBUG_TAG
#define FM_DEBUG_TAG "fm"
#endif

#if FM_DEBUG_LEVEL >= FM_DEBUG_LOW
#define FM_ERR_LOG(...)                                                    \
    do {                                                                   \
        FM_DEBUG_PRINTF("[E/%s] ", FM_DEBUG_TAG);                          \
        FM_DEBUG_PRINTF(__VA_ARGS__);                                      \
        FM_DEBUG_PRINTF("\n");                                             \
    } while (0)
#else
#define FM_ERR_LOG(...) ((void)0)
#endif

#if FM_DEBUG_LEVEL >= FM_DEBUG_MEDIUM
#define FM_EVENT_LOG(...)                                                  \
    do {                                                                   \
        FM_DEBUG_PRINTF("[I/%s] ", FM_DEBUG_TAG);                          \
        FM_DEBUG_PRINTF(__VA_ARGS__);                                      \
        FM_DEBUG_PRINTF("\n");                                             \
    } while (0)
#else
#define FM_EVENT_LOG(...) ((void)0)
#endif

#if FM_DEBUG_LEVEL >= FM_DEBUG_HIGH
#define FM_TRACE_LOG(...)                                                  \
    do {                                                                   \
        FM_DEBUG_PRINTF("[T/%s] ", FM_DEBUG_TAG);                          \
        FM_DEBUG_PRINTF(__VA_ARGS__);                                      \
        FM_DEBUG_PRINTF("\n");                                             \
    } while (0)
/* ورود/خروج تابع — فقط در بالاترین سطح. */
#define FM_TRACE_ENTER(fn)     FM_TRACE_LOG("-> %s", fn)
#define FM_TRACE_EXIT(fn, r)   FM_TRACE_LOG("<- %s = %d", fn, (int)(r))
#else
#define FM_TRACE_LOG(...)      ((void)0)
#define FM_TRACE_ENTER(fn)     ((void)0)
#define FM_TRACE_EXIT(fn, r)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* بررسی‌های بدون کرش                                                  */
/* ------------------------------------------------------------------ */
/*
 * جایگزین assert. هرگز برنامه را نمی‌خواباند: لاگ می‌زند و خطا برمی‌گرداند.
 *
 *     FM_CHECK(f != NULL, FM_ERR_INVALID_ARGUMENT, "file handle is NULL");
 */
#define FM_CHECK(cond, ret, ...)                                           \
    do {                                                                   \
        if (!(cond))                                                       \
        {                                                                  \
            FM_ERR_LOG(__VA_ARGS__);                                       \
            return (ret);                                                  \
        }                                                                  \
    } while (0)

/* بررسی NULL — رایج‌ترین حالت. */
#define FM_CHECK_PTR(p, ret)                                               \
    FM_CHECK((p) != NULL, (ret), "%s is NULL", #p)

/* بررسی بدون return (برای توابع void). */
#define FM_CHECK_VOID(cond, ...)                                           \
    do {                                                                   \
        if (!(cond))                                                       \
        {                                                                  \
            FM_ERR_LOG(__VA_ARGS__);                                       \
            return;                                                        \
        }                                                                  \
    } while (0)

/* نام سطح فعلی، برای چاپ در راه‌اندازی. */
static inline const char *FM_Debug_LevelName(void)
{
#if FM_DEBUG_LEVEL == FM_DEBUG_OFF
    return "off";
#elif FM_DEBUG_LEVEL == FM_DEBUG_LOW
    return "low";
#elif FM_DEBUG_LEVEL == FM_DEBUG_MEDIUM
    return "medium";
#else
    return "high";
#endif
}

#endif /* FM_DEBUG_H */
