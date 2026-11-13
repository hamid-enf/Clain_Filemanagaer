#!/bin/sh
# check_flag_stamp.sh -- a changed compiler flag must rebuild the objects.
#
# make compares file times only.  Before the flags stamp
# (.fm_build_flags, see the Makefile) "make FM_USE_JSON=0" after a normal
# build recompiled nothing and linked old objects.  This script builds
# the library, then asks make (with -q: "is anything out of date?")
#   1. same flags again            -> nothing to do
#   2. one more -D                 -> out of date
#   3. the first flags again       -> out of date again (they changed back)
#   4. after rebuilding            -> nothing to do
#   5. a test header was touched   -> fm_test is out of date
#
# Needs GNU make 4.2+; prints SKIP with an older one.
#
#     sh tools/check_flag_stamp.sh
cd "$(dirname "$0")/.." || exit 1

MAKE=${MAKE:-make}
if ! printf 'ifeq ($(file < /dev/null),)\nall: ; @echo ok\nendif\n' |
        $MAKE -s -f - 2>/dev/null | grep -q ok; then
    echo "SKIP check_flag_stamp: this make has no \$(file <...)"
    exit 0
fi
case $($MAKE --version | head -1) in
    *" 3."*|*" 4.0"*|*" 4.1"*)
        echo "SKIP check_flag_stamp: GNU make older than 4.2"
        exit 0 ;;
esac

rc=0
uptodate() { $MAKE -s -q "$@" >/dev/null 2>&1; }
fail() { echo "FAIL flag stamp: $1"; rc=1; }

$MAKE -s -j4 libfilemanager.a >/dev/null 2>&1 || { echo "FAIL flag stamp: build"; exit 1; }
uptodate libfilemanager.a || fail "same flags: make wants to rebuild"
uptodate libfilemanager.a CFLAGS_EXTRA=-DFM_STAMP_PROBE=1 &&
    fail "a new -D did not make the objects out of date"
# The archive is out of date on its own account whenever an object is
# rebuilt *or* its own rule sees the stamp, so the question above stays
# "yes" even when one object rule has lost the stamp -- and make then
# relinks objects that were never recompiled.  Ask one object of every
# object rule of the Makefile, not the archive.
for o in src/fm_core.o src/fm_audio_enc.o src/fm_video_enc.o \
         backends/fm_posix_fs.o backends/fm_ramdisk.o \
         third_party/littlefs/lfs.o \
         third_party/h264bsd/src/h264bsd_seq_param_set.o \
         third_party/picojpeg/picojpeg.o third_party/stb/stb_vorbis.o \
         third_party/opus/src/opus_decoder.o; do
    [ -f "$o" ] || continue          # that part is switched off in this build
    uptodate "$o" CFLAGS_EXTRA=-DFM_STAMP_PROBE=1 &&
        fail "a new -D did not make $o out of date"
done
# ...and every object rule must be covered by the list above
rules=$(grep -c '^[a-z_0-9/]*%\.o: [a-z_0-9/]*%\.c' Makefile)
[ "$rules" -eq 7 ] || fail "the Makefile has $rules object rules, this script knows 7"
uptodate libfilemanager.a &&
    fail "going back to the first flags did not make them out of date"
$MAKE -s -j4 libfilemanager.a >/dev/null 2>&1 || fail "rebuild"
uptodate libfilemanager.a || fail "after the rebuild make is not done"

$MAKE -s -j4 fm_test >/dev/null 2>&1 || fail "fm_test build"
uptodate fm_test || fail "fm_test: make wants to rebuild at once"
sleep 1
touch test/audio_save_tests.h
uptodate fm_test && fail "a touched test header did not rebuild fm_test"

[ $rc -eq 0 ] && echo "check_flag_stamp: OK"
exit $rc
