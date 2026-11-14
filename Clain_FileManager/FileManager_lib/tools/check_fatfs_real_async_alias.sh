#!/bin/sh
# Requires the source/ directory of ST stm32-mw-fatfs at dd9126e957ae841a13ccbe2115d39a1fb7414126.
# Runs on a fake RAM disk; does not exercise physical hardware.
set -eu
cd "$(dirname "$0")/.."
SRC=${1:?pass FatFS source directory}
SRC=$(cd "$SRC" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
LOCK=${FS_LOCK:-16}
case "$LOCK" in 0|16) ;; *) echo "FS_LOCK must be 0 or 16" >&2; exit 1;; esac
sed -e 's/#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
    -e 's/#define FF_CODE_PAGE[[:space:]]*932/#define FF_CODE_PAGE 437/' \
    -e 's/#define FF_USE_LFN[[:space:]]*0/#define FF_USE_LFN 2/' \
    -e "s/#define FF_FS_LOCK[[:space:]]*0/#define FF_FS_LOCK $LOCK/" \
    "$SRC/ffconf_template.h" > "$TMP/ffconf.h"
grep -q '^#define FF_USE_LFN 2' "$TMP/ffconf.h"
CC=${CC:-cc}
FLAGS="-std=c99 -O1 -g -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 -DFM_FATFS_PROVIDE_FATTIME=1 -DFM_USE_POSIX=0 -DFM_USE_LITTLEFS=0 -DFM_USE_IMAGE=0 -DFM_USE_AUDIO=0 -DFM_USE_VIDEO=0 -DFM_USE_H264=0 -DFM_USE_MJPEG=0 -DFM_USE_VORBIS=0 -DFM_USE_MP3=0 -ffunction-sections -fdata-sections"
# shellcheck disable=SC2086
$CC $FLAGS -I"$TMP" -I"$SRC" -Iinclude -Ibackends -Isrc \
    tools/fatfs_real_async_alias_host.c src/*.c backends/fm_fatfs.c backends/fm_ramdisk.c \
    "$SRC/ff.c" "$SRC/ffunicode.c" -Wl,--gc-sections -lm -o "$TMP/probe"
"$TMP/probe"
