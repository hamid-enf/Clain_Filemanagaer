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
    for dep in $(sed -n 's/.*FM-REQUIRES:[[:space:]]*//p' "$f"); do
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
exit $rc
