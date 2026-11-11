/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "dma2d.h"
#include "fatfs.h"
#include "i2c.h"
#include "i2s.h"
#include "jpeg.h"
#include "mdma.h"
#include "quadspi.h"
#include "sdmmc.h"
#include "spi.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"
#include "fmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "defines.h"      /* library headers (top_api), shared flags and buffers */
#include "tests.h"        /* SDRAM write / read-back test                        */
#include "test_tft.h"     /* TFT init + test screen                              */

/* Board glue. main() below only shows WHAT runs and in which order;
 * HOW each step works is in these headers (FileManager_lib/board). */
#include "../../FileManager_lib/board/app_display.h"        /* LCD present + SPI DMA callbacks  */
#include "../../FileManager_lib/board/app_audio_hooks.h"    /* PCM taps, recorder, Music_*      */
#include "../../FileManager_lib/board/app_video_hooks.h"    /* video track / frame hooks        */
#include "../../FileManager_lib/board/app_uart_commands.h"  /* UART command line                */
#include "../../FileManager_lib/board/app_flow.h"           /* the App_* steps called by main() */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* Project symbols (Properties > C/C++ Build > Settings > MCU GCC Compiler >
 * Preprocessor). They are not defined in a header:
 *
 *   FM_USE_FATFS=1  FM_USE_AUDIO=1  FM_USE_MP3=1  FM_USE_VIDEO=1
 *   FM_USE_IMAGE=1  FM_USE_USB_MSC=1  FM_USE_DEVICE_ADAPTERS=1
 */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Nothing here on purpose: callbacks, hooks and commands are in the
 * board/app_*.h headers included above.
 *
 * Keep this file ASCII-only: CubeMX rewrites main.c on every "Generate
 * Code" and can damage non-ASCII text in the user sections. */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  App_MpuUserRegions();                  /* MPU region 3 (D2 SRAM, no cache): must run before MPU_Config() */
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_MDMA_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_FMC_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();
  MX_I2S1_Init();
  MX_SPI2_Init();
  MX_JPEG_Init();
  MX_DMA2D_Init();
  MX_I2C2_Init();
  MX_QUADSPI_Init();
  MX_SPI4_Init();
//  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */

  /* ---- 1) bring-up ------------------------------------------------------ */
  FM_Log_UseHalUart(&huart1);            /* printf / FM_Log -> USART1               */
  FM_SD_Hal_Attach(&hsd1);               /* SD card driver                          */
  Ex35_Sd_FatRoot();                     /* "/" = FAT32 on the SD card              */
  SimpleImage_UseDma2dHandle(&hdma2d);   /* DMA2D for colour conversion             */
  App_PrintBanner();                     /* cache state, port, debug level          */
  App_UartCommandsBegin();               /* UART command line (DMA + idle)          */
  App_SdramDmaTest();                    /* SDRAM write / read-back through DMA     */
  App_DisplayBegin();                    /* SPI TFT + test screen                   */
  App_LibraryBegin();                    /* SDRAM allocator, FileManager_Init(), hardware JPEG */

  /* ---- 2) examples (comment a line out to skip that group) -------------- */
  App_RunFileExamples();                 /* ex_1 .. ex_11          RAM disk         */
  App_RunSdExamples();                   /* exS_*, exM_*, exP_*    SD card          */
  App_RunQspiExamples();                 /* exQ_*, exQM_*, exQP_*, exT_*  QSPI flash */
  App_RunSpiFlashExamples();             /* exF_*                  SPI flash (SPI4) */
  App_RunEepromExamples();               /* exI_*, exE_*           I2C EEPROM       */

  /* ---- 3) players -------------------------------------------------------- */
  App_PlayersBegin();                    /* slideshow, then music or video (music_activated) */

  /* ---- 4) SD card to the PC over USB, until the UART command "sdmcu" ----
   * USB is started in there: MX_USB_DEVICE_Init() above must stay commented. */
  App_SdToPc();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    LcdSpiDma_Poll();                    /* finish a pending LCD transfer           */

    /* UART commands: the interrupt only stores them, the work is done here */
    App_MusicCommandPoll();              /* start / stop                            */
    CmdMusic_ProcessPending();           /* every other command (top_api/code_cmd.h) */
    Music_RecPump();                     /* "rectest": recorder file I/O            */

    /* players: each Poll returns at once when its player is idle */
    SimpleAudio_Poll();
    SimpleImage_Poll();
    SimpleVideo_Poll();

    App_HeartbeatPoll();                 /* every 2 s: LED + live audio level       */
    Ex35_UsbPoll();                      /* USB is serviced here, not in an interrupt */

    App_VideoCommandPoll();              /* vstrt / vstop                           */
    App_FlashEraseCommandPoll();         /* fers                                    */
    App_SdToPcCommandPoll();             /* sdpc ... sdmcu                          */
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0xc0000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;


  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
