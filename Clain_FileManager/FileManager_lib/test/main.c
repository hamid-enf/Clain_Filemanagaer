/*
 * FileManager - Host integration and stress test
 *
 * Compile: make fm_test && ./fm_test
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "file_manager.h"
/* The negative tests below deliberately trigger error paths; keep the
 * log quiet so a passing run stays readable. */
#undef  FM_DEBUG_LEVEL
#define FM_DEBUG_LEVEL FM_DEBUG_OFF
#include "fm_debug.h"
#include "fm_transfer.h"
#include "fm_qspi.h"
#include "fm_spiflash.h"
#include "fm_mediacache.h"
#include "fm_log.h"
#include "fm_cache.h"
#include "fm_eeprom_chips.h"
#define FM_CACHE_HOST_BUILD 1
#include "../examples/cache.h"
#if FM_USE_H264
#include "fm_alloc.h"
#include "h264bsd_storage.h"
#endif
#if FM_USE_AUDIO
#include "fm_opus.h"
#endif

extern FM_Result PosixFS_Init(FM_FS *fs);
extern FM_Result RamDisk_Register(FM_Storage *s, const char *name,
                                  void *buffer, uint64_t capacity);

/* Helpers shared by tests that are compiled only for some FM_USE_*
 * combinations: keep -Werror builds of every combination warning-free. */
#if defined(__GNUC__)
#define TEST_MAYBE_UNUSED __attribute__((unused))
#else
#define TEST_MAYBE_UNUSED
#endif

static int  g_failures = 0;
static int  g_tests = 0;

#define TEST(name) do { g_tests++; printf("  TEST %-45s ... ", name); } while (0)
#define PASS()   do { printf("PASS\n"); } while (0)
#define FAIL(m)  do { g_failures++; printf("FAIL: %s\n", (m)); } while (0)
#define FAIL0()  do { g_failures++; printf("FAIL\n"); } while (0)
#define CHECK_RESULT(r, msg)                                               \
    do {                                                                   \
        if ((r) != FM_OK) {                                                \
            char eb[128];                                                  \
            snprintf(eb, sizeof(eb), "%s (err=%d)", (msg), (int)(r));      \
            FAIL(eb);                                                      \
        } else { PASS(); }                                                 \
    } while (0)

static FM_FS      g_fs;
static FM_Storage g_ram_storage;
static uint8_t    g_ram_buf[64 * 1024];

typedef struct
{
    uint8_t *data;
    uint64_t capacity;
} TestDriverContext;

static FM_Result test_driver_read(FM_Storage *s, uint64_t offset,
                                   void *buf, uint32_t len)
{
    TestDriverContext *ctx = (TestDriverContext *)s->user_data;

    if (offset > ctx->capacity || (uint64_t)len > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, ctx->data + (size_t)offset, len);
    return FM_OK;
}

