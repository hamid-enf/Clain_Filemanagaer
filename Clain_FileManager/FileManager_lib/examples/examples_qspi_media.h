/*
 * QSPI Media examples — Audio / Video / Image روی /qspi
 *
 * پیش‌نیاز:
 *   QSPI_examples_begin() از examples_qspi.h موفق شده باشد.
 *   این مثال‌ها mount را نمی‌بندند؛ در پایان QSPI_examples_end() را caller
 *   صدا می‌زند.
 *
 * فایل‌های اختیاری که می‌توانید از USB روی QSPI کپی کنید:
 *   /qspi/input.mp3  یا /qspi/input.wav
 *   /qspi/input.avi  یا /qspi/input.mp4
 *
 * اگر فایل ورودی وجود نداشته باشد، مثال یک WAV/FMRV/BMP کوچک می‌سازد تا
 * مسیر کامل بدون فایل خارجی هم قابل آزمایش باشد.
 *
 *   if (QSPI_examples_begin() == FM_OK) {
 *       exQM_1_audio_detect_process_save();
 *       exQM_2_video_detect_to_bgr565();
 *       exQM_3_image_bgr_effect_resize_save();
 *       QSPI_examples_end();
 *   }
 */

#ifndef EXAMPLES_FILE_QSPI_MEDIA_H_
#define EXAMPLES_FILE_QSPI_MEDIA_H_

#if !defined(FM_HOST_BUILD) || (FM_HOST_BUILD == 0)

#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
#include "fm_video.h"
#include "fm_image.h"
#include "fm_media.h"
#include "fm_log.h"
#include "media_data_io.h"

#define printf FM_Log

static FM_Bool exqm_is_mounted(void)
{
    FM_Bool mounted = FM_FALSE;

    if (FS_IsMounted("/qspi", &mounted) != FM_OK || !mounted)
    {
        printf("QSPI Media: first call QSPI_examples_begin()\n");
        return FM_FALSE;
    }
    return FM_TRUE;
}

static const char *exqm_audio_input(void)
{
    FM_Bool exists = FM_FALSE;

    if (File_Exists("/qspi/input.mp3", &exists) == FM_OK && exists)
        return "/qspi/input.mp3";
    exists = FM_FALSE;
    if (File_Exists("/qspi/input.wav", &exists) == FM_OK && exists)
        return "/qspi/input.wav";
    return NULL;
}

