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

/* The same, with one SimpleVideo_SeekTo(to_sec) as soon as `at_frames`
 * pictures are in the file. */
static int play_seek_save(const char *dest, uint32_t at_frames, double to_sec)
{
    int loops = 0;
    int sought = 0;
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
        if (!sought && SimpleVideo_RecFrames() >= at_frames)
        {
            (void)SimpleVideo_SeekTo(to_sec);
            sought = 1;
        }
        loops++;
    }
    (void)SimpleVideo_Stop();
    return loops;
}

/* What a saved file with a seek in it must be.  Every picture of the
 * test video is one grey level and every sound sample of it is unique,
 * so both tell where in the source they come from.
 *   new_pics   slots that show another picture than the slot before
 *   in_sync    of those, the slots whose sound (the SRC_SPF sound frames
 *              at the slot's own time in the file) is the sound of that
 *              very picture in the source, on both channels
 *   silent     of those, the slots with no sound at all (silence, or the
 *              sound track has ended)
 *   wrong      of those, the slots with the sound of ANOTHER picture:
 *              this is what "out of sync" means, and it must be 0
 *   held       slots that repeat the picture before
 *   cut_slot   the slot where the pictures stop being "the next one"
 *   first_new  the picture in that slot
 *   old_sound  sound frames at the start of the file that are the sound
 *              of the source from its start (= the sound before the seek)
 */
typedef struct
{
    uint32_t new_pics;
    uint32_t in_sync;
    uint32_t silent;
    uint32_t wrong;
    uint32_t held;
    int      cut_slot;
    int      first_new;
    int      jumps;             /* how often the picture number went back
                                 * or skipped ahead */
    int      known;             /* every slot is a picture of the source */
    size_t   old_sound;
} SeekCheck;

/* Which picture of the source is this grey level?  The levels are 12
 * apart and a saved one may be 6 off, so a level half-way is read as
 * "the picture after `prev`" or as `prev` itself when that fits. */
static int picture_of(int grey, int prev)
{
    int g;

    if (prev + 1 < SRC_N &&
        abs(grey - grey_of((uint32_t)(prev + 1))) <= GREY_TOL)
        return prev + 1;
    if (prev >= 0 && abs(grey - grey_of((uint32_t)prev)) <= GREY_TOL)
        return prev;
    g = (grey - 16 + 6) / 12;
    if (g < 0 || g >= SRC_N ||
        abs(grey - grey_of((uint32_t)g)) > GREY_TOL)
        return -1;
    return g;
}

static void seek_check(const SavedVideo *s, SeekCheck *c)
{
    uint32_t k;
    int prev = -1;

    memset(c, 0, sizeof(*c));
    c->cut_slot = -1;
    c->first_new = -1;
    c->known = 1;
    while (c->old_sound < s->snd_samples / 2u &&
           c->old_sound < (size_t)(SRC_N * SRC_SPF) &&
           g_out_snd[2u * c->old_sound] == g_src_snd[c->old_sound] &&
           g_out_snd[2u * c->old_sound + 1u] == g_src_snd[c->old_sound])
    {
        c->old_sound++;
    }
    for (k = 0; k < s->pictures; k++)
    {
        int g = picture_of(s->grey[k], prev);
        size_t at = (size_t)k * SRC_SPF;
        size_t j;
        int same = 1;
        int quiet = 1;

        if (g < 0)
        {
            c->known = 0;
            continue;
        }
        if (g == prev)
        {
            c->held++;
            continue;
        }
        if (prev >= 0 && g != prev + 1)
        {
            if (c->jumps == 0)
            {
                c->cut_slot = (int)k;
                c->first_new = g;
            }
            c->jumps++;
        }
        prev = g;
        c->new_pics++;
        for (j = 0; j < (size_t)SRC_SPF; j++)
        {
            int16_t want = g_src_snd[(size_t)g * SRC_SPF + j];

            if ((at + j) * 2u + 1u >= s->snd_samples)
            {
                same = 0;                   /* the sound track ends here */
                break;
            }
            if (g_out_snd[2u * (at + j)] != want ||
                g_out_snd[2u * (at + j) + 1u] != want)
                same = 0;
            if (g_out_snd[2u * (at + j)] != 0 ||
                g_out_snd[2u * (at + j) + 1u] != 0)
                quiet = 0;
        }
        if (same)       c->in_sync++;
        else if (quiet) c->silent++;
        else            c->wrong++;
    }
}

