/*
 * defines.h — Project-wide Definitions and Master Top API Header
 *
 *  Author: HAMSHAHRI / Arena.ai
 */

#ifndef SRC_DEFINES_H_
#define SRC_DEFINES_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "main.h"

/* -------------------------------------------------------------------------- */
/* Memory Map Definitions                                                     */
/* -------------------------------------------------------------------------- */
#define SdRamBaseAddress   (0xC0000000UL)
#define SDRAM_BASE_ADDRESS (0xC0000000UL)

/* -------------------------------------------------------------------------- */
/* Logging Macros                                                             */
/* -------------------------------------------------------------------------- */
#include "fm_board_config.h"
#include "fm_debug.h"
#include "fm_log.h"
#include "fm_log_uart_hal.h"
#include "fm_sd_hal.h"

#ifndef printf
#define printf FM_Log
#endif

#ifndef puts
#define puts(s) FM_Log("%s\n", (s))
#endif

/* -------------------------------------------------------------------------- */
/* Library & Component Headers                                                */
/* -------------------------------------------------------------------------- */
#include "examples_file.h"
#include "examples_sdcard.h"
#include "examples_sdcard_media.h"
#include "examples_sdcard_player.h"

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
#include "examples_qspi.h"
#include "examples_qspi_media.h"
#include "examples_qspi_player.h"
#include "qspi_transfers.h"

#include "dma2d_ycbcr.h"
#include "spi_flash_dma_port.h"

#include "fm_eeprom.h"
#include "eeprom_simple.h"
#include "i2c_eeprom_dma.h"

#define USB_MSC_DEFINE_CONTEXT
#define USB_SD_CUBE_USB
#include "usb_msc.h"
#include "usb_sd_presenter.h"

#include "ILI9341_STM32_Driver.h"
#include "ILI9341_GFX.h"

/* -------------------------------------------------------------------------- */
/* Buffer Size Definitions                                                    */
/* -------------------------------------------------------------------------- */
#ifndef UART_RX_BUF_SIZE
#define UART_RX_BUF_SIZE 20
#endif

#ifndef CMD_BUF_SIZE
#define CMD_BUF_SIZE 20
#endif

/* -------------------------------------------------------------------------- */
/* Global Variables for Application State                                     */
/* -------------------------------------------------------------------------- */
extern SPI_HandleTypeDef hspi2;
extern JPEG_HandleTypeDef hjpeg;
extern UART_HandleTypeDef huart1;

static uint32_t *sdram_buf = (uint32_t*)SdRamBaseAddress;

static uint8_t uart_rx_dma_buffer[UART_RX_BUF_SIZE] __attribute__((section(".dma_buffer")));
static uint8_t data_p[CMD_BUF_SIZE];
static volatile uint32_t old_pos = 0;
static uint32_t current_pos = 0;
static uint32_t len_p = 0;

static volatile uint8_t sd_pc_mcu = 1;
static volatile uint8_t viedo_start = 0;
static volatile uint8_t qspi_erase = 0;
static uint8_t music_activated = 0;

/* -------------------------------------------------------------------------- */
/* Modular Top API Subsystems                                                 */
/* -------------------------------------------------------------------------- */
#include "app_display.h"
#include "app_audio.h"
#include "app_video.h"
#include "app_image.h"
#include "app_files.h"
#include "app_usb.h"
#include "code_cmd.h"

#endif /* SRC_DEFINES_H_ */
