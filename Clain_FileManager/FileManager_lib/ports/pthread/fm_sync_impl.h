/*
 * FileManager - POSIX/pthread synchronization port
 *
 * Build with:  -DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM -Iports/pthread
 * Link with:  -lpthread
 *
 * Used for host-side testing and desktop tools.
 */

#ifndef FM_SYNC_IMPL_PTHREAD_H
#define FM_SYNC_IMPL_PTHREAD_H

#include <pthread.h>

#include "fm_types.h"

typedef struct
{
    pthread_mutex_t handle;
} FM_Mutex;

static inline FM_Result FM_Mutex_Create(FM_Mutex *m)
{
    if (m == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (pthread_mutex_init(&m->handle, NULL) != 0)
    {
        return FM_ERR_INTERNAL;
    }
    return FM_OK;
}

static inline void FM_Mutex_Lock(FM_Mutex *m)
{
    if (m != NULL)
    {
        (void)pthread_mutex_lock(&m->handle);
    }
}

static inline void FM_Mutex_Unlock(FM_Mutex *m)
{
    if (m != NULL)
    {
        (void)pthread_mutex_unlock(&m->handle);
    }
}

static inline void FM_Mutex_Destroy(FM_Mutex *m)
{
    if (m != NULL)
    {
        (void)pthread_mutex_destroy(&m->handle);
    }
}

#endif /* FM_SYNC_IMPL_PTHREAD_H */
