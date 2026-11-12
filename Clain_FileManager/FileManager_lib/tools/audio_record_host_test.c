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

/* ---- section 7: saving the music that is playing -------------------- */

/* Data callback of section 7: halves every sample, so the test can tell
 * that the saved sound is the one AFTER the application's processing. */
static void save_half_gain(void *ctx, int16_t *pcm, size_t samples,
                           uint32_t rate, uint16_t channels)
{
    size_t i;

    (void)ctx; (void)rate; (void)channels;
    for (i = 0; i < samples; i++)
    {
        pcm[i] = (int16_t)(pcm[i] / 2);
    }
}

/* What the I2S interrupts do on the board: report a played half, let the
 * main loop refill it.  Runs until the playlist is over. */
static int save_play_out(int max_halves)
{
    int n = 0;

    while (SimpleAudio_IsRunning() && n < max_halves)
    {
        if ((n & 1) == 0) AudioDma_HalfCallback(&sa.dma);
        else              AudioDma_FullCallback(&sa.dma);
        (void)SimpleAudio_Poll();
        n++;
    }
    (void)SimpleAudio_Poll();
    return n;
}

/* Writes a test track: `frames` frames of a sine at `freq`. */
static FM_Result save_make_track(const char *path, uint32_t rate,
                                 uint16_t channels, uint32_t frames,
                                 double freq)
{
    static int16_t buf[2 * 24000];
    uint32_t i;
    FM_Result r;

    for (i = 0; i < frames; i++)
    {
        int16_t v = (int16_t)(12000.0 * sin(2.0 * 3.14159265358979 * freq *
                                            (double)i / (double)rate));
        if (channels == 2u)
        {
            buf[2u * i] = v;
            buf[2u * i + 1u] = (int16_t)(-v / 2);
        }
        else
        {
            buf[i] = v;
        }
    }
    r = SimpleAudio_RecStart(path, rate, channels);
    if (r == FM_OK) r = SimpleAudio_RecWrite(buf, frames);
    if (r == FM_OK) r = SimpleAudio_RecStop();
    return r;
}

/* Decodes a whole file to interleaved 16-bit samples (up to `cap`). */
static FM_Result save_load(const char *path, int16_t *pcm, size_t cap,
                           size_t *samples, uint32_t *rate,
                           uint16_t *channels, int *format)
{
    FM_Audio a;
    FM_AudioInfo info;
    size_t total = 0u;
    FM_Result r = Audio_Open(path, &a);

    *samples = 0u;
    if (r != FM_OK) return r;
    memset(&info, 0, sizeof(info));
    (void)Audio_GetInfo(&a, &info);
    *rate = info.sample_rate;
    *channels = info.channels;
    if (format != NULL) *format = (strcmp(info.codec, "FLAC") == 0);
    for (;;)
    {
        size_t got = 0u;

        if (total >= cap) break;
        r = Audio_Read(&a, pcm + total, cap - total, &got);
        if (r != FM_OK || got == 0u) break;
        total += got;
    }
    (void)Audio_Close(&a);
    *samples = total;
    return (r == FM_ERR_EOF) ? FM_OK : r;
}

static int save_file_missing(const char *path)
{
    FM_Bool exists = FM_TRUE;

    return File_Exists(path, &exists) == FM_OK && !exists;
}

/* ---- section 8: saving video ---------------------------------------- */

static uint32_t save_video_calls;

/* Frame callback of SimpleVideo_Convert: paints the two top rows red
 * (video stores colour at half resolution, so one row alone would mix
 * with the row under it). */
