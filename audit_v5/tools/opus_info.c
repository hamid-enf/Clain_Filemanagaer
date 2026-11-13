#include <stdio.h>
#include "file_manager.h"
#include "fm_audio.h"
extern FM_Result FM_Opus_Init(void);
static FM_FS g_fs;
int main(int argc,char**argv){FM_Audio a;FM_Result r;(void)argc;(void)FileManager_Init();(void)PosixFS_Init(&g_fs);(void)FS_Mount("/",&g_fs);
 (void)FM_Opus_Init(); r=Audio_Open(argv[1],&a); printf("open %d",(int)r); if(!r){printf(" codec=%s total=%llu",a.info.codec,(unsigned long long)a.info.total_frames);Audio_Close(&a);} printf("\n");return 0;}
