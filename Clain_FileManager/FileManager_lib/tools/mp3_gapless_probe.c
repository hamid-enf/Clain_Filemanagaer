/* FM-NOT-FIRMWARE: host-only companion of check_mp3_gapless.py. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
int main(int argc, char **argv) {
    FM_FS fs; FM_Audio a; FM_Result r; int16_t pcm[4096]; size_t n;
    if(argc != 2 || FileManager_Init()!=FM_OK || PosixFS_Init(&fs)!=FM_OK || FS_Mount("/",&fs)!=FM_OK) return 2;
    memset(&a,0,sizeof a); r=Audio_Open(argv[1],&a); if(r!=FM_OK){fprintf(stderr,"open %d\n",r);return 3;}
    fprintf(stderr,"rate=%u channels=%u info_frames=%llu\n",(unsigned)a.info.sample_rate,(unsigned)a.info.channels,(unsigned long long)a.info.total_frames);
    do {n=0;r=Audio_Read(&a,pcm,4096,&n);if(n && fwrite(pcm,sizeof pcm[0],n,stdout)!=n)return 4;} while(r==FM_OK && n);
    Audio_Close(&a);return r==FM_ERR_EOF||r==FM_OK?0:5;
}

#endif /* USE_HAL_DRIVER */
