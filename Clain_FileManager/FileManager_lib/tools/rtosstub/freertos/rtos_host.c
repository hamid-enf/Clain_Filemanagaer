/* FreeRTOS kernel calls used by ports/freertos, on pthreads.
 * Host testing only -- see tools/rtosstub/README.md */
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
