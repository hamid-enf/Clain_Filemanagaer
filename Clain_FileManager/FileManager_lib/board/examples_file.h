/*
 * examples_file.h — مثال‌های ۱ تا ۱۰ روی RAM-disk (بدون SD/QSPI)
 * =============================================================
 *
 * این فایل جایگزین/بازسازی‌شده‌ی `examples_file.h` است که در `defines.h`
 * include می‌شود. همه‌ی توابعی که `main.c` صدا می‌زند اینجاست:
 *
 *     ex_1_file_basic                    (File Basics)
 *     ex_2_file_copy_move_rename         (Copy / Move / Rename)
 *     ex_3_file_advance                  (Advanced handle)
 *     ex_4_file_directory                (Directories)
 *     ex_5_file_json                     (JSON)
 *     ex_6_file_text_line_edit           (Text line editing)
 *     ex_7_file_Crash_Safe_Writes        (Safe/crash-safe writes)
 *     ex_8_file_Data_Logger              (CSV logger)
 *     ex_9_file_crc_cheaksum             (CRC / checksum)
 *     ex_10_file_BMP_Image_Scale_Convert (BMP + scale + convert)
 *
 * طراحی:
 *   - یک بافر ۱۲۸KB در `.sdram_buffers` برای RAM-disk استفاده می‌شود.
 *   - هر مثال مستقل است: در ابتدا RAM را mount می‌کند و در انتها
 *     unmount + unregister می‌کند تا بعد از بلوک `ex_*` بتوانیم در
 *     `SD_examples_begin()` دوباره "/" را روی SD بگذاریم.
 *   - چون توابع static هستند و این فایل توسط چند کد (`main.c`, `mdma.c`, ...)
 *     به‌صورت یکسان include می‌شود، هیچ نماد (symbol) تکراری در لینک نمی‌دهد.
 */

#ifndef BOARD_EXAMPLES_FILE_H_
#define BOARD_EXAMPLES_FILE_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "file_manager.h"
#include "fm_storage.h"
#include "fm_log.h"

#ifndef printf
#define printf FM_Log
#endif
#ifndef puts
#define puts(s) FM_Log("%s\n", (s))
#endif

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                   */
/* ------------------------------------------------------------------ */
/* نکته‌ی مهم: در main این برد Ex35_Sd_FatRoot() قبل از این بلوک فراخوانی
 * می‌شود و "/" را روی SD مونت کرده است. پس این مثال‌ها همان "/" آماده را
 * استفاده می‌کنند و هیچ RAM-disk تکراری/بزرگی نمی‌سازیم. این هم دقیقاً
 * همان چیزی است که بهینه‌سازی بافرها را حفظ می‌کند (بدون ۱۲۸KB اضافه).
 */
static FM_Bool g_file_ready = FM_FALSE;

