/*
 * debug_log_tests.h -- tests for the debug levels (fm_debug.h) and the log
 * sink (fm_log.h).
 *
 * Included by test/main.c.  Two parts:
 *
 *   1. A sink that stays registered for the whole run.  The library
 *      formats a message only when a sink is registered, so without it
 *      no format string was ever executed by the test suite: a "%s"
 *      given a number would have shipped.  With it every message of the
 *      active debug level is formatted (under ASan in "make asan"), is
 *      counted, and is checked for shape.  Set FM_TEST_LOG=1 in the
 *      environment to see the messages on stderr.
 *
 *   2. test_debug_levels(): what each level prints and what it costs.
 */
#ifndef DEBUG_LOG_TESTS_H
#define DEBUG_LOG_TESTS_H

static unsigned long g_dl_msgs;        /* sink calls                      */
static unsigned long g_dl_bytes;       /* bytes delivered                 */
static unsigned long g_dl_bad;         /* messages with a wrong shape     */
static char          g_dl_first_bad[96];
static int           g_dl_echo;

static void dl_global_sink(void *ctx, const char *data, size_t len)
{
    size_t i;
    int    bad = 0;

    (void)ctx;
    /* With threads several tasks deliver at once: a message is formatted
     * on its own stack, but these counters are shared. */
#if FM_USE_THREADS
    (void)__sync_fetch_and_add(&g_dl_msgs, 1ul);
    (void)__sync_fetch_and_add(&g_dl_bytes, (unsigned long)len);
#else
    g_dl_msgs++;
    g_dl_bytes += (unsigned long)len;
#endif

    if (len == 0u || len >= (size_t)FM_LOG_BUF_SIZE)
    {
        bad = 1;
    }
    for (i = 0; i < len && !bad; i++)
    {
        if (data[i] == '\0')
        {
            bad = 1;                       /* length and content disagree */
        }
    }
    /* A levelled message ("[E/tag] ...") is one line in one call. */
    if (!bad && len >= 4u && data[0] == '[' && data[2] == '/' &&
        (data[1] == 'E' || data[1] == 'I' || data[1] == 'T'))
    {
        if (data[len - 1u] != '\n')
        {
            bad = 1;
        }
        for (i = 0; i + 1u < len && !bad; i++)
        {
            if (data[i] == '\n')
            {
                bad = 1;                   /* two lines in one message    */
            }
        }
    }
    if (bad)
    {
#if FM_USE_THREADS
        if (__sync_fetch_and_add(&g_dl_bad, 1ul) == 0ul)
#else
        if (g_dl_bad++ == 0u)
#endif
        {
            size_t n = (len < sizeof(g_dl_first_bad) - 1u)
                           ? len : sizeof(g_dl_first_bad) - 1u;
            memcpy(g_dl_first_bad, data, n);
            g_dl_first_bad[n] = '\0';
        }
    }
    if (g_dl_echo)
    {
        (void)fwrite(data, 1u, len, stderr);
    }
}

static void dl_install_global_sink(void)
{
    const char *e = getenv("FM_TEST_LOG");

    g_dl_echo = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    FM_Log_SetSink(dl_global_sink, NULL);
}

/* ------------------------------------------------------------------ */
/* Capture sink for the tests below                                    */
/* ------------------------------------------------------------------ */
static char          g_dlc_buf[2048];
static size_t        g_dlc_len;
static unsigned long g_dlc_calls;
static size_t        g_dlc_last_len;
static char          g_dlc_last_end;
static int           g_dlc_relog;        /* sink logs again this often  */
static unsigned long g_dlc_depth_max;
static unsigned long g_dlc_depth;

