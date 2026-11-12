/*
 * Shared FM_Idle fan-out for SimpleAudio (17) and SimpleVideo (19).
 *
 * FM_SetIdle holds a single hook, but both players need CPU time during
 * long decodes (image decode starves music; video decode starves the
 * video's own PCM track).  This mux registers one dispatcher with
 * FM_SetIdle and fans it out to every added function.
 *
 * Included from 17 / 19; do not add this file to the CubeMX sources list.
 */

#ifndef FM_TOPAPI_SIMPLE_IDLE_MUX_H
#define FM_TOPAPI_SIMPLE_IDLE_MUX_H

#include "file_manager.h"

#define SIMPLE_IDLE_MAX 4u

static void (*simple_idle_fns[SIMPLE_IDLE_MAX])(void);

static void simple_idle_dispatch(void)
{
    uint32_t i;

    for (i = 0; i < SIMPLE_IDLE_MAX; i++)
    {
        if (simple_idle_fns[i] != NULL)
        {
            simple_idle_fns[i]();
        }
    }
}

/* Register fn (once); keeps any previously added hooks running. */
static void SimpleIdle_Add(void (*fn)(void))
{
    uint32_t i;

    for (i = 0; i < SIMPLE_IDLE_MAX; i++)
    {
        if (simple_idle_fns[i] == fn)
        {
            return;
        }
    }
    for (i = 0; i < SIMPLE_IDLE_MAX; i++)
    {
        if (simple_idle_fns[i] == NULL)
        {
            simple_idle_fns[i] = fn;
            break;
        }
    }
    FM_SetIdle(simple_idle_dispatch);
}

/* Unregister a hook.  Stopped players should drop their FM_Idle hook so a
 * later Begin failure cannot leave dead refill callbacks behind (and so the
 * idle dispatch never burns time on a player that is not running). */
static void SimpleIdle_Remove(void (*fn)(void))
{
    uint32_t i;

    if (fn == NULL)
    {
        return;
    }
    for (i = 0; i < SIMPLE_IDLE_MAX; i++)
    {
        if (simple_idle_fns[i] == fn)
        {
            simple_idle_fns[i] = NULL;
            return;
        }
    }
}

#endif /* FM_TOPAPI_SIMPLE_IDLE_MUX_H */