static void ex_file_step(const char *what, FM_Result r)
{
    /* چاپ خطا فقط وقتی چیزی شکست خورده — خروجی تمیز می‌ماند */
    if (r != FM_OK)
        printf("  !! %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
}

static void ex_file_begin(void)
{
    FM_Result r;
    FM_Bool mounted = FM_FALSE;

    if (g_file_ready)
        return;

    r = FileManager_Init();
    ex_file_step("FileManager_Init", r);
    if (r != FM_OK)
        return;

    (void)FS_IsMounted("/", &mounted);
    if (!mounted)
    {
        printf("  !! ex_file: '/' is not mounted - call Ex35_Sd_FatRoot() "
               "or SD_examples_begin() first\n");
        return;
    }

    g_file_ready = FM_TRUE;
}

static void ex_file_end(void)
{
    /* "/" متعلق به SD است و توسط Ex35_Sd_FatRoot / SD_examples_begin مدیریت
     * می‌شود؛ این‌جا آن را unmount نمی‌کنیم. */
    g_file_ready = FM_FALSE;
}

/* ------------------------------------------------------------------ */
/* ex_1 — File Basics                                                 */
/* ------------------------------------------------------------------ */
static void ex_1_file_basic(void)
{
    char buf[128];
    FM_Bool exists;
    uint64_t size;

    ex_file_begin();
    if (!g_file_ready) return;

    File_WriteText("/greeting.txt", "Hello, FileManager!");
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

    ex_file_end();
}

/* ------------------------------------------------------------------ */
/* ex_2 — Copy, Move, Rename                                          */
/* ------------------------------------------------------------------ */
static void rmc_copy_progress(uint64_t done, uint64_t total, void *ud)
{
    (void)ud;
    printf("  copy: %lu / %lu\n", (unsigned long)done, (unsigned long)total);
}

static void ex_2_file_copy_move_rename(void)
{
    char buf[128];
    FM_Bool exists;

    ex_file_begin();
    if (!g_file_ready) return;

    File_WriteText("/original.txt", "This is the original file content.");

    printf("Copy:\n");
    ex_file_step("File_Copy", File_Copy("/original.txt", "/copy.txt",
                                        rmc_copy_progress, NULL));
    File_ReadText("/copy.txt", buf, sizeof(buf), NULL);
    printf("  content: %s\n", buf);

    ex_file_step("File_Rename", File_Rename("/copy.txt", "/renamed.txt"));
    File_ReadText("/renamed.txt", buf, sizeof(buf), NULL);
    printf("Renamed: %s\n", buf);

    Directory_Create("/backup");               /* بار دوم: EXISTS، عادی */
    ex_file_step("File_Move",
                 File_Move("/renamed.txt", "/backup/final.txt", NULL, NULL));
    File_ReadText("/backup/final.txt", buf, sizeof(buf), NULL);
    printf("Moved: %s\n", buf);

    File_Exists("/renamed.txt", &exists);
    printf("Original exists? %s\n", exists ? "yes" : "no");

    ex_file_end();
}

/* ------------------------------------------------------------------ */
/* ex_3 — Advanced File Handle                                         */
/* ------------------------------------------------------------------ */
static void ex_3_file_advance(void)
{
    FM_File *f;
    uint64_t pos, size;
    char buf[32];
    uint32_t n;

    ex_file_begin();
    if (!g_file_ready) return;

    f = File_Open("/data.bin", FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (!f) { printf("open failed\n"); ex_file_end(); return; }
    File_Write(f, "AAAA", 4, &n);
    File_Write(f, "BBBB", 4, &n);
    File_Write(f, "CCCC", 4, &n);
    File_Tell(f, &pos);
    printf("After write, pos = %lu\n", (unsigned long)pos);
    File_Close(f);

    f = File_Open("/data.bin", FM_OPEN_READ);
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
    File_Seek(f, 0, FM_SEEK_END);
    printf("EOF? %s\n", File_EOF(f) ? "yes" : "no");
    File_Close(f);

    ex_file_end();
}

/* ------------------------------------------------------------------ */
/* ex_4 — Directories                                                 */
/* ------------------------------------------------------------------ */
static FM_Result dir_print_item(const FM_FileInfo *info, void *ud)
{
    (void)ud;
    printf("  [%s] %-18s %lu bytes\n",
           info->type == FM_ITEM_DIR ? "D" : "F",
           info->name, (unsigned long)info->size);
    return FM_OK;
}

static void ex_4_file_directory(void)
{
    FM_Dir *d;
    FM_DirEntry entry;
    uint32_t count;
    uint64_t total;

    ex_file_begin();
    if (!g_file_ready) return;

    Directory_Create("/logs");
    Directory_Create("/logs/2026");
    Directory_Create("/config");
    File_WriteText("/readme.txt", "root readme");
    File_WriteText("/config/app.txt", "key=value");
    File_WriteText("/logs/2026/jan.csv", "1,2,3");
    File_WriteText("/logs/2026/feb.csv", "4,5,6");

    printf("Contents of /:\n");
    d = Directory_Open("/");
    while (Directory_Read(d, &entry) == FM_OK)
        printf("  %s %-18s %lu\n",
               entry.type == FM_ITEM_DIR ? "[D]" : "[F]",
               entry.name, (unsigned long)entry.size);
    Directory_Close(d);

    printf("\nRecursive scan:\n");
    Directory_ScanRecursive("/", dir_print_item, NULL, NULL, NULL);

    Directory_Count("/logs", FM_ITEM_FILE, &count);
    printf("\nFiles in /logs: %u\n", (unsigned)count);

    Directory_Size("/", &total);
    printf("Total size: %lu bytes\n", (unsigned long)total);

    Directory_DeleteRecursive("/logs");
    printf("\nAfter cleanup:\n");
    d = Directory_Open("/");
    while (Directory_Read(d, &entry) == FM_OK)
        printf("  %s\n", entry.name);
    Directory_Close(d);

    ex_file_end();
}

/* ------------------------------------------------------------------ */
/* ex_5 — JSON                                                       */
/* ------------------------------------------------------------------ */
static void ex_5_file_json(void)
{
#if FM_USE_JSON
    ex_file_begin();
    if (!g_file_ready) return;

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

        File_WriteText("/config.json", JsonWriter_StringValue(&w));
        printf("Written:\n%s\n\n", JsonWriter_StringValue(&w));
    }

    {
        FM_Json doc;
        char text[256];
        uint32_t tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
        char name[32];
        int32_t baud;
        FM_Bool debug;
        size_t count;
        float v0, v1, v2;

        Json_LoadFile("/config.json", &doc, text, sizeof(text),
                      tokens, sizeof(tokens));

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

    {
        static char     s_text[256];
        static uint32_t s_tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
        char  name[32];
        int32_t baud;

        Json_LoadString("/config.json", "name", name, sizeof(name),
                        s_text, sizeof(s_text),
                        s_tokens, sizeof(s_tokens));
        printf("[api] name = %s\n", name);

        Json_LoadInt("/config.json", "baudrate", &baud,
                     s_text, sizeof(s_text),
                     s_tokens, sizeof(s_tokens));
        printf("[api] baud = %d\n", (int)baud);
    }

    ex_file_end();
#else
    printf("ex_5 JSON: skipped (FM_USE_JSON=0)\n");
#endif /* FM_USE_JSON */
}

/* ------------------------------------------------------------------ */
/* ex_6 — Text Line Editing                                           */
/* ------------------------------------------------------------------ */
#if FM_USE_TEXT
static void txt_show(const char *label, const char *path)
{
    char buf[512];
    printf("--- %s ---\n", label);
    File_ReadText(path, buf, sizeof(buf), NULL);
    printf("%s\n\n", buf);
}
#endif /* FM_USE_TEXT */

static void ex_6_file_text_line_edit(void)
{
#if FM_USE_TEXT
    size_t n;

    ex_file_begin();
    if (!g_file_ready) return;

    File_WriteText("/cfg.txt",
                   "host=192.168.1.1\nport=8080\ntimeout=30\ndebug=false\n");
    txt_show("Original", "/cfg.txt");

    Text_Replace("/cfg.txt", "8080", "9090");
    txt_show("Replace 8080->9090", "/cfg.txt");

    Text_InsertLine("/cfg.txt", 3, "retry=3");
    txt_show("InsertLine 3", "/cfg.txt");

    Text_DeleteLine("/cfg.txt", 4);
    txt_show("DeleteLine 4", "/cfg.txt");

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
        while (Text_ReadLine(f, line, sizeof(line), NULL) == FM_OK)
            printf("  %d: %s\n", ++i, line);
        File_Close(f);
    }

    ex_file_end();
#else
    printf("ex_6 text line edit: skipped (FM_USE_TEXT=0)\n");
#endif /* FM_USE_TEXT */
}

/* ------------------------------------------------------------------ */
/* ex_7 — Safe / Crash-Safe Writes                                    */
/* ------------------------------------------------------------------ */
#if FM_USE_SAFE_WRITE
static void safe_show(const char *label)
{
    char buf[256];
    File_ReadText("/settings.txt", buf, sizeof(buf), NULL);
    printf("  %s => \"%s\"\n", label, buf);
}
#endif /* FM_USE_SAFE_WRITE */

static void ex_7_file_Crash_Safe_Writes(void)
{
#if FM_USE_SAFE_WRITE
    ex_file_begin();
    if (!g_file_ready) return;

    File_WriteText("/settings.txt", "version=1");
    safe_show("Initial");

    File_SafeReplace("/settings.txt", "version=2", 9);
    safe_show("SafeReplace");

    File_SafeWriteText("/settings.txt", "version=3");
    safe_show("SafeWriteText");

    File_SafeAppend("/settings.txt", "\nextra=data", 11);
    safe_show("SafeAppend");

    File_Transaction("/settings.txt", "version=FINAL", 14);
    safe_show("Transaction");

    File_Recover("/settings.txt");
    printf("  Recover done (no stale .fm_tmp).\n");

    ex_file_end();
#else
    printf("ex_7 crash-safe writes: skipped (FM_USE_SAFE_WRITE=0)\n");
#endif /* FM_USE_SAFE_WRITE */
}

/* ------------------------------------------------------------------ */
/* ex_8 — Data Logger (CSV)                                           */
/* ------------------------------------------------------------------ */
static void ex_8_file_Data_Logger(void)
{
#if FM_USE_DATA_LOGGER
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_LoggerInfo info;
    float values[3];

    ex_file_begin();
    if (!g_file_ready) return;

    Directory_Create("/logs");

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_file_size = 512;
    cfg.max_files     = 3;
    cfg.flush_records = 5;

    ex_file_step("Logger_Init",
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
    printf("Files:     %u\n", (unsigned)info.files_created);
    printf("Records:   %lu\n", (unsigned long)info.record_count);
    printf("Size:      %lu bytes\n", (unsigned long)info.file_size);
    Logger_Stop(&lg);

    printf("\nLog files:\n");
    {
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (Directory_Read(d, &e) == FM_OK)
        {
            char fullpath[FM_PATH_MAX];
            Path_Join("/logs", e.name, fullpath, sizeof(fullpath));
            printf("  %s (%lu bytes)\n", e.name, (unsigned long)e.size);
        }
        Directory_Close(d);
    }

    {
        FM_Dir *d = Directory_Open("/logs");
        FM_DirEntry e;
        while (Directory_Read(d, &e) == FM_OK)
        {
            if (e.type == FM_ITEM_FILE)
            {
                char fullpath[FM_PATH_MAX];
                char content[512];

                Path_Join("/logs", e.name, fullpath, sizeof(fullpath));
                printf("\nContent of %s:\n", fullpath);
                if (File_ReadText(fullpath, content, sizeof(content), NULL)
                    == FM_OK)
                    printf("%s\n", content);
                else
                    printf("  (could not read)\n");
                break;
            }
        }
        Directory_Close(d);
    }

    ex_file_end();
#else
    printf("ex_8 data logger: skipped (FM_USE_DATA_LOGGER=0)\n");
#endif /* FM_USE_DATA_LOGGER */
}

/* ------------------------------------------------------------------ */
/* ex_9 — CRC & Checksum                                              */
/* ------------------------------------------------------------------ */
static void ex_9_file_crc_cheaksum(void)
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

    ex_file_begin();
    if (!g_file_ready) return;

    {
        char text[256];
        size_t rd;

        File_WriteText("/data.bin", "important sensor data");
        File_ReadAll("/data.bin", text, sizeof(text), &rd);
        printf("\nFile CRC32 = 0x%08lX (%lu bytes)\n",
               (unsigned long)CRC_Calculate32(text, rd), (unsigned long)rd);
    }

    ex_file_end();
#else
    printf("ex_9 CRC / checksum: skipped (FM_USE_CRC=0)\n");
#endif /* FM_USE_CRC */
}

/* ------------------------------------------------------------------ */
/* ex_10 — BMP Image + Scale + Convert                                 */
/* ------------------------------------------------------------------ */
static void ex_10_file_BMP_Image_Scale_Convert(void)
{
    FM_ImageInfo info;

    ex_file_begin();
    if (!g_file_ready) return;

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
        {
            for (int x = 0; x < 4; x++)
            {
                int i = (y * 4 + x) * 3;
                pixels[i + 0] = (uint8_t)(x * 85);
                pixels[i + 1] = (uint8_t)(y * 85);
                pixels[i + 2] = 128;
            }
        }

        ex_file_step("Image_Save", Image_Save("/test.bmp", &img));
        printf("Saved 4x4 BMP to /test.bmp\n");
    }

    Image_GetInfo("/test.bmp", &info);
    printf("Image: %ux%u, %u-bit, codec=%s\n",
           info.width, info.height, info.bit_depth, info.codec);

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

    {
        FM_Image src, dst;
        uint8_t src_px[4 * 4 * 3];
        uint8_t dst_px[4 * 4 * 2];

        memset(&src, 0, sizeof(src));
        Image_Load("/test.bmp", &src, src_px, sizeof(src_px));

        memset(&dst, 0, sizeof(dst));
        dst.data = dst_px;
        dst.data_capacity = sizeof(dst_px);

        ex_file_step("Image_Convert",
                     Image_Convert(&src, &dst, FM_PIXEL_RGB565));
        printf("Converted RGB888 -> RGB565: %u bytes\n",
               (unsigned)dst.data_size);
    }

    ex_file_step("Image_ResizeFile",
                 Image_ResizeFile("/test.bmp", "/small.bmp", 2, 2,
                                  FM_SCALE_NEAREST));
    Image_GetInfo("/small.bmp", &info);
    printf("Resized file: %ux%u\n", info.width, info.height);

    ex_file_end();
}

