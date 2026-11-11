#!/bin/sh
# Build a harness against the UNSANITIZED library (the log buffer size is a
# compile-time constant, so a -DFM_LOG_BUF_SIZE harness must be linked with a
# library built with the same value).
#   build_log_harness.sh <harness.c> <out> [extra flags]
set -e
LIB=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
gcc -std=gnu99 -O1 -g -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 \
  -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 \
  -DFM_USE_TEXT=1 -DFM_USE_JSON=1 $3 \
  -I$LIB/include -I$LIB/backends -I$LIB/src -o "$2" "$1" $LIB/libfilemanager.a -lm