static void save_video_stamp(void *ctx, uint8_t *rgb, uint16_t w, uint16_t h,
                             uint32_t frame)
{
    uint32_t x;

    (void)ctx; (void)h; (void)frame;
    save_video_calls++;
    for (x = 0; x < 2u * (uint32_t)w; x++)
    {
        rgb[3u * x] = 255u; rgb[3u * x + 1u] = 0u; rgb[3u * x + 2u] = 0u;
    }
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

    /* ---- 7) saving the music that is playing ---------------------- */
    {
        static int16_t src[2 * 24000];
        static int16_t got[2 * 64000];
        size_t ns, ng, i;
        uint32_t rate;
        uint16_t ch;
        int fmt, bad;

        (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
        (void)Directory_DeleteRecursive("/saved");
        (void)Directory_Create(SIMPLE_AUDIO_FOLDER);
        r = save_make_track(SIMPLE_AUDIO_FOLDER "/a.wav", 16000, 1, 16000,
                            440.0);
        if (r == FM_OK)
            r = save_make_track(SIMPLE_AUDIO_FOLDER "/b.wav", 16000, 2, 8000,
                                880.0);
        if (r == FM_OK)
            r = save_make_track(SIMPLE_AUDIO_FOLDER "/c.wav", 22050, 1, 11025,
                                330.0);
        ok("three test tracks (16k mono, 16k stereo, 22.05k mono)",
           r == FM_OK);

        /* --- one file per track, speaker off, with DSP, volume 30 ---
         * The data callback runs only when the output includes
         * "callback"; the recorder then gets what it left. */
        SimpleAudio_SetDataCallback(save_half_gain, NULL);
        (void)SimpleAudio_SetVolume(30);
        (void)SimpleAudio_SetPlayMode(FM_PLAY_MODE_NEXT);
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK);
        ok("RecPlaying(\"/x.mp3\") without an MP3 encoder -> unsupported",
           SimpleAudio_RecPlaying("/saved/x.mp3") == FM_ERR_UNSUPPORTED &&
           !SimpleAudio_RecActive());
        r = SimpleAudio_RecPlaying("/saved");
        ok("RecPlaying(\"/saved\") before Begin", r == FM_OK &&
           SimpleAudio_RecActive());
        ok("a second RecPlaying / RecStart / RecWrite is refused (busy)",
           SimpleAudio_RecPlaying("/saved2") == FM_ERR_BUSY &&
           SimpleAudio_RecStart("/saved/m.wav", 16000, 1) == FM_ERR_BUSY &&
           SimpleAudio_RecWrite(src, 4) == FM_ERR_BUSY);
        r = SimpleAudio_Begin();
        ok("Begin while a recording is armed", r == FM_OK);
        ok("output = callback: nothing reaches the DMA buffer",
           keep_dma_silent());
        (void)save_play_out(2000);
        ok("the playlist ran to its end", !SimpleAudio_IsRunning());
        ok("per-track mode stays armed after the playlist ended",
           SimpleAudio_RecActive() && !sa_rec.active);

        r = save_load(SIMPLE_AUDIO_FOLDER "/a.wav", src, 2 * 24000, &ns,
                      &rate, &ch, NULL);
        r = (r == FM_OK) ? save_load("/saved/a.wav", got, 2 * 64000, &ng,
                                     &rate, &ch, &fmt) : r;
        bad = 0;
        for (i = 0; i < ns && 2u * i + 1u < ng; i++)
        {
            if (got[2u * i] != src[i] / 2 || got[2u * i + 1u] != src[i] / 2)
                bad++;
        }
        ok("/saved/a.wav: 16 kHz stereo, the whole track, no more",
           r == FM_OK && rate == 16000u && ch == 2u && ns == 16000u &&
           ng == 32000u);
        ok("  = the data callback's output, before the volume", bad == 0);

        r = save_load(SIMPLE_AUDIO_FOLDER "/b.wav", src, 2 * 24000, &ns,
                      &rate, &ch, NULL);
        r = (r == FM_OK) ? save_load("/saved/b.wav", got, 2 * 64000, &ng,
                                     &rate, &ch, NULL) : r;
        bad = 0;
        for (i = 0; i < ns && i < ng; i++)
        {
            if (got[i] != src[i] / 2) bad++;
        }
        ok("/saved/b.wav: stereo track, sample for sample",
           r == FM_OK && rate == 16000u && ch == 2u && ns == 16000u &&
           ng == ns && bad == 0);
        r = save_load("/saved/c.wav", got, 2 * 64000, &ng, &rate, &ch, NULL);
        ok("/saved/c.wav: the 22.05 kHz track kept its own rate",
           r == FM_OK && rate == 22050u && ch == 2u && ng == 22050u);

        /* --- Stop + Begin: still armed; FLAC + mono; stop mid-way --- */
        ok("RecSetFormat(FLAC) + RecSetChannels(1)",
           SimpleAudio_RecSetFormat(FM_AUDIO_FILE_FLAC) == FM_OK &&
           SimpleAudio_RecSetChannels(1) == FM_OK &&
           SimpleAudio_RecSetChannels(3) == FM_ERR_INVALID_ARGUMENT &&
           SimpleAudio_RecSetFormat(FM_AUDIO_FILE_MP3) ==
               FM_ERR_UNSUPPORTED);
        r = SimpleAudio_Begin();
        ok("second Begin: saving restarts by itself, now a.flac",
           r == FM_OK && sa_rec.active &&
           strcmp(SimpleAudio_RecPath(), "/saved/a.flac") == 0);
        ok("SimpleAudio_Stop() closes the open file",
           SimpleAudio_Stop() == FM_OK && !sa_rec.active &&
           SimpleAudio_RecActive());
        r = save_load("/saved/a.flac", got, 2 * 64000, &ng, &rate, &ch,
                      &fmt);
        bad = 0;
        (void)save_load(SIMPLE_AUDIO_FOLDER "/a.wav", src, 2 * 24000, &ns,
                        &rate, &ch, NULL);
        r = (r == FM_OK) ? save_load("/saved/a.flac", got, 2 * 64000, &ng,
                                     &rate, &ch, &fmt) : r;
        for (i = 0; i < ng && i < ns; i++)
        {
            if (got[i] != src[i] / 2) bad++;
        }
        ok("/saved/a.flac: FLAC, mono, what was played until the stop",
           r == FM_OK && fmt == 1 && ch == 1u &&
           ng > 0u && ng <= 16000u && bad == 0);
        ok("RecStop() ends the per-track mode",
           SimpleAudio_RecStop() == FM_OK && !SimpleAudio_RecActive() &&
           SimpleAudio_RecStop() == FM_ERR_NOT_READY);
        (void)SimpleAudio_RecSetFormat(FM_AUDIO_FILE_AUTO);
        (void)SimpleAudio_RecSetChannels(0);

        /* --- one file for everything, started in the middle; output
         *     "none": no callback, no speaker, the file is still made --- */
        SimpleAudio_SetDataCallback(NULL, NULL);
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_NONE);
        r = SimpleAudio_Begin();
        ok("output = none: nothing reaches the DMA buffer",
           r == FM_OK && keep_dma_silent());
        if (r == FM_OK) r = SimpleAudio_RecPlaying("/saved/all.flac");
        ok("RecPlaying(\"/saved/all.flac\") while playing", r == FM_OK);
        (void)save_play_out(2000);
        ok("single file: saving ended where the sample rate changed",
           !SimpleAudio_RecActive() &&
           SimpleAudio_RecStop() == FM_ERR_NOT_READY);
        r = save_load("/saved/all.flac", got, 2 * 64000, &ng, &rate, &ch,
                      &fmt);
        (void)save_load(SIMPLE_AUDIO_FOLDER "/b.wav", src, 2 * 24000, &ns,
                        &rate, &ch, NULL);
        bad = 0;
        for (i = 0; i < ns && i < ng; i++)
        {
            /* the last 8000 frames of the file are track b */
            if (ng >= ns && got[ng - ns + i] != src[i]) bad++;
        }
        ok("/saved/all.flac: a (from where saving began) then all of b",
           r == FM_OK && fmt == 1 && ng >= 16000u &&
           ng <= 48000u && (ng % 2u) == 0u && bad == 0);

        /* --- same folder, same extension: the source is not touched --- */
        (void)SimpleAudio_SetPlayMode(FM_PLAY_MODE_STOP);
        r = SimpleAudio_RecPlaying(SIMPLE_AUDIO_FOLDER);
        if (r == FM_OK) r = SimpleAudio_Begin();
        (void)save_play_out(2000);
        ok("saving into the music folder itself -> a_rec.wav",
           r == FM_OK &&
           strcmp(SimpleAudio_RecPath(),
                  SIMPLE_AUDIO_FOLDER "/a_rec.wav") == 0);
        (void)SimpleAudio_RecStop();
        r = save_load(SIMPLE_AUDIO_FOLDER "/a.wav", got, 2 * 64000, &ng,
                      &rate, &ch, NULL);
        ok("  the track itself is still the original (mono, 16000)",
           r == FM_OK && ch == 1u && ng == 16000u);
        (void)File_Delete(SIMPLE_AUDIO_FOLDER "/a_rec.wav");

        /* --- your own sound while music plays --- */
        (void)SimpleAudio_SetPlayMode(FM_PLAY_MODE_REPEAT_ALL);
        r = SimpleAudio_RecStart("/saved/mic.wav", 8000, 1);
        ok("RecStart for your own sound", r == FM_OK);
        r = SimpleAudio_Begin();
        ok("Begin is not blocked by a recording any more", r == FM_OK);
        for (i = 0; i < 4000u; i++) src[i] = (int16_t)(i * 3u);
        r = SimpleAudio_RecWrite(src, 4000);
        AudioDma_HalfCallback(&sa.dma);
        (void)SimpleAudio_Poll();
        if (r == FM_OK) r = SimpleAudio_RecWrite(src, 4000);
        ok("RecWrite between polls of a running player", r == FM_OK &&
           SimpleAudio_IsRunning());
        r = SimpleAudio_RecStop();
        (void)SimpleAudio_Stop();
        r = (r == FM_OK) ? save_load("/saved/mic.wav", got, 2 * 64000, &ng,
                                     &rate, &ch, NULL) : r;
        ok("/saved/mic.wav holds exactly what was written",
           r == FM_OK && rate == 8000u && ch == 1u && ng == 8000u &&
           got[100] == 300 && got[4100] == 300);

        /* --- ADPCM by option, FLAC by name, nothing left on error --- */
        (void)SimpleAudio_RecSetFormat(FM_AUDIO_FILE_ADPCM);
        r = save_make_track("/saved/small.wav", 16000, 1, 16000, 500.0);
        (void)SimpleAudio_RecSetFormat(FM_AUDIO_FILE_AUTO);
        {
            uint64_t sz_a = 0u, sz_w = 0u;

            (void)File_SizeOf("/saved/small.wav", &sz_a);
            (void)File_SizeOf(SIMPLE_AUDIO_FOLDER "/a.wav", &sz_w);
            ok("RecSetFormat(ADPCM): a .wav of about a quarter the size",
               r == FM_OK && sz_a > 0u && sz_a * 3u < sz_w);
        }
        ok("RecStart(\"/saved/x.mp3\") without an encoder: no file",
           SimpleAudio_RecStart("/saved/x.mp3", 16000, 1) ==
               FM_ERR_UNSUPPORTED &&
           save_file_missing("/saved/x.mp3") && !SimpleAudio_RecActive());

        /* --- file to file, no playback --- */
        r = SimpleAudio_Convert(SIMPLE_AUDIO_FOLDER "/b.wav",
                                "/saved/conv.flac", save_half_gain, NULL);
        (void)save_load(SIMPLE_AUDIO_FOLDER "/b.wav", src, 2 * 24000, &ns,
                        &rate, &ch, NULL);
        r = (r == FM_OK) ? save_load("/saved/conv.flac", got, 2 * 64000,
                                     &ng, &rate, &ch, &fmt) : r;
        bad = 0;
        for (i = 0; i < ns && i < ng; i++)
        {
            if (got[i] != src[i] / 2) bad++;
        }
        ok("SimpleAudio_Convert: WAV -> processed FLAC, every sample",
           r == FM_OK && fmt == 1 && ch == 2u &&
           ng == ns && ns == 16000u && bad == 0);
        ok("SimpleAudio_Convert of a missing file leaves nothing behind",
           SimpleAudio_Convert("/saved/none.wav", "/saved/none.flac", NULL,
                               NULL) != FM_OK &&
           save_file_missing("/saved/none.flac"));

        (void)SimpleAudio_SetVolume(100);
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER);
        (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
        (void)Directory_DeleteRecursive("/saved");
    }

    /* ---- 8) saving video: MJPEG AVI ------------------------------- */
    {
        enum { VW = 64, VH = 48, VN = 10 };
        static uint8_t rgb[VW * VH * 3];
        static uint8_t fb565[VW * VH * 2];
        static uint8_t back[VW * VH * 3];
        static int16_t snd[800];
        FM_Video v;
        FM_VideoInfo vi;
        FM_AudioTrackInfo ai;
        size_t i;
        int n;

        (void)Directory_DeleteRecursive("/vrec");
        (void)Directory_Create("/vrec");
        for (i = 0; i < (size_t)VW * VH; i++)
        {
            /* left half green, right half blue */
            uint8_t g = ((i % VW) < VW / 2) ? 255u : 0u;
            uint16_t px = (uint16_t)(g ? 0x07E0u : 0x001Fu);

            rgb[3u * i] = 0u; rgb[3u * i + 1u] = g;
            rgb[3u * i + 2u] = (uint8_t)(255u - g);
            fb565[2u * i] = (uint8_t)(px & 0xFFu);
            fb565[2u * i + 1u] = (uint8_t)(px >> 8);
        }
        for (i = 0; i < 800u; i++) snd[i] = (int16_t)(i * 5);

        ok("RecWrite before RecStart -> not ready",
           SimpleVideo_RecWrite(rgb, sizeof(rgb)) == FM_ERR_NOT_READY &&
           !SimpleVideo_RecActive());
        ok("RecSetQuality(85) / RecSetAudio(8000, 1)",
           SimpleVideo_RecSetQuality(85) == FM_OK &&
           SimpleVideo_RecSetQuality(101) == FM_ERR_INVALID_ARGUMENT &&
           SimpleVideo_RecSetAudio(8000, 1) == FM_OK &&
           SimpleVideo_RecSetAudio(8000, 5) == FM_ERR_INVALID_ARGUMENT);
        r = SimpleVideo_RecStart("/vrec/cam.avi", VW, VH, 10, 0);
        ok("RecStart(\"/vrec/cam.avi\") opens at once", r == FM_OK &&
           SimpleVideo_RecActive());
        ok("a second RecStart is refused (busy)",
           SimpleVideo_RecStart("/vrec/two.avi", VW, VH, 10, 0) ==
               FM_ERR_BUSY);
        ok("RecWrite with a wrong byte count is refused",
           SimpleVideo_RecWrite(rgb, 100) == FM_ERR_INVALID_ARGUMENT);
        ok("RecWriteJpeg with something that is no JPEG is refused",
           SimpleVideo_RecWriteJpeg(rgb, 200) == FM_ERR_INVALID_FORMAT);
        for (n = 0; r == FM_OK && n < VN; n++)
        {
            r = (n & 1) ? SimpleVideo_RecWrite(rgb, sizeof(rgb))
                        : SimpleVideo_RecWritePixels(fb565, FM_PIXEL_RGB565);
            if (r == FM_OK) r = SimpleVideo_RecWriteAudio(snd, 800);
        }
        ok("ten frames (RGB565 and RGB888 by turns) with their sound",
           r == FM_OK && SimpleVideo_RecFrames() == (uint32_t)VN);
        r = SimpleVideo_RecStop();
        ok("RecStop() closes the file", r == FM_OK &&
           !SimpleVideo_RecActive());

        memset(&vi, 0, sizeof(vi));
        memset(&ai, 0, sizeof(ai));
        memset(back, 0, sizeof(back));
        r = Video_Open("/vrec/cam.avi", &v);
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            (void)Video_GetAudioInfo(&v, &ai);
            r = Video_DecodeFrame(&v, back, sizeof(back));
            (void)Video_Close(&v);
        }
        ok("the file is an MJPEG AVI: 64x48, 10 fps, 10 frames, 8 kHz mono",
           r == FM_OK && strcmp(vi.container, "AVI") == 0 &&
           vi.codec == FM_VIDEO_CODEC_MJPEG && vi.width == VW &&
           vi.height == VH && vi.fps == 10u && vi.frame_count == (uint32_t)VN &&
           ai.sample_rate == 8000u && ai.channels == 1u);
        ok("its first frame is green on the left and blue on the right",
           back[3 * (20 * VW + 8) + 1] > 230u &&
           back[3 * (20 * VW + 8) + 2] < 25u &&
           back[3 * (20 * VW + 56) + 2] > 230u &&
           back[3 * (20 * VW + 56) + 1] < 25u);

        /* file to file with a callback on every frame */
        (void)SimpleVideo_RecSetAudio(0, 0);
        r = SimpleVideo_Convert("/vrec/cam.avi", "/vrec/out.avi",
                                save_video_stamp, NULL);
        memset(back, 0, sizeof(back));
        memset(&vi, 0, sizeof(vi));
        memset(&ai, 0, sizeof(ai));
        if (r == FM_OK) r = Video_Open("/vrec/out.avi", &v);
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            (void)Video_GetAudioInfo(&v, &ai);
            r = Video_DecodeFrame(&v, back, sizeof(back));
            (void)Video_Close(&v);
        }
        ok("SimpleVideo_Convert: every frame went through the callback",
           r == FM_OK && save_video_calls == (uint32_t)VN &&
           vi.frame_count == (uint32_t)VN);
        ok("  the red row it painted is in the new file, sound kept",
           back[3 * 8] > 200u && back[3 * 8 + 1] < 70u &&
           back[3 * (20 * VW + 8) + 1] > 230u && ai.sample_rate == 8000u);

        /* any other extension: the raw FMRV writer, through Poll, as
         * before */
        r = SimpleVideo_RecStart("/vrec/raw.fmrv", VW, VH, 10, 0);
        (void)SimpleVideo_Poll();
        if (r == FM_OK) r = SimpleVideo_RecWrite(rgb, sizeof(rgb));
        ok("RecWritePixels on a raw FMRV recording -> unsupported",
           SimpleVideo_RecWritePixels(fb565, FM_PIXEL_RGB565) ==
               FM_ERR_UNSUPPORTED);
        if (r == FM_OK) r = SimpleVideo_RecStop();
        (void)SimpleVideo_Poll();
        memset(&vi, 0, sizeof(vi));
        if (r == FM_OK) r = Video_Open("/vrec/raw.fmrv", &v);
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            (void)Video_Close(&v);
        }
        ok("a name without .avi still records raw FMRV",
           r == FM_OK && strcmp(vi.container, "FMRV") == 0 &&
           vi.frame_count == 1u && !SimpleVideo_RecActive());

        (void)SimpleVideo_RecSetQuality(0);
        (void)Directory_DeleteRecursive("/vrec");
    }

    printf("\nResults: %s\n", fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
