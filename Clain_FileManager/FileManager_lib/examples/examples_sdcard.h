/*
 * examples_sdcard.h — معادل SD Card مثال‌های ۱ تا ۱۰
 *
 * همین مثال‌های 1_examples_file.h ولی به‌جای RAM-disk روی SD Card واقعی
 * از طریق FatFS adapter کتابخانه (fm_fatfs.c).
 *
 * تفاوت‌ها با نسخه‌ی RAM:
 *   - مونت فقط «یک‌بار» در SD_examples_begin() انجام می‌شود (به‌جای
 *     mount/unmount در هر مثال) و در SD_examples_end() بسته می‌شود.
 *   - FileManager_MountMemory / Storage_Unregister("RAM") اینجا معنا ندارد؛
 *     درایو 0 (sd_diskio کیوب) از قبل هست و FatFS_Init به آن وصل می‌شود.
 *   - همه‌ی printfها با %lu/%d/%s هستند (سازگار با mini-printf).
 *
 * پیش‌نیاز:
 *   FM_USE_FATFS=1
 *   sd_diskio.c کیوب در بیلد می‌ماند (Exclude لازم نیست)
 *
 * استفاده در main:
 *     #include "examples_sdcard.h"
 *     ...
 *     SD_examples_begin();
 *     exS_1_file_basic();          ... تا exS_10
 *     SD_examples_end();
 *
 * نکته: خروجی مثال‌ها روی SD مانا است (فایل‌ها می‌مانند) ولی اجرای
 * دوباره بی‌خطر است چون File_WriteText فایل قبلی را truncate می‌کند.
 */

#ifndef EXAMPLES_FILE_SDCARD_ALL_H_
#define EXAMPLES_FILE_SDCARD_ALL_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_fatfs.h"
#include "fm_log.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

/*----------------------------------------------------------------------*/
/* SETUP یک‌بار برای همه‌ی مثال‌ها                                        */
/*----------------------------------------------------------------------*/

static FM_FS g_sd_fs;      /* آبجکت FS متصل به درایو 0 (SD) */

