/* Host stand-in for task.h -- see tools/rtosstub/README.md */
#ifndef RTOSSTUB_TASK_H
#define RTOSSTUB_TASK_H

#include "FreeRTOS.h"

struct tskTaskControlBlock;
typedef struct tskTaskControlBlock *TaskHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle(void);

#endif