/* ------------------------------------------------------------------ */
/* ex_11 — کدک‌های جدید صدا: WAV(ADPCM/A-law/µ-law) + FLAC + Vorbis   */
/*         + Opus (دیکد واقعی پس از PR #25)                           */
/* ------------------------------------------------------------------ */
/*
 * فایل‌های نمونهٔ ریپو را روی SD در پوشهٔ /media بریزید (اگر پوشه
 * نبود، ریشهٔ / هم چک می‌شود). منبع فایل‌ها در ریپو:
 *
 *   audio_test/wav_adpcm_ima_22k.wav   (WAV IMA-ADPCM — دیکد جدید)
 *   audio_test/wav_alaw_8k.wav         (WAV A-law   — دیکد جدید)
 *   audio_test/wav_mulaw_8k.wav        (WAV µ-law  — دیکد جدید)
 *   test/vectors/test_flac.flac        (FLAC via dr_flac)
 *   test/vectors/test_vorbis.ogg       (Vorbis via stb_vorbis)
 *   test/vectors/test_opus.ogg         (Opus via libopus — دیکد واقعی)
 *
 * هر فایل: Audio_Open → چاپ استریم → دیکد کامل (طول/پیک) → Close.
 * فقط از API هسته استفاده می‌کند (بدون HAL) و روی هر پیکربندی کامپایل
 * می‌شود؛ کدک‌های غیرفعال با #if حذف و فقط اطلاع داده می‌شوند.
 *
 * نکتهٔ حافظه: FLAC/Vorbis/Opus مثل همیشهٔ این کتابخانه «کل فایل در
 * RAM دیکد می‌شود» — فایل‌های نمونه کوچک‌اند (۵..۱۶KB) ولی برای
 * آهنگ‌های واقعی به SDRAM و سقف FM_OPUS_MAX_PCM_BYTES دقت کنید.
 */
