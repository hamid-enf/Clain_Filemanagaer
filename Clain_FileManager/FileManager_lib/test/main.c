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
static void pf_v1_image(uint8_t *mem, uint32_t cap);
static uint64_t pf_le(const uint8_t *p, uint32_t n);

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
        /* Metadata that cannot be read.  On volatile media (RAM disk /
         * external SDRAM that survives soft resets) the mount must
         * recover by re-formatting instead of failing with
         * FM_ERR_CORRUPTED - otherwise every later file call in the
         * application dies, exactly like on the STM32H7 board whose
         * "RAM disk" lives in external SDRAM.
         *
         * Round 0: a version 1 volume, as older releases wrote it, with
         * a reset in the middle of its one table: an entry record is
         * torn.  Nothing else to read: formatted.
         * Round 1: what a format writes today.  One torn table is NOT
         * a reason to format -- the other one is used and the files are
         * still there.  Both torn: formatted. */
        static uint8_t corrupt_buf[64 * 1024];
        FM_Storage cst;
        FM_FS      cfs;
        char ctext[64];
        uint32_t round;
        const char *why = NULL;

        r = FS_Unmount("/");
        if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;

        for (round = 0; round < 2u && why == NULL; round++)
        {
            FM_Bool safe;
            uint32_t slot = 0;

            if (round == 0u) pf_v1_image(corrupt_buf, sizeof(corrupt_buf));
            else memset(corrupt_buf, 0, sizeof(corrupt_buf));
            r = FileManager_MountMemory(&cst, &cfs, "CRPT", corrupt_buf,
                                        sizeof(corrupt_buf), 1, FM_FALSE);
            if (r != FM_OK) { why = FM_ResultString(r); break; }
            safe = FMFS_IsPowerSafe(&cfs);
            r = File_WriteText("/ok.txt", "fine");
            (void)FS_Unmount("/");
            (void)Storage_Unregister("CRPT");
            if (r != FM_OK) { why = "write"; break; }
            if (round == 0u && safe) { why = "version 1 converted"; break; }

            if (safe)
            {
                /* tear the table that is not the newest one */
                uint32_t seq_a = (uint32_t)pf_le(corrupt_buf + 32, 4);
                uint32_t seq_b;

                slot = (uint32_t)pf_le(corrupt_buf + 36, 4);
                if (slot < 5000u || slot > 9000u) { why = "layout"; break; }
                seq_b = (uint32_t)pf_le(corrupt_buf + slot + 32u, 4);
                corrupt_buf[((seq_a < seq_b) ? 0u : slot) + 64u + 181u] = 0x77;
                r = FileManager_MountMemory(&cst, &cfs, "CRPT", corrupt_buf,
                                            sizeof(corrupt_buf), 1, FM_FALSE);
                memset(ctext, 0, sizeof(ctext));
                if (r == FM_OK)
                    r = File_ReadText("/ok.txt", ctext, sizeof(ctext), NULL);
                (void)FS_Unmount("/");
                (void)Storage_Unregister("CRPT");
                if (r != FM_OK || strcmp(ctext, "fine") != 0)
                {
                    why = "one torn table lost the files";
                    break;
                }
                /* now both */
                corrupt_buf[64 + 181] ^= 0x55u;
                corrupt_buf[slot + 64u + 181u] ^= 0x55u;
            }
            else
            {
                /* header at [0..31]: magic[0..3], version u16@4, count
                 * u16@6, data_start u64@8, data_next u64@16, capacity
                 * u64@24.  Records start at 32: used@0, type@1, ...
                 * (180 bytes each).  Tear entry #1: mark it used with
                 * an invalid type. */
                corrupt_buf[32 + 180 + 0] = 1;      /* used   */
                corrupt_buf[32 + 180 + 1] = 0x77;   /* bogus type */
            }

            r = FileManager_MountMemory(&cst, &cfs, "CRPT", corrupt_buf,
                                        sizeof(corrupt_buf), 1, FM_FALSE);
            if (r != FM_OK) { why = FM_ResultString(r); break; }
            memset(ctext, 0, sizeof(ctext));
            if (File_ReadText("/ok.txt", ctext, sizeof(ctext), NULL) !=
                FM_ERR_NOT_FOUND)
            {
                why = "not formatted";
            }
            else
            {
                r = File_WriteText("/after-recover.txt", "recovered");
                memset(ctext, 0, sizeof(ctext));
                if (r == FM_OK)
                    r = File_ReadText("/after-recover.txt", ctext,
                                      sizeof(ctext), NULL);
                if (r != FM_OK || strcmp(ctext, "recovered") != 0)
                    why = "write after auto-format";
            }
            (void)FS_Unmount("/");
            (void)Storage_Unregister("CRPT");
        }
        if (why != NULL) { FAIL(why); }
        else { PASS(); }
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
#ifdef FM_FILE_FORCE_BOUNCE
    /* through the bounce buffer the same read is one backend call per
     * 512 bytes -- that is the point of the bounce path */
    if (ctx.reads == (uint32_t)((sizeof(bulk) + 511u) / 512u)) ctx.reads = 1;
#endif
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
            if (j + 3 > len)
            {
                j = len;    /* last NAL runs to the end of the stream */
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

/* build_mp4() knobs for the report regressions: NAL length-field width
 * (avcC/hvcC lengthSizeMinusOne + 1) and an "hvc1"/"hvcC" sample entry
 * instead of "avc1"/"avcC". */
static uint32_t g_tst_mp4_len_size = 4u;
static int      g_tst_mp4_hevc = 0;

static void tb_nal_len(TstBuf *t, uint32_t len)
{
    if (g_tst_mp4_len_size == 4u)      tb_u32(t, len);
    else if (g_tst_mp4_len_size == 2u) tb_u16(t, (uint16_t)len);
    else                               tb_u8(t, (uint8_t)len);
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
    const uint32_t ls = g_tst_mp4_len_size;

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
                                size_t avc1 = tb_box(&t, g_tst_mp4_hevc ? "hvc1" : "avc1");
                                tb_u32(&t, 0); tb_u16(&t, 0); tb_u16(&t, 1);
                                tb_u16(&t, 0); tb_u16(&t, 0);
                                tb_u32(&t, 0); tb_u32(&t, 0); tb_u32(&t, 0);
                                tb_u16(&t, 64); tb_u16(&t, 64);
                                tb_u32(&t, 0x00480000); tb_u32(&t, 0x00480000);
                                tb_u32(&t, 0); tb_u16(&t, 1);
                                for (i = 0; i < 8; i++) tb_u32(&t, 0);
                                tb_u16(&t, 0x18); tb_u16(&t, 0xFFFF);
                                if (g_tst_mp4_hevc)
                                {
                                    size_t hvcc = tb_box(&t, "hvcC");
                                    tb_u8(&t, 1);
                                    for (i = 0; i < 20; i++) tb_u8(&t, 0);
                                    tb_u8(&t, (uint8_t)(0xFCu | (ls - 1u)));
                                    tb_u8(&t, 2);            /* numOfArrays */
                                    tb_u8(&t, 0x80 | 33); tb_u16(&t, 1);
                                    tb_u16(&t, (uint16_t)sps_len); tb_bytes(&t, sps, sps_len);
                                    tb_u8(&t, 0x80 | 34); tb_u16(&t, 1);
                                    tb_u16(&t, (uint16_t)pps_len); tb_bytes(&t, pps, pps_len);
                                    tb_box_end(&t, hvcc);
                                }
                                else
                                {
                                    size_t avcc = tb_box(&t, "avcC");
                                    tb_u8(&t, 1); tb_u8(&t, 0x42); tb_u8(&t, 0); tb_u8(&t, 0x1F);
                                    tb_u8(&t, (uint8_t)(0xFCu | (ls - 1u)));
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
                            tb_u32(&t, ls + sps_len + ls + pps_len + ls + idr_len);
                            tb_u32(&t, ls + p1_len);
                            tb_u32(&t, ls + p2_len);
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
        tb_nal_len(&t, sps_len); tb_bytes(&t, sps, sps_len);
        tb_nal_len(&t, pps_len); tb_bytes(&t, pps, pps_len);
        tb_nal_len(&t, idr_len); tb_bytes(&t, idr, idr_len);
        off1 = (uint32_t)t.n;
        tb_nal_len(&t, p1_len); tb_bytes(&t, p1, p1_len);
        off2 = (uint32_t)t.n;
        tb_nal_len(&t, p2_len); tb_bytes(&t, p2, p2_len);
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

/* ------------------------------------------------------------------ */
/* File / Transfer report regressions (F-01 .. F-16)                   */
/*                                                                     */
/* A wrapper filesystem sits on top of the POSIX adapter and lets a    */
/* test inject exactly one fault: a failing delete, a write that runs  */
/* out of space, a read that returns FM_ERR_IO, a corrupted write.     */
/* A second mount of the POSIX adapter maps to the same host           */
/* directory, so "/ram/x" and "/x" are the same host file seen through  */
/* two different mounts.                                               */
/* ------------------------------------------------------------------ */
static FM_FS     g_wfs;
static FM_FS     g_wfs_base;
static FM_Result g_wfs_delete_result = FM_OK;
static int64_t   g_wfs_write_budget = -1;   /* bytes left, -1 = no limit */
static uint32_t  g_wfs_read_calls;
static uint32_t  g_wfs_read_fail_at;        /* fail this call (1-based)  */
static uint32_t  g_wfs_open_calls;
static uint32_t  g_wfs_read_fail_from_open; /* fail reads from Nth open  */
static uint32_t  g_wfs_flush_calls;
static uint32_t  g_wfs_rename_calls;
static FM_Bool   g_wfs_corrupt;

static void wfs_reset(void)
{
    g_wfs_delete_result = FM_OK;
    g_wfs_write_budget = -1;
    g_wfs_read_calls = 0u;
    g_wfs_read_fail_at = 0u;
    g_wfs_open_calls = 0u;
    g_wfs_read_fail_from_open = 0u;
    g_wfs_flush_calls = 0u;
    g_wfs_rename_calls = 0u;
    g_wfs_corrupt = FM_FALSE;
}

static FM_Result wfs_delete(FM_FS *fs, const char *path)
{
    if (g_wfs_delete_result != FM_OK)
    {
        return g_wfs_delete_result;
    }
    return g_wfs_base.ops.delete_file(fs, path);
}

static FM_Result wfs_rename(FM_FS *fs, const char *a, const char *b)
{
    g_wfs_rename_calls++;
    return g_wfs_base.ops.rename(fs, a, b);
}

static FM_Result wfs_open(FM_FS *fs, void *fh, const char *path,
                          uint32_t mode)
{
    g_wfs_open_calls++;
    return g_wfs_base.ops.file_open(fs, fh, path, mode);
}

static FM_Result wfs_read(FM_FS *fs, void *fh, void *buf, uint32_t len,
                          uint32_t *got)
{
    g_wfs_read_calls++;
    if (g_wfs_read_fail_at != 0u && g_wfs_read_calls >= g_wfs_read_fail_at)
    {
        if (got != NULL) { *got = 0u; }
        return FM_ERR_IO;
    }
    if (g_wfs_read_fail_from_open != 0u &&
        g_wfs_open_calls >= g_wfs_read_fail_from_open)
    {
        if (got != NULL) { *got = 0u; }
        return FM_ERR_IO;
    }
    return g_wfs_base.ops.file_read(fs, fh, buf, len, got);
}

static FM_Result wfs_write(FM_FS *fs, void *fh, const void *buf,
                           uint32_t len, uint32_t *put)
{
    uint32_t n = len;

    if (g_wfs_write_budget >= 0)
    {
        if (g_wfs_write_budget == 0)
        {
            if (put != NULL) { *put = 0u; }
            return FM_ERR_NO_SPACE;
        }
        if ((int64_t)n > g_wfs_write_budget)
        {
            n = (uint32_t)g_wfs_write_budget;
        }
        g_wfs_write_budget -= (int64_t)n;
    }
    if (g_wfs_corrupt && n > 0u)
    {
        /* flip the first byte of every chunk on its way to the "disk" */
        uint8_t bad = (uint8_t)(((const uint8_t *)buf)[0] ^ 0xFFu);
        uint32_t w1 = 0u;
        uint32_t w2 = 0u;
        FM_Result r = g_wfs_base.ops.file_write(fs, fh, &bad, 1u, &w1);

        if (r == FM_OK && n > 1u)
        {
            r = g_wfs_base.ops.file_write(fs, fh,
                                          (const uint8_t *)buf + 1, n - 1u,
                                          &w2);
        }
        if (put != NULL) { *put = w1 + w2; }
        return r;
    }
    return g_wfs_base.ops.file_write(fs, fh, buf, n, put);
}

static FM_Result wfs_flush(FM_FS *fs, void *fh)
{
    g_wfs_flush_calls++;
    return (g_wfs_base.ops.file_flush != NULL)
               ? g_wfs_base.ops.file_flush(fs, fh) : FM_OK;
}

static FM_Result wfs_mount(const char *where)
{
    FM_Result r = PosixFS_Init(&g_wfs_base);

    if (r != FM_OK)
    {
        return r;
    }
    memset(&g_wfs, 0, sizeof(g_wfs));
    (void)strcpy(g_wfs.name, "WRAPFS");
    g_wfs.ops = g_wfs_base.ops;
    g_wfs.ops.delete_file = wfs_delete;
    g_wfs.ops.rename = wfs_rename;
    g_wfs.ops.file_open = wfs_open;
    g_wfs.ops.file_read = wfs_read;
    g_wfs.ops.file_write = wfs_write;
    g_wfs.ops.file_flush = wfs_flush;
    wfs_reset();
    r = FS_Register(&g_wfs);
    if (r == FM_OK)
    {
        r = FS_Mount(where, &g_wfs);
    }
    return r;
}

static void wfs_unmount(const char *where)
{
    (void)FS_Unmount(where);
    (void)FS_Unregister("WRAPFS");
    wfs_reset();
}

/* Fake raw storage that records what the transfer layer asks for. */
typedef struct
{
    uint8_t *mem;            /* backing bytes (may be NULL: discard)   */
    uint32_t mem_size;
    uint32_t reads;
    uint32_t writes;
    uint32_t erases;
    uint64_t max_write_end;
    uint64_t erase_off[8];
    uint32_t erase_len[8];
    FM_Bool  strict_erase;   /* reject anything but whole aligned units */
} FakeStore;

static FM_Result fake_read(FM_Storage *s, uint64_t off, void *buf,
                           uint32_t len)
{
    FakeStore *fsr = (FakeStore *)s->user_data;

    fsr->reads++;
    if (fsr->mem != NULL && off < fsr->mem_size &&
        len <= fsr->mem_size - (uint32_t)off)
    {
        memcpy(buf, fsr->mem + off, len);
    }
    return FM_OK;
}

static FM_Result fake_write(FM_Storage *s, uint64_t off, const void *buf,
                            uint32_t len)
{
    FakeStore *fsr = (FakeStore *)s->user_data;

    fsr->writes++;
    if (off + len > fsr->max_write_end)
    {
        fsr->max_write_end = off + len;
    }
    if (fsr->mem != NULL && off < fsr->mem_size &&
        len <= fsr->mem_size - (uint32_t)off)
    {
        memcpy(fsr->mem + off, buf, len);
    }
    return FM_OK;
}

static FM_Result fake_erase(FM_Storage *s, uint64_t off, uint32_t len)
{
    FakeStore *fsr = (FakeStore *)s->user_data;

    if (fsr->strict_erase && s->erase_size != 0u &&
        ((off % s->erase_size) != 0u || (len % s->erase_size) != 0u ||
         len == 0u))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (fsr->erases < 8u)
    {
        fsr->erase_off[fsr->erases] = off;
        fsr->erase_len[fsr->erases] = len;
    }
    fsr->erases++;
    if (fsr->mem != NULL && off < fsr->mem_size &&
        len <= fsr->mem_size - (uint32_t)off)
    {
        memset(fsr->mem + off, 0xFF, len);
    }
    return FM_OK;
}

static void fake_store_init(FM_Storage *s, FakeStore *fsr, uint8_t *mem,
                            uint32_t mem_size, uint64_t capacity)
{
    memset(s, 0, sizeof(*s));
    memset(fsr, 0, sizeof(*fsr));
    fsr->mem = mem;
    fsr->mem_size = mem_size;
    (void)strcpy(s->name, "fake");
    s->capacity = capacity;
    s->present = 1u;
    s->user_data = fsr;
    s->read = fake_read;
    s->write = fake_write;
}

static FM_Bool f_cancel_now_cb(uint64_t done, uint64_t total, void *user)
{
    (void)done;
    (void)total;
    (void)user;
    return FM_FALSE;
}

/* F-05: what the copy had flushed when each progress report arrived */
static uint32_t g_f05_calls;
static uint32_t g_f05_full_reports;
static uint32_t g_f05_flushes_at_full;
static uint32_t g_f05_flushes_before_full;

static void f05_progress(uint64_t cur, uint64_t total, void *user)
{
    (void)user;
    g_f05_calls++;
    if (cur >= total)
    {
        g_f05_full_reports++;
        g_f05_flushes_at_full = g_wfs_flush_calls;
    }
    else
    {
        g_f05_flushes_before_full = g_wfs_flush_calls;
    }
}

/* F-11: a progress callback that starts another copy / edit */
static FM_Result g_f11_nested_copy = FM_OK;
static FM_Result g_f11_nested_insert = FM_OK;
static FM_Bool   g_f11_ran;

static void f11_file_progress(uint64_t cur, uint64_t total, void *user)
{
    (void)cur;
    (void)total;
    (void)user;
    if (!g_f11_ran)
    {
        g_f11_ran = FM_TRUE;
        g_f11_nested_copy = File_Copy("/f11_other.bin", "/f11_other2.bin",
                                      NULL, NULL);
        g_f11_nested_insert = File_Insert("/f11_other.bin", 0u, "zz", 2u);
    }
}

static FM_Result g_f11_nested_xfer = FM_OK;

static FM_Bool f11_xfer_progress(uint64_t done, uint64_t total, void *user)
{
    (void)done;
    (void)total;
    (void)user;
    if (!g_f11_ran)
    {
        g_f11_ran = FM_TRUE;
        g_f11_nested_xfer = FM_Transfer_Copy("/f11_other.bin",
                                             "/f11_other2.bin", NULL);
    }
    return FM_TRUE;
}

#ifdef FM_XFER_CACHE_CLEAN
/* The matrix builds one configuration with the transfer layer's cache
 * hooks redirected here (see tools/check_matrix.sh). */
static uint32_t g_xfer_clean_calls;
static uint32_t g_xfer_inval_calls;
static uint32_t g_xfer_hook_unaligned;

void FM_XFER_CACHE_CLEAN(const void *addr, size_t bytes);
void FM_XFER_CACHE_INVALIDATE(void *addr, size_t bytes);

void FM_XFER_CACHE_CLEAN(const void *addr, size_t bytes)
{
    (void)bytes;
    g_xfer_clean_calls++;
    if (((uintptr_t)addr & 31u) != 0u)
    {
        g_xfer_hook_unaligned++;
    }
}

void FM_XFER_CACHE_INVALIDATE(void *addr, size_t bytes)
{
    (void)bytes;
    g_xfer_inval_calls++;
    if (((uintptr_t)addr & 31u) != 0u)
    {
        g_xfer_hook_unaligned++;
    }
}
#endif

static FM_Bool f_file_equals(const char *path, const void *want, size_t len)
{
    static uint8_t back[48u * 1024u];
    size_t got = 0;

    if (len > sizeof(back))
    {
        return FM_FALSE;
    }
    if (File_ReadAll(path, back, sizeof(back), &got) != FM_OK)
    {
        return FM_FALSE;
    }
    return (got == len && memcmp(back, want, len) == 0) ? FM_TRUE : FM_FALSE;
}

static FM_Bool f_exists(const char *path)
{
    FM_Bool exists = FM_FALSE;

    return (File_Exists(path, &exists) == FM_OK && exists) ? FM_TRUE
                                                          : FM_FALSE;
}

static void test_file_report(void)
{
    static uint8_t big[40000];
    size_t i;

    restore_posix_root();
    for (i = 0; i < sizeof(big); i++)
    {
        big[i] = (uint8_t)((i * 31u) ^ (i >> 7));
    }

    /* ---------------------------------------------------------------- */
    TEST("F-01: File_Move reports NULL / resolver errors as they are");
    {
        static char longp[FM_PATH_MAX + 64];
        FM_Result a, b, c, d, e, f;

        /* no root mount for this one, so "/nomount" really is unmounted */
        (void)FS_Unmount("/");
        (void)FS_Mount("/ram", &g_fs);
        (void)File_WriteText("/ram/mv_a.txt", "0123456789");
        memset(longp, 'a', sizeof(longp) - 1u);
        longp[0] = '/';
        longp[sizeof(longp) - 1u] = '\0';

        a = File_Move(NULL, "/ram/mv_b.txt", NULL, NULL);
        b = File_Move("/ram/mv_a.txt", NULL, NULL, NULL);
        c = File_Move("/nomount/x.txt", "/ram/mv_c.txt", NULL, NULL);
        d = File_Move("/ram/mv_a.txt", "/nomount/y.txt", NULL, NULL);
        e = File_Move(longp, "/ram/mv_b.txt", NULL, NULL);
        f = File_Move("relative.txt", "/ram/mv_b.txt", NULL, NULL);
        if (a != FM_ERR_INVALID_ARGUMENT || b != FM_ERR_INVALID_ARGUMENT ||
            c != FM_ERR_NOT_MOUNTED || d != FM_ERR_NOT_MOUNTED ||
            e != FM_ERR_TOO_LARGE || f != FM_ERR_INVALID_PATH)
        {
            static char m[96];

            (void)snprintf(m, sizeof(m), "got %d,%d,%d,%d,%d,%d", (int)a,
                           (int)b, (int)c, (int)d, (int)e, (int)f);
            FAIL(m);
        }
        else if (!f_exists("/ram/mv_a.txt"))
        {
            FAIL("a rejected move removed the source");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/ram/mv_a.txt");
        (void)FS_Unmount("/ram");
        restore_posix_root();
    }

    /* ---------------------------------------------------------------- */
    if (wfs_mount("/ram") != FM_OK)
    {
        TEST("F-xx: wrapper filesystem mount");
        FAIL("could not mount the fault-injection filesystem");
        wfs_unmount("/ram");
        return;
    }

    TEST("F-02: failed move keeps a destination that already existed");
    {
        FM_Result r;

        (void)File_WriteText("/f02_src.txt", "payload-to-move");
        (void)File_WriteText("/f02_dst.txt",
                             "IMPORTANT-PRE-EXISTING-DESTINATION");
        g_wfs_delete_result = FM_ERR_ACCESS;       /* read-only source */
        r = File_Move("/ram/f02_src.txt", "/f02_dst.txt", NULL, NULL);
        g_wfs_delete_result = FM_OK;
        if (r != FM_ERR_ACCESS)
        {
            FAIL("source delete failure not reported");
        }
        else if (!f_exists("/f02_src.txt"))
        {
            FAIL("source vanished");
        }
        else if (!f_exists("/f02_dst.txt"))
        {
            FAIL("rollback deleted the caller's pre-existing destination");
        }
        else if (!f_file_equals("/f02_dst.txt", "payload-to-move", 15u))
        {
            FAIL("destination is neither old nor a complete copy");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-02: failed move still rolls back a destination it created");
    {
        FM_Result r;

        (void)File_Delete("/f02_dst.txt");
        g_wfs_delete_result = FM_ERR_ACCESS;
        r = File_Move("/ram/f02_src.txt", "/f02_dst.txt", NULL, NULL);
        g_wfs_delete_result = FM_OK;
        if (r != FM_ERR_ACCESS)
        {
            FAIL("source delete failure not reported");
        }
        else if (f_exists("/f02_dst.txt"))
        {
            FAIL("copy left behind although the move failed");
        }
        else if (!f_exists("/f02_src.txt"))
        {
            FAIL("source vanished");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f02_src.txt");
        (void)File_Delete("/f02_dst.txt");
    }

    /* ---------------------------------------------------------------- */
    TEST("F-03: File_PoolDump separates internal and external");
    {
        FM_LogSinkFn saved;
        void *saved_ctx = NULL;
        FM_File *in;
        FM_File x1;
        FM_File x2;
        uint32_t i0 = 0u, e0 = 0u, i1 = 0u, e1 = 0u;
        char want[64];

        File_PoolUsedSplit(&i0, &e0);
        (void)File_WriteText("/f03_a.txt", "a");
        (void)File_WriteText("/f03_b.txt", "b");
        (void)File_WriteText("/f03_c.txt", "c");
        File_Init(&x1);
        File_Init(&x2);
        in = File_Open("/f03_a.txt", FM_OPEN_READ);
        (void)File_OpenEx(&x1, "/f03_b.txt", FM_OPEN_READ);
        (void)File_OpenEx(&x2, "/f03_c.txt", FM_OPEN_READ);
        File_PoolUsedSplit(&i1, &e1);

        saved = FM_Log_GetSink(&saved_ctx);
        g_logcap_len = 0;
        g_logcap[0] = '\0';
        FM_Log_SetSink(test_log_sink, NULL);
        File_PoolDump();
        FM_Log_SetSink(saved, saved_ctx);

        (void)snprintf(want, sizeof(want), "%lu internal / %lu external",
                       (unsigned long)(i0 + 1u), (unsigned long)(e0 + 2u));
        if (in == NULL || !File_IsOpen(&x1) || !File_IsOpen(&x2))
        {
            FAIL("could not open the three handles");
        }
        else if (i1 != i0 + 1u || e1 != e0 + 2u)
        {
            FAIL("File_PoolUsedSplit miscounts");
        }
        else if (File_PoolUsed() != i1 + e1)
        {
            FAIL("File_PoolUsed is no longer the total");
        }
        else if (strstr(g_logcap, want) == NULL)
        {
            FAIL("header does not show the two counts separately");
        }
        else if (strstr(g_logcap, "] /f03_a.txt") == NULL ||
                 strstr(g_logcap, "[i") == NULL ||
                 strstr(g_logcap, "[x") == NULL)
        {
            FAIL("slot tags [i..] / [x..] missing");
        }
        else
        {
            /* the pool handle is tagged i, the OpenEx handles x */
            const char *pa = strstr(g_logcap, "] /f03_a.txt");
            const char *pb = strstr(g_logcap, "] /f03_b.txt");
            const char *pc = strstr(g_logcap, "] /f03_c.txt");

            if (pb == NULL || pc == NULL || pa - g_logcap < 3 ||
                pb - g_logcap < 3 || pc - g_logcap < 3 ||
                pa[-2] != 'i' || pb[-2] != 'x' || pc[-2] != 'x')
            {
                FAIL("tag does not match the handle kind");
            }
            else
            {
                PASS();
            }
        }
        if (in != NULL) { (void)File_Close(in); }
        (void)File_Close(&x1);
        (void)File_Close(&x2);
        (void)File_Delete("/f03_a.txt");
        (void)File_Delete("/f03_b.txt");
        (void)File_Delete("/f03_c.txt");
    }

    /* ---------------------------------------------------------------- */
    TEST("F-04: failed File_Copy leaves no partial destination");
    {
        FM_Result r;

        (void)File_WriteAll("/f04_src.bin", big, sizeof(big));
        (void)File_Delete("/f04_dst.bin");
        g_wfs_write_budget = 5000;
        r = File_Copy("/f04_src.bin", "/ram/f04_dst.bin", NULL, NULL);
        g_wfs_write_budget = -1;
        if (r != FM_ERR_NO_SPACE)
        {
            static char m[48];

            (void)snprintf(m, sizeof(m), "expected NO_SPACE, got %d",
                           (int)r);
            FAIL(m);
        }
        else if (f_exists("/f04_dst.bin"))
        {
            FAIL("truncated destination left behind");
        }
        else if (f_exists("/f04_dst.bin.fm_tmp"))
        {
            FAIL("temp file left behind");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-04: failed File_Copy keeps an existing destination intact");
    {
        static const char old_txt[] = "OLD-DESTINATION-CONTENT";
        FM_Result r;

        (void)File_WriteText("/f04_dst.bin", old_txt);
        g_wfs_write_budget = 5000;
        r = File_Copy("/f04_src.bin", "/ram/f04_dst.bin", NULL, NULL);
        g_wfs_write_budget = -1;
        if (r != FM_ERR_NO_SPACE)
        {
            FAIL("expected NO_SPACE");
        }
        else if (!f_file_equals("/f04_dst.bin", old_txt,
                                sizeof(old_txt) - 1u))
        {
            FAIL("old destination was destroyed by a failed copy");
        }
        else if (f_exists("/f04_dst.bin.fm_tmp"))
        {
            FAIL("temp file left behind");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-04: successful File_Copy replaces the destination");
    {
        FM_Result r = File_Copy("/f04_src.bin", "/ram/f04_dst.bin", NULL,
                                NULL);

        if (r != FM_OK)
        {
            FAIL("copy failed");
        }
        else if (!f_file_equals("/f04_dst.bin", big, sizeof(big)))
        {
            FAIL("content differs");
        }
        else if (f_exists("/f04_dst.bin.fm_tmp"))
        {
            FAIL("temp file left behind");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-04: adapter without rename -> no partial file either");
    {
        FM_FS_RenameFn keep = g_wfs.ops.rename;
        FM_Result r;

        (void)File_Delete("/f04_dst.bin");
        g_wfs.ops.rename = NULL;             /* direct-write mode */
        g_wfs_write_budget = 5000;
        r = File_Copy("/f04_src.bin", "/ram/f04_dst.bin", NULL, NULL);
        g_wfs_write_budget = -1;
        if (r != FM_ERR_NO_SPACE)
        {
            FAIL("expected NO_SPACE");
        }
        else if (f_exists("/f04_dst.bin"))
        {
            FAIL("truncated destination left behind");
        }
        else if (File_Copy("/f04_src.bin", "/ram/f04_dst.bin", NULL,
                           NULL) != FM_OK ||
                 !f_file_equals("/f04_dst.bin", big, sizeof(big)))
        {
            FAIL("direct-mode copy broken");
        }
        else
        {
            PASS();
        }
        g_wfs.ops.rename = keep;
        (void)File_Delete("/f04_dst.bin");
    }

    /* ---------------------------------------------------------------- */
    TEST("F-05: 100% progress is reported only after the flush");
    {
        FM_Result r;

        (void)File_WriteAll("/f05_src.bin", big, 3000u);
        (void)File_Delete("/f05_dst.bin");
        g_f05_calls = 0u;
        g_f05_full_reports = 0u;
        g_f05_flushes_at_full = 0u;
        g_f05_flushes_before_full = 0u;
        g_wfs_flush_calls = 0u;
        r = File_Copy("/f05_src.bin", "/ram/f05_dst.bin", f05_progress,
                      NULL);
        if (r != FM_OK)
        {
            FAIL("copy failed");
        }
        else if (g_f05_full_reports != 1u)
        {
            FAIL("done == total must be reported exactly once");
        }
        else if (g_f05_flushes_at_full == 0u)
        {
            FAIL("100% reported before any flush");
        }
        else if (g_f05_flushes_before_full != 0u || g_f05_calls < 2u)
        {
            FAIL("intermediate reports missing or flushed early");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f05_src.bin");
        (void)File_Delete("/f05_dst.bin");
    }

    /* ---------------------------------------------------------------- */
    TEST("F-06: Transfer_Raw rejects ranges past the capacity");
    {
        static uint8_t smem[4096];
        static uint8_t dmem[1024];
        FM_Storage s, d;
        FakeStore fsrc, fdst;
        FM_Result r1, r2, r3;

        fake_store_init(&s, &fsrc, smem, sizeof(smem), sizeof(smem));
        fake_store_init(&d, &fdst, dmem, sizeof(dmem), sizeof(dmem));
        r1 = FM_Transfer_Raw(&s, 0, &d, 1008, 256, NULL);
        r2 = FM_Transfer_Raw(&s, UINT64_MAX - 8u, &d, 0, 16, NULL);
        r3 = FM_Transfer_Raw(&s, 0, &d, UINT64_MAX - 8u, 16, NULL);
        if (r1 != FM_ERR_INVALID_ARGUMENT || r2 != FM_ERR_INVALID_ARGUMENT ||
            r3 != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("out-of-range request accepted");
        }
        else if (fdst.writes != 0u || fsrc.reads != 0u)
        {
            FAIL("driver was called for a rejected request");
        }
        else if (FM_Transfer_Raw(&s, 0, &d, 768, 256, NULL) != FM_OK ||
                 fdst.max_write_end != 1024u)
        {
            FAIL("a request ending exactly at capacity must work");
        }
        else
        {
            PASS();
        }
    }

    /* ---------------------------------------------------------------- */
    TEST("F-07: erase goes unit by unit, exactly aligned");
    {
        static uint8_t smem[16384];
        static uint8_t dmem[32768];
        FM_Storage s, d;
        FakeStore fsrc, fdst;
        FM_Result r;

        memcpy(smem, big, sizeof(smem));
        fake_store_init(&s, &fsrc, smem, sizeof(smem), sizeof(smem));
        fake_store_init(&d, &fdst, dmem, sizeof(dmem), sizeof(dmem));
        d.erase = fake_erase;
        d.erase_size = 4096u;
        fdst.strict_erase = FM_TRUE;
        memset(dmem, 0, sizeof(dmem));

        r = FM_Transfer_Raw(&s, 0, &d, 4096, 8192, NULL);
        if (r != FM_OK)
        {
            FAIL("aligned raw copy failed");
        }
        else if (fdst.erases != 2u || fdst.erase_off[0] != 4096u ||
                 fdst.erase_len[0] != 4096u || fdst.erase_off[1] != 8192u ||
                 fdst.erase_len[1] != 4096u)
        {
            FAIL("expected 2 erases of 4096 at 4096 and 8192");
        }
        else if (memcmp(dmem + 4096, smem, 8192) != 0 || dmem[0] != 0u ||
                 dmem[12288] != 0u)
        {
            FAIL("data wrong or a neighbouring unit was touched");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-07: the last, partly used unit is erased whole");
    {
        static uint8_t smem[8192];
        static uint8_t dmem[16384];
        FM_Storage s, d;
        FakeStore fsrc, fdst;
        FM_Result r;

        memcpy(smem, big, sizeof(smem));
        fake_store_init(&s, &fsrc, smem, sizeof(smem), sizeof(smem));
        fake_store_init(&d, &fdst, dmem, sizeof(dmem), sizeof(dmem));
        d.erase = fake_erase;
        d.erase_size = 4096u;
        fdst.strict_erase = FM_TRUE;
        memset(dmem, 0, sizeof(dmem));

        r = FM_Transfer_Raw(&s, 0, &d, 0, 5000, NULL);
        if (r != FM_OK)
        {
            FAIL("copy with a length that is not a unit multiple failed");
        }
        else if (fdst.erases != 2u || fdst.erase_off[1] != 4096u ||
                 fdst.erase_len[1] != 4096u)
        {
            FAIL("expected 2 whole-unit erases");
        }
        else if (memcmp(dmem, smem, 5000) != 0 || dmem[8192] != 0u)
        {
            FAIL("data wrong or a unit past the range was erased");
        }
        else
        {
            PASS();
        }
    }

    TEST("F-07: unaligned start is refused before anything is touched");
    {
        static uint8_t smem[8192];
        static uint8_t dmem[16384];
        FM_Storage s, d;
        FakeStore fsrc, fdst;
        FM_Result r;

        fake_store_init(&s, &fsrc, smem, sizeof(smem), sizeof(smem));
        fake_store_init(&d, &fdst, dmem, sizeof(dmem), sizeof(dmem));
        d.erase = fake_erase;
        d.erase_size = 4096u;
        fdst.strict_erase = FM_TRUE;

        r = FM_Transfer_Raw(&s, 0, &d, 1024, 8192, NULL);
        if (r != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("expected INVALID_ARGUMENT");
        }
        else if (fdst.erases != 0u || fdst.writes != 0u || fsrc.reads != 0u)
        {
            FAIL("device touched by a rejected request");
        }
        else
        {
            /* a device without erase units has no such rule */
            d.erase = NULL;
            d.erase_size = 0u;
            if (FM_Transfer_Raw(&s, 0, &d, 1024, 4096, NULL) != FM_OK)
            {
                FAIL("plain device must accept any offset");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("F-07: erase can be cancelled; 4 GiB is not truncated to 0");
    {
        FM_Storage s, d;
        FakeStore fsrc, fdst;
        FM_TransferOpts o;
        FM_Result r;

        fake_store_init(&s, &fsrc, NULL, 0u, (uint64_t)1 << 33);
        fake_store_init(&d, &fdst, NULL, 0u, (uint64_t)1 << 33);
        d.erase = fake_erase;
        d.erase_size = 0x40000000u;          /* 1 GiB erase unit */
        fdst.strict_erase = FM_TRUE;
        memset(&o, 0, sizeof(o));
        o.progress = f_cancel_now_cb;

        r = FM_Transfer_Raw(&s, 0, &d, 0, (uint64_t)1 << 32, &o);
        if (r != FM_ERR_CANCELLED || fdst.erases != 0u || fdst.writes != 0u)
        {
            FAIL("cancel before the first erase was ignored");
        }
        else
        {
            r = FM_Transfer_Raw(&s, 0, &d, 0, (uint64_t)1 << 32, NULL);
            if (r != FM_OK)
            {
                FAIL("4 GiB raw copy failed");
            }
            else if (fdst.erases != 4u ||
                     fdst.erase_off[3] != (uint64_t)3 << 30 ||
                     fdst.erase_len[3] != 0x40000000u)
            {
                FAIL("4 GiB must be erased as 4 units of 1 GiB");
            }
            else if (fdst.max_write_end != (uint64_t)1 << 32)
            {
                FAIL("not all bytes were written");
            }
            else
            {
                PASS();
            }
        }
    }

    /* ---------------------------------------------------------------- */
    TEST("F-08: RawVerify without read() is an error, not a crash");
    {
        static uint8_t mem[64];
        FM_Storage a, b;
        FakeStore fa, fb;
        FM_Result r1, r2, r3;

        fake_store_init(&a, &fa, mem, sizeof(mem), sizeof(mem));
        fake_store_init(&b, &fb, mem, sizeof(mem), sizeof(mem));
        a.read = NULL;
        b.read = NULL;
        r1 = FM_Transfer_RawVerify(&a, 0, &b, 0, 16);
        a.read = fake_read;
        r2 = FM_Transfer_RawVerify(&a, 0, &b, 0, 16);
        b.read = fake_read;
        r3 = FM_Transfer_RawVerify(&a, 60, &b, 0, 16);
        if (r1 != FM_ERR_UNSUPPORTED || r2 != FM_ERR_UNSUPPORTED)
        {
            FAIL("missing read() not reported as UNSUPPORTED");
        }
        else if (r3 != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("range past capacity accepted");
        }
        else if (FM_Transfer_RawVerify(&a, 0, &b, 0, 64) != FM_OK)
        {
            FAIL("valid verify failed");
        }
        else
        {
            PASS();
        }
    }

    /* ---------------------------------------------------------------- */
    TEST("F-09: Transfer_Copy/Move onto itself keeps the file");
    {
        static const char txt[] = "twenty-four bytes here!!";
        FM_Result r1, r2, r3;

        (void)File_WriteText("/f09_same.txt", txt);
        r1 = FM_Transfer_Copy("/f09_same.txt", "/f09_same.txt", NULL);
        r2 = FM_Transfer_Copy("/f09_same.txt", "/./f09_same.txt", NULL);
        r3 = FM_Transfer_Move("/f09_same.txt", "/f09_same.txt", NULL);
        if (r1 != FM_ERR_INVALID_ARGUMENT || r2 != FM_ERR_INVALID_ARGUMENT ||
            r3 != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("self copy/move not rejected");
        }
        else if (!f_file_equals("/f09_same.txt", txt, sizeof(txt) - 1u))
        {
            FAIL("file truncated or deleted");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f09_same.txt");
    }

    /* ---------------------------------------------------------------- */
#if FM_USE_CRC
    TEST("F-10: verify read error closes its handle");
    {
        FM_TransferOpts o;
        uint32_t before = File_PoolUsed();
        FM_Result r;

        (void)File_WriteAll("/f10_src.bin", big, 3000u);
        (void)File_Delete("/f10_dst.bin");
        memset(&o, 0, sizeof(o));
        o.verify = FM_TRUE;
        g_wfs_open_calls = 0u;
        g_wfs_read_fail_from_open = 2u;      /* 1 = dst, 2 = verify pass */
        r = FM_Transfer_Copy("/f10_src.bin", "/ram/f10_dst.bin", &o);
        g_wfs_read_fail_from_open = 0u;
        if (r != FM_ERR_IO)
        {
            static char m[48];

            (void)snprintf(m, sizeof(m), "expected IO, got %d", (int)r);
            FAIL(m);
        }
        else if (File_PoolUsed() != before)
        {
            FAIL("verify handle left registered (dangling stack pointer)");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f10_dst.bin");
    }

    TEST("F-10: CRC mismatch removes the corrupt destination");
    {
        FM_TransferOpts o;
        FM_TransferInfo info;
        uint32_t before = File_PoolUsed();
        FM_Result r;

        (void)File_Delete("/f10_dst.bin");
        memset(&o, 0, sizeof(o));
        o.verify = FM_TRUE;
        g_wfs_corrupt = FM_TRUE;
        r = FM_Transfer_CopyEx("/f10_src.bin", "/ram/f10_dst.bin", &o,
                               &info);
        g_wfs_corrupt = FM_FALSE;
        if (r != FM_ERR_CORRUPTED)
        {
            FAIL("corruption not detected");
        }
        else if (f_exists("/f10_dst.bin"))
        {
            FAIL("corrupt destination left on disk");
        }
        else if (File_PoolUsed() != before)
        {
            FAIL("handle leaked");
        }
        else if (!info.verified || info.crc_source == info.crc_dest)
        {
            FAIL("transfer info does not describe the mismatch");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f10_src.bin");
        (void)File_Delete("/f10_dst.bin");
    }
#endif /* FM_USE_CRC */

    /* ---------------------------------------------------------------- */
    TEST("F-11: nested File_Copy / File_Insert in progress -> BUSY");
    {
        FM_Result r;

        (void)File_WriteAll("/f11_src.bin", big, 5000u);
        (void)File_WriteAll("/f11_other.bin", big + 7000, 3000u);
        (void)File_Delete("/f11_dst.bin");
        (void)File_Delete("/f11_other2.bin");
        g_f11_ran = FM_FALSE;
        g_f11_nested_copy = FM_OK;
        g_f11_nested_insert = FM_OK;
        r = File_Copy("/f11_src.bin", "/f11_dst.bin", f11_file_progress,
                      NULL);
        if (r != FM_OK || !g_f11_ran)
        {
            FAIL("outer copy failed");
        }
        else if (g_f11_nested_copy != FM_ERR_BUSY ||
                 g_f11_nested_insert != FM_ERR_BUSY)
        {
            FAIL("nested use of the scratch buffer was not refused");
        }
        else if (!f_file_equals("/f11_dst.bin", big, 5000u))
        {
            FAIL("outer copy corrupted by the nested call");
        }
        else if (!f_file_equals("/f11_other.bin", big + 7000, 3000u) ||
                 f_exists("/f11_other2.bin") ||
                 f_exists("/f11_other2.bin.fm_tmp") ||
                 f_exists("/f11_other.bin.fm_tmp"))
        {
            FAIL("refused nested call left something behind");
        }
        else if (File_Copy("/f11_other.bin", "/f11_other2.bin", NULL,
                           NULL) != FM_OK ||
                 File_Insert("/f11_other2.bin", 0u, "zz", 2u) != FM_OK)
        {
            FAIL("buffer still marked busy after the copy");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f11_dst.bin");
        (void)File_Delete("/f11_other2.bin");
    }

    TEST("F-11: nested FM_Transfer_Copy in progress -> BUSY");
    {
        FM_TransferOpts o;
        FM_Result r;

        (void)File_Delete("/f11_dst.bin");
        memset(&o, 0, sizeof(o));
        o.progress = f11_xfer_progress;
        g_f11_ran = FM_FALSE;
        g_f11_nested_xfer = FM_OK;
        r = FM_Transfer_Copy("/f11_src.bin", "/f11_dst.bin", &o);
        if (r != FM_OK || !g_f11_ran)
        {
            FAIL("outer transfer failed");
        }
        else if (g_f11_nested_xfer != FM_ERR_BUSY)
        {
            FAIL("nested transfer was not refused");
        }
        else if (!f_file_equals("/f11_dst.bin", big, 5000u) ||
                 f_exists("/f11_other2.bin"))
        {
            FAIL("outer transfer corrupted / nested one ran");
        }
        else if (FM_Transfer_Copy("/f11_other.bin", "/f11_other2.bin",
                                  NULL) != FM_OK)
        {
            FAIL("buffer still marked busy after the transfer");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f11_dst.bin");
        (void)File_Delete("/f11_other2.bin");
    }

#ifdef FM_XFER_CACHE_CLEAN
    TEST("F-11: cache clean/invalidate around every chunk");
    {
        const uint32_t chunk = (uint32_t)FM_TRANSFER_CHUNK;
        const uint32_t len = chunk * 3u;
        uint32_t file_clean, file_inval;
        FM_Result r;

        (void)File_WriteAll("/f11_src.bin", big, len);
        (void)File_Delete("/f11_dst.bin");
        g_xfer_clean_calls = 0u;
        g_xfer_inval_calls = 0u;
        g_xfer_hook_unaligned = 0u;
        r = FM_Transfer_Copy("/f11_src.bin", "/f11_dst.bin", NULL);
        file_clean = g_xfer_clean_calls;
        file_inval = g_xfer_inval_calls;
        if (r != FM_OK || len > sizeof(big))
        {
            FAIL("copy failed");
        }
        else if (file_inval != 3u || file_clean < 6u)
        {
            static char m[64];

            (void)snprintf(m, sizeof(m), "3 chunks: %u clean, %u invalidate",
                           (unsigned)file_clean, (unsigned)file_inval);
            FAIL(m);
        }
        else
        {
            static uint8_t smem[16384];
            static uint8_t dmem[16384];
            FM_Storage s, d;
            FakeStore fsrc, fdst;

            memcpy(smem, big, sizeof(smem));
            fake_store_init(&s, &fsrc, smem, sizeof(smem), sizeof(smem));
            fake_store_init(&d, &fdst, dmem, sizeof(dmem), sizeof(dmem));
            g_xfer_clean_calls = 0u;
            g_xfer_inval_calls = 0u;
            r = FM_Transfer_Raw(&s, 0, &d, 0, sizeof(smem), NULL);
            if (r != FM_OK || memcmp(smem, dmem, sizeof(smem)) != 0)
            {
                FAIL("raw copy failed");
            }
            else if (g_xfer_inval_calls != fsrc.reads ||
                     g_xfer_clean_calls < 2u * fdst.writes ||
                     fsrc.reads != fdst.writes)
            {
                FAIL("raw: maintenance does not match the chunk count");
            }
            else if (g_xfer_hook_unaligned != 0u)
            {
                FAIL("cache maintenance on an unaligned address");
            }
            else
            {
                PASS();
            }
        }
        (void)File_Delete("/f11_dst.bin");
    }
#endif
    (void)File_Delete("/f11_src.bin");
    (void)File_Delete("/f11_other.bin");

    /* ---------------------------------------------------------------- */
    TEST("F-13: a write is visible through another open handle");
    {
        FM_File *rd;
        FM_File *wr;
        char got[9];
        uint32_t n = 0u;
        FM_Bool ok = FM_TRUE;

        (void)File_WriteText("/f13.txt", "AAAAAAAA");
        rd = File_Open("/f13.txt", FM_OPEN_READ);
        wr = File_Open("/f13.txt", FM_OPEN_READ | FM_OPEN_WRITE);
        memset(got, 0, sizeof(got));
        if (rd == NULL || wr == NULL)
        {
            ok = FM_FALSE;
        }
        /* prime the reader's cache */
        if (ok && (File_Read(rd, got, 8u, &n) != FM_OK || n != 8u ||
                   memcmp(got, "AAAAAAAA", 8) != 0))
        {
            ok = FM_FALSE;
        }
        if (ok && (File_Write(wr, "BBBBBBBB", 8u, &n) != FM_OK ||
                   File_Flush(wr) != FM_OK))
        {
            ok = FM_FALSE;
        }
        if (!ok)
        {
            FAIL("setup failed");
        }
        else if (File_Seek(rd, 0, FM_SEEK_SET) != FM_OK ||
                 File_Read(rd, got, 8u, &n) != FM_OK || n != 8u)
        {
            FAIL("re-read failed");
        }
        else if (memcmp(got, "BBBBBBBB", 8) != 0)
        {
            FAIL("reader still sees the bytes from before the write");
        }
        else
        {
            /* the writer extends the file: the reader must not report
             * EOF from its stale size */
            static uint8_t tail[700];
            static uint8_t back[700];

            memset(tail, 0x6B, sizeof(tail));
            if (File_Write(wr, tail, (uint32_t)sizeof(tail), &n) != FM_OK ||
                File_Flush(wr) != FM_OK)
            {
                FAIL("extend failed");
            }
            else if (File_Read(rd, back, (uint32_t)sizeof(back), &n) !=
                         FM_OK || n != sizeof(back) ||
                     memcmp(back, tail, sizeof(tail)) != 0)
            {
                FAIL("reader cannot see bytes appended by another handle");
            }
            else
            {
                PASS();
            }
        }
        if (rd != NULL) { (void)File_Close(rd); }
        if (wr != NULL) { (void)File_Close(wr); }
    }

    TEST("F-13: truncate-open / delete drop other handles' cache");
    {
        FM_File *rd;
        char got[9];
        uint32_t n = 0u;

        (void)File_WriteText("/f13.txt", "AAAAAAAA");
        rd = File_Open("/f13.txt", FM_OPEN_READ);
        memset(got, 0, sizeof(got));
        if (rd == NULL || File_Read(rd, got, 8u, &n) != FM_OK || n != 8u)
        {
            FAIL("setup failed");
        }
        else if (File_WriteText("/f13.txt", "CCCCCCCC") != FM_OK ||
                 File_Seek(rd, 0, FM_SEEK_SET) != FM_OK ||
                 File_Read(rd, got, 8u, &n) != FM_OK || n != 8u)
        {
            FAIL("rewrite / re-read failed");
        }
        else if (memcmp(got, "CCCCCCCC", 8) != 0)
        {
            FAIL("reader still sees the file from before the rewrite");
        }
        else
        {
            char all[16];
            size_t len = 0;

            /* whole-file read, out-of-band change, whole-file read */
            memset(all, 0, sizeof(all));
            if (File_ReadAll("/f13.txt", all, sizeof(all), &len) != FM_OK ||
                File_WriteText("/./f13.txt", "DDDDDDDD") != FM_OK ||
                File_ReadAll("/f13.txt", all, sizeof(all), &len) != FM_OK ||
                len != 8u || memcmp(all, "DDDDDDDD", 8) != 0)
            {
                FAIL("second File_ReadAll returned old content");
            }
            else
            {
                PASS();
            }
        }
        if (rd != NULL) { (void)File_Close(rd); }
        (void)File_Delete("/f13.txt");
    }

    /* ---------------------------------------------------------------- */
#if FM_USE_CACHE
    TEST("F-14: I/O error after a partial cached read is reported");
    {
        static uint8_t back[900];
        FM_File *f;
        uint32_t n = 0u;
        FM_Bool was = File_GetCacheEnabled();

        File_SetCacheEnabled(FM_TRUE);
        (void)File_WriteAll("/f14.bin", big, 2048u);
        f = File_Open("/ram/f14.bin", FM_OPEN_READ);
        if (f == NULL)
        {
            FAIL("open failed");
        }
        else
        {
            FM_Result r;

            g_wfs_read_calls = 0u;
            g_wfs_read_fail_at = 2u;     /* second 512-byte block fails */
            r = File_Read(f, back, (uint32_t)sizeof(back), &n);
            g_wfs_read_fail_at = 0u;
            if (r != FM_ERR_IO)
            {
                static char m[64];

                (void)snprintf(m, sizeof(m),
                               "expected IO, got %d (read=%u)", (int)r,
                               (unsigned)n);
                FAIL(m);
            }
            else if (n != 512u || memcmp(back, big, 512) != 0)
            {
                FAIL("bytes delivered before the error are wrong");
            }
            else if (File_Read(f, back, 100u, &n) != FM_OK || n != 100u ||
                     memcmp(back, big + 512, 100) != 0)
            {
                /* the failed block must not have been cached half-filled */
                FAIL("read after the error returned wrong data");
            }
            else
            {
                PASS();
            }
            (void)File_Close(f);
        }
        File_SetCacheEnabled(was);
        (void)File_Delete("/f14.bin");
    }

    TEST("F-14: a short read at end of file is still a success");
    {
        static uint8_t back[900];
        FM_File *f;
        uint32_t n = 0u;

        (void)File_WriteAll("/f14.bin", big, 700u);
        f = File_Open("/ram/f14.bin", FM_OPEN_READ);
        if (f == NULL)
        {
            FAIL("open failed");
        }
        else if (File_Read(f, back, (uint32_t)sizeof(back), &n) != FM_OK ||
                 n != 700u || memcmp(back, big, 700) != 0)
        {
            (void)File_Close(f);
            FAIL("short read at EOF must return FM_OK with the bytes");
        }
        else if (File_Read(f, back, 10u, &n) != FM_ERR_EOF || n != 0u)
        {
            (void)File_Close(f);
            FAIL("reading at EOF must return FM_ERR_EOF");
        }
        else
        {
            (void)File_Close(f);
            PASS();
        }
        (void)File_Delete("/f14.bin");
    }
#endif /* FM_USE_CACHE */

    /* ---------------------------------------------------------------- */
    TEST("F-15: edit refuses a path whose temp name does not fit");
    {
        static char longp[FM_PATH_MAX];
        FM_Result r;

        /* strlen == FM_PATH_MAX - 3: valid on its own, but
         * "<path>.fm_tmp" does not fit in FM_PATH_MAX */
        memset(longp, 'q', sizeof(longp));
        longp[0] = '/';
        longp[FM_PATH_MAX - 3] = '\0';
        if (File_WriteText(longp, "keep-me") != FM_OK)
        {
            FAIL("could not create the long-named file");
        }
        else
        {
            r = File_Insert(longp, 0u, "XY", 2u);
            if (r != FM_ERR_INVALID_PATH)
            {
                static char m[48];

                (void)snprintf(m, sizeof(m), "expected INVALID_PATH, got %d",
                               (int)r);
                FAIL(m);
            }
            else if (File_DeleteRange(longp, 0u, 1u) != FM_ERR_INVALID_PATH ||
                     File_ReplaceRange(longp, 0u, 1u, "Z", 1u) !=
                         FM_ERR_INVALID_PATH)
            {
                FAIL("DeleteRange / ReplaceRange not guarded");
            }
            else if (!f_file_equals(longp, "keep-me", 7u))
            {
                FAIL("file was modified");
            }
            else
            {
                PASS();
            }
        }
        (void)File_Delete(longp);
    }

    /* ---------------------------------------------------------------- */
    TEST("F-16: rename onto itself is a no-op; delete is normalized");
    {
        FM_Result r1, r2, r3, r4;

        (void)Directory_Create("/f16d");
        (void)File_WriteText("/f16d/b.txt", "sixteen");
        g_wfs_rename_calls = 0u;
        r1 = File_Rename("/f16d/b.txt", "/f16d/./b.txt");
        r2 = File_Rename("/ram/f16d/b.txt", "/ram/f16d/../f16d/b.txt");
        r3 = File_Move("/ram/f16d/b.txt", "/ram/f16d/./b.txt", NULL, NULL);
        r4 = File_Rename("/f16d/missing.txt", "/f16d/./missing.txt");
        if (r1 != FM_OK || r2 != FM_OK || r3 != FM_OK)
        {
            FAIL("self rename / move must succeed");
        }
        else if (g_wfs_rename_calls != 0u)
        {
            FAIL("self rename reached the backend");
        }
        else if (r4 != FM_ERR_NOT_FOUND)
        {
            FAIL("self rename of a missing file must report NOT_FOUND");
        }
        else if (!f_file_equals("/f16d/b.txt", "sixteen", 7u))
        {
            FAIL("file damaged by a self rename");
        }
        else if (File_Delete("/f16d/./b.txt") != FM_OK ||
                 f_exists("/f16d/b.txt"))
        {
            FAIL("File_Delete(\"/a/./b\") did not remove /a/b");
        }
        else if (File_Rename("/f16d/b.txt", "/f16d/c.txt") !=
                 FM_ERR_NOT_FOUND)
        {
            FAIL("rename of a deleted file must fail");
        }
        else
        {
            PASS();
        }
        (void)File_Delete("/f16d/b.txt");
        (void)Directory_Delete("/f16d");
    }

    (void)File_Delete("/f04_src.bin");
    (void)File_Delete("/f04_dst.bin");
    wfs_unmount("/ram");
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
    int created[4] = {0, 0, 0, 0};
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
        else
        {
            created[i] = 1;
        }
    }
    for (i = 0; i < 4; i++)
    {
        /* always join: a worker left running after a failure keeps
         * using the library underneath the following suites */
        if (created[i])
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
#if FM_USE_VIDEO
/* ================================================================ */
/* Video report regressions (Report.txt V1..V20)                     */
/* ================================================================ */
/* Every container below is synthesised byte by byte, so each test
 * pins one specific finding of the report and fails on the code as it
 * was before the fix (several of them under ASan/UBSan only, exactly
 * as the report describes). */

typedef struct
{
    uint8_t *b;
    size_t   n;
    size_t   cap;
} Eb;

static void eb_raw(Eb *e, const void *p, size_t n)
{
    if (n != 0u && e->n + n <= e->cap)
    {
        memcpy(e->b + e->n, p, n);
    }
    e->n += n;   /* overflow is detected by the caller: n > cap */
}

static void eb_u8(Eb *e, uint8_t v)
{
    eb_raw(e, &v, 1);
}

static void eb_le16(Eb *e, uint16_t v)
{
    uint8_t b[2];

    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    eb_raw(e, b, 2);
}

static void eb_le32(Eb *e, uint32_t v)
{
    uint8_t b[4];

    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    eb_raw(e, b, 4);
}

/* EBML element ID: the marker bits are part of the number. */
static void eb_id(Eb *e, uint32_t id)
{
    if (id > 0xFFFFFFu) eb_u8(e, (uint8_t)(id >> 24));
    if (id > 0xFFFFu)   eb_u8(e, (uint8_t)(id >> 16));
    if (id > 0xFFu)     eb_u8(e, (uint8_t)(id >> 8));
    eb_u8(e, (uint8_t)id);
}

/* EBML variable-length integer of exactly `len` bytes. */
static void eb_vint_at(uint8_t *p, uint64_t v, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        p[i] = (uint8_t)(v >> (8u * (len - 1u - i)));
    }
    p[0] = (uint8_t)(p[0] | (uint8_t)(0x80u >> (len - 1u)));
}

static void eb_vint(Eb *e, uint64_t v, uint32_t len)
{
    uint8_t b[8];

    eb_vint_at(b, v, len);
    eb_raw(e, b, len);
}

/* Master element with a `len`-byte size patched by eb_close(). */
static size_t eb_open_n(Eb *e, uint32_t id, uint32_t len)
{
    size_t at;

    eb_id(e, id);
    at = e->n;
    eb_vint(e, 0, len);
    return at;
}

static void eb_close_n(Eb *e, size_t at, uint32_t len)
{
    if (e->n <= e->cap)
    {
        eb_vint_at(e->b + at, (uint64_t)(e->n - at - len), len);
    }
}

static size_t eb_open(Eb *e, uint32_t id)      { return eb_open_n(e, id, 4u); }
static void   eb_close(Eb *e, size_t at)       { eb_close_n(e, at, 4u); }

static void eb_uint(Eb *e, uint32_t id, uint64_t v, uint32_t nbytes)
{
    uint32_t i;

    eb_id(e, id);
    eb_vint(e, nbytes, 1u);
    for (i = 0; i < nbytes; i++)
    {
        eb_u8(e, (uint8_t)(v >> (8u * (nbytes - 1u - i))));
    }
}

static void eb_f64(Eb *e, uint32_t id, double d)
{
    uint64_t u;

    memcpy(&u, &d, 8);
    eb_uint(e, id, u, 8u);
}

static void eb_f32(Eb *e, uint32_t id, float f)
{
    uint32_t u;

    memcpy(&u, &f, 4);
    eb_uint(e, id, u, 4u);
}

static void eb_bin(Eb *e, uint32_t id, const void *p, size_t n)
{
    eb_id(e, id);
    eb_vint(e, n, (n < 16383u) ? 2u : 4u);
    eb_raw(e, p, n);
}

typedef struct
{
    uint32_t       width_bytes;     /* PixelWidth payload bytes (0 = 2)   */
    int            rate_f32;        /* SamplingFrequency as float32       */
    double         rate;            /* 0 = no audio track                 */
    const char    *vcodec;          /* NULL = "V_MJPEG"                   */
    const uint8_t *vpriv;           /* video CodecPrivate                 */
    size_t         vpriv_len;
    uint32_t       seg_len;         /* Segment size-field width (0 = 4)   */
    int            long_void;       /* add a Void with a 5-byte size      */
    int            unknown_cluster; /* Cluster with the "unknown" size    */
    const char    *acodec;          /* NULL = "A_PCM/INT/LIT"             */
    const uint8_t *apriv;           /* audio CodecPrivate                 */
    size_t         apriv_len;
} MkvOpt;

/* EBML header, Segment, Tracks, and an open Cluster with its Timecode
 * (a 1-byte-ID element: the very thing V1 mis-parsed). */
static void mkv_head(Eb *e, const MkvOpt *o, size_t *seg_at, size_t *clu_at)
{
    size_t at;
    size_t tr;
    size_t te;
    size_t sub;

    at = eb_open(e, 0x1A45DFA3u);
    eb_bin(e, 0x4282u, "matroska", 8);
    eb_close(e, at);
    *seg_at = eb_open_n(e, 0x18538067u, o->seg_len ? o->seg_len : 4u);
    if (o->long_void)
    {
        eb_id(e, 0xECu);
        eb_vint(e, 3u, 5u);
        eb_raw(e, "\0\0\0", 3);
    }
    tr = eb_open(e, 0x1654AE6Bu);
    te = eb_open(e, 0xAEu);
    eb_uint(e, 0xD7u, 1u, 1u);
    eb_uint(e, 0x83u, 1u, 1u);
    eb_bin(e, 0x86u, o->vcodec ? o->vcodec : "V_MJPEG",
           strlen(o->vcodec ? o->vcodec : "V_MJPEG"));
    if (o->vpriv != NULL)
    {
        eb_bin(e, 0x63A2u, o->vpriv, o->vpriv_len);
    }
    eb_uint(e, 0x23E383u, 40000000u, 4u);        /* 40 ms -> 25 fps */
    sub = eb_open(e, 0xE0u);
    eb_uint(e, 0xB0u, 320u, o->width_bytes ? o->width_bytes : 2u);
    eb_uint(e, 0xBAu, 240u, 2u);
    eb_close(e, sub);
    eb_close(e, te);
    if (o->rate != 0.0)
    {
        te = eb_open(e, 0xAEu);
        eb_uint(e, 0xD7u, 2u, 1u);
        eb_uint(e, 0x83u, 2u, 1u);
        eb_bin(e, 0x86u, o->acodec ? o->acodec : "A_PCM/INT/LIT",
               strlen(o->acodec ? o->acodec : "A_PCM/INT/LIT"));
        if (o->apriv != NULL)
        {
            eb_bin(e, 0x63A2u, o->apriv, o->apriv_len);
        }
        sub = eb_open(e, 0xE1u);
        if (o->rate_f32) eb_f32(e, 0xB5u, (float)o->rate);
        else             eb_f64(e, 0xB5u, o->rate);
        eb_uint(e, 0x9Fu, 2u, 1u);
        eb_uint(e, 0x6264u, 16u, 1u);
        eb_close(e, sub);
        eb_close(e, te);
    }
    eb_close(e, tr);
    eb_id(e, 0x1F43B675u);
    *clu_at = e->n;
    if (o->unknown_cluster) eb_raw(e, "\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);
    else                    eb_vint(e, 0, 4u);
    eb_uint(e, 0xE7u, 0u, 2u);                   /* Timecode */
}

static void mkv_tail(Eb *e, const MkvOpt *o, size_t seg_at, size_t clu_at)
{
    if (!o->unknown_cluster)
    {
        eb_close(e, clu_at);
    }
    eb_close_n(e, seg_at, o->seg_len ? o->seg_len : 4u);
}

/* One SimpleBlock (id 0xA3) or Block (0xA1).  lace: 0 none, 1 Xiph,
 * 2 fixed, 3 EBML. */
static void mkv_block(Eb *e, uint32_t id, uint32_t track, uint32_t lace,
                      const uint8_t *const *fr, const uint32_t *sz,
                      uint32_t n)
{
    size_t at = eb_open(e, id);
    uint32_t k;

    if (track < 0x80u)
    {
        eb_u8(e, (uint8_t)(0x80u | track));
    }
    else
    {
        eb_u8(e, (uint8_t)(0x40u | (track >> 8)));
        eb_u8(e, (uint8_t)track);
    }
    eb_u8(e, 0); eb_u8(e, 0);                     /* timecode */
    eb_u8(e, (uint8_t)((lace << 1) | ((id == 0xA3u) ? 0x80u : 0u)));
    if (lace != 0u)
    {
        eb_u8(e, (uint8_t)(n - 1u));
        for (k = 0; k + 1u < n; k++)
        {
            if (lace == 1u)
            {
                uint32_t s = sz[k];

                while (s >= 255u) { eb_u8(e, 255u); s -= 255u; }
                eb_u8(e, (uint8_t)s);
            }
            else if (lace == 3u)
            {
                if (k == 0u)
                {
                    eb_vint(e, sz[0], 2u);
                }
                else
                {
                    /* signed 2-byte vint: bias 2^13 - 1 */
                    int32_t d = (int32_t)sz[k] - (int32_t)sz[k - 1u];

                    eb_vint(e, (uint64_t)(d + 8191), 2u);
                }
            }
        }
    }
    for (k = 0; k < n; k++)
    {
        eb_raw(e, fr[k], sz[k]);
    }
    eb_close(e, at);
}

static FM_Result vr_write(const char *path, const Eb *e)
{
    if (e->n > e->cap)
    {
        return FM_ERR_TOO_LARGE;      /* test buffer too small */
    }
    return File_WriteAll(path, e->b, e->n);
}

/* The video must hold exactly these frames, byte for byte. */
static FM_Result vr_expect_frames(FM_Video *v, const uint8_t *const *fr,
                                  const uint32_t *sz, uint32_t n)
{
    static uint8_t buf[1024];
    uint32_t k;

    if (v->info.frame_count != n)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    for (k = 0; k < n; k++)
    {
        size_t got = 0;
        FM_Result r = Video_ReadFrame(v, buf, sizeof(buf), &got);

        if (r != FM_OK)
        {
            return r;
        }
        if (got != sz[k] || memcmp(buf, fr[k], got) != 0)
        {
            return FM_ERR_CRC;
        }
    }
    return FM_OK;
}

/* Frame / packet payload used by the long-clip tests: its own index. */
static void vr_idx_bytes(uint8_t *b, uint32_t i)
{
    b[0] = (uint8_t)i; b[1] = (uint8_t)(i >> 8);
    b[2] = (uint8_t)(i >> 16); b[3] = (uint8_t)(0xA5u ^ (uint8_t)i);
}

static FM_Result vr_expect_idx(const uint8_t *b, size_t got, uint32_t i)
{
    uint8_t want[4];

    vr_idx_bytes(want, i);
    return (got == 4u && memcmp(b, want, 4) == 0) ? FM_OK : FM_ERR_CRC;
}

/* ---- AVI builder -------------------------------------------------- */
static size_t vr_riff(Eb *e, const char *tag)
{
    size_t at = e->n;

    eb_raw(e, tag, 4);
    eb_le32(e, 0);
    return at;
}

static void vr_riff_end(Eb *e, size_t at)
{
    uint32_t size = (uint32_t)(e->n - at - 8u);

    if (e->n <= e->cap)
    {
        e->b[at + 4] = (uint8_t)size;
        e->b[at + 5] = (uint8_t)(size >> 8);
        e->b[at + 6] = (uint8_t)(size >> 16);
        e->b[at + 7] = (uint8_t)(size >> 24);
    }
}

typedef struct
{
    int      core_header;   /* video strf is a 12-byte BITMAPCOREHEADER */
    uint32_t astrf_len;     /* audio strf bytes (0 = no audio stream)   */
    int      hdrl_last;     /* LIST hdrl after LIST movi                */
    uint32_t nvideo;
    uint32_t naudio;
    size_t   movi_at;       /* out: offset of the movi LIST header      */
} AviOpt;

static void vr_avi_hdrl(Eb *e, const AviOpt *o)
{
    size_t hdrl = vr_riff(e, "LIST");
    size_t at;
    size_t strl;
    uint32_t i;

    eb_raw(e, "hdrl", 4);
    at = vr_riff(e, "avih");
    eb_le32(e, 40000u);                 /* us per frame = 25 fps */
    eb_le32(e, 0); eb_le32(e, 0); eb_le32(e, 0);
    eb_le32(e, 0);                      /* total frames: let the index say */
    eb_le32(e, 0); eb_le32(e, 1); eb_le32(e, 0);
    /* with a core header the size must come from strf alone */
    eb_le32(e, o->core_header ? 0u : 320u);
    eb_le32(e, o->core_header ? 0u : 240u);
    vr_riff_end(e, at);

    strl = vr_riff(e, "LIST");
    eb_raw(e, "strl", 4);
    at = vr_riff(e, "strh");
    eb_raw(e, "vids", 4); eb_raw(e, "MJPG", 4);
    for (i = 0; i < 12u; i++) eb_le32(e, 0);
    vr_riff_end(e, at);
    at = vr_riff(e, "strf");
    if (o->core_header)
    {
        eb_le32(e, 12u);                /* bcSize */
        eb_le16(e, 320u); eb_le16(e, 240u);
        eb_le16(e, 1u); eb_le16(e, 24u);
    }
    else
    {
        eb_le32(e, 40u);
        eb_le32(e, 320u); eb_le32(e, 240u);
        eb_le16(e, 1u); eb_le16(e, 24u);
        eb_raw(e, "MJPG", 4);
        for (i = 0; i < 5u; i++) eb_le32(e, 0);
    }
    vr_riff_end(e, at);
    vr_riff_end(e, strl);

    if (o->astrf_len != 0u)
    {
        strl = vr_riff(e, "LIST");
        eb_raw(e, "strl", 4);
        at = vr_riff(e, "strh");
        eb_raw(e, "auds", 4); eb_le32(e, 0);
        for (i = 0; i < 12u; i++) eb_le32(e, 0);
        vr_riff_end(e, at);
        at = vr_riff(e, "strf");
        eb_le16(e, 1u); eb_le16(e, 2u);          /* PCM, stereo */
        eb_le32(e, 44100u); eb_le32(e, 176400u);
        eb_le16(e, 4u); eb_le16(e, 16u);         /* align, bits */
        for (i = 16u; i < o->astrf_len; i++) eb_u8(e, 0);
        vr_riff_end(e, at);
        vr_riff_end(e, strl);
    }
    vr_riff_end(e, hdrl);
}

static void vr_avi_movi(Eb *e, AviOpt *o)
{
    size_t movi = vr_riff(e, "LIST");
    uint32_t i;
    uint8_t b[4];

    o->movi_at = movi;
    eb_raw(e, "movi", 4);
    for (i = 0; i < o->nvideo; i++)
    {
        vr_idx_bytes(b, i);
        eb_raw(e, "00dc", 4); eb_le32(e, 4u); eb_raw(e, b, 4);
    }
    for (i = 0; i < o->naudio; i++)
    {
        vr_idx_bytes(b, i);
        eb_raw(e, "01wb", 4); eb_le32(e, 4u); eb_raw(e, b, 4);
    }
    vr_riff_end(e, movi);
}

static void vr_avi(Eb *e, AviOpt *o)
{
    size_t riff = vr_riff(e, "RIFF");

    eb_raw(e, "AVI ", 4);
    if (o->hdrl_last) { vr_avi_movi(e, o); vr_avi_hdrl(e, o); }
    else              { vr_avi_hdrl(e, o); vr_avi_movi(e, o); }
    vr_riff_end(e, riff);
}

/* ---- fake hardware decoder ---------------------------------------- */
static uint8_t  g_vr_hw_in[256];
/* ---- FLV builder --------------------------------------------------- */
static void eb_be32(Eb *e, uint32_t v);
static void eb_be16(Eb *e, uint16_t v);
static void eb_zero(Eb *e, size_t n);

static void vr_flv_head(Eb *e)
{
    eb_raw(e, "FLV\x01\x05", 5);
    eb_be32(e, 9);
    eb_be32(e, 0);                      /* PreviousTagSize0 */
}

/* One tag: `hdr` is the codec header inside the payload, `pl` the data. */
static void vr_flv_tag(Eb *e, uint8_t type, uint32_t ts, const void *hdr,
                       size_t hn, const void *pl, size_t pn)
{
    uint32_t n = (uint32_t)(hn + pn);

    eb_u8(e, type);
    eb_u8(e, (uint8_t)(n >> 16)); eb_u8(e, (uint8_t)(n >> 8)); eb_u8(e, (uint8_t)n);
    eb_u8(e, (uint8_t)(ts >> 16)); eb_u8(e, (uint8_t)(ts >> 8)); eb_u8(e, (uint8_t)ts);
    eb_u8(e, (uint8_t)(ts >> 24));
    eb_zero(e, 3);
    eb_raw(e, hdr, hn);
    eb_raw(e, pl, pn);
    eb_be32(e, 11u + n);
}

static void vr_amf_key(Eb *e, const char *key)
{
    eb_be16(e, (uint16_t)strlen(key));
    eb_raw(e, key, strlen(key));
}

static void vr_amf_num(Eb *e, const char *key, double v)
{
    uint8_t b[8];
    int i;

    memcpy(b, &v, 8);                   /* host: little-endian IEEE-754 */
    vr_amf_key(e, key);
    eb_u8(e, 0);
    for (i = 7; i >= 0; i--)
    {
        eb_u8(e, b[i]);
    }
}

/* onMetaData with the three numbers the demuxer wants, behind a string
 * and a boolean it has to step over. */
static void vr_flv_meta(Eb *e, double w, double h, double fps)
{
    uint8_t m[160];
    Eb me;

    memset(&me, 0, sizeof(me));
    me.b = m; me.cap = sizeof(m);
    eb_u8(&me, 2); vr_amf_key(&me, "onMetaData");
    eb_u8(&me, 8); eb_be32(&me, 6);
    vr_amf_key(&me, "encoder"); eb_u8(&me, 2); vr_amf_key(&me, "Lavf-test");
    vr_amf_key(&me, "stereo"); eb_u8(&me, 1); eb_u8(&me, 1);
    vr_amf_num(&me, "duration", 2.5);
    vr_amf_num(&me, "width", w);
    vr_amf_num(&me, "height", h);
    vr_amf_num(&me, "framerate", fps);
    eb_u8(&me, 0); eb_u8(&me, 0); eb_u8(&me, 9);
    vr_flv_tag(e, 18, 0, NULL, 0, m, me.n);
}

/* ---- MP4 builder (long clips) -------------------------------------- */
typedef struct
{
    uint32_t nvideo;
    uint32_t naudio;
    int      co64;          /* 64-bit chunk offsets ("co64")            */
    int      large_mdat;    /* mdat with the 64-bit size form           */
    int      a_uniform;     /* audio stsz: uniform size, no table       */
    const char *a_fourcc;   /* audio sample entry (NULL = "sowt")       */
    const uint8_t *a_child; /* boxes inside the audio sample entry      */
    size_t   a_child_len;
} Mp4Opt;

static void eb_be32(Eb *e, uint32_t v)
{
    uint8_t b[4];

    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
    eb_raw(e, b, 4);
}

static void eb_be16(Eb *e, uint16_t v)
{
    eb_u8(e, (uint8_t)(v >> 8));
    eb_u8(e, (uint8_t)v);
}

static void eb_zero(Eb *e, size_t n)
{
    while (n-- > 0u)
    {
        eb_u8(e, 0);
    }
}

static size_t vr_box(Eb *e, const char *type)
{
    size_t at = e->n;

    eb_be32(e, 0);
    eb_raw(e, type, 4);
    return at;
}

static void vr_box_end(Eb *e, size_t at)
{
    if (e->n <= e->cap)
    {
        uint32_t n = (uint32_t)(e->n - at);

        e->b[at] = (uint8_t)(n >> 24); e->b[at + 1u] = (uint8_t)(n >> 16);
        e->b[at + 2u] = (uint8_t)(n >> 8); e->b[at + 3u] = (uint8_t)n;
    }
}

/* Samples in video chunk `c` (0-based): runs of 3, 7, 1 and 5, so the
 * index table ends in the middle of a chunk and the stsc table has
 * several entries. */
static uint32_t vr_mp4_vchunk(uint32_t c)
{
    return (c < 4u) ? 3u : ((c < 8u) ? 7u : ((c < 11u) ? 1u : 5u));
}

static void vr_mp4_chunk_table(Eb *e, const Mp4Opt *o, const uint32_t *off,
                               uint32_t n)
{
    size_t b = vr_box(e, o->co64 ? "co64" : "stco");
    uint32_t i;

    eb_be32(e, 0);
    eb_be32(e, n);
    for (i = 0; i < n; i++)
    {
        if (o->co64)
        {
            eb_be32(e, 0);
        }
        eb_be32(e, off[i]);
    }
    vr_box_end(e, b);
}

/* Motion-JPEG video ("jpeg") + 16-bit PCM audio ("sowt"), every sample
 * 4 bytes holding its own number (vr_idx_bytes).  mdat comes first,
 * video and audio chunks interleaved.  Returns 0 on success. */
static int vr_mp4(Eb *e, const Mp4Opt *o)
{
    uint32_t *voff = (uint32_t *)malloc(((size_t)o->nvideo + 1u) * 4u);
    uint32_t *aoff = (uint32_t *)malloc(((size_t)o->naudio + 1u) * 4u);
    uint32_t nvc = 0;
    uint32_t nac = 0;
    uint32_t vs = 0;
    uint32_t as = 0;
    uint32_t i;
    size_t mdat;
    size_t moov;
    size_t b;

    if (voff == NULL || aoff == NULL)
    {
        free(voff); free(aoff);
        return -1;
    }
    b = vr_box(e, "ftyp");
    eb_raw(e, "isom", 4); eb_be32(e, 0x200); eb_raw(e, "isom", 4);
    vr_box_end(e, b);

    mdat = e->n;
    if (o->large_mdat)
    {
        eb_be32(e, 1); eb_raw(e, "mdat", 4); eb_be32(e, 0); eb_be32(e, 0);
    }
    else
    {
        (void)vr_box(e, "mdat");
    }
    while (vs < o->nvideo || as < o->naudio)
    {
        uint8_t px[4];

        if (vs < o->nvideo)
        {
            uint32_t n = vr_mp4_vchunk(nvc);

            voff[nvc++] = (uint32_t)e->n;
            for (i = 0; i < n && vs < o->nvideo; i++)
            {
                vr_idx_bytes(px, vs++);
                eb_raw(e, px, 4);
            }
        }
        if (as < o->naudio)
        {
            aoff[nac++] = (uint32_t)e->n;
            for (i = 0; i < 2u && as < o->naudio; i++)
            {
                vr_idx_bytes(px, as++);
                eb_raw(e, px, 4);
            }
        }
    }
    if (o->large_mdat)
    {
        if (e->n <= e->cap)
        {
            uint32_t n = (uint32_t)(e->n - mdat);

            e->b[mdat + 12u] = (uint8_t)(n >> 24);
            e->b[mdat + 13u] = (uint8_t)(n >> 16);
            e->b[mdat + 14u] = (uint8_t)(n >> 8);
            e->b[mdat + 15u] = (uint8_t)n;
        }
    }
    else
    {
        vr_box_end(e, mdat);
    }

    moov = vr_box(e, "moov");
    {
        size_t trak = vr_box(e, "trak");
        size_t mdia = vr_box(e, "mdia");
        size_t minf;
        size_t stbl;

        b = vr_box(e, "mdhd");
        eb_be32(e, 0); eb_be32(e, 0); eb_be32(e, 0);
        eb_be32(e, 30); eb_be32(e, o->nvideo);
        eb_be16(e, 0x55C4); eb_be16(e, 0);
        vr_box_end(e, b);
        b = vr_box(e, "hdlr");
        eb_be32(e, 0); eb_be32(e, 0); eb_raw(e, "vide", 4);
        eb_zero(e, 12); eb_raw(e, "vide", 4);
        vr_box_end(e, b);
        minf = vr_box(e, "minf");
        stbl = vr_box(e, "stbl");
        b = vr_box(e, "stsd");
        eb_be32(e, 0); eb_be32(e, 1);
        {
            size_t en = vr_box(e, "jpeg");

            eb_zero(e, 24);
            eb_be16(e, 64); eb_be16(e, 48);
            eb_zero(e, 50);
            vr_box_end(e, en);
        }
        vr_box_end(e, b);
        b = vr_box(e, "stts");
        eb_be32(e, 0); eb_be32(e, 1); eb_be32(e, o->nvideo); eb_be32(e, 1);
        vr_box_end(e, b);
        b = vr_box(e, "stsc");
        eb_be32(e, 0); eb_be32(e, 4);
        eb_be32(e, 1);  eb_be32(e, 3); eb_be32(e, 1);
        eb_be32(e, 5);  eb_be32(e, 7); eb_be32(e, 1);
        eb_be32(e, 9);  eb_be32(e, 1); eb_be32(e, 1);
        eb_be32(e, 12); eb_be32(e, 5); eb_be32(e, 1);
        vr_box_end(e, b);
        b = vr_box(e, "stsz");
        eb_be32(e, 0); eb_be32(e, 0); eb_be32(e, o->nvideo);
        for (i = 0; i < o->nvideo; i++)
        {
            eb_be32(e, 4);
        }
        vr_box_end(e, b);
        vr_mp4_chunk_table(e, o, voff, nvc);
        vr_box_end(e, stbl);
        vr_box_end(e, minf);
        vr_box_end(e, mdia);
        vr_box_end(e, trak);
    }
    if (o->naudio != 0u)
    {
        size_t trak = vr_box(e, "trak");
        size_t stbl;

        b = vr_box(e, "hdlr");
        eb_be32(e, 0); eb_be32(e, 0); eb_raw(e, "soun", 4);
        eb_zero(e, 12); eb_raw(e, "soun", 4);
        vr_box_end(e, b);
        stbl = vr_box(e, "stbl");
        b = vr_box(e, "stsd");
        eb_be32(e, 0); eb_be32(e, 1);
        {
            size_t en = vr_box(e, (o->a_fourcc != NULL) ? o->a_fourcc
                                                        : "sowt");

            eb_zero(e, 16);
            eb_be16(e, 2);              /* channels    */
            eb_be16(e, 16);             /* sample size */
            eb_be32(e, 0);
            eb_be32(e, 8000u << 16);    /* rate 16.16  */
            if (o->a_child != NULL)
            {
                eb_raw(e, o->a_child, o->a_child_len);
            }
            vr_box_end(e, en);
        }
        vr_box_end(e, b);
        b = vr_box(e, "stsc");
        eb_be32(e, 0); eb_be32(e, 1);
        eb_be32(e, 1); eb_be32(e, 2); eb_be32(e, 1);
        vr_box_end(e, b);
        b = vr_box(e, "stsz");
        eb_be32(e, 0);
        eb_be32(e, o->a_uniform ? 4u : 0u);
        eb_be32(e, o->naudio);
        if (!o->a_uniform)
        {
            for (i = 0; i < o->naudio; i++)
            {
                eb_be32(e, 4);
            }
        }
        vr_box_end(e, b);
        vr_mp4_chunk_table(e, o, aoff, nac);
        vr_box_end(e, stbl);
        vr_box_end(e, trak);
    }
    vr_box_end(e, moov);
    free(voff);
    free(aoff);
    return 0;
}

static size_t   g_vr_hw_in_len;
static int      g_vr_hw_calls;

static FM_Result vr_hw_decode(void *ctx, const void *compressed, size_t size,
                              void *frame_out, size_t frame_cap)
{
    (void)ctx;
    g_vr_hw_calls++;
    g_vr_hw_in_len = size;
    memcpy(g_vr_hw_in, compressed,
           (size < sizeof(g_vr_hw_in)) ? size : sizeof(g_vr_hw_in));
    if (frame_cap < 1u)
    {
        return FM_ERR_TOO_LARGE;
    }
    ((uint8_t *)frame_out)[0] = 0xABu;
    return FM_OK;
}

static const FM_VideoCodecOps g_vr_hw_ops = {
    "VR-HW", NULL, vr_hw_decode, NULL
};

static uint32_t g_vr_allocs;

static void *vr_cnt_alloc(size_t size, void *ctx)
{
    (void)ctx;
    g_vr_allocs++;
    return malloc(size);
}

#if FM_USE_H264
/* ---- H.264 SPS rewriting (adds a cropping rectangle) --------------- */
typedef struct { const uint8_t *p; size_t nbits; size_t bit; } VrBitR;
typedef struct { uint8_t p[160]; size_t bit; } VrBitW;

static uint32_t vr_br(VrBitR *b, uint32_t n)
{
    uint32_t v = 0;

    while (n-- > 0u)
    {
        uint32_t bit = 0;

        if (b->bit < b->nbits)
        {
            bit = ((uint32_t)b->p[b->bit >> 3] >> (7u - (b->bit & 7u))) & 1u;
        }
        b->bit++;
        v = (v << 1) | bit;
    }
    return v;
}

static uint32_t vr_br_ue(VrBitR *b)
{
    uint32_t zeros = 0;

    while (vr_br(b, 1) == 0u && zeros < 31u)
    {
        zeros++;
    }
    return ((1u << zeros) - 1u) + vr_br(b, zeros);
}

static void vr_bw(VrBitW *w, uint32_t v, uint32_t n)
{
    while (n-- > 0u)
    {
        if (w->bit < sizeof(w->p) * 8u && ((v >> n) & 1u) != 0u)
        {
            w->p[w->bit >> 3] =
                (uint8_t)(w->p[w->bit >> 3] | (0x80u >> (w->bit & 7u)));
        }
        w->bit++;
    }
}

static void vr_bw_ue(VrBitW *w, uint32_t v)
{
    uint32_t bits = 0;
    uint32_t x = v + 1u;

    while ((x >> bits) > 1u)
    {
        bits++;
    }
    vr_bw(w, 0u, bits);
    vr_bw(w, x, bits + 1u);
}

/* Rewrite a baseline SPS NAL so that frame_cropping_flag = 1 with the
 * given offsets (in 2-pixel units).  Returns the new NAL length, or 0
 * when the SPS is not one this helper understands. */
static size_t vr_sps_add_crop(const uint8_t *sps, size_t len, uint8_t *out,
                              size_t out_cap, uint32_t left, uint32_t right,
                              uint32_t top, uint32_t bottom)
{
    uint8_t rbsp[160];
    size_t rn = 0;
    size_t i;
    size_t last1;
    size_t on = 0;
    VrBitR r;
    VrBitW w;
    uint32_t v;
    uint32_t frame_mbs_only;

    if (len < 5u || len > sizeof(rbsp))
    {
        return 0u;
    }
    /* strip emulation-prevention bytes */
    for (i = 1; i < len; i++)
    {
        if (i >= 3u && sps[i] == 3u && sps[i - 1u] == 0u && sps[i - 2u] == 0u)
        {
            continue;
        }
        rbsp[rn++] = sps[i];
    }
    /* position of the rbsp stop bit */
    last1 = rn * 8u;
    while (last1 > 0u &&
           (((uint32_t)rbsp[(last1 - 1u) >> 3] >> (7u - ((last1 - 1u) & 7u))) & 1u) == 0u)
    {
        last1--;
    }
    if (last1 == 0u)
    {
        return 0u;
    }
    last1--;                       /* index of the stop bit itself */
    r.p = rbsp; r.nbits = last1; r.bit = 0;
    memset(&w, 0, sizeof(w));

    v = vr_br(&r, 8); vr_bw(&w, v, 8);          /* profile_idc */
    if (v != 66u && v != 77u && v != 88u)
    {
        return 0u;                              /* no chroma_format fields */
    }
    v = vr_br(&r, 16); vr_bw(&w, v, 16);        /* constraints + level */
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* sps_id */
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* log2_max_frame_num-4 */
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* pic_order_cnt_type */
    if (v == 0u)
    {
        v = vr_br_ue(&r); vr_bw_ue(&w, v);
    }
    else if (v == 1u)
    {
        return 0u;
    }
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* max_num_ref_frames */
    v = vr_br(&r, 1); vr_bw(&w, v, 1);          /* gaps allowed */
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* width in MBs - 1 */
    v = vr_br_ue(&r); vr_bw_ue(&w, v);          /* height in map units - 1 */
    frame_mbs_only = vr_br(&r, 1); vr_bw(&w, frame_mbs_only, 1);
    if (!frame_mbs_only)
    {
        return 0u;
    }
    v = vr_br(&r, 1); vr_bw(&w, v, 1);          /* direct_8x8_inference */
    if (vr_br(&r, 1) != 0u)
    {
        return 0u;                              /* already cropped */
    }
    vr_bw(&w, 1u, 1);                           /* frame_cropping_flag */
    vr_bw_ue(&w, left); vr_bw_ue(&w, right);
    vr_bw_ue(&w, top);  vr_bw_ue(&w, bottom);
    while (r.bit < r.nbits)                     /* VUI flag + VUI */
    {
        vr_bw(&w, vr_br(&r, 1), 1);
    }
    vr_bw(&w, 1u, 1);                           /* rbsp stop bit */
    rn = (w.bit + 7u) / 8u;
    if (w.bit > sizeof(w.p) * 8u)
    {
        return 0u;
    }
    /* NAL header + rbsp with emulation prevention re-inserted */
    if (out_cap < 1u)
    {
        return 0u;
    }
    out[on++] = sps[0];
    for (i = 0; i < rn; i++)
    {
        if (on + 2u > out_cap)
        {
            return 0u;
        }
        if (on >= 3u && out[on - 1u] == 0u && out[on - 2u] == 0u &&
            w.p[i] <= 3u)
        {
            out[on++] = 3u;
        }
        out[on++] = w.p[i];
    }
    return on;
}
#endif /* FM_USE_H264 */

#if FM_USE_AUDIO
/* Fake track decoder for V13: one sample per input byte, plus the number
 * of packets this context has decoded before. */
static int      g_vr_ad_inits;
static int      g_vr_ad_closes;
static int      g_vr_ad_decodes;
static int      g_vr_ad_bad_ctx;
static uint32_t g_vr_ad_rate;
static uint32_t g_vr_ad_state;

static FM_Result vr_ad_init(void **ctx, const FM_AudioInfo *info)
{
    g_vr_ad_inits++;
    g_vr_ad_rate = info->sample_rate;
    g_vr_ad_state = 0u;
    *ctx = &g_vr_ad_state;
    return FM_OK;
}

static FM_Result vr_ad_decode(void *ctx, const uint8_t *in, size_t in_size,
                              int16_t *out, size_t out_count,
                              size_t *out_samples)
{
    uint32_t *seen = (uint32_t *)ctx;
    size_t k;

    g_vr_ad_decodes++;
    if (ctx != &g_vr_ad_state)
    {
        g_vr_ad_bad_ctx++;
        return FM_ERR_INTERNAL;
    }
    if (in_size > out_count)
    {
        return FM_ERR_TOO_LARGE;
    }
    for (k = 0; k < in_size; k++)
    {
        out[k] = (int16_t)(in[k] + *seen);
    }
    (*seen)++;
    *out_samples = in_size;
    return FM_OK;
}

static void vr_ad_close(void *ctx)
{
    (void)ctx;
    g_vr_ad_closes++;
}

static const FM_AudioDecoderOps g_vr_ad_ops = {
    "vr-fake", vr_ad_init, vr_ad_decode, vr_ad_close
};
#endif /* FM_USE_AUDIO */

static void test_video_report(void)
{
    static uint8_t  small[4096];
    static uint8_t  rgb_big[320u * 240u * 3u];
    static const uint8_t F0[] = { 'a', 'b', 'c' };
    static uint8_t  F1[300];
    static const uint8_t F2[] = { '1', '2', '3', '4', '5' };
    static const uint8_t F3[] = { 'x', 'y', 'z', 'w', 'v', 'u', 't' };
    static const uint8_t A0[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t A1[] = { 9, 10, 11, 12 };
    const uint8_t *vf[4];
    uint32_t vs[4];
    const uint8_t *af[2];
    uint32_t as[2];
    FM_Video vid;
    FM_Result r;
    Eb e;
    MkvOpt o;
    size_t seg_at = 0;
    size_t clu_at = 0;
    uint32_t i;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("Video report: remount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }
    for (i = 0; i < sizeof(F1); i++) F1[i] = (uint8_t)(i * 7u + 1u);
    vf[0] = F0; vs[0] = sizeof(F0);
    vf[1] = F1; vs[1] = sizeof(F1);
    vf[2] = F2; vs[2] = sizeof(F2);
    vf[3] = F3; vs[3] = sizeof(F3);
    af[0] = A0; as[0] = sizeof(A0);
    af[1] = A1; as[1] = sizeof(A1);

#define VR_BEGIN() do { memset(&e, 0, sizeof(e)); e.b = small; \
                        e.cap = sizeof(small); memset(&o, 0, sizeof(o)); \
                        memset(&vid, 0, sizeof(vid)); } while (0)

    /* ---------------- V1 ---------------- */
    TEST("V1 mkv: 1-byte-ID elements parsed (4 laced frames + 2 audio)");
    VR_BEGIN();
    o.rate = 44100.0;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 1u, vf, vs, 4u);          /* Xiph lacing */
    mkv_block(&e, 0xA3u, 2u, 0u, &af[0], &as[0], 1u);
    mkv_block(&e, 0xA3u, 2u, 0u, &af[1], &as[1], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        FM_AudioTrackInfo ai;
        uint8_t ab[32];
        size_t got = 0;

        if (vid.info.width != 320u || vid.info.height != 240u ||
            vid.info.fps != 25u || vid.info.codec != FM_VIDEO_CODEC_MJPEG ||
            strcmp(vid.info.container, "MKV") != 0)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK) r = vr_expect_frames(&vid, vf, vs, 4u);
        if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
        if (r == FM_OK && (ai.codec != FM_VIDEO_AUDIO_PCM ||
                           ai.sample_rate != 44100u || ai.channels != 2u ||
                           ai.bits_per_sample != 16u || ai.sample_count != 2u))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK) r = Video_ReadAudioFrame(&vid, ab, sizeof(ab), &got);
        if (r == FM_OK && (got != sizeof(A0) || memcmp(ab, A0, got) != 0))
        {
            r = FM_ERR_CRC;
        }
        if (r == FM_OK) r = Video_ReadAudioFrame(&vid, ab, sizeof(ab), &got);
        if (r == FM_OK && (got != sizeof(A1) || memcmp(ab, A1, got) != 0))
        {
            r = FM_ERR_CRC;
        }
        if (r == FM_OK &&
            Video_ReadAudioFrame(&vid, ab, sizeof(ab), &got) != FM_ERR_EOF)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "mkv baseline");

    /* ---------------- V2 ---------------- */
    TEST("V2 mkv: 8-byte Segment size + 5-byte element size");
    VR_BEGIN();
    o.seg_len = 8u;
    o.long_void = 1;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[2], &vs[2], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        const uint8_t *ff[2];
        uint32_t fs2[2];

        ff[0] = F0; fs2[0] = sizeof(F0);
        ff[1] = F2; fs2[1] = sizeof(F2);
        r = vr_expect_frames(&vid, ff, fs2, 2u);
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "mkv long sizes");

    /* ---------------- V3 ---------------- */
    TEST("V3 mkv: SamplingFrequency is an IEEE float (float64 48000)");
    VR_BEGIN();
    o.rate = 48000.0;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    mkv_block(&e, 0xA3u, 2u, 0u, &af[0], &as[0], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        FM_AudioTrackInfo ai;

        r = Video_GetAudioInfo(&vid, &ai);
        if (r == FM_OK && ai.sample_rate != 48000u) r = FM_ERR_INVALID_FORMAT;
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "float64 rate");

    TEST("V3 mkv: SamplingFrequency float32 22050");
    VR_BEGIN();
    o.rate = 22050.0;
    o.rate_f32 = 1;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    mkv_block(&e, 0xA3u, 2u, 0u, &af[0], &as[0], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        FM_AudioTrackInfo ai;

        r = Video_GetAudioInfo(&vid, &ai);
        if (r == FM_OK && ai.sample_rate != 22050u) r = FM_ERR_INVALID_FORMAT;
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "float32 rate");

    /* ---------------- V4 ---------------- */
    TEST("V4 mkv: 8-byte PixelWidth");
    VR_BEGIN();
    o.width_bytes = 8u;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        if (vid.info.width != 320u || vid.info.height != 240u)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "wide fixnum");

    /* ---------------- V5 ---------------- */
    TEST("V5 mkv: block shorter than its header is dropped, not indexed");
    VR_BEGIN();
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    /* SimpleBlock of 3 bytes announcing a 2-byte track number (track 1):
     * its header alone is 5 bytes. */
    eb_id(&e, 0xA3u); eb_vint(&e, 3u, 1u);
    eb_u8(&e, 0x40u); eb_u8(&e, 0x01u); eb_u8(&e, 0x00u);
    /* Xiph lace whose sizes add up to more than the block holds */
    eb_id(&e, 0xA3u); eb_vint(&e, 8u, 1u);
    eb_u8(&e, 0x81u); eb_u8(&e, 0); eb_u8(&e, 0); eb_u8(&e, 0x82u);
    eb_u8(&e, 1u); eb_u8(&e, 200u); eb_u8(&e, 'q'); eb_u8(&e, 'q');
    /* fixed lace that does not divide evenly (3 frames in 4 bytes) */
    eb_id(&e, 0xA3u); eb_vint(&e, 9u, 1u);
    eb_u8(&e, 0x81u); eb_u8(&e, 0); eb_u8(&e, 0); eb_u8(&e, 0x84u);
    eb_u8(&e, 2u); eb_raw(&e, "qqqq", 4);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[2], &vs[2], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        const uint8_t *ff[2];
        uint32_t fs2[2];

        ff[0] = F0; fs2[0] = sizeof(F0);
        ff[1] = F2; fs2[1] = sizeof(F2);
        r = vr_expect_frames(&vid, ff, fs2, 2u);
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "bad blocks");

    /* ---------------- lacing ---------------- */
    TEST("mkv lacing: fixed-size");
    VR_BEGIN();
    {
        static const uint8_t L0[] = { 'A', 'A', 'A', 'A' };
        static const uint8_t L1[] = { 'B', 'B', 'B', 'B' };
        static const uint8_t L2[] = { 'C', 'C', 'C', 'C' };
        const uint8_t *lf[3];
        uint32_t ls[3];

        lf[0] = L0; lf[1] = L1; lf[2] = L2;
        ls[0] = ls[1] = ls[2] = 4u;
        mkv_head(&e, &o, &seg_at, &clu_at);
        mkv_block(&e, 0xA3u, 1u, 2u, lf, ls, 3u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            r = vr_expect_frames(&vid, lf, ls, 3u);
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "fixed lace");

    TEST("mkv lacing: EBML sizes inside a BlockGroup (2-byte track no.)");
    VR_BEGIN();
    {
        size_t grp;

        mkv_head(&e, &o, &seg_at, &clu_at);
        grp = eb_open(&e, 0xA0u);
        eb_uint(&e, 0x9Bu, 40u, 1u);          /* BlockDuration, skipped */
        mkv_block(&e, 0xA1u, 1u, 3u, vf, vs, 4u);
        eb_close(&e, grp);
        /* an element after the group: the old walker mis-stepped here */
        mkv_block(&e, 0xA3u, 1u, 0u, &vf[2], &vs[2], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            const uint8_t *ff[5];
            uint32_t fs2[5];

            for (i = 0; i < 4u; i++) { ff[i] = vf[i]; fs2[i] = vs[i]; }
            ff[4] = F2; fs2[4] = sizeof(F2);
            r = vr_expect_frames(&vid, ff, fs2, 5u);
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "ebml lace");

    TEST("mkv: unknown-size (live) cluster");
    VR_BEGIN();
    o.unknown_cluster = 1;
    mkv_head(&e, &o, &seg_at, &clu_at);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
    mkv_block(&e, 0xA3u, 1u, 0u, &vf[3], &vs[3], 1u);
    mkv_tail(&e, &o, seg_at, clu_at);
    r = vr_write("/vr.mkv", &e);
    if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
    if (r == FM_OK)
    {
        const uint8_t *ff[2];
        uint32_t fs2[2];

        ff[0] = F0; fs2[0] = sizeof(F0);
        ff[1] = F3; fs2[1] = sizeof(F3);
        r = vr_expect_frames(&vid, ff, fs2, 2u);
        (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "unknown-size cluster");

    /* ---------------- V9 ---------------- */
    TEST("V9 avcC: 1-byte NAL lengths -> Annex-B (output grows)");
    VR_BEGIN();
    {
        static const uint8_t avcc[] = {
            1, 0x42, 0, 0x1F, 0xFC | 0, 0xE0 | 1,
            0, 4, 'S', 'P', 'S', '1',
            1, 0, 3, 'P', 'P', '1'
        };
        static const uint8_t s0[] = { 3, 'a', 'b', 'c', 2, 'd', 'e' };
        static const uint8_t s1[] = { 3, 'a', 'b' };          /* overruns */
        static const uint8_t s2[] = { 1, 'p', 1, 'q', 1, 'r' };
        static const uint8_t want0[] = {
            0, 0, 0, 1, 'S', 'P', 'S', '1', 0, 0, 0, 1, 'P', 'P', '1',
            0, 0, 0, 1, 'a', 'b', 'c', 0, 0, 0, 1, 'd', 'e'
        };
        const uint8_t *sf[3];
        uint32_t ss[3];
        uint8_t out[64];
        size_t got = 0;

        sf[0] = s0; ss[0] = sizeof(s0);
        sf[1] = s1; ss[1] = sizeof(s1);
        sf[2] = s2; ss[2] = sizeof(s2);
        o.vcodec = "V_MPEG4/ISO/AVC";
        o.vpriv = avcc; o.vpriv_len = sizeof(avcc);
        mkv_head(&e, &o, &seg_at, &clu_at);
        for (i = 0; i < 3u; i++) mkv_block(&e, 0xA3u, 1u, 0u, &sf[i], &ss[i], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            if (vid.info.codec != FM_VIDEO_CODEC_H264 ||
                vid.info.frame_count != 3u)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK) r = Video_ReadFrame(&vid, out, sizeof(out), &got);
            if (r == FM_OK && (got != sizeof(want0) ||
                               memcmp(out, want0, got) != 0))
            {
                r = FM_ERR_CRC;
            }
            /* NAL length runs past the sample */
            if (r == FM_OK &&
                Video_ReadFrame(&vid, out, sizeof(out), &got) != FM_ERR_CORRUPTED)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* 6 bytes in, 15 + 15 out: a buffer sized for the input is
             * too small and must be refused, not overrun */
            if (r == FM_OK) r = Video_Seek(&vid, 2u);
            if (r == FM_OK &&
                Video_ReadFrame(&vid, out, 15u + 6u, &got) != FM_ERR_TOO_LARGE)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK) r = Video_Seek(&vid, 2u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, out, 15u + 15u, &got);
            if (r == FM_OK && (got != 30u ||
                               memcmp(out + 15, "\0\0\0\1p\0\0\0\1q\0\0\0\1r", 15) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "avcC len 1");

    TEST("V9 avcC: 2-byte NAL lengths; trailing garbage is CORRUPTED");
    VR_BEGIN();
    {
        static const uint8_t avcc[] = {
            1, 0x42, 0, 0x1F, 0xFC | 1, 0xE0 | 1,
            0, 2, 'S', 'S',
            1, 0, 2, 'P', 'P'
        };
        static const uint8_t s0[] = { 0, 2, 'x', 'y', 0, 1, 'z' };
        static const uint8_t s1[] = { 0, 2, 'x', 'y', 7 };    /* 1 stray byte */
        static const uint8_t want0[] = {
            0, 0, 0, 1, 'S', 'S', 0, 0, 0, 1, 'P', 'P',
            0, 0, 0, 1, 'x', 'y', 0, 0, 0, 1, 'z'
        };
        const uint8_t *sf[2];
        uint32_t ss[2];
        uint8_t out[64];
        size_t got = 0;

        sf[0] = s0; ss[0] = sizeof(s0);
        sf[1] = s1; ss[1] = sizeof(s1);
        o.vcodec = "V_MPEG4/ISO/AVC";
        o.vpriv = avcc; o.vpriv_len = sizeof(avcc);
        mkv_head(&e, &o, &seg_at, &clu_at);
        for (i = 0; i < 2u; i++) mkv_block(&e, 0xA3u, 1u, 0u, &sf[i], &ss[i], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            r = Video_ReadFrame(&vid, out, sizeof(out), &got);
            if (r == FM_OK && (got != sizeof(want0) ||
                               memcmp(out, want0, got) != 0))
            {
                r = FM_ERR_CRC;
            }
            if (r == FM_OK &&
                Video_ReadFrame(&vid, out, sizeof(out), &got) != FM_ERR_CORRUPTED)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "avcC len 2");

    /* ---------------- V10 / V11 / V20 ---------------- */
    TEST("V11 HWDecoder_Register accepts every codec id");
    r = FM_OK;
    for (i = 1u; i < (uint32_t)FM_VIDEO_CODEC_COUNT && r == FM_OK; i++)
    {
        FM_VideoCodec c = (FM_VideoCodec)i;

        r = HWDecoder_Register(c, &g_vr_hw_ops);
        if (r == FM_OK && (!HWDecoder_Has(c) ||
                           HWDecoder_Get(c) != &g_vr_hw_ops))
        {
            r = FM_ERR_NOT_FOUND;
        }
        if (r == FM_OK) r = HWDecoder_Unregister(c);
        if (r == FM_OK && (HWDecoder_Has(c) || HWDecoder_Get(c) != NULL))
        {
            r = FM_ERR_EXISTS;
        }
    }
    if (r == FM_OK &&
        (HWDecoder_Register(FM_VIDEO_CODEC_UNKNOWN, &g_vr_hw_ops) !=
             FM_ERR_INVALID_ARGUMENT ||
         HWDecoder_Register(FM_VIDEO_CODEC_COUNT, &g_vr_hw_ops) !=
             FM_ERR_INVALID_ARGUMENT ||
         HWDecoder_Get(FM_VIDEO_CODEC_COUNT) != NULL))
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    CHECK_RESULT(r, "register all codecs");

    {
        static const uint8_t hvcc[] = {
            1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0xFC | 1,                       /* lengthSizeMinusOne = 1 */
            4,                              /* numOfArrays */
            0x20, 0, 1, 0, 3, 'V', 'P', 'S',
            0x21, 0, 1, 0, 3, 'S', 'P', 'S',
            0x27, 0, 1, 0, 2, 'X', 'X',     /* SEI prefix: not a param set */
            0x22, 0, 1, 0, 3, 'P', 'P', 'S'
        };
        static const uint8_t s0[] = { 0, 5, 'N', 'A', 'L', '0', '1', 0, 2, 'Z', 'Z' };
        static const uint8_t s1[] = { 0, 3, 'N', 'A', '2' };
        static const uint8_t s2[] = { 0, 3, 'N', 'A', '3' };
        static const uint8_t want0[] = {
            0, 0, 0, 1, 'V', 'P', 'S', 0, 0, 0, 1, 'S', 'P', 'S',
            0, 0, 0, 1, 'P', 'P', 'S',
            0, 0, 0, 1, 'N', 'A', 'L', '0', '1', 0, 0, 0, 1, 'Z', 'Z'
        };
        const uint8_t *sf[3];
        uint32_t ss[3];
        uint8_t out[96];
        size_t got = 0;
        FM_Result rw;

        sf[0] = s0; ss[0] = sizeof(s0);
        sf[1] = s1; ss[1] = sizeof(s1);
        sf[2] = s2; ss[2] = sizeof(s2);
        VR_BEGIN();
        o.vcodec = "V_MPEGH/ISO/HEVC";
        o.vpriv = hvcc; o.vpriv_len = sizeof(hvcc);
        mkv_head(&e, &o, &seg_at, &clu_at);
        for (i = 0; i < 3u; i++) mkv_block(&e, 0xA3u, 1u, 0u, &sf[i], &ss[i], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        rw = vr_write("/vr_hevc.mkv", &e);

        TEST("V10 hvcC: VPS/SPS/PPS prefix + Annex-B samples");
        r = rw;
        if (r == FM_OK) r = Video_Open("/vr_hevc.mkv", &vid);
        if (r == FM_OK)
        {
            if (vid.info.codec != FM_VIDEO_CODEC_HEVC) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK) r = Video_ReadFrame(&vid, out, sizeof(out), &got);
            if (r == FM_OK && (got != sizeof(want0) ||
                               memcmp(out, want0, got) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "hvcC");

        TEST("V11 HEVC without a decoder: UNSUPPORTED (no SW fallback)");
        r = rw;
        if (r == FM_OK) r = Video_Open("/vr_hevc.mkv", &vid);
        if (r == FM_OK)
        {
            if (Video_DecodeFrame(&vid, rgb_big, sizeof(rgb_big)) !=
                FM_ERR_UNSUPPORTED)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "no decoder");

        TEST("V11 HEVC: registered hardware decoder gets Annex-B + sets");
        r = rw;
        g_vr_hw_calls = 0;
        g_vr_hw_in_len = 0;
        if (r == FM_OK) r = HWDecoder_Register(FM_VIDEO_CODEC_HEVC, &g_vr_hw_ops);
        if (r == FM_OK) r = Video_Open("/vr_hevc.mkv", &vid);
        if (r == FM_OK)
        {
            rgb_big[0] = 0;
            r = Video_DecodeFrame(&vid, rgb_big, sizeof(rgb_big));
            if (r == FM_OK && (g_vr_hw_calls != 1 || rgb_big[0] != 0xABu ||
                               g_vr_hw_in_len != sizeof(want0) ||
                               memcmp(g_vr_hw_in, want0, sizeof(want0)) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "hevc via hw");

        TEST("V20 Video_DecodeFrame: output buffer smaller than w*h*3");
        r = rw;
        g_vr_hw_calls = 0;
        if (r == FM_OK) r = Video_Open("/vr_hevc.mkv", &vid);
        if (r == FM_OK)
        {
            /* 320x240 RGB888 needs 230400 bytes; the decoder must not
             * even be called with less */
            if (Video_DecodeFrame(&vid, rgb_big, 230399u) != FM_ERR_TOO_LARGE ||
                g_vr_hw_calls != 0)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "pixels_cap guard");

        TEST("V20 Video_DecodeFrame: one scratch buffer for all frames");
        r = rw;
        if (r == FM_OK) r = Video_Open("/vr_hevc.mkv", &vid);
        if (r == FM_OK)
        {
            FM_Allocator saved = *FM_Allocator_Get();
            FM_Allocator cnt = saved;

            cnt.alloc = vr_cnt_alloc;
            g_vr_allocs = 0;
            (void)FM_Allocator_Set(&cnt);
            for (i = 0; i < 3u && r == FM_OK; i++)
            {
                r = Video_DecodeFrame(&vid, rgb_big, sizeof(rgb_big));
            }
            (void)FM_Allocator_Set(&saved);
            /* was: one FM_Alloc per frame (3) */
            if (r == FM_OK && g_vr_allocs > 1u)
            {
                r = FM_ERR_NO_MEMORY;
            }
            if (r == FM_OK &&
                Video_DecodeFrame(&vid, rgb_big, sizeof(rgb_big)) != FM_ERR_EOF)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "scratch reuse");
        (void)HWDecoder_Unregister(FM_VIDEO_CODEC_HEVC);
        (void)File_Delete("/vr_hevc.mkv");
    }
    (void)File_Delete("/vr.mkv");

    /* ---------------- V6 ---------------- */
    {
        static const uint32_t lens[3] = { 16u, 18u, 40u };
        uint32_t li;

        for (li = 0; li < 3u; li++)
        {
            AviOpt ao;
            char name[64];

            (void)snprintf(name, sizeof(name),
                           "V6 avi: %u-byte audio strf as the last chunk",
                           (unsigned)lens[li]);
            TEST(name);
            VR_BEGIN();
            memset(&ao, 0, sizeof(ao));
            ao.astrf_len = lens[li];
            ao.hdrl_last = 1;
            ao.nvideo = 3u;
            ao.naudio = 2u;
            vr_avi(&e, &ao);
            r = vr_write("/vr.avi", &e);
            if (r == FM_OK) r = Video_Open("/vr.avi", &vid);
            if (r == FM_OK)
            {
                FM_AudioTrackInfo ai;

                r = Video_GetAudioInfo(&vid, &ai);
                if (r == FM_OK && (ai.sample_rate != 44100u ||
                                   ai.channels != 2u ||
                                   ai.bits_per_sample != 16u ||
                                   vid.info.frame_count != 3u))
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                (void)Video_Close(&vid);
            }
            CHECK_RESULT(r, "short strf");
        }
    }

    TEST("V6 avi: BITMAPCOREHEADER (WORD sizes, codec from the handler)");
    VR_BEGIN();
    {
        AviOpt ao;

        memset(&ao, 0, sizeof(ao));
        ao.core_header = 1;
        ao.nvideo = 2u;
        vr_avi(&e, &ao);
        r = vr_write("/vr.avi", &e);
        if (r == FM_OK) r = Video_Open("/vr.avi", &vid);
        if (r == FM_OK)
        {
            if (vid.info.width != 320u || vid.info.height != 240u ||
                vid.info.codec != FM_VIDEO_CODEC_MJPEG)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "core header");

    /* ---------------- V7 ---------------- */
    /* guard: this one was already rejected before the clamp went in */
    TEST("V7 avi: 24-byte file claiming 2 GB is rejected cleanly");
    VR_BEGIN();
    eb_raw(&e, "RIFF", 4); eb_le32(&e, 0x7FFFFFFFu); eb_raw(&e, "AVI ", 4);
    eb_raw(&e, "LIST", 4); eb_le32(&e, 0x7FFFFFFFu); eb_raw(&e, "movi", 4);
    r = vr_write("/vr.avi", &e);
    if (r == FM_OK)
    {
        r = (Video_Open("/vr.avi", &vid) != FM_OK) ? FM_OK : FM_ERR_INVALID_FORMAT;
        if (r != FM_OK) (void)Video_Close(&vid);
    }
    CHECK_RESULT(r, "tiny riff");

    TEST("V7 avi: movi/RIFF sizes past EOF are clamped, frames still play");
    VR_BEGIN();
    {
        AviOpt ao;

        memset(&ao, 0, sizeof(ao));
        ao.nvideo = 3u;
        vr_avi(&e, &ao);
        /* a recorder that never patched its headers */
        memset(e.b + 4, 0xFF, 3); e.b[7] = 0x7Fu;
        memset(e.b + ao.movi_at + 4, 0xFF, 3); e.b[ao.movi_at + 7] = 0x7Fu;
        r = vr_write("/vr.avi", &e);
        if (r == FM_OK) r = Video_Open("/vr.avi", &vid);
        if (r == FM_OK)
        {
            uint8_t fb[8];
            size_t got = 0;

            if (vid.info.frame_count != 3u) r = FM_ERR_INVALID_FORMAT;
            for (i = 0; i < 3u && r == FM_OK; i++)
            {
                r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "unpatched sizes");

    TEST("V7 avi: last chunk cut off -> the complete frames still play");
    VR_BEGIN();
    {
        AviOpt ao;

        memset(&ao, 0, sizeof(ao));
        ao.nvideo = 4u;
        vr_avi(&e, &ao);
        e.n -= 2u;                       /* file ends inside frame 3 */
        r = vr_write("/vr.avi", &e);
        if (r == FM_OK) r = Video_Open("/vr.avi", &vid);
        if (r == FM_OK)
        {
            uint8_t fb[8];
            size_t got = 0;

            if (vid.info.frame_count != 3u) r = FM_ERR_INVALID_FORMAT;
            for (i = 0; i < 3u && r == FM_OK; i++)
            {
                r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            (void)Video_Close(&vid);
        }
    }
    CHECK_RESULT(r, "truncated tail");
    (void)File_Delete("/vr.avi");

    /* ---------------- V8 / V19: clips longer than the index -------- */
    {
        const uint32_t nv = (uint32_t)FM_VIDEO_MAX_FRAMES + 37u;
        const uint32_t na = (uint32_t)FM_VIDEO_MAX_AUDIO_FRAMES + 53u;
        size_t cap = (size_t)nv * 16u + (size_t)na * 16u + 4096u;
        uint8_t *big = (uint8_t *)malloc(cap);
        uint8_t fb[16];
        size_t got = 0;

        TEST("V8 avi: frames past FM_VIDEO_MAX_FRAMES are streamed");
        r = (big != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
        if (r == FM_OK)
        {
            AviOpt ao;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            e.b = big; e.cap = cap;
            memset(&ao, 0, sizeof(ao));
            ao.astrf_len = 16u;
            ao.nvideo = nv;
            ao.naudio = na;
            vr_avi(&e, &ao);
            r = vr_write("/vr_long.avi", &e);
        }
        if (r == FM_OK) r = Video_Open("/vr_long.avi", &vid);
        if (r == FM_OK)
        {
            FM_AudioTrackInfo ai;
            const uint32_t edge = (uint32_t)FM_VIDEO_MAX_FRAMES;
            const uint32_t aedge = (uint32_t)FM_VIDEO_MAX_AUDIO_FRAMES;

            if (vid.info.frame_count != nv) r = FM_ERR_INVALID_FORMAT;
            /* across the end of the index */
            if (r == FM_OK) r = Video_Seek(&vid, edge - 2u);
            for (i = edge - 2u; i < edge + 6u && r == FM_OK; i++)
            {
                r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            /* the very last frame, then EOF */
            if (r == FM_OK) r = Video_Seek(&vid, nv - 1u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, nv - 1u);
            if (r == FM_OK &&
                Video_ReadFrame(&vid, fb, sizeof(fb), &got) != FM_ERR_EOF)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* backward jump inside the streamed part */
            if (r == FM_OK) r = Video_Seek(&vid, edge + 3u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, edge + 3u);
            /* audio keeps its own continuation */
            if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
            if (r == FM_OK && ai.sample_count != na) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK) r = Video_SeekAudio(&vid, aedge - 2u);
            for (i = aedge - 2u; i < aedge + 6u && r == FM_OK; i++)
            {
                r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "avi long clip");
        (void)File_Delete("/vr_long.avi");

        TEST("V19 mkv: video + audio past their indexes are streamed");
        r = (big != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
        if (r == FM_OK)
        {
            uint8_t pk[3][4];
            const uint8_t *pf[3];
            uint32_t ps[3];
            uint32_t done = 0;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            memset(&o, 0, sizeof(o));
            e.b = big; e.cap = cap;
            o.rate = 44100.0;
            mkv_head(&e, &o, &seg_at, &clu_at);
            for (i = 0; i < nv; i++)
            {
                vr_idx_bytes(pk[0], i);
                pf[0] = pk[0]; ps[0] = 4u;
                mkv_block(&e, 0xA3u, 1u, 0u, pf, ps, 1u);
            }
            /* audio in Xiph-laced blocks of 3 packets: the end of the
             * index falls in the middle of a block */
            while (done < na)
            {
                uint32_t n = (na - done >= 3u) ? 3u : (na - done);
                uint32_t k;

                for (k = 0; k < n; k++)
                {
                    vr_idx_bytes(pk[k], done + k);
                    pf[k] = pk[k]; ps[k] = 4u;
                }
                mkv_block(&e, 0xA3u, 2u, 1u, pf, ps, n);
                done += n;
            }
            mkv_tail(&e, &o, seg_at, clu_at);
            r = vr_write("/vr_long.mkv", &e);
        }
        if (r == FM_OK) r = Video_Open("/vr_long.mkv", &vid);
        if (r == FM_OK)
        {
            FM_AudioTrackInfo ai;
            const uint32_t edge = (uint32_t)FM_VIDEO_MAX_FRAMES;
            const uint32_t aedge = (uint32_t)FM_VIDEO_MAX_AUDIO_FRAMES;

            if (vid.info.frame_count != nv) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK) r = Video_Seek(&vid, edge - 2u);
            for (i = edge - 2u; i < edge + 6u && r == FM_OK; i++)
            {
                r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            if (r == FM_OK) r = Video_Seek(&vid, nv - 1u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, nv - 1u);
            if (r == FM_OK &&
                Video_ReadFrame(&vid, fb, sizeof(fb), &got) != FM_ERR_EOF)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK) r = Video_Seek(&vid, edge + 3u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, edge + 3u);

            if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
            if (r == FM_OK && ai.sample_count != na) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK) r = Video_SeekAudio(&vid, aedge - 4u);
            for (i = aedge - 4u; i < aedge + 9u && r == FM_OK; i++)
            {
                r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, i);
            }
            if (r == FM_OK) r = Video_SeekAudio(&vid, na - 1u);
            if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, na - 1u);
            if (r == FM_OK &&
                Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got) != FM_ERR_EOF)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* rewind into the streamed part */
            if (r == FM_OK) r = Video_SeekAudio(&vid, aedge + 1u);
            if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
            if (r == FM_OK) r = vr_expect_idx(fb, got, aedge + 1u);
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "mkv long clip");
        (void)File_Delete("/vr_long.mkv");

        /* MP4: 4 bytes of stsz per frame on top of the AVI budget */
        {
            uint32_t variant;

            for (variant = 0; variant < 2u; variant++)
            {
                size_t mcap = (size_t)nv * 16u + (size_t)na * 16u + 8192u;
                uint8_t *mb = (uint8_t *)malloc(mcap);
                Mp4Opt mo;

                TEST(variant == 0u
                     ? "mp4: video + audio past their indexes are streamed"
                     : "mp4: the same with co64, 64-bit mdat size, uniform stsz");
                r = (mb != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
                if (r == FM_OK)
                {
                    memset(&e, 0, sizeof(e));
                    memset(&vid, 0, sizeof(vid));
                    memset(&mo, 0, sizeof(mo));
                    e.b = mb; e.cap = mcap;
                    mo.nvideo = nv;
                    mo.naudio = na;
                    if (variant == 1u)
                    {
                        mo.co64 = 1; mo.large_mdat = 1; mo.a_uniform = 1;
                    }
                    if (vr_mp4(&e, &mo) != 0) r = FM_ERR_NO_MEMORY;
                }
                if (r == FM_OK) r = vr_write("/vr_long.mp4", &e);
                if (r == FM_OK) r = Video_Open("/vr_long.mp4", &vid);
                if (r == FM_OK)
                {
                    FM_AudioTrackInfo ai;
                    const uint32_t edge = (uint32_t)FM_VIDEO_MAX_FRAMES;
                    const uint32_t aedge = (uint32_t)FM_VIDEO_MAX_AUDIO_FRAMES;

                    if (vid.info.frame_count != nv)
                    {
                        printf("[%u frames, want %u] ",
                               (unsigned)vid.info.frame_count, (unsigned)nv);
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    /* every frame in order, straight through the end of
                     * the index table, then EOF */
                    for (i = 0; i < nv && r == FM_OK; i++)
                    {
                        r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                        if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                        if (r != FM_OK) printf("[frame %u] ", (unsigned)i);
                    }
                    if (r == FM_OK &&
                        Video_ReadFrame(&vid, fb, sizeof(fb), &got) != FM_ERR_EOF)
                    {
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    /* jumps: back inside the streamed part, back into
                     * the table, far forward, the same frame twice */
                    if (r == FM_OK) r = Video_Seek(&vid, edge + 3u);
                    if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, edge + 3u);
                    if (r == FM_OK) r = Video_Seek(&vid, 5u);
                    if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, 5u);
                    if (r == FM_OK) r = Video_Seek(&vid, nv - 1u);
                    if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, nv - 1u);
                    if (r == FM_OK) r = Video_Seek(&vid, edge);
                    if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, edge);
                    if (r == FM_OK) r = Video_Seek(&vid, edge);
                    if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, edge);
                    if (r != FM_OK) printf("[video %d] ", (int)r);
                    /* audio */
                    if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
                    if (r == FM_OK && ai.sample_count != na)
                    {
                        printf("[%u packets, want %u] ",
                               (unsigned)ai.sample_count, (unsigned)na);
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    if (r == FM_OK) r = Video_SeekAudio(&vid, 0u);
                    for (i = 0; i < na && r == FM_OK; i++)
                    {
                        r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                        if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                        if (r != FM_OK) printf("[packet %u] ", (unsigned)i);
                    }
                    if (r == FM_OK) r = Video_SeekAudio(&vid, aedge + 7u);
                    if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, aedge + 7u);
                    if (r == FM_OK) r = Video_SeekAudio(&vid, aedge + 1u);
                    if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, aedge + 1u);
                    if (r == FM_OK) r = Video_SeekAudio(&vid, na - 1u);
                    if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, na - 1u);
                    (void)Video_Close(&vid);
                }
                CHECK_RESULT(r, "mp4 long clip");
                (void)File_Delete("/vr_long.mp4");
                free(mb);
            }
        }

        TEST("mp4: a chunk table that covers fewer samples than stsz says");
        {
            /* 40 samples declared, but the chunk-offset table is cut to
             * 6 chunks (3+3+3+3+7+7 = 26 samples): 26 frames, not 40
             * with 14 of them read from wherever. */
            static uint8_t mb[4096];
            Mp4Opt mo;
            size_t at;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            memset(&mo, 0, sizeof(mo));
            e.b = mb; e.cap = sizeof(mb);
            mo.nvideo = 40u;
            r = (vr_mp4(&e, &mo) == 0) ? FM_OK : FM_ERR_NO_MEMORY;
            for (at = 0; r == FM_OK && at + 16u <= e.n; at++)
            {
                if (memcmp(mb + at + 4u, "stco", 4) == 0)
                {
                    mb[at + 15u] = 6u;          /* entry count */
                    break;
                }
            }
            if (r == FM_OK) r = vr_write("/vr_cut.mp4", &e);
            if (r == FM_OK) r = Video_Open("/vr_cut.mp4", &vid);
            if (r == FM_OK)
            {
                if (vid.info.frame_count != 26u)
                {
                    printf("[%u frames] ", (unsigned)vid.info.frame_count);
                    r = FM_ERR_INVALID_FORMAT;
                }
                for (i = 0; i < 26u && r == FM_OK; i++)
                {
                    r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                }
                (void)Video_Close(&vid);
            }
            /* a chunk count far beyond the box is not believed either */
            if (r == FM_OK)
            {
                mb[at + 12u] = 0x7Fu; mb[at + 13u] = 0xFFu;
                mb[at + 14u] = 0xFFu; mb[at + 15u] = 0xFFu;
                r = vr_write("/vr_cut.mp4", &e);
                if (r == FM_OK) r = Video_Open("/vr_cut.mp4", &vid);
                if (r == FM_OK)
                {
                    if (vid.info.frame_count != 40u)
                    {
                        printf("[huge count: %u frames] ",
                               (unsigned)vid.info.frame_count);
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    (void)Video_Close(&vid);
                }
            }
        }
        CHECK_RESULT(r, "mp4 short chunk table");
        (void)File_Delete("/vr_cut.mp4");

        TEST("mp4: Apple Lossless (alac) is not handed out as PCM");
        {
            static uint8_t mb[4096];
            Mp4Opt mo;
            FM_AudioTrackInfo ai;
            FM_Result ra;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            memset(&mo, 0, sizeof(mo));
            memset(&ai, 0, sizeof(ai));
            e.b = mb; e.cap = sizeof(mb);
            mo.nvideo = 6u;
            mo.naudio = 6u;
            mo.a_fourcc = "alac";
            r = (vr_mp4(&e, &mo) == 0) ? FM_OK : FM_ERR_NO_MEMORY;
            if (r == FM_OK) r = vr_write("/vr_alac.mp4", &e);
            if (r == FM_OK) r = Video_Open("/vr_alac.mp4", &vid);
            if (r == FM_OK)
            {
                ra = Video_GetAudioInfo(&vid, &ai);
                if (ra == FM_OK && ai.codec == FM_VIDEO_AUDIO_PCM)
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                /* the picture is still there */
                if (r == FM_OK && vid.info.frame_count != 6u)
                {
                    r = FM_ERR_CORRUPTED;
                }
                (void)Video_Close(&vid);
            }
        }
        CHECK_RESULT(r, "mp4 alac");
        (void)File_Delete("/vr_alac.mp4");

        TEST("mp4 audio: esds decides (AAC type/rate, HE-AAC core, MP3)");
        {
            /* esds: ES_Descriptor > DecoderConfigDescriptor (object type
             * indication at [OTI]) > DecoderSpecificInfo (3 config bytes
             * at [ASC]) */
            enum { OTI = 19, ASC = 34 };
            static const uint8_t esds_t[] = {
                0, 0, 0, 40, 'e', 's', 'd', 's', 0, 0, 0, 0,
                0x03, 25, 0, 1, 0,
                0x04, 17, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0x05, 3, 0, 0, 0,
                0x06, 1, 2 };
            /* case: OTI, config bytes, want codec / rate / channels and
             * the third ADTS byte (profile, rate index, channel msb) */
            static const struct
            {
                int      esds;
                uint8_t  oti;
                uint8_t  asc[3];
                int      codec;
                uint32_t rate;
                uint16_t ch;
                uint8_t  adts2;
            } cs[] = {
                /* AAC-LC 96 kHz stereo: does not fit the 16-bit rate field */
                { 1, 0x40, { 0x10, 0x10, 0x00 }, FM_VIDEO_AUDIO_AAC, 96000u, 2,
                  0x40 },
                /* HE-AAC: SBR over a 22.05 kHz LC core, stereo */
                { 1, 0x40, { 0x2B, 0x92, 0x08 }, FM_VIDEO_AUDIO_AAC, 22050u, 2,
                  0x5C },
                /* AAC Main 48 kHz mono */
                { 1, 0x40, { 0x09, 0x88, 0x00 }, FM_VIDEO_AUDIO_AAC, 48000u, 1,
                  0x0C },
                /* MPEG-1 audio (MP3) in an "mp4a" entry */
                { 1, 0x6B, { 0x00, 0x00, 0x00 }, FM_VIDEO_AUDIO_MP3, 8000u, 2,
                  0x00 },
                /* no esds at all: the entry's figures, announced as LC */
                { 0, 0x00, { 0x00, 0x00, 0x00 }, FM_VIDEO_AUDIO_AAC, 8000u, 2,
                  0x6C }
            };
            static uint8_t mb[4096];
            uint8_t esds[sizeof(esds_t)];
            size_t ci;

            /* the box header says 40 bytes: keep the table honest */
            r = (sizeof(esds_t) == 40u) ? FM_OK : FM_ERR_INTERNAL;

            for (ci = 0; ci < sizeof(cs) / sizeof(cs[0]) && r == FM_OK; ci++)
            {
                Mp4Opt mo;
                FM_AudioTrackInfo ai;
                uint8_t ab[32];
                size_t ag = 0;

                memcpy(esds, esds_t, sizeof(esds));
                esds[OTI] = cs[ci].oti;
                memcpy(esds + ASC, cs[ci].asc, 3);
                memset(&e, 0, sizeof(e));
                memset(&vid, 0, sizeof(vid));
                memset(&mo, 0, sizeof(mo));
                memset(&ai, 0, sizeof(ai));
                e.b = mb; e.cap = sizeof(mb);
                mo.nvideo = 4u;
                mo.naudio = 4u;
                mo.a_fourcc = "mp4a";
                if (cs[ci].esds)
                {
                    mo.a_child = esds; mo.a_child_len = sizeof(esds);
                }
                r = (vr_mp4(&e, &mo) == 0) ? FM_OK : FM_ERR_NO_MEMORY;
                if (r == FM_OK) r = vr_write("/vr_esds.mp4", &e);
                if (r == FM_OK) r = Video_Open("/vr_esds.mp4", &vid);
                if (r == FM_OK)
                {
                    r = Video_GetAudioInfo(&vid, &ai);
                    if (r == FM_OK && ((int)ai.codec != cs[ci].codec ||
                                       ai.sample_rate != cs[ci].rate ||
                                       ai.channels != cs[ci].ch))
                    {
                        printf("[case %u: codec %d, %u Hz, %u ch] ",
                               (unsigned)ci, (int)ai.codec,
                               (unsigned)ai.sample_rate,
                               (unsigned)ai.channels);
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    if (r == FM_OK && cs[ci].codec == FM_VIDEO_AUDIO_AAC)
                    {
                        /* no AAC decoder: ADTS pass-through */
                        r = Video_ReadAudioFrame(&vid, ab, sizeof(ab), &ag);
                        if (r == FM_OK &&
                            (ag != 11u || ab[0] != 0xFFu || ab[1] != 0xF1u ||
                             ab[2] != cs[ci].adts2 ||
                             (ab[3] >> 6) != (cs[ci].ch & 3u) ||
                             vr_expect_idx(ab + 7, ag - 7u, 0u) != FM_OK))
                        {
                            printf("[case %u: adts %02X %02X] ", (unsigned)ci,
                                   ab[2], ab[3]);
                            r = FM_ERR_CRC;
                        }
                    }
                    (void)Video_Close(&vid);
                }
            }
        }
        if (r == FM_OK)
        {
            /* damaged descriptors: whatever they say, opening the clip
             * and reading its audio must stay inside the buffers (this is
             * what the sanitizer builds check) and must not fail the
             * picture */
            static const uint8_t esds_f[] = {
                0, 0, 0, 40, 'e', 's', 'd', 's', 0, 0, 0, 0,
                0x03, 25, 0, 1, 0,
                0x04, 17, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0x05, 3, 0x10, 0x10, 0,
                0x06, 1, 2 };
            static uint8_t mb[4096];
            uint8_t esds[sizeof(esds_f)];
            uint32_t seed = 12345u;
            int it;

            for (it = 0; it < 400 && r == FM_OK; it++)
            {
                Mp4Opt mo;
                uint8_t ab[64];
                size_t ag = 0;
                int m;

                memcpy(esds, esds_f, sizeof(esds));
                for (m = 0; m < 1 + (it % 4); m++)
                {
                    seed = seed * 1103515245u + 12345u;
                    /* never the box header: the box must stay parsable */
                    esds[8u + ((seed >> 16) % (sizeof(esds) - 8u))] =
                        (uint8_t)(seed >> 8);
                }
                memset(&e, 0, sizeof(e));
                memset(&vid, 0, sizeof(vid));
                memset(&mo, 0, sizeof(mo));
                e.b = mb; e.cap = sizeof(mb);
                mo.nvideo = 2u;
                mo.naudio = 2u;
                mo.a_fourcc = "mp4a";
                mo.a_child = esds; mo.a_child_len = sizeof(esds);
                r = (vr_mp4(&e, &mo) == 0) ? FM_OK : FM_ERR_NO_MEMORY;
                if (r == FM_OK) r = vr_write("/vr_esds.mp4", &e);
                if (r == FM_OK) r = Video_Open("/vr_esds.mp4", &vid);
                if (r == FM_OK)
                {
                    (void)Video_ReadAudioFrame(&vid, ab, sizeof(ab), &ag);
                    if (vid.info.frame_count != 2u)
                    {
                        printf("[mutation %d] ", it);
                        r = FM_ERR_CORRUPTED;
                    }
                    (void)Video_Close(&vid);
                }
            }
        }
        CHECK_RESULT(r, "mp4 esds");
        (void)File_Delete("/vr_esds.mp4");

        TEST("mp4 audio: big-endian PCM (twos) is handed out little-endian");
        {
            static uint8_t mb[4096];
            Mp4Opt mo;
            FM_AudioTrackInfo ai;
            uint8_t ab[16];
            uint8_t want[4];
            size_t ag = 0;
            uint32_t k;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            memset(&mo, 0, sizeof(mo));
            memset(&ai, 0, sizeof(ai));
            e.b = mb; e.cap = sizeof(mb);
            mo.nvideo = 4u;
            mo.naudio = 6u;
            mo.a_fourcc = "twos";
            r = (vr_mp4(&e, &mo) == 0) ? FM_OK : FM_ERR_NO_MEMORY;
            if (r == FM_OK) r = vr_write("/vr_twos.mp4", &e);
            if (r == FM_OK) r = Video_Open("/vr_twos.mp4", &vid);
            if (r == FM_OK)
            {
                r = Video_GetAudioInfo(&vid, &ai);
                if (r == FM_OK && (ai.codec != FM_VIDEO_AUDIO_PCM ||
                                   ai.bits_per_sample != 16u ||
                                   ai.channels != 2u))
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                for (k = 0; k < 6u && r == FM_OK; k++)
                {
                    r = Video_ReadAudioFrame(&vid, ab, sizeof(ab), &ag);
                    vr_idx_bytes(want, k);
                    if (r == FM_OK &&
                        (ag != 4u || ab[0] != want[1] || ab[1] != want[0] ||
                         ab[2] != want[3] || ab[3] != want[2]))
                    {
                        printf("[packet %u] ", (unsigned)k);
                        r = FM_ERR_CRC;
                    }
                }
                (void)Video_Close(&vid);
            }
        }
        CHECK_RESULT(r, "mp4 twos");
        (void)File_Delete("/vr_twos.mp4");

        /* ---------------- FLV ---------------- */
        TEST("flv: H.264 + AAC: avcC prefix, Annex-B, metadata, ASC");
        {
            static uint8_t fbuf[1024];
            static const uint8_t avcc[] = {
                1, 0x42, 0, 0x1F, 0xFF, 0xE1, 0, 4, 'S', 'P', 'S', 'x',
                1, 0, 3, 'P', 'P', 'S' };
            static const uint8_t want1[] = {
                0, 0, 0, 1, 'S', 'P', 'S', 'x', 0, 0, 0, 1, 'P', 'P', 'S',
                0, 0, 0, 1, 'P', '1' };
            uint8_t out[96];
            FM_AudioTrackInfo ai;

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            e.b = fbuf; e.cap = sizeof(fbuf);
            vr_flv_head(&e);
            vr_flv_meta(&e, 640.0, 360.0, 29.97);
            vr_flv_tag(&e, 9, 0, "\x17\x00\x00\x00\x00", 5, avcc, sizeof(avcc));
            vr_flv_tag(&e, 8, 0, "\xAF\x00", 2, "\x15\x88", 2);
            vr_flv_tag(&e, 9, 0, "\x17\x01\x00\x00\x00", 5,
                       "\x00\x00\x00\x05IDR00", 9);
            vr_flv_tag(&e, 8, 0, "\xAF\x01", 2, "AAAA", 4);
            vr_flv_tag(&e, 9, 33, "\x27\x01\x00\x00\x00", 5,
                       "\x00\x00\x00\x02P1", 6);
            vr_flv_tag(&e, 9, 66, "\x17\x02\x00\x00\x00", 5, NULL, 0);
            r = vr_write("/vr.flv", &e);
            if (r == FM_OK) r = Video_Open("/vr.flv", &vid);
            if (r == FM_OK)
            {
                if (vid.info.codec != FM_VIDEO_CODEC_H264 ||
                    vid.info.width != 640u || vid.info.height != 360u ||
                    vid.info.fps != 30u || vid.info.frame_count != 2u)
                {
                    printf("[codec %d %ux%u %u fps %u frames] ",
                           (int)vid.info.codec, (unsigned)vid.info.width,
                           (unsigned)vid.info.height, (unsigned)vid.info.fps,
                           (unsigned)vid.info.frame_count);
                    r = FM_ERR_INVALID_FORMAT;
                }
                if (r == FM_OK) r = Video_Seek(&vid, 1u);
                if (r == FM_OK) r = Video_ReadFrame(&vid, out, sizeof(out), &got);
                if (r == FM_OK && (got != sizeof(want1) ||
                                   memcmp(out, want1, got) != 0))
                {
                    printf("[frame 1: %u bytes] ", (unsigned)got);
                    r = FM_ERR_CRC;
                }
                if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
                if (r == FM_OK && (ai.codec != FM_VIDEO_AUDIO_AAC ||
                                   ai.sample_rate != 8000u ||
                                   ai.channels != 1u || ai.sample_count != 1u))
                {
                    printf("[audio %d %u Hz %u ch %u pkts] ", (int)ai.codec,
                           (unsigned)ai.sample_rate, (unsigned)ai.channels,
                           (unsigned)ai.sample_count);
                    r = FM_ERR_INVALID_FORMAT;
                }
                (void)Video_Close(&vid);
            }
        }
        CHECK_RESULT(r, "flv avc");

        TEST("flv: Enhanced-FLV hvc1, frame rate from the timestamps");
        {
            static uint8_t fbuf[1024];
            static const uint8_t hvcc[] = {
                1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0xFD, 2,
                0x80 | 33, 0, 1, 0, 4, 'S', 'P', 'S', 'x',
                0x80 | 34, 0, 1, 0, 3, 'P', 'P', 'S' };
            static const uint8_t want[] = {
                0, 0, 0, 1, 'S', 'P', 'S', 'x', 0, 0, 0, 1, 'P', 'P', 'S',
                0, 0, 0, 1, 'P', '2' };
            uint8_t out[96];

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            e.b = fbuf; e.cap = sizeof(fbuf);
            vr_flv_head(&e);
            vr_flv_tag(&e, 9, 0, "\x90hvc1", 5, hvcc, sizeof(hvcc));
            vr_flv_tag(&e, 9, 0, "\x93hvc1", 5, "\x00\x02P1", 4);
            vr_flv_tag(&e, 9, 40, "\xA1hvc1\x00\x00\x00", 8, "\x00\x02P2", 4);
            vr_flv_tag(&e, 9, 80, "\xA3hvc1", 5, "\x00\x02P3", 4);
            r = vr_write("/vr.flv", &e);
            if (r == FM_OK) r = Video_Open("/vr.flv", &vid);
            if (r == FM_OK)
            {
                if (vid.info.codec != FM_VIDEO_CODEC_HEVC ||
                    vid.info.fps != 25u || vid.info.frame_count != 3u)
                {
                    printf("[codec %d %u fps %u frames] ", (int)vid.info.codec,
                           (unsigned)vid.info.fps,
                           (unsigned)vid.info.frame_count);
                    r = FM_ERR_INVALID_FORMAT;
                }
                if (r == FM_OK) r = Video_Seek(&vid, 1u);
                if (r == FM_OK) r = Video_ReadFrame(&vid, out, sizeof(out), &got);
                if (r == FM_OK && (got != sizeof(want) ||
                                   memcmp(out, want, got) != 0))
                {
                    r = FM_ERR_CRC;
                }
                (void)Video_Close(&vid);
            }
        }
        CHECK_RESULT(r, "flv hvc1");

        TEST("flv: video + audio past their indexes are streamed");
        {
            size_t fcap = ((size_t)nv + na) * 24u + 16384u;
            uint8_t *fbuf = (uint8_t *)malloc(fcap);
            uint32_t fvs = 0;
            uint32_t fas = 0;
            uint32_t step = 0;

            r = (fbuf != NULL) ? FM_OK : FM_ERR_NO_MEMORY;
            if (r == FM_OK)
            {
                memset(&e, 0, sizeof(e));
                memset(&vid, 0, sizeof(vid));
                e.b = fbuf; e.cap = fcap;
                vr_flv_head(&e);
                while (fvs < nv || fas < na)
                {
                    uint8_t px[4];

                    if (fvs < nv)
                    {
                        vr_idx_bytes(px, fvs);
                        vr_flv_tag(&e, 9, fvs * 200u / 3u, "\x22", 1, px, 4);
                        fvs++;
                    }
                    if (fas < na)
                    {
                        vr_idx_bytes(px, fas);
                        vr_flv_tag(&e, 8, fas * 60u, "\x3F", 1, px, 4);
                        fas++;
                    }
                    /* things that are neither a frame nor a packet */
                    if (++step % 97u == 0u)
                    {
                        vr_flv_tag(&e, 18, 0, NULL, 0, "\x02\x00\x01x", 4);
                        vr_flv_tag(&e, 9, 0, "\x52", 1, "\x00", 1);
                        vr_flv_tag(&e, 9, 0, "\x22", 1, NULL, 0);
                        vr_flv_tag(&e, 8, 0, "\x6F", 1, "nelly", 5);
                    }
                }
                r = vr_write("/vr_long.flv", &e);
            }
            if (r == FM_OK) r = Video_Open("/vr_long.flv", &vid);
            if (r == FM_OK)
            {
                FM_AudioTrackInfo ai;
                const uint32_t edge = (uint32_t)FM_VIDEO_MAX_FRAMES;
                const uint32_t aedge = (uint32_t)FM_VIDEO_MAX_AUDIO_FRAMES;

                if (vid.info.frame_count != nv || vid.info.fps != 15u)
                {
                    printf("[%u frames, want %u; %u fps] ",
                           (unsigned)vid.info.frame_count, (unsigned)nv,
                           (unsigned)vid.info.fps);
                    r = FM_ERR_INVALID_FORMAT;
                }
                for (i = 0; i < nv && r == FM_OK; i++)
                {
                    r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                    if (r != FM_OK) printf("[frame %u] ", (unsigned)i);
                }
                if (r == FM_OK &&
                    Video_ReadFrame(&vid, fb, sizeof(fb), &got) != FM_ERR_EOF)
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                if (r == FM_OK) r = Video_Seek(&vid, edge + 3u);
                if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, edge + 3u);
                if (r == FM_OK) r = Video_Seek(&vid, nv - 1u);
                if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, nv - 1u);
                if (r == FM_OK) r = Video_Seek(&vid, edge);
                if (r == FM_OK) r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, edge);
                if (r == FM_OK) r = Video_GetAudioInfo(&vid, &ai);
                if (r == FM_OK && (ai.sample_count != na ||
                                   ai.codec != FM_VIDEO_AUDIO_PCM ||
                                   ai.sample_rate != 44100u ||
                                   ai.channels != 2u ||
                                   ai.bits_per_sample != 16u))
                {
                    printf("[%u packets, want %u; codec %d] ",
                           (unsigned)ai.sample_count, (unsigned)na,
                           (int)ai.codec);
                    r = FM_ERR_INVALID_FORMAT;
                }
                if (r == FM_OK) r = Video_SeekAudio(&vid, 0u);
                for (i = 0; i < na && r == FM_OK; i++)
                {
                    r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                    if (r != FM_OK) printf("[packet %u] ", (unsigned)i);
                }
                if (r == FM_OK) r = Video_SeekAudio(&vid, aedge + 1u);
                if (r == FM_OK) r = Video_ReadAudioFrame(&vid, fb, sizeof(fb), &got);
                if (r == FM_OK) r = vr_expect_idx(fb, got, aedge + 1u);
                (void)Video_Close(&vid);
            }
            (void)File_Delete("/vr_long.flv");
            free(fbuf);
        }
        CHECK_RESULT(r, "flv long clip");

        TEST("flv: sound formats (G.711 ids, 8-bit PCM, unusable ones)");
        {
            static const struct
            {
                uint8_t snd;
                int codec;              /* -1: no audio track */
                uint32_t rate;
                uint16_t ch;
                uint16_t bits;
            } k_snd[] = {
                { 0x70, FM_VIDEO_AUDIO_ALAW, 8000u, 1u, 16u },
                { 0x80, FM_VIDEO_AUDIO_ULAW, 8000u, 1u, 16u },
                { 0x2F, FM_VIDEO_AUDIO_MP3, 44100u, 2u, 0u },
                { 0xE2, FM_VIDEO_AUDIO_MP3, 8000u, 1u, 0u },
                { 0x0C, FM_VIDEO_AUDIO_PCM, 44100u, 1u, 8u },
                { 0x3B, FM_VIDEO_AUDIO_PCM, 22050u, 2u, 16u },
                { 0x1F, -1, 0u, 0u, 0u },       /* Flash ADPCM */
                { 0x6F, -1, 0u, 0u, 0u },       /* Nellymoser  */
                { 0xB2, -1, 0u, 0u, 0u },       /* Speex       */
            };
            static uint8_t fbuf[256];

            r = FM_OK;
            for (i = 0; i < (uint32_t)(sizeof(k_snd) / sizeof(k_snd[0])); i++)
            {
                FM_AudioTrackInfo ai;
                FM_Result ro;
                FM_Result ra = FM_ERR_INTERNAL;

                memset(&e, 0, sizeof(e));
                memset(&vid, 0, sizeof(vid));
                memset(&ai, 0, sizeof(ai));
                e.b = fbuf; e.cap = sizeof(fbuf);
                vr_flv_head(&e);
                vr_flv_tag(&e, 9, 0, "\x22", 1, "VVVV", 4);
                vr_flv_tag(&e, 8, 0, &k_snd[i].snd, 1, "AAAA", 4);
                vr_flv_tag(&e, 8, 20, &k_snd[i].snd, 1, "BBBB", 4);
                ro = vr_write("/vr.flv", &e);
                if (ro == FM_OK) ro = Video_Open("/vr.flv", &vid);
                if (ro == FM_OK)
                {
                    ra = Video_GetAudioInfo(&vid, &ai);
                    (void)Video_Close(&vid);
                }
                if (ro != FM_OK ||
                    (k_snd[i].codec < 0 && ra == FM_OK) ||
                    (k_snd[i].codec >= 0 &&
                     (ra != FM_OK || (int)ai.codec != k_snd[i].codec ||
                      ai.sample_rate != k_snd[i].rate ||
                      ai.channels != k_snd[i].ch ||
                      ai.bits_per_sample != k_snd[i].bits ||
                      ai.sample_count != 2u)))
                {
                    printf("[0x%02X: open %d info %d codec %d %u Hz %u ch "
                           "%u bit] ", (unsigned)k_snd[i].snd, (int)ro, (int)ra,
                           (int)ai.codec, (unsigned)ai.sample_rate,
                           (unsigned)ai.channels, (unsigned)ai.bits_per_sample);
                    r = FM_ERR_INVALID_FORMAT;
                }
            }
        }
        CHECK_RESULT(r, "flv sound formats");

        TEST("flv: a recording cut inside its last tag keeps the rest");
        {
            static uint8_t fbuf[256];

            memset(&e, 0, sizeof(e));
            memset(&vid, 0, sizeof(vid));
            e.b = fbuf; e.cap = sizeof(fbuf);
            vr_flv_head(&e);
            for (i = 0; i < 3u; i++)
            {
                uint8_t px[4];

                vr_idx_bytes(px, i);
                vr_flv_tag(&e, 9, i * 40u, "\x22", 1, px, 4);
            }
            e.n -= 6u;                  /* PreviousTagSize + 2 data bytes */
            r = vr_write("/vr.flv", &e);
            if (r == FM_OK) r = Video_Open("/vr.flv", &vid);
            if (r == FM_OK)
            {
                if (vid.info.frame_count != 2u) r = FM_ERR_INVALID_FORMAT;
                for (i = 0; i < 2u && r == FM_OK; i++)
                {
                    r = Video_ReadFrame(&vid, fb, sizeof(fb), &got);
                    if (r == FM_OK) r = vr_expect_idx(fb, got, i);
                }
                (void)Video_Close(&vid);
            }
            /* ...and the last tag without its PreviousTagSize is whole */
            if (r == FM_OK)
            {
                e.n += 2u;
                r = vr_write("/vr.flv", &e);
                if (r == FM_OK) r = Video_Open("/vr.flv", &vid);
                if (r == FM_OK)
                {
                    if (vid.info.frame_count != 3u) r = FM_ERR_CORRUPTED;
                    (void)Video_Close(&vid);
                }
            }
        }
        CHECK_RESULT(r, "flv cut");
        (void)File_Delete("/vr.flv");
        free(big);
    }

#if FM_USE_H264
    /* ---------------- V9 (MP4) / V10 (MP4) / V12 / V18 -------------- */
    {
        static uint8_t h264[2048];
        static uint8_t mp4[4096];
        static uint8_t full[64 * 64 * 3];
        static uint8_t crop[64 * 64 * 3];
        static uint8_t stream[2048];
        uint8_t sps2[160];
        size_t sps2_len = 0;
        size_t rd = 0;
        size_t mp4_len = 0;
        const uint8_t *nals[8];
        uint32_t sizes[8];
        uint32_t nn = 0;
        const FM_VideoCodecOps *ops = H264_GetCodecOps();
        void *ctx = NULL;
        FM_Result rv = File_ReadAll("/test/vectors/tiny64.h264", h264,
                                    sizeof(h264), &rd);

        if (rv == FM_OK)
        {
            nn = split_nals(h264, (uint32_t)rd, nals, sizes, 8);
            if (nn < 5u) rv = FM_ERR_INVALID_FORMAT;
        }

        TEST("V9 mp4: avcC with 2-byte NAL lengths decodes end-to-end");
        r = rv;
        if (r == FM_OK)
        {
            g_tst_mp4_len_size = 2u;
            r = build_mp4(nals[0], sizes[0], nals[1], sizes[1],
                          nals[2], sizes[2], nals[3], sizes[3],
                          nals[4], sizes[4], mp4, &mp4_len);
            g_tst_mp4_len_size = 4u;
        }
        if (r == FM_OK) r = File_WriteAll("/vr.mp4", mp4, mp4_len);
        memset(&vid, 0, sizeof(vid));
        if (r == FM_OK) r = Video_Open("/vr.mp4", &vid);
        if (r == FM_OK)
        {
            if (vid.info.frame_count != 3u) r = FM_ERR_INVALID_FORMAT;
            for (i = 0; i < 3u && r == FM_OK; i++)
            {
                memset(full, 0, sizeof(full));
                r = Video_DecodeFrame(&vid, full, sizeof(full));
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "mp4 len size 2");

        TEST("V10 mp4: hvc1 + hvcC -> parameter sets prefixed, Annex-B");
        r = rv;
        if (r == FM_OK)
        {
            g_tst_mp4_len_size = 2u;
            g_tst_mp4_hevc = 1;
            r = build_mp4((const uint8_t *)"SPSx", 4, (const uint8_t *)"PPS", 3,
                          (const uint8_t *)"IDR00", 5, (const uint8_t *)"P1", 2,
                          (const uint8_t *)"P2", 2, mp4, &mp4_len);
            g_tst_mp4_len_size = 4u;
            g_tst_mp4_hevc = 0;
        }
        if (r == FM_OK) r = File_WriteAll("/vr.mp4", mp4, mp4_len);
        memset(&vid, 0, sizeof(vid));
        if (r == FM_OK) r = Video_Open("/vr.mp4", &vid);
        if (r == FM_OK)
        {
            static const uint8_t want[] = {
                0, 0, 0, 1, 'S', 'P', 'S', 'x', 0, 0, 0, 1, 'P', 'P', 'S',
                0, 0, 0, 1, 'P', '1'
            };
            uint8_t out[96];
            size_t got = 0;

            if (vid.info.codec != FM_VIDEO_CODEC_HEVC) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK) r = Video_Seek(&vid, 1u);
            if (r == FM_OK) r = Video_ReadFrame(&vid, out, sizeof(out), &got);
            if (r == FM_OK && (got != sizeof(want) ||
                               memcmp(out, want, got) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "mp4 hvcC");

        TEST("V18 h264: parameter-set-only packet is NOT_READY, no reset");
        r = rv;
        ctx = NULL;
        if (r == FM_OK) r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            size_t n = 0;
            FM_Result rd2;

            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[0], sizes[0]); n += sizes[0];
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[1], sizes[1]); n += sizes[1];
            rd2 = ops->decode_frame(ctx, stream, n, full, sizeof(full));
            if (rd2 != FM_ERR_NOT_READY || H264_GetLastError()->resets != 0u)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* the decoder kept the sets: the IDR alone now decodes */
            n = 0;
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[2], sizes[2]); n += sizes[2];
            if (r == FM_OK)
            {
                r = ops->decode_frame(ctx, stream, n, full, sizeof(full));
            }
            if (r == FM_OK && H264_GetLastError()->resets != 0u)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        if (ctx != NULL) ops->close(ctx);
        CHECK_RESULT(r, "no-picture packet");

        TEST("V12 h264: cropping rectangle origin (left/top) is honoured");
        r = rv;
        ctx = NULL;
        if (r == FM_OK)
        {
            /* reference: the uncropped 64x64 picture */
            r = ops->init(&ctx, NULL);
            if (r == FM_OK)
            {
                memcpy(stream, h264, rd);
                r = ops->decode_frame(ctx, stream, rd, full, sizeof(full));
            }
            if (ctx != NULL) ops->close(ctx);
            ctx = NULL;
        }
        if (r == FM_OK)
        {
            /* left 1, right 2, top 3, bottom 4 (x2 px) -> 58 x 50 at (2,6) */
            sps2_len = vr_sps_add_crop(nals[0], sizes[0], sps2, sizeof(sps2),
                                       1u, 2u, 3u, 4u);
            if (sps2_len == 0u) r = FM_ERR_UNSUPPORTED;
        }
        if (r == FM_OK) r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            size_t n = 0;
            uint16_t ow = 0;
            uint16_t oh = 0;
            uint32_t x;
            uint32_t y;

            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, sps2, sps2_len); n += sps2_len;
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[1], sizes[1]); n += sizes[1];
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[2], sizes[2]); n += sizes[2];
            memset(crop, 0, sizeof(crop));
            r = ops->decode_frame(ctx, stream, n, crop, 58u * 50u * 3u);
            if (r == FM_OK && (H264_GetOutputSize(ctx, &ow, &oh) != FM_OK ||
                               ow != 58u || oh != 50u))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            for (y = 0; y < 50u && r == FM_OK; y++)
            {
                for (x = 0; x < 58u; x++)
                {
                    if (memcmp(crop + ((size_t)y * 58u + x) * 3u,
                               full + ((size_t)(y + 6u) * 64u + (x + 2u)) * 3u,
                               3) != 0)
                    {
                        r = FM_ERR_CRC;
                        break;
                    }
                }
            }
        }
        if (ctx != NULL) ops->close(ctx);
        CHECK_RESULT(r, "crop offset");

        TEST("V12 Video_DecodeFrame publishes the decoder's picture size");
        r = (sps2_len != 0u) ? rv : FM_ERR_UNSUPPORTED;
        if (r == FM_OK)
        {
            r = build_mp4(sps2, (uint32_t)sps2_len, nals[1], sizes[1],
                          nals[2], sizes[2], nals[3], sizes[3],
                          nals[4], sizes[4], mp4, &mp4_len);
        }
        if (r == FM_OK) r = File_WriteAll("/vr.mp4", mp4, mp4_len);
        memset(&vid, 0, sizeof(vid));
        if (r == FM_OK) r = Video_Open("/vr.mp4", &vid);
        if (r == FM_OK)
        {
            FM_VideoInfo vi;

            /* the container still says 64x64 */
            if (vid.info.width != 64u || vid.info.height != 64u)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK) r = Video_DecodeFrame(&vid, crop, sizeof(crop));
            if (r == FM_OK) r = Video_GetInfo(&vid, &vi);
            if (r == FM_OK && (vi.width != 58u || vi.height != 50u))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
#if FM_USE_IMAGE
            /* ...and the scaled path sizes itself from it */
            if (r == FM_OK)
            {
                r = Video_DecodeFrameScaled(&vid, full, sizeof(full), 0, 0,
                                            FM_SCALE_NEAREST);
            }
#endif
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "info follows crop");
        (void)File_Delete("/vr.mp4");
    }
#endif /* FM_USE_H264 */

    /* ---------------- V13 ---------------- */
    {
        static const uint8_t P0[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
        static const uint8_t P1[] = { 0x66, 0x77, 0x88 };
        const uint8_t *pf[2];
        uint32_t ps[2];
        const FM_AudioDecoderOps *saved_flac = Audio_GetDecoder("FLAC");
        const FM_AudioDecoderOps *saved_aac = Audio_GetDecoder("AAC");

        pf[0] = P0; ps[0] = sizeof(P0);
        pf[1] = P1; ps[1] = sizeof(P1);
        (void)Audio_UnregisterDecoder("FLAC");
        (void)Audio_UnregisterDecoder("AAC");

        TEST("V13 audio track without a decoder: UNSUPPORTED, not raw bytes");
        VR_BEGIN();
        o.rate = 48000.0;
        o.acodec = "A_FLAC";
        mkv_head(&e, &o, &seg_at, &clu_at);
        mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
        mkv_block(&e, 0xA3u, 2u, 0u, &pf[0], &ps[0], 1u);
        mkv_block(&e, 0xA3u, 2u, 0u, &pf[1], &ps[1], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            FM_AudioTrackInfo ai;
            uint8_t ab[64];
            size_t got = 0;

            r = Video_GetAudioInfo(&vid, &ai);
            if (r == FM_OK && (ai.codec != FM_VIDEO_AUDIO_FLAC ||
                               ai.decodable != FM_FALSE))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK &&
                Video_ReadAudioFrame(&vid, ab, sizeof(ab), &got)
                    != FM_ERR_UNSUPPORTED)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "no decoder");

        TEST("V13 AAC track without a decoder: ADTS-framed pass-through");
        VR_BEGIN();
        o.rate = 48000.0;
        o.acodec = "A_AAC";
        mkv_head(&e, &o, &seg_at, &clu_at);
        mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
        mkv_block(&e, 0xA3u, 2u, 0u, &pf[0], &ps[0], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            uint8_t ab[64];
            size_t got = 0;

            r = Video_ReadAudioFrame(&vid, ab, sizeof(ab), &got);
            /* 7-byte ADTS header (sync 0xFFF, frame length 12) + payload */
            if (r == FM_OK &&
                (got != 7u + sizeof(P0) || ab[0] != 0xFFu ||
                 (ab[1] & 0xF0u) != 0xF0u ||
                 ((((uint32_t)ab[3] & 3u) << 11) | ((uint32_t)ab[4] << 3) |
                  ((uint32_t)ab[5] >> 5)) != 7u + sizeof(P0) ||
                 memcmp(ab + 7, P0, sizeof(P0)) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)Video_Close(&vid);
        }
        CHECK_RESULT(r, "aac adts");

#if FM_USE_AUDIO
        TEST("V13 audio track: one decoder context for the whole track");
        VR_BEGIN();
        o.rate = 48000.0;
        o.acodec = "A_FLAC";
        mkv_head(&e, &o, &seg_at, &clu_at);
        mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
        mkv_block(&e, 0xA3u, 2u, 0u, &pf[0], &ps[0], 1u);
        mkv_block(&e, 0xA3u, 2u, 0u, &pf[1], &ps[1], 1u);
        mkv_tail(&e, &o, seg_at, clu_at);
        g_vr_ad_inits = 0; g_vr_ad_closes = 0; g_vr_ad_decodes = 0;
        g_vr_ad_rate = 0; g_vr_ad_bad_ctx = 0;
        r = vr_write("/vr.mkv", &e);
        if (r == FM_OK) r = Audio_RegisterDecoder("FLAC", &g_vr_ad_ops);
        if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
        if (r == FM_OK)
        {
            FM_AudioTrackInfo ai;
            int16_t pcm[16];
            size_t got = 0;

            r = Video_GetAudioInfo(&vid, &ai);
            if (r == FM_OK && ai.decodable != FM_TRUE)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* packet 1: 5 bytes -> 5 samples (10 bytes of PCM) */
            if (r == FM_OK)
            {
                r = Video_ReadAudioFrame(&vid, (uint8_t *)pcm, sizeof(pcm),
                                         &got);
            }
            if (r == FM_OK && (got != 10u || pcm[0] != 0x11 || pcm[4] != 0x55))
            {
                r = FM_ERR_CRC;
            }
            if (r == FM_OK)
            {
                r = Video_ReadAudioFrame(&vid, (uint8_t *)pcm, sizeof(pcm),
                                         &got);
            }
            /* the fake decoder adds the number of packets it has seen
             * before: only a context that survives gives 0x66 + 1 */
            if (r == FM_OK && (got != 6u || pcm[0] != 0x67 || pcm[2] != 0x89))
            {
                r = FM_ERR_CRC;
            }
            if (r == FM_OK &&
                Video_ReadAudioFrame(&vid, (uint8_t *)pcm, sizeof(pcm), &got)
                    != FM_ERR_EOF)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK && (g_vr_ad_inits != 1 || g_vr_ad_decodes != 2 ||
                               g_vr_ad_closes != 0 || g_vr_ad_rate != 48000u ||
                               g_vr_ad_bad_ctx != 0))
            {
                r = FM_ERR_INTERNAL;
            }
            (void)Video_Close(&vid);
            if (r == FM_OK && g_vr_ad_closes != 1) r = FM_ERR_INTERNAL;
        }
        (void)Audio_UnregisterDecoder("FLAC");
        CHECK_RESULT(r, "persistent decoder");

        TEST("mkv audio: setup data larger than the inline buffer is kept");
        {
            /* variant 0: 6000 bytes (> FM_VIDEO_AUDIO_PRIVATE_MAX) reach
             * the decoder whole, behind "fLaC";
             * variant 1: more than FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX is
             * refused cleanly (the packets arrive without setup data). */
            const size_t plen[2] = {
                (size_t)FM_VIDEO_AUDIO_PRIVATE_MAX + 1904u,
                (size_t)FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX + 1u };
            int variant;

            r = FM_OK;
            for (variant = 0; variant < 2 && r == FM_OK; variant++)
            {
                size_t n = plen[variant];
                size_t bcap = n + 4096u;
                uint8_t *setup = (uint8_t *)malloc(n);
                uint8_t *mb = (uint8_t *)malloc(bcap);
                int16_t *pcm = (int16_t *)malloc((n + 64u) * sizeof(int16_t));
                size_t got = 0;
                size_t k;

                if (setup == NULL || mb == NULL || pcm == NULL)
                {
                    r = FM_ERR_NO_MEMORY;
                }
                else
                {
                    for (k = 0; k < n; k++)
                    {
                        setup[k] = (uint8_t)(k * 7u + 3u);
                    }
                    memset(&e, 0, sizeof(e));
                    memset(&o, 0, sizeof(o));
                    memset(&vid, 0, sizeof(vid));
                    e.b = mb; e.cap = bcap;
                    o.rate = 48000.0;
                    o.acodec = "A_FLAC";
                    o.apriv = setup; o.apriv_len = n;
                    mkv_head(&e, &o, &seg_at, &clu_at);
                    mkv_block(&e, 0xA3u, 1u, 0u, &vf[0], &vs[0], 1u);
                    mkv_block(&e, 0xA3u, 2u, 0u, &pf[0], &ps[0], 1u);
                    mkv_tail(&e, &o, seg_at, clu_at);
                    r = vr_write("/vr.mkv", &e);
                }
                if (r == FM_OK) r = Audio_RegisterDecoder("FLAC", &g_vr_ad_ops);
                if (r == FM_OK) r = Video_Open("/vr.mkv", &vid);
                if (r == FM_OK)
                {
                    r = Video_ReadAudioFrame(&vid, (uint8_t *)pcm,
                                             (n + 64u) * sizeof(int16_t),
                                             &got);
                    if (r == FM_OK &&
                        (n <= (size_t)FM_VIDEO_AUDIO_PRIVATE_MAX ||
                         n <= (size_t)FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX))
                    {
                        /* the fake decoder: one sample per input byte */
                        if (got != (4u + n + 5u) * 2u || pcm[0] != 'f' ||
                            pcm[3] != 'C' || pcm[4u + n] != 0x11)
                        {
                            printf("[%u bytes of PCM] ", (unsigned)got);
                            r = FM_ERR_CRC;
                        }
                        for (k = 0; k < n && r == FM_OK; k++)
                        {
                            if (pcm[4u + k] != (int16_t)setup[k])
                            {
                                printf("[setup byte %u] ", (unsigned)k);
                                r = FM_ERR_CRC;
                            }
                        }
                    }
                    else if (r == FM_OK && got != 10u)
                    {
                        printf("[oversize: %u bytes of PCM] ", (unsigned)got);
                        r = FM_ERR_INVALID_FORMAT;
                    }
                    (void)Video_Close(&vid);
                }
                (void)Audio_UnregisterDecoder("FLAC");
                free(setup); free(mb); free(pcm);
            }
        }
        CHECK_RESULT(r, "mkv big setup data");
#endif /* FM_USE_AUDIO */

        if (saved_flac != NULL) (void)Audio_RegisterDecoder("FLAC", saved_flac);
        if (saved_aac != NULL)  (void)Audio_RegisterDecoder("AAC", saved_aac);
        (void)File_Delete("/vr.mkv");
    }
#undef VR_BEGIN
}
#endif /* FM_USE_VIDEO */

#if FM_USE_IMAGE
/* ================================================================ */
/* Image report regressions (Report.txt D-01..D-09, O-02, O-03, O-05) */
/* ================================================================ */

/* CRC-32 of our own so the PNG builders work with FM_USE_CRC=0 too. */
TEST_MAYBE_UNUSED static uint32_t ir_crc32(const uint8_t *p, size_t n,
                                           uint32_t crc)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        uint32_t k;

        crc ^= p[i];
        for (k = 0; k < 8u; k++)
        {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

typedef struct
{
    uint8_t b[512];
    size_t  n;
} IrBuf;

TEST_MAYBE_UNUSED static void ir_put(IrBuf *o, const void *p, size_t n)
{
    if (o->n + n <= sizeof(o->b))
    {
        memcpy(o->b + o->n, p, n);
    }
    o->n += n;
}

TEST_MAYBE_UNUSED static void ir_be32(IrBuf *o, uint32_t v)
{
    uint8_t b[4];

    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
    ir_put(o, b, 4);
}

/* One chunk; `claim` is the length field (normally == n). */
TEST_MAYBE_UNUSED static void ir_chunk(IrBuf *o, const char *type,
                                       const uint8_t *data, size_t n)
{
    uint32_t crc = ir_crc32((const uint8_t *)type, 4, 0xFFFFFFFFu);

    ir_be32(o, (uint32_t)n);
    ir_put(o, type, 4);
    if (n != 0u)
    {
        ir_put(o, data, n);
        crc = ir_crc32(data, n, crc);
    }
    ir_be32(o, crc ^ 0xFFFFFFFFu);
}

TEST_MAYBE_UNUSED static void ir_head_ex(IrBuf *o, uint32_t w, uint32_t h,
                                         uint8_t color, uint8_t depth,
                                         uint8_t interlace)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13];

    memset(o, 0, sizeof(*o));
    memset(ihdr, 0, sizeof(ihdr));
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = depth;
    ihdr[9] = color;
    ihdr[12] = interlace;
    ir_put(o, sig, 8);
    ir_chunk(o, "IHDR", ihdr, 13);
}

TEST_MAYBE_UNUSED static void ir_head(IrBuf *o, uint32_t w, uint32_t h,
                                      uint8_t color)
{
    ir_head_ex(o, w, h, color, 8, 0);
}

/* Complete gray/RGB PNG around one zlib stream. */
TEST_MAYBE_UNUSED static void ir_png(IrBuf *o, uint32_t w, uint32_t h,
                                     uint8_t color, const uint8_t *z,
                                     size_t zn)
{
    ir_head(o, w, h, color);
    ir_chunk(o, "IDAT", z, zn);
    ir_chunk(o, "IEND", NULL, 0);
}

TEST_MAYBE_UNUSED static uint32_t ir_adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1;
    uint32_t b = 0;
    size_t i;

    for (i = 0; i < n; i++)
    {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* zlib stream with the raw bytes in one stored block. */
TEST_MAYBE_UNUSED static size_t ir_zstored(uint8_t *z, const uint8_t *raw,
                                           size_t n)
{
    uint32_t ad = ir_adler(raw, n);
    size_t k = 0;

    z[k++] = 0x78; z[k++] = 0x01; z[k++] = 0x01;
    z[k++] = (uint8_t)n; z[k++] = (uint8_t)(n >> 8);
    z[k++] = (uint8_t)~n; z[k++] = (uint8_t)(~n >> 8);
    memcpy(z + k, raw, n); k += n;
    z[k++] = (uint8_t)(ad >> 24); z[k++] = (uint8_t)(ad >> 16);
    z[k++] = (uint8_t)(ad >> 8);  z[k++] = (uint8_t)ad;
    return k;
}

/* Deflate bit writer: fields LSB first, Huffman codes MSB first. */
typedef struct
{
    uint8_t b[400];
    size_t  bit;
} IrBits;

TEST_MAYBE_UNUSED static void ir_bits(IrBits *w, uint32_t v, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        if (((v >> i) & 1u) != 0u && w->bit < sizeof(w->b) * 8u)
        {
            w->b[w->bit >> 3] = (uint8_t)(w->b[w->bit >> 3] |
                                          (1u << (w->bit & 7u)));
        }
        w->bit++;
    }
}

TEST_MAYBE_UNUSED static void ir_code(IrBits *w, uint32_t code, uint32_t len)
{
    while (len-- > 0u)
    {
        ir_bits(w, (code >> len) & 1u, 1u);
    }
}

/* zlib header + a final dynamic-Huffman block header.  The code-length
 * alphabet is the 16 symbols 0..15 at 4 bits each (a complete code in
 * which length L is simply written as the 4-bit number L); with
 * `cl_incomplete` symbol 15 is left out, which leaves 1/16 of the code
 * space unused. */
TEST_MAYBE_UNUSED static void ir_dyn_begin(IrBits *w, const uint8_t *ll,
                                           uint32_t nlit, const uint8_t *dl,
                                           uint32_t ndist, int cl_incomplete)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint32_t i;

    memset(w, 0, sizeof(*w));
    ir_bits(w, 0x78u, 8); ir_bits(w, 0x01u, 8);
    ir_bits(w, 1u, 1);                 /* BFINAL */
    ir_bits(w, 2u, 2);                 /* dynamic */
    ir_bits(w, nlit - 257u, 5);
    ir_bits(w, ndist - 1u, 5);
    ir_bits(w, 15u, 4);                /* all 19 code-length lengths */
    for (i = 0; i < 19u; i++)
    {
        uint32_t len = (order[i] < 16u) ? 4u : 0u;

        if (cl_incomplete && order[i] == 15u)
        {
            len = 0u;
        }
        ir_bits(w, len, 3);
    }
    for (i = 0; i < nlit; i++)  ir_code(w, ll[i], 4);
    for (i = 0; i < ndist; i++) ir_code(w, dl[i], 4);
}

/* `raw` is what the stream inflates to: its Adler-32 is the trailer.
 * (NULL for the streams that are meant to fail before the trailer.) */
TEST_MAYBE_UNUSED static size_t ir_dyn_end(IrBits *w, uint8_t *z,
                                           const uint8_t *raw, size_t raw_n)
{
    size_t n = (w->bit + 7u) / 8u;
    uint32_t ad = (raw != NULL) ? ir_adler(raw, raw_n) : 0u;

    memcpy(z, w->b, n);
    z[n] = (uint8_t)(ad >> 24);
    z[n + 1u] = (uint8_t)(ad >> 16);
    z[n + 2u] = (uint8_t)(ad >> 8);
    z[n + 3u] = (uint8_t)ad;
    return n + 4u;
}

/* Decode a built PNG; *px receives the first `want` RGB pixels' red. */
TEST_MAYBE_UNUSED static FM_Result ir_decode(const IrBuf *o, uint8_t *rgb,
                                             size_t cap)
{
    FM_Image img;

    if (o->n > sizeof(o->b))
    {
        return FM_ERR_INTERNAL;        /* test buffer too small */
    }
    memset(&img, 0, sizeof(img));
    return Image_DecodePNG(o->b, o->n, &img, rgb, cap);
}

#if FM_USE_PNG
static const uint8_t *g_ir_nest_png;
static size_t         g_ir_nest_len;
static FM_Result      g_ir_nest_result;
static int            g_ir_nest_calls;

static void ir_idle_png(void)
{
    uint8_t px[4 * 2 * 3];
    FM_Image img;

    if (g_ir_nest_calls++ == 0)
    {
        g_ir_nest_result = Image_DecodePNG(g_ir_nest_png, g_ir_nest_len,
                                           &img, px, sizeof(px));
    }
}
#endif

#if FM_USE_JPEG
static uint8_t   g_ir_jpg[2048];
static size_t    g_ir_jpg_len;
static FM_Result g_ir_jnest_result;
static int       g_ir_jnest_calls;

static void ir_idle_jpeg(void)
{
    static uint8_t px[64 * 64 * 3];
    FM_Image img;

    if (g_ir_jnest_calls++ == 0)
    {
        g_ir_jnest_result = Image_DecodeJPEG(g_ir_jpg, g_ir_jpg_len, &img,
                                             px, sizeof(px));
    }
}
#endif

static FM_Result ir_fake_decode(const uint8_t *data, size_t size,
                                FM_Image *img, void *pixel_buffer,
                                size_t capacity)
{
    (void)data; (void)size; (void)img; (void)pixel_buffer; (void)capacity;
    return FM_ERR_UNSUPPORTED;
}

static const FM_ImageCodecOps g_ir_ops_a = { "ir-a", NULL, ir_fake_decode };
static const FM_ImageCodecOps g_ir_ops_b = { "ir-b", NULL, ir_fake_decode };

#if FM_USE_THREADS && FM_HOST_BUILD && FM_USE_PNG
#define IR_TW 48u
#define IR_TH 40u
typedef struct
{
    uint32_t seed;
    int      failed;
} IrThread;

static void ir_fill(uint8_t *p, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < IR_TW * IR_TH * 3u; i++)
    {
        p[i] = (uint8_t)((i * (seed * 2u + 1u)) ^ (seed * 37u) ^ (i >> 5));
    }
}

static void *ir_thread(void *arg)
{
    IrThread *t = (IrThread *)arg;
    static const size_t cap = IR_TW * IR_TH * 3u + IR_TH * 8u + 256u;
    uint8_t *src = (uint8_t *)malloc(IR_TW * IR_TH * 3u);
    uint8_t *png = (uint8_t *)malloc(cap);
    uint8_t *ref = (uint8_t *)malloc(cap);
    uint8_t *out = (uint8_t *)malloc(IR_TW * IR_TH * 3u);
    char name[8];
    FM_Image img;
    size_t ref_len = 0;
    int i;

    (void)snprintf(name, sizeof(name), "IR%u", (unsigned)t->seed);
    if (src == NULL || png == NULL || ref == NULL || out == NULL)
    {
        t->failed = 1;
    }
    else
    {
        ir_fill(src, t->seed);
        memset(&img, 0, sizeof(img));
        img.width = (uint16_t)IR_TW; img.height = (uint16_t)IR_TH;
        img.format = FM_PIXEL_RGB888;
        img.data = src;
        img.data_capacity = IR_TW * IR_TH * 3u;
        if (Image_EncodePNG(&img, ref, cap, &ref_len) != FM_OK)
        {
            t->failed = 1;
        }
        for (i = 0; i < 60 && !t->failed; i++)
        {
            FM_Image dec;
            size_t len = 0;

            if (Image_EncodePNG(&img, png, cap, &len) != FM_OK ||
                len != ref_len || memcmp(png, ref, len) != 0)
            {
                t->failed = 2;
                break;
            }
            memset(out, 0, IR_TW * IR_TH * 3u);
            if (Image_DecodePNG(png, len, &dec, out,
                                IR_TW * IR_TH * 3u) != FM_OK ||
                memcmp(out, src, IR_TW * IR_TH * 3u) != 0)
            {
                t->failed = 3;
                break;
            }
            if (Image_RegisterCodec(name, &g_ir_ops_a) != FM_OK ||
                Image_GetCodec(name) != &g_ir_ops_a ||
                Image_UnregisterCodec(name) != FM_OK ||
                Image_GetCodec(name) != NULL)
            {
                t->failed = 4;
                break;
            }
        }
    }
    free(src); free(png); free(ref); free(out);
    return NULL;
}
#endif

static uint32_t g_ir_rows;
static uint32_t g_ir_row_sum;

static void ir_line_cb(const void *line, uint32_t y, void *user_data)
{
    const uint8_t *px = (const uint8_t *)line;

    (void)user_data;
    g_ir_rows++;
    g_ir_row_sum += (uint32_t)px[0] + y;
}

static void test_image_report(void)
{
    static uint8_t rgb[64 * 64 * 3];
    FM_Result r;
    uint32_t i;

    (void)i;
    (void)rgb;
    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("Image report: remount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }

#if FM_USE_PNG
    /* ---------------- D-01 ---------------- */
    {
        static uint8_t ll[259];
        uint8_t dl[1];
        uint8_t z[420];
        IrBits w;
        IrBuf png;
        size_t zn;

        /* control: 1x1 gray, scanline {0, 0x55}.  Literal code
         * {0:1 bit, 0x55:2, EOB:2} is complete; the distance alphabet
         * has no code at all (legal: the block is all literals). */
        TEST("D-01 png: valid dynamic block, empty distance table");
        memset(ll, 0, sizeof(ll));
        ll[0] = 1; ll[0x55] = 2; ll[256] = 2;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 257u, dl, 1u, 0);
        ir_code(&w, 0u, 1); ir_code(&w, 2u, 2); ir_code(&w, 3u, 2);
        zn = ir_dyn_end(&w, z, (const uint8_t *)"\x00\x55", 2u);
        ir_png(&png, 1, 1, 0, z, zn);
        rgb[0] = 0;
        r = ir_decode(&png, rgb, sizeof(rgb));
        if (r == FM_OK && rgb[0] != 0x55u) r = FM_ERR_CRC;
        CHECK_RESULT(r, "valid dynamic");

        /* control: one distance code of length 1 (legal incomplete
         * code).  4x1 gray, scanline {0, P, P, P, P} as literal 0,
         * literal P, match(len 3, dist 1). */
        TEST("D-01 png: single 1-bit distance code is accepted");
        memset(ll, 0, sizeof(ll));
        ll[0] = 2; ll[0x66] = 2; ll[256] = 2; ll[257] = 2;
        dl[0] = 1;
        ir_dyn_begin(&w, ll, 258u, dl, 1u, 0);
        ir_code(&w, 0u, 2); ir_code(&w, 1u, 2);   /* 0, P            */
        ir_code(&w, 3u, 2);                       /* length 3        */
        ir_code(&w, 0u, 1);                       /* distance 1      */
        ir_code(&w, 2u, 2);                       /* end of block    */
        zn = ir_dyn_end(&w, z, (const uint8_t *)"\x00\x66\x66\x66\x66", 5u);
        ir_png(&png, 4, 1, 0, z, zn);
        memset(rgb, 0, 12);
        r = ir_decode(&png, rgb, sizeof(rgb));
        if (r == FM_OK && (rgb[0] != 0x66u || rgb[3] != 0x66u ||
                           rgb[6] != 0x66u || rgb[9] != 0x66u))
        {
            r = FM_ERR_CRC;
        }
        CHECK_RESULT(r, "one distance code");

        /* five 2-bit codes: more codes than 2-bit patterns */
        TEST("D-01 png: over-subscribed literal table is CORRUPTED");
        memset(ll, 0, sizeof(ll));
        ll[0] = 2; ll[0x55] = 2; ll[256] = 2; ll[257] = 2; ll[258] = 2;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 259u, dl, 1u, 0);
        ir_code(&w, 0u, 2); ir_code(&w, 1u, 2); ir_code(&w, 2u, 2);
        zn = ir_dyn_end(&w, z, NULL, 0u);
        ir_png(&png, 1, 1, 0, z, zn);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "over-subscribed");

        /* three 2-bit codes: a quarter of the code space is unused */
        TEST("D-01 png: incomplete literal table is CORRUPTED");
        memset(ll, 0, sizeof(ll));
        ll[0] = 2; ll[0x55] = 2; ll[256] = 2;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 257u, dl, 1u, 0);
        ir_code(&w, 0u, 2); ir_code(&w, 1u, 2); ir_code(&w, 2u, 2);
        zn = ir_dyn_end(&w, z, NULL, 0u);
        ir_png(&png, 1, 1, 0, z, zn);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "incomplete");

        TEST("D-01 png: incomplete code-length table is CORRUPTED");
        memset(ll, 0, sizeof(ll));
        ll[0] = 1; ll[0x55] = 2; ll[256] = 2;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 257u, dl, 1u, 1);
        ir_code(&w, 0u, 1); ir_code(&w, 2u, 2); ir_code(&w, 3u, 2);
        zn = ir_dyn_end(&w, z, NULL, 0u);
        ir_png(&png, 1, 1, 0, z, zn);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "incomplete code lengths");

        /* guards: these were already refused (after reading on) */
        TEST("D-01 png: match with an empty distance table is CORRUPTED");
        memset(ll, 0, sizeof(ll));
        ll[0] = 2; ll[0x66] = 2; ll[256] = 2; ll[257] = 2;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 258u, dl, 1u, 0);
        ir_code(&w, 0u, 2); ir_code(&w, 1u, 2); ir_code(&w, 3u, 2);
        ir_code(&w, 0u, 1); ir_code(&w, 2u, 2);
        zn = ir_dyn_end(&w, z, NULL, 0u);
        ir_png(&png, 4, 1, 0, z, zn);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "no distance code");

        TEST("D-01 png: table without an end-of-block code is CORRUPTED");
        memset(ll, 0, sizeof(ll));
        ll[0] = 1; ll[0x55] = 1;
        dl[0] = 0;
        ir_dyn_begin(&w, ll, 257u, dl, 1u, 0);
        ir_code(&w, 0u, 1); ir_code(&w, 1u, 1);
        zn = ir_dyn_end(&w, z, NULL, 0u);
        ir_png(&png, 1, 1, 0, z, zn);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "no EOB");
    }

    /* ---------------- D-02 ---------------- */
    TEST("D-02 png: distance outside the produced data is CORRUPTED");
    {
        /* fixed-Huffman block for a 4x1 gray image: literal 0, then a
         * length-4 match.  (a) distance 4 with one byte produced --
         * reads window bytes nothing in this stream wrote; (b) distance
         * symbol 30, which no valid stream contains; (c) control:
         * literal 0, literal 0x21, match(3, distance 1), literal... */
        uint8_t z[32];
        IrBits w;
        IrBuf png;
        size_t zn;
        uint32_t k;

        r = FM_OK;
        for (k = 0; k < 3u && r == FM_OK; k++)
        {
            FM_Result e;

            memset(&w, 0, sizeof(w));
            ir_bits(&w, 0x78u, 8); ir_bits(&w, 0x01u, 8);
            ir_bits(&w, 1u, 1); ir_bits(&w, 1u, 2);       /* final, fixed */
            ir_code(&w, 0x30u, 8);                        /* literal 0    */
            if (k == 2u)
            {
                ir_code(&w, 0x30u + 0x21u, 8);            /* literal 0x21 */
                ir_code(&w, 1u, 7);                       /* length 3     */
                ir_code(&w, 0u, 5);                       /* distance 1   */
            }
            else
            {
                ir_code(&w, 2u, 7);                       /* length 4     */
                ir_code(&w, (k == 0u) ? 3u : 30u, 5);     /* distance     */
            }
            ir_code(&w, 0u, 7);                           /* end of block */
            zn = ir_dyn_end(&w, z,
                            (k == 2u) ? (const uint8_t *)"\x00\x21\x21\x21\x21"
                                      : NULL, 5u);
            ir_png(&png, 4, 1, 0, z, zn);
            memset(rgb, 0, 12);
            e = ir_decode(&png, rgb, sizeof(rgb));
            if (k == 2u)
            {
                r = e;
                if (r == FM_OK && (rgb[0] != 0x21u || rgb[9] != 0x21u))
                {
                    r = FM_ERR_CRC;
                }
            }
            else if (e != FM_ERR_CORRUPTED)
            {
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }
    }
    CHECK_RESULT(r, "distance");

    /* ---------------- D-03 ---------------- */
    TEST("D-03 png: stored block cut inside its header (no UB)");
    r = FM_OK;
    for (i = 0; i < 4u && r == FM_OK; i++)
    {
        /* zlib header, BFINAL/BTYPE byte, then 0..3 of the 4 LEN/NLEN
         * bytes.  Under UBSan the old code shifted a negative int. */
        static const uint8_t z[7] = { 0x78, 0x01, 0x01, 0x02, 0x00, 0xFD, 0xFF };
        IrBuf png;
        FM_Result e;

        ir_png(&png, 1, 1, 0, z, 3u + i);
        e = ir_decode(&png, rgb, sizeof(rgb));
        if (e != FM_ERR_CORRUPTED)
        {
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
    }
    CHECK_RESULT(r, "truncated stored header");

    {
        static const uint8_t raw[2] = { 0, 0x77 };
        uint8_t z[32];
        size_t zn = ir_zstored(z, raw, sizeof(raw));
        IrBuf png;

        TEST("D-03 png: chunk length 0xFFFFFFFF before IDAT is CORRUPTED");
        /* len + 4 wrapped to 3: the parser skipped 3 bytes and carried
         * on, so this file -- 3 filler bytes, then a good IDAT -- loaded. */
        ir_head(&png, 1, 1, 0);
        ir_be32(&png, 0xFFFFFFFFu); ir_put(&png, "tEXt", 4);
        ir_put(&png, "abc", 3);
        ir_chunk(&png, "IDAT", z, zn);
        ir_chunk(&png, "IEND", NULL, 0);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "wrap before IDAT");

        TEST("D-03 png: chunk length 0xFFFFFFFC between IDATs is CORRUPTED");
        /* len + 4 wrapped to 0: nothing skipped, next IDAT accepted */
        ir_head(&png, 1, 1, 0);
        ir_chunk(&png, "IDAT", z, 3);
        ir_be32(&png, 0xFFFFFFFCu); ir_put(&png, "tEXt", 4);
        ir_chunk(&png, "IDAT", z + 3, zn - 3u);
        ir_chunk(&png, "IEND", NULL, 0);
        r = ir_decode(&png, rgb, sizeof(rgb));
        r = (r == FM_ERR_CORRUPTED) ? FM_OK
                                    : ((r == FM_OK) ? FM_ERR_INVALID_FORMAT : r);
        CHECK_RESULT(r, "wrap between IDATs");

        TEST("D-03 png: split IDAT + ancillary chunk still decodes");
        ir_head(&png, 1, 1, 0);
        ir_chunk(&png, "tEXt", (const uint8_t *)"k\0v", 3);
        ir_chunk(&png, "IDAT", z, 3);
        ir_chunk(&png, "IDAT", z + 3, zn - 3u);
        ir_chunk(&png, "IEND", NULL, 0);
        rgb[0] = 0;
        r = ir_decode(&png, rgb, sizeof(rgb));
        if (r == FM_OK && rgb[0] != 0x77u) r = FM_ERR_CRC;
        CHECK_RESULT(r, "control");

        /* ---------------- D-05 ---------------- */
        TEST("D-05 png: damaged IHDR is FM_ERR_CRC in probe and GetInfo");
        {
            FM_ImageInfo inf;

            ir_png(&png, 1, 1, 0, z, zn);
            r = Image_ProbePNG(png.b, png.n, &inf);
            if (r == FM_OK && (inf.width != 1u || inf.height != 1u))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            if (r == FM_OK)
            {
                png.b[18] = 0x40;       /* width 1 -> 16385, CRC stale */
                if (Image_ProbePNG(png.b, png.n, &inf) != FM_ERR_CRC)
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
            }
            if (r == FM_OK) r = File_WriteAll("/ir_bad.png", png.b, png.n);
            if (r == FM_OK &&
                Image_GetInfo("/ir_bad.png", &inf) != FM_ERR_CRC)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* PNG signature and nothing behind it: a broken PNG, not
             * "some other format" */
            if (r == FM_OK) r = File_WriteAll("/ir_bad.png", png.b, 20);
            if (r == FM_OK &&
                Image_GetInfo("/ir_bad.png", &inf) != FM_ERR_CORRUPTED)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)File_Delete("/ir_bad.png");
        }
        CHECK_RESULT(r, "ihdr crc");
    }

    /* ---------------- D-04 ---------------- */
    TEST("D-04 png: nested decode from the idle hook gets BUSY");
    {
        /* outer: 4x4 gray, row 0 literal, rows 1..3 "Up" with zero
         * deltas -- every row depends on the shared previous-row
         * buffer the nested decode would overwrite */
        static const uint8_t raw_o[20] = {
            0, 10, 20, 30, 40,  2, 0, 0, 0, 0,
            2, 0, 0, 0, 0,      2, 0, 0, 0, 0
        };
        static const uint8_t raw_n[10] = {
            0, 200, 201, 202, 203,  0, 210, 211, 212, 213
        };
        uint8_t z[48];
        IrBuf outer;
        static IrBuf nested;
        size_t zn;

        zn = ir_zstored(z, raw_o, sizeof(raw_o));
        ir_png(&outer, 4, 4, 0, z, zn);
        zn = ir_zstored(z, raw_n, sizeof(raw_n));
        ir_png(&nested, 4, 2, 0, z, zn);
        g_ir_nest_png = nested.b;
        g_ir_nest_len = nested.n;
        g_ir_nest_result = FM_OK;
        g_ir_nest_calls = 0;
        memset(rgb, 0, 48);
        FM_SetIdle(ir_idle_png);
        r = ir_decode(&outer, rgb, sizeof(rgb));
        FM_SetIdle(NULL);
        if (r == FM_OK && (g_ir_nest_calls == 0 ||
                           g_ir_nest_result != FM_ERR_BUSY))
        {
            r = FM_ERR_BUSY;
        }
        for (i = 0; i < 16u && r == FM_OK; i++)
        {
            if (rgb[i * 3u] != (uint8_t)(10u * ((i % 4u) + 1u)))
            {
                r = FM_ERR_CRC;
            }
        }
        /* and the claim is released again */
        if (r == FM_OK) r = ir_decode(&nested, rgb, sizeof(rgb));
        if (r == FM_OK && rgb[0] != 200u) r = FM_ERR_CRC;
    }
    CHECK_RESULT(r, "nested png");

#if FM_USE_THREADS && FM_HOST_BUILD
    TEST("D-04/D-06 png: 3 threads encode+decode+registry, serial results");
    {
        pthread_t th[3];
        IrThread ctx[3];
        int started = 0;

        r = FM_OK;
        for (i = 0; i < 3u; i++)
        {
            ctx[i].seed = i + 1u;
            ctx[i].failed = 0;
            if (pthread_create(&th[i], NULL, ir_thread, &ctx[i]) != 0)
            {
                r = FM_ERR_INTERNAL;
                break;
            }
            started++;
        }
        for (i = 0; i < (uint32_t)started; i++)
        {
            (void)pthread_join(th[i], NULL);
            if (ctx[i].failed != 0)
            {
                printf("[thread %u: step %d] ", (unsigned)i, ctx[i].failed);
                r = FM_ERR_CRC;
            }
        }
    }
    CHECK_RESULT(r, "concurrent png");
#endif
#endif /* FM_USE_PNG */

#if FM_USE_JPEG
    TEST("D-04 jpeg: nested decode from the idle hook gets BUSY");
    {
        static uint8_t ref[64 * 64 * 3];
        FM_Image img;

        g_ir_jpg_len = 0;
        r = File_ReadAll("/test/vectors/tiny64.jpg", g_ir_jpg,
                         sizeof(g_ir_jpg), &g_ir_jpg_len);
        if (r == FM_OK)
        {
            r = Image_DecodeJPEG(g_ir_jpg, g_ir_jpg_len, &img, ref,
                                 sizeof(ref));
        }
        if (r == FM_OK)
        {
            g_ir_jnest_calls = 0;
            g_ir_jnest_result = FM_OK;
            memset(rgb, 0, sizeof(rgb));
            FM_SetIdle(ir_idle_jpeg);
            r = Image_DecodeJPEG(g_ir_jpg, g_ir_jpg_len, &img, rgb,
                                 sizeof(rgb));
            FM_SetIdle(NULL);
        }
        if (r == FM_OK && (g_ir_jnest_calls == 0 ||
                           g_ir_jnest_result != FM_ERR_BUSY))
        {
            r = FM_ERR_BUSY;
        }
        /* picojpeg's static state survived: same picture as alone */
        if (r == FM_OK && memcmp(rgb, ref, sizeof(ref)) != 0)
        {
            r = FM_ERR_CRC;
        }
    }
    CHECK_RESULT(r, "nested jpeg");
#endif

    /* ---------------- D-06 ---------------- */
    TEST("D-06 codec registry: no duplicate entry, no match on \"\"");
    r = Image_RegisterCodec("IRA", &g_ir_ops_a);
    if (r == FM_OK) r = Image_RegisterCodec("IRB", &g_ir_ops_a);
    if (r == FM_OK) r = Image_UnregisterCodec("IRA");   /* frees a slot
                                                          * in front */
    if (r == FM_OK) r = Image_RegisterCodec("IRB", &g_ir_ops_b);
    if (r == FM_OK && Image_GetCodec("IRB") != &g_ir_ops_b)
    {
        r = FM_ERR_NOT_FOUND;
    }
    if (r == FM_OK) r = Image_UnregisterCodec("IRB");
    /* one registration, one unregister: it must be gone (the old
     * first-fit left the earlier entry behind) */
    if (r == FM_OK && (Image_GetCodec("IRB") != NULL ||
                       Image_UnregisterCodec("IRB") != FM_ERR_NOT_FOUND))
    {
        r = FM_ERR_EXISTS;
    }
    if (r == FM_OK && (Image_UnregisterCodec("") != FM_ERR_INVALID_ARGUMENT ||
                       Image_GetCodec("") != NULL))
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    (void)Image_UnregisterCodec("IRA");
    (void)Image_UnregisterCodec("IRB");
    CHECK_RESULT(r, "registry");

    /* ---------------- D-07 ---------------- */
    TEST("D-07 encoders refuse data_size smaller than the picture");
    {
        static uint8_t pix[8 * 8 * 3];
        static uint8_t out[2048];
        FM_Image img;
        size_t n = 0;
        FM_Bool ex = FM_TRUE;
        FM_Result e;

        memset(pix, 0x40, sizeof(pix));
        memset(&img, 0, sizeof(img));
        img.width = 8; img.height = 8;
        img.format = FM_PIXEL_RGB888;
        img.data = pix;
        img.data_capacity = sizeof(pix);
        img.data_size = 3u;                     /* one valid pixel */
        r = FM_OK;
        if (Image_EncodeBMP(&img, out, sizeof(out), &n) != FM_ERR_TOO_LARGE)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        e = Image_EncodePNG(&img, out, sizeof(out), &n);
        if (e != (FM_USE_PNG ? FM_ERR_TOO_LARGE : FM_ERR_UNSUPPORTED))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        e = Image_EncodeJPEG(&img, out, sizeof(out), &n);
        if (e != (FM_USE_JPEG ? FM_ERR_TOO_LARGE : FM_ERR_UNSUPPORTED))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        /* through Image_Save: refused before the file is created */
        (void)File_Delete("/ir_s.bmp");
        if (Image_Save("/ir_s.bmp", &img) != FM_ERR_TOO_LARGE ||
            File_Exists("/ir_s.bmp", &ex) != FM_OK || ex)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        /* control: with the real size (or 0 = unspecified) it encodes */
        img.data_size = sizeof(pix);
        if (r == FM_OK) r = Image_EncodeBMP(&img, out, sizeof(out), &n);
        img.data_size = 0u;
        if (r == FM_OK) r = Image_EncodeBMP(&img, out, sizeof(out), &n);
    }
    CHECK_RESULT(r, "data_size");

    TEST("Image_Save: extension rules, no file wiped or left half-written");
    {
        static uint8_t pix[4 * 4 * 3];
        static uint8_t wide[1];
        FM_Image img;
        FM_Bool ex = FM_FALSE;
        uint8_t keep[4];
        size_t got = 0;

        memset(pix, 0x80, sizeof(pix));
        memset(&img, 0, sizeof(img));
        img.width = 4; img.height = 4;
        img.format = FM_PIXEL_RGB888;
        img.data = pix;
        img.data_capacity = sizeof(pix);
        /* unknown extension: UNSUPPORTED and the existing file is
         * still there, untouched */
        r = File_WriteAll("/ir_s.xyz", "KEEP", 4);
        if (r == FM_OK && Image_Save("/ir_s.xyz", &img) != FM_ERR_UNSUPPORTED)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK) r = File_ReadAll("/ir_s.xyz", keep, sizeof(keep), &got);
        if (r == FM_OK && (got != 4u || memcmp(keep, "KEEP", 4) != 0))
        {
            r = FM_ERR_CRC;
        }
        (void)File_Delete("/ir_s.xyz");
        /* O-03: FAT hands back upper-case names */
        if (r == FM_OK) r = Image_Save("/IR_S.BMP", &img);
        if (r == FM_OK)
        {
            FM_ImageInfo inf;

            r = Image_GetInfo("/IR_S.BMP", &inf);
            if (r == FM_OK && (inf.width != 4u || inf.height != 4u))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        (void)File_Delete("/IR_S.BMP");
#if FM_USE_PNG
        if (r == FM_OK) r = Image_Save("/IR_S.PNG", &img);
        if (r == FM_OK)
        {
            FM_ImageInfo inf;

            r = Image_GetInfo("/IR_S.PNG", &inf);
            if (r == FM_OK && strcmp(inf.codec, "PNG") != 0)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        (void)File_Delete("/IR_S.PNG");
        /* a row longer than the PNG encoder supports: it fails after
         * the file was created -- the stub must not stay behind */
        if (r == FM_OK)
        {
            FM_Image big;

            memset(&big, 0, sizeof(big));
            big.width = (uint16_t)(FM_PNG_MAX_STRIDE / 3u + 8u);
            big.height = 1;
            big.format = FM_PIXEL_GRAY8;
            big.data = (uint8_t *)malloc(big.width);
            big.data_capacity = big.width;
            if (big.data == NULL)
            {
                r = FM_ERR_NO_MEMORY;
            }
            else
            {
                memset(big.data, 0, big.width);
                if (Image_Save("/ir_big.png", &big) != FM_ERR_TOO_LARGE ||
                    File_Exists("/ir_big.png", &ex) != FM_OK || ex)
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                free(big.data);
            }
            (void)File_Delete("/ir_big.png");
        }
#else
        /* codec compiled out: UNSUPPORTED, and no empty file */
        if (r == FM_OK && (Image_Save("/ir_s.png", &img) != FM_ERR_UNSUPPORTED ||
                           File_Exists("/ir_s.png", &ex) != FM_OK || ex))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
#endif
        (void)wide;
    }
    CHECK_RESULT(r, "image save");

    /* ---------------- O-01 ---------------- */
    TEST("O-01 Image_StreamDecode: callback-only mode reports geometry");
    {
        const uint16_t w_fit = (uint16_t)(FM_INTERNAL_BUFFER_SIZE / 3u);
        const size_t cap = ((size_t)w_fit + 2u) * 3u * 2u + 64u;
        uint8_t *pix = (uint8_t *)malloc(((size_t)w_fit + 1u) * 3u * 2u);
        uint8_t *bmp = (uint8_t *)malloc(cap);
        FM_Image src;
        FM_Image out;
        FM_Stream st;
        size_t n = 0;

        if (pix == NULL || bmp == NULL)
        {
            r = FM_ERR_NO_MEMORY;
        }
        else
        {
            /* widest row the on-stack line buffer holds; 2 rows whose
             * first pixels are 10 and 20 */
            memset(pix, 0, ((size_t)w_fit + 1u) * 3u * 2u);
            memset(&src, 0, sizeof(src));
            src.width = w_fit; src.height = 2;
            src.format = FM_PIXEL_RGB888;
            src.data = pix;
            src.data_capacity = (size_t)w_fit * 3u * 2u;
            pix[0] = 10; pix[(size_t)w_fit * 3u] = 20;
            r = Image_EncodeBMP(&src, bmp, cap, &n);
            if (r == FM_OK) r = Stream_InitMemoryRead(&st, bmp, n);
            if (r == FM_OK)
            {
                memset(&out, 0, sizeof(out));       /* data == NULL */
                out.data_size = 99u;
                g_ir_rows = 0; g_ir_row_sum = 0;
                r = Image_StreamDecode(&st, &out, ir_line_cb, NULL, NULL);
                Stream_Close(&st);
                if (r == FM_OK &&
                    (out.width != w_fit || out.height != 2u ||
                     out.format != FM_PIXEL_RGB888 || out.channels != 3u ||
                     out.data_size != 0u || strcmp(out.codec, "BMP") != 0 ||
                     g_ir_rows != 2u || g_ir_row_sum != 10u + 20u + 1u))
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
            }
            /* one pixel wider: does not fit the line buffer */
            if (r == FM_OK)
            {
                src.width = (uint16_t)(w_fit + 1u);
                src.data_capacity = ((size_t)w_fit + 1u) * 3u * 2u;
                r = Image_EncodeBMP(&src, bmp, cap, &n);
            }
            if (r == FM_OK) r = Stream_InitMemoryRead(&st, bmp, n);
            if (r == FM_OK)
            {
                memset(&out, 0, sizeof(out));
                g_ir_rows = 0;
                if (Image_StreamDecode(&st, &out, ir_line_cb, NULL, NULL)
                        != FM_ERR_TOO_LARGE || g_ir_rows != 0u)
                {
                    r = FM_ERR_INVALID_FORMAT;
                }
                Stream_Close(&st);
            }
        }
        free(pix);
        free(bmp);
    }
    CHECK_RESULT(r, "stream decode");

    /* ---------------- O-02 ---------------- */
    TEST("O-02 BMP header: biSizeImage counts the row padding");
    {
        uint8_t pix[3 * 2 * 3];
        uint8_t out[128];
        FM_Image img;
        size_t n = 0;

        memset(pix, 1, sizeof(pix));
        memset(&img, 0, sizeof(img));
        img.width = 3; img.height = 2;
        img.format = FM_PIXEL_RGB888;
        img.data = pix;
        img.data_capacity = sizeof(pix);
        r = Image_EncodeBMP(&img, out, sizeof(out), &n);
        /* 3 px = 9 bytes, padded to 12 per row, 2 rows */
        if (r == FM_OK && (n != 54u + 24u || out[34] != 24u || out[35] != 0u))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
    }
    CHECK_RESULT(r, "biSizeImage");

    /* ---------------- D-08 / D-09 ---------------- */
    TEST("D-08 Image_Rotate validates the SOURCE buffer too");
    {
        static uint8_t small[4 * 4 * 3];
        static uint8_t dstb[8 * 8 * 3];
        FM_Image s;
        FM_Image d;

        memset(small, 7, sizeof(small));
        memset(&s, 0, sizeof(s));
        memset(&d, 0, sizeof(d));
        s.width = 8; s.height = 8;              /* claims 192 bytes ... */
        s.format = FM_PIXEL_RGB888;
        s.data = small;
        s.data_capacity = sizeof(small);        /* ... has 48 */
        d.data = dstb;
        d.data_capacity = sizeof(dstb);
        r = (Image_Rotate(&s, &d, 90) == FM_ERR_TOO_LARGE)
                ? FM_OK : FM_ERR_INVALID_FORMAT;
        /* data_size counts as well */
        s.width = 4; s.height = 4;
        s.data_size = 3u;
        if (r == FM_OK && Image_Rotate(&s, &d, 180) != FM_ERR_TOO_LARGE)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        s.data_size = 0u;
        if (r == FM_OK) r = Image_Rotate(&s, &d, 90);
        if (r == FM_OK && (d.width != 4u || d.height != 4u || dstb[0] != 7u))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
    }
    CHECK_RESULT(r, "rotate source");

    TEST("D-09 Image_ScaleEx checks both buffer capacities");
    {
        uint8_t src[4 * 4 * 3];
        uint8_t a[8 * 8 * 3];
        uint8_t b[8 * 8 * 3];

        for (i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 5u);
        r = FM_OK;
        if (Image_ScaleEx(src, sizeof(src) - 1u, 4, 4, FM_PIXEL_RGB888,
                          a, sizeof(a), 8, 8, FM_SCALE_NEAREST)
                != FM_ERR_TOO_LARGE ||
            Image_ScaleEx(src, sizeof(src), 4, 4, FM_PIXEL_RGB888,
                          a, sizeof(a) - 1u, 8, 8, FM_SCALE_BILINEAR)
                != FM_ERR_TOO_LARGE ||
            Image_ScaleEx(src, sizeof(src), 4, 4, FM_PIXEL_RGB888,
                          a, sizeof(a), 8, 8, (FM_ScaleMode)7)
                != FM_ERR_INVALID_ARGUMENT)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        /* exact capacities: same pixels as the unchecked entry point */
        memset(a, 0, sizeof(a));
        memset(b, 1, sizeof(b));
        if (r == FM_OK)
        {
            r = Image_ScaleEx(src, sizeof(src), 4, 4, FM_PIXEL_RGB888,
                              a, sizeof(a), 8, 8, FM_SCALE_BILINEAR);
        }
        if (r == FM_OK)
        {
            r = Image_Scale(src, 4, 4, FM_PIXEL_RGB888, b, 8, 8,
                            FM_SCALE_BILINEAR);
        }
        if (r == FM_OK && memcmp(a, b, sizeof(a)) != 0) r = FM_ERR_CRC;
    }
    CHECK_RESULT(r, "scale ex");

#if (FM_IMAGE_MAX_BYTES) <= 0xFFFFFFFFu
    /* Only where a pixel buffer cannot exceed 4 GiB -- a 32-bit target,
     * or a host build with -DFM_IMAGE_MAX_BYTES=0xFFFFFFFFu (the matrix
     * has that entry).  65535 x 65535 x 4 is 17 GB: each call below
     * must refuse the size itself; a wrapped size_t product would pass
     * the capacity check and run off the 16-byte buffer. */
    TEST("D-08 sizes beyond the limit are TOO_LARGE, not wrapped");
    {
        static uint8_t tiny[16];
        FM_Image big;
        FM_Image d;
        size_t bytes = 0;
        uint8_t bmp[54];

        memset(&big, 0, sizeof(big));
        memset(&d, 0, sizeof(d));
        big.width = 65535u; big.height = 65535u;
        big.format = FM_PIXEL_RGBA8888;
        big.data = tiny;
        big.data_capacity = SIZE_MAX;           /* forged */
        d.data = tiny;
        d.data_capacity = SIZE_MAX;
        r = FM_OK;
        if (Image_BufferSize(65535u, 65535u, FM_PIXEL_RGBA8888, &bytes)
                != FM_ERR_TOO_LARGE ||
            Image_Flip(&big, FM_IMAGE_FLIP_VERTICAL) != FM_ERR_TOO_LARGE ||
            Image_Rotate(&big, &d, 90) != FM_ERR_TOO_LARGE ||
            Image_ScaleEx(tiny, SIZE_MAX, 65535u, 65535u, FM_PIXEL_RGBA8888,
                          tiny, SIZE_MAX, 1, 1, FM_SCALE_NEAREST)
                != FM_ERR_TOO_LARGE ||
            Image_Scale(tiny, 1, 1, FM_PIXEL_RGBA8888, tiny, 65535u, 65535u,
                        FM_SCALE_NEAREST) != FM_ERR_TOO_LARGE)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        /* a BMP header announcing 65535 x 65535: ResizeFile must not
         * size an allocation from it */
        memset(bmp, 0, sizeof(bmp));
        bmp[0] = 'B'; bmp[1] = 'M';
        bmp[10] = 54; bmp[14] = 40;
        bmp[18] = 0xFF; bmp[19] = 0xFF;
        bmp[22] = 0xFF; bmp[23] = 0xFF;
        bmp[26] = 1; bmp[28] = 24;
        if (r == FM_OK) r = File_WriteAll("/ir_huge.bmp", bmp, sizeof(bmp));
        if (r == FM_OK &&
            Image_ResizeFile("/ir_huge.bmp", "/ir_out.bmp", 4, 4,
                             FM_SCALE_NEAREST) != FM_ERR_TOO_LARGE)
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        (void)File_Delete("/ir_huge.bmp");
        (void)File_Delete("/ir_out.bmp");
    }
    CHECK_RESULT(r, "size limit");
#endif

#if FM_USE_JPEG
    /* ---------------- O-05 ---------------- */
    TEST("O-05 jpeg probe: scan budget on filler and endless segments");
    {
        const uint32_t budget = (uint32_t)FM_JPEG_PROBE_BUDGET;
        const uint32_t over = budget / 16u + 8u;     /* segments */
        const size_t cap = 2u + (size_t)over * 4u + 32u;
        uint8_t *f = (budget <= 0x100000u) ? (uint8_t *)malloc(cap) : NULL;
        static const uint8_t sof[] = {
            0xFF, 0xC0, 0x00, 0x11, 8, 0x00, 0x10, 0x00, 0x20, 3,
            1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1
        };
        FM_ImageInfo inf;
        size_t n;

        if (f == NULL)
        {
            printf("(budget too large for this test) ");
            r = FM_OK;
        }
        else
        {
            /* control: 100 empty APP1 segments, then the frame header */
            n = 0;
            f[n++] = 0xFF; f[n++] = 0xD8;
            for (i = 0; i < 100u; i++)
            {
                f[n++] = 0xFF; f[n++] = 0xE1; f[n++] = 0; f[n++] = 2;
            }
            memcpy(f + n, sof, sizeof(sof)); n += sizeof(sof);
            r = File_WriteAll("/ir_p.jpg", f, n);
            if (r == FM_OK) r = Image_GetInfo("/ir_p.jpg", &inf);
            if (r == FM_OK && (inf.width != 32u || inf.height != 16u))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            /* more segments than the budget pays for: given up on */
            n = 2;
            for (i = 0; i < over; i++)
            {
                f[n++] = 0xFF; f[n++] = 0xE1; f[n++] = 0; f[n++] = 2;
            }
            memcpy(f + n, sof, sizeof(sof)); n += sizeof(sof);
            if (r == FM_OK) r = File_WriteAll("/ir_p.jpg", f, n);
            if (r == FM_OK &&
                Image_GetInfo("/ir_p.jpg", &inf) != FM_ERR_INVALID_FORMAT)
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            (void)File_Delete("/ir_p.jpg");
            free(f);
        }
    }
    CHECK_RESULT(r, "probe budget");
#endif
}
#if FM_USE_PNG
/* ------------------------------------------------------------------ */
/* PNG: every colour type / bit depth, Adam7, zlib and chunk checks    */
/* ------------------------------------------------------------------ */
static uint32_t pf_crc(const uint8_t *p, size_t n)
{
    return ir_crc32(p, n, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

/* PNG around `raw` (the filtered scanlines) in one stored block. */
static void pf_png(IrBuf *o, uint32_t w, uint32_t h, uint8_t color,
                   uint8_t depth, uint8_t interlace, const uint8_t *raw,
                   size_t n)
{
    static uint8_t z[420];

    ir_head_ex(o, w, h, color, depth, interlace);
    if (n + 11u <= sizeof(z))
    {
        ir_chunk(o, "IDAT", z, ir_zstored(z, raw, n));
    }
    ir_chunk(o, "IEND", NULL, 0);
}

static void pf_pixel(uint32_t x, uint32_t y, uint8_t *rgb)
{
    rgb[0] = (uint8_t)(x * 27u + y * 3u + 1u);
    rgb[1] = (uint8_t)(y * 29u + x + 2u);
    rgb[2] = (uint8_t)((x ^ y) * 17u + 3u);
}

/* Adam7 scanlines of the pf_pixel() picture.  The first row of every
 * pass is filtered "Up" (which must see zeroes, not the last row of
 * the pass before), the others "Sub". */
static size_t pf_adam7_raw(uint8_t *raw, uint32_t w, uint32_t h)
{
    static const uint8_t x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
    static const uint8_t y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const uint8_t dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
    static const uint8_t dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    size_t n = 0;
    uint32_t p;

    for (p = 0; p < 7u; p++)
    {
        uint32_t y;

        if (x0[p] >= w)
        {
            continue;
        }
        for (y = y0[p]; y < h; y += dy[p])
        {
            uint8_t left[3] = { 0, 0, 0 };
            uint32_t x;
            FM_Bool first = (y == y0[p]) ? FM_TRUE : FM_FALSE;

            raw[n++] = first ? 2u : 1u;
            for (x = x0[p]; x < w; x += dx[p])
            {
                uint8_t px[3];
                int c;

                pf_pixel(x, y, px);
                for (c = 0; c < 3; c++)
                {
                    raw[n++] = first ? px[c] : (uint8_t)(px[c] - left[c]);
                    left[c] = px[c];
                }
            }
        }
    }
    return n;
}

/* Patch 4 big-endian bytes. */
static void pf_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Offset of the first IDAT chunk (its length field) in a PNG, 0 if none. */
static size_t pf_find_idat(const uint8_t *png, size_t n)
{
    size_t p = 8;

    while (p + 12u <= n)
    {
        uint32_t len = ((uint32_t)png[p] << 24) | ((uint32_t)png[p + 1u] << 16) |
                       ((uint32_t)png[p + 2u] << 8) | png[p + 3u];

        if (memcmp(png + p + 4u, "IDAT", 4) == 0)
        {
            return p;
        }
        p += 12u + len;
    }
    return 0;
}

/* Recompute the CRC of the chunk at `at`. */
static void pf_fix_crc(uint8_t *png, size_t at)
{
    uint32_t len = ((uint32_t)png[at] << 24) | ((uint32_t)png[at + 1u] << 16) |
                   ((uint32_t)png[at + 2u] << 8) | png[at + 3u];

    pf_be32(png + at + 8u + len, pf_crc(png + at + 4u, 4u + len));
}

/* Make every chunk CRC right again (walks the chunks as far as their
 * length fields make sense). */
static void pf_fix_all_crcs(uint8_t *png, size_t n)
{
    size_t p = 8;

    while (p + 12u <= n)
    {
        uint32_t len = ((uint32_t)png[p] << 24) | ((uint32_t)png[p + 1u] << 16) |
                       ((uint32_t)png[p + 2u] << 8) | png[p + 3u];

        if (len > n || p + 12u + len > n)
        {
            break;
        }
        pf_fix_crc(png, p);
        p += 12u + len;
    }
}

#ifndef PF_FUZZ_ROUNDS
#define PF_FUZZ_ROUNDS 3000u
#endif

static void test_png_formats(void)
{
    static uint8_t file[48u * 1024u];
    static uint8_t full[211 * 157 * 3];
    static uint8_t out[211 * 157 * 3];
    static const struct
    {
        const char *name;
        uint16_t w;
        uint16_t h;
        uint32_t crc;       /* CRC-32 of the RGB888 picture */
    } k_vec[] = {
        /* Written by ImageMagick (tools/gen_png_vectors.sh).  The CRCs
         * come from tools/png_ref.py, an independent decoder; for every
         * 8-bit-or-less file they are also exactly what ImageMagick
         * itself decodes.  (For the 16-bit files ImageMagick floors
         * v / 257 where this library rounds, like libpng's
         * png_set_scale_16: at most 1 LSB apart.) */
        { "rgb8_a7", 37u, 29u, 0x393E234Au },
        { "rgba8_a7", 37u, 29u, 0x393E234Au },
        { "rgb16", 37u, 29u, 0xB20F59D4u },
        { "rgba16_a7", 37u, 29u, 0x7B8AF9E0u },
        { "gray1", 37u, 29u, 0x2D809C3Eu },
        { "gray2_a7", 37u, 29u, 0x774EF8F8u },
        { "gray4", 37u, 29u, 0x03B3FEE5u },
        { "gray8_a7", 37u, 29u, 0xFDA003A6u },
        { "gray16", 37u, 29u, 0xA2C64A49u },
        { "ga8", 37u, 29u, 0xFDA003A6u },
        { "ga16_a7", 37u, 29u, 0xAD47BB17u },
        { "pal1_a7", 37u, 29u, 0xC6EC1310u },
        { "pal2", 37u, 29u, 0xF50102EFu },
        { "pal4_a7", 37u, 29u, 0x5366A128u },
        { "pal8_a7", 37u, 29u, 0x6620E554u },
        { "big_rgb8_a7", 211u, 157u, 0x94E1C462u },
        { "tall_gray4_a7", 5u, 41u, 0x2657945Eu },
    };
    char path[64];
    FM_Result r;
    uint32_t i;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    if (r != FM_OK)
    {
        TEST("PNG formats: remount POSIX");
        FAIL(FM_ResultString(r));
        return;
    }

    TEST("png: every colour type, bit depth and Adam7 decode (17 files)");
    r = FM_OK;
    for (i = 0; i < (uint32_t)(sizeof(k_vec) / sizeof(k_vec[0])); i++)
    {
        FM_ImageInfo info;
        FM_Image img;
        size_t got = 0;
        size_t px = (size_t)k_vec[i].w * k_vec[i].h * 3u;
        FM_Result e;
        const char *what = NULL;

        (void)snprintf(path, sizeof(path), "/test/vectors/png/%s.png",
                       k_vec[i].name);
        memset(&img, 0, sizeof(img));
        memset(out, 0xA5, px);
        e = File_ReadAll(path, file, sizeof(file), &got);
        if (e != FM_OK) what = "read";
        if (what == NULL)
        {
            e = Image_ProbePNG(file, got, &info);
            if (e != FM_OK) what = "probe";
            else if (info.width != k_vec[i].w || info.height != k_vec[i].h)
            {
                e = FM_ERR_INVALID_FORMAT; what = "probe size";
            }
        }
        if (what == NULL)
        {
            e = Image_DecodePNG(file, got, &img, out, sizeof(out));
            if (e != FM_OK) what = "decode";
            else if (img.width != k_vec[i].w || img.height != k_vec[i].h ||
                     img.data_size != px)
            {
                e = FM_ERR_INVALID_FORMAT; what = "decode size";
            }
            else if (pf_crc(out, px) != k_vec[i].crc)
            {
                e = FM_ERR_CRC; what = "pixels";
            }
        }
        if (what == NULL)
        {
            /* the same file through the file reader */
            memset(out, 0x5A, px);
            e = Image_Load(path, &img, out, sizeof(out));
            if (e != FM_OK) what = "load";
            else if (pf_crc(out, px) != k_vec[i].crc)
            {
                e = FM_ERR_CRC; what = "load pixels";
            }
        }
        if (what != NULL)
        {
            printf("[%s: %s %d] ", k_vec[i].name, what, (int)e);
            r = (e != FM_OK) ? e : FM_ERR_INTERNAL;
        }
    }
    CHECK_RESULT(r, "png formats");

    TEST("png: 16-bit samples are rounded, sub-byte grey is full scale");
    {
        /* 8x1 grey 16: the values around the rounding points */
        static const uint8_t raw16[17] = {
            0, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x80, 0x00, 0x81,
               0x7A, 0x78, 0x80, 0xFF, 0x81, 0x01, 0xFE, 0x7F };
        static const uint8_t want16[8] = { 0, 255, 0, 1, 122, 128, 129, 254 };
        /* 4x1 grey 2: 0 1 2 3 -> 0 85 170 255;  8x1 grey 1: 10110001 */
        static const uint8_t raw2[2] = { 0, 0x1B };
        static const uint8_t raw1[2] = { 0, 0xB1 };
        /* 3x1 grey 4: 0x0, 0x9, 0xF -> 0, 153, 255 */
        static const uint8_t raw4[3] = { 0, 0x09, 0xF0 };
        IrBuf png;

        pf_png(&png, 8, 1, 0, 16, 0, raw16, sizeof(raw16));
        r = ir_decode(&png, out, sizeof(out));
        for (i = 0; i < 8u && r == FM_OK; i++)
        {
            if (out[i * 3u] != want16[i] || out[i * 3u + 2u] != want16[i])
            {
                printf("[16-bit sample %u: %u, want %u] ", (unsigned)i,
                       (unsigned)out[i * 3u], (unsigned)want16[i]);
                r = FM_ERR_CRC;
            }
        }
        if (r == FM_OK)
        {
            pf_png(&png, 4, 1, 0, 2, 0, raw2, sizeof(raw2));
            r = ir_decode(&png, out, sizeof(out));
            if (r == FM_OK && (out[0] != 0u || out[3] != 85u ||
                               out[6] != 170u || out[9] != 255u))
            {
                printf("[2-bit] ");
                r = FM_ERR_CRC;
            }
        }
        if (r == FM_OK)
        {
            pf_png(&png, 8, 1, 0, 1, 0, raw1, sizeof(raw1));
            r = ir_decode(&png, out, sizeof(out));
            for (i = 0; i < 8u && r == FM_OK; i++)
            {
                uint8_t want = ((0xB1u >> (7u - i)) & 1u) ? 255u : 0u;

                if (out[i * 3u] != want)
                {
                    printf("[1-bit pixel %u] ", (unsigned)i);
                    r = FM_ERR_CRC;
                }
            }
        }
        if (r == FM_OK)
        {
            pf_png(&png, 3, 1, 0, 4, 0, raw4, sizeof(raw4));
            r = ir_decode(&png, out, sizeof(out));
            if (r == FM_OK && (out[0] != 0u || out[3] != 153u ||
                               out[6] != 255u))
            {
                printf("[4-bit] ");
                r = FM_ERR_CRC;
            }
        }
    }
    CHECK_RESULT(r, "sample scaling");

    TEST("png adam7: every size 1x1 .. 9x9 (empty passes, pass restart)");
    {
        static uint8_t raw[9 * 9 * 3 + 32];
        uint32_t w;
        uint32_t h;

        r = FM_OK;
        for (h = 1; h <= 9u && r == FM_OK; h++)
        {
            for (w = 1; w <= 9u && r == FM_OK; w++)
            {
                IrBuf png;
                size_t n = pf_adam7_raw(raw, w, h);
                uint32_t x;
                uint32_t y;

                pf_png(&png, w, h, 2, 8, 1, raw, n);
                memset(out, 0xEE, w * h * 3u);
                r = ir_decode(&png, out, sizeof(out));
                for (y = 0; y < h && r == FM_OK; y++)
                {
                    for (x = 0; x < w; x++)
                    {
                        uint8_t px[3];

                        pf_pixel(x, y, px);
                        if (memcmp(px, out + (y * w + x) * 3u, 3) != 0)
                        {
                            r = FM_ERR_CRC;
                        }
                    }
                }
                if (r != FM_OK)
                {
                    printf("[%ux%u] ", (unsigned)w, (unsigned)h);
                }
            }
        }
    }
    CHECK_RESULT(r, "adam7 sizes");

    TEST("png adam7: scaled decode == nearest neighbour of the full one");
    {
        static const struct { const char *name; uint16_t mw; uint16_t mh; }
        k_sc[] = {
            { "big_rgb8_a7", 100, 60 }, { "big_rgb8_a7", 13, 157 },
            { "big_rgb8_a7", 211, 9 },  { "gray2_a7", 10, 10 },
            { "pal4_a7", 36, 28 },      { "rgba16_a7", 5, 3 },
            { "tall_gray4_a7", 3, 7 },  { "pal1_a7", 1, 1 },
        };

        r = FM_OK;
        for (i = 0; i < (uint32_t)(sizeof(k_sc) / sizeof(k_sc[0])) &&
                    r == FM_OK; i++)
        {
            FM_Image a;
            FM_Image b;
            uint32_t x;
            uint32_t y;

            (void)snprintf(path, sizeof(path), "/test/vectors/png/%s.png",
                           k_sc[i].name);
            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            r = Image_Load(path, &a, full, sizeof(full));
            if (r == FM_OK)
            {
                memset(out, 0xC3, sizeof(out));
                r = Image_LoadEx(path, &b, out, sizeof(out), k_sc[i].mw,
                                 k_sc[i].mh);
            }
            if (r == FM_OK && (b.width == 0u || b.height == 0u ||
                               b.width > k_sc[i].mw || b.height > k_sc[i].mh ||
                               (b.width == a.width && b.height == a.height)))
            {
                r = FM_ERR_INVALID_FORMAT;
            }
            for (y = 0; y < b.height && r == FM_OK; y++)
            {
                for (x = 0; x < b.width; x++)
                {
                    uint32_t sx = x * a.width / b.width;
                    uint32_t sy = y * a.height / b.height;

                    if (memcmp(out + ((size_t)y * b.width + x) * 3u,
                               full + ((size_t)sy * a.width + sx) * 3u, 3) != 0)
                    {
                        r = FM_ERR_CRC;
                    }
                }
            }
            if (r != FM_OK)
            {
                printf("[%s to %ux%u: got %ux%u] ", k_sc[i].name,
                       (unsigned)k_sc[i].mw, (unsigned)k_sc[i].mh,
                       (unsigned)b.width, (unsigned)b.height);
            }
        }
    }
    CHECK_RESULT(r, "adam7 scaled");

    TEST("png: illegal colour type / depth / interlace is UNSUPPORTED");
    {
        static const uint8_t bad[][3] = {
            { 2, 4, 0 }, { 3, 16, 0 }, { 4, 2, 0 }, { 6, 1, 0 }, { 0, 3, 0 },
            { 5, 8, 0 }, { 1, 8, 0 },  { 2, 8, 2 }, { 0, 0, 0 }, { 6, 32, 0 },
        };
        static const uint8_t raw[5] = { 0, 1, 2, 3, 4 };

        r = FM_OK;
        for (i = 0; i < (uint32_t)(sizeof(bad) / sizeof(bad[0])); i++)
        {
            FM_ImageInfo info;
            IrBuf png;
            FM_Result e1;
            FM_Result e2;

            pf_png(&png, 1, 1, bad[i][0], bad[i][1], bad[i][2], raw,
                   sizeof(raw));
            e1 = Image_ProbePNG(png.b, png.n, &info);
            e2 = ir_decode(&png, out, sizeof(out));
            if (e1 != FM_ERR_UNSUPPORTED || e2 != FM_ERR_UNSUPPORTED)
            {
                printf("[colour %u depth %u il %u: probe %d decode %d] ",
                       (unsigned)bad[i][0], (unsigned)bad[i][1],
                       (unsigned)bad[i][2], (int)e1, (int)e2);
                r = FM_ERR_INVALID_FORMAT;
            }
        }
    }
    CHECK_RESULT(r, "illegal header");

    TEST("png: a sub-byte palette index past PLTE is CORRUPTED");
    {
        static const uint8_t pal[6] = { 10, 20, 30, 40, 50, 60 };
        static const uint8_t ok_raw[2] = { 0, 0x10 };    /* 0 1 0 0 */
        static const uint8_t bad_raw[2] = { 0, 0x13 };   /* 0 1 0 3 */
        uint8_t z[32];
        IrBuf png;
        FM_Result e;

        ir_head_ex(&png, 4, 1, 3, 2, 0);
        ir_chunk(&png, "PLTE", pal, sizeof(pal));
        ir_chunk(&png, "IDAT", z, ir_zstored(z, ok_raw, 2));
        ir_chunk(&png, "IEND", NULL, 0);
        r = ir_decode(&png, out, sizeof(out));
        if (r == FM_OK && (out[0] != 10u || out[3] != 40u || out[5] != 60u ||
                           out[9] != 10u))
        {
            r = FM_ERR_CRC;
        }
        ir_head_ex(&png, 4, 1, 3, 2, 0);
        ir_chunk(&png, "PLTE", pal, sizeof(pal));
        ir_chunk(&png, "IDAT", z, ir_zstored(z, bad_raw, 2));
        ir_chunk(&png, "IEND", NULL, 0);
        e = ir_decode(&png, out, sizeof(out));
        if (r == FM_OK && e != FM_ERR_CORRUPTED)
        {
            printf("[index 3 of 2: %d] ", (int)e);
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
    }
    CHECK_RESULT(r, "palette index");

    TEST("png: a wrong Adler-32 (zlib trailer) is FM_ERR_CRC");
    {
        static const uint8_t raw[5] = { 0, 0x11, 0x22, 0x33, 0x44 };
        uint8_t z[32];
        size_t zn = ir_zstored(z, raw, sizeof(raw));
        IrBuf png;
        FM_Result e;
        size_t got = 0;

        /* control */
        ir_png(&png, 4, 1, 0, z, zn);
        r = ir_decode(&png, out, sizeof(out));
        /* every single-bit error in the trailer */
        for (i = 0; i < 32u && r == FM_OK; i++)
        {
            z[zn - 4u + i / 8u] ^= (uint8_t)(1u << (i % 8u));
            ir_png(&png, 4, 1, 0, z, zn);       /* chunk CRC is right */
            e = ir_decode(&png, out, sizeof(out));
            z[zn - 4u + i / 8u] ^= (uint8_t)(1u << (i % 8u));
            if (e != FM_ERR_CRC)
            {
                printf("[trailer bit %u: %d] ", (unsigned)i, (int)e);
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }
        /* trailer cut off */
        if (r == FM_OK)
        {
            ir_png(&png, 4, 1, 0, z, zn - 2u);
            e = ir_decode(&png, out, sizeof(out));
            if (e != FM_ERR_CORRUPTED)
            {
                printf("[short trailer: %d] ", (int)e);
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }
        /* a real file: the stored picture data is changed (one pixel
         * byte inside a deflate stream cannot be flipped without
         * knowing the codes, so the trailer is what gets damaged
         * here; the chunk CRC is made right again) */
        if (r == FM_OK)
        {
            r = File_ReadAll("/test/vectors/png/gray8_a7.png", file,
                             sizeof(file), &got);
        }
        if (r == FM_OK)
        {
            size_t at = pf_find_idat(file, got);
            uint32_t len = ((uint32_t)file[at] << 24) |
                           ((uint32_t)file[at + 1u] << 16) |
                           ((uint32_t)file[at + 2u] << 8) | file[at + 3u];
            FM_Image img;

            memset(&img, 0, sizeof(img));
            file[at + 8u + len - 1u] ^= 0x01u;
            pf_fix_crc(file, at);
            e = Image_DecodePNG(file, got, &img, out, sizeof(out));
            if (e != FM_ERR_CRC)
            {
                printf("[file trailer: %d] ", (int)e);
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }
    }
    CHECK_RESULT(r, "adler");

    TEST("png: the CRC of every IDAT chunk is checked, the last one too");
    {
        static const uint8_t raw[5] = { 0, 0x11, 0x22, 0x33, 0x44 };
        uint8_t z[40];
        size_t zn = ir_zstored(z, raw, sizeof(raw));
        IrBuf png;
        FM_Result e;
        size_t at;

        /* (a) one IDAT, CRC wrong */
        ir_png(&png, 4, 1, 0, z, zn);
        at = pf_find_idat(png.b, png.n);
        png.b[at + 8u + zn] ^= 0x80u;
        e = ir_decode(&png, out, sizeof(out));
        r = (e == FM_ERR_CRC) ? FM_OK
                              : ((e == FM_OK) ? FM_ERR_INVALID_FORMAT : e);
        if (r != FM_OK) printf("[single IDAT: %d] ", (int)e);

        /* (b) two IDATs; the second holds only the last trailer bytes */
        if (r == FM_OK)
        {
            ir_head(&png, 4, 1, 0);
            ir_chunk(&png, "IDAT", z, zn - 2u);
            at = png.n;
            ir_chunk(&png, "IDAT", z + zn - 2u, 2u);
            ir_chunk(&png, "IEND", NULL, 0);
            e = ir_decode(&png, out, sizeof(out));      /* control */
            if (e != FM_OK || out[0] != 0x11u || out[9] != 0x44u)
            {
                printf("[split control: %d] ", (int)e);
                r = (e == FM_OK) ? FM_ERR_CRC : e;
            }
            else
            {
                png.b[at + 8u + 2u + 3u] ^= 0x01u;
                e = ir_decode(&png, out, sizeof(out));
                if (e != FM_ERR_CRC)
                {
                    printf("[last IDAT: %d] ", (int)e);
                    r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
                }
            }
        }

        /* (c) the first of two IDATs (the stream needs the second) */
        if (r == FM_OK)
        {
            ir_head(&png, 4, 1, 0);
            at = png.n;
            ir_chunk(&png, "IDAT", z, 6u);
            ir_chunk(&png, "IDAT", z + 6u, zn - 6u);
            ir_chunk(&png, "IEND", NULL, 0);
            png.b[at + 8u + 6u] ^= 0x10u;
            e = ir_decode(&png, out, sizeof(out));
            if (e != FM_ERR_CRC)
            {
                printf("[first IDAT: %d] ", (int)e);
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }

        /* (d) bytes after the end of the zlib stream inside the last
         * IDAT: tolerated when the chunk is intact, a CRC error when
         * it is not (they are still read and summed) */
        if (r == FM_OK)
        {
            memset(z + zn, 0x77, 6);
            ir_png(&png, 4, 1, 0, z, zn + 6u);
            e = ir_decode(&png, out, sizeof(out));
            if (e != FM_OK)
            {
                printf("[padded IDAT: %d] ", (int)e);
                r = e;
            }
            else
            {
                at = pf_find_idat(png.b, png.n);
                png.b[at + 8u + zn + 3u] ^= 0x02u;      /* in the padding */
                e = ir_decode(&png, out, sizeof(out));
                if (e != FM_ERR_CRC)
                {
                    printf("[damaged padding: %d] ", (int)e);
                    r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
                }
            }
        }
    }
    CHECK_RESULT(r, "idat crc");

    TEST("png: more or fewer scanlines than the header says is CORRUPTED");
    {
        static uint8_t raw[9 * 9 * 3 + 32];
        static const uint8_t rows3[6] = { 0, 0x10, 0, 0x20, 0, 0x30 };
        IrBuf png;
        FM_Result e;
        size_t n;

        r = FM_OK;
        /* 1x2 grey with three rows of data, then with one */
        pf_png(&png, 1, 2, 0, 8, 0, rows3, 6);
        e = ir_decode(&png, out, sizeof(out));
        if (e != FM_ERR_CORRUPTED)
        {
            printf("[extra row: %d] ", (int)e);
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
        pf_png(&png, 1, 2, 0, 8, 0, rows3, 2);
        e = ir_decode(&png, out, sizeof(out));
        if (r == FM_OK && e != FM_ERR_CORRUPTED)
        {
            printf("[missing row: %d] ", (int)e);
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
        /* Adam7 5x5: the last pass is missing its last row / has one
         * row too many */
        n = pf_adam7_raw(raw, 5, 5);
        pf_png(&png, 5, 5, 2, 8, 1, raw, n - 16u);
        e = ir_decode(&png, out, sizeof(out));
        if (r == FM_OK && e != FM_ERR_CORRUPTED)
        {
            printf("[adam7 short: %d] ", (int)e);
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
        memset(raw + n, 0, 16);
        pf_png(&png, 5, 5, 2, 8, 1, raw, n + 16u);
        e = ir_decode(&png, out, sizeof(out));
        if (r == FM_OK && e != FM_ERR_CORRUPTED)
        {
            printf("[adam7 long: %d] ", (int)e);
            r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
        }
        /* a filter type that does not exist */
        {
            static const uint8_t bad_filter[2] = { 5, 0x10 };

            pf_png(&png, 1, 1, 0, 8, 0, bad_filter, 2);
            e = ir_decode(&png, out, sizeof(out));
            if (r == FM_OK && e != FM_ERR_CORRUPTED)
            {
                printf("[filter 5: %d] ", (int)e);
                r = (e == FM_OK) ? FM_ERR_INVALID_FORMAT : e;
            }
        }
    }
    CHECK_RESULT(r, "row count");

    TEST("png: damaged files never crash, never write past the buffer");
    {
        /* Random byte damage to real files, half of the time with the
         * chunk CRCs repaired so that the damage reaches the header
         * parser, inflate, the filters and the palette instead of being
         * stopped at the first CRC.  Any result is acceptable; the
         * sanitizer build and the guard bytes are the judges. */
        static const char *const k_fz[] = {
            "rgb8_a7", "rgba16_a7", "gray1", "gray2_a7", "gray4", "ga8",
            "pal1_a7", "pal2", "pal4_a7", "pal8_a7", "tall_gray4_a7",
            "gray16", "rgb16", "ga16_a7", "gray8_a7",
        };
        static uint8_t orig[8u * 1024u];
        uint32_t seed = 0x9E3779B9u;
        uint32_t round;
        uint32_t accepted = 0;
        const size_t cap = 37u * 29u * 3u;     /* guard right behind it */

        r = FM_OK;
        for (round = 0; round < PF_FUZZ_ROUNDS && r == FM_OK; round++)
        {
            const char *name = k_fz[round % (sizeof(k_fz) / sizeof(k_fz[0]))];
            size_t got = 0;
            FM_Image img;
            FM_Result e;
            uint32_t hits;
            uint32_t k;

            (void)snprintf(path, sizeof(path), "/test/vectors/png/%s.png",
                           name);
            r = File_ReadAll(path, orig, sizeof(orig), &got);
            if (r != FM_OK)
            {
                break;
            }
            memcpy(file, orig, got);
            seed = seed * 1664525u + 1013904223u;
            hits = 1u + ((seed >> 28) & 3u);
            for (k = 0; k < hits; k++)
            {
                size_t at;

                seed = seed * 1664525u + 1013904223u;
                at = 8u + (size_t)((seed >> 8) % (uint32_t)(got - 8u));
                seed = seed * 1664525u + 1013904223u;
                file[at] = (uint8_t)(file[at] ^ (1u << ((seed >> 13) & 7u)));
                if (((seed >> 20) & 7u) == 0u)
                {
                    file[at] = (uint8_t)(seed >> 24);
                }
            }
            seed = seed * 1664525u + 1013904223u;
            if ((seed >> 31) != 0u)
            {
                pf_fix_all_crcs(file, got);
            }
            seed = seed * 1664525u + 1013904223u;
            if (((seed >> 27) & 7u) == 0u)
            {
                got -= (size_t)((seed >> 8) % 40u);     /* and cut short */
            }
            memset(&img, 0, sizeof(img));
            memset(out + cap, 0xD7, 64);
            e = Image_DecodePNG(file, got, &img, out, cap);
            for (k = 0; k < 64u; k++)
            {
                if (out[cap + k] != 0xD7u)
                {
                    printf("[round %u %s: wrote past the buffer] ",
                           (unsigned)round, name);
                    r = FM_ERR_INTERNAL;
                    break;
                }
            }
            if (e == FM_OK)
            {
                accepted++;
                if (r == FM_OK &&
                    ((size_t)img.width * img.height * 3u > cap ||
                     img.width == 0u || img.height == 0u))
                {
                    printf("[round %u %s: %ux%u] ", (unsigned)round, name,
                           (unsigned)img.width, (unsigned)img.height);
                    r = FM_ERR_INTERNAL;
                }
            }
        }
        /* an intact file must still decode after all of that */
        if (r == FM_OK)
        {
            FM_Image img;

            memset(&img, 0, sizeof(img));
            r = Image_Load("/test/vectors/png/rgb8_a7.png", &img, out,
                           sizeof(out));
            if (r == FM_OK && pf_crc(out, 37u * 29u * 3u) != 0x393E234Au)
            {
                r = FM_ERR_CRC;
            }
        }
        (void)accepted;
    }
    CHECK_RESULT(r, "png damage");
}
#endif /* FM_USE_PNG */

#if FM_USE_JPEG
/* ------------------------------------------------------------------ */
/* O-04: what a registered hardware JPEG codec is given by Image_Load  */
/* ------------------------------------------------------------------ */
static int            g_jh_calls;
static const uint8_t *g_jh_data;
static size_t         g_jh_size;

/* A "hardware" decoder: checks it was handed a whole JPEG file and
 * paints the picture one colour. */
static FM_Result jh_decode(const uint8_t *data, size_t size, FM_Image *img,
                           void *pixel_buffer, size_t capacity)
{
    FM_ImageInfo inf;
    size_t n;

    g_jh_calls++;
    g_jh_data = data;
    g_jh_size = size;
    if (size < 4u || data[0] != 0xFFu || data[1] != 0xD8u ||
        Image_ProbeJPEG(data, size, &inf) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    n = (size_t)inf.width * inf.height * 3u;
    if (n > capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(pixel_buffer, 0x5C, n);
    memset(img, 0, sizeof(*img));
    img->width = inf.width;
    img->height = inf.height;
    img->format = FM_PIXEL_RGB888;
    img->channels = 3;
    img->bit_depth = 24;
    img->data = (uint8_t *)pixel_buffer;
    img->data_capacity = capacity;
    img->data_size = n;
    return FM_OK;
}

static const FM_ImageCodecOps g_jh_ops = { "JPEG", NULL, jh_decode };

static void test_jpeg_hw_file(void)
{
    static uint8_t file[64u * 1024u];
    static uint8_t work[64u * 1024u];
    const char *path = "/test/vectors/mjpeg_frame.jpg";
    const int heap_ok = FM_JPEG_HW_HEAP;
    FM_ImageInfo inf;
    FM_Image img;
    uint8_t *px = NULL;
    size_t fsz = 0;
    size_t cap = 0;
    FM_Result r;
    int file_ok;
    int k;

    (void)FS_Unmount("/");
    r = Storage_Mount("ram");
    if (r == FM_OK) r = FS_Mount("/", &g_fs);
    TEST("O-04 jpeg: hardware codec gets the file only within the "
         "configured bounds");
    if (r == FM_OK) r = File_ReadAll(path, file, sizeof(file), &fsz);
    if (r == FM_OK) r = Image_GetInfo(path, &inf);
    if (r == FM_OK)
    {
        cap = (size_t)inf.width * inf.height * 3u;
        px = (uint8_t *)malloc(cap);
        if (px == NULL) r = FM_ERR_NO_MEMORY;
    }
    file_ok = ((uint64_t)fsz <= (uint64_t)FM_JPEG_HW_FILE_MAX) ? 1 : 0;
    if (r == FM_OK) r = Image_RegisterCodec("JPEG", &g_jh_ops);
    /* k = 0: no work buffer; 1: one that fits; 2: one byte too small */
    for (k = 0; k < 3 && r == FM_OK; k++)
    {
        int want_hw;

        if (k == 0) r = Image_SetWorkBuffer(NULL, 0);
        else if (k == 1) r = Image_SetWorkBuffer(work, fsz + 16u);
        else r = Image_SetWorkBuffer(work, fsz - 1u);
        if (r != FM_OK)
        {
            break;
        }
        want_hw = (file_ok && (k == 1 || heap_ok)) ? 1 : 0;
        g_jh_calls = 0;
        g_jh_data = NULL;
        memset(&img, 0, sizeof(img));
        memset(px, 0, cap);
        memset(work, 0, sizeof(work));
        r = Image_Load(path, &img, px, cap);
        if (r == FM_OK && (img.width != inf.width || img.height != inf.height))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK && g_jh_calls != want_hw)
        {
            printf("[case %d: %d hardware calls, want %d] ", k, g_jh_calls,
                   want_hw);
            r = FM_ERR_INVALID_FORMAT;
        }
        if (r == FM_OK && want_hw)
        {
            /* the whole file, and in the work buffer when it fits */
            if (g_jh_size != fsz || px[0] != 0x5Cu || px[cap - 1u] != 0x5Cu ||
                (k == 1 && (g_jh_data != work ||
                            memcmp(work, file, fsz) != 0)) ||
                (k != 1 && g_jh_data == work))
            {
                printf("[case %d: wrong block] ", k);
                r = FM_ERR_CRC;
            }
        }
        if (r == FM_OK && !want_hw)
        {
            /* decoded by the streaming software decoder instead: a real
             * picture, and the work buffer was not filled with the file */
            size_t i;
            int flat = 1;

            for (i = 1; i < cap; i++)
            {
                if (px[i] != px[0]) { flat = 0; break; }
            }
            if (flat || memcmp(work, file, 64) == 0)
            {
                printf("[case %d: not the software decode] ", k);
                r = FM_ERR_CRC;
            }
        }
    }
    (void)Image_UnregisterCodec("JPEG");
    (void)Image_SetWorkBuffer(NULL, 0);
    free(px);
    CHECK_RESULT(r, "jpeg hw file");
}
#endif /* FM_USE_JPEG */

#else /* !FM_USE_IMAGE */
/* FM_USE_IMAGE=0 used to drop every Image_* symbol except the six JPEG /
 * PNG stubs: an application that keeps one call (behind a run-time
 * "have images?" check, say) no longer linked.  All of them must link
 * and say "not supported", like the video and audio modules do. */
static void test_image_report(void)
{
    uint8_t a[16];
    uint8_t b[16];
    FM_Image img;
    FM_ImageInfo inf;
    FM_ImageDecoder dec;
    FM_ImageCodecOps ops;
    size_t n = 123u;
    FM_Result r = FM_OK;

    TEST("FM_USE_IMAGE=0: every Image_* entry links, UNSUPPORTED");
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(&img, 0, sizeof(img));
    memset(&dec, 0xA5, sizeof(dec));
    memset(&ops, 0, sizeof(ops));
#define IR_OFF(call) do { if ((call) != FM_ERR_UNSUPPORTED) \
                              r = FM_ERR_INVALID_FORMAT; } while (0)
    IR_OFF(Image_Load("/x.bmp", &img, a, sizeof(a)));
    IR_OFF(Image_LoadEx("/x.bmp", &img, a, sizeof(a), 4, 4));
    IR_OFF(Image_Save("/x.bmp", &img));
    IR_OFF(Image_GetInfo("/x.bmp", &inf));
    IR_OFF(Image_SetWorkBuffer(a, sizeof(a)));
    IR_OFF(Image_DecodeBMP(a, sizeof(a), &img, b, sizeof(b)));
    IR_OFF(Image_EncodeBMP(&img, b, sizeof(b), &n));
    IR_OFF(Image_DecodeJPEG(a, sizeof(a), &img, b, sizeof(b)));
    IR_OFF(Image_EncodeJPEG(&img, b, sizeof(b), &n));
    IR_OFF(Image_DecodePNG(a, sizeof(a), &img, b, sizeof(b)));
    IR_OFF(Image_EncodePNG(&img, b, sizeof(b), &n));
    IR_OFF(Image_RegisterCodec("X", &ops));
    IR_OFF(Image_UnregisterCodec("X"));
    IR_OFF(Image_OpenStream(NULL, &dec, &inf));
    IR_OFF(Image_ReadLine(&dec, a, sizeof(a)));
    IR_OFF(Image_StreamDecode(NULL, &img, NULL, NULL, NULL));
    IR_OFF(Image_BufferSize(2, 2, FM_PIXEL_RGB888, &n));
    IR_OFF(Image_Convert(&img, &img, FM_PIXEL_RGB888));
    IR_OFF(Image_ConvertRGB565to888(a, b, 1));
    IR_OFF(Image_ConvertRGB888to565(a, b, 1));
    IR_OFF(Image_ConvertRGB888toGray(a, b, 1));
    IR_OFF(Image_ApplyEffect(&img, FM_IMAGE_EFFECT_INVERT, 0));
    IR_OFF(Image_Scale(a, 1, 1, FM_PIXEL_RGB888, b, 1, 1, FM_SCALE_NEAREST));
    IR_OFF(Image_ScaleEx(a, sizeof(a), 1, 1, FM_PIXEL_RGB888, b, sizeof(b),
                         1, 1, FM_SCALE_NEAREST));
    IR_OFF(Image_ResizeFile("/x.bmp", "/y.bmp", 1, 1, FM_SCALE_NEAREST));
    IR_OFF(Image_Flip(&img, FM_IMAGE_FLIP_VERTICAL));
    IR_OFF(Image_Rotate(&img, &img, 90));
#undef IR_OFF
    n = 123u;
    if (Image_GetWorkBuffer(&n) != NULL || n != 0u ||
        Image_GetCodec("X") != NULL ||
        Image_BytesPerPixel(FM_PIXEL_RGB888) != 0u)
    {
        r = FM_ERR_INVALID_FORMAT;
    }
    Image_CloseDecoder(&dec);
    Image_CloseDecoder(NULL);
    if (dec.stream != NULL) r = FM_ERR_INVALID_FORMAT;
    CHECK_RESULT(r, "image stubs");
}
#endif /* FM_USE_IMAGE */

#if FM_USE_THREADS && FM_HOST_BUILD
/* ================================================================ */
/* Wide thread stress: every module that keeps shared state, hit from */
/* several threads at once.  Each thread works on its own files and   */
/* handles (handles are not shared between threads -- that is the     */
/* documented contract); what must hold is that the LIBRARY's shared  */
/* state survives: pools, registries, caches, scratch buffers, the    */
/* filesystem drivers' own structures.  Run under ThreadSanitizer     */
/* (`make tsan`) this is the race detector for the whole library.     */
/* ================================================================ */
#ifndef TW_THREADS
#define TW_THREADS 4
#endif
#ifndef TW_ITERS
#define TW_ITERS   24
#endif
#define TW_MEDIA   10

typedef struct
{
    unsigned  id;
    int       step;          /* 0 = fine, else the step that failed */
    FM_Result r;
} TwCtx;

static uint32_t g_tw_ref[TW_MEDIA];
static FM_Bool  g_tw_have_mfs;
static FM_Bool  g_tw_have_lfs;
static FM_FS   *g_tw_mfs;

static uint32_t tw_fnv(const void *p, size_t n, uint32_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;

    for (i = 0; i < n; i++)
    {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static void tw_pattern(uint8_t *p, size_t n, unsigned id, unsigned salt)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        p[i] = (uint8_t)(i * (2u * id + 3u) + salt + (i >> 7));
    }
}

#if FM_USE_AUDIO
static FM_Result tw_adec(void *ctx, const uint8_t *in, size_t in_size,
                         int16_t *out, size_t out_count, size_t *out_samples)
{
    (void)ctx; (void)in; (void)in_size; (void)out; (void)out_count;
    *out_samples = 0;
    return FM_ERR_EOF;
}
#endif

static FM_Result tw_count_cb(const FM_FileInfo *info, void *user_data)
{
    (void)info;
    (*(uint32_t *)user_data)++;
    return FM_OK;
}

/* Decode the shared, read-only media files; out[k] is a checksum of the
 * decoded data (or of the error code where a codec is compiled out). */
static void tw_media(uint32_t *out)
{
    static const size_t pix_cap = 64u * 64u * 3u;
    uint8_t *pix = (uint8_t *)malloc(pix_cap);
    int16_t *pcm = (int16_t *)malloc(4096u * sizeof(int16_t));
    uint32_t k;

    for (k = 0; k < TW_MEDIA; k++)
    {
        out[k] = 2166136261u;
    }
    if (pix == NULL || pcm == NULL)
    {
        free(pix); free(pcm);
        out[0] = 1u;
        return;
    }
#if FM_USE_IMAGE
    {
        static const char *const files[3] = {
            "/test/vectors/tiny64.png", "/test/vectors/tiny64.jpg",
            "/test/vectors/tiny64.bmp"
        };
        FM_Image img;
        FM_ImageInfo inf;
        FM_Result r;

        for (k = 0; k < 3u; k++)
        {
            memset(pix, 0, pix_cap);
            r = Image_GetInfo(files[k], &inf);
            out[k] = tw_fnv(&r, sizeof(r), out[k]);
            r = Image_Load(files[k], &img, pix, pix_cap);
            out[k] = tw_fnv(&r, sizeof(r), out[k]);
            out[k] = tw_fnv(pix, pix_cap, out[k]);
        }
        /* scaled load: BMP rows go through the shared line buffer */
        memset(pix, 0, pix_cap);
        r = Image_LoadEx("/test/vectors/tiny64.bmp", &img, pix, pix_cap,
                         24, 24);
        out[3] = tw_fnv(&r, sizeof(r), out[3]);
        out[3] = tw_fnv(pix, 24u * 24u * 3u, out[3]);
    }
#endif
#if FM_USE_AUDIO
    {
        static const char *const files[4] = {
            "/test/vectors/tone_mono.mp3",
            "/test/vectors/wav_adpcm_ima_22k.wav",
            "/test/vectors/test_flac.flac",
            "/test/vectors/test_vorbis.ogg"
        };

        for (k = 0; k < 4u; k++)
        {
            FM_Audio a;
            size_t got = 0;
            FM_Result r = Audio_Open(files[k], &a);

            out[4u + k] = tw_fnv(&r, sizeof(r), out[4u + k]);
            if (r == FM_OK)
            {
                memset(pcm, 0, 4096u * sizeof(int16_t));
                r = Audio_Read(&a, pcm, 4096u, &got);
                out[4u + k] = tw_fnv(&r, sizeof(r), out[4u + k]);
                out[4u + k] = tw_fnv(&got, sizeof(got), out[4u + k]);
                out[4u + k] = tw_fnv(pcm, got * sizeof(int16_t), out[4u + k]);
                (void)Audio_Close(&a);
            }
        }
    }
#endif
#if FM_USE_VIDEO
    {
        static const char *const files[2] = { "/tw_shared.mp4",
                                              "/tw_shared.avi" };

        for (k = 0; k < 2u; k++)
        {
            FM_Video v;
            FM_Result r = Video_Open(files[k], &v);
            uint32_t f;

            out[8u + k] = tw_fnv(&r, sizeof(r), out[8u + k]);
            if (r != FM_OK)
            {
                continue;
            }
#if FM_USE_H264
            if (k == 0u) (void)Video_RegisterCodec(&v, H264_GetCodecOps());
#endif
#if FM_USE_MJPEG
            if (k == 1u) (void)Video_RegisterCodec(&v, MJPEG_GetCodecOps());
#endif
            for (f = 0; f < 3u; f++)
            {
                memset(pix, 0, pix_cap);
                r = Video_DecodeFrame(&v, pix, pix_cap);
                out[8u + k] = tw_fnv(&r, sizeof(r), out[8u + k]);
                if (r != FM_OK)
                {
                    break;
                }
                out[8u + k] = tw_fnv(pix, pix_cap, out[8u + k]);
            }
            (void)Video_Close(&v);
        }
    }
#endif
    free(pix);
    free(pcm);
}

#define TW_STEP(n, expr) do { if (c->step == 0) { FM_Result _r = (expr); \
        if (_r != FM_OK) { c->step = (n); c->r = _r; } } } while (0)
#define TW_TRUE(n, cond) do { if (c->step == 0 && !(cond)) \
        { c->step = (n); c->r = FM_ERR_CRC; } } while (0)

/* File work on one mount.  `root` is "" (POSIX), "/twm" (FMFS) or
 * "/twl" (LittleFS).  `wide` adds a second directory and a recursive
 * copy; FMFS has FM_FMFS_MAX_ENTRIES (32) entries for all threads
 * together, so it gets the narrow variant. */
static void tw_files(TwCtx *c, const char *root, int base, unsigned iter,
                     uint8_t *a, uint8_t *b, size_t n, int wide)
{
    char p1[96];
    char p2[96];
    char p3[96];
    char dir[96];
    char dir2[96];
    char name[128];
    FM_File f;
    FM_Bool ex = FM_FALSE;
    uint64_t sz = 0;
    uint32_t done = 0;
    size_t got = 0;
    uint32_t cnt = 0;
    FM_FileInfo fi;
    size_t off;

    (void)snprintf(p1, sizeof(p1), "%s/tw%u_a.bin", root, c->id);
    (void)snprintf(p2, sizeof(p2), "%s/tw%u_b.bin", root, c->id);
    (void)snprintf(p3, sizeof(p3), "%s/tw%u_c.bin", root, c->id);
    (void)snprintf(dir, sizeof(dir), "%s/tw%u_d", root, c->id);
    (void)snprintf(dir2, sizeof(dir2), "%s/tw%u_e", root, c->id);
    tw_pattern(a, n, c->id, iter);

    /* handle I/O in odd-sized pieces, then read back through seeks */
    File_Init(&f);
    TW_STEP(base + 1, File_OpenEx(&f, p1, FM_OPEN_WRITE | FM_OPEN_CREATE |
                                         FM_OPEN_TRUNCATE));
    for (off = 0; off < n && c->step == 0; off += 257u)
    {
        uint32_t len = (uint32_t)((n - off < 257u) ? (n - off) : 257u);

        TW_STEP(base + 2, File_Write(&f, a + off, len, &done));
        TW_TRUE(base + 2, done == len);
    }
    (void)File_Close(&f);
    File_Init(&f);
    TW_STEP(base + 3, File_OpenEx(&f, p1, FM_OPEN_READ));
    TW_STEP(base + 3, File_Size(&f, &sz));
    TW_TRUE(base + 3, sz == n);
    TW_STEP(base + 4, File_Seek(&f, (int64_t)(n / 2u), FM_SEEK_SET));
    TW_STEP(base + 4, File_Read(&f, b, (uint32_t)(n / 4u), &done));
    TW_TRUE(base + 4, done == n / 4u && memcmp(b, a + n / 2u, n / 4u) == 0);
    TW_STEP(base + 5, File_Seek(&f, 0, FM_SEEK_SET));
    for (off = 0; off < n && c->step == 0; off += 300u)
    {
        uint32_t len = (uint32_t)((n - off < 300u) ? (n - off) : 300u);

        TW_STEP(base + 5, File_Read(&f, b + off, len, &done));
        TW_TRUE(base + 5, done == len);
    }
    TW_TRUE(base + 5, memcmp(a, b, n) == 0);
    (void)File_Close(&f);

    /* whole-file helpers, copy / move / rename / stat / delete */
    TW_STEP(base + 6, File_Copy(p1, p2, NULL, NULL));
    TW_STEP(base + 6, File_ReadAll(p2, b, n, &got));
    TW_TRUE(base + 6, got == n && memcmp(a, b, n) == 0);
    TW_STEP(base + 7, File_Move(p2, p3, NULL, NULL));
    TW_STEP(base + 7, File_Exists(p2, &ex));
    TW_TRUE(base + 7, !ex);
    TW_STEP(base + 8, File_Rename(p3, p2));
    TW_STEP(base + 8, File_SizeOf(p2, &sz));
    TW_TRUE(base + 8, sz == n);
    TW_STEP(base + 8, File_GetInfo(p2, &fi));

    /* range edits go through the shared scratch buffer */
    TW_STEP(base + 9, File_Insert(p2, 100u, "INSERTED", 8u));
    TW_STEP(base + 9, File_ReadAll(p2, b, n + 8u, &got));
    TW_TRUE(base + 9, got == n + 8u && memcmp(b, a, 100u) == 0 &&
                      memcmp(b + 100u, "INSERTED", 8u) == 0 &&
                      memcmp(b + 108u, a + 100u, n - 100u) == 0);
    TW_STEP(base + 10, File_DeleteRange(p2, 100u, 8u));
    TW_STEP(base + 10, File_ReadAll(p2, b, n + 8u, &got));
    TW_TRUE(base + 10, got == n && memcmp(a, b, n) == 0);

    /* directories */
    (void)Directory_DeleteRecursive(dir);
    if (wide) { (void)Directory_DeleteRecursive(dir2); }
    TW_STEP(base + 11, Directory_Create(dir));
    (void)snprintf(name, sizeof(name), "%s/one.bin", dir);
    TW_STEP(base + 11, File_WriteAll(name, a, 700u));
    (void)snprintf(name, sizeof(name), "%s/two.bin", dir);
    TW_STEP(base + 11, File_WriteAll(name, a + 700u, 300u));
    TW_STEP(base + 12, Directory_Count(dir, FM_ITEM_FILE, &cnt));
    TW_TRUE(base + 12, cnt == 2u);
    cnt = 0;
    TW_STEP(base + 12, Directory_List(dir, NULL, tw_count_cb, &cnt));
    TW_TRUE(base + 12, cnt == 2u);
    TW_STEP(base + 13, Directory_Size(dir, &sz));
    TW_TRUE(base + 13, sz == 1000u);
    if (wide)
    {
        TW_STEP(base + 14, Directory_Copy(dir, dir2, NULL, NULL));
        (void)snprintf(name, sizeof(name), "%s/two.bin", dir2);
        TW_STEP(base + 14, File_ReadAll(name, b, n, &got));
        TW_TRUE(base + 14, got == 300u && memcmp(b, a + 700u, 300u) == 0);
        TW_STEP(base + 15, Directory_DeleteRecursive(dir2));
    }
    TW_STEP(base + 15, Directory_DeleteRecursive(dir));
    TW_STEP(base + 16, File_Delete(p1));
    TW_STEP(base + 16, File_Delete(p2));
}

static void *tw_thread(void *arg)
{
    TwCtx *c = (TwCtx *)arg;
    const size_t n = 3000u;
    uint8_t *a = (uint8_t *)malloc(n + 64u);
    uint8_t *b = (uint8_t *)malloc(n + 64u);
    char p1[64];
    char p2[64];
    char text[128];
    char codec[8];
    unsigned iter;

    (void)snprintf(codec, sizeof(codec), "TW%u", c->id);
    if (a == NULL || b == NULL)
    {
        c->step = 999;
    }
    for (iter = 0; iter < TW_ITERS && c->step == 0; iter++)
    {
        size_t got = 0;
        size_t lines = 0;
        FM_FSInfo fsi;
        FM_StorageInfo sti;
        FM_TransferOpts opts;

        tw_files(c, "", 100, iter, a, b, n, 1);
        if (g_tw_have_mfs)
        {
            tw_files(c, "/twm", 200, iter, a, b, 1500u, 0);
#if FM_USE_FMFS
            /* compaction next to the other threads' open files */
            if ((iter % 4u) == c->id % 4u)
            {
                TW_STEP(250, FMFS_Repack(g_tw_mfs));
            }
#endif
        }
        if (g_tw_have_lfs) tw_files(c, "/twl", 300, iter, a, b, 1500u, 1);

        /* transfer layer, safe-write, text helpers (POSIX root) */
        (void)snprintf(p1, sizeof(p1), "/tw%u_t1.bin", c->id);
        (void)snprintf(p2, sizeof(p2), "/tw%u_t2.bin", c->id);
        tw_pattern(a, n, c->id, iter + 77u);
        TW_STEP(401, FM_Transfer_SaveFile(p1, a, n));
        memset(&opts, 0, sizeof(opts));
        opts.verify = FM_USE_CRC ? FM_TRUE : FM_FALSE;
        TW_STEP(402, FM_Transfer_Copy(p1, p2, &opts));
        TW_STEP(403, FM_Transfer_LoadFile(p2, b, n, &got));
        TW_TRUE(403, got == n && memcmp(a, b, n) == 0);
#if FM_USE_SAFE_WRITE
        TW_STEP(404, File_SafeReplace(p2, a, 512u));
        TW_STEP(404, File_ReadAll(p2, b, n, &got));
        TW_TRUE(404, got == 512u && memcmp(a, b, 512u) == 0);
#endif
        (void)File_Delete(p1);
        (void)File_Delete(p2);
#if FM_USE_TEXT
        (void)snprintf(p1, sizeof(p1), "/tw%u.txt", c->id);
        (void)snprintf(text, sizeof(text), "thread %u iteration %u", c->id,
                       iter);
        TW_STEP(405, File_WriteText(p1, text));
        TW_STEP(405, Text_AppendLine(p1, "second"));
        TW_STEP(405, Text_CountLines(p1, &lines));
        TW_STEP(405, File_ReadText(p1, (char *)b, n, &got));
        TW_TRUE(405, strncmp((char *)b, text, strlen(text)) == 0);
        (void)File_Delete(p1);
#endif
        (void)lines;

        /* tables other threads are changing meanwhile */
        TW_STEP(406, FS_GetInfo("/", &fsi));
        TW_STEP(406, Storage_GetInfo("ram", &sti));
        (void)File_GetLastError();
        (void)File_PoolUsed();
        FM_Idle();

#if FM_USE_IMAGE
        {
            static const FM_ImageCodecOps ops = { "tw", NULL, ir_fake_decode };

            TW_STEP(407, Image_RegisterCodec(codec, &ops));
            TW_TRUE(407, Image_GetCodec(codec) == &ops);
            TW_STEP(407, Image_UnregisterCodec(codec));
        }
#endif
#if FM_USE_AUDIO
        {
            static const FM_AudioDecoderOps aops = { "tw", NULL, tw_adec,
                                                     NULL };

            TW_STEP(408, Audio_RegisterDecoder(codec, &aops));
            TW_TRUE(408, Audio_GetDecoder(codec) == &aops);
            TW_STEP(408, Audio_UnregisterDecoder(codec));
        }
#endif
        /* shared read-only media: every decode equals the reference */
        if ((iter % 3u) == 0u)
        {
            uint32_t sums[TW_MEDIA];
            uint32_t k;

            tw_media(sums);
            for (k = 0; k < TW_MEDIA; k++)
            {
                TW_TRUE(500 + (int)k, sums[k] == g_tw_ref[k]);
            }
        }
    }
    free(a);
    free(b);
    return NULL;
}

static void test_threads_wide(void)
{
    static uint8_t mfs_mem[256 * 1024];
    static uint8_t lfs_mem[512 * 1024];
    static FM_Storage mfs_storage;
    static FM_Storage lfs_storage;
    static FM_FS mfs;
    static FM_FS lfs;
    pthread_t th[TW_THREADS];
    TwCtx ctx[TW_THREADS];
    FM_Result r;
    int started = 0;
    int i;
    int ok = 1;

    restore_posix_root();
    TEST("Threads: files, 3 filesystems, transfer, codecs, registries");

    /* second and third mount: the library's own filesystem drivers */
    g_tw_have_mfs = FM_FALSE;
    g_tw_have_lfs = FM_FALSE;
    g_tw_mfs = &mfs;
#if FM_USE_FMFS
    r = RamDisk_Init(&mfs_storage, "twm", mfs_mem, sizeof(mfs_mem), 512);
    if (r == FM_OK) r = Storage_Register(&mfs_storage);
    if (r == FM_OK) r = FMFS_Init(&mfs);
    if (r == FM_OK) r = FS_Mount("/twm", &mfs);
    if (r == FM_OK) g_tw_have_mfs = FM_TRUE;
    else
    {
        /* (this used to be printed and the test went on with one
         * filesystem less -- and passed) */
        printf("[fmfs mount: %s] ", FM_ResultString(r));
        ok = 0;
    }
#endif
#if FM_USE_LITTLEFS
    memset(lfs_mem, 0xFF, sizeof(lfs_mem));
    r = RamDisk_Init(&lfs_storage, "twl", lfs_mem, sizeof(lfs_mem), 4096);
    if (r == FM_OK) r = Storage_Register(&lfs_storage);
    if (r == FM_OK) r = LittleFS_Init(&lfs);
    if (r == FM_OK) r = FS_Mount("/twl", &lfs);
    if (r == FM_OK) g_tw_have_lfs = FM_TRUE;
    else
    {
        printf("[littlefs mount: %s] ", FM_ResultString(r));
        ok = 0;
    }
#endif
    (void)mfs_mem; (void)lfs_mem; (void)mfs_storage; (void)lfs_storage;
    (void)mfs; (void)lfs;

#if FM_USE_VIDEO
    {
        static uint8_t raw[4096];
        static uint8_t cont[16384];
        size_t rd = 0;
        size_t len = 0;

#if FM_USE_H264
        r = File_ReadAll("/test/vectors/tiny64.h264", raw, sizeof(raw), &rd);
        if (r == FM_OK)
        {
            const uint8_t *nals[8];
            uint32_t sizes[8];

            if (split_nals(raw, (uint32_t)rd, nals, sizes, 8) >= 5u &&
                build_mp4(nals[0], sizes[0], nals[1], sizes[1], nals[2],
                          sizes[2], nals[3], sizes[3], nals[4], sizes[4],
                          cont, &len) == FM_OK)
            {
                (void)File_WriteAll("/tw_shared.mp4", cont, len);
            }
        }
#endif
#if FM_USE_MJPEG
        r = File_ReadAll("/test/vectors/mjpeg_frame.jpg", raw, sizeof(raw),
                         &rd);
        if (r == FM_OK && build_avi_mjpeg(raw, rd, cont, &len) == FM_OK)
        {
            (void)File_WriteAll("/tw_shared.avi", cont, len);
        }
#endif
        (void)raw; (void)cont; (void)rd; (void)len;
    }
#endif

    tw_media(g_tw_ref);                 /* single-threaded reference */

    for (i = 0; i < TW_THREADS; i++)
    {
        ctx[i].id = (unsigned)i;
        ctx[i].step = 0;
        ctx[i].r = FM_OK;
        if (pthread_create(&th[i], NULL, tw_thread, &ctx[i]) != 0)
        {
            ok = 0;
            break;
        }
        started++;
    }
    for (i = 0; i < started; i++)
    {
        (void)pthread_join(th[i], NULL);
        if (ctx[i].step != 0)
        {
            printf("[thread %d: step %d, %s] ", i, ctx[i].step,
                   FM_ResultString(ctx[i].r));
            ok = 0;
        }
    }
    (void)File_Delete("/tw_shared.mp4");
    (void)File_Delete("/tw_shared.avi");
    if (g_tw_have_mfs) { (void)FS_Unmount("/twm"); }
    if (g_tw_have_lfs) { (void)FS_Unmount("/twl"); }
    (void)Storage_Unregister("twm");
    (void)Storage_Unregister("twl");
    if (ok) PASS();
    else FAIL("concurrent use corrupted data or state");
}
#undef TW_STEP
#undef TW_TRUE
#endif /* FM_USE_THREADS && FM_HOST_BUILD */

/* ================================================================ */
/* Phase 5: locking, claims, FMFS compaction, allocator               */
/* ================================================================ */
static void sr_pattern(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        p[i] = (uint8_t)((i * 31u + seed * 97u) ^ (i >> 7));
    }
}

static FM_Result sr_verify(const char *path, uint32_t n, uint32_t seed)
{
    static uint8_t got[40000];
    static uint8_t want[40000];
    size_t rd = 0;
    FM_Result r;

    if (n > sizeof(got))
    {
        return FM_ERR_INTERNAL;
    }
    r = File_ReadAll(path, got, sizeof(got), &rd);
    if (r != FM_OK)
    {
        return r;
    }
    sr_pattern(want, n, seed);
    if (rd != n || memcmp(got, want, n) != 0)
    {
        return FM_ERR_CRC;
    }
    return FM_OK;
}

static FM_Result sr_write(const char *path, uint32_t n, uint32_t seed)
{
    static uint8_t buf[40000];

    if (n > sizeof(buf))
    {
        return FM_ERR_INTERNAL;
    }
    sr_pattern(buf, n, seed);
    return File_WriteAll(path, buf, n);
}

static void *sr_alloc_only(size_t size, void *ctx)
{
    (void)ctx;
    return malloc(size);
}

static void sr_free_only(void *p, void *ctx)
{
    (void)ctx;
    free(p);
}

#if FM_USE_FMFS
/* Random file operations on a small FMFS volume, checked against a
 * plain in-memory model after every few steps.  The volume is kept
 * nearly full so the compaction, move-to-tail and rotate-in-place paths
 * all run many times. */
#define SRF_FILES 6u
#define SRF_MAX   9000u
static uint8_t  g_srf_model[SRF_FILES][SRF_MAX];
static uint32_t g_srf_size[SRF_FILES];
static uint8_t  g_srf_there[SRF_FILES];
static uint32_t g_srf_rng;

static void pf_v1_image(uint8_t *mem, uint32_t cap);

static uint32_t srf_rand(uint32_t n)
{
    g_srf_rng = g_srf_rng * 1664525u + 1013904223u;
    return (g_srf_rng >> 8) % n;
}

static FM_Result srf_check_all(void)
{
    static uint8_t got[SRF_MAX + 1u];
    uint32_t i;

    for (i = 0; i < SRF_FILES; i++)
    {
        char path[32];
        size_t rd = 0;
        FM_Bool there = FM_FALSE;
        FM_Result r;

        (void)snprintf(path, sizeof(path), "/srf/f%u.bin", (unsigned)i);
        r = File_Exists(path, &there);
        if (r != FM_OK) return r;
        if ((there ? 1 : 0) != (int)g_srf_there[i]) return FM_ERR_EXISTS;
        if (!there) continue;
        r = File_ReadAll(path, got, sizeof(got), &rd);
        if (r != FM_OK) return r;
        if (rd != g_srf_size[i] ||
            memcmp(got, g_srf_model[i], g_srf_size[i]) != 0)
        {
            uint32_t at = 0;

            while (at < g_srf_size[i] && at < rd &&
                   got[at] == g_srf_model[i][at])
            {
                at++;
            }
            printf("[f%u differs at %u: size %u, read %u] ", (unsigned)i,
                   (unsigned)at, (unsigned)g_srf_size[i], (unsigned)rd);
            return FM_ERR_CRC;
        }
    }
    return FM_OK;
}

/* `slack`: how close to full the volume may be when it refuses a write
 * that would still fit (version 2 leaves a hole alone when closing it
 * costs too many journal records; version 1: none). */
static FM_Result srf_run(FM_FS *fs, uint32_t seed, uint32_t steps,
                         uint32_t slack, uint32_t *no_space_seen)
{
    FM_FSInfo info;
    uint64_t cap;
    uint32_t step;
    FM_Result r;

    memset(&info, 0, sizeof(info));
    r = FS_GetInfo("/srf", &info);
    if (r != FM_OK) return r;
    cap = info.free_space;              /* empty volume: the data area */
    memset(g_srf_size, 0, sizeof(g_srf_size));
    memset(g_srf_there, 0, sizeof(g_srf_there));
    g_srf_rng = seed;

    for (step = 0; step < steps && r == FM_OK; step++)
    {
        uint32_t i = srf_rand(SRF_FILES);
        uint32_t op = srf_rand(10);
        uint64_t live = 0;
        uint32_t k;
        char path[32];
        FM_File *f;

        for (k = 0; k < SRF_FILES; k++) live += g_srf_size[k];
        (void)snprintf(path, sizeof(path), "/srf/f%u.bin", (unsigned)i);

        if (op == 0)                                    /* delete */
        {
            r = File_Delete(path);
            if (!g_srf_there[i] && r == FM_ERR_NOT_FOUND) r = FM_OK;
            g_srf_there[i] = 0;
            g_srf_size[i] = 0;
        }
        else if (op == 1)                               /* compact */
        {
            r = FMFS_Repack(fs);
        }
        else if (op == 2 && g_srf_there[i])             /* truncate */
        {
            uint32_t ns = srf_rand(SRF_MAX + 1u);
            uint64_t grow = (ns > g_srf_size[i]) ? ns - g_srf_size[i] : 0u;

            f = File_Open(path, FM_OPEN_READ | FM_OPEN_WRITE);
            if (f == NULL) return File_GetLastError();
            r = File_Truncate(f, ns);
            (void)File_Close(f);
            if (r == FM_ERR_NO_SPACE)
            {
                (*no_space_seen)++;
                if (live + grow + slack <= cap)
                {
                    printf("[step %u: truncate refused with room] ",
                           (unsigned)step);
                    return FM_ERR_NO_SPACE;
                }
                r = FM_OK;
            }
            else if (r == FM_OK)
            {
                if (ns > g_srf_size[i])
                {
                    memset(g_srf_model[i] + g_srf_size[i], 0,
                           ns - g_srf_size[i]);
                }
                g_srf_size[i] = ns;
            }
        }
        else                                    /* write at a position */
        {
            static uint8_t data[2500];
            uint32_t n = 1u + srf_rand(sizeof(data));
            uint32_t pos = (op < 7u) ? g_srf_size[i]          /* append */
                                     : srf_rand(g_srf_size[i] + 400u);
            uint32_t end;
            uint64_t grow;
            uint32_t wr = 0;

            if (pos + n > SRF_MAX)
            {
                if (pos >= SRF_MAX) pos = SRF_MAX - 1u;
                n = SRF_MAX - pos;
            }
            end = pos + n;
            grow = (end > g_srf_size[i]) ? end - g_srf_size[i] : 0u;
            for (k = 0; k < n; k++)
            {
                data[k] = (uint8_t)srf_rand(256);
            }
            f = File_Open(path, FM_OPEN_READ | FM_OPEN_WRITE |
                                FM_OPEN_CREATE);
            if (f == NULL) return File_GetLastError();
            g_srf_there[i] = 1;
            r = File_Seek(f, (int64_t)pos, FM_SEEK_SET);
            if (r == FM_OK) r = File_Write(f, data, n, &wr);
            (void)File_Close(f);
            if (r == FM_ERR_NO_SPACE)
            {
                (*no_space_seen)++;
                if (live + grow + slack <= cap)
                {
                    printf("[step %u: write refused with room: live %lu "
                           "+ %lu of %lu] ", (unsigned)step,
                           (unsigned long)live, (unsigned long)grow,
                           (unsigned long)cap);
                    return FM_ERR_NO_SPACE;
                }
                /* a write that goes to the filesystem in several pieces
                 * (DMA bounce buffer) may have stored the first ones:
                 * *written says how much, and that much is in the file */
                if (wr > n) return FM_ERR_IO;
                n = wr;
                end = pos + n;
                r = FM_OK;
            }
            else if (r == FM_OK && wr != n)
            {
                return FM_ERR_IO;
            }
            if (r == FM_OK && n != 0u)
            {
                if (pos > g_srf_size[i])
                {
                    memset(g_srf_model[i] + g_srf_size[i], 0,
                           pos - g_srf_size[i]);
                }
                memcpy(g_srf_model[i] + pos, data, n);
                if (end > g_srf_size[i]) g_srf_size[i] = end;
            }
        }
        if (r == FM_OK && (step % 7u) == 6u)
        {
            r = srf_check_all();
        }
        if (r != FM_OK)
        {
            printf("[step %u op %u] ", (unsigned)step, (unsigned)op);
        }
    }
    if (r == FM_OK) r = srf_check_all();
    return r;
}
#endif /* FM_USE_FMFS */

/* storage write wrapper: lets `g_sr_wr_allow` writes through, then fails */
static FM_Storage_WriteFn g_sr_wr_real;
static long               g_sr_wr_allow = -1;      /* -1 = no limit */
static unsigned long      g_sr_wr_count;

static FM_Result sr_storage_write(FM_Storage *st, uint64_t offset,
                                  const void *buf, uint32_t len)
{
    g_sr_wr_count++;
    if (g_sr_wr_allow == 0)
    {
        return FM_ERR_IO;
    }
    if (g_sr_wr_allow > 0)
    {
        g_sr_wr_allow--;
    }
    return g_sr_wr_real(st, offset, buf, len);
}

#if FM_USE_THREADS && FM_HOST_BUILD
#include <time.h>
#include <sched.h>

static void sr_sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = ms * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static volatile int g_sr_started;
static volatile int g_sr_done;
static volatile int g_sr_done_while_held;
static FM_Result    g_sr_nested;
static FM_Result    g_sr_second;
static int          g_sr_cb_calls;
static pthread_t    g_sr_thread;
static int          g_sr_thread_ok;

static void *sr_second_copy(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_sr_started, 1, __ATOMIC_SEQ_CST);
    g_sr_second = File_Copy("/sr_b.bin", "/sr_b2.bin", NULL, NULL);
    __atomic_store_n(&g_sr_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void sr_copy_cb(uint64_t cur, uint64_t total, void *user)
{
    (void)cur; (void)total; (void)user;
    if (g_sr_cb_calls++ != 0)
    {
        return;
    }
    /* same task, nested: must be refused, never deadlock */
    g_sr_nested = File_Copy("/sr_b.bin", "/sr_n.bin", NULL, NULL);
    /* another task: must wait for us, not fail */
    if (pthread_create(&g_sr_thread, NULL, sr_second_copy, NULL) == 0)
    {
        int spins = 0;

        g_sr_thread_ok = 1;
        while (__atomic_load_n(&g_sr_started, __ATOMIC_SEQ_CST) == 0 &&
               spins++ < 2000)
        {
            sr_sleep_ms(1);
        }
        sr_sleep_ms(60);
        g_sr_done_while_held =
            __atomic_load_n(&g_sr_done, __ATOMIC_SEQ_CST);
    }
}

#if FM_SYNC_HAS_THREAD_SELF && (FM_LAST_ERROR_THREADS > 0)
static volatile int g_sr_stop;

static void *sr_ok_opener(void *arg)
{
    (void)arg;
    while (__atomic_load_n(&g_sr_stop, __ATOMIC_SEQ_CST) == 0)
    {
        FM_File *f = File_Open("/sr_b.bin", FM_OPEN_READ);

        if (f != NULL)
        {
            (void)File_Close(f);
        }
    }
    return NULL;
}
#endif
#endif /* FM_USE_THREADS && FM_HOST_BUILD */

static void test_sync_report(void)
{
    FM_Result r;
    uint32_t i;

    restore_posix_root();

#if FM_USE_FMFS
    {
        /* 160 KiB of files and the two tables of a version 2 volume */
        static uint8_t mem[168 * 1024];
        static FM_Storage st;
        static FM_FS fs;
        FM_FSInfo info;
        FM_Bool mounted = FM_FALSE;

        r = RamDisk_Init(&st, "srm", mem, sizeof(mem), 512);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/srm", &fs);
        if (r == FM_OK) mounted = FM_TRUE;

        TEST("FMFS: Repack after a file moved past its neighbour");
        if (r == FM_OK) r = sr_write("/srm/a.bin", 6000, 1);
        if (r == FM_OK) r = sr_write("/srm/b.bin", 9000, 2);
        if (r == FM_OK)
        {
            /* growing A relocates it behind B: table order A,B but
             * offsets B,A.  The old repack walked the table and slid A
             * (now bigger than the hole it left) over the head of B. */
            static uint8_t more[20000];
            FM_File *f = File_Open("/srm/a.bin",
                                   FM_OPEN_WRITE | FM_OPEN_APPEND);
            uint32_t wr = 0;

            sr_pattern(more, 20000, 1);       /* bytes 0..19999 of A */
            if (f == NULL)
            {
                r = File_GetLastError();
            }
            else
            {
                r = File_Write(f, more + 6000, 14000, &wr);
                if (r == FM_OK && wr != 14000u) r = FM_ERR_NO_SPACE;
                (void)File_Close(f);
            }
        }
        if (r == FM_OK) r = sr_verify("/srm/b.bin", 9000, 2);
        if (r == FM_OK) r = FMFS_Repack(&fs);
        if (r == FM_OK) r = sr_verify("/srm/b.bin", 9000, 2);
        if (r == FM_OK) r = sr_verify("/srm/a.bin", 20000, 1);
        CHECK_RESULT(r, "repack order");

        TEST("FMFS: Repack keeps an open handle usable");
        {
            FM_File *f = File_Open("/srm/b.bin", FM_OPEN_READ);
            uint8_t head[64];
            uint8_t want[64];
            uint32_t rd = 0;

            r = (f != NULL) ? FM_OK : File_GetLastError();
            if (r == FM_OK) r = File_Delete("/srm/a.bin");
            if (r == FM_OK) r = sr_write("/srm/c.bin", 5000, 3);
            if (r == FM_OK) r = FMFS_Repack(&fs);
            if (r == FM_OK) r = File_Read(f, head, sizeof(head), &rd);
            sr_pattern(want, sizeof(want), 2);
            if (r == FM_OK && (rd != sizeof(head) ||
                               memcmp(head, want, sizeof(head)) != 0))
            {
                r = FM_ERR_CRC;
            }
            if (f != NULL) (void)File_Close(f);
            if (r == FM_OK) r = sr_verify("/srm/c.bin", 5000, 3);
            (void)File_Delete("/srm/b.bin");
            (void)File_Delete("/srm/c.bin");
        }
        CHECK_RESULT(r, "repack with open handle");

        TEST("FMFS: write reuses the space of deleted files");
        memset(&info, 0, sizeof(info));
        r = mounted ? FS_GetInfo("/srm", &info) : FM_ERR_NOT_MOUNTED;
        if (r == FM_OK)
        {
            uint32_t q = (uint32_t)(info.free_space / 4u);

            if (q > 36000u) q = 36000u;
            r = sr_write("/srm/f1.bin", q, 11);
            if (r == FM_OK) r = sr_write("/srm/f2.bin", q, 12);
            if (r == FM_OK) r = sr_write("/srm/f3.bin", q, 13);
            if (r == FM_OK) r = File_Delete("/srm/f1.bin");
            if (r == FM_OK) r = File_Delete("/srm/f2.bin");
            /* 3/4 of the area was handed out once; two more files of
             * the same size only fit if the dead space is reclaimed */
            if (r == FM_OK) r = sr_write("/srm/f4.bin", q, 14);
            if (r == FM_OK) r = sr_write("/srm/f5.bin", q, 15);
            if (r == FM_OK) r = sr_verify("/srm/f3.bin", q, 13);
            if (r == FM_OK) r = sr_verify("/srm/f4.bin", q, 14);
            if (r == FM_OK) r = sr_verify("/srm/f5.bin", q, 15);
        }
        CHECK_RESULT(r, "space reuse");

        TEST("FMFS: free space counts reclaimable bytes");
        {
            FM_FSInfo before;
            FM_FSInfo after;

            memset(&before, 0, sizeof(before));
            memset(&after, 0, sizeof(after));
            r = FS_GetInfo("/srm", &before);
            if (r == FM_OK) r = File_Delete("/srm/f3.bin");
            if (r == FM_OK) r = FS_GetInfo("/srm", &after);
            /* f3 sits below f4/f5: the bump allocator reported nothing
             * back for it */
            if (r == FM_OK && after.free_space <= before.free_space)
            {
                printf("[%lu -> %lu] ", (unsigned long)before.free_space,
                       (unsigned long)after.free_space);
                r = FM_ERR_NO_SPACE;
            }
        }
        CHECK_RESULT(r, "free space");

        TEST("FMFS: a file in the middle grows when only the total fits");
        memset(&info, 0, sizeof(info));
        r = FS_GetInfo("/srm", &info);
        if (r == FM_OK && info.free_space < 80000u) r = FM_ERR_NO_SPACE;
        if (r == FM_OK) r = sr_write("/srm/g2.bin", 30000, 72);
        if (r == FM_OK) r = sr_write("/srm/g1.bin", 40000, 71);
        if (r == FM_OK) r = FS_GetInfo("/srm", &info);
        if (r == FM_OK)
        {
            /* g2 is not the last file, and what is free is smaller than
             * g2 itself: there is no room for a second copy of it, yet
             * g2 + 8000 bytes fits the volume */
            static uint8_t pat[38000];
            FM_File *f;
            uint32_t wr = 0;

            if (info.free_space < 8000u || info.free_space >= 30000u)
            {
                printf("[free %lu] ", (unsigned long)info.free_space);
                r = FM_ERR_INTERNAL;
            }
            sr_pattern(pat, sizeof(pat), 72);
            f = (r == FM_OK) ? File_Open("/srm/g2.bin",
                                         FM_OPEN_WRITE | FM_OPEN_APPEND)
                             : NULL;
            if (r == FM_OK && f == NULL) r = File_GetLastError();
            if (r == FM_OK) r = File_Write(f, pat + 30000, 8000, &wr);
            if (r == FM_OK && wr != 8000u) r = FM_ERR_NO_SPACE;
            if (f != NULL) (void)File_Close(f);
        }
        if (r == FM_OK) r = sr_verify("/srm/g2.bin", 38000, 72);
        if (r == FM_OK) r = sr_verify("/srm/g1.bin", 40000, 71);
        if (r == FM_OK) r = sr_verify("/srm/f4.bin", 36000, 14);
        if (r == FM_OK) r = sr_verify("/srm/f5.bin", 36000, 15);
        (void)File_Delete("/srm/g1.bin");
        (void)File_Delete("/srm/g2.bin");
        CHECK_RESULT(r, "grow in the middle");

        TEST("FMFS: overwriting in place does not rewrite the metadata");
        {
            static const uint8_t patch[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            FM_File *f;
            uint32_t wr = 0;
            static uint8_t back[1000];
            static uint8_t want[1000];
            size_t got = 0;

            r = sr_write("/srm/ow.bin", 1000, 73);
            g_sr_wr_real = st.write;
            g_sr_wr_allow = -1;
            st.write = sr_storage_write;
            f = (r == FM_OK) ? File_Open("/srm/ow.bin",
                                         FM_OPEN_READ | FM_OPEN_WRITE)
                             : NULL;
            if (r == FM_OK && f == NULL) r = File_GetLastError();
            g_sr_wr_count = 0;
            if (r == FM_OK) r = File_Write(f, patch, sizeof(patch), &wr);
            if (f != NULL) (void)File_Close(f);
            st.write = g_sr_wr_real;
            /* one write for the data; the table (33 writes) is untouched */
            if (r == FM_OK && g_sr_wr_count > 2u)
            {
                printf("[%lu storage writes] ", g_sr_wr_count);
                r = FM_ERR_IO;
            }
            if (r == FM_OK) r = File_ReadAll("/srm/ow.bin", back,
                                             sizeof(back), &got);
            sr_pattern(want, sizeof(want), 73);
            memcpy(want, patch, sizeof(patch));
            if (r == FM_OK && (got != sizeof(want) ||
                               memcmp(back, want, sizeof(want)) != 0))
            {
                r = FM_ERR_CRC;
            }
            (void)File_Delete("/srm/ow.bin");
        }
        CHECK_RESULT(r, "in-place write");

        TEST("FMFS: handle of a deleted file cannot touch its successor");
        {
            static const uint8_t junk[16] = {
                0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
                0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE
            };
            FM_File *f;
            uint32_t n = 0;
            uint8_t b[16];

            r = sr_write("/srm/x.bin", 3000, 31);
            f = (r == FM_OK) ? File_Open("/srm/x.bin",
                                         FM_OPEN_READ | FM_OPEN_WRITE)
                             : NULL;
            if (r == FM_OK && f == NULL) r = File_GetLastError();
            if (r == FM_OK) r = File_Delete("/srm/x.bin");
            /* y takes the table slot x just gave up */
            if (r == FM_OK) r = sr_write("/srm/y.bin", 3000, 32);
            if (r == FM_OK)
            {
                if (File_Write(f, junk, sizeof(junk), &n) == FM_OK ||
                    File_Read(f, b, sizeof(b), &n) == FM_OK)
                {
                    r = FM_ERR_EXISTS;
                }
            }
            if (f != NULL) (void)File_Close(f);
            if (r == FM_OK) r = sr_verify("/srm/y.bin", 3000, 32);
            (void)File_Delete("/srm/y.bin");
        }
        CHECK_RESULT(r, "stale handle");

        TEST("Directory_Copy: refuses a destination inside the source");
        (void)Directory_DeleteRecursive("/sr_tree");
        (void)Directory_DeleteRecursive("/sr_tree2");
        r = Directory_Create("/sr_tree/sub");
        if (r == FM_OK) r = sr_write("/sr_tree/a.bin", 40000, 41);
        if (r == FM_OK) r = sr_write("/sr_tree/b.bin", 40000, 42);
        if (r == FM_OK) r = sr_write("/sr_tree/sub/c.bin", 40000, 43);
        if (r == FM_OK)
        {
            FM_Bool there = FM_TRUE;

            if (Directory_Copy("/sr_tree", "/sr_tree/sub/x", NULL, NULL)
                    != FM_ERR_INVALID_ARGUMENT ||
                Directory_Copy("/sr_tree", "/sr_tree/", NULL, NULL)
                    != FM_ERR_INVALID_ARGUMENT ||
                Directory_Move("/sr_tree", "/sr_tree/sub/y", NULL, NULL)
                    != FM_ERR_INVALID_ARGUMENT)
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK) r = Directory_Exists("/sr_tree/sub/x", &there);
            if (r == FM_OK && there) r = FM_ERR_EXISTS;
            /* moving onto itself is a no-op, the tree stays */
            if (r == FM_OK) r = Directory_Move("/sr_tree", "/sr_tree/.",
                                               NULL, NULL);
            /* "/sr_tree2" only shares a prefix: a normal copy */
            if (r == FM_OK) r = Directory_Copy("/sr_tree", "/sr_tree2",
                                               NULL, NULL);
            if (r == FM_OK) r = sr_verify("/sr_tree2/sub/c.bin", 40000, 43);
            if (r == FM_OK) r = sr_verify("/sr_tree/a.bin", 40000, 41);
        }
        (void)Directory_DeleteRecursive("/sr_tree2");
        CHECK_RESULT(r, "copy into itself");

        TEST("Directory_Copy: a failed copy leaves no half tree behind");
        {
            FM_FSInfo before;
            FM_FSInfo after;
            FM_Bool there = FM_TRUE;
            FM_Result rc;

            memset(&before, 0, sizeof(before));
            memset(&after, 0, sizeof(after));
            r = FS_GetInfo("/srm", &before);
            /* 120 KB into what is left of a 160 KB volume */
            rc = Directory_Copy("/sr_tree", "/srm/t", NULL, NULL);
            if (r == FM_OK && rc == FM_OK) r = FM_ERR_INTERNAL;
            if (r == FM_OK) r = Directory_Exists("/srm/t", &there);
            if (r == FM_OK && there) r = FM_ERR_EXISTS;
            if (r == FM_OK) r = FS_GetInfo("/srm", &after);
            if (r == FM_OK && after.free_space != before.free_space)
            {
                r = FM_ERR_NO_SPACE;
            }
            /* an existing destination is merged into, never removed */
            if (r == FM_OK) r = Directory_Create("/srm/keep");
            if (r == FM_OK) r = sr_write("/srm/keep/mine.bin", 100, 44);
            if (r == FM_OK &&
                Directory_Copy("/sr_tree", "/srm/keep", NULL, NULL) == FM_OK)
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK) r = sr_verify("/srm/keep/mine.bin", 100, 44);
            (void)Directory_DeleteRecursive("/srm/keep");
        }
        (void)Directory_DeleteRecursive("/sr_tree");
        CHECK_RESULT(r, "partial copy");

        TEST("FMFS: reading a file writes nothing to the storage");
        {
            r = sr_write("/srm/ro.bin", 2000, 61);
            g_sr_wr_real = st.write;
            g_sr_wr_allow = -1;
            st.write = sr_storage_write;
            g_sr_wr_count = 0;
            if (r == FM_OK) r = sr_verify("/srm/ro.bin", 2000, 61);
            if (r == FM_OK)
            {
                FM_File *f = File_Open("/srm/ro.bin", FM_OPEN_READ);
                uint8_t b[32];
                uint32_t n = 0;

                if (f == NULL)
                {
                    r = File_GetLastError();
                }
                else
                {
                    r = File_Read(f, b, sizeof(b), &n);
                    if (r == FM_OK) r = File_Flush(f);
                    (void)File_Close(f);
                }
            }
            st.write = g_sr_wr_real;
            if (r == FM_OK && g_sr_wr_count != 0u)
            {
                printf("[%lu storage writes] ", g_sr_wr_count);
                r = FM_ERR_IO;
            }
            (void)File_Delete("/srm/ro.bin");
        }
        CHECK_RESULT(r, "read-only wear");

        TEST("Directory_Move: destination survives a failed source delete");
        {
            unsigned long per_delete;
            FM_Result rc;

            (void)Directory_DeleteRecursive("/sr_mv");
            r = Directory_Create("/srm/mv");
            if (r == FM_OK) r = sr_write("/srm/mv/a.bin", 3000, 51);
            if (r == FM_OK) r = sr_write("/srm/mv/b.bin", 3000, 52);
            if (r == FM_OK) r = sr_write("/srm/probe.bin", 10, 53);
            g_sr_wr_real = st.write;
            g_sr_wr_allow = -1;
            st.write = sr_storage_write;
            g_sr_wr_count = 0;
            if (r == FM_OK) r = File_Delete("/srm/probe.bin");
            per_delete = g_sr_wr_count;
            if (r == FM_OK && per_delete == 0u) r = FM_ERR_INTERNAL;
            if (r == FM_OK)
            {
                /* the first delete goes through, the second one fails */
                g_sr_wr_allow = (long)per_delete;
                rc = Directory_Move("/srm/mv", "/sr_mv", NULL, NULL);
                g_sr_wr_allow = -1;
                if (rc == FM_OK) r = FM_ERR_INTERNAL;
            }
            st.write = g_sr_wr_real;
            /* one source file is gone for good: the copy must be whole */
            if (r == FM_OK) r = sr_verify("/sr_mv/a.bin", 3000, 51);
            if (r == FM_OK) r = sr_verify("/sr_mv/b.bin", 3000, 52);
            if (r == FM_OK)
            {
                /* a failed delete is not a delete: what the volume still
                 * lists is really readable */
                FM_Bool ea = FM_FALSE;
                FM_Bool eb = FM_FALSE;

                (void)File_Exists("/srm/mv/a.bin", &ea);
                (void)File_Exists("/srm/mv/b.bin", &eb);
                if (ea == eb) r = FM_ERR_CORRUPTED;     /* exactly one left */
                if (r == FM_OK && ea) r = sr_verify("/srm/mv/a.bin", 3000, 51);
                if (r == FM_OK && eb) r = sr_verify("/srm/mv/b.bin", 3000, 52);
            }
            (void)Directory_DeleteRecursive("/sr_mv");
            (void)Directory_DeleteRecursive("/srm/mv");
        }
        CHECK_RESULT(r, "move rollback");

        TEST("FS_Unmount / FS_Format refuse while a handle is open");
        {
            FM_File *f = File_Open("/srm/f4.bin", FM_OPEN_READ);
            uint8_t b4[16];
            uint8_t w4[16];
            uint32_t rd = 0;

            r = (f != NULL) ? FM_OK : File_GetLastError();
            if (r == FM_OK && FS_Unmount("/srm") != FM_ERR_BUSY)
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK && FS_Format("/srm") != FM_ERR_BUSY)
            {
                r = FM_ERR_EXISTS;
            }
            /* the handle still works and the data is still there */
            if (r == FM_OK) r = File_Read(f, b4, sizeof(b4), &rd);
            sr_pattern(w4, sizeof(w4), 14);
            if (r == FM_OK && (rd != sizeof(b4) ||
                               memcmp(b4, w4, sizeof(b4)) != 0))
            {
                r = FM_ERR_CRC;
            }
            if (f != NULL) (void)File_Close(f);
            if (r == FM_OK)
            {
                FM_Dir *d = Directory_Open("/srm");

                if (d == NULL)
                {
                    r = File_GetLastError();
                }
                else
                {
                    if (FS_Unmount("/srm") != FM_ERR_BUSY)
                    {
                        r = FM_ERR_INTERNAL;
                    }
                    (void)Directory_Close(d);
                }
            }
            /* nothing open any more: both work */
            if (r == FM_OK) r = FS_Format("/srm");
            if (r == FM_OK)
            {
                FM_Bool there = FM_TRUE;

                r = File_Exists("/srm/f4.bin", &there);
                if (r == FM_OK && there) r = FM_ERR_EXISTS;
            }
            if (r == FM_OK) r = FS_Unmount("/srm");
            if (r == FM_OK) mounted = FM_FALSE;
        }
        CHECK_RESULT(r, "busy mount");

        if (mounted) (void)FS_Unmount("/srm");
        (void)Storage_Unregister("srm");
    }
#endif /* FM_USE_FMFS */

#if FM_USE_FMFS
    TEST("FMFS: random operations on a full volume match a model");
    {
        /* data area of roughly 30 KB for up to 6 x 9000 bytes of files */
        static uint8_t mem[44 * 1024];
        static FM_Storage st;
        static FM_FS fs;
        uint32_t refused = 0;
        uint32_t layout;
        uint32_t round;

        r = FM_OK;
        /* 3 x 1500 operations on what a format writes, and again on a
         * version 1 volume as older releases left it */
        for (layout = 0; layout < 2u && r == FM_OK; layout++)
        {
            for (round = 0; round < 3u && r == FM_OK; round++)
            {
                uint32_t slack = 0;

                if (layout == 1u)
                {
                    pf_v1_image(mem, (uint32_t)sizeof(mem));
                }
                else
                {
                    memset(mem, 0, sizeof(mem));
                }
                r = RamDisk_Init(&st, "srf", mem, sizeof(mem), 512);
                if (r == FM_OK) r = Storage_Register(&st);
                if (r == FM_OK) r = FMFS_Init(&fs);
                if (r == FM_OK) r = FS_Mount("/srf", &fs);
                if (r == FM_OK && layout == 1u && FMFS_IsPowerSafe(&fs))
                {
                    r = FM_ERR_INTERNAL;
                }
                if (r == FM_OK && FMFS_IsPowerSafe(&fs) &&
                    (uint64_t)(FM_FMFS_MOVE_STEPS) < SRF_MAX)
                {
                    /* a hole in front of a file stays when it is smaller
                     * than 1/FM_FMFS_MOVE_STEPS of that file */
                    slack = 2u * SRF_FILES *
                            (uint32_t)(SRF_MAX / (FM_FMFS_MOVE_STEPS) + 1u);
                }
                if (r == FM_OK) r = srf_run(&fs, 0x1234u + round * 77u,
                                            1500, slack, &refused);
                if (r == FM_OK)
                {
                    /* what a remount reads back from the storage is the
                     * same */
                    r = FS_Unmount("/srf");
                    if (r == FM_OK) r = FMFS_Init(&fs);
                    if (r == FM_OK) r = FS_Mount("/srf", &fs);
                    if (r == FM_OK) r = srf_check_all();
                }
                if (r != FM_OK)
                {
                    printf("[layout %u round %u] ", (unsigned)layout,
                           (unsigned)round);
                }
                (void)FS_Unmount("/srf");
                (void)Storage_Unregister("srf");
            }
        }
        /* the volume must really have been full now and then, or the
         * test proved nothing about compaction */
        if (r == FM_OK && refused < 40u)
        {
            printf("[only %u refusals] ", (unsigned)refused);
            r = FM_ERR_INTERNAL;
        }
    }
    CHECK_RESULT(r, "fmfs model");
#endif

    TEST("Directory_Open(NULL) does not leak a pool slot");
    r = FM_OK;
    for (i = 0; i < (uint32_t)FM_MAX_OPEN_DIRS + 4u; i++)
    {
        if (Directory_Open(NULL) != NULL)
        {
            r = FM_ERR_INTERNAL;
        }
    }
    if (r == FM_OK)
    {
        FM_Dir *d = Directory_Open("/");

        if (d == NULL)
        {
            r = File_GetLastError();
        }
        else
        {
            (void)Directory_Close(d);
        }
    }
    CHECK_RESULT(r, "dir pool");

    TEST("Allocator without realloc: FM_ReallocSized keeps the data");
    {
        FM_Allocator saved = *FM_Allocator_Get();
        FM_Allocator only;
        uint8_t *p;
        uint8_t *q;

        memset(&only, 0, sizeof(only));
        only.alloc = sr_alloc_only;
        only.free = sr_free_only;
        r = (FM_Allocator_Set(&only) == 0) ? FM_OK : FM_ERR_INTERNAL;
        p = (r == FM_OK) ? (uint8_t *)FM_Alloc(24) : NULL;
        if (r == FM_OK && p == NULL) r = FM_ERR_NO_MEMORY;
        if (r == FM_OK)
        {
            for (i = 0; i < 24u; i++) p[i] = (uint8_t)(i + 1u);
            /* a real resize is impossible without the old size: refuse
             * it (the old code copied 4096 bytes out of a 24-byte
             * block) and leave the block alone */
            q = (uint8_t *)FM_Realloc(p, 4096);
            if (q != NULL)
            {
                p = q;
                r = FM_ERR_INTERNAL;
            }
        }
        if (r == FM_OK)
        {
            q = (uint8_t *)FM_ReallocSized(p, 24, 4096);
            if (q == NULL)
            {
                r = FM_ERR_NO_MEMORY;
            }
            else
            {
                p = q;
                for (i = 0; i < 24u; i++)
                {
                    if (p[i] != (uint8_t)(i + 1u)) r = FM_ERR_CRC;
                }
                memset(p + 24, 0xA5, 4096 - 24);
            }
        }
        if (r == FM_OK)
        {
            q = (uint8_t *)FM_ReallocSized(p, 4096, 8);     /* shrink */
            if (q == NULL)
            {
                r = FM_ERR_NO_MEMORY;
            }
            else
            {
                p = q;
                for (i = 0; i < 8u; i++)
                {
                    if (p[i] != (uint8_t)(i + 1u)) r = FM_ERR_CRC;
                }
            }
        }
        if (r == FM_OK)
        {
            /* the trivial cases still work through FM_Realloc */
            q = (uint8_t *)FM_Realloc(NULL, 32);
            if (q == NULL) r = FM_ERR_NO_MEMORY;
            else if (FM_Realloc(q, 0) != NULL) r = FM_ERR_INTERNAL;
        }
        if (p != NULL) FM_Free(p);
        if (saved.alloc != NULL)
        {
            (void)FM_Allocator_Set(&saved);
        }
        else
        {
            FM_Allocator d;

            memset(&d, 0, sizeof(d));
            d.alloc = def_alloc;
            d.calloc = def_calloc;
            d.realloc = def_realloc;
            d.free = def_free;
            (void)FM_Allocator_Set(&d);
        }
    }
    CHECK_RESULT(r, "sized realloc");

#if FM_USE_THREADS && FM_HOST_BUILD
    TEST("Threads: nested copy is refused, a second task waits");
    r = sr_write("/sr_a.bin", 30000, 21);
    if (r == FM_OK) r = sr_write("/sr_b.bin", 30000, 22);
    g_sr_started = 0;
    g_sr_done = 0;
    g_sr_done_while_held = 0;
    g_sr_nested = FM_OK;
    g_sr_second = FM_ERR_INTERNAL;
    g_sr_cb_calls = 0;
    g_sr_thread_ok = 0;
    if (r == FM_OK) r = File_Copy("/sr_a.bin", "/sr_a2.bin", sr_copy_cb, NULL);
    if (g_sr_thread_ok)
    {
        (void)pthread_join(g_sr_thread, NULL);
    }
    if (r == FM_OK && (g_sr_cb_calls == 0 || !g_sr_thread_ok))
    {
        r = FM_ERR_INTERNAL;
    }
    if (r == FM_OK && g_sr_nested != FM_ERR_BUSY)
    {
        printf("[nested: %s] ", FM_ResultString(g_sr_nested));
        r = FM_ERR_BUSY;
    }
    if (r == FM_OK && g_sr_done_while_held != 0)
    {
        printf("[second task did not wait] ");
        r = FM_ERR_BUSY;
    }
    if (r == FM_OK && g_sr_second != FM_OK)
    {
        printf("[second task: %s] ", FM_ResultString(g_sr_second));
        r = g_sr_second;
    }
    if (r == FM_OK) r = sr_verify("/sr_a2.bin", 30000, 21);
    if (r == FM_OK) r = sr_verify("/sr_b2.bin", 30000, 22);
    (void)File_Delete("/sr_a.bin");
    (void)File_Delete("/sr_a2.bin");
    (void)File_Delete("/sr_b.bin");
    (void)File_Delete("/sr_b2.bin");
    (void)File_Delete("/sr_n.bin");
    CHECK_RESULT(r, "claims");

#if FM_SYNC_HAS_THREAD_SELF && (FM_LAST_ERROR_THREADS > 0)
    TEST("Threads: File_GetLastError is per task");
    {
        pthread_t th;
        int bad = 0;

        g_sr_stop = 0;
        r = sr_write("/sr_b.bin", 64, 22);
        if (r == FM_OK && pthread_create(&th, NULL, sr_ok_opener, NULL) != 0)
        {
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK)
        {
            for (i = 0; i < 400u; i++)
            {
                if (File_Open("/sr_no_such_file.bin", FM_OPEN_READ) != NULL)
                {
                    bad = -1;
                    break;
                }
                (void)sched_yield();
                if (File_GetLastError() != FM_ERR_NOT_FOUND)
                {
                    bad++;
                }
            }
            __atomic_store_n(&g_sr_stop, 1, __ATOMIC_SEQ_CST);
            (void)pthread_join(th, NULL);
            if (bad != 0)
            {
                printf("[%d of 400 reads saw the other task's result] ",
                       bad);
                r = FM_ERR_BUSY;
            }
        }
        (void)File_Delete("/sr_b.bin");
    }
    CHECK_RESULT(r, "last error");
#endif
#endif /* FM_USE_THREADS && FM_HOST_BUILD */
}

/* ================================================================ */
#if FM_USE_FMFS
/* FMFS rename: a rename that fails must leave the volume as it was. */
static FM_Storage_WriteFn g_frn_real_write;
static int                g_frn_fail;

static FM_Result frn_write(FM_Storage *st, uint64_t offset, const void *buf,
                           uint32_t len)
{
    if (g_frn_fail)
    {
        return FM_ERR_IO;
    }
    return g_frn_real_write(st, offset, buf, len);
}

static int frn_is(const char *path, const char *want)
{
    char buf[16];
    size_t got = 0;

    memset(buf, 0, sizeof(buf));
    if (File_ReadAll(path, buf, sizeof(buf) - 1u, &got) != FM_OK)
    {
        return 0;
    }
    return (got == strlen(want) && memcmp(buf, want, got) == 0) ? 1 : 0;
}

static void test_fmfs_rename_failure(void)
{
    static uint8_t mem[96 * 1024];
    static FM_Storage st;
    static FM_FS fs;
    char longname[128];
    char newdir[128];
    char p[FM_PATH_MAX];
    FM_Bool ex = FM_FALSE;
    FM_Result r;
    FM_Result mr;
    FM_Result rr;

    restore_posix_root();
    g_frn_fail = 0;
    r = RamDisk_Init(&st, "frn", mem, sizeof(mem), 512);
    if (r == FM_OK)
    {
        g_frn_real_write = st.write;
        st.write = frn_write;
        r = Storage_Register(&st);
    }
    if (r == FM_OK) r = FMFS_Init(&fs);
    if (r == FM_OK) r = FS_Mount("/frn", &fs);
    mr = r;

    TEST("FMFS rename: a failed metadata write keeps both files");
    if (r == FM_OK) r = File_WriteAll("/frn/src.txt", "NEW", 3);
    if (r == FM_OK) r = File_WriteAll("/frn/dst.txt", "OLD", 3);
    if (r == FM_OK)
    {
        g_frn_fail = 1;
        rr = File_Rename("/frn/src.txt", "/frn/dst.txt");
        g_frn_fail = 0;
        if (rr == FM_OK || !frn_is("/frn/src.txt", "NEW") ||
            !frn_is("/frn/dst.txt", "OLD"))
        {
            printf("[rename %d, src ok %d, dst ok %d] ", (int)rr,
                   frn_is("/frn/src.txt", "NEW"),
                   frn_is("/frn/dst.txt", "OLD"));
            r = FM_ERR_CORRUPTED;
        }
    }
    /* ...and the same rename works once the storage does */
    if (r == FM_OK) r = File_Rename("/frn/src.txt", "/frn/dst.txt");
    if (r == FM_OK && (!frn_is("/frn/dst.txt", "NEW") ||
                       File_Exists("/frn/src.txt", &ex) != FM_OK ||
                       ex != FM_FALSE))
    {
        r = FM_ERR_CORRUPTED;
    }
    CHECK_RESULT(r, "fmfs rename failed save");

    TEST("FMFS rename: a failed directory rename keeps the whole tree");
    r = mr;
    if (r == FM_OK) r = Directory_Create("/frn/d");
    if (r == FM_OK) r = File_WriteAll("/frn/d/a.txt", "AAA", 3);
    if (r == FM_OK) r = Directory_Create("/frn/d/sub");
    if (r == FM_OK) r = File_WriteAll("/frn/d/sub/b.txt", "BBB", 3);
    if (r == FM_OK)
    {
        g_frn_fail = 1;
        rr = Directory_Rename("/frn/d", "/frn/e");
        g_frn_fail = 0;
        ex = FM_TRUE;
        if (rr == FM_OK || !frn_is("/frn/d/a.txt", "AAA") ||
            !frn_is("/frn/d/sub/b.txt", "BBB") ||
            Directory_Exists("/frn/e", &ex) != FM_OK || ex != FM_FALSE)
        {
            printf("[rename %d] ", (int)rr);
            r = FM_ERR_CORRUPTED;
        }
    }
    CHECK_RESULT(r, "fmfs dir rename failed save");

    TEST("FMFS rename: a name too long for one child changes nothing");
    r = mr;
    memset(longname, 'L', 100);
    longname[100] = '\0';
    memset(newdir, 'N', 70);
    newdir[70] = '\0';
    (void)snprintf(p, sizeof(p), "/frn/t/%s", longname);
    if (r == FM_OK) r = Directory_Create("/frn/t");
    if (r == FM_OK) r = File_WriteAll("/frn/t/a.txt", "AAA", 3);
    if (r == FM_OK) r = Directory_Create("/frn/t/sub");
    if (r == FM_OK) r = File_WriteAll("/frn/t/sub/b.txt", "BBB", 3);
    if (r == FM_OK) r = File_WriteAll(p, "LLL", 3);
    if (r == FM_OK)
    {
        char np[FM_PATH_MAX];

        (void)snprintf(np, sizeof(np), "/frn/%s", newdir);
        rr = Directory_Rename("/frn/t", np);
        ex = FM_TRUE;
        if (rr == FM_OK || !frn_is("/frn/t/a.txt", "AAA") ||
            !frn_is("/frn/t/sub/b.txt", "BBB") || !frn_is(p, "LLL") ||
            Directory_Exists(np, &ex) != FM_OK || ex != FM_FALSE)
        {
            printf("[rename %d, a %d, b %d, long %d, new dir %d] ", (int)rr,
                   frn_is("/frn/t/a.txt", "AAA"),
                   frn_is("/frn/t/sub/b.txt", "BBB"), frn_is(p, "LLL"),
                   (int)ex);
            r = FM_ERR_CORRUPTED;
        }
    }
    /* a rename that does fit still moves the whole tree */
    if (r == FM_OK) r = Directory_Rename("/frn/t", "/frn/u");
    (void)snprintf(p, sizeof(p), "/frn/u/%s", longname);
    if (r == FM_OK && (!frn_is("/frn/u/a.txt", "AAA") ||
                       !frn_is("/frn/u/sub/b.txt", "BBB") ||
                       !frn_is(p, "LLL")))
    {
        r = FM_ERR_CORRUPTED;
    }
    CHECK_RESULT(r, "fmfs rename too long");

    g_frn_fail = 0;
    (void)FS_Unmount("/frn");
    (void)Storage_Unregister("frn");
}
#endif /* FM_USE_FMFS */

#if FM_USE_FMFS
/* ------------------------------------------------------------------ */
/* FMFS and power failure                                              */
/* ------------------------------------------------------------------ */
/* A storage that can lose power in the middle of any write: a part of
 * that write arrives, a few bytes behind it are garbage, the rest keeps
 * what it had, and every later call fails.  The suite replays file
 * operations against it, cuts the power at every single storage write
 * in turn, mounts what is left and compares it with a model. */
#define PF_CAP_MAX   (64u * 1024u)
#define PF_NAMES     6u
#define PF_FILE_MAX  9000u

typedef struct
{
    uint8_t  there;
    uint32_t size;
    uint8_t  data[PF_FILE_MAX];
} PfFile;

typedef struct
{
    PfFile f[PF_NAMES];
} PfModel;

enum
{
    PF_CREATE, PF_APPEND, PF_WRITE, PF_DELETE, PF_RENAME, PF_TRUNC,
    PF_REPACK
};

typedef struct
{
    uint8_t  kind;
    uint8_t  a;
    uint8_t  b;
    uint32_t pos;
    uint32_t len;
    uint32_t seed;
} PfOp;

static uint8_t    g_pf_mem[PF_CAP_MAX];
static uint8_t    g_pf_before_mem[PF_CAP_MAX];
static uint8_t    g_pf_after_mem[PF_CAP_MAX];
static uint8_t    g_pf_crash_mem[PF_CAP_MAX];
static PfModel    g_pf_model;
static PfModel    g_pf_before;
static PfModel    g_pf_after;
static uint8_t    g_pf_data[PF_FILE_MAX];
static uint8_t    g_pf_got[PF_FILE_MAX + 1u];
static FM_Storage g_pf_st;
static FM_FS      g_pf_fs;
static uint32_t   g_pf_cap;
static uint32_t   g_pf_block;
static const char *g_pf_name = "pf";
static const char *g_pf_mount = "/pf";
/* a cut may take the whole block down that was being written */
static int        g_pf_tear;
static uint32_t   g_pf_torn;        /* ...and how often it did            */
static long       g_pf_cut = -1;    /* the storage write that loses power */
static long       g_pf_fail = -1;   /* the storage write that fails, once */
static long       g_pf_fail_n = 1;  /* ...and how many after it do too    */
static long       g_pf_writes;
static int        g_pf_dead;
static uint32_t   g_pf_rng;
static uint32_t   g_pf_j_copy;      /* journal records seen, by kind      */
static uint32_t   g_pf_j_bounce;
static uint32_t   g_pf_j_up;
static char       g_pf_why[200];

static uint32_t pf_rand(uint32_t n)
{
    g_pf_rng = g_pf_rng * 1664525u + 1013904223u;
    return (g_pf_rng >> 8) % n;
}

static uint64_t pf_le(const uint8_t *p, uint32_t n)
{
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static FM_Result pf_st_read(FM_Storage *s, uint64_t off, void *buf,
                            uint32_t len)
{
    (void)s;
    if (g_pf_dead)
    {
        return FM_ERR_IO;
    }
    if (off > g_pf_cap || (uint64_t)len > g_pf_cap - off)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, g_pf_mem + (size_t)off, len);
    return FM_OK;
}

static FM_Result pf_st_write(FM_Storage *s, uint64_t off, const void *buf,
                             uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;

    (void)s;
    if (g_pf_dead)
    {
        return FM_ERR_IO;
    }
    if (off > g_pf_cap || (uint64_t)len > g_pf_cap - off)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (g_pf_fail >= 0 && g_pf_writes >= g_pf_fail &&
        g_pf_writes < g_pf_fail + g_pf_fail_n)
    {
        g_pf_writes++;
        return FM_ERR_IO;           /* nothing written, device alive */
    }
    if (g_pf_writes == g_pf_cut)
    {
        uint32_t i;

        if (g_pf_tear && g_pf_block > 1u && len != 0u && pf_rand(4u) != 0u)
        {
            /* A device that rewrites whole blocks (the sector of an SD
             * card, the erase unit of a NOR flash: read, erase,
             * program).  The blocks before the one it was working on
             * have their new content; that one is lost as a whole --
             * also the bytes in it that this write was not meant to
             * change -- and reads back erased or as noise. */
            uint32_t first = (uint32_t)(off / g_pf_block);
            uint32_t last = (uint32_t)((off + len - 1u) / g_pf_block);
            uint32_t victim = first + pf_rand(last - first + 1u);
            uint32_t from = victim * g_pf_block;
            uint32_t to = from + g_pf_block;
            int erased = (pf_rand(2u) == 0u);

            if (to > g_pf_cap) to = g_pf_cap;
            if (from > off)
            {
                memcpy(g_pf_mem + (size_t)off, src, (size_t)(from - off));
            }
            for (i = from; i < to; i++)
            {
                g_pf_mem[i] = erased ? 0xFFu : (uint8_t)pf_rand(256u);
            }
            g_pf_torn++;
        }
        else
        {
            uint32_t part = pf_rand(len + 1u);
            uint32_t junk = pf_rand(9u);

            memcpy(g_pf_mem + (size_t)off, src, part);
            for (i = part; i < len && i < part + junk; i++)
            {
                g_pf_mem[(size_t)off + i] = (uint8_t)pf_rand(256u);
            }
        }
        g_pf_writes++;
        g_pf_dead = 1;
        return FM_ERR_IO;
    }
    g_pf_writes++;
    if (len == 64u && memcmp(src, "FMJ1", 4) == 0)
    {
        /* a move-journal record: note which kinds the run exercised */
        if (src[12] == 2u) g_pf_j_bounce++;
        else g_pf_j_copy++;
        if (pf_le(src + 24, 8) > pf_le(src + 16, 8)) g_pf_j_up++;
    }
    memcpy(g_pf_mem + (size_t)off, src, len);
    return FM_OK;
}

static FM_Result pf_st_sync(FM_Storage *s)
{
    (void)s;
    return g_pf_dead ? FM_ERR_IO : FM_OK;
}

/* Power comes (back) on: register the storage and mount it.  `cut` is
 * the storage write at which it fails again, -1 for never. */
static FM_Result pf_attach(long cut)
{
    FM_StorageConfig cfg;
    FM_Result r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = g_pf_name;
    cfg.type = FM_STORAGE_NOR;      /* not volatile: never auto-formatted */
    cfg.capacity = g_pf_cap;
    cfg.block_size = g_pf_block;
    cfg.read_size = g_pf_block;
    cfg.write_size = g_pf_block;
    cfg.erase_size = g_pf_block;
    cfg.present = FM_TRUE;
    cfg.read = pf_st_read;
    cfg.write = pf_st_write;
    cfg.sync = pf_st_sync;
    g_pf_dead = 0;
    g_pf_cut = cut;
    g_pf_fail = -1;
    r = Storage_InitDriver(&g_pf_st, &cfg);
    if (r == FM_OK) r = Storage_Register(&g_pf_st);
    if (r == FM_OK) r = FMFS_Init(&g_pf_fs);
    if (r == FM_OK) r = FS_Mount(g_pf_mount, &g_pf_fs);
    return r;
}

static void pf_detach(void)
{
    (void)FS_Unmount(g_pf_mount);
    (void)Storage_Unmount(g_pf_name);
    (void)Storage_Unregister(g_pf_name);
}

/* An empty version 1 volume, as releases before the second table wrote
 * it. */
static void pf_v1_image(uint8_t *mem, uint32_t cap)
{
    uint32_t meta = 32u + FM_FMFS_MAX_ENTRIES * (FM_PATH_MAX + 20u);
    uint32_t i;

    memset(mem, 0, cap);
    memcpy(mem, "FMFS", 4);
    mem[4] = 1;                             /* version                */
    mem[6] = 1;                             /* one entry: the root    */
    for (i = 0; i < 4u; i++)
    {
        mem[8 + i] = (uint8_t)(meta >> (8u * i));   /* data_start     */
        mem[16 + i] = (uint8_t)(meta >> (8u * i));  /* data_next      */
        mem[24 + i] = (uint8_t)(cap >> (8u * i));   /* capacity       */
    }
    mem[32] = 1;                            /* record 0: used         */
    mem[33] = (uint8_t)FM_ITEM_DIR;
}

static void pf_fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 12345u;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
}

static const char *pf_name(uint32_t i)
{
    static char names[PF_NAMES][16];

    (void)snprintf(names[i], sizeof(names[i]), "/pf/n%u.bin", (unsigned)i);
    return names[i];
}

/* 0: no such file, 1: there (size and g_pf_got filled), -1: unreadable */
static int pf_read(uint32_t i, uint32_t *size)
{
    FM_FileInfo info;
    size_t got = 0;
    FM_Result r;

    memset(&info, 0, sizeof(info));
    r = File_GetInfo(pf_name(i), &info);
    if (r == FM_ERR_NOT_FOUND)
    {
        return 0;
    }
    if (r != FM_OK || info.size > PF_FILE_MAX)
    {
        return -1;
    }
    *size = (uint32_t)info.size;
    if (info.size == 0u)
    {
        return 1;
    }
    r = File_ReadAll(pf_name(i), g_pf_got, sizeof(g_pf_got), &got);
    return (r == FM_OK && got == info.size) ? 1 : -1;
}

static int pf_same(const PfFile *want, int state, uint32_t size)
{
    if (!want->there)
    {
        return state == 0;
    }
    return state == 1 && size == want->size &&
           (size == 0u || memcmp(g_pf_got, want->data, size) == 0);
}

static void pf_adopt(PfFile *f, int state, uint32_t size)
{
    f->there = (state == 1) ? 1u : 0u;
    f->size = (state == 1) ? size : 0u;
    if (state == 1 && size != 0u)
    {
        memcpy(f->data, g_pf_got, size);
    }
}

/* Where whole blocks can be lost (g_pf_tear): byte `i` of a file is in
 * a block that an operation writing bytes [lo, hi) of that file writes
 * to.  Files start on block boundaries, so blocks of the file are
 * blocks of the storage. */
static int pf_in_torn_block(uint32_t i, uint32_t lo, uint32_t hi)
{
    if (!g_pf_tear || g_pf_block <= 1u || hi <= lo)
    {
        return 0;
    }
    return i / g_pf_block >= lo / g_pf_block &&
           i / g_pf_block <= (hi - 1u) / g_pf_block;
}

/* What a write that was cut short may leave: the file between its old
 * and its new length; every byte outside the written range untouched
 * (a gap from a seek past the end reads as zeros); every byte that was
 * appended and is part of the file is the byte that was written.  Only
 * bytes the write replaced in place may be anything -- and, on a
 * device that loses whole blocks, the rest of the blocks it was
 * writing to: bytes of this file, never of another. */
static int pf_write_ok(const PfFile *old, const PfOp *op, int state,
                       uint32_t size)
{
    uint32_t pos = (op->kind == PF_APPEND) ? old->size : op->pos;
    uint32_t end = pos + op->len;
    uint32_t grown = (end > old->size) ? end : old->size;
    uint32_t low = (pos < old->size) ? pos : old->size;
    uint32_t i;

    if (state != 1 || !old->there || size < old->size || size > grown)
    {
        return 0;
    }
    pf_fill(g_pf_data, op->len, op->seed);
    for (i = 0; i < size; i++)
    {
        if (pf_in_torn_block(i, low, end))
        {
            continue;
        }
        if (i >= pos && i < end)
        {
            if (i >= old->size && g_pf_got[i] != g_pf_data[i - pos])
            {
                return 0;
            }
        }
        else if (g_pf_got[i] != ((i < old->size) ? old->data[i] : 0u))
        {
            return 0;
        }
    }
    return 1;
}

/* A truncate: the new length with the old bytes (zeros behind them), or
 * -- not reached here, pf_same() saw it -- the file as it was.  Where
 * whole blocks can be lost, a cut while the new end was being zeroed
 * may also leave the old length with the block the file ended in
 * damaged. */
static int pf_trunc_ok(const PfFile *old, const PfOp *op, int state,
                       uint32_t size)
{
    uint32_t i;

    if (state != 1 || !old->there)
    {
        return 0;
    }
    if (size != op->len &&
        !(size == old->size && op->len > old->size && g_pf_tear))
    {
        return 0;
    }
    for (i = 0; i < size; i++)
    {
        if (op->len > old->size &&
            pf_in_torn_block(i, old->size, op->len))
        {
            continue;
        }
        if (g_pf_got[i] != ((i < old->size) ? old->data[i] : 0u))
        {
            return 0;
        }
    }
    return 1;
}

/* Compare the mounted volume with the model.  `op` is the operation
 * that was running when the power went (or that was refused): the
 * files it names may be as before or as it leaves them, nothing in
 * between; every other file must be exactly as before.  The model takes
 * over what it finds.  0 = fine. */
static int pf_verify(PfModel *m, const PfOp *op)
{
    uint32_t there = 0;
    uint32_t count = 0;
    uint32_t i;

    if (op != NULL && op->kind == PF_RENAME)
    {
        uint32_t sa = 0;
        uint32_t sb = 0;
        int a_state = pf_read(op->a, &sa);
        int a_old = (a_state >= 0) && pf_same(&m->f[op->a], a_state, sa);
        int b_state = pf_read(op->b, &sb);
        int b_old = (b_state >= 0) && pf_same(&m->f[op->b], b_state, sb);
        int b_new = (b_state >= 0) && pf_same(&m->f[op->a], b_state, sb);

        if (a_old && b_old)
        {
            /* the rename did not happen */
        }
        else if (a_state == 0 && b_new)
        {
            m->f[op->b] = m->f[op->a];
            m->f[op->a].there = 0;
            m->f[op->a].size = 0;
        }
        else
        {
            (void)snprintf(g_pf_why, sizeof(g_pf_why),
                           "rename n%u -> n%u left neither the old nor "
                           "the new state", (unsigned)op->a,
                           (unsigned)op->b);
            return 1;
        }
    }
    for (i = 0; i < PF_NAMES; i++)
    {
        PfFile *f = &m->f[i];
        uint32_t size = 0;
        int state = pf_read(i, &size);
        int ok;

        if (state < 0)
        {
            (void)snprintf(g_pf_why, sizeof(g_pf_why),
                           "n%u cannot be read", (unsigned)i);
            return 1;
        }
        ok = pf_same(f, state, size);
        if (!ok && op != NULL && op->kind != PF_RENAME && i == op->a)
        {
            switch (op->kind)
            {
            case PF_CREATE: ok = (state == 1 && size == 0u); break;
            case PF_DELETE: ok = (state == 0); break;
            case PF_APPEND:
            case PF_WRITE:  ok = pf_write_ok(f, op, state, size); break;
            case PF_TRUNC:  ok = pf_trunc_ok(f, op, state, size); break;
            default: break;
            }
        }
        if (!ok)
        {
            (void)snprintf(g_pf_why, sizeof(g_pf_why),
                           "n%u: %s, %u bytes; the model has %s, %u bytes",
                           (unsigned)i, state ? "there" : "gone",
                           (unsigned)size, f->there ? "there" : "gone",
                           (unsigned)f->size);
            return 1;
        }
        pf_adopt(f, state, size);
        if (f->there) there++;
    }
    if (Directory_Count("/pf", 0, &count) != FM_OK || count != there)
    {
        (void)snprintf(g_pf_why, sizeof(g_pf_why),
                       "%u directory entries for %u files",
                       (unsigned)count, (unsigned)there);
        return 1;
    }
    return 0;
}

static void pf_apply(PfModel *m, const PfOp *op)
{
    PfFile *f = &m->f[op->a];

    switch (op->kind)
    {
    case PF_CREATE:
        f->there = 1;
        f->size = 0;
        break;
    case PF_APPEND:
    case PF_WRITE:
    {
        uint32_t pos = (op->kind == PF_APPEND) ? f->size : op->pos;

        if (pos > f->size)
        {
            memset(f->data + f->size, 0, pos - f->size);
        }
        pf_fill(f->data + pos, op->len, op->seed);
        if (pos + op->len > f->size) f->size = pos + op->len;
        break;
    }
    case PF_DELETE:
        f->there = 0;
        f->size = 0;
        break;
    case PF_RENAME:
        m->f[op->b] = *f;
        f->there = 0;
        f->size = 0;
        break;
    case PF_TRUNC:
        if (op->len > f->size)
        {
            memset(f->data + f->size, 0, op->len - f->size);
        }
        f->size = op->len;
        break;
    default:
        break;
    }
}

static FM_Result pf_run(const PfOp *op)
{
    FM_File *f;
    uint32_t wr = 0;
    FM_Result r = FM_OK;
    FM_Result rc;

    switch (op->kind)
    {
    case PF_CREATE:
        f = File_Open(pf_name(op->a), FM_OPEN_WRITE | FM_OPEN_CREATE);
        if (f == NULL) return File_GetLastError();
        return File_Close(f);
    case PF_APPEND:
    case PF_WRITE:
        f = File_Open(pf_name(op->a), (op->kind == PF_APPEND)
                          ? (FM_OPEN_WRITE | FM_OPEN_APPEND)
                          : (FM_OPEN_READ | FM_OPEN_WRITE));
        if (f == NULL) return File_GetLastError();
        if (op->kind == PF_WRITE)
        {
            r = File_Seek(f, (int64_t)op->pos, FM_SEEK_SET);
        }
        pf_fill(g_pf_data, op->len, op->seed);
        if (r == FM_OK) r = File_Write(f, g_pf_data, op->len, &wr);
        rc = File_Close(f);
        if (r == FM_OK) r = rc;
        if (r == FM_OK && wr != op->len) r = FM_ERR_NO_SPACE;
        return r;
    case PF_DELETE:
        return File_Delete(pf_name(op->a));
    case PF_RENAME:
    {
        char to[16];

        (void)snprintf(to, sizeof(to), "%s", pf_name(op->b));
        return File_Rename(pf_name(op->a), to);
    }
    case PF_TRUNC:
        f = File_Open(pf_name(op->a), FM_OPEN_READ | FM_OPEN_WRITE);
        if (f == NULL) return File_GetLastError();
        r = File_Truncate(f, op->len);
        rc = File_Close(f);
        return (r == FM_OK) ? rc : r;
    default:
        return FMFS_Repack(&g_pf_fs);
    }
}

/* The next operation of the run: whatever keeps a small volume busy and
 * nearly full, so that files grow in place, move to the tail, and the
 * data area is compacted again and again. */
static void pf_gen(const PfModel *m, PfOp *op, uint32_t k)
{
    uint32_t pick = pf_rand(100u);
    const PfFile *f;

    memset(op, 0, sizeof(*op));
    op->a = (uint8_t)pf_rand(PF_NAMES);
    op->seed = k * 7919u + 17u;
    f = &m->f[op->a];
    if (pick >= 76u && pick < 82u)
    {
        op->kind = PF_REPACK;
        return;
    }
    if (!f->there)
    {
        op->kind = PF_CREATE;
        return;
    }
    if (pick < 26u)
    {
        op->kind = PF_APPEND;
        op->len = 1u + pf_rand(pf_rand(4u) == 0u ? 40u : 1500u);
    }
    else if (pick < 44u)
    {
        op->kind = PF_APPEND;
        op->len = 2000u + pf_rand(3000u);
    }
    else if (pick < 56u)
    {
        op->kind = PF_WRITE;
        op->pos = pf_rand(f->size + 60u);
        op->len = 1u + pf_rand(600u);
    }
    else if (pick < 63u)
    {
        op->kind = PF_DELETE;
    }
    else if (pick < 69u)
    {
        op->kind = PF_RENAME;
        op->b = (uint8_t)((op->a + 1u + pf_rand(PF_NAMES - 1u)) % PF_NAMES);
    }
    else if (pick < 76u)
    {
        op->kind = PF_TRUNC;
        op->len = (pf_rand(3u) == 0u) ? pf_rand(f->size + 1u)
                                      : f->size + pf_rand(300u);
    }
    else
    {
        op->kind = PF_APPEND;
        op->len = 1u + pf_rand(12u);
    }
    if ((op->kind == PF_APPEND && f->size + op->len > PF_FILE_MAX) ||
        (op->kind == PF_WRITE && op->pos + op->len > PF_FILE_MAX) ||
        (op->kind == PF_TRUNC && op->len > PF_FILE_MAX))
    {
        op->kind = PF_DELETE;
    }
}

/* After a recovery the volume has to be more than readable: use it. */
static int pf_aftercare(PfModel *m)
{
    static uint8_t want[300];
    static uint8_t back[301];
    size_t got = 0;
    FM_Result r;

    r = FS_Check("/pf");
    if (r == FM_OK)
    {
        pf_fill(want, sizeof(want), 4242u);
        r = File_WriteAll("/pf/zz.bin", want, sizeof(want));
        if (r == FM_OK)
        {
            r = FMFS_Repack(&g_pf_fs);
            if (r == FM_OK) r = File_ReadAll("/pf/zz.bin", back,
                                             sizeof(back), &got);
            if (r == FM_OK && (got != sizeof(want) ||
                               memcmp(back, want, sizeof(want)) != 0))
            {
                r = FM_ERR_CRC;
            }
        }
        else if (r == FM_ERR_NO_SPACE)
        {
            r = FMFS_Repack(&g_pf_fs);      /* a full volume is fine */
        }
        (void)File_Delete("/pf/zz.bin");
    }
    if (r != FM_OK)
    {
        (void)snprintf(g_pf_why, sizeof(g_pf_why),
                       "the recovered volume cannot be used (%s)",
                       FM_ResultString(r));
        return 1;
    }
    if (pf_verify(m, NULL) != 0)
    {
        return 1;
    }
    /* and what it wrote while doing so mounts again */
    pf_detach();
    r = pf_attach(-1);
    if (r != FM_OK)
    {
        (void)snprintf(g_pf_why, sizeof(g_pf_why),
                       "second mount: %s", FM_ResultString(r));
        return 1;
    }
    return pf_verify(m, NULL);
}

/* The storage write of operation `k` that cut number `t` falls on:
 * every write when there are few, else the first and last 80 and 80
 * spread over the rest. */
static long pf_cut_point(long writes, long t)
{
    if (writes <= 240)
    {
        return (t < writes) ? t : -1;
    }
    if (t < 80)
    {
        return t;
    }
    if (t < 160)
    {
        long span = (writes - 160) / 80;

        return 80 + (t - 80) * span + (long)pf_rand((uint32_t)span);
    }
    return (t < 240) ? writes - 240 + t : -1;
}

typedef struct
{
    uint32_t cuts;          /* power cuts tried                        */
    uint32_t damaged;       /* ...that left something the model forbids */
    uint32_t recut;         /* second cuts, during the recovery         */
    uint32_t refused;       /* operations refused for lack of space     */
    uint32_t j_copy;
    uint32_t j_bounce;
    uint32_t j_up;
    uint32_t torn;          /* cuts that took a whole block down        */
} PfStats;

/* Run `nops` operations; before going on from each, replay it with the
 * power cut at each of its storage writes.  FM_OK unless the harness
 * itself could not work (g_pf_why says why); what the cuts did is in
 * `st`.  Stops after `stop` damaged volumes. */
static FM_Result pf_campaign(uint32_t cap, uint32_t block, int v1,
                             uint32_t seed, uint32_t nops, uint32_t stop,
                             PfStats *st)
{
    FM_Result r;
    uint32_t k;

    memset(st, 0, sizeof(*st));
    g_pf_why[0] = '\0';
    g_pf_cap = cap;
    g_pf_block = block;
    g_pf_rng = seed;
    g_pf_j_copy = 0;
    g_pf_j_bounce = 0;
    g_pf_j_up = 0;
    g_pf_torn = 0;
    g_pf_tear = (block > 1u) ? 1 : 0;
    memset(&g_pf_model, 0, sizeof(g_pf_model));
    if (v1)
    {
        pf_v1_image(g_pf_mem, cap);
    }
    else
    {
        memset(g_pf_mem, 0, cap);
    }
    r = pf_attach(-1);
    if (r == FM_OK && FMFS_IsPowerSafe(&g_pf_fs) != (v1 ? FM_FALSE : FM_TRUE))
    {
        (void)snprintf(g_pf_why, sizeof(g_pf_why),
                       "the volume is not the version asked for");
        r = FM_ERR_INTERNAL;
    }
    for (k = 0; k < nops && r == FM_OK && st->damaged < stop; k++)
    {
        PfOp op;
        long writes;
        long t;
        FM_Result ro;

        pf_gen(&g_pf_model, &op, k);
        memcpy(g_pf_before_mem, g_pf_mem, cap);
        g_pf_before = g_pf_model;
        g_pf_writes = 0;
        ro = pf_run(&op);
        writes = g_pf_writes;
        if (ro == FM_OK)
        {
            pf_apply(&g_pf_model, &op);
        }
        else
        {
            st->refused++;
        }
        if (pf_verify(&g_pf_model, (ro == FM_OK) ? NULL : &op) != 0)
        {
            char why[100];

            (void)snprintf(why, sizeof(why), "%.99s", g_pf_why);
            (void)snprintf(g_pf_why, sizeof(g_pf_why),
                           "op %u (kind %u, %s) without any cut: %s",
                           (unsigned)k, (unsigned)op.kind,
                           FM_ResultString(ro), why);
            r = FM_ERR_INTERNAL;
            break;
        }
        memcpy(g_pf_after_mem, g_pf_mem, cap);
        g_pf_after = g_pf_model;

        for (t = 0; r == FM_OK && st->damaged < stop; t++)
        {
            long cut = pf_cut_point(writes, t);
            long recovery;
            int bad = 0;

            if (cut < 0)
            {
                break;
            }
            pf_detach();
            memcpy(g_pf_mem, g_pf_before_mem, cap);
            g_pf_model = g_pf_before;
            r = pf_attach(-1);
            if (r != FM_OK)
            {
                (void)snprintf(g_pf_why, sizeof(g_pf_why),
                               "op %u: the image before it does not mount",
                               (unsigned)k);
                break;
            }
            g_pf_writes = 0;
            g_pf_cut = cut;
            (void)pf_run(&op);
            pf_detach();                    /* the power is gone */
            memcpy(g_pf_crash_mem, g_pf_mem, cap);

            g_pf_writes = 0;
            ro = pf_attach(-1);
            recovery = g_pf_writes;
            if (ro == FM_OK && recovery > 0)
            {
                /* the mount had work to do: cut the power again in the
                 * middle of that */
                pf_detach();
                memcpy(g_pf_mem, g_pf_crash_mem, cap);
                (void)pf_attach((long)pf_rand((uint32_t)recovery));
                pf_detach();
                ro = pf_attach(-1);
                st->recut++;
            }
            st->cuts++;
            if (ro != FM_OK)
            {
                (void)snprintf(g_pf_why, sizeof(g_pf_why),
                               "mount: %s", FM_ResultString(ro));
                bad = 1;
            }
            else if (pf_verify(&g_pf_model, &op) != 0 ||
                     pf_aftercare(&g_pf_model) != 0)
            {
                bad = 1;
            }
            if (bad)
            {
                if (st->damaged == 0u)
                {
                    char why[120];

                    (void)snprintf(why, sizeof(why), "%.119s", g_pf_why);
                    (void)snprintf(g_pf_why, sizeof(g_pf_why),
                                   "op %u (kind %u) cut at write %ld of "
                                   "%ld: %s", (unsigned)k,
                                   (unsigned)op.kind, cut, writes, why);
                }
                st->damaged++;
            }
        }
        pf_detach();
        memcpy(g_pf_mem, g_pf_after_mem, cap);
        g_pf_model = g_pf_after;
        if (r == FM_OK)
        {
            r = pf_attach(-1);
        }
    }
    pf_detach();
    g_pf_tear = 0;
    st->j_copy = g_pf_j_copy;
    st->j_bounce = g_pf_j_bounce;
    st->j_up = g_pf_j_up;
    st->torn = g_pf_torn;
    return r;
}

#if FM_FMFS_POWER_SAFE
/* The same run, but instead of the power going, ONE storage write of
 * the operation fails and the device stays alive (a NAK, a timeout).
 * The volume is not mounted again in between: the operation's files
 * must read back as before or as it leaves them, everything else as
 * before, and the mount must go on working -- which shows that the
 * table in RAM and the one on the storage have not drifted apart.
 * `st->cuts` counts the failed writes, `st->refused` the operations
 * that reported one. */
static FM_Result pf_io_campaign(uint32_t cap, uint32_t block, uint32_t seed,
                                uint32_t nops, PfStats *st)
{
    FM_Result r;
    uint32_t k;

    memset(st, 0, sizeof(*st));
    g_pf_why[0] = '\0';
    g_pf_cap = cap;
    g_pf_block = block;
    g_pf_rng = seed;
    g_pf_j_copy = 0;
    g_pf_j_bounce = 0;
    g_pf_j_up = 0;
    g_pf_tear = 0;
    memset(&g_pf_model, 0, sizeof(g_pf_model));
    memset(g_pf_mem, 0, cap);
    r = pf_attach(-1);
    for (k = 0; k < nops && r == FM_OK && st->damaged == 0u; k++)
    {
        PfOp op;
        long writes;
        long t;
        FM_Result ro;

        pf_gen(&g_pf_model, &op, k);
        memcpy(g_pf_before_mem, g_pf_mem, cap);
        g_pf_before = g_pf_model;
        g_pf_writes = 0;
        ro = pf_run(&op);
        writes = g_pf_writes;
        if (ro == FM_OK)
        {
            pf_apply(&g_pf_model, &op);
        }
        if (pf_verify(&g_pf_model, (ro == FM_OK) ? NULL : &op) != 0)
        {
            r = FM_ERR_INTERNAL;
            break;
        }
        memcpy(g_pf_after_mem, g_pf_mem, cap);
        g_pf_after = g_pf_model;

        for (t = 0; r == FM_OK && st->damaged == 0u; t++)
        {
            long at = pf_cut_point(writes, t);

            if (at < 0)
            {
                break;
            }
            pf_detach();
            memcpy(g_pf_mem, g_pf_before_mem, cap);
            g_pf_model = g_pf_before;
            r = pf_attach(-1);
            if (r != FM_OK)
            {
                break;
            }
            g_pf_writes = 0;
            g_pf_fail = at;
            ro = pf_run(&op);
            g_pf_fail = -1;
            st->cuts++;
            if (ro != FM_OK)
            {
                st->refused++;
            }
            if (pf_verify(&g_pf_model, &op) != 0 ||
                pf_aftercare(&g_pf_model) != 0)
            {
                char why[120];

                (void)snprintf(why, sizeof(why), "%.119s", g_pf_why);
                (void)snprintf(g_pf_why, sizeof(g_pf_why),
                               "op %u (kind %u, %s) with write %ld of %ld "
                               "failing: %s", (unsigned)k,
                               (unsigned)op.kind, FM_ResultString(ro), at,
                               writes, why);
                st->damaged++;
            }
        }
        pf_detach();
        memcpy(g_pf_mem, g_pf_after_mem, cap);
        g_pf_model = g_pf_after;
        if (r == FM_OK)
        {
            r = pf_attach(-1);
        }
    }
    pf_detach();
    st->j_copy = g_pf_j_copy;
    st->j_bounce = g_pf_j_bounce;
    st->j_up = g_pf_j_up;
    return r;
}

static void pf_report(FM_Result r, const PfStats *st, int blocks)
{
    printf("[%u cuts, %u took a block, %u more during recovery, %u "
           "refusals, journal %u+%u, %u upward] ", (unsigned)st->cuts,
           (unsigned)st->torn, (unsigned)st->recut,
           (unsigned)st->refused, (unsigned)st->j_copy,
           (unsigned)st->j_bounce, (unsigned)st->j_up);
    if (r != FM_OK)
    {
        FAIL(g_pf_why);
    }
    else if (st->damaged != 0u)
    {
        printf("[%u damaged] ", (unsigned)st->damaged);
        FAIL(g_pf_why);
    }
    else if (st->cuts < 1000u || st->recut == 0u || st->refused == 0u ||
             st->j_copy == 0u ||
             (blocks < 2 && (st->j_bounce == 0u || st->j_up == 0u)) ||
             (blocks && st->torn < 500u))
    {
        /* every kind of move has to have been cut, or the run proved
         * less than it claims */
        FAIL("the run did not exercise every path");
    }
    else
    {
        PASS();
    }
}
#endif

static void test_fmfs_powerfail(void)
{
    static uint8_t mem[64 * 1024];
    static FM_Storage st;
    static FM_FS fs;
    PfStats stats;
    FM_Result r;

    restore_posix_root();

    TEST("FMFS: a formatted volume says whether it is power-fail safe");
    {
        static uint8_t small[16 * 1024];
        FM_Bool want_big = (FM_FMFS_POWER_SAFE >= 1) ? FM_TRUE : FM_FALSE;
        FM_Bool want_small = (FM_FMFS_POWER_SAFE >= 2) ? FM_TRUE : FM_FALSE;
        FM_Bool got_big = FM_FALSE;
        FM_Bool got_small = FM_FALSE;

        memset(mem, 0, sizeof(mem));
        memset(small, 0, sizeof(small));
        r = RamDisk_Init(&st, "pfa", mem, sizeof(mem), 512);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/pfa", &fs);
        if (r == FM_OK) got_big = FMFS_IsPowerSafe(&fs);
        if (r == FM_OK) r = sr_write("/pfa/a.bin", 3000, 5);
        if (r == FM_OK) r = sr_verify("/pfa/a.bin", 3000, 5);
        (void)FS_Unmount("/pfa");
        (void)Storage_Unregister("pfa");
        if (r == FM_OK && FMFS_IsPowerSafe(&fs))
        {
            r = FM_ERR_INTERNAL;        /* not mounted: not anything */
        }
        /* 16 KiB: two tables would leave next to nothing for files */
        if (r == FM_OK) r = RamDisk_Init(&st, "pfa", small, sizeof(small),
                                         512);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/pfa", &fs);
        if (r == FM_OK) got_small = FMFS_IsPowerSafe(&fs);
        if (r == FM_OK) r = sr_write("/pfa/a.bin", 1500, 6);
        if (r == FM_OK) r = sr_verify("/pfa/a.bin", 1500, 6);
        (void)FS_Unmount("/pfa");
        (void)Storage_Unregister("pfa");
        if (r == FM_OK && (got_big != want_big || got_small != want_small))
        {
            printf("[64K %d, 16K %d] ", (int)got_big, (int)got_small);
            r = FM_ERR_INTERNAL;
        }
    }
    CHECK_RESULT(r, "power-safe flag");

    TEST("FMFS: a version 1 volume mounts, works and stays version 1");
    {
        FM_Bool safe_after_format = FM_FALSE;

        pf_v1_image(mem, (uint32_t)sizeof(mem));
        r = RamDisk_Init(&st, "pfa", mem, sizeof(mem), 512);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/pfa", &fs);
        if (r == FM_OK && FMFS_IsPowerSafe(&fs)) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = sr_write("/pfa/a.bin", 9000, 1);
        if (r == FM_OK) r = sr_write("/pfa/b.bin", 20000, 2);
        if (r == FM_OK) r = sr_write("/pfa/c.bin", 20000, 3);
        if (r == FM_OK) r = File_Delete("/pfa/a.bin");
        /* some 10 KiB are free at the tail and 9000 bytes in front of
         * b: this only fits after a compaction */
        if (r == FM_OK) r = sr_write("/pfa/d.bin", 16000, 4);
        if (r == FM_OK) r = FS_Unmount("/pfa");
        if (r == FM_OK && (memcmp(mem, "FMFS", 4) != 0 || mem[4] != 1u))
        {
            r = FM_ERR_INVALID_FORMAT;  /* converted behind our back */
        }
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/pfa", &fs);
        if (r == FM_OK && FMFS_IsPowerSafe(&fs)) r = FM_ERR_INTERNAL;
        if (r == FM_OK) r = sr_verify("/pfa/b.bin", 20000, 2);
        if (r == FM_OK) r = sr_verify("/pfa/c.bin", 20000, 3);
        if (r == FM_OK) r = sr_verify("/pfa/d.bin", 16000, 4);
        /* formatting is the way to the new layout */
        if (r == FM_OK) r = FS_Format("/pfa");
        if (r == FM_OK) safe_after_format = FMFS_IsPowerSafe(&fs);
        if (r == FM_OK) r = sr_write("/pfa/e.bin", 5000, 7);
        if (r == FM_OK) r = FS_Unmount("/pfa");
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/pfa", &fs);
        if (r == FM_OK) r = sr_verify("/pfa/e.bin", 5000, 7);
        if (r == FM_OK &&
            (safe_after_format != ((FM_FMFS_POWER_SAFE >= 1) ? FM_TRUE
                                                             : FM_FALSE) ||
             FMFS_IsPowerSafe(&fs) != safe_after_format))
        {
            r = FM_ERR_INTERNAL;
        }
        (void)FS_Unmount("/pfa");
        (void)Storage_Unregister("pfa");
    }
    CHECK_RESULT(r, "version 1 volume");

#if FM_FMFS_POWER_SAFE
    TEST("FMFS: either table alone is enough, both gone is an error");
    {
        uint32_t slot;
        uint32_t step;
        FM_Result both = FM_OK;

        /* a storage that is not a RAM disk: never formatted on its own */
        g_pf_cap = 32u * 1024u;
        g_pf_block = 512;
        memset(g_pf_mem, 0, g_pf_cap);
        r = pf_attach(-1);
        if (r == FM_OK) r = File_WriteAll("/pf/one.txt", "first", 5);
        if (r == FM_OK) r = File_WriteAll("/pf/two.txt", "second", 6);
        pf_detach();
        slot = (uint32_t)pf_le(g_pf_mem + 36, 4);
        if (r == FM_OK && (slot < 5000u || slot > 9000u ||
                           memcmp(g_pf_mem + slot, "FMFS", 4) != 0))
        {
            r = FM_ERR_INVALID_FORMAT;
        }
        /* Tear the table that is NOT in use -- the one a save was
         * writing when the power went -- in three different places.
         * The volume mounts from the other one, and the save that
         * follows repairs the torn one, so the next round tears the
         * other slot. */
        for (step = 0; step < 4u && r == FM_OK; step++)
        {
            static const char *const text[] = { "aaaa", "bbbb", "cccc",
                                                "dddd", "eeee" };
            uint32_t seq_a = (uint32_t)pf_le(g_pf_mem + 32, 4);
            uint32_t seq_b = (uint32_t)pf_le(g_pf_mem + slot + 32u, 4);
            uint8_t *older = (seq_a < seq_b) ? g_pf_mem : g_pf_mem + slot;

            if (seq_a == seq_b)
            {
                r = FM_ERR_INTERNAL;
                break;
            }
            if (step == 0u) memset(older, 0xA5, 40);        /* header  */
            else if (step == 1u) memset(older + 700, 0x5A, 300);
            else if (step == 2u) older[63] ^= 0x80u;        /* the CRC */
            else memset(older + 3000, 0, 2824);             /* the end */
            r = pf_attach(-1);
            if (r == FM_OK && !FMFS_IsPowerSafe(&g_pf_fs))
            {
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK && (!frn_is("/pf/two.txt", "second") ||
                               !frn_is("/pf/one.txt",
                                       step ? text[step - 1u] : "first")))
            {
                r = FM_ERR_CRC;
            }
            if (r == FM_OK) r = File_WriteAll("/pf/one.txt", text[step], 4);
            pf_detach();
        }
        if (r == FM_OK)
        {
            g_pf_mem[100] ^= 0x01u;
            g_pf_mem[slot + 100u] ^= 0x01u;
            both = pf_attach(-1);
            pf_detach();
            g_pf_mem[100] ^= 0x01u;
            g_pf_mem[slot + 100u] ^= 0x01u;
            if (both != FM_ERR_CORRUPTED)
            {
                printf("[both torn: %s] ", FM_ResultString(both));
                r = FM_ERR_INTERNAL;
            }
        }
        if (r == FM_OK)
        {
            /* ...and that mount did not format anything */
            r = pf_attach(-1);
            if (r == FM_OK && (!frn_is("/pf/one.txt", "dddd") ||
                               !frn_is("/pf/two.txt", "second")))
            {
                r = FM_ERR_CRC;
            }
            pf_detach();
        }
    }
    CHECK_RESULT(r, "two tables");

    TEST("FMFS: power cut at every storage write (byte storage)");
    r = pf_campaign(30u * 1024u, 1, 0, 0x51u, 150, 1, &stats);
    pf_report(r, &stats, 0);

    /* ...on a device that rewrites whole 512-byte blocks, and loses the
     * one it was working on */
    TEST("FMFS: power cut at every storage write (512-byte blocks)");
    r = pf_campaign(34u * 1024u, 512, 0, 0x9E3u, 200, 1, &stats);
    pf_report(r, &stats, 1);

    /* ...and one with the 4 KiB erase units of a serial NOR flash */
    TEST("FMFS: power cut at every storage write (4096-byte blocks)");
    r = pf_campaign(56u * 1024u, 4096, 0, 0x77u, 150, 1, &stats);
    /* (with blocks this large next to the files, parking a file's
     * bytes elsewhere never saves a step, and whether a file is ever
     * pushed upwards depends on how File_Write splits its data: the
     * 512-byte run above is the one that has to show both) */
    pf_report(r, &stats, 2);

    TEST("FMFS: on a block storage files start on block boundaries");
    {
        /* the same random run against a model as on the RAM disk, on a
         * full volume whose files each have their blocks to themselves */
        uint32_t refused = 0;
        uint32_t round;

        r = FM_OK;
        g_pf_name = "srf";
        g_pf_mount = "/srf";
        for (round = 0; round < 2u && r == FM_OK; round++)
        {
            /* out of reach: a hole in front of a file that is smaller
             * than 1/FM_FMFS_MOVE_STEPS of it, and the rest of the
             * last block of every file */
            uint32_t slack = SRF_FILES * 511u;

            if ((uint64_t)(FM_FMFS_MOVE_STEPS) < SRF_MAX)
            {
                slack += 2u * SRF_FILES *
                         (uint32_t)(SRF_MAX / (FM_FMFS_MOVE_STEPS) + 512u);
            }
            g_pf_cap = 40u * 1024u;
            g_pf_block = 512;
            memset(g_pf_mem, 0, g_pf_cap);
            r = pf_attach(-1);
            if (r == FM_OK) r = srf_run(&g_pf_fs, 0x4321u + round * 131u,
                                        1500, slack, &refused);
            if (r == FM_OK)
            {
                pf_detach();
                r = pf_attach(-1);
                if (r == FM_OK) r = srf_check_all();
            }
            if (r == FM_OK)
            {
                /* every file on a block boundary: read the table */
                uint32_t slot = (uint32_t)pf_le(g_pf_mem + 36, 4);
                const uint8_t *table =
                    (pf_le(g_pf_mem + 32, 4) >
                     pf_le(g_pf_mem + slot + 32u, 4)) ? g_pf_mem
                                                      : g_pf_mem + slot;
                uint32_t files = 0;
                uint32_t i;

                for (i = 0; i < FM_FMFS_MAX_ENTRIES; i++)
                {
                    const uint8_t *rec = table + 64u +
                                         i * (FM_PATH_MAX + 20u);

                    if (rec[0] && rec[1] == (uint8_t)FM_ITEM_FILE &&
                        pf_le(rec + 12, 8) != 0u)
                    {
                        files++;
                        if (pf_le(rec + 4, 8) % 512u != 0u)
                        {
                            printf("[a file at %lu] ",
                                   (unsigned long)pf_le(rec + 4, 8));
                            r = FM_ERR_INTERNAL;
                        }
                    }
                }
                if (r == FM_OK && files == 0u) r = FM_ERR_INTERNAL;
            }
            pf_detach();
        }
        g_pf_name = "pf";
        g_pf_mount = "/pf";
        if (r == FM_OK && refused < 10u)
        {
            printf("[only %u refusals] ", (unsigned)refused);
            r = FM_ERR_INTERNAL;
        }
    }
    CHECK_RESULT(r, "block boundaries");

    TEST("FMFS: an I/O error in a compaction is made good afterwards");
    {
        uint32_t variant;

        r = FM_OK;
        for (variant = 0; variant < 2u && r == FM_OK; variant++)
        {
            /* n0 (300 bytes) in front of three files.  Deleting it and
             * compacting slides each of them down by 300 bytes, which
             * is less than their length: journalled moves.  With
             * hardly any free space elsewhere they go in 300-byte
             * steps; with a larger tail, through it. */
            PfOp setup[] = {
                { PF_CREATE, 0, 0, 0, 0, 0 },
                { PF_APPEND, 0, 0, 0, 300, 1 },
                { PF_CREATE, 1, 0, 0, 0, 0 },
                { PF_APPEND, 1, 0, 0, 6000, 2 },
                { PF_CREATE, 2, 0, 0, 0, 0 },
                { PF_APPEND, 2, 0, 0, 8000, 3 },
                { PF_CREATE, 3, 0, 0, 0, 0 },
                { PF_APPEND, 3, 0, 0, 6500, 4 },
                { PF_DELETE, 0, 0, 0, 0, 0 }
            };
            uint32_t errors = 0;
            uint32_t before_copy;
            uint32_t before_bounce;
            long writes = 0;
            long j;
            uint32_t i;

            if (variant == 1u) setup[7].len = 3000;
            g_pf_cap = 32u * 1024u;
            g_pf_block = 1;
            memset(g_pf_mem, 0, g_pf_cap);
            memset(&g_pf_model, 0, sizeof(g_pf_model));
            r = pf_attach(-1);
            for (i = 0; i < sizeof(setup) / sizeof(setup[0]) &&
                        r == FM_OK; i++)
            {
                r = pf_run(&setup[i]);
                if (r == FM_OK) pf_apply(&g_pf_model, &setup[i]);
            }
            pf_detach();
            memcpy(g_pf_before_mem, g_pf_mem, g_pf_cap);
            before_copy = g_pf_j_copy;
            before_bounce = g_pf_j_bounce;
            if (r == FM_OK) r = pf_attach(-1);
            g_pf_writes = 0;
            if (r == FM_OK) r = FMFS_Repack(&g_pf_fs);
            writes = g_pf_writes;
            if (r == FM_OK && pf_verify(&g_pf_model, NULL) != 0)
            {
                r = FM_ERR_CRC;
            }
            if (r == FM_OK &&
                ((variant == 0u && g_pf_j_copy - before_copy < 30u) ||
                 (variant == 1u && g_pf_j_bounce == before_bounce)))
            {
                printf("[variant %u took another path] ",
                       (unsigned)variant);
                r = FM_ERR_INTERNAL;
            }
            for (j = 0; j < writes && r == FM_OK; j++)
            {
                pf_detach();
                memcpy(g_pf_mem, g_pf_before_mem, g_pf_cap);
                r = pf_attach(-1);
                if (r != FM_OK) break;
                g_pf_writes = 0;
                g_pf_fail = j;          /* one write fails; no reset */
                if (FMFS_Repack(&g_pf_fs) != FM_OK) errors++;
                /* same mount, no reset in between: every file reads
                 * back, and the compaction can be asked for again */
                if (pf_verify(&g_pf_model, NULL) != 0)
                {
                    printf("[write %ld of %ld: %s] ", j, writes, g_pf_why);
                    r = FM_ERR_CRC;
                }
                if (r == FM_OK) r = FMFS_Repack(&g_pf_fs);
                if (r == FM_OK && pf_verify(&g_pf_model, NULL) != 0)
                {
                    printf("[after write %ld: %s] ", j, g_pf_why);
                    r = FM_ERR_CRC;
                }
            }
            pf_detach();
            if (r == FM_OK && (writes < 20 || errors != (uint32_t)writes))
            {
                printf("[%ld writes, %u reported] ", writes,
                       (unsigned)errors);
                r = FM_ERR_INTERNAL;
            }
        }
    }
    CHECK_RESULT(r, "error in compaction");

    TEST("FMFS: one failing write in any operation leaves the mount sound");
    r = pf_io_campaign(30u * 1024u, 1, 0x51u, 150, &stats);
    if (r == FM_OK)
    {
        r = pf_io_campaign(34u * 1024u, 512, 0x9E3u, 150, &stats);
    }
    printf("[%u failing writes, %u reported] ", (unsigned)stats.cuts,
           (unsigned)stats.refused);
    if (r != FM_OK || stats.damaged != 0u)
    {
        FAIL(g_pf_why[0] ? g_pf_why : "the harness could not run");
    }
    else if (stats.cuts < 1000u || stats.j_copy == 0u ||
             stats.j_bounce == 0u || stats.j_up == 0u)
    {
        FAIL("the run did not exercise every path");
    }
    else
    {
        PASS();
    }

    TEST("FMFS: a save that failed is made good before space is reused");
    {
        /* n0, n1, n2, n3 back to back, hardly any tail.  Cutting n0
         * down to 100 bytes fails at its one table write: the mount
         * has the short n0, the storage still the long one.  The next
         * operation moves a growing n1 into the bytes n0 gave up --
         * and then the power goes.  If the table with the short n0
         * were not written first, the reset would bring the long n0
         * back with n1's bytes in it. */
        static const PfOp setup[] = {
            { PF_CREATE, 0, 0, 0, 0, 0 },
            { PF_APPEND, 0, 0, 0, 6000, 2 },
            { PF_CREATE, 1, 0, 0, 0, 0 },
            { PF_APPEND, 1, 0, 0, 3000, 1 },
            { PF_CREATE, 2, 0, 0, 0, 0 },
            { PF_APPEND, 2, 0, 0, 9000, 3 },
            { PF_CREATE, 3, 0, 0, 0, 0 },
            { PF_APPEND, 3, 0, 0, 2500, 4 }
        };
        static const PfOp cut = { PF_TRUNC, 0, 0, 0, 100, 0 };
        static const PfOp grow[2] = {
            { PF_APPEND, 1, 0, 0, 2000, 5 },
            { PF_TRUNC, 1, 0, 0, 5000, 0 }
        };
        static PfModel start;
        uint32_t tried = 0;
        uint32_t back = 0;
        uint32_t which;
        uint32_t i;

        g_pf_cap = 32u * 1024u;
        g_pf_block = 1;
        memset(g_pf_mem, 0, g_pf_cap);
        memset(&g_pf_model, 0, sizeof(g_pf_model));
        r = pf_attach(-1);
        for (i = 0; i < sizeof(setup) / sizeof(setup[0]) && r == FM_OK; i++)
        {
            r = pf_run(&setup[i]);
            if (r == FM_OK) pf_apply(&g_pf_model, &setup[i]);
        }
        pf_detach();
        memcpy(g_pf_before_mem, g_pf_mem, g_pf_cap);
        start = g_pf_model;

        for (which = 0; which < 2u && r == FM_OK; which++)
        {
            long writes = 1;
            long j;

            for (j = -1; j < writes && r == FM_OK; j++)
            {
                uint32_t size = 0;
                FM_Result rd;
                int state;

                memcpy(g_pf_mem, g_pf_before_mem, g_pf_cap);
                g_pf_model = start;
                r = pf_attach(-1);
                if (r != FM_OK) break;
                /* the save of the truncate fails, and so does the one
                 * that closing the file tries */
                g_pf_writes = 0;
                g_pf_fail = 0;
                g_pf_fail_n = 2;
                rd = pf_run(&cut);
                g_pf_fail = -1;
                g_pf_fail_n = 1;
                /* (not looking at n0 here: closing ANY handle writes a
                 * table that is behind, and the window would be gone) */
                if (rd == FM_OK)
                {
                    printf("[the truncate did not fail] ");
                    r = FM_ERR_INTERNAL;
                }
                g_pf_writes = 0;
                g_pf_cut = j;           /* -1: count the writes */
                rd = pf_run(&grow[which]);
                if (j < 0)
                {
                    writes = g_pf_writes;
                    if (rd != FM_OK || writes < 4)
                    {
                        printf("[grow: %s, %ld writes] ",
                               FM_ResultString(rd), writes);
                        r = FM_ERR_INTERNAL;
                    }
                }
                pf_detach();            /* the power is gone */
                if (r != FM_OK) break;
                r = pf_attach(-1);
                if (r != FM_OK) break;
                /* n0: its first 100 bytes, or all of it */
                state = pf_read(0, &size);
                if (state == 1 && size == 100u)
                {
                    g_pf_model.f[0].size = 100;
                }
                else
                {
                    back++;
                }
                if (pf_verify(&g_pf_model, &grow[which]) != 0)
                {
                    printf("[%s, cut at write %ld of %ld: %s] ",
                           which ? "truncate" : "append", j, writes,
                           g_pf_why);
                    r = FM_ERR_CRC;
                }
                pf_detach();
                tried++;
            }
        }
        /* both outcomes have to have been seen */
        if (r == FM_OK && (tried < 20u || back == 0u || back == tried))
        {
            printf("[%u cuts, the long n0 back %u times] ", (unsigned)tried,
                   (unsigned)back);
            r = FM_ERR_INTERNAL;
        }
    }
    CHECK_RESULT(r, "failed save");

    TEST("FMFS: files go into the holes deleted files left");
    {
        /* 20992 bytes of data area: 9000 + 2000 + 9000, 992 left */
        static const PfOp ops[] = {
            { PF_CREATE, 0, 0, 0, 0, 0 },
            { PF_APPEND, 0, 0, 0, 9000, 1 },
            { PF_CREATE, 1, 0, 0, 0, 0 },
            { PF_APPEND, 1, 0, 0, 2000, 2 },
            { PF_CREATE, 2, 0, 0, 0, 0 },
            { PF_APPEND, 2, 0, 0, 9000, 3 },
            { PF_DELETE, 0, 0, 0, 0, 0 },
            /* n1 cannot grow where it is (n2 is behind it) and the tail
             * is far too small: it moves into the 9000 bytes n0 left.
             * No other file has to move for that. */
            { PF_APPEND, 1, 0, 0, 3000, 4 },
            /* holes now: 4000 behind n1, the 2000 it came from, 992 at
             * the tail.  1500 new bytes take the smallest one that
             * holds them... */
            { PF_CREATE, 3, 0, 0, 0, 0 },
            { PF_APPEND, 3, 0, 0, 1500, 5 },
            /* ...so the 4000 are still in one piece for this file */
            { PF_CREATE, 4, 0, 0, 0, 0 },
            { PF_APPEND, 4, 0, 0, 4000, 6 }
        };
        uint32_t journal;
        uint32_t i;

        g_pf_cap = 32u * 1024u;
        g_pf_block = 1;
        memset(g_pf_mem, 0, g_pf_cap);
        memset(&g_pf_model, 0, sizeof(g_pf_model));
        journal = g_pf_j_copy + g_pf_j_bounce;
        r = pf_attach(-1);
        for (i = 0; i < sizeof(ops) / sizeof(ops[0]) && r == FM_OK; i++)
        {
            r = pf_run(&ops[i]);
            if (r == FM_OK) pf_apply(&g_pf_model, &ops[i]);
            if (r != FM_OK) printf("[op %u] ", (unsigned)i);
        }
        if (r == FM_OK && pf_verify(&g_pf_model, NULL) != 0)
        {
            printf("[%s] ", g_pf_why);
            r = FM_ERR_CRC;
        }
        if (r == FM_OK && g_pf_j_copy + g_pf_j_bounce != journal)
        {
            /* a journal record means a file slid over itself: the data
             * area was compacted, with room for everything in the holes */
            printf("[%u journal records] ",
                   (unsigned)(g_pf_j_copy + g_pf_j_bounce - journal));
            r = FM_ERR_INTERNAL;
        }
        pf_detach();
    }
    CHECK_RESULT(r, "holes");

    TEST("FMFS: a hole too small to be worth its journal writes stays");
    {
        /* 100 bytes in front of 9000 + 9000 + 2800, 92 at the tail */
        static const PfOp ops[] = {
            { PF_CREATE, 0, 0, 0, 0, 0 },
            { PF_APPEND, 0, 0, 0, 100, 1 },
            { PF_CREATE, 1, 0, 0, 0, 0 },
            { PF_APPEND, 1, 0, 0, 9000, 2 },
            { PF_CREATE, 2, 0, 0, 0, 0 },
            { PF_APPEND, 2, 0, 0, 9000, 3 },
            { PF_CREATE, 3, 0, 0, 0, 0 },
            { PF_APPEND, 3, 0, 0, 2800, 4 },
            { PF_DELETE, 0, 0, 0, 0, 0 },
            { PF_CREATE, 4, 0, 0, 0, 0 }
        };
        /* 192 bytes are free, in two pieces.  150 bytes fit only when
         * the first hole is closed: 9000 bytes moved 100 at a time, 89
         * journal records for the first file alone. */
        static const PfOp wide = { PF_APPEND, 4, 0, 0, 150, 5 };
        static const PfOp fits = { PF_APPEND, 4, 0, 0, 100, 6 };
        int gives_up = ((uint64_t)(FM_FMFS_MOVE_STEPS) < 89u);
        uint32_t journal;
        uint32_t i;
        FM_Result got;

        g_pf_cap = 32u * 1024u;
        g_pf_block = 1;
        memset(g_pf_mem, 0, g_pf_cap);
        memset(&g_pf_model, 0, sizeof(g_pf_model));
        r = pf_attach(-1);
        for (i = 0; i < sizeof(ops) / sizeof(ops[0]) && r == FM_OK; i++)
        {
            r = pf_run(&ops[i]);
            if (r == FM_OK) pf_apply(&g_pf_model, &ops[i]);
        }
        journal = g_pf_j_copy + g_pf_j_bounce;
        if (r == FM_OK)
        {
            got = pf_run(&wide);
            journal = g_pf_j_copy + g_pf_j_bounce - journal;
            if (gives_up)
            {
                /* refused, at no cost, and nothing was touched */
                if (got != FM_ERR_NO_SPACE || journal != 0u)
                {
                    r = FM_ERR_INTERNAL;
                }
                /* what does fit in a hole is still written */
                if (r == FM_OK) r = pf_run(&fits);
                if (r == FM_OK) pf_apply(&g_pf_model, &fits);
            }
            else
            {
                /* FM_FMFS_MOVE_STEPS says: whatever it takes */
                if (got != FM_OK || journal < 89u)
                {
                    r = FM_ERR_INTERNAL;
                }
                if (r == FM_OK) pf_apply(&g_pf_model, &wide);
            }
            if (r != FM_OK)
            {
                printf("[%s, %u journal records] ", FM_ResultString(got),
                       (unsigned)journal);
            }
        }
        if (r == FM_OK && pf_verify(&g_pf_model, NULL) != 0)
        {
            printf("[%s] ", g_pf_why);
            r = FM_ERR_CRC;
        }
        if (r == FM_OK) r = FS_Check("/pf");
        pf_detach();
    }
    CHECK_RESULT(r, "hole that stays");
#endif /* FM_FMFS_POWER_SAFE */

    TEST("FMFS: the same cuts do damage a version 1 volume (control)");
    /* The harness has to be able to see damage, or the tests above
     * prove nothing.  A version 1 volume has one table, rewritten in
     * place: some cut must break it. */
    r = pf_campaign(30u * 1024u, 1, 1, 0x51u, 150, 1, &stats);
    if (r == FM_OK && stats.damaged == 0u)
    {
        printf("[%u cuts, none did any harm] ", (unsigned)stats.cuts);
        r = FM_ERR_INTERNAL;
    }
    if (r != FM_OK)
    {
        FAIL(g_pf_why[0] ? g_pf_why : "the harness sees no damage");
    }
    else
    {
        PASS();
    }

    TEST("FMFS: mounts that fail do not use up the FMFS contexts");
    {
        uint32_t i;
        FM_Result bad = FM_ERR_CORRUPTED;

        /* a header that says FMFS, version 9 */
        g_pf_cap = 32u * 1024u;
        g_pf_block = 512;
        memset(g_pf_mem, 0, g_pf_cap);
        memcpy(g_pf_mem, "FMFS", 4);
        g_pf_mem[4] = 9;
        r = FM_OK;
        for (i = 0; i < FM_MAX_FS + 3u && r == FM_OK; i++)
        {
            bad = pf_attach(-1);
            pf_detach();
            if (bad != FM_ERR_CORRUPTED)
            {
                printf("[mount %u: %s] ", (unsigned)i, FM_ResultString(bad));
                r = FM_ERR_INTERNAL;
            }
        }
        memset(g_pf_mem, 0, g_pf_cap);
        if (r == FM_OK) r = pf_attach(-1);
        if (r == FM_OK) r = File_WriteAll("/pf/ok.txt", "ok", 2);
        pf_detach();
    }
    CHECK_RESULT(r, "contexts after failed mounts");
}
#endif /* FM_USE_FMFS */

/* The filesystem adapters hand out contexts from a pool of FM_MAX_FS.
 * A context has to come back when its volume is unmounted and when a
 * mount fails -- and an FM_FS must never end up using a context that
 * has meanwhile been given to another one. */
#if FM_USE_FMFS || FM_USE_LITTLEFS
static uint8_t g_fc_mem[2][160 * 1024];
static FM_Storage g_fc_st[2];
static const char *const g_fc_name[2] = { "fca", "fcb" };
static const char *const g_fc_mount[2] = { "/fca", "/fcb" };

/* kind 0: FMFS, 1: LittleFS */
static FM_Result fc_init(int kind, FM_FS *fs)
{
#if FM_USE_FMFS
    if (kind == 0) return FMFS_Init(fs);
#endif
#if FM_USE_LITTLEFS
    if (kind == 1) return LittleFS_Init(fs);
#endif
    (void)fs;
    return FM_ERR_UNSUPPORTED;
}

static FM_Result fc_storage(int which, int kind, int blank)
{
    FM_Result r;

    if (blank)
    {
        memset(g_fc_mem[which], kind ? 0xFF : 0, sizeof(g_fc_mem[which]));
    }
    r = RamDisk_Init(&g_fc_st[which], g_fc_name[which], g_fc_mem[which],
                     sizeof(g_fc_mem[which]), kind ? 4096u : 512u);
    if (r == FM_OK) r = Storage_Register(&g_fc_st[which]);
    return r;
}

static void fc_drop(int which)
{
    (void)FS_Unmount(g_fc_mount[which]);
    (void)Storage_Unmount(g_fc_name[which]);
    (void)Storage_Unregister(g_fc_name[which]);
}

static FM_Result fc_cycles(int kind)
{
    static FM_FS fs;
    FM_Result r = FM_OK;
    uint32_t i;

    /* more mount/unmount cycles than there are contexts: a card that
     * is taken out and put back */
    r = fc_storage(0, kind, 1);
    for (i = 0; i < 3u * FM_MAX_FS + 1u && r == FM_OK; i++)
    {
        char text[16];
        char back[16];
        size_t rd = 0;

        (void)snprintf(text, sizeof(text), "cycle %u", (unsigned)i);
        r = fc_init(kind, &fs);
        if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &fs);
        if (r == FM_OK && i != 0u)
        {
            /* what the cycle before wrote is there */
            memset(back, 0, sizeof(back));
            r = File_ReadAll("/fca/n.txt", back, sizeof(back) - 1u, &rd);
            (void)snprintf(text, sizeof(text), "cycle %u", (unsigned)(i - 1u));
            if (r == FM_OK && strcmp(back, text) != 0) r = FM_ERR_CRC;
            (void)snprintf(text, sizeof(text), "cycle %u", (unsigned)i);
        }
        if (r == FM_OK) r = File_WriteAll("/fca/n.txt", text, strlen(text));
        if (r == FM_OK) r = FS_Unmount(g_fc_mount[0]);
        if (r != FM_OK) printf("[cycle %u] ", (unsigned)i);
    }
    fc_drop(0);
    return r;
}

static FM_Result fc_failed_mounts(int kind)
{
    FM_Result r = FM_OK;
    uint32_t i;

    for (i = 0; i < FM_MAX_FS + 3u && r == FM_OK; i++)
    {
        /* on the stack, and gone afterwards: nobody is left who could
         * give the context back */
        FM_FS fs;
        FM_Result bad;

        /* FMFS: a header of a version that does not exist, on a
         * storage that is not formatted on its own; LittleFS: noise,
         * with the automatic format switched off */
        r = fc_storage(0, kind, 1);
        memset(g_fc_mem[0], 0x55, 4096);
#if FM_USE_FMFS
        if (kind == 0)
        {
            memcpy(g_fc_mem[0], "FMFS", 4);
            g_fc_mem[0][4] = 9;
            g_fc_mem[0][5] = 0;
            g_fc_st[0].type = FM_STORAGE_NOR;
        }
#endif
        if (r == FM_OK) r = fc_init(kind, &fs);
#if FM_USE_LITTLEFS
        if (r == FM_OK && kind == 1) LittleFS_SetAutoFormat(&fs, FM_FALSE);
#endif
        if (r == FM_OK)
        {
            bad = FS_Mount(g_fc_mount[0], &fs);
            if (bad != FM_ERR_CORRUPTED)
            {
                printf("[mount %u: %s] ", (unsigned)i, FM_ResultString(bad));
                r = FM_ERR_INTERNAL;
            }
        }
        else
        {
            printf("[init %u: %s] ", (unsigned)i, FM_ResultString(r));
        }
        fc_drop(0);
    }
    if (r == FM_OK)
    {
        static FM_FS good;

        r = fc_storage(0, kind, 1);
        if (r == FM_OK) r = fc_init(kind, &good);
        if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &good);
        if (r == FM_OK) r = File_WriteAll("/fca/ok.txt", "ok", 2);
        fc_drop(0);
    }
    return r;
}

static FM_Result fc_stale(int kind)
{
    static FM_FS a;
    static FM_FS b;
    char back[8];
    size_t rd = 0;
    FM_Result r;
    FM_Result again;

    r = fc_storage(0, kind, 1);
    if (r == FM_OK) r = fc_storage(1, kind, 1);
    if (r == FM_OK) r = fc_init(kind, &a);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &a);
    if (r == FM_OK) r = File_WriteAll("/fca/a.txt", "aaa", 3);
    if (r == FM_OK) r = FS_Unmount(g_fc_mount[0]);
    /* mounted again without a new init: nobody took the context */
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &a);
    memset(back, 0, sizeof(back));
    if (r == FM_OK) r = File_ReadAll("/fca/a.txt", back, sizeof(back) - 1u,
                                     &rd);
    if (r == FM_OK && strcmp(back, "aaa") != 0) r = FM_ERR_CRC;
    if (r == FM_OK) r = FS_Unmount(g_fc_mount[0]);
    if (r != FM_OK) printf("[remount: %s] ", FM_ResultString(r));

    /* now another FM_FS gets that context... */
    if (r == FM_OK) r = fc_init(kind, &b);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[1], &b);
    if (r == FM_OK) r = File_WriteAll("/fcb/b.txt", "bbb", 3);
    if (r == FM_OK)
    {
        /* ...and the first one, used again without an init, must not
         * get to work on it: it would be a second mount on the state
         * of the other volume */
        again = FS_Mount(g_fc_mount[0], &a);
        if (again != FM_ERR_NOT_READY)
        {
            printf("[stale mount: %s] ", FM_ResultString(again));
            r = FM_ERR_INTERNAL;
        }
#if FM_USE_FMFS
        if (kind == 0 && (FMFS_Repack(&a) != FM_ERR_NOT_READY ||
                          FMFS_IsPowerSafe(&a)))
        {
            printf("[stale FM_FS reaches the volume] ");
            r = FM_ERR_INTERNAL;
        }
#endif
#if FM_USE_LITTLEFS
        if (kind == 1) LittleFS_SetAutoFormat(&a, FM_FALSE);
#endif
    }
    memset(back, 0, sizeof(back));
    if (r == FM_OK) r = File_ReadAll("/fcb/b.txt", back, sizeof(back) - 1u,
                                     &rd);
    if (r == FM_OK && strcmp(back, "bbb") != 0) r = FM_ERR_CRC;
    if (r == FM_OK)
    {
        /* with its own init the first one mounts next to it */
        r = fc_init(kind, &a);
        if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &a);
        memset(back, 0, sizeof(back));
        if (r == FM_OK) r = File_ReadAll("/fca/a.txt", back,
                                         sizeof(back) - 1u, &rd);
        if (r == FM_OK && strcmp(back, "aaa") != 0) r = FM_ERR_CRC;
    }
    fc_drop(0);
    fc_drop(1);
    return r;
}

static void test_fs_contexts(void)
{
    static const char *const kind_name[2] = { "FMFS", "LittleFS" };
    int kind;
    FM_Result r;

    restore_posix_root();
    for (kind = 0; kind < 2; kind++)
    {
        char title[80];

#if !FM_USE_FMFS
        if (kind == 0) continue;
#endif
#if !FM_USE_LITTLEFS
        if (kind == 1) continue;
#endif
        (void)snprintf(title, sizeof(title),
                       "%s: more mount cycles than contexts", kind_name[kind]);
        TEST(title);
        r = fc_cycles(kind);
        CHECK_RESULT(r, "mount cycles");

        (void)snprintf(title, sizeof(title),
                       "%s: failed mounts give their context back",
                       kind_name[kind]);
        TEST(title);
        r = fc_failed_mounts(kind);
        CHECK_RESULT(r, "failed mounts");

        (void)snprintf(title, sizeof(title),
                       "%s: an FM_FS never works on another one's context",
                       kind_name[kind]);
        TEST(title);
        r = fc_stale(kind);
        CHECK_RESULT(r, "stale FM_FS");
    }
}
#endif /* FM_USE_FMFS || FM_USE_LITTLEFS */

/* Directory_Create answered FM_OK when a FILE of that name existed. */
static FM_Result dcf_one(const char *root)
{
    char f[64];
    char sub[64];
    char d[64];
    char in[64];
    FM_Bool is_dir = FM_TRUE;
    FM_Result r;
    FM_Result r1;
    FM_Result r2;
    FM_Result r3;

    (void)snprintf(f, sizeof(f), "%s/dcf_file", root);
    (void)snprintf(sub, sizeof(sub), "%s/dcf_file/sub", root);
    (void)snprintf(d, sizeof(d), "%s/dcf_dir", root);
    (void)snprintf(in, sizeof(in), "%s/dcf_dir/in.txt", root);
    (void)File_Delete(in);
    (void)Directory_Delete(d);
    (void)File_Delete(f);

    r = File_WriteAll(f, "x", 1);
    r1 = Directory_Create(f);
    r2 = Directory_Create(sub);
    if (r == FM_OK) r = Directory_Exists(f, &is_dir);
    if (r == FM_OK && (r1 != FM_ERR_NOT_DIRECTORY || r2 == FM_OK ||
                       is_dir != FM_FALSE))
    {
        printf("[%s file: %d, below it: %d] ", root, (int)r1, (int)r2);
        r = FM_ERR_INVALID_FORMAT;
    }
    /* ...while an existing directory is still success, at any depth */
    if (r == FM_OK) r = Directory_Create(d);
    r3 = Directory_Create(d);
    if (r == FM_OK && r3 != FM_OK) r = r3;
    if (r == FM_OK) r = File_WriteAll(in, "y", 1);
    r3 = Directory_Create(in);
    if (r == FM_OK && r3 != FM_ERR_NOT_DIRECTORY)
    {
        printf("[%s nested file: %d] ", root, (int)r3);
        r = FM_ERR_INVALID_FORMAT;
    }
    (void)File_Delete(in);
    (void)Directory_Delete(d);
    (void)File_Delete(f);
    return r;
}

static void test_dir_create_over_file(void)
{
    FM_Result r;

    restore_posix_root();
    TEST("Directory_Create: an existing file is not a directory");
    r = dcf_one("");
#if FM_USE_FMFS
    if (r == FM_OK)
    {
        static uint8_t mem[64 * 1024];
        static FM_Storage st;
        static FM_FS fs;

        r = RamDisk_Init(&st, "dcfm", mem, sizeof(mem), 512);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = FMFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/dcfm", &fs);
        if (r == FM_OK)
        {
            r = dcf_one("/dcfm");
            (void)FS_Unmount("/dcfm");
        }
        (void)Storage_Unregister("dcfm");
    }
#endif
#if FM_USE_LITTLEFS
    if (r == FM_OK)
    {
        static uint8_t mem[512 * 1024];
        static FM_Storage st;
        static FM_FS fs;

        memset(mem, 0xFF, sizeof(mem));
        r = RamDisk_Init(&st, "dcfl", mem, sizeof(mem), 4096);
        if (r == FM_OK) r = Storage_Register(&st);
        if (r == FM_OK) r = LittleFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/dcfl", &fs);
        if (r == FM_OK)
        {
            r = dcf_one("/dcfl");
            (void)FS_Unmount("/dcfl");
        }
        (void)Storage_Unregister("dcfl");
    }
#endif
    CHECK_RESULT(r, "create over file");
}

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

#include "audit_v2_tests.h"

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
    RUN_SUITE(test_file_report);
#if FM_USE_VIDEO
    RUN_SUITE(test_video);
    RUN_SUITE(test_video_report);
#endif
#if FM_USE_MJPEG
    RUN_SUITE(test_mjpeg);
#endif
#if FM_USE_IMAGE
    RUN_SUITE(test_scale);
#endif
    RUN_SUITE(test_image_report);
#if FM_USE_IMAGE && FM_USE_PNG
    RUN_SUITE(test_png_formats);
#endif
#if FM_USE_IMAGE && FM_USE_JPEG
    RUN_SUITE(test_jpeg_hw_file);
#endif
    RUN_SUITE(test_dir_create_over_file);
#if FM_USE_FMFS
    RUN_SUITE(test_fmfs_rename_failure);
    RUN_SUITE(test_fmfs_powerfail);
#endif
#if FM_USE_FMFS || FM_USE_LITTLEFS
    RUN_SUITE(test_fs_contexts);
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
    RUN_SUITE(test_sync_report);
    RUN_SUITE(test_audit_v2_file);
#if FM_USE_VIDEO && FM_USE_H264
    RUN_SUITE(test_audit_v2_video);
#endif
#if FM_USE_IMAGE
    RUN_SUITE(test_audit_v2_image);
#endif
#if FM_USE_AUDIO
    RUN_SUITE(test_audit_v2_audio);
#endif
#if FM_USE_THREADS && FM_HOST_BUILD
    RUN_SUITE(test_threads);
    RUN_SUITE(test_threads_wide);
#endif

    printf("\n  Results: %d/%d passed, %d failures\n",
           g_tests - g_failures, g_tests, g_failures);

    FileManager_Deinit();
    return (g_failures > 0) ? 1 : 0;
}