/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_config.h - Compile-time configuration
 *
 * Every feature is gated by FM_USE_* macros.  When a feature is disabled
 * (0) its code is compiled out completely and no ROM/RAM is consumed.
 *
 * To override the defaults without editing this file, define
 * FM_USE_USER_CONFIG and provide a header named "fm_config_user.h"
 * somewhere on the include path, or pass -DFM_USE_X=... on the command
 * line.  Command-line definitions always win (this file never
 * re-defines an already-defined macro).
 */

#ifndef FM_CONFIG_H
#define FM_CONFIG_H

/* ------------------------------------------------------------------ */
/* Optional user configuration                                          */
/* ------------------------------------------------------------------ */
#ifdef FM_USE_USER_CONFIG
#include "fm_config_user.h"
#endif

/*
 * BUFFERS(...) از fm_board_config.h اینجا هم اثر می‌گذارد.
 *
 * این فایل ممکن است بدون fm_board_config.h هم include شود (بیلد هاست،
 * پورت‌های کوچک)، پس اگر کلید تعریف نشده بود پیش‌فرض NORMAL می‌گیریم و
 * هیچ‌چیز عوض نمی‌شود. یعنی رفتار قبلی دست‌نخورده می‌ماند.
 */
#include "fm_switches.h"
#include "fm_board_config.h"
#ifndef FM_BUFFERS
#define FM_BUFFERS NORMAL
#endif
#ifndef FM_BUF_SCALE
#if   FM_BUFFERS == SMALL
#define FM_BUF_SCALE_NUM 1u
#define FM_BUF_SCALE_DEN 4u
#elif FM_BUFFERS == LARGE
#define FM_BUF_SCALE_NUM 4u
#define FM_BUF_SCALE_DEN 1u
#elif FM_BUFFERS == HUGE
#define FM_BUF_SCALE_NUM 8u
#define FM_BUF_SCALE_DEN 1u
#else
#define FM_BUF_SCALE_NUM 1u
#define FM_BUF_SCALE_DEN 1u
#endif
#define FM_BUF_SCALED_RAW(base) \
    (((base) * FM_BUF_SCALE_NUM) / FM_BUF_SCALE_DEN)
#define FM_BUF_SCALE(base)                                   \
    ((FM_BUF_SCALED_RAW(base) < 256u) ? 256u                 \
                                      : (FM_BUF_SCALED_RAW(base) & ~31u))
#endif

/* ------------------------------------------------------------------ */
/* Feature switches (defaults)                                         */
/* ------------------------------------------------------------------ */
#ifndef FM_USE_TEXT
#define FM_USE_TEXT 1                 /* text file API (line based)    */
#endif
#ifndef FM_USE_BINARY
#define FM_USE_BINARY 1               /* typed binary read/write       */
#endif
#ifndef FM_USE_JSON
#define FM_USE_JSON 1                 /* JSON plugin                   */
#endif
#ifndef FM_USE_DATA_LOGGER
#define FM_USE_DATA_LOGGER 1          /* data logger plugin            */
#endif
#ifndef FM_USE_EXPLORER
#define FM_USE_EXPLORER 1             /* file explorer / navigation    */
#endif
#ifndef FM_USE_ASYNC
#define FM_USE_ASYNC 1                /* cooperative async operations  */
#endif
#ifndef FM_USE_CRC
#define FM_USE_CRC 1                  /* CRC16 / CRC32 / checksums     */
#endif
#ifndef FM_USE_SAFE_WRITE
#define FM_USE_SAFE_WRITE 1           /* atomic / crash-safe writes    */
#endif
#ifndef FM_USE_MEDIA_DETECT
#define FM_USE_MEDIA_DETECT 1         /* file type detection           */
#endif
#ifndef FM_USE_STREAM
#define FM_USE_STREAM 1               /* generic stream API            */
#endif
#if FM_USE_STREAM == 0 && (FM_USE_IMAGE == 1 || FM_USE_BINARY == 1)
/* The image codecs and the typed binary API are built on streams. */
#undef FM_USE_STREAM
#define FM_USE_STREAM 1
#endif
#if FM_PORT_TIER == 1
#define FM_TIER1_FEAT_DEFAULT 0      /* file-only on <64K RAM */
#else
#define FM_TIER1_FEAT_DEFAULT 1
#endif
#ifndef FM_USE_IMAGE
#define FM_USE_IMAGE FM_TIER1_FEAT_DEFAULT  /* image plugin (BMP codec)      */
#endif
#ifndef FM_USE_AUDIO
#define FM_USE_AUDIO FM_TIER1_FEAT_DEFAULT  /* audio plugin (WAV PCM codec)  */
#endif
#ifndef FM_USE_MP3
#define FM_USE_MP3 1                  /* MP3 decoding (minimp3)        */
#endif
/* Trace the WAV PCM read path step by step through FM_Log (board
 * diagnostics: shows the exact file offset when a read stops
 * responding inside the SD/FatFS layer). Default off (zero cost). */
