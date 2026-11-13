/* Audio_Seek self-consistency: samples after a seek must equal the same
 * position of a straight decode.  usage: seekself <file> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
static FM_FS g_fs; static FM_Storage g_ram; static uint8_t buf[64*1024];
int main(int argc, char **argv)
{
    FM_Audio a; FM_Result r; int16_t *whole; size_t n = 0, got, k, at;
    double t, dur; int bad = 0, seeks = 0;
    (void)argc;
    (void)FileManager_Init(); (void)RamDisk_Register(&g_ram, "ram", buf, sizeof buf);
    (void)Storage_Mount("ram"); (void)PosixFS_Init(&g_fs); (void)FS_Mount("/", &g_fs);
    if (Audio_Open(argv[1], &a) != FM_OK) { printf("open fail\n"); return 1; }
    whole = malloc(200u << 20);
    for (;;) { r = Audio_Read(&a, whole + n, 4096, &got); n += got; if (r != FM_OK) break; }
    dur = (double)(n / a.info.channels) / a.info.sample_rate;
    Audio_Close(&a);
    for (t = 0.0; t < dur; t += 0.0137) {
        uint16_t ch;
        int16_t probe[512];
        if (Audio_Open(argv[1], &a) != FM_OK) { printf("reopen fail\n"); return 1; }
        ch = a.info.channels;
        r = Audio_Seek(&a, t);
        if (r != FM_OK) { Audio_Close(&a); continue; }
        seeks++;
        at = (size_t)(a.frame_index * ch);
        r = Audio_Read(&a, probe, 512, &got);
        Audio_Close(&a);
        if (got > 128) got = 128;
        if (at + got > n) continue;
        for (k = 0; k < got; k++) {
            long d = (long)probe[k] - (long)whole[at + k];
            if (d < 0) d = -d;
            if (d > 2) { if (bad < 4) printf("  mismatch t=%.4f n=%zu\n", t, k); bad++; break; }
        }
    }
    printf("  %-22s %7zu samples %5.2f s, %d seeks, %d mismatches\n",
           strrchr(argv[1], '/') + 1, n, dur, seeks, bad);
    return 0;
}
