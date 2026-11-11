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
#include "fm_config.h"          /* FM_USE_THREADS */

#ifndef FM_LOG_DISABLE

static FM_LogSinkFn g_sink;
static void        *g_sink_ctx;

/* بافر فرمت. ایستا و بدون malloc — روی میکرو نمی‌شود چند کیلوبایت
 * استک را برای یک پیام لاگ خرج کرد. */
#if !FM_USE_THREADS
static char g_buf[FM_LOG_BUF_SIZE];
#endif
/* With FM_USE_THREADS=1 the message is formatted on the caller's stack
 * instead: two tasks logging at the same time used to format into the
 * one static buffer and each sent a mix of both messages (or a string
 * whose terminator the other task had just moved).  A lock is the wrong
 * tool here -- a sink is allowed to call back into the library (a file
 * sink does), which may log again, and messages from an ISR must not
 * block -- so each call simply owns its buffer.  Cost: FM_LOG_BUF_SIZE
 * bytes of task stack while a message is being formatted. */

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

/* ------------------------------------------------------------------ */
/* One message at a time (FM_USE_THREADS=0)                            */
/* ------------------------------------------------------------------ */
/*
 * Without threads there is one format buffer, and two things can ask for
 * it while it is in use:
 *
 *   - the sink itself, by logging or by calling a library function that
 *     reports an error.  Unchecked, that recursion ends only when the
 *     stack does;
 *   - an interrupt that logs in the middle of a message from the main
 *     loop.  Unchecked, it formats over the buffer the main loop is
 *     about to send.
 *
 * In both cases the second message is dropped and counted.  A plain flag
 * is enough: whatever interrupts runs to completion before the
 * interrupted code continues, so it either sees the flag set and leaves,
 * or sets and clears it in between two instructions of the code below.
 *
 * With threads each message is formatted on its caller's stack and
 * nothing is shared, so nothing is dropped; a flag could not tell a
 * second task from a recursion anyway.
 */
static volatile unsigned long g_dropped;

#if !FM_USE_THREADS
static volatile unsigned char g_busy;

static int log_enter(void)
{
    if (g_busy != 0u)
    {
        g_dropped++;
        return 0;
    }
    g_busy = 1u;
    return 1;
}

static void log_leave(void)
{
    g_busy = 0u;
}
#else
static int  log_enter(void) { return 1; }
static void log_leave(void) { }
#endif

unsigned long FM_Log_Dropped(void)
{
    return g_dropped;
}

/* Hand `len` bytes to the sink.  The caller holds the "busy" claim. */
static int log_deliver(const char *buf, size_t len)
{
    FM_LogSinkFn sink = g_sink;      /* read once: it may be replaced */

    if (sink == NULL || len == 0u)
    {
        return 0;
    }
    sink(g_sink_ctx, buf, len);
    return (int)len;
}

int FM_Log_Write(const char *buf, size_t len)
{
    int n;

    if (buf == NULL || len == 0u || g_sink == NULL)
    {
        return 0;
    }
    if (!log_enter())
    {
        return 0;
    }
    n = log_deliver(buf, len);
    log_leave();
    return n;
}

int FM_Log_PutChar(int ch)
{
    char c = (char)(ch & 0xFF);
    return FM_Log_Write(&c, 1u);
}

/* Format the message into one buffer and send it with one sink call.
 * With `tag` set it is a levelled message: "[<level>/<tag>] " goes in
 * front and it always ends in a line break, also when it had to be cut. */
static int log_format_send(int level, const char *tag, const char *fmt,
                           va_list ap)
{
#if FM_USE_THREADS
    char g_buf[FM_LOG_BUF_SIZE];
#endif
    size_t used = 0u;
    size_t room;
    int n;
    int sent;

    if (g_sink == NULL || fmt == NULL)
    {
        return 0;
    }
    if (!log_enter())
    {
        return 0;
    }

    /* One byte is kept back for the line break of a levelled message. */
    room = sizeof(g_buf) - ((tag != NULL) ? 1u : 0u);

    if (tag != NULL)
    {
        /* Written by hand: a second pass through vsnprintf for six
         * characters would double the cost of every message. */
        g_buf[used++] = '[';
        g_buf[used++] = (char)level;
        g_buf[used++] = '/';
        while (*tag != '\0' && used < 24u)
        {
            g_buf[used++] = *tag++;
        }
        g_buf[used++] = ']';
        g_buf[used++] = ' ';
    }

    n = vsnprintf(g_buf + used, room - used, fmt, ap);
    if (n < 0)
    {
        log_leave();
        return -1;                   /* format error */
    }
    /* vsnprintf returns the length the text NEEDED, not what it wrote. */
    used += ((size_t)n >= room - used) ? (room - used - 1u) : (size_t)n;

    if (room != sizeof(g_buf))
    {
        g_buf[used++] = '\n';        /* the byte kept back above */
    }

    sent = log_deliver(g_buf, used);
    log_leave();
    return sent;
}

int FM_Log_V(const char *fmt, va_list ap)
{
    return log_format_send(0, NULL, fmt, ap);
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

int FM_Log_Tagged(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = log_format_send(level, (tag != NULL) ? tag : "fm", fmt, ap);
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
int FM_Log_Tagged(int level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
    return 0;
}
unsigned long FM_Log_Dropped(void) { return 0u; }
void FM_Log_Init(void) { }

#endif /* FM_LOG_DISABLE */
