/*
 * fm_log.c — پیاده‌سازی خروجی متنی
 *
 * هیچ چیزی از سخت‌افزار نمی‌داند. فقط فرمت می‌کند و به مقصدی که ثبت
 * شده تحویل می‌دهد.
 */

#include <stdio.h>
#include <string.h>

/* تعریف خود fm_log.c نباید هشدار deprecated بدهد. */
#define FM_LOG_ALLOW_INIT 1
#include "fm_log.h"

#ifndef FM_LOG_DISABLE

static FM_LogSinkFn g_sink;
static void        *g_sink_ctx;

/* بافر فرمت. ایستا و بدون malloc — روی میکرو نمی‌شود چند کیلوبایت
 * استک را برای یک پیام لاگ خرج کرد. */
static char g_buf[FM_LOG_BUF_SIZE];

void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx)
{
    /* ترتیب مهم است: اول ctx، بعد تابع. اگر برعکس بود و وقفه‌ای بین
     * این دو خط رخ می‌داد، sink جدید با ctx قدیمی صدا زده می‌شد. */
    g_sink_ctx = ctx;
    g_sink = sink;
}

FM_LogSinkFn FM_Log_GetSink(void *ctx_out)
{
    if (ctx_out != NULL)
    {
        *(void **)ctx_out = g_sink_ctx;
    }
    return g_sink;
}

int FM_Log_IsActive(void)
{
    return (g_sink != NULL) ? 1 : 0;
}

#if defined(FM_HOST_BUILD) && FM_HOST_BUILD
static void fm_log_stdout_sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)fwrite(data, 1u, len, stdout);
    (void)fflush(stdout);
}

void FM_Log_UseStdout(void)
{
    FM_Log_SetSink(fm_log_stdout_sink, NULL);
}
#endif

int FM_Log_Write(const char *buf, size_t len)
{
    FM_LogSinkFn sink = g_sink;      /* یک‌بار بخوان: ممکن است عوض شود */

    if (buf == NULL || len == 0u)
    {
        return 0;
    }
    if (sink == NULL)
    {
        return 0;
    }
    sink(g_sink_ctx, buf, len);
    return (int)len;
}

int FM_Log_PutChar(int ch)
{
    char c = (char)(ch & 0xFF);

    return FM_Log_Write(&c, 1u);
}

int FM_Log_V(const char *fmt, va_list ap)
{
    int n;

    if (g_sink == NULL || fmt == NULL)
    {
        return 0;
    }

    n = vsnprintf(g_buf, sizeof(g_buf), fmt, ap);
    if (n < 0)
    {
        return -1;                   /* خطای فرمت */
    }
    /* vsnprintf طولی که *لازم بود* را برمی‌گرداند، نه آنچه نوشت.
     * اگر پیام بلندتر بود، فقط همان مقدار نوشته‌شده را بفرست. */
    if ((size_t)n >= sizeof(g_buf))
    {
        n = (int)sizeof(g_buf) - 1;
    }
    return FM_Log_Write(g_buf, (size_t)n);
}

int FM_Log(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = FM_Log_V(fmt, ap);
    va_end(ap);
    return n;
}

void FM_Log_Init(void)
{
    /* برای سازگاری با کد قدیمی نگه داشته شده. راه‌اندازی حالا با
     * FM_Log_SetSink یا FM_Log_UseHalUart انجام می‌شود. */
}

#else  /* FM_LOG_DISABLE */

void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx) { (void)sink; (void)ctx; }
FM_LogSinkFn FM_Log_GetSink(void *ctx_out) { (void)ctx_out; return NULL; }
int FM_Log_IsActive(void) { return 0; }
int FM_Log_Write(const char *buf, size_t len) { (void)buf; (void)len; return 0; }
int FM_Log_PutChar(int ch) { (void)ch; return 0; }
int FM_Log_V(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }
int FM_Log(const char *fmt, ...) { (void)fmt; return 0; }
void FM_Log_Init(void) { }

#endif /* FM_LOG_DISABLE */
