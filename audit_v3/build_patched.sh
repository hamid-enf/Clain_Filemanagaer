#!/bin/sh
# Build a *patched copy* of the library and a harness against it.
#   build_patched.sh <patchfile> <harness.c> <out>
set -e
SRC=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
DST=/tmp/lib_patched
rm -rf "$DST"
mkdir -p "$DST"
tar -C "$SRC" -cf - --exclude='*.o' --exclude='*.a' . | tar -C "$DST" -xf -
# $1 is a python mutation script (or "-"): it runs with the copied tree as
# its working directory and edits files in place.  Patches are applied as
# edits, not with patch(1), because the library sources use CRLF endings.
EDIT=""
if [ -n "$1" ] && [ "$1" != "-" ]; then
  EDIT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
fi
if [ -n "$EDIT" ]; then
  ( cd "$DST" && python3 "$EDIT" )
fi
cd "$DST"
make -j2 libfilemanager.a \
  CFLAGS="-Iinclude -Ibackends -Isrc -Ithird_party/littlefs -Ithird_party/h264bsd/src \
  -Ithird_party/minimp3 -Ithird_party/picojpeg -Ithird_party/dr_flac -Ithird_party/stb \
  -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 \
  -DFM_USE_FATFS=0 -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 -DFM_USE_JSON=1 \
  -DFM_USE_TEXT=1 -DFM_USE_BINARY=1 -DFM_USE_ASYNC=1 -DFM_USE_SAFE_WRITE=1 \
  -DFM_USE_CRC=1 -DFM_USE_STREAM=1 -DFM_USE_CACHE=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 \
  -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -fno-omit-frame-pointer" >/tmp/patch_build.log 2>&1 \
  || { tail -30 /tmp/patch_build.log; exit 1; }
gcc -std=gnu99 -O1 -g -fsanitize=address,undefined \
  -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=1 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 \
  -DFM_USE_LITTLEFS=1 -DFM_USE_THREADS=0 -DFM_USE_TEXT=1 -DFM_USE_STREAM=1 \
  -DFM_USE_SAFE_WRITE=1 -DFM_USE_JSON=1 -DFM_USE_CACHE=1 -DFM_USE_CRC=1 \
  -DFM_USE_ASYNC=1 -DFM_USE_BINARY=1 -DFM_USE_PRINTF=1 -DFM_DEBUG=1 \
  -I"$DST/include" -I"$DST/backends" -I"$DST/src" \
  -o "$3" "$2" "$DST/libfilemanager.a" -lm
echo "patched build ok: $3"
