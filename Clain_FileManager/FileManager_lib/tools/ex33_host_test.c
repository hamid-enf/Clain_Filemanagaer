/*
 * ex33_host_test.c — host validation for examples/33_audio_record_with_tap.h
 *
 * Example 33 documents the "new" audio plumbing (pre/post PCM taps,
 * recording, video frame/audio access, folder copy).  Those APIs live
 * in header-only layers that also compile on the host with the HAL
 * stub, so this harness includes the very example header the board
 * uses and *executes* it against the real library:
 *
 *   - taps install and are invoked with editable buffers (+6 dB DSP)
 *   - SimpleAudio_RecStart/RecWrite/RecStop produce a playable WAV
 *   - Directory_Copy copies a nested tree with contents intact
 *   - SimpleVideo taps install; frame/audio getters behave when idle
 *
 *   gcc -Iinclude -Iexamples -Ibackends -Isrc -Itools/halstub \
 *       tools/ex33_host_test.c libfilemanager.a -lm -o ex33test
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "file_manager.h"
#include "fm_filesystem.h"

extern FM_Result PosixFS_Init(FM_FS *fs);

/* Pulls in simple_audio.h + simple_video.h + fm_directory.h and the
 * example's own static helpers (my_pre_dsp, my_post_tap, ...). */
#include "33_audio_record_with_tap.h"

static int fails;

/* The board's main.c owns the I2S handles; under the HAL stub the test
 * provides them so simple_audio.h / simple_i2s_mux.h link. */
I2S_HandleTypeDef hi2s1;
I2S_HandleTypeDef hi2s2;