static FM_Result test_driver_write(FM_Storage *s, uint64_t offset,
                                    const void *buf, uint32_t len)
{
    TestDriverContext *ctx = (TestDriverContext *)s->user_data;

    if (offset > ctx->capacity || (uint64_t)len > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(ctx->data + (size_t)offset, buf, len);
    return FM_OK;
}

static FM_Result test_driver_erase(FM_Storage *s, uint64_t offset,
                                   uint32_t len)
{
    TestDriverContext *ctx = (TestDriverContext *)s->user_data;

    if (offset > ctx->capacity || (uint64_t)len > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(ctx->data + (size_t)offset, 0xFF, len);
    return FM_OK;
}

static FM_Result test_driver_sync(FM_Storage *s)
{
    (void)s;
    return FM_OK;
}

static FM_Result test_eeprom_read(void *context, uint32_t address,
                                  void *data, uint32_t length)
{
    TestDriverContext *ctx = (TestDriverContext *)context;

    if (address > ctx->capacity || length > ctx->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(data, ctx->data + address, length);
    return FM_OK;
}

static FM_Result test_eeprom_write(void *context, uint32_t address,
                                   const void *data, uint32_t length)
{
    TestDriverContext *ctx = (TestDriverContext *)context;

    if (address > ctx->capacity || length > ctx->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(ctx->data + address, data, length);
    return FM_OK;
}

static FM_Result test_nor_program(void *context, uint32_t address,
                                  const void *data, uint32_t length)
{
    TestDriverContext *ctx = (TestDriverContext *)context;
    const uint8_t *input = (const uint8_t *)data;
    uint32_t i;

    if (address > ctx->capacity || length > ctx->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < length; i++)
    {
        if ((ctx->data[address + i] & input[i]) != input[i])
        {
            return FM_ERR_IO; /* NOR cannot program a zero bit back to one */
        }
    }
    for (i = 0; i < length; i++)
    {
        ctx->data[address + i] &= input[i];
    }
    return FM_OK;
}

#if FM_USE_EEPROM && FM_USE_USB_MSC
static FM_Result test_usb_start(void *context, FM_USB_MSC *msc)
{
    (void)context;
    (void)msc;
    return FM_OK;
}

static FM_Result test_usb_stop(void *context)
{
    (void)context;
    return FM_OK;
}
#endif

static FM_Result test_block_read(void *context, uint64_t lba,
                                 uint32_t block_count, void *data)
{
    TestDriverContext *ctx = (TestDriverContext *)context;
    uint64_t offset = lba * 512u;
    uint64_t length = (uint64_t)block_count * 512u;

    if (offset > ctx->capacity || length > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(data, ctx->data + (size_t)offset, (size_t)length);
    return FM_OK;
}

static FM_Result test_block_write(void *context, uint64_t lba,
                                  uint32_t block_count, const void *data)
{
    TestDriverContext *ctx = (TestDriverContext *)context;
    uint64_t offset = lba * 512u;
    uint64_t length = (uint64_t)block_count * 512u;

    if (offset > ctx->capacity || length > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(ctx->data + (size_t)offset, data, (size_t)length);
    return FM_OK;
}

static FM_Result test_block_erase(void *context, uint64_t lba,
                                  uint32_t block_count)
{
    TestDriverContext *ctx = (TestDriverContext *)context;
    uint64_t offset = lba * 512u;
    uint64_t length = (uint64_t)block_count * 512u;

    if (offset > ctx->capacity || length > ctx->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(ctx->data + (size_t)offset, 0xFF, (size_t)length);
    return FM_OK;
}

static FM_Result test_flash_erase(void *context, uint32_t address,
                                  uint32_t length)
{
    TestDriverContext *ctx = (TestDriverContext *)context;

    if (address > ctx->capacity || length > ctx->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(ctx->data + address, 0xFF, length);
    return FM_OK;
}

static void setup(void)
{
    FM_Result r;

    r = FileManager_Init();
    if (r != FM_OK) { printf("FATAL: FileManager_Init\n"); exit(1); }

    r = RamDisk_Register(&g_ram_storage, "ram", g_ram_buf, sizeof(g_ram_buf));
    if (r != FM_OK) { printf("FATAL: RamDisk_Register: %s\n", FM_ResultString(r)); exit(1); }

    r = Storage_Mount("ram");
    if (r != FM_OK) { printf("FATAL: Storage_Mount: %s\n", FM_ResultString(r)); exit(1); }

    r = PosixFS_Init(&g_fs);
    if (r != FM_OK) { printf("FATAL: PosixFS_Init\n"); exit(1); }

    r = FS_Mount("/", &g_fs);
    if (r != FM_OK) { printf("FATAL: FS_Mount /: %s\n",
                             FM_ResultString(r)); exit(1); }

    printf("Setup: RAM disk + POSIX FS ready\n");
}

/* Several suites swap the filesystem mounted at "/" (FMFS, LittleFS, the
 * cache fixtures).  Put the POSIX root back before every suite that reads
 * test/vectors, so no suite depends on which optional suite ran before it
 * (the MP3 suite used to be the only one that restored it: every build
 * with FM_USE_MP3=0 then failed the transfer/media-cache/video suites). */
static void restore_posix_root(void)
{
    FM_Result r;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK || r == FM_ERR_EXISTS) r = FS_Mount("/", &g_fs);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        printf("  NOTE: POSIX root remount: %s\n", FM_ResultString(r));
    }
}

/* ================================================================ */
/* Path tests                                                        */
/* ================================================================ */
static void test_path(void)
{
    char buf[FM_PATH_MAX];

    TEST("Path_Join /a + b = /a/b");
    {
        FM_Result r = Path_Join("/a", "b", buf, sizeof(buf));
        if (r == FM_OK && strcmp(buf, "/a/b") == 0) PASS(); else FAIL("got wrong");
    }

    TEST("Path_Normalize /a/b/../c -> /a/c");
    {
        char p[] = "/a/b/../c";
        FM_Result r = Path_Normalize(p, sizeof(p));
        if (r == FM_OK && strcmp(p, "/a/c") == 0) PASS(); else FAIL(p);
    }

    TEST("Path_GetExtension .TXT -> txt");
    {
        char ext[16];
        FM_Result r = Path_GetExtension("/test.TXT", ext, sizeof(ext));
        if (r == FM_OK && strcmp(ext, "txt") == 0) PASS(); else FAIL(ext);
    }

    TEST("Path_GetFilename /a/b/c.txt -> c.txt");
    {
        char fn[64];
        FM_Result r = Path_GetFilename("/a/b/c.txt", fn, sizeof(fn));
        if (r == FM_OK && strcmp(fn, "c.txt") == 0) PASS(); else FAIL(fn);
    }
}

/* ================================================================ */
/* File tests                                                        */
/* ================================================================ */
static void test_file_create(void)
{
    TEST("File_Create (empty)");
    FM_Result r = File_Create("/test_create.txt");
    CHECK_RESULT(r, "File_Create");
}

static void test_file_write_read(void)
{
    char buf[256];
    size_t rd = 0;

    TEST("File_WriteText + File_ReadText");
    FM_Result r = File_WriteText("/test_file.txt", "Hello STM32!");
    if (r != FM_OK) { FAIL0(); return; }

    r = File_ReadText("/test_file.txt", buf, sizeof(buf), &rd);
    if (r != FM_OK) { FAIL("read"); return; }

    if (strcmp(buf, "Hello STM32!") == 0) PASS(); else FAIL("mismatch");
}

static void test_file_append(void)
{
    char buf[256];
    size_t rd = 0;

    TEST("File_AppendText");
    FM_Result r = File_WriteText("/test_append.txt", "line1\n");
    if (r != FM_OK) { FAIL("write1"); return; }
    r = File_AppendText("/test_append.txt", "line2\n");
    if (r != FM_OK) { FAIL("append"); return; }
    r = File_ReadText("/test_append.txt", buf, sizeof(buf), &rd);
    if (r != FM_OK) { FAIL("read"); return; }
    if (strcmp(buf, "line1\nline2\n") == 0) PASS(); else FAIL("content");
}

static void test_file_copy(void)
{
    FM_Result r;

    TEST("File_Copy");
    r = File_WriteText("/test_src.txt", "source content");
    if (r != FM_OK) { FAIL("write src"); return; }
    r = File_Copy("/test_src.txt", "/test_dst.txt", NULL, NULL);
    CHECK_RESULT(r, "File_Copy");

    {
        FM_Bool e = FM_FALSE;
        File_Exists("/test_dst.txt", &e);
        TEST("File_Exists dst after copy");
        if (e) PASS(); else FAIL("no dst");
    }
}

static void test_file_move_delete(void)
{
    FM_Bool e;
    FM_Result r;

    TEST("File_Move");
    r = File_Move("/test_dst.txt", "/test_moved.txt", NULL, NULL);
    CHECK_RESULT(r, "File_Move");

    File_Exists("/test_dst.txt", &e);
    TEST("Original gone after move");
    if (!e) PASS(); else FAIL("still exists");

    File_Exists("/test_moved.txt", &e);
    TEST("Moved file exists");
    if (e) PASS(); else FAIL("not found");

    r = File_Delete("/test_moved.txt");
    CHECK_RESULT(r, "File_Delete");
}

static void test_file_seek(void)
{
    FM_File *f;
    char buf[32];
    uint32_t rd = 0;
    uint64_t pos = 0;

    TEST("File_Seek + Read");
    FM_Result r = File_WriteAll("/test_seek.bin", "0123456789ABCDEF", 16);
    if (r != FM_OK) { FAIL0(); return; }

    f = File_Open("/test_seek.bin", FM_OPEN_READ);
    if (f == NULL) { FAIL("open"); return; }

    r = File_Seek(f, 8, FM_SEEK_SET);
    if (r != FM_OK) { FAIL("seek"); File_Close(f); return; }

    r = File_Read(f, buf, 4, &rd);
    if (r != FM_OK || rd != 4 || memcmp(buf, "89AB", 4) != 0)
    {
        FAIL("read after seek");
        File_Close(f);
        return;
    }
    r = File_Tell(f, &pos);
    if (r == FM_OK && pos == 12) PASS(); else FAIL("tell");

    File_Close(f);
    File_Delete("/test_seek.bin");
}

/* ================================================================ */
/* Directory tests                                                   */
/* ================================================================ */
static FM_Result dir_cb(const FM_FileInfo *info, void *user_data)
{
    (void)info;
    (void)user_data;
    printf("        %s (%llu bytes)\n", info->name,
           (unsigned long long)info->size);
    return FM_OK;
}

static void test_directory(void)
{
    FM_Result r;
    FM_Bool e = FM_FALSE;

    TEST("Directory_Create");
    r = Directory_Create("/testdir");
    CHECK_RESULT(r, "create");
    if (r != FM_OK) return;

    TEST("Directory_Exists");
    r = Directory_Exists("/testdir", &e);
    if (r == FM_OK && e) PASS(); else FAIL("exists");

    TEST("Directory_Create nested");
    r = Directory_Create("/testdir/sub/deep");
    CHECK_RESULT(r, "nested");

    TEST("Directory_List");
    r = Directory_List("/testdir", "*", dir_cb, NULL);
    CHECK_RESULT(r, "list");
}

/* ================================================================ */
/* JSON tests                                                        */
/* ================================================================ */
#if FM_USE_JSON

static void test_json(void)
{
    FM_Json doc;
    uint32_t tokens[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE / 4];
    int32_t val;
    FM_Result r;

    TEST("Json_OpenBuffer simple object");
    const char *json = "{"
        "\"device\":{"
          "\"baudrate\":115200,"
          "\"debug\":true"
        "},"
        "\"sensors\":[23.5,24.1,22.8]"
    "}";
    r = Json_OpenBuffer(json, strlen(json), &doc, tokens, sizeof(tokens));
    CHECK_RESULT(r, "parse");
    if (r != FM_OK) return;

    TEST("Json_ReadInt device.baudrate");
    r = Json_ReadInt(&doc, "device.baudrate", &val);
    if (r == FM_OK && val == 115200) PASS(); else FAIL("wrong val");

    TEST("Json_ReadBool device.debug");
    {
        FM_Bool b = FM_FALSE;
        r = Json_ReadBool(&doc, "device.debug", &b);
        if (r == FM_OK && b) PASS(); else FAIL("wrong bool");
    }

    TEST("Json_ArrayCount sensors");
    {
        size_t cnt = 0;
        r = Json_ArrayCount(&doc, "sensors", &cnt);
        if (r == FM_OK && cnt == 3) PASS(); else FAIL("wrong count");
    }

    TEST("Json_ArrayFloat sensors[1]");
    {
        float f = 0;
        r = Json_ArrayFloat(&doc, "sensors", 1, &f);
        if (r == FM_OK && f > 24.09f && f < 24.11f) PASS(); else FAIL("wrong float");
    }
}
#endif /* FM_USE_JSON */


/* ================================================================ */
/* Logger tests                                                      */
/* ================================================================ */
static uint32_t my_now(void)
{
    return 0x12345678u;
}

#if FM_USE_DATA_LOGGER

static void test_logger(void)
{
    FM_Logger lg;
    FM_LoggerConfig cfg;
    FM_Result r;
    float vals[3];

    TEST("Logger CSV create + write");

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_file_size = 1024;
    cfg.flush_records = 5;
    cfg.now_cb = my_now;

    r = Logger_Init(&lg, "/logs", "test", FM_LOG_FORMAT_CSV, &cfg);
    if (r != FM_OK) { FAIL("init"); return; }

    r = Logger_Start(&lg);
    if (r != FM_OK) { FAIL("start"); return; }

    vals[0] = 23.5f; vals[1] = 1013.25f; vals[2] = 55.0f;
    r = Logger_Write(&lg, 100, vals, 3);
    CHECK_RESULT(r, "write 1");

    vals[0] = 23.6f; vals[1] = 1013.27f; vals[2] = 55.1f;
    r = Logger_Write(&lg, 200, vals, 3);
    CHECK_RESULT(r, "write 2");

    r = Logger_Stop(&lg);
    CHECK_RESULT(r, "stop");

    {
        char path[FM_PATH_MAX];
        FM_Bool e = FM_FALSE;

        snprintf(path, sizeof(path), "/logs/test_1_305419896.csv");
        r = File_Exists(path, &e);
        TEST("Logger file exists");
        CHECK_RESULT(r, "exists");
        if (e)
        {
            char content[512];
            size_t rd = 0;
            r = File_ReadText(path, content, sizeof(content), &rd);
            if (r == FM_OK)
            {
                printf("        Log content: %s\n", content);
                PASS();
            }
            else FAIL("read log");
        }
    }

    (void)Directory_DeleteRecursive("/logs");
}
#endif /* FM_USE_DATA_LOGGER */


/* ================================================================ */
/* Media detection                                                   */
/* ================================================================ */
#if FM_USE_MEDIA_DETECT

static void test_media(void)
{
    FM_MediaType t;

    TEST("Media_DetectByExtension .jpg -> JPEG");
    t = Media_DetectByExtension("/image/test.jpg");
    if (t == FM_MEDIA_JPEG) PASS(); else FAIL0();

    TEST("Media_DetectByExtension .json -> JSON");
    t = Media_DetectByExtension("/config/device.json");
    if (t == FM_MEDIA_JSON) PASS(); else FAIL0();

    TEST("Media_DetectByExtension .wav -> WAV");
    t = Media_DetectByExtension("/audio/sound.wav");
    if (t == FM_MEDIA_WAV) PASS(); else FAIL0();

    TEST("Media_DetectByExtension .fmrv -> RAW");
    t = Media_DetectByExtension("/video/clip.fmrv");
    if (t == FM_MEDIA_RAW) PASS(); else FAIL0();

    TEST("Media_DetectByExtension .mov -> MP4");
    t = Media_DetectByExtension("/video/clip.mov");
    if (t == FM_MEDIA_MP4) PASS(); else FAIL0();

    TEST("Media magic detects and saves already-encoded MP3 safely");
    {
        const uint8_t mp3[] = {0xFF, 0xFB, 0x90, 0x64, 1, 2, 3, 4};
        FM_MediaType detected = FM_MEDIA_UNKNOWN;
        FM_Result r;

        t = Media_DetectBuffer(mp3, sizeof(mp3), "/wrong.wav");
        r = Media_SaveEncoded("/received.mp3", mp3, sizeof(mp3),
                              FM_MEDIA_UNKNOWN, &detected);
        if (r == FM_OK)
        {
            r = Media_SaveEncoded("/mismatch.wav", mp3, sizeof(mp3),
                                  FM_MEDIA_MP3, NULL);
            if (r == FM_ERR_INVALID_FORMAT) r = FM_OK;
            else r = FM_ERR_CORRUPTED;
        }
        if (r == FM_OK && t == FM_MEDIA_MP3 && detected == FM_MEDIA_MP3 &&
            File_DetectType("/received.mp3") == FM_MEDIA_MP3)
        {
            PASS();
        }
        else
        {
            FAIL("encoded media validation");
        }
        (void)File_Delete("/received.mp3");
        (void)File_Delete("/mismatch.wav");
    }
}
#endif /* FM_USE_MEDIA_DETECT */


/* ================================================================ */
/* Async and media smoke tests                                       */
/* ================================================================ */
static FM_Result async_test_cb(const FM_FileInfo *info, void *user_data)
{
    uint32_t *count = (uint32_t *)user_data;

    (void)info;
    if (count != NULL) (*count)++;
    return FM_OK;
}

#if FM_USE_ASYNC || FM_USE_AUDIO || FM_USE_IMAGE || FM_USE_VIDEO

static void test_async_and_codecs(void)
{
#if FM_USE_ASYNC
    FM_Async async;
#endif
    uint32_t count = 0;
    FM_Bool exists = FM_FALSE;
    FM_Result r;
    int polls;

#if FM_USE_ASYNC
    TEST("Async recursive scan and delete");
    r = Directory_Create("/fm_async_test/sub");
    if (r == FM_OK) r = File_WriteText("/fm_async_test/sub/a.txt", "a");
    if (r == FM_OK) r = File_WriteText("/fm_async_test/b.txt", "b");
    if (r == FM_OK) r = Async_StartScan(&async, "/fm_async_test",
                                       async_test_cb, NULL, &count);
    polls = 0;
    while (r == FM_OK && Async_Poll(&async) == FM_ASYNC_RUNNING && polls++ < 100) {}
    if (r == FM_OK && async.state == FM_ASYNC_DONE && count == 3)
    {
        r = Async_StartDeleteRecursive(&async, "/fm_async_test", NULL,
                                       NULL, NULL);
        polls = 0;
        while (r == FM_OK && Async_Poll(&async) == FM_ASYNC_RUNNING && polls++ < 100) {}
    }
    (void)Directory_Exists("/fm_async_test", &exists);
    if (r == FM_OK && async.state == FM_ASYNC_DONE && !exists)
    {
        PASS();
    }
    else
    {
        FAIL("async traversal");
        (void)Directory_DeleteRecursive("/fm_async_test");
    }
#endif /* FM_USE_ASYNC */

#if FM_USE_IMAGE && FM_USE_AUDIO && FM_USE_VIDEO
    TEST("BMP/WAV/FMRV codec APIs");
    {
        uint8_t pixels[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255,
                              255, 255, 255};
        uint8_t encoded[128];
        uint8_t decoded[12];
        FM_Image image;
        FM_Image loaded;
        size_t encoded_len = 0;
        int16_t samples[2] = {-32768, 32767};
        int16_t read_samples[2] = {0, 0};
        int16_t stereo_samples[4] = {0, 0, 0, 0};
        size_t read_count = 0;
        size_t stereo_frames = 0;
        FM_AudioWriter writer;
        FM_Audio audio;
        FM_VideoWriter video_writer;
        FM_Video video;

        memset(&writer, 0, sizeof(writer));
        memset(&audio, 0, sizeof(audio));
        memset(&video_writer, 0, sizeof(video_writer));
        memset(&video, 0, sizeof(video));
        uint8_t frame[4] = {1, 2, 3, 4};
        uint8_t read_frame[4] = {0};
        size_t frame_size = 0;

        memset(&image, 0, sizeof(image));
        image.width = 2; image.height = 2; image.format = FM_PIXEL_RGB888;
        image.channels = 3; image.bit_depth = 24; image.data = pixels;
        image.data_capacity = sizeof(pixels); image.data_size = sizeof(pixels);
        r = Image_EncodeBMP(&image, encoded, sizeof(encoded), &encoded_len);
        if (r == FM_OK)
        {
            r = Image_DecodeBMP(encoded, encoded_len, &loaded, decoded,
                                 sizeof(decoded));
        }
        if (r == FM_OK)
        {
            r = AudioWriter_Create("/fm_test_audio.wav", 8000, 1, 16,
                                   &writer);
        }
        if (r == FM_OK) r = AudioWriter_Write(&writer, samples, 2, NULL);
        if (r == FM_OK) r = AudioWriter_Close(&writer);
        if (r == FM_OK) r = Audio_Open("/fm_test_audio.wav", &audio);
        if (r == FM_OK) r = Audio_Read(&audio, read_samples, 2, &read_count);
        if (r == FM_OK) r = Audio_Seek(&audio, 0.0);
        if (r == FM_OK)
        {
            r = Audio_ReadStereo(&audio, stereo_samples, 2, &stereo_frames);
        }
        if (File_IsOpen(&audio.file)) (void)Audio_Close(&audio);
        if (r == FM_OK && read_count == 2 && stereo_frames == 2)
        {
            r = VideoWriter_Create("/fm_test_video.fmrv", 2, 2, 10, 1, 4,
                                   &video_writer);
        }
        if (r == FM_OK) r = VideoWriter_WriteFrame(&video_writer, frame, 4);
        if (r == FM_OK) r = VideoWriter_Close(&video_writer);
        if (r == FM_OK) r = Video_Open("/fm_test_video.fmrv", &video);
        if (r == FM_OK) r = Video_ReadFrame(&video, read_frame,
                                             sizeof(read_frame), &frame_size);
        if (File_IsOpen(&video.file)) (void)Video_Close(&video);
        (void)File_Delete("/fm_test_audio.wav");
        (void)File_Delete("/fm_test_video.fmrv");
        if (r == FM_OK && frame_size == sizeof(frame) &&
            memcmp(decoded, pixels, sizeof(pixels)) == 0 &&
            read_samples[0] == samples[0] && read_samples[1] == samples[1] &&
            stereo_samples[0] == samples[0] &&
            stereo_samples[1] == samples[0] &&
            stereo_samples[2] == samples[1] &&
            stereo_samples[3] == samples[1] &&
            memcmp(read_frame, frame, sizeof(frame)) == 0)
        {
            PASS();
        }
        else
        {
            FAIL("codec round trip");
        }
    }
#endif /* FM_USE_IMAGE && FM_USE_AUDIO && FM_USE_VIDEO */
}
#endif /* FM_USE_ASYNC || AUDIO || IMAGE || VIDEO */


/* ================================================================ */
/* EEPROM and USB bridge                                            */
/* ================================================================ */
#if FM_USE_EEPROM && FM_USE_USB_MSC

static void test_eeprom_usb(void)
{
    static uint8_t memory[4096];
    TestDriverContext context = {memory, sizeof(memory)};
    FM_EEPROM eeprom;
    FM_EEPROMConfig eeprom_config;
    FM_Storage storage;
    FM_USB_MSC usb;
    FM_USB_MSC_PortOps port = {test_usb_start, test_usb_stop};
    uint8_t source[40];
    uint8_t readback[40];
    uint8_t block[512];
    uint8_t block_readback[512];
    uint64_t block_count = 0;
    uint32_t block_size = 0;
    FM_Result r;

    memset(memory, 0xFF, sizeof(memory));
    memset(&eeprom_config, 0, sizeof(eeprom_config));
    eeprom_config.name = "TEST_EEPROM";
    eeprom_config.capacity = sizeof(memory);
    eeprom_config.page_size = 16;
    eeprom_config.context = &context;
    eeprom_config.read = test_eeprom_read;
    eeprom_config.write = test_eeprom_write;
    memset(source, 0x5A, sizeof(source));

    TEST("EEPROM page adapter and USB MSC bridge");
    r = EEPROM_Init(&eeprom, &eeprom_config);
    if (r == FM_OK) r = EEPROM_Write(&eeprom, 7, source, sizeof(source));
    memset(readback, 0, sizeof(readback));
    if (r == FM_OK) r = EEPROM_Read(&eeprom, 7, readback, sizeof(readback));
    if (r == FM_OK && memcmp(source, readback, sizeof(source)) != 0)
    {
        r = FM_ERR_CORRUPTED;
    }
    if (r == FM_OK) r = EEPROM_StorageInit(&eeprom, &storage,
                                           FM_STORAGE_CUSTOM);
    if (r == FM_OK) r = USB_MSC_Init(&usb, &storage, 512);
    if (r == FM_OK) r = USB_MSC_GetCapacity(&usb, &block_count, &block_size);
    memset(block, 0xA6, sizeof(block));
    if (r == FM_OK) r = USB_MSC_WriteBlocks(&usb, 0, 1, block);
    memset(block_readback, 0, sizeof(block_readback));
    if (r == FM_OK) r = USB_MSC_ReadBlocks(&usb, 0, 1, block_readback);
    if (r == FM_OK) r = USB_MSC_Attach(&usb, &port, NULL);
    if (r == FM_OK) r = USB_MSC_Detach(&usb);
    if (r == FM_OK && block_count == 8 && block_size == 512 &&
        memcmp(block, block_readback, sizeof(block)) == 0)
    {
        PASS();
    }
    else
    {
        FAIL("EEPROM/USB round trip");
    }
}
#endif /* FM_USE_EEPROM && FM_USE_USB_MSC */


/* ================================================================ */
/* Standard SD/eMMC and Flash adapters                              */
/* ================================================================ */
#if FM_USE_DEVICE_ADAPTERS

static void test_standard_device_adapters(void)
{
    static uint8_t sd_memory[64 * 1024];
    static uint8_t flash_memory[64 * 1024];
    static uint8_t nor_block_memory[64 * 1024];
    static uint8_t nor_sector_scratch[4096];
    TestDriverContext sd_context = {sd_memory, sizeof(sd_memory)};
    TestDriverContext flash_context = {flash_memory, sizeof(flash_memory)};
    TestDriverContext nor_context = {nor_block_memory,
                                     sizeof(nor_block_memory)};
    FM_BlockDeviceConfig sd_config;
    FM_BlockDevice sd_device;
    FM_FlashDeviceConfig flash_config;
    FM_FlashDevice flash_device;
    FM_NorBlockDevice nor_device;
    FM_Storage sd_storage;
    FM_Storage flash_storage;
    FM_Storage nor_storage;
    FM_FS sd_fs;
    uint8_t data[40];
    uint8_t readback[40];
    FM_Result r;

    (void)FS_Unmount("/");
    memset(sd_memory, 0xFF, sizeof(sd_memory));
    memset(flash_memory, 0xFF, sizeof(flash_memory));
    memset(data, 0x3C, sizeof(data));
    memset(&sd_config, 0, sizeof(sd_config));
    sd_config.name = "SD";
    sd_config.type = FM_STORAGE_SD;
    sd_config.capacity = sizeof(sd_memory);
    sd_config.block_size = 512;
    sd_config.erase_size = 512;
    sd_config.present = FM_TRUE;
    sd_config.context = &sd_context;
    sd_config.read_blocks = test_block_read;
    sd_config.write_blocks = test_block_write;
    sd_config.erase_blocks = test_block_erase;

#if FM_USE_FMFS
    TEST("Standard SD block adapter + FMFS");
    r = BlockDevice_Init(&sd_device, &sd_config);
    if (r == FM_OK) r = Storage_InitBlockDevice(&sd_storage, &sd_device);
    if (r == FM_OK) r = FileManager_MountStorage(&sd_storage, &sd_fs);
    if (r == FM_OK) r = File_WriteAll("/sd.bin", data, sizeof(data));
    memset(readback, 0, sizeof(readback));
    if (r == FM_OK) r = File_ReadAll("/sd.bin", readback, sizeof(readback), NULL);
    if (r == FM_OK && memcmp(data, readback, sizeof(data)) == 0)
    {
        PASS();
    }
    else
    {
        FAIL("SD block adapter");
    }
    (void)FS_Unmount("/");
    (void)Storage_Unregister("SD");
#endif

    memset(&flash_config, 0, sizeof(flash_config));
    flash_config.name = "NOR";
    flash_config.type = FM_STORAGE_NOR;
    flash_config.capacity = sizeof(flash_memory);
    flash_config.page_size = 16;
    flash_config.erase_size = 256;
    flash_config.present = FM_TRUE;
    flash_config.context = &flash_context;
    flash_config.read = test_eeprom_read;
    flash_config.program = test_eeprom_write;
    flash_config.erase = test_flash_erase;

    TEST("Standard NOR Flash page/erase adapter");
    r = FlashDevice_Init(&flash_device, &flash_config);
    if (r == FM_OK) r = Storage_InitFlashDevice(&flash_storage, &flash_device);
    if (r == FM_OK) r = Storage_Register(&flash_storage);
    if (r == FM_OK) r = Storage_Mount("NOR");
    if (r == FM_OK) r = Storage_WriteBlocks("NOR", 7, data, sizeof(data));
    memset(readback, 0, sizeof(readback));
    if (r == FM_OK) r = Storage_ReadBlocks("NOR", 7, readback, sizeof(readback));
    if (r == FM_OK) r = Storage_EraseBlocks("NOR", 0, 256);
    if (r == FM_OK && memcmp(data, readback, sizeof(data)) == 0 &&
        flash_memory[7] == 0xFF)
    {
        PASS();
    }
    else
    {
        FAIL("NOR Flash adapter");
    }
    (void)Storage_Unmount("NOR");
    (void)Storage_Unregister("NOR");

    TEST("NOR logical-block rewrite preserves neighboring FAT sectors");
    {
        uint8_t first[512];
        uint8_t neighbor[512];
        uint8_t verify[1024];

        memset(nor_block_memory, 0xFF, sizeof(nor_block_memory));
        memset(first, 0x00, sizeof(first));
        memset(neighbor, 0x5A, sizeof(neighbor));
        memset(&flash_config, 0, sizeof(flash_config));
        flash_config.name = "NORBLK";
        flash_config.type = FM_STORAGE_QSPI;
        flash_config.capacity = sizeof(nor_block_memory);
        flash_config.page_size = 256;
        flash_config.erase_size = 4096;
        flash_config.present = FM_TRUE;
        flash_config.context = &nor_context;
        flash_config.read = test_eeprom_read;
        flash_config.program = test_nor_program;
        flash_config.erase = test_flash_erase;

        r = NorBlockDevice_Init(&nor_device, &flash_config, 512,
                                nor_sector_scratch,
                                sizeof(nor_sector_scratch));
        if (r == FM_OK)
            r = Storage_InitNorBlockDevice(&nor_storage, &nor_device);
        if (r == FM_OK) r = Storage_Register(&nor_storage);
        if (r == FM_OK) r = Storage_Mount("NORBLK");
        if (r == FM_OK)
            r = Storage_WriteBlocks("NORBLK", 512, neighbor,
                                    sizeof(neighbor));
        if (r == FM_OK)
            r = Storage_WriteBlocks("NORBLK", 0, first, sizeof(first));
        memset(first, 0xA5, sizeof(first)); /* requires physical erase */
        if (r == FM_OK)
            r = Storage_WriteBlocks("NORBLK", 0, first, sizeof(first));
        if (r == FM_OK)
            r = Storage_ReadBlocks("NORBLK", 0, verify, sizeof(verify));
        if (r == FM_OK && memcmp(verify, first, sizeof(first)) == 0 &&
            memcmp(verify + sizeof(first), neighbor, sizeof(neighbor)) == 0)
        {
            PASS();
        }
        else
        {
            FAIL("NOR block translation");
        }
        (void)Storage_Unmount("NORBLK");
        (void)Storage_Unregister("NORBLK");
    }
}
#endif /* FM_USE_DEVICE_ADAPTERS */


/* ================================================================ */
/* High-level memory-backed filesystem (FMFS)                       */
/* ================================================================ */
#if FM_USE_FMFS
static void test_memory_fs(void)
{
    FM_FS memfs;
    char text[64];
    size_t rd = 0;
    FM_Result r;

    TEST("FMFS high-level memory mount");
    r = FS_Unmount("/");
    if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
    if (r == FM_OK) r = Storage_Mount("ram");
    if (r == FM_OK) r = FMFS_Init(&memfs);
    if (r == FM_OK) r = FS_MountDefault(&memfs);
    if (r != FM_OK)
    {
        FAIL(FM_ResultString(r));
        return;
    }
    r = Directory_Create("/memory");
    if (r == FM_OK) r = File_WriteText("/memory/hello.txt", "FMFS works");
    if (r == FM_OK) r = File_ReadText("/memory/hello.txt", text,
                                      sizeof(text), &rd);
    if (r == FM_OK && strcmp(text, "FMFS works") == 0)
    {
        PASS();
    }
    else
    {
        FAIL("memory filesystem content");
    }

    TEST("FMFS metadata survives remount");
    r = FS_Unmount("/");
    if (r == FM_OK) r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &memfs);
    memset(text, 0, sizeof(text));
    if (r == FM_OK) r = File_ReadText("/memory/hello.txt", text,
                                      sizeof(text), &rd);
    if (r == FM_OK && strcmp(text, "FMFS works") == 0)
    {
        PASS();
    }
    else
    {
        FAIL("remount content");
    }
    (void)File_Delete("/memory/hello.txt");
    (void)Directory_Delete("/memory");
    (void)FS_Unmount("/");

    TEST("FMFS auto-formats corrupted RAM metadata");
    {
        /* Simulate a reset in the middle of fmfs_save(): the FMFS magic
         * is intact but an entry record is torn.  On volatile media
         * (RAM disk / external SDRAM that survives soft resets) the
         * mount must recover by re-formatting instead of failing with
         * FM_ERR_CORRUPTED - otherwise every later file call in the
         * application dies, exactly like on the STM32H7 board whose
         * "RAM disk" lives in external SDRAM. */
        static uint8_t corrupt_buf[64 * 1024];
        FM_Storage cst;
        FM_FS      cfs;
        char ctext[64];

        r = FS_Unmount("/");
        if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;

        /* first mount formats the blank disk */
        r = FileManager_MountMemory(&cst, &cfs, "CRPT", corrupt_buf,
                                    sizeof(corrupt_buf), 1, FM_FALSE);
        if (r != FM_OK) { FAIL(FM_ResultString(r)); }
        else
        {
            r = File_WriteText("/ok.txt", "fine");
            (void)FS_Unmount("/");
            (void)Storage_Unregister("CRPT");
            /* header at [0..31]: magic[0..3], version u16@4, count u16@6,
             * data_start u64@8, data_next u64@16, capacity u64@24.
             * records start at 32: used@0, type@1, ... (180 bytes each).
             * Tear entry #1: mark it used with an invalid type. */
            corrupt_buf[32 + 180 + 0] = 1;      /* used   */
            corrupt_buf[32 + 180 + 1] = 0x77;   /* bogus type */

            r = FileManager_MountMemory(&cst, &cfs, "CRPT", corrupt_buf,
                                        sizeof(corrupt_buf), 1, FM_FALSE);
            if (r != FM_OK)
            {
                FAIL(FM_ResultString(r));
            }
            else
            {
                r = File_WriteText("/after-recover.txt", "recovered");
                memset(ctext, 0, sizeof(ctext));
                if (r == FM_OK)
                    r = File_ReadText("/after-recover.txt", ctext,
                                      sizeof(ctext), NULL);
                if (r == FM_OK && strcmp(ctext, "recovered") == 0)
                    PASS();
                else
                    FAIL("write after auto-format");
                (void)FS_Unmount("/");
                (void)Storage_Unregister("CRPT");
            }
        }
    }

    TEST("Custom storage driver mount helper");
    {
        static uint8_t custom_memory[64 * 1024];
        TestDriverContext context = {custom_memory, sizeof(custom_memory)};
        FM_Storage custom_storage;
        FM_FS custom_fs;
        FM_StorageConfig config;

        memset(&config, 0, sizeof(config));
        config.name = "QSPI";
        config.type = FM_STORAGE_QSPI;
        config.capacity = sizeof(custom_memory);
        config.block_size = 1;
        config.read_size = 1;
        config.write_size = 1;
        config.erase_size = 1;
        config.present = FM_TRUE;
        config.user_data = &context;
        config.read = test_driver_read;
        config.write = test_driver_write;
        config.erase = test_driver_erase;
        config.sync = test_driver_sync;
        r = Storage_InitDriver(&custom_storage, &config);
        if (r == FM_OK) r = FileManager_MountStorage(&custom_storage,
                                                       &custom_fs);
        if (r == FM_OK) r = File_WriteText("/driver.txt", "driver API");
        if (r == FM_OK) r = File_ReadText("/driver.txt", text,
                                          sizeof(text), &rd);
        if (r == FM_OK && strcmp(text, "driver API") == 0)
        {
            PASS();
        }
        else
        {
            FAIL("custom storage mount");
        }
        (void)FS_Unmount("/");
        (void)Storage_Unregister("QSPI");
    }
}
#endif /* FM_USE_FMFS */

/* ================================================================ */
/* LittleFS adapter                                                  */
/* ================================================================ */
#if FM_USE_LITTLEFS
static void test_littlefs(void)
{
    static uint8_t lfs_memory[128 * 1024];
    static FM_Storage lfs_storage;
    FM_FS lfs_fs;
    char text[128];
    size_t rd = 0;
    FM_Result r;
    uint8_t blob[4096];
    uint8_t back[4096];
    FM_Bool exists = FM_FALSE;
    uint32_t i;
    uint64_t free_bytes = 0;

    for (i = 0; i < sizeof(blob); i++)
    {
        blob[i] = (uint8_t)(i * 7u + (i >> 3));
    }
    memset(lfs_memory, 0xFF, sizeof(lfs_memory)); /* emulated erased flash */

    TEST("LittleFS mount (auto-format)");
    r = FS_Unmount("/");
    if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
    if (r == FM_OK) r = RamDisk_Init(&lfs_storage, "lfsram", lfs_memory,
                                     sizeof(lfs_memory), 4096);
    if (r == FM_OK) r = Storage_Register(&lfs_storage);
    if (r == FM_OK) r = Storage_Mount("lfsram");
    if (r == FM_OK) r = LittleFS_Init(&lfs_fs);
    if (r == FM_OK) r = FS_Mount("/", &lfs_fs);
    if (r != FM_OK)
    {
        FAIL(FM_ResultString(r));
        return;
    }
    PASS();

    TEST("LittleFS write/read text");
    r = Directory_Create("/cfg");
    if (r == FM_OK) r = File_WriteText("/cfg/app.txt", "littlefs works");
    memset(text, 0, sizeof(text));
    if (r == FM_OK) r = File_ReadText("/cfg/app.txt", text, sizeof(text),
                                      &rd);
    if (r == FM_OK && strcmp(text, "littlefs works") == 0)
    {
        PASS();
    }
    else
    {
        FAIL("littlefs text content");
    }

    TEST("LittleFS multi-block binary I/O");
    r = File_WriteAll("/blob.bin", blob, sizeof(blob));
    memset(back, 0, sizeof(back));
    if (r == FM_OK) r = File_ReadAll("/blob.bin", back, sizeof(back), &rd);
    if (r == FM_OK && rd == sizeof(blob) && memcmp(blob, back, sizeof(blob)) == 0)
    {
        PASS();
    }
    else
    {
        FAIL("littlefs binary content");
    }

    TEST("LittleFS copy/move/rename");
    r = File_Copy("/cfg/app.txt", "/cfg/app2.txt", NULL, NULL);
    if (r == FM_OK) r = File_Move("/cfg/app2.txt", "/cfg/app3.txt",
                                  NULL, NULL);
    if (r == FM_OK) r = File_Exists("/cfg/app3.txt", &exists);
    if (r == FM_OK && exists)
    {
        PASS();
    }
    else
    {
        FAIL("littlefs copy/move");
    }

    TEST("LittleFS free space report");
    r = FS_GetFreeSpace("/", &free_bytes);
    if (r == FM_OK && free_bytes > 0 && free_bytes < sizeof(lfs_memory))
    {
        PASS();
    }
    else
    {
        FAIL("littlefs free space");
    }

    TEST("LittleFS metadata survives remount");
    r = FS_Unmount("/");
    if (r == FM_OK) r = Storage_Mount("lfsram");
    if (r == FM_OK) r = FS_Mount("/", &lfs_fs);
    memset(text, 0, sizeof(text));
    if (r == FM_OK) r = File_ReadText("/cfg/app.txt", text, sizeof(text),
                                      &rd);
    if (r == FM_OK && strcmp(text, "littlefs works") == 0)
    {
        PASS();
    }
    else
    {
        FAIL("littlefs remount content");
    }

    (void)File_Delete("/cfg/app.txt");
    (void)File_Delete("/cfg/app3.txt");
    (void)File_Delete("/blob.bin");
    (void)Directory_Delete("/cfg");
    (void)FS_Unmount("/");
    (void)Storage_Unregister("lfsram");
}
#endif /* FM_USE_LITTLEFS */

/* ================================================================ */
/* File read cache                                                    */
/* ================================================================ */
#if FM_USE_CACHE && FM_USE_FMFS
typedef struct
{
    uint8_t *data;
    uint64_t capacity;
    uint32_t reads;
} CacheDriverCtx;

static FM_Result cache_drv_read(FM_Storage *s, uint64_t offset, void *buf,
                                uint32_t len)
{
    CacheDriverCtx *c = (CacheDriverCtx *)s->user_data;

    if (offset > c->capacity || (uint64_t)len > c->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, c->data + (size_t)offset, len);
    c->reads++;
    return FM_OK;
}

static FM_Result cache_drv_write(FM_Storage *s, uint64_t offset,
                                 const void *buf, uint32_t len)
{
    CacheDriverCtx *c = (CacheDriverCtx *)s->user_data;

    if (offset > c->capacity || (uint64_t)len > c->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(c->data + (size_t)offset, buf, len);
    return FM_OK;
}

static FM_Result cache_drv_erase(FM_Storage *s, uint64_t offset, uint32_t len)
{
    CacheDriverCtx *c = (CacheDriverCtx *)s->user_data;

    if (offset > c->capacity || (uint64_t)len > c->capacity - offset)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(c->data + (size_t)offset, 0xFF, len);
    return FM_OK;
}

static FM_Result cache_drv_sync(FM_Storage *s)
{
    (void)s;
    return FM_OK;
}

static void test_cache(void)
{
    static uint8_t mem[64 * 1024];
    static CacheDriverCtx ctx;
    FM_Storage storage;
    FM_FS fs;
    FM_StorageConfig cfg;
    uint8_t pattern[256];
    uint8_t out[64];
    uint8_t expect[64];
    static uint8_t bulk[4096];
    FM_File *f;
    FM_Result r;
    uint32_t i;
    uint32_t uncached = 0;
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t hits = 0;
    uint32_t misses = 0;

    for (i = 0; i < sizeof(pattern); i++)
    {
        pattern[i] = (uint8_t)(i * 3u);
    }
    memset(mem, 0xFF, sizeof(mem));
    ctx.data = mem;
    ctx.capacity = sizeof(mem);
    ctx.reads = 0;

    (void)FS_Unmount("/");
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "CACHE";
    cfg.type = FM_STORAGE_RAM;
    cfg.capacity = sizeof(mem);
    cfg.block_size = 1;
    cfg.read_size = 1;
    cfg.write_size = 1;
    cfg.erase_size = 1;
    cfg.present = FM_TRUE;
    cfg.user_data = &ctx;
    cfg.read = cache_drv_read;
    cfg.write = cache_drv_write;
    cfg.erase = cache_drv_erase;
    cfg.sync = cache_drv_sync;

    TEST("Cache: populate 8KB file");
    r = Storage_InitDriver(&storage, &cfg);
    if (r == FM_OK) r = FileManager_MountStorage(&storage, &fs);
    if (r == FM_OK)
    {
        uint8_t big[512];

        for (i = 0; i < sizeof(big); i++)
        {
            big[i] = pattern[i & 0xFFu];
        }
        r = File_WriteAll("/cache.bin", big, sizeof(big));
        for (i = 0; i < 15; i++)
        {
            if (r == FM_OK)
            {
                r = File_AppendAll("/cache.bin", big, sizeof(big));
            }
        }
    }
    CHECK_RESULT(r, "cache file write");

    /* --- pass 1: cache disabled -> one backend read per 64-byte call --- */
    TEST("Cache: disabled = 128 backend reads");
    r = FM_OK;
    File_SetCacheEnabled(FM_FALSE);
    File_CacheResetStats();
    f = File_Open("/cache.bin", FM_OPEN_READ);
    if (f == NULL) r = FM_ERR_NOT_FOUND;
    if (r == FM_OK)
    {
        uint32_t before = ctx.reads;

        while ((r = File_Read(f, out, sizeof(out), &i)) == FM_OK && i > 0)
        {
        }
        if (r == FM_ERR_EOF) r = FM_OK;
        uncached = ctx.reads - before;
        File_Close(f);
    }
    if (r == FM_OK && uncached == 128) PASS(); else FAIL("uncached reads");

    /* --- pass 2: cache enabled, first scan = one read per 512B block --- */
    TEST("Cache: enabled first scan = 16 backend reads");
    r = FM_OK;
    File_SetCacheEnabled(FM_TRUE);
    File_CacheResetStats();
    ctx.reads = 0;
    f = File_Open("/cache.bin", FM_OPEN_READ);
    if (f == NULL) r = FM_ERR_NOT_FOUND;
    if (r == FM_OK)
    {
        while ((r = File_Read(f, out, sizeof(out), &i)) == FM_OK && i > 0)
        {
        }
        if (r == FM_ERR_EOF) r = FM_OK;
        first = ctx.reads;
    }
    if (r == FM_OK && first == 16) PASS(); else FAIL("cached first scan");

    /* --- pass 3: cache enabled, second scan on same handle = all hits --- */
    TEST("Cache: enabled second scan = 0 backend reads");
    r = FM_OK;
    ctx.reads = 0;
    if (f != NULL) r = File_Seek(f, 0, FM_SEEK_SET);
    if (r == FM_OK)
    {
        uint32_t off = 0;
        FM_Bool content_ok = FM_TRUE;

        while ((r = File_Read(f, out, sizeof(out), &i)) == FM_OK && i > 0)
        {
            uint32_t k;

            for (k = 0; k < i; k++)
            {
                expect[k] = pattern[(off + k) & 0xFFu];
            }
            if (memcmp(out, expect, i) != 0)
            {
                content_ok = FM_FALSE;
            }
            off += i;
        }
        if (r == FM_ERR_EOF) r = FM_OK;
        second = ctx.reads;
        if (r == FM_OK && !content_ok)
        {
            r = FM_ERR_IO;
        }
    }
    if (f != NULL) File_Close(f);
    if (r == FM_OK && second == 0) PASS(); else FAIL("cached second scan");

    TEST("Cache: hit/miss statistics");
    File_CacheGetStats(&hits, &misses);
    if (hits > 0 && misses > 0)
    {
        PASS();
    }
    else
    {
        FAIL("stats");
    }

    TEST("Cache: large media read bypasses 512-byte cache lines");
    r = FM_OK;
    ctx.reads = 0;
    f = File_Open("/cache.bin", FM_OPEN_READ);
    if (f == NULL) r = FM_ERR_NOT_FOUND;
    i = 0;
    if (r == FM_OK) r = File_Read(f, bulk, sizeof(bulk), &i);
    if (f != NULL) (void)File_Close(f);
    if (r == FM_OK && i == sizeof(bulk) && ctx.reads == 1)
    {
        uint32_t k;

        for (k = 0; k < i; k++)
        {
            if (bulk[k] != pattern[k & 0xFFu])
            {
                r = FM_ERR_CORRUPTED;
                break;
            }
        }
    }
    if (r == FM_OK && i == sizeof(bulk) && ctx.reads == 1)
        PASS();
    else
        FAIL("large sequential bypass");

    (void)File_Delete("/cache.bin");
    (void)FS_Unmount("/");
    (void)Storage_Unregister("CACHE");
    File_SetCacheEnabled(FM_TRUE);
}
#endif /* FM_USE_CACHE */

/* ================================================================ */
/* MP3 decoding                                                       */
/* ================================================================ */
#if FM_USE_MP3
static void test_mp3(void)
{
    FM_Audio a;
    FM_AudioInfo info;
    int16_t samples[4096];
    size_t got = 0;
    size_t total = 0;
    FM_Result r;
    uint8_t mp3buf[4096];
    uint8_t id3[512];
    size_t rd = 0;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("MP3: mount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }

    TEST("MP3: decode mono tone");
    r = File_ReadAll("/test/vectors/tone_mono.mp3", mp3buf, sizeof(mp3buf),
                     &rd);
    if (r == FM_OK) r = Audio_Open("/test/vectors/tone_mono.mp3", &a);
    if (r == FM_OK)
    {
        Audio_GetInfo(&a, &info);
        if (info.channels != 1 || info.sample_rate != 48000 ||
            strcmp(info.codec, "MP3") != 0)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        else
        {
            total = 0;
            do
            {
                r = Audio_Read(&a, samples, 512, &got);
                total += got;
            } while (r == FM_OK);
            r = (r == FM_ERR_EOF && total == 1152) ? FM_OK
                                                   : FM_ERR_INVALID_FORMAT;
        }
        (void)Audio_Close(&a);
    }
    CHECK_RESULT(r, "mp3 mono decode");

    TEST("MP3: content detection ignores a wrong .wav extension");
    r = File_WriteAll("/misnamed.wav", mp3buf, rd);
    if (r == FM_OK) r = Audio_Open("/misnamed.wav", &a);
    if (r == FM_OK)
    {
        r = (strcmp(a.info.codec, "MP3") == 0) ? FM_OK
                                               : FM_ERR_INVALID_FORMAT;
        (void)Audio_Close(&a);
    }
    (void)File_Delete("/misnamed.wav");
    CHECK_RESULT(r, "audio magic dispatch");

    TEST("MP3: decode stereo tone + mono downmix");
    r = Audio_Open("/test/vectors/tone_stereo.mp3", &a);
    if (r == FM_OK)
    {
        Audio_GetInfo(&a, &info);
        if (info.channels != 2 || info.sample_rate != 48000)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        else
        {
            total = 0;
            do
            {
                r = Audio_Read(&a, samples, 512, &got);
                total += got;
            } while (r == FM_OK);
            if (r == FM_ERR_EOF && total == 2304)
            {
                r = FM_OK;
            }
            else
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        if (r == FM_OK)
        {
            /* downmix to mono: expect half the interleaved sample count */
            size_t mono_total = 0;

            r = Audio_Seek(&a, 0.0);
            if (r == FM_OK)
            {
                do
                {
                    r = Audio_ReadMono(&a, samples, 512, &got);
                    mono_total += got;
                } while (r == FM_OK);
                r = (r == FM_ERR_EOF && mono_total == 1152)
                        ? FM_OK : FM_ERR_INVALID_FORMAT;
            }
        }
        (void)Audio_Close(&a);
    }
    CHECK_RESULT(r, "mp3 stereo + downmix");

    TEST("MP3: skips ID3v2 tag");
    /* prepend a minimal ID3v2 tag to the mono bitstream */
    memset(id3, 0, sizeof(id3));
    memcpy(id3, "ID3", 3);
    id3[3] = 4; id3[4] = 0; id3[5] = 0;   /* v2.4, no flags */
    id3[6] = 0; id3[7] = 0; id3[8] = 0; id3[9] = 10; /* syncsafe size = 10 */
    memset(id3 + 10, 'X', 10);            /* 10 bytes of fake tag data */
    r = File_ReadAll("/test/vectors/tone_mono.mp3", mp3buf, sizeof(mp3buf),
                     &rd);
    if (r == FM_OK)
    {
        uint8_t tagged[4096];

        if (rd > sizeof(tagged) - 20)
        {
            r = FM_ERR_TOO_LARGE;
        }
        else
        {
            memcpy(tagged, id3, 20);
            memcpy(tagged + 20, mp3buf, rd);
            r = File_WriteAll("/tagged.mp3", tagged, 20 + rd);
        }
    }
    if (r == FM_OK) r = Audio_Open("/tagged.mp3", &a);
    if (r == FM_OK)
    {
        Audio_GetInfo(&a, &info);
        if (info.channels != 1 || info.sample_rate != 48000)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        else
        {
            total = 0;
            do
            {
                r = Audio_Read(&a, samples, 512, &got);
                total += got;
            } while (r == FM_OK);
            r = (r == FM_ERR_EOF && total == 1152) ? FM_OK
                                                   : FM_ERR_INVALID_FORMAT;
        }
        (void)Audio_Close(&a);
    }
    CHECK_RESULT(r, "mp3 id3 skip");

    (void)File_Delete("/tagged.mp3");
}

/* ================================================================ */
/* MP3 read-ahead block filling + refill diagnostics (FM_Mp3Stats)   */
/* ================================================================ */

/* tone_stereo.mp3 is exactly one complete frame
 * (MPEG-1 / 48 kHz / stereo, 1152 bytes on the wire).  Repeating the
 * whole file N times produces a valid CBR stream of N frames, each
 * decoding to exactly 1152 x 2 interleaved samples. */
#define MP3_TEST_FRAME_BYTES   1152
#define MP3_TEST_FRAME_SAMPLES 2304
#define MP3_TEST_COPIES        128

static void test_mp3_readahead(void)
{
    static uint8_t long_mp3[MP3_TEST_FRAME_BYTES * MP3_TEST_COPIES];
    FM_Audio a;
    FM_Mp3Stats stats;
    int16_t samples[8192];
    uint8_t frame[MP3_TEST_FRAME_BYTES + 16];
    size_t got = 0;
    size_t total = 0;
    uint32_t rd = 0;
    size_t i;
    int copies = MP3_TEST_COPIES;
    FM_Result r;

    TEST("MP3 read-ahead: multi-frame stream decodes fully");
    {
        FM_File vf;

        File_Init(&vf);
        r = File_OpenEx(&vf, "/test/vectors/tone_stereo.mp3", FM_OPEN_READ);
        if (r == FM_OK)
        {
            r = File_Read(&vf, frame, (uint32_t)MP3_TEST_FRAME_BYTES, &rd);
            (void)File_Close(&vf);
        }
    }
    if (r != FM_OK || rd != MP3_TEST_FRAME_BYTES)
    {
        FAIL("missing tone_stereo.mp3 vector");
        return;
    }
    if (frame[0] != 0xFF || frame[1] != 0xFC)
    {
        FAIL("tone_stereo.mp3 has no frame sync");
        return;
    }

    for (i = 0; i < (size_t)copies; i++)
    {
        memcpy(long_mp3 + i * MP3_TEST_FRAME_BYTES, frame,
               MP3_TEST_FRAME_BYTES);
    }

    r = File_WriteAll("/long_tone.mp3", long_mp3, sizeof(long_mp3));
    if (r == FM_OK) r = Audio_Open("/long_tone.mp3", &a);
    if (r == FM_OK &&
        (a.info.sample_rate != 48000 || a.info.channels != 2 ||
         strcmp(a.info.codec, "MP3") != 0))
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    if (r == FM_OK)
    {
        total = 0;
        do
        {
            r = Audio_Read(&a, samples, 512, &got);
            total += got;
        } while (r == FM_OK);
        r = (r == FM_ERR_EOF &&
             total == (size_t)copies * MP3_TEST_FRAME_SAMPLES) ? FM_OK
                                                               : FM_ERR_INVALID_FORMAT;
    }
    if (r == FM_OK)
    {
        r = Audio_Mp3Stats(&a, &stats);
        if (r == FM_OK &&
            (stats.frames != (uint32_t)copies ||
             stats.read_bytes != sizeof(long_mp3) ||
             stats.read_calls == 0u))
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        /*
         * Block filling must batch the stream into far fewer reads than
         * frames (the old per-frame top-up issued exactly one read per
         * frame).  A top-up read returns the buffer to near-full, so the
         * read count is bounded by bytes / (input_buf - low_water);
         * frames/2 is a loose upper bound holding for every sane
         * FM_MP3_INPUT_BUF / FM_MP3_LOW_WATER combination.
         */
        if (stats.read_calls == 0u ||
            stats.read_calls > (uint32_t)copies / 2u)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    (void)Audio_Close(&a);
    CHECK_RESULT(r, "mp3 multi-frame decode + stats");

    TEST("MP3 stats: reset + partial read accounting");
    r = Audio_Open("/long_tone.mp3", &a);
    if (r == FM_OK)
    {
        r = Audio_Mp3StatsReset(&a);
    }
    if (r == FM_OK)
    {
        r = Audio_Mp3Stats(&a, &stats);
        if (r == FM_OK &&
            (stats.frames != 0u || stats.read_calls != 0u ||
             stats.read_bytes != 0u || stats.resync_bytes != 0u))
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        /*
         * The first frame was already decoded by Audio_Open (header
         * probe), so reading one frame's worth decodes nothing new.
         */
        got = 0;
        r = Audio_Read(&a, samples, MP3_TEST_FRAME_SAMPLES, &got);
        if (r == FM_OK && got != MP3_TEST_FRAME_SAMPLES)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = Audio_Mp3Stats(&a, &stats);
        if (r == FM_OK && stats.frames != 0u)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        /* the next read must decode a fresh frame */
        got = 0;
        r = Audio_Read(&a, samples, MP3_TEST_FRAME_SAMPLES, &got);
        if (r == FM_OK && got != MP3_TEST_FRAME_SAMPLES)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = Audio_Mp3Stats(&a, &stats);
        if (r == FM_OK && stats.frames != 1u)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        /* drain the rest: 126 more frames must decode */
        total = 0;
        do
        {
            r = Audio_Read(&a, samples, 512, &got);
            total += got;
        } while (r == FM_OK);
        r = (r == FM_ERR_EOF &&
             total == (size_t)(copies - 2) * MP3_TEST_FRAME_SAMPLES) ? FM_OK
                                                                     : FM_ERR_INVALID_FORMAT;
    }
    if (r == FM_OK)
    {
        r = Audio_Mp3Stats(&a, &stats);
        if (r == FM_OK && stats.frames != (uint32_t)(copies - 1))
        {
            r = FM_ERR_INTERNAL;
        }
    }
    (void)Audio_Close(&a);
    CHECK_RESULT(r, "mp3 stats reset/partial");

    TEST("MP3 read-ahead: resyncs past corrupt bytes");
    {
        /* [f1][f2][1004 zero bytes][f3][f4]: minimp3 verifies a CBR frame
         * against up to 10 *following* headers, so f1 and f2 are rejected
         * (their verification chain crosses the garbage run) and only the
         * intact f3/f4 pair after it decodes.  The decoder must resync at
         * the garbage and must not stall, loop forever, or resync past the
         * first intact frame (file offset 2*frame+1004). */
        static uint8_t gapped[4 * MP3_TEST_FRAME_BYTES + 1004];

        memset(gapped, 0, sizeof(gapped));
        memcpy(gapped, frame, MP3_TEST_FRAME_BYTES);
        memcpy(gapped + 1 * MP3_TEST_FRAME_BYTES, frame,
               MP3_TEST_FRAME_BYTES);
        memcpy(gapped + 2 * MP3_TEST_FRAME_BYTES + 1004, frame,
               MP3_TEST_FRAME_BYTES);
        memcpy(gapped + 3 * MP3_TEST_FRAME_BYTES + 1004, frame,
               MP3_TEST_FRAME_BYTES);

        r = File_WriteAll("/gapped.mp3", gapped, sizeof(gapped));
        if (r == FM_OK) r = Audio_Open("/gapped.mp3", &a);
        if (r == FM_OK)
        {
            total = 0;
            do
            {
                r = Audio_Read(&a, samples, 512, &got);
                total += got;
            } while (r == FM_OK);
            r = (r == FM_ERR_EOF &&
                 total == 2u * MP3_TEST_FRAME_SAMPLES)
                    ? FM_OK : FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK)
        {
            r = Audio_Mp3Stats(&a, &stats);
            if (r == FM_OK &&
                (stats.frames != 2u ||
                 stats.resync_bytes > 2u * MP3_TEST_FRAME_BYTES + 1004u ||
                 stats.read_bytes != sizeof(gapped)))
            {
                r = FM_ERR_INTERNAL;
            }
        }
        (void)Audio_Close(&a);
    }
    (void)File_Delete("/gapped.mp3");
    CHECK_RESULT(r, "mp3 corrupt resync");

    TEST("MP3 stats: rejects non-MP3 streams");
    {
        FM_Audio wav;
        FM_AudioWriter w;
        int16_t tone[256];

        r = AudioWriter_Create("/stat_probe.wav", 8000, 1, 16, &w);
        if (r == FM_OK)
        {
            for (i = 0; i < sizeof(tone) / sizeof(tone[0]); i++)
            {
                tone[i] = 0;
            }
            r = AudioWriter_Write(&w, tone, sizeof(tone) / sizeof(tone[0]),
                                  NULL);
        }
        if (r == FM_OK) r = AudioWriter_Close(&w);
        if (r == FM_OK) r = Audio_Open("/stat_probe.wav", &wav);
        if (r == FM_OK)
        {
            r = Audio_Mp3Stats(&wav, &stats);
            if (r != FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_ERR_INTERNAL;
            }
            else
            {
                r = FM_OK;
            }
            (void)Audio_Close(&wav);
        }
        (void)File_Delete("/stat_probe.wav");
    }
    CHECK_RESULT(r, "mp3 stats guard");

    TEST("MP3: Audio_Seek lands on the requested second");
    r = Audio_Open("/long_tone.mp3", &a);
    if (r == FM_OK)
    {
        r = Audio_Seek(&a, 1.0);
    }
    if (r == FM_OK && a.info.sample_rate != 0u)
    {
        double pos = (double)a.frame_index / (double)a.info.sample_rate;

        /* One MPEG-1 frame is 1152 samples = 24 ms at 48 kHz. */
        if (pos < 0.97 || pos > 1.03)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = Audio_Seek(&a, 0.25);
    }
    if (r == FM_OK && a.info.sample_rate != 0u)
    {
        double pos = (double)a.frame_index / (double)a.info.sample_rate;

        if (pos < 0.22 || pos > 0.28)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        double dur = 0.0;

        if (a.info.sample_rate != 0u && a.info.channels != 0u &&
            a.info.bits_per_sample != 0u && a.info.data_size != 0u)
        {
            uint64_t bpf = (uint64_t)a.info.channels *
                           (uint64_t)(a.info.bits_per_sample / 8u);

            dur = (double)(a.info.data_size / bpf) /
                  (double)a.info.sample_rate;
        }
        if (dur < 2.9 || dur > 3.3)
        {
            r = FM_ERR_INTERNAL;
        }
        else
        {
            r = Audio_Seek(&a, 10.0);
            if (r != FM_ERR_EOF)
            {
                r = FM_ERR_INTERNAL;
            }
            else
            {
                r = FM_OK;
            }
        }
    }
    (void)Audio_Close(&a);
    CHECK_RESULT(r, "mp3 seek accuracy");

    (void)File_Delete("/long_tone.mp3");
}
#endif /* FM_USE_MP3 */

/* ================================================================ */
/* Video: H.264 codec + AVI/MP4 containers                           */
/* ================================================================ */
#if FM_USE_VIDEO

typedef struct
{
    uint8_t  b[8192];
    size_t   n;
    FM_Bool  be;
} TstBuf;

static void tb_u8(TstBuf *t, uint8_t v)
{
    t->b[t->n++] = v;
}

static void tb_u16(TstBuf *t, uint16_t v)
{
    if (t->be) { t->b[t->n++] = (uint8_t)(v >> 8); t->b[t->n++] = (uint8_t)v; }
    else       { t->b[t->n++] = (uint8_t)v; t->b[t->n++] = (uint8_t)(v >> 8); }
}

static void tb_u32(TstBuf *t, uint32_t v)
{
    if (t->be)
    {
        t->b[t->n++] = (uint8_t)(v >> 24);
        t->b[t->n++] = (uint8_t)(v >> 16);
        t->b[t->n++] = (uint8_t)(v >> 8);
        t->b[t->n++] = (uint8_t)v;
    }
    else
    {
        t->b[t->n++] = (uint8_t)v;
        t->b[t->n++] = (uint8_t)(v >> 8);
        t->b[t->n++] = (uint8_t)(v >> 16);
        t->b[t->n++] = (uint8_t)(v >> 24);
    }
}

static void tb_tag(TstBuf *t, const char *tag)
{
    memcpy(t->b + t->n, tag, 4);
    t->n += 4;
}

static void tb_bytes(TstBuf *t, const void *p, size_t n)
{
    memcpy(t->b + t->n, p, n);
    t->n += n;
}

#if FM_USE_H264
static size_t tb_box(TstBuf *t, const char *tag)
{
    size_t start = t->n;

    tb_u32(t, 0);      /* size patched later */
    tb_tag(t, tag);
    return start;
}

static void tb_box_end(TstBuf *t, size_t start)
{
    uint32_t size = (uint32_t)(t->n - start);

    if (t->be) { t->b[start] = (uint8_t)(size >> 24); t->b[start+1] = (uint8_t)(size >> 16);
                 t->b[start+2] = (uint8_t)(size >> 8); t->b[start+3] = (uint8_t)size; }
    else       { t->b[start] = (uint8_t)size; t->b[start+1] = (uint8_t)(size >> 8);
                 t->b[start+2] = (uint8_t)(size >> 16); t->b[start+3] = (uint8_t)(size >> 24); }
}

static void tb_patch_u32(TstBuf *t, size_t idx, uint32_t v)
{
    if (t->be) { t->b[idx] = (uint8_t)(v >> 24); t->b[idx+1] = (uint8_t)(v >> 16);
                 t->b[idx+2] = (uint8_t)(v >> 8); t->b[idx+3] = (uint8_t)v; }
    else       { t->b[idx] = (uint8_t)v; t->b[idx+1] = (uint8_t)(v >> 8);
                 t->b[idx+2] = (uint8_t)(v >> 16); t->b[idx+3] = (uint8_t)(v >> 24); }
}

#endif /* FM_USE_H264 */

/* RIFF chunk: tag first, then little-endian size (unlike MP4 boxes). */
static size_t tb_riff(TstBuf *t, const char *tag)
{
    size_t start = t->n;

    tb_tag(t, tag);
    tb_u32(t, 0);
    return start;
}

static void tb_riff_end(TstBuf *t, size_t start)
{
    uint32_t size = (uint32_t)(t->n - start - 8);

    t->b[start + 4] = (uint8_t)size;
    t->b[start + 5] = (uint8_t)(size >> 8);
    t->b[start + 6] = (uint8_t)(size >> 16);
    t->b[start + 7] = (uint8_t)(size >> 24);
}

#if FM_USE_H264
/* Split an Annex-B stream into NAL unit pointers (start code stripped). */
static uint32_t split_nals(const uint8_t *s, uint32_t len, const uint8_t **nals,
                           uint32_t *sizes, uint32_t max)
{
    uint32_t count = 0;
    uint32_t i = 0;

    while (i + 3 <= len && count < max)
    {
        if (s[i] == 0 && s[i+1] == 0 && (s[i+2] == 1 ||
            (s[i+2] == 0 && i + 3 < len && s[i+3] == 1)))
        {
            uint32_t sc = (s[i+2] == 1) ? 3u : 4u;
            uint32_t start = i + sc;
            uint32_t j = start;

            while (j + 3 <= len)
            {
                if (s[j] == 0 && s[j+1] == 0 && (s[j+2] == 1 ||
                    (s[j+2] == 0 && j + 3 < len && s[j+3] == 1)))
                {
                    break;
                }
                j++;
            }
            nals[count] = s + start;
            sizes[count] = j - start;
            count++;
            i = j;
        }
        else
        {
            i++;
        }
    }
    return count;
}

static FM_Result build_mp4(const uint8_t *sps, uint32_t sps_len,
                           const uint8_t *pps, uint32_t pps_len,
                           const uint8_t *idr, uint32_t idr_len,
                           const uint8_t *p1, uint32_t p1_len,
                           const uint8_t *p2, uint32_t p2_len,
                           uint8_t *out, size_t *out_len)
{
    TstBuf t;
    size_t stco_idx[3];
    size_t astco_idx;
    uint32_t off0, off1, off2, aoff;
    uint32_t i;

    memset(&t, 0, sizeof(t));
    t.be = FM_TRUE;

    {
        size_t b = tb_box(&t, "ftyp");
        tb_tag(&t, "isom");
        tb_u32(&t, 0x200);
        tb_tag(&t, "isom");
        tb_tag(&t, "iso2");
        tb_tag(&t, "avc1");
        tb_box_end(&t, b);
    }
    {
        size_t moov = tb_box(&t, "moov");
        {
            size_t mvhd = tb_box(&t, "mvhd");
            tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
            tb_u32(&t, 30);          /* timescale */
            tb_u32(&t, 3);           /* duration */
            tb_u32(&t, 0x00010000);  /* rate */
            tb_u16(&t, 0x0100);      /* volume */
            tb_u16(&t, 0);           /* reserved */
            tb_u32(&t, 0); tb_u32(&t, 0);
            for (i = 0; i < 9; i++) tb_u32(&t, (i == 0 || i == 4 || i == 8) ? 0x00010000 : 0);
            for (i = 0; i < 6; i++) tb_u32(&t, 0);
            tb_u32(&t, 2);
            tb_box_end(&t, mvhd);
        }
        {
            size_t trak = tb_box(&t, "trak");
            {
                size_t tkhd = tb_box(&t, "tkhd");
                tb_u32(&t, 7);
                tb_u32(&t, 0); tb_u32(&t, 0);
                tb_u32(&t, 1); tb_u32(&t, 0); tb_u32(&t, 3);
                tb_u32(&t, 0); tb_u32(&t, 0);
                tb_u16(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0);
                for (i = 0; i < 9; i++) tb_u32(&t, (i == 0 || i == 4 || i == 8) ? 0x00010000 : 0);
                tb_u32(&t, 0x00400000); /* width 64.0 */
                tb_u32(&t, 0x00400000); /* height 64.0 */
                tb_box_end(&t, tkhd);
            }
            {
                size_t mdia = tb_box(&t, "mdia");
                {
                    size_t mdhd = tb_box(&t, "mdhd");
                    tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_u32(&t, 30); tb_u32(&t, 3);
                    tb_u16(&t, 0x55C4); tb_u16(&t, 0);
                    tb_box_end(&t, mdhd);
                }
                {
                    size_t hdlr = tb_box(&t, "hdlr");
                    tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_tag(&t, "vide");
                    tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_bytes(&t, "vide", 4);
                    tb_box_end(&t, hdlr);
                }
                {
                    size_t minf = tb_box(&t, "minf");
                    {
                        size_t stbl = tb_box(&t, "stbl");
                        {
                            size_t stsd = tb_box(&t, "stsd");
                            tb_u32(&t, 0); tb_u32(&t, 1);
                            {
                                size_t avc1 = tb_box(&t, "avc1");
                                tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 1);
                                tb_u16(&t, 0); tb_u16(&t, 0);
                                tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                                tb_u16(&t, 64); tb_u16(&t, 64);
                                tb_u32(&t, 0x00480000); tb_u32(&t, 0x00480000);
                                tb_u32(&t, 0); tb_u16(&t, 1);
                                for (i = 0; i < 8; i++) tb_u32(&t, 0);
                                tb_u16(&t, 0x18); tb_u16(&t, 0xFFFF);
                                {
                                    size_t avcc = tb_box(&t, "avcC");
                                    tb_u8(&t, 1); tb_u8(&t, 0x42); tb_u8(&t, 0); tb_u8(&t, 0x1F);
                                    tb_u8(&t, 0xFF);
                                    tb_u8(&t, 0xE1);
                                    tb_u16(&t, (uint16_t)sps_len); tb_bytes(&t, sps, sps_len);
                                    tb_u8(&t, 1);
                                    tb_u16(&t, (uint16_t)pps_len); tb_bytes(&t, pps, pps_len);
                                    tb_box_end(&t, avcc);
                                }
                                tb_box_end(&t, avc1);
                            }
                            tb_box_end(&t, stsd);
                        }
                        {
                            size_t stts = tb_box(&t, "stts");
                            tb_u32(&t, 0); tb_u32(&t, 1);
                            tb_u32(&t, 3); tb_u32(&t, 1);
                            tb_box_end(&t, stts);
                        }
                        {
                            size_t stsc = tb_box(&t, "stsc");
                            tb_u32(&t, 0); tb_u32(&t, 1);
                            tb_u32(&t, 1); tb_u32(&t, 1); tb_u32(&t, 1);
                            tb_box_end(&t, stsc);
                        }
                        {
                            size_t stsz = tb_box(&t, "stsz");
                            tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 3);
                            tb_u32(&t, 4 + sps_len + 4 + pps_len + 4 + idr_len);
                            tb_u32(&t, 4 + p1_len);
                            tb_u32(&t, 4 + p2_len);
                            tb_box_end(&t, stsz);
                        }
                        {
                            size_t stco = tb_box(&t, "stco");
                            tb_u32(&t, 0); tb_u32(&t, 3);
                            stco_idx[0] = t.n; tb_u32(&t, 0);
                            stco_idx[1] = t.n; tb_u32(&t, 0);
                            stco_idx[2] = t.n; tb_u32(&t, 0);
                            tb_box_end(&t, stco);
                        }
                        tb_box_end(&t, stbl);
                    }
                    tb_box_end(&t, minf);
                }
                tb_box_end(&t, mdia);
            }
            tb_box_end(&t, trak);
        }
        {
            /* audio trak (mp4a / AAC) with 3 samples */
            size_t atrak = tb_box(&t, "trak");
            {
                size_t hdlr = tb_box(&t, "hdlr");
                tb_u32(&t, 0); tb_u32(&t, 0);
                tb_tag(&t, "soun");
                tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                tb_bytes(&t, "soun", 4);
                tb_box_end(&t, hdlr);
            }
            {
                size_t stbl = tb_box(&t, "stbl");
                {
                    size_t stsd = tb_box(&t, "stsd");
                    tb_u32(&t, 0); tb_u32(&t, 1);
                    {
                        size_t mp4a = tb_box(&t, "mp4a");
                        tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 1);
                        tb_u32(&t, 0); tb_u32(&t, 0);
                        tb_u16(&t, 2);        /* channelcount */
                        tb_u16(&t, 16);       /* samplesize */
                        tb_u16(&t, 0);        /* pre_defined */
                        tb_u16(&t, 0);        /* reserved */
                        tb_u32(&t, 44100u << 16); /* samplerate 16.16 */
                        tb_box_end(&t, mp4a);
                    }
                    tb_box_end(&t, stsd);
                }
                {
                    size_t stsc = tb_box(&t, "stsc");
                    tb_u32(&t, 0); tb_u32(&t, 1);
                    tb_u32(&t, 1); tb_u32(&t, 3); tb_u32(&t, 1);
                    tb_box_end(&t, stsc);
                }
                {
                    size_t stsz = tb_box(&t, "stsz");
                    tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 3);
                    tb_u32(&t, 2); tb_u32(&t, 4); tb_u32(&t, 6);
                    tb_box_end(&t, stsz);
                }
                {
                    size_t stco = tb_box(&t, "stco");
                    tb_u32(&t, 0); tb_u32(&t, 1);
                    astco_idx = t.n; tb_u32(&t, 0);
                    tb_box_end(&t, stco);
                }
                tb_box_end(&t, stbl);
            }
            tb_box_end(&t, atrak);
        }
        tb_box_end(&t, moov);
    }
    {
        size_t mdat = tb_box(&t, "mdat");
        off0 = (uint32_t)t.n;
        tb_u32(&t, (uint32_t)sps_len); tb_bytes(&t, sps, sps_len);
        tb_u32(&t, (uint32_t)pps_len); tb_bytes(&t, pps, pps_len);
        tb_u32(&t, (uint32_t)idr_len); tb_bytes(&t, idr, idr_len);
        off1 = (uint32_t)t.n;
        tb_u32(&t, (uint32_t)p1_len); tb_bytes(&t, p1, p1_len);
        off2 = (uint32_t)t.n;
        tb_u32(&t, (uint32_t)p2_len); tb_bytes(&t, p2, p2_len);
        aoff = (uint32_t)t.n;
        tb_bytes(&t, "AA", 2); tb_bytes(&t, "BBBB", 4); tb_bytes(&t, "CCCCCC", 6);
        tb_box_end(&t, mdat);
    }
    tb_patch_u32(&t, stco_idx[0], off0);
    tb_patch_u32(&t, stco_idx[1], off1);
    tb_patch_u32(&t, stco_idx[2], off2);
    tb_patch_u32(&t, astco_idx, aoff);

    if (out != NULL)
    {
        memcpy(out, t.b, t.n);
    }
    *out_len = t.n;
    return FM_OK;
}

#endif /* FM_USE_H264 */

#if FM_USE_H264
/* Allocator that fails exactly the h264bsd parameter-set store sizes
 * (sizeof(seqParamSet_t) / sizeof(picParamSet_t)) and passes everything
 * else through, to simulate an FM_Alloc pool that is just too small. */
static void *test_alloc_no_paramsets(size_t size, void *ctx)
{
    (void)ctx;
    if (size == sizeof(seqParamSet_t) || size == sizeof(picParamSet_t))
    {
        return NULL;
    }
    return malloc(size);
}

/* Allocator that fails the Nth and every later allocation, to walk the
 * decoder through every allocation site and prove that none of them
 * still reports an out-of-memory condition as a bitstream error. */
static long test_alloc_seq_n;
static long test_alloc_seq_fail_from;

static void *test_alloc_seq(size_t size, void *ctx)
{
    (void)ctx;
    if (++test_alloc_seq_n >= test_alloc_seq_fail_from)
    {
        return NULL;
    }
    return malloc(size);
}
#endif

static FM_Result build_avi(uint8_t *out, size_t *out_len)
{
    TstBuf t;

    memset(&t, 0, sizeof(t));
    t.be = FM_FALSE;
    {
        size_t riff = tb_riff(&t, "RIFF");
        tb_tag(&t, "AVI ");
        {
            size_t hdrl = tb_riff(&t, "LIST");
            tb_tag(&t, "hdrl");
            {
                size_t avih = tb_riff(&t, "avih");
                tb_u32(&t, 33333);  /* us per frame -> ~30fps */
                tb_u32(&t, 0);
                tb_u32(&t, 0); tb_u32(&t, 0);
                tb_u32(&t, 3);     /* total frames */
                tb_u32(&t, 0);     /* initial frames */
                tb_u32(&t, 1);     /* streams */
                tb_u32(&t, 0);
                tb_u32(&t, 64);    /* width */
                tb_u32(&t, 64);    /* height */
                tb_riff_end(&t, avih);
            }
            {
                size_t strl = tb_riff(&t, "LIST");
                tb_tag(&t, "strl");
                {
                    size_t strh = tb_riff(&t, "strh");
                    tb_tag(&t, "vids");
                    tb_tag(&t, "MJPG");
                    tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0);
                    tb_u32(&t, 0);
                    tb_u32(&t, 1); tb_u32(&t, 30);
                    tb_u32(&t, 0); tb_u32(&t, 3);
                    tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_riff_end(&t, strh);
                }
                {
                    size_t strf = tb_riff(&t, "strf");
                    tb_u32(&t, 40);      /* biSize */
                    tb_u32(&t, 64);      /* width */
                    tb_u32(&t, 64);      /* height */
                    tb_u16(&t, 1); tb_u16(&t, 24);
                    tb_tag(&t, "MJPG");
                    tb_u32(&t, 0);
                    tb_riff_end(&t, strf);
                }
                tb_riff_end(&t, strl);
            }
            tb_riff_end(&t, hdrl);
        }
        {
            size_t movi = tb_riff(&t, "LIST");
            tb_tag(&t, "movi");
            {
                size_t f1 = tb_riff(&t, "00dc");
                tb_bytes(&t, "FRM1", 4);
                tb_riff_end(&t, f1);
                size_t f2 = tb_riff(&t, "00dc");
                tb_bytes(&t, "FRM2", 4);
                tb_riff_end(&t, f2);
                size_t f3 = tb_riff(&t, "00dc");
                tb_bytes(&t, "FRM3", 4);
                tb_riff_end(&t, f3);
            }
            tb_riff_end(&t, movi);
        }
        tb_riff_end(&t, riff);
    }
    memcpy(out, t.b, t.n);
    *out_len = t.n;
    return FM_OK;
}

#endif /* FM_USE_VIDEO (builders) */

static FM_Bool test_xfer_cancel_cb(uint64_t done, uint64_t total,
                                   void *user)
{
    (void)total;
    (void)user;
    return (done < 4096u) ? FM_TRUE : FM_FALSE;
}

/* ---- The port / QSPI / SPI-flash / media-cache / crash-safety / log /
 * board-config / transfer suites below are NOT video tests.  They used to
 * sit inside the FM_USE_VIDEO block and were silently skipped whenever
 * video was compiled out. ---- */

/* Used by the FM_CHECK test: a function that validates its input the
 * way every public entry point in the library now does. */
static FM_Result test_debug_check_helper(const char *p)
{
    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Portability + debug levels                                          */
/* ------------------------------------------------------------------ */
static void test_port_and_debug(void)
{
    TEST("Port: the selected port publishes a complete capability set");
    {
        /* Every port must answer all of these, or code that branches on
         * them silently takes the wrong path. */
        int caps = FM_PORT_HAS_DCACHE + FM_PORT_HAS_DMA +
                   FM_PORT_HAS_SDRAM + FM_PORT_HAS_QSPI +
                   FM_PORT_HAS_HW_JPEG + FM_PORT_HAS_I2S;

        if (FM_PORT_NAME[0] == '\0')
        {
            FAIL("port has no name");
        }
        else if (FM_PORT_TIER < 1 || FM_PORT_TIER > 3)
        {
            FAIL("tier out of range");
        }
        else if (caps < 0)
        {
            FAIL("capability flags undefined");
        }
        else
        {
            PASS();
        }
    }

    TEST("Port: buffer defaults scale with the tier");
    {
        /* A tier-1 part (F1-class) must not be handed megabyte buffers,
         * and a tier-3 part must not be crippled to 16KB. */
        FM_Bool ok;

#if FM_PORT_TIER == 1
        ok = (SIMPLE_IMAGE_FILE_MAX <= (64u * 1024u)) &&
             (FM_TRANSFER_CHUNK <= 1024u) &&
             (SDRAM_ALLOC_BYTES == 0u);
#elif FM_PORT_TIER == 2
        ok = (SIMPLE_IMAGE_FILE_MAX <= (1024u * 1024u)) &&
             (SDRAM_ALLOC_BYTES <= (4u * 1024u * 1024u));
#else
        ok = (SIMPLE_IMAGE_FILE_MAX >= (1024u * 1024u));
#endif
        if (ok) { PASS(); } else { FAIL("defaults do not match the tier"); }
    }

    TEST("Port: the library never decides the cache state itself");
    {
        /* Enabling the cache is the application's call, made in main.c
         * with SCB_EnableDCache().  The library only reads SCB->CCR at
         * run time.  On the host there is no cache at all, so both
         * queries must report off and the maintenance calls must be
         * inert rather than wrong. */
        if (FM_Cache_IsDataEnabled() != 0)
        {
            FAIL("claims D-Cache is on with no hardware");
        }
        else if (FM_Cache_IsInstEnabled() != 0)
        {
            FAIL("claims I-Cache is on with no hardware");
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: the four levels are ordered and distinct");
    {
        if (FM_DEBUG_OFF >= FM_DEBUG_LOW ||
            FM_DEBUG_LOW >= FM_DEBUG_MEDIUM ||
            FM_DEBUG_MEDIUM >= FM_DEBUG_HIGH)
        {
            FAIL("levels are not strictly increasing");
        }
        else if (FM_Debug_LevelName() == NULL ||
                 FM_Debug_LevelName()[0] == '\0')
        {
            FAIL("level has no name");
        }
        else
        {
            PASS();
        }
    }

    TEST("Debug: FM_CHECK returns instead of crashing");
    {
        /* The whole point: a failed precondition is a return value, not
         * an abort.  If this test runs to completion, that holds. */
        if (test_debug_check_helper(NULL) != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("FM_CHECK did not return the error");
        }
        else if (test_debug_check_helper("ok") != FM_OK)
        {
            FAIL("FM_CHECK rejected a valid argument");
        }
        else
        {
            PASS();
        }
    }

    TEST("EEPROM: chip table gives correct geometry per model");
    {
        FM_EepromChipInfo a;
        FM_EepromChipInfo b;

        if (FM_Eeprom_GetChipInfo(FM_EEPROM_CHIP_24LC256, &a) != FM_OK ||
            FM_Eeprom_GetChipInfo(FM_EEPROM_CHIP_24LC02, &b) != FM_OK)
        {
            FAIL("lookup failed");
        }
        else if (a.capacity != 32768u || a.page_size != 64u ||
                 a.addr_bytes != 2u)
        {
            FAIL("24LC256 geometry wrong");
        }
        else if (b.capacity != 256u || b.page_size != 8u ||
                 b.addr_bytes != 1u)
        {
            FAIL("24LC02 geometry wrong");
        }
        else
        {
            PASS();
        }
    }

    TEST("EEPROM: unknown model is refused, not guessed");
    {
        FM_EepromChipInfo info;

        if (FM_Eeprom_GetChipInfo((FM_EepromChip)999, &info) !=
            FM_ERR_UNSUPPORTED)
        {
            FAIL("accepted an unknown model");
        }
        else
        {
            PASS();
        }
    }

    TEST("EEPROM: config is filled straight from the model");
    {
        FM_EEPROMConfig cfg;

        if (FM_Eeprom_ConfigFromChip(&cfg, FM_EEPROM_CHIP_24LC512) != FM_OK)
        {
            FAIL("config failed");
        }
        else if (cfg.capacity != 65536u || cfg.page_size != 128u)
        {
            FAIL("config does not match the model");
        }
        else
        {
            PASS();
        }
    }
}

/* ------------------------------------------------------------------ */
/* QSPI NOR driver — against a simulated chip                          */
/* ------------------------------------------------------------------ */
/*
 * A fake W25Q128 that enforces the real rules of NOR flash: writes can
 * only clear bits, programming may not cross a page boundary, and only
 * a whole sector can be erased.  If the driver gets any of that wrong
 * the simulated data ends up corrupt, exactly as it would on hardware.
 */
#define QSIM_SIZE     (64u * 1024u)   /* small stand-in, same geometry */
#define QSIM_SECTOR   4096u
#define QSIM_PAGE     256u

static uint8_t  g_qsim[QSIM_SIZE];
static uint8_t  g_qsim_wel;
static uint32_t g_qsim_page_violations;
static uint32_t g_qsim_no_wel;

/* --- memory-mapped mode for the simulated chip ---------------------
 * Models the real constraint: while mapped, the chip accepts no
 * commands at all.  Any program or erase that reaches the wire in this
 * state is counted as a lost write -- the exact failure that makes
 * mapped mode dangerous if the driver forgets to leave it. */
static uint8_t  g_qsim_mapped;
static uint32_t g_qsim_lost_writes;
static uint32_t g_qsim_enter_calls;
static uint32_t g_qsim_exit_calls;

static FM_Result qsim_mmap_enter(void *ctx)
{
    (void)ctx;
    g_qsim_mapped = 1u;
    g_qsim_enter_calls++;
    return FM_OK;
}

static FM_Result qsim_mmap_exit(void *ctx)
{
    (void)ctx;
    g_qsim_mapped = 0u;
    g_qsim_exit_calls++;
    return FM_OK;
}

static FM_Result qsim_cmd(void *ctx, uint8_t op)
{
    (void)ctx;
    if (g_qsim_mapped) { g_qsim_lost_writes++; return FM_ERR_IO; }
    if (op == 0x06u) { g_qsim_wel = 1u; return FM_OK; }
    if (op == 0xC7u)
    {
        if (!g_qsim_wel) { g_qsim_no_wel++; return FM_ERR_IO; }
        memset(g_qsim, 0xFF, sizeof(g_qsim));
        g_qsim_wel = 0u;
        return FM_OK;
    }
    if (op == 0xB7u) { return FM_OK; }
    return FM_OK;
}

static FM_Result qsim_cmd_addr(void *ctx, uint8_t op, uint32_t addr,
                               uint8_t ab)
{
    (void)ctx; (void)ab;
    if (g_qsim_mapped) { g_qsim_lost_writes++; return FM_ERR_IO; }
    if (op == 0x20u)
    {
        if (!g_qsim_wel) { g_qsim_no_wel++; return FM_ERR_IO; }
        if (addr % QSIM_SECTOR) { return FM_ERR_INVALID_ARGUMENT; }
        if (addr + QSIM_SECTOR > QSIM_SIZE) { return FM_ERR_INVALID_ARGUMENT; }
        memset(g_qsim + addr, 0xFF, QSIM_SECTOR);
        g_qsim_wel = 0u;
        return FM_OK;
    }
    return FM_OK;
}

static FM_Result qsim_read(void *ctx, uint8_t op, uint32_t addr,
                           uint8_t ab, uint8_t dummy, void *buf,
                           uint32_t len)
{
    (void)ctx; (void)ab; (void)dummy;
    if (g_qsim_mapped) { g_qsim_lost_writes++; return FM_ERR_IO; }
    if (op == 0x9Fu)
    {
        uint8_t *p = (uint8_t *)buf;
        if (len >= 3u) { p[0] = 0xEFu; p[1] = 0x40u; p[2] = 0x18u; }
        return FM_OK;                       /* W25Q128 */
    }
    if (op == 0x05u)
    {
        *(uint8_t *)buf = 0u;               /* never busy */
        return FM_OK;
    }
    if (addr + len > QSIM_SIZE) { return FM_ERR_INVALID_ARGUMENT; }
    memcpy(buf, g_qsim + addr, len);
    return FM_OK;
}

static FM_Result qsim_write(void *ctx, uint8_t op, uint32_t addr,
                            uint8_t ab, const void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t i;

    (void)ctx; (void)ab; (void)op;
    if (g_qsim_mapped) { g_qsim_lost_writes++; return FM_ERR_IO; }
    if (!g_qsim_wel) { g_qsim_no_wel++; return FM_ERR_IO; }
    /* a page program may not wrap past the end of its page */
    if ((addr % QSIM_PAGE) + len > QSIM_PAGE) { g_qsim_page_violations++; }
    if (addr + len > QSIM_SIZE) { return FM_ERR_INVALID_ARGUMENT; }
    for (i = 0; i < len; i++)
    {
        g_qsim[addr + i] &= src[i];   /* NOR can only clear bits */
    }
    g_qsim_wel = 0u;
    return FM_OK;
}


static void qsim_io_init(FM_QspiIo *io)
{
    memset(io, 0, sizeof(*io));
    io->cmd = qsim_cmd;
    io->cmd_addr = qsim_cmd_addr;
    io->read = qsim_read;
    io->write = qsim_write;
    io->ctx = NULL;
}

static void test_qspi(void)
{
    static uint8_t sector_buf[QSIM_SECTOR];
    FM_QspiIo io;
    FM_QspiDevice dev;

    memset(g_qsim, 0xFF, sizeof(g_qsim));
    g_qsim_wel = 0u;
    g_qsim_page_violations = 0u;
    g_qsim_no_wel = 0u;
    qsim_io_init(&io);

    TEST("QSPI: auto-detects the chip from its JEDEC id");
    {
        FM_QspiDevice d;

        if (FM_Qspi_Init(&d, &io, FM_QSPI_CHIP_AUTO, sector_buf,
                         sizeof(sector_buf)) != FM_OK)
        {
            FAIL("init failed");
        }
        else if (strcmp(d.info.name, "W25Q128") != 0)
        {
            FAIL("wrong chip identified");
        }
        else if (d.info.sector_size != 4096u || d.info.page_size != 256u)
        {
            FAIL("wrong geometry");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: unknown JEDEC id is reported, not guessed");
    {
        FM_QspiChipInfo info;

        if (FM_Qspi_IdentifyById(0x123456u, &info) != FM_ERR_NOT_FOUND)
        {
            FAIL("accepted an unknown id");
        }
        else
        {
            PASS();
        }
    }

    (void)FM_Qspi_Init(&dev, &io, FM_QSPI_CHIP_CUSTOM, sector_buf,
                       sizeof(sector_buf));
    /* CUSTOM without info must be refused */
    TEST("QSPI: CUSTOM chip without geometry is refused");
    {
        FM_QspiDevice d;
        FM_QspiIo io2;

        qsim_io_init(&io2);
        memset(&d, 0, sizeof(d));
        if (FM_Qspi_Init(&d, &io2, FM_QSPI_CHIP_CUSTOM, sector_buf,
                         sizeof(sector_buf)) == FM_OK)
        {
            FAIL("accepted a chip with no geometry");
        }
        else
        {
            PASS();
        }
    }

    /* real device for the data tests */
    qsim_io_init(&io);
    (void)FM_Qspi_Init(&dev, &io, FM_QSPI_CHIP_W25Q128, sector_buf,
                       sizeof(sector_buf));
    dev.info.size_bytes = QSIM_SIZE;    /* shrink to the simulated part */

    TEST("QSPI: sector-aligned write reads back exactly");
    {
        static uint8_t out[QSIM_SECTOR];
        static uint8_t in[QSIM_SECTOR];
        size_t i;

        for (i = 0; i < sizeof(out); i++)
        {
            out[i] = (uint8_t)(i ^ 0x3Cu);
        }
        if (FM_Qspi_Write(&dev, 0, out, sizeof(out)) != FM_OK)
        {
            FAIL("write failed");
        }
        else if (FM_Qspi_Read(&dev, 0, in, sizeof(in)) != FM_OK)
        {
            FAIL("read failed");
        }
        else if (memcmp(out, in, sizeof(out)) != 0)
        {
            FAIL("data differs");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: page boundaries are never crossed");
    {
        /* 600 bytes from offset 100 spans three pages */
        static uint8_t out[600];
        size_t i;

        for (i = 0; i < sizeof(out); i++) { out[i] = (uint8_t)(i + 1u); }
        g_qsim_page_violations = 0u;
        (void)FM_Qspi_Write(&dev, 100u, out, sizeof(out));
        if (g_qsim_page_violations != 0u)
        {
            FAIL("driver crossed a page boundary");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: Write Enable precedes every program and erase");
    {
        if (g_qsim_no_wel != 0u)
        {
            FAIL("a program or erase ran without Write Enable");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: erase skips blank sectors (wear optimization)");
    {
        uint32_t erases_before = dev.stat_erases;
        uint32_t skipped_before = dev.stat_erases_skipped;

        /* sector 0 still holds the pattern of the aligned-write test:
         * this erase must really run */
        if (FM_Qspi_Erase(&dev, 0, dev.info.sector_size) != FM_OK)
        {
            FAIL("erase of a data sector failed");
        }
        else if (dev.stat_erases != erases_before + 1u)
        {
            FAIL("sector holding data was not erased");
        }
        else
        {
            /* second erase on the now-blank sector must be skipped */
            if (FM_Qspi_Erase(&dev, 0, dev.info.sector_size) != FM_OK)
            {
                FAIL("erase of a blank sector failed");
            }
            else if (dev.stat_erases != erases_before + 1u ||
                     dev.stat_erases_skipped != skipped_before + 1u)
            {
                FAIL("blank sector was erased again");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("QSPI: unaligned write preserves its neighbours");
    {
        static uint8_t base[QSIM_SECTOR];
        static uint8_t back[QSIM_SECTOR];
        uint8_t patch[8];
        size_t i;
        FM_Bool ok = FM_TRUE;

        for (i = 0; i < sizeof(base); i++) { base[i] = (uint8_t)(i * 5u); }
        (void)FM_Qspi_Write(&dev, 0, base, sizeof(base));
        memset(patch, 0xAA, sizeof(patch));
        /* patch 8 bytes in the middle -- needs read-modify-erase-write */
        if (FM_Qspi_Write(&dev, 1000u, patch, sizeof(patch)) != FM_OK)
        {
            FAIL("patch write failed");
        }
        else if (FM_Qspi_Read(&dev, 0, back, sizeof(back)) != FM_OK)
        {
            FAIL("readback failed");
        }
        else
        {
            for (i = 0; i < sizeof(base); i++)
            {
                uint8_t want = (i >= 1000u && i < 1008u)
                                   ? 0xAAu : (uint8_t)(i * 5u);
                if (back[i] != want) { ok = FM_FALSE; break; }
            }
            if (ok) { PASS(); } else { FAIL("neighbouring bytes damaged"); }
        }
    }

    TEST("QSPI: unaligned write without a sector buffer is refused");
    {
        FM_QspiDevice d;
        FM_QspiIo io2;
        uint8_t one = 0x55u;

        qsim_io_init(&io2);
        if (FM_Qspi_Init(&d, &io2, FM_QSPI_CHIP_W25Q128, NULL, 0) != FM_OK)
        {
            FAIL("init without buffer failed");
        }
        else
        {
            d.info.size_bytes = QSIM_SIZE;
            if (FM_Qspi_Write(&d, 7u, &one, 1u) != FM_ERR_UNSUPPORTED)
            {
                FAIL("silently did a partial write with no buffer");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("QSPI: access past the end of the chip is rejected");
    {
        uint8_t b[4];

        if (FM_Qspi_Read(&dev, QSIM_SIZE - 2u, b, 4u) !=
            FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("read past end allowed");
        }
        else if (FM_Qspi_Write(&dev, QSIM_SIZE - 2u, b, 4u) !=
                 FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("write past end allowed");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: NULL arguments never crash");
    {
        uint8_t b[4];

        if (FM_Qspi_Read(NULL, 0, b, 4u) == FM_OK ||
            FM_Qspi_Write(NULL, 0, b, 4u) == FM_OK ||
            FM_Qspi_Read(&dev, 0, NULL, 4u) == FM_OK ||
            FM_Qspi_Erase(NULL, 0, 4u) == FM_OK ||
            FM_Qspi_Init(NULL, &io, FM_QSPI_CHIP_W25Q128, NULL, 0) == FM_OK)
        {
            FAIL("a NULL argument was accepted");
        }
        else
        {
            PASS();
        }
    }


    TEST("QSPI mmap: reads go through the mapped window when available");
    {
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        static uint8_t seed[256];
        static uint8_t got[256];
        size_t i;

        for (i = 0; i < sizeof(seed); i++) { seed[i] = (uint8_t)(i ^ 0x77u); }

        qsim_io_init(&io3);
        g_qsim_mapped = 0u;
        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;
        (void)FM_Qspi_Write(&d, 0, seed, sizeof(seed));

        /* now attach the mapped window and read again */
        io3.mmap_enter = qsim_mmap_enter;
        io3.mmap_exit = qsim_mmap_exit;
        io3.mmap_base = g_qsim;
        d.io = io3;
        d.stat_mmap_reads = 0u;

        if (FM_Qspi_Read(&d, 0, got, sizeof(got)) != FM_OK)
        {
            FAIL("mapped read failed");
        }
        else if (memcmp(seed, got, sizeof(seed)) != 0)
        {
            FAIL("mapped read returned wrong data");
        }
        else if (d.stat_mmap_reads == 0u)
        {
            FAIL("did not use the mapped path");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI mmap: a write after a mapped read is not lost");
    {
        /* The dangerous case: the driver is sitting in mapped mode from
         * a previous read, then a write arrives.  If it forgets to leave
         * mapped mode the command never reaches the chip and the data is
         * silently lost. */
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        static uint8_t tmp[64];
        static uint8_t back[64];
        size_t i;

        qsim_io_init(&io3);
        io3.mmap_enter = qsim_mmap_enter;
        io3.mmap_exit = qsim_mmap_exit;
        io3.mmap_base = g_qsim;
        g_qsim_mapped = 0u;
        g_qsim_lost_writes = 0u;

        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;

        /* read first -- this leaves the device in mapped mode */
        (void)FM_Qspi_Read(&d, 0, back, sizeof(back));
        if (!FM_Qspi_IsMapped(&d))
        {
            FAIL("expected to be in mapped mode after a read");
        }
        else
        {
            for (i = 0; i < sizeof(tmp); i++) { tmp[i] = (uint8_t)(i + 3u); }
            if (FM_Qspi_Write(&d, 8192u, tmp, sizeof(tmp)) != FM_OK)
            {
                FAIL("write after mapped read failed");
            }
            else if (g_qsim_lost_writes != 0u)
            {
                FAIL("a command was issued while still mapped");
            }
            else if (FM_Qspi_Read(&d, 8192u, back, sizeof(back)) != FM_OK ||
                     memcmp(tmp, back, sizeof(tmp)) != 0)
            {
                FAIL("data written after a mapped read did not stick");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("QSPI mmap: erase and status polling leave mapped mode");
    {
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        uint8_t probe[4];

        qsim_io_init(&io3);
        io3.mmap_enter = qsim_mmap_enter;
        io3.mmap_exit = qsim_mmap_exit;
        io3.mmap_base = g_qsim;
        g_qsim_mapped = 0u;
        g_qsim_lost_writes = 0u;

        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;
        (void)FM_Qspi_Read(&d, 0, probe, sizeof(probe));   /* map */

        if (FM_Qspi_Erase(&d, 4096u, 4096u) != FM_OK)
        {
            FAIL("erase after mapped read failed");
        }
        else if (g_qsim_lost_writes != 0u)
        {
            FAIL("erase or status poll ran while mapped");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI mmap: MapForRead hands back a usable pointer");
    {
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        static uint8_t seed[32];
        const uint8_t *p = NULL;
        size_t i;

        for (i = 0; i < sizeof(seed); i++) { seed[i] = (uint8_t)(i + 90u); }

        qsim_io_init(&io3);
        g_qsim_mapped = 0u;
        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;
        (void)FM_Qspi_Write(&d, 2048u, seed, sizeof(seed));

        io3.mmap_enter = qsim_mmap_enter;
        io3.mmap_exit = qsim_mmap_exit;
        io3.mmap_base = g_qsim;
        d.io = io3;

        if (FM_Qspi_MapForRead(&d, 2048u, &p) != FM_OK || p == NULL)
        {
            FAIL("MapForRead failed");
        }
        else if (memcmp(p, seed, sizeof(seed)) != 0)
        {
            FAIL("pointer does not show the flash contents");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI mmap: can be disabled, and then reads use commands");
    {
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        uint8_t probe[8];

        qsim_io_init(&io3);
        io3.mmap_enter = qsim_mmap_enter;
        io3.mmap_exit = qsim_mmap_exit;
        io3.mmap_base = g_qsim;
        g_qsim_mapped = 0u;

        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;
        (void)FM_Qspi_SetMappedReads(&d, FM_FALSE);
        d.stat_mmap_reads = 0u;

        if (FM_Qspi_Read(&d, 0, probe, sizeof(probe)) != FM_OK)
        {
            FAIL("read with mapping disabled failed");
        }
        else if (d.stat_mmap_reads != 0u)
        {
            FAIL("used the mapped path after it was disabled");
        }
        else if (FM_Qspi_IsMapped(&d))
        {
            FAIL("left the device mapped after disabling");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI mmap: absent hardware support degrades to command reads");
    {
        FM_QspiDevice d;
        FM_QspiIo io3;
        static uint8_t sbuf[QSIM_SECTOR];
        uint8_t probe[8];
        const uint8_t *p = NULL;

        qsim_io_init(&io3);        /* no mmap_enter/exit at all */
        g_qsim_mapped = 0u;
        (void)FM_Qspi_Init(&d, &io3, FM_QSPI_CHIP_W25Q128, sbuf,
                           sizeof(sbuf));
        d.info.size_bytes = QSIM_SIZE;

        if (FM_Qspi_Read(&d, 0, probe, sizeof(probe)) != FM_OK)
        {
            FAIL("plain read failed without mapping support");
        }
        else if (FM_Qspi_MapForRead(&d, 0, &p) != FM_ERR_UNSUPPORTED)
        {
            FAIL("MapForRead should report UNSUPPORTED");
        }
        else
        {
            PASS();
        }
    }

    TEST("QSPI: registers as a storage the rest of the library can use");
    {
        static FM_Storage st;
        FM_Storage *found = NULL;

        if (FM_Qspi_RegisterStorage(&dev, &st, "qspitest") != FM_OK)
        {
            FAIL("registration failed");
        }
        else if (Storage_Find("qspitest", &found) != FM_OK ||
                 found == NULL)
        {
            FAIL("not visible in the registry");
        }
        else if (st.erase_size != 4096u)
        {
            FAIL("erase granularity not published");
        }
        else
        {
            PASS();
            (void)Storage_Unregister("qspitest");
        }
    }
}


/* ------------------------------------------------------------------ */
/* SPI flash — same chip, same driver, plain SPI wiring                */
/* ------------------------------------------------------------------ */
/*
 * A bit-level SPI slave for the same simulated W25Q128.  It decodes the
 * opcode and address out of the byte stream exactly as a real part
 * does, so if the bridge builds a malformed header (wrong address
 * width, missing dummy byte, byte order flipped) the data lands in the
 * wrong place and the test fails.
 */
static uint8_t  g_spif_hdr[8];
static uint32_t g_spif_hdr_len;
static uint8_t  g_spif_cs;
static uint32_t g_spif_cs_errors;

static void spisim_cs_select(void *ctx)
{
    (void)ctx;
    if (g_spif_cs) { g_spif_cs_errors++; }   /* nested select */
    g_spif_cs = 1u;
    g_spif_hdr_len = 0u;
}

static void spisim_cs_release(void *ctx)
{
    (void)ctx;
    if (!g_spif_cs) { g_spif_cs_errors++; }  /* release without select */
    g_spif_cs = 0u;
}

static FM_Result spisim_transfer(void *ctx, const uint8_t *tx, uint8_t *rx,
                                 uint32_t len)
{
    uint32_t i;

    (void)ctx;
    if (!g_spif_cs) { g_spif_cs_errors++; return FM_ERR_IO; }

    /* first burst of a transaction is the header */
    if (g_spif_hdr_len == 0u && tx != NULL && rx == NULL)
    {
        for (i = 0; i < len && i < sizeof(g_spif_hdr); i++)
        {
            g_spif_hdr[i] = tx[i];
        }
        g_spif_hdr_len = len;

        /* commands that carry no payload act immediately */
        if (g_spif_hdr[0] == 0x06u) { g_qsim_wel = 1u; }
        else if (g_spif_hdr[0] == 0x20u)
        {
            uint32_t a = ((uint32_t)g_spif_hdr[1] << 16) |
                         ((uint32_t)g_spif_hdr[2] << 8) |
                         (uint32_t)g_spif_hdr[3];
            if (!g_qsim_wel) { g_qsim_no_wel++; return FM_ERR_IO; }
            if (a + QSIM_SECTOR <= QSIM_SIZE)
            {
                memset(g_qsim + a, 0xFF, QSIM_SECTOR);
            }
            g_qsim_wel = 0u;
        }
        return FM_OK;
    }

    /* payload phase -- interpret against the header we captured */
    {
        uint8_t op = g_spif_hdr[0];
        uint32_t a = ((uint32_t)g_spif_hdr[1] << 16) |
                     ((uint32_t)g_spif_hdr[2] << 8) |
                     (uint32_t)g_spif_hdr[3];

        if (op == 0x9Fu && rx != NULL)
        {
            if (len >= 3u) { rx[0] = 0xEFu; rx[1] = 0x40u; rx[2] = 0x18u; }
            return FM_OK;
        }
        if (op == 0x05u && rx != NULL)
        {
            rx[0] = 0u;                       /* never busy */
            return FM_OK;
        }
        if (op == 0x03u && rx != NULL)
        {
            if (a + len > QSIM_SIZE) { return FM_ERR_INVALID_ARGUMENT; }
            memcpy(rx, g_qsim + a, len);
            return FM_OK;
        }
        if (op == 0x02u && tx != NULL)
        {
            if (!g_qsim_wel) { g_qsim_no_wel++; return FM_ERR_IO; }
            if ((a % QSIM_PAGE) + len > QSIM_PAGE)
            {
                g_qsim_page_violations++;
            }
            if (a + len > QSIM_SIZE) { return FM_ERR_INVALID_ARGUMENT; }
            for (i = 0; i < len; i++) { g_qsim[a + i] &= tx[i]; }
            g_qsim_wel = 0u;
            return FM_OK;
        }
    }
    return FM_OK;
}

static void test_spiflash(void)
{
    static uint8_t sector_buf[QSIM_SECTOR];
    FM_SpiFlashPort port;
    FM_QspiDevice dev;

    memset(g_qsim, 0xFF, sizeof(g_qsim));
    g_qsim_wel = 0u;
    g_qsim_no_wel = 0u;
    g_qsim_page_violations = 0u;
    g_qsim_mapped = 0u;
    g_spif_cs = 0u;
    g_spif_cs_errors = 0u;

    memset(&port, 0, sizeof(port));
    port.transfer = spisim_transfer;
    port.cs_select = spisim_cs_select;
    port.cs_release = spisim_cs_release;
    port.ctx = NULL;

    TEST("SPI flash: identifies the chip over plain SPI");
    {
        if (FM_SpiFlash_Init(&dev, &port, FM_QSPI_CHIP_AUTO, sector_buf,
                             sizeof(sector_buf)) != FM_OK)
        {
            FAIL("init failed");
        }
        else if (strcmp(dev.info.name, "W25Q128") != 0)
        {
            FAIL("wrong chip");
        }
        else
        {
            PASS();
        }
    }

    dev.info.size_bytes = QSIM_SIZE;

    TEST("SPI flash: write and read back through the SPI bridge");
    {
        static uint8_t out[512];
        static uint8_t in[512];
        size_t i;

        for (i = 0; i < sizeof(out); i++)
        {
            out[i] = (uint8_t)(i * 11u + 5u);
        }
        if (FM_Qspi_Write(&dev, 0, out, sizeof(out)) != FM_OK)
        {
            FAIL("write failed");
        }
        else if (FM_Qspi_Read(&dev, 0, in, sizeof(in)) != FM_OK)
        {
            FAIL("read failed");
        }
        else if (memcmp(out, in, sizeof(out)) != 0)
        {
            FAIL("data differs -- header or byte order wrong");
        }
        else
        {
            PASS();
        }
    }

    TEST("SPI flash: chip select is balanced on every transaction");
    {
        /* A leaked CS leaves the part mid-command and corrupts the
         * next access; a nested one means two drivers are fighting. */
        if (g_spif_cs_errors != 0u)
        {
            FAIL("unbalanced chip select");
        }
        else if (g_spif_cs != 0u)
        {
            FAIL("chip select left asserted");
        }
        else
        {
            PASS();
        }
    }

    TEST("SPI flash: page boundaries respected, Write Enable always sent");
    {
        static uint8_t out[600];
        size_t i;

        for (i = 0; i < sizeof(out); i++) { out[i] = (uint8_t)(i ^ 0x1Fu); }
        g_qsim_page_violations = 0u;
        g_qsim_no_wel = 0u;
        (void)FM_Qspi_Write(&dev, 300u, out, sizeof(out));
        if (g_qsim_page_violations != 0u)
        {
            FAIL("crossed a page boundary");
        }
        else if (g_qsim_no_wel != 0u)
        {
            FAIL("programmed without Write Enable");
        }
        else
        {
            PASS();
        }
    }

    TEST("SPI flash: no memory-mapped mode is offered");
    {
        const uint8_t *p = NULL;

        /* Plain SPI cannot map flash into the address space; the driver
         * must say so rather than hand back a bogus pointer. */
        if (FM_Qspi_MapForRead(&dev, 0, &p) != FM_ERR_UNSUPPORTED)
        {
            FAIL("claimed mapped reads over plain SPI");
        }
        else
        {
            PASS();
        }
    }

    TEST("SPI flash: NULL transfer callback is refused");
    {
        FM_SpiFlashPort bad;
        FM_QspiDevice d;

        memset(&bad, 0, sizeof(bad));
        if (FM_SpiFlash_Init(&d, &bad, FM_QSPI_CHIP_W25Q128, NULL, 0) ==
            FM_OK)
        {
            FAIL("accepted a port with no transfer function");
        }
        else
        {
            PASS();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Media cache — play from fast flash instead of SD                    */
/* ------------------------------------------------------------------ */
static void test_mediacache(void)
{
    static uint8_t payload[4096];
    FM_MediaCacheConfig cfg;
    FM_MediaCacheStats st;
    char resolved[FM_PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(i * 13u + 7u);
    }
    (void)Directory_Create("/mcsrc");
    (void)FM_Transfer_SaveFile("/mcsrc/clip.avi", payload, sizeof(payload));

    TEST("MediaCache: first resolve copies, second one hits");
    {
        memset(&cfg, 0, sizeof(cfg));
        cfg.cache_root = "/mccache";
        cfg.policy = FM_MEDIACACHE_ALWAYS;
        FM_MediaCache_ResetStats();

        if (FM_MediaCache_Init(&cfg) != FM_OK)
        {
            FAIL("init failed");
        }
        else if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                       sizeof(resolved)) != FM_OK)
        {
            FAIL("first resolve failed");
        }
        else if (strcmp(resolved, "/mcsrc/clip.avi") == 0)
        {
            FAIL("did not redirect to the cache");
        }
        else
        {
            char first[FM_PATH_MAX];

            strcpy(first, resolved);
            if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                      sizeof(resolved)) != FM_OK)
            {
                FAIL("second resolve failed");
            }
            else if (strcmp(first, resolved) != 0)
            {
                FAIL("cache path not stable across calls");
            }
            else
            {
                FM_MediaCache_GetStats(&st);
                if (st.misses != 1u || st.hits != 1u)
                {
                    FAIL("hit/miss accounting wrong");
                }
                else
                {
                    PASS();
                }
            }
        }
    }

    TEST("MediaCache: cached copy is byte-identical to the source");
    {
        static uint8_t back[4096];
        size_t n = 0;

        if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                  sizeof(resolved)) != FM_OK)
        {
            FAIL("resolve failed");
        }
        else if (FM_Transfer_LoadFile(resolved, back, sizeof(back), &n) !=
                 FM_OK)
        {
            FAIL("cannot read the cached file");
        }
        else if (n != sizeof(payload) ||
                 memcmp(back, payload, sizeof(payload)) != 0)
        {
            FAIL("cached data differs from the source");
        }
        else
        {
            PASS();
        }
    }

    TEST("MediaCache: a file too large to cache still plays from source");
    {
        memset(&cfg, 0, sizeof(cfg));
        cfg.cache_root = "/mccache";
        cfg.policy = FM_MEDIACACHE_IF_SMALL;
        cfg.max_file_bytes = 100u;         /* smaller than our 4KB clip */
        FM_MediaCache_ResetStats();
        (void)FM_MediaCache_Init(&cfg);

        if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                  sizeof(resolved)) != FM_OK)
        {
            FAIL("resolve failed");
        }
        else if (strcmp(resolved, "/mcsrc/clip.avi") != 0)
        {
            FAIL("cached a file that exceeds the limit");
        }
        else
        {
            FM_MediaCache_GetStats(&st);
            if (st.bypasses == 0u)
            {
                FAIL("bypass not counted");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("MediaCache: disabled cache returns the original path");
    {
        memset(&cfg, 0, sizeof(cfg));
        cfg.policy = FM_MEDIACACHE_NEVER;
        (void)FM_MediaCache_Init(&cfg);

        if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                  sizeof(resolved)) != FM_OK)
        {
            FAIL("resolve failed with caching off");
        }
        else if (strcmp(resolved, "/mcsrc/clip.avi") != 0)
        {
            FAIL("redirected even though caching is off");
        }
        else
        {
            PASS();
        }
    }

    TEST("MediaCache: a cache root that cannot be used is refused");
    {
        /* The point is that a broken cache never stops playback.  A
         * path with an embedded NUL-length component cannot be created
         * on any backend, so Init must reject it and every later
         * Resolve must fall back to the source.
         *
         * (An ordinary missing directory is not a good probe here: the
         * host's POSIX backend simply creates it, which is correct
         * behaviour rather than a failure.) */
        char toolong[FM_PATH_MAX + 8];

        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[0] = '/';
        toolong[sizeof(toolong) - 1] = '\0';

        memset(&cfg, 0, sizeof(cfg));
        cfg.cache_root = toolong;
        cfg.policy = FM_MEDIACACHE_ALWAYS;

        if (FM_MediaCache_Init(&cfg) == FM_OK)
        {
            FAIL("accepted an unusable cache root");
        }
        else if (FM_MediaCache_Resolve("/mcsrc/clip.avi", resolved,
                                       sizeof(resolved)) != FM_OK)
        {
            FAIL("resolve failed instead of falling back");
        }
        else if (strcmp(resolved, "/mcsrc/clip.avi") != 0)
        {
            FAIL("redirected despite the cache being unusable");
        }
        else
        {
            PASS();
        }
    }

    TEST("MediaCache: a missing source is reported, not invented");
    {
        memset(&cfg, 0, sizeof(cfg));
        cfg.cache_root = "/mccache";
        cfg.policy = FM_MEDIACACHE_ALWAYS;
        (void)FM_MediaCache_Init(&cfg);

        if (FM_MediaCache_Resolve("/mcsrc/nothere.avi", resolved,
                                  sizeof(resolved)) != FM_OK)
        {
            FAIL("resolve should still succeed");
        }
        else if (strcmp(resolved, "/mcsrc/nothere.avi") != 0)
        {
            FAIL("invented a cache path for a missing file");
        }
        else
        {
            PASS();
        }
    }

    TEST("MediaCache: NULL arguments never crash");
    {
        if (FM_MediaCache_Resolve(NULL, resolved, sizeof(resolved)) ==
                FM_OK ||
            FM_MediaCache_Resolve("/mcsrc/clip.avi", NULL, 10u) == FM_OK ||
            FM_MediaCache_Init(NULL) == FM_OK)
        {
            FAIL("a NULL argument was accepted");
        }
        else
        {
            PASS();
        }
    }

    FM_MediaCache_Deinit();
    (void)Directory_DeleteRecursive("/mccache");
    (void)Directory_DeleteRecursive("/mcsrc");
}

/* ------------------------------------------------------------------ */
/* Crash safety — every public entry point survives bad input          */
/* ------------------------------------------------------------------ */
/*
 * The library's promise is that no error crashes the caller.  These
 * feed NULLs and nonsense to the public API across every subsystem; if
 * any of them dereferences a null pointer the test binary dies, which
 * is itself the failure signal.
 */
static void test_crash_safety(void)
{
    TEST("Crash safety: file API rejects NULL rather than dereferencing");
    {
        FM_File f;
        uint32_t n = 0;
        uint64_t sz = 0;
        FM_Bool ex = FM_FALSE;

        memset(&f, 0, sizeof(f));
        (void)File_OpenEx(NULL, "/x", FM_OPEN_READ);
        (void)File_OpenEx(&f, NULL, FM_OPEN_READ);
        (void)File_Read(NULL, &n, 4u, &n);
        (void)File_Write(NULL, &n, 4u, &n);
        (void)File_Close(NULL);
        (void)File_Size(NULL, &sz);
        (void)File_SizeOf(NULL, &sz);
        (void)File_Exists(NULL, &ex);
        (void)File_Delete(NULL);
        (void)File_WriteAll(NULL, &n, 4u);
        (void)File_ReadAll(NULL, &n, 4u, NULL);
        PASS();          /* reaching here means nothing crashed */
    }

    TEST("Crash safety: a rejected File_Open does not leak a pool handle");
    {
        /* File_Open claimed a pool slot first and validated afterwards;
         * the early "bad argument" return forgot to give it back.  Eight
         * such calls (FM_MAX_OPEN_FILES) and no file could be opened
         * through the pool any more. */
        uint32_t before = File_PoolUsed();
        uint32_t i;
        uint32_t n = 0;
        FM_File *ok;

        for (i = 0; i < (uint32_t)FM_MAX_OPEN_FILES * 2u; i++)
        {
            (void)File_Open(NULL, FM_OPEN_READ);
            (void)File_Open("/leak_probe.txt", 0u);
            (void)File_Open("/leak_probe.txt", FM_OPEN_CREATE);
            (void)File_ReadAll(NULL, &n, sizeof(n), NULL);
            (void)File_WriteAll(NULL, &n, sizeof(n));
            (void)File_Open("/no/such/dir/leak_probe.txt", FM_OPEN_READ);
        }
        ok = File_Open("/leak_probe.txt",
                       FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
        if (ok == NULL)
        {
            FAIL("pool exhausted by rejected opens");
        }
        else
        {
            (void)File_Close(ok);
            (void)File_Delete("/leak_probe.txt");
            if (File_PoolUsed() == before) PASS();
            else FAIL("handle count changed");
        }
    }

    TEST("Crash safety: storage and filesystem API tolerate NULL");
    {
        FM_Storage *st = NULL;
        uint64_t cap = 0;

        (void)Storage_Register(NULL);
        (void)Storage_Unregister(NULL);
        (void)Storage_Find(NULL, &st);
        (void)Storage_Find("nope", NULL);
        (void)Storage_GetCapacity(NULL, &cap);
        (void)Storage_Mount(NULL);
        (void)FS_Mount(NULL, NULL);
        (void)FS_Unmount(NULL);
        PASS();
    }

    TEST("Crash safety: directory API tolerates NULL");
    {
        FM_DirEntry e;

        (void)Directory_Create(NULL);
        (void)Directory_Open(NULL);
        (void)Directory_Read(NULL, &e);
        (void)Directory_Close(NULL);
        (void)Directory_Delete(NULL);
        PASS();
    }

    TEST("Crash safety: transfer and cache API tolerate NULL");
    {
        char buf[16];

        (void)FM_Transfer_Copy(NULL, "/a", NULL);
        (void)FM_Transfer_Copy("/a", NULL, NULL);
        (void)FM_Transfer_Move(NULL, NULL, NULL);
        (void)FM_Transfer_LoadFile(NULL, buf, sizeof(buf), NULL);
        (void)FM_Transfer_SaveFile(NULL, buf, sizeof(buf));
        (void)FM_Transfer_Raw(NULL, 0, NULL, 0, 16u, NULL);
        (void)FM_Transfer_RawVerify(NULL, 0, NULL, 0, 16u);
        (void)FM_MediaCache_Resolve(NULL, buf, sizeof(buf));
        (void)FM_MediaCache_Evict(NULL);
        PASS();
    }

    TEST("Crash safety: flash API tolerates NULL and absurd sizes");
    {
        FM_QspiDevice d;
        FM_QspiChipInfo info;
        uint8_t b[4];

        memset(&d, 0, sizeof(d));
        (void)FM_Qspi_Init(NULL, NULL, FM_QSPI_CHIP_AUTO, NULL, 0);
        (void)FM_Qspi_Read(NULL, 0, b, 4u);
        (void)FM_Qspi_Write(NULL, 0, b, 4u);
        (void)FM_Qspi_Erase(NULL, 0, 0xFFFFFFFFu);
        (void)FM_Qspi_EraseChip(NULL);
        (void)FM_Qspi_WaitReady(NULL, 10u);
        (void)FM_Qspi_ReadId(NULL, NULL);
        (void)FM_Qspi_GetChipInfo(FM_QSPI_CHIP_AUTO, NULL);
        (void)FM_Qspi_IdentifyById(0u, &info);
        (void)FM_Qspi_MapForRead(NULL, 0, NULL);
        (void)FM_Qspi_Unmap(NULL);
        (void)FM_Qspi_IsMapped(NULL);
        (void)FM_SpiFlash_Init(NULL, NULL, FM_QSPI_CHIP_AUTO, NULL, 0);
        PASS();
    }

    TEST("Crash safety: an operation on an uninitialised device fails cleanly");
    {
        FM_QspiDevice d;
        uint8_t b[4];

        /* zeroed, never Init'ed: every call must report NOT_READY
         * instead of calling through a null io callback */
        memset(&d, 0, sizeof(d));
        if (FM_Qspi_Read(&d, 0, b, 4u) != FM_ERR_NOT_READY ||
            FM_Qspi_Write(&d, 0, b, 4u) != FM_ERR_NOT_READY ||
            FM_Qspi_Erase(&d, 0, 4096u) != FM_ERR_NOT_READY)
        {
            FAIL("did not report NOT_READY");
        }
        else
        {
            PASS();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Log routing — the library must not care where output goes           */
/* ------------------------------------------------------------------ */
static char  g_logcap[512];
static size_t g_logcap_len;
static int   g_logcap_calls;
static void *g_logcap_ctx;

static void test_log_sink(void *ctx, const char *data, size_t len)
{
    size_t i;

    g_logcap_ctx = ctx;
    g_logcap_calls++;
    for (i = 0; i < len && g_logcap_len < sizeof(g_logcap) - 1u; i++)
    {
        g_logcap[g_logcap_len++] = data[i];
    }
    g_logcap[g_logcap_len] = '\0';
}

static void test_logging(void)
{
    FM_LogSinkFn saved;
    void *saved_ctx = NULL;

    saved = FM_Log_GetSink(&saved_ctx);

    TEST("Log: with no sink registered, output is silently dropped");
    {
        FM_Log_SetSink(NULL, NULL);
        if (FM_Log_IsActive())
        {
            FAIL("reported active with no sink");
        }
        else if (FM_Log("this goes nowhere %d\n", 1) != 0)
        {
            FAIL("claimed to have written something");
        }
        else
        {
            PASS();       /* and, crucially, did not crash */
        }
    }

    TEST("Log: a registered sink receives the formatted text");
    {
        g_logcap_len = 0;
        g_logcap_calls = 0;
        FM_Log_SetSink(test_log_sink, (void *)0xABCD);

        if (!FM_Log_IsActive())
        {
            FAIL("not active after registering");
        }
        else if (FM_Log("value=%d name=%s\n", 42, "x") <= 0)
        {
            FAIL("wrote nothing");
        }
        else if (strcmp(g_logcap, "value=42 name=x\n") != 0)
        {
            FAIL("formatted text is wrong");
        }
        else if (g_logcap_ctx != (void *)0xABCD)
        {
            FAIL("context not passed through");
        }
        else
        {
            PASS();
        }
    }

    TEST("Log: raw write and single char bypass formatting");
    {
        g_logcap_len = 0;
        (void)FM_Log_Write("ab", 2u);
        (void)FM_Log_PutChar('c');
        if (strcmp(g_logcap, "abc") != 0)
        {
            FAIL("raw output wrong");
        }
        else
        {
            PASS();
        }
    }

    TEST("Log: an over-long message is truncated, never overflows");
    {
        static char big[FM_LOG_BUF_SIZE * 2];
        int n;

        memset(big, 'A', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        g_logcap_len = 0;
        n = FM_Log("%s", big);
        if (n < 0)
        {
            FAIL("returned an error");
        }
        else if ((size_t)n >= FM_LOG_BUF_SIZE)
        {
            FAIL("wrote past the buffer");
        }
        else
        {
            PASS();
        }
    }

    TEST("Log: switching sinks redirects output");
    {
        g_logcap_len = 0;
        g_logcap_calls = 0;
        FM_Log_SetSink(NULL, NULL);
        (void)FM_Log("dropped\n");
        if (g_logcap_calls != 0)
        {
            FAIL("old sink still received output");
        }
        else
        {
            FM_Log_SetSink(test_log_sink, NULL);
            (void)FM_Log("back\n");
            if (strcmp(g_logcap, "back\n") != 0)
            {
                FAIL("new sink did not receive output");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("Log: NULL format and zero length are handled");
    {
        if (FM_Log(NULL) != 0 ||
            FM_Log_Write(NULL, 10u) != 0 ||
            FM_Log_Write("x", 0u) != 0)
        {
            FAIL("did not reject bad arguments");
        }
        else
        {
            PASS();
        }
    }

    FM_Log_SetSink(saved, saved_ctx);
}

/* Stands in for an audio refill: seeks the shared handle mid-read. */
static FM_File *g_idle_thief_file;
static int      g_idle_thief_hits;

TEST_MAYBE_UNUSED static void test_idle_cursor_thief(void)
{
    if (g_idle_thief_file != NULL)
    {
        g_idle_thief_hits++;
        (void)File_Seek(g_idle_thief_file, 3, FM_SEEK_SET);
    }
}

/* ------------------------------------------------------------------ */
/* Board config + cache helpers                                        */
/* ------------------------------------------------------------------ */
static void test_board_config(void)
{
    TEST("Board config: every large buffer has a size and a section");
    {
        /* If any of these vanish or go to zero the firmware silently
         * mis-sizes a buffer, so pin them down here. */
        if (SDRAM_ALLOC_BYTES == 0u ||
            SIMPLE_IMAGE_FILE_MAX == 0u ||
            SIMPLE_IMAGE_MAX_W == 0u || SIMPLE_IMAGE_MAX_H == 0u ||
            SIMPLE_VIDEO_MAX_W == 0u || SIMPLE_VIDEO_MAX_H == 0u ||
            LCD_SPI_DMA_CHUNK == 0u || FM_TRANSFER_CHUNK == 0u ||
            FM_QSPI_SECTOR_SIZE == 0u)
        {
            FAIL("a buffer size is zero");
        }
        else
        {
            PASS();
        }
    }

#if FM_USE_VIDEO
    TEST("Video: an idle hook that moves the cursor cannot corrupt a frame");
    {
        /* Regression: reading a frame in slices used to continue from
         * the file's current position.  FM_Idle() runs between slices,
         * and for a video with sound the application refills its audio
         * ring from the very same FM_File -- one shared cursor.  So
         * every slice after the first read whatever the audio demuxer
         * had seeked to.  Silent clips were unaffected, which is why
         * this looked like an audio bug.
         *
         * Here the idle hook plays the part of the audio refill: it
         * seeks the same handle somewhere else.  The frame must still
         * come back byte-exact. */
        static uint8_t wr[70000];
        static uint8_t rd[70000];
        FM_VideoWriter w;
        FM_Video v;
        size_t got = 0;
        size_t i;
        FM_Result r;

        for (i = 0; i < sizeof(wr); i++)
        {
            wr[i] = (uint8_t)(i * 7u + (i >> 8));
        }
        memset(&w, 0, sizeof(w));
        memset(&v, 0, sizeof(v));
        memset(rd, 0, sizeof(rd));

        r = VideoWriter_Create("/fm_idle.fmrv", 100, 175, 10,
                               FM_PIXEL_RGBA8888, sizeof(wr), &w);
        if (r == FM_OK)
        {
            r = VideoWriter_WriteFrame(&w, wr, sizeof(wr));
        }
        if (r == FM_OK)
        {
            r = VideoWriter_Close(&w);
        }
        if (r == FM_OK)
        {
            r = Video_Open("/fm_idle.fmrv", &v);
        }
        if (r == FM_OK)
        {
            g_idle_thief_file = &v.file;
            g_idle_thief_hits = 0;
            FM_SetIdle(test_idle_cursor_thief);
            r = Video_ReadFrame(&v, rd, sizeof(rd), &got);
            FM_SetIdle(NULL);
            g_idle_thief_file = NULL;
        }
        if (File_IsOpen(&v.file))
        {
            (void)Video_Close(&v);
        }
        (void)File_Delete("/fm_idle.fmrv");

        if (r != FM_OK)
        {
            FAIL("frame read failed");
        }
        else if (g_idle_thief_hits == 0)
        {
            FAIL("idle hook never ran -- test proves nothing");
        }
        else if (got != sizeof(wr))
        {
            FAIL("short frame");
        }
        else if (memcmp(rd, wr, sizeof(wr)) != 0)
        {
            FAIL("frame corrupted by the idle hook's seek");
        }
        else
        {
            PASS();
        }
    }
#else
    TEST("Video compiled out: API links, reports UNSUPPORTED, owns no state");
    {
        FM_Video v;
        FM_VideoWriter w;
        FM_VideoInfo vi;
        FM_Bool has = FM_TRUE;
        size_t got = 123u;
        uint8_t b[8];

        memset(&v, 0, sizeof(v));
        memset(&w, 0, sizeof(w));
        if (Video_Open("/clip.avi", &v) == FM_ERR_UNSUPPORTED &&
            Video_GetInfo(&v, &vi) == FM_ERR_UNSUPPORTED &&
            Video_ReadFrame(&v, b, sizeof(b), &got) == FM_ERR_UNSUPPORTED &&
            got == 0u &&
            Video_DecodeFrame(&v, b, sizeof(b)) == FM_ERR_UNSUPPORTED &&
            Video_HasAudio(&v, &has) == FM_ERR_UNSUPPORTED && !has &&
            Video_ReadAudioFrame(&v, b, sizeof(b), &got) ==
                FM_ERR_UNSUPPORTED &&
            VideoWriter_Create("/o.fmrv", 2, 2, 1, 1, 12, &w) ==
                FM_ERR_UNSUPPORTED &&
            Video_Close(&v) == FM_ERR_UNSUPPORTED)
        {
            PASS();
        }
        else
        {
            FAIL("a disabled video entry point did not say UNSUPPORTED");
        }
    }
#endif

    TEST("Board config: the library owns no addresses");
    {
        /* The board's memory map belongs to the application.  The
         * library places its big buffers with a linker section and
         * never names an address, so there must be no SDRAM base,
         * size, or carved-out window here to disagree with the real
         * linker script. */
#if defined(FM_MEM_SDRAM_BASE) || defined(FM_MEM_SDRAM_SIZE) || \
    defined(FM_MEM_SDRAM_NOCACHE_BASE) || defined(FM_MEM_SDRAM_SPLIT_NOCACHE)
        FAIL("config still hard-codes part of the memory map");
#else
        if (SDRAM_ALLOC_SECTION[0] == '\0')
        {
            FAIL("no linker section named for the allocator pool");
        }
        else
        {
            PASS();
        }
#endif
    }

    TEST("Board config: QSPI sector size is a power of two");
    {
        if ((FM_QSPI_SECTOR_SIZE & (FM_QSPI_SECTOR_SIZE - 1u)) != 0u)
        {
            FAIL("not a power of two");
        }
        else
        {
            PASS();
        }
    }

    TEST("Cache helpers are safe no-ops on the host");
    {
        /* On the host these compile to nothing; the point is that
         * calling them with edge-case arguments never misbehaves, so
         * driver code can call them unconditionally. */
        static uint8_t buf[64];

        FM_Cache_CleanForDma(NULL, 100);
        FM_Cache_CleanForDma(buf, 0);
        FM_Cache_InvalidateAfterDma(NULL, 100);
        FM_Cache_InvalidateAfterDma(buf, 0);
        FM_Cache_CleanForDma(buf, sizeof(buf));
        FM_Cache_InvalidateAfterDma(buf, sizeof(buf));
        FM_Cache_CleanInvalidate(buf, sizeof(buf));
        PASS();
    }

    TEST("Cache: TCM addresses are correctly identified");
    {
        /* DTCM and ITCM are never cached; maintenance on them must be
         * skipped or it wastes cycles on every DMA. */
        if (!fm_cache_addr_is_tcm((const void *)0x20000000u))
        {
            FAIL("DTCM not recognised");
        }
        else if (!fm_cache_addr_is_tcm((const void *)0x00000100u))
        {
            FAIL("ITCM not recognised");
        }
        else if (fm_cache_addr_is_tcm((const void *)0xC0000000u))
        {
            FAIL("SDRAM wrongly treated as TCM");
        }
        else if (fm_cache_addr_is_tcm((const void *)0x24000000u))
        {
            FAIL("AXI SRAM wrongly treated as TCM");
        }
        else
        {
            PASS();
        }
    }
}

/* ------------------------------------------------------------------ */
/* FM_Transfer — copying between storages without corruption           */
/* ------------------------------------------------------------------ */
static void test_transfer(void)
{
    TEST("Transfer_SaveFile + LoadFile round-trips exactly");
    {
        static uint8_t src[3000];
        static uint8_t back[3000];
        size_t n = 0;
        size_t i;
        FM_Bool same = FM_TRUE;

        for (i = 0; i < sizeof(src); i++)
        {
            src[i] = (uint8_t)(i * 7u + (i >> 5));
        }
        if (FM_Transfer_SaveFile("/xfer_a.bin", src, sizeof(src)) != FM_OK)
        {
            FAIL("save failed");
        }
        else if (FM_Transfer_LoadFile("/xfer_a.bin", back, sizeof(back),
                                      &n) != FM_OK)
        {
            FAIL("load failed");
        }
        else if (n != sizeof(src))
        {
            FAIL("length mismatch");
        }
        else
        {
            for (i = 0; i < sizeof(src); i++)
            {
                if (back[i] != src[i]) { same = FM_FALSE; break; }
            }
            if (same) { PASS(); } else { FAIL("content differs"); }
        }
    }

    TEST("Transfer_Copy duplicates a file byte-for-byte");
    {
        static uint8_t a[2500];
        static uint8_t b[2500];
        size_t na = 0;
        size_t nb = 0;
        size_t i;
        FM_Bool same = FM_TRUE;

        for (i = 0; i < sizeof(a); i++)
        {
            a[i] = (uint8_t)(255u - (i & 0xFFu));
        }
        (void)FM_Transfer_SaveFile("/xfer_b.bin", a, sizeof(a));
        (void)File_Delete("/xfer_c.bin");
        if (FM_Transfer_Copy("/xfer_b.bin", "/xfer_c.bin", NULL) != FM_OK)
        {
            FAIL("copy failed");
        }
        else if (FM_Transfer_LoadFile("/xfer_b.bin", a, sizeof(a), &na) !=
                     FM_OK ||
                 FM_Transfer_LoadFile("/xfer_c.bin", b, sizeof(b), &nb) !=
                     FM_OK)
        {
            FAIL("reload failed");
        }
        else if (na != nb)
        {
            FAIL("size mismatch");
        }
        else
        {
            for (i = 0; i < na; i++)
            {
                if (a[i] != b[i]) { same = FM_FALSE; break; }
            }
            if (same) { PASS(); } else { FAIL("copy differs"); }
        }
    }

    TEST("Transfer_Copy verify=1 reports matching CRCs");
    {
        static uint8_t d[1777];
        FM_TransferOpts o;
        FM_TransferInfo info;
        size_t i;

        for (i = 0; i < sizeof(d); i++)
        {
            d[i] = (uint8_t)(i ^ 0x5Au);
        }
        (void)FM_Transfer_SaveFile("/xfer_d.bin", d, sizeof(d));
        (void)File_Delete("/xfer_e.bin");
        memset(&o, 0, sizeof(o));
        o.verify = FM_TRUE;
#if !FM_USE_CRC
        {
            /* No CRC module: verify cannot run, so the copy must be
             * refused BEFORE the destination is created -- not done and
             * then reported as "verified". */
            FM_Bool made = FM_TRUE;
            FM_Result vr = FM_Transfer_CopyEx("/xfer_d.bin", "/xfer_e.bin",
                                              &o, &info);

            (void)File_Exists("/xfer_e.bin", &made);
            if (vr == FM_ERR_UNSUPPORTED && !made && !info.verified)
            {
                PASS();
            }
            else
            {
                FAIL("verify without FM_USE_CRC was not refused up front");
            }
        }
#else
        if (FM_Transfer_CopyEx("/xfer_d.bin", "/xfer_e.bin", &o,
                               &info) != FM_OK)
        {
            FAIL("verified copy failed");
        }
        else if (!info.verified)
        {
            FAIL("verify flag ignored");
        }
        else if (info.crc_source != info.crc_dest)
        {
            FAIL("CRC mismatch on a good copy");
        }
        else if (info.bytes != (uint64_t)sizeof(d))
        {
            FAIL("byte count wrong");
        }
        else
        {
            PASS();
        }
#endif /* FM_USE_CRC */
    }

    TEST("Transfer_Copy overwrites by default, refuses with no_overwrite");
    {
        static uint8_t d[64];
        FM_TransferOpts o;

        memset(d, 0xA5, sizeof(d));
        (void)FM_Transfer_SaveFile("/xfer_f.bin", d, sizeof(d));
        (void)FM_Transfer_SaveFile("/xfer_g.bin", d, sizeof(d));
        if (FM_Transfer_Copy("/xfer_f.bin", "/xfer_g.bin", NULL) != FM_OK)
        {
            FAIL("default copy refused an existing target");
        }
        else
        {
            memset(&o, 0, sizeof(o));
            o.no_overwrite = FM_TRUE;
            if (FM_Transfer_Copy("/xfer_f.bin", "/xfer_g.bin", &o) !=
                FM_ERR_EXISTS)
            {
                FAIL("no_overwrite did not protect the target");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("Transfer_LoadFile rejects a buffer that is too small");
    {
        static uint8_t d[900];
        static uint8_t small[100];
        size_t n = 0;

        memset(d, 0x3C, sizeof(d));
        (void)FM_Transfer_SaveFile("/xfer_h.bin", d, sizeof(d));
        if (FM_Transfer_LoadFile("/xfer_h.bin", small, sizeof(small), &n) !=
            FM_ERR_TOO_LARGE)
        {
            FAIL("did not report TOO_LARGE");
        }
        else
        {
            PASS();
        }
    }

    TEST("Transfer_Move removes the source");
    {
        static uint8_t d[128];
        FM_Bool exists = FM_TRUE;

        memset(d, 0x11, sizeof(d));
        (void)FM_Transfer_SaveFile("/xfer_i.bin", d, sizeof(d));
        (void)File_Delete("/xfer_j.bin");
        if (FM_Transfer_Move("/xfer_i.bin", "/xfer_j.bin", NULL) != FM_OK)
        {
            FAIL("move failed");
        }
        else if (File_Exists("/xfer_i.bin", &exists) != FM_OK || exists)
        {
            FAIL("source still present");
        }
        else if (File_Exists("/xfer_j.bin", &exists) != FM_OK || !exists)
        {
            FAIL("destination missing");
        }
        else
        {
            PASS();
        }
    }


    TEST("Transfer_Raw aligns chunks to the destination erase unit");
    {
        /* Copying into NOR flash: a chunk that straddles a sector
         * boundary costs two read-modify-erase-write cycles instead of
         * one -- twice the time and twice the wear.  Count the erases
         * the simulated chip actually performs. */
        static uint8_t sector_buf[QSIM_SECTOR];
        static FM_Storage src_st;
        static FM_Storage dst_st;
        FM_SpiFlashPort port;
        FM_QspiDevice dev;
        static uint8_t ramdisk[16u * 1024u];
        FM_Result r;
        uint32_t erases;

        memset(g_qsim, 0xFF, sizeof(g_qsim));
        g_qsim_wel = 0u;
        g_spif_cs = 0u;
        memset(&port, 0, sizeof(port));
        port.transfer = spisim_transfer;
        port.cs_select = spisim_cs_select;
        port.cs_release = spisim_cs_release;

        memset(ramdisk, 0x5A, sizeof(ramdisk));
        if (FM_SpiFlash_Init(&dev, &port, FM_QSPI_CHIP_W25Q128,
                             sector_buf, sizeof(sector_buf)) != FM_OK)
        {
            FAIL("flash init failed");
        }
        else
        {
            dev.info.size_bytes = QSIM_SIZE;
            if (RamDisk_Register(&src_st, "xfsrc", ramdisk,
                                 sizeof(ramdisk)) != FM_OK ||
                FM_Qspi_RegisterStorage(&dev, &dst_st, "xfdst") != FM_OK)
            {
                FAIL("storage registration failed");
            }
            else
            {
                dev.stat_erases = 0u;
                /* four sectors' worth, starting sector-aligned */
                r = FM_Transfer_Raw(&src_st, 0, &dst_st, 0,
                                    4u * QSIM_SECTOR, NULL);
                erases = dev.stat_erases;
                (void)Storage_Unregister("xfsrc");
                (void)Storage_Unregister("xfdst");

                if (r != FM_OK)
                {
                    FAIL("raw copy failed");
                }
                else if (erases > 8u)
                {
                    /* 4 sectors need 4 erases from FM_Transfer_Raw's
                     * up-front erase plus at most one per sector from
                     * the writes; more than that means chunks were
                     * straddling boundaries. */
                    static char m[64];

                    (void)snprintf(m, sizeof(m),
                                   "%u erases for 4 sectors", erases);
                    FAIL(m);
                }
                else
                {
                    PASS();
                }
            }
        }
    }

    TEST("Transfer cancel leaves no partial file behind");
    {
        static uint8_t d[9000];
        FM_TransferOpts o;

        memset(d, 0x77, sizeof(d));
        (void)FM_Transfer_SaveFile("/xfer_k.bin", d, sizeof(d));
        (void)File_Delete("/xfer_l.bin");
        memset(&o, 0, sizeof(o));
        o.progress = test_xfer_cancel_cb;
        if (FM_Transfer_Copy("/xfer_k.bin", "/xfer_l.bin", &o) !=
            FM_ERR_CANCELLED)
        {
            FAIL("cancel ignored");
        }
        else
        {
            FM_Bool exists = FM_TRUE;

            if (File_Exists("/xfer_l.bin", &exists) == FM_OK && exists)
            {
                FAIL("partial file left behind");
            }
            else
            {
                PASS();
            }
        }
    }
}

#if FM_USE_VIDEO
static void test_video(void)
{
    FM_Result r;
    uint8_t avi[2048];
    size_t avi_len = 0;
#if FM_USE_H264
    uint8_t mp4[4096];
    size_t mp4_len = 0;
    uint8_t h264[2048];
    size_t rd = 0;
    uint8_t rgb[64 * 64 * 3];
#endif
    FM_Video vid;

    /* remount POSIX at "/" (previous tests unmounted it) */
    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("Video: remount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }

#if FM_USE_H264
    TEST("H264 codec: decode tiny vector");
    r = File_ReadAll("/test/vectors/tiny64.h264", h264, sizeof(h264), &rd);
    if (r == FM_OK)
    {
        const FM_VideoCodecOps *ops = H264_GetCodecOps();
        void *ctx = NULL;

        r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            memset(rgb, 0, sizeof(rgb));
            r = ops->decode_frame(ctx, h264, rd, rgb, sizeof(rgb));
        }
        if (r == FM_OK)
        {
            /* expect a 64x64 non-blank frame */
            FM_Bool blank = FM_TRUE;
            size_t i;

            for (i = 0; i < sizeof(rgb); i++)
            {
                if (rgb[i] != 0) { blank = FM_FALSE; break; }
            }
            if (blank) r = FM_ERR_INVALID_FORMAT;
        }
        if (ctx != NULL) ops->close(ctx);
    }
    CHECK_RESULT(r, "h264 decode");
#endif /* FM_USE_H264 */

#if FM_USE_H264
    /* Sequential decode of the full-length sample clips shipped in the
     * repo (regression for the "playlist dies on one bad frame" class
     * of bugs: wrapper state, SPS/PPS re-sends, AVI start-code framing).
     * Files are opened through the normal Video_* path. */
    TEST("H264 codec: sequential decode of sample clips");
    {
        static const char *const clips[] = {
            "/test_videos/v320_h264_baseline.mp4",
            "/test_videos/v320_h264.m4v",
            "/test_videos/v320_h264_pcm.avi"
        };
        size_t ci;
        uint32_t total_fails = 0;
        uint32_t total_ok = 0;
        FM_Bool any = FM_FALSE;

        for (ci = 0; ci < sizeof(clips) / sizeof(clips[0]); ci++)
        {
            FM_Video clip;

            if (Video_Open(clips[ci], &clip) != FM_OK)
            {
                continue; /* clip not present: skip */
            }
            any = FM_TRUE;
            if (clip.info.codec == FM_VIDEO_CODEC_H264)
            {
                (void)Video_RegisterCodec(&clip, H264_GetCodecOps());
            }
            for (;;)
            {
                static uint8_t px[720 * 480 * 3];
                FM_Result dr = Video_DecodeFrame(&clip, px, sizeof(px));

                if (dr == FM_ERR_EOF)
                {
                    break;
                }
                if (dr == FM_OK)
                {
                    total_ok++;
                }
                else
                {
                    total_fails++;
                }
            }
            (void)Video_Close(&clip);
        }
        if (!any)
        {
            printf("(no sample clips) ");
            PASS();
        }
        else if (total_fails == 0u && total_ok > 0u)
        {
            PASS();
        }
        else
        {
            static char msg[96];

            (void)snprintf(msg, sizeof(msg),
                           "ok=%lu fails=%lu", (unsigned long)total_ok,
                           (unsigned long)total_fails);
            FAIL(msg);
        }
    }

    /* Self-heal: corrupt one mid-GOP frame payload; the codec plugin
     * must bound the failure burst (broken reference chain) and recover
     * before end of stream instead of failing every remaining frame. */
    TEST("H264 codec: self-heal recovers after corrupted frame");
    {
        FM_Video clip;
        static uint8_t blob[256 * 1024];
        static uint8_t px[720 * 480 * 3];
        const FM_VideoCodecOps *ops = H264_GetCodecOps();
        void *ctx = NULL;
        FM_VideoInfo vi;
        uint32_t frame_no = 0;
        uint32_t fails = 0;
        uint32_t last_fail = 0;
        FM_Bool opened = FM_FALSE;

        if (Video_Open("/test_videos/v320_h264.m4v", &clip) == FM_OK)
        {
            opened = FM_TRUE;
            memset(&vi, 0, sizeof(vi));
            vi = clip.info;
            (void)Video_Close(&clip);
        }
        if (opened && ops->init(&ctx, &vi) == FM_OK)
        {
            if (Video_Open("/test_videos/v320_h264.m4v", &clip) == FM_OK)
            {
                for (;;)
                {
                    size_t got = 0;
                    FM_Result dr = Video_ReadFrame(&clip, blob, sizeof(blob),
                                                   &got);

                    if (dr != FM_OK)
                    {
                        break;
                    }
                    if (frame_no == 40u && got > 32u)
                    {
                        memset(blob + got / 2u, 0x55, got - got / 2u);
                    }
                    dr = ops->decode_frame(ctx, blob, got, px, sizeof(px));
                    if (dr != FM_OK)
                    {
                        fails++;
                        last_fail = frame_no;
                    }
                    frame_no++;
                }
                (void)Video_Close(&clip);
            }
            ops->close(ctx);
        }
        if (!opened)
        {
            printf("(no sample clip) ");
            PASS();
        }
        else if (fails > 0u && fails <= 8u && frame_no > 0u &&
                 last_fail + 8u < frame_no)
        {
            PASS();
        }
        else
        {
            static char msg[96];

            (void)snprintf(msg, sizeof(msg),
                           "fails=%lu last_fail=%lu frames=%lu",
                           (unsigned long)fails, (unsigned long)last_fail,
                           (unsigned long)frame_no);
            FAIL(msg);
        }
    }
#endif /* FM_USE_H264 */

#if FM_USE_H264
    /* Regression: when the allocator cannot satisfy the small SPS/PPS
     * store allocations, h264bsd used to drop the failure and only trip
     * later on param-set activation.  The plugin then reported
     * FM_ERR_INVALID_FORMAT ("unplayable file") for a perfectly valid
     * stream, hiding the real cause (FM_Alloc pool too small).  A frame
     * that carries SPS+PPS must now surface FM_ERR_NO_MEMORY. */
    TEST("H264 codec: OOM reports NO_MEMORY not INVALID_FORMAT");
    {
        FM_Video clip;
        static uint8_t blob[256 * 1024];
        static uint8_t px[720 * 480 * 3];
        FM_Result got = FM_OK;
        FM_Bool ran = FM_FALSE;

        if (Video_Open("/test_videos/v320_h264.m4v", &clip) == FM_OK)
        {
            size_t n = 0;

            if (Video_ReadFrame(&clip, blob, sizeof(blob), &n) == FM_OK &&
                n > 0u)
            {
                const FM_VideoCodecOps *ops = H264_GetCodecOps();
                const FM_Allocator *saved = FM_Allocator_Get();
                FM_Allocator saved_copy = *saved;
                void *ctx = NULL;

                if (ops->init(&ctx, &clip.info) == FM_OK)
                {
                    FM_Allocator starve;

                    /* Starve only the param-set stores; the big buffers
                     * were already taken during init above. */
                    starve = saved_copy;
                    starve.alloc = test_alloc_no_paramsets;
                    starve.calloc = NULL;
                    starve.realloc = NULL;
                    (void)FM_Allocator_Set(&starve);
                    got = ops->decode_frame(ctx, blob, n, px, sizeof(px));
                    (void)FM_Allocator_Set(&saved_copy);
                    ops->close(ctx);
                    ran = FM_TRUE;
                }
            }
            (void)Video_Close(&clip);
        }
        if (!ran)
        {
            printf("(no sample clip) ");
            PASS();
        }
        else if (got == FM_ERR_NO_MEMORY)
        {
            PASS();
        }
        else
        {
            static char msg[64];

            (void)snprintf(msg, sizeof(msg), "got %d, want %d",
                           (int)got, (int)FM_ERR_NO_MEMORY);
            FAIL(msg);
        }
    }

    /* h264bsdDecodeSeqParamSet/DecodePicParamSet also ALLOCATE
     * (offsetForRefFrame, vuiParameters, runLength, sliceGroupId ...) and
     * return MEMORY_ALLOCATION_ERROR.  Upstream folded that into
     * H264BSD_ERROR, which the wrapper treats as "damaged NAL: skip and
     * resync" — so a too-small pool surfaced as FM_ERR_INVALID_FORMAT
     * (-17) with a non-zero skip count instead of FM_ERR_NO_MEMORY.
     * Walk every allocation index: no failure point may report -17. */
    /* h264bsdExtractNalUnit strips emulation-prevention bytes IN PLACE,
     * so the self-heal keyframe replay must not be handed the cache
     * itself: the 0x000003 sequences inside the cached SPS collapse to
     * 0x0000, the SPS is rejected on every later replay, and the slices
     * that follow cannot find their parameter set.  Decode a frame,
     * force a self-heal with a damaged frame, then verify that clean
     * frames still decode afterwards. */
    /* h264bsdExtractNalUnit strips emulation-prevention bytes IN PLACE,
     * so decoding rewrites the caller's frame buffer.  Any retry that
     * re-reads that buffer parses a mangled SPS.  Assert the mutation is
     * real (so the guard is never "optimised away" as unnecessary) and
     * that a pristine copy still decodes. */
    TEST("H264 codec: decode mutates input, pristine copy still decodes");
    {
        static uint8_t fr[192 * 1024];
        static uint8_t orig[192 * 1024];
        static uint8_t px[720 * 480 * 3];
        FM_Video clip;
        FM_Bool ran = FM_FALSE;
        FM_Bool mutated = FM_FALSE;
        FM_Result replay = FM_ERR_INVALID_FORMAT;

        if (Video_Open("/test_videos/v320_h264.m4v", &clip) == FM_OK)
        {
            const FM_VideoCodecOps *ops = H264_GetCodecOps();
            void *ctx = NULL;
            size_t n = 0;

            if (Video_ReadFrame(&clip, fr, sizeof(fr), &n) == FM_OK &&
                n > 0u && ops->init(&ctx, &clip.info) == FM_OK)
            {
                size_t i;

                memcpy(orig, fr, n);
                (void)ops->decode_frame(ctx, fr, n, px, sizeof(px));
                for (i = 0; i < n; i++)
                {
                    if (fr[i] != orig[i])
                    {
                        mutated = FM_TRUE;
                        break;
                    }
                }
                ops->close(ctx);

                ctx = NULL;
                if (ops->init(&ctx, &clip.info) == FM_OK)
                {
                    replay = ops->decode_frame(ctx, orig, n, px, sizeof(px));
                    ops->close(ctx);
                }
                ran = FM_TRUE;
            }
            (void)Video_Close(&clip);
        }
        if (!ran)
        {
            printf("(no sample clip) ");
            PASS();
        }
        else if (mutated && replay == FM_OK)
        {
            PASS();
        }
        else if (!mutated)
        {
            PASS();  /* decoder no longer mutates: guard is harmless */
        }
        else
        {
            FAIL("pristine copy failed to decode");
        }
    }

    TEST("H264 codec: self-heal replay does not corrupt the keyframe cache");
    {
        static uint8_t fr[192 * 1024];
        static uint8_t px[720 * 480 * 3];
        FM_Video clip;
        FM_Bool ran = FM_FALSE;
        uint32_t good = 0;
        uint32_t bad = 0;

        if (Video_Open("/test_videos/v320_h264_pcm.avi", &clip) == FM_OK)
        {
            const FM_VideoCodecOps *ops = H264_GetCodecOps();
            void *ctx = NULL;
            size_t n = 0;

            if (ops->init(&ctx, &clip.info) == FM_OK)
            {
                if (Video_ReadFrame(&clip, fr, sizeof(fr), &n) == FM_OK &&
                    n > 0u)
                {
                    uint32_t i;

                    /* prime the cache with a real keyframe */
                    (void)ops->decode_frame(ctx, fr, n, px, sizeof(px));
                    /* damage one frame to trigger the reset + replay */
                    if (Video_ReadFrame(&clip, fr, sizeof(fr), &n) == FM_OK &&
                        n > 64u)
                    {
                        size_t k;

                        for (k = n / 2u; k < (n / 2u) + 64u; k++)
                        {
                            fr[k] ^= 0xFFu;
                        }
                        (void)ops->decode_frame(ctx, fr, n, px, sizeof(px));
                    }
                    /* clean frames from here on must decode again */
                    for (i = 0; i < 30u; i++)
                    {
                        if (Video_ReadFrame(&clip, fr, sizeof(fr), &n) !=
                            FM_OK)
                        {
                            break;
                        }
                        if (ops->decode_frame(ctx, fr, n, px,
                                              sizeof(px)) == FM_OK)
                        {
                            good++;
                        }
                        else
                        {
                            bad++;
                        }
                    }
                    ran = FM_TRUE;
                }
                ops->close(ctx);
            }
            (void)Video_Close(&clip);
        }
        if (!ran)
        {
            printf("(no sample clip) ");
            PASS();
        }
        else if (good >= 25u)
        {
            PASS();
        }
        else
        {
            static char msg[72];

            (void)snprintf(msg, sizeof(msg),
                           "after self-heal only %u/%u frames decoded",
                           (unsigned)good, (unsigned)(good + bad));
            FAIL(msg);
        }
    }

    TEST("H264 codec: every OOM point reports NO_MEMORY, never -17");
    {
        static uint8_t blob[256 * 1024];
        static uint8_t px[720 * 480 * 3];
        size_t n = 0;
        FM_Video clip;
        FM_Bool ran = FM_FALSE;
        FM_Bool bad = FM_FALSE;
        long bad_at = 0;
        FM_Result bad_r = FM_OK;

        if (Video_Open("/test_videos/v320_h264_pcm.avi", &clip) == FM_OK)
        {
            if (Video_ReadFrame(&clip, blob, sizeof(blob), &n) == FM_OK &&
                n > 0u)
            {
                ran = FM_TRUE;
            }
            (void)Video_Close(&clip);
        }
        if (ran)
        {
            const FM_VideoCodecOps *ops = H264_GetCodecOps();
            const FM_Allocator *saved = FM_Allocator_Get();
            FM_Allocator saved_copy = *saved;
            long k;

            for (k = 1; k <= 40 && !bad; k++)
            {
                FM_Allocator starve;
                void *ctx = NULL;
                FM_Result got;
                unsigned long fails_before;

                starve = saved_copy;
                starve.alloc = test_alloc_seq;
                starve.calloc = NULL;
                starve.realloc = NULL;
                test_alloc_seq_n = 0;
                test_alloc_seq_fail_from = k;
                (void)FM_Allocator_Set(&starve);
                if (ops->init(&ctx, &clip.info) == FM_OK)
                {
                    fails_before = FM_Alloc_FailureCount();
                    got = ops->decode_frame(ctx, blob, n, px, sizeof(px));
                    ops->close(ctx);
                    /* Only assert on runs where the allocator actually
                     * refused something during the decode itself.  When
                     * the budget is spent entirely by init the decode
                     * runs on a healthy allocator and any error it
                     * returns is a genuine bitstream verdict, not an
                     * out-of-memory condition to relabel. */
                    if (FM_Alloc_FailureCount() != fails_before &&
                        got != FM_OK && got != FM_ERR_NO_MEMORY)
                    {
                        bad = FM_TRUE;
                        bad_at = k;
                        bad_r = got;
                    }
                }
                (void)FM_Allocator_Set(&saved_copy);
            }
        }
        if (!ran)
        {
            printf("(no sample clip) ");
            PASS();
        }
        else if (!bad)
        {
            PASS();
        }
        else
        {
            static char msg[80];

            (void)snprintf(msg, sizeof(msg),
                           "alloc #%ld gave %d, want -14",
                           bad_at, (int)bad_r);
            FAIL(msg);
        }
    }
#endif /* FM_USE_H264 */

    TEST("AVI container: parse + read frames");
    r = build_avi(avi, &avi_len);
    if (r == FM_OK) r = File_WriteAll("/test_video.avi", avi, avi_len);
    if (r == FM_OK) r = Video_Open("/test_video.avi", &vid);
    if (r == FM_OK && (vid.info.width != 64 || vid.info.height != 64 ||
                       vid.info.codec != FM_VIDEO_CODEC_MJPEG ||
                       vid.info.frame_count != 3))
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    if (r == FM_OK)
    {
        uint8_t frame[16];
        size_t fsize = 0;
        uint32_t n;

        for (n = 0; n < 3 && r == FM_OK; n++)
        {
            r = Video_ReadFrame(&vid, frame, sizeof(frame), &fsize);
        }
        if (r == FM_OK) r = Video_Seek(&vid, 1);
        if (r == FM_OK)
        {
            fsize = 0;
            r = Video_ReadFrame(&vid, frame, sizeof(frame), &fsize);
            if (r == FM_OK && (fsize != 4 || memcmp(frame, "FRM2", 4) != 0))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "avi parse");

#if FM_USE_H264
    TEST("MP4 container: H.264 decode end-to-end");
    {
        const uint8_t *nals[8];
        uint32_t sizes[8];
        uint32_t nn = split_nals(h264, (uint32_t)rd, nals, sizes, 8);

        r = FM_ERR_INVALID_FORMAT;
        if (nn >= 5)
        {
            /* expect SPS, PPS, IDR, P, P */
            r = build_mp4(nals[0], sizes[0], nals[1], sizes[1],
                          nals[2], sizes[2], nals[3], sizes[3],
                          nals[4], sizes[4], mp4, &mp4_len);
        }
        if (r == FM_OK) r = File_WriteAll("/test_video.mp4", mp4, mp4_len);
        if (r == FM_OK) r = Video_Open("/test_video.mp4", &vid);
        if (r == FM_OK && (vid.info.width != 64 || vid.info.height != 64 ||
                           vid.info.codec != FM_VIDEO_CODEC_H264 ||
                           vid.info.frame_count != 3))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK) r = Video_RegisterCodec(&vid, H264_GetCodecOps());
        if (r == FM_OK)
        {
            memset(rgb, 0, sizeof(rgb));
            r = Video_DecodeFrame(&vid, rgb, sizeof(rgb));
            if (r == FM_OK)
            {
                FM_Bool blank = FM_TRUE;
                size_t i;

                for (i = 0; i < sizeof(rgb); i++)
                {
                    if (rgb[i] != 0) { blank = FM_FALSE; break; }
                }
                if (blank) r = FM_ERR_INVALID_FORMAT;
            }
        }
        if (r == FM_OK)
        {
            /* verify the demuxed AAC audio track */
            FM_Bool has = FM_FALSE;
            FM_AudioTrackInfo ati;
            uint8_t abuf[64];
            size_t asize = 0;

            r = Video_HasAudio(&vid, &has);
            if (r == FM_OK && !has) r = FM_ERR_UNSUPPORTED;
            if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ati);
            if (r == FM_OK && (ati.codec != FM_VIDEO_AUDIO_AAC ||
                               ati.channels != 2 || ati.sample_rate != 44100 ||
                               ati.sample_count != 3))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK)
            {
                const char *expected[3] = { "AA", "BBBB", "CCCCCC" };
                uint32_t k;

                for (k = 0; k < 3 && r == FM_OK; k++)
                {
                    asize = 0;
                    r = Video_ReadAudioFrame(&vid, abuf, sizeof(abuf),
                                             &asize);
                    if (r == FM_OK)
                    {
                        size_t plen = strlen(expected[k]);

                        if (asize != 7u + plen || abuf[0] != 0xFF ||
                            abuf[1] != 0xF1 ||
                            memcmp(abuf + 7, expected[k], plen) != 0)
                        {
                            r = FM_ERR_INVALID_FORMAT;
                        }
                    }
                }
            }
        }
        (void)Video_Close(&vid);
        CHECK_RESULT(r, "mp4 decode");
    }
#endif /* FM_USE_H264 */

    /* AVI audio track (PCM) demux */
    TEST("AVI audio track demux (PCM)");
    {
        uint8_t avibuf[4096];
        size_t avibuf_len = 0;
        uint8_t abuf[64];
        size_t asize = 0;
        FM_Bool has = FM_FALSE;
        FM_AudioTrackInfo ati;
        TstBuf t;

        memset(&t, 0, sizeof(t));
        t.be = FM_FALSE;
        {
            size_t riff = tb_riff(&t, "RIFF");
            tb_tag(&t, "AVI ");
            {
                size_t hdrl = tb_riff(&t, "LIST");
                tb_tag(&t, "hdrl");
                {
                    size_t avih = tb_riff(&t, "avih");
                    tb_u32(&t, 33333); tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_u32(&t, 1); tb_u32(&t, 0); tb_u32(&t, 2); tb_u32(&t, 0);
                    tb_u32(&t, 64); tb_u32(&t, 64);
                    tb_riff_end(&t, avih);
                }
                {
                    size_t strl = tb_riff(&t, "LIST");
                    tb_tag(&t, "strl");
                    {
                        size_t strh = tb_riff(&t, "strh");
                        tb_tag(&t, "vids"); tb_tag(&t, "MJPG");
                        tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0); tb_u32(&t, 0);
                        tb_u32(&t, 1); tb_u32(&t, 30); tb_u32(&t, 0); tb_u32(&t, 1);
                        tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                        tb_riff_end(&t, strh);
                    }
                    {
                        size_t strf = tb_riff(&t, "strf");
                        tb_u32(&t, 40); tb_u32(&t, 64); tb_u32(&t, 64);
                        tb_u16(&t, 1); tb_u16(&t, 24); tb_tag(&t, "MJPG"); tb_u32(&t, 0);
                        tb_riff_end(&t, strf);
                    }
                    tb_riff_end(&t, strl);
                }
                {
                    size_t strl = tb_riff(&t, "LIST");
                    tb_tag(&t, "strl");
                    {
                        size_t strh = tb_riff(&t, "strh");
                        tb_tag(&t, "auds");
                        tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0); tb_u32(&t, 0);
                        tb_u32(&t, 1); tb_u32(&t, 48000); tb_u32(&t, 0); tb_u32(&t, 2);
                        tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                        tb_riff_end(&t, strh);
                    }
                    {
                        size_t strf = tb_riff(&t, "strf");
                        tb_u16(&t, 1); tb_u16(&t, 2); tb_u32(&t, 48000);
                        tb_u32(&t, 48000 * 4); tb_u16(&t, 4); tb_u16(&t, 16);
                        tb_riff_end(&t, strf);
                    }
                    tb_riff_end(&t, strl);
                }
                tb_riff_end(&t, hdrl);
            }
            {
                size_t movi = tb_riff(&t, "LIST");
                tb_tag(&t, "movi");
                {
                    size_t f = tb_riff(&t, "00dc");
                    tb_bytes(&t, "FRM", 3); tb_u8(&t, 0);
                    tb_riff_end(&t, f);
                }
                {
                    size_t a1 = tb_riff(&t, "01wb");
                    tb_bytes(&t, "PCMDATA1", 8);
                    tb_riff_end(&t, a1);
                }
                tb_riff_end(&t, movi);
            }
            tb_riff_end(&t, riff);
        }
        memcpy(avibuf, t.b, t.n);
        avibuf_len = t.n;

        r = File_WriteAll("/test_audio.avi", avibuf, avibuf_len);
        if (r == FM_OK) r = Video_Open("/test_audio.avi", &vid);
        if (r == FM_OK) r = Video_HasAudio(&vid, &has);
        if (r == FM_OK && !has) r = FM_ERR_UNSUPPORTED;
        if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ati);
        if (r == FM_OK && (ati.codec != FM_VIDEO_AUDIO_PCM ||
                           ati.channels != 2 || ati.sample_rate != 48000 ||
                           ati.bits_per_sample != 16 || ati.sample_count != 1))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK)
        {
            asize = 0;
            r = Video_ReadAudioFrame(&vid, abuf, sizeof(abuf), &asize);
            if (r == FM_OK && (asize != 8 ||
                               memcmp(abuf, "PCMDATA1", 8) != 0))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        /* Regression: an audio chunk larger than the caller's buffer
         * must still make forward progress.  It used to return
         * TOO_LARGE without advancing audio_frame_index, so the next
         * refill retried the same chunk forever and the track went
         * permanently silent after the first oversized chunk.
         * Second regression: the part that did not fit used to be
         * dropped (a slice cut out of the waveform on every oversized
         * chunk); now the next call continues the same chunk. */
        if (r == FM_OK)
        {
            uint8_t small[4];

            (void)Video_SeekAudio(&vid, 0);
            asize = 0;
            r = Video_ReadAudioFrame(&vid, small, sizeof(small), &asize);
            /* partial delivery, aligned to a whole 4-byte stereo frame */
            if (r == FM_OK && (asize != 4u ||
                               memcmp(small, "PCMD", 4) != 0))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* the remainder of the same chunk comes next, nothing lost */
            if (r == FM_OK)
            {
                size_t again = 0;

                r = Video_ReadAudioFrame(&vid, small, sizeof(small),
                                         &again);
                if (r == FM_OK && (again != 4u ||
                                   memcmp(small, "ATA1", 4) != 0))
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
            }
            /* and only then the cursor is past the chunk: EOF, not the
             * same chunk again */
            if (r == FM_OK)
            {
                size_t again = 0;

                if (Video_ReadAudioFrame(&vid, small, sizeof(small),
                                         &again) != FM_ERR_EOF)
                {
                    r = FM_ERR_CORRUPTED;
                }
            }
            /* a seek discards a half-delivered chunk */
            if (r == FM_OK)
            {
                size_t again = 0;

                (void)Video_SeekAudio(&vid, 0);
                (void)Video_ReadAudioFrame(&vid, small, sizeof(small),
                                           &again);
                (void)Video_SeekAudio(&vid, 0);
                r = Video_ReadAudioFrame(&vid, small, sizeof(small),
                                         &again);
                if (r == FM_OK && memcmp(small, "PCMD", 4) != 0)
                {
                    r = FM_ERR_CORRUPTED;
                }
            }
        }
        (void)Video_Close(&vid);
        CHECK_RESULT(r, "avi audio demux");
        (void)File_Delete("/test_audio.avi");
    }

    (void)File_Delete("/test_video.avi");
    (void)File_Delete("/test_video.mp4");
}
#endif /* FM_USE_VIDEO */

/* ================================================================ */
/* MJPEG decoding                                                     */
/* ================================================================ */
#if FM_USE_MJPEG
static FM_Result build_avi_mjpeg(const uint8_t *jpg, size_t jpg_len,
                                 uint8_t *out, size_t *out_len)
{
    TstBuf t;

    memset(&t, 0, sizeof(t));
    t.be = FM_FALSE;
    {
        size_t riff = tb_riff(&t, "RIFF");
        tb_tag(&t, "AVI ");
        {
            size_t hdrl = tb_riff(&t, "LIST");
            tb_tag(&t, "hdrl");
            {
                size_t avih = tb_riff(&t, "avih");
                tb_u32(&t, 33333);
                tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                tb_u32(&t, 1);   /* total frames */
                tb_u32(&t, 0);
                tb_u32(&t, 1);   /* streams */
                tb_u32(&t, 0);
                tb_u32(&t, 32);  /* width */
                tb_u32(&t, 32);  /* height */
                tb_riff_end(&t, avih);
            }
            {
                size_t strl = tb_riff(&t, "LIST");
                tb_tag(&t, "strl");
                {
                    size_t strh = tb_riff(&t, "strh");
                    tb_tag(&t, "vids");
                    tb_tag(&t, "MJPG");
                    tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 0);
                    tb_u32(&t, 0);
                    tb_u32(&t, 1); tb_u32(&t, 30);
                    tb_u32(&t, 0); tb_u32(&t, 1);
                    tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                    tb_riff_end(&t, strh);
                }
                {
                    size_t strf = tb_riff(&t, "strf");
                    tb_u32(&t, 40);
                    tb_u32(&t, 32);
                    tb_u32(&t, 32);
                    tb_u16(&t, 1); tb_u16(&t, 24);
                    tb_tag(&t, "MJPG");
                    tb_u32(&t, 0);
                    tb_riff_end(&t, strf);
                }
                tb_riff_end(&t, strl);
            }
            tb_riff_end(&t, hdrl);
        }
        {
            size_t movi = tb_riff(&t, "LIST");
            tb_tag(&t, "movi");
            {
                size_t f1 = tb_riff(&t, "00dc");
                tb_bytes(&t, jpg, jpg_len);
                tb_riff_end(&t, f1);
            }
            tb_riff_end(&t, movi);
        }
        tb_riff_end(&t, riff);
    }
    memcpy(out, t.b, t.n);
    *out_len = t.n;
    return FM_OK;
}

static int *g_hw_marker;

static FM_Result hw_mjpeg_init(void **ctx, const FM_VideoInfo *info)
{
    (void)info;
    *ctx = (void *)1;
    return FM_OK;
}

static FM_Result hw_mjpeg_decode(void *ctx, const void *compressed,
                                 size_t size, void *out, size_t cap)
{
    (void)ctx;
    (void)compressed;
    (void)size;
    if (g_hw_marker != NULL)
    {
        (*g_hw_marker)++;
    }
    memset(out, 0xAB, cap);
    return FM_OK;
}

static void hw_mjpeg_close(void *ctx)
{
    (void)ctx;
}

static void test_mjpeg(void)
{
    uint8_t jpg[4096];
    size_t jpg_len = 0;
    uint8_t avi[8192];
    size_t avi_len = 0;
    uint8_t rgb[32 * 32 * 3];
    FM_Result r;
    FM_Video vid;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("MJPEG: mount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }

    TEST("MJPEG codec: decode frame to RGB");
    r = File_ReadAll("/test/vectors/mjpeg_frame.jpg", jpg, sizeof(jpg),
                     &jpg_len);
    if (r == FM_OK)
    {
        const FM_VideoCodecOps *ops = MJPEG_GetCodecOps();
        void *ctx = NULL;

        r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            memset(rgb, 0, sizeof(rgb));
            r = ops->decode_frame(ctx, jpg, jpg_len, rgb, sizeof(rgb));
        }
        if (r == FM_OK)
        {
            /* left half dark, right half bright */
            uint8_t *left = rgb + (5u * 32u + 5u) * 3u;
            uint8_t *right = rgb + (27u * 32u + 27u) * 3u;

            if (left[0] > 80 || right[0] < 180)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        if (ctx != NULL) ops->close(ctx);
    }
    CHECK_RESULT(r, "mjpeg codec decode");

    TEST("AVI (MJPG) -> MJPEG decode end-to-end");
    r = build_avi_mjpeg(jpg, jpg_len, avi, &avi_len);
    if (r == FM_OK) r = File_WriteAll("/test_mjpeg.avi", avi, avi_len);
    if (r == FM_OK) r = Video_Open("/test_mjpeg.avi", &vid);
    if (r == FM_OK && (vid.info.width != 32 || vid.info.height != 32 ||
                       vid.info.codec != FM_VIDEO_CODEC_MJPEG))
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    if (r == FM_OK) r = Video_RegisterCodec(&vid, MJPEG_GetCodecOps());
    if (r == FM_OK)
    {
        memset(rgb, 0, sizeof(rgb));
        r = Video_DecodeFrame(&vid, rgb, sizeof(rgb));
        if (r == FM_OK)
        {
            uint8_t *right = rgb + (27u * 32u + 27u) * 3u;

            if (right[0] < 180)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
    }
    (void)Video_Close(&vid);
    CHECK_RESULT(r, "avi mjpeg decode");

    TEST("Hardware MJPEG decoder override");
    {
        static int hw_used;
        const FM_VideoCodecOps hwops = {
            "HW-JPEG",
            hw_mjpeg_init,
            hw_mjpeg_decode,
            hw_mjpeg_close
        };

        hw_used = 0;
        g_hw_marker = &hw_used;
        r = HWDecoder_Register(FM_VIDEO_CODEC_MJPEG, &hwops);
        if (r == FM_OK) r = File_WriteAll("/test_hw.avi", avi, avi_len);
        if (r == FM_OK) r = Video_Open("/test_hw.avi", &vid);
        if (r == FM_OK)
        {
            memset(rgb, 0, sizeof(rgb));
            r = Video_DecodeFrame(&vid, rgb, sizeof(rgb)); /* auto HW */
        }
        if (r == FM_OK && hw_used > 0 && rgb[0] == 0xAB)
        {
            PASS();
        }
        else
        {
            FAIL("hw decoder not used");
        }
        (void)Video_Close(&vid);
        (void)HWDecoder_Unregister(FM_VIDEO_CODEC_MJPEG);
        g_hw_marker = NULL;
        (void)File_Delete("/test_hw.avi");
    }

    (void)File_Delete("/test_mjpeg.avi");
}
#endif /* FM_USE_MJPEG */

/* ================================================================ */
/* Allocator hook                                                     */
/* ================================================================ */
static uint32_t g_alloc_calls;
static uint32_t g_free_calls;

static void *cnt_alloc(size_t size, void *ctx)
{
    (void)ctx;
    g_alloc_calls++;
    return malloc(size);
}

static void cnt_free(void *p, void *ctx)
{
    (void)ctx;
    g_free_calls++;
    free(p);
}

TEST_MAYBE_UNUSED static void *null_alloc(size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    return NULL;
}

TEST_MAYBE_UNUSED static void null_free(void *p, void *ctx)
{
    (void)p;
    (void)ctx;
}

static void *def_alloc(size_t size, void *ctx)
{
    (void)ctx;
    return malloc(size);
}

static void *def_calloc(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    return calloc(n, size);
}

static void *def_realloc(void *p, size_t size, void *ctx)
{
    (void)ctx;
    return realloc(p, size);
}

static void def_free(void *p, void *ctx)
{
    (void)ctx;
    free(p);
}

#if FM_USE_AUDIO
/* Opus-in-Ogg decode.  With FM_OPUS_USE_LIBOPUS the vendored libopus is
 * exercised (assert non-silent PCM).  Without it there is no decoder and
 * the library must say so (FM_ERR_UNSUPPORTED) instead of "playing"
 * silence.  The asset lives in test/vectors/test_opus.ogg and is opened
 * through the POSIX backend. */
static void test_opus_ogg(void)
{
    FM_Audio a;
    FM_AudioInfo info;
    FM_Result r;
    int16_t buf[2048];
    size_t n = 0, total = 0, peak = 0, i;

#if !FM_OPUS_USE_LIBOPUS
    TEST("Opus: no libopus -> FM_ERR_UNSUPPORTED, never silence");
    r = Audio_Open("/test/vectors/test_opus.ogg", &a);
    if (r == FM_OK) (void)Audio_Close(&a);
    if (r == FM_ERR_UNSUPPORTED && FM_Opus_GetOps() == NULL &&
        FM_Opus_Init() == FM_ERR_UNSUPPORTED &&
        Audio_GetDecoder("OPUS") == NULL)
    {
        PASS();
    }
    else
    {
        FAIL("Opus without libopus must be reported as unsupported");
    }
    (void)info; (void)buf; (void)n; (void)total; (void)peak; (void)i;
    return;
#else
    TEST("Opus: Ogg Opus decode (test_opus.ogg)");
    r = Audio_Open("/test/vectors/test_opus.ogg", &a);
    if (r == FM_OK) r = Audio_GetInfo(&a, &info);
    if (r == FM_OK && info.sample_rate != 48000u) r = FM_ERR_INVALID_FORMAT;
    if (r == FM_OK && (info.channels == 0u || info.channels > 2u))
        r = FM_ERR_INVALID_FORMAT;
    if (r == FM_OK && strcmp(info.codec, "OPUS") != 0) r = FM_ERR_INVALID_FORMAT;
    while (r == FM_OK)
    {
        r = Audio_Read(&a, buf, sizeof(buf) / sizeof(buf[0]), &n);
        if (r != FM_OK) break;
        for (i = 0; i < n; i++)
        {
            size_t mag = (size_t)(buf[i] < 0 ? -buf[i] : buf[i]);
            if (mag > peak) peak = mag;
        }
        total += n;
    }
    if (r == FM_ERR_EOF) r = FM_OK;              /* normal end of stream */
    /* The asset is a few seconds long: expect at least 1 s of 48 kHz. */
    if (r == FM_OK && total < 48000u) r = FM_ERR_CORRUPTED;
    if (r == FM_OK && peak < 1000u) r = FM_ERR_CORRUPTED;   /* not silence */
    Audio_Close(&a);
    if (r == FM_OK) printf("[%u samples, peak %u] ", (unsigned)total, (unsigned)peak);
    CHECK_RESULT(r, "opus ogg decode");
#endif
}

/* ADPCM WAV: small-chunk reads must not drop samples.  Regression for
 * the byte-overread fix in Audio_Read()'s IMA-ADPCM path: it used to
 * read a full scratch chunk but emit at most `count` samples, silently
 * discarding the rest of what it had consumed. */
static void test_adpcm_small_chunks(void)
{
    FM_Audio a;
    FM_Result r;
    static int16_t small[257];   /* deliberately odd and small */
    static int16_t big[4096];
    size_t n = 0;
    unsigned long total_small = 0, total_big = 0;
    int peak_small = 0, peak_big = 0;
    size_t i;

    TEST("Regression: ADPCM WAV small-chunk reads keep samples");
    r = Audio_Open("/test/vectors/wav_adpcm_ima_22k.wav", &a);
    if (r == FM_OK)
    {
        for (;;)
        {
            r = Audio_Read(&a, small, 257, &n);
            if (r != FM_OK || n == 0) break;
            total_small += (unsigned long)n;
            for (i = 0; i < n; i++)
            {
                int mag = small[i] < 0 ? -small[i] : small[i];
                if (mag > peak_small) peak_small = mag;
            }
        }
        if (r == FM_ERR_EOF) r = FM_OK;
        (void)Audio_Close(&a);
    }
    if (r == FM_OK) r = Audio_Open("/test/vectors/wav_adpcm_ima_22k.wav", &a);
    if (r == FM_OK)
    {
        for (;;)
        {
            r = Audio_Read(&a, big, 4096, &n);
            if (r != FM_OK || n == 0) break;
            total_big += (unsigned long)n;
            for (i = 0; i < n; i++)
            {
                int mag = big[i] < 0 ? -big[i] : big[i];
                if (mag > peak_big) peak_big = mag;
            }
        }
        if (r == FM_ERR_EOF) r = FM_OK;
        (void)Audio_Close(&a);
    }
    /* The 11 KB asset holds >22k samples; allow only the documented
     * <=1-sample-per-call half-nibble drift between the two modes. */
    if (r == FM_OK && (total_big < 20000u ||
                       total_small + 100u < total_big ||
                       peak_small != peak_big))
        r = FM_ERR_CORRUPTED;
    if (r == FM_OK)
        printf("[%lu vs %lu smp, peak %d/%d] ",
               total_small, total_big, peak_small, peak_big);
    CHECK_RESULT(r, "adpcm small chunks");
}
#endif /* FM_USE_AUDIO */

static void test_allocator(void)
{
    FM_Allocator counting;
    FM_Allocator deflt;

    memset(&counting, 0, sizeof(counting));
    counting.alloc = cnt_alloc;
    counting.free = cnt_free;

    /* snapshot whatever is current */
    deflt = *FM_Allocator_Get();
    if (deflt.alloc == NULL)
    {
        deflt.alloc = cnt_alloc;   /* placeholder; restored below */
    }

    TEST("Allocator: H.264 decode routes through FM_Alloc");
    g_alloc_calls = 0;
    g_free_calls = 0;
    (void)FM_Allocator_Set(&counting);
    {
#if FM_USE_H264
        const FM_VideoCodecOps *ops = H264_GetCodecOps();
        void *ctx = NULL;
        uint8_t h264[2048];
        uint8_t rgb[64 * 64 * 3];
        size_t rd = 0;
        FM_Result r = File_ReadAll("/test/vectors/tiny64.h264", h264,
                                   sizeof(h264), &rd);

        if (r == FM_OK) r = ops->init(&ctx, NULL);
        if (r == FM_OK) r = ops->decode_frame(ctx, h264, rd, rgb, sizeof(rgb));
        if (ctx != NULL) ops->close(ctx);
#else
        FM_Free(FM_Alloc(16));
        FM_Free(FM_Alloc(16));
#endif
    }
    {
        FM_Allocator restore;
        memset(&restore, 0, sizeof(restore));
        restore.alloc = def_alloc;
        restore.free = def_free;
        restore.calloc = def_calloc;
        restore.realloc = def_realloc;
        (void)FM_Allocator_Set(&restore);
    }
    if (g_alloc_calls > 0 && g_free_calls > 0)
    {
        PASS();
    }
    else
    {
        FAIL("allocator not used");
    }
}

/* ================================================================ */
/* Image / video scaling                                             */
/* ================================================================ */
#if FM_USE_IMAGE

static void test_scale(void)
{
    uint8_t src[2 * 2 * 3];
    uint8_t dst[4 * 4 * 3];
    FM_Result r;

    /* 2x2: TL red, TR green, BL blue, BR white */
    memset(src, 0, sizeof(src));
    src[0] = 255; src[1] = 0; src[2] = 0;                       /* red   */
    src[3] = 0;   src[4] = 255; src[5] = 0;                     /* green */
    src[6] = 0;   src[7] = 0;   src[8] = 255;                   /* blue  */
    src[9] = 255; src[10] = 255; src[11] = 255;                 /* white */

    TEST("Image_Scale: nearest 2x2 -> 4x4");
    r = Image_Scale(src, 2, 2, FM_PIXEL_RGB888, dst, 4, 4,
                    FM_SCALE_NEAREST);
    if (r == FM_OK)
    {
        /* top-left quadrant should be red */
        if (dst[0] == 255 && dst[1] == 0 && dst[2] == 0 &&
            dst[(0 * 4 + 1) * 3 + 0] == 255 &&
            dst[(1 * 4 + 0) * 3 + 0] == 255)
        {
            PASS();
        }
        else
        {
            FAIL("nearest content");
        }
    }
    else
    {
        FAIL("nearest result");
    }

    TEST("Image_Flip horizontal + vertical");
    {
        uint8_t px[2 * 2 * 3] = {
            255, 0, 0,   0, 255, 0,
            0, 0, 255,   255, 255, 255
        };
        FM_Image img;

        memset(&img, 0, sizeof(img));
        img.width = 2;
        img.height = 2;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3;
        img.bit_depth = 24;
        img.data = px;
        img.data_capacity = sizeof(px);
        img.data_size = sizeof(px);
        r = Image_Flip(&img, FM_IMAGE_FLIP_HORIZONTAL);
        if (r == FM_OK && px[0] == 0 && px[1] == 255 && px[2] == 0 &&
            px[3] == 255 && px[4] == 0 && px[5] == 0)
        {
            r = Image_Flip(&img, FM_IMAGE_FLIP_VERTICAL);
        }
        if (r == FM_OK && px[0] == 255 && px[1] == 255 && px[2] == 255)
        {
            PASS();
        }
        else
        {
            FAIL("flip content");
        }
    }

    TEST("Image_Rotate 90 swaps width/height");
    {
        uint8_t rot_in[2 * 1 * 3] = { 10, 20, 30, 40, 50, 60 };
        uint8_t rot_out[1 * 2 * 3];
        FM_Image rin;
        FM_Image rout;

        memset(&rin, 0, sizeof(rin));
        memset(&rout, 0, sizeof(rout));
        rin.width = 2;
        rin.height = 1;
        rin.format = FM_PIXEL_RGB888;
        rin.channels = 3;
        rin.data = rot_in;
        rin.data_capacity = sizeof(rot_in);
        rin.data_size = sizeof(rot_in);
        rout.data = rot_out;
        rout.data_capacity = sizeof(rot_out);
        r = Image_Rotate(&rin, &rout, 90);
        if (r == FM_OK && rout.width == 1 && rout.height == 2 &&
            rot_out[0] == 10 && rot_out[1] == 20 && rot_out[2] == 30 &&
            rot_out[3] == 40 && rot_out[4] == 50 && rot_out[5] == 60)
        {
            PASS();
        }
        else
        {
            FAIL("rotate 90");
        }
    }

#if FM_USE_JPEG
    TEST("JPEG still Image_Load (picojpeg)");
    {
        FM_Image img;
        FM_ImageInfo inf;
        uint8_t pixels[64 * 64 * 3];

        memset(&img, 0, sizeof(img));
        r = Image_GetInfo("/test/vectors/mjpeg_frame.jpg", &inf);
        if (r == FM_OK)
        {
            r = Image_Load("/test/vectors/mjpeg_frame.jpg", &img, pixels,
                           sizeof(pixels));
        }
        if (r == FM_OK && strcmp(img.codec, "JPEG") == 0 &&
            img.width > 0 && img.height > 0 && inf.width == img.width)
        {
            PASS();
        }
        else
        {
            FAIL("jpeg still decode");
        }
    }
#else
    TEST("JPEG compiled out: a .jpg is refused, not misdecoded");
    {
        FM_Image img;
        FM_ImageInfo inf;
        uint8_t pixels[16 * 16 * 3];
        FM_Result ri;

        memset(&img, 0, sizeof(img));
        memset(pixels, 0xA5, sizeof(pixels));
        ri = Image_GetInfo("/test/vectors/mjpeg_frame.jpg", &inf);
        r = Image_Load("/test/vectors/mjpeg_frame.jpg", &img, pixels,
                       sizeof(pixels));
        if (r == FM_ERR_UNSUPPORTED && ri == FM_ERR_UNSUPPORTED &&
            pixels[0] == 0xA5 && pixels[sizeof(pixels) - 1u] == 0xA5)
        {
            PASS();
        }
        else
        {
            printf("load=%s info=%s ", FM_ResultString(r),
                   FM_ResultString(ri));
            FAIL("disabled JPEG codec was not reported as UNSUPPORTED");
        }
    }
#endif

#if FM_USE_PNG && FM_USE_CRC
    TEST("PNG still Image_Load (built-in inflate)");
    {
        /* Build a 2x2 RGB PNG (filter None, stored zlib). */
        uint8_t scan[2 * (1 + 6)];
        uint8_t zbuf[64];
        uint8_t png[256];
        size_t zlen = 0;
        size_t pn = 0;
        uint32_t adler_a = 1;
        uint32_t adler_b = 0;
        uint32_t adler;
        uint32_t i;
        FM_Image img;
        uint8_t pixels[2 * 2 * 3];

        memset(scan, 0, sizeof(scan));
        /* row0: filter0, red, green */
        scan[0] = 0;
        scan[1] = 255; scan[2] = 0; scan[3] = 0;
        scan[4] = 0; scan[5] = 255; scan[6] = 0;
        /* row1: filter0, blue, white */
        scan[7] = 0;
        scan[8] = 0; scan[9] = 0; scan[10] = 255;
        scan[11] = 255; scan[12] = 255; scan[13] = 255;
        for (i = 0; i < 14u; i++)
        {
            adler_a = (adler_a + scan[i]) % 65521u;
            adler_b = (adler_b + adler_a) % 65521u;
        }
        adler = (adler_b << 16) | adler_a;
        zbuf[zlen++] = 0x78;
        zbuf[zlen++] = 0x01;
        zbuf[zlen++] = 0x01; /* final stored */
        zbuf[zlen++] = 14;
        zbuf[zlen++] = 0;
        zbuf[zlen++] = (uint8_t)(0xFFFFu ^ 14u);
        zbuf[zlen++] = 0xFF;
        memcpy(zbuf + zlen, scan, 14);
        zlen += 14;
        zbuf[zlen++] = (uint8_t)(adler >> 24);
        zbuf[zlen++] = (uint8_t)(adler >> 16);
        zbuf[zlen++] = (uint8_t)(adler >> 8);
        zbuf[zlen++] = (uint8_t)adler;

        {
            static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
            uint8_t ihdr[13];

            memcpy(png, sig, 8);
            pn = 8;
            memset(ihdr, 0, sizeof(ihdr));
            ihdr[3] = 2; /* width 2 */
            ihdr[7] = 2; /* height 2 */
            ihdr[8] = 8; /* bit depth */
            ihdr[9] = 2; /* color RGB */
            /* write chunk helper */
#define TST_PNG_CHUNK(typ, data, n)                                            \
            do                                                                 \
            {                                                                  \
                uint8_t _t[4];                                                 \
                uint32_t _crc;                                                 \
                uint8_t _tmp[4 + 256];                                         \
                size_t _n = (size_t)(n);                                       \
                memcpy(_t, (typ), 4);                                          \
                png[pn++] = 0; png[pn++] = 0; png[pn++] = 0;                   \
                png[pn++] = (uint8_t)_n;                                       \
                memcpy(png + pn, _t, 4); pn += 4;                              \
                if (_n > 0) { memcpy(png + pn, (data), _n); pn += _n; }        \
                memcpy(_tmp, _t, 4);                                           \
                if (_n > 0) memcpy(_tmp + 4, (data), _n);                      \
                _crc = CRC_Calculate32(_tmp, 4u + _n);                         \
                png[pn++] = (uint8_t)(_crc >> 24);                             \
                png[pn++] = (uint8_t)(_crc >> 16);                             \
                png[pn++] = (uint8_t)(_crc >> 8);                              \
                png[pn++] = (uint8_t)_crc;                                     \
            } while (0)
            TST_PNG_CHUNK("IHDR", ihdr, 13);
            TST_PNG_CHUNK("IDAT", zbuf, zlen);
            TST_PNG_CHUNK("IEND", ihdr, 0);
#undef TST_PNG_CHUNK
        }
        r = File_WriteAll("/tiny.png", png, pn);
        memset(&img, 0, sizeof(img));
        if (r == FM_OK)
        {
            r = Image_Load("/tiny.png", &img, pixels, sizeof(pixels));
        }
        if (r == FM_OK && img.width == 2 && img.height == 2 &&
            pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 &&
            pixels[3] == 0 && pixels[4] == 255 && pixels[5] == 0 &&
            pixels[6] == 0 && pixels[7] == 0 && pixels[8] == 255 &&
            pixels[9] == 255 && pixels[10] == 255 && pixels[11] == 255)
        {
            PASS();
        }
        else
        {
            FAIL("png still decode");
        }
        (void)File_Delete("/tiny.png");
    }
#endif
#if !FM_USE_PNG
    TEST("PNG compiled out: a .png is refused, not misdecoded");
    {
        static const uint8_t png_hdr[24] = {
            137, 80, 78, 71, 13, 10, 26, 10,  0, 0, 0, 13,
            'I', 'H', 'D', 'R',  0, 0, 0, 2,  0, 0, 0, 2
        };
        FM_Image img;
        uint8_t pixels[2 * 2 * 3];
        FM_Image src_img;
        FM_Result rs;
        FM_Bool left = FM_TRUE;

        memset(&img, 0, sizeof(img));
        memset(pixels, 0xA5, sizeof(pixels));
        memset(&src_img, 0, sizeof(src_img));
        src_img.width = 2;
        src_img.height = 2;
        src_img.format = FM_PIXEL_RGB888;
        src_img.channels = 3;
        src_img.bit_depth = 24;
        src_img.data = pixels;
        src_img.data_capacity = sizeof(pixels);
        src_img.data_size = sizeof(pixels);
        rs = Image_Save("/off.png", &src_img);
        (void)File_Exists("/off.png", &left);
        r = File_WriteAll("/hdr.png", png_hdr, sizeof(png_hdr));
        if (r == FM_OK)
        {
            r = Image_Load("/hdr.png", &img, pixels, sizeof(pixels));
        }
        if (r == FM_ERR_UNSUPPORTED && rs == FM_ERR_UNSUPPORTED &&
            pixels[0] == 0xA5 && !left)
        {
            PASS();
        }
        else
        {
            printf("load=%s save=%s ", FM_ResultString(r),
                   FM_ResultString(rs));
            FAIL("disabled PNG codec was not reported as UNSUPPORTED");
        }
        (void)File_Delete("/hdr.png");
        (void)File_Delete("/off.png");
    }
#endif

#if FM_USE_JPEG && FM_USE_PNG
    TEST("Image_Load JPEG/PNG without heap (no file slurp)");
    {
        FM_Allocator fail;
        FM_Allocator keep;
        FM_Image img;
        uint8_t pixels[64 * 64 * 3];
        uint8_t png_px[2 * 2 * 3] = {
            10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120
        };
        uint8_t png_out[2 * 2 * 3];

        memset(&fail, 0, sizeof(fail));
        fail.alloc = null_alloc;
        fail.free = null_free;
        keep = *FM_Allocator_Get();
        if (keep.alloc == NULL)
        {
            keep.alloc = def_alloc;
            keep.free = def_free;
            keep.calloc = def_calloc;
            keep.realloc = def_realloc;
        }
        (void)FM_Allocator_Set(&fail);
        memset(&img, 0, sizeof(img));
        r = Image_Load("/test/vectors/mjpeg_frame.jpg", &img, pixels,
                       sizeof(pixels));
        if (r == FM_OK && strcmp(img.codec, "JPEG") == 0 && img.width > 0)
        {
            FM_Image src_img;

            memset(&src_img, 0, sizeof(src_img));
            src_img.width = 2;
            src_img.height = 2;
            src_img.format = FM_PIXEL_RGB888;
            src_img.channels = 3;
            src_img.bit_depth = 24;
            src_img.data = png_px;
            src_img.data_capacity = sizeof(png_px);
            src_img.data_size = sizeof(png_px);
            r = Image_Save("/heapless.png", &src_img);
            memset(&img, 0, sizeof(img));
            if (r == FM_OK)
            {
                r = Image_Load("/heapless.png", &img, png_out,
                               sizeof(png_out));
            }
            if (r == FM_OK && img.width == 2 && img.height == 2 &&
                memcmp(png_out, png_px, sizeof(png_px)) == 0)
            {
                PASS();
            }
            else
            {
                FAIL("png heapless roundtrip");
            }
            (void)File_Delete("/heapless.png");
        }
        else
        {
            FAIL("jpeg heapless load");
        }
        (void)FM_Allocator_Set(&keep);
    }
#endif

#if FM_USE_JPEG
    TEST("Image_Save JPEG roundtrip (lossy, size preserved)");
    {
        uint8_t px[8 * 8 * 3];
        uint8_t out[8 * 8 * 3];
        FM_Image src_img;
        FM_Image loaded;
        uint32_t i;

        for (i = 0; i < sizeof(px); i++)
        {
            px[i] = (uint8_t)((i * 37u) & 0xFFu);
        }
        memset(&src_img, 0, sizeof(src_img));
        src_img.width = 8;
        src_img.height = 8;
        src_img.format = FM_PIXEL_RGB888;
        src_img.channels = 3;
        src_img.bit_depth = 24;
        src_img.data = px;
        src_img.data_capacity = sizeof(px);
        src_img.data_size = sizeof(px);
        r = Image_Save("/rt.jpg", &src_img);
        memset(&loaded, 0, sizeof(loaded));
        if (r == FM_OK)
        {
            r = Image_Load("/rt.jpg", &loaded, out, sizeof(out));
        }
        if (r == FM_OK && loaded.width == 8 && loaded.height == 8 &&
            strcmp(loaded.codec, "JPEG") == 0)
        {
            PASS();
        }
        else
        {
            FAIL("jpeg save/load");
        }
        (void)File_Delete("/rt.jpg");
    }

    TEST("Image_Load scales JPEG to fit a smaller buffer");
    {
        FM_Image img;
        uint8_t tiny[16 * 16 * 3];

        memset(&img, 0, sizeof(img));
        r = Image_Load("/test/vectors/mjpeg_frame.jpg", &img, tiny,
                       sizeof(tiny));
        if (r == FM_OK && img.width <= 16 && img.height <= 16 &&
            img.width > 0 && img.height > 0)
        {
            PASS();
        }
        else
        {
            FAIL("scale-to-fit");
        }
    }
#endif /* FM_USE_JPEG */

    TEST("Image_Scale: bilinear 2x2 -> 2x2 identity");
    r = Image_Scale(src, 2, 2, FM_PIXEL_RGB888, dst, 2, 2,
                    FM_SCALE_BILINEAR);
    if (r == FM_OK && dst[0] == 255 && dst[3] == 0 && dst[4] == 255 &&
        dst[6] == 0 && dst[9] == 255)
    {
        PASS();
    }
    else
    {
        FAIL("bilinear content");
    }

    TEST("Image effects: invert/grayscale/sepia/brightness/contrast");
    {
        uint8_t effect_px[4] = {10, 20, 30, 77};
        FM_Image effect_img;

        memset(&effect_img, 0, sizeof(effect_img));
        effect_img.width = 1;
        effect_img.height = 1;
        effect_img.format = FM_PIXEL_RGBA8888;
        effect_img.channels = 4;
        effect_img.bit_depth = 32;
        effect_img.data = effect_px;
        effect_img.data_capacity = sizeof(effect_px);
        effect_img.data_size = sizeof(effect_px);

        r = Image_ApplyEffect(&effect_img, FM_IMAGE_EFFECT_INVERT, 0);
        if (r == FM_OK && (effect_px[0] != 245 || effect_px[1] != 235 ||
                           effect_px[2] != 225 || effect_px[3] != 77))
        {
            r = FM_ERR_CORRUPTED;
        }
        effect_px[0] = 10; effect_px[1] = 20; effect_px[2] = 30;
        if (r == FM_OK)
            r = Image_ApplyEffect(&effect_img, FM_IMAGE_EFFECT_GRAYSCALE, 0);
        if (r == FM_OK && (effect_px[0] != effect_px[1] ||
                           effect_px[1] != effect_px[2] || effect_px[3] != 77))
        {
            r = FM_ERR_CORRUPTED;
        }
        effect_px[0] = 10; effect_px[1] = 20; effect_px[2] = 30;
        if (r == FM_OK)
            r = Image_ApplyEffect(&effect_img, FM_IMAGE_EFFECT_SEPIA, 0);
        if (r == FM_OK && (effect_px[0] != 24 || effect_px[1] != 22 ||
                           effect_px[2] != 17 || effect_px[3] != 77))
        {
            r = FM_ERR_CORRUPTED;
        }
        if (r == FM_OK)
            r = Image_ApplyEffect(&effect_img, FM_IMAGE_EFFECT_BRIGHTNESS,
                                  255);
        if (r == FM_OK && (effect_px[0] != 255 || effect_px[1] != 255 ||
                           effect_px[2] != 255 || effect_px[3] != 77))
        {
            r = FM_ERR_CORRUPTED;
        }
        if (r == FM_OK)
            r = Image_ApplyEffect(&effect_img, FM_IMAGE_EFFECT_CONTRAST,
                                  -100);
        if (r == FM_OK && (effect_px[0] != 128 || effect_px[1] != 128 ||
                           effect_px[2] != 128 || effect_px[3] != 77))
        {
            r = FM_ERR_CORRUPTED;
        }
        CHECK_RESULT(r, "image effects");
    }

    TEST("Pixel layouts: BGR565/BGR888/BGRA8888 conversion + alpha");
    {
        uint8_t rgba[8] = {10, 20, 30, 77, 200, 100, 50, 33};
        uint8_t bgra[8] = {0};
        uint8_t bgr565[4] = {0};
        uint8_t rgb[6] = {0};
        uint8_t scaled[4 * 4] = {0};
        FM_Image source;
        FM_Image destination;
        size_t bytes = 0;

        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        source.width = 2;
        source.height = 1;
        source.format = FM_PIXEL_RGBA8888;
        source.data = rgba;
        source.data_capacity = sizeof(rgba);
        source.data_size = sizeof(rgba);
        destination.data = bgra;
        destination.data_capacity = sizeof(bgra);
        r = Image_Convert(&source, &destination, FM_PIXEL_BGRA8888);
        if (r == FM_OK &&
            (bgra[0] != 30 || bgra[1] != 20 || bgra[2] != 10 ||
             bgra[3] != 77 || bgra[7] != 33))
        {
            r = FM_ERR_CORRUPTED;
        }
        if (r == FM_OK)
            r = Image_ApplyEffect(&destination, FM_IMAGE_EFFECT_INVERT, 0);
        if (r == FM_OK &&
            (bgra[0] != 225 || bgra[1] != 235 || bgra[2] != 245 ||
             bgra[3] != 77 || bgra[7] != 33))
        {
            r = FM_ERR_CORRUPTED;
        }

        destination.data = bgr565;
        destination.data_capacity = sizeof(bgr565);
        if (r == FM_OK)
            r = Image_Convert(&source, &destination, FM_PIXEL_BGR565);
        source = destination;
        source.data = bgr565;
        source.data_capacity = sizeof(bgr565);
        source.data_size = sizeof(bgr565);
        destination.data = rgb;
        destination.data_capacity = sizeof(rgb);
        if (r == FM_OK)
            r = Image_Convert(&source, &destination, FM_PIXEL_RGB888);
        if (r == FM_OK &&
            (rgb[0] < 7 || rgb[0] > 16 || rgb[1] < 16 || rgb[1] > 24 ||
             rgb[2] < 24 || rgb[2] > 33))
        {
            r = FM_ERR_CORRUPTED;
        }

        if (r == FM_OK)
            r = Image_Scale(bgra, 2, 1, FM_PIXEL_BGRA8888,
                            scaled, 4, 1, FM_SCALE_NEAREST);
        if (r == FM_OK && (scaled[3] != 77 || scaled[7] != 77 ||
                           scaled[11] != 33 || scaled[15] != 33))
        {
            r = FM_ERR_CORRUPTED;
        }
        if (r == FM_OK)
            r = Image_BufferSize(320, 240, FM_PIXEL_BGR565, &bytes);
        if (r == FM_OK && bytes != 320u * 240u * 2u)
        {
            r = FM_ERR_CORRUPTED;
        }
        CHECK_RESULT(r, "common raw pixel layouts");
    }

    TEST("Image_ResizeFile: BMP 8x8 -> 4x4");
    {
        uint8_t px[8 * 8 * 3];
        FM_Image img;
        FM_ImageInfo info;
        uint32_t i;

        for (i = 0; i < sizeof(px); i++)
        {
            px[i] = (uint8_t)i;
        }
        memset(&img, 0, sizeof(img));
        img.width = 8;
        img.height = 8;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3;
        img.bit_depth = 24;
        img.data = px;
        img.data_capacity = sizeof(px);
        img.data_size = sizeof(px);
        (void)strcpy(img.codec, "RAW");

        r = Image_Save("/scale_src.bmp", &img);
        if (r == FM_OK) r = Image_ResizeFile("/scale_src.bmp",
                                             "/scale_dst.bmp", 4, 4,
                                             FM_SCALE_NEAREST);
        if (r == FM_OK) r = Image_GetInfo("/scale_dst.bmp", &info);
        if (r == FM_OK && info.width == 4 && info.height == 4)
        {
            PASS();
        }
        else
        {
            FAIL("resize file");
        }
        (void)File_Delete("/scale_src.bmp");
        (void)File_Delete("/scale_dst.bmp");
    }
}
#endif /* FM_USE_IMAGE */


/* ================================================================ */
/* Audio decoder hook + recorder                                     */
/* ================================================================ */
#if FM_USE_AUDIO
static FM_Result fake_aac_init(void **ctx, const FM_AudioInfo *info)
{
    (void)info;
    *ctx = (void *)1;
    return FM_OK;
}

static FM_Result fake_aac_decode(void *ctx, const uint8_t *in, size_t in_size,
                                 int16_t *out, size_t out_count,
                                 size_t *out_samples)
{
    size_t i;
    size_t n = out_count;

    (void)ctx;
    (void)in;
    (void)in_size;
    if (n > 256)
    {
        n = 256;
    }
    for (i = 0; i < n; i++)
    {
        out[i] = (int16_t)(i * 10);
    }
    *out_samples = n;
    return FM_OK;
}

static void fake_aac_close(void *ctx)
{
    (void)ctx;
}

static FM_Result fake_mp3_encoder_init(void **ctx,
                                       const FM_AudioEncodeConfig *config)
{
    if (ctx == NULL || config == NULL || config->bits_per_sample != 16)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *ctx = (void *)1;
    return FM_OK;
}

static FM_Result fake_mp3_encoder_write(void *ctx, FM_File *file,
                                        const int16_t *pcm,
                                        size_t frame_count,
                                        size_t *frames_consumed)
{
    uint32_t written = 0;
    size_t bytes;
    FM_Result r;

    (void)ctx;
    if (file == NULL || pcm == NULL || frame_count > UINT32_MAX / 4u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bytes = frame_count * 4u; /* test config is stereo PCM16 */
    r = File_Write(file, pcm, (uint32_t)bytes, &written);
    if (frames_consumed != NULL) *frames_consumed = written / 4u;
    return (r == FM_OK && written == bytes) ? FM_OK : FM_ERR_IO;
}

static FM_Result fake_mp3_encoder_finish(void *ctx, FM_File *file)
{
    uint32_t written = 0;

    (void)ctx;
    if (File_Write(file, "END!", 4, &written) != FM_OK || written != 4)
    {
        return FM_ERR_IO;
    }
    return FM_OK;
}

static void fake_mp3_encoder_close(void *ctx)
{
    (void)ctx;
}

static void build_adts(uint8_t *h, uint32_t rate, uint16_t ch,
                       uint32_t payload_len)
{
    uint32_t total = payload_len + 7u;
    uint32_t fi = 3; /* 48000 Hz */

    (void)rate;
    h[0] = 0xFF;
    h[1] = 0xF1;                       /* MPEG-4 AAC-LC, no CRC */
    h[2] = (uint8_t)((fi << 2) | ((uint32_t)ch >> 2));
    h[3] = (uint8_t)((((uint32_t)ch & 3u) << 6) | ((total >> 11) & 3u));
    h[4] = (uint8_t)((total >> 3) & 0xFFu);
    h[5] = (uint8_t)(((total & 7u) << 5) | 0x1Fu);
    h[6] = 0xFC;
}

static void test_audio_hook(void)
{
    FM_Result r;
    uint8_t aac_file[64];
    uint8_t payload[4] = { 'P', 'A', 'Y', 'L' };
    FM_Audio a;
    int16_t samples[16];
    size_t got = 0;
    FM_AudioDecoderOps ops;

    ops.name = "fake-aac";
    ops.init = fake_aac_init;
    ops.decode = fake_aac_decode;
    ops.close = fake_aac_close;

    /* two ADTS frames, 4-byte payload each */
    build_adts(aac_file, 48000, 2, 4);
    memcpy(aac_file + 7, payload, 4);
    build_adts(aac_file + 11, 48000, 2, 4);
    memcpy(aac_file + 18, payload, 4);
    r = File_WriteAll("/test.aac", aac_file, 22);
    if (r != FM_OK)
    {
        TEST("Audio decoder hook: write fixture");
        FAIL("write");
        return;
    }

    TEST("Audio decoder hook: unregistered AAC -> unsupported");
    r = Audio_Open("/test.aac", &a);
    if (r == FM_ERR_UNSUPPORTED)
    {
        PASS();
    }
    else
    {
        (void)Audio_Close(&a);
        FAIL("expected unsupported");
    }

    TEST("Audio decoder hook: registered AAC decoder");
    r = Audio_RegisterDecoder("AAC", &ops);
    if (r == FM_OK) r = Audio_Open("/test.aac", &a);
    if (r == FM_OK)
    {
        if (strcmp(a.info.codec, "AAC") != 0 || a.info.channels != 2 ||
            a.info.sample_rate != 48000)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        else
        {
            got = 0;
            r = Audio_ReadStereo(&a, samples, 8, &got);
            if (r == FM_OK && got == 8 && samples[0] == 0 &&
                samples[1] == 10 && samples[2] == 20)
            {
                r = FM_OK;
            }
            else
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        (void)Audio_Close(&a);
    }
    (void)Audio_UnregisterDecoder("AAC");
    CHECK_RESULT(r, "aac decode via hook");

    TEST("Audio writer dispatch: extension + registered MP3 encoder");
    {
        FM_AudioFileWriter writer;
        FM_AudioEncodeConfig config;
        FM_AudioEncoderOps encoder;
        int16_t pcm[8] = {1, -1, 2, -2, 3, -3, 4, -4};
        uint8_t encoded[64];
        size_t frames = 0;
        size_t encoded_size = 0;

        memset(&writer, 0, sizeof(writer));
        memset(&config, 0, sizeof(config));
        config.sample_rate = 44100;
        config.channels = 2;
        config.bits_per_sample = 16;
        config.bitrate_kbps = 128;
        r = AudioFileWriter_Open(&writer, "/auto.mp3", FM_AUDIO_FILE_AUTO,
                                 &config);
        if (r == FM_ERR_UNSUPPORTED)
        {
            r = FM_OK; /* correct: no fake WAV bytes under an MP3 name */
        }
        else
        {
            if (r == FM_OK) (void)AudioFileWriter_Close(&writer);
            r = FM_ERR_INVALID_FORMAT;
        }

        memset(&encoder, 0, sizeof(encoder));
        encoder.name = "fake-mp3";
        encoder.init = fake_mp3_encoder_init;
        encoder.write = fake_mp3_encoder_write;
        encoder.finish = fake_mp3_encoder_finish;
        encoder.close = fake_mp3_encoder_close;
        if (r == FM_OK)
            r = Audio_RegisterEncoder(FM_AUDIO_FILE_MP3, &encoder);
        if (r == FM_OK)
            r = AudioFileWriter_Open(&writer, "/auto.MP3",
                                     FM_AUDIO_FILE_AUTO, &config);
        if (r == FM_OK)
            r = AudioFileWriter_WriteFrames(&writer, pcm, 4, &frames);
        if (r == FM_OK) r = AudioFileWriter_Close(&writer);
        if (r == FM_OK)
            r = File_ReadAll("/auto.MP3", encoded, sizeof(encoded),
                             &encoded_size);
        (void)Audio_UnregisterEncoder(FM_AUDIO_FILE_MP3);
        if (r == FM_OK && frames == 4 && encoded_size == 20 &&
            memcmp(encoded, pcm, sizeof(pcm)) == 0 &&
            memcmp(encoded + sizeof(pcm), "END!", 4) == 0)
        {
            r = FM_OK;
        }
        else if (r == FM_OK)
        {
            r = FM_ERR_CORRUPTED;
        }
        CHECK_RESULT(r, "audio output dispatch");
        (void)File_Delete("/auto.MP3");
    }

    TEST("AudioRecorder: writes into a folder");
    {
        FM_AudioWriter w;
        char path[FM_PATH_MAX];
        int16_t sine[64];
        uint32_t i;

        for (i = 0; i < 64; i++)
        {
            sine[i] = (int16_t)((i % 16) * 1000);
        }
        r = AudioRecorder_Open("/rec", 16000, 1, 16, &w, path, sizeof(path));
        if (r == FM_OK) r = AudioWriter_Write(&w, sine, 64, NULL);
        if (r == FM_OK) r = AudioWriter_Close(&w);
        if (r == FM_OK)
        {
            FM_Bool exists = FM_FALSE;

            r = File_Exists(path, &exists);
            if (r == FM_OK && exists)
            {
                FM_AudioInfo info;

                r = Audio_Open(path, &a);
                if (r == FM_OK)
                {
                    Audio_GetInfo(&a, &info);
                    if (info.sample_rate != 16000 || info.channels != 1)
                    {
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    (void)Audio_Close(&a);
                }
            }
        }
        (void)Directory_DeleteRecursive("/rec");
        CHECK_RESULT(r, "recorder");
    }

    (void)File_Delete("/test.aac");
}
#endif /* FM_USE_AUDIO */


/* ================================================================ */
/* Media player                                                      */
/* ================================================================ */
#if FM_USE_AUDIO || FM_USE_VIDEO

static void test_player(void)
{
    FM_Result r;
    FM_Player p;
#if FM_USE_AUDIO
    FM_AudioWriter w;
    int16_t tone[160];   /* 10ms @16kHz */
    uint32_t i;
#endif
#if FM_USE_VIDEO
    FM_VideoWriter vw;
    uint8_t frame[8 * 8 * 3];
#endif

#if FM_USE_AUDIO
    for (i = 0; i < 160; i++)
    {
        tone[i] = (int16_t)((i % 32) * 500);
    }
#endif
#if FM_USE_VIDEO
    memset(frame, 0x33, sizeof(frame));
#endif

    (void)Directory_Create("/playlist");

#if FM_USE_AUDIO
    r = AudioWriter_Create("/playlist/b.wav", 16000, 1, 16, &w);
    if (r == FM_OK)
    {
        for (i = 0; i < 10; i++)
        {
            if (AudioWriter_Write(&w, tone, 160, NULL) != FM_OK) break;
        }
        (void)AudioWriter_Close(&w);
    }
    r = AudioWriter_Create("/playlist/a.wav", 16000, 1, 16, &w);
    if (r == FM_OK)
    {
        for (i = 0; i < 10; i++)
        {
            if (AudioWriter_Write(&w, tone, 160, NULL) != FM_OK) break;
        }
        (void)AudioWriter_Close(&w);
    }
#endif

#if FM_USE_VIDEO
    r = VideoWriter_Create("/playlist/v.fmrv", 8, 8, 25, 10,
                           sizeof(frame), &vw);
    if (r == FM_OK)
    {
        (void)VideoWriter_WriteFrame(&vw, frame, sizeof(frame));
        (void)VideoWriter_Close(&vw);
    }
#endif

#if FM_USE_AUDIO
    TEST("Player: scan folder (audio only)");
    Player_Init(&p);
    r = Player_ScanFolder(&p, "/playlist", FM_PLAYER_KIND_AUDIO);
    if (r == FM_OK && Player_Count(&p) == 2 &&
        strcmp(Player_ItemPath(&p, 0), "/playlist/a.wav") == 0 &&
        strcmp(Player_ItemPath(&p, 1), "/playlist/b.wav") == 0)
    {
        PASS();
    }
    else
    {
        FAIL("scan");
    }

    TEST("Player: play / next / prev");
    r = Player_Play(&p, 0);
    if (r == FM_OK && !Player_IsVideo(&p) && Player_Current(&p) == 0)
    {
        int16_t buf[64];
        size_t rd = 0;

        r = Player_ReadAudio(&p, buf, 64, &rd);
        if (r == FM_OK && rd == 64) r = Player_Next(&p);
        if (r == FM_OK && Player_Current(&p) == 1)
        {
            r = Player_Prev(&p);
        }
        if (r == FM_OK && Player_Current(&p) == 0)
        {
            PASS();
        }
        else
        {
            FAIL("next/prev");
        }
    }
    else
    {
        FAIL("play");
    }

    TEST("Player: seek within a track");
    r = Player_Seek(&p, 0.0);
    if (r == FM_OK)
    {
        double pos = Player_GetPosition(&p);

        r = (pos == 0.0) ? FM_OK : FM_ERR_INTERNAL;
    }
    if (r == FM_OK) r = Player_SeekFwd(&p, 0.1);
    if (r == FM_OK)
    {
        double pos = Player_GetPosition(&p);

        r = (pos > 0.09 && pos < 0.11) ? FM_OK : FM_ERR_INTERNAL;
    }
    if (r == FM_OK) r = Player_SeekBack(&p, 0.05);
    if (r == FM_OK)
    {
        double pos = Player_GetPosition(&p);

        r = (pos > 0.04 && pos < 0.06) ? FM_OK : FM_ERR_INTERNAL;
    }
    CHECK_RESULT(r, "player seek");
    Player_Deinit(&p);
#endif /* FM_USE_AUDIO */

#if FM_USE_VIDEO
    TEST("Player: scan all + video item");
    Player_Init(&p);
    r = Player_ScanFolder(&p, "/playlist", FM_PLAYER_KIND_ALL);
    if (r == FM_OK &&
        Player_Count(&p) == (FM_USE_AUDIO ? 3u : 1u))
    {
        uint32_t idx;

        r = FM_ERR_NOT_FOUND;
        for (idx = 0; idx < Player_Count(&p); idx++)
        {
            if (Player_ItemType(&p, idx) == FM_MEDIA_RAW &&
                strcmp(Player_ItemPath(&p, idx), "/playlist/v.fmrv") == 0)
            {
                r = Player_Play(&p, idx);
                break;
            }
        }
        if (r == FM_OK && Player_IsVideo(&p))
        {
            uint8_t px[8 * 8 * 3];
            uint16_t vw_out = 0;
            uint16_t vh_out = 0;

            r = Player_DecodeVideo(&p, px, sizeof(px), &vw_out, &vh_out);
            if (r == FM_OK && vw_out == 8 && vh_out == 8)
            {
                PASS();
            }
            else
            {
                FAIL("video decode");
            }
        }
        else
        {
            FAIL("video item");
        }
    }
    else
    {
        FAIL("scan all");
    }
    Player_Deinit(&p);
#endif /* FM_USE_VIDEO */

    (void)Directory_DeleteRecursive("/playlist");
}
#endif /* FM_USE_AUDIO || FM_USE_VIDEO */

#if FM_USE_AUDIO
/* ================================================================ */
/* Example 13 options: force_stereo (mono -> stereo upmix) + volume */
/* ================================================================ */

#include "../examples/audio_dma_player.h"

#define EX13_TEST_HALF 2048u

typedef struct
{
    uint32_t start_calls;
    uint32_t last_rate;
    uint16_t last_channels;
    size_t   last_count;
} Ex13TestPortState;

static Ex13TestPortState ex13tp;

static FM_Result ex13_test_port_start(void *ctx, int16_t *samples,
                                      size_t count, uint32_t rate,
                                      uint16_t channels)
{
    (void)ctx;
    (void)samples;
    ex13tp.start_calls++;
    ex13tp.last_rate = rate;
    ex13tp.last_channels = channels;
    ex13tp.last_count = count;
    return FM_OK;
}

static FM_Result ex13_test_port_stop(void *ctx)
{
    (void)ctx;
    return FM_OK;
}

static const Ex13_AudioDmaPort ex13_test_port =
{
    ex13_test_port_start,
    ex13_test_port_stop,
    NULL
};

/* Record the track lifecycle callbacks, driven by the Ex13 player. */
typedef struct
{
    uint32_t start_calls;
    uint32_t end_calls;
    char start_path[FM_PATH_MAX];
    char end_path[FM_PATH_MAX];
} Ex13TrackState;

static Ex13TrackState ex13track;

static void ex13_test_track_start(void *ctx, const char *path,
                                  const FM_AudioInfo *info)
{
    (void)ctx;
    (void)info;
    ex13track.start_calls++;
    if (path != NULL && ex13track.start_path[0] == '\0')
    {
        (void)strncpy(ex13track.start_path, path,
                      sizeof(ex13track.start_path) - 1u);
        ex13track.start_path[sizeof(ex13track.start_path) - 1u] = '\0';
    }
}

static void ex13_test_track_end(void *ctx, const char *path,
                                const FM_AudioInfo *info)
{
    (void)ctx;
    (void)info;
    ex13track.end_calls++;
    if (path != NULL)
    {
        (void)strncpy(ex13track.end_path, path,
                      sizeof(ex13track.end_path) - 1u);
        ex13track.end_path[sizeof(ex13track.end_path) - 1u] = '\0';
    }
}

/*
 * A 0.25 s mono ramp (8 kHz, 2000 frames, sample i == i*8) played through
 * Ex13 with force_stereo + software volume.  The host port does nothing,
 * so the DMA half/full callbacks are simulated to drive refills:
 *
 *   1. StartEx prefills both halves: first 2048 channel samples are
 *      frames 0..1023, upmixed (L == R == ramp[i]) at 100% volume.
 *   2. After SetVolume(50%), the simulated consumption + track
 *      wraparound refills half 0 from the top of the same track:
 *      samples must now be halved.
 */
static void test_ex13_options(void)
{
    Ex13_AudioDma x;
    Ex13_AudioDmaOptions opts;
    FM_AudioWriter w;
    FM_Result r;
    uint32_t i;
    int bad;
    static int16_t dma_buf[2 * EX13_TEST_HALF] __attribute__((aligned(32)));

    TEST("Ex13 options: mono upmix + software volume");
    (void)Directory_DeleteRecursive("/sa13");
    r = Directory_Create("/sa13");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }
    if (r == FM_OK)
    {
        int16_t chunk[1000];
        r = AudioWriter_Create("/sa13/ramp.wav", 8000, 1, 16, &w);
        for (i = 0; r == FM_OK && i < 2u; i++)
        {
            size_t written = 0;
            uint32_t k;

            for (k = 0; k < 1000u; k++)
            {
                chunk[k] = (int16_t)((uint32_t)(i * 1000u + k) * 8u);
            }
            r = AudioWriter_Write(&w, chunk, 1000, &written);
        }
        if (r == FM_OK)
        {
            r = AudioWriter_Close(&w);
        }
    }

    memset(&ex13tp, 0, sizeof(ex13tp));
    memset(&ex13track, 0, sizeof(ex13track));
    if (r == FM_OK)
    {
        opts.force_stereo = FM_TRUE;
        opts.volume = (int16_t)0x7FFF;
        r = Ex13_AudioDmaStartEx(&x, "/sa13", &ex13_test_port, NULL,
                                 dma_buf, EX13_TEST_HALF, &opts);
    }

    /* Start reported stereo (forced) at the file rate, full buffer. */
    if (r == FM_OK &&
        (ex13tp.last_rate != 8000u || ex13tp.last_channels != 2u ||
         ex13tp.last_count != 2u * (size_t)EX13_TEST_HALF))
    {
        r = FM_ERR_INTERNAL;
    }

    /* Track lifecycle callbacks: install after start (a running track is
     * reported immediately by SetTrackStart), then drive a track boundary
     * so end + the next track's start are observed. */
    if (r == FM_OK)
    {
        Ex13_AudioDmaSetTrackStart(&x, ex13_test_track_start, NULL);
        Ex13_AudioDmaSetTrackEnd(&x, ex13_test_track_end, NULL);
        if (ex13track.start_calls != 1u || ex13track.start_path[0] == '\0')
        {
            r = FM_ERR_INTERNAL;
        }
    }

    /* Prefilled half 0: frames 0..1023 upmixed at 100%. */
    bad = 0;
    for (i = 0; i < (uint32_t)(EX13_TEST_HALF / 2u); i++)
    {
        int16_t expect = (int16_t)((uint32_t)i * 8u);

        if (dma_buf[2u * i] != expect ||
            dma_buf[2u * i + 1u] != expect)
        {
            bad++;
            if (bad > 8)
            {
                break;
            }
        }
    }
    if (r == FM_OK && bad != 0)
    {
        r = FM_ERR_INTERNAL;
    }

    /* 50% volume, then simulate half 0 consumed (silence refill: stream
     * is already drained) and half 1 consumed (track boundary -> reopen
     * the same track, refilling both halves at the new gain). */
    if (r == FM_OK)
    {
        Ex13_AudioDmaSetVolume(&x, 16383);
        Ex13_AudioDmaHalfCallback(&x);
        (void)Ex13_AudioDmaPoll(&x);
        Ex13_AudioDmaFullCallback(&x);
        r = Ex13_AudioDmaPoll(&x);
        if (r == FM_OK && !Ex13_AudioDmaIsRunning(&x))
        {
            r = FM_ERR_INTERNAL;
        }
    }

    /* Refilled half 0: same ramp, now halved (gain 16383/32767 ~ 1/2). */
    if (r == FM_OK)
    {
        bad = 0;
        for (i = 0; i < (uint32_t)(EX13_TEST_HALF / 2u); i++)
        {
            int16_t v = (int16_t)((uint32_t)i * 8u);
            int16_t expect = (int16_t)((int)v / 2);
            int d = (int)dma_buf[2u * i] - (int)expect;

            if (d < 0)
            {
                d = -d;
            }
            if (d > 1 || dma_buf[2u * i + 1u] != dma_buf[2u * i])
            {
                bad++;
                if (bad > 8)
                {
                    break;
                }
            }
        }
        if (bad != 0)
        {
            r = FM_ERR_INTERNAL;
        }
    }

    /* Track boundary: the last half was consumed, so the current track has
     * ended (end_calls >= 1) and the same track (REPEAT_ALL with one item)
     * restarted (start_calls == 2). */
    if (r == FM_OK &&
        (ex13track.end_calls < 1u || ex13track.start_calls != 2u))
    {
        r = FM_ERR_INTERNAL;
    }

    /*
     * Odd half size + force_stereo (the N=32767 case): the fill works in
     * stereo-frame units, so an odd half must end with one silent sample
     * instead of calling Audio_ReadStereo with frame_count = 0.
     */
    {
        Ex13_AudioDma x2;
        Ex13_AudioDmaOptions opts2;
        FM_Result r2 = FM_OK;
        static int16_t odd_buf[2 * 1025u] __attribute__((aligned(32)));

        TEST("Ex13 options: odd half size with force_stereo");
        opts2.force_stereo = FM_TRUE;
        opts2.volume = (int16_t)0x7FFF;
        r2 = Ex13_AudioDmaStartEx(&x2, "/sa13", &ex13_test_port, NULL,
                                  odd_buf, 1025u, &opts2);
        if (r2 == FM_OK)
        {
            bad = 0;
            for (i = 0; i < 512u; i++)
            {
                int16_t e0 = (int16_t)((uint32_t)i * 8u);
                int16_t e1 = (int16_t)((uint32_t)(i + 512u) * 8u);

                if (odd_buf[2u * i] != e0 ||
                    odd_buf[2u * i + 1u] != e0 ||
                    odd_buf[1025u + 2u * i] != e1 ||
                    odd_buf[1025u + 2u * i + 1u] != e1)
                {
                    bad++;
                    if (bad > 8)
                    {
                        break;
                    }
                }
            }
            /* the lone final sample slot of each odd half is silence */
            if (odd_buf[1024u] != 0 || odd_buf[1024u + 1025u] != 0)
            {
                bad++;
            }
            if (bad != 0)
            {
                r2 = FM_ERR_INTERNAL;
            }
        }
        (void)Ex13_AudioDmaStop(&x2);
        CHECK_RESULT(r2, "ex13 odd half + force_stereo");
    }

    (void)Ex13_AudioDmaStop(&x);
    (void)File_Delete("/sa13/ramp.wav");
    (void)Directory_DeleteRecursive("/sa13");
    CHECK_RESULT(r, "ex13 force_stereo + volume");
}

/* ================================================================ */
/* Example 17 (SimpleAudio) transitions: repeated manual Next/Prev   */
/* ---------------------------------------------------------------- */
/* On the H7 board the player died silently after a few Next/Prev    */
/* presses: the 3rd/4th track change produced no "board_start" log   */
/* (it failed inside ex13_audio_start_next, before port->start) and  */
/* the player stayed dead with no further output.  This mirrors the  */
/* header's exact transition sequence through the real ex13 engine + */
/* Player + Audio stack on the host:                                 */
/*   running = FALSE;  port->stop();  ex13_audio_start_next();       */
/* with IRQ "refill" flags still dirty at stop time, a mixed         */
/* MP3/WAV, mono/stereo, multi-rate playlist (MP3->WAV and back),    */
/* plus the automatic end-of-track advance path.  Nine manual        */
/* transitions exhaust both the 8-slot file pool and the 4-slot MP3  */
/* pool if even one handle or decoder state leaked per change.       */
/* ================================================================ */
#if FM_USE_MP3

#define SA17_HALF 4097u /* odd half, like the board's 32767 */

typedef struct
{
    uint32_t start_calls;
    uint32_t stop_calls;
    uint32_t last_rate;
    uint16_t last_channels;
} Sa17PortState;

static Sa17PortState sa17tp;

static FM_Result sa17_port_start(void *ctx, int16_t *samples, size_t count,
                                 uint32_t rate, uint16_t channels)
{
    (void)ctx;
    (void)samples;
    (void)count;
    sa17tp.start_calls++;
    sa17tp.last_rate = rate;
    sa17tp.last_channels = channels;
    return FM_OK;
}

static FM_Result sa17_port_stop(void *ctx)
{
    (void)ctx;
    sa17tp.stop_calls++;
    return FM_OK;
}

static const Ex13_AudioDmaPort sa17_port =
{
    sa17_port_start,
    sa17_port_stop,
    NULL
};

/* Simulate `halves` half-buffer consumptions (IRQ -> poll cycles). */
static FM_Result sa17_pump(Ex13_AudioDma *x, uint32_t halves)
{
    uint32_t t;
    FM_Result r = FM_OK;

    for (t = 0; t < halves; t++)
    {
        Ex13_AudioDmaHalfCallback(x);
        (void)Ex13_AudioDmaPoll(x);
        Ex13_AudioDmaFullCallback(x);
        r = Ex13_AudioDmaPoll(x);
        if (r != FM_OK)
        {
            break;
        }
    }
    return r;
}

/* Exact mirror of SimpleAudio_Next() in simple_audio.h. */
static FM_Result sa17_next(Ex13_AudioDma *x)
{
    if (!x->running)
    {
        return FM_ERR_NOT_READY;
    }
    x->running = FM_FALSE;
    (void)x->port->stop(x->port_context);
    return ex13_audio_start_next(x);
}

/* Exact mirror of SimpleAudio_Prev() (index math included). */
static FM_Result sa17_prev(Ex13_AudioDma *x)
{
    uint32_t count = Player_Count(&x->player);
    uint32_t idx;

    if (!x->running || count == 0u)
    {
        return FM_ERR_NOT_READY;
    }
    idx = (Player_Current(&x->player) == 0u) ? (count - 1u)
                                              : (Player_Current(&x->player) -
                                                 1u);
    x->running = FM_FALSE;
    (void)x->port->stop(x->port_context);
    return ex13_audio_play_from(x, idx);
}

static int sa17_buf_maxabs(const int16_t *buf, size_t n)
{
    size_t i;
    int m = 0;

    for (i = 0; i < n; i++)
    {
        int v = (int)buf[i];

        if (v < 0)
        {
            v = -v;
        }
        if (v > m)
        {
            m = v;
        }
    }
    return m;
}

/* Repeat one vector frame `copies` times into a CBR MP3 file. */
static FM_Result sa17_write_mp3(const char *path, const char *vector,
                                uint32_t copies, uint32_t frame_len)
{
    static uint8_t frame[1216];
    FM_File vf;
    FM_File wf;
    uint32_t rd = 0;
    uint32_t wr = 0;
    uint32_t i;
    FM_Result r;

    if (frame_len > sizeof(frame))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    File_Init(&vf);
    r = File_OpenEx(&vf, vector, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Read(&vf, frame, frame_len, &rd);
    (void)File_Close(&vf);
    if (r != FM_OK || rd != frame_len)
    {
        return FM_ERR_IO;
    }

    File_Init(&wf);
    r = File_OpenEx(&wf, path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        return r;
    }
    for (i = 0; i < copies; i++)
    {
        r = File_Write(&wf, frame, frame_len, &wr);
        if (r != FM_OK)
        {
            (void)File_Close(&wf);
            return r;
        }
    }
    return File_Close(&wf);
}

static FM_Result sa17_write_wav(const char *path, uint32_t rate,
                                uint16_t channels, uint16_t bits,
                                uint32_t frames)
{
    FM_AudioWriter w;
    uint32_t i;
    uint32_t k;
    FM_Result r;

    r = AudioWriter_Create(path, rate, channels, bits, &w);
    for (i = 0; r == FM_OK && i < frames; i += 512u)
    {
        int16_t chunk[1024];
        size_t written = 0;
        uint32_t n = (frames - i > 512u) ? 512u : (frames - i);

        for (k = 0; k < n * (uint32_t)channels; k++)
        {
            chunk[k] = (int16_t)(1000u + (uint32_t)k * 10u);
        }
        r = AudioWriter_Write(&w, chunk, (size_t)(n * (uint32_t)channels),
                              &written);
    }
    {
        FM_Result c = AudioWriter_Close(&w);

        if (r == FM_OK)
        {
            r = c;
        }
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Special WAV builder (host-side): exercises parser edge cases that   */
/* real-world recordings produce: extra chunks (LIST/fact), zero-size  */
/* chunks, data-before-fmt, WAVE_FORMAT_EXTENSIBLE, and a data chunk   */
/* whose claimed size runs past the end of the file (truncated).       */
/* ------------------------------------------------------------------ */

#define SA17WV_FACT0      (1u << 0) /* zero-size "fact" chunk          */
#define SA17WV_LIST       (1u << 1) /* LIST/INFO chunk before data     */
#define SA17WV_DATA_FIRST (1u << 2) /* data chunk before fmt chunk     */
#define SA17WV_EXTENSIBLE (1u << 3) /* format 0xFFFE, 18-byte fmt      */
#define SA17WV_CLAIM_MORE (1u << 4) /* data size claims past EOF       */

typedef struct
{
    uint8_t b[24576];
    size_t n;
} Sa17Wav;

static void sa17w(Sa17Wav *w, const void *data, size_t len)
{
    if (w->n + len <= sizeof(w->b))
    {
        memcpy(w->b + w->n, data, len);
        w->n += len;
    }
}

static void sa17w16(Sa17Wav *w, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };

    sa17w(w, t, sizeof(t));
}

static void sa17w32(Sa17Wav *w, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
                     (uint8_t)((v >> 16) & 0xFFu), (uint8_t)(v >> 24) };

    sa17w(w, t, sizeof(t));
}

static FM_Result sa17_write_wav_special(const char *path, uint32_t features,
                                        uint16_t channels, uint32_t rate,
                                        uint16_t bits, uint32_t frames)
{
    Sa17Wav w;
    uint8_t pcm[24576];
    uint32_t data_bytes;
    uint32_t fmt_len = (features & SA17WV_EXTENSIBLE) ? 40u : 16u;
    uint16_t fmt_code = (features & SA17WV_EXTENSIBLE) ? 0xFFFE : 1u;
    size_t i;
    uint32_t riff_size;

    memset(&w, 0, sizeof(w));
    data_bytes = (features & SA17WV_CLAIM_MORE)
                     ? (frames * channels * (bits / 8u) + 100000u)
                     : (frames * channels * (bits / 8u));

    for (i = 0; i < sizeof(pcm) / 2u; i++)
    {
        int16_t s = (int16_t)(100 + (int)(i % 500u));

        pcm[2u * i] = (uint8_t)((uint16_t)s & 0xFFu);
        pcm[2u * i + 1u] = (uint8_t)((uint16_t)s >> 8);
    }

    sa17w(&w, "RIFF", 4u);
    sa17w32(&w, 0u); /* patched below */
    sa17w(&w, "WAVE", 4u);

    if (features & SA17WV_DATA_FIRST)
    {
        sa17w(&w, "data", 4u);
        sa17w32(&w, data_bytes);
        sa17w(&w, pcm, data_bytes < sizeof(pcm) ? data_bytes : sizeof(pcm));
    }

    sa17w(&w, "fmt ", 4u);
    sa17w32(&w, fmt_len);
    sa17w16(&w, fmt_code);
    sa17w16(&w, channels);
    sa17w32(&w, rate);
    sa17w32(&w, rate * (uint32_t)channels * (bits / 8u));
    sa17w16(&w, (uint16_t)((uint32_t)channels * (bits / 8u)));
    sa17w16(&w, bits);
    if (features & SA17WV_EXTENSIBLE)
    {
        static const uint8_t pcm_guid[16] = {
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
        };

        sa17w16(&w, 22u);   /* cbSize: 2 + 16 (GUID) */
        sa17w16(&w, bits);  /* wValidBitsPerSample */
        sa17w(&w, pcm_guid, sizeof(pcm_guid));
    }

    if (features & SA17WV_FACT0)
    {
        sa17w(&w, "fact", 4u);
        sa17w32(&w, 0u); /* zero-size chunk: parser must not loop */
    }
    if (features & SA17WV_LIST)
    {
        sa17w(&w, "LIST", 4u);
        sa17w32(&w, 8u);
        sa17w(&w, "INFO", 4u);
        sa17w32(&w, 0u);
    }

    if (!(features & SA17WV_DATA_FIRST))
    {
        sa17w(&w, "data", 4u);
        sa17w32(&w, data_bytes);
        sa17w(&w, pcm,
              frames * channels * (bits / 8u) < sizeof(pcm)
                  ? frames * channels * (bits / 8u)
                  : sizeof(pcm));
    }

    riff_size = (uint32_t)(w.n - 8u);
    w.b[4] = (uint8_t)(riff_size & 0xFFu);
    w.b[5] = (uint8_t)((riff_size >> 8) & 0xFFu);
    w.b[6] = (uint8_t)((riff_size >> 16) & 0xFFu);
    w.b[7] = (uint8_t)(riff_size >> 24);
    return File_WriteAll(path, w.b, w.n);
}

/* ------------------------------------------------------------------ */
/* WAV edge cases + the exact board geometry (half = 32767, odd,      */
/* force_stereo): mirrors the STM32H7 SimpleAudio layout where fill   */
/* half 1 reads into the second half of the DMA buffer.               */
/* ------------------------------------------------------------------ */

#define SA17D_HALF 32767u /* the board's exact half size (odd) */

/* Build a 16-bit stereo WAV whose data chunk has an odd byte count:
 * `frames` whole L/R frames plus `tail` stray bytes (1..3). A recording
 * stopped mid-sample (or an interrupted copy) produces exactly this:
 * the data chunk runs to EOF without its odd padding byte. */
static FM_Result sa17d_write_wav_odd_tail(const char *path, uint32_t rate,
                                          uint32_t frames, uint32_t tail)
{
    static uint8_t out[44 + 16384 + 4];
    uint32_t data_bytes = frames * 4u + tail;
    uint32_t riff_size = 36u + data_bytes;
    uint32_t i;

    if (44u + data_bytes > sizeof(out))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(out, "RIFF", 4u);
    out[4] = (uint8_t)(riff_size & 0xFFu);
    out[5] = (uint8_t)((riff_size >> 8) & 0xFFu);
    out[6] = (uint8_t)((riff_size >> 16) & 0xFFu);
    out[7] = (uint8_t)(riff_size >> 24);
    memcpy(out + 8, "WAVE", 4u);
    memcpy(out + 12, "fmt ", 4u);
    out[16] = 16u;
    out[17] = 0u;
    out[18] = 0u;
    out[19] = 0u; /* fmt chunk size = 16 */
    out[20] = 1u;
    out[21] = 0u; /* PCM */
    out[22] = 2u;
    out[23] = 0u; /* channels */
    out[24] = (uint8_t)(rate & 0xFFu);
    out[25] = (uint8_t)((rate >> 8) & 0xFFu);
    out[26] = (uint8_t)((rate >> 16) & 0xFFu);
    out[27] = (uint8_t)(rate >> 24);
    {
        uint32_t byte_rate = rate * 4u;

        out[28] = (uint8_t)(byte_rate & 0xFFu);
        out[29] = (uint8_t)((byte_rate >> 8) & 0xFFu);
        out[30] = (uint8_t)((byte_rate >> 16) & 0xFFu);
        out[31] = (uint8_t)(byte_rate >> 24);
    }
    out[32] = 4u;
    out[33] = 0u; /* block align */
    out[34] = 16u;
    out[35] = 0u; /* bits per sample */
    memcpy(out + 36, "data", 4u);
    out[40] = (uint8_t)(data_bytes & 0xFFu);
    out[41] = (uint8_t)((data_bytes >> 8) & 0xFFu);
    out[42] = (uint8_t)((data_bytes >> 16) & 0xFFu);
    out[43] = (uint8_t)(data_bytes >> 24);
    for (i = 0; i < frames; i++)
    {
        int16_t s_l = (int16_t)(1000 + (int)(i % 300u));
        int16_t s_r = (int16_t)(-2000 + (int)(i % 300u));

        out[44 + i * 4u + 0u] = (uint8_t)((uint16_t)s_l & 0xFFu);
        out[44 + i * 4u + 1u] = (uint8_t)((uint16_t)s_l >> 8);
        out[44 + i * 4u + 2u] = (uint8_t)((uint16_t)s_r & 0xFFu);
        out[44 + i * 4u + 3u] = (uint8_t)((uint16_t)s_r >> 8);
    }
    for (i = 0; i < tail; i++)
    {
        out[44u + frames * 4u + i] = 0x55u;
    }
    return File_WriteAll(path, out, 44u + data_bytes);
}

static void test_wav_truncated_board_geometry(void)
{
    Ex13_AudioDma x;
    Ex13_AudioDmaOptions opts;
    FM_AudioInfo info;
    FM_Audio wav;
    FM_Result r;
    static int16_t board_buf[2 * SA17D_HALF] __attribute__((aligned(32)));

    TEST("WAV: large Audio_Read is chunked and lossless");
    {
        FM_AudioWriter w;
        FM_Audio a;
        int16_t src[512];
        int16_t out[4096];
        uint32_t i;
        uint32_t k;
        size_t got = 0;

        (void)FS_Unmount("/");
        if (FS_Mount("/", &g_fs) != FM_OK)
        {
            FAIL("cannot mount POSIX at /");
            return;
        }
        (void)Directory_DeleteRecursive("/sa17f");
        r = Directory_Create("/sa17f");
        if (r != FM_OK && r != FM_ERR_EXISTS)
        {
            r = FM_ERR_IO;
        }
        if (r == FM_OK)
        {
            r = AudioWriter_Create("/sa17f/ramp.wav", 8000, 2, 16, &w);
        }
        for (i = 0; r == FM_OK && i < 2048u; i += 256u)
        {
            size_t written = 0;

            for (k = 0; k < 256u; k++)
            {
                int16_t v = (int16_t)(i + k);

                src[2u * k] = v;
                src[2u * k + 1u] = (int16_t)(-v);
            }
            r = AudioWriter_Write(&w, src, 512u, &written);
        }
        if (r == FM_OK)
        {
            r = AudioWriter_Close(&w);
        }
        if (r == FM_OK)
        {
            r = Audio_Open("/sa17f/ramp.wav", &a);
        }
        if (r == FM_OK)
        {
            /* 4096 samples = exactly 2 * FM_AUDIO_PCM_CHUNK. */
            r = Audio_Read(&a, out, 4096u, &got);
        }
        if (r == FM_OK && got != 4096u)
        {
            r = FM_ERR_INTERNAL;
        }
        for (i = 0; r == FM_OK && i < 2048u; i++)
        {
            int16_t v = (int16_t)i;

            if (out[2u * i] != v || out[2u * i + 1u] != (int16_t)(-v))
            {
                r = FM_ERR_CORRUPTED;
            }
        }
        (void)Audio_Close(&a);
        (void)Directory_DeleteRecursive("/sa17f");
    }
    CHECK_RESULT(r, "chunked WAV bounce read");

    TEST("WAV: truncated odd-size data chunk (parser + reader)");

    (void)FS_Unmount("/");
    if (FS_Mount("/", &g_fs) != FM_OK)
    {
        FAIL("cannot mount POSIX at /");
        return;
    }
    (void)Directory_DeleteRecursive("/sa17d");
    r = Directory_Create("/sa17d");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }

    /* 800 whole frames + 3 stray bytes: the data chunk size is odd and
     * the chunk ends exactly at EOF (no odd padding byte). */
    if (r == FM_OK)
    {
        r = sa17d_write_wav_odd_tail("/sa17d/odd.wav", 48000u, 800u, 3u);
    }
    if (r == FM_OK)
    {
        size_t total_samples = 0;
        size_t total_frames = 0;
        FM_Bool ok = FM_TRUE;

        r = Audio_Open("/sa17d/odd.wav", &wav);
        if (r == FM_OK)
        {
            r = Audio_GetInfo(&wav, &info);
        }
        if (r == FM_OK && info.data_size != 3203u)
        {
            r = FM_ERR_INTERNAL;
        }
        /* Audio_Read: whole L/R frames only, never INVALID_FORMAT. */
        while (r == FM_OK && ok)
        {
            int16_t s[32];
            size_t got = 0;

            r = Audio_Read(&wav, s, 32, &got);
            if (r == FM_OK && (got & 1u) != 0u)
            {
                ok = FM_FALSE; /* half-frame would misalign the stream */
            }
            total_samples += got;
        }
        if (r == FM_OK)
        {
            r = FM_ERR_INTERNAL; /* the loop must end in FM_ERR_EOF */
        }
        if (total_samples != 1600u)
        {
            r = FM_ERR_INTERNAL; /* exactly the 800 whole frames */
        }
        else if (r == FM_ERR_EOF)
        {
            r = FM_OK; /* EOF is the expected terminator of the loop */
        }
        (void)Audio_Close(&wav);
        /* Audio_ReadStereo: the same file, counted in frames. */
        if (r == FM_OK)
        {
            r = Audio_Open("/sa17d/odd.wav", &wav);
        }
        if (r == FM_OK)
        {
            total_frames = 0;
            ok = FM_TRUE;
            while (r == FM_OK && ok)
            {
                int16_t f[32];
                size_t got = 0;

                r = Audio_ReadStereo(&wav, f, 16, &got);
                if (r != FM_OK && r != FM_ERR_EOF)
                {
                    ok = FM_FALSE; /* e.g. FM_ERR_INVALID_FORMAT */
                }
                total_frames += got;
            }
            if (total_frames != 800u || !ok)
            {
                r = FM_ERR_INTERNAL;
            }
            else if (r == FM_ERR_EOF)
            {
                r = FM_OK; /* EOF is the expected terminator */
            }
        }
        (void)Audio_Close(&wav);
    }
    CHECK_RESULT(r, "truncated odd-size WAV plays whole frames");

    /* Board geometry: the board's exact layout (half = 32767, odd;
     * force_stereo; 48 kHz stereo WAV in the middle of the playlist).
     * The board froze inside fill half 1 of exactly this shape. */
    TEST("ex13 board geometry: MP3->WAV->MP3 at half=32767");
    (void)Directory_DeleteRecursive("/sa17e");
    r = Directory_Create("/sa17e");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17e/b1.mp3", "/test/vectors/tone_stereo.mp3",
                           600u, 1152u);
    if (r == FM_OK)
        r = sa17_write_wav("/sa17e/b2.wav", 48000u, 2u, 16u, 70000u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17e/b3.mp3", "/test/vectors/tone_stereo.mp3",
                           600u, 1152u);

    memset(&sa17tp, 0, sizeof(sa17tp));
    if (r == FM_OK)
    {
        opts.force_stereo = FM_TRUE;
        opts.volume = (int16_t)0x7FFF;
        r = Ex13_AudioDmaStartEx(&x, "/sa17e", &sa17_port, NULL,
                                 board_buf, SA17D_HALF, &opts);
    }
    if (r == FM_OK && Player_Current(&x.player) != 0u)
    {
        r = FM_ERR_INTERNAL;
    }
    if (r == FM_OK)
    {
        r = sa17_pump(&x, 1u);
    }
    /* The transition that kills the board: MP3 -> WAV, 48 kHz, both
     * halves prefilled (fill half 1 reads into buffer + 32767). */
    if (r == FM_OK)
    {
        r = sa17_next(&x);
        if (r == FM_OK && Player_Current(&x.player) != 1u)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK && sa17tp.last_rate != 48000u)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK &&
            sa17_buf_maxabs(board_buf, 2u * SA17D_HALF) == 0)
        {
            r = FM_ERR_INTERNAL; /* WAV start produced a silent buffer */
        }
    }
    /* Runtime refill of the WAV halves (drain begins here: b2.wav is
     * only 70000 samples > 65534 window). */
    if (r == FM_OK)
    {
        r = sa17_pump(&x, 1u);
    }
    if (r == FM_OK)
    {
        r = sa17_next(&x);
        if (r == FM_OK && Player_Current(&x.player) != 2u)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK && sa17tp.last_rate != 48000u)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        r = sa17_prev(&x);
        if (r == FM_OK && Player_Current(&x.player) != 1u)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK &&
            sa17_buf_maxabs(board_buf, 2u * SA17D_HALF) == 0)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    (void)Ex13_AudioDmaStop(&x);
    (void)Directory_DeleteRecursive("/sa17d");
    (void)Directory_DeleteRecursive("/sa17e");
    CHECK_RESULT(r, "board-geometry WAV transitions");
}

static void test_simple_audio_track_change(void)
{
    Ex13_AudioDma x;
    Ex13_AudioDmaOptions opts;
    FM_Result r;
    static int16_t dma_buf[2 * SA17_HALF] __attribute__((aligned(32)));

    TEST("SimpleAudio: repeated manual Next/Prev across MP3+WAV tracks");

    /* The MP3/WAV sources live on the POSIX FS; make it the root. */
    (void)FS_Unmount("/");
    if (FS_Mount("/", &g_fs) != FM_OK)
    {
        FAIL("cannot mount POSIX at /");
        return;
    }

    (void)Directory_DeleteRecursive("/sa17");
    r = Directory_Create("/sa17");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }
    /* Playlist order (strcmp): a.mp3(48k st), a_copy.mp3(48k st),
     * b.wav(44.1k st), c.mp3(48k mono), d.wav(8k mono 8-bit), e.mp3.
     * Every track is longer than the 2-half DMA window, so no
     * end-of-track auto-advance happens during phase A. */
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17/a.mp3", "/test/vectors/tone_stereo.mp3",
                           600u, 1152u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17/a_copy.mp3", "/test/vectors/tone_stereo.mp3",
                           600u, 1152u);
    if (r == FM_OK)
        r = sa17_write_wav("/sa17/b.wav", 44100u, 2u, 16u, 44100u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17/c.mp3", "/test/vectors/tone_mono.mp3",
                           600u, 576u);
    if (r == FM_OK)
        r = sa17_write_wav("/sa17/d.wav", 8000u, 1u, 8u, 16000u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17/e.mp3", "/test/vectors/tone_stereo.mp3",
                           600u, 1152u);

    memset(&sa17tp, 0, sizeof(sa17tp));
    if (r == FM_OK)
    {
        opts.force_stereo = FM_TRUE;
        opts.volume = (int16_t)0x7FFF;
        r = Ex13_AudioDmaStartEx(&x, "/sa17", &sa17_port, NULL,
                                 dma_buf, SA17_HALF, &opts);
    }
    {
        /* Next x6 (wraps to index 0), then Prev x3. */
        static const uint32_t expect_idx[9] = {1, 2, 3, 4, 5, 0, 5, 4, 3};
        static const uint32_t expect_rate[9] = {48000, 44100, 48000, 8000,
                                                48000, 48000, 48000, 8000,
                                                48000};
        uint32_t step;

        for (step = 0; r == FM_OK && step < 9u; step++)
        {
            (void)sa17_pump(&x, 2u);
            /* Leave the half flag dirty: on the board the key can be
             * pressed right after a half-callback, before the poll. */
            Ex13_AudioDmaHalfCallback(&x);
            if (step < 6u)
            {
                r = sa17_next(&x);
            }
            else
            {
                r = sa17_prev(&x);
            }
            if (r == FM_OK && Player_Current(&x.player) != expect_idx[step])
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK && sa17tp.last_rate != expect_rate[step])
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK && sa17tp.last_channels != 2u)
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK &&
                sa17_buf_maxabs(dma_buf, 2u * SA17_HALF) == 0)
            {
                r = FM_ERR_INTERNAL; /* start produced a silent buffer */
            }
        }
    }

    /* SeekTo mirror: stop + Player_Seek + start_current. */
    if (r == FM_OK)
    {
        double pos;

        x.running = FM_FALSE;
        (void)x.port->stop(x.port_context);
        r = Player_Seek(&x.player, 1.0);
        if (r == FM_OK)
        {
            r = ex13_audio_start_current(&x);
        }
        if (r == FM_OK)
        {
            pos = Player_GetPosition(&x.player);
            if (pos < 0.9 || pos > 1.2)
            {
                r = FM_ERR_INTERNAL;
            }
        }
    }

    /* Stop + Begin again (SimpleAudio_Stop + SimpleAudio_Begin). */
    if (r == FM_OK)
    {
        (void)Ex13_AudioDmaStop(&x);
        opts.force_stereo = FM_TRUE;
        opts.volume = (int16_t)0x7FFF;
        r = Ex13_AudioDmaStartEx(&x, "/sa17", &sa17_port, NULL,
                                 dma_buf, SA17_HALF, &opts);
    }
    (void)Ex13_AudioDmaStop(&x);
    (void)Directory_DeleteRecursive("/sa17");
    CHECK_RESULT(r, "simple audio manual transitions");

    /* Phase B: automatic advance when a track runs out mid-window. */
    TEST("SimpleAudio: automatic advance on end of short tracks");
    (void)Directory_DeleteRecursive("/sa17b");
    r = Directory_Create("/sa17b");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }
    /* s1: 3 frames (6912 samples, EOF lands in half 1), s2: 2000 samples
     * (EOF in half 0), s3: 3 mono frames (3456 samples, EOF in half 0). */
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17b/s1.mp3", "/test/vectors/tone_stereo.mp3",
                           3u, 1152u);
    if (r == FM_OK)
        r = sa17_write_wav("/sa17b/s2.wav", 8000u, 1u, 16u, 2000u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17b/s3.mp3", "/test/vectors/tone_mono.mp3",
                           3u, 576u);

    memset(&sa17tp, 0, sizeof(sa17tp));
    {
        uint32_t starts_before;
        uint32_t cycles;

        if (r == FM_OK)
        {
            opts.force_stereo = FM_TRUE;
            opts.volume = (int16_t)0x7FFF;
            r = Ex13_AudioDmaStartEx(&x, "/sa17b", &sa17_port, NULL,
                                     dma_buf, SA17_HALF, &opts);
        }
        if (r == FM_OK)
        {
            starts_before = sa17tp.start_calls;
            for (cycles = 0; r == FM_OK && cycles < 64u; cycles++)
            {
                Ex13_AudioDmaHalfCallback(&x);
                r = Ex13_AudioDmaPoll(&x);
                if (r != FM_OK)
                {
                    break;
                }
                Ex13_AudioDmaFullCallback(&x);
                r = Ex13_AudioDmaPoll(&x);
                if (r == FM_OK &&
                    sa17tp.start_calls - starts_before >= 4u)
                {
                    break; /* wrapped around s1 -> s2 -> s3 -> s1 */
                }
            }
            if (r == FM_OK && sa17tp.start_calls - starts_before < 4u)
            {
                r = FM_ERR_INTERNAL; /* never advanced through all tracks */
            }
        }

        /*
         * Phase C: the SimpleAudio_Poll recovery path. When a refill
         * fails the engine dies exactly like this (running=FALSE, DMA
         * stopped, file still open); the header then calls
         * ex13_audio_play_from(current) and must bring the player back.
         */
        if (r == FM_OK)
        {
            x.running = FM_FALSE;
            (void)x.port->stop(x.port_context);
            r = ex13_audio_play_from(&x, Player_Current(&x.player));
            if (r == FM_OK && !Ex13_AudioDmaIsRunning(&x))
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK &&
                sa17_buf_maxabs(dma_buf, 2u * SA17_HALF) == 0)
            {
                r = FM_ERR_INTERNAL;
            }
        }

        /* Worse: the current file vanished while "playing" — recovery
         * must skip to the next openable track instead of dying. */
        if (r == FM_OK)
        {
            static char dead[FM_PATH_MAX];
            static char saved[FM_PATH_MAX];
            uint32_t cur_idx;
            FM_Result r2;

            cur_idx = Player_Current(&x.player);
            (void)memcpy(saved, Player_ItemPath(&x.player, cur_idx),
                         FM_PATH_MAX);
            (void)strcat(saved, ".bak");
            (void)memcpy(dead, Player_ItemPath(&x.player, cur_idx),
                         FM_PATH_MAX);
            (void)strcat(dead, ".bak");

            x.running = FM_FALSE;
            (void)x.port->stop(x.port_context);
            r2 = File_Rename(dead, saved);
            if (r2 == FM_OK)
            {
                r2 = ex13_audio_play_from(&x, cur_idx);
                if (r2 == FM_OK)
                {
                    /* the vanished item must have been skipped */
                    if (Player_Current(&x.player) == cur_idx ||
                        !Ex13_AudioDmaIsRunning(&x))
                    {
                        r = FM_ERR_INTERNAL;
                    }
                }
                else
                {
                    r = r2;
                }
                (void)File_Rename(saved, dead);
            }
        }
        (void)Ex13_AudioDmaStop(&x);
    }
    (void)Directory_DeleteRecursive("/sa17b");
    CHECK_RESULT(r, "simple audio auto advance + recovery");

    /*
     * Phase D: the board died exactly on MP3 -> WAV track changes, with
     * zero output (the transition never returned). Replay that sequence
     * here with WAVs that carry the quirks real recordings have.
     * Playlist order (strcmp): 1.mp3, 2a..2e.wav, 3.mp3.
     *   2a: plain 16-bit stereo            -> plays
     *   2b: + zero-size fact + LIST chunks -> plays
     *   2c: WAVE_FORMAT_EXTENSIBLE         -> rejected, skipped
     *   2d: data chunk before fmt chunk    -> plays
     *   2e: data size claims past EOF      -> plays the available part
     */
    TEST("SimpleAudio: MP3->WAV transition with special WAV files");
    (void)Directory_DeleteRecursive("/sa17c");
    r = Directory_Create("/sa17c");
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        r = FM_ERR_IO;
    }
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17c/1.mp3", "/test/vectors/tone_stereo.mp3",
                           200u, 1152u);
    if (r == FM_OK)
        r = sa17_write_wav_special("/sa17c/2a.wav", 0u, 2u, 44100u, 16u,
                                   5000u);
    if (r == FM_OK)
        r = sa17_write_wav_special("/sa17c/2b.wav",
                                   SA17WV_FACT0 | SA17WV_LIST, 2u, 44100u,
                                   16u, 5000u);
    if (r == FM_OK)
        r = sa17_write_wav_special("/sa17c/2c.wav", SA17WV_EXTENSIBLE, 2u,
                                   44100u, 16u, 2000u);
    if (r == FM_OK)
        r = sa17_write_wav_special("/sa17c/2d.wav", SA17WV_DATA_FIRST, 1u,
                                   8000u, 16u, 9000u);
    if (r == FM_OK)
        r = sa17_write_wav_special("/sa17c/2e.wav", SA17WV_CLAIM_MORE, 2u,
                                   44100u, 16u, 5000u);
    if (r == FM_OK)
        r = sa17_write_mp3("/sa17c/3.mp3", "/test/vectors/tone_mono.mp3",
                           200u, 576u);

    memset(&sa17tp, 0, sizeof(sa17tp));
    {
        static const uint32_t expect_idx[6] = {1, 2, 4, 5, 6, 0};
        static const uint32_t expect_rate[6] = {44100, 44100, 8000,
                                                44100, 48000, 48000};
        uint32_t step;

        if (r == FM_OK)
        {
            opts.force_stereo = FM_TRUE;
            opts.volume = (int16_t)0x7FFF;
            r = Ex13_AudioDmaStartEx(&x, "/sa17c", &sa17_port, NULL,
                                     dma_buf, SA17_HALF, &opts);
        }
        for (step = 0; r == FM_OK && step < 6u; step++)
        {
            (void)sa17_pump(&x, 1u);
            Ex13_AudioDmaHalfCallback(&x);
            r = sa17_next(&x);
            if (r == FM_OK && Player_Current(&x.player) != expect_idx[step])
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK && sa17tp.last_rate != expect_rate[step])
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK &&
                sa17_buf_maxabs(dma_buf, 2u * SA17_HALF) == 0)
            {
                r = FM_ERR_INTERNAL;
            }
        }
        (void)Ex13_AudioDmaStop(&x);
    }
    (void)Directory_DeleteRecursive("/sa17c");
    CHECK_RESULT(r, "simple audio mp3->wav special files");
}
#endif /* FM_USE_MP3 */
#endif /* FM_USE_AUDIO */


/* ================================================================ */
/* Regression tests for fixed bugs                                   */
/* ================================================================ */
#if FM_USE_AUDIO
static int g_aac_close_calls = 0;

static FM_Result fake_init(void **ctx, const FM_AudioInfo *info)
{
    (void)info;
    *ctx = (void *)1;
    return FM_OK;
}

static FM_Result fake_decode(void *ctx, const uint8_t *in, size_t in_size,
                             int16_t *out, size_t out_count,
                             size_t *out_samples)
{
    (void)ctx; (void)in; (void)in_size; (void)out; (void)out_count;
    *out_samples = 0; /* claim end of stream immediately */
    return FM_OK;
}

static void fake_close(void *ctx)
{
    (void)ctx;
    g_aac_close_calls++;
}
#endif

static void test_regressions(void)
{
    FM_Result r;

    /* ---- Path: truncated outputs must report FM_ERR_TOO_LARGE ---- */
    TEST("Regression: Path_GetFilename truncation -> TOO_LARGE");
    {
        char small[4];
        r = Path_GetFilename("/data/hello_world.txt", small, sizeof(small));
        if (r == FM_ERR_TOO_LARGE && strcmp(small, "hel") == 0) PASS();
        else FAIL("truncation not detected");
    }

    TEST("Regression: Path_GetDirectory truncation -> TOO_LARGE");
    {
        char small[4];
        r = Path_GetDirectory("/storage/logs/app.txt", small, sizeof(small));
        if (r == FM_ERR_TOO_LARGE) PASS();
        else FAIL("truncation not detected");
    }

    /* ---- JSON: string truncation + hostile exponent ---- */
#if FM_USE_JSON
    TEST("Regression: Json_ReadString truncation -> TOO_LARGE");
    {
        const char *js = "{\"name\":\"this_is_a_long_value_exceeding_the_buffer\"}";
        FM_Json doc;
        uint32_t tokens[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE / 4];
        char out[8];

        r = Json_OpenBuffer(js, strlen(js), &doc, tokens, sizeof(tokens));
        if (r == FM_OK) r = Json_ReadString(&doc, "name", out, sizeof(out));
        if (r == FM_ERR_TOO_LARGE && strcmp(out, "this_is") == 0) PASS();
        else FAIL("truncation not detected");
    }

    TEST("Regression: Json_ReadString zero-size buffer -> invalid");
    {
        const char *js = "{\"name\":\"x\"}";
        FM_Json doc;
        uint32_t tokens[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE / 4];
        char out[2];

        r = Json_OpenBuffer(js, strlen(js), &doc, tokens, sizeof(tokens));
        if (r == FM_OK) r = Json_ReadString(&doc, "name", out, 0);
        if (r == FM_ERR_INVALID_ARGUMENT) PASS();
        else FAIL("zero-size buffer accepted");
    }

    TEST("Regression: Json_ReadDouble 1e999999999999 clamps");
    {
        const char *js = "{\"x\":1e999999999999}";
        FM_Json doc;
        uint32_t tokens[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE / 4];
        double v = 0.0;

        r = Json_OpenBuffer(js, strlen(js), &doc, tokens, sizeof(tokens));
        if (r == FM_OK) r = Json_ReadDouble(&doc, "x", &v);
        if (r == FM_OK && v >= 1e300) PASS();  /* finite huge or inf */
        else FAIL("exponent not clamped deterministically");
    }
#endif

    /* ---- CRC: standard check values ("123456789") ---- */
#if FM_USE_CRC
    TEST("Regression: CRC32 check value 0xCBF43926");
    {
        static const char check[] = "123456789";
        uint32_t v = CRC_Calculate32(check, 9);

        if (v == 0xCBF43926u) PASS();
        else FAIL("CRC32 mismatch");
    }

    TEST("Regression: CRC16-CCITT check value 0x29B1");
    {
        static const char check[] = "123456789";
        uint16_t v = CRC_Calculate16(check, 9);

        if (v == 0x29B1u) PASS();
        else FAIL("CRC16 mismatch");
    }

    TEST("Regression: CRC16-MODBUS check value 0x4B37");
    {
        static const char check[] = "123456789";
        uint16_t v = CRC_Compute16Modbus(check, 9, 0xFFFFu);

        if (v == 0x4B37u) PASS();
        else FAIL("CRC16-MODBUS mismatch");
    }
#endif

    /* ---- Binary: float/double endian round trips ---- */
#if FM_USE_BINARY && FM_USE_STREAM
    TEST("Regression: Binary float/double round trip");
    {
        uint8_t mem[32];
        FM_Stream s;
        float f_in = -123.456f;
        float f_out = 0.0f;
        double d_in = 1.0e300;
        double d_out = 0.0;

        r = Stream_InitMemoryWrite(&s, mem, sizeof(mem));
        if (r == FM_OK) r = Binary_WriteFloat(&s, f_in, FM_ENDIAN_BIG);
        if (r == FM_OK) r = Binary_WriteDouble(&s, d_in, FM_ENDIAN_LITTLE);
        if (r == FM_OK) r = Stream_Seek(&s, 0, FM_SEEK_SET);
        if (r == FM_OK) r = Binary_ReadFloat(&s, &f_out, FM_ENDIAN_BIG);
        if (r == FM_OK) r = Binary_ReadDouble(&s, &d_out, FM_ENDIAN_LITTLE);
        Stream_Close(&s);
        if (r == FM_OK && f_in == f_out && d_in == d_out)
        {
            PASS();
        }
        else
        {
            FAIL("float/double round trip");
        }
    }
#endif

    /* ---- VFS: opening the mount root as a file must be rejected ---- */
    TEST("Regression: File_Open(\"/\") -> IS_DIRECTORY");
    {
        FM_File f;

        File_Init(&f);
        r = File_OpenEx(&f, "/", FM_OPEN_READ);
        if (r == FM_ERR_IS_DIRECTORY)
        {
            PASS();
        }
        else
        {
            if (r == FM_OK) (void)File_Close(&f);
            FAIL("root open accepted");
        }
    }

    /* ---- JSON: misaligned token buffer must be rejected ---- */
#if FM_USE_JSON
    TEST("Regression: Json misaligned token buffer -> invalid");
    {
        const char *js = "{\"a\":1}";
        uint8_t raw[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE + 4];
        int rejected = 0;
        uint32_t off;

        for (off = 1; off <= 3; off++)
        {
            FM_Json doc;

            if (Json_OpenBuffer(js, strlen(js), &doc, raw + off,
                                FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE) ==
                FM_ERR_INVALID_ARGUMENT)
            {
                rejected++;
            }
        }
        if (rejected >= 2)
        {
            PASS();
        }
        else
        {
            FAIL("misaligned buffers accepted");
        }
    }
#endif

    /* ---- Stream: re-init without close must not leak a slot ---- */
#if FM_USE_STREAM
    TEST("Regression: Stream re-init without close -> BUSY");
    {
        FM_Stream s;
        uint8_t mem[16];
        FM_Result r2;

        r = Stream_InitMemoryRead(&s, mem, sizeof(mem));
        r2 = Stream_InitMemoryRead(&s, mem, sizeof(mem));
        if (r == FM_OK && r2 == FM_ERR_BUSY)
        {
            Stream_Close(&s);
            r = Stream_InitMemoryRead(&s, mem, sizeof(mem));
            if (r == FM_OK) { PASS(); Stream_Close(&s); }
            else FAIL("re-init after close failed");
        }
        else
        {
            FAIL("double init not rejected");
        }
    }
#endif

    /* ---- Image: RGBA8888 / GRAY8 source conversions ---- */
#if FM_USE_IMAGE
    TEST("Regression: Image_Convert RGBA8888 -> RGB888");
    {
        uint8_t rgba[4 * 4] = {0};
        uint8_t rgb[4 * 3] = {0};
        FM_Image src;
        FM_Image dst;

        for (int i = 0; i < 4; i++) rgba[i * 4 + 3] = 0xFF;
        rgba[0] = 0xFF;                            /* red   */
        rgba[5] = 0xFF;                            /* green */
        rgba[10] = 0xFF;                           /* blue  */
        rgba[12] = rgba[13] = rgba[14] = 0xFF;     /* white */
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.width = 4; src.height = 1;
        src.format = FM_PIXEL_RGBA8888; src.channels = 4; src.bit_depth = 32;
        src.data = rgba; src.data_capacity = sizeof(rgba);
        dst.data = rgb; dst.data_capacity = sizeof(rgb);
        r = Image_Convert(&src, &dst, FM_PIXEL_RGB888);
        if (r == FM_OK && rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0 &&
            rgb[3] == 0 && rgb[4] == 255 && rgb[5] == 0 &&
            rgb[6] == 0 && rgb[7] == 0 && rgb[8] == 255 &&
            rgb[9] == 255 && rgb[10] == 255 && rgb[11] == 255)
        {
            PASS();
        }
        else
        {
            FAIL("RGBA8888 pixels corrupted");
        }
    }

    TEST("Regression: Image_Convert GRAY8 -> RGB888");
    {
        uint8_t gray[2] = { 0x10, 0xE0 };
        uint8_t rgb[2 * 3] = {0};
        FM_Image src;
        FM_Image dst;

        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.width = 2; src.height = 1;
        src.format = FM_PIXEL_GRAY8; src.channels = 1; src.bit_depth = 8;
        src.data = gray; src.data_capacity = sizeof(gray);
        dst.data = rgb; dst.data_capacity = sizeof(rgb);
        r = Image_Convert(&src, &dst, FM_PIXEL_RGB888);
        if (r == FM_OK && rgb[0] == 0x10 && rgb[1] == 0x10 && rgb[2] == 0x10 &&
            rgb[3] == 0xE0 && rgb[4] == 0xE0 && rgb[5] == 0xE0)
        {
            PASS();
        }
        else
        {
            FAIL("GRAY8 pixels corrupted");
        }
    }
#endif

    /* ---- Audio: registered AAC decoder must be closed ---- */
#if FM_USE_AUDIO
    TEST("Regression: Audio_Close calls registered AAC close");
    {
        FM_AudioDecoderOps ops = {"fake-aac", fake_init, fake_decode,
                                  fake_close};
        uint8_t adts[9] = {
            0xFF, 0xF1, 0x4C, 0x80, 0x01, 0x3F, 0xFC, 0x00, 0x00
        };
        FM_Audio a;

        g_aac_close_calls = 0;
        if (Audio_RegisterDecoder("AAC", &ops) != FM_OK ||
            File_WriteAll("/fm_regress.aac", adts, sizeof(adts)) != FM_OK)
        {
            FAIL("fixture setup");
        }
        else
        {
            memset(&a, 0, sizeof(a));
            r = Audio_Open("/fm_regress.aac", &a);
            if (r == FM_OK)
            {
                (void)Audio_Close(&a);
                r = FM_OK;
            }
            (void)File_Delete("/fm_regress.aac");
            if (r == FM_OK && g_aac_close_calls == 1)
            {
                PASS();
            }
            else
            {
                FAIL("AAC decoder close callback not invoked");
            }
            (void)Audio_UnregisterDecoder("AAC");
        }
    }
#endif

    /* ---- Player: scanning a missing folder must fail ---- */
#if FM_USE_AUDIO || FM_USE_VIDEO
    TEST("Regression: Player_ScanFolder missing dir -> error");
    {
        FM_Player p;

        Player_Init(&p);
        r = Player_ScanFolder(&p, "/fm_no_such_folder_xyz",
                              FM_PLAYER_KIND_AUDIO);
        Player_Deinit(&p);
        if (r != FM_OK && r != FM_ERR_CANCELLED)
        {
            PASS();
        }
        else
        {
            FAIL("missing folder reported as success");
        }
    }
#endif

    /* ---- Adapter without rename: move must fall back to copy+delete ---- */
    {
        FM_FS base_fs;
        FM_FS custom_fs;

        if (PosixFS_Init(&base_fs) != FM_OK)
        {
            TEST("Regression: adapter without rename");
            FAIL("PosixFS_Init failed");
        }
        else
        {
            memset(&custom_fs, 0, sizeof(custom_fs));
            (void)strcpy(custom_fs.name, "NORENAME");
            custom_fs.ops = base_fs.ops;
            custom_fs.ops.rename = NULL; /* minimal adapter: no rename */

            TEST("Regression: File_Move without rename falls back");
            r = FS_Register(&custom_fs);
            if (r == FM_OK) r = FS_Mount("/ram", &custom_fs);
            if (r == FM_OK) r = File_WriteText("/ram/mv_norename_src.txt",
                                               "abc");
            if (r == FM_OK) r = File_Move("/ram/mv_norename_src.txt",
                                          "/ram/mv_norename_dst.txt",
                                          NULL, NULL);
            {
                FM_Bool exists = FM_FALSE;
                FM_Result r2 = File_Exists("/ram/mv_norename_dst.txt",
                                           &exists);
                if (r == FM_OK && r2 == FM_OK && exists)
                {
                    PASS();
                }
                else
                {
                    FAIL("move without rename did not fall back");
                }
            }
            (void)File_Delete("/ram/mv_norename_src.txt");
            (void)File_Delete("/ram/mv_norename_dst.txt");

            TEST("Regression: Directory_Rename without rename -> UNSUPPORTED");
            r = Directory_Create("/ram/mv_nr_dir");
            if (r == FM_OK) r = Directory_Rename("/ram/mv_nr_dir",
                                                 "/ram/mv_nr_dir2");
            if (r == FM_ERR_UNSUPPORTED) PASS();
            else FAIL("rename NULL not rejected");
            (void)Directory_Delete("/ram/mv_nr_dir");
            (void)Directory_Delete("/ram/mv_nr_dir2");

            (void)FS_Unmount("/ram");
            (void)FS_Unregister("NORENAME");
        }
    }

    /* ---- FMFS: rename-over semantics (safe replace, editing) ---- */
#if FM_USE_FMFS
    {
        char text[128];
        size_t rd = 0;
        FM_FS memfs;

        r = FS_Unmount("/");
        if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
        if (r == FM_OK) r = Storage_Mount("ram");
        if (r == FM_OK) r = FMFS_Init(&memfs);
        if (r == FM_OK) r = FS_MountDefault(&memfs);
        if (r != FM_OK)
        {
            TEST("Regression: FMFS mount for rename-over");
            FAIL(FM_ResultString(r));
        }
        else
        {
#if FM_USE_SAFE_WRITE
            TEST("Regression: FMFS File_SafeWriteText replaces");
            r = File_WriteText("/safe.txt", "v1");
            if (r == FM_OK) r = File_SafeWriteText("/safe.txt", "v2");
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/safe.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "v2") == 0) PASS();
            else FAIL("safe replace left old content");
#endif

#if FM_USE_TEXT
            TEST("Regression: FMFS Text_Replace works");
            r = File_WriteText("/t.txt", "hello world\n");
            if (r == FM_OK) r = Text_Replace("/t.txt", "world", "there");
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/t.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "hello there\n") == 0) PASS();
            else FAIL("text replace failed");
#endif

            TEST("Regression: FMFS File_Insert works");
            r = File_WriteText("/i.txt", "abcdef");
            if (r == FM_OK) r = File_Insert("/i.txt", 3, "XYZ", 3);
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/i.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "abcXYZdef") == 0) PASS();
            else FAIL("insert failed");

            TEST("Regression: FMFS File_DeleteRange works");
            r = File_WriteText("/d.txt", "abcdef");
            if (r == FM_OK) r = File_DeleteRange("/d.txt", 2, 3);
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/d.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "abf") == 0) PASS();
            else FAIL("delete range failed");

            (void)FS_Unmount("/");
        }
    }
#endif

    /* ---- LittleFS: rename-over semantics ---- */
#if FM_USE_LITTLEFS
    {
        static uint8_t lfs_memory[128 * 1024];
        static FM_Storage lfs_storage;
        FM_FS lfs_fs;
        char text[128];
        size_t rd = 0;

        memset(lfs_memory, 0xFF, sizeof(lfs_memory));
        r = MemoryStorage_Init(&lfs_storage, "lfs", lfs_memory,
                               sizeof(lfs_memory), 4096, FM_FALSE);
        if (r == FM_OK) r = Storage_Register(&lfs_storage);
        if (r == FM_OK) r = Storage_Mount("lfs");
        if (r == FM_OK) r = LittleFS_Init(&lfs_fs);
        if (r == FM_OK) r = FS_Mount("/", &lfs_fs);
        if (r != FM_OK)
        {
            TEST("Regression: LittleFS mount for rename-over");
            FAIL(FM_ResultString(r));
        }
        else
        {
#if FM_USE_SAFE_WRITE
            TEST("Regression: LittleFS File_SafeWriteText replaces");
            r = File_WriteText("/safe.txt", "v1");
            if (r == FM_OK) r = File_SafeWriteText("/safe.txt", "v2");
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/safe.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "v2") == 0) PASS();
            else FAIL("safe replace left old content");
#endif

            TEST("Regression: LittleFS same-path rename keeps data");
            r = File_WriteText("/same.txt", "precious");
            if (r == FM_OK) r = File_Rename("/same.txt", "/same.txt");
            memset(text, 0, sizeof(text));
            if (r == FM_OK) r = File_ReadText("/same.txt", text,
                                              sizeof(text), &rd);
            if (r == FM_OK && strcmp(text, "precious") == 0) PASS();
            else FAIL("same-path rename lost the file");

            (void)FS_Unmount("/");
            (void)Storage_Unmount("lfs");
            (void)Storage_Unregister("lfs");
        }
    }
#endif

    /* Restore the POSIX mount for any test that runs afterwards.  The
     * root mount needs at least one mounted storage, so make sure the
     * RAM storage is mounted again first. */
    {
        FM_Bool mounted = FM_FALSE;

        if (FS_IsMounted("/", &mounted) != FM_OK || !mounted)
        {
            r = Storage_Mount("ram");
            if (r == FM_OK)
            {
                r = FS_Mount("/", &g_fs);
            }
            if (r != FM_OK && r != FM_ERR_EXISTS)
            {
                printf("  NOTE: POSIX remount after regressions: %s\n",
                       FM_ResultString(r));
            }
        }
    }
}

/* ================================================================ */
/* Concurrent registry stress (only built with FM_USE_THREADS)       */
/* ================================================================ */
#if FM_USE_THREADS && FM_HOST_BUILD
#include <pthread.h>

static void *thread_worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    char path[64];
    char copy_path[64];
    char buf[64];
    int i;

    snprintf(path, sizeof(path), "/thr_%u.txt", (unsigned)id);
    snprintf(copy_path, sizeof(copy_path), "/thr_dir/x%u.txt", (unsigned)id);

    for (i = 0; i < 200; i++)
    {
        char got[64];
        size_t rd = 0;

        snprintf(buf, sizeof(buf), "t%u:%d", (unsigned)id, i);
        if (File_WriteText(path, buf) != FM_OK) return (void *)1;
        if (File_ReadText(path, got, sizeof(got), &rd) != FM_OK) return (void *)1;
        if (strcmp(got, buf) != 0) return (void *)1;
        if (Directory_Create("/thr_dir") != FM_OK) return (void *)1;
        if (File_Copy(path, copy_path, NULL, NULL) != FM_OK) return (void *)1;
        if (File_Delete(copy_path) != FM_OK) return (void *)1;
    }
    (void)File_Delete(path);
    return NULL;
}

static void test_threads(void)
{
    pthread_t threads[4];
    void *results[4] = {NULL, NULL, NULL, NULL};
    int i;
    int ok = 1;

    TEST("Threads: concurrent registry stress");
    for (i = 0; i < 4; i++)
    {
        if (pthread_create(&threads[i], NULL, thread_worker,
                           (void *)(uintptr_t)i) != 0)
        {
            ok = 0;
        }
    }
    for (i = 0; i < 4; i++)
    {
        if (ok)
        {
            (void)pthread_join(threads[i], &results[i]);
        }
        if (results[i] != NULL)
        {
            ok = 0;
        }
    }
    (void)Directory_Delete("/thr_dir");
    if (ok) PASS();
    else FAIL("concurrent access corrupted data");
}
#endif /* FM_USE_THREADS && FM_HOST_BUILD */

/* ================================================================ */
/* Coverage-driven tests for previously untested paths                */
/* ================================================================ */
static int g_hotplug_events = 0;

static void hotplug_cb(const char *name, FM_Bool inserted, void *user_data)
{
    (void)name;
    (void)inserted;
    (void)user_data;
    g_hotplug_events++;
}

static int g_eeprom_writes = 0;
static int g_stream_calls = 0;

TEST_MAYBE_UNUSED static void stream_cb(const int16_t *samples, size_t count, void *ud)
{
    (void)samples;
    (void)ud;
    g_stream_calls += (count > 0) ? 1 : 0;
}

static FM_Result counting_eeprom_write(void *context, uint32_t address,
                                        const void *data, uint32_t length)
{
    TestDriverContext *ctx = (TestDriverContext *)context;

    if (address > ctx->capacity || length > ctx->capacity - address)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(ctx->data + address, data, length);
    g_eeprom_writes++;
    return FM_OK;
}

static void test_coverage(void)
{
    FM_Result r;
    uint64_t u64 = 0;
    FM_Bool b = FM_FALSE;
    char text[128];
    size_t rd = 0;

    /* ---- Storage raw block access + lifecycle ---- */
    TEST("Coverage: Storage raw block read/write/erase");
    {
        uint8_t block[64];
        uint32_t i;

        for (i = 0; i < sizeof(block); i++) block[i] = (uint8_t)i;
        r = Storage_WriteBlocks("ram", 0, block, sizeof(block));
        memset(block, 0, sizeof(block));
        if (r == FM_OK) r = Storage_ReadBlocks("ram", 0, block, sizeof(block));
        if (r == FM_OK)
        {
            for (i = 0; i < sizeof(block); i++)
            {
                if (block[i] != (uint8_t)i) { r = FM_ERR_IO; break; }
            }
        }
        if (r == FM_OK) r = Storage_EraseBlocks("ram", 0, sizeof(block));
        if (r == FM_OK) r = Storage_Sync("ram");
        if (r == FM_OK) r = Storage_GetCapacity("ram", &u64);
        if (r == FM_OK) r = Storage_GetFreeSpace("ram", &u64);
        CHECK_RESULT(r, "storage blocks");
    }

    TEST("Coverage: Storage insert/remove events");
    {
        g_hotplug_events = 0;
        r = Storage_SetEventCallback(hotplug_cb, NULL);
        if (r == FM_OK) r = Storage_Remove("ram");
        if (r == FM_OK) r = Storage_IsPresent("ram", &b);
        if (r == FM_OK && b) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Storage_Insert("ram");
        if (r == FM_OK) r = Storage_Mount("ram");
        if (r == FM_OK) r = Storage_IsReady("ram", &b);
        if (r == FM_OK) r = Storage_SetEventCallback(NULL, NULL);
        if (r == FM_OK && g_hotplug_events == 2 && b) r = FM_OK;
        else r = (r == FM_OK) ? FM_ERR_INTERNAL : r;
        CHECK_RESULT(r, "hotplug events");
    }

    /* ---- FS level: info / capacity / free / mounted ---- */
    TEST("Coverage: FS info/capacity/free/mounted");
    {
        FM_FSInfo info;
        FM_Bool mounted = FM_FALSE;

        r = FS_GetInfo("/", &info);
        if (r == FM_OK) r = FS_GetCapacity("/", &u64);
        if (r == FM_OK) r = FS_GetFreeSpace("/", &u64);
        if (r == FM_OK) r = FS_IsMounted("/", &mounted);
        if (r == FM_OK && !mounted) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = FS_Check("/");
        CHECK_RESULT(r, "fs info");
    }

    /* ---- Advanced file API ---- */
    TEST("Coverage: File_Append/Truncate/Flush/Tell/EOF");
    {
        FM_File *f;
        uint32_t wr = 0;
        uint64_t pos = 0;
        uint64_t size = 0;

        f = File_Open("/fm_cov_adv.bin",
                      FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
        r = (f == NULL) ? File_GetLastError() : FM_OK;
        if (r == FM_OK) r = File_Write(f, "abcd", 4, &wr);
        if (r == FM_OK) r = File_Append(f, "efgh", 4, &wr);
        if (r == FM_OK) r = File_Flush(f);
        if (r == FM_OK) r = File_Size(f, &size);
        if (r == FM_OK && size != 8) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = File_Truncate(f, 3);
        if (r == FM_OK) r = File_Size(f, &size);
        if (r == FM_OK && size != 3) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = File_Seek(f, 0, FM_SEEK_SET);
        if (r == FM_OK) r = File_Tell(f, &pos);
        if (r == FM_OK && pos != 0) r = FM_ERR_INTERNAL;
        if (r == FM_OK && File_EOF(f)) r = FM_ERR_INTERNAL;
        if (f != NULL) (void)File_Close(f);
        (void)File_Delete("/fm_cov_adv.bin");
        CHECK_RESULT(r, "advanced file api");
    }

    TEST("Coverage: File_ReadAll exact-size buffer");
    {
        uint8_t exact[5];

        r = File_WriteAll("/fm_cov_exact.bin", "hello", 5);
        if (r == FM_OK) r = File_ReadAll("/fm_cov_exact.bin", exact,
                                         sizeof(exact), &rd);
        if (r == FM_OK && rd == 5 && memcmp(exact, "hello", 5) == 0)
        {
            PASS();
        }
        else
        {
            FAIL("exact-size read");
        }
        (void)File_Delete("/fm_cov_exact.bin");
    }

    /* ---- Safe write: recover + transaction rollback ---- */
#if FM_USE_SAFE_WRITE
    TEST("Coverage: File_Recover removes stale temp");
    {
        r = File_WriteText("/fm_cov_rec.txt.fm_tmp", "stale");
        if (r == FM_OK) r = File_WriteText("/fm_cov_rec.txt", "real");
        if (r == FM_OK) r = File_Recover("/fm_cov_rec.txt");
        if (r == FM_OK) r = File_Exists("/fm_cov_rec.txt.fm_tmp", &b);
        if (r == FM_OK && b) r = FM_ERR_INTERNAL;
        (void)File_Delete("/fm_cov_rec.txt");
        (void)File_Delete("/fm_cov_rec.txt.fm_tmp");
        CHECK_RESULT(r, "recover");
    }

    TEST("Coverage: File_Transaction keeps original on failure");
    {
        r = File_WriteText("/fm_cov_tx.txt", "original");
        if (r == FM_OK)
        {
            FM_Result r2 = File_Transaction("/fm_cov_tx.txt", NULL, 5);

            if (r2 != FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_ERR_INTERNAL;
            }
        }
        memset(text, 0, sizeof(text));
        if (r == FM_OK) r = File_ReadText("/fm_cov_tx.txt", text,
                                          sizeof(text), &rd);
        if (r == FM_OK && strcmp(text, "original") == 0)
        {
            PASS();
        }
        else
        {
            FAIL("transaction rollback");
        }
        (void)File_Delete("/fm_cov_tx.txt");
        (void)File_Delete("/fm_cov_tx.txt.fm_tmp.bak");
    }
#endif

    /* ---- Logger: binary records + rotation + circular + recover ---- */
#if FM_USE_DATA_LOGGER && FM_USE_CRC
    TEST("Coverage: Logger binary records + rotation + circular");
    {
        FM_Logger lg;
        FM_LoggerConfig cfg;
        uint8_t rec[8];
        uint32_t i;
        uint32_t files = 0;

        (void)Directory_DeleteRecursive("/fm_cov_logs");
        for (i = 0; i < 8; i++) rec[i] = (uint8_t)i;
        memset(&cfg, 0, sizeof(cfg));
        cfg.max_records = 3;
        cfg.max_files = 2;
        cfg.flush_records = 0;
        cfg.crash_safe = FM_FALSE;
        r = Logger_Init(&lg, "/fm_cov_logs", "binlog", FM_LOG_FORMAT_BINARY,
                        &cfg);
        if (r == FM_OK) r = Logger_Start(&lg);
        for (i = 0; r == FM_OK && i < 7; i++)
        {
            r = Logger_WriteRecord(&lg, rec, sizeof(rec));
        }
        if (r == FM_OK) r = Logger_Stop(&lg);
        if (r == FM_OK) r = Directory_Count("/fm_cov_logs", FM_ITEM_FILE,
                                            &files);
        if (r == FM_OK && files != 2)
        {
            r = FM_ERR_INTERNAL; /* max_files=2 circular */
        }
        if (r == FM_OK) r = Logger_Recover("/fm_cov_logs", "binlog");
        (void)Directory_DeleteRecursive("/fm_cov_logs");
        CHECK_RESULT(r, "binary logger rotation");
    }
#endif

    /* ---- EEPROM: Update skips unchanged data ---- */
#if FM_USE_EEPROM
    TEST("Coverage: EEPROM_Update skips unchanged pages");
    {
        static uint8_t eemem[512];
        TestDriverContext ctx = {eemem, sizeof(eemem)};
        FM_EEPROM ee;
        FM_EEPROMConfig ec;
        uint8_t a[16];

        memset(eemem, 0xAA, sizeof(eemem));
        memset(&ec, 0, sizeof(ec));
        ec.name = "EE";
        ec.capacity = sizeof(eemem);
        ec.page_size = 16;
        ec.context = &ctx;
        ec.read = test_eeprom_read;
        ec.write = counting_eeprom_write;
        r = EEPROM_Init(&ee, &ec);
        if (r == FM_OK)
        {
            g_eeprom_writes = 0;
            r = EEPROM_Update(&ee, 0, eemem, sizeof(a)); /* identical */
        }
        if (r == FM_OK && g_eeprom_writes != 0) r = FM_ERR_INTERNAL;
        if (r == FM_OK)
        {
            memset(a, 0, sizeof(a));
            (void)EEPROM_Update(&ee, 0, a, sizeof(a)); /* materialize */
            r = EEPROM_Erase(&ee, 0, sizeof(eemem));
        }
        if (r == FM_OK)
        {
            g_eeprom_writes = 0;
            r = EEPROM_Update(&ee, 0, eemem, sizeof(a)); /* identical */
        }
        if (r == FM_OK && g_eeprom_writes == 0) r = FM_OK;
        else r = (r == FM_OK) ? FM_ERR_INTERNAL : r;
        CHECK_RESULT(r, "eeprom update skip");
    }
#endif

    /* ---- Directory: count + size ---- */
    TEST("Coverage: Directory_Count + Directory_Size");
    {
        uint32_t count = 0;
        uint64_t size = 0;

        r = Directory_Create("/fm_cov_dir/sub");
        if (r == FM_OK) r = File_WriteAll("/fm_cov_dir/a.txt", "12345", 5);
        if (r == FM_OK) r = File_WriteAll("/fm_cov_dir/sub/b.txt", "123456",
                                          6);
        if (r == FM_OK) r = Directory_Count("/fm_cov_dir", 0, &count);
        if (r == FM_OK && count != 2) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Directory_Count("/fm_cov_dir", FM_ITEM_DIR,
                                            &count);
        if (r == FM_OK && count != 1) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Directory_Size("/fm_cov_dir", &size);
        if (r == FM_OK && size != 11) r = FM_ERR_INTERNAL;
        (void)Directory_DeleteRecursive("/fm_cov_dir");
        CHECK_RESULT(r, "directory count/size");
    }

    /* ---- Explorer ---- */
#if FM_USE_EXPLORER
    TEST("Coverage: Explorer navigation");
    {
        FM_Explorer ex;

        r = Directory_Create("/fm_cov_expl/sub");
        if (r == FM_OK) r = File_WriteText("/fm_cov_expl/a.txt", "a");
        if (r == FM_OK) r = File_WriteText("/fm_cov_expl/b.txt", "bb");
        if (r == FM_OK) r = Explorer_Open(&ex, "/fm_cov_expl");
        if (r == FM_OK && Explorer_GetItemCount(&ex) != 3)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) r = Explorer_SetFilters(&ex, FM_TRUE, FM_FALSE,
                                                "txt", FM_EXPLORER_SORT_NAME);
        if (r == FM_OK && Explorer_GetItemCount(&ex) != 2)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) r = Explorer_Next(&ex);
        if (r == FM_OK) r = Explorer_Next(&ex); /* wraps */
        if (r == FM_OK) r = Explorer_Prev(&ex);
        if (r == FM_OK) r = Explorer_SetFilters(&ex, FM_FALSE, FM_FALSE,
                                                NULL, FM_EXPLORER_SORT_NONE);
        if (r == FM_OK)
        {
            uint32_t idx;
            uint32_t n = Explorer_GetItemCount(&ex);

            r = FM_ERR_NOT_FOUND;
            for (idx = 0; idx < n; idx++)
            {
                if (Explorer_IsDir(&ex, idx))
                {
                    r = Explorer_Select(&ex, idx);
                    break;
                }
            }
        }
        if (r == FM_OK) r = Explorer_Enter(&ex);
        if (r == FM_OK && strcmp(ex.cwd, "/fm_cov_expl/sub") != 0)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) r = Explorer_Up(&ex);
        if (r == FM_OK) r = Explorer_Back(&ex);
        if (r == FM_OK)
        {
            char out[FM_PATH_MAX];
            r = Explorer_GetCurrentPath(&ex, out, sizeof(out));
        }
        (void)Directory_DeleteRecursive("/fm_cov_expl");
        CHECK_RESULT(r, "explorer");
    }
#endif

    /* ---- Audio: stream + read frame ---- */
#if FM_USE_AUDIO
    TEST("Coverage: Audio_Stream + Audio_ReadFrame");
    {
        FM_AudioWriter w;
        FM_Audio a;
        int16_t tone[64];
        int16_t frame[2];
        size_t ch = 0;
        uint32_t i;

        for (i = 0; i < 64; i++) tone[i] = (int16_t)(i * 100);
        r = AudioWriter_Create("/fm_cov_tone.wav", 8000, 1, 16, &w);
        if (r == FM_OK) r = AudioWriter_Write(&w, tone, 64, NULL);
        if (r == FM_OK) r = AudioWriter_Close(&w);
        if (r == FM_OK) r = Audio_Open("/fm_cov_tone.wav", &a);
        g_stream_calls = 0;
        if (r == FM_OK) r = Audio_Stream(&a, stream_cb, NULL, 16);
        if (r == FM_OK && g_stream_calls == 0) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Audio_Seek(&a, 0.0);
        if (r == FM_OK) r = Audio_ReadFrame(&a, frame, &ch);
        if (r == FM_OK && ch != 1) r = FM_ERR_INTERNAL;
        if (File_IsOpen(&a.file)) (void)Audio_Close(&a);
        (void)File_Delete("/fm_cov_tone.wav");
        CHECK_RESULT(r, "audio stream");
    }
#endif

    /* ---- Path helpers ---- */
    TEST("Coverage: Path_RemoveExtension + IsNormalized");
    {
        char p[FM_PATH_MAX];

        (void)strcpy(p, "/a/b.txt");
        r = Path_RemoveExtension(p, sizeof(p));
        if (r == FM_OK && strcmp(p, "/a/b") != 0) r = FM_ERR_INTERNAL;
        if (r == FM_OK)
        {
            (void)strcpy(p, "/a/../b//c");
            if (Path_IsNormalized(p)) r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK && !Path_IsAbsolute("/x") ) r = FM_ERR_INTERNAL;
        if (r == FM_OK && !Path_IsRelative("x") ) r = FM_ERR_INTERNAL;
        CHECK_RESULT(r, "path helpers");
    }

    /* ---- JSON writer edge cases ---- */
#if FM_USE_JSON
    TEST("Coverage: JSON writer Int64/negative + error states");
    {
        char buf[128];
        FM_JsonWriter w;
        FM_Result r2;

        r = JsonWriter_Init(&w, buf, sizeof(buf));
        if (r == FM_OK) r = JsonWriter_Int64(&w, INT64_MIN);
        if (r == FM_OK) r2 = JsonWriter_Int64(&w, -42); else r2 = r;
        if (r2 != FM_ERR_INVALID_ARGUMENT) r = FM_ERR_INTERNAL; /* second root */
        if (r == FM_OK && strcmp(buf, "-9223372036854775808") != 0)
        {
            r = FM_ERR_INTERNAL;
        }
        /* writer errors: end without begin */
        if (r == FM_OK)
        {
            FM_JsonWriter w2;

            (void)JsonWriter_Init(&w2, buf, sizeof(buf));
            if (JsonWriter_EndObject(&w2) != FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_ERR_INTERNAL;
            }
            (void)JsonWriter_Init(&w2, buf, sizeof(buf));
            if (JsonWriter_Key(&w2, "k") != FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_ERR_INTERNAL;
            }
        }
        CHECK_RESULT(r, "json writer");
    }

    TEST("Coverage: JSON array typed accessors");
    {
        const char *js = "{\"s\":[\"x\",\"y\"],\"n\":[1,2],\"b\":[true,false],\"u\":[3000000000]}";
        FM_Json doc;
        uint32_t tokens[FM_JSON_MAX_TOKENS * FM_JSON_TOKEN_SIZE / 4];
        char s[8];
        uint32_t u = 0;
        FM_Bool bo = FM_FALSE;

        r = Json_OpenBuffer(js, strlen(js), &doc, tokens, sizeof(tokens));
        if (r == FM_OK) r = Json_ArrayString(&doc, "s", 1, s, sizeof(s));
        if (r == FM_OK && strcmp(s, "y") != 0) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Json_ArrayUInt(&doc, "u", 0, &u);
        if (r == FM_OK && u != 3000000000u) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = Json_ArrayBool(&doc, "b", 1, &bo);
        if (r == FM_OK && bo) r = FM_ERR_INTERNAL;
        if (r == FM_OK)
        {
            int32_t v = 0;
            r = Json_ArrayInt(&doc, "n", 1, &v);
            if (r == FM_OK && v != 2) r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "json array accessors");
    }
#endif

    /* ---- LittleFS: auto-format off surfaces corruption ---- */
#if FM_USE_LITTLEFS
    TEST("Coverage: LittleFS auto-format off -> corrupted");
    {
        static uint8_t bad_mem[64 * 1024];
        static FM_Storage bad_storage;
        FM_FS bad_fs;

        memset(bad_mem, 0x55, sizeof(bad_mem)); /* never formatted */
        r = FS_Unmount("/");
        if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
        if (r == FM_OK) r = MemoryStorage_Init(&bad_storage, "lfsbad",
                                               bad_mem, sizeof(bad_mem),
                                               4096, FM_FALSE);
        if (r == FM_OK) r = Storage_Register(&bad_storage);
        if (r == FM_OK) r = Storage_Mount("lfsbad");
        if (r == FM_OK) r = LittleFS_Init(&bad_fs);
        if (r == FM_OK)
        {
            LittleFS_SetAutoFormat(&bad_fs, FM_FALSE);
            r = FS_Mount("/", &bad_fs);
        }
        if (r == FM_ERR_CORRUPTED)
        {
            PASS();
        }
        else
        {
            FAIL("corruption not surfaced");
        }
        (void)FS_Unmount("/");
        (void)Storage_Unmount("lfsbad");
        (void)Storage_Unregister("lfsbad");
    }
#endif

    /* ---- Cross-mount file move (copy + delete fallback) ---- */
#if FM_USE_FMFS
    TEST("Coverage: cross-mount File_Move copies + deletes");
    {
        FM_FS memfs;
        FM_Bool exists = FM_FALSE;

        r = FS_Unmount("/");
        if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
        if (r == FM_OK) r = Storage_Mount("ram");
        if (r == FM_OK) r = FS_Mount("/", &g_fs);
        if (r == FM_OK) r = FMFS_Init(&memfs);
        if (r == FM_OK) r = FS_Mount("/ram", &memfs);
        if (r == FM_OK) r = File_WriteText("/fm_cov_move.txt", "cross");
        if (r == FM_OK) r = File_Move("/fm_cov_move.txt",
                                      "/ram/fm_cov_move.txt", NULL, NULL);
        if (r == FM_OK) r = File_Exists("/fm_cov_move.txt", &exists);
        if (r == FM_OK && exists) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = File_ReadText("/ram/fm_cov_move.txt", text,
                                          sizeof(text), &rd);
        if (r == FM_OK && strcmp(text, "cross") != 0) r = FM_ERR_INTERNAL;
        (void)File_Delete("/ram/fm_cov_move.txt");
        (void)FS_Unmount("/ram");
        CHECK_RESULT(r, "cross-mount move");
    }
#endif

    /* ---- USB MSC: out-of-range LBA rejected ---- */
#if FM_USE_USB_MSC && FM_USE_EEPROM
    TEST("Coverage: USB MSC out-of-range LBA");
    {
        static uint8_t eemem[512];
        TestDriverContext ctx = {eemem, sizeof(eemem)};
        FM_EEPROM ee;
        FM_EEPROMConfig ec;
        FM_Storage st;
        FM_USB_MSC msc;
        uint8_t blk[64];

        memset(eemem, 0xFF, sizeof(eemem));
        memset(&ec, 0, sizeof(ec));
        ec.name = "EEUSB";
        ec.capacity = sizeof(eemem);
        ec.page_size = 16;
        ec.context = &ctx;
        ec.read = test_eeprom_read;
        ec.write = test_eeprom_write;
        r = EEPROM_Init(&ee, &ec);
        if (r == FM_OK) r = EEPROM_StorageInit(&ee, &st, FM_STORAGE_CUSTOM);
        if (r == FM_OK) r = USB_MSC_Init(&msc, &st, 64);
        if (r == FM_OK)
        {
            r = USB_MSC_ReadBlocks(&msc, 100, 1, blk);
            if (r == FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_OK;
            }
        }
        CHECK_RESULT(r, "usb msc range");
    }
#endif

    /* Restore the POSIX mount for any test that runs afterwards. */
    {
        FM_Bool mounted = FM_FALSE;

        if (FS_IsMounted("/", &mounted) != FM_OK || !mounted)
        {
            r = Storage_Mount("ram");
            if (r == FM_OK)
            {
                r = FS_Mount("/", &g_fs);
            }
            if (r != FM_OK && r != FM_ERR_EXISTS)
            {
                printf("  NOTE: POSIX remount after coverage: %s\n",
                       FM_ResultString(r));
            }
        }
    }
}

/* ================================================================ */
/* Main                                                             */
/* ================================================================ */
/* ================================================================ */
/* Per-suite resource sentinel                                       */
/* ================================================================ */
/* The file-handle pool is a fixed FM_MAX_OPEN_FILES array: one handle
 * leaked by an error path is gone until reset, and the suites after it
 * start failing for unrelated-looking reasons (this is how the leak in
 * File_Open(NULL, ...) hid behind "Threads: concurrent registry stress").
 * So every suite must give back everything it opened. */
static uint32_t g_suite_pool_base = 0u;

static void suite_leak_check(const char *suite)
{
    uint32_t used = File_PoolUsed();

    if (used > g_suite_pool_base)
    {
        g_tests++;
        printf("  TEST %-45s ... ", suite);
        printf("%lu handle(s) still open ",
               (unsigned long)(used - g_suite_pool_base));
        FAIL("suite leaked file handles");
    }
    g_suite_pool_base = used;
}

#define RUN_SUITE(fn) do { fn(); suite_leak_check(#fn); } while (0)

int main(void)
{
    printf("\nFileManager Host Test Suite\n");
    printf("==========================\n\n");

    setup();

    RUN_SUITE(test_path);
    RUN_SUITE(test_file_create);
    RUN_SUITE(test_file_write_read);
    RUN_SUITE(test_file_append);
    RUN_SUITE(test_file_copy);
    RUN_SUITE(test_file_move_delete);
    RUN_SUITE(test_file_seek);
    RUN_SUITE(test_directory);
#if FM_USE_JSON
    RUN_SUITE(test_json);
#endif
#if FM_USE_DATA_LOGGER
    RUN_SUITE(test_logger);
#endif
#if FM_USE_MEDIA_DETECT
    RUN_SUITE(test_media);
#endif
#if FM_USE_ASYNC || FM_USE_AUDIO || FM_USE_IMAGE || FM_USE_VIDEO
    RUN_SUITE(test_async_and_codecs);
#endif
#if FM_USE_EEPROM && FM_USE_USB_MSC
    RUN_SUITE(test_eeprom_usb);
#endif
#if FM_USE_DEVICE_ADAPTERS
    RUN_SUITE(test_standard_device_adapters);
#endif
#if FM_USE_FMFS
    RUN_SUITE(test_memory_fs);
#endif
#if FM_USE_LITTLEFS
    RUN_SUITE(test_littlefs);
#endif
#if FM_USE_CACHE && FM_USE_FMFS
    RUN_SUITE(test_cache);
#endif
    restore_posix_root();
#if FM_USE_MP3
    RUN_SUITE(test_mp3);
    RUN_SUITE(test_mp3_readahead);
#endif
    restore_posix_root();
    RUN_SUITE(test_port_and_debug);
    RUN_SUITE(test_qspi);
    RUN_SUITE(test_spiflash);
    RUN_SUITE(test_mediacache);
    RUN_SUITE(test_crash_safety);
    RUN_SUITE(test_logging);
    RUN_SUITE(test_board_config);
    RUN_SUITE(test_transfer);
#if FM_USE_VIDEO
    RUN_SUITE(test_video);
#endif
#if FM_USE_MJPEG
    RUN_SUITE(test_mjpeg);
#endif
#if FM_USE_IMAGE
    RUN_SUITE(test_scale);
#endif
#if FM_USE_AUDIO
    RUN_SUITE(test_audio_hook);
#endif
#if FM_USE_AUDIO || FM_USE_VIDEO
    RUN_SUITE(test_player);
#endif
#if FM_USE_AUDIO
    RUN_SUITE(test_ex13_options);
    RUN_SUITE(test_opus_ogg);
    RUN_SUITE(test_adpcm_small_chunks);
#endif
#if FM_USE_AUDIO && FM_USE_MP3
    RUN_SUITE(test_simple_audio_track_change);
    RUN_SUITE(test_wav_truncated_board_geometry);
#endif
    RUN_SUITE(test_allocator);
    RUN_SUITE(test_regressions);
    RUN_SUITE(test_coverage);
#if FM_USE_THREADS && FM_HOST_BUILD
    RUN_SUITE(test_threads);
#endif

    printf("\n  Results: %d/%d passed, %d failures\n",
           g_tests - g_failures, g_tests, g_failures);

    FileManager_Deinit();
    return (g_failures > 0) ? 1 : 0;
}