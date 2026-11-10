#ifndef USBD_CORE_H
#define USBD_CORE_H
#include "stm32h7xx_hal.h"

typedef struct {
    uint8_t dummy;
} USBD_HandleTypeDef;

extern USBD_HandleTypeDef hUsbDeviceFS;

static inline uint8_t USBD_Start(USBD_HandleTypeDef *pdev) { (void)pdev; return 0; }
static inline uint8_t USBD_Stop(USBD_HandleTypeDef *pdev) { (void)pdev; return 0; }

#endif
