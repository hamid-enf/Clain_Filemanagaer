#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
static FM_FS g_fs;
int main(int argc,char**argv){
  FM_Audio a; size_t got; int16_t s[64]; FM_Result r;
  (void)argc;(void)FileManager_Init();(void)PosixFS_Init(&g_fs);(void)FS_Mount("/",&g_fs);
  if(Audio_Open(argv[1],&a)){printf("open fail\n");return 1;}
  r=Audio_Read(&a,s,64,&got);
  printf("read1 %d got %zu fi %llu\n",(int)r,got,(unsigned long long)a.frame_index);
  r=Audio_Seek(&a,1e9);
  printf("seek(1e9) -> %d fi %llu\n",(int)r,(unsigned long long)a.frame_index);
  r=Audio_Read(&a,s,64,&got);
  printf("read after failed seek %d got %zu fi %llu\n",(int)r,got,(unsigned long long)a.frame_index);
  r=Audio_Seek(&a,0.0);
  printf("seek(0) -> %d fi %llu\n",(int)r,(unsigned long long)a.frame_index);
  r=Audio_Read(&a,s,64,&got);
  printf("read2 %d got %zu fi %llu\n",(int)r,got,(unsigned long long)a.frame_index);
  Audio_Close(&a);return 0;
}
