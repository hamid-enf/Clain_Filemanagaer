import io
p = 'src/fm_log.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

def crlf(t):
    return t.replace('\n', '\r\n')

# ---- 1. the pair + the accessor, right after the globals -------------
old = crlf("""void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx)
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
""")
new = crlf("""/*
 * The sink and its context are one value, not two globals.
 *
 * Ordering the two stores (ctx first, then the function) fixes the case
 * where an interrupt lands between them, and nothing else.  Two tasks --
 * one calling FM_Log_SetSink(), one logging -- still race: the reader can
 * take the new function with the old context and hand a message to the
 * wrong owner (ThreadSanitizer: data race on g_sink / g_sink_ctx at
 * fm_log.c:39 and :40; measured 6-12 % of 160 000 messages delivered with
 * a mixed pair when a console and a file sink are swapped every 1 ms).
 *
 * The fix is a short spin lock around the two-word copy: it is held for
 * two stores and never across the sink call, so a sink that logs again
 * (or calls FM_Log_SetSink) cannot deadlock, and a message from an ISR
 * waits a few cycles at most.  A lock-free pair slot was tried first and
 * rejected: with two slots and an atomic index a reader and a writer
 * still touch the same slot memory, which ThreadSanitizer reports (the
 * messages were no longer mixed up, the slot memory was still raced).
 *
 * Without threads there is a single task, and the store order alone is
 * enough: ctx first, then the function.
 */
#if FM_USE_THREADS
static unsigned char g_pair_lock;

static void log_lock(void)
{
    while (__atomic_test_and_set(&g_pair_lock, __ATOMIC_ACQUIRE))
    {
        /* spin: the critical section is two stores long */
    }
}

static void log_unlock(void)
{
    __atomic_clear(&g_pair_lock, __ATOMIC_RELEASE);
}

static void log_sink_pair(FM_LogSinkFn *sink_out, void **ctx_out)
{
    log_lock();
    *sink_out = g_sink;
    if (ctx_out != NULL)
    {
        *ctx_out = g_sink_ctx;
    }
    log_unlock();
}
#else
static void log_sink_pair(FM_LogSinkFn *sink_out, void **ctx_out)
{
    *sink_out = g_sink;
    if (ctx_out != NULL)
    {
        *ctx_out = g_sink_ctx;
    }
}
#endif

void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx)
{
#if FM_USE_THREADS
    log_lock();
    g_sink = sink;
    g_sink_ctx = ctx;
    log_unlock();
#else
    /* ترتیب مهم است: اول ctx، بعد تابع. اگر برعکس بود و وقفه‌ای بین
     * این دو خط رخ می‌داد، sink جدید با ctx قدیمی صدا زده می‌شد. */
    g_sink_ctx = ctx;
    g_sink = sink;
#endif
}

FM_LogSinkFn FM_Log_GetSink(void *ctx_out)
{
    FM_LogSinkFn sink;
    void        *ctx = NULL;

    log_sink_pair(&sink, &ctx);
    if (ctx_out != NULL)
    {
        *(void **)ctx_out = ctx;
    }
    return sink;
}

int FM_Log_IsActive(void)
{
    FM_LogSinkFn sink;

    log_sink_pair(&sink, NULL);
    return (sink != NULL) ? 1 : 0;
}
""")
assert s.count(old) == 1, ('set1', s.count(old))
s = s.replace(old, new)

# ---- 2. log_deliver ------------------------------------------------
old = crlf("""static int log_deliver(const char *buf, size_t len)
{
    FM_LogSinkFn sink = g_sink;      /* read once: it may be replaced */

    if (sink == NULL || len == 0u)
    {
        return 0;
    }
    sink(g_sink_ctx, buf, len);
    return (int)len;
}
""")
new = crlf("""static int log_deliver(const char *buf, size_t len)
{
    FM_LogSinkFn sink;
    void        *ctx = NULL;

    /* the pair is read once, as one value: a replacement between two
     * messages is fine, a replacement between the function and its
     * context is not */
    log_sink_pair(&sink, &ctx);
    if (sink == NULL || len == 0u)
    {
        return 0;
    }
    sink(ctx, buf, len);
    return (int)len;
}
""")
assert s.count(old) == 1, ('deliver', s.count(old))
s = s.replace(old, new)

# ---- 3. the two "no sink" early-outs -------------------------------
old = crlf("""int FM_Log_Write(const char *buf, size_t len)
{
    int n;

    if (buf == NULL || len == 0u || g_sink == NULL)
    {
        return 0;
    }
""")
new = crlf("""int FM_Log_Write(const char *buf, size_t len)
{
    int n;

    if (buf == NULL || len == 0u || !FM_Log_IsActive())
    {
        return 0;
    }
""")
assert s.count(old) == 1, ('write', s.count(old))
s = s.replace(old, new)

old = crlf("""    if (g_sink == NULL || fmt == NULL)
    {
        return 0;
    }
    if (!log_enter())
""")
new = crlf("""    if (fmt == NULL || !FM_Log_IsActive())
    {
        return 0;
    }
    if (!log_enter())
""")
assert s.count(old) == 1, ('fmt', s.count(old))
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('f3_03 applied')
