/*
 * qspi_transfers.h — انتقال داده بین حافظه‌ها (SD ↔ QSPI ↔ SDRAM ↔ RAM)
 * ===========================================================================
 *
 * این فایل بعد از مرتب‌سازی، فقط بخش یکتای خودش را دارد: تست‌های انتقال.
 * بخش «کار معمولی با فایل روی QSPI» (نوشتن/خواندن متن، پوشه‌ها، دادهٔ
 * باینری، CRC) قبلاً با مثال‌های exQ_1 تا exQ_10 در
 * examples_qspi.h پوشش داده می‌شود و نسخه‌های تکراری‌اش حذف
 * شدند. راه‌اندازی و mount هم فقط یک‌جاست:
 *
 *     QSPI_examples_begin()   از examples_qspi.h
 *     QSPI_examples_end()     از examples_qspi.h
 *
 * استفاده در main (ترتیب کامل در examples_qspi.h نوشته شده):
 *
 *     #include "examples_qspi.h"       <- begin/end + exQ_1..10
 *     #include "qspi_transfers.h"      <- exT_1..exT_4
 *     ...
 *     if (QSPI_examples_begin() == FM_OK) {
 *         exQ_1_file_basic();                 <- تست‌های فایل
 *         ...
 *         exT_1_sd_to_qspi();                 <- انتقال
 *         exT_2_qspi_to_sdram();
 *         exT_3_verify_roundtrip();
 *         exT_4_all_combinations();
 *         QSPI_examples_end();
 *     }
 *
 * پیش‌نیاز: "/qspi" مونت شده باشد — با هر درایوری (Ex15 یا Ex26).
 * اگر مونت نباشد، هر تست تمیز «رد شد» می‌دهد و crash نمی‌کند.
 *
 * ---------------------------------------------------------------------
 * نکتهٔ مهم دربارهٔ فلش NOR که با SD فرق دارد
 * ---------------------------------------------------------------------
 * فلش NOR سه خاصیت دارد که SD ندارد و اگر ندانید داده خراب می‌شود:
 *
 *  ۱) نوشتن فقط ۱ را به ۰ تبدیل می‌کند. برای برگرداندن ۰ به ۱ باید کل
 *     سکتور پاک (erase) شود. یعنی «تغییر یک بایت» در واقع یعنی: خواندن
 *     کل سکتور ۴KB، تغییر در RAM، پاک‌کردن سکتور، نوشتن دوباره.
 *     FileManager این را خودش انجام می‌دهد، ولی بدانید که یک نوشتن کوچک
 *     گران است.
 *
 *  ۲) تعداد پاک‌شدن محدود است (حدود ۱۰۰ هزار بار برای هر سکتور). پس
 *     لاگ پرتکرار روی QSPI نگذارید؛ آن را روی SD بگذارید.
 *     (به همین دلیل تست Logger عمداً در exQ_8 روی /qspi/logs با
 *     rotation کوچک اجرا می‌شود — برای لاگ واقعی از SD استفاده کنید.)
 *
 *  ۳) در حالت memory-mapped فقط خواندنی است. آدرس FM_MEM_QSPI_BASE را
 *     می‌شود مثل حافظه خواند (حتی memcpy) ولی نوشتن روی آن کار نمی‌کند —
 *     باید از API فایل یا دستورات QSPI استفاده کنید.
 */

#ifndef FM_EXAMPLE_25_QSPI_AND_TRANSFERS_H
#define FM_EXAMPLE_25_QSPI_AND_TRANSFERS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "file_manager.h"
#include "fm_transfer.h"
#include "fm_filesystem.h"
#include "fm_board_config.h"
#include "fm_log.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

/* ==================================================================== */
/* ابزار مشترک                                                          */
/* ==================================================================== */

