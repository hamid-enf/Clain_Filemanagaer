#!/bin/sh
# Build a harness against a tree that is NOT the audited checkout (e.g.
# /tmp/lib_all, which has all four edits applied).
#   build_against.sh <tree> <harness.c> <out>
set -e
T=$1
make -C "$T" -j2 libfilemanager.a \
  CFLAGS="-I$T/include -I$T/backends -I$T/src -I$T/third_party/littlefs \
  -I$T/third_party/h264bsd/src -I$T/third_party/minimp3 -I$T/third_party/picojpeg \
  -I$T/third_party/dr_flac -I$T/third_party/stb -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
  -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 -DFM_USE_FATFS=0 \
  -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 -DFM_USE_JSON=1 -DFM_USE_TEXT=1 \
  -DFM_USE_BINARY=1 -DFM_USE_ASYNC=1 -DFM_USE_SAFE_WRITE=1 -DFM_USE_CRC=1 \
  -DFM_USE_STREAM=1 -DFM_USE_CACHE=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 \
  -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -fno-omit-frame-pointer" >/tmp/against_build.log 2>&1 \
  || { tail -25 /tmp/against_build.log; exit 1; }
gcc -std=gnu99 -O1 -g -fsanitize=address,undefined -D_GNU_SOURCE \
  -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 \
  -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 -DFM_USE_TEXT=1 -DFM_USE_STREAM=1 \
  -DFM_USE_SAFE_WRITE=1 -DFM_USE_EXPLORER=1 -DFM_USE_JSON=1 -DFM_USE_CACHE=1 \
  -DFM_USE_CRC=1 -DFM_USE_ASYNC=1 -DFM_USE_BINARY=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 \
  -I"$T/include" -I"$T/backends" -I"$T/src" -o "$3" "$2" "$T/libfilemanager.a" -lm
