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
FM_PORT_STM32H7 FM_PORT_GENERIC"

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

exit $rc
