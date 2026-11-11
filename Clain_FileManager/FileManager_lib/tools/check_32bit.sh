#!/bin/sh
# check_32bit.sh -- build and run the host tests as a 32-bit program.
#
# Every target this library is written for is 32-bit (size_t, long and
# pointers are 4 bytes), the development PC is 64-bit.  A format string
# that prints a size_t with "%lu", a shift by 32, an offset kept in a
# size_t, a struct whose padding changes -- none of these show on the PC.
# This run builds the whole library and the whole test program for
# 32-bit x86 and executes it, with clang, so it is a second compiler too.
#
# Needs one of
#   - zig as a C compiler:  pip install ziglang   (or "zig" on the PATH)
#   - a gcc that can do -m32 (gcc-multilib)
# Prints SKIP and succeeds when neither is there.
#
#   sh tools/check_32bit.sh          plain build
#   sh tools/check_32bit.sh ubsan    with UBSan (zig only; traps on UB)
cd "$(dirname "$0")/.." || exit 1

PY=${PYTHON:-python3}
mode=${1:-plain}
extra="-Wno-unknown-warning-option"
log=fm_32bit.log

if $PY -m ziglang version >/dev/null 2>&1; then
    CC32="$PY -m ziglang cc -target x86-linux-musl"
    AR32="$PY -m ziglang ar"
    what="zig cc $($PY -m ziglang version), x86-linux-musl"
elif command -v zig >/dev/null 2>&1; then
    CC32="zig cc -target x86-linux-musl"
    AR32="zig ar"
    what="zig cc $(zig version), x86-linux-musl"
elif echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
    CC32="gcc -m32"
    AR32="ar"
    what="gcc -m32"
    extra=""
else
    echo "SKIP check_32bit: no 32-bit compiler (pip install ziglang, or gcc-multilib)"
    exit 0
fi

if [ "$mode" = "ubsan" ]; then
    case "$CC32" in
    *zig*) extra="$extra -fsanitize=undefined -fsanitize-trap=undefined" ;;
    *)     extra="$extra -fsanitize=undefined -fno-sanitize-recover=undefined" ;;
    esac
fi

make --no-print-directory clean >/dev/null 2>&1
if ! make --no-print-directory fm_test CC="$CC32" AR="$AR32" \
        CFLAGS_EXTRA="$extra" >"$log" 2>&1; then
    grep -E ": error|undefined reference|undefined symbol" "$log" | head -20
    echo "FAIL check_32bit: build ($what); full log: $log"
    exit 1
fi

# really 32-bit?  (ELF class byte: 1 = 32-bit, 2 = 64-bit)
class=$(od -An -tu1 -j4 -N1 fm_test | tr -d ' ')
if [ "$class" != "1" ]; then
    echo "FAIL check_32bit: fm_test is not a 32-bit program (ELF class $class)"
    exit 1
fi

./fm_test >"$log" 2>&1
rc=$?
res=$(grep -a "Results:" "$log" | tail -1 | sed 's/^ *//')
if [ $rc -ne 0 ] || [ -z "$res" ]; then
    grep -a "FAIL" "$log" | head -20
    echo "FAIL check_32bit: tests (rc=$rc, $what); full log: $log"
    exit 1
fi
echo "OK   32-bit $mode ($what): $res"
rm -f "$log"
make --no-print-directory clean >/dev/null 2>&1
exit 0
