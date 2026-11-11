# FileManager - Host build (Linux/POSIX)

CC      ?= gcc
LD       = $(CC)
AR      ?= ar

# Feature switches can be overridden, for example:
# make libfilemanager.a FM_USE_JSON=0 FM_USE_AUDIO=0
FM_USE_POSIX        ?= 1
FM_USE_RAMDISK     ?= 1
FM_USE_FMFS        ?= 1
FM_USE_FATFS       ?= 0
FM_USE_LITTLEFS    ?= 1
FM_USE_IMAGE       ?= 1
FM_USE_AUDIO       ?= 1
FM_USE_MP3         ?= 1
FM_USE_VIDEO       ?= 1
FM_USE_H264        ?= 1
FM_USE_MJPEG       ?= 1
FM_USE_JPEG        ?= 1
FM_USE_PNG         ?= 1
FM_USE_EEPROM      ?= 1
FM_USE_USB_MSC     ?= 1
FM_USE_DEVICE_ADAPTERS ?= 1
FM_USE_THREADS     ?= 0
FM_USE_JSON        ?= 1
FM_USE_TEXT        ?= 1
FM_USE_BINARY      ?= 1
FM_USE_DATA_LOGGER ?= 1
FM_USE_EXPLORER    ?= 1
FM_USE_ASYNC       ?= 1
FM_USE_MEDIA_DETECT ?= 1
FM_USE_SAFE_WRITE  ?= 1
FM_USE_CRC         ?= 1
FM_USE_STREAM      ?= 1
FM_USE_PRINTF      ?= 1
FM_USE_CACHE       ?= 1
FM_USE_FLAC        ?= 1
FM_USE_VORBIS      ?= 1
FM_USE_OPUS        ?= 1
# Real Opus decode via the vendored libopus (decoder-only vendor tree).
# Off by default so plain host builds stay fast; `make FM_USE_OPUS_LIB=1`
# compiles third_party/opus and enables src/fm_opus.c (mirrors what
# STM32CubeIDE builds do, where the vendor tree is compiled
# unconditionally).  Without it Opus is reported as FM_ERR_UNSUPPORTED.
FM_USE_OPUS_LIB    ?= 0

# ------------------------------------------------------------------
# Effective feature set.  fm_config.h switches a codec off whenever its
# parent subsystem is off; mirror exactly those rules here so the object
# list and include paths follow the EFFECTIVE configuration.  Otherwise
# e.g. `FM_USE_IMAGE=0 FM_USE_JPEG=1` or `FM_USE_VIDEO=0 FM_USE_MJPEG=1`
# would still link picojpeg (and all of its static tables) into a build
# that cannot use it.
# ------------------------------------------------------------------
ifeq ($(FM_USE_IMAGE),0)
override FM_USE_JPEG := 0
override FM_USE_PNG  := 0
endif
ifeq ($(FM_USE_VIDEO),0)
override FM_USE_H264  := 0
override FM_USE_MJPEG := 0
endif
ifeq ($(FM_USE_AUDIO),0)
override FM_USE_MP3      := 0
override FM_USE_FLAC     := 0
override FM_USE_VORBIS   := 0
override FM_USE_OPUS     := 0
endif
ifeq ($(FM_USE_OPUS),0)
override FM_USE_OPUS_LIB := 0
endif

# Base flags first so that command-line CFLAGS overrides and the optional
# blocks below always see the defaults defined.
CFLAGS  ?= -Wall -Wextra -Wconversion -Wshadow -Wpedantic -std=c99 \
           -O2 -g
LDFLAGS ?=

