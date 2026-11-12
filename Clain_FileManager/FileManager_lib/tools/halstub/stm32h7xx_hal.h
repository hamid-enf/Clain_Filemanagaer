/* حداقل تعاریف HAL برای «فقط بررسی سینتکس» مثال‌های برد.
 * هدفش اجرا نیست — فقط اینکه خطاهای کامپایل مثل نام غلط یک ثابت،
 * قبل از رسیدن به برد گرفته شوند. */
#ifndef FM_HALSTUB_HAL_H
#define FM_HALSTUB_HAL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define __IO volatile
#define __I  volatile const
#define __O  volatile

typedef struct { volatile uint32_t CR1, CR2, CFG1, CFG2, IER, SR, IFCR,
                                   TXDR, RXDR; } SPI_TypeDef;
typedef struct { volatile uint32_t CR1, CFGR, SR, DR, I2SCFGR, NDTR; } DMA_Stream_TypeDef;

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
typedef struct
{
    uint32_t MODER;
    uint32_t OTYPER;
    uint32_t OSPEEDR;
    uint32_t PUPDR;
    uint32_t IDR;
    uint32_t ODR;
    uint32_t BSRR;
    uint32_t LCKR;
    uint32_t AFR[2];
} GPIO_TypeDef;
typedef enum
{
    GPIO_PIN_0  = 0x0001u, GPIO_PIN_1  = 0x0002u, GPIO_PIN_2  = 0x0004u,
    GPIO_PIN_3  = 0x0008u, GPIO_PIN_4  = 0x0010u, GPIO_PIN_5  = 0x0020u,
    GPIO_PIN_6  = 0x0040u, GPIO_PIN_7  = 0x0080u, GPIO_PIN_8  = 0x0100u,
    GPIO_PIN_9  = 0x0200u, GPIO_PIN_10 = 0x0400u
} GPIO_Pin;
static GPIO_TypeDef fm_halstub_gpio_dummy __attribute__((unused));
#define GPIOA (&fm_halstub_gpio_dummy)
#define GPIOB (&fm_halstub_gpio_dummy)
#define GPIOC (&fm_halstub_gpio_dummy)
#define GPIOD (&fm_halstub_gpio_dummy)
#define GPIOE (&fm_halstub_gpio_dummy)
typedef struct {
    uint32_t Direction, PeriphInc, MemInc, PeriphDataAlignment,
             MemDataAlignment, Mode, Priority, FIFOMode;
} DMA_InitTypeDef;
typedef struct { uint32_t dummy; } PCD_HandleTypeDef;
typedef struct { volatile uint32_t State; uint32_t ErrorCode;
                 uint32_t CardType; uint32_t LogBlockNbr;
                 uint32_t LogBlockSize; } SD_HandleTypeDef;
typedef struct { uint32_t LogBlockNbr; uint32_t LogBlockSize; }
    HAL_SD_CardInfoTypeDef;
static inline HAL_StatusTypeDef HAL_SD_GetCardInfo(
    SD_HandleTypeDef *hsd, HAL_SD_CardInfoTypeDef *ci)
{ (void)hsd; ci->LogBlockNbr = 0; ci->LogBlockSize = 512; return HAL_OK; }
typedef enum { HAL_SD_CARD_READY = 0, HAL_SD_CARD_IDENTIFICATION,
               HAL_SD_CARD_STANDBY, HAL_SD_CARD_TRANSFER,
               HAL_SD_CARD_SENDING, HAL_SD_CARD_RECEIVING,
               HAL_SD_CARD_PROGRAMMING, HAL_SD_CARD_DISCONNECTED,
               HAL_SD_CARD_ERROR } HAL_SD_CardStateType;
static inline HAL_SD_CardStateType HAL_SD_GetCardState(
    SD_HandleTypeDef *hsd) { (void)hsd; return HAL_SD_CARD_TRANSFER; }
