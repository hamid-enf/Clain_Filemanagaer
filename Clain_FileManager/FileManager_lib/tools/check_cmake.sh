#!/bin/sh
# check_cmake.sh — CMakeLists.txt is a second build description next to
# the Makefile; this script actually runs it instead of trusting it.
#
#     sh tools/check_cmake.sh
#
# For each configuration: configure, build, and (host configurations) run
# the whole test-suite through ctest.  Also checked:
#   - the library as an IDE/target project would get it (no FM_HOST_BUILD)
#     has no unresolved libopus symbols;
#   - add_subdirectory() from a parent project links a program.
#
# Exit status 0 = everything passed.  Without cmake on the PATH the script
# says so and exits 0 (set FM_REQUIRE_CMAKE=1 to make that an error).
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if ! command -v cmake >/dev/null 2>&1; then
    echo "SKIP check_cmake: cmake is not installed"
    [ "${FM_REQUIRE_CMAKE:-0}" = "1" ] && exit 1
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
rc=0
n=0

host_one() {   # $1 = label ; rest = cmake -D options
    label=$1
    shift
    n=$((n + 1))
    b="$TMP/b$n"
    if cmake -S "$ROOT" -B "$b" $GEN -DCMAKE_BUILD_TYPE=Release \
             -DFM_BUILD_TESTS=ON "$@" >"$b.log" 2>&1 &&
       cmake --build "$b" -j "$JOBS" >>"$b.log" 2>&1 &&
       (cd "$b" && ctest --output-on-failure >>"$b.log" 2>&1); then
        res=$( (cd "$ROOT" && "$b/fm_test" 2>/dev/null) | grep "Results:" |
               sed 's/^ *//')
        echo "OK   cmake $label  [$res]"
    else
        echo "FAIL cmake $label"
        grep -E "error|FAILED|undefined reference|Results:|FAIL:" "$b.log" |
            head -8
        rc=1
    fi
    rm -rf "$b"
}

host_one "default + cache"            -DFM_USE_CACHE=ON
host_one "cache off"                  -DFM_USE_CACHE=OFF
host_one "THREADS pthread"            -DFM_USE_CACHE=ON -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=pthread
host_one "THREADS freertos"           -DFM_USE_CACHE=ON -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=freertos
host_one "THREADS cmsis_rtos2"        -DFM_USE_CACHE=ON -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=cmsis_rtos2
host_one "real libopus"               -DFM_USE_CACHE=ON -DFM_USE_OPUS_LIB=ON
host_one "LITTLEFS/H264/MJPEG/MP3 off" -DFM_USE_LITTLEFS=OFF -DFM_USE_H264=OFF -DFM_USE_MJPEG=OFF -DFM_USE_MP3=OFF
host_one "IMAGE off"                  -DFM_USE_IMAGE=OFF
host_one "VIDEO off"                  -DFM_USE_VIDEO=OFF
host_one "AUDIO off"                  -DFM_USE_AUDIO=OFF
host_one "FLAC/VORBIS/OPUS off"       -DFM_USE_FLAC=OFF -DFM_USE_VORBIS=OFF -DFM_USE_OPUS=OFF

# The library the way a target project gets it: no FM_HOST_BUILD, default
# options.  It must not reference libopus unless libopus is built in.
n=$((n + 1))
b="$TMP/b$n"
if cmake -S "$ROOT" -B "$b" $GEN -DCMAKE_BUILD_TYPE=Release >"$b.log" 2>&1 &&
   cmake --build "$b" -j "$JOBS" >>"$b.log" 2>&1; then
    und=$(nm "$b/libfilemanager.a" 2>/dev/null | grep -c " U opus_")
    if [ "$und" -eq 0 ]; then
        echo "OK   cmake target-style library (no unresolved libopus symbols)"
    else
        echo "FAIL cmake target-style library: $und unresolved opus_* symbols"
        rc=1
    fi
else
    echo "FAIL cmake target-style library"
    grep -E "error|FAILED" "$b.log" | head -8
    rc=1
fi
rm -rf "$b"

# add_subdirectory() from a parent project.
n=$((n + 1))
p="$TMP/parent"
b="$TMP/b$n"
mkdir -p "$p"
{
    echo 'cmake_minimum_required(VERSION 3.13)'
    echo 'project(fm_parent C)'
    echo 'set(FM_HOST_BUILD ON CACHE BOOL "" FORCE)'
    echo "add_subdirectory(\"$ROOT\" filemanager)"
    echo 'add_executable(app app.c)'
    echo 'target_link_libraries(app PRIVATE filemanager m)'
} > "$p/CMakeLists.txt"
{
    echo '#include "file_manager.h"'
    echo '#include <string.h>'
    echo 'static uint8_t disk[64 * 1024];'
    echo 'static FM_Storage blk;'
    echo 'static FM_FS fs;'
    echo 'int main(void)'
    echo '{'
    echo '    char buf[16];'
    echo '    if (FileManager_Init() != FM_OK) return 1;'
    echo '    if (FileManager_MountMemory(&blk, &fs, "RAM", disk, sizeof(disk),'
    echo '                                1, FM_FALSE) != FM_OK) return 2;'
    echo '    if (File_WriteText("/a.txt", "hello") != FM_OK) return 3;'
    echo '    if (File_ReadText("/a.txt", buf, sizeof(buf), NULL) != FM_OK) return 4;'
    echo '    return (strcmp(buf, "hello") == 0) ? 0 : 5;'
    echo '}'
} > "$p/app.c"
if cmake -S "$p" -B "$b" $GEN >"$b.log" 2>&1 &&
   cmake --build "$b" -j "$JOBS" >>"$b.log" 2>&1 && "$b/app"; then
    echo "OK   cmake add_subdirectory() from a parent project"
else
    echo "FAIL cmake add_subdirectory() from a parent project (rc=$?)"
    grep -E "error|FAILED|undefined reference" "$b.log" | head -8
    rc=1
fi

echo "== cmake: $n configurations"
exit $rc
