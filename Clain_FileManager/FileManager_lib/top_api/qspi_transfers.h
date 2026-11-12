/*
 * qspi_transfers.h — انتقال داده بین حافظه‌ها (SD ↔ QSPI ↔ SDRAM ↔ RAM)
 * ===========================================================================
 *
 * این فایل بعد از مرتب‌سازی، فقط بخش یکتای خودش را دارد: تست‌های انتقال.
 * بخش «کار معمولی با فایل روی QSPI» (نوشتن/خواندن متن، پوشه‌ها، دادهٔ
 * باینری، CRC) قبلاً با مثال‌های QspiDemo_* در
 * examples_qspi.h پوشش داده می‌شود و نسخه‌های تکراری‌اش حذف
 * شدند. راه‌اندازی و mount هم فقط یک‌جاست:
 *
 *     QspiFlash_Begin()   از examples_qspi.h
 *     QspiFlash_End()     از examples_qspi.h
 *
 * استفاده در main (ترتیب کامل در examples_qspi.h نوشته شده):
 *
 *     #include "examples_qspi.h"       <- QspiFlash_Begin/End + QspiDemo_*
 *     #include "qspi_transfers.h"      <- TransferDemo_*
 *     ...
 *     if (QspiFlash_Begin() == FM_OK) {
 *         QspiDemo_Basic();                 <- تست‌های فایل
 *         ...
 *         TransferDemo_SdToQspi();                 <- انتقال
 *         TransferDemo_QspiToSdram();
 *         TransferDemo_VerifyRoundtrip();
 *         TransferDemo_AllCombinations();
 *         QspiFlash_End();
 *     }
 *
 * پیش‌نیاز: "/qspi" مونت شده باشد — با هر درایوری (QspiW25q یا QspiDriver).
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
 *     (به همین دلیل تست Logger عمداً در QspiDemo_DataLogger روی /qspi/logs با
 *     rotation کوچک اجرا می‌شود — برای لاگ واقعی از SD استفاده کنید.)
 *
 *  ۳) در حالت memory-mapped فقط خواندنی است. آدرس FM_MEM_QSPI_BASE را
 *     می‌شود مثل حافظه خواند (حتی memcpy) ولی نوشتن روی آن کار نمی‌کند —
 *     باید از API فایل یا دستورات QSPI استفاده کنید.
 */

#ifndef FM_TOPAPI_QSPI_TRANSFERS_H
#define FM_TOPAPI_QSPI_TRANSFERS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "file_manager.h"
#include "fm_transfer.h"
#include "fm_filesystem.h"
#include "fm_board_config.h"
#include "fm_log.h"

/* CRC-32 for the round-trip checks below.  With FM_USE_CRC=0 the library
 * has no CRC module, so the example carries the same loop itself
 * (reflected 0xEDB88320, no init / final XOR: identical results to
 * CRC_Compute32) instead of breaking the build. */