static inline HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef *hsd,
    uint8_t *d, uint32_t a, uint32_t n, uint32_t t) { (void)hsd;(void)d;(void)a;(void)n;(void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef *hsd,
    const uint8_t *d, uint32_t a, uint32_t n, uint32_t t) { (void)hsd;(void)d;(void)a;(void)n;(void)t; return HAL_OK; }
typedef enum { OTG_FS_IRQn = 67 } IRQn_Type;
static inline void HAL_NVIC_DisableIRQ(IRQn_Type irqn) { (void)irqn; }

static inline HAL_StatusTypeDef HAL_PCD_IRQHandler(PCD_HandleTypeDef *hpcd)
{ (void)hpcd; return HAL_OK; }

typedef struct DMA_HandleTypeDef_s_t {
    DMA_Stream_TypeDef *Instance; DMA_InitTypeDef Init; uint32_t ErrorCode;
    void (*XferCpltCallback)(struct DMA_HandleTypeDef_s_t *h);
    void (*XferHalfCpltCallback)(struct DMA_HandleTypeDef_s_t *h);
    void (*XferErrorCallback)(struct DMA_HandleTypeDef_s_t *h);
    void (*XferAbortCallback)(struct DMA_HandleTypeDef_s_t *h);
    void *Parent;
} DMA_HandleTypeDef_s;

typedef struct {
    SPI_TypeDef *Instance; uint32_t State; uint32_t ErrorCode;
    DMA_HandleTypeDef_s *hdmatx;
    DMA_HandleTypeDef_s *hdmarx;
} SPI_HandleTypeDef;
static inline HAL_StatusTypeDef HAL_SPI_FlushRxFifo(SPI_HandleTypeDef *h) { (void)h; return HAL_OK; }

typedef struct {
    uint32_t Mode, Standard, DataFormat, MCLKOutput, AudioFreq,
             CPOL, ClockSource, FullDuplexMode;
} I2S_InitTypeDef;
typedef struct {
    uint32_t Instance; uint32_t State; I2S_InitTypeDef Init;
    DMA_HandleTypeDef_s *hdmatx;
    DMA_HandleTypeDef_s *hdmarx;
} I2S_HandleTypeDef;

typedef struct {
    uint32_t ImageWidth, ImageHeight, ChromaSubsampling, ColorSpace,
             ImageQuality;
} JPEG_ConfTypeDef;
typedef struct { uint32_t Instance; JPEG_ConfTypeDef Conf; } JPEG_HandleTypeDef;

typedef struct {
    uint32_t InstructionMode, Instruction, AddressMode, AddressSize,
             Address, DataMode, NbData, DummyCycles, SIOOMode, DdrMode,
             DdrHoldHalfCycle, AlternateByteMode, AlternateBytes,
             AlternateBytesSize;
} QSPI_CommandTypeDef;
typedef struct {
    uint32_t Match, Mask, Interval, StatusBytesSize, MatchMode,
             AutomaticStop;
} QSPI_AutoPollingTypeDef;
typedef struct {
    uint32_t TimeOutActivation, TimeOutPeriod;
} QSPI_MemoryMappedTypeDef;
/* State و hmdma: درایورهای DMA محورِ qspi_w25q.h / qspi_driver.h به آن‌ها نگاه می‌کنند. */
typedef struct {
    uint32_t Instance; uint32_t State; void *hmdma;
} QSPI_HandleTypeDef;
#define HAL_QSPI_STATE_READY 1u
typedef struct { uint32_t Instance; } SDRAM_HandleTypeDef;

typedef DMA_HandleTypeDef_s DMA_HandleTypeDef;
typedef struct { uint32_t Instance; } UART_HandleTypeDef;


/* The clock stands still unless a host test moves it: set
 * halstub_tick_ms, or install halstub_tick_hook (called on every
 * HAL_GetTick, e.g. to advance the clock and fire the I2S "interrupts"). */
static volatile uint32_t halstub_tick_ms __attribute__((unused));
static void (*halstub_tick_hook)(void) __attribute__((unused));
static inline uint32_t HAL_GetTick(void)
{
    if (halstub_tick_hook != NULL)
    {
        halstub_tick_hook();
    }
    return halstub_tick_ms;
}
static inline void HAL_Delay(uint32_t ms) { (void)ms; }
static inline void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t n, GPIO_PinState s)
{ (void)p; (void)n; (void)s; }
static inline void HAL_GPIO_TogglePin(GPIO_TypeDef *p, uint16_t n)
{ (void)p; (void)n; }

