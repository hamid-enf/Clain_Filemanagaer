#!/bin/sh
# Host build of the FatFS adapter tests with AddressSanitizer + UBSan.
# Mirrors the on-target configuration:
#   FM_USE_FATFS=1  FM_FATFS_PROVIDE_DISKIO=0  (user provides the diskio /
#   in this case the mock replaces ff.c entirely)
#
#     sh test/build_fatfs_host.sh        (also: make fatfs)
#
# The library is compiled once, from every file in src/ -- a hand-kept
# list of sources went stale as soon as a module was added, and the
# script then failed at the link without anybody noticing, because it
# was not part of `make check`.  It is now.
set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

FLAGS="-Wall -Wextra -std=c99 -g -O1 \
 -fsanitize=address,undefined -fno-omit-frame-pointer \
 -D_GNU_SOURCE -DFM_HOST_BUILD=1 -DFM_USE_POSIX=0 -DFM_USE_LITTLEFS=0 \
 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 \
 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 -DFM_USE_THREADS=0 \
 -DFM_USE_CACHE=0 -DFM_INTERNAL_BUFFER_SIZE=4096 \
 -DFM_USE_H264=0 -DFM_USE_MJPEG=0 -DFM_USE_VORBIS=0 \
 -DFM_UART_LOG_NO_HAL=1 \
 -I. -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
 -Ithird_party/picojpeg -Ithird_party/minimp3"

fail=0

# ---- the adapter under the library's own warning set ----
# (the Makefile only compiles backends/fm_fatfs.c with FM_USE_FATFS=1,
#  which needs a FatFS header: this is the one place it meets
#  -Wconversion/-Wshadow, in both thread modes and all header generations)
echo "adapter: -Wconversion -Wshadow -Werror, threads off/on ..."
for t in "-DFM_USE_THREADS=0" \
         "-DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM -Iports/pthread"; do
    for g in "" "-DMOCK_FATFS_MODERN=1" \
             "-DMOCK_FATFS_LEGACY=1 -DFM_FATFS_MKFS_VARIANT=1" \
             "-DMOCK_FF_VOLUMES=2"; do
        # shellcheck disable=SC2086
        $CC -Wall -Wextra -Wconversion -Wshadow -std=c99 -Werror \
            -fsyntax-only -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
            -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 $t $g \
            -I. -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
            backends/fm_fatfs.c || exit 1
    done
done

# ---- the table of open objects (HIGH-02 of the V5 audit) ----
# CubeMX writes _FS_LOCK 2 into ffconf.h; with that, Directory_Copy cannot
# open its first file.  The adapter has to refuse to compile, with a
# message that names the setting -- and compile when told it is meant.
echo "adapter: _FS_LOCK below the handles of the library is a compile error ..."
for g in "" "-DMOCK_FATFS_MODERN=1"; do
    # shellcheck disable=SC2086
    if $CC -std=c99 -fsyntax-only -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
        -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 -DMOCK_FS_LOCK=2 $g \
        -I. -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
        backends/fm_fatfs.c 2> "$OUT/lock.log"; then
        echo "FAILED: the adapter compiled with _FS_LOCK = 2"
        exit 1
    fi
    grep -q "_FS_LOCK / FF_FS_LOCK is smaller" "$OUT/lock.log" || {
        cat "$OUT/lock.log"
        echo "FAILED: _FS_LOCK = 2 stopped the build, but not with the message"
        exit 1
    }
    # shellcheck disable=SC2086
    $CC -std=c99 -fsyntax-only -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
        -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 -DMOCK_FS_LOCK=2 $g \
        -DFM_FATFS_ALLOW_SMALL_LOCK=1 \
        -I. -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
        backends/fm_fatfs.c || exit 1
    # 0 = the table is off: accepted
    # shellcheck disable=SC2086
    $CC -std=c99 -fsyntax-only -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
        -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 -DMOCK_FS_LOCK=0 $g \
        -I. -Iinclude -Ibackends -Isrc -Itest/fatfs_mock \
        backends/fm_fatfs.c || exit 1
