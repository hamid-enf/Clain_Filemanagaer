#include <stdio.h>
#include <stdlib.h>
#include "file_manager.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
#include "stm32h7xx_hal.h"
I2S_HandleTypeDef hi2s1; I2S_HandleTypeDef hi2s2;
#include "audio_dma_player.h"
static int16_t buf[2*1024] __attribute__((aligned(32)));
static int starts;
static FM_Result ps(void*c,int16_t*s,size_t n,uint32_t r,uint16_t ch){(void)c;(void)s;(void)n;(void)r;(void)ch;starts++;return FM_OK;}
static FM_Result pt(void*c){(void)c;return FM_OK;}
static const AudioDma_Port port={ps,pt,NULL};
int main(int argc,char**argv){
  FM_FS fs; AudioDma x; AudioDma_Config cfg; FM_Result r; int k=1;
  if(argc>1) k=atoi(argv[1]);
  (void)FileManager_Init();(void)PosixFS_Init(&fs);(void)FS_Mount("/",&fs);
  AudioDma_ConfigDefaults(&cfg);
  r=AudioDma_StartConfig(&x,"/home/user/musicT",&port,NULL,buf,1024,&cfg);
  printf("start %d\n",(int)r); if(r)return 1;
  /* the main loop is blocked: the DMA asks for both halves before Poll runs */
  AudioDma_HalfCallback(&x);
  AudioDma_FullCallback(&x);
  if(k>1) { for(int i=0;i<k;i++){AudioDma_HalfCallback(&x);AudioDma_FullCallback(&x);} }
  r=AudioDma_Poll(&x);
  printf("after a stall of %d half-pairs: Poll=%d running=%d underrun=%d\n",k,(int)r,(int)AudioDma_IsRunning(&x),(int)x.underrun);
  return 0;}
