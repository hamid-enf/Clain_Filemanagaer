#ifndef FM_HALSTUB_ILI9341_DRIVER_H
#define FM_HALSTUB_ILI9341_DRIVER_H
#include "stm32h7xx_hal.h"

#define WHITE 0xFFFF
#define SCREEN_HORIZONTAL_1 1

static inline void ILI9341_Init(void) {}
static inline void ILI9341_Fill_Screen(uint16_t c) { (void)c; }
static inline void ILI9341_Set_Rotation(uint8_t r) { (void)r; }
static inline void ILI9341_Set_Address(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) { (void)x1; (void)y1; (void)x2; (void)y2; }

#endif
