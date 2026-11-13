/* In-process mutation fuzzer over Audio_Open/Read/Seek.  ASan+UBSan build.
 *   fuzzloop <seed-file> <iterations> [maxreads]
 * Every iteration mutates a copy of the seed in memory, writes it to the
 * ram disk, opens it, reads it to the end, seeks around, reads again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "file_manager.h"
#include "fm_audio.h"

static FM_FS g_fs; static FM_Storage g_ram; static uint8_t *rdisk;
static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(int argc, char **argv)
{
    FM_Audio a; FM_Result r; size_t got; int16_t pcm[4096];
    static uint8_t seed[8u << 20], buf[8u << 20];
    size_t n = 0, i;
    long iters = 1000, it;
    FILE *f;

    if (argc < 2) { fprintf(stderr, "usage: %s seed [iters]\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror("seed"); return 2; }
    n = fread(seed, 1, sizeof seed, f);
    fclose(f);
    iters = (argc > 2) ? atol(argv[2]) : 1000;
    rdisk = calloc(1, 16u << 20);
    (void)FileManager_Init();
    (void)RamDisk_Register(&g_ram, "ram", rdisk, 16u << 20);
    (void)Storage_Mount("ram");
    (void)PosixFS_Init(&g_fs);
    (void)FS_Mount("/", &g_fs);

    for (it = 0; it < iters; it++) {
        size_t len = n, k, muts = 1 + (rnd() % 8);
        memcpy(buf, seed, n);
        for (k = 0; k < muts; k++) {
            uint32_t op = rnd() % 5;
            size_t at = rnd() % (len ? len : 1);
            switch (op) {
            case 0: if (len) buf[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: if (len) buf[at] = (uint8_t)rnd(); break;
            case 2: if (len) len = at ? at : 1; break;            /* truncate  */
            case 3: if (len + 4 < sizeof buf) {                   /* insert 4  */
                        memmove(buf + at + 4, buf + at, len - at);
                        buf[at] = (uint8_t)rnd(); buf[at+1] = (uint8_t)rnd();
                        buf[at+2] = (uint8_t)rnd(); buf[at+3] = (uint8_t)rnd();
                        len += 4; }
                    break;
            default: if (len > 4 && len - at >= 4) {             /* delete 4 */
                        memmove(buf + at, buf + at + 4, len - at - 4);
                        len -= 4; }
                    break;
            }
        }
        (void)File_WriteAll("/fz.bin", buf, (uint32_t)len);
        r = Audio_Open("/fz.bin", &a);
        if (r != FM_OK) continue;
        {
            unsigned reads = 0;
            while ((r = Audio_Read(&a, pcm, 4096, &got)) == FM_OK && got) {
                if (++reads > 4000) break;
            }
            (void)Audio_Seek(&a, 0.37);
            (void)Audio_Read(&a, pcm, 4096, &got);
            (void)Audio_Seek(&a, -5.0);
            (void)Audio_Seek(&a, 1e18);
            (void)Audio_Read(&a, pcm, 1, &got);
            (void)Audio_Close(&a);
        }
        if ((it % 1000) == 0) fprintf(stderr, "%ld", it);
        (void)i;
    }
    fprintf(stderr, "\ndone %ld iterations on %s\n", iters, argv[1]);
    return 0;
}
