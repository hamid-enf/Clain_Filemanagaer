/*
 * Host test of examples/examples_save_media.h: the four save examples are
 * RUN here, against the real library and the real players, and the files
 * they write are opened again and compared with what went in.
 *
 *   plain:     SaveDemo_AudioPlain, SaveDemo_VideoPlain
 *   callback:  the music / the video that is playing, saved again through
 *              the data and frame callbacks (step by step and with the
 *              all-in-one functions)
 *
 * The board's interrupts are simulated: the I2S "half played" events are
 * raised by the test (for the all-in-one music example from a timer
 * signal, as a real interrupt would), and the clock of the HAL stub is
 * moved from HAL_GetTick, slowly (every frame is shown) or fast (the
 * player has to drop frames, the recorder has to fill them).
 *
 *     make saveexamplestest
 *     ./fm_saveexamplestest -v      (also prints what the examples log)
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
#include <signal.h>
#include <sys/time.h>

#include "file_manager.h"
#include "fm_filesystem.h"

extern FM_Result PosixFS_Init(FM_FS *fs);

#include "simple_audio.h"
#include "simple_video.h"
#include "examples_save_media.h"

I2S_HandleTypeDef hi2s1;
I2S_HandleTypeDef hi2s2;

static int fails;

/* -v: what the examples print (FM_Log) is shown between the checks. */
static void log_to_stdout(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)fwrite(data, 1u, len, stdout);
}