#ifndef FM_AUDIO_TRACE
#define FM_AUDIO_TRACE 0
#endif
#if FM_USE_AUDIO == 0
/* MP3 playback is part of the audio subsystem. */
#undef FM_USE_MP3
#define FM_USE_MP3 0
#endif
#ifndef FM_USE_VIDEO
#define FM_USE_VIDEO FM_TIER1_FEAT_DEFAULT  /* video plugin (FMRV/raw)        */
#endif
#if FM_PORT_TIER == 1
#define FM_H264_TIER_DEFAULT 0
#else
#define FM_H264_TIER_DEFAULT 1
#endif
#ifndef FM_USE_H264
#define FM_USE_H264 FM_H264_TIER_DEFAULT  /* H.264 codec (h264bsd plugin)   */
#endif
#ifndef FM_USE_MJPEG
#define FM_USE_MJPEG FM_TIER1_FEAT_DEFAULT  /* MJPEG codec (picojpeg plugin)  */
#endif
#ifndef FM_USE_JPEG
#define FM_USE_JPEG FM_TIER1_FEAT_DEFAULT  /* still JPEG (picojpeg / HW)     */
#endif
#ifndef FM_USE_PNG
#define FM_USE_PNG FM_TIER1_FEAT_DEFAULT  /* still PNG decode / encode      */
#endif
#if FM_USE_IMAGE == 0
/* Still JPEG / PNG are image codecs: with the image subsystem off they
 * must not pull in picojpeg, the inflate window or any scratch RAM. */
#undef FM_USE_JPEG
#define FM_USE_JPEG 0
#undef FM_USE_PNG
#define FM_USE_PNG 0
#endif
/*
 * PNG decode scratch.  Deflate's matching window is 32 KB; two
 * reconstructed scanlines need FM_PNG_MAX_STRIDE bytes each.  16384
 * covers 4K-wide gray and ~5461-wide RGB.  On the MCU these arrays
 * are placed in FM_IMAGE_SECTION (default ".sdram_buffers" — match
 * the Cube linker script).  Host builds keep them in normal BSS.
 * Override smaller on tiny MCUs, or disable PNG via FM_USE_PNG=0.
 */
#ifndef FM_PNG_WINDOW
#define FM_PNG_WINDOW 32768u
#endif
#ifndef FM_PNG_MAX_STRIDE
#define FM_PNG_MAX_STRIDE 16384u
#endif
#ifndef FM_IMAGE_LINE_MAX
#define FM_IMAGE_LINE_MAX 16384u      /* BMP scale-to-fit row scratch   */
#endif
#if FM_USE_VIDEO == 0
/* The H.264 / MJPEG decoders are video codec plugins. */
#undef FM_USE_H264
#define FM_USE_H264 0
#undef FM_USE_MJPEG
#define FM_USE_MJPEG 0
#endif
#ifndef FM_USE_EEPROM
#define FM_USE_EEPROM 1               /* EEPROM byte-storage adapter   */
#endif
#ifndef FM_USE_USB_MSC
#define FM_USE_USB_MSC 1              /* USB mass-storage bridge       */
#endif
#ifndef FM_USE_DEVICE_ADAPTERS
#define FM_USE_DEVICE_ADAPTERS 1      /* SD/Flash standard adapters    */
#endif
#ifndef FM_USE_PRINTF
#define FM_USE_PRINTF 1               /* Logger_Printf (vsnprintf)     */
#endif

#ifndef FM_USE_THREADS
#define FM_USE_THREADS 0              /* 1 = thread-safe core          */
#endif
#ifndef FM_USE_CACHE
#define FM_USE_CACHE 0                /* optional file-level cache     */
#endif
#ifndef FM_CACHE_LINE_SIZE
#define FM_CACHE_LINE_SIZE 512        /* read-cache block size (bytes) */
#endif
#ifndef FM_CACHE_LINES
#define FM_CACHE_LINES 16             /* number of read-cache blocks   */
#endif
#ifndef FM_USE_DMA
#define FM_USE_DMA 0                  /* DMA alignment hooks           */
#endif

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */
#ifndef FM_USE_RAMDISK
#define FM_USE_RAMDISK 1              /* RAM disk storage driver       */
#endif
#ifndef FM_USE_FMFS
#define FM_USE_FMFS 1                 /* built-in mini flash FS        */
#endif
#ifndef FM_USE_POSIX
#define FM_USE_POSIX 0                /* host POSIX FS adapter         */
#endif
#ifndef FM_USE_FATFS
#define FM_USE_FATFS 0                /* FatFS adapter (see backends)  */
#endif
#ifndef FM_USE_LITTLEFS
#define FM_USE_LITTLEFS 0             /* LittleFS adapter              */
#endif

