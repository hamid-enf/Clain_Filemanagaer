/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_sync.h - Optional synchronization abstraction
 *
 * When FM_USE_THREADS == 0 every function compiles to a no-op and the
 * core is fully deterministic / single-threaded.
 *
 * When FM_USE_THREADS == 1 a platform implementation must be selected by
 * defining FM_SYNC_CUSTOM and putting one of the ready-made ports on the
 * include path (they provide fm_sync_impl.h):
 *
 *   ports/pthread       POSIX pthreads (host testing, desktop tools)
 *   ports/freertos      FreeRTOS (STM32, ESP32/ESP-IDF, ...)
 *   ports/cmsis_rtos2   CMSIS-RTOS2 (STM32Cube, other Cortex-M)
 *
 * A fully custom implementation can be provided by defining FM_SYNC_CUSTOM
 * and supplying your own fm_sync_impl.h on the include path.
 *
 * A port provides
 *
 *   FM_Mutex, FM_Mutex_Create / _Lock / _Unlock / _Destroy
 *       a plain (non-recursive) mutex.  The core never locks one twice
 *       from the same thread.
 *
 * and, optionally, the identity of the calling thread:
 *
 *   #define FM_SYNC_HAS_THREAD_SELF 1
 *   typedef ... FM_ThreadId;
 *   FM_ThreadId FM_Thread_Self(void);
 *   int         FM_Thread_Equal(FM_ThreadId a, FM_ThreadId b);
 *
 * With thread identity the shared scratch resources (copy buffer, image
 * codec state, transfer buffer) BLOCK a second thread until the first is
 * done, and a nested call from the SAME thread (progress callback, idle
 * hook) gets FM_ERR_BUSY.  Without it the core cannot tell the two cases
 * apart, so it never blocks: any second user gets FM_ERR_BUSY and has to
 * retry.  All three ready-made ports provide it.
 *
 * Lock hierarchy (outermost first; a thread only ever takes locks further
 * down the list while holding one further up):
 *
 *   mount-table operations (FS_Mount / FS_Unmount)
 *   transfer buffer  >  codec scratch  >  copy buffer
 *   read cache       >  DMA bounce buffer
 *   per-mount filesystem lock
 *   VFS tables (FM_Lock), storage registry, last-error slot
 *
 * User callbacks (progress, idle, storage events) are never invoked with
 * a VFS-table, storage-registry or per-mount lock held.
 *
 * The log sink is the exception: a message is delivered where it is
 * produced, and a storage driver or a filesystem reports its errors from
 * inside an operation, with the per-mount lock held.  A log sink must
 * therefore never call back into the library (the mutexes are not
 * recursive, the call would wait for its own caller) and should not
 * block for long.  See "Rules for a sink" in fm_log.h.
 */

#ifndef FM_SYNC_H
#define FM_SYNC_H

#include "fm_types.h"

FM_BEGIN_DECLS

#if FM_USE_THREADS

#ifndef FM_SYNC_CUSTOM
#error "FM_USE_THREADS=1 requires FM_SYNC_CUSTOM and a sync port (see fm_sync.h)"
#endif

#include "fm_sync_impl.h"

#ifndef FM_SYNC_HAS_THREAD_SELF
#define FM_SYNC_HAS_THREAD_SELF 0
#endif

#else /* !FM_USE_THREADS */

typedef struct
{
    uint8_t _dummy;
} FM_Mutex;

static inline FM_Result FM_Mutex_Create(FM_Mutex *m) { (void)m; return FM_OK; }
static inline void FM_Mutex_Lock(FM_Mutex *m)        { (void)m; }
static inline void FM_Mutex_Unlock(FM_Mutex *m)      { (void)m; }
static inline void FM_Mutex_Destroy(FM_Mutex *m)     { (void)m; }

#define FM_SYNC_HAS_THREAD_SELF 0

#endif /* FM_USE_THREADS */

FM_END_DECLS

#endif /* FM_SYNC_H */