static void ok(const char *what, int cond)
{
    printf("  %-62s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

/* ---- helpers --------------------------------------------------------- */

static int file_missing(const char *path)
{
    FM_Bool exists = FM_TRUE;

    return File_Exists(path, &exists) == FM_OK && !exists;
}

/* Decodes a whole sound file to interleaved 16-bit samples. */
static FM_Result load_audio(const char *path, int16_t *pcm, size_t cap,
                            size_t *samples, uint32_t *rate,
                            uint16_t *channels)
{
    static FM_Audio a;
    FM_AudioInfo info;
    size_t total = 0u;
    FM_Result r = Audio_Open(path, &a);

    *samples = 0u;
    *rate = 0u;
    *channels = 0u;
    if (r != FM_OK) return r;
    memset(&info, 0, sizeof(info));
    (void)Audio_GetInfo(&a, &info);
    *rate = info.sample_rate;
    *channels = info.channels;
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

static FM_Result make_track(const char *path, uint32_t rate,
                            uint16_t channels, uint32_t frames,
                            int16_t seed)
{
    static int16_t buf[2 * 24000];
    uint32_t i;
    FM_Result r;

    for (i = 0; i < frames * channels; i++)
    {
        buf[i] = (int16_t)(((int32_t)i * 37 + seed) % 20001 - 10000);
    }
    r = SimpleAudio_RecStart(path, rate, channels);
    if (r == FM_OK) r = SimpleAudio_RecWrite(buf, frames);
    if (r == FM_OK) r = SimpleAudio_RecStop();
    return r;
}

/* ---- the test video --------------------------------------------------- */

enum { SRC_W = 64, SRC_H = 48, SRC_FPS = 10, SRC_N = 20, SRC_RATE = 16000 };
#define SRC_SPF (SRC_RATE / SRC_FPS)            /* sound frames per picture */

static uint8_t g_rgb[SRC_W * SRC_H * 3];
static int16_t g_src_snd[SRC_N * SRC_SPF];

/* Picture n is one flat grey level, so a saved picture tells its number.
 * The levels are 12 apart: RGB565 and JPEG move a level by up to 6. */
static int grey_of(uint32_t n) { return 16 + 12 * (int)n; }
#define GREY_TOL 6

static FM_Result make_video(const char *path)
{
    static FM_VideoFileWriter w;
    FM_VideoSaveConfig c;
    FM_Result r;
    uint32_t n;
    uint32_t i;

    memset(&c, 0, sizeof(c));
    c.width = SRC_W;
    c.height = SRC_H;
    c.fps = SRC_FPS;
    c.quality = 95u;
    c.audio_rate = SRC_RATE;
    c.audio_channels = 1u;
    for (i = 0; i < (uint32_t)(SRC_N * SRC_SPF); i++)
    {
        g_src_snd[i] = (int16_t)((i * 13u) % 16001u) - 8000;
    }
    r = VideoFileWriter_Open(&w, path, &c);
    for (n = 0; r == FM_OK && n < SRC_N; n++)
    {
        memset(g_rgb, grey_of(n), sizeof(g_rgb));
        r = VideoFileWriter_WriteFrame(&w, g_rgb, FM_PIXEL_RGB888);
        if (r == FM_OK)
        {
            r = VideoFileWriter_WriteAudio(&w, g_src_snd + n * SRC_SPF,
                                           SRC_SPF);
        }
    }
    if (r == FM_OK) r = VideoFileWriter_Close(&w);
    else (void)VideoFileWriter_Close(&w);
    return r;
}

typedef struct
{
    FM_VideoInfo vi;
    FM_AudioTrackInfo ai;
    int      grey[64];          /* level of every picture */
    uint32_t pictures;
    size_t   snd_samples;
    int      snd_same;          /* sound == the source, L == R */
    int      snd_prefix;        /* sound is a prefix of / equals the source */
} SavedVideo;

static int16_t g_out_snd[2 * SRC_N * SRC_SPF + 70000];

/* Opens a saved AVI and reads back everything the tests look at. */
static FM_Result inspect_video(const char *path, SavedVideo *s)
{
    static FM_Video v;
    static uint8_t px[320 * 240 * 3];
    FM_Result r = Video_Open(path, &v);
    size_t i;

    memset(s, 0, sizeof(*s));
    if (r != FM_OK) return r;
    (void)Video_GetInfo(&v, &s->vi);
    (void)Video_GetAudioInfo(&v, &s->ai);
    while (s->pictures < 64u)
    {
        long sum = 0;
        size_t n = (size_t)s->vi.width * s->vi.height * 3u;

        r = Video_DecodeFrame(&v, px, sizeof(px));
        if (r != FM_OK) break;
        for (i = 0; i < n; i++) sum += px[i];
        s->grey[s->pictures++] = (int)(sum / (long)n);
    }
    if (r == FM_ERR_EOF) r = FM_OK;
    for (;;)
    {
        size_t got = 0u;
        FM_Result ra;

        if (s->snd_samples * 2u >= sizeof(g_out_snd)) break;
        ra = Video_ReadAudioFrame(&v, (uint8_t *)g_out_snd +
                                  s->snd_samples * 2u,
                                  sizeof(g_out_snd) - s->snd_samples * 2u,
                                  &got);
        if (ra != FM_OK || got == 0u) break;
        s->snd_samples += got / 2u;
    }
    (void)Video_Close(&v);
    s->snd_prefix = 1;
    for (i = 0; i + 1u < s->snd_samples; i += 2u)
    {
        if (i / 2u >= (size_t)(SRC_N * SRC_SPF) ||
            g_out_snd[i] != g_src_snd[i / 2u] ||
            g_out_snd[i + 1u] != g_src_snd[i / 2u])
        {
            s->snd_prefix = 0;
            break;
        }
    }
    s->snd_same = s->snd_prefix &&
                  s->snd_samples == (size_t)(2 * SRC_N * SRC_SPF);
    return r;
}

/* ---- the simulated board --------------------------------------------- */

static uint32_t sim_step_ms;        /* clock advance per HAL_GetTick */
static uint32_t sim_half_ms;        /* play time of one I2S half */
static uint32_t sim_next_irq;
static uint8_t  sim_which;
static uint8_t  sim_busy;

/* HAL_GetTick hook: time passes, and whenever one half of the I2S ring
 * has been "played" the interrupt of that half is raised. */
static void sim_tick(void)
{
    if (sim_busy) return;
    sim_busy = 1u;
    halstub_tick_ms += sim_step_ms;
    if (sv_i2s_running != 0u)
    {
        while ((int32_t)(halstub_tick_ms - sim_next_irq) >= 0)
        {
            if (sim_which == 0u) sv_i2s_half(&hi2s1);
            else                 sv_i2s_full(&hi2s1);
            sim_which ^= 1u;
            sim_next_irq += sim_half_ms;
        }
    }
    else
    {
        sim_next_irq = halstub_tick_ms + sim_half_ms;
        sim_which = 0u;
    }
    sim_busy = 0u;
}

static void sim_video_clock(uint32_t step_ms)
{
    sim_step_ms = step_ms;
    sim_half_ms = (uint32_t)(((uint64_t)SIMPLE_VIDEO_HALF_SAMPLES / 2u) *
                             1000u / SRC_RATE);
    sim_next_irq = halstub_tick_ms + sim_half_ms;
    sim_which = 0u;
    halstub_tick_hook = sim_tick;
}

/* Timer signal = the I2S interrupt of the music player. */
static volatile sig_atomic_t alarm_half;

static void alarm_irq(int sig)
{
    (void)sig;
    if (sa.dma.running)
    {
        if (alarm_half == 0) AudioDma_HalfCallback(&sa.dma);
        else                 AudioDma_FullCallback(&sa.dma);
        alarm_half = !alarm_half;
    }
}

static void alarm_start(void)
{
    struct sigaction sa_new;
    struct itimerval it;

    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = alarm_irq;
    sa_new.sa_flags = SA_RESTART;
    (void)sigaction(SIGALRM, &sa_new, NULL);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 500;
    it.it_value = it.it_interval;
    (void)setitimer(ITIMER_REAL, &it, NULL);
}

static void alarm_stop(void)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    (void)setitimer(ITIMER_REAL, &it, NULL);
    (void)signal(SIGALRM, SIG_IGN);
}

/* Plays the video folder with the example's callbacks installed, the way
 * main.c does: Poll and Pump in a loop. */
static int play_and_save_video(const char *dest, uint32_t stop_after)
{
    int loops = 0;
    FM_Result r;

    (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    SimpleVideo_SetAudioDataCallback(SaveDemo_VideoSoundCallback, NULL);
    SimpleVideo_SetFrameCallback(SaveDemo_VideoFrameCallback, NULL);
    r = SaveDemo_VideoRecStart(dest);
    if (r == FM_OK) r = SimpleVideo_Begin();
    if (r == FM_OK) r = SimpleVideo_Start();
    if (r != FM_OK) return -1;
    while (SaveDemo_VideoRecActive() && loops < 200000)
    {
        (void)SimpleVideo_Poll();
        (void)SaveDemo_VideoRecPump();
        if (stop_after != 0u && SimpleVideo_RecFrames() >= stop_after)
        {
            (void)SaveDemo_VideoRecStop();
        }
        loops++;
    }
    (void)SimpleVideo_Stop();
    return loops;
}

int main(int argc, char **argv)
{
    static int16_t got[2 * 64000];
    static int16_t src_a[16000];
    static int16_t src_b[2 * 8000];
    FM_FS fs;
    FM_Result r;
    size_t n = 0u;
    size_t i;
    uint32_t rate = 0u;
    uint16_t ch = 0u;

    if (FileManager_Init() != FM_OK ||
        PosixFS_Init(&fs) != FM_OK ||
        FS_Mount("/", &fs) != FM_OK)
    {
        printf("FATAL: fs setup\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
    {
        FM_Log_SetSink(log_to_stdout, NULL);
    }
    printf("examples_save_media.h host test\n");
    printf("===============================\n");
    (void)Directory_DeleteRecursive(SAVE_DEMO_DIR);
    (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
    (void)Directory_DeleteRecursive(SIMPLE_VIDEO_FOLDER);

    /* ---- 1) plain: sound ------------------------------------------- */
    printf("\n[1] SaveDemo_AudioPlain\n");
    {
        uint64_t wav = 0u, flac = 0u;
        int same = 1;

        ok("the example returns FM_OK", SaveDemo_AudioPlain() == FM_OK);
        r = load_audio(SAVE_DEMO_TONE_WAV, got, 64000, &n, &rate, &ch);
        for (i = 0; i < n; i++)
        {
            if (got[i] != save_demo_sine16[i & 15u]) same = 0;
        }
        ok("tone.wav: 16 kHz mono, 2 s, the tone sample for sample",
           r == FM_OK && rate == 16000u && ch == 1u && n == 32000u && same);
        same = 1;
        r = load_audio(SAVE_DEMO_TONE_FLAC, got, 64000, &n, &rate, &ch);
        for (i = 0; i < n; i++)
        {
            if (got[i] != save_demo_sine16[i & 15u]) same = 0;
        }
        ok("tone.flac: the same sound, lossless",
           r == FM_OK && rate == 16000u && ch == 1u && n == 32000u && same);
        (void)File_SizeOf(SAVE_DEMO_TONE_WAV, &wav);
        (void)File_SizeOf(SAVE_DEMO_TONE_FLAC, &flac);
        ok("tone.flac is smaller than tone.wav", flac != 0u && flac < wav);
        ok("nothing is left recording", !SimpleAudio_RecActive());
    }

    /* ---- 2) plain: video ------------------------------------------- */
    printf("\n[2] SaveDemo_VideoPlain\n");
    {
        static FM_Video v;
        static uint8_t px[160 * 120 * 3];
        FM_VideoInfo vi;
        FM_AudioTrackInfo ai;
        size_t snd = 0u;
        int same = 1;

        memset(&vi, 0, sizeof(vi));
        memset(&ai, 0, sizeof(ai));
        ok("the example returns FM_OK", SaveDemo_VideoPlain() == FM_OK);
        r = Video_Open(SAVE_DEMO_BARS_AVI, &v);
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            (void)Video_GetAudioInfo(&v, &ai);
            r = Video_DecodeFrame(&v, px, sizeof(px));
            for (;;)
            {
                size_t g = 0u;

                if (Video_ReadAudioFrame(&v, (uint8_t *)got + snd * 2u,
                                         sizeof(got) - snd * 2u,
                                         &g) != FM_OK || g == 0u) break;
                snd += g / 2u;
            }
            (void)Video_Close(&v);
        }
        ok("bars.avi: MJPEG AVI, 160x120, 10 fps, 30 pictures",
           r == FM_OK && strcmp(vi.container, "AVI") == 0 &&
           vi.codec == FM_VIDEO_CODEC_MJPEG && vi.width == 160u &&
           vi.height == 120u && vi.fps == 10u && vi.frame_count == 30u);
        /* picture 0: white square at x 0..23, y 48..71; bar 5 is red */
        ok("picture 0 shows the bars and the white square",
           px[3 * (60 * 160 + 10)] > 235 && px[3 * (60 * 160 + 10) + 1] > 235 &&
           px[3 * (60 * 160 + 10) + 2] > 235 &&
           px[3 * (10 * 160 + 110)] > 220 &&
           px[3 * (10 * 160 + 110) + 1] < 40 &&
           px[3 * (10 * 160 + 150)] < 25);
        for (i = 0; i < snd; i++)
        {
            if (got[i] != save_demo_sine16[i & 15u]) same = 0;
        }
        ok("its sound track: 16 kHz mono, 3 s, the tone sample for sample",
           ai.sample_rate == 16000u && ai.channels == 1u && snd == 48000u &&
           same);
        ok("nothing is left recording", !SimpleVideo_RecActive());
    }

    /* ---- 3) callback: the music that is playing ---------------------- */
    printf("\n[3] music through the data callback (step by step)\n");
    {
        size_t na = 0u, nb = 0u;
        int same = 1;
        int loops = 0;

        (void)Directory_Create(SIMPLE_AUDIO_FOLDER);
        r = make_track(SIMPLE_AUDIO_FOLDER "/a.wav", 16000, 1, 16000, 5);
        if (r == FM_OK)
            r = make_track(SIMPLE_AUDIO_FOLDER "/b.wav", 16000, 2, 8000, 900);
        if (r == FM_OK)
            r = make_track(SIMPLE_AUDIO_FOLDER "/c.wav", 22050, 1, 11025, 77);
        ok("three test tracks written", r == FM_OK);
        (void)load_audio(SIMPLE_AUDIO_FOLDER "/a.wav", src_a, 16000, &na,
                         &rate, &ch);
        (void)load_audio(SIMPLE_AUDIO_FOLDER "/b.wav", src_b, 16000, &nb,
                         &rate, &ch);

        ok("Pump and Stop with nothing started are harmless",
           SaveDemo_MusicRecPump() == FM_OK &&
           SaveDemo_MusicRecStop() == FM_ERR_NOT_READY &&
           !SaveDemo_MusicRecActive());

        /* the order the header asks for: callback, RecStart, then play */
        (void)SimpleAudio_SetPlayMode(FM_PLAY_MODE_NEXT);
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
        SimpleAudio_SetDataCallback(SaveDemo_MusicCallback, NULL);
        r = SaveDemo_MusicRecStart("/save/step.wav");
        ok("RecStart arms the recorder, a second one is refused",
           r == FM_OK && SaveDemo_MusicRecActive() &&
           SaveDemo_MusicRecStart("/save/other.wav") == FM_ERR_BUSY);
        ok("no file before the first sound", file_missing("/save/step.wav"));
        r = SimpleAudio_Begin();
        while (SimpleAudio_IsRunning() && SaveDemo_MusicRecActive() &&
               loops < 5000)
        {
            if ((loops & 1) == 0) AudioDma_HalfCallback(&sa.dma);
            else                  AudioDma_FullCallback(&sa.dma);
            (void)SimpleAudio_Poll();
            (void)SaveDemo_MusicRecPump();
            loops++;
        }
        ok("the track with another sample rate ends the file by itself",
           r == FM_OK && !SaveDemo_MusicRecActive() &&
           !SimpleAudio_RecActive());
        (void)SimpleAudio_Stop();
        r = load_audio("/save/step.wav", got, 2 * 64000, &n, &rate, &ch);
        ok("one stereo 16 kHz file holding track a then track b",
           r == FM_OK && rate == 16000u && ch == 2u &&
           n == 2u * na + nb);
        for (i = 0; i < na && 2u * i + 1u < n; i++)
        {
            if (got[2u * i] != src_a[i] || got[2u * i + 1u] != src_a[i])
                same = 0;
        }
        ok("track a (mono) is there sample for sample, on both channels",
           na == 16000u && same);
        same = 1;
        for (i = 0; i < nb && 2u * na + i < n; i++)
        {
            if (got[2u * na + i] != src_b[i]) same = 0;
        }
        ok("track b (stereo) follows it sample for sample",
           nb == 16000u && same);
        ok("nothing was dropped", save_demo_music.ring.dropped == 0u);

        /* speaker only: the data callback is never called */
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER);
        r = SaveDemo_MusicRecStart("/save/silent.wav");
        if (r == FM_OK) r = SimpleAudio_Begin();
        for (loops = 0; loops < 6; loops++)
        {
            if ((loops & 1) == 0) AudioDma_HalfCallback(&sa.dma);
            else                  AudioDma_FullCallback(&sa.dma);
            (void)SimpleAudio_Poll();
            (void)SaveDemo_MusicRecPump();
        }
        ok("output SPEAKER: nothing arrives, Stop says so, no file",
           r == FM_OK && SaveDemo_MusicRecStop() == FM_OK &&
           file_missing("/save/silent.wav"));
        (void)SimpleAudio_Stop();
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
        SimpleAudio_SetDataCallback(NULL, NULL);
    }

    printf("\n[4] SaveDemo_MusicFromCallback (all in one, timer = interrupt)\n");
    {
        int same = 1;

        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER);
        alarm_start();
        r = SaveDemo_MusicFromCallback("/save/one.wav", 1u);
        alarm_stop();
        ok("the example returns FM_OK", r == FM_OK);
        r = load_audio("/save/one.wav", got, 2 * 64000, &n, &rate, &ch);
        for (i = 0; i < 16000u && 2u * i + 1u < n; i++)
        {
            if (got[2u * i] != src_a[i] || got[2u * i + 1u] != src_a[i])
                same = 0;
        }
        ok("at least 1 s saved, from the first sample of the first track",
           r == FM_OK && rate == 16000u && ch == 2u && n >= 2u * 16000u &&
           same);
        ok("playback stopped, callback removed, output restored",
           !SimpleAudio_IsRunning() && sa.dma.pcm_tap_pre == NULL &&
           SimpleAudio_GetOutput() == FM_AUDIO_OUT_SPEAKER &&
           !SaveDemo_MusicRecActive() && !SimpleAudio_RecActive());
        (void)SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);
    }

    /* ---- 5) callback: the video that is playing ---------------------- */
    printf("\n[5] video through the frame and sound callbacks\n");
    {
        static SavedVideo s;
        int loops;
        int order = 1;
        int steps = 0;

        (void)Directory_Create(SIMPLE_VIDEO_FOLDER);
        ok("test video written (64x48, 10 fps, 20 pictures, 16 kHz mono)",
           make_video(SIMPLE_VIDEO_FOLDER "/a.avi") == FM_OK);
        ok("Pump and Stop with nothing started are harmless",
           SaveDemo_VideoRecPump() == FM_OK &&
           SaveDemo_VideoRecStop() == FM_ERR_NOT_READY);

        /* a) the player keeps up: every picture arrives */
        sim_video_clock(1u);
        loops = play_and_save_video("/save/keep.avi", 0u);
        r = inspect_video("/save/keep.avi", &s);
        ok("the recording ends by itself at the end of the video",
           loops > 0 && loops < 200000 && !SaveDemo_VideoRecActive() &&
           !SimpleVideo_RecActive());
        ok("saved file: MJPEG AVI, 64x48, 10 fps, 20 pictures",
           r == FM_OK && s.vi.codec == FM_VIDEO_CODEC_MJPEG &&
           s.vi.width == SRC_W && s.vi.height == SRC_H &&
           s.vi.fps == SRC_FPS && s.vi.frame_count == SRC_N &&
           s.pictures == SRC_N);
        for (i = 0; i < s.pictures; i++)
        {
            if (abs(s.grey[i] - grey_of((uint32_t)i)) > GREY_TOL) order = 0;
        }
        ok("every picture is the picture of the source, in order", order);
        ok("no picture had to be repeated", save_demo_video.repeated == 0u);
        ok("sound track: stereo 16 kHz", s.ai.sample_rate == SRC_RATE &&
           s.ai.channels == 2u);
        ok("the whole sound of the video, sample for sample",
           s.snd_same);
        ok("no sound was dropped", save_demo_video.ring.dropped == 0u);

        /* b) the player is too slow and drops pictures */
        sim_video_clock(45u);
        loops = play_and_save_video("/save/slow.avi", 0u);
        r = inspect_video("/save/slow.avi", &s);
        ok("slow player: the file still has all 20 picture slots",
           loops > 0 && r == FM_OK && s.vi.frame_count == SRC_N &&
           s.pictures == SRC_N);
        order = 1;
        for (i = 0; i < s.pictures; i++)
        {
            /* slot i holds picture i, or an earlier one repeated */
            int g = (s.grey[i] - 16 + 6) / 12;

            if (g < 0 || g > (int)i ||
                abs(s.grey[i] - grey_of((uint32_t)g)) > GREY_TOL)
                order = 0;
            if (i > 0 && s.grey[i] < s.grey[i - 1u] - GREY_TOL) order = 0;
            if (i > 0 && abs(s.grey[i] - s.grey[i - 1u]) > GREY_TOL) steps++;
        }
        ok("dropped pictures are filled with the one before, never later",
           order && save_demo_video.repeated > 0u &&
           (uint32_t)steps + 1u + save_demo_video.repeated == SRC_N);
        ok("the sound is complete all the same", s.snd_same);
        printf("      (%lu of %u pictures were repeats)\n",
               (unsigned long)save_demo_video.repeated, (unsigned)SRC_N);

        /* c) stopped by the application in the middle */
        sim_video_clock(1u);
        loops = play_and_save_video("/save/part.avi", 6u);
        r = inspect_video("/save/part.avi", &s);
        ok("RecStop in the middle: a complete, shorter file",
           loops > 0 && r == FM_OK && s.vi.frame_count == 6u &&
           s.pictures == 6u && s.snd_prefix && s.snd_samples > 0u &&
           abs(s.grey[5] - grey_of(5)) <= GREY_TOL);

        /* d) sound output without the callback: picture only */
        sim_video_clock(1u);
        (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_SPEAKER);
        SimpleVideo_SetFrameCallback(SaveDemo_VideoFrameCallback, NULL);
        r = SaveDemo_VideoRecStart("/save/mute.avi");
        if (r == FM_OK) r = SimpleVideo_Begin();
        if (r == FM_OK) r = SimpleVideo_Start();
        for (loops = 0; SaveDemo_VideoRecActive() && loops < 200000; loops++)
        {
            (void)SimpleVideo_Poll();
            (void)SaveDemo_VideoRecPump();
        }
        (void)SimpleVideo_Stop();
        r = inspect_video("/save/mute.avi", &s);
        ok("output SPEAKER: all pictures, and no sound track in the file",
           r == FM_OK && s.pictures == SRC_N && s.snd_samples == 0u);
        (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    }

    printf("\n[6] SaveDemo_VideoFromCallback (all in one)\n");
    {
        static SavedVideo s;
        int order = 1;

        sim_video_clock(1u);
        (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_SPEAKER);
        r = SaveDemo_VideoFromCallback("/save/all.avi", 0u);
        ok("the example returns FM_OK", r == FM_OK);
        r = inspect_video("/save/all.avi", &s);
        for (i = 0; i < s.pictures; i++)
        {
            if (abs(s.grey[i] - grey_of((uint32_t)i)) > GREY_TOL) order = 0;
        }
        ok("the whole video: 20 pictures in order, the whole sound",
           r == FM_OK && s.pictures == SRC_N && order && s.snd_same);
        ok("playback stopped, callbacks removed, output restored",
           !SimpleVideo_IsRunning() && sv.frame_cb == NULL &&
           sv_pcm_tap_pre == NULL &&
           SimpleVideo_GetAudioOutput() == FM_AUDIO_OUT_SPEAKER &&
           !SaveDemo_VideoRecActive() && !SimpleVideo_RecActive());
        r = SaveDemo_VideoFromCallback("/save/five.avi", 5u);
        (void)inspect_video("/save/five.avi", &s);
        ok("max_frames = 5 stops after 5 pictures",
           r == FM_OK && s.pictures == 5u);
        (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    }
    halstub_tick_hook = NULL;

    (void)Directory_DeleteRecursive(SAVE_DEMO_DIR);
    (void)Directory_DeleteRecursive(SIMPLE_AUDIO_FOLDER);
    (void)Directory_DeleteRecursive(SIMPLE_VIDEO_FOLDER);

    printf("\nResults: %s\n", fails == 0 ? "all PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
