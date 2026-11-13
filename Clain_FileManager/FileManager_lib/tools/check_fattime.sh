#!/bin/sh
# Test the optional FatFS clock fallback, including a strong application RTC.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/diskio.h" <<'EOF'
#include "ff.h"
typedef BYTE DSTATUS;
typedef enum { RES_OK, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;
#define STA_NOINIT 1
#define STA_PROTECT 4
#define CTRL_SYNC 0
#define GET_SECTOR_COUNT 1
#define GET_SECTOR_SIZE 2
#define GET_BLOCK_SIZE 3
#define CTRL_TRIM 4
EOF
cat > "$TMP/main.c" <<'EOF'
#include <stdint.h>
extern uint32_t get_fattime(void);
#ifndef WANT
#define WANT 0x50210000u
#endif
int main(void) { return get_fattime() != WANT; }
EOF
cat > "$TMP/rtc.c" <<'EOF'
#include <stdint.h>
uint32_t get_fattime(void) { return 0x581c1234u; }
EOF
compile() {
    # Deliberately drop unrelated adapter code at link, so no mock FAT is needed.
    $CC -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=1 \
        -I"$TMP" -Iinclude -Ibackends -Isrc -Itest/fatfs_mock "$@" \
        -c backends/fm_fatfs.c -o "$TMP/fatfs.o"
}
compile
$CC -Wl,--gc-sections "$TMP/main.c" "$TMP/fatfs.o" -o "$TMP/test"
"$TMP/test"
$CC -DWANT=0x581c1234u -Wl,--gc-sections "$TMP/main.c" "$TMP/rtc.c" "$TMP/fatfs.o" -o "$TMP/test"
"$TMP/test"
compile -DFM_FATFS_FATTIME_VALUE=0x54220000u
$CC -DWANT=0x54220000u -Wl,--gc-sections "$TMP/main.c" "$TMP/fatfs.o" -o "$TMP/test"
"$TMP/test"
for opt in '-DFM_FATFS_PROVIDE_FATTIME=0' '-D_FS_NORTC=1' '-DFF_FS_NORTC=1'; do
    compile "$opt"
    if nm "$TMP/fatfs.o" | grep -q ' get_fattime$'; then
        echo "unexpected fallback for $opt" >&2
        exit 1
    fi
done
$CC -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 \
    -Iinclude -Ibackends -Isrc -Itest/fatfs_mock -c backends/fm_fatfs.c -o "$TMP/fatfs.o"
if nm "$TMP/fatfs.o" | grep -q ' get_fattime$'; then
    echo 'unexpected fallback with external diskio' >&2; exit 1
fi
$CC -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -DFM_HOST_BUILD=1 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 \
    -DFM_FATFS_PROVIDE_FATTIME=1 -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
    -c backends/fm_fatfs.c -o "$TMP/fatfs.o"
$CC -Wl,--gc-sections "$TMP/main.c" "$TMP/fatfs.o" -o "$TMP/test"
"$TMP/test"
echo 'FatFS fallback time, RTC override, config and no-RTC checks PASS'