/* ------------------------------------------------------------------ */
/* Sizing limits (adjust for your target)                              */
/* ------------------------------------------------------------------ */
#ifndef FM_PATH_MAX
#define FM_PATH_MAX 160               /* max absolute path length      */
#endif
#ifndef FM_NAME_MAX
#define FM_NAME_MAX 64                /* max file/dir entry name       */
#endif
#ifndef FM_MAX_OPEN_FILES
#define FM_MAX_OPEN_FILES 8           /* advanced file handle pool     */
#endif
#ifndef FM_MAX_OPEN_DIRS
#define FM_MAX_OPEN_DIRS 4            /* directory handle pool         */
#endif
#ifndef FM_DIR_MAX_DEPTH
#define FM_DIR_MAX_DEPTH 16           /* recursion depth for scanning  */
#endif
#ifndef FM_MAX_STORAGES
#define FM_MAX_STORAGES 4             /* registered storage devices    */
#endif
#ifndef FM_MAX_MOUNTS
#define FM_MAX_MOUNTS 4               /* mounted filesystem mount pts  */
#endif
#ifndef FM_MAX_FS
#define FM_MAX_FS 4                   /* registered FS adapters        */
#endif
#ifndef FM_FMFS_MAX_ENTRIES
#define FM_FMFS_MAX_ENTRIES 32        /* built-in FS file/dir entries  */
#endif
#ifndef FM_INTERNAL_BUFFER_SIZE
#define FM_INTERNAL_BUFFER_SIZE 512   /* core scratch buffer (chunks)  */
#endif
#ifndef FM_FS_HANDLE_SIZE
#define FM_FS_HANDLE_SIZE 24          /* per-open handle scratch (VFS) */
#endif
#ifndef FM_FS_DIRHANDLE_SIZE
/* Must be large enough for the largest enabled backend directory handle.
 * The POSIX adapter stores a DIR pointer plus a bounded path here. */
#define FM_FS_DIRHANDLE_SIZE (FM_PATH_MAX + 32) /* per-open dir handle */
#endif
#ifndef FM_STORAGE_DRIVER_DATA_SIZE
#define FM_STORAGE_DRIVER_DATA_SIZE 32   /* per-storage driver context    */
#endif
#ifndef FM_MAX_EXPLORER_ITEMS
#define FM_MAX_EXPLORER_ITEMS 32      /* cached entries in explorer    */
#endif
#ifndef FM_EXPLORER_MAX_DEPTH
#define FM_EXPLORER_MAX_DEPTH 16      /* back-history depth            */
#endif
#ifndef FM_LOGGER_BUFFER_SIZE
#define FM_LOGGER_BUFFER_SIZE 512     /* logger write buffer           */
#endif
#ifndef FM_JSON_MAX_TOKENS
#define FM_JSON_MAX_TOKENS 256        /* JSON parse token capacity     */
#endif
#ifndef FM_ASYNC_CHUNK_SIZE
#define FM_ASYNC_CHUNK_SIZE FM_BUF_SCALE(512u)  /* async copy chunk    */
#endif
#ifndef FM_MEDIA_MAGIC_SCAN
#define FM_MEDIA_MAGIC_SCAN 16        /* bytes read for magic detect   */
#endif
/* Optional extended audio codecs (off on tiny MCUs)
 * TIER 1 (F1/G0/L4) has <64K RAM — disable heavy codecs by default
 * to save .text and heap. User can still force FM_USE_FLAC=1
 * in fm_board_config.h if needed. */
