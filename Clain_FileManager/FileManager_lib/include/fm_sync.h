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

#else /* !FM_USE_THREADS */

typedef struct
{
    uint8_t _dummy;
} FM_Mutex;

static inline FM_Result FM_Mutex_Create(FM_Mutex *m) { (void)m; return FM_OK; }
static inline void FM_Mutex_Lock(FM_Mutex *m)        { (void)m; }
static inline void FM_Mutex_Unlock(FM_Mutex *m)      { (void)m; }
static inline void FM_Mutex_Destroy(FM_Mutex *m)     { (void)m; }

#endif /* FM_USE_THREADS */

FM_END_DECLS

#endif /* FM_SYNC_H */
