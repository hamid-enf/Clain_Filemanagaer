/*
 * Host runner for examples/examples_sdcard_player.h — creates two
 * short WAV files in /music (like songs on the real card), then runs the
 * three playback-control examples.
 */

#include <math.h>
#include "../examples/examples_sdcard.h"
#include "../examples/examples_sdcard_player.h"
#include "../examples/audio_dma_player.h"

typedef struct
{
    uint32_t starts;
    uint32_t stops;
    uint32_t cache_cleans;
    uint32_t sample_rate;
    uint16_t channels;
    uint8_t next_half;
} FakeAudioDma;

static FM_Result fake_audio_start(void *context, int16_t *samples,
                                  size_t sample_count, uint32_t sample_rate,
                                  uint16_t channels)
{
    FakeAudioDma *fake = (FakeAudioDma *)context;

    if (samples == NULL || sample_count == 0) return FM_ERR_INVALID_ARGUMENT;
    fake->starts++;
    fake->sample_rate = sample_rate;
    fake->channels = channels;
    fake->next_half = 0;
    return FM_OK;
}

static FM_Result fake_audio_stop(void *context)
{
    FakeAudioDma *fake = (FakeAudioDma *)context;

    fake->stops++;
    return FM_OK;
}

static void fake_audio_clean(void *context, const void *address, size_t bytes)
{
    FakeAudioDma *fake = (FakeAudioDma *)context;

    if (address != NULL && bytes != 0) fake->cache_cleans++;
}

static int test_audio_dma_bridge(void)
{
    static Ex13_AudioDma audio;
    static int16_t dma_buffer[2 * 128]
        __attribute__((aligned(32)));
    static const Ex13_AudioDmaPort port = {
        fake_audio_start, fake_audio_stop, fake_audio_clean
    };
    FakeAudioDma fake;
    FM_Result r;
    uint32_t ticks;

    memset(&fake, 0, sizeof(fake));
    r = Ex13_AudioDmaStart(&audio, "/music", &port, &fake,
                           dma_buffer, 128);
    if (r != FM_OK || fake.starts != 1 || fake.sample_rate != 8000 ||
        fake.channels != 1 || !Ex13_AudioDmaIsRunning(&audio))
    {
        printf("audio DMA bridge start FAILED (%d)\n", (int)r);
        (void)Ex13_AudioDmaStop(&audio);
        return 1;
    }

    /* Simulate circular-DMA IRQs. A 4000-sample track must drain, stop,
     * advance, reconfigure, and start the second track automatically. */
    for (ticks = 0; ticks < 64 && fake.starts < 2; ticks++)
    {
        uint8_t half = fake.next_half;

        fake.next_half ^= 1u;
        if (half == 0)
            Ex13_AudioDmaHalfCallback(&audio);
        else
            Ex13_AudioDmaFullCallback(&audio);

        r = Ex13_AudioDmaPoll(&audio);
        if (r != FM_OK) break;
    }

    if (r != FM_OK || fake.starts < 2 || fake.stops < 1 ||
        fake.cache_cleans == 0 || !Ex13_AudioDmaIsRunning(&audio))
    {
        printf("audio DMA bridge refill/next FAILED (%d)\n", (int)r);
        (void)Ex13_AudioDmaStop(&audio);
        return 1;
    }

    r = Ex13_AudioDmaStop(&audio);
    if (r != FM_OK || fake.stops < 2 || Ex13_AudioDmaIsRunning(&audio))
    {
        printf("audio DMA bridge stop FAILED (%d)\n", (int)r);
        return 1;
    }

    printf("audio DMA bridge: refill + auto-next OK\n");
    return 0;
}

static void make_two_wavs(void)
{
    const char *names[2] = { "/music/track_1.wav", "/music/track_2.wav" };
    int n;

    Directory_Create("/music");

    for (n = 0; n < 2; n++)
    {
        FM_AudioWriter w;
        int16_t chunk[512];
        uint32_t i;

        if (AudioWriter_Create(names[n], 8000, 1, 16, &w) != FM_OK) return;
        for (i = 0; i < 4000u; i += 512u)
        {
            uint32_t k, len = (4000u - i < 512u) ? (4000u - i) : 512u;
            for (k = 0; k < len; k++)
            {
                double t = (double)(i + k) / 8000.0;
                chunk[k] = (int16_t)(5000.0 *
                    sin(2.0 * 3.14159265358979 * (440.0 + 110.0 * n) * t));
            }
            (void)AudioWriter_Write(&w, chunk, len, NULL);
        }
        (void)AudioWriter_Close(&w);
    }
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

    make_two_wavs();

    exP_1_music_controls();
    exP_2_video_controls();
    exP_3_remote_buttons();
    if (test_audio_dma_bridge() != 0)
    {
        SD_examples_end();
        return 1;
    }

    SD_examples_end();
    printf("\n===== ALL PLAYER EXAMPLES DONE OK =====\n");
    return 0;
}