# Synchronization port used when FM_USE_THREADS=1.  The port provides
# fm_sync_impl.h on the include path.  Choose: pthread, freertos, cmsis_rtos2.
FM_SYNC_IMPL       ?= pthread
ifeq ($(FM_USE_THREADS),1)
CFLAGS  += -DFM_SYNC_CUSTOM -Iports/$(FM_SYNC_IMPL)
ifeq ($(FM_SYNC_IMPL),pthread)
LDFLAGS += -lpthread
else
# freertos / cmsis_rtos2 on the host: the real port header, with the few
# kernel calls it uses implemented on pthreads (tools/rtosstub).  This is
# what lets the suite run through those two ports at all.
CFLAGS    += -Itools/rtosstub/$(FM_SYNC_IMPL)
SYNC_HOST := tools/rtosstub/$(FM_SYNC_IMPL)/rtos_host.c
LDFLAGS   += -lpthread
endif
endif

CFLAGS  += -D_GNU_SOURCE -DFM_HOST_BUILD=1 \
            -DFM_USE_POSIX=$(FM_USE_POSIX) -DFM_USE_RAMDISK=$(FM_USE_RAMDISK) \
            -DFM_USE_FMFS=$(FM_USE_FMFS) -DFM_USE_FATFS=$(FM_USE_FATFS) \
            -DFM_USE_LITTLEFS=$(FM_USE_LITTLEFS) \
            -DFM_USE_MP3=$(FM_USE_MP3) \
            -DFM_USE_EEPROM=$(FM_USE_EEPROM) -DFM_USE_USB_MSC=$(FM_USE_USB_MSC) \
            -DFM_USE_DEVICE_ADAPTERS=$(FM_USE_DEVICE_ADAPTERS) \
            -DFM_USE_THREADS=$(FM_USE_THREADS) -DFM_USE_JSON=$(FM_USE_JSON) \
            -DFM_USE_TEXT=$(FM_USE_TEXT) -DFM_USE_BINARY=$(FM_USE_BINARY) \
            -DFM_USE_DATA_LOGGER=$(FM_USE_DATA_LOGGER) \
            -DFM_USE_EXPLORER=$(FM_USE_EXPLORER) -DFM_USE_ASYNC=$(FM_USE_ASYNC) \
            -DFM_USE_MEDIA_DETECT=$(FM_USE_MEDIA_DETECT) \
            -DFM_USE_SAFE_WRITE=$(FM_USE_SAFE_WRITE) -DFM_USE_CRC=$(FM_USE_CRC) \
            -DFM_USE_STREAM=$(FM_USE_STREAM) -DFM_USE_CACHE=$(FM_USE_CACHE) \
            -DFM_USE_PRINTF=$(FM_USE_PRINTF) -DFM_DEBUG=1
# Tier-aware features: only pass -D if explicitly set on command line,
# otherwise let fm_config.h decide based on FM_PORT_TIER (saves RAM on F1/G0)
ifeq ($(origin FM_USE_IMAGE),command line)
CFLAGS += -DFM_USE_IMAGE=$(FM_USE_IMAGE)
endif
ifeq ($(origin FM_USE_AUDIO),command line)
CFLAGS += -DFM_USE_AUDIO=$(FM_USE_AUDIO)
endif
ifeq ($(origin FM_USE_VIDEO),command line)
CFLAGS += -DFM_USE_VIDEO=$(FM_USE_VIDEO)
endif
ifeq ($(origin FM_USE_H264),command line)
CFLAGS += -DFM_USE_H264=$(FM_USE_H264)
endif
ifeq ($(origin FM_USE_MJPEG),command line)
CFLAGS += -DFM_USE_MJPEG=$(FM_USE_MJPEG)
endif
ifeq ($(origin FM_USE_JPEG),command line)
CFLAGS += -DFM_USE_JPEG=$(FM_USE_JPEG)
endif
ifeq ($(origin FM_USE_PNG),command line)
CFLAGS += -DFM_USE_PNG=$(FM_USE_PNG)
endif
ifeq ($(origin FM_USE_FLAC),command line)
CFLAGS += -DFM_USE_FLAC=$(FM_USE_FLAC)
endif
ifeq ($(origin FM_USE_VORBIS),command line)
CFLAGS += -DFM_USE_VORBIS=$(FM_USE_VORBIS)
endif
ifeq ($(origin FM_USE_OPUS),command line)
CFLAGS += -DFM_USE_OPUS=$(FM_USE_OPUS)
endif
ifeq ($(origin FM_USE_AAC),command line)
CFLAGS += -DFM_USE_AAC=$(FM_USE_AAC)
endif
# Extra defines for matrix / CI runs, e.g. CFLAGS_EXTRA="-DFM_DEBUG_LEVEL=3"
CFLAGS  += $(CFLAGS_EXTRA)

