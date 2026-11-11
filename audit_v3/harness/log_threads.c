/* Audit v3: the log layer with FM_USE_THREADS=1.
 *
 * T1: many threads log at level 3 while another thread keeps swapping the
 *     sink between two contexts that must pair (sink A <-> ctx A).
 *     A message delivered to sink B with ctx A is a mismatch: the log
 *     says "context=<letter>" and the check counts the violations.
 * T2: the same without swapping (control).
 *
 * Build (threads + TSan):
 *   see build_log_threads.sh
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_log.h"

static int g_stop;
static volatile unsigned long g_mismatch;
static volatile unsigned long g_msgs;

#define MAGIC_A 0xAAAAAAAAu
#define MAGIC_B 0xBBBBBBBBu

static void sink_a(void *ctx, const char *data, size_t len);

/* Two sinks, each of which insists on its own context. */
static void sink_check(void *ctx, const char *data, size_t len, unsigned magic)
{
    (void)data;
    (void)len;
    __sync_fetch_and_add(&g_msgs, 1);
    if ((unsigned)(size_t)ctx != magic)
    {
        __sync_fetch_and_add(&g_mismatch, 1);
    }
}

static void sink_a(void *ctx, const char *data, size_t len)
{
    sink_check(ctx, data, len, MAGIC_A);
}

static void sink_b(void *ctx, const char *data, size_t len)
{
    sink_check(ctx, data, len, MAGIC_B);
}

typedef struct
{
    int id;
    unsigned n;
} Worker;

static void *writer(void *arg)
{
    Worker *w = (Worker *)arg;
    unsigned i;
    char buf[64];

    for (i = 0; i < w->n; i++)
    {
        sprintf(buf, "thread %d message %u", w->id, i);
        FM_Log_Tagged('T', "thr", "%s %08x", buf, i);
        FM_Log("%s", buf);
    }
    return NULL;
}

static void *swapper(void *arg)
{
    (void)arg;
    while (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) == 0)
    {
        FM_Log_SetSink(sink_a, (void *)(size_t)MAGIC_A);
        FM_Log_SetSink(sink_b, (void *)(size_t)MAGIC_B);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t th[9];
    Worker w[8];
    int i;
    int swap = (argc > 1) ? atoi(argv[1]) : 1;
    int nthreads = 4;
    unsigned per = 20000u;

    FileManager_Init();
    FM_Log_SetSink(sink_a, (void *)(size_t)MAGIC_A);

    for (i = 0; i < nthreads; i++)
    {
        w[i].id = i;
        w[i].n = per;
        pthread_create(&th[i], NULL, writer, &w[i]);
    }
    if (swap) pthread_create(&th[nthreads], NULL, swapper, NULL);

    for (i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    if (swap) pthread_join(th[nthreads], NULL);

    FM_Log_SetSink(NULL, NULL);
    printf("messages=%lu with a mismatched (sink, ctx) pair=%lu%s\n",
           g_msgs, g_mismatch, swap ? "" : " (control: no swapping)");
    return g_mismatch ? 1 : 0;
}
