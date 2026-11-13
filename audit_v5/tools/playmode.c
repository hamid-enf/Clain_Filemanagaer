#include <stdio.h>
#include <string.h>
#include "file_manager.h"
extern FM_Result PosixFS_Init(FM_FS *fs);
#include "stm32h7xx_hal.h"
I2S_HandleTypeDef hi2s1; I2S_HandleTypeDef hi2s2;
#include "simple_audio.h"
int main(void){
  FM_FS fs; FM_Result r;
  (void)FileManager_Init();(void)PosixFS_Init(&fs);(void)FS_Mount("/",&fs);
  (void)Directory_Create("/pmr");
  { static uint8_t b[200000]; size_t rd=0;
    File_ReadAll("/test/vectors/wav_alaw_8k.wav",b,sizeof(b),&rd);
    File_WriteAll("/pmr/a.wav",b,rd); }
  r=SimpleAudio_SetPlayMode(FM_PLAY_MODE_NEXT);
  printf("SetPlayMode(NEXT) before Begin -> %d\n",(int)r);
  r=SimpleAudio_Begin();
  printf("Begin -> %d ; dma.play_mode=%d (NEXT=1) ; cfg=%d\n",(int)r,(int)sa.dma.play_mode,(int)SimpleAudio_GetPlayMode());
  (void)SimpleAudio_Stop();
  r=SimpleAudio_Begin();
  printf("after Stop+Begin: Begin=%d dma.play_mode=%d cfg=%d\n",(int)r,(int)sa.dma.play_mode,(int)SimpleAudio_GetPlayMode());
  (void)SimpleAudio_Stop(); (void)Directory_DeleteRecursive("/pmr"); return 0; }