CFLAGS  += -Iinclude -Ibackends -Isrc
ifeq ($(FM_USE_LITTLEFS),1)
CFLAGS  += -Ithird_party/littlefs
endif
ifeq ($(FM_USE_H264),1)
CFLAGS  += -Ithird_party/h264bsd/src
endif
ifeq ($(FM_USE_MP3),1)
CFLAGS  += -Ithird_party/minimp3
endif
ifeq ($(FM_USE_MJPEG),1)
CFLAGS  += -Ithird_party/picojpeg
endif
ifeq ($(FM_USE_JPEG),1)
CFLAGS  += -Ithird_party/picojpeg
endif
# FLAC via dr_flac (single header)
ifeq ($(FM_USE_FLAC),1)
CFLAGS  += -Ithird_party/dr_flac
endif
# Vorbis via stb_vorbis
ifeq ($(FM_USE_VORBIS),1)
CFLAGS  += -Ithird_party/stb
endif

OBJS     = src/fm_core.o src/fm_crc.o src/fm_path.o \
           src/fm_transfer.o src/fm_qspi.o src/fm_spiflash.o src/fm_eeprom_chips.o \
           src/fm_mediacache.o src/fm_log.o \
           src/fm_storage.o src/fm_filesystem.o \
           src/fm_file.o src/fm_directory.o \
           src/fm_stream.o src/fm_text.o src/fm_binary.o \
           src/fm_json.o src/fm_safe.o \
           src/fm_logger.o src/fm_explorer.o \
           src/fm_media.o src/fm_async.o src/fm_fmfs.o \
           src/fm_image.o src/fm_jpeg.o src/fm_png.o \
           src/fm_audio.o src/fm_video.o \
           src/fm_eeprom.o src/fm_usb_msc.o src/fm_device.o \
           src/fm_h264.o src/fm_mjpeg.o src/fm_flac.o src/fm_vorbis.o src/fm_opus.o src/fm_alloc.o \
           src/fm_hw.o src/fm_player.o src/fm_sd_hal.o \
           backends/fm_posix_fs.o backends/fm_ramdisk.o

# stb_vorbis.c is vendored and has no FM_USE_* guard of its own: only
# build it when the Vorbis decoder is really enabled.
ifeq ($(FM_USE_VORBIS),1)
OBJS     += third_party/stb/stb_vorbis.o
endif

ifeq ($(FM_USE_FATFS),1)
OBJS     += backends/fm_fatfs.o
endif
ifeq ($(FM_USE_LITTLEFS),1)
OBJS     += backends/fm_littlefs.o third_party/littlefs/lfs.o \
           third_party/littlefs/lfs_util.o