/* cache / MPU */
#define __DSB() ((void)0)
#define __ISB() ((void)0)
typedef struct { uint32_t CCR; } SCB_Type_stub;
/*
 * SCB باید واقعا وجود داشته باشد، نه فقط extern.
 * قبلا فقط اعلام شده بود و هیچ‌کس تعریفش نمی‌کرد؛ تا وقتی نگهداری کش
 * (به خاطر باگ FM_HOST_BUILD) از بیلد حذف می‌شد کسی متوجه نمی‌شد،
 * ولی به محض اینکه آن باگ درست شد لینک تست‌ها می‌شکست.
 * کش خاموش = CCR صفر، که پیش‌فرض درستی برای تست‌هاست.
 */
static SCB_Type_stub fm_halstub_scb_storage = { 0u };
static SCB_Type_stub *SCB = &fm_halstub_scb_storage;
#define SCB_CCR_DC_Msk (1u << 16)
#define SCB_CCR_IC_Msk (1u << 17)
/* بیت CCR را واقعاً عوض می‌کنند، مثل CMSIS واقعی. اگر خالی باشند
 * تست‌هایی که وضعیت کش را می‌سنجند بی‌معنی می‌شوند. */
static inline void SCB_EnableICache(void)  { SCB->CCR |=  SCB_CCR_IC_Msk; }
static inline void SCB_DisableICache(void) { SCB->CCR &= ~SCB_CCR_IC_Msk; }
static inline void SCB_EnableDCache(void)  { SCB->CCR |=  SCB_CCR_DC_Msk; }
static inline void SCB_DisableDCache(void) { SCB->CCR &= ~SCB_CCR_DC_Msk; }
static inline void SCB_CleanDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }
static inline void SCB_InvalidateDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }
#define __DCACHE_PRESENT 1U

