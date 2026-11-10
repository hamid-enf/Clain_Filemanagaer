/*
 * defines.h
 *
 *  Created on: Aug 28, 2026
 *      Author: HAMSHAHRI
 */

#ifndef SRC_DEFINES_H_
#define SRC_DEFINES_H_

#include "math.h"


#define SdRamBaseAddress (0xC0000000)
uint32_t *sdram_buf = (uint32_t*)SdRamBaseAddress;



#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define printf FM_Log
#define puts(s)   FM_Log("%s\n", (s))
//#include "printf_redirect.h"
//#include "fm_uart_log.h"

#include "fm_board_config.h"
#include "fm_debug.h"
#include "fm_log_uart_hal.h"

#include "examples_file.h"
#include "examples_sdcard.h"
#include "examples_sdcard_media.h"
#include "examples_sdcard_player.h"

//#include "audio_dma_player.h"
//#include "test_audio_player.h"
#include "simple_audio.h"

#include "spi_tft_dma.h"
#include "simple_image.h"
#define SIMPLE_VIDEO_I2S 1
#include "simple_video.h"
#include "hw_jpeg.h"
#include "sdram_alloc.h"
#include "hw_jpeg_video.h"
#include "cache.h"

#include "fm_qspi.h"
#include "examples_qspi.h"        /* begin/end + exQ_1..10 */
#include "examples_qspi_media.h"  /* اختیاری: exQM_1..3   */
#include "examples_qspi_player.h" /* اختیاری: exQP_1..3   */
#include "qspi_transfers.h"       /* انتقال: exT_1..4     */

#include "dma2d_ycbcr.h"
#include "spi_flash_dma_port.h"

#include "fm_eeprom.h"
#include "eeprom_simple.h"
#include "i2c_eeprom_dma.h"

/* USER CODE 0 (قبل از defines.h) */
#define EX16_USB_DEFINE_CONTEXT
#define EX35_CUBE_USB
#include "usb_msc.h"
#include "usb_sd_presenter.h"

#include "ILI9341_STM32_Driver.h"
#include "ILI9341_GFX.h"
#include <stdio.h>


extern SPI_HandleTypeDef hspi2;

FRESULT res;
FIL file;
__attribute__((aligned(32))) uint32_t bw,br;
DIR dir;
FILINFO fno;
__attribute__((aligned(32))) char buf[128];

#define size_data 10
uint32_t start_t = 0,end_t = 0;
uint32_t dataw [size_data] , datar[size_data];

SimpleVideoInfo inf;
int16_t pcm[2048];
size_t got_n = 0;

extern JPEG_HandleTypeDef hjpeg;

uint8_t music_activated = 0;


volatile uint8_t sd_pc_mcu =1;
volatile uint8_t viedo_start = 0;
volatile uint8_t qspi_erase = 0;


#endif /* SRC_DEFINES_H_ */
