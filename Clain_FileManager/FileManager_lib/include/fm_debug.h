/*
 * fm_debug.h — سیستم دیباگ چهار حالته
 * ====================================
 *
 * چهار حالت، در fm_board_config.h انتخاب می‌شود:
 *
 *   FM_DEBUG_OFF    (0) — هیچ چیز. کد لاگ اصلاً کامپایل نمی‌شود، پس نه
 *                         فلش می‌گیرد نه یک سیکل CPU.
 *   FM_DEBUG_LOW    (1) — فقط خطاها. چیزی که واقعاً غلط پیش رفته.
 *   FM_DEBUG_MEDIUM (2) — خطا + رویدادهای مهم.
 *   FM_DEBUG_HIGH   (3) — خطا + رویداد + ردگیری ریز.
 *
 * What each level really prints (kept in step with the code; the test
 * suite checks the lines marked *):
 *
 *   LOW     an operation that failed for a reason the caller could not
 *           have expected: a NULL argument*, a mount or format that
 *           failed*, a close that could not flush, a storage driver
 *           error (QSPI, SPI flash, EEPROM, SD), a media file that could
 *           not be opened or parsed, a failed allocation with its size*.
 *           Nothing on a successful path*.  NOT printed: "file not
 *           found" on open or delete* -- that is an answer, not a fault
 *           (it is an event at MEDIUM).
 *   MEDIUM  + one line per life-cycle step: init, storage and filesystem
 *           mount / unmount / format, file open* / close / delete*,
 *           image loaded, audio / video opened, player track change,
 *           media-cache decisions, flash chip identified.
 *   HIGH    + every allocation and release*, every QSPI read / write /
 *           erase, raw-copy chunks, cache hits.  This level changes
 *           timing: a line per storage read is far more than a UART can
 *           carry during playback.  Use it to find a fault, not to
 *           measure anything.
 *
 * Not logged at any level: the inside of the decoders (per frame, per
 * packet), directory walks, JSON / text helpers.  Those report through
 * their result codes only.  FM_TRACE_ENTER / FM_TRACE_EXIT exist for
 * application and port code; the library does not trace every function.
 *
 * What it costs (Cortex-M7 Thumb-2, clang -Os, all features on, library
 * code without third_party, 203 KB at OFF): LOW +7.3 KB, MEDIUM +9.3 KB,
 * HIGH +9.9 KB of flash.  Measured on object files, not on a board.
 * RAM: the one format buffer of fm_log.c (FM_LOG_BUF_SIZE, 512 bytes;
 * with FM_USE_THREADS=1 it is on the stack of the task that logs
 * instead).  Time: nothing is formatted until a sink is registered; with
 * one, a message is one vsnprintf and one sink call -- see fm_log.h for
 * what a blocking UART sink adds.
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
 *     [T/alloc] alloc 4096 -> 0xc0001000
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

#if FM_DEBUG_LEVEL < FM_DEBUG_OFF || FM_DEBUG_LEVEL > FM_DEBUG_HIGH
#error "FM_DEBUG_LEVEL must be 0 (off), 1 (low), 2 (medium) or 3 (high)"
#endif

/*
 * Where the messages go.
 *
 * Default: FM_Log_Tagged() (fm_log.h), which routes to the sink the
 * application registered (UART, SWO, USB, a ring buffer, nothing).  The
 * library never touches hardware itself.  Prefix, text and line break
 * are formatted into one buffer and handed over in ONE sink call.
 *
 * To send the messages somewhere else define FM_DEBUG_PRINTF as a
 * printf-like function in fm_board_config.h.  A message is then three
 * calls of it (prefix, text, line break), as it always was.
 *
 * FM_LOG_DISABLE removes the log output itself.  Leaving the level on
 * would then keep every message string and every call in the firmware
 * for an output that cannot appear, so it forces the level to OFF.
 */
#if defined(FM_LOG_DISABLE) && !defined(FM_DEBUG_PRINTF)
#undef  FM_DEBUG_LEVEL
#define FM_DEBUG_LEVEL FM_DEBUG_OFF
#endif

/*
 * Every message carries the tag of its subsystem.  Define FM_DEBUG_TAG
 * at the top of each .c file:
 *
 *     #define FM_DEBUG_TAG "video"
 *     #include "fm_debug.h"
 */
#ifndef FM_DEBUG_TAG
#define FM_DEBUG_TAG "fm"
#endif

#if FM_DEBUG_LEVEL > FM_DEBUG_OFF
#ifdef FM_DEBUG_PRINTF
#define FM_DEBUG_EMIT_(lvl, ...)                                           \
    do {                                                                   \
        FM_DEBUG_PRINTF("[%c/%s] ", (lvl), FM_DEBUG_TAG);                  \
        FM_DEBUG_PRINTF(__VA_ARGS__);                                      \
        FM_DEBUG_PRINTF("\n");                                             \
    } while (0)
#else
#include "fm_log.h"
#define FM_DEBUG_EMIT_(lvl, ...)                                           \
    ((void)FM_Log_Tagged((lvl), FM_DEBUG_TAG, __VA_ARGS__))
#endif
#endif

/*
 * A message of a level that is switched off expands to nothing: its
 * arguments are not evaluated (never put a side effect in one) and its
 * text is not in the firmware.
 */
#if FM_DEBUG_LEVEL >= FM_DEBUG_LOW
#define FM_ERR_LOG(...)        FM_DEBUG_EMIT_('E', __VA_ARGS__)
#else
#define FM_ERR_LOG(...)        ((void)0)
#endif

#if FM_DEBUG_LEVEL >= FM_DEBUG_MEDIUM
#define FM_EVENT_LOG(...)      FM_DEBUG_EMIT_('I', __VA_ARGS__)
#else
#define FM_EVENT_LOG(...)      ((void)0)
#endif

#if FM_DEBUG_LEVEL >= FM_DEBUG_HIGH
#define FM_TRACE_LOG(...)      FM_DEBUG_EMIT_('T', __VA_ARGS__)
/* Function entry / exit.  For application and port code that wants it;
 * the library itself does not trace every function. */
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
