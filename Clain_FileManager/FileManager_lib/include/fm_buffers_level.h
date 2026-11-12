/*
 * fm_buffers_level.h — FM_BUFFERS را به یک عدد ثابت تبدیل می‌کند
 *
 *     FM_BUFFERS_LEVEL   1 = SMALL   2 = NORMAL   3 = LARGE   4 = HUGE
 *
 * چرا لازم است؟ شما می‌نویسید
 *
 *     #define FM_BUFFERS HUGE
 *
 * و کتابخانه با «#if FM_BUFFERS == HUGE» تصمیم می‌گیرد. ولی HUGE (و SMALL،
 * NORMAL، LARGE) نام‌های کوتاهی‌اند که ممکن است جای دیگری هم تعریف شده
 * باشند. نمونهٔ واقعی: <math.h> بعضی کامپایلرها (نسخه‌هایی از newlib روی
 * ARM؛ HUGE یک ثابت قدیمی یونیکس است) خودش دارد
 *
 *     #define HUGE ((float)3.40282346638528860e+38)
 *
 * و اگر math.h زودتر از کتابخانه include شده باشد، «#if FM_BUFFERS == HUGE»
 * به یک عدد اعشاری می‌رسد و بیلد با پیام نامفهومِ
 * «missing binary operator before token» می‌ایستد.
 *
 * اینجا چهار نام فقط برای همین چند خط به مقدار خودشان برده می‌شوند
 * (push_macro / pop_macro) و بعد به حالت قبل برمی‌گردند؛ پس نه ترتیب
 * include مهم است و نه HUGE ی math.h برای کد شما خراب می‌شود. بقیهٔ کتابخانه
 * فقط FM_BUFFERS_LEVEL را نگاه می‌کند که یک عدد ساده است.
 *
 * در کد خودتان هم اگر خواستید مقایسه کنید:
 *
 *     #if FM_BUFFERS_LEVEL >= 3      // LARGE یا HUGE
 */
#ifndef FM_BUFFERS_LEVEL

#include "fm_switches.h"

#ifndef FM_BUFFERS
#define FM_BUFFERS NORMAL
#endif

#pragma push_macro("SMALL")
#pragma push_macro("NORMAL")
#pragma push_macro("LARGE")
#pragma push_macro("HUGE")
#undef SMALL
#undef NORMAL
#undef LARGE
#undef HUGE
#define SMALL  1
#define NORMAL 2
#define LARGE  3
#define HUGE   4

#if   FM_BUFFERS == SMALL
#define FM_BUFFERS_LEVEL 1
#elif FM_BUFFERS == NORMAL
#define FM_BUFFERS_LEVEL 2
#elif FM_BUFFERS == LARGE
#define FM_BUFFERS_LEVEL 3
#elif FM_BUFFERS == HUGE
#define FM_BUFFERS_LEVEL 4
#else
#error "FM_BUFFERS باید یکی از SMALL / NORMAL / LARGE / HUGE باشد"
#endif

#pragma pop_macro("HUGE")
#pragma pop_macro("LARGE")
#pragma pop_macro("NORMAL")
#pragma pop_macro("SMALL")

#endif /* FM_BUFFERS_LEVEL */
