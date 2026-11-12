/*
 * code_cmd.h — دیسپچ تمیز و بدون هنگِ دستورهای UART
 *
 *  Created on: Aug 28, 2026
 *      Author: HAMSHAHRI
 *
 *  طراحی (نسخهٔ بازنویسی‌شده):
 *
 *   UART DMA ISR  →  فقط جداکردن خط فرمان (cmd_process_uart_packet) و
 *                    کپی‌کردن آن در pending_cmd. هیچ printf/FM_Log،
 *                    هیچ کار فایل/SD/I2S اینجا انجام نمی‌شود — همهٔ
 *                    آن‌ها در ISR خطر هنگ دارند (FM_Log خودش UART است).
 *
 *   حلقهٔ اصلی    →  CmdMusic_ProcessPending(): یک snapshot قفل‌شده از
 *                    فرمان می‌گیرد و با یک جدول کوچک دیسپچ می‌کند.
 *
 *  دستورها همان نام‌های قبلی‌اند (سازگاری کامل با اسکریپت/عادت شما)،
 *  فقط چند نام استاندارد هم به‌عنوان alias اضافه شده: vstart, vpause,
 *  vresume. شاخهٔ مردهٔ تکراری vstrt (که هیچ‌وقت اجرا نمی‌شد) حذف شد.
 *
 *  اصلاح‌های نسبت به نسخهٔ قدیمی:
 *   - printf حذف از ISR (پرچم pending_overflow، چاپ از حلقهٔ اصلی)
 *   - snapshot فرمان با قفل کوتاه IRQ → دیگر فرمان نصف‌کاره اجرا نمی‌شود
 *   - دیسپچ جدولی به‌جای زنجیرهٔ if-else حرف‌به‌حرف
 *   - پیام unknown یک‌خطی و خوانا
 */

#ifndef SRC_CODE_CMD_MUSIC_H_
#define SRC_CODE_CMD_MUSIC_H_

#include "main.h"
//#include "defines.h"
#include "../src/defines.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* فرمان در انتظار: ISR می‌نویسد، حلقهٔ اصلی می‌خواند/پاک می‌کند.      */
/* ------------------------------------------------------------------ */
static uint8_t pending_cmd[CMD_BUF_SIZE];
static volatile uint8_t pending_cmd_ready;
static volatile uint32_t pending_cmd_len;

/* پرچم سرریز/فرمان طولانی — در ISR فقط ست می‌شود، چاپ در حلقهٔ اصلی. */
static volatile uint8_t pending_overflow;

/* ------------------------------------------------------------------ */
/* حالت پخش (رفتار پایان ترک/ویدیو) — قابلیت PR #24/#25:             */
/*   0 STOP        : یک ترک تمام شد → توقف (مناسب پرزنت)            */
/*   1 NEXT        : تمام شد → بعدی، آخر لیست → توقف                */
/*   2 REPEAT_ONE  : تمام شد → همین ترک دوباره                      */
/*   3 REPEAT_ALL  : تمام شد → بعدی، آخر → اول (لوپ پوشه، قدیمی)    */
/* ------------------------------------------------------------------ */
static void cmd_set_play_mode(int mode)
{
    const char *n = (mode == FM_PLAY_MODE_STOP)       ? "STOP" :
                    (mode == FM_PLAY_MODE_NEXT)       ? "NEXT" :
                    (mode == FM_PLAY_MODE_REPEAT_ONE) ? "REPEAT_ONE"
                                                      : "REPEAT_ALL";
    (void)SimpleAudio_SetPlayMode((FM_PlayMode)mode);
    (void)SimpleVideo_SetPlayMode((FM_PlayMode)mode);
    printf("play mode: %s (audio+video)\n", n);
}