endif
ifeq ($(FM_USE_H264),1)
H264BSD_SRCS := $(wildcard third_party/h264bsd/src/*.c)
OBJS     += $(H264BSD_SRCS:.c=.o)
endif
ifeq ($(FM_USE_OPUS_LIB),1)
CFLAGS   += -DFM_OPUS_USE_LIBOPUS=1
OPUS_INC  = -Ithird_party/opus/include -Ithird_party/opus/celt \
            -Ithird_party/opus/silk -Ithird_party/opus/src
OPUS_SRCS := $(wildcard third_party/opus/src/*.c) \
             $(wildcard third_party/opus/celt/*.c) \
             $(wildcard third_party/opus/celt/arm/*.c) \
             $(wildcard third_party/opus/silk/*.c) \
             $(wildcard third_party/opus/silk/arm/*.c)
OBJS     += $(OPUS_SRCS:.c=.o)
endif

# h264bsd predates strict-aliasing rules (it type-puns mv_t via u32
# pointers and relies on other permissive behaviours).  GCC may
# miscompile it at -O1/-O2 with aliasing analysis on some targets
# (mid-slice "invalid format" failures on ARM with byte-identical
# input).  Never let aliasing optimizations near it.  picojpeg is pure
# entropy-decode hot loop: -O3 is measurably faster and well-tested.
third_party/h264bsd/src/%.o: CFLAGS += -fno-strict-aliasing
third_party/picojpeg/picojpeg.o: CFLAGS += -O3
src/fm_flac.o: CFLAGS += -Wno-conversion -Wno-sign-conversion -Wno-unused-function -Wno-pedantic -Wno-shadow -Wno-sign-compare
src/fm_vorbis.o: CFLAGS += -Wno-conversion -Wno-sign-conversion -Wno-unused-function -Wno-pedantic -Wno-shadow -Wno-sign-compare -Wno-unused-parameter -Wno-maybe-uninitialized
src/fm_opus.o: CFLAGS += -Wno-conversion -Wno-sign-conversion -Wno-unused-function -Wno-pedantic -Wno-shadow -Wno-sign-compare -Wno-unused-parameter

PICOJPEG_NEEDED := 0
ifeq ($(FM_USE_MJPEG),1)
PICOJPEG_NEEDED := 1
endif
ifeq ($(FM_USE_JPEG),1)
PICOJPEG_NEEDED := 1
endif
ifeq ($(PICOJPEG_NEEDED),1)
OBJS     += third_party/picojpeg/picojpeg.o
endif

LIBS     = libfilemanager.a

EXAMPLES = examples/setup examples/file_basics \
           examples/file_copy_move examples/advanced_handle \
           examples/directory examples/json_example \
           examples/text_lines examples/safe_write \
           examples/data_logger examples/crc_example \
           examples/image_bmp

.PHONY: all clean test examples examples_run asan tsan ports cmake check matrix matrix-asan matrix-quick fatfs m32

all: $(LIBS) fm_test

examples: $(EXAMPLES)

examples/%: examples/%.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(OBJS) $(LDFLAGS) -lm

examples_run: examples
	@for ex in $(EXAMPLES); do \
	    echo ""; \
	    echo "================================================"; \
	    echo "  Running: $$ex"; \
	    echo "================================================"; \
	    ./$$ex; \
	done

# Static library
libfilemanager.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

# Objects
src/%.o: src/%.c include/fm*.h
	$(CC) $(CFLAGS) -c -o $@ $<

backends/%.o: backends/%.c include/fm*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Vendored third-party code is built with relaxed warnings: it is upstream
# code, not maintained under this project's strict warning set.  littlefs
# diagnostics are silenced because the first-mount "corrupted" report is an
# expected condition that the adapter auto-formats.
third_party/littlefs/%.o: third_party/littlefs/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-shadow \
		-DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -c -o $@ $<

third_party/h264bsd/src/%.o: third_party/h264bsd/src/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-shadow \
		-Wno-unused-function -Wno-unused-parameter \
		-Wno-unused-but-set-variable -Wno-maybe-uninitialized \
		-include include/fm_alloc.h \
		-Dmalloc=FM_Alloc -Dfree=FM_Free -c -o $@ $<

third_party/picojpeg/%.o: third_party/picojpeg/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-shadow \
		-Wno-unused-function -Wno-unused-parameter -Wno-sequence-point \
		-Wno-unused-but-set-variable -Wno-pedantic \
		-c -o $@ $<

# Vendored libopus (decoder-only tree).  No -D flags: the vendor carries
# working defaults (see include/opus_types.h / celt/stack_alloc.h), which
# is exactly how STM32CubeIDE compiles it — this rule verifies that path.
third_party/opus/%.o: third_party/opus/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-shadow \
		-Wno-unused-function -Wno-unused-parameter -Wno-pedantic \
		-Wno-sign-compare -Wno-maybe-uninitialized $(OPUS_INC) \
		-c -o $@ $<

# stb_vorbis single-file library, compiled as its own object (the sole
# implementation TU — src/fm_vorbis.c includes it header-only).
third_party/stb/%.o: third_party/stb/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-shadow \
		-Wno-unused-function -Wno-unused-parameter -Wno-pedantic \
		-Wno-maybe-uninitialized -c -o $@ $<

# Test executable
fm_test: test/main.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ test/main.c $(SYNC_HOST) $(OBJS) $(LDFLAGS) -lm

# ------------------------------------------------------------------
# asan — the suite under AddressSanitizer + UndefinedBehaviorSanitizer
#
# Builds into a separate directory so it never mixes instrumented and
# plain objects (linking those together silently produces a binary with
# no sanitizer in it at all, which looks like a clean run).
# ------------------------------------------------------------------
asan:
	@$(MAKE) --no-print-directory clean >/dev/null
	@mkdir -p build-asan
	@$(MAKE) --no-print-directory libfilemanager.a \
		CFLAGS="$(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
	@$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o build-asan/fm_test \
		test/main.c libfilemanager.a -lm
	@n=$$(nm build-asan/fm_test | grep -c __asan); \
		if [ "$$n" -eq 0 ]; then \
			echo "ERROR: sanitizer not linked in -- result would be meaningless"; \
			exit 1; \
		fi; \
		echo "sanitizer active ($$n symbols)"
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
		./build-asan/fm_test
	@$(MAKE) --no-print-directory clean >/dev/null

# tsan — the suite with FM_USE_THREADS=1 under ThreadSanitizer.  Any
# data race or lock-order inversion makes the run fail (exit code 66).
# -Wno-error: the instrumentation makes gcc report a false
# "may be used uninitialized" in src/fm_video.c.
#   make tsan                        pthread port
#   make tsan FM_SYNC_IMPL=freertos  (or cmsis_rtos2) through that port
tsan:
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory fm_test FM_USE_THREADS=1 \
		FM_SYNC_IMPL=$(FM_SYNC_IMPL) \
		CFLAGS_EXTRA="-g -O1 -fsanitize=thread -Wno-error" \
		LDFLAGS="-fsanitize=thread -lpthread" >fm_tsan.log 2>&1 || \
		{ grep -E ": error|undefined reference" fm_tsan.log | head -20; exit 1; }
	@n=$$(nm fm_test | grep -c __tsan); \
		if [ "$$n" -eq 0 ]; then \
			echo "ERROR: ThreadSanitizer not linked in"; exit 1; \
		fi; \
		echo "ThreadSanitizer active ($$n symbols)"
	@TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 exitcode=66" \
		./fm_test > fm_tsan.log 2>&1; rc=$$?; \
		grep -E "Results:|FAIL" fm_tsan.log; \
		echo "ThreadSanitizer warnings: $$(grep -c 'WARNING: ThreadSanitizer' fm_tsan.log)"; \
		if [ $$rc -ne 0 ]; then \
			grep -A12 'WARNING: ThreadSanitizer' fm_tsan.log | head -60; \
			echo "full log: fm_tsan.log"; exit 1; \
		fi; rm -f fm_tsan.log
	@$(MAKE) --no-print-directory clean >/dev/null

# ports — every supported MCU family must still compile
ports:
	@sh tools/check_ports.sh

# m32 — the whole suite built and run as a 32-bit program with clang
# (the targets are 32-bit, the PC is not).  Needs "pip install ziglang"
# or a gcc with -m32; prints SKIP otherwise.
#   make m32              plain
#   make m32 M32=ubsan    with UBSan
M32 ?= plain
m32:
	@sh tools/check_32bit.sh $(M32)

# cmake — CMakeLists.txt really configured, built and tested (13
# configurations, incl. the three sync ports).  Skipped with a message
# when cmake is not installed.
cmake:
	@sh tools/check_cmake.sh

# matrix — every FM_USE_* combination must build (-Werror), pass its
# tests and leave no static memory behind for a disabled subsystem.
#   make matrix        plain build of all configurations
#   make matrix-asan   the same under ASan + UBSan + LeakSanitizer
#   make matrix-quick  only the 8 IMAGE/VIDEO/AUDIO combinations
matrix:
	@sh tools/check_matrix.sh full
matrix-asan:
	@sh tools/check_matrix.sh asan
matrix-quick:
	@sh tools/check_matrix.sh quick

# cachetest — cache maintenance must follow the real SCB register.
# Separate from fm_test because it fakes the CMSIS headers.
cachetest:
	@$(CC) -Wall -Wextra -std=c99 -Iinclude \
		test/cache_runtime_test.c -o fm_cachetest
	@./fm_cachetest

# videohooks — the DMA2D/LTDC hooks and their software fallbacks.
# Needs the HAL stub, so it cannot live inside fm_test.
videohooks: $(LIBS)
	@$(CC) -Wall -Wextra -std=c99 -Iinclude -Iexamples -Itools/halstub \
		test/video_hooks_test.c $(LIBS) -lm -o fm_videohooks
	@./fm_videohooks

# ex33test — example 33 (record + taps + folder copy) against the real
# library on the host: records a WAV, reopens it, verifies length and
# level, installs every tap, and copies a nested tree.
ex33test: $(LIBS)
	@$(CC) -Wall -Wextra -std=gnu11 -Wno-unused-function -DFM_HOST_BUILD=1 \
		-Iinclude -Iexamples -Ibackends -Isrc -Itools/halstub \
		tools/ex33_host_test.c $(LIBS) -lm -o fm_ex33test
	@./fm_ex33test

# tapstest — the PCM-tap block of board/main.c (Example 33 wiring)
# executed on the host.  The section is extracted VERBATIM from
# board/main.c between "AGC_TARGET_PEAK" (the pre-tap DSP banner) and
# the UART defines, so what is tested is exactly what ships to the board.
tapstest: $(LIBS)
	@sed -n '/^#define AGC_TARGET_PEAK/,/^#define UART_RX_BUF_SIZE/p' \
		board/main.c | head -n -1 > /tmp/fm_main_taps.h
	@$(CC) -Wall -Wextra -std=gnu11 -Wno-unused-function -DFM_HOST_BUILD=1 \
		-Iinclude -Ibackends -Isrc -I/tmp \
		tools/board_taps_host_test.c $(LIBS) -lm -o fm_tapstest
	@./fm_tapstest

# fatfs — the FatFS adapter against a mock FatFS (three header
# generations, one and two drives) and the SD-card example set, all under
# ASan + UBSan.  The adapter is not in the default host build (it needs
# ff.h), so this is the only place it is compiled and run.
fatfs:
	@sh test/build_fatfs_host.sh

# check — everything: tests, sanitizers, ports, examples
check: test cachetest videohooks ex33test tapstest asan ports fatfs
	@sh tools/check_examples.sh
	@sh tools/check_cmake.sh
	@sh tools/check_32bit.sh

clean:
	rm -rf build-asan
	rm -f $(LIBS) fm_test fm_cachetest fm_videohooks fm_ex33test fm_tapstest fm_tsan.log fm_32bit.log $(EXAMPLES)
	find src backends third_party -name "*.o" -delete

test: fm_test
	./fm_test