static void seek_print(const SavedVideo *s, const SeekCheck *c)
{
    printf("      (%u slots: %u new pictures, %u held; slot %d shows "
           "picture %d;\n       sound under its picture %u, none %u, "
           "wrong %u; sound %lu frames,\n       the first %lu from the "
           "start of the video)\n", (unsigned)s->pictures,
           (unsigned)c->new_pics, (unsigned)c->held, c->cut_slot,
           c->first_new, (unsigned)c->in_sync, (unsigned)c->silent,
           (unsigned)c->wrong,
           (unsigned long)(s->snd_samples / 2u),
           (unsigned long)c->old_sound);
}

/* ---- [7]: what the speaker gets from a clip ---------------------------- */
static uint32_t heard_samples;
static uint32_t heard_loud;

static void heard_cb(void *ctx, int16_t *pcm, size_t samples,
                     uint32_t sample_rate, uint16_t channels)
{
    size_t i;

    (void)ctx; (void)sample_rate; (void)channels;
    for (i = 0; i < samples; i++)
    {
        if (pcm[i] > 500 || pcm[i] < -500) heard_loud++;
    }
    heard_samples += (uint32_t)samples;
}

/* Puts one test vector alone into the video folder under `as`, plays it
 * to its end and counts what reached the sound callback.  Returns the
 * number of loud samples, or -1 when the clip did not start. */
