#!/bin/sh
# check_examples.sh — همهٔ مثال‌ها باید کامپایل شوند.
#
# مثال‌ها بخشی از بیلد کتابخانه نیستند، پس اگر امضای یک API عوض شود
# بی‌صدا می‌شکنند و کاربر روی برد اولین کسی است که می‌فهمد.
#
# مثال‌های سخت‌افزاری با یک HAL کوچک شبیه‌سازی‌شده (tools/halstub)
# بررسی می‌شوند: اجرا نمی‌شوند، فقط سینتکس و نام‌ها چک می‌شود. همین
# کافی است تا خطایی مثل نام غلط یک ثابت (FM_ERR_NOT_SUPPORTED به‌جای
# FM_ERR_UNSUPPORTED) اینجا گرفته شود نه روی برد.
#
#     sh tools/check_examples.sh
set -e
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0
plain=0
stubbed=0
skipped=0

for f in examples/*.h; do
    # هدرهایی که عمداً ترتیب include را الزام می‌کنند
    if grep -q 'include .* before ' "$f" 2>/dev/null; then
        skipped=$((skipped + 1))
        continue
    fi

    # «FM-REQUIRES: a.h b.h» یعنی این مثال روی مثال‌های دیگری سوار
    # است. به‌جای رد کردنش، پیش‌نیازها را جلوتر include می‌کنیم تا
    # واقعاً کامپایل شود — وگرنه یک مثال بی‌صدا بی‌آزمون می‌ماند.
    : > "$TMP/t.c"
    # (منبع‌ها CRLF هستند: بدون حذف \r نام آخرین پیش‌نیاز «x.h\r» می‌شود
    #  و include ساخته‌شده نامعتبر است.)
    for dep in $(tr -d '\r' < "$f" | sed -n 's/.*FM-REQUIRES:[[:space:]]*//p'); do
        printf '#include "examples/%s"\n' "$dep" >> "$TMP/t.c"
    done
    printf '#include "%s"\nint main(void){return 0;}\n' "$f" >> "$TMP/t.c"

    # اول بدون stub (مثال‌های خالص)
    if gcc -fsyntax-only -Iinclude -I. -Iexamples \
           "$TMP/t.c" 2>"$TMP/e1"; then
        plain=$((plain + 1))
        continue
    fi

    # بعد با HAL شبیه‌سازی‌شده (مثال‌های برد)
    if gcc -fsyntax-only -Iinclude -I. -Iexamples -Itools/halstub \
           -DSTM32H743xx "$TMP/t.c" 2>"$TMP/e2"; then
        stubbed=$((stubbed + 1))
        continue
    fi

    # هنوز خطا دارد: اگر فقط هدر گمشده است، رد کن؛ وگرنه خطای واقعی است
    if grep -q 'No such file or directory' "$TMP/e2"; then
        skipped=$((skipped + 1))
        continue
    fi
    echo "FAIL $f"
    grep 'error:' "$TMP/e2" | head -3 | sed 's/^/     /'
    rc=1
done

echo "examples: $plain plain, $stubbed with HAL stub, $skipped skipped"

# ---------------------------------------------------------------------
# FM_USE_VIDEO=0 (and every media module off): the examples must still
# compile -- the board defines.h includes simple_video.h unconditionally --
# and the video player must leave NO static storage behind (it used to
# keep sv_rgb/sv_frame, 11 MB, and failed to compile at all).
# ---------------------------------------------------------------------
off=0
for D in "-DFM_USE_VIDEO=0" \
         "-DFM_USE_VIDEO=0 -DFM_USE_IMAGE=0 -DFM_USE_AUDIO=0"; do
    for f in examples/*.h; do
        if grep -q 'include .* before ' "$f" 2>/dev/null; then
            continue
        fi
        : > "$TMP/t.c"
        for dep in $(tr -d '\r' < "$f" | sed -n 's/.*FM-REQUIRES:[[:space:]]*//p'); do
            printf '#include "examples/%s"\n' "$dep" >> "$TMP/t.c"
        done
        printf '#include "%s"\nint main(void){return 0;}\n' "$f" >> "$TMP/t.c"
        if gcc -fsyntax-only $D -Iinclude -I. -Iexamples \
               "$TMP/t.c" 2>"$TMP/e1"; then
            off=$((off + 1)); continue
        fi
        if gcc -fsyntax-only $D -Iinclude -I. -Iexamples -Itools/halstub \
               -DSTM32H743xx "$TMP/t.c" 2>"$TMP/e2"; then
            off=$((off + 1)); continue
        fi
        if grep -q 'No such file or directory' "$TMP/e2"; then
            continue
        fi
        echo "FAIL $f  ($D)"
        grep 'error:' "$TMP/e2" | head -3 | sed 's/^/     /'
        rc=1
    done
done
echo "examples with video off: $off compiled"

printf '#include "examples/simple_video.h"\nint main(void){return SimpleVideo_Begin()==FM_ERR_UNSUPPORTED?0:1;}\n' > "$TMP/v0.c"
if gcc -c -DFM_USE_VIDEO=0 -Iinclude -I. -Iexamples -Itools/halstub \
       -DSTM32H743xx "$TMP/v0.c" -o "$TMP/v0.o" 2>"$TMP/e3"; then
    # Only the HAL stub's own dummies may own storage; nothing named sv_*.
    if nm "$TMP/v0.o" | grep -E ' [bBdDcC] ' | grep -q ' sv_'; then
        echo "FAIL simple_video.h keeps static storage with FM_USE_VIDEO=0:"
        nm -S "$TMP/v0.o" | grep -E ' [bBdDcC] sv_' | sed 's/^/     /'
        rc=1
    else
        echo "simple_video.h with FM_USE_VIDEO=0: no player storage" \
             "(data+bss $(size "$TMP/v0.o" | awk 'NR==2{print $2+$3}') B, HAL stub only)"
    fi
else
    echo "FAIL simple_video.h does not compile with FM_USE_VIDEO=0"
    grep 'error:' "$TMP/e3" | head -3 | sed 's/^/     /'
    rc=1
fi
exit $rc