/* ------------------------------------------------------------------ */
/* پوش‌های کوچک برای فرمان‌های پارامتری (جدول تابع بدون آرگومان می‌خواهد) */
/* ------------------------------------------------------------------ */
static void cmd_mode0(void) { cmd_set_play_mode((int)FM_PLAY_MODE_STOP); }
static void cmd_mode1(void) { cmd_set_play_mode((int)FM_PLAY_MODE_NEXT); }
static void cmd_mode2(void) { cmd_set_play_mode((int)FM_PLAY_MODE_REPEAT_ONE); }
static void cmd_mode3(void) { cmd_set_play_mode((int)FM_PLAY_MODE_REPEAT_ALL); }

/* --- صدا کجا برود؟ (موزیک: aoutN، صدای ویدیو: voutN) ---------------- */
static void cmd_audio_out(int video, FM_AudioOutput out)
{
    static const char *const names[4] =
        { "none", "speaker", "callback", "speaker+callback" };

    if (video)
    {
        (void)SimpleVideo_SetAudioOutput(out);
    }
    else
    {
        (void)SimpleAudio_SetOutput(out);
    }
    printf("%s audio output: %s\n", video ? "video" : "music",
           names[(unsigned)out & 3u]);
}
static void cmd_aout1(void) { cmd_audio_out(0, FM_AUDIO_OUT_SPEAKER); }
static void cmd_aout2(void) { cmd_audio_out(0, FM_AUDIO_OUT_CALLBACK); }
static void cmd_aout3(void) { cmd_audio_out(0, FM_AUDIO_OUT_BOTH); }
static void cmd_vout0(void) { cmd_audio_out(1, FM_AUDIO_OUT_NONE); }
static void cmd_vout1(void) { cmd_audio_out(1, FM_AUDIO_OUT_SPEAKER); }
static void cmd_vout2(void) { cmd_audio_out(1, FM_AUDIO_OUT_CALLBACK); }
static void cmd_vout3(void) { cmd_audio_out(1, FM_AUDIO_OUT_BOTH); }

/* --- SimpleImage (اسلایدشو) --------------------------------------- */
static void cmd_ipoll(void) { printf("SimpleImage_Poll\n");    SimpleImage_Poll(); }
static void cmd_inext(void) { printf("SimpleImage_Next\n");    SimpleImage_Next(); }
static void cmd_iprev(void) { printf("SimpleImage_Prev\n");    SimpleImage_Prev(); }
static void cmd_iro90(void) { printf("SimpleImage_Rotate 90\n"); SimpleImage_Rotate(90); }
static void cmd_iflph(void) { printf("SimpleImage_FlipH\n");   SimpleImage_FlipH(); }
static void cmd_iflpv(void) { printf("SimpleImage_FlipV\n");   SimpleImage_FlipV(); }
static void cmd_irsze(void) { printf("SimpleImage_Resize 320x240\n"); SimpleImage_Resize(320, 240); }
static void cmd_iefft(void) { printf("SimpleImage_Effect sepia\n"); SimpleImage_Effect(FM_IMAGE_EFFECT_SEPIA, 0); }
static void cmd_isavp(void) { printf("SimpleImage_Save png\n"); SimpleImage_Save("/photos/out.png"); }
static void cmd_isavb(void) { printf("SimpleImage_Save bmp\n"); SimpleImage_Save("/photos/out.bmp"); }
static void cmd_isavj(void) { printf("SimpleImage_Save jpeg\n"); SimpleImage_Save("/photos/out.jpeg"); }