static void ok(const char *what, int cond)
{
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void)
{
    FM_FS fs;
    FM_Result r;

    if (FileManager_Init() != FM_OK ||
        PosixFS_Init(&fs) != FM_OK ||
        FS_Mount("/", &fs) != FM_OK)
    {
        printf("FATAL: fs setup\n");
        return 1;
    }

    printf("Example 33 (audio record + taps) host test\n");
    printf("==========================================\n");

    /* ---- 1) taps install ------------------------------------------ */
    ex33_audio_tap_setup();
    ok("SimpleAudio_SetPrePcmTap installed",
       sa.dma.pcm_tap_pre != NULL);
    ok("SimpleAudio_SetPostPcmTap installed",
       sa.dma.pcm_tap != NULL);
    ok("SimpleAudio_SetTrackStart installed", sa.dma.on_track_start != NULL);
    ok("SimpleAudio_SetTrackEnd installed", sa.dma.on_track_end != NULL);

    /* ---- 2) the example's +6 dB DSP behaves (pure function) ------ */
    {
        int16_t buf[4] = {1000, 20000, -20000, -1000};
        int16_t exp[4] = {2000, 32767, -32768, -2000};
        size_t keep = rec_ring_len;

        my_pre_dsp(NULL, buf, 4, 48000, 2);
        ok("my_pre_dsp: +6 dB with saturation",
           buf[0] == exp[0] && buf[1] == exp[1] &&
           buf[2] == exp[2] && buf[3] == exp[3]);
        ok("my_pre_dsp: does not record when idle",
           rec_ring_len == keep);
    }

    /* ---- 3) video taps install; getters sane while idle ----------- */
    ex33_video_setup();
    ok("SimpleVideo_SetPrePcmTap installed", sv_pcm_tap_pre != NULL);
    ok("SimpleVideo_SetPostPcmTap installed", sv_pcm_tap_post != NULL);
    ok("SimpleVideo_SetTrackStartCallback installed",
       sv.track_start != NULL);
    ok("SimpleVideo_SetTrackEndCallback installed", sv.track_end != NULL);
    ok("SimpleVideo_SetFrameCallback installed", sv.frame_cb != NULL);
    {
        uint16_t w = 9, h = 9;
        size_t got = 99;
        FM_AudioTrackInfo ai;

        ok("SimpleVideo_GetFrame idle -> zero dims (no frame)",
           SimpleVideo_GetFrame(&w, &h) != NULL && w == 0 && h == 0);
        ok("SimpleVideo_ReadAudio idle -> not OK",
           SimpleVideo_ReadAudio((int16_t[8]){0}, 4, &got) != FM_OK);
        ok("SimpleVideo_GetAudioInfo idle -> not OK",
           SimpleVideo_GetAudioInfo(&ai) != FM_OK);
    }
    {
        int16_t v[2] = {1000, -1000};
        my_video_pre_tap(NULL, v, 2, 48000, 2);
        ok("my_video_pre_tap: -6 dB halving",
           v[0] == 500 && v[1] == -500);
    }

    /* ---- 4) record: RecStart/RecWrite/RecStop -> playable WAV ---- */
    {
        static int16_t tone[4800];
        size_t i;
        FM_Audio a;
        FM_AudioInfo info;
        size_t got = 0;
        unsigned long total = 0;
        int16_t chk[512];
        int peak = 0;

        (void)Directory_DeleteRecursive("/rec");
        for (i = 0; i < sizeof(tone) / sizeof(tone[0]); i++)
        {
            /* 440 Hz @16 kHz, 1/3 of full scale */
            double t = (double)i / 16000.0;
            tone[i] = (int16_t)(10000.0 *
                                sin(2.0 * 3.14159265358979 * 440.0 * t));
        }

        r = SimpleAudio_RecStart("/rec", 16000, 1);
        ok("SimpleAudio_RecStart(/rec, 16 kHz, mono)", r == FM_OK);
        if (r == FM_OK)
        {
            r = SimpleAudio_RecWrite(tone,
                                     sizeof(tone) / sizeof(tone[0]));
            ok("SimpleAudio_RecWrite(4800 frames)", r == FM_OK);
            r = SimpleAudio_RecStop();
            ok("SimpleAudio_RecStop()", r == FM_OK);
            ok("SimpleAudio_RecPath() names a wav",
               strstr(SimpleAudio_RecPath(), ".wav") != NULL);
        }

        r = (r == FM_OK) ? Audio_Open(SimpleAudio_RecPath(), &a)
                         : FM_ERR_INVALID_ARGUMENT;
        if (r == FM_OK) r = Audio_GetInfo(&a, &info);
        ok("recorded file reopens via Audio_Open", r == FM_OK);
        if (r == FM_OK)
        {
            ok("recorded rate/ch = 16000/1",
               info.sample_rate == 16000u && info.channels == 1u);
            for (;;)
            {
                r = Audio_Read(&a, chk, 512, &got);
                if (r != FM_OK || got == 0) break;
                total += got;
                for (i = 0; i < got; i++)
                {
                    int m = chk[i] < 0 ? -chk[i] : chk[i];
                    if (m > peak) peak = m;
                }
            }
            ok("recorded length round-trips (4800 frames)",
               total == 4800u);
            ok("recorded level round-trips (peak > 9000)",
               peak > 9000);
            (void)Audio_Close(&a);
        }
        (void)Directory_DeleteRecursive("/rec");
    }

    /* ---- 5) folder copy ------------------------------------------- */
    {
        char txt[64];
        FM_Bool exists = FM_FALSE;

        (void)Directory_DeleteRecursive("/ex33src");
        (void)Directory_DeleteRecursive("/ex33dst");
        (void)Directory_Create("/ex33src/a/b");
        (void)File_WriteText("/ex33src/a/hello.txt", "salam");
        (void)File_WriteText("/ex33src/a/b/deep.txt", "deep-salam");

        r = ex33_folder_copy_demo("/ex33src", "/ex33dst");
        ok("Directory_Copy nested /ex33src -> /ex33dst", r == FM_OK);
        if (r == FM_OK)
        {
            r = File_ReadText("/ex33dst/a/hello.txt", txt,
                              sizeof(txt), NULL);
            ok("copied shallow file content", r == FM_OK &&
               strcmp(txt, "salam") == 0);
            r = File_ReadText("/ex33dst/a/b/deep.txt", txt,
                              sizeof(txt), NULL);
            ok("copied nested file content", r == FM_OK &&
               strcmp(txt, "deep-salam") == 0);
        }
        (void)File_Exists("/ex33dst/a/hello.txt", &exists);
        ok("cleanup precondition sane", exists == FM_TRUE);
        (void)Directory_DeleteRecursive("/ex33src");
        (void)Directory_DeleteRecursive("/ex33dst");
    }

    printf("\nResults: %s\n", fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
