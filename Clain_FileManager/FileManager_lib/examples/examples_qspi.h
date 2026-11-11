/*
 * examples_qspi.h — مالکِ «تنها» begin/end مثال‌های QSPI
 * + معادل کامل مثال‌های File شماره 1 تا 10 روی QSPI
 *
 * این فایل تنها جایی است که QSPI_examples_begin() /
 * QSPI_examples_format() / QSPI_examples_end() را تعریف می‌کند.
 * بقیهٔ فایل‌های QSPI (مدیا، پلیر، انتقال 25) فقط از مونت آمادهٔ
 * "/qspi" استفاده می‌کنند و تعریف تکراری ندارند — پس همه را می‌توان
 * با هم در tests.h включ کرد و پشت‌سرهم تست کرد.
 *
 * Backend سخت‌افزاری در qspi_w25q.h جدا شده است. این فایل
 * فقط همان File/Directory/JSON/Text/SafeWrite/Logger/CRC/Image workflow را
 * روی مسیر /qspi اجرا می‌کند. در نتیجه API سطح بالا به W25Q256 قفل نیست؛
 * با عوض‌کردن callbackهای read/program/erase همین مثال‌ها روی NOR دیگری
 * نیز اجرا می‌شوند.
 *
 * ترتیب کامل تست QSPI (همان ترتیب SD که قبلاً پاس شده):
 *
 *   MX_QUADSPI_Init();                 <- CubeMX، قبل از همهٔ این‌ها
 *   if (QSPI_examples_begin() == FM_OK) {
 *       exQ_1_file_basic();                    <- فایل ۱..۱۰
 *       exQ_2_file_copy_move_rename();
 *       ...
 *       exQ_10_file_BMP_Image_Scale_Convert();
 *
 *       exQM_1_audio_detect_process_save();    <- مدیا (اختیاری)
 *       exQM_2_video_detect_to_bgr565();
 *       exQM_3_image_bgr_effect_resize_save();
 *
 *       exQP_1_music_controls();               <- پلیر (اختیاری)
 *       exQP_2_video_controls();
 *       exQP_3_remote_buttons();
 *
 *       exT_1_sd_to_qspi();                    <- انتقال ۲۵
 *       exT_2_qspi_to_sdram();
 *       exT_3_verify_roundtrip();
 *       exT_4_all_combinations();
 *
 *       QSPI_examples_end();
 *   }
 *   else if (service_button_confirmed)         <- فقط flash نو/خراب
 *       QSPI_examples_format();
 *
 * دو راه انداختن درایور — begin خودش انتخاب می‌کند:
 *   ۱) خودکار (پیش‌فرض همین فایل): Ex15_QSPI_Begin() با درایور W25Q256.
 *   ۲) از بیرون: اگر خودتان قبلش فلش را با درایور دیگر (مثلاً
 *      Ex26_QspiBegin در qspi_driver.h) راه انداخته و روی
 *      "/qspi" مونت کرده باشید، begin همان را پیدا می‌کند، دوباره
 *      کاری نمی‌کند و فقط گزارش فضای آزاد می‌دهد. در این حالت
 *      QSPI_examples_end() هم چیزی را نمی‌بندد — بستنش با caller.
 *
 * نکته: W25Q256 نو باید یک‌بار FAT format شود. برای جلوگیری از پاک‌شدن
 * اتفاقی داده، format خودکار نیست؛ QSPI_examples_format() را فقط با فرمان
 * کاربر/دکمهٔ سرویس اجرا کنید.
 */

#ifndef EXAMPLES_FILE_QSPI_ALL_H_
#define EXAMPLES_FILE_QSPI_ALL_H_

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_log.h"
#include "qspi_w25q.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

static void qspi_step(const char *what, FM_Result r)
{
    if (r != FM_OK)
    {
        printf("  !! %s -> %s (%d)\n", what,
               File_GetErrorString(r), (int)r);
    }
}

/* آیا begin خودش درایور را راه انداخته؟ اگر از بیرون مونت آمده باشد،
 * end حق ندارد چیزی را ببندد. */
static FM_Bool qspi_started_here = FM_FALSE;

