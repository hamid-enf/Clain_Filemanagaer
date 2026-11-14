#!/bin/sh
# Requires source/ from STMicroelectronics/stm32-mw-fatfs at
# dd9126e957ae841a13ccbe2115d39a1fb7414126; no third-party files are vendored.
set -eu
cd "$(dirname "$0")/.."
SRC=${1:?pass the FatFS source directory}
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
    -c tools/fatfs_real_rename_host.c -o "$TMP/test.o"
$CC -std=c99 -ffunction-sections -fdata-sections -I"$TMP" -c "$TMP/ff.c" -o "$TMP/ff.o"
$CC -Wl,--gc-sections "$TMP/test.o" "$TMP/ff.o" -o "$TMP/test"
"$TMP/test"
