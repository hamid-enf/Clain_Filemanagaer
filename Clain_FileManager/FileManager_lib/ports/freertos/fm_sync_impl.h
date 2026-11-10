/*
 * FileManager - FreeRTOS synchronization port
 *
 * Build with:  -DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM -Iports/freertos
 *
 * A mutex is modeled with a FreeRTOS mutex semaphore.  Works unchanged on
 * STM32 (HAL + FreeRTOS / CMSIS) and ESP32 (ESP-IDF FreeRTOS).
 */

#ifndef FM_SYNC_IMPL_FREERTOS_H
#define FM_SYNC_IMPL_FREERTOS_H

#include "FreeRTOS.h"
#include "semphr.h"

#include "fm_types.h"

typedef struct
{
    SemaphoreHandle_t handle;
} FM_Mutex;

static inline FM_Result FM_Mutex_Create(FM_Mutex *m)
{
    if (m == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    m->handle = xSemaphoreCreateMutex();
    return (m->handle != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
}

static inline void FM_Mutex_Lock(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        (void)xSemaphoreTake(m->handle, portMAX_DELAY);
    }
}

static inline void FM_Mutex_Unlock(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        (void)xSemaphoreGive(m->handle);
    }
}

static inline void FM_Mutex_Destroy(FM_Mutex *m)
{
    if (m != NULL && m->handle != NULL)
    {
        vSemaphoreDelete(m->handle);
        m->handle = NULL;
    }
}

#endif /* FM_SYNC_IMPL_FREERTOS_H */
