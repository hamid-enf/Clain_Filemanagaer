#!/bin/sh
# Host build of the FatFS adapter test with AddressSanitizer + UBSan.
# Mirrors the on-target configuration:
#   FM_USE_FATFS=1  FM_FATFS_PROVIDE_DISKIO=0  (user provides the diskio /
#   in this case the mock replaces ff.c entirely)
set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
BASE_FLAGS="-Wall -Wextra -std=c99 -g -O1 \
 -fsanitize=address,undefined -fno-omit-frame-pointer \
 -DFM_HOST_BUILD=1 -DFM_USE_POSIX=0 -DFM_USE_LITTLEFS=0 \
 -DFM_USE_FATFS=1 -DFM_FATFS_PROVIDE_DISKIO=0 \
 -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 -DFM_USE_THREADS=0 \
 -DFM_USE_CACHE=0 -DFM_INTERNAL_BUFFER_SIZE=4096 \
 -Iinclude -Ibackends -Isrc -Itest/fatfs_mock"

CORE_SRCS="src/fm_core.c src/fm_path.c src/fm_storage.c src/fm_filesystem.c \
 src/fm_file.c src/fm_directory.c src/fm_fmfs.c src/fm_stream.c \
 src/fm_text.c backends/fm_ramdisk.c backends/fm_fatfs.c \
 test/fatfs_mock/ff_mock.c test/fatfs_host_test.c"

fail=0

# Classic generation: STM32Cube middleware style (R0.12c..R0.13c,
# _FFCONF 68300, 5-argument f_mkfs) - matches real CubeMX projects.
echo "building fatfs_host_test  [classic: _FFCONF 68300, f_mkfs(5 args)] ..."
$CC $BASE_FLAGS -o /tmp/fatfs_host_test_classic $CORE_SRCS -lm || exit 1
echo "running classic ..."
/tmp/fatfs_host_test_classic || fail=1

# Modern generation: R0.15 style (FF_DEFINED, MKFS_PARM, 4-argument f_mkfs).
echo ""
echo "building fatfs_host_test  [modern: FF_DEFINED 15100, MKFS_PARM] ..."
$CC $BASE_FLAGS -DMOCK_FATFS_MODERN=1 -o /tmp/fatfs_host_test_modern \
    $CORE_SRCS -lm || exit 1
echo "running modern ..."
/tmp/fatfs_host_test_modern || fail=1

# Legacy generation: no revision macro, R0.12a-style 4-argument f_mkfs.
# Selected explicitly with FM_FATFS_MKFS_VARIANT=1.
echo ""
echo "building fatfs_host_test  [legacy: no rev macro, f_mkfs(4 args)] ..."
$CC $BASE_FLAGS -DMOCK_FATFS_LEGACY=1 -DFM_FATFS_MKFS_VARIANT=1 \
    -o /tmp/fatfs_host_test_legacy $CORE_SRCS -lm || exit 1
echo "running legacy ..."
/tmp/fatfs_host_test_legacy || fail=1

# Full SD-card example set (examples/examples_sdcard.h) against the
# classic mock - the same sequence the board runs.
echo ""
echo "building sdcard_examples_host  [classic mock] ..."
$CC $BASE_FLAGS -I. \
    src/fm_core.c src/fm_path.c src/fm_storage.c src/fm_filesystem.c \
    src/fm_file.c src/fm_directory.c src/fm_fmfs.c src/fm_stream.c \
    src/fm_text.c src/fm_json.c src/fm_safe.c src/fm_logger.c \
    src/fm_image.c src/fm_jpeg.c src/fm_png.c src/fm_crc.c \
    src/fm_media.c src/fm_alloc.c \
    backends/fm_ramdisk.c backends/fm_fatfs.c \
    src/fm_log.c third_party/picojpeg/picojpeg.c \
    test/fatfs_mock/ff_mock.c test/sdcard_examples_host.c \
    -Ithird_party/picojpeg \
    -DFM_UART_LOG_NO_HAL=1 \
    -o /tmp/sdcard_examples_host -lm || exit 1
echo "running sdcard examples ..."
/tmp/sdcard_examples_host | tail -25
/tmp/sdcard_examples_host >/dev/null 2>&1 || fail=1

# Media examples (audio/video) against the classic mock.
echo ""
echo "building media_examples_host  [classic mock] ..."
$CC $BASE_FLAGS -I. -Ithird_party/minimp3 -Ithird_party/picojpeg \
    src/fm_core.c src/fm_path.c src/fm_storage.c src/fm_filesystem.c \
    src/fm_file.c src/fm_directory.c src/fm_fmfs.c src/fm_stream.c \
    src/fm_text.c src/fm_json.c src/fm_safe.c src/fm_logger.c \
    src/fm_image.c src/fm_jpeg.c src/fm_png.c src/fm_crc.c \
    src/fm_media.c src/fm_alloc.c \
    src/fm_audio.c src/fm_video.c src/fm_hw.c \
    backends/fm_ramdisk.c backends/fm_fatfs.c \
    src/fm_log.c third_party/picojpeg/picojpeg.c \
    test/fatfs_mock/ff_mock.c test/media_examples_host.c \
    -DFM_UART_LOG_NO_HAL=1 -DFM_USE_H264=0 -DFM_USE_MJPEG=0 \
    -o /tmp/media_examples_host -lm || exit 1
echo "running media examples ..."
/tmp/media_examples_host | tail -45
/tmp/media_examples_host >/dev/null 2>&1 || fail=1

# Player control examples against the classic mock.
echo ""
echo "building player_examples_host  [classic mock] ..."
$CC $BASE_FLAGS -I. -Ithird_party/minimp3 -Ithird_party/picojpeg \
    src/fm_core.c src/fm_path.c src/fm_storage.c src/fm_filesystem.c \
    src/fm_file.c src/fm_directory.c src/fm_fmfs.c src/fm_stream.c \
    src/fm_text.c src/fm_json.c src/fm_safe.c src/fm_logger.c \
    src/fm_image.c src/fm_jpeg.c src/fm_png.c src/fm_crc.c \
    src/fm_media.c src/fm_alloc.c \
    src/fm_audio.c src/fm_video.c src/fm_hw.c src/fm_player.c \
    backends/fm_ramdisk.c backends/fm_fatfs.c \
    src/fm_log.c third_party/picojpeg/picojpeg.c \
    test/fatfs_mock/ff_mock.c test/player_examples_host.c \
    -DFM_UART_LOG_NO_HAL=1 -DFM_USE_H264=0 -DFM_USE_MJPEG=0 \
    -o /tmp/player_examples_host -lm || exit 1
echo "running player examples ..."
/tmp/player_examples_host | tail -50
/tmp/player_examples_host >/dev/null 2>&1 || fail=1

exit $fail