static long play_and_listen(const char *vector, const char *as)
{
    char dst[96];
    int loops = 0;
    int started = 0;
    FM_Result r;

    (void)Directory_DeleteRecursive(SIMPLE_VIDEO_FOLDER);
    (void)Directory_Create(SIMPLE_VIDEO_FOLDER);
    (void)snprintf(dst, sizeof(dst), "%s/%s", SIMPLE_VIDEO_FOLDER, as);
    if (File_Copy(vector, dst, NULL, NULL) != FM_OK) return -1;
    heard_samples = 0u;
    heard_loud = 0u;
    sim_video_clock(1u);
    (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    (void)SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP);
    SimpleVideo_SetFrameCallback(NULL, NULL);
    SimpleVideo_SetAudioDataCallback(heard_cb, NULL);
    r = SimpleVideo_Begin();
    if (r == FM_OK) r = SimpleVideo_Start();
    if (r != FM_OK)
    {
        SimpleVideo_SetAudioDataCallback(NULL, NULL);
        return -1;
    }
    /* Start() takes effect in Poll; at the end of the one clip the
     * player stops (play mode STOP) */
    while (loops < 20000)
    {
        (void)SimpleVideo_Poll();
        loops++;
        if (SimpleVideo_IsRunning()) started = 1;
        else if (started) break;
    }
    (void)SimpleVideo_Stop();
    SimpleVideo_SetAudioDataCallback(NULL, NULL);
    return (long)heard_loud;
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

    /* ---- 5b) a seek while the video is being saved ------------------ */
    printf("\n[5b] SimpleVideo_SeekTo in the middle of a recording\n");
    {
        static SavedVideo s;
        SeekCheck c;
        int loops;
        long snd_frames;
        long gap;

        /* forward: after 5 pictures jump to 1.2 s (picture 12) */
        sim_video_clock(1u);
        loops = play_seek_save("/save/fwd.avi", 5u, 1.2);
        r = inspect_video("/save/fwd.avi", &s);
        seek_check(&s, &c);
        seek_print(&s, &c);
        snd_frames = (long)(s.snd_samples / 2u);
        /* the picture before the cut is held for the sound the player
         * had already decoded, and for less than one picture more */
        gap = (long)c.cut_slot * SRC_SPF - (long)c.old_sound;
        ok("forward seek: one file, closed by itself at the end",
           loops > 0 && loops < 200000 && r == FM_OK &&
           save_demo_video.cuts == 1u && s.vi.frame_count == s.pictures);
        ok("  it is a cut: picture 12 follows, then 13..19 in order",
           c.known && c.jumps == 1 && c.first_new == 12 &&
           abs(s.grey[s.pictures - 1u] - grey_of(SRC_N - 1)) <= GREY_TOL);
        ok("  the picture is not frozen for the length of the jump",
           c.cut_slot > 0 && gap >= 0 && gap < SRC_SPF);
        ok("  every picture has its own sound under it, also after it",
           c.new_pics == 13u && c.in_sync == c.new_pics && c.wrong == 0u);
        ok("  picture track and sound track have the same length",
           labs(snd_frames - (long)s.pictures * SRC_SPF) <= SRC_SPF);
        ok("  no sound was dropped", save_demo_video.ring.dropped == 0u);

        /* backward: after 12 pictures jump back to 0.3 s (picture 3) */
        sim_video_clock(1u);
        loops = play_seek_save("/save/back.avi", 12u, 0.3);
        r = inspect_video("/save/back.avi", &s);
        seek_check(&s, &c);
        seek_print(&s, &c);
        snd_frames = (long)(s.snd_samples / 2u);
        gap = (long)c.cut_slot * SRC_SPF - (long)c.old_sound;
        ok("backward seek: the file goes on (it used to end there)",
           loops > 0 && r == FM_OK && save_demo_video.cuts == 1u &&
           s.pictures > SRC_N && s.vi.frame_count == s.pictures);
        ok("  picture 3 follows, then everything to the end, in order",
           c.known && c.jumps == 1 && c.first_new == 3 &&
           gap >= 0 && gap < SRC_SPF &&
           abs(s.grey[s.pictures - 1u] - grey_of(SRC_N - 1)) <= GREY_TOL);
        ok("  every picture has its own sound under it",
           c.new_pics == 12u + 17u && c.in_sync == c.new_pics &&
           c.wrong == 0u);
        ok("  picture track and sound track have the same length",
           labs(snd_frames - (long)s.pictures * SRC_SPF) <= SRC_SPF);

        /* A player that is too slow, and a seek.  It drops pictures (so
         * the picture number skips more than once), and after the seek
         * its sound starts later in the video than its picture: by then
         * its clock has run on for the time the picture took to decode.
         * So the first picture after the seek has no sound of its own
         * (the player never made it: silence), and the video is over
         * before the player has decoded the last of the sound.  What
         * must hold: no picture ever has the sound of another one. */
        sim_video_clock(45u);
        loops = play_seek_save("/save/slowseek.avi", 4u, 1.0);
        r = inspect_video("/save/slowseek.avi", &s);
        seek_check(&s, &c);
        seek_print(&s, &c);
        snd_frames = (long)(s.snd_samples / 2u);
        ok("slow player + seek: one complete file",
           loops > 0 && r == FM_OK && save_demo_video.cuts == 1u &&
           c.known && s.vi.frame_count == s.pictures &&
           abs(s.grey[s.pictures - 1u] - grey_of(SRC_N - 1)) <= GREY_TOL);
        ok("  no picture has the sound of another picture",
           c.wrong == 0u && c.in_sync >= 3u &&
           c.in_sync + c.silent == c.new_pics);
        ok("  the sound track is not longer than the picture track",
           snd_frames > 0 &&
           snd_frames <= (long)s.pictures * SRC_SPF + SRC_SPF);
        /* The sound of the new place starts later than its picture here,
         * and the cut writes that distance as silence (the branch
         * "snd_at > pic_at" of save_demo_video_cut).  Without it the
         * checks above still hold -- the sound simply sits one picture
         * early: 3 in sync, 3 silent, 32 000 sound frames.  The clock of
         * this test is simulated, so the figures are exact. */
        ok("  the sound that starts late is put behind its silence",
           c.in_sync == 4u && c.silent == 2u && snd_frames == 43392L);
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
    /* ---- 7) the playback side of SimpleVideo ------------------------ */
    printf("\n[7] SimpleVideo: tracks, containers and the clock\n");
    {
        FM_AudioTrackInfo a;
        long loud;

        /* The tone of every vector is far above 500; silence is 0.  A
         * second of 8 kHz mono sound is 8000 samples. */
        loud = play_and_listen("/test/vectors/avi_mp3.avi", "a.avi");
        ok("an MP3 track in AVI is heard (it was silence)", loud > 4000);
        loud = play_and_listen("/test/vectors/avi_pcm24.avi", "a.avi");
        ok("a 24-bit PCM track is heard (it was silence)", loud > 1000);
        loud = play_and_listen("/test/vectors/mov_pcm_s8.mov", "a.mov");
        ok("an 8-bit PCM track is heard (the I2S never started)", loud > 1000);
        loud = play_and_listen("/test/vectors/mov_pcm_s16.mov", "a.mov");
        ok("a 16-bit PCM track is heard, as before", loud > 1000);
        /* the folder scan */
        loud = play_and_listen("/test/vectors/mkv_mp3_delay.mkv", "a.mkv");
        ok("a folder with only an .mkv plays, with its sound", loud > 1000);
        loud = play_and_listen("/test/vectors/mkv_vorbis.mkv", "a.webm");
        ok("a folder with only a .webm is found", loud >= 0);

        /* which tracks the speaker takes: the library's `decodable`
         * decides, not the width of the samples in the file */
        memset(&a, 0, sizeof(a));
        a.codec = FM_VIDEO_AUDIO_MP3; a.decodable = FM_TRUE;
        ok("MP3, width 0, decodable: played", sv_track_decodes_inline(&a));
        a.codec = FM_VIDEO_AUDIO_PCM; a.bits_per_sample = 24u;
        ok("PCM 24 bit, decodable: played", sv_track_decodes_inline(&a));
        a.bits_per_sample = 12u; a.decodable = FM_FALSE;
        ok("PCM 12 bit, not decodable: not played",
           !sv_track_decodes_inline(&a));
        a.codec = FM_VIDEO_AUDIO_ADPCM; a.bits_per_sample = 16u;
        ok("ADPCM the library cannot decode: not played",
           !sv_track_decodes_inline(&a));

        /* where the sound restarts after a seek: 30 s into a 60 s PCM
         * track of 22050 Hz (1 323 000 packets).  In 32 bits the product
         * wrapped and the answer was packet 17 369 (0.79 s). */
        ok("seek: sound packet at 30 s of 60 s, 1 323 000 packets",
           sv_audio_index_at(30000u, 1323000u, 60000u) == 661500u);
        ok("seek: 1500 s into a 30 min AVI of 19 380 packets",
           sv_audio_index_at(1500000u, 19380u, 1800000u) == 16150u);
        ok("seek: past the end is the last packet",
           sv_audio_index_at(70000u, 1323000u, 60000u) == 1322999u);

        /* the picture that is due: the exact rate, not the rounded one */
        ok("29.97 fps: after 30 min picture 53 946 is due, not 54 000",
           sv_frame_at_ms(1800000u, 30000u, 1001u) == 53946u);
        ok("12.5 fps: after 60 s picture 750 is due, not 780",
           sv_frame_at_ms(60000u, 1000000u, 80000u) == 750u);
        ok("25 fps: unchanged", sv_frame_at_ms(60000u, 25u, 1u) == 1500u);
        ok("49 days of 60 fps do not wrap",
           sv_frame_at_ms(0xFFFFFFFFu, 60u, 1u) == 257698037u);
        ok("the time of a picture is the inverse",
           sv_ms_of_frame(53946u, 30000u, 1001u) == 1799998u &&
           sv_ms_of_frame(750u, 1000000u, 80000u) == 60000u);
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