#if FM_USE_AUDIO
static const char *ex_11_find(const char *name)
{
    static char path[80];
    FM_Bool exists = FM_FALSE;

    (void)snprintf(path, sizeof(path), "/media/%s", name);
    if (File_Exists(path, &exists) == FM_OK && exists)
        return path;
    (void)snprintf(path, sizeof(path), "/%s", name);
    if (File_Exists(path, &exists) == FM_OK && exists)
        return path;
    return NULL;
}

static void ex_11_try(const char *name)
{
    const char *path = ex_11_find(name);
    FM_Audio a;
    FM_AudioInfo info;
    FM_Result r;
    int16_t buf[2048];
    size_t n = 0, total = 0, i;
    int peak = 0;

    if (path == NULL)
    {
        printf("  %-24s skip (not on SD)\n", name);
        printf("  -> copy from repo: audio_test/%s (and test/vectors/*)\n"
               "     to the SD root (or /music), then re-run\n", name);
        return;
    }

    memset(&a, 0, sizeof(a));
    r = Audio_Open(path, &a);
    if (r != FM_OK)
    {
        printf("  %-24s OPEN FAIL: %s (%d)\n", name,
               File_GetErrorString(r), (int)r);
        return;
    }
    if (Audio_GetInfo(&a, &info) != FM_OK)
        memset(&info, 0, sizeof(info));

    for (;;)
    {
        r = Audio_Read(&a, buf, 2048, &n);
        if (r != FM_OK || n == 0)
            break;                          /* FM_ERR_EOF = پایان طبیعی */
        for (i = 0; i < n; i++)
        {
            int mag = (buf[i] < 0) ? -buf[i] : buf[i];
            if (mag > peak) peak = mag;
        }
        total += n;
    }
    (void)Audio_Close(&a);

    printf("  %-24s %5lu Hz %u ch %-7s %6lu smp peak %5d  %s\n",
           name, (unsigned long)info.sample_rate,
           (unsigned)info.channels, info.codec,
           (unsigned long)(total / (info.channels ? info.channels : 1u)),
           peak, (peak > 0) ? "OK" : "silent");
}
#endif /* FM_USE_AUDIO */

static void ex_11_audio_codecs(void)
{
#if FM_USE_AUDIO
    ex_file_begin();
    if (!g_file_ready) return;

    printf("--- audio codecs (new decoders) ---\n");
    ex_11_try("wav_adpcm_ima_22k.wav");
    ex_11_try("wav_alaw_8k.wav");
    ex_11_try("wav_mulaw_8k.wav");
#if FM_USE_FLAC
    ex_11_try("test_flac.flac");
#else
    printf("  %-24s skip (FM_USE_FLAC=0)\n", "test_flac.flac");
#endif
#if FM_USE_VORBIS
    ex_11_try("test_vorbis.ogg");
#else
    printf("  %-24s skip (FM_USE_VORBIS=0)\n", "test_vorbis.ogg");
#endif
#if FM_USE_OPUS
    ex_11_try("test_opus.ogg");
#else
    printf("  %-24s skip (FM_USE_OPUS=0)\n", "test_opus.ogg");
#endif
    printf("(peak>0 یعنی دیکد واقعی؛ silent یعنی fallback/سکوت)\n");

    ex_file_end();
#endif /* FM_USE_AUDIO */
}

#endif /* BOARD_EXAMPLES_FILE_H_ */