static void sd_step(const char *what, FM_Result r)
{
    /* چاپ خطا فقط وقتی چیزی شکست خورده — خروجی تمیز می‌ماند */
    if (r != FM_OK)
        printf("  !! %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
}

/* صدا زدن یک‌بار در main، قبل از مثال‌ها */
FM_Result SD_examples_begin(void)
{
    FM_Result r;
    FM_Bool already = FM_FALSE;

    r = FileManager_Init();
    if (r != FM_OK) return r;

    /* اگر ریشه از قبل مونت است (مثلاً FMFS خودکار بعد از FM_SD_Hal_Attach،
     * یا begin دوم در همان بوت)، دوباره روی آن FS نگذار - دو فایل‌سیستم
     * روی یک volume = طوفان خطا و خرابی متادیتا. همان مونت را استفاده کن.
     * (همین الگو در QSPI_examples_begin هم هست.) */
    (void)FS_IsMounted("/", &already);
    if (already)
    {
        FM_FSInfo info;

        r = FS_GetInfo("/", &info);
        sd_step("FS_GetInfo(/) [reusing existing mount]", r);
        if (r == FM_OK)
        {
            printf("SD: %s, free %lu KB (existing root mount reused)\n",
                   info.type, (unsigned long)(info.free_space / 1024u));
        }
        return r;
    }

    r = FatFS_Init(&g_sd_fs, 0);          /* درایو 0 -> FileManager */
    sd_step("FatFS_Init(0)", r);
    if (r != FM_OK) return r;

    r = FS_Mount("/", &g_sd_fs);          /* ریشه = SD */
    sd_step("FS_Mount(\"/\")", r);
    if (r != FM_OK) return r;

    {
        FM_FSInfo info;                    /* اولین دسترسی واقعی به کارت */
        r = FS_GetInfo("/", &info);
        sd_step("FS_GetInfo", r);
        if (r == FM_OK)
            printf("SD: %s, free %lu KB\n", info.type,
                   (unsigned long)(info.free_space / 1024u));
    }
    return r;
}

/* در پایان (اختیاری — مونت را می‌بندد) */
void SD_examples_end(void)
{
    (void)FS_Unmount("/");
}

/*----------------------------------------------------------------------*/
/* exS_1 — File Basics (معادل ex_1_file_basic روی SD)                    */
/*----------------------------------------------------------------------*/

void exS_1_file_basic(void)
{
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    File_WriteText("/greeting.txt", "Hello, SD FileManager!");
    printf("Written: /greeting.txt\n");

    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("Read:    %s\n", buf);

    File_Exists("/greeting.txt", &exists);
    printf("Exists?  %s\n", exists ? "yes" : "no");

    File_Exists("/no-such-file.txt", &exists);
    printf("NoFile?  %s\n", exists ? "yes" : "no");

    File_SizeOf("/greeting.txt", &size);
    printf("Size:    %lu bytes\n", (unsigned long)size);

    File_WriteText("/greeting.txt", "New content!");
    File_ReadText("/greeting.txt", buf, sizeof(buf), NULL);
    printf("Overwrite: %s\n", buf);

    File_Create("/empty.txt");
    File_SizeOf("/empty.txt", &size);
    printf("Empty size: %lu\n", (unsigned long)size);

    File_Delete("/greeting.txt");
    File_Exists("/greeting.txt", &exists);
    printf("Deleted? %s\n", exists ? "no" : "yes");
}

/*----------------------------------------------------------------------*/
/* exS_2 — Copy, Move, Rename                                            */
/*----------------------------------------------------------------------*/

static void sds_copy_progress(uint64_t done, uint64_t total, void *ud)
{
    (void)ud;
    printf("  copy: %lu / %lu\n", (unsigned long)done, (unsigned long)total);
}

void exS_2_file_copy_move_rename(void)
{
    char buf[128];

    File_WriteText("/original.txt", "This is the original file content.");

    printf("Copy:\n");
    sd_step("File_Copy", File_Copy("/original.txt", "/copy.txt",
                                   sds_copy_progress, NULL));

    File_ReadText("/copy.txt", buf, sizeof(buf), NULL);
    printf("  content: %s\n", buf);

    sd_step("File_Rename", File_Rename("/copy.txt", "/renamed.txt"));
    File_ReadText("/renamed.txt", buf, sizeof(buf), NULL);
    printf("Renamed: %s\n", buf);

    Directory_Create("/backup");               /* بار دوم: EXISTS، عادی */
    sd_step("File_Move", File_Move("/renamed.txt", "/backup/final.txt",
                                   NULL, NULL));
    File_ReadText("/backup/final.txt", buf, sizeof(buf), NULL);
    printf("Moved: %s\n", buf);

    {
        FM_Bool exists;
        File_Exists("/renamed.txt", &exists);
        printf("Original exists? %s\n", exists ? "yes" : "no");
    }
}

/*----------------------------------------------------------------------*/
/* exS_3 — Advanced File Handle                                          */
/*----------------------------------------------------------------------*/

void exS_3_file_advance(void)
{
    FM_File *f;
    uint64_t pos, size;
    char buf[32];
    uint32_t n;

    f = File_Open("/data.bin", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (!f) { printf("open failed\n"); return; }
    File_Write(f, "AAAA", 4, &n);
    File_Write(f, "BBBB", 4, &n);
    File_Write(f, "CCCC", 4, &n);
    File_Tell(f, &pos);
    printf("After write, pos = %lu\n", (unsigned long)pos);
    File_Close(f);

    f = File_Open("/data.bin", FM_OPEN_READ);
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

    f = File_Open("/data.bin", FM_OPEN_READ | FM_OPEN_WRITE);
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

    f = File_Open("/data.bin", FM_OPEN_READ);
    if (f) {
        File_Seek(f, 0, FM_SEEK_END);
        printf("EOF? %s\n", File_EOF(f) ? "yes" : "no");
        File_Close(f);
    }
}

/*----------------------------------------------------------------------*/
/* exS_4 — Directories                                                   */
/*----------------------------------------------------------------------*/

static FM_Result sds_print_item(const FM_FileInfo *info, void *ud)
{
    (void)ud;
    printf("  [%s] %-18s %lu bytes\n",
           info->type == FM_ITEM_DIR ? "D" : "F",
           info->name, (unsigned long)info->size);
    return FM_OK;
}

void exS_4_file_directory(void)
{
    FM_Dir *d;
    FM_DirEntry entry;
    uint32_t count;
    uint64_t total;

    Directory_Create("/logs");
    Directory_Create("/logs/2026");
    Directory_Create("/config");
    File_WriteText("/readme.txt", "root readme");
    File_WriteText("/config/app.txt", "key=value");
    File_WriteText("/logs/2026/jan.csv", "1,2,3");
    File_WriteText("/logs/2026/feb.csv", "4,5,6");

    printf("Contents of /:\n");
    d = Directory_Open("/");
    while (d && Directory_Read(d, &entry) == FM_OK)
        printf("  %s %-18s %lu\n",
               entry.type == FM_ITEM_DIR ? "[D]" : "[F]",
               entry.name, (unsigned long)entry.size);
    if (d) Directory_Close(d);

    printf("\nRecursive scan:\n");
    Directory_ScanRecursive("/", sds_print_item, NULL, NULL, NULL);

    Directory_Count("/logs", FM_ITEM_FILE, &count);
    printf("\nFiles in /logs: %lu\n", (unsigned long)count);

    Directory_Size("/", &total);
    printf("Total size: %lu bytes\n", (unsigned long)total);

    Directory_DeleteRecursive("/logs");
    printf("\nAfter cleanup (logs removed):\n");
    d = Directory_Open("/");
    while (d && Directory_Read(d, &entry) == FM_OK)
        printf("  %s\n", entry.name);
    if (d) Directory_Close(d);
}

/*----------------------------------------------------------------------*/
/* exS_5 — JSON                                                          */
/*----------------------------------------------------------------------*/

#if FM_USE_JSON
static char     sds_text[256];
static uint32_t sds_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
#endif /* FM_USE_JSON */

void exS_5_file_json(void)
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

        sd_step("WriteText config.json",
                File_WriteText("/config.json", JsonWriter_StringValue(&w)));
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

        sd_step("Json_LoadFile",
                Json_LoadFile("/config.json", &doc, sds_text,
                              sizeof(sds_text), sds_tokens,
                              sizeof(sds_tokens)));

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

        Json_LoadString("/config.json", "name", name, sizeof(name),
                        s_text, sizeof(s_text), s_tokens, sizeof(s_tokens));
        printf("[api] name = %s\n", name);

        Json_LoadInt("/config.json", "baudrate", &baud,
                     s_text, sizeof(s_text), s_tokens, sizeof(s_tokens));
        printf("[api] baud = %d\n", (int)baud);
    }
#else
    printf("exS_5 JSON: skipped (FM_USE_JSON=0)\n");
#endif /* FM_USE_JSON */
}

