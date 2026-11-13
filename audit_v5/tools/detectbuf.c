#include <stdio.h>
#include "file_manager.h"
int main(void){ const char*b="ID3"; printf("len3 -> %d\n",(int)Media_DetectBuffer((const uint8_t*)b,3,"x.flac"));
 const char*b2="ID3\x04\x00\x00\x00\x00\x00\x40"; printf("len10 -> %d\n",(int)Media_DetectBuffer((const uint8_t*)b2,10,"x.flac")); return 0;}
