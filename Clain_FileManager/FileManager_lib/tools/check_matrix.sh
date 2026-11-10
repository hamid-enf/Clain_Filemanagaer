#!/bin/sh
# check_matrix.sh - build + run the host suite for every FM_USE_* combination
#
#   sh tools/check_matrix.sh            # plain build of every combination
#   sh tools/check_matrix.sh asan       # same, under ASan + UBSan + LSan
#   sh tools/check_matrix.sh quick      # only the 8 IMAGE/VIDEO/AUDIO combos
#
# For every combination it
#   1. builds libfilemanager.a + fm_test from scratch with -Werror,
#   2. runs the suite (must be 0 failures; sanitizer reports are fatal),
#   3. checks with size/ar that a disabled subsystem owns NO static storage
#      (.data + .bss = 0) and that its vendored codec is not even linked.
#
# Exit status is non-zero if any combination fails.

MODE="${1:-full}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
LOG="${TMPDIR:-/tmp}/fm_matrix.log"
fail=0
ran=0

SAN=""
if [ "$MODE" = "asan" ]; then
    SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
fi

# Objects that belong to each subsystem.  With the subsystem switched off
# they may only contain stateless stubs: .data + .bss must be 0 bytes.
IMG_OBJS='fm_image.o fm_jpeg.o fm_png.o'
VID_OBJS='fm_video.o fm_h264.o fm_mjpeg.o fm_hw.o'
AUD_OBJS='fm_audio.o fm_flac.o fm_vorbis.o fm_opus.o'

ram_of() {    # $* = object names ; prints total .data+.bss they own
    size libfilemanager.a 2>/dev/null | awk -v list=" $* " '
        { n = $6 } index(list, " " n " ") { t += $2 + $3 } END { print t + 0 }'
}

has_obj() {   # $1 = regex ; true when the archive contains such an object
    ar t libfilemanager.a 2>/dev/null | grep -Eq "$1"
}

run_one() {   # $1 = label ; rest = make variables
    label="$1"; shift
    ran=$((ran + 1))
    make clean >/dev/null 2>&1
    if ! make -j"$JOBS" fm_test "$@" \
            CFLAGS_EXTRA="-Werror -Wno-error=cpp $SAN $CFLAGS_EXTRA" \
            LDFLAGS="$SAN" >"$LOG" 2>&1; then
        echo "FAIL  $label  (build)"; grep -E ": error|undefined reference|\*\*\* " "$LOG" | cut -c1-220 | head -6
        fail=$((fail + 1)); return
    fi
    if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
            ./fm_test >"$LOG" 2>&1; then
        echo "FAIL  $label  (run)  $(grep 'Results:' "$LOG")"
        grep -E "FAIL|ERROR: |runtime error" "$LOG" | cut -c1-220 | head -8
        fail=$((fail + 1)); return
    fi
    res=$(grep 'Results:' "$LOG" | sed 's/^ *//')
    leftover=""
    img=1; vid=1; aud=1; jpg=1; mjp=1
    for kv in "$@"; do
        case "$kv" in
            FM_USE_IMAGE=0) img=0 ;;
            FM_USE_VIDEO=0) vid=0 ;;
            FM_USE_AUDIO=0) aud=0 ;;
            FM_USE_JPEG=0)  jpg=0 ;;
            FM_USE_MJPEG=0) mjp=0 ;;
        esac
    done
    if [ $img = 0 ]; then
        [ "$(ram_of $IMG_OBJS)" != 0 ] && leftover="$leftover image-RAM"
    fi
    if [ $vid = 0 ]; then
        [ "$(ram_of $VID_OBJS)" != 0 ] && leftover="$leftover video-RAM"
        has_obj '^h264bsd' && leftover="$leftover h264bsd-linked"
    fi
    if [ $aud = 0 ]; then
        [ "$(ram_of $AUD_OBJS)" != 0 ] && leftover="$leftover audio-RAM"
        has_obj '^stb_vorbis|^opus_|^celt|^silk' && leftover="$leftover audio-codec-linked"
    fi
    # picojpeg serves JPEG stills (IMAGE+JPEG) and MJPEG video (VIDEO+MJPEG)
    need_pico=0
    [ $img = 1 ] && [ $jpg = 1 ] && need_pico=1
    [ $vid = 1 ] && [ $mjp = 1 ] && need_pico=1
    if [ $need_pico = 0 ] && has_obj '^picojpeg'; then
        leftover="$leftover picojpeg-linked"
    fi
    if [ -n "$leftover" ]; then
        echo "FAIL  $label  (disabled subsystem left behind:$leftover)"
        fail=$((fail + 1)); return
    fi
    echo "ok    $label  $res"
}

link_one() {  # $1 = label ; build + link only.  For configurations that
              # must compile but whose reduced feature set (documented)
              # makes parts of the suite fail by design.
    label="$1"; shift
    ran=$((ran + 1))
    make clean >/dev/null 2>&1
    if ! make -j"$JOBS" fm_test "$@" \
            CFLAGS_EXTRA="-Werror -Wno-error=cpp $SAN $CFLAGS_EXTRA" \
            LDFLAGS="$SAN" >"$LOG" 2>&1; then
        echo "FAIL  $label  (build)"; grep -E ": error|undefined reference|\*\*\* " "$LOG" | cut -c1-220 | head -6
        fail=$((fail + 1)); return
    fi
    echo "ok    $label  (builds and links)"
}

