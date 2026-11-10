# Third-party libraries

Vendored dependencies that are too large or too specialized to live inside
`src/`.  Each one keeps its own license file next to the sources.

| Directory   | Project                                        | Version / commit                                    | License          |
|-------------|-----------------------------------------------|-----------------------------------------------------|------------------|
| `littlefs/` | [littlefs](https://github.com/littlefs-project/littlefs) | v2.11.3                                     | BSD-3-Clause     |
| `h264bsd/`  | [h264bsd](https://github.com/oneam/h264bsd)    | `42bcb5d753ad86d84903354bf3c68423c28adb7b`          | Apache-2.0 + MIT |
| `minimp3/`  | [minimp3](https://github.com/lieff/minimp3)    | `afb604c06bc8beb145fecd42c0ceb5b867c75f7f`          | CC0-1.0          |
| `picojpeg/` | [picojpeg](https://github.com/richgel999/picojpeg) | public-domain single-file decoder                | Public domain    |
| `opus/`     | [libopus](https://github.com/xiph/opus) (decoder-only) | master snapshot, 2025 (post-1.5: DRED/OSCE era) | BSD-3-Clause     |

## littlefs

A small, power-loss-tolerant embedded filesystem.  It is used by the
`FM_USE_LITTLEFS` filesystem adapter (`backends/fm_littlefs.c`).

Only the amalgamated core is kept (`lfs.c`, `lfs.h`, `lfs_util.h`,
`lfs_util.c`).  Configure it with `lfs_config.h` style options or the
`LFS_*` macros; the adapter supplies the read/write/erase callbacks from a
`FM_Storage` driver.

## h264bsd

A self-contained software H.264 decoder extracted from the Android Open
Source Project.  It is the default decode backend for the optional
`FM_USE_H264` codec plugin (`src/fm_h264.c`) and produces YUV420p frames.

Notes:

* The decoder is **not** allocation-free: it allocates its DPB/reference
  picture buffers with `malloc`/`free` (size depends on the configured
  maximum resolution).  The FileManager core itself stays allocation-free;
  only this optional plugin allocates.
* Memory usage can be tuned through `h264bsd_cfg.h`
  (`MAX_NUM_REF_PICS`, ...) and `h264bsd_storage.c`.
* The decoder expects an Annex-B byte stream (`00 00 01` start codes).
  The FileManager AVI/MP4 demuxers convert length-prefixed AVCC data to
  Annex-B before handing it over.

## minimp3

A single-header MP3 (MPEG-1/2 Layer III) decoder with a tiny RAM footprint
(~a few KB of work memory) and no heap requirement.  It backs the
`FM_USE_MP3` codec in the audio subsystem (`src/fm_audio.c`).

Only the core `minimp3.h` is kept.  It decodes raw MP3 frames; ID3v2/ID3v1
tag skipping is handled by the FileManager audio adapter.

## picojpeg

A public-domain baseline JPEG decoder optimized for microcontrollers
(~2.3 KB work memory, no heap).  It backs the `FM_USE_MJPEG` codec plugin
(`src/fm_mjpeg.c`) used for MJPEG video (USB cameras, ESP32-CAM, AVI MJPEG).

Notes:

* Baseline sequential only (grayscale + YCbCr); progressive JPEG is not
  supported.
* Not thread-safe by design (static work arrays).
* The decoder pulls bytes through a callback; the FileManager plugin feeds
  it from an in-memory frame buffer.

## opus

The reference [libopus](https://github.com/xiph/opus) implementation,
**trimmed to a decoder-only embedded vendor** (this library is a media
*player*).  It backs the `FM_USE_OPUS` codec (`src/fm_opus.c`): real
`opus_decode()` of Ogg Opus music files (`Audio_Open("*.opus")`) and of
Opus audio tracks inside Matroska/`FM_VIDEO_AUDIO_OPUS` video.

### What was kept / removed and why

Upstream libopus is built by CMake/meson, which pass a pile of
`-D` flags and per-architecture source lists.  IDE projects
(STM32CubeIDE) instead compile **every** `.c` file under the project
tree with one global flag set.  Feeding the full upstream tree to such a
build fails three ways, so the vendor is pre-trimmed:

* Removed files that carry their own `main()` (`opus_demo.c`,
  `opus_compare.c`, `osce_compare.c`, `qext_compare.c`,
  `repacketizer_demo.c`, `celt/opus_custom_demo.c`) and the unit-test
  trees (`celt/tests`, `silk/tests`, `celt/dump_modes`) — they would
  collide with the application's `main()` at link time.
* Removed the encoder entry points (`src/opus_encoder.c`,
  `opus_multistream_encoder.c`, `opus_projection_encoder.c`,
  `analysis.c`, `mlp*.c`) and the SILK encoder guts
  (`silk/float/`, `silk/fixed/`, `silk/enc_API.c`, `init_encoder.c`,
  `control_codec.c`, `HP_variable_cutoff.c`) — they need
  `-Isilk/float -Isilk/fixed` include paths that IDE builds do not
  have, and a player never encodes.  Decoder closure is link-complete
  on its own (verified: all objects link with no undefined symbols).
* Removed NEON/NE10 sources (`celt/arm/*neon*`, `silk/arm/*neon*`,
  `silk/fixed/arm/`) — they need `-mfpu=neon`, not `-mfpu=fpv5-d16`.
* Removed x86/MIPS/xtensa SIMD directories — dead code on every
  supported target.

### Build configuration: nothing to add

Upstream errors out (`OPUS_BUILD _MUST_ be defined`, `VAR_ARRAYS/USE_ALLOCA/...`)
unless its build system passes `-D` flags.  IDE builds pass none, so the
vendor carries three clearly-marked default blocks (search
`FM vendor patch`):

| Site | Default | Effect |
|------|---------|--------|
| `include/opus_types.h` | `OPUS_BUILD=1` | required by `src/opus_decoder.c` (pulled into every TU) |
| `celt/stack_alloc.h`   | `VAR_ARRAYS` (C99 VLAs) | temporary-allocation mode |
| `celt/float_cast.h`    | `HAVE_LRINTF` on glibc/newlib | uses `lrintf()` instead of a slow cast (removes the `#warning`) |

Any explicit `-D` from your build system still wins, and all defaults can
be turned off with `-DFM_OPUS_NO_VENDOR_DEFAULTS` (then supply
`-DOPUS_BUILD -DVAR_ARRAYS` yourself).  `make -j` in the repo root never
compiles this tree unless asked: `make FM_USE_OPUS_LIB=1` (host) or
`-DFM_USE_OPUS_LIB=ON` (CMake) build it and switch `src/fm_opus.c` from
the silence fallback to real decoding.

### Memory notes

* Decoding is whole-file into RAM (same model as FLAC/Vorbis here):
  compressed file ≤ `FM_OPUS_MAX_FILE_BYTES` (5 MB), decoded PCM capped
  by `FM_OPUS_MAX_PCM_BYTES` (16 MB default; longer tracks are
  truncated with a log line).  Both are overridable in
  `fm_board_config.h`.  Stereo 48 kHz s16 needs 192 KB of PCM per
  second.
* libopus allocates its decoder state with `malloc` internally
  (`opus_decoder_get_size(2)` ≈ 20 KB); route it to SDRAM via
  `FM_Allocator_Set()`/`_sbrk` if the default heap is small.
* Optional: `-DFIXED_POINT` builds the fixed-point CELT/SILK paths
  (smaller, no FPU use); the default float build performs well on the
  H7's fpv5-d16 FPU.
