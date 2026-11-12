/*
 * spi_tft_dma.h — STM32H7 SPI TFT DMA (generic, not an ILI9341 / LVGL driver)
 *
 * FileManager فقط پیکسل می‌دهد. پنجره/DC/CS و GUI مال برد است.
 *
 * مشکل HAL_SPI_Transmit_DMA روی H7: هر تکه SPI را با TSIZE جدید ریست
 * می‌کند و یک بایت در مرز کم/زیاد می‌شود → شیفت و رنگین‌کمان. اینجا
 * TSIZE=0 است؛ SPI تا آخر فریم روشن می‌ماند و DMA فقط TXDR را پر می‌کند.
 * bounce در D1/D2 (DMA1 SDRAM/DTCM را نمی‌خواند). ping-pong تا memcpy
 * تکهٔ بعد با SPI تکهٔ فعلی هم‌پوشانی کند.
 *
 * CubeMX: Mem→Periph, Normal, Mem inc, Byte/Byte, FIFO ¼. NVIC استریم DMA.
 *
 *   LcdSpiDma_Bind(&hspi2, LCD_CS_GPIO_Port, LCD_CS_Pin);
 *
 *   void App_LcdBlit(const void *px, uint16_t w, uint16_t h,
 *                    FM_PixelFormat fmt, size_t bytes)
 *   {
 *       (void)fmt;
 *       ILI9341_Set_Address(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
 *       HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
 *       (void)LcdSpiDma_Send(px, bytes);
 *   }
 *
 * LVGL (v8/v9) — همان Send، فقط ناحیهٔ dirty:
 *
 *   void my_flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
 *   {
 *       uint16_t w = (uint16_t)(a->x2 - a->x1 + 1);
 *       uint16_t h = (uint16_t)(a->y2 - a->y1 + 1);
 *       ILI9341_Set_Address((uint16_t)a->x1, (uint16_t)a->y1,
 *                           (uint16_t)a->x2, (uint16_t)a->y2);
 *       HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
 *       (void)LcdSpiDma_Send(px, (size_t)w * (size_t)h * 2u);
 *       lv_display_flush_ready(disp);          // بعد از Send = blocking
 *   }
 *
 * LVGL رنگ را RGB565 بگذار (یا BGR565 مثل پنل). اگر SWAP16 را SimpleImage
 * انجام می‌دهد، LVGL را روی native LE بگذار و swap نکن؛ وگرنه دو بار swap
 * می‌شود. FileManager را با lv_conf.h قاطی نکن — flush مال main.c است.
 *
 * ویدیو: LcdSpiDma_Wait(); LcdSpiDma_Begin(); در while LcdSpiDma_Poll().
 */

#ifndef FM_TOPAPI_SPI_TFT_DMA_H
#define FM_TOPAPI_SPI_TFT_DMA_H


#ifndef SPI_TFT_DMA_H_
#define SPI_TFT_DMA_H_

#include <stddef.h>
#include "fm_board_config.h"
#include "fm_cache.h"          /* نگهداری کش — بی‌اثر اگر کش خاموش باشد */
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "spi.h"
#include "fm_log.h"
#include "file_manager.h"

#ifndef LCD_SPI_DMA_SECTION
#define LCD_SPI_DMA_SECTION ".dma_buffer"
#endif
#ifndef LCD_SPI_DMA_CHUNK
#define LCD_SPI_DMA_CHUNK 32768u
#endif
#ifndef LCD_SPI_DMA_PINGPONG
#define LCD_SPI_DMA_PINGPONG 1
#endif
#ifndef LCD_SPI_DMA_TIMEOUT_MS
#define LCD_SPI_DMA_TIMEOUT_MS 500u
#endif
/* During the SD-to-LCD SPI DMA transfers (especially the ~36 ms video
 * present wait) the CPU is otherwise spinning in a tight loop.  With
 * this off, the I2S refill for AVI+PCM video is starved exactly when
 * the decoder reaches the present wait quickly (320x240 clips), and the
 * DMA replays stale ring halves — the "بریده بریده" audio.  FM_Idle()
 * is the normal cooperative scheduler: it fans out to the currently
 * active player's refill hook (music poll / video I2S poll), so letting
 * the SPI waits yield refills audio without touching the SPI timing. */