#if FM_PORT_TIER == 1
#define FM_TIER1_CODEC_DEFAULT 0
#else
#define FM_TIER1_CODEC_DEFAULT 1
#endif
#ifndef FM_USE_FLAC
#define FM_USE_FLAC FM_TIER1_CODEC_DEFAULT
#endif
#ifndef FM_USE_VORBIS
#define FM_USE_VORBIS FM_TIER1_CODEC_DEFAULT
#endif
#ifndef FM_USE_OPUS
#define FM_USE_OPUS FM_TIER1_CODEC_DEFAULT
#endif
#ifndef FM_OPUS_USE_LIBOPUS
/* Decode Opus with the vendored libopus (third_party/opus).  Without it
 * there is no Opus decoder at all: Opus files / tracks are reported as
 * FM_ERR_UNSUPPORTED (never as silent "successful" playback).
 *
 * The vendored tree is always compiled by IDE projects that drop this
 * library into their source tree (STM32CubeIDE compiles every .c file
 * under FileManager_lib), so on-target builds default to real decoding.
 * The host Makefile/CMake only compiles libopus when asked for
 * (make FM_USE_OPUS_LIB=1), and tools/check_ports.sh compiles each port
 * with -DFM_HOST_BUILD, so the default resolves to "no Opus decoder"
 * there.  Override explicitly in fm_board_config.h if you need to. */
#if FM_USE_OPUS && !defined(FM_HOST_BUILD)
#define FM_OPUS_USE_LIBOPUS 1
#else
#define FM_OPUS_USE_LIBOPUS 0
#endif
#endif
#ifndef FM_OPUS_MAX_FILE_BYTES
/* Largest Ogg Opus file pulled whole into RAM for decode (like FLAC/Vorbis). */
#define FM_OPUS_MAX_FILE_BYTES (5u * 1024u * 1024u)
#endif
#ifndef FM_OPUS_MAX_PCM_BYTES
/* Whole-file decode cap: stereo 48 kHz s16 needs 192 KB per second.
 * A longer track is truncated to this many bytes of PCM (and a warning
 * logged) instead of exhausting the heap.  Raise/lower it in
 * fm_board_config.h for your SDRAM size. */
#if FM_BUFFERS == SMALL
#define FM_OPUS_MAX_PCM_BYTES (2u * 1024u * 1024u)
#else
#define FM_OPUS_MAX_PCM_BYTES (16u * 1024u * 1024u)
#endif
#endif
#ifndef FM_USE_AAC
#define FM_USE_AAC FM_TIER1_CODEC_DEFAULT
#endif
#if FM_USE_AUDIO == 0
/* Every audio codec is part of the audio subsystem.  With FM_USE_AUDIO
 * off none of them may be compiled: no decoder tables, no codec
 * statics, no references to the (compiled-out) decoder registry. */
#undef FM_USE_MP3
#define FM_USE_MP3 0
#undef FM_USE_FLAC
#define FM_USE_FLAC 0
#undef FM_USE_VORBIS
#define FM_USE_VORBIS 0
#undef FM_USE_OPUS
#define FM_USE_OPUS 0
#undef FM_USE_AAC
#define FM_USE_AAC 0
#endif
#if FM_USE_OPUS == 0
#undef FM_OPUS_USE_LIBOPUS
#define FM_OPUS_USE_LIBOPUS 0
#endif
#ifndef FM_VIDEO_MAX_FRAMES
/* 4096 frames = 273 s at 15 fps, 136 s at 30 fps (was 1024 = 68 s).
 * Scaled with FM_BUFFERS for tiny MCUs: SMALL->1024, NORMAL->4096,
 * LARGE->4096, HUGE->8192. Longer clips then stream from the movi list
 * (avi_audio continuation) instead of truncating. */
#if FM_BUFFERS == SMALL
#define FM_VIDEO_MAX_FRAMES 1024
#elif FM_BUFFERS == HUGE
#define FM_VIDEO_MAX_FRAMES 8192
#else
#define FM_VIDEO_MAX_FRAMES 4096
#endif
#endif
#ifndef FM_VIDEO_MAX_AUDIO_FRAMES
/* Audio packet index of an open video, sized independently of the video
 * frame index: a compressed track has several packets per picture.
 * Packets past the index are streamed from the container (AVI movi /
 * MKV clusters); MP4 and FLV stop at this limit and say so in the log. */
#define FM_VIDEO_MAX_AUDIO_FRAMES FM_VIDEO_MAX_FRAMES
#endif
#ifndef FM_VIDEO_AUDIO_PRIVATE_MAX
/* Codec setup data kept per open video (MKV CodecPrivate, MP4 dfLa /
 * dOps).  Vorbis setup headers are routinely 3-4 KB. */
