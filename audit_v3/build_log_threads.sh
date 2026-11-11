#!/bin/sh
# Build the log-layer thread harness against a FM_USE_THREADS=1 + TSan library.
set -e
LIB=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
cd "$LIB"
make clean >/dev/null 2>&1 || true
make -j2 libfilemanager.a FM_USE_THREADS=1 FM_SYNC_IMPL=pthread \
  CFLAGS_EXTRA="-g -O1 -fsanitize=thread -Wno-error" \
  LDFLAGS="-fsanitize=thread -lpthread" >/tmp/build_thr.log 2>&1 \
  || { tail -30 /tmp/build_thr.log; exit 1; }
gcc -std=gnu99 -O1 -g -fsanitize=thread -Wno-error -DFM_USE_THREADS=1 \
  -DFM_SYNC_CUSTOM -I$LIB/ports/pthread -I$LIB/include -I$LIB/backends -I$LIB/src \
  -o "${1:-/tmp/log_threads}" "${2:-/home/user/Clain_Filemanagaer/audit_v3/harness/log_threads.c}" \
  $LIB/libfilemanager.a -lm -lpthread
