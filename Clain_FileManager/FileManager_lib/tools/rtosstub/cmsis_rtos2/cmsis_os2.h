/* Host stand-in for cmsis_os2.h -- see tools/rtosstub/README.md */
#ifndef RTOSSTUB_CMSIS_OS2_H
#define RTOSSTUB_CMSIS_OS2_H

#include <stdint.h>
#include <stddef.h>

#define osWaitForever 0xFFFFFFFFU

typedef enum
{
    osOK             =  0,
    osError          = -1,
    osErrorTimeout   = -2,
    osErrorResource  = -3,
    osErrorParameter = -4,
    osErrorNoMemory  = -5,
    osErrorISR       = -6,
    osStatusReserved = 0x7FFFFFFF
} osStatus_t;

typedef void *osMutexId_t;
typedef void *osThreadId_t;

typedef struct
{
    const char *name;
    uint32_t    attr_bits;
    void       *cb_mem;
    uint32_t    cb_size;
} osMutexAttr_t;

osMutexId_t  osMutexNew(const osMutexAttr_t *attr);
osStatus_t   osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);
osStatus_t   osMutexRelease(osMutexId_t mutex_id);
osStatus_t   osMutexDelete(osMutexId_t mutex_id);
osThreadId_t osThreadGetId(void);

#endif
