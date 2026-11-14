#!/bin/sh
# Needs source/ from STMicroelectronics/stm32-mw-fatfs at
# dd9126e957ae841a13ccbe2115d39a1fb7414126; exercises fake storage only.
set -eu
cd "$(dirname "$0")/.."
SRC=${1:?pass the FatFS source directory}
SRC=$(cd "$SRC" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$SRC/ffconf_template.h" "$TMP/ffconf.h"
${CC:-cc} -std=c99 -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 \
    -DFM_FATFS_PROVIDE_DISKIO=1 -ffunction-sections -fdata-sections \
    -I"$TMP" -I"$SRC" -Iinclude -Ibackends -Isrc \
    tools/fatfs_diskio_bounds_host.c -Wl,--gc-sections -o "$TMP/probe"
"$TMP/probe"