static void q_step(const char *what, FM_Result r)
{
    if (r != FM_OK)
    {
        printf("  %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
    }
}

/* برای هر مثال: اگر "/qspi" مونت نیست، تمیز رد شو.
 * (begin از examples_qspi.h باید قبلاً صدا شده باشد — با هر
 * درایوری.) */
static FM_Bool qspi_ready(void)
{
    FM_Bool mounted = FM_FALSE;

    if (FS_IsMounted("/qspi", &mounted) != FM_OK || !mounted)
    {
        puts("  (QSPI آماده نیست — اول QSPI_examples_begin() را صدا بزنید)");
        return FM_FALSE;
    }
    return FM_TRUE;
}

/* ==================================================================== */
/* بخش B — انتقال داده بین حافظه‌ها                                     */
/* ==================================================================== */
/*
 * همهٔ این‌ها از FM_Transfer_* استفاده می‌کنند که:
 *   - تکه‌تکه کپی می‌کند (بافر بزرگ لازم نیست)
 *   - اگر بخواهید با CRC صحت را بررسی می‌کند
 *   - فلش را قبل از نوشتن پاک می‌کند (و سکتورهای خالی را رد می‌کند)
 *   - می‌شود وسط کار لغوش کرد
 *
 * کش و بدون کش — چرا اینجا هیچ نگهداری کشی لازم نیست:
 *   همهٔ انتقال‌های این بخش روی مسیر File_* یعنی CPU (memcpy) هستند؛
 *   CPU با کش خودش همیشه نسخهٔ درست را می‌بیند. DMA فقط داخل درایورهای
 *   سخت‌افزاری رخ می‌دهد (fm_xdma.h برای QSPI/SPI-فلش، مثال ۳۲ برای
 *   I2C) و همان‌جا Clean/Invalidate خودکار انجام می‌شود. پس چه SDRAM
 *   کش‌دار باشد چه نباشد، این تست‌ها درست کار می‌کنند — exT_2 که بافرش
 *   در ناحیهٔ کش‌پذیر SDRAM است هم مستثنا نیست.
 */

/*
 * گزارش پیشرفت — برای فایل‌های بزرگ مفید است.
 *
 * بهینه: فقط روی تغییرِ پلهٔ ۱۰٪ چاپ می‌کند، نه هر فراخوانی. بافر
 * لاگ (FM_LOG_BUF_SIZE) کوچک است؛ اسپم درصد، خروجی بقیهٔ تست‌ها را
 * می‌بلعد. برای لغو، FM_FALSE برگردانید.
 */
static FM_Bool q_progress(uint64_t done, uint64_t total, void *user)
{
    static uint32_t last_step = 100u;
    uint32_t pct;

    (void)user;
    if (total == 0u) return FM_TRUE;

    pct = (uint32_t)((done * 100u) / total);
    if (pct / 10u != last_step / 10u || pct == 0u || pct >= 100u)
    {
        printf("    %lu%%\r", (unsigned long)pct);
        last_step = pct;
        if (pct >= 100u) puts("");   /* خط تمام شد */
    }
    return FM_TRUE;
}

/* exT_1 — از SD کارت به QSPI.
 *
 * کاربرد واقعی: فایلی که همیشه لازم دارید (فونت، لوگو، کالیبراسیون) را
 * از SD روی فلش داخلی ببرید تا بدون کارت هم در دسترس باشد.
 *
 * منبع پیش‌فرض /config.json است (مثال ۵ همان را می‌سازد). اگر روی SD
 * نبود، یک فایل نمونه می‌سازد تا تست همیشه قابل اجرا باشد. */
void exT_1_sd_to_qspi(void)
{
    FM_TransferOpts o;
    FM_TransferInfo info;
    FM_Result r;
    const char *src = "/config.json";
    FM_Bool exists = FM_FALSE;

    puts("\n================ exT_1_sd_to_qspi =================");
    if (!qspi_ready()) return;

    if (File_Exists(src, &exists) != FM_OK || !exists)
    {
        src = "/qspi_demo_src.txt";
        q_step("write demo source",
               File_WriteText(src,
                   "QSPI transfer demo source file (from SD side)."));
    }

    memset(&o, 0, sizeof(o));
    o.verify = FM_TRUE;        /* بعد از کپی CRC دو طرف مقایسه شود */
    o.progress = q_progress;

    r = FM_Transfer_CopyEx(src, "/qspi/config.json", &o, &info);
    if (r == FM_OK)
    {
        printf("  copied %lu bytes (%s), CRC 0x%08lX (verified)\n",
               (unsigned long)info.bytes, src,
               (unsigned long)info.crc_dest);
    }
    else
    {
        q_step("SD -> QSPI", r);
    }
}

/* exT_2 — از QSPI به SDRAM (بافر حافظه).
 *
 * کاربرد واقعی: یک کلیپ یا تصویر را یک‌بار در SDRAM بارگذاری کنید تا
 * پخش/نمایشش سریع باشد و هر فریم از فلش خوانده نشود.
 *
 * ورودی همان فایلی است که exT_1 کپی کرده؛ پس این دو را پشت هم صدا بزنید. */
void exT_2_qspi_to_sdram(void)
{
    /* این بافر در SDRAM می‌نشیند — سکشن از fm_board_config.h می‌آید. */
#if defined(__GNUC__) && !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
    __attribute__((section(SIMPLE_VIDEO_SECTION), aligned(32)))
#endif
    static uint8_t sdram_buf[64u * 1024u];
    size_t len = 0;
    FM_Result r;

    puts("\n=============== exT_2_qspi_to_sdram ===============");
    if (!qspi_ready()) return;

    /* بافر در ناحیهٔ کش‌پذیر SDRAM است — اشکالی ندارد: بارگذاری از
     * مسیر CPU انجام می‌شود و کش خودبه‌خود همگام است. */

    r = FM_Transfer_LoadFile("/qspi/config.json", sdram_buf,
                             sizeof(sdram_buf), &len);
    if (r == FM_OK)
    {
        printf("  loaded %lu bytes into SDRAM at %p\n",
               (unsigned long)len, (void *)sdram_buf);
        printf("  first bytes: %02X %02X %02X %02X\n",
               sdram_buf[0], sdram_buf[1], sdram_buf[2], sdram_buf[3]);
    }
    else
    {
        q_step("QSPI -> SDRAM", r);
    }
}

/* exT_3 — رفت و برگشت کامل: RAM → QSPI → RAM، و اثبات یکی بودن. */
void exT_3_verify_roundtrip(void)
{
    static uint8_t src[2048];
    static uint8_t dst[2048];
    size_t i;
    size_t got = 0;
    uint32_t c1;
    uint32_t c2;

    puts("\n============== exT_3_verify_roundtrip =============");
    if (!qspi_ready()) return;

    for (i = 0; i < sizeof(src); i++)
    {
        src[i] = (uint8_t)((i * 31u) ^ (i >> 3));
    }
    c1 = CRC_Compute32(src, sizeof(src), 0u);

    q_step("RAM -> QSPI",
           FM_Transfer_SaveFile("/qspi/rt.bin", src, sizeof(src)));
    q_step("QSPI -> RAM",
           FM_Transfer_LoadFile("/qspi/rt.bin", dst, sizeof(dst), &got));

    c2 = CRC_Compute32(dst, got, 0u);
    printf("  %lu bytes, CRC %08lX -> %08lX : %s\n",
           (unsigned long)got, (unsigned long)c1, (unsigned long)c2,
           (c1 == c2) ? "IDENTICAL" : "CORRUPTED");
}

/* exT_4 — همهٔ ترکیب‌ها در یک جدول.
 *
 * چون همه‌چیز پشت مسیر فایل انتزاع شده، مبدأ و مقصد فقط دو رشته‌اند.
 * منبع می‌تواند SD باشد و مقصد QSPI، یا برعکس، یا هر ترکیب دیگری. */
void exT_4_all_combinations(void)
{
    static const char *const paths[] = {
        "/ram_area.bin",     /* RAM disk / SDRAM که mount شده */
        "/qspi/area.bin",    /* فلش QSPI                      */
        "/sd_area.bin"       /* SD کارت (روی /)               */
    };
    static const char *const names[] = { "RAM", "QSPI", "SD" };
    static uint8_t seed[256];
    size_t i;
    size_t j;

    puts("\n============= exT_4_all_combinations ==============");
    if (!qspi_ready()) return;

    for (i = 0; i < sizeof(seed); i++)
    {
        seed[i] = (uint8_t)(i + 1u);
    }
    /* هر مبدأ را بساز */
    for (i = 0; i < 3u; i++)
    {
        (void)FM_Transfer_SaveFile(paths[i], seed, sizeof(seed));
    }

    puts("  from -> to      result");
    for (i = 0; i < 3u; i++)
    {
        for (j = 0; j < 3u; j++)
        {
            FM_TransferOpts o;
            FM_Result r;

            if (i == j)
            {
                continue;
            }
            memset(&o, 0, sizeof(o));
            o.verify = FM_TRUE;
            r = FM_Transfer_Copy(paths[i], paths[j], &o);
            printf("  %-4s -> %-4s     %s\n", names[i], names[j],
                   (r == FM_OK) ? "OK" : File_GetErrorString(r));
        }
    }
}

#endif /* FM_EXAMPLE_25_QSPI_AND_TRANSFERS_H */