/*----------------------------------------------------------------------*/
/* exS_6 — Text Line Editing                                             */
/*----------------------------------------------------------------------*/

#if FM_USE_TEXT
static void sds_show(const char *label, const char *path)
{
    char buf[512];
    printf("--- %s ---\n", label);
    File_ReadText(path, buf, sizeof(buf), NULL);
    printf("%s\n\n", buf);
}
#endif /* FM_USE_TEXT */

void exS_6_file_text_line_edit(void)
{
#if FM_USE_TEXT
    size_t n;

    File_WriteText("/cfg.txt",
        "host=192.168.1.1\nport=8080\ntimeout=30\ndebug=false\n");
    sds_show("Original", "/cfg.txt");

    sd_step("Text_Replace", Text_Replace("/cfg.txt", "8080", "9090"));
    sds_show("Replace 8080->9090", "/cfg.txt");

    sd_step("Text_InsertLine", Text_InsertLine("/cfg.txt", 3, "retry=3"));
    sds_show("InsertLine 3", "/cfg.txt");

    sd_step("Text_DeleteLine", Text_DeleteLine("/cfg.txt", 4));
    sds_show("DeleteLine 4", "/cfg.txt");

    Text_CountLines("/cfg.txt", &n);
    printf("Lines: %lu\n", (unsigned long)n);

    {
        size_t line;
        if (Text_Find("/cfg.txt", "retry", &line) == FM_OK)
            printf("'retry' at line %lu\n", (unsigned long)line);
    }

    {
        FM_File *f = File_Open("/cfg.txt", FM_OPEN_READ);
        char line[128];
        int i = 0;
        printf("\nReadLine:\n");
        while (f && Text_ReadLine(f, line, sizeof(line), NULL) == FM_OK)
            printf("  %d: %s\n", ++i, line);
        if (f) File_Close(f);
    }
#else
    printf("exS_6 text line edit: skipped (FM_USE_TEXT=0)\n");
#endif /* FM_USE_TEXT */
}

/*----------------------------------------------------------------------*/
/* exS_7 — Crash-Safe Writes                                             */
/*----------------------------------------------------------------------*/

#if FM_USE_SAFE_WRITE
static void sds_show2(const char *label)
{
    char buf[256];
    File_ReadText("/settings.txt", buf, sizeof(buf), NULL);
    printf("  %s => \"%s\"\n", label, buf);
}
#endif /* FM_USE_SAFE_WRITE */

