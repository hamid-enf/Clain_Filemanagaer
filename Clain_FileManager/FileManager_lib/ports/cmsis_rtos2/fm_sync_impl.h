/*
 * FileManager - CMSIS-RTOS2 synchronization port
 *
 * Build with:  -DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM -Iports/cmsis_rtos2
 *
 * Uses the CMSIS-RTOS2 API, available on STM32Cube (cmsis_os2.h) and other
 * Cortex-M toolchains.
 */

#ifndef FM_SYNC_IMPL_CMSIS_RTOS2_H
#define FM_SYNC_IMPL_CMSIS_RTOS2_H

#include "cmsis_os2.h"

#include "fm_types.h"

typedef struct
{
    osMutexId_t handle;
} FM_Mutex;

static inline FM_Result FM_Mutex_Create(FM_Mutex *m)
{
    if (m == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    m->handle = osMutexNew(NULL);
    return (m->handle != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
}

static inline void FM_Mutex_Lock(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        (void)osMutexAcquire(m->handle, osWaitForever);
    }
}

static inline void FM_Mutex_Unlock(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        (void)osMutexRelease(m->handle);
    }
}

static inline void FM_Mutex_Destroy(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        (void)osMutexDelete(m->handle);
        m->handle = NULL;
    }
}

#endif /* FM_SYNC_IMPL_CMSIS_RTOS2_H */
