/* Host stand-in for FreeRTOS.h -- see tools/rtosstub/README.md */
#ifndef RTOSSTUB_FREERTOS_H
#define RTOSSTUB_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t      TickType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdFALSE       ((BaseType_t)0)
#define pdTRUE        ((BaseType_t)1)
#define pdPASS        (pdTRUE)
#define pdFAIL        (pdFALSE)

#define configUSE_MUTEXES                  1
#define INCLUDE_xTaskGetCurrentTaskHandle  1

#endif
