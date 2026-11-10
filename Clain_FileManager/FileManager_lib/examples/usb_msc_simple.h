/*
 * usb_msc_simple.h — هر حافظه‌ای را روی USB به کامپیوتر نشان بده
 * ==================================================================
 *
 * کاری که می‌کند: برد شما مثل یک فلش‌مموری به کامپیوتر وصل می‌شود و
 * محتوای SD کارت / QSPI / SDRAM را می‌بینید و مستقیم فایل کپی می‌کنید.
 *
 * ساده‌ترین شکل:
 *
 *     USB_examples_begin("sd");     // SD کارت را روی USB بگذار
 *     ...
 *     while (1) { USB_examples_poll(); }
 *
 * و برای عوض کردن حافظه‌ای که نشان داده می‌شود:
 *
 *     exU_2_switch_to("qspi");
 *
 * ---------------------------------------------------------------------
 * چیزی که باید بدانید
 * ---------------------------------------------------------------------
 * وقتی حافظه‌ای روی USB است، کامپیوتر فکر می‌کند صاحب انحصاری آن است.
 * اگر همزمان خودتان هم روی همان حافظه بنویسید، فایل‌سیستم خراب می‌شود —
 * چون ویندوز/لینوکس جدول FAT را کش می‌کند و از تغییرات شما خبر ندارد.
 *
 * دو راه امن:
 *   ۱) وقتی USB وصل است، خودتان دست نزنید (رایج‌ترین).
 *   ۲) قبل از دست‌زدن، USB_MSC_SetMediaPresent(msc, FM_FALSE) بزنید تا
 *      کامپیوتر «کارت را بیرون‌آمده» ببیند، کارتان را بکنید، بعد دوباره
 *      TRUE کنید. کامپیوتر آن‌وقت دوباره می‌خواند.
 *
 * مثال exU_4 دقیقاً همین الگو را نشان می‌دهد.
 *
 * ---------------------------------------------------------------------
 * اتصال به USB واقعی
 * ---------------------------------------------------------------------
 * این فایل به استک USB کیوب وابسته نیست. شما دو تابع می‌دهید (start و
 * stop) و در callback های SCSI کیوب، USB_MSC_ReadBlocks /
 * USB_MSC_WriteBlocks را صدا می‌زنید. نمونهٔ اتصال در انتهای فایل.
 */
#ifndef FM_EXAMPLE_27_USB_MSC_SIMPLE_H
#define FM_EXAMPLE_27_USB_MSC_SIMPLE_H

#include <stdio.h>
#include <string.h>

#include "file_manager.h"
#include "fm_usb_msc.h"
#include "fm_storage.h"
#include "fm_board_config.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

static FM_USB_MSC g_msc;
static FM_Bool    g_msc_ready;