static void dl_capture_sink(void *ctx, const char *data, size_t len)
{
    size_t i;

    (void)ctx;
    g_dlc_calls++;
    g_dlc_last_len = len;
    g_dlc_last_end = (len > 0u) ? data[len - 1u] : '\0';
    for (i = 0; i < len && g_dlc_len < sizeof(g_dlc_buf) - 1u; i++)
    {
        g_dlc_buf[g_dlc_len++] = data[i];
    }
    g_dlc_buf[g_dlc_len] = '\0';

    g_dlc_depth++;
    if (g_dlc_depth > g_dlc_depth_max)
    {
        g_dlc_depth_max = g_dlc_depth;
    }
    /* A sink that logs: what a file sink does when its own write fails
     * and the library reports that.  The depth limit only keeps an
     * unguarded library from taking the test program down with it. */
    if (g_dlc_relog && g_dlc_depth < 64u)
    {
        (void)FM_Log("from inside the sink\n");
        (void)FM_Log_Tagged('E', "sink", "nested %d", 1);
        (void)FM_Log_Write("raw", 3u);
    }
    g_dlc_depth--;
}

static void dl_capture_reset(void)
{
    g_dlc_len = 0u;
    g_dlc_calls = 0u;
    g_dlc_last_len = 0u;
    g_dlc_last_end = '\0';
    g_dlc_buf[0] = '\0';
    g_dlc_relog = 0;
    g_dlc_depth = 0u;
    g_dlc_depth_max = 0u;
}

static void *dl_fail_alloc(size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    return NULL;
}

static void dl_fail_free(void *p, void *ctx)
{
    (void)p;
    (void)ctx;
}

/* A filesystem object with no storage and no operations: mounting it
 * has to fail. */
static FM_FS g_dl_dead_fs;