#ifndef LCD_SPI_DMA_IDLE
#define LCD_SPI_DMA_IDLE 1
#endif

#if LCD_SPI_DMA_CHUNK < 4u || LCD_SPI_DMA_CHUNK > 65532u
#error "LCD_SPI_DMA_CHUNK must be 4..65532"
#endif

#if LCD_SPI_DMA_PINGPONG
#define LCD_SPI_DMA_SIDES 2u
#else
#define LCD_SPI_DMA_SIDES 1u
#endif

typedef struct
{
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
    const uint8_t *src;
    size_t total;
    size_t sent;
    size_t prefetch_n;
    volatile uint8_t done;
    volatile uint32_t err;
    uint8_t running;
    uint8_t bounce_used;
    uint8_t bounce_idx;
    uint8_t prefetched;
    uint8_t spi_open;
    uint8_t need_cstart;
    uint8_t logged;
    uint32_t saved_ier;
    uint32_t saved_comm;
} LcdSpiDmaState;

static LcdSpiDmaState lcd_spi_dma;

__attribute__((section(LCD_SPI_DMA_SECTION), aligned(32)))
static uint8_t lcd_spi_dma_bounce[LCD_SPI_DMA_SIDES][LCD_SPI_DMA_CHUNK];

FM_Result LcdSpiDma_Poll(void);
FM_Result LcdSpiDma_Wait(void);

static int lcd_spi_dma_in_range(uintptr_t a, uintptr_t end,
                                uintptr_t lo, uintptr_t hi)
{
    return (a >= lo && end <= hi && a < end) ? 1 : 0;
}

static int lcd_spi_dma_can_dma(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    uintptr_t end = a + n;

    if (p == NULL || n == 0u)
    {
        return 0;
    }
    if ((a & 3u) != 0u)
    {
        return 0;
    }
    if (lcd_spi_dma_in_range(a, end, 0x24000000u, 0x24100000u) ||
        lcd_spi_dma_in_range(a, end, 0x30000000u, 0x30080000u) ||
        lcd_spi_dma_in_range(a, end, 0x38000000u, 0x38010000u))
    {
        return 1;
    }
    return 0;
}

