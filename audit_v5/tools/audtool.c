/* External CLI over the real library: info | dec | seek | mono | s16 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
static FM_FS g_fs; static FM_Storage g_ram; static uint8_t rbuf[64*1024];
int main(int argc,char**argv){
  FM_Audio a; FM_AudioInfo info; FM_Result r; size_t got; int16_t s[4096];
  (void)FileManager_Init(); (void)RamDisk_Register(&g_ram,"ram",rbuf,sizeof rbuf);
  (void)Storage_Mount("ram"); (void)PosixFS_Init(&g_fs); (void)FS_Mount("/",&g_fs);
  if(argc<3){fprintf(stderr,"usage: audtool cmd file [arg]\n");return 2;}
  r=Audio_Open(argv[2],&a);
  if(r!=FM_OK){printf("open: %d (%s)\n",(int)r,FM_ResultString(r));return 1;}
  (void)Audio_GetInfo(&a,&info);
  if(!strcmp(argv[1],"info")){
    printf("codec=%s rate=%u ch=%u bits=%u data_off=%llu data_size=%llu total_frames=%llu block_align=%u\n",
      info.codec,info.sample_rate,info.channels,info.bits_per_sample,
      (unsigned long long)info.data_offset,(unsigned long long)info.data_size,
      (unsigned long long)info.total_frames,info.block_align);
  } else if(!strcmp(argv[1],"seek")){
    r=Audio_Seek(&a,atof(argv[3])); printf("seek %s -> %d frame_index=%llu\n",argv[3],(int)r,
      (unsigned long long)a.frame_index);
  } else {
    if(argc>3 && strcmp(argv[3],"-")){ r=Audio_Seek(&a,atof(argv[3]));
      if(r){printf("seek %d\n",r);return 1;} }
    for(;;){ r=Audio_Read(&a,s,4096,&got);
      if(!strcmp(argv[1],"mono")||!strcmp(argv[1],"s16")) fwrite(s,2,got,stdout);
      if(r!=FM_OK) break; }
    fprintf(stderr,"end %d\n",(int)r);
  }
  Audio_Close(&a); return 0;
}
