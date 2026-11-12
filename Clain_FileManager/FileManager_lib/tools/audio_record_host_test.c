/*
 * audio_record_host_test.c — host validation for examples/audio_record_with_tap.h
 *
 * audio_record_with_tap.h documents the "new" audio plumbing (pre/post PCM taps,
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
 *       tools/audio_record_host_test.c libfilemanager.a -lm -o audiorecordtest
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
 * example's own static helpers (audio_record_pre_dsp, audio_record_post_tap, ...). */
#include "audio_record_with_tap.h"

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

/* callbacks of section 6: count what reaches the application */
static int keep_data_calls, keep_monitor_calls, keep_starts, keep_ends;
static int keep_data_peak;

static void keep_data(void *ctx, int16_t *pcm, size_t samples,
                      uint32_t rate, uint16_t channels)
{
    size_t i;

    (void)ctx; (void)rate; (void)channels;
    keep_data_calls++;
    for (i = 0; i < samples; i++)
    {
        int m = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (m > keep_data_peak) keep_data_peak = m;
    }
}
static void keep_monitor(void *ctx, int16_t *pcm, size_t samples,
                         uint32_t rate, uint16_t channels)
{
    (void)ctx; (void)pcm; (void)samples; (void)rate; (void)channels;
    keep_monitor_calls++;
}
static void keep_start(void *ctx, const char *path, const FM_AudioInfo *info)
{
    (void)ctx; (void)path; (void)info;
    keep_starts++;
}
static void keep_end(void *ctx, const char *path, const FM_AudioInfo *info)
{
    (void)ctx; (void)path; (void)info;
    keep_ends++;
}
static int keep_dma_silent(void)
{
    size_t i;

    for (i = 0; i < 2u * (size_t)SIMPLE_AUDIO_HALF_SAMPLES; i++)
    {
        if (simple_audio_dma_buf[i] != 0) return 0;
    }
    return 1;
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

    printf("audio_record_with_tap.h (audio record + taps) host test\n");
    printf("==========================================\n");

    /* ---- 1) taps install ------------------------------------------ */
    audio_record_tap_setup();
    ok("SimpleAudio_SetDataCallback installed",
       sa.dma.pcm_tap_pre != NULL && sa_cfg.data_cb != NULL);
    ok("SimpleAudio_SetMonitorCallback installed",
       sa.dma.pcm_tap != NULL && sa_cfg.monitor_cb != NULL);
    ok("SimpleAudio_SetTrackStartCallback installed",
       sa.dma.on_track_start != NULL && sa_cfg.track_start_cb != NULL);
    ok("SimpleAudio_SetTrackEndCallback installed",
       sa.dma.on_track_end != NULL && sa_cfg.track_end_cb != NULL);
    ok("SimpleAudio output default = speaker + callback",
       SimpleAudio_GetOutput() == FM_AUDIO_OUT_BOTH);
    ok("SimpleAudio_SetOutput(callback) is kept",
       SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK) == FM_OK &&
       SimpleAudio_GetOutput() == FM_AUDIO_OUT_CALLBACK &&
       sa_cfg.output == FM_AUDIO_OUT_CALLBACK);
    ok("SimpleAudio_SetOutput rejects an unknown value",
       SimpleAudio_SetOutput((FM_AudioOutput)7) != FM_OK &&
       SimpleAudio_GetOutput() == FM_AUDIO_OUT_CALLBACK);
    (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
    ok("SimpleAudio_SetVolume 0..200, GetVolume",
       SimpleAudio_SetVolume(150) == FM_OK && SimpleAudio_GetVolume() == 150u &&
       SimpleAudio_SetVolume(250) == FM_OK && SimpleAudio_GetVolume() == 200u &&
       SimpleAudio_SetVolume(100) == FM_OK && SimpleAudio_GetVolume() == 100u);

    /* ---- 2) the example's +6 dB DSP behaves (pure function) ------ */
    {
        int16_t buf[4] = {1000, 20000, -20000, -1000};
        int16_t exp[4] = {2000, 32767, -32768, -2000};
        size_t keep = rec_ring_len;

        audio_record_pre_dsp(NULL, buf, 4, 48000, 2);
        ok("audio_record_pre_dsp: +6 dB with saturation",
           buf[0] == exp[0] && buf[1] == exp[1] &&
           buf[2] == exp[2] && buf[3] == exp[3]);
        ok("audio_record_pre_dsp: does not record when idle",
           rec_ring_len == keep);
    }

    /* ---- 3) video taps install; getters sane while idle ----------- */
    audio_record_video_setup();
    ok("SimpleVideo_SetAudioDataCallback installed", sv_pcm_tap_pre != NULL);
    ok("SimpleVideo_SetAudioMonitorCallback installed",
       sv_pcm_tap_post != NULL);
    ok("SimpleVideo audio output = speaker + callback",
       SimpleVideo_GetAudioOutput() == FM_AUDIO_OUT_BOTH);
    ok("SimpleVideo idle: not running, not paused",
       !SimpleVideo_IsRunning() && !SimpleVideo_IsPaused());
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
        ok("SimpleVideo_AudioDataRead idle -> not OK",
           SimpleVideo_AudioDataRead((int16_t[8]){0}, 4, &got) != FM_OK);
        ok("SimpleVideo_GetAudioInfo idle -> not OK",
           SimpleVideo_GetAudioInfo(&ai) != FM_OK);
    }
    {
        int16_t v[2] = {1000, -1000};
        audio_record_video_pre_tap(NULL, v, 2, 48000, 2);
        ok("audio_record_video_pre_tap: -6 dB halving",
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

        (void)Directory_DeleteRecursive("/recsrc");
        (void)Directory_DeleteRecursive("/recdst");
        (void)Directory_Create("/recsrc/a/b");
        (void)File_WriteText("/recsrc/a/hello.txt", "salam");
        (void)File_WriteText("/recsrc/a/b/deep.txt", "deep-salam");

        r = audio_record_folder_copy("/recsrc", "/recdst");
        ok("Directory_Copy nested /recsrc -> /recdst", r == FM_OK);
        if (r == FM_OK)
        {
            r = File_ReadText("/recdst/a/hello.txt", txt,
                              sizeof(txt), NULL);
            ok("copied shallow file content", r == FM_OK &&
               strcmp(txt, "salam") == 0);
            r = File_ReadText("/recdst/a/b/deep.txt", txt,
                              sizeof(txt), NULL);
            ok("copied nested file content", r == FM_OK &&
               strcmp(txt, "deep-salam") == 0);
        }
        (void)File_Exists("/recdst/a/hello.txt", &exists);
        ok("cleanup precondition sane", exists == FM_TRUE);
        (void)Directory_DeleteRecursive("/recsrc");
        (void)Directory_DeleteRecursive("/recdst");
    }

    /* ---- 6) output switch + callbacks survive Stop()/Begin() ------ */
    {
        static int16_t tone[16000];
        SimpleAudioInfo si;
        size_t i;

        (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
        for (i = 0; i < sizeof(tone) / sizeof(tone[0]); i++)
        {
            tone[i] = (int16_t)(12000.0 *
                                sin(2.0 * 3.14159265358979 * 440.0 *
                                    (double)i / 16000.0));
        }
        (void)Directory_Create(SIMPLE_AUDIO_FOLDER);
        r = SimpleAudio_RecStart(SIMPLE_AUDIO_FOLDER "/keep.wav", 16000, 1);
        if (r == FM_OK) r = SimpleAudio_RecWrite(tone, 16000);
        if (r == FM_OK) r = SimpleAudio_RecStop();
        ok("a one second test track in the music folder", r == FM_OK);

        /* everything is chosen BEFORE Begin, once */
        SimpleAudio_SetDataCallback(keep_data, NULL);
        SimpleAudio_SetMonitorCallback(keep_monitor, NULL);
        SimpleAudio_SetTrackStartCallback(keep_start, NULL);
        SimpleAudio_SetTrackEndCallback(keep_end, NULL);
        (void)SimpleAudio_SetVolume(60);
        (void)SimpleAudio_SetPlayMode(FM_PLAY_MODE_REPEAT_ONE);
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK);
        ok("not running before Begin", !SimpleAudio_IsRunning());

        r = SimpleAudio_Begin();
        ok("SimpleAudio_Begin()", r == FM_OK);
        ok("the first buffer already went through both callbacks",
           keep_data_calls >= 1 && keep_monitor_calls >= 1 &&
           keep_data_peak > 11000);
        ok("the first track was reported once", keep_starts == 1);
        ok("output = callback: the DMA buffer is silence",
           keep_dma_silent());
        SimpleAudio_Info(&si);
        ok("Info: running, volume 60, repeat one, output callback",
           si.running == 1u && si.volume_pct == 60u &&
           si.play_mode == FM_PLAY_MODE_REPEAT_ONE &&
           si.output == FM_AUDIO_OUT_CALLBACK);

        ok("SimpleAudio_Stop()", SimpleAudio_Stop() == FM_OK &&
           !SimpleAudio_IsRunning());
        keep_data_calls = 0;
        keep_monitor_calls = 0;

        /* a second Begin: nothing is installed again */
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER);
        r = SimpleAudio_Start();
        ok("SimpleAudio_Start() begins a stopped player",
           r == FM_OK && SimpleAudio_IsRunning());
        ok("output = speaker: no PCM callback, samples in the DMA buffer",
           keep_data_calls == 0 && keep_monitor_calls == 0 &&
           !keep_dma_silent());
        ok("track callbacks survived Stop()/Begin()", keep_starts == 2);
        ok("volume and play mode survived Stop()/Begin()",
           SimpleAudio_GetVolume() == 60u &&
           SimpleAudio_GetPlayMode() == FM_PLAY_MODE_REPEAT_ONE &&
           sa.dma.volume == (int32_t)((60u * 0x7FFFu) / 100u));
        ok("SimpleAudio_Start() while playing is a no-op",
           SimpleAudio_Start() == FM_OK && keep_starts == 2);

        (void)SimpleAudio_Stop();
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
        r = SimpleAudio_Begin();
        ok("output = both: callbacks and samples in the DMA buffer",
           r == FM_OK && keep_data_calls >= 1 && keep_monitor_calls >= 1 &&
           !keep_dma_silent());
        (void)SimpleAudio_Stop();
        ok("every started track was closed with an end report",
           keep_ends == keep_starts);

        SimpleAudio_SetDataCallback(NULL, NULL);
        SimpleAudio_SetMonitorCallback(NULL, NULL);
        SimpleAudio_SetTrackStartCallback(NULL, NULL);
        SimpleAudio_SetTrackEndCallback(NULL, NULL);
        (void)SimpleAudio_SetVolume(100);
        (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
    }

    printf("\nResults: %s\n", fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