/* Audio_Open نوع WAV/MP3 را از bytes تشخیص می‌دهد و هر دو را PCM16 می‌کند. */
void exQM_1_audio_detect_process_save(void)
{
    static int16_t pcm[256 * 2];
    Ex14_AudioReader reader;
    const char *input;
    size_t frames = 0;
    FM_Result r;
    uint32_t i;

    if (!exqm_is_mounted()) return;
    input = exqm_audio_input();
    if (input == NULL)
    {
        /* فقط برای ساخت fixture: 256 frame mono PCM16 -> WAV */
        for (i = 0; i < 256; i++) pcm[i] = (int16_t)((int32_t)i * 120 - 15000);
        r = Ex14_AudioSavePCM16("/qspi/input.wav", pcm, 256, 16000, 1);
        if (r != FM_OK)
        {
            printf("audio fixture failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            return;
        }
        input = "/qspi/input.wav";
    }

    memset(&reader, 0, sizeof(reader));
    r = Ex14_AudioOpen(&reader, input);
    if (r == FM_OK)
    {
        printf("audio detected: codec=%s rate=%lu channels=%d\n",
               reader.info.codec, (unsigned long)reader.info.sample_rate,
               (int)reader.info.channels);
        r = Ex14_AudioReadStereo(&reader, pcm, 256, &frames);
    }
    if (r == FM_OK)
    {
        for (i = 0; i < frames * 2u; i++) pcm[i] = (int16_t)(pcm[i] / 2);
        r = Ex14_AudioSavePCM16("/qspi/audio_processed.wav", pcm, frames,
                                reader.info.sample_rate, 2);
    }
    if (reader.opened) (void)Ex14_AudioClose(&reader);
    printf("audio process/save: %s (%d), frames=%lu\n",
           File_GetErrorString(r), (int)r, (unsigned long)frames);
}

static const char *exqm_video_input(void)
{
    FM_Bool exists = FM_FALSE;

    if (File_Exists("/qspi/input.avi", &exists) == FM_OK && exists)
        return "/qspi/input.avi";
    exists = FM_FALSE;
    if (File_Exists("/qspi/input.mp4", &exists) == FM_OK && exists)
        return "/qspi/input.mp4";
    return "/qspi/input.fmrv";
}

/* Container از محتوا تشخیص داده می‌شود؛ layout خروجی مستقل و BGR565 است. */
void exQM_2_video_detect_to_bgr565(void)
{
    static uint8_t rgb_work[16 * 16 * 3];
    static uint8_t bgr565[16 * 16 * 2];
    Ex14_VideoReader reader;
    FM_VideoWriter writer;
    const char *input;
    FM_Bool exists = FM_FALSE;
    FM_Result r;
    uint32_t i;

    if (!exqm_is_mounted()) return;
    input = exqm_video_input();
    (void)File_Exists(input, &exists);
    if (!exists)
    {
        memset(&writer, 0, sizeof(writer));
        for (i = 0; i < 16u * 16u; i++)
        {
            rgb_work[i * 3u] = (uint8_t)(i & 0xFFu);
            rgb_work[i * 3u + 1u] = (uint8_t)((i * 3u) & 0xFFu);
            rgb_work[i * 3u + 2u] = 80;
        }
        r = Ex14_VideoWriterBeginRGB888(&writer, input, 16, 16, 10, 1);
        if (r == FM_OK)
            r = Ex14_VideoWriterWriteRGB888(&writer, rgb_work,
                                             sizeof(rgb_work));
        if (r == FM_OK) r = Ex14_VideoWriterEnd(&writer);
        if (r != FM_OK)
        {
            if (File_IsOpen(&writer.file))
                (void)Ex14_VideoWriterEnd(&writer);
            printf("video fixture failed: %s (%d)\n",
                   File_GetErrorString(r), (int)r);
            return;
        }
    }

    memset(&reader, 0, sizeof(reader));
    r = Ex14_VideoOpen(&reader, input);
    if (r == FM_OK)
    {
        printf("video detected: container=%s codec=%d %dx%d\n",
               reader.info.container, (int)reader.info.codec,
               (int)reader.info.width, (int)reader.info.height);
        if (reader.info.width != 16 || reader.info.height != 16)
        {
            /* این بافر نمونه 16x16 است؛ در برنامه بافر را از info اندازه دهید. */
            r = FM_ERR_TOO_LARGE;
        }
    }
    if (r == FM_OK)
        r = Ex14_VideoReadAs(&reader, FM_PIXEL_BGR565,
                             bgr565, sizeof(bgr565),
                             rgb_work, sizeof(rgb_work));
    if (reader.opened) (void)Ex14_VideoClose(&reader);
    printf("video BGR565 frame: %s (%d), bytes=%lu\n",
           File_GetErrorString(r), (int)r,
           (unsigned long)sizeof(bgr565));
}

void exQM_3_image_bgr_effect_resize_save(void)
{
    static uint8_t rgb_source[4 * 4 * 3];
    static uint8_t gray_work[4 * 4];
    static uint8_t rgb_work[4 * 4 * 3];
    static uint8_t bgr[4 * 4 * 3];
    static uint8_t bgr_scaled[8 * 8 * 3];
    FM_Image image;
    FM_Result r;
    uint32_t i;

    if (!exqm_is_mounted()) return;
    for (i = 0; i < 16; i++)
    {
        rgb_source[i * 3u] = (uint8_t)(i * 12u);
        rgb_source[i * 3u + 1u] = (uint8_t)(255u - i * 10u);
        rgb_source[i * 3u + 2u] = 60;
    }
    r = Ex14_ImageSaveBMP("/qspi/image_input.bmp", rgb_source, 4, 4,
                          FM_PIXEL_RGB888);
    if (r == FM_OK)
        r = Ex14_ImageLoadAs("/qspi/image_input.bmp", &image,
                             FM_PIXEL_BGR888,
                             gray_work, sizeof(gray_work),
                             rgb_work, sizeof(rgb_work),
                             bgr, sizeof(bgr));
    if (r == FM_OK)
        r = Image_ApplyEffect(&image, FM_IMAGE_EFFECT_SEPIA, 0);
    if (r == FM_OK)
        r = Ex14_ImageResize(bgr, 4, 4,
                             bgr_scaled, sizeof(bgr_scaled), 8, 8,
                             FM_PIXEL_BGR888, FM_SCALE_BILINEAR);
    if (r == FM_OK)
        r = Ex14_ImageSaveBMP("/qspi/image_processed.bmp",
                              bgr_scaled, 8, 8, FM_PIXEL_BGR888);
    printf("image BGR/effect/resize/save: %s (%d)\n",
           File_GetErrorString(r), (int)r);
}

/* دادهٔ از قبل encodeشده (مثلاً MP3 دریافتی از شبکه) بدون decode/re-encode
 * ذخیره می‌شود، ولی magic و پسوند قبل از write با هم validate می‌شوند. */
FM_Result exQM_SaveAlreadyEncoded(const char *path, const void *data,
                                  size_t size, FM_MediaType expected)
{
    FM_MediaType detected = FM_MEDIA_UNKNOWN;
    FM_Result r = Media_SaveEncoded(path, data, size, expected, &detected);

    printf("encoded save: detected=%s result=%s (%d)\n",
           Media_TypeName(detected), File_GetErrorString(r), (int)r);
    return r;
}

#endif /* target */
#endif /* EXAMPLES_FILE_QSPI_MEDIA_H_ */