#ifndef DEMO_CRC32
#if FM_USE_CRC
#define DEMO_CRC32(data, len, crc) CRC_Compute32((data), (len), (crc))
#else
static uint32_t demo_crc32_local(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    unsigned j;

    for (i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (j = 0; j < 8u; j++)
        {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}
#define DEMO_CRC32(data, len, crc) demo_crc32_local((data), (len), (crc))
#endif
#endif /* DEMO_CRC32 */

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

/* ==================================================================== */
/* ابزار مشترک                                                          */
/* ==================================================================== */

static void transfer_demo_step(const char *what, FM_Result r)
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
        puts("  (QSPI آماده نیست — اول QspiFlash_Begin() را صدا بزنید)");
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
 *   سخت‌افزاری رخ می‌دهد (fm_xdma.h برای QSPI/SPI-فلش، i2c_eeprom_dma.h برای
 *   I2C) و همان‌جا Clean/Invalidate خودکار انجام می‌شود. پس چه SDRAM
 *   کش‌دار باشد چه نباشد، این تست‌ها درست کار می‌کنند — TransferDemo_QspiToSdram که بافرش
 *   در ناحیهٔ کش‌پذیر SDRAM است هم مستثنا نیست.
 */

/*
 * گزارش پیشرفت — برای فایل‌های بزرگ مفید است.
 *
 * بهینه: فقط روی تغییرِ پلهٔ ۱۰٪ چاپ می‌کند، نه هر فراخوانی. بافر
 * لاگ (FM_LOG_BUF_SIZE) کوچک است؛ اسپم درصد، خروجی بقیهٔ تست‌ها را
 * می‌بلعد. برای لغو، FM_FALSE برگردانید.
 */
static FM_Bool transfer_demo_progress(uint64_t done, uint64_t total, void *user)
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

/* TransferDemo_SdToQspi — از SD کارت به QSPI.
 *
 * کاربرد واقعی: فایلی که همیشه لازم دارید (فونت، لوگو، کالیبراسیون) را
 * از SD روی فلش داخلی ببرید تا بدون کارت هم در دسترس باشد.
 *
 * منبع پیش‌فرض /config.json است (FileDemo_Json همان را می‌سازد). اگر روی SD
 * نبود، یک فایل نمونه می‌سازد تا تست همیشه قابل اجرا باشد. */
void TransferDemo_SdToQspi(void)
{
    FM_TransferOpts o;
    FM_TransferInfo info;
    FM_Result r;
    const char *src = "/config.json";
    FM_Bool exists = FM_FALSE;

    puts("\n================ TransferDemo_SdToQspi =================");
    if (!qspi_ready()) return;

    if (File_Exists(src, &exists) != FM_OK || !exists)
    {
        src = "/qspi_demo_src.txt";
        transfer_demo_step("write demo source",
               File_WriteText(src,
                   "QSPI transfer demo source file (from SD side)."));
    }

    memset(&o, 0, sizeof(o));
    o.verify = FM_TRUE;        /* بعد از کپی CRC دو طرف مقایسه شود */
    o.progress = transfer_demo_progress;

    r = FM_Transfer_CopyEx(src, "/qspi/config.json", &o, &info);
    if (r == FM_OK)
    {
        printf("  copied %lu bytes (%s), CRC 0x%08lX (verified)\n",
               (unsigned long)info.bytes, src,
               (unsigned long)info.crc_dest);
    }
    else
    {
        transfer_demo_step("SD -> QSPI", r);
    }
}

/* TransferDemo_QspiToSdram — از QSPI به SDRAM (بافر حافظه).
 *
 * کاربرد واقعی: یک کلیپ یا تصویر را یک‌بار در SDRAM بارگذاری کنید تا
 * پخش/نمایشش سریع باشد و هر فریم از فلش خوانده نشود.
 *
 * ورودی همان فایلی است که TransferDemo_SdToQspi کپی کرده؛ پس این دو را پشت هم صدا بزنید. */
void TransferDemo_QspiToSdram(void)
{
    /* این بافر در SDRAM می‌نشیند — سکشن از fm_board_config.h می‌آید. */
#if defined(__GNUC__) && !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
    __attribute__((section(SIMPLE_VIDEO_SECTION), aligned(32)))
#endif
    static uint8_t sdram_buf[64u * 1024u];
    size_t len = 0;
    FM_Result r;

    puts("\n=============== TransferDemo_QspiToSdram ===============");
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
        transfer_demo_step("QSPI -> SDRAM", r);
    }
}

/* TransferDemo_VerifyRoundtrip — رفت و برگشت کامل: RAM → QSPI → RAM، و اثبات یکی بودن. */
void TransferDemo_VerifyRoundtrip(void)
{
    static uint8_t src[2048];
    static uint8_t dst[2048];
    size_t i;
    size_t got = 0;
    uint32_t c1;
    uint32_t c2;

    puts("\n============== TransferDemo_VerifyRoundtrip =============");
    if (!qspi_ready()) return;

    for (i = 0; i < sizeof(src); i++)
    {
        src[i] = (uint8_t)((i * 31u) ^ (i >> 3));
    }
    c1 = DEMO_CRC32(src, sizeof(src), 0u);

    transfer_demo_step("RAM -> QSPI",
           FM_Transfer_SaveFile("/qspi/rt.bin", src, sizeof(src)));
    transfer_demo_step("QSPI -> RAM",
           FM_Transfer_LoadFile("/qspi/rt.bin", dst, sizeof(dst), &got));

    c2 = DEMO_CRC32(dst, got, 0u);
    printf("  %lu bytes, CRC %08lX -> %08lX : %s\n",
           (unsigned long)got, (unsigned long)c1, (unsigned long)c2,
           (c1 == c2) ? "IDENTICAL" : "CORRUPTED");
}

/* TransferDemo_AllCombinations — همهٔ ترکیب‌ها در یک جدول.
 *
 * چون همه‌چیز پشت مسیر فایل انتزاع شده، مبدأ و مقصد فقط دو رشته‌اند.
 * منبع می‌تواند SD باشد و مقصد QSPI، یا برعکس، یا هر ترکیب دیگری. */
void TransferDemo_AllCombinations(void)
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

    puts("\n============= TransferDemo_AllCombinations ==============");
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

#endif /* FM_TOPAPI_QSPI_TRANSFERS_H */
