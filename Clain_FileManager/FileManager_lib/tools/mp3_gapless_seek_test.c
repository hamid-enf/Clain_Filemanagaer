/* FM-NOT-FIRMWARE: host-only MP3 differential and seek regression test. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
extern FM_Result PosixFS_Init(FM_FS *fs);

static int check(FM_Audio *a, const int16_t *ref, size_t start, size_t frames,
                 unsigned channels)
{
    int16_t buf[514];
    size_t done = 0;
    while (done < frames) {
        size_t requested = ((done / 2u) % 257u + 1u) * channels;
        size_t got = 0, i;
        FM_Result r;
        if (requested > (frames - done) * channels) requested = (frames - done) * channels;
        r = Audio_Read(a, buf, requested, &got);
        if (r != FM_OK || got == 0 || got % channels || got > requested ||
            got > (frames - done) * channels) return 1;
        for (i = 0; i < got; ++i) {
            int delta = (int)buf[i] - (int)ref[(start + done) * channels + i];
            if (delta < -2 || delta > 2) {
                fprintf(stderr, "PCM mismatch frame=%zu channel=%zu delta=%d\n",
                        start + done + i / channels, i % channels, delta);
                return 1;
            }
        }
        done += got / channels;
        if (a->frame_index != start + done) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    FM_FS fs;
    FM_Audio a;
    FILE *f;
    int16_t *ref;
    size_t frames, bytes, i;
    unsigned rate, channels;
    size_t seeks[6];
    FM_Result r;
    if (argc != 6) return 2;
    rate = (unsigned)strtoul(argv[3], NULL, 10);
    channels = (unsigned)strtoul(argv[4], NULL, 10);
    frames = (size_t)strtoull(argv[5], NULL, 10);
    if (!rate || !channels || channels > 2 || frames < 4) return 2;
    bytes = frames * channels * sizeof(*ref);
    ref = malloc(bytes);
    f = fopen(argv[2], "rb");
    if (!ref || !f || fread(ref, 1, bytes, f) != bytes || fgetc(f) != EOF) return 2;
    fclose(f);
    if (FileManager_Init() != FM_OK || PosixFS_Init(&fs) != FM_OK ||
        FS_Mount("/", &fs) != FM_OK) return 2;
    memset(&a, 0, sizeof a);
    if (Audio_Open(argv[1], &a) != FM_OK || a.info.sample_rate != rate ||
        a.info.channels != channels || a.info.total_frames != frames ||
        a.info.data_size != bytes) {
        fprintf(stderr, "incorrect metadata: frames=%llu rate=%u channels=%u\n",
                (unsigned long long)a.info.total_frames, (unsigned)a.info.sample_rate,
                (unsigned)a.info.channels);
        return 3;
    }
    if (check(&a, ref, 0, frames, channels)) return 4;
    { int16_t b[2]; size_t got = 7;
      if (Audio_Read(&a, b, channels, &got) != FM_ERR_EOF || got != 0) return 5; }
    seeks[0] = 0; seeks[1] = 1; seeks[2] = frames / 3;
    seeks[3] = frames / 2; seeks[4] = frames - 1; seeks[5] = 0;
    for (i = 0; i < 6; ++i) {
        size_t start = seeks[i], n = frames - start;
        if (n > 333) n = 333;
        r = Audio_Seek(&a, ((double)start + 0.01) / rate);
        if (r != FM_OK || a.frame_index != start || check(&a, ref, start, n, channels)) {
            fprintf(stderr, "seek failed at %zu, result=%d position=%llu\n", start, (int)r,
                    (unsigned long long)a.frame_index);
            return 6;
        }
    }
    r = Audio_Seek(&a, ((double)frames + 1.0) / rate);
    if (r != FM_ERR_EOF || a.frame_index != frames) return 7;
    { int16_t b[2]; size_t got = 7;
      if (Audio_Read(&a, b, channels, &got) != FM_ERR_EOF || got != 0) return 8; }
    if (Audio_Seek(&a, 0.0) != FM_OK || check(&a, ref, 0, 4, channels)) return 9;
    Audio_Close(&a);
    free(ref);
    return 0;
}
#endif
