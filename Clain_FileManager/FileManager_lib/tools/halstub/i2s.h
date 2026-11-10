#ifndef FM_HALSTUB_i2s_H
#define FM_HALSTUB_i2s_H
#include "stm32h7xx_hal.h"

extern I2S_HandleTypeDef hi2s1;
extern I2S_HandleTypeDef hi2s2;

/* simple_audio.h busy-pause path uses these; no-ops under the stub
 * (no real DMA engine on the host). */
static inline HAL_StatusTypeDef HAL_I2S_DMAPause(I2S_HandleTypeDef *hi2s)
{ (void)hi2s; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2S_DMAResume(I2S_HandleTypeDef *hi2s)
{ (void)hi2s; return HAL_OK; }
#endif
