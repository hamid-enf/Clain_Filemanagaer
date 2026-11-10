/* Host stand-in for semphr.h -- see tools/rtosstub/README.md */
#ifndef RTOSSTUB_SEMPHR_H
#define RTOSSTUB_SEMPHR_H

#include "FreeRTOS.h"

struct QueueDefinition;
typedef struct QueueDefinition *QueueHandle_t;
typedef QueueHandle_t           SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t xSemaphore,
                                 TickType_t xBlockTime);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t xSemaphore);
void              vSemaphoreDelete(SemaphoreHandle_t xSemaphore);

#endif
