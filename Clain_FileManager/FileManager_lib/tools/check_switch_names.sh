#!/bin/sh
# check_switch_names.sh -- FM_BUFFERS must work whatever else defines the
# short names SMALL / NORMAL / LARGE / HUGE.
#
# The user writes "#define FM_BUFFERS HUGE".  HUGE is also an old Unix
# constant: some <math.h> (versions of newlib, the C library of the ARM
# compiler) have their own
#     #define HUGE ((float)3.40282346638528860e+38)
# and when math.h came first, "#if FM_BUFFERS == HUGE" stopped the build
# with "missing binary operator before token".  The math.h of this PC
# does not define HUGE, so the host build never shows it; this script
# defines it by hand, the way such a math.h does.
#
# For each of the four levels, with
#   - nothing else defined,
#   - newlib's HUGE defined before the library,
#   - a foreign "#define LARGE 1" and "#define NORMAL 0" before it,
# the level, the buffer scale and FM_VIDEO_MAX_FRAMES must come out the
# same, and a HUGE that was there before must still be there afterwards.
#
#     sh tools/check_switch_names.sh
cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0
n=0

expect() {   # level -> "level num/den frames"
    case $1 in
        SMALL)  echo "1 1/4 1024" ;;
        NORMAL) echo "2 1/1 4096" ;;
        LARGE)  echo "3 4/1 4096" ;;
        HUGE)   echo "4 8/1 8192" ;;
    esac
}

for pre in none newlib_huge foreign; do
    for lv in SMALL NORMAL LARGE HUGE; do
        {
            case $pre in
                newlib_huge)
                    echo '#define HUGE ((float)3.40282346638528860e+38)' ;;
                foreign)
                    echo '#define LARGE 1'
                    echo '#define NORMAL 0' ;;
            esac
            echo "#define FM_BUFFERS $lv"
            echo '#include "fm_config.h"'
            echo '#include <stdio.h>'
            echo 'int main(void) {'
            echo '    printf("%d %u/%u %d", FM_BUFFERS_LEVEL,'
            echo '           FM_BUF_SCALE_NUM, FM_BUF_SCALE_DEN,'
            echo '           (int)FM_VIDEO_MAX_FRAMES); putchar(10);'
            if [ $pre = newlib_huge ]; then
                # the float HUGE of math.h must have survived
                echo '    return (HUGE > 1.0e30f) ? 0 : 3;'
            else
                echo '    return 0;'
            fi
            echo '}'
        } > "$TMP/t.c"
        n=$((n + 1))
        if ! $CC -std=gnu11 -Wall -Werror -DFM_HOST_BUILD=1 -Iinclude \
                -Ibackends -Isrc "$TMP/t.c" -o "$TMP/t" 2> "$TMP/err"; then
            echo "FAIL switch names: FM_BUFFERS=$lv ($pre) does not compile"
            head -5 "$TMP/err"
            rc=1
            continue
        fi
        got=$("$TMP/t"); st=$?
        if [ "$got" != "$(expect $lv)" ] || [ $st -ne 0 ]; then
            echo "FAIL switch names: FM_BUFFERS=$lv ($pre): got '$got'" \
                 "(exit $st), expected '$(expect $lv)'"
            rc=1
        fi
    done
done

# a value that is none of the four must still be refused
printf '#define FM_BUFFERS 7\n#include "fm_config.h"\nint main(void){return 0;}\n' \
    > "$TMP/t.c"
n=$((n + 1))
if $CC -std=gnu11 -DFM_HOST_BUILD=1 -Iinclude -Ibackends -Isrc \
        -fsyntax-only "$TMP/t.c" 2>/dev/null; then
    echo "FAIL switch names: FM_BUFFERS=7 was accepted"
    rc=1
fi

[ $rc -eq 0 ] && echo "check_switch_names: OK ($n cases)"
exit $rc