void exS_7_file_Crash_Safe_Writes(void)
{
#if FM_USE_SAFE_WRITE
    File_WriteText("/settings.txt", "version=1");
    sds_show2("Initial");

    sd_step("File_SafeReplace",
            File_SafeReplace("/settings.txt", "version=2", 9));
    sds_show2("SafeReplace");

    sd_step("File_SafeWriteText",
            File_SafeWriteText("/settings.txt", "version=3"));
    sds_show2("SafeWriteText");

    sd_step("File_SafeAppend",
            File_SafeAppend("/settings.txt", "\nextra=data", 11));
    sds_show2("SafeAppend");

    sd_step("File_Transaction",
            File_Transaction("/settings.txt", "version=FINAL", 14));
    sds_show2("Transaction");

    File_Recover("/settings.txt");
    printf("  Recover done (no stale .fm_tmp).\n");
#else
    printf("exS_7 crash-safe writes: skipped (FM_USE_SAFE_WRITE=0)\n");
#endif /* FM_USE_SAFE_WRITE */
}

/*----------------------------------------------------------------------*/
/* exS_8 — Data Logger (CSV)                                             */
/*----------------------------------------------------------------------*/

void exS_8_file_Data_Logger(void)
{
#if FM_USE_DATA_LOGGER
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_LoggerInfo info;
    float values[3];

    Directory_Create("/logs");               /* بار دوم: EXISTS، عادی */

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_file_size = 512;
    cfg.max_files     = 3;
    cfg.flush_records = 5;

    sd_step("Logger_Init",
            Logger_Init(&lg, "/logs", "sensor", FM_LOG_FORMAT_CSV, &cfg));
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
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (d && Directory_Read(d, &e) == FM_OK)
            printf("  %s (%lu bytes)\n", e.name, (unsigned long)e.size);
        if (d) Directory_Close(d);
    }

    /* محتوای اولین فایل — FM_Log بافر 256 بایتی دارد؛ بلندتر از آن
     * truncate می‌شود (FM_LOG_BUF_SIZE را بزرگ کنید برای خروجی کامل) */
    {
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (d && Directory_Read(d, &e) == FM_OK)
        {
            if (e.type == FM_ITEM_FILE)
            {
                char fullpath[FM_PATH_MAX];
                char content[512];

                Path_Join("/logs", e.name, fullpath, sizeof(fullpath));
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
    printf("exS_8 data logger: skipped (FM_USE_DATA_LOGGER=0)\n");
#endif /* FM_USE_DATA_LOGGER */
}

/*----------------------------------------------------------------------*/
/* exS_9 — CRC & Checksum                                                */
/*----------------------------------------------------------------------*/

void exS_9_file_crc_cheaksum(void)
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

        File_WriteText("/data.bin", "important sensor data");
        File_ReadAll("/data.bin", text, sizeof(text), &rd);
        printf("\nFile CRC32 = 0x%08lX (%lu bytes)\n",
               (unsigned long)CRC_Calculate32(text, rd), (unsigned long)rd);
    }
#else
    printf("exS_9 CRC / checksum: skipped (FM_USE_CRC=0)\n");
#endif /* FM_USE_CRC */
}

/*----------------------------------------------------------------------*/
/* exS_10 — BMP Image + Scale + Convert                                  */
/*----------------------------------------------------------------------*/

void exS_10_file_BMP_Image_Scale_Convert(void)
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

        sd_step("Image_Save", Image_Save("/test.bmp", &img));
        printf("Saved 4x4 BMP to /test.bmp\n");
    }

    /* 2. بارگذاری و بررسی */
    Image_GetInfo("/test.bmp", &info);
    printf("Image: %lux%lu, %lu-bit, codec=%s\n",
           (unsigned long)info.width, (unsigned long)info.height,
           (unsigned long)info.bit_depth, info.codec);

    /* 3. تغییر اندازه 4x4 -> 8x8 */
    {
        uint8_t src[4 * 4 * 3];
        uint8_t dst[8 * 8 * 3];
        FM_Image src_img;

        memset(&src_img, 0, sizeof(src_img));
        Image_Load("/test.bmp", &src_img, src, sizeof(src));

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
        Image_Load("/test.bmp", &src, src_px, sizeof(src_px));

        memset(&dst, 0, sizeof(dst));
        dst.data = dst_px;
        dst.data_capacity = sizeof(dst_px);

        Image_Convert(&src, &dst, FM_PIXEL_RGB565);
        printf("Converted RGB888 -> RGB565: %lu bytes\n",
               (unsigned long)dst.data_size);
    }

    /* 5. Resize فایل مستقیم */
    sd_step("Image_ResizeFile",
            Image_ResizeFile("/test.bmp", "/small.bmp", 2, 2,
                             FM_SCALE_NEAREST));
    Image_GetInfo("/small.bmp", &info);
    printf("Resized file: %lux%lu\n",
           (unsigned long)info.width, (unsigned long)info.height);
}

#endif /* EXAMPLES_FILE_SDCARD_ALL_H_ */