/* --- SimpleVideo --------------------------------------------------- */
static void cmd_vstrt(void) { viedo_start = 1; }   /* شروع از حلقهٔ اصلی مثل قبل */
static void cmd_vstart(void){ viedo_start = 1; }   /* alias املای استاندارد */
static void cmd_vnext(void) { printf("SimpleVideo_Next\n");   SimpleVideo_Next(); }
static void cmd_vprev(void) { printf("SimpleVideo_Prev\n");   SimpleVideo_Prev(); }
static void cmd_vrsze(void) { printf("SimpleVideo_SetOutputSize 320x240\n"); SimpleVideo_SetOutputSize(320, 240); }
static void cmd_vsavp(void) { printf("SimpleVideo_SaveFrame\n"); SimpleVideo_SaveFrame("/videos/out.avi"); }
static void cmd_vstop(void) { viedo_start = 3; }   /* توقف از حلقهٔ اصلی */
static void cmd_vpaus(void) { printf("SimpleVideo_Pause\n");  SimpleVideo_Pause(); }
static void cmd_vpause(void){ printf("SimpleVideo_Pause\n");  SimpleVideo_Pause(); }  /* alias */
static void cmd_vrsme(void) { printf("SimpleVideo_Resume\n"); SimpleVideo_Resume(); }
static void cmd_vresume(void){ printf("SimpleVideo_Resume\n"); SimpleVideo_Resume(); } /* alias */
static void cmd_vnp10(void) { printf("SimpleVideo_SeekBy +10\n");  SimpleVideo_SeekBy(+10); }
static void cmd_vpn10(void) { printf("SimpleVideo_SeekBy -10\n");  SimpleVideo_SeekBy(-10); }
static void cmd_vvol9(void) { printf("SimpleVideo_SetVolume 90\n"); SimpleVideo_SetVolume(90); }
static void cmd_vvol5(void) { printf("SimpleVideo_SetVolume 50\n"); SimpleVideo_SetVolume(50); }

/* --- SD / QSPI ------------------------------------------------------ */
static void cmd_sdmcu(void) { printf("sd MCU\n"); sd_pc_mcu = 0; }
static void cmd_sdpc(void)  { printf("sd PC\n");  sd_pc_mcu = 1; }
static void cmd_fers(void)  { printf("qspi_erase\n"); qspi_erase = 1; }

