/* Play a folder through AudioDma with a host port and record, byte for
 * byte, the samples the sink would have played.
 *   dmaplay <folder> <out.raw> <mode 0..3> <stall_ms> <volume> <output 0..3> [half] [maxhalf]
 * The I2S half/full interrupts are simulated in the main loop; a stall of
 * N ms (real sleep) before each poll simulates a slow super-loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "file_manager.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
#include "stm32h7xx_hal.h"
I2S_HandleTypeDef hi2s1;
I2S_HandleTypeDef hi2s2;
#include "audio_dma_player.h"

static FILE *out;
static size_t half_n;
static int16_t buf[2 * 32768] __attribute__((aligned(32)));
static int starts, ends;
static long captured;

static void tap(void *c, int16_t *p, size_t n, uint32_t r, uint16_t ch)
{ (void)c; (void)r; (void)ch; if (out) { fwrite(p, 2, n, out); captured += (long)n; } }
static void on_start(void *c, const char *p, const FM_AudioInfo *i)
{ (void)c; (void)i; starts++; if (starts < 8) fprintf(stderr, "start %s\n", p); }
static void on_end(void *c, const char *p, const FM_AudioInfo *i)
{ (void)c; (void)i; ends++; if (ends < 8) fprintf(stderr, "end   %s\n", p); }
static FM_Result pstart(void *c, int16_t *s, size_t n, uint32_t r, uint16_t ch)
{ (void)c;(void)s;(void)n;(void)r;(void)ch; return FM_OK; }
static FM_Result pstop(void *c) { (void)c; return FM_OK; }
static const AudioDma_Port port = { pstart, pstop, NULL };

int main(int argc, char **argv)
{
    FM_FS fs; AudioDma x; FM_Result r; AudioDma_Config cfg;
    int n, maxh, stall, errs = 0; long underruns = 0;

    if (argc < 7) { fprintf(stderr, "usage: %s folder out mode stall volume output [half] [maxhalf]\n", argv[0]); return 2; }
    half_n = (argc > 7) ? (size_t)atoi(argv[7]) : 2048u;
    maxh   = (argc > 8) ? atoi(argv[8]) : 400000;
    stall  = atoi(argv[4]);
    (void)FileManager_Init(); (void)PosixFS_Init(&fs);
    if (FS_Mount("/", &fs) != FM_OK) { fprintf(stderr, "mount failed\n"); return 2; }
    out = fopen(argv[2], "wb");
    if (!out) { perror("out"); return 2; }
    AudioDma_ConfigDefaults(&cfg);
    cfg.play_mode = (FM_PlayMode)atoi(argv[3]);
    cfg.volume = (int32_t)atoi(argv[5]);
    cfg.output = (FM_AudioOutput)atoi(argv[6]);
    cfg.monitor_cb = tap;
    cfg.track_start_cb = on_start;
    cfg.track_end_cb = on_end;
    r = AudioDma_StartConfig(&x, argv[1], &port, NULL, buf, half_n, &cfg);
    fprintf(stderr, "start=%d count=%u mode=%d vol=%d\n", (int)r,
            (unsigned)Player_Count(&x.player), (int)cfg.play_mode, (int)cfg.volume);
    if (r != FM_OK) return 1;
    for (n = 0; n < maxh && AudioDma_IsRunning(&x); n++) {
        if ((n & 1) == 0) AudioDma_HalfCallback(&x);
        else              AudioDma_FullCallback(&x);
        r = AudioDma_Poll(&x);
        if (x.underrun) underruns++;
        if (r != FM_OK) {
            errs++;
            fprintf(stderr, "poll half %d -> %d (%s) cur=%u\n", n, (int)r,
                    FM_ResultString(r), (unsigned)Player_Current(&x.player));
            if (errs > 4) break;
        }
        if (stall > 0) usleep((useconds_t)stall * 1000u);
    }
    fprintf(stderr, "halves=%d running=%d starts=%d ends=%d errs=%d underruns=%ld captured=%ld samples\n",
            n, (int)AudioDma_IsRunning(&x), starts, ends, errs, underruns, captured);
    fflush(out); fclose(out);
    (void)AudioDma_Stop(&x);
    return 0;
}
