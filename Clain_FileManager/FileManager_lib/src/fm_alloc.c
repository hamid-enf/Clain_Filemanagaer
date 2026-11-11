/*
 * FileManager - Optional memory allocation hooks (see fm_alloc.h)
 */

#define FM_DEBUG_TAG "alloc"
#include "fm_debug.h"
#include "fm_alloc.h"
#include "fm_internal.h"

#include <stdlib.h>
#include <string.h>

static FM_Allocator g_allocator = {
    NULL, NULL, NULL, NULL, NULL
};

/* Count of allocation requests that returned NULL.  Subsystems use this
 * to tell "the pool ran out" apart from "the input was bad": h264bsd,
 * for instance, reports a failed ALLOCATE deep inside a NAL parser as a
 * generic bitstream error, so an undersized FM_Alloc pool used to
 * surface as FM_ERR_INVALID_FORMAT.  Comparing this counter across a
 * decode makes the real cause unambiguous. */
static unsigned long g_alloc_failures;

static void *default_alloc(size_t size, void *ctx)
{
    (void)ctx;
    return malloc(size);
}

static void *default_calloc(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    return calloc(n, size);
}

static void *default_realloc(void *p, size_t size, void *ctx)
{
    (void)ctx;
    return realloc(p, size);
}

static void default_free(void *p, void *ctx)
{
    (void)ctx;
    free(p);
}

/* Fallbacks built on the registered alloc/free so a custom allocator
 * that only provides alloc+free stays self-consistent.  Mixing the
 * system calloc/realloc with a custom free corrupts both heaps. */
static void *fallback_calloc(size_t n, size_t size, void *ctx)
{
    void *p;
    size_t total = n * size;

    if (n != 0 && total / n != size)
    {
        return NULL; /* overflow */
    }
    p = g_allocator.alloc(total, ctx);
    if (p != NULL)
    {
        memset(p, 0, total);
    }
    return p;
}

/* An allocator without realloc cannot grow a block through the plain
 * FM_Realloc(): the old block size is unknown, and the former "copy
 * `size` bytes anyway" read past the end of the old block whenever the
 * block grew (which is the only reason anybody calls realloc).  The
 * fallback therefore only handles the cases that need no copy; every
 * caller inside the library knows its old size and uses
 * FM_ReallocSized(), which is always correct. */
static void *fallback_realloc(void *p, size_t size, void *ctx)
{
    if (p == NULL)
    {
        return g_allocator.alloc(size, ctx);
    }
    if (size == 0)
    {
        g_allocator.free(p, ctx);
        return NULL;
    }
    return NULL;                /* old block left untouched */
}

int FM_Allocator_Set(const FM_Allocator *a)
{
    if (a == NULL)
    {
        return -1;
    }
    if (a->alloc == NULL || a->free == NULL)
    {
        return -1;
    }
    g_allocator = *a;
    if (g_allocator.calloc == NULL)
    {
        g_allocator.calloc = fallback_calloc;
    }
    if (g_allocator.realloc == NULL)
    {
        g_allocator.realloc = fallback_realloc;
    }
    return 0;
}

const FM_Allocator *FM_Allocator_Get(void)
{
    return &g_allocator;
}

static const FM_Allocator *current(void)
{
    if (g_allocator.alloc == NULL)
    {
        g_allocator.alloc = default_alloc;
        g_allocator.calloc = default_calloc;
        g_allocator.realloc = default_realloc;
        g_allocator.free = default_free;
    }
    return &g_allocator;
}

unsigned long FM_Alloc_FailureCount(void)
{
    unsigned long n;

    FM_Lock();
    n = g_alloc_failures;
    FM_Unlock();
    return n;
}

static void alloc_failed(void)
{
    FM_Lock();
    g_alloc_failures++;
    FM_Unlock();
}

/* Every failed allocation is an error line: running out of heap is the
 * most common reason a decoder refuses a file that plays on the PC, and
 * the result code alone (FM_ERR_NO_MEMORY) does not say how much was
 * asked for.  At the HIGH level every allocation and release is traced,
 * which is how a leak is found. */
void *FM_Alloc(size_t size)
{
    void *p = current()->alloc(size, g_allocator.ctx);

    if (p == NULL && size != 0)
    {
        alloc_failed();
        FM_ERR_LOG("out of memory: %lu bytes", (unsigned long)size);
    }
    else
    {
        FM_TRACE_LOG("alloc %lu -> %p", (unsigned long)size, p);
    }
    return p;
}

void *FM_Calloc(size_t n, size_t size)
{
    void *p = current()->calloc(n, size, g_allocator.ctx);

    if (p == NULL && n != 0 && size != 0)
    {
        alloc_failed();
        FM_ERR_LOG("out of memory: %lu x %lu bytes", (unsigned long)n,
                   (unsigned long)size);
    }
    else
    {
        FM_TRACE_LOG("calloc %lu x %lu -> %p", (unsigned long)n,
                     (unsigned long)size, p);
    }
    return p;
}

void *FM_Realloc(void *p, size_t size)
{
    void *np = current()->realloc(p, size, g_allocator.ctx);

    if (np == NULL && size != 0)
    {
        alloc_failed();
        FM_ERR_LOG("out of memory: growing a block to %lu bytes",
                   (unsigned long)size);
    }
    else
    {
        FM_TRACE_LOG("realloc %p to %lu -> %p", p, (unsigned long)size, np);
    }
    return np;
}

void *FM_ReallocSized(void *p, size_t old_size, size_t new_size)
{
    const FM_Allocator *a = current();
    void *np;

    if (a->realloc != fallback_realloc)
    {
        return FM_Realloc(p, new_size);
    }
    if (p == NULL)
    {
        return FM_Alloc(new_size);
    }
    if (new_size == 0)
    {
        a->free(p, g_allocator.ctx);
        return NULL;
    }
    np = FM_Alloc(new_size);
    if (np != NULL)
    {
        memcpy(np, p, (old_size < new_size) ? old_size : new_size);
        a->free(p, g_allocator.ctx);
    }
    return np;
}

void FM_Free(void *p)
{
    if (p != NULL)
    {
        FM_TRACE_LOG("free %p", p);
    }
    current()->free(p, g_allocator.ctx);
}