/* ------------------------------------------------------------------ */
/* جدول فرمان‌ها — جستجوی خطی؛ ~۴۰ ورودی، برای UART بیش از کافی.      */
/* ------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    void (*fn)(void);
} CmdMusic_Entry;

static const CmdMusic_Entry cmd_table[] =
{
    /* --- موزیک (SimpleAudio) --- */
    { "next",   CmdMusic_Next },
    { "prev",   CmdMusic_Prev },
    { "np10",   CmdMusic_Forward10 },
    { "pn10",   CmdMusic_Back10 },
    { "stop",   CmdMusic_Stop },
    { "info",   CmdMusic_Info },
    { "start",  CmdMusic_Start },
    { "pause",  CmdMusic_Pause },
    { "pauset", CmdMusic_TogglePause },
    { "resume", CmdMusic_Resume },
    { "vol50",  CmdMusic_Volume50 },
    { "vol90",  CmdMusic_Volume90 },
    { "mode0",  cmd_mode0 },
    { "mode1",  cmd_mode1 },
    { "mode2",  cmd_mode2 },
    { "mode3",  cmd_mode3 },

    /* --- مسیر صدا: 1 = بلندگو، 2 = فقط کالبک، 3 = هر دو، 0 = هیچ --- */
    { "aout1",  cmd_aout1 },
    { "aout2",  cmd_aout2 },
    { "aout3",  cmd_aout3 },
    { "vout0",  cmd_vout0 },
    { "vout1",  cmd_vout1 },
    { "vout2",  cmd_vout2 },
    { "vout3",  cmd_vout3 },

    /* --- خواندن PCM خام (audio_record_with_tap.h) --- */
    { "dtrd",   Music_DataRead     },
    { "dtrdm",  Music_DataReadMono },

    /* --- audio_record_with_tap.h: قلاب‌ها/ضبط/کپی — همه اتوماتیک از همین‌جا ---
     * (توابع در main.c، بخش «قلاب‌های PCM» — با توضیح فارسی قبل/بعد ولوم) */
    { "tap1",    Music_TapBoostOn  },   /* نرمالایزر بلندی (AGC) روشن */
    { "tap0",    Music_TapBoostOff },   /* نرمالایزر خاموش */
    { "rectest", Music_RecTest     },   /* ضبط ۵ ثانیه از صدای بلندگو */
    { "vinfo",   Music_VideoInfo    },  /* مشخصات صدای ویدیو */
    { "pools",   Music_PoolDump     },  /* دیاگ -14: هندل + FM_Alloc + SDRAM آزاد */
    { "vvol2",   Music_VideoVol200    }, /* ولوم ویدیو ۲۰۰٪ (بلندتر از موزیک) */
    { "istart",  Music_ImageStart     }, /* اسلایدشو تصویر از حلقهٔ اصلی */
    { "EnableAudio", Music_ToggleVideoAudio }, /* صدای ویدیو خاموش/روشن */
    { "vnopres", Music_ToggleVideoPresent },   /* تست کراکل: نمایش LCD خاموش/روشن */

    /* --- SimpleImage --- */
    { "ipoll",  cmd_ipoll },
    { "inext",  cmd_inext },
    { "iprev",  cmd_iprev },
    { "iro90",  cmd_iro90 },
    { "iflph",  cmd_iflph },
    { "iflpv",  cmd_iflpv },
    { "irsze",  cmd_irsze },
    { "iefft",  cmd_iefft },
    { "isavp",  cmd_isavp },
    { "isavb",  cmd_isavb },
    { "isavj",  cmd_isavj },

    /* --- SimpleVideo --- */
    { "vstrt",  cmd_vstrt },
    { "vstart", cmd_vstart },
    { "vnext",  cmd_vnext },
    { "vprev",  cmd_vprev },
    { "vrsze",  cmd_vrsze },
    { "vsavp",  cmd_vsavp },
    { "vstop",  cmd_vstop },
    { "vpaus",  cmd_vpaus },
    { "vpause", cmd_vpause },
    { "vrsme",  cmd_vrsme },
    { "vresume",cmd_vresume },
    { "vnp10",  cmd_vnp10 },
    { "vpn10",  cmd_vpn10 },
    { "vvol9",  cmd_vvol9 },
    { "vvol5",  cmd_vvol5 },
    /* قلاب‌های شروع/پایان ویدیو + فریم داده (DSP مستقل از نمایش) */
    { "vmut",   Music_VideoMute   },   /* صدای ویدیو فقط به کالبک (= vout2) */
    { "vunmut", Music_VideoUnmute },   /* بلندگو + کالبک (= vout3) */

    /* --- ذخیرهٔ چیزی که پخش می‌شود، از کالبک‌ها (board/app_save_hooks.h) --- */
    { "arec",   Cmd_SaveMusicStart },  /* موزیک -> /save/music_cb.wav */
    { "arecs",  Cmd_SaveMusicStop  },
    { "vrec",   Cmd_SaveVideoStart },  /* ویدیو -> /save/video_cb.avi */
    { "vrecs",  Cmd_SaveVideoStop  },

    /* --- SD / QSPI --- */
    { "sdmcu",  cmd_sdmcu },
    { "sdpc",   cmd_sdpc  },
    { "fers",   cmd_fers  },
};

#define CMD_TABLE_LEN (sizeof(cmd_table) / sizeof(cmd_table[0]))

/* ------------------------------------------------------------------ */
/* دیسپچ — فقط از حلقهٔ اصلی صدا زده می‌شود.                          */
/* ------------------------------------------------------------------ */
static void cmd_dispatch(const uint8_t *data, uint32_t len)
{
    char cmd[CMD_BUF_SIZE];
    uint32_t i;
    size_t e;

    if (len == 0u || len >= CMD_BUF_SIZE)
    {
        return;                     /* فرمان خالی/بلند — قبلاً گزارش شده */
    }

    /* فقط کاراکترهای چاپی؛ ضد نویز/باینری تصادفی روی خط */
    for (i = 0u; i < len; i++)
    {
        if (data[i] < 32u || data[i] > 126u)
        {
            printf("bad char 0x%02X in command\n", (unsigned)data[i]);
            return;
        }
        cmd[i] = (char)data[i];
    }
    cmd[len] = '\0';

    for (e = 0u; e < CMD_TABLE_LEN; e++)
    {
        if (strcmp(cmd_table[e].name, cmd) == 0)
        {
            cmd_table[e].fn();
            return;
        }
    }

    printf("unknown command: %s (len %lu)\n", cmd, (unsigned long)len);
}

