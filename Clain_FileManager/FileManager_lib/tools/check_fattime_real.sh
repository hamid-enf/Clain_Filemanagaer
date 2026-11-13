#!/bin/sh
# Run with the source/ directory of STMicroelectronics/stm32-mw-fatfs
# commit dd9126e957ae841a13ccbe2115d39a1fb7414126 (not vendored).
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SRC=${1:?provide the source directory from stm32-mw-fatfs}
SRC=$(cd "$SRC" && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$SRC/ff.c" "$SRC/ff.h" "$SRC/diskio.h" "$TMP/"
sed -e 's/#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
    -e 's/#define FF_CODE_PAGE[[:space:]]*932/#define FF_CODE_PAGE 437/' \
    "$SRC/ffconf_template.h" > "$TMP/ffconf.h"
grep -q '^#define FF_USE_MKFS 1' "$TMP/ffconf.h"
grep -q '^#define FF_CODE_PAGE 437' "$TMP/ffconf.h"
$CC -std=c99 -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 \
    -DFM_FATFS_PROVIDE_DISKIO=0 -DFM_FATFS_PROVIDE_FATTIME=1 \
    -ffunction-sections -fdata-sections -I"$TMP" -Iinclude -Ibackends -Isrc \
    -c backends/fm_fatfs.c -o "$TMP/backend.o"
$CC -std=c99 -ffunction-sections -fdata-sections -I"$TMP" -c "$TMP/ff.c" -o "$TMP/ff.o"
for mode in default strong; do
    flags=''
    if [ "$mode" = strong ]; then flags=-DSTRONG; fi
    # The test supplies an in-memory disk; unreferenced adapter functions are dropped.
    $CC -Wl,--gc-sections $flags -I"$TMP" "$ROOT/tools/fatfs_real_time_host.c" \
        "$TMP/ff.o" "$TMP/backend.o" -o "$TMP/test"
    "$TMP/test"
done
echo 'Real FatFS timestamp fallback and strong RTC override PASS'