done

# ---- the library, once (nothing in it but the adapter sees ff.h) ----
echo "building the library  [asan+ubsan, FM_USE_FATFS=1] ..."
objs=""
for f in src/*.c backends/fm_ramdisk.c; do
    o="$OUT/$(basename "$f" .c).o"
    $CC $FLAGS -c -o "$o" "$f" || exit 1
    objs="$objs $o"
done
$CC $FLAGS -w -c -o "$OUT/picojpeg.o" third_party/picojpeg/picojpeg.c \
    || exit 1
objs="$objs $OUT/picojpeg.o"

# build NAME "extra flags" main.c  ->  $OUT/NAME
# (the adapter and the mock are compiled per variant: they are the two
#  files that depend on the FatFS header generation)
build() {
    name=$1; extra=$2; main=$3
    # shellcheck disable=SC2086
    $CC $FLAGS $extra -o "$OUT/$name" \
        backends/fm_fatfs.c test/fatfs_mock/ff_mock.c "$main" \
        $objs -lm || exit 1
}

# run NAME "last line it has to print"
# The examples report a step that failed with a line starting "  !!" and
# carry on, so the exit code alone says nothing about them: the closing
# line has to be there and no "!!" line may be.
run() {
    name=$1; want=$2
    ok=1
    "$OUT/$name" > "$OUT/$name.log" 2>&1 || ok=0
    grep -q -- "$want" "$OUT/$name.log" || ok=0
    if grep -q '^ *!!' "$OUT/$name.log"; then ok=0; fi
    if [ "$ok" -eq 1 ]; then
        grep -- "$want" "$OUT/$name.log" | tail -n 1
    else
        cat "$OUT/$name.log"
        echo "FAILED: $name"
        fail=1
    fi
}

# Classic generation: STM32Cube middleware style (R0.12c..R0.13c,
# _FFCONF 68300, 5-argument f_mkfs) - matches real CubeMX projects.
echo "fatfs_host_test  [classic: _FFCONF 68300, f_mkfs(5 args)] ..."
build classic "" test/fatfs_host_test.c
run classic ' passed, 0 failures'

# Modern generation: R0.15 style (FF_DEFINED, MKFS_PARM, 4-argument f_mkfs).
echo "fatfs_host_test  [modern: FF_DEFINED 15100, MKFS_PARM] ..."
build modern "-DMOCK_FATFS_MODERN=1" test/fatfs_host_test.c
run modern ' passed, 0 failures'

# Legacy generation: no revision macro, R0.12a-style 4-argument f_mkfs.
# Selected explicitly with FM_FATFS_MKFS_VARIANT=1.
echo "fatfs_host_test  [legacy: no rev macro, f_mkfs(4 args)] ..."
build legacy "-DMOCK_FATFS_LEGACY=1 -DFM_FATFS_MKFS_VARIANT=1" \
    test/fatfs_host_test.c
run legacy ' passed, 0 failures'

# Two drives (SD on 0:, USB on 1:): the tests that need a second volume.
echo "fatfs_host_test  [classic, FF_VOLUMES=2] ..."
build twodrives "-DMOCK_FF_VOLUMES=2" test/fatfs_host_test.c
run twodrives ' passed, 0 failures'

# Full SD-card example set (examples/examples_sdcard.h) against the
# classic mock - the same sequence the board runs.
echo "sdcard_examples_host  [classic mock] ..."
build sdcard "" test/sdcard_examples_host.c
run sdcard 'ALL SD EXAMPLES DONE OK'

# Media examples (audio/video) against the classic mock.
echo "media_examples_host  [classic mock] ..."
build media "" test/media_examples_host.c
run media 'ALL MEDIA EXAMPLES DONE OK'

# Player control examples against the classic mock.
echo "player_examples_host  [classic mock] ..."
build player "" test/player_examples_host.c
run player 'ALL PLAYER EXAMPLES DONE OK'

if [ "$fail" -eq 0 ]; then
    echo "fatfs: all host builds passed"
fi
exit $fail
