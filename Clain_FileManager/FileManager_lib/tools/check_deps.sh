#!/bin/sh
# check_deps.sh -- a header that a source includes must rebuild its object.
#
# make compares file times of the prerequisites it knows.  The object
# rules used to name only include/fm*.h, so after editing a header under
# src/, backends/ or third_party/ (or an example header the tests
# include) `make test` linked the old objects and reported the old
# result: a broken header could pass 589/589.  The Makefile now reads the
# header lists the compiler writes (-MMD); this script proves it:
#   1. after a build, nothing is out of date;
#   2. touching a private header makes every object that includes it out
#      of date, and no object that does not;
#   3. the same for a third-party header and for a header fm_test includes.
#
#     sh tools/check_deps.sh
cd "$(dirname "$0")/.." || exit 1

MAKE=${MAKE:-make}
rc=0
uptodate() { $MAKE -s -q "$@" >/dev/null 2>&1; }
fail() { echo "FAIL deps: $1"; rc=1; }

$MAKE -s -j4 libfilemanager.a fm_test >/dev/null 2>&1 || { echo "FAIL deps: build"; exit 1; }
uptodate libfilemanager.a fm_test || fail "after a build make still wants to rebuild"

# $1 = header, $2 = an object that includes it, $3 = an object that does not
probe() {
    [ -f "$1" ] || { fail "$1 does not exist (update this script)"; return; }
    sleep 1
    touch "$1"
    uptodate "$2" && fail "touching $1 did not make $2 out of date"
    uptodate "$3" || fail "touching $1 made $3 out of date, which does not include it"
    $MAKE -s -j4 libfilemanager.a fm_test >/dev/null 2>&1 || fail "rebuild after touching $1"
    uptodate "$2" || fail "$2 is still out of date after the rebuild"
}

probe src/fm_internal.h              src/fm_file.o   src/fm_crc.o
probe src/fm_adpcm.h                 src/fm_audio.o  src/fm_crc.o
probe third_party/minimp3/minimp3.h  src/fm_audio.o  src/fm_crc.o
probe third_party/h264bsd/src/h264bsd_util.h \
      third_party/h264bsd/src/h264bsd_seq_param_set.o src/fm_crc.o
probe examples/cache.h               fm_test         src/fm_crc.o
probe test/audit_v4_tests.h          fm_test         src/fm_crc.o

[ $rc -eq 0 ] && echo "check_deps: OK (6 headers)"
exit $rc