/* ------------------------------------------------------------------ */
/* ISR-safe front end: فقط بافر می‌کند — نه printf، نه فایل، نه I2S. */
/* ------------------------------------------------------------------ */
static void cmd_take_line(uint32_t len)
{
    if (len == 0u)
    {
        return;
    }
    if (len >= CMD_BUF_SIZE)
    {
        pending_overflow = 1u;      /* چاپ از حلقهٔ اصلی */
        return;
    }

    memcpy(pending_cmd, data_p, len);
    pending_cmd_len = len;
    pending_cmd_ready = 1u;
}

/* ------------------------------------------------------------------ */
/* اجرای فرمان بافرشده از حلقهٔ اصلی (کار واقعی اینجاست).              */
/* فرمانِ درحال‌اجرا در برابر UART ISR جدید قفل می‌شود (snapshot).      */
/* ------------------------------------------------------------------ */
void CmdMusic_ProcessPending(void)
{
    uint8_t cmd[CMD_BUF_SIZE];
    uint32_t len;
    uint32_t primask;

    if (pending_overflow != 0u)
    {
        pending_overflow = 0u;
        printf("command overflow (max %u)\n", (unsigned)(CMD_BUF_SIZE - 1u));
    }

    if (pending_cmd_ready == 0u)
    {
        return;
    }

    /* قفل کوتاه: بدون این، فرمان جدیدِ وسطِ memcpy فرمان قبلی را
     * نصف‌کاره اجرا می‌کند (تورن‌رید). چند میکروثانیه است. */
    primask = __get_PRIMASK();
    __disable_irq();
    pending_cmd_ready = 0u;
    len = pending_cmd_len;
    memcpy(cmd, (const void *)pending_cmd, len);
    if (primask == 0u)
    {
        __enable_irq();
    }

    cmd[len] = '\0';
    cmd_dispatch(cmd, len);
}

/* ------------------------------------------------------------------ */
/* جداکنندهٔ خط فرمان از بایت‌های DMA — بدون printf (در ISR اجرا می‌شود) */
/* ------------------------------------------------------------------ */
static void cmd_process_uart_packet(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        uint8_t c = data[i];

        if (c == '\n')
        {
            if (len_p > 0u && data_p[len_p - 1u] == '\r')
            {
                len_p--;
            }
            cmd_take_line(len_p);
            len_p = 0u;
        }
        else
        {
            if (len_p < CMD_BUF_SIZE - 1u)
            {
                data_p[len_p] = c;
                len_p++;
            }
            else
            {
                pending_overflow = 1u;   /* چاپ از حلقهٔ اصلی */
                len_p = 0u;              /* ریست تا ادامهٔ خط نویز نگیرد */
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* UART DMA Circular Buffer — فقط جمع‌آوری.                           */
/* ------------------------------------------------------------------ */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    uint32_t chunk;

    if (huart->Instance != USART1)
    {
        return;
    }

    current_pos = Size;
    if (current_pos == old_pos)
    {
        return;                     /* دادهٔ جدیدی نیست */
    }

    if (current_pos > old_pos)
    {
        chunk = current_pos - old_pos;
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *)&uart_rx_dma_buffer[old_pos], chunk);
        cmd_process_uart_packet(&uart_rx_dma_buffer[old_pos], chunk);
    }
    else
    {
        /* Wrap-around: دنبالهٔ انتهای بافر + شروع بافر */
        chunk = UART_RX_BUF_SIZE - old_pos;
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *)&uart_rx_dma_buffer[old_pos], chunk);
        cmd_process_uart_packet(&uart_rx_dma_buffer[old_pos], chunk);

        if (current_pos > 0u)
        {
            chunk = current_pos;
            SCB_InvalidateDCache_by_Addr(
                (uint32_t *)&uart_rx_dma_buffer[0], chunk);
            cmd_process_uart_packet(&uart_rx_dma_buffer[0], chunk);
        }
    }

    old_pos = current_pos;
    if (old_pos >= UART_RX_BUF_SIZE)
    {
        old_pos = 0u;
    }
}

#endif /* SRC_CODE_CMD_MUSIC_H_ */
