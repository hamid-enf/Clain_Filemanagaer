#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_manager.h"
#include "fm_audio.h"
static FM_FS g_fs;
int main(int argc,char**argv){
  FM_Audio a; FM_Result r; int16_t *whole; size_t n=0,got; double t,dur;
  (void)argc;
  (void)FileManager_Init();(void)PosixFS_Init(&g_fs);(void)FS_Mount("/",&g_fs);
  if(Audio_Open(argv[1],&a)!=FM_OK){printf("open fail\n");return 1;}
  whole=malloc(200u<<20);
  for(;;){r=Audio_Read(&a,whole+n,4096,&got);n+=got;if(r!=FM_OK)break;}
  dur=(double)(n/a.info.channels)/a.info.sample_rate; Audio_Close(&a);
  for(t=0.5;t<dur-1.0;t+=1.0){
    FM_Audio b; uint16_t ch; size_t i; int16_t probe[8192]; long best=-1; double bestd=1e18;
    if(Audio_Open(argv[1],&b)!=FM_OK)return 1;
    ch=b.info.channels;
    if(Audio_Seek(&b,t)!=FM_OK){Audio_Close(&b);continue;}
    { unsigned long long fi=b.frame_index; double rep=(double)fi/b.info.sample_rate;
      Audio_Read(&b,probe,4096,&got);
      for(long lag=-22050;lag<=22050;lag++){
        long s=(long)fi*ch+lag;
        if(s<0||(size_t)(s+1024)>n)continue;
        double d=0; for(i=0;i<1024;i++) d+=fabs((double)probe[i]-(double)whole[s+i]);
        if(d<bestd){bestd=d;best=lag;}
      }
      printf("%.2f req  reported %.4f  truth %.4f  error %+.3f s\n",t,rep,((double)fi+best)/b.info.sample_rate,rep-((double)fi+best)/b.info.sample_rate);
    }
    Audio_Close(&b);
  }
  free(whole); return 0;}
