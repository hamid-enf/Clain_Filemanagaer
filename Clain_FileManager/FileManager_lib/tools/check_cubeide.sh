#!/bin/sh
# check_cubeide.sh — فایل‌های اضافه نباید وارد فرم‌ور شوند.
#
# STM32CubeIDE هر فایل .c را که زیر یک Source Folder باشد کامپایل می‌کند،
# و کل FileManager_lib یک Source Folder است. پس هر .c که جزو فرم‌ور نیست
# (مثال‌های کامپیوتر با main() خودشان، تست‌ها، ابزارها، فایل‌های دیاگ)
# باید در بیلد فرم‌ور «هیچ» تولید کند؛ وگرنه main دوم، _sbrk دوم یا یک
# include پیدانشده بیلد پروژه را می‌شکند.
#
# قاعده: فقط این پوشه‌ها کد فرم‌ور دارند
#
#     src/  backends/  third_party/  top_api/
#
# هر .c بیرون از این‌ها باید با نشانهٔ FM-NOT-FIRMWARE و یک #if در
# ابتدای فایل خودش را کنار بگذارد. این اسکریپت هر کدام را با ماکروهای
# یک پروژهٔ CubeMX و «بدون هیچ مسیر include» کامپایل می‌کند و می‌خواهد
# که آبجکت هیچ نمادی نداشته باشد.
#
#     sh tools/check_cubeide.sh
set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0
n=0

# همان نمادهایی که پروژهٔ برد تعریف می‌کند (Core/Src/main.c، بخش PTD):
# فقط نمادهای خود CubeMX، بدون هیچ FM_...
DEFS="-DDEBUG -DUSE_PWR_LDO_SUPPLY -DUSE_HAL_DRIVER -DSTM32H743xx"

# پروژه نباید به هیچ Symbol با نام FM_... نیاز داشته باشد. اگر
# FM_USE_FATFS در بیلد فریمور 0 بماند، backends/fm_fatfs.c خالی کامپایل
# می‌شود و لینک با «undefined reference to FatFS_Init» می‌افتد.
cat > "$TMP/nosym.c" <<'NOSYM'
#include "fm_config.h"
#include "fm_fatfs.h"
#if !FM_USE_FATFS
#error "FM_USE_FATFS is 0 in a firmware build without FM_ project symbols"
#endif
#if FM_FATFS_PROVIDE_DISKIO
#error "FM_FATFS_PROVIDE_DISKIO is 1 although FM_SD_DISKIO is CUBEMX"
#endif
#if !FM_USE_AUDIO || !FM_USE_MP3 || !FM_USE_VIDEO || !FM_USE_IMAGE
#error "a media switch is 0 in a firmware build without FM_ project symbols"
#endif
NOSYM
if $CC $DEFS -std=gnu11 -Iinclude -fsyntax-only "$TMP/nosym.c" 2>"$TMP/err"; then
    echo "cubeide: a firmware build needs no FM_ project symbol"
else
    echo "FAIL firmware build without FM_ project symbols:"
    grep -m3 'error' "$TMP/err" | sed 's/^/     /'
    rc=1
fi

for f in $(find . -name '*.c' \
               ! -path './src/*' ! -path './backends/*' \
               ! -path './third_party/*' ! -path './top_api/*' \
               ! -path './build*' | sort); do
    n=$((n + 1))
    if ! grep -q 'FM-NOT-FIRMWARE' "$f"; then
        echo "FAIL $f: no FM-NOT-FIRMWARE guard (CubeIDE would compile it)"
        rc=1
        continue
    fi
    # -nostdinc و بدون -I: اگر چیزی بیرون از #if مانده باشد همین‌جا می‌افتد
    if ! $CC $DEFS -std=gnu11 -nostdinc -c "$f" -o "$TMP/o.o" 2>"$TMP/err"; then
        echo "FAIL $f: does not compile to nothing in a firmware build"
        head -3 "$TMP/err" | sed 's/^/     /'
        rc=1
        continue
    fi
    if [ -n "$(nm "$TMP/o.o")" ]; then
        echo "FAIL $f: leaves symbols in a firmware build:"
        nm "$TMP/o.o" | head -5 | sed 's/^/     /'
        rc=1
    fi
done

# دو main() در یک پروژه: هیچ .c داخل پوشه‌های فرم‌ور نباید main تعریف کند
for f in $(grep -l -E '^[[:space:]]*int[[:space:]]+main[[:space:]]*\(' \
               src/*.c backends/*.c top_api/*.c 2>/dev/null); do
    echo "FAIL $f: defines main() inside a firmware folder"
    rc=1
done

if [ $rc -eq 0 ]; then
    echo "cubeide: $n non-firmware .c files compile to nothing"
fi
exit $rc
