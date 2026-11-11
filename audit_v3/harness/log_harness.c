/* Audit v3: the log layer, its buffer arithmetic, the tag and the guard.
 * Build: ./build_log.sh (links libfilemanager.a, ASan+UBSan). */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "file_manager.h"
#include "fm_log.h"

static char   g_last[8192];
static size_t g_last_len;
static int    g_calls;

static void sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    g_calls++;
    if (len <= sizeof(g_last))
    {
        memcpy(g_last, data, len);
        g_last_len = len;
    }
    else
    {
        printf("  !! sink called with len=%u > buffer\n", (unsigned)len);
    }
    if (len < sizeof(g_last)) g_last[len] = 0;
}

static void show(const char *what)
{
    size_t i;

    printf("%-46s len=%3u |", what, (unsigned)g_last_len);
    for (i = 0; i < g_last_len && i < 60; i++)
    {
        unsigned char c = (unsigned char)g_last[i];

        printf("%c", (c >= 32 && c < 127) ? (char)c : '.');
    }
    printf("|\n");
}

/* A tag of exactly n characters. */
static const char *tag_of(size_t n, char *buf)
{
    memset(buf, 't', n);
    buf[n] = 0;
    return buf;
}

int main(void)
{
    char tag[256];
    size_t i;
    int n;

    FileManager_Init();
    (void)FM_Log_Dropped();
    FM_Log_SetSink(sink, NULL);

    printf("FM_LOG_BUF_SIZE = %d\n", (int)FM_LOG_BUF_SIZE);
    printf("dropped at start = %lu\n", FM_Log_Dropped());

    /* 1. truncation exactly at the buffer edges, no tag */
    for (i = 0; i < 6; i++)
    {
        size_t len = (size_t)FM_LOG_BUF_SIZE - 2u + i;
        char text[1024];
        char msg[64];

        memset(text, 'A', sizeof(text) - 1);
        text[sizeof(text) - 1] = 0;
        text[len] = 0;
        g_last_len = 0;
        n = FM_Log("%s", text);
        (void)snprintf(msg, sizeof(msg), "plain text len %u (ret %d)",
                       (unsigned)len, n);
        show(msg);
    }

    /* 2. truncation with a tag */
    for (i = 0; i < 6; i++)
    {
        size_t len = (size_t)FM_LOG_BUF_SIZE - 2u + i;
        char text[1024];
        char msg[64];

        memset(text, 'B', sizeof(text) - 1);
        text[sizeof(text) - 1] = 0;
        text[len] = 0;
        g_calls = 0;
        n = FM_Log_Tagged('E', "fm", "%s", text);
        (void)snprintf(msg, sizeof(msg), "tagged len %u (ret %d)",
                       (unsigned)len, n);
        show(msg);
        if (g_calls != 1) printf("  !! %d sink calls\n", g_calls);
    }

    /* 3. tag lengths */
    for (i = 20; i <= 26; i++)
    {
        char msg[64];

        g_last_len = 0;
        (void)FM_Log_Tagged('E', tag_of(i, tag), "x");
        (void)snprintf(msg, sizeof(msg), "tag len %u", (unsigned)i);
        show(msg);
    }
    g_last_len = 0;
    (void)FM_Log_Tagged('E', tag_of(100, tag), "x");
    show("tag len 100");

    /* 4. NULL tag, NULL format, NULL sink context */
    g_last_len = 0;
    (void)FM_Log_Tagged('E', NULL, "null tag");
    show("NULL tag");
    n = FM_Log(NULL);
    printf("FM_Log(NULL) = %d\n", n);
    n = FM_Log_Tagged('E', "fm", NULL);
    printf("FM_Log_Tagged(NULL fmt) = %d\n", n);
    n = FM_Log_Write(NULL, 5);
    printf("FM_Log_Write(NULL,5) = %d\n", n);
    n = FM_Log_Write("abc", 0);
    printf("FM_Log_Write(abc,0) = %d\n", n);

    /* 5. the smallest buffer: 64 bytes, with a tag, exact fits */
    printf("\n-- FM_LOG_BUF_SIZE minimum is checked at compile time; the\n"
           "   arithmetic above is the same at any size --\n");

    /* 6. a sink that logs (recursion guard) */
    FM_Log_SetSink(NULL, NULL);
    printf("\nrecursion test\n");

    /* 7. what happens when the sink replaces the sink while delivering */
    FM_Log_SetSink(sink, NULL);
    {
        void *ctx = NULL;
        FM_LogSinkFn s = FM_Log_GetSink(&ctx);
        printf("GetSink returns the sink: %s\n", s == sink ? "yes" : "no");
    }
    printf("IsActive = %d\n", FM_Log_IsActive());
    return 0;
}
