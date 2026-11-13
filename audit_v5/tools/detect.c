#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_media.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
static const char *names[] = {"flac_id3","ogg_id3","wav_id3","mp3_id3","flac_id3_big","ogg_id3_big","wav_id3_big"};
int main(void)
{
    FM_FS fs; unsigned i; static uint8_t buf[1<<20]; size_t rd=0;
    (void)FileManager_Init(); (void)PosixFS_Init(&fs); (void)FS_Mount("/", &fs);
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        char p[128];
        snprintf(p, sizeof p, "/home/user/aud5/tagtest/%s.bin", names[i]);
        { FILE *f = fopen(p, "rb"); rd = f ? fread(buf, 1, sizeof buf, f) : 0;
          if (f) fclose(f); }
        printf("  %-13s len=%zu detect=%d (BINARY=6 MP3=13 FLAC=15 OGG=16 WAV=12)\n",
               names[i], rd, (int)Media_DetectBuffer(buf, rd, p));
    }
    return 0;
}
