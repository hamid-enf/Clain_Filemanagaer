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

void CmdMusic_Next(void)
{
    printf("next\n");
    SimpleAudio_Next();
}

void CmdMusic_Prev(void)
{
    printf("prev\n");
    SimpleAudio_Prev();
}

void CmdMusic_Forward10(void)
{
    printf("np10\n");
//    SimpleAudio_SeekTo(10.0);
    SimpleAudio_SeekBy(+10.0);
}

void CmdMusic_Back10(void)
{
    printf("pn10\n");
    SimpleAudio_SeekBy(-10.0);
}

void CmdMusic_TogglePause(void) {
	printf("pauset \n");
	SimpleAudio_TogglePause();
}
void CmdMusic_Pause(void) {
	printf("pause \n");
	SimpleAudio_Pause();
}
void CmdMusic_Resume(void) {
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

void CmdMusic_Start(void)
{
    printf("start\n");
    music_cmd = 1;
}

void CmdMusic_Stop(void)
{
    printf("stop\n");
    music_cmd = 2;
}

SimpleAudioInfo g_music_info;
void CmdMusic_Info(void)
{
    printf("info\n");
    SimpleAudio_Info(&g_music_info);
    printf("bit:%d - channels:%d - codec:%s  - duration:%0.2f -  name:%s -  playlist_count:%lu -  playlist_index:%lu  -sample_rate:%lu  - volume_pct:%d  \n",g_music_info.bits,g_music_info.channels,g_music_info.codec,g_music_info.duration,
    		g_music_info.name,(unsigned long)g_music_info.playlist_count,(unsigned long)g_music_info.playlist_index,(unsigned long)g_music_info.sample_rate,g_music_info.volume_pct);
}

void CmdMusic_Volume50(void)
{
    printf("vol50\n");
    SimpleAudio_SetVolume(50);
//    SimpleAudio_SetVolume(g_music_info.volume_pct + 5);
}

void CmdMusic_Volume90(void)
{
    printf("vol90\n");
    SimpleAudio_SetVolume(90);
//    SimpleAudio_SetVolume(g_music_info.volume_pct + 5);
}

/* -------------------------------------------------------------------------- */
/* Command Parser                                                             */
/* -------------------------------------------------------------------------- */

#include "../top_api/code_cmd.h"

#endif /* FM_BOARD_APP_UART_COMMANDS_H_ */