static void test_debug_levels(void)
{
    FM_LogSinkFn saved;
    void *saved_ctx = NULL;
    int lib_level = FileManager_GetDebugLevel();

    saved = FM_Log_GetSink(&saved_ctx);
    FM_Log_SetSink(dl_capture_sink, NULL);

#ifndef FM_LOG_DISABLE
    TEST("Debug: a levelled message is one sink call and one line");
    {
        dl_capture_reset();
        (void)FM_Log_Tagged('E', "unit", "value %d and %s", 42, "text");
        if (g_dlc_calls != 1u)
        {
            char m[80];
            (void)snprintf(m, sizeof(m), "%lu sink calls for one message",
                           g_dlc_calls);
            FAIL(m);
        }
        else if (strcmp(g_dlc_buf, "[E/unit] value 42 and text\n") != 0)
        {
            FAIL("wrong text");
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: a message longer than the buffer is cut and keeps its line end");
    {
        static char big[FM_LOG_BUF_SIZE * 2];

        memset(big, 'x', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        dl_capture_reset();
        (void)FM_Log_Tagged('I', "unit", "%s", big);
        if (g_dlc_calls != 1u || g_dlc_last_len >= (size_t)FM_LOG_BUF_SIZE + 1u ||
            g_dlc_last_len < (size_t)FM_LOG_BUF_SIZE - 2u)
        {
            char m[80];
            (void)snprintf(m, sizeof(m), "calls %lu, length %lu",
                           g_dlc_calls, (unsigned long)g_dlc_last_len);
            FAIL(m);
        }
        else if (g_dlc_last_end != '\n')
        {
            FAIL("line end lost");
        }
        else
        {
            dl_capture_reset();
            (void)FM_Log("%s", big);            /* plain FM_Log: just cut */
            if (g_dlc_calls != 1u ||
                g_dlc_last_len != (size_t)FM_LOG_BUF_SIZE - 1u)
            {
                FAIL("plain message not cut to the buffer");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("Debug: a NULL tag and an over-long tag are handled");
    {
        dl_capture_reset();
        (void)FM_Log_Tagged('E', NULL, "a");
        (void)FM_Log_Tagged('E', "0123456789012345678901234567890123456789",
                            "b");
        if (g_dlc_calls != 2u || strncmp(g_dlc_buf, "[E/fm] a\n", 9) != 0 ||
            g_dlc_last_end != '\n' || g_dlc_last_len > 32u)
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

#if !FM_USE_THREADS
    TEST("Debug: a sink that logs does not recurse (nested messages dropped)");
    {
        unsigned long before = FM_Log_Dropped();
        unsigned long dropped;

        dl_capture_reset();
        g_dlc_relog = 1;
        (void)FM_Log("outer\n");
        g_dlc_relog = 0;
        dropped = FM_Log_Dropped() - before;
        if (g_dlc_depth_max != 1u || g_dlc_calls != 1u)
        {
            char m[96];
            (void)snprintf(m, sizeof(m),
                           "sink entered %lu deep, %lu calls",
                           g_dlc_depth_max, g_dlc_calls);
            FAIL(m);
        }
        else if (dropped != 3u)
        {
            char m[96];
            (void)snprintf(m, sizeof(m), "%lu dropped, expected 3", dropped);
            FAIL(m);
        }
        else if (strcmp(g_dlc_buf, "outer\n") != 0)
        {
            FAIL("outer message damaged by the nested ones");
        }
        else
        {
            /* and logging works again afterwards */
            dl_capture_reset();
            (void)FM_Log("after\n");
            if (strcmp(g_dlc_buf, "after\n") != 0)
            {
                FAIL("log stayed blocked after the nested call");
            }
            else
            {
                PASS();
            }
        }
    }
#endif

#endif /* !FM_LOG_DISABLE */

    TEST("Debug: the library reports the level it was built with");
    {
        if (lib_level != FM_DEBUG_LEVEL_UNDER_TEST)
        {
            char m[96];
            (void)snprintf(m, sizeof(m), "library says %d, build asked for %d",
                           lib_level, FM_DEBUG_LEVEL_UNDER_TEST);
            FAIL(m);
        }
        else
        {
            PASS();
        }
    }

#ifdef FM_DEBUG_PRINTF
    /* The build sends the levelled messages to its own printf-like
     * function (-DFM_DEBUG_PRINTF=...), not through the sink.  What the
     * sink sees is then what it sees at OFF: nothing.  The tests below
     * check exactly that. */
    lib_level = FM_DEBUG_OFF;
#endif

    TEST("Debug: an error path prints at LOW and above, nothing at OFF");
    {
        FM_Result r;

        dl_capture_reset();
        r = File_Delete(NULL);              /* FM_CHECK_PTR -> error line */
        if (r == FM_OK)
        {
            FAIL("File_Delete(NULL) succeeded");
        }
        else if (lib_level == FM_DEBUG_OFF && g_dlc_calls != 0u)
        {
            FAIL(g_dlc_buf);
        }
        else if (lib_level >= FM_DEBUG_LOW &&
                 (g_dlc_calls != 1u ||
                  strcmp(g_dlc_buf, "[E/file] path is NULL\n") != 0))
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: the success path is silent at LOW, one line per event at MEDIUM");
    {
        FM_Result r;
        uint8_t byte = 7u;
        int wrong = 0;

        r = File_WriteAll("/dl_ok.bin", &byte, 1u);
        dl_capture_reset();
        if (r == FM_OK)
        {
            FM_File *f = File_Open("/dl_ok.bin", FM_OPEN_READ);
            if (f != NULL)
            {
                (void)File_Close(f);
            }
            else
            {
                r = FM_ERR_IO;
            }
        }
        if (r == FM_OK)
        {
            r = File_Delete("/dl_ok.bin");
        }
        if (r != FM_OK)
        {
            FAIL("setup failed");
        }
        else
        {
            if (lib_level <= FM_DEBUG_LOW && g_dlc_calls != 0u)
            {
                wrong = 1;
            }
            if (lib_level >= FM_DEBUG_MEDIUM &&
                (strstr(g_dlc_buf, "[I/file] opened /dl_ok.bin") == NULL ||
                 strstr(g_dlc_buf, "[I/file] deleted /dl_ok.bin\n") == NULL))
            {
                wrong = 1;
            }
            if (strstr(g_dlc_buf, "[E/") != NULL)
            {
                wrong = 1;
            }
            if (wrong)
            {
                FAIL(g_dlc_buf);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("Debug: deleting a file that is not there is not reported as an error");
    {
        FM_Result r;

        dl_capture_reset();
        r = File_Delete("/dl_never_existed.bin");
        if (r != FM_ERR_NOT_FOUND)
        {
            FAIL("expected NOT_FOUND");
        }
        else if (strstr(g_dlc_buf, "[E/") != NULL)
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: a failed allocation is an error line with its size");
    {
        FM_Allocator saved_alloc = *FM_Allocator_Get();
        FM_Allocator failing;
        void *p;

        memset(&failing, 0, sizeof(failing));
        failing.alloc = dl_fail_alloc;
        failing.free = dl_fail_free;
        dl_capture_reset();
        (void)FM_Allocator_Set(&failing);
        p = FM_Alloc(1234u);
        (void)FM_Allocator_Set(&saved_alloc);
        if (p != NULL)
        {
            FAIL("allocation succeeded");
        }
        else if (lib_level >= FM_DEBUG_LOW &&
                 strstr(g_dlc_buf, "[E/alloc] out of memory: 1234 bytes\n")
                     == NULL)
        {
            FAIL(g_dlc_buf);
        }
        else if (lib_level == FM_DEBUG_OFF && g_dlc_calls != 0u)
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: HIGH traces every allocation and release, lower levels none");
    {
        void *p;
        int has_alloc;
        int has_free;

        dl_capture_reset();
        p = FM_Alloc(77u);
        FM_Free(p);
        has_alloc = (strstr(g_dlc_buf, "[T/alloc] alloc 77 -> ") != NULL);
        has_free = (strstr(g_dlc_buf, "[T/alloc] free ") != NULL);
        if (p == NULL)
        {
            FAIL("allocation failed");
        }
        else if (lib_level >= FM_DEBUG_HIGH && !(has_alloc && has_free))
        {
            FAIL(g_dlc_buf);
        }
        else if (lib_level < FM_DEBUG_HIGH && g_dlc_calls != 0u)
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: a mount that fails is an error line, a format is an event");
    {
        FM_Result r;
        int wrong = 0;

        dl_capture_reset();
        r = FS_Format("/dl_no_such_mount");
        if (r == FM_OK)
        {
            wrong = 1;
        }
        /* refused before anything was attempted: nothing to report */
        if (g_dlc_calls != 0u)
        {
            wrong = 1;
        }
        dl_capture_reset();
        r = FS_Mount("/dl_bad", &g_dl_dead_fs);
        if (r == FM_OK)
        {
            (void)FS_Unmount("/dl_bad");
            wrong = 1;
        }
        if (lib_level >= FM_DEBUG_LOW &&
            strstr(g_dlc_buf, "[E/fs] mount /dl_bad failed (") == NULL)
        {
            wrong = 1;
        }
        if (lib_level == FM_DEBUG_OFF && g_dlc_calls != 0u)
        {
            wrong = 1;
        }
        if (wrong)
        {
            FAIL(g_dlc_buf);
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: with no sink a message costs no formatting");
    {
        /* "%s" with a bad pointer would crash if it were formatted. */
        FM_Log_SetSink(NULL, NULL);
        dl_capture_reset();
        (void)FM_Log_Tagged('E', "unit", "%d", 1);
        (void)File_Delete(NULL);
        FM_Log_SetSink(dl_capture_sink, NULL);
        if (g_dlc_calls != 0u)
        {
            FAIL("sink called while unregistered");
        }
        else
        {
            PASS();
        }
    }

    FM_Log_SetSink(saved, saved_ctx);
}

#endif /* DEBUG_LOG_TESTS_H */
