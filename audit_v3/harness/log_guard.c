/* Audit v3: the one-message-at-a-time guard of fm_log.c with
 * FM_USE_THREADS=0, and the FM_CHECK macros.
 *
 * A SIGALRM handler stands in for an interrupt that logs.  The main loop
 * logs long messages; the handler logs a short one.  Nothing may recurse,
 * overwrite the buffer in flight, or leave the guard set.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include "file_manager.h"
#include "fm_log.h"

static volatile unsigned long g_isr_msgs;
static volatile unsigned long g_seen_partial;
static char g_expect;            /* the character the message in flight is made of */
static volatile int g_in_flight;

static void sink(void *ctx, const char *data, size_t len)
{
    size_t i;

    (void)ctx;
    if (len == 0) return;
    /* A levelled message is "[<c>/<tag>] <text>\n" and a plain one is just
     * the text; either way every byte of the body is the same letter. */
    if (len > 8 && data[0] == '[' && data[3] == '/')
    {
        char body = data[7];
        int mixed = 0;

        if (g_in_flight && g_expect != 0 && body != g_expect && body != 'i')
        {
            mixed = 1;
        }
        for (i = 8; i < len && data[i] != '\n'; i++)
        {
            if (data[i] != body && data[i] != 'i') mixed = 1;
        }
        if (mixed) __sync_fetch_and_add(&g_seen_partial, 1);
    }
}

static void on_alarm(int sig)
{
    (void)sig;
    g_isr_msgs++;
    FM_Log_Tagged('E', "isr", "iiiiiiiiiiii");   /* must be dropped or whole */
}

int main(void)
{
    struct itimerval it;
    unsigned long dropped0, dropped1;
    char text[600];
    int i;
    int bad = 0;

    FileManager_Init();
    FM_Log_SetSink(sink, NULL);

    /* 1. recursion: a sink that logs -- the inner message must be dropped */
    dropped0 = FM_Log_Dropped();
    FM_Log_Tagged('E', "outer", "%s", "0123456789");
    dropped1 = FM_Log_Dropped();
    printf("plain message, dropped delta = %lu\n", dropped1 - dropped0);

    /* 2. the guard must be free again after every message */
    {
        int k;
        for (k = 0; k < 1000; k++)
        {
            FM_Log_Tagged('I', "loop", "message %d", k);
        }
        printf("1000 messages, dropped delta = %lu\n",
               FM_Log_Dropped() - dropped1);
    }

    /* 3. a message from a signal handler in the middle of a long one */
    memset(text, 'X', sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    signal(SIGALRM, on_alarm);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 200;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);
    for (i = 0; i < 200000; i++)
    {
        g_expect = 'X';
        FM_Log_Tagged('T', "main", "%s", text);
    }
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    signal(SIGALRM, SIG_DFL);
    printf("interrupts that logged = %lu, messages with mixed content = %lu, "
           "dropped = %lu\n", g_isr_msgs, g_seen_partial, FM_Log_Dropped());
    if (g_seen_partial != 0) bad = 1;

    /* 4. does the guard ever stay stuck?  A few messages after the storm */
    dropped0 = FM_Log_Dropped();
    for (i = 0; i < 10; i++) FM_Log_Tagged('E', "after", "ok");
    printf("after the storm: dropped delta = %lu (0 = guard released)\n",
           FM_Log_Dropped() - dropped0);
    if (FM_Log_Dropped() != dropped0) bad = 1;

    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