static void u_step(const char *what, FM_Result r)
{
    if (r != FM_OK)
    {
        printf("  %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
    }
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */
/*
 * حافظه‌ای با نام داده‌شده را روی USB بگذار.
 *
 * نام‌های رایج: "sd"، "qspi"، "ram"
 */
static FM_Result USB_examples_begin(const char *storage_name)
{
    FM_Storage *st = NULL;
    FM_Result r;

    puts("\n**************** USB_examples_begin ****************");

    r = Storage_Find(storage_name, &st);
    if (r != FM_OK || st == NULL)
    {
        printf("  حافظهٔ '%s' پیدا نشد — اول ثبتش کنید.\n", storage_name);
        g_msc_ready = FM_FALSE;
        return FM_ERR_NOT_FOUND;
    }

    r = USB_MSC_Init(&g_msc, st, NULL);
    u_step("USB_MSC_Init", r);
    if (r != FM_OK)
    {
        g_msc_ready = FM_FALSE;
        return r;
    }

    g_msc_ready = FM_TRUE;
    {
        uint64_t blocks = 0;
        uint32_t bsize = 0;

        if (USB_MSC_GetCapacity(&g_msc, &blocks, &bsize) == FM_OK)
        {
            printf("  '%s' روی USB: %lu بلوک × %lu بایت = %lu MB\n",
                   storage_name, (unsigned long)blocks,
                   (unsigned long)bsize,
                   (unsigned long)((blocks * bsize) / (1024u * 1024u)));
        }
    }
    puts("  حالا کابل USB را به کامپیوتر بزنید.");
    return FM_OK;
}

static void USB_examples_end(void)
{
    if (g_msc_ready)
    {
        (void)USB_MSC_Detach(&g_msc);
        g_msc_ready = FM_FALSE;
    }
    puts("**************** USB_examples_end ****************\n");
}

#define U_REQUIRE()                                                        \
    do {                                                                   \
        if (!g_msc_ready) { puts("  (USB آماده نیست — رد شد)"); return; }  \
    } while (0)

/* ------------------------------------------------------------------ */
/* exU_1 — نمایش وضعیت                                                 */
/* ------------------------------------------------------------------ */
static void exU_1_status(void)
{
    uint64_t blocks = 0;
    uint32_t bsize = 0;

    puts("\n================== exU_1_status ==================");
    U_REQUIRE();

    if (USB_MSC_GetCapacity(&g_msc, &blocks, &bsize) == FM_OK)
    {
        printf("  بلوک‌ها : %lu\n", (unsigned long)blocks);
        printf("  اندازه  : %lu بایت\n", (unsigned long)bsize);
        printf("  ظرفیت   : %lu KB\n",
               (unsigned long)((blocks * bsize) / 1024u));
    }
}

/* ------------------------------------------------------------------ */
/* exU_2 — عوض کردن حافظهٔ نمایش‌داده‌شده                               */
/* ------------------------------------------------------------------ */
/*
 * مثلاً از SD به QSPI سوییچ کنید بدون ریست کردن برد.
 *
 * ترتیب مهم است: اول «کارت را بیرون بیاور»، بعد عوض کن، بعد «کارت
 * جدید را بگذار». وگرنه کامپیوتر با جدول FAT قدیمی روی محتوای جدید
 * کار می‌کند و خرابش می‌کند.
 */
static void exU_2_switch_to(const char *storage_name)
{
    FM_Storage *st = NULL;
    FM_Result r;

    puts("\n================ exU_2_switch_to =================");
    U_REQUIRE();

    r = Storage_Find(storage_name, &st);
    if (r != FM_OK || st == NULL)
    {
        printf("  '%s' پیدا نشد\n", storage_name);
        return;
    }

    /* ۱) به کامپیوتر بگو رسانه رفت */
    u_step("eject", USB_MSC_SetMediaPresent(&g_msc, FM_FALSE));
    /* ۲) حالا امن است که عوض شود */
    u_step("re-init", USB_MSC_Init(&g_msc, st, NULL));
    /* ۳) رسانهٔ جدید را اعلام کن */
    u_step("insert", USB_MSC_SetMediaPresent(&g_msc, FM_TRUE));

    printf("  حالا '%s' روی USB است.\n", storage_name);
}

/* ------------------------------------------------------------------ */
/* exU_3 — خواندن/نوشتن بلوکی (همان چیزی که کامپیوتر می‌بیند)          */
/* ------------------------------------------------------------------ */
static void exU_3_block_io(void)
{
    static uint8_t block[512];
    uint64_t blocks = 0;
    uint32_t bsize = 0;

    puts("\n================= exU_3_block_io =================");
    U_REQUIRE();

    if (USB_MSC_GetCapacity(&g_msc, &blocks, &bsize) != FM_OK ||
        bsize > sizeof(block))
    {
        puts("  اندازهٔ بلوک بزرگ‌تر از بافر نمونه است");
        return;
    }
    if (USB_MSC_ReadBlocks(&g_msc, 0u, block, 1u) == FM_OK)
    {
        printf("  بلوک ۰: %02X %02X %02X %02X ... ",
               block[0], block[1], block[2], block[3]);
        /* امضای بوت‌سکتور FAT */
        if (block[510] == 0x55u && block[511] == 0xAAu)
        {
            puts("(بوت‌سکتور معتبر)");
        }
        else
        {
            puts("(فرمت نشده یا خام)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* exU_4 — دسترسی امن همزمان                                           */
/* ------------------------------------------------------------------ */
/*
 * الگوی درست وقتی خودتان می‌خواهید فایلی بنویسید در حالی که USB وصل است.
 */
static void exU_4_safe_local_write(void)
{
    FM_Result r;

    puts("\n============ exU_4_safe_local_write =============");
    U_REQUIRE();

    puts("  ۱) رسانه را از دید کامپیوتر بردار");
    u_step("eject", USB_MSC_SetMediaPresent(&g_msc, FM_FALSE));

    puts("  ۲) حالا نوشتن محلی امن است");
    r = File_WriteText("/usb_note.txt", "written while USB was detached");
    u_step("write", r);

    puts("  ۳) رسانه را برگردان — کامپیوتر دوباره می‌خواند");
    u_step("insert", USB_MSC_SetMediaPresent(&g_msc, FM_TRUE));

    puts("  حالا فایل جدید در کامپیوتر دیده می‌شود.");
}

/* ------------------------------------------------------------------ */
/* در حلقهٔ اصلی                                                       */
/* ------------------------------------------------------------------ */
static void USB_examples_poll(void)
{
    if (g_msc_ready)
    {
        (void)USB_MSC_Sync(&g_msc);
    }
}

/*
 * ---------------------------------------------------------------------
 * وصل کردن به استک USB کیوب
 * ---------------------------------------------------------------------
 * در فایل usbd_storage_if.c که کیوب می‌سازد، بدنهٔ توابع را با این‌ها
 * عوض کنید:
 *
 *   int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *block_num,
 *                                 uint16_t *block_size)
 *   {
 *       uint64_t n = 0; uint32_t s = 0;
 *       (void)lun;
 *       if (USB_MSC_GetCapacity(&g_msc, &n, &s) != FM_OK) { return -1; }
 *       *block_num  = (uint32_t)n;
 *       *block_size = (uint16_t)s;
 *       return 0;
 *   }
 *
 *   int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr,
 *                          uint16_t blk_len)
 *   {
 *       (void)lun;
 *       return (USB_MSC_ReadBlocks(&g_msc, blk_addr, buf, blk_len)
 *               == FM_OK) ? 0 : -1;
 *   }
 *
 *   int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr,
 *                           uint16_t blk_len)
 *   {
 *       (void)lun;
 *       return (USB_MSC_WriteBlocks(&g_msc, blk_addr, buf, blk_len)
 *               == FM_OK) ? 0 : -1;
 *   }
 *
 *   int8_t STORAGE_IsReady_FS(uint8_t lun)
 *   {
 *       (void)lun;
 *       return g_msc_ready ? 0 : -1;
 *   }
 *
 * نکتهٔ کش: اگر D-Cache روشن است و USB با DMA کار می‌کند، بافر buf را
 * قبل از خواندن Clean و بعد از نوشتن Invalidate کنید
 * (examples/cache.h).
 * ---------------------------------------------------------------------
 */

#endif /* FM_EXAMPLE_27_USB_MSC_SIMPLE_H */
