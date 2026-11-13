#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "file_manager.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
#include "stm32h7xx_hal.h"
I2S_HandleTypeDef hi2s1; I2S_HandleTypeDef hi2s2;
#include "simple_audio.h"
/* Main loop blocked for `stall_ms` before every Poll; the I2S interrupts
 * are raised while it is blocked, as a real DMA would. */
int main(int argc,char**argv){
  FM_FS fs; FM_Result r; int n, stall = (argc>1)?atoi(argv[1]):50;
  int starts=0;
  (void)FileManager_Init();(void)PosixFS_Init(&fs);(void)FS_Mount("/",&fs);
  r=SimpleAudio_Begin(); printf("begin %d\n",(int)r); if(r) return 1;
  printf("half=%u samples, rate=%u, half period = %.1f ms\n",(unsigned)SIMPLE_AUDIO_HALF_SAMPLES,
         (unsigned)sa.dma.sample_rate,
         1000.0*(double)SIMPLE_AUDIO_HALF_SAMPLES/((double)sa.dma.sample_rate*(double)sa.dma.channels));
  for(n=0;n<400 && SimpleAudio_IsRunning();n++){
    if(n&1) AudioDma_FullCallback(&sa.dma); else AudioDma_HalfCallback(&sa.dma);
    usleep((useconds_t)stall*1000);
    r=SimpleAudio_Poll();
    starts = (int)sa.refill_count;
    if(r!=FM_OK && r!=FM_ERR_NOT_READY){ printf("poll %d at %d\n",(int)r,n); }
  }
  printf("stall %d ms: halves=%d restarts=%u refills=%u running=%d\n", stall, n,
         (unsigned)sa.restart_count,(unsigned)sa.refill_count,(int)SimpleAudio_IsRunning());
  (void)SimpleAudio_Stop(); (void)starts; return 0;}
