/*
 * FileManager - Optional memory allocation hooks (see fm_alloc.h)
 */

#define FM_DEBUG_TAG "alloc"
#include "fm_debug.h"
#include "fm_alloc.h"

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

static void *fallback_realloc(void *p, size_t size, void *ctx)
{
    void *np;

    if (p == NULL)
    {
        return g_allocator.alloc(size, ctx);
    }
    if (size == 0)
    {
        g_allocator.free(p, ctx);
        return NULL;
    }
    /* The old block size is unknown here, so grow-copy conservatively:
     * allocate the new size and copy `size` bytes worth (the caller is
     * only allowed to rely on min(old,new) anyway; copying from a
     * larger old block is safe because it is still allocated). */
    np = g_allocator.alloc(size, ctx);
    if (np != NULL)
    {
        memcpy(np, p, size);
        g_allocator.free(p, ctx);
    }
    return np;
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
    return g_alloc_failures;
}

void *FM_Alloc(size_t size)
{
    void *p = current()->alloc(size, g_allocator.ctx);

    if (p == NULL && size != 0)
    {
        g_alloc_failures++;
    }
    return p;
}

void *FM_Calloc(size_t n, size_t size)
{
    void *p = current()->calloc(n, size, g_allocator.ctx);

    if (p == NULL && n != 0 && size != 0)
    {
        g_alloc_failures++;
    }
    return p;
}

void *FM_Realloc(void *p, size_t size)
{
    void *np = current()->realloc(p, size, g_allocator.ctx);

    if (np == NULL && size != 0)
    {
        g_alloc_failures++;
    }
    return np;
}

void FM_Free(void *p)
{
    current()->free(p, g_allocator.ctx);
}
