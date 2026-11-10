/* CMSIS-RTOS2 kernel calls used by ports/cmsis_rtos2, on pthreads.
 * Host testing only -- see tools/rtosstub/README.md */
#include <pthread.h>
#include <stdlib.h>

#include "cmsis_os2.h"

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    pthread_mutex_t *m = malloc(sizeof(*m));

    (void)attr;
    if (m != NULL && pthread_mutex_init(m, NULL) != 0)
    {
        free(m);
        m = NULL;
    }
    return m;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    (void)timeout;
    return (pthread_mutex_lock((pthread_mutex_t *)mutex_id) == 0)
               ? osOK : osError;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    return (pthread_mutex_unlock((pthread_mutex_t *)mutex_id) == 0)
               ? osOK : osErrorResource;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
    (void)pthread_mutex_destroy((pthread_mutex_t *)mutex_id);
    free(mutex_id);
    return osOK;
}

/* One distinct, stable address per thread stands in for its thread id. */
osThreadId_t osThreadGetId(void)
{
    static __thread char self;

    return (osThreadId_t)&self;
}
