#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
extern FM_Result FM_Opus_Init(void);
static FM_FS g_fs; static FM_Storage g_ram; static unsigned char rb[64*1024];
int main(int argc,char**argv){FM_Audio a;FM_Result r;size_t got;int16_t s[4096];
 (void)argc;(void)FileManager_Init();(void)RamDisk_Register(&g_ram,"ram",rb,sizeof rb);
 (void)Storage_Mount("ram");(void)PosixFS_Init(&g_fs);(void)FS_Mount("/",&g_fs);
 (void)FM_Opus_Init();
 if(Audio_Open(argv[1],&a)){printf("open fail\n");return 1;}
 for(;;){r=Audio_Read(&a,s,4096,&got);fwrite(s,2,got,stdout);if(r!=FM_OK)break;}
 Audio_Close(&a);return 0;}