FM_Result QSPI_examples_begin(void)
{
    FM_FSInfo info;
    FM_Bool mounted = FM_FALSE;
    FM_Result r;

    qspi_started_here = FM_FALSE;

    /* اگر caller خودش QSPI را راه انداخته باشد (مثلاً Ex26_QspiBegin
     * + FMFS_Init + FS_Mount("/qspi"))، همان مونت را استفاده کن —
     * راه‌اندازی دوم باعث دو فایل‌سیستم روی یک مسیر می‌شود. */
    (void)FS_IsMounted(EX15_QSPI_MOUNT_PATH, &mounted);
    if (mounted)
    {
        printf("QSPI: /qspi already mounted - reusing it.\n");
        r = FS_GetInfo(EX15_QSPI_MOUNT_PATH, &info);
        qspi_step("FS_GetInfo(/qspi)", r);
        if (r == FM_OK)
        {
            printf("QSPI: %s, free %lu KB\n", info.type,
                   (unsigned long)(info.free_space / 1024u));
        }
        return r;
    }

    r = Ex15_QSPI_Begin();
    qspi_step("Ex15_QSPI_Begin", r);
    if (r != FM_OK)
    {
        /* ریشهٔ رایج این خطاها فلش نو/خالی است — نه خرابی سخت‌افزار. */
        if (r == FM_ERR_CORRUPTED || r == FM_ERR_NOT_FOUND)
        {
            printf("  QSPI flash khali/new ast - yek bar: "
                   "QSPI_examples_format();\n");
        }
        return r;
    }
    qspi_started_here = FM_TRUE;

    r = FS_GetInfo(EX15_QSPI_MOUNT_PATH, &info);
    qspi_step("FS_GetInfo(/qspi)", r);
    if (r == FM_OK)
    {
        printf("QSPI: %s, free %lu KB\n", info.type,
               (unsigned long)(info.free_space / 1024u));
    }
    else
    {
        printf("QSPI FAT is missing/corrupt; after explicit user approval, "
               "run QSPI_examples_format() once.\n");
    }
    return r;
}

FM_Result QSPI_examples_format(void)
{
    /* تشخیص درست: مونت هست یا نه — نه started_here. در حالت فلش نو/خالی
     * اولین FS_Mount با -12 شکست خورده و /qspi اصلاً مونت نیست؛
     * FS_Format روی مسیر مونت‌نشده NOT_MOUNTED می‌داد و فرمت هرگز
     * انجام نمی‌شد (باگ واقعی برد). آنجا باید مستقیم از طریق درایو
     * فرمت کرد و مونت کرد — همین کار را Ex15_QSPI_FormatAndMount
     * می‌کند. اگر /qspi مونت است (شروع بیرونی یا begin موفق)، همان
     * FS_Format کافی است. */
    {
        FM_Bool mounted = FM_FALSE;

        (void)FS_IsMounted(EX15_QSPI_MOUNT_PATH, &mounted);
        if (!mounted)
        {
            FM_Result r = Ex15_QSPI_FormatAndMount();

            if (r == FM_ERR_NOT_READY)
            {
                /* format قبل از هر begin ی هم صدا زده شده؟ خودم دستگاه
                 * را ثبت می‌کنم (-12 روی فلش خالی طبیعی است) و بعد
                 * فرمت می‌کنم - ترتیب فراخوانی دیگر مهم نیست. */
                (void)Ex15_QSPI_Begin();
                r = Ex15_QSPI_FormatAndMount();
            }
            qspi_step("Ex15_QSPI_FormatAndMount", r);
            return r;
        }
    }
    {
        FM_Result r = FS_Format(EX15_QSPI_MOUNT_PATH);

        qspi_step("FS_Format(/qspi)", r);
        return r;
    }
}

void QSPI_examples_end(void)
{
    /* فقط چیزی را می‌بندیم که خودمان باز کرده‌ایم. */
    if (qspi_started_here)
    {
        Ex15_QSPI_End();
        qspi_started_here = FM_FALSE;
    }
}

/*----------------------------------------------------------------------*/
/* exQ_1 — File Basics (معادل ex_1_file_basic روی QSPI)                    */
/*----------------------------------------------------------------------*/

