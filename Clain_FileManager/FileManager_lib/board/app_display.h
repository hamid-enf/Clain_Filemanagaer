/*
 * app_display.h — نمایشگر SPI: قلاب‌های DMA و دو تابع present
 * ===========================================================
 *
 *   HAL_SPI_TxCpltCallback / HAL_SPI_ErrorCallback
 *       پایان یا خطای انتقال DMA را به درایور نمایشگر (spi_tft_dma.h)
 *       می‌رسانند.
 *   my_lcd_blit        present اسلایدشو  (SimpleImage_SetPresent)
 *   my_lcd_blit_video  present ویدیو     (SimpleVideo_SetPresent)
 *
 * فقط از main.c شامل می‌شود (بعد از defines.h) — توضیح در board/README.md.
 */
#ifndef FM_BOARD_APP_DISPLAY_H_
#define FM_BOARD_APP_DISPLAY_H_

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    LcdSpiDma_OnTxCplt(hspi);
}
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    LcdSpiDma_OnError(hspi);
}

/*------------------------------------------------------------------------------------*/
// images

//FM_PIXEL_GRAY8    = 1,
///* 16-bit values are stored little-endian. RGB565 has R in bits 15..11;
// * BGR565 has B in bits 15..11. */
//FM_PIXEL_RGB565   = 2,
//FM_PIXEL_RGB888   = 3,  /* R,G,B */
//FM_PIXEL_RGBA8888 = 4,  /* R,G,B,A */
//FM_PIXEL_BGR565   = 5,
//FM_PIXEL_BGR888   = 6,  /* B,G,R */
//FM_PIXEL_BGRA8888 = 7   /* B,G,R,A */

static const char *fmt_s[] = {
    "GRAY8","RGB565","RGB888","RGBA8888","BGR565","BGR888","BGRA8888"
};

void my_lcd_blit(const void *px, uint16_t w, uint16_t h,
                 FM_PixelFormat fmt, size_t bytes)
{
    (void)fmt;
    if (w == 0u || h == 0u || bytes == 0u)
        return;

    /* SimpleImage_SetOutputSize(320, 240) used to be called from HERE,
     * i.e. inside the present callback.  Once the image is loaded that
     * call queues a SI_CMD_RESIZE, which the next SimpleImage_Poll turns
     * into a bilinear rescale + a second blit — every time.  The output
     * size is already set once in main() before SimpleImage_Begin(). */

    ILI9341_Set_Address(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
    HAL_GPIO_WritePin(tft_dc_GPIO_Port, tft_dc_Pin, GPIO_PIN_SET);
    (void)LcdSpiDma_Send(px, bytes);   /* عکس: blocking */

#if FM_LOG_PER_FRAME
    printf("w:%d * h:%d | fmt:%s  | size:%d \n", w, h, fmt_s[fmt-1], bytes);
#else
    (void)fmt_s;
#endif
}

void my_lcd_blit_video(const void *px, uint16_t w, uint16_t h,
                 FM_PixelFormat fmt, size_t bytes)
{
    (void)fmt;
    if (w == 0u || h == 0u || bytes == 0u)
        return;

    ILI9341_Set_Address(0, 0, (uint16_t)(w - 1u), (uint16_t)(h - 1u));
    HAL_GPIO_WritePin(tft_dc_GPIO_Port, tft_dc_Pin, GPIO_PIN_SET);
    /* FIX 2026-09: blocking present (sequential bus) restores 320
     * audio.  Async (LcdSpiDma_Begin + Wait) overlapped the 30 ms SPI
     * TX DMA with the audio SD reads inside decode N+1 — with D-Cache
     * on and a 186 ms half, the PCM bytes were fetched under contention
     * and arrived short/corrupt (starve stayed 0 but audio choppy).
     * With the video half now doubled to 372 ms and DMA priorities
     * tuned below, async can be re-enabled by defining
     * SIMPLE_VIDEO_ASYNC_PRESENT=1 for LTDC panels; SPI TFT stays
     * sequential where budget = SD+decode+SPI ≈45 ms for 320@15fps. */
#if defined(SIMPLE_VIDEO_ASYNC_PRESENT) && SIMPLE_VIDEO_ASYNC_PRESENT
    (void)LcdSpiDma_Begin(px, bytes);
#else
    (void)LcdSpiDma_Send(px, bytes);
#endif

#if FM_LOG_PER_FRAME
    printf("w:%d * h:%d | fmt:%s  | size:%d \n", w, h, fmt_s[fmt-1], bytes);
#endif
}

#endif /* FM_BOARD_APP_DISPLAY_H_ */
