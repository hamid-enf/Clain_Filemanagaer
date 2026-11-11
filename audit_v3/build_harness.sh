#!/bin/sh
# build_harness.sh <source.c> <output>  -- compile a harness against the lib
set -e
LIB=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
gcc -std=gnu99 -O1 -g -fsanitize=address,undefined \
  -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 \
  -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 -DFM_USE_TEXT=1 -DFM_USE_STREAM=1 \
  -DFM_USE_SAFE_WRITE=1 -DFM_USE_EXPLORER=1 -DFM_USE_JSON=1 -DFM_USE_CACHE=1 \
  -DFM_USE_CRC=1 -DFM_USE_ASYNC=1 -DFM_USE_BINARY=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 \
  -I$LIB/include -I$LIB/backends -I$LIB/src \
  -o "$2" "$1" $LIB/libfilemanager.a -lm
