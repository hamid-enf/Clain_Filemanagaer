/*
 * FileManager - Optional memory allocation hooks
 *
 * The core framework is allocation-free by design.  A few *optional*
 * subsystems (the H.264 decoder, the media player playlist, image resize
 * convenience helpers) need dynamic memory on some targets.  They route
 * every allocation through this module, so an application can redirect it
 * to external RAM (SDRAM on STM32 H7, PSRAM on ESP32) or a static pool
 * without touching the core.
 *
 * Usage:
 *     FM_Allocator ext = { my_alloc, my_calloc, my_realloc, my_free, ctx };
 *     FM_Allocator_Set(&ext);
 *
 * The default allocator is malloc/calloc/realloc/free.
 *
 * `calloc` and `realloc` may be NULL.  calloc is then built from
 * alloc + memset.  realloc cannot be built from alloc/free alone (the
 * old block size is unknown), so without it FM_Realloc() only accepts
 * the trivial cases (NULL block, size 0) and returns NULL for a real
 * resize, leaving the block valid; use FM_ReallocSized(), which works
 * with every allocator.  The library itself only uses FM_ReallocSized().
 *
 * Thread safety (FM_USE_THREADS=1): the library calls the allocator
 * from whichever task runs a decoder or the playlist, without holding
 * any library lock.  A custom allocator must therefore be thread-safe
 * itself, and FM_Allocator_Set() must not be called while another task
 * is inside the library.
 */

#ifndef FM_ALLOC_H
#define FM_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void *(*alloc)(size_t size, void *ctx);
    void *(*calloc)(size_t n, size_t size, void *ctx);
    void *(*realloc)(void *p, size_t size, void *ctx);
    void  (*free)(void *p, void *ctx);
    void  *ctx;
} FM_Allocator;

/* Install a new allocator (copied).  Returns FM_OK, or FM_ERR_INVALID_ARGUMENT
 * if a required callback is missing. */
int FM_Allocator_Set(const FM_Allocator *a);
const FM_Allocator *FM_Allocator_Get(void);

/* Number of allocation requests that have returned NULL since start-up.
 * Monotonic.  Sample it before and after an operation to find out
 * whether a failure was caused by memory exhaustion rather than by the
 * data being processed. */
unsigned long FM_Alloc_FailureCount(void);

void *FM_Alloc(size_t size);
void *FM_Calloc(size_t n, size_t size);
void *FM_Realloc(void *p, size_t size);
/* Resize a block whose current size the caller knows.  Uses the
 * allocator's realloc when it has one, alloc + copy + free otherwise.
 * On failure NULL is returned and `p` stays valid. */
void *FM_ReallocSized(void *p, size_t old_size, size_t new_size);
void  FM_Free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* FM_ALLOC_H */