void exQ_1_file_basic(void)
{
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    File_WriteText("/qspi/greeting.txt", "Hello, QSPI FileManager!");
    printf("Written: /qspi/greeting.txt\n");

    File_ReadText("/qspi/greeting.txt", buf, sizeof(buf), NULL);
    printf("Read:    %s\n", buf);

    File_Exists("/qspi/greeting.txt", &exists);
    printf("Exists?  %s\n", exists ? "yes" : "no");

    File_Exists("/qspi/no-such-file.txt", &exists);
    printf("NoFile?  %s\n", exists ? "yes" : "no");

    File_SizeOf("/qspi/greeting.txt", &size);
    printf("Size:    %lu bytes\n", (unsigned long)size);

    File_WriteText("/qspi/greeting.txt", "New content!");
    File_ReadText("/qspi/greeting.txt", buf, sizeof(buf), NULL);
    printf("Overwrite: %s\n", buf);

    File_Create("/qspi/empty.txt");
    File_SizeOf("/qspi/empty.txt", &size);
    printf("Empty size: %lu\n", (unsigned long)size);

    File_Delete("/qspi/greeting.txt");
    File_Exists("/qspi/greeting.txt", &exists);
    printf("Deleted? %s\n", exists ? "no" : "yes");
}

/*----------------------------------------------------------------------*/
/* exQ_2 — Copy, Move, Rename                                            */
/*----------------------------------------------------------------------*/

static void qs_copy_progress(uint64_t done, uint64_t total, void *ud)
{
    (void)ud;
    printf("  copy: %lu / %lu\n", (unsigned long)done, (unsigned long)total);
}

void exQ_2_file_copy_move_rename(void)
{
    char buf[128];

    File_WriteText("/qspi/original.txt", "This is the original file content.");

    printf("Copy:\n");
    qspi_step("File_Copy", File_Copy("/qspi/original.txt", "/qspi/copy.txt",
                                   qs_copy_progress, NULL));

    File_ReadText("/qspi/copy.txt", buf, sizeof(buf), NULL);
    printf("  content: %s\n", buf);

    qspi_step("File_Rename", File_Rename("/qspi/copy.txt", "/qspi/renamed.txt"));
    File_ReadText("/qspi/renamed.txt", buf, sizeof(buf), NULL);
    printf("Renamed: %s\n", buf);

    Directory_Create("/qspi/backup");               /* بار دوم: EXISTS، عادی */
    qspi_step("File_Move", File_Move("/qspi/renamed.txt", "/qspi/backup/final.txt",
                                   NULL, NULL));
    File_ReadText("/qspi/backup/final.txt", buf, sizeof(buf), NULL);
    printf("Moved: %s\n", buf);

    {
        FM_Bool exists;
        File_Exists("/qspi/renamed.txt", &exists);
        printf("Original exists? %s\n", exists ? "yes" : "no");
    }
}

/*----------------------------------------------------------------------*/
/* exQ_3 — Advanced File Handle                                          */
/*----------------------------------------------------------------------*/

