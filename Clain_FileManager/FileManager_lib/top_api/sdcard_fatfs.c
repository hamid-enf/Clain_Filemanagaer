/*
 * Example 11 — SD Card via FatFS (STM32H7 / CubeMX target only)
 *
 * نسخه‌ی درستِ همان ex_1_SdCard_file_basic که روی برد اجرا می‌کنید — با این
 * تفاوت‌ها:
 *   1) نتیجه‌ی «همه‌ی» فراخوانی‌ها چک و چاپ می‌شود؛ هیچ خطایی بی‌صدا نمی‌ماند.
 *   2) FileManager_Init دوباره صدا زده نمی‌شود اگر قبلاً init شده.
 *   3) قبل از mount، درایو FatFS با f_getfree واقعاً validate می‌شود (چون
 *      f_mount با opt=0 فقط «lazy» ثبت می‌کند و خطای دیسک را نشان نمی‌دهد).
 *   4) فقط از %d / %s / %lu استفاده شده که با newlib و mini-printf هر دو
 *      درست چاپ می‌شوند (نه %zu / %u).
 *
 *پیش‌نیاز پروژه CubeMX:
 *   FM_USE_FATFS برابر 1 و FM_SD_DISKIO برابر CUBEMX؛ هر دو را
 *   fm_board_config.h در بیلد فریمور خودش می‌گذارد، Symbol لازم نیست.
 *   (diskio خود CubeMX — sd_diskio.c — به درایو 0 وصل است)
 *
 * این فایل روی هاست (FM_HOST_BUILD=1) به هیچی ترجمه نمی‌شود.
 */

#include "fm_config.h"   /* FM_USE_FATFS may come from fm_board_config.h */
#if defined(FM_USE_FATFS) && FM_USE_FATFS && \
    !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_fatfs.h"

#define PRT ok_printf

/* چاپ خطا همراه با متن نتیجه — دیگر هیچ فراخوانی بی‌پاسخ نمی‌ماند */
static void ok_printf(const char *what, FM_Result r)
{
    if (r == FM_OK)
    {
        printf("[OK]   %s\n", what);
    }
    else
    {
        printf("[ERR]  %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
    }
}

static FM_FS g_sd_fs;

void ex_11_sdcard_fatfs(void)
{
    FM_Result r;
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    /* ۱) هسته (اگر در main صدا زده‌اید، دوباره زدنش بی‌ضرر است) */
    r = FileManager_Init();
    ok_printf("FileManager_Init", r);

    /* ۲) اتصال درایو 0 فت‌‌اف‌اس به FileManager */
    r = FatFS_Init(&g_sd_fs, 0);
    ok_printf("FatFS_Init(0)", r);
    if (r != FM_OK) return;

    /* ۳) مونت روی "/" — f_mount داخل adapter با opt=0 (lazy) است، یعنی
     *    دیسک واقعاً خوانده نمی‌شود تا اولین عملیات. اعتبارسنجی واقعی
     *    را بعد از مونت با FS_GetInfo انجام می‌دهیم (از داخل adapter
     *    به f_getfree می‌رود) تا اگر کارت مشکل دارد همین‌جا و با پیام
     *    روشن متوقف شویم، نه وسط File_WriteText.
     *    توجه: اگر قبل از این تابع، کسی f_mount(NULL,...,1) زده باشد،
     *    درایو 0 «ثبت» نیست و f_getfree خام قبل از مونت کار نمی‌کند؛
     *    بنابراین ترتیب درست: اول FS_Mount، بعد اعتبارسنجی. */
    r = FS_Mount("/", &g_sd_fs);
    ok_printf("FS_Mount(\"/\")", r);
    if (r != FM_OK) return;

    {
        FM_FSInfo info;
        r = FS_GetInfo("/", &info);
        ok_printf("FS_GetInfo (real disk access)", r);
        if (r != FM_OK)
        {
            printf("SD card problem (insert/format?). Stop.\n");
            (void)FS_Unmount("/");
            return;
        }
        printf("       fs=%s free=%lu KB\n", info.type,
               (unsigned long)(info.free_space / 1024u));
    }

    /* ۴) عملیات پایه — همه چک می‌شوند */
    r = File_WriteText("/greeting.txt", "Hello from SD Card!");
    ok_printf("File_WriteText", r);

    r = File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    ok_printf("File_ReadText", r);
    printf("       read: %s\n", buf);

    r = File_SizeOf("/greeting.txt", &size);
    ok_printf("File_SizeOf", r);
    printf("       size: %lu\n", (unsigned long)size);

    r = File_WriteText("/greeting.txt", "New SD Card Content!");
    ok_printf("overwrite File_WriteText", r);

    r = File_Create("/empty.txt");
    ok_printf("File_Create", r);

    r = File_Delete("/greeting.txt");
    ok_printf("File_Delete", r);

    r = File_Exists("/greeting.txt", &exists);
    ok_printf("File_Exists", r);
    printf("       exists after delete: %d\n", (int)exists);

    /* ۵) جمع‌کردن */
    r = FS_Unmount("/");
    ok_printf("FS_Unmount", r);
}

#else  /* host build: nothing to run here */

/* روی هاست این مثال خالی است؛ فقط روی برد (FM_USE_FATFS=1 و بدون
 * FM_HOST_BUILD) کامپایل می‌شود. */

#endif