#define FM_VIDEO_AUDIO_PRIVATE_MAX 4096
#endif
#ifndef FM_MAX_VIDEO
#define FM_MAX_VIDEO 4                 /* simultaneously open videos      */
#endif
#ifndef FM_MAX_AUDIO
/* Allow one music + one video-audio decode concurrently (FLAC/Opus). */
#if FM_USE_FLAC || FM_USE_VORBIS || FM_USE_OPUS
#define FM_MAX_AUDIO 2
#else
#define FM_MAX_AUDIO 1
#endif
#endif
/*
 * MP3 decode input buffer (bytes).  The MP3 stream is pulled into this
 * buffer in blocks and decoded out of it (see FM_MP3_LOW_WATER).  The
 * larger the buffer, the fewer File_Read calls one audio refill needs:
 * at 4 KB a 48 kHz refill costs a handful of reads, at 32 KB the whole
 * refill usually fits in the buffer (0-1 reads).  Cost: one buffer per
 * simultaneously open stream (FM_MAX_AUDIO x FM_MP3_INPUT_BUF bytes).
 */
#ifndef FM_MP3_INPUT_BUF
#define FM_MP3_INPUT_BUF 4096         /* MP3 decode input buffer (bytes)  */
#endif
/*
 * MP3 input read-ahead low-water mark (bytes).  When the unconsumed tail
 * of FM_MP3_INPUT_BUF drops below this value, the buffer is topped up to
 * full in ONE File_Read.  This batches the stream into a few large
 * sequential reads per refill instead of one small read per MP3 frame.
 *
 * Why it matters for realtime playback: every File_Read is a separate
 * SDMMC command, and the *number* of commands — not the number of bytes —
 * dominates refill jitter on real cards (each command adds fixed overhead
 * plus a chance of an intermittent 10-200 ms card stall).  A refill that
 * issues 15 small reads has a high probability of hitting one stall;
 * a refill that issues 3-4 large reads does not.  That is the difference
 * between WAV (one big read per refill, smooth) and MP3 (many small
 * reads, crackle/underrun on the same card).
 *
 * 0 = automatic: FM_MP3_INPUT_BUF / 4 (clamped to >= 1024).
 * When overriding, keep FM_MP3_LOW_WATER <= FM_MP3_INPUT_BUF / 2.
 */
#ifndef FM_MP3_LOW_WATER
#define FM_MP3_LOW_WATER 0
#endif
/*
 * WAV PCM is pulled in this many samples per File_Read (then converted
 * into the caller's buffer).  A realtime player that DMA-plays out of the
 * same SRAM as SDMMC must never let FatFS/SDMMC DMA target the I2S/SAI
 * buffer: on STM32H7 that dual-master access deadlocks the AXI bus —
 * which is exactly the "hang on the second WAV half after an MP3 track"
 * failure.  Staging through this small aligned bounce buffer keeps the
 * two DMAs on separate memory.  2048 samples = 4 KB at 16-bit.
 */
#ifndef FM_AUDIO_PCM_CHUNK
#define FM_AUDIO_PCM_CHUNK FM_BUF_SCALE(2048u)  /* WAV File_Read size    */
#endif

/* ------------------------------------------------------------------ */
/* Internal buffer control                                             */
/* ------------------------------------------------------------------ */
#ifndef FM_USE_INTERNAL_BUFFER
#define FM_USE_INTERNAL_BUFFER 1      /* core scratch buffer present   */
#endif

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */
#ifndef FM_DEBUG
#define FM_DEBUG 0                    /* assertions / debug checks     */
#endif
#ifndef FM_HOST_BUILD
#define FM_HOST_BUILD 0               /* host (desktop) build          */
#endif

/* Re-evaluate SDRAM placement now that FM_HOST_BUILD has a value. */
#ifndef FM_IMAGE_SCRATCH
# ifdef FM_IMAGE_SECTION
#  define FM_IMAGE_SCRATCH \
        __attribute__((section(FM_IMAGE_SECTION), aligned(32)))
# elif !FM_HOST_BUILD
#  define FM_IMAGE_SECTION ".sdram_buffers"
#  define FM_IMAGE_SCRATCH \
        __attribute__((section(FM_IMAGE_SECTION), aligned(32)))
# else
#  define FM_IMAGE_SCRATCH
# endif
#endif

/* DMA/SDMMC on Cortex-M7 requires 32-byte alignment whenever D-Cache
 * may be on.  Gating this on FM_USE_DMA left the FatFS/File_Copy scratch
 * unaligned; Cube's SD DMA then invalidated neighbouring cache lines and
 * FatFS returned FR_INT_ERR (-26).  Always align on the target. */
#ifndef FM_DMA_ALIGNED
#if !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
#define FM_DMA_ALIGNED __attribute__((aligned(32)))
#else
#define FM_DMA_ALIGNED
#endif
#endif

#ifdef __cplusplus
#define FM_BEGIN_DECLS extern "C" {
#define FM_END_DECLS }
#else
#define FM_BEGIN_DECLS
#define FM_END_DECLS
#endif

#endif /* FM_CONFIG_H */