void exQ_3_file_advance(void)
{
    FM_File *f;
    uint64_t pos, size;
    char buf[32];
    uint32_t n;

    f = File_Open("/qspi/data.bin", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (!f) { printf("open failed\n"); return; }
    File_Write(f, "AAAA", 4, &n);
    File_Write(f, "BBBB", 4, &n);
    File_Write(f, "CCCC", 4, &n);
    File_Tell(f, &pos);
    printf("After write, pos = %lu\n", (unsigned long)pos);
    File_Close(f);

    f = File_Open("/qspi/data.bin", FM_OPEN_READ);
    if (!f) { printf("open failed\n"); return; }
    File_Seek(f, 4, FM_SEEK_SET);
    File_Read(f, buf, 4, &n);
    buf[n] = '\0';
    printf("At offset 4: %s\n", buf);

    File_Seek(f, -4, FM_SEEK_END);
    File_Read(f, buf, 4, &n);
    buf[n] = '\0';
    printf("Last 4 bytes: %s\n", buf);

    File_Size(f, &size);
    printf("Size: %lu\n", (unsigned long)size);
    File_Close(f);

    f = File_Open("/qspi/data.bin", FM_OPEN_READ | FM_OPEN_WRITE);
    if (!f) { printf("open failed\n"); return; }
    File_Truncate(f, 6);
    File_Size(f, &size);
    printf("After truncate: size = %lu\n", (unsigned long)size);

    File_Seek(f, 0, FM_SEEK_SET);
    File_Read(f, buf, 6, &n);
    buf[n] = '\0';
    printf("Content: %s\n", buf);

    File_Seek(f, 0, FM_SEEK_END);
    File_Write(f, "DDDD", 4, &n);
    File_Tell(f, &pos);
    printf("After append, pos = %lu\n", (unsigned long)pos);
    File_Close(f);

    f = File_Open("/qspi/data.bin", FM_OPEN_READ);
    if (f) {
        File_Seek(f, 0, FM_SEEK_END);
        printf("EOF? %s\n", File_EOF(f) ? "yes" : "no");
        File_Close(f);
    }
}

/*----------------------------------------------------------------------*/
/* exQ_4 — Directories                                                   */
/*----------------------------------------------------------------------*/

static FM_Result qs_print_item(const FM_FileInfo *info, void *ud)
{
    (void)ud;
    printf("  [%s] %-18s %lu bytes\n",
           info->type == FM_ITEM_DIR ? "D" : "F",
           info->name, (unsigned long)info->size);
    return FM_OK;
}

void exQ_4_file_directory(void)
{
    FM_Dir *d;
    FM_DirEntry entry;
    uint32_t count;
    uint64_t total;

    Directory_Create("/qspi/logs");
    Directory_Create("/qspi/logs/2026");
    Directory_Create("/qspi/config");
    File_WriteText("/qspi/readme.txt", "root readme");
    File_WriteText("/qspi/config/app.txt", "key=value");
    File_WriteText("/qspi/logs/2026/jan.csv", "1,2,3");
    File_WriteText("/qspi/logs/2026/feb.csv", "4,5,6");

    printf("Contents of /:\n");
    d = Directory_Open("/qspi/");
    while (d && Directory_Read(d, &entry) == FM_OK)
        printf("  %s %-18s %lu\n",
               entry.type == FM_ITEM_DIR ? "[D]" : "[F]",
               entry.name, (unsigned long)entry.size);
    if (d) Directory_Close(d);

    printf("\nRecursive scan:\n");
    Directory_ScanRecursive("/qspi/", qs_print_item, NULL, NULL, NULL);

    Directory_Count("/qspi/logs", FM_ITEM_FILE, &count);
    printf("\nFiles in /logs: %lu\n", (unsigned long)count);

    Directory_Size("/qspi/", &total);
    printf("Total size: %lu bytes\n", (unsigned long)total);

    Directory_DeleteRecursive("/qspi/logs");
    printf("\nAfter cleanup (logs removed):\n");
    d = Directory_Open("/qspi/");
    while (d && Directory_Read(d, &entry) == FM_OK)
        printf("  %s\n", entry.name);
    if (d) Directory_Close(d);
}

/*----------------------------------------------------------------------*/
/* exQ_5 — JSON                                                          */
/*----------------------------------------------------------------------*/

#if FM_USE_JSON
static char     qs_text[256];
static uint32_t qs_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
#endif /* FM_USE_JSON */

void exQ_5_file_json(void)
{
#if FM_USE_JSON
    /* 1. نوشتن */
    {
        char buf[256];
        FM_JsonWriter w;
        JsonWriter_Init(&w, buf, sizeof(buf));

        JsonWriter_BeginObject(&w);
          JsonWriter_Key(&w, "name");     JsonWriter_String(&w, "STM32");
          JsonWriter_Key(&w, "baudrate"); JsonWriter_Int(&w, 115200);
          JsonWriter_Key(&w, "debug");    JsonWriter_Bool(&w, FM_TRUE);
          JsonWriter_Key(&w, "sensors");
          JsonWriter_BeginArray(&w);
            JsonWriter_Float(&w, 23.5f, 1);
            JsonWriter_Float(&w, 24.1f, 1);
            JsonWriter_Float(&w, 22.8f, 1);
          JsonWriter_EndArray(&w);
        JsonWriter_EndObject(&w);

        qspi_step("WriteText config.json",
                File_WriteText("/qspi/config.json", JsonWriter_StringValue(&w)));
        printf("Written:\n%s\n\n", JsonWriter_StringValue(&w));
    }

    /* 2. خواندن — بافرها static تا فشار روی stack نباشد */
    {
        FM_Json doc;
        char name[32];
        int32_t baud;
        FM_Bool debug;
        size_t count;
        float v0, v1, v2;

        qspi_step("Json_LoadFile",
                Json_LoadFile("/qspi/config.json", &doc, qs_text,
                              sizeof(qs_text), qs_tokens,
                              sizeof(qs_tokens)));

        Json_ReadString(&doc, "name", name, sizeof(name));
        Json_ReadInt(&doc, "baudrate", &baud);
        Json_ReadBool(&doc, "debug", &debug);
        Json_ArrayCount(&doc, "sensors", &count);
        Json_ArrayFloat(&doc, "sensors", 0, &v0);
        Json_ArrayFloat(&doc, "sensors", 1, &v1);
        Json_ArrayFloat(&doc, "sensors", 2, &v2);

        printf("name    = %s\n", name);
        printf("baud    = %d\n", (int)baud);
        printf("debug   = %s\n", debug ? "true" : "false");
        printf("sensors = [%lu] %.1f, %.1f, %.1f\n",
               (unsigned long)count, v0, v1, v2);
    }

    /* 3. API سبک embedded */
    {
        static char     s_text[256];
        static uint32_t s_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
        char  name[32];
        int32_t baud;

        Json_LoadString("/qspi/config.json", "name", name, sizeof(name),
                        s_text, sizeof(s_text), s_tokens, sizeof(s_tokens));
        printf("[api] name = %s\n", name);

        Json_LoadInt("/qspi/config.json", "baudrate", &baud,
                     s_text, sizeof(s_text), s_tokens, sizeof(s_tokens));
        printf("[api] baud = %d\n", (int)baud);
    }
#else
    printf("exQ_5 JSON: skipped (FM_USE_JSON=0)\n");
#endif /* FM_USE_JSON */
}

/*----------------------------------------------------------------------*/
/* exQ_6 — Text Line Editing                                             */
/*----------------------------------------------------------------------*/

#if FM_USE_TEXT
static void qs_show(const char *label, const char *path)
{
    char buf[512];
    printf("--- %s ---\n", label);
    File_ReadText(path, buf, sizeof(buf), NULL);
    printf("%s\n\n", buf);
}
#endif /* FM_USE_TEXT */

void exQ_6_file_text_line_edit(void)
{
#if FM_USE_TEXT
    size_t n;

    File_WriteText("/qspi/cfg.txt",
        "host=192.168.1.1\nport=8080\ntimeout=30\ndebug=false\n");
    qs_show("Original", "/qspi/cfg.txt");

    qspi_step("Text_Replace", Text_Replace("/qspi/cfg.txt", "8080", "9090"));
    qs_show("Replace 8080->9090", "/qspi/cfg.txt");

    qspi_step("Text_InsertLine", Text_InsertLine("/qspi/cfg.txt", 3, "retry=3"));
    qs_show("InsertLine 3", "/qspi/cfg.txt");

    qspi_step("Text_DeleteLine", Text_DeleteLine("/qspi/cfg.txt", 4));
    qs_show("DeleteLine 4", "/qspi/cfg.txt");

    Text_CountLines("/qspi/cfg.txt", &n);
    printf("Lines: %lu\n", (unsigned long)n);

    {
        size_t line;
        if (Text_Find("/qspi/cfg.txt", "retry", &line) == FM_OK)
            printf("'retry' at line %lu\n", (unsigned long)line);
    }

    {
        FM_File *f = File_Open("/qspi/cfg.txt", FM_OPEN_READ);
        char line[128];
        int i = 0;
        printf("\nReadLine:\n");
        while (f && Text_ReadLine(f, line, sizeof(line), NULL) == FM_OK)
            printf("  %d: %s\n", ++i, line);
        if (f) File_Close(f);
    }
#else
    printf("exQ_6 text line edit: skipped (FM_USE_TEXT=0)\n");
#endif /* FM_USE_TEXT */
}

/*----------------------------------------------------------------------*/
/* exQ_7 — Crash-Safe Writes                                             */
/*----------------------------------------------------------------------*/

#if FM_USE_SAFE_WRITE
static void qs_show2(const char *label)
{
    char buf[256];
    File_ReadText("/qspi/settings.txt", buf, sizeof(buf), NULL);
    printf("  %s => \"%s\"\n", label, buf);
}
#endif /* FM_USE_SAFE_WRITE */

void exQ_7_file_Crash_Safe_Writes(void)
{
#if FM_USE_SAFE_WRITE
    File_WriteText("/qspi/settings.txt", "version=1");
    qs_show2("Initial");

    qspi_step("File_SafeReplace",
            File_SafeReplace("/qspi/settings.txt", "version=2", 9));
    qs_show2("SafeReplace");

    qspi_step("File_SafeWriteText",
            File_SafeWriteText("/qspi/settings.txt", "version=3"));
    qs_show2("SafeWriteText");

    qspi_step("File_SafeAppend",
            File_SafeAppend("/qspi/settings.txt", "\nextra=data", 11));
    qs_show2("SafeAppend");

    qspi_step("File_Transaction",
            File_Transaction("/qspi/settings.txt", "version=FINAL", 14));
    qs_show2("Transaction");

    File_Recover("/qspi/settings.txt");
    printf("  Recover done (no stale .fm_tmp).\n");
#else
    printf("exQ_7 crash-safe writes: skipped (FM_USE_SAFE_WRITE=0)\n");
#endif /* FM_USE_SAFE_WRITE */
}

/*----------------------------------------------------------------------*/
/* exQ_8 — Data Logger (CSV)                                             */
/*----------------------------------------------------------------------*/

void exQ_8_file_Data_Logger(void)
{
#if FM_USE_DATA_LOGGER
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_LoggerInfo info;
    float values[3];

    Directory_Create("/qspi/logs");               /* بار دوم: EXISTS، عادی */

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_file_size = 512;
    cfg.max_files     = 3;
    cfg.flush_records = 5;

    qspi_step("Logger_Init",
            Logger_Init(&lg, "/qspi/logs", "sensor", FM_LOG_FORMAT_CSV, &cfg));
    Logger_Start(&lg);

    for (int i = 0; i < 15; i++)
    {
        values[0] = 20.0f + (float)i * 0.5f;
        values[1] = 1000.0f + (float)i;
        values[2] = 50.0f + (float)i * 2.0f;
        Logger_Write(&lg, (uint32_t)(i * 100), values, 3);
    }

    Logger_GetInfo(&lg, &info);
    printf("Files:     %lu\n", (unsigned long)info.files_created);
    printf("Records:   %lu\n", (unsigned long)info.record_count);
    printf("Size:      %lu bytes\n", (unsigned long)info.file_size);
    Logger_Stop(&lg);

    printf("\nLog files:\n");
    {
        FM_Dir *d = Directory_Open("/qspi/logs");
        FM_DirEntry e;
        while (d && Directory_Read(d, &e) == FM_OK)
            printf("  %s (%lu bytes)\n", e.name, (unsigned long)e.size);
        if (d) Directory_Close(d);
    }

    /* محتوای اولین فایل — FM_Log بافر 256 بایتی دارد؛ بلندتر از آن
     * truncate می‌شود (FM_LOG_BUF_SIZE را بزرگ کنید برای خروجی کامل) */
    {
        FM_Dir *d = Directory_Open("/qspi/logs");
        FM_DirEntry e;
        while (d && Directory_Read(d, &e) == FM_OK)
        {
            if (e.type == FM_ITEM_FILE)
            {
                char fullpath[FM_PATH_MAX];
                char content[512];

                Path_Join("/qspi/logs", e.name, fullpath, sizeof(fullpath));
                printf("\nContent of %s:\n", fullpath);

                if (File_ReadText(fullpath, content, sizeof(content), NULL)
                    == FM_OK)
                {
                    printf("%s\n", content);
                }
                else
                {
                    printf("  (could not read)\n");
                }
                break;
            }
        }
        if (d) Directory_Close(d);
    }
#else
    printf("exQ_8 data logger: skipped (FM_USE_DATA_LOGGER=0)\n");
#endif /* FM_USE_DATA_LOGGER */
}

/*----------------------------------------------------------------------*/
/* exQ_9 — CRC & Checksum                                                */
/*----------------------------------------------------------------------*/

void exQ_9_file_crc_cheaksum(void)
{
#if FM_USE_CRC
    const char *data = "123456789";
    size_t len = strlen(data);

    printf("Data: \"%s\" (%lu bytes)\n\n", data, (unsigned long)len);

    printf("CRC16-CCITT  = 0x%04X\n", CRC_Calculate16(data, len));
    printf("CRC16-MODBUS = 0x%04X\n", CRC_Compute16Modbus(data, len, 0xFFFF));
    printf("CRC32        = 0x%08lX  (check: 0xCBF43926)\n",
           (unsigned long)CRC_Calculate32(data, len));
    printf("Checksum8    = 0x%02X\n", CRC_Checksum8(data, len));
    printf("Checksum32   = 0x%08lX\n",
           (unsigned long)CRC_Checksum32(data, len));

    {
        char text[256];
        size_t rd;

        File_WriteText("/qspi/data.bin", "important sensor data");
        File_ReadAll("/qspi/data.bin", text, sizeof(text), &rd);
        printf("\nFile CRC32 = 0x%08lX (%lu bytes)\n",
               (unsigned long)CRC_Calculate32(text, rd), (unsigned long)rd);
    }
#else
    printf("exQ_9 CRC / checksum: skipped (FM_USE_CRC=0)\n");
#endif /* FM_USE_CRC */
}

/*----------------------------------------------------------------------*/
/* exQ_10 — BMP Image + Scale + Convert                                  */
/*----------------------------------------------------------------------*/

void exQ_10_file_BMP_Image_Scale_Convert(void)
{
    FM_ImageInfo info;

    /* 1. ساخت تصویر 4x4 RGB888 و ذخیره */
    {
        FM_Image img;
        uint8_t pixels[4 * 4 * 3];

        memset(&img, 0, sizeof(img));
        img.width  = 4;
        img.height = 4;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3;
        img.bit_depth = 24;
        img.data = pixels;
        img.data_capacity = sizeof(pixels);
        img.data_size = sizeof(pixels);

        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                int i = (y * 4 + x) * 3;
                pixels[i + 0] = (uint8_t)(x * 85);
                pixels[i + 1] = (uint8_t)(y * 85);
                pixels[i + 2] = 128;
            }

        qspi_step("Image_Save", Image_Save("/qspi/test.bmp", &img));
        printf("Saved 4x4 BMP to /qspi/test.bmp\n");
    }

    /* 2. بارگذاری و بررسی */
    Image_GetInfo("/qspi/test.bmp", &info);
    printf("Image: %lux%lu, %lu-bit, codec=%s\n",
           (unsigned long)info.width, (unsigned long)info.height,
           (unsigned long)info.bit_depth, info.codec);

    /* 3. تغییر اندازه 4x4 -> 8x8 */
    {
        uint8_t src[4 * 4 * 3];
        uint8_t dst[8 * 8 * 3];
        FM_Image src_img;

        memset(&src_img, 0, sizeof(src_img));
        Image_Load("/qspi/test.bmp", &src_img, src, sizeof(src));

        Image_Scale(src, 4, 4, FM_PIXEL_RGB888,
                    dst, 8, 8, FM_SCALE_NEAREST);
        printf("Scaled 4x4 -> 8x8 (nearest)\n");

        Image_Scale(src, 4, 4, FM_PIXEL_RGB888,
                    dst, 8, 8, FM_SCALE_BILINEAR);
        printf("Scaled 4x4 -> 8x8 (bilinear)\n");
    }

    /* 4. تبدیل RGB888 -> RGB565 */
    {
        FM_Image src, dst;
        uint8_t src_px[4 * 4 * 3];
        uint8_t dst_px[4 * 4 * 2];

        memset(&src, 0, sizeof(src));
        Image_Load("/qspi/test.bmp", &src, src_px, sizeof(src_px));

        memset(&dst, 0, sizeof(dst));
        dst.data = dst_px;
        dst.data_capacity = sizeof(dst_px);

        Image_Convert(&src, &dst, FM_PIXEL_RGB565);
        printf("Converted RGB888 -> RGB565: %lu bytes\n",
               (unsigned long)dst.data_size);
    }

    /* 5. Resize فایل مستقیم */
    qspi_step("Image_ResizeFile",
            Image_ResizeFile("/qspi/test.bmp", "/qspi/small.bmp", 2, 2,
                             FM_SCALE_NEAREST));
    Image_GetInfo("/qspi/small.bmp", &info);
    printf("Resized file: %lux%lu\n",
           (unsigned long)info.width, (unsigned long)info.height);
}

#endif /* !FM_HOST_BUILD */
#endif /* EXAMPLES_FILE_QSPI_ALL_H_ */
