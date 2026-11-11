/*
 * Host runner for examples/examples_sdcard_media.h — creates two
 * fake media files (a small "MP3" and an AVI-header file) in the mock
 * filesystem, then runs the five media examples exactly like the board.
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include "../examples/examples_sdcard.h"
#include "../examples/examples_sdcard_media.h"
#include "../examples/media_data_io.h"

static int test_media_data_io(void)
{
    static int16_t mono[32];
    static int16_t stereo[32 * 2];
    static uint8_t frame[16 * 16 * 3];
    static uint8_t frame_rgb_work[16 * 16 * 3];
    static uint8_t frame_bgr565[16 * 16 * 2];
    static uint8_t image_rgb[2 * 2 * 3] = {
        255, 0, 0, 0, 255, 0,
        0, 0, 255, 255, 255, 255
    };
    static uint8_t image_loaded[2 * 2 * 3];
    static uint8_t image_gray_work[2 * 2];
    static uint8_t image_scaled[4 * 4 * 3];
    Ex14_AudioReader audio;
    Ex14_VideoReader video;
    FM_VideoWriter video_writer;
    FM_Image image;
    size_t count = 0;
    FM_Result r;

    r = Ex14_AudioOpen(&audio, "/tone.wav");
    if (r == FM_OK) r = Ex14_AudioReadMono(&audio, mono, 32, &count);
    if (r == FM_OK && count == 32) r = Ex14_AudioSeek(&audio, 0.0);
    if (r == FM_OK) r = Ex14_AudioReadStereo(&audio, stereo, 32, &count);
    if (r == FM_OK && (count != 32 || stereo[0] != stereo[1]))
        r = FM_ERR_CORRUPTED;
    if (r == FM_OK)
        r = Ex14_AudioSavePCM16("/processed.wav", stereo, count,
                                audio.info.sample_rate, 2);
    if (audio.opened) (void)Ex14_AudioClose(&audio);
    if (r != FM_OK)
    {
        printf("media data audio FAILED: %s (%d)\n",
               File_GetErrorString(r), (int)r);
        return 1;
    }

    r = Ex14_VideoOpen(&video, "/clip.fmrv");
    if (r == FM_OK) r = Ex14_VideoReadRGB888(&video, frame, sizeof(frame));
    if (r == FM_OK) r = Video_Seek(&video.handle, 0);
    if (r == FM_OK)
        r = Ex14_VideoReadAs(&video, FM_PIXEL_BGR565,
                             frame_bgr565, sizeof(frame_bgr565),
                             frame_rgb_work, sizeof(frame_rgb_work));
    if (video.opened) (void)Ex14_VideoClose(&video);
    if (r == FM_OK && frame_bgr565[2] == 0 && frame_bgr565[3] == 0)
        r = FM_ERR_CORRUPTED;
    if (r == FM_OK)
        r = Ex14_VideoWriterBeginRGB888(&video_writer, "/processed.fmrv",
                                        16, 16, 10, 1);
    if (r == FM_OK)
        r = Ex14_VideoWriterWriteRGB888(&video_writer, frame, sizeof(frame));
    if (r == FM_OK) r = Ex14_VideoWriterEnd(&video_writer);
    if (r != FM_OK)
    {
        printf("media data video FAILED: %s (%d)\n",
               File_GetErrorString(r), (int)r);
        return 1;
    }

    r = Ex14_ImageSaveRGB888("/image_input.bmp", image_rgb, 2, 2);
    if (r == FM_OK)
        r = Ex14_ImageLoadRGB888("/image_input.bmp", &image,
                                 image_gray_work, sizeof(image_gray_work),
                                 image_loaded, sizeof(image_loaded));
    if (r == FM_OK)
        r = Image_ApplyEffect(&image, FM_IMAGE_EFFECT_SEPIA, 0);
    if (r == FM_OK)
        r = Ex14_ImageResizeRGB888(image.data, image.width, image.height,
                                   image_scaled, sizeof(image_scaled),
                                   4, 4, FM_SCALE_NEAREST);
    if (r == FM_OK)
        r = Ex14_ImageSaveRGB888("/image_processed.bmp", image_scaled, 4, 4);
    if (r != FM_OK)
    {
        printf("media data image FAILED: %s (%d)\n",
               File_GetErrorString(r), (int)r);
        return 1;
    }

    printf("media data I/O: PCM16 + RGB/BGR video + image effects OK\n");
    return 0;
}

static void make_fake_media(void)
{
    static uint8_t mp3[2048];
    static uint8_t avi[3000];
    uint32_t i;

    Directory_Create("/music");

    /* fake MP3: frame-sync header + pseudo-random payload */
    mp3[0] = 0xFF; mp3[1] = 0xFB; mp3[2] = 0x90; mp3[3] = 0x00;
    for (i = 4; i < sizeof(mp3); i++) mp3[i] = (uint8_t)(i * 7u + 3u);
    (void)File_WriteAll("/music/song.mp3", mp3, sizeof(mp3));

    /* fake AVI: RIFF header + payload */
    avi[0] = 'R'; avi[1] = 'I'; avi[2] = 'F'; avi[3] = 'F';
    avi[4] = (uint8_t)(sizeof(avi)); avi[5] = 0; avi[6] = 0x0B; avi[7] = 0;
    avi[8] = 'A'; avi[9] = 'V'; avi[10] = 'I'; avi[11] = ' ';
    for (i = 12; i < sizeof(avi); i++) avi[i] = (uint8_t)(i * 13u + 5u);
    (void)File_WriteAll("/clip_fast.avi", avi, sizeof(avi));
}

int main(void)
{
    /* The examples print through FM_Log (as on the board, where that is
     * the UART).  Without a sink every line -- the "!!" of a step that
     * failed included -- went nowhere and this runner could not fail. */
    FM_Log_UseStdout();

    if (SD_examples_begin() != FM_OK)
    {
        printf("SD_examples_begin FAILED\n");
        return 1;
    }

    make_fake_media();

    exM_1_audio_wav();
    exM_2_video_fmrv();
    exM_3_copy_song();
    exM_4_copy_video();
    exM_5_media_detect();
    if (test_media_data_io() != 0)
    {
        SD_examples_end();
        return 1;
    }

    SD_examples_end();
    printf("\n===== ALL MEDIA EXAMPLES DONE OK =====\n");
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
