#!/bin/sh
# check_ports.sh — هر پورت پشتیبانی‌شده باید کامپایل شود.
#
# هستهٔ کتابخانه نباید به STM32H7 وابسته باشد. این اسکریپت آن ادعا را
# واقعاً می‌سنجد: کل src/ را با پروفایل هر پورت می‌سازد.
#
#     sh tools/check_ports.sh
#
# خروجی صفر یعنی همه‌چیز درست است.
set -e
cd "$(dirname "$0")/.."

PORTS="FM_PORT_STM32F1 FM_PORT_STM32F4 FM_PORT_STM32F7 \
FM_PORT_STM32G0 FM_PORT_STM32G4 FM_PORT_STM32L4 \
FM_PORT_STM32H7 FM_PORT_STM32H5 FM_PORT_GENERIC"

INC="-Iinclude -Isrc -Ibackends -Ithird_party/littlefs \
-Ithird_party/picojpeg -Ithird_party/minimp3 -Ithird_party/h264bsd/src \
-Ithird_party/dr_flac -Ithird_party/stb -Ithird_party/opus -Ithird_party/opus/include -Ithird_party/opus/celt -Ithird_party/opus/silk"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0

for p in $PORTS; do
    # پورت‌های کم‌توان کدک سنگین ندارند؛ همان چیزی که واقعاً استفاده می‌شود.
    case "$p" in
        *F1|*G0|*G4|*L4)
            FEAT="-DFM_USE_H264=0 -DFM_USE_VIDEO=0 -DFM_USE_MP3=0" ;;
        *)
            FEAT="" ;;
    esac

    n=0
    bad=0
    for f in src/*.c; do
        if gcc -c -Wall -std=c99 -DFM_HOST_BUILD -DFM_PORT="$p" $FEAT \
               $INC -o "$TMP/$(basename "$f" .c).o" "$f" 2>"$TMP/err"; then
            n=$((n + 1))
        else
            bad=$((bad + 1))
            echo "FAIL $p : $f"
            head -3 "$TMP/err"
        fi
    done
    if [ "$bad" -eq 0 ]; then
        echo "OK   $p  ($n files)"
    else
        rc=1
    fi
done

# تشخیص خودکار پورت از ماکروی قطعه‌ای که CubeMX تعریف می‌کند، و
# override شدن قابلیت‌ها از بیرون (وعدهٔ fm_port.h).
detect() {   # $1 = device macro ; $2 = expected FM_PORT_NAME
    {
        echo '#include "fm_port.h"'
        echo '#include <string.h>'
        echo "int main(void) { return strcmp(FM_PORT_NAME, \"$2\") != 0; }"
    } > "$TMP/det.c"
    if gcc -Wall -Werror -std=c99 -U__linux__ -D"$1" -Iinclude \
           -o "$TMP/det" "$TMP/det.c" 2>"$TMP/err" && "$TMP/det"; then
        echo "OK   -D$1 -> $2"
    else
        echo "FAIL -D$1 is not detected as $2"
        head -3 "$TMP/err"
        rc=1
    fi
}
detect STM32H743xx STM32H7
detect STM32H563xx STM32H5
detect STM32H503xx STM32H5
detect STM32H5     STM32H5
detect STM32F746xx STM32F7
detect STM32F407xx STM32F4
detect STM32F103xB STM32F1
detect STM32G0     STM32G0
detect STM32G4     STM32G4
detect STM32L4     STM32L4
detect FM_NO_SUCH_DEVICE generic

{
    echo '#define FM_PORT           FM_PORT_STM32F1'
    echo '#define FM_PORT_HAS_SDRAM 1'
    echo '#define FM_PORT_TIER      2'
    echo '#include "fm_port.h"'
    echo 'typedef char ovr_sdram[(FM_PORT_HAS_SDRAM == 1) ? 1 : -1];'
    echo 'typedef char ovr_tier[(FM_PORT_TIER == 2) ? 1 : -1];'
    echo 'typedef char ovr_rest[(FM_PORT_HAS_QSPI == 0 && FM_PORT_HAS_DMA == 1) ? 1 : -1];'
    echo 'typedef char ovr_buf[(FM_PORT_DEF_TRANSFER_CHUNK == 2048u) ? 1 : -1];'
    echo 'int main(void) { return 0; }'
} > "$TMP/ovr.c"
if gcc -Wall -Wextra -Werror -std=c99 -Iinclude -o "$TMP/ovr" "$TMP/ovr.c" \
       2>"$TMP/err"; then
    echo "OK   port capabilities can be overridden before fm_port.h"
else
    echo "FAIL overriding a port capability"
    head -3 "$TMP/err"
    rc=1
fi

# پورت‌های همگام‌سازی RTOS: هدر واقعی پورت در برابر امضای هدرهای کرنل
# (tools/rtosstub) کامپایل می‌شود، با FM_USE_THREADS=1.
for rtos in freertos cmsis_rtos2; do
    n=0
    bad=0
    for f in src/*.c backends/fm_ramdisk.c backends/fm_littlefs.c; do
        if gcc -c -Wall -Wextra -Werror -std=c99 -DFM_HOST_BUILD \
               -DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM \
               -Iports/$rtos -Itools/rtosstub/$rtos \
               $INC -o "$TMP/$(basename "$f" .c).o" "$f" 2>"$TMP/err"; then
            n=$((n + 1))
        else
            bad=$((bad + 1))
            echo "FAIL sync port $rtos : $f"
            head -3 "$TMP/err"
        fi
    done
    if [ "$bad" -eq 0 ]; then
        echo "OK   sync port $rtos  ($n files, FM_USE_THREADS=1)"
    else
        rc=1
    fi
done

exit $rc
