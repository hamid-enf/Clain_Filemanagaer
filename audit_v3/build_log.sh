#!/bin/sh
set -e
LIB=/home/user/Clain_Filemanagaer/Clain_FileManager/FileManager_lib
gcc -std=gnu99 -O1 -g -fsanitize=address,undefined -I$LIB/include -I$LIB/backends -I$LIB/src \
  -o /tmp/log_harness harness/log_harness.c $LIB/libfilemanager.a -lm
