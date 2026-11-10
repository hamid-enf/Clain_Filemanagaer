/*
 * examples_qspi_player.h — سناریوهای کنترلی موزیک و ویدئو
 *
 * اینجا «طرز استفاده» از لایه‌ی Player کتابخانه را نشان می‌دهیم: همان
 * دکمه‌هایی که روی پخش‌کننده‌های واقعی دارید:
 *
 *   ▶ PLAY    → Player_Play(p, index)
 *   ⏭ NEXT    → Player_Next(p)          (آخر لیست → برمی‌گردد به اول)
 *   ⏮ PREV    → Player_Prev(p)
 *   ⏩ SEEK+   → Player_SeekFwd(p, seconds)
 *   ⏪ SEEK-   → Player_SeekBack(p, seconds)
 *   ⏹ STOP    → Player_Stop(p)
 *   🔊 خروجی  → Player_ReadAudio(p, buf, count, &read)   → به SAI/I2S/DAC
 *   🖼 خروجی  → Player_DecodeVideo(p, pixels, cap, &w, &h) → به LCD
 *
 * سه مثال:
 *   exQP_1_music_controls : کنترل کامل موزیک (لیست، پخش، seek، after/next)
 *   exQP_2_video_controls : کنترل ویدئو (لیست، فریم‌به‌فریم، seek، next)
 *   exQP_3_remote_buttons : شبیه‌سازی «ریموت» — جدول دکمه‌ها + switch
 *
 * پیش‌نیاز: QSPI_examples_begin() قبلاً صدا شده باشد (مونت روی "/qspi").
 *
 * نکته‌ی حافظه: لیست پخش از fm_alloc (پیش‌فرض malloc) حافظه می‌گیرد؛
 * هر آیتم ~164 بایت است (100 آهنگ ≈ 16KB). اگر heap کوچک است یا می‌خواهید
 * از SDRAM بگیرید:
 *     FM_Allocator ext = { my_sdram_alloc, ..., my_sdram_free, ctx };
 *     FM_Allocator_Set(&ext);
 */

#ifndef EXAMPLES_FILE_QSPI_PLAYER_H_
#define EXAMPLES_FILE_QSPI_PLAYER_H_

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_player.h"
#include "fm_media.h"
#include "fm_log.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

static FM_Player g_qspi_player;    /* بزرگ است (~1KB) — حتماً static، نه stack */

