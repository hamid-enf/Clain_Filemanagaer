/*
 * app_uart_commands.h — خط فرمان UART
 * ===================================
 *
 * مسیر یک دستور:
 *
 *   USART1 RX (DMA حلقوی + Idle)
 *     └─ HAL_UARTEx_RxEventCallback      وقفه: فقط خط را جدا و کپی می‌کند
 *          └─ CmdMusic_ProcessPending()  حلقهٔ اصلی: تابع دستور را صدا می‌زند
 *
 * جدول دستورها و دو تابع بالا در top_api/code_cmd.h هستند؛ این فایل بافر
 * دریافت و دستورهای سادهٔ موزیک (next، prev، start، stop، ...) را دارد.
 * هیچ کار فایل/SD/I2S داخل وقفه انجام نمی‌شود: start و stop فقط پرچم
 * music_cmd را می‌گذارند و App_MusicCommandPoll() (app_flow.h) اجرا می‌کند.
 *
 * فقط از main.c شامل می‌شود (بعد از app_video_hooks.h).
 */
#ifndef FM_BOARD_APP_UART_COMMANDS_H_
#define FM_BOARD_APP_UART_COMMANDS_H_

#define UART_RX_BUF_SIZE    20
#define CMD_BUF_SIZE        20

uint8_t uart_rx_dma_buffer[UART_RX_BUF_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));

static uint8_t data_p[CMD_BUF_SIZE];

volatile uint32_t old_pos = 0;
uint32_t current_pos = 0;
uint32_t length = 0;
uint32_t len_p = 0;

/* -------------------------------------------------------------------------- */
/* Commands                                                                   */
/* -------------------------------------------------------------------------- */

void next(void)
{
    printf("next\n");
    SimpleAudio_Next();
}

void prev(void)
{
    printf("prev\n");
    SimpleAudio_Prev();
}

void np10(void)
{
    printf("np10\n");
//    SimpleAudio_SeekTo(10.0);
    SimpleAudio_SeekBy(+10.0);
}

void pn10(void)
{
    printf("pn10\n");
    SimpleAudio_SeekBy(-10.0);
}

void pauset(void) {
	printf("pauset \n");
	SimpleAudio_TogglePause();
}
void pause(void) {
	printf("pause \n");
	SimpleAudio_Pause();
}
void resume(void) {
	printf("resume \n");
	SimpleAudio_Resume();
}

/*
 * start/stop از داخل HAL_UARTEx_RxEventCallback (وقفهٔ UART) صدا زده
 * می‌شوند. قبلاً همان‌جا SimpleVideo_Stop() + SimpleAudio_Begin() اجرا
 * می‌شد؛ یعنی داخل ISR: بستن فایل ویدیو، fade صدا با busy-wait تا ۴۰ms،
 * اسکن پوشهٔ موزیک، باز کردن فایل و پر کردن دو نیمهٔ DMA از روی SD.
 * خواندن SD منتظر وقفهٔ SDMMC/DMA می‌ماند که با اولویت مساوی/پایین‌تر
 * نمی‌تواند وقفهٔ UART را قطع کند ⇒ همان «هنگ موقع رفتن به موزیک».
 *
 * حالا فقط یک پرچم ست می‌شود و کار واقعی در حلقهٔ اصلی انجام می‌شود
 * (دقیقاً همان الگوی viedo_start). SimpleAudio_Next/Prev/Seek/Pause/
 * Volume خودشان از قبل ISR-safe هستند (فقط پرچم می‌گذارند).
 */
volatile uint8_t music_cmd = 0;   /* 1 = start music, 2 = stop music */

void start(void)
{
    printf("start\n");
    music_cmd = 1;
}

void stop(void)
{
    printf("stop\n");
    music_cmd = 2;
}

SimpleAudioInfo i;
void info(void)
{
    printf("info\n");
    SimpleAudio_Info(&i);
    printf("bit:%d - channels:%d - codec:%s  - duration:%0.2f -  name:%s -  playlist_count:%u -  playlist_index:%u  -sample_rate:%u  - volume_pct:%d  \n",i.bits,i.channels,i.codec,i.duration,
    		i.name,i.playlist_count,i.playlist_index,i.sample_rate,i.volume_pct);
}

void vol50(void)
{
    printf("vol50\n");
    SimpleAudio_SetVolume(50);
//    SimpleAudio_SetVolume(i.volume_pct + 5);
}

void vol90(void)
{
    printf("vol90\n");
    SimpleAudio_SetVolume(90);
//    SimpleAudio_SetVolume(i.volume_pct + 5);
}

/* -------------------------------------------------------------------------- */
/* Command Parser                                                             */
/* -------------------------------------------------------------------------- */

#include "../top_api/code_cmd.h"

#endif /* FM_BOARD_APP_UART_COMMANDS_H_ */