static void lcd_spi_dma_cs(uint8_t low)
{
    if (lcd_spi_dma.cs_port == NULL)
    {
        return;
    }
    HAL_GPIO_WritePin(lcd_spi_dma.cs_port, lcd_spi_dma.cs_pin,
                      low ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static size_t lcd_spi_dma_step(void)
{
    uint32_t align;

    if (lcd_spi_dma.hspi == NULL || lcd_spi_dma.hspi->hdmatx == NULL)
    {
        return 1u;
    }
    align = lcd_spi_dma.hspi->hdmatx->Init.MemDataAlignment;
    if (align == DMA_MDATAALIGN_WORD)
    {
        return 4u;
    }
    if (align == DMA_MDATAALIGN_HALFWORD)
    {
        return 2u;
    }
    return 1u;
}

static uint32_t lcd_spi_dma_beats(size_t n)
{
    size_t step = lcd_spi_dma_step();

    return (step == 0u) ? 0u : (uint32_t)(n / step);
}

static size_t lcd_spi_dma_chunk_n(size_t left)
{
    size_t n = LCD_SPI_DMA_CHUNK;
    size_t step = lcd_spi_dma_step();

    if (n > left)
    {
        n = left;
    }
    if (step > 1u)
    {
        n -= (n % step);
    }
    return n;
}

static void lcd_spi_dma_clean(const void *p, size_t n)
{
    FM_Cache_CleanForDma(p, n);
}

void LcdSpiDma_OnTxCplt(SPI_HandleTypeDef *hspi)
{
    (void)hspi;
}

void LcdSpiDma_OnError(SPI_HandleTypeDef *hspi)
{
    if (lcd_spi_dma.hspi != NULL && hspi == lcd_spi_dma.hspi)
    {
        lcd_spi_dma.err = hspi->ErrorCode;
        lcd_spi_dma.done = 1u;
    }
}

static void lcd_spi_dma_dma_cplt(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    lcd_spi_dma.done = 1u;
}

static void lcd_spi_dma_dma_err(DMA_HandleTypeDef *hdma)
{
    lcd_spi_dma.err = (hdma != NULL) ? hdma->ErrorCode : 1u;
    lcd_spi_dma.done = 1u;
}

void LcdSpiDma_Bind(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port,
                    uint16_t cs_pin)
{
    memset(&lcd_spi_dma, 0, sizeof(lcd_spi_dma));
    lcd_spi_dma.hspi = hspi;
    lcd_spi_dma.cs_port = cs_port;
    lcd_spi_dma.cs_pin = cs_pin;
#if defined(DMA_PRIORITY_LOW) && !defined(FM_HOST_BUILD)
    /* LCD اولویت DMA پایین می‌گیرد تا استریم I2S (صدا) همیشه زودتر
     * سرویس شود — وگرنه پرش‌های بزرگ LCD باعث underrun صدا و
     * «خش‌خش/بریده‌بریده» می‌شوند (توضیح: SimpleI2S_BoostDmaPriority). */
    if (hspi != NULL && hspi->hdmatx != NULL &&
        hspi->hdmatx->Init.Priority > DMA_PRIORITY_LOW)
    {
        hspi->hdmatx->Init.Priority = DMA_PRIORITY_LOW;
        (void)HAL_DMA_DeInit(hspi->hdmatx);
        (void)HAL_DMA_Init(hspi->hdmatx);
    }
#endif
    FM_Log("LcdSpiDma: bounce=%08lX chunk=%u pingpong=%u\n",
           (unsigned long)(uintptr_t)&lcd_spi_dma_bounce[0][0],
           (unsigned)LCD_SPI_DMA_CHUNK, (unsigned)LCD_SPI_DMA_PINGPONG);
    if (!lcd_spi_dma_can_dma(&lcd_spi_dma_bounce[0][0], LCD_SPI_DMA_CHUNK))
    {
        FM_Log("LcdSpiDma: bounce is NOT DMA-capable (DTCM/SDRAM). "
               "Put .dma_buffer in AXI-SRAM 0x24000000 or D2 0x30000000\n");
    }
}

static void lcd_spi_dma_restore(void)
{
    SPI_HandleTypeDef *h = lcd_spi_dma.hspi;
    SPI_TypeDef *s;

    if (h == NULL)
    {
        return;
    }
    s = h->Instance;
    CLEAR_BIT(s->CFG1, SPI_CFG1_TXDMAEN);
    CLEAR_BIT(s->CR1, SPI_CR1_SPE);
    MODIFY_REG(s->CFG2, SPI_CFG2_COMM, lcd_spi_dma.saved_comm);
#ifdef SPI_IFCR_EOTC
    s->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_OVRC | SPI_IFCR_UDRC |
              SPI_IFCR_SUSPC;
#endif
    s->IER = lcd_spi_dma.saved_ier;
    h->ErrorCode = HAL_SPI_ERROR_NONE;
    h->State = HAL_SPI_STATE_READY;
    lcd_spi_dma.spi_open = 0u;
    lcd_spi_dma.running = 0u;
    lcd_spi_dma.prefetched = 0u;
    lcd_spi_dma_cs(0);
}

static FM_Result lcd_spi_dma_wait_txc(void)
{
    SPI_TypeDef *s;
    uint32_t t0;

    if (lcd_spi_dma.hspi == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    s = lcd_spi_dma.hspi->Instance;
    t0 = HAL_GetTick();
#ifdef SPI_SR_TXC
    while ((s->SR & SPI_SR_TXC) == 0u)
    {
#if LCD_SPI_DMA_IDLE
        FM_Idle();
#endif
        if ((HAL_GetTick() - t0) > LCD_SPI_DMA_TIMEOUT_MS)
        {
#ifdef SPI_CR1_CSUSP
            SET_BIT(s->CR1, SPI_CR1_CSUSP);
#endif
            FM_Log("LcdSpiDma: TXC timeout SR=%08lX\n", (unsigned long)s->SR);
            return FM_ERR_TIMEOUT;
        }
    }
#else
    {
        uint32_t spins;

        for (spins = 0u; spins < 1024u; spins++)
        {
            (void)s->SR;
        }
    }
#endif
    return FM_OK;
}

static FM_Result lcd_spi_dma_open(void)
{
    SPI_HandleTypeDef *h = lcd_spi_dma.hspi;
    SPI_TypeDef *s;

    if (h == NULL || h->hdmatx == NULL)
    {
        FM_Log("LcdSpiDma: no hdmatx — CubeMX SPI TX DMA?\n");
        return FM_ERR_NOT_READY;
    }
    s = h->Instance;
    if (h->State != HAL_SPI_STATE_READY)
    {
        (void)HAL_SPI_Abort(h);
    }
    CLEAR_BIT(s->CR1, SPI_CR1_SPE);
    lcd_spi_dma.saved_ier = s->IER;
    s->IER = 0u;
    lcd_spi_dma.saved_comm = s->CFG2 & SPI_CFG2_COMM;
#ifdef SPI_DIRECTION_2LINES_TXONLY
    MODIFY_REG(s->CFG2, SPI_CFG2_COMM, SPI_DIRECTION_2LINES_TXONLY);
#endif
    CLEAR_BIT(s->CFG1, SPI_CFG1_RXDMAEN);
    SET_BIT(s->CFG1, SPI_CFG1_TXDMAEN);
    MODIFY_REG(s->CR2, SPI_CR2_TSIZE, 0u);
#ifdef SPI_IFCR_EOTC
    s->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_OVRC | SPI_IFCR_UDRC |
              SPI_IFCR_SUSPC;
#endif
    h->State = HAL_SPI_STATE_BUSY_TX;
    lcd_spi_dma_cs(1);
    SET_BIT(s->CR1, SPI_CR1_SPE);
    lcd_spi_dma.need_cstart = 1u;
    lcd_spi_dma.spi_open = 1u;
    return FM_OK;
}

static FM_Result lcd_spi_dma_write_tail(const uint8_t *p, size_t n)
{
    SPI_TypeDef *s;
    size_t i;
    uint32_t t0;

    if (n == 0u)
    {
        return FM_OK;
    }
    if (lcd_spi_dma.spi_open == 0u)
    {
        FM_Result r = lcd_spi_dma_open();

        if (r != FM_OK)
        {
            return r;
        }
    }
    s = lcd_spi_dma.hspi->Instance;
    for (i = 0; i < n; i++)
    {
        t0 = HAL_GetTick();
        while ((s->SR & SPI_SR_TXP) == 0u)
        {
#if LCD_SPI_DMA_IDLE
            FM_Idle();
#endif
            if ((HAL_GetTick() - t0) > LCD_SPI_DMA_TIMEOUT_MS)
            {
                return FM_ERR_TIMEOUT;
            }
        }
        *((__IO uint8_t *)&s->TXDR) = p[i];
    }
    return FM_OK;
}

static FM_Result lcd_spi_dma_start_dma(const uint8_t *ptr, size_t n)
{
    DMA_HandleTypeDef *hdma;
    uint32_t beats;

    hdma = lcd_spi_dma.hspi->hdmatx;
    hdma->XferCpltCallback = lcd_spi_dma_dma_cplt;
    hdma->XferErrorCallback = lcd_spi_dma_dma_err;
    hdma->XferHalfCpltCallback = NULL;
    lcd_spi_dma_clean(ptr, n);
    lcd_spi_dma.done = 0u;
    lcd_spi_dma.err = 0u;
    beats = lcd_spi_dma_beats(n);
    if (beats == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (HAL_DMA_Start_IT(hdma, (uint32_t)ptr,
                         (uint32_t)&lcd_spi_dma.hspi->Instance->TXDR,
                         beats) != HAL_OK)
    {
        FM_Log("LcdSpiDma: HAL_DMA_Start_IT fail dma_err=%lu\n",
               (unsigned long)hdma->ErrorCode);
        return FM_ERR_IO;
    }
    if (lcd_spi_dma.need_cstart != 0u)
    {
        SET_BIT(lcd_spi_dma.hspi->Instance->CR1, SPI_CR1_CSTART);
        lcd_spi_dma.need_cstart = 0u;
    }
    return FM_OK;
}

static FM_Result lcd_spi_dma_close_frame(void)
{
    FM_Result r = lcd_spi_dma_wait_txc();

    lcd_spi_dma_restore();
    return r;
}

static void lcd_spi_dma_fill(uint8_t side, const uint8_t *src, size_t n)
{
    memcpy(lcd_spi_dma_bounce[side], src, n);
}

static void lcd_spi_dma_prefetch(void)
{
#if LCD_SPI_DMA_PINGPONG
    size_t left;
    size_t n;

    if (lcd_spi_dma.bounce_used == 0u || lcd_spi_dma.prefetched != 0u)
    {
        return;
    }
    left = lcd_spi_dma.total - lcd_spi_dma.sent;
    n = lcd_spi_dma_chunk_n(left);
    if (n == 0u)
    {
        return;
    }
    lcd_spi_dma_fill(lcd_spi_dma.bounce_idx, lcd_spi_dma.src + lcd_spi_dma.sent,
                     n);
    lcd_spi_dma.prefetch_n = n;
    lcd_spi_dma.prefetched = 1u;
#endif
}

static FM_Result lcd_spi_dma_kick(void)
{
    const uint8_t *ptr;
    size_t left;
    size_t n;
    uint8_t side;
    FM_Result r;

    if (lcd_spi_dma.hspi == NULL || lcd_spi_dma.src == NULL)
    {
        return FM_ERR_NOT_READY;
    }
    left = lcd_spi_dma.total - lcd_spi_dma.sent;
    if (left == 0u)
    {
        return lcd_spi_dma_close_frame();
    }
    n = lcd_spi_dma_chunk_n(left);
    if (n == 0u)
    {
        r = lcd_spi_dma_write_tail(lcd_spi_dma.src + lcd_spi_dma.sent, left);
        lcd_spi_dma.sent = lcd_spi_dma.total;
        if (r != FM_OK)
        {
            lcd_spi_dma_restore();
            return r;
        }
        return lcd_spi_dma_close_frame();
    }
    if (lcd_spi_dma.bounce_used != 0u)
    {
        side = lcd_spi_dma.bounce_idx;
        if (lcd_spi_dma.prefetched != 0u)
        {
            n = lcd_spi_dma.prefetch_n;
            lcd_spi_dma.prefetched = 0u;
        }
        else
        {
            lcd_spi_dma_fill(side, lcd_spi_dma.src + lcd_spi_dma.sent, n);
        }
        ptr = lcd_spi_dma_bounce[side];
#if LCD_SPI_DMA_PINGPONG
        lcd_spi_dma.bounce_idx = (uint8_t)(side ^ 1u);
#endif
    }
    else
    {
        ptr = lcd_spi_dma.src + lcd_spi_dma.sent;
    }
    if (lcd_spi_dma.spi_open == 0u)
    {
        r = lcd_spi_dma_open();
        if (r != FM_OK)
        {
            return r;
        }
    }
    r = lcd_spi_dma_start_dma(ptr, n);
    if (r != FM_OK)
    {
        lcd_spi_dma_restore();
        return r;
    }
    lcd_spi_dma.sent += n;
    lcd_spi_dma.running = 1u;
    lcd_spi_dma_prefetch();
    return FM_OK;
}

static FM_Result lcd_spi_dma_wait_chunk(void)
{
    uint32_t t0 = HAL_GetTick();

    while (lcd_spi_dma.done == 0u)
    {
#if LCD_SPI_DMA_IDLE
        FM_Idle();
#endif
        if ((HAL_GetTick() - t0) > LCD_SPI_DMA_TIMEOUT_MS)
        {
            FM_Log("LcdSpiDma: DMA TC timeout dma_err=%lu\n",
                   (lcd_spi_dma.hspi != NULL &&
                    lcd_spi_dma.hspi->hdmatx != NULL)
                       ? (unsigned long)lcd_spi_dma.hspi->hdmatx->ErrorCode
                       : 0ul);
            if (lcd_spi_dma.hspi != NULL && lcd_spi_dma.hspi->hdmatx != NULL)
            {
                (void)HAL_DMA_Abort(lcd_spi_dma.hspi->hdmatx);
            }
            lcd_spi_dma_restore();
            return FM_ERR_TIMEOUT;
        }
    }
    if (lcd_spi_dma.err != 0u)
    {
        FM_Log("LcdSpiDma: DMA err=%lu (FEIF→FIFO on)\n",
               (unsigned long)lcd_spi_dma.err);
        lcd_spi_dma_restore();
        return FM_ERR_IO;
    }
    return FM_OK;
}

static FM_Result lcd_spi_dma_start(const void *data, size_t bytes, uint8_t wait)
{
    FM_Result r;

    if (lcd_spi_dma.hspi == NULL)
    {
        FM_Log("LcdSpiDma: Bind() first\n");
        return FM_ERR_NOT_READY;
    }
    if (data == NULL && bytes != 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (lcd_spi_dma.running != 0u || lcd_spi_dma.spi_open != 0u)
    {
        r = LcdSpiDma_Wait();
        if (r != FM_OK)
        {
            return r;
        }
    }
    if (bytes == 0u)
    {
        return FM_OK;
    }
    lcd_spi_dma.src = (const uint8_t *)data;
    lcd_spi_dma.total = bytes;
    lcd_spi_dma.sent = 0;
    lcd_spi_dma.bounce_idx = 0u;
    lcd_spi_dma.prefetched = 0u;
    lcd_spi_dma.bounce_used = lcd_spi_dma_can_dma(data, bytes) ? 0u : 1u;
    if (lcd_spi_dma.logged == 0u)
    {
        lcd_spi_dma.logged = 1u;
        FM_Log("LcdSpiDma: src=%08lX bytes=%lu bounce=%u step=%u\n",
               (unsigned long)(uintptr_t)data, (unsigned long)bytes,
               (unsigned)lcd_spi_dma.bounce_used,
               (unsigned)lcd_spi_dma_step());
    }
    r = lcd_spi_dma_kick();
    if (r != FM_OK)
    {
        return r;
    }
    if (wait == 0u)
    {
        return FM_OK;
    }
    return LcdSpiDma_Wait();
}

FM_Result LcdSpiDma_Poll(void)
{
    if (lcd_spi_dma.running == 0u)
    {
        return FM_OK;
    }
    if (lcd_spi_dma.done == 0u)
    {
        return FM_OK;
    }
    if (lcd_spi_dma.err != 0u)
    {
        FM_Log("LcdSpiDma: DMA err=%lu\n", (unsigned long)lcd_spi_dma.err);
        lcd_spi_dma_restore();
        return FM_ERR_IO;
    }
    return lcd_spi_dma_kick();
}

FM_Result LcdSpiDma_Wait(void)
{
    FM_Result r;

    while (lcd_spi_dma.running != 0u || lcd_spi_dma.spi_open != 0u)
    {
        if (lcd_spi_dma.running != 0u && lcd_spi_dma.done == 0u)
        {
            r = lcd_spi_dma_wait_chunk();
            if (r != FM_OK)
            {
                return r;
            }
        }
        r = LcdSpiDma_Poll();
        if (r != FM_OK)
        {
            return r;
        }
        if (lcd_spi_dma.running == 0u && lcd_spi_dma.spi_open == 0u)
        {
            break;
        }
    }
    return FM_OK;
}

FM_Result LcdSpiDma_Send(const void *data, size_t bytes)
{
    return lcd_spi_dma_start(data, bytes, 1u);
}

FM_Result LcdSpiDma_Begin(const void *data, size_t bytes)
{
    return lcd_spi_dma_start(data, bytes, 0u);
}

uint8_t LcdSpiDma_Busy(void)
{
    return (lcd_spi_dma.running != 0u || lcd_spi_dma.spi_open != 0u) ? 1u : 0u;
}

#endif /* SPI_TFT_DMA_H_ */

#endif /* FM_TOPAPI_SPI_TFT_DMA_H */