static void exqp_step(const char *what, FM_Result r)
{
    if (r != FM_OK && r != FM_ERR_EOF)
        printf("  !! %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
}

static void exqp_print_playlist(FM_Player *p)
{
    uint32_t i;
    uint32_t n = Player_Count(p);

    printf("playlist: %lu track(s)\n", (unsigned long)n);
    for (i = 0; i < n; i++)
        printf("  [%lu] %-24s (%s)\n", (unsigned long)i,
               Player_ItemPath(p, i),
               Media_TypeName(Player_ItemType(p, i)));
}

/*----------------------------------------------------------------------*/
/* exQP_1 — کنترل کامل موزیک                                              */
/*----------------------------------------------------------------------*/

void exQP_1_music_controls(void)
{
    int16_t samples[256];        /* بافر خروجی «خط صدا» */
    size_t  got = 0;
    uint32_t k;

    printf("=== exQP_1: music controls ===\n");

    /* ۱) ساخت لیست پخش از پوشه‌ی /music (بازگشتی + مرتب‌شده) */
    exqp_step("Player_Init", Player_Init(&g_qspi_player));
    exqp_step("Player_ScanFolder(/music)",
             Player_ScanFolder(&g_qspi_player, "/qspi/music", FM_PLAYER_KIND_AUDIO));
    exqp_print_playlist(&g_qspi_player);

    if (Player_Count(&g_qspi_player) == 0)
    {
        printf("no audio files in /music\n");
        Player_Deinit(&g_qspi_player);
        return;
    }

    /* ۲) ▶ PLAY — ترک ۰ */
    exqp_step("Player_Play(0)", Player_Play(&g_qspi_player, 0));
    printf("now playing: %s\n", Player_ItemPath(&g_qspi_player,
                                                Player_Current(&g_qspi_player)));

    /* ۳) پخش: هر Player_ReadAudio یک تکه نمونه‌ی آماده‌ی DAC برمی‌گرداند.
     *    روی برد به‌جای شمارش، این نمونه‌ها را به SAI/I2S بدهید. */
    for (k = 0; k < 3; k++)
    {
        FM_Result r = Player_ReadAudio(&g_qspi_player, samples, 256, &got);
        exqp_step("Player_ReadAudio", r);
        if (r == FM_ERR_EOF)
        {
            /* مثل پخش‌کننده‌ی واقعی: پایان ترک → خودکار بعدی */
            printf("track finished -> auto next\n");
            (void)Player_Next(&g_qspi_player);
            break;
        }
    }
    printf("position %.2f / %.2f s\n",
           Player_GetPosition(&g_qspi_player), Player_GetDuration(&g_qspi_player));

    /* ۴) ⏩ دکمه‌ی جلو بردن ۰.۱ ثانیه */
    exqp_step("Player_SeekFwd(0.1)", Player_SeekFwd(&g_qspi_player, 0.1));
    (void)Player_ReadAudio(&g_qspi_player, samples, 256, &got);
    printf("after seek+: position %.2f s\n", Player_GetPosition(&g_qspi_player));

    /* ۵) ⏪ دکمه‌ی عقب بردن */
    exqp_step("Player_SeekBack(0.2)", Player_SeekBack(&g_qspi_player, 0.2));

    /* ۶) ⏭ NEXT / ⏮ PREV */
    exqp_step("Player_Next", Player_Next(&g_qspi_player));
    printf("now playing: %s\n", Player_ItemPath(&g_qspi_player,
                                                Player_Current(&g_qspi_player)));
    exqp_step("Player_Prev", Player_Prev(&g_qspi_player));

    /* ۷) ⏹ STOP و آزادسازی */
    exqp_step("Player_Stop", Player_Stop(&g_qspi_player));
    Player_Deinit(&g_qspi_player);      /* لیست پخش آزاد می‌شود */
    printf("stopped, playlist freed\n");
}

/*----------------------------------------------------------------------*/
/* exQP_2 — کنترل ویدئو (فریم‌به‌فریم)                                     */
/*----------------------------------------------------------------------*/

#define EXP_VW 16
#define EXP_VH 16
#define EXP_VFRAMES 8

/* ساخت یک کلیپ کوچک FMRV برای دمو (روی کارت شما هم ساخته می‌شود) */
static void exqp_make_clip(const char *path, uint8_t base)
{
    FM_VideoWriter w;
    static uint8_t px[EXP_VW * EXP_VH * 3];   /* static: از stack نمی‌گیرد */
    uint32_t f;
    int x, y;

    if (VideoWriter_Create(path, EXP_VW, EXP_VH, 10, EXP_VFRAMES,
                           sizeof(px), &w) != FM_OK) return;
    for (f = 0; f < EXP_VFRAMES; f++)
    {
        for (y = 0; y < EXP_VH; y++)
            for (x = 0; x < EXP_VW; x++)
            {
                int i = (y * EXP_VW + x) * 3;
                px[i + 0] = (uint8_t)(x * 16);
                px[i + 1] = (uint8_t)(y * 16);
                px[i + 2] = (uint8_t)(base + f);
            }
        (void)VideoWriter_WriteFrame(&w, px, sizeof(px));
    }
    (void)VideoWriter_Close(&w);
}

void exQP_2_video_controls(void)
{
    static uint8_t pixels[EXP_VW * EXP_VH * 3];  /* بافر فریم RGB888 */
    uint16_t w = 0, h = 0;
    uint32_t frame;

    printf("=== exQP_2: video controls ===\n");

    /* ۱) دو کلیپ دمو در /vlist (برای فایل‌های واقعی خودتان فقط پوشه را
     *    عوض کنید — FMRV/AVI-MJPEG/MP4 پشتیبانی می‌شوند) */
    Directory_Create("/qspi/vlist");
    exqp_make_clip("/qspi/vlist/clip_a.fmrv", 10);
    exqp_make_clip("/qspi/vlist/clip_b.fmrv", 100);
    printf("made 2 demo clips in /vlist\n");

    /* ۲) لیست پخشِ فقط-ویدئویی */
    exqp_step("Player_Init", Player_Init(&g_qspi_player));
    exqp_step("Player_ScanFolder(/vlist)",
             Player_ScanFolder(&g_qspi_player, "/qspi/vlist", FM_PLAYER_KIND_VIDEO));
    exqp_print_playlist(&g_qspi_player);

    /* ۳) ▶ PLAY کلیپ ۰ */
    exqp_step("Player_Play(0)", Player_Play(&g_qspi_player, 0));

    /* ۴) پخش: هر Player_DecodeVideo «یک فریم» RGB888 تحویل می‌دهد.
     *    روی برد این حلقه را با تم period تیک 1000/fps میلی‌ثانیه صدا
     *    بزنید و pixels را روی LCD بکشید. */
    frame = 0;
    for (;;)
    {
        FM_Result r = Player_DecodeVideo(&g_qspi_player, pixels,
                                         sizeof(pixels), &w, &h);
        if (r == FM_ERR_EOF)
        {
            printf("clip finished after %lu frames -> next\n",
                   (unsigned long)frame);
            break;   /* پایان → در پخش‌کننده‌ی واقعی: Player_Next */
        }
        exqp_step("Player_DecodeVideo", r);
        if (r != FM_OK) break;
        frame++;
        if (frame % 4u == 0u)
            printf("  frame %lu (%ux%u)  t=%.2f s\n",
                   (unsigned long)frame, (unsigned)w, (unsigned)h,
                   Player_GetPosition(&g_qspi_player));
    }

    /* ۵) ⏪⏩ جابه‌جایی به میانه‌ی کلیپ */
    exqp_step("Player_Seek(0.3)", Player_Seek(&g_qspi_player, 0.3));
    (void)Player_DecodeVideo(&g_qspi_player, pixels, sizeof(pixels), &w, &h);
    printf("after seek 0.3s: %ux%u frame decoded\n", (unsigned)w, (unsigned)h);

    /* ۶) ⏭ NEXT → کلیپ دوم، یک فریم از آن */
    exqp_step("Player_Next", Player_Next(&g_qspi_player));
    printf("now playing: %s\n", Player_ItemPath(&g_qspi_player,
                                                Player_Current(&g_qspi_player)));
    exqp_step("decode after next",
             Player_DecodeVideo(&g_qspi_player, pixels, sizeof(pixels), &w, &h));

    /* ۷) ⏹ STOP + پاک‌کردن فایل‌های دمو */
    exqp_step("Player_Stop", Player_Stop(&g_qspi_player));
    Player_Deinit(&g_qspi_player);
    (void)File_Delete("/qspi/vlist/clip_a.fmrv");
    (void)File_Delete("/qspi/vlist/clip_b.fmrv");
    printf("stopped, demo clips removed\n");
}

/*----------------------------------------------------------------------*/
/* exQP_3 — اتصال دکمه‌های واقعی (ریموت شبیه‌سازی‌شده)                     */
/*----------------------------------------------------------------------*/

/* نگاشت ۱:۱ با GPIOهای برد شما — به‌جای این جدول، در HAL_GPIO_EXTI_Callback
 * همان switch را صدا بزنید.
 *
 * پیشوند QBTN_: پلیر SD (examples_sdcard_player.h) همان دکمه‌ها
 * را با BTN_ دارد؛ این فایل ممکن است کنار آن include شود (مثل
 * tests.h پروژه‌های واقعی)، پس نام‌ها نباید تداخل کنند. */
typedef enum
{
    QBTN_PLAY = 0,
    QBTN_NEXT,
    QBTN_PREV,
    QBTN_FWD,      /* seek جلو  */
    QBTN_BACK,     /* seek عقب  */
    QBTN_TICK,     /* تیک پخش (هر 20ms یک‌بار از تایمر) */
    QBTN_STOP
} ExqpButton;

static void exqp_handle_button(FM_Player *p, ExqpButton btn)
{
    int16_t samples[128];
    size_t  got = 0;

    switch (btn)
    {
    case QBTN_PLAY:
        (void)Player_Play(p, Player_Current(p));
        printf("[PLAY ] %s\n", Player_ItemPath(p, Player_Current(p)));
        break;
    case QBTN_NEXT:
        (void)Player_Next(p);
        printf("[NEXT ] %s\n", Player_ItemPath(p, Player_Current(p)));
        break;
    case QBTN_PREV:
        (void)Player_Prev(p);
        printf("[PREV ] %s\n", Player_ItemPath(p, Player_Current(p)));
        break;
    case QBTN_FWD:
        (void)Player_SeekFwd(p, 5.0);
        printf("[FWD  ] +5.0s -> %.2f s\n", Player_GetPosition(p));
        break;
    case QBTN_BACK:
        (void)Player_SeekBack(p, 5.0);
        printf("[BACK ] -5.0s -> %.2f s\n", Player_GetPosition(p));
        break;
    case QBTN_TICK:
        /* یک تکه‌ی کوچک صدا — روی برد: بده به SAI/I2S DMA */
        if (Player_ReadAudio(p, samples, 128, &got) == FM_ERR_EOF)
        {
            printf("[TICK ] track end -> next\n");
            (void)Player_Next(p);
        }
        break;
    case QBTN_STOP:
        (void)Player_Stop(p);
        printf("[STOP ]\n");
        break;
    default:
        break;
    }
}

void exQP_3_remote_buttons(void)
{
    /* سناریوی فشار دادن دکمه‌ها توسط کاربر */
    static const ExqpButton script[] = {
        QBTN_PLAY, QBTN_TICK, QBTN_TICK, QBTN_FWD, QBTN_TICK,
        QBTN_NEXT, QBTN_TICK, QBTN_PREV, QBTN_TICK, QBTN_BACK, QBTN_STOP
    };
    uint32_t i;

    printf("=== exQP_3: remote button script ===\n");

    exqp_step("Player_Init", Player_Init(&g_qspi_player));
    exqp_step("Player_ScanFolder(/music)",
             Player_ScanFolder(&g_qspi_player, "/qspi/music", FM_PLAYER_KIND_AUDIO));
    if (Player_Count(&g_qspi_player) == 0)
    {
        printf("no audio files in /music\n");
        Player_Deinit(&g_qspi_player);
        return;
    }

    for (i = 0; i < sizeof(script) / sizeof(script[0]); i++)
        exqp_handle_button(&g_qspi_player, script[i]);

    Player_Deinit(&g_qspi_player);
}

#endif /* target */
#endif /* EXAMPLES_FILE_QSPI_PLAYER_H_ */