/* SPI / I2S / QSPI */
static inline HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n)
{ (void)h; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)d; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h) { (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)d; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
    uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t t)
{ (void)h; (void)tx; (void)rx; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_Receive_DMA(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n)
{ (void)h; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *h,
    uint8_t *tx, uint8_t *rx, uint16_t n)
{ (void)h; (void)tx; (void)rx; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2S_Transmit_DMA(I2S_HandleTypeDef *h, uint16_t *d, uint16_t n)
{ (void)h; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2S_Receive_DMA(I2S_HandleTypeDef *h, uint16_t *d, uint16_t n)
{ (void)h; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2S_DMAStop(I2S_HandleTypeDef *h) { (void)h; return HAL_OK; }

#define HAL_MAX_DELAY 0xFFFFFFFFU

/* --- constants the examples reference --------------------------------- */
#define QSPI_INSTRUCTION_1_LINE      1u
#define QSPI_INSTRUCTION_NONE        0u
#define QSPI_ADDRESS_NONE            0u
#define QSPI_ADDRESS_1_LINE          1u
#define QSPI_ADDRESS_24_BITS         2u
#define QSPI_ADDRESS_32_BITS         3u
#define QSPI_DATA_NONE               0u
#define QSPI_DATA_1_LINE             1u
#define QSPI_DATA_4_LINES            4u
#define QSPI_SIOO_INST_EVERY_CMD     0u
#define QSPI_DDR_MODE_DISABLE        0u
#define QSPI_ALTERNATE_BYTES_NONE    0u
#define QSPI_TIMEOUT_COUNTER_DISABLE 0u

#define I2S_DATAFORMAT_16B           0u
#define I2S_DATAFORMAT_16B_EXTENDED  1u
#define I2S_DATAFORMAT_24B           2u
#define I2S_DATAFORMAT_32B           3u
#define I2S_MODE_MASTER_TX           0u
#define I2S_MODE_MASTER_RX           1u
#define I2S_STANDARD_PHILIPS         0u
#define I2S_MCLKOUTPUT_DISABLE       0u
#define I2S_MCLKOUTPUT_ENABLE        1u
#define I2S_CPOL_LOW                 0u
#define I2S_FULLDUPLEXMODE_DISABLE   0u
#define I2S_AUDIOFREQ_8K          8000u
#define I2S_AUDIOFREQ_11K        11025u
#define I2S_AUDIOFREQ_16K        16000u
#define I2S_AUDIOFREQ_22K        22050u
#define I2S_AUDIOFREQ_32K        32000u
#define I2S_AUDIOFREQ_44K        44100u
#define I2S_AUDIOFREQ_48K        48000u
#define I2S_AUDIOFREQ_96K        96000u

#define DMA_MDATAALIGN_BYTE          0u
#define DMA_MDATAALIGN_HALFWORD      1u
#define DMA_MDATAALIGN_WORD          2u
#define DMA_PDATAALIGN_BYTE          0u
#define DMA_PDATAALIGN_HALFWORD      1u
#define DMA_PDATAALIGN_WORD          2u

#define JPEG_420_SUBSAMPLING         0u
#define JPEG_422_SUBSAMPLING         1u
#define JPEG_444_SUBSAMPLING         2u
#define QSPI_DDR_HHC_ANALOG_DELAY    0u
#define QSPI_DDR_HHC_HALF_CLK_DELAY  1u
#define QSPI_MATCH_MODE_AND          0u
#define QSPI_AUTOMATIC_STOP_ENABLE   1u
#define JPEG_CHROMA_SAMPLING_420     0u
#define JPEG_CHROMA_SAMPLING_422     1u
#define JPEG_CHROMA_SAMPLING_444     2u
#define JPEG_YCBCR_COLORSPACE        0u
#define JPEG_GRAYSCALE_COLORSPACE    1u
#define JPEG_CMYK_COLORSPACE         2u

#define SPI_CFG1_TXDMAEN     (1u << 15)
#define SPI_CFG1_RXDMAEN     (1u << 14)
#define SPI_CR1_SPE          (1u << 0)
#define SPI_CR1_CSTART       (1u << 9)
#define SPI_CFG2_COMM        (3u << 17)
#define SPI_CFG2_COMM_0      (1u << 17)
#define SPI_CFG1_DSIZE       (0x1Fu)
#define SPI_IFCR_EOTC        (1u << 3)
#define SPI_IFCR_TXTFC       (1u << 4)
#define SPI_SR_EOT           (1u << 3)
#define SPI_SR_TXP           (1u << 1)
#define SPI_IFCR_OVRC        (1u << 6)
#define SPI_IFCR_UDRC        (1u << 5)
#define SPI_IFCR_SUSPC       (1u << 11)
#define SPI_IFCR_MODFC       (1u << 9)
#define SPI_IFCR_TSERFC      (1u << 10)
#define SPI_IFCR_CRCEC       (1u << 7)
#define SPI_IFCR_TIFREC      (1u << 8)

#define HAL_SPI_ERROR_NONE           0u
#define HAL_SPI_STATE_BUSY_TX        3u
#define HAL_SPI_STATE_BUSY           2u
#define SPI_CR2_TSIZE                0xFFFFu
#define HAL_I2S_STATE_READY          1u
#define HAL_I2S_STATE_BUSY_TX        3u
#define HAL_SPI_STATE_READY          1u

/* --- more functions the examples call ---------------------------------- */
static inline HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h,
    const uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)d; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h,
    uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)d; (void)n; (void)t; return HAL_OK; }

static inline HAL_StatusTypeDef HAL_I2S_Init(I2S_HandleTypeDef *h)
{ (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2S_DeInit(I2S_HandleTypeDef *h)
{ (void)h; return HAL_OK; }
static inline uint32_t HAL_I2S_GetState(I2S_HandleTypeDef *h)
{ (void)h; return HAL_I2S_STATE_READY; }
static inline uint32_t HAL_SPI_GetState(SPI_HandleTypeDef *h)
{ (void)h; return HAL_SPI_STATE_READY; }
static inline HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef_s *h)
{ (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef_s *h)
{ (void)h; return HAL_OK; }

static inline HAL_StatusTypeDef HAL_QSPI_Command(QSPI_HandleTypeDef *h,
    QSPI_CommandTypeDef *c, uint32_t t) { (void)h; (void)c; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_Transmit(QSPI_HandleTypeDef *h,
    uint8_t *d, uint32_t t) { (void)h; (void)d; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_Receive(QSPI_HandleTypeDef *h,
    uint8_t *d, uint32_t t) { (void)h; (void)d; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_AutoPolling(QSPI_HandleTypeDef *h,
    QSPI_CommandTypeDef *c, void *cfg, uint32_t t)
{ (void)h; (void)c; (void)cfg; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_MemoryMapped(QSPI_HandleTypeDef *h,
    QSPI_CommandTypeDef *c, QSPI_MemoryMappedTypeDef *m)
{ (void)h; (void)c; (void)m; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_Abort(QSPI_HandleTypeDef *h)
{ (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_Transmit_DMA(QSPI_HandleTypeDef *h,
    uint8_t *d) { (void)h; (void)d; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_QSPI_Receive_DMA(QSPI_HandleTypeDef *h,
    uint8_t *d) { (void)h; (void)d; return HAL_OK; }

/* I2C — پل EEPROM i2c_eeprom_dma.h (فقط سینتکس) */
typedef struct {
    uint32_t Instance, AddressingMode, DualAddressMode, OwnAddress1,
             OwnAddress2, GeneralCallMode, NoStretchMode;
} I2C_InitTypeDef;
typedef struct {
    uint32_t Instance; uint32_t State; uint32_t ErrorCode;
    I2C_InitTypeDef Init;
    DMA_HandleTypeDef_s *hdmatx;
    DMA_HandleTypeDef_s *hdmarx;
} I2C_HandleTypeDef;
#define HAL_I2C_STATE_READY     0x20u
#define HAL_I2C_ERROR_NONE      0u
#define I2C_MEMADD_SIZE_8BIT    1u
#define I2C_MEMADD_SIZE_16BIT   2u
static inline HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h,
    uint16_t a, uint16_t m, uint16_t ms, uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)a; (void)m; (void)ms; (void)d; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h,
    uint16_t a, uint16_t m, uint16_t ms, uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)a; (void)m; (void)ms; (void)d; (void)n; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2C_Mem_Read_DMA(I2C_HandleTypeDef *h,
    uint16_t a, uint16_t m, uint16_t ms, uint8_t *d, uint16_t n)
{ (void)h; (void)a; (void)m; (void)ms; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2C_Mem_Write_DMA(I2C_HandleTypeDef *h,
    uint16_t a, uint16_t m, uint16_t ms, uint8_t *d, uint16_t n)
{ (void)h; (void)a; (void)m; (void)ms; (void)d; (void)n; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h,
    uint16_t a, uint32_t trials, uint32_t t)
{ (void)h; (void)a; (void)trials; (void)t; return HAL_OK; }

static inline HAL_StatusTypeDef HAL_JPEG_ConfigEncoding(JPEG_HandleTypeDef *h,
    JPEG_ConfTypeDef *c) { (void)h; (void)c; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_JPEG_GetInfo(JPEG_HandleTypeDef *h,
    JPEG_ConfTypeDef *c) { (void)h; (void)c; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_JPEG_Decode(JPEG_HandleTypeDef *h,
    uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outlen, uint32_t t)
{ (void)h; (void)in; (void)inlen; (void)out; (void)outlen; (void)t; return HAL_OK; }

/* ------------------------------------------------------------------ */
/* MPU — enough of it to syntax-check a board MPU_Config()             */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t  Enable;
    uint8_t  Number;
    uint32_t BaseAddress;
    uint8_t  Size;
    uint8_t  SubRegionDisable;
    uint8_t  TypeExtField;
    uint8_t  AccessPermission;
    uint8_t  DisableExec;
    uint8_t  IsShareable;
    uint8_t  IsCacheable;
    uint8_t  IsBufferable;
} MPU_Region_InitTypeDef;

#define MPU_REGION_ENABLE            1u
#define MPU_REGION_DISABLE           0u

#define MPU_REGION_NUMBER0           0u
#define MPU_REGION_NUMBER1           1u
#define MPU_REGION_NUMBER2           2u
#define MPU_REGION_NUMBER3           3u
#define MPU_REGION_NUMBER4           4u
#define MPU_REGION_NUMBER5           5u
#define MPU_REGION_NUMBER6           6u
#define MPU_REGION_NUMBER7           7u

#define MPU_REGION_SIZE_256B         7u
#define MPU_REGION_SIZE_1KB          9u
#define MPU_REGION_SIZE_32KB         14u
#define MPU_REGION_SIZE_64KB         15u
#define MPU_REGION_SIZE_128KB        16u
#define MPU_REGION_SIZE_256KB        17u
#define MPU_REGION_SIZE_512KB        18u
#define MPU_REGION_SIZE_1MB          19u
#define MPU_REGION_SIZE_2MB          20u
#define MPU_REGION_SIZE_4MB          21u
#define MPU_REGION_SIZE_8MB          22u
#define MPU_REGION_SIZE_16MB         23u
#define MPU_REGION_SIZE_32MB         24u
#define MPU_REGION_SIZE_64MB         25u
#define MPU_REGION_SIZE_4GB          31u

#define MPU_TEX_LEVEL0               0u
#define MPU_TEX_LEVEL1               1u
#define MPU_TEX_LEVEL2               2u

#define MPU_REGION_NO_ACCESS         0u
#define MPU_REGION_PRIV_RW           1u
#define MPU_REGION_PRIV_RW_URO       2u
#define MPU_REGION_FULL_ACCESS       3u
#define MPU_REGION_PRIV_RO           5u
#define MPU_REGION_PRIV_RO_URO       6u

#define MPU_INSTRUCTION_ACCESS_ENABLE   0u
#define MPU_INSTRUCTION_ACCESS_DISABLE  1u
#define MPU_ACCESS_SHAREABLE            1u
#define MPU_ACCESS_NOT_SHAREABLE        0u
#define MPU_ACCESS_CACHEABLE            1u
#define MPU_ACCESS_NOT_CACHEABLE        0u
#define MPU_ACCESS_BUFFERABLE           1u
#define MPU_ACCESS_NOT_BUFFERABLE       0u

#define MPU_PRIVILEGED_DEFAULT       0x04u
#define MPU_HFNMI_PRIVDEF            0x06u

/* --- DMA2D (Chrom-Art) ------------------------------------------- */
typedef struct {
    uint32_t InputOffset, InputColorMode, AlphaMode, InputAlpha,
             ChromaSubSampling, RedBlueSwap, AlphaInverted;
} DMA2D_LayerCfgTypeDef;
typedef struct {
    uint32_t Mode, ColorMode, OutputOffset, BytesSwap, LineOffsetMode;
} DMA2D_InitTypeDef;
typedef struct {
    uint32_t Instance;
    DMA2D_InitTypeDef Init;
    DMA2D_LayerCfgTypeDef LayerCfg[2];
} DMA2D_HandleTypeDef;

#define DMA2D_M2M_PFC            1u
#define DMA2D_OUTPUT_RGB888      1u
#define DMA2D_OUTPUT_RGB565      2u
#define DMA2D_INPUT_YCBCR        11u
#define DMA2D_NO_CSS             0u
#define DMA2D_CSS_422            1u
#define DMA2D_CSS_420            2u
#define DMA2D_REPLACE_ALPHA      1u
#define DMA2D_REGULAR_ALPHA      0u
#define DMA2D_RB_REGULAR         0u
#define DMA2D_BYTES_REGULAR      0u
#define DMA2D_BYTES_SWAP         1u
#define DMA2D_INPUT_RGB888       1u
#define DMA2D_NO_MODIF_ALPHA     0u
#define DMA2D_RB_SWAP            1u
#define DMA2D_LOM_PIXELS         0u

#ifndef __HAL_RCC_DMA2D_CLK_ENABLE
#define __HAL_RCC_DMA2D_CLK_ENABLE() do { } while (0)
#endif

static inline HAL_StatusTypeDef HAL_DMA2D_Init(DMA2D_HandleTypeDef *h)
{ (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_DMA2D_DeInit(DMA2D_HandleTypeDef *h)
{ (void)h; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_DMA2D_ConfigLayer(DMA2D_HandleTypeDef *h,
    uint32_t l) { (void)h; (void)l; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_DMA2D_Start(DMA2D_HandleTypeDef *h,
    uint32_t s, uint32_t d, uint32_t w, uint32_t y)
{ (void)h; (void)s; (void)d; (void)w; (void)y; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_DMA2D_PollForTransfer(
    DMA2D_HandleTypeDef *h, uint32_t t) { (void)h; (void)t; return HAL_OK; }

/* --- LTDC ---------------------------------------------------------- */
typedef struct {
    uint32_t WindowX0, WindowX1, WindowY0, WindowY1;
    uint32_t PixelFormat, Alpha, Alpha0, BlendingFactor1, BlendingFactor2;
    uint32_t FBStartAdress, ImageWidth, ImageHeight, Backcolor;
} LTDC_LayerCfgTypeDef;
typedef struct {
    uint32_t Instance;
    uint32_t State;
    LTDC_LayerCfgTypeDef LayerCfg[2];
} LTDC_HandleTypeDef;

#define LTDC_PIXEL_FORMAT_RGB565    2u
#define LTDC_PIXEL_FORMAT_RGB888    1u
#define LTDC_PIXEL_FORMAT_ARGB8888  0u
#define LTDC_LAYER_1                0u

static inline HAL_StatusTypeDef HAL_LTDC_SetAddress(LTDC_HandleTypeDef *h,
    uint32_t a, uint32_t l) { (void)h; (void)a; (void)l; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_LTDC_ConfigLayer(LTDC_HandleTypeDef *h,
    LTDC_LayerCfgTypeDef *c, uint32_t l)
{ (void)h; (void)c; (void)l; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_LTDC_SetPixelFormat(
    LTDC_HandleTypeDef *h, uint32_t f, uint32_t l)
{ (void)h; (void)f; (void)l; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_LTDC_ProgramLineEvent(
    LTDC_HandleTypeDef *h, uint32_t l) { (void)h; (void)l; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_LTDC_Reload(LTDC_HandleTypeDef *h,
    uint32_t t) { (void)h; (void)t; return HAL_OK; }
#define LTDC_RELOAD_VERTICAL_BLANKING 1u
#define LTDC_RELOAD_IMMEDIATE         0u

static inline void HAL_MPU_Disable(void) { }
static inline void HAL_MPU_Enable(uint32_t x) { (void)x; }
static inline void HAL_MPU_ConfigRegion(MPU_Region_InitTypeDef *r) { (void)r; }

#endif
