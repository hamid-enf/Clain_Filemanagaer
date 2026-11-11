#!/bin/sh
# Build an FM_USE_THREADS=1 + TSan library from a tree (e.g. /tmp/lib_all2,
# all five edits) and link a threaded harness against it.
#   build_threads_against.sh <tree> <harness.c> <out>
set -e
T=$1
make -C "$T" -j2 libfilemanager.a FM_USE_THREADS=1 FM_SYNC_IMPL=pthread \
  CFLAGS_EXTRA="-g -O1 -fsanitize=thread -Wno-error" \
  LDFLAGS="-fsanitize=thread -lpthread" >/tmp/thr_tree.log 2>&1 \
  || { tail -25 /tmp/thr_tree.log; exit 1; }
gcc -std=gnu99 -O1 -g -fsanitize=thread -Wno-error -DFM_USE_THREADS=1 \
  -DFM_SYNC_CUSTOM -I"$T/ports/pthread" -I"$T/include" -I"$T/backends" \
  -I"$T/src" -o "$3" "$2" "$T/libfilemanager.a" -lm -lpthread
