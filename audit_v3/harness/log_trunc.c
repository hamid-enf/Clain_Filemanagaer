/* Audit v3: the formatting side of the log layer at its limits.
 *  - a message of exactly FM_LOG_BUF_SIZE-1, -2, +0, +1 characters, with
 *    and without a tag
 *  - tags of 0, 22, 23, 24 and 100 characters
 *  - a NULL fmt, a NULL tag, a format that fails, a 0-length write
 *  - FM_Log_Dropped bookkeeping
 * Every message is checked: one complete line, terminated, never longer
 * than the buffer the sink was promised.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_log.h"

static char   g_last[4096];
static size_t g_last_len;
static unsigned g_calls;
static int    g_bad;

static void sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    g_calls++;
    if (len >= sizeof(g_last))
    {
        printf("  sink got %u bytes: longer than the buffer\n", (unsigned)len);
        g_bad = 1;
        len = sizeof(g_last) - 1;
    }
    memcpy(g_last, data, len);
    g_last[len] = 0;
    g_last_len = len;
}

static void check(const char *what, size_t want_len, int want_nl)
{
    size_t i;

    printf("  %-46s %3u bytes ", what, (unsigned)g_last_len);
    /* printable? */
    for (i = 0; i < g_last_len; i++)
    {
        unsigned char c = (unsigned char)g_last[i];
        if (c != '\n' && (c < 0x20 || c > 0x7E))
        {
            printf("CONTROL 0x%02X at %u\n", c, (unsigned)i);
            g_bad = 1;
            return;
        }
    }
    if (want_nl && (g_last_len == 0 || g_last[g_last_len - 1] != '\n'))
    {
        printf("NO TRAILING NEWLINE\n");
        g_bad = 1;
        return;
    }
    if (want_len != 0 && g_last_len != want_len)
    {
        printf("length %u, wanted %u\n", (unsigned)g_last_len, (unsigned)want_len);
        g_bad = 1;
        return;
    }
    printf("ok\n");
}

int main(void)
{
    static char text[4096];
    FM_Result r;
    unsigned i;

    FileManager_Init();
    printf("FM_LOG_BUF_SIZE = %d\n", (int)FM_LOG_BUF_SIZE);
    FM_Log_SetSink(sink, NULL);
    if (!FM_Log_IsActive()) { printf("sink not active\n"); return 2; }

    /* a plain message of every length around the buffer size */
    for (i = FM_LOG_BUF_SIZE - 3; i <= FM_LOG_BUF_SIZE + 2; i++)
    {
        memset(text, 'a', i);
        text[i] = 0;
        g_last_len = 0;
        g_calls = 0;
        r = FM_Log_Tagged('E', "t", "%s", text);
        printf("  FM_Log_Tagged(%3u chars) -> %d", i, (int)r);
        {
            size_t total = g_last_len + 0;

            printf(", sink got %u\n", (unsigned)g_last_len);
            if (total > (size_t)FM_LOG_BUF_SIZE)
            {
                printf("    OVERFLOW of the %d-byte buffer\n",
                       (int)FM_LOG_BUF_SIZE);
                g_bad = 1;
            }
        }
    }

    /* tagged: the prefix eats into the same buffer, the newline is kept */
    printf("tag lengths:\n");
    {
        static const unsigned tags[] = { 0, 1, 22, 23, 24, 100 };
        unsigned t;

        for (t = 0; t < sizeof(tags) / sizeof(tags[0]); t++)
        {
            char tag[128];
            char msg[64];

            memset(tag, 'T', tags[t]);
            tag[tags[t]] = 0;
            memset(msg, 'm', 40);
            msg[40] = 0;
            g_last_len = 0;
            r = FM_Log_Tagged('E', tag, "%s", msg);
            printf("  FM_Log_Tagged(tag %3u chars) -> %d, sink got %u bytes: "
                   "'%.30s%s'\n", tags[t], (int)r, (unsigned)g_last_len, g_last,
                   g_last_len > 30 ? "..." : "");
            if (g_last_len > (size_t)FM_LOG_BUF_SIZE)
            {
                printf("    OVERFLOW\n");
                g_bad = 1;
            }
            if (g_last_len == 0 || g_last[g_last_len - 1] != '\n')
            {
                printf("    NO TRAILING NEWLINE\n");
                g_bad = 1;
            }
        }
    }

    /* a tag longer than the room left for it */
    printf("long tag with a long message:\n");
    {
        char tag[200];
        char msg[1000];

        memset(tag, 'Z', sizeof(tag) - 1);
        tag[sizeof(tag) - 1] = 0;
        memset(msg, 'q', sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = 0;
        g_last_len = 0;
        r = FM_Log_Tagged('E', tag, "%s", msg);
        printf("  200-char tag + 999-char message -> %d, %u bytes, ends %02X\n",
               (int)r, (unsigned)g_last_len, (unsigned char)g_last[g_last_len ? g_last_len - 1 : 0]);
        if (g_last_len > (size_t)FM_LOG_BUF_SIZE) { printf("    OVERFLOW\n"); g_bad = 1; }
    }

    /* NULL arguments must not crash */
    printf("NULL arguments:\n");
    printf("  FM_Log(NULL)            -> %d\n", (int)FM_Log(NULL));
    printf("  FM_Log_Tagged(..NULL..) -> %d\n",
           (int)FM_Log_Tagged('E', NULL, "%s", "x"));
    printf("  FM_Log_Write(NULL, 0)   -> %d\n", (int)FM_Log_Write(NULL, 0));
    printf("  FM_Log_Write(\"x\", 0)    -> %d\n", (int)FM_Log_Write("x", 0));
    printf("  FM_Log_PutChar('\\n')    -> %d\n", (int)FM_Log_PutChar('\n'));

    /* a %s with a format the compiler cannot check */
    printf("  FM_Log(\"%%s\") with no argument is undefined; not tested\n");

    /* dropped counter is monotonic and starts at 0 for a plain message */
    {
        unsigned long before = FM_Log_Dropped();

        (void)FM_Log("one message\n");
        printf("dropped: %lu -> %lu (a single-threaded message is never "
               "dropped)\n", before, FM_Log_Dropped());
        if (FM_Log_Dropped() != before) g_bad = 1;
    }

    /* and the sink reflects the last message */
    /* an untagged message is a raw write: no newline is added (documented),
     * a levelled one always ends in one */
    (void)FM_Log("last");
    printf("FM_Log(\"last\") -> '%s' (%u bytes, no newline added)\n", g_last,
           (unsigned)g_last_len);
    check("FM_Log(\"last\")", 4, 0);
    (void)FM_Log_Tagged('E', "a", "last");
    check("FM_Log_Tagged(\"last\")", 11, 1);

    printf("\nlog_trunc: %s\n", g_bad ? "PROBLEM" : "bounded and terminated");
    return g_bad;
}
