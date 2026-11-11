/* FreeRTOS kernel calls used by ports/freertos, on pthreads.
 * Host testing only -- see tools/rtosstub/README.md */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <pthread.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

struct QueueDefinition
{
    pthread_mutex_t m;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct QueueDefinition *q = malloc(sizeof(*q));

    if (q != NULL && pthread_mutex_init(&q->m, NULL) != 0)
    {
        free(q);
        q = NULL;
    }
    return q;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    (void)xBlockTime;
    return (pthread_mutex_lock(&xSemaphore->m) == 0) ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    return (pthread_mutex_unlock(&xSemaphore->m) == 0) ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    (void)pthread_mutex_destroy(&xSemaphore->m);
    free(xSemaphore);
}

/* One distinct, stable address per thread stands in for its TCB. */
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    static __thread char self;

    return (TaskHandle_t)(void *)&self;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