echo "== FM_USE_IMAGE / FM_USE_VIDEO / FM_USE_AUDIO  (mode: $MODE)"
for i in 0 1; do for v in 0 1; do for a in 0 1; do
    run_one "I=$i V=$v A=$a" FM_USE_IMAGE=$i FM_USE_VIDEO=$v FM_USE_AUDIO=$a
done; done; done

if [ "$MODE" != "quick" ]; then
    echo "== FM_USE_JPEG / FM_USE_PNG / FM_USE_MJPEG"
    for j in 0 1; do for p in 0 1; do for m in 0 1; do
        run_one "J=$j P=$p M=$m" FM_USE_JPEG=$j FM_USE_PNG=$p FM_USE_MJPEG=$m
    done; done; done

    echo "== contradictory / single-switch configurations"
    run_one "IMAGE=0 JPEG=1 PNG=1 MJPEG=0" FM_USE_IMAGE=0 FM_USE_JPEG=1 FM_USE_PNG=1 FM_USE_MJPEG=0
    run_one "VIDEO=0 MJPEG=1 H264=1"       FM_USE_VIDEO=0 FM_USE_MJPEG=1 FM_USE_H264=1
    run_one "AUDIO=0 MP3=1 FLAC=1 OPUS=1"  FM_USE_AUDIO=0 FM_USE_MP3=1 FM_USE_FLAC=1 FM_USE_VORBIS=1 FM_USE_OPUS=1
    run_one "H264=0"                       FM_USE_H264=0
    run_one "MP3=0"                        FM_USE_MP3=0
    run_one "FLAC=0 VORBIS=0 OPUS=0"       FM_USE_FLAC=0 FM_USE_VORBIS=0 FM_USE_OPUS=0
    run_one "CACHE=0"                      FM_USE_CACHE=0
    run_one "CRC=0"                        FM_USE_CRC=0
    run_one "THREADS=1"                    FM_USE_THREADS=1
    run_one "OPUS_LIB=1 (real libopus)"    FM_USE_OPUS_LIB=1
    CFLAGS_EXTRA="-DFM_DEBUG_LEVEL=3" run_one "FM_DEBUG_LEVEL=3 (all log macros expanded)"
    # F-11: redirect the transfer layer's cache-maintenance hooks to
    # counters in test/main.c and check them against the chunk count.
    CFLAGS_EXTRA="-DFM_XFER_CACHE_CLEAN=fm_test_xfer_clean -DFM_XFER_CACHE_INVALIDATE=fm_test_xfer_inval" \
        run_one "transfer cache hooks instrumented"
    CFLAGS_EXTRA="-DFM_TRANSFER_CACHE_MAINT=0" run_one "FM_TRANSFER_CACHE_MAINT=0"
    # No scratch buffer: File_Copy / File_Insert report FM_ERR_NO_MEMORY by
    # design, so only the build is checked (report F-12, Test/L).
    CFLAGS_EXTRA="-DFM_USE_INTERNAL_BUFFER=0" link_one "FM_USE_INTERNAL_BUFFER=0"
    # Image report D-04: the codec scratch claim is a real mutex with
    # threads on; the suite runs a 3-thread PNG encode/decode stress.
    run_one "THREADS=1 IMAGE=0"            FM_USE_THREADS=1 FM_USE_IMAGE=0
    run_one "THREADS=1 JPEG=0 PNG=0"       FM_USE_THREADS=1 FM_USE_JPEG=0 FM_USE_PNG=0
    # Image report D-08: no 32-bit host run is available, so the 4 GiB
    # pixel-buffer limit of a 32-bit size_t is imposed through the
    # config knob; that enables the "sizes beyond the limit" test.
    CFLAGS_EXTRA="-DFM_IMAGE_MAX_BYTES=0xFFFFFFFFu" \
        run_one "FM_IMAGE_MAX_BYTES=4GiB (32-bit size_t limit, simulated)"
    # Thread safety: the same suite (with its multi-thread stress tests)
    # through the FreeRTOS and CMSIS-RTOS2 port headers; the kernel calls
    # they use are implemented on pthreads in tools/rtosstub.
    run_one "THREADS=1 port=freertos"      FM_USE_THREADS=1 FM_SYNC_IMPL=freertos
    run_one "THREADS=1 port=cmsis_rtos2"   FM_USE_THREADS=1 FM_SYNC_IMPL=cmsis_rtos2
    # On the MCU every unaligned File_Read/File_Write goes through the
    # shared 512-byte DMA bounce buffer; host builds bypass it.  Force
    # the bounce path so it is exercised single- and multi-threaded.
    CFLAGS_EXTRA="-DFM_FILE_FORCE_BOUNCE" run_one "FM_FILE_FORCE_BOUNCE"
    CFLAGS_EXTRA="-DFM_FILE_FORCE_BOUNCE" run_one "FM_FILE_FORCE_BOUNCE THREADS=1" FM_USE_THREADS=1
    # One shared File_GetLastError() slot instead of one per task.
    CFLAGS_EXTRA="-DFM_LAST_ERROR_THREADS=0" run_one "THREADS=1 FM_LAST_ERROR_THREADS=0" FM_USE_THREADS=1
    # A "mutex only" sync port (no thread identity): a busy scratch
    # resource is reported as FM_ERR_BUSY instead of being waited for, so
    # the multi-thread suites fail by design -- only the build is checked.
    CFLAGS_EXTRA="-DFM_SYNC_PTHREAD_NO_THREAD_SELF" link_one "THREADS=1 sync port without thread identity" FM_USE_THREADS=1
fi

make clean >/dev/null 2>&1
echo "== $ran configurations, $fail failed"
[ "$fail" -eq 0 ]
