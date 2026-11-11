#!/bin/sh
# Build a patched copy of the library with FM_USE_THREADS=1 + TSan and link
# a threaded harness against it.
#   build_patched_threads.sh <edit.py|-> <harness.c> <out>
set -e
SRC=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
DST=/tmp/lib_patched_thr
rm -rf "$DST"
mkdir -p "$DST"
tar -C "$SRC" -cf - --exclude='*.o' --exclude='*.a' . | tar -C "$DST" -xf -
EDIT=""
if [ -n "$1" ] && [ "$1" != "-" ]; then
  EDIT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
fi
if [ -n "$EDIT" ]; then ( cd "$DST" && python3 "$EDIT" ); fi
cd "$DST"
make -j2 libfilemanager.a FM_USE_THREADS=1 FM_SYNC_IMPL=pthread \
  CFLAGS_EXTRA="-g -O1 -fsanitize=thread -Wno-error" \
  LDFLAGS="-fsanitize=thread -lpthread" >/tmp/patch_thr.log 2>&1 \
  || { tail -30 /tmp/patch_thr.log; exit 1; }
gcc -std=gnu99 -O1 -g -fsanitize=thread -Wno-error -DFM_USE_THREADS=1 \
  -DFM_SYNC_CUSTOM -I"$DST/ports/pthread" -I"$DST/include" -I"$DST/backends" \
  -I"$DST/src" -o "$3" "$2" "$DST/libfilemanager.a" -lm -lpthread
echo "patched threads build ok: $3"
