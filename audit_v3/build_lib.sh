#!/bin/sh
# Build libfilemanager.a (ASan+UBSan) with the project's own make.
set -e
LIB=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
cd "$LIB"
make clean >/dev/null 2>&1 || true
make -j2 libfilemanager.a \
  CFLAGS="-Iinclude -Ibackends -Isrc -Ithird_party/littlefs -Ithird_party/h264bsd/src \
  -Ithird_party/minimp3 -Ithird_party/picojpeg -Ithird_party/dr_flac -Ithird_party/stb \
  -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 \
  -DFM_USE_FATFS=0 -DFM_USE_LITTLEFS=1 -DFM_USE_MP3=1 -DFM_USE_EEPROM=1 -DFM_USE_USB_MSC=1 \
  -DFM_USE_DEVICE_ADAPTERS=1 -DFM_USE_THREADS=${THREADS:-0} -DFM_USE_JSON=1 -DFM_USE_TEXT=1 \
  -DFM_USE_BINARY=1 -DFM_USE_DATA_LOGGER=1 -DFM_USE_EXPLORER=1 -DFM_USE_ASYNC=1 \
  -DFM_USE_MEDIA_DETECT=1 -DFM_USE_SAFE_WRITE=1 -DFM_USE_CRC=1 -DFM_USE_STREAM=1 \
  -DFM_USE_CACHE=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 -std=c99 -O1 -g -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer ${EXTRA:-}" >/tmp/build.log 2>&1 \
  || { tail -40 /tmp/build.log; exit 1; }
echo built
