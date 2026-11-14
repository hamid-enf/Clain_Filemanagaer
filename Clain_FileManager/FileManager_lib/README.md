# FileManager

> راهنمای فارسی کتابخانه (قابلیت‌ها، شروع سریع، همهٔ توابع با نمونه‌کد):
> [`docs/GUIDE_FA.md`](docs/GUIDE_FA.md)

A lightweight, allocation-free, portable **Unified File & Storage Framework** for C projects, embedded firmware and host tools.

The library puts a high-level API over filesystems and storage drivers. Applications can use `File_WriteText`, `File_ReadAll`, `File_Copy`, `File_Move`, `Directory_Create` and similar operations without handling block sizes, chunks, directory handles or filesystem-specific calls.

## High-level start

For a caller-owned RAM, SDRAM or memory-mapped storage region, setup can be reduced to one call:

```c
#include "file_manager.h"

static uint8_t storage_memory[64 * 1024];
static FM_Storage storage;
static FM_FS fs;

FileManager_Init();
FileManager_MountMemory(&storage, &fs, "RAM", storage_memory,
                         sizeof(storage_memory), 1, FM_FALSE);

Directory_Create("/config");
File_WriteText("/config/app.txt", "hello\n");

char text[128];
File_ReadText("/config/app.txt", text, sizeof(text), NULL);
File_Copy("/config/app.txt", "/config/backup.txt", NULL, NULL);
File_Move("/config/backup.txt", "/backup.txt", NULL, NULL);
```

`FMFS` stores its metadata in the supplied storage and uses the storage callback table for file data. The same high-level API therefore works with RAM/SDRAM and with custom byte/block drivers.

## Features

| Feature | Status |
|---------|--------|
| File API: create, read, write, append, copy, move, delete | ✅ |
| Directory creation, recursive listing, copy and delete | ✅ |
| Safe path join/normalize and mount-aware VFS | ✅ |
| Built-in FMFS on caller-owned storage | ✅ |
| LittleFS adapter (vendored littlefs v2.11) | ✅ |
| FatFS adapter (uses the FatFS that CubeMX generates) | ✅ |
| POSIX filesystem adapter for host testing | ✅ |
| Pointer-backed RAM/SDRAM/memory storage adapter | ✅ |
| Custom storage callbacks for SD, eMMC, NOR/QSPI and other devices | ✅ |
| Standard SD/eMMC block-device adapter | ✅ |
| Standard NOR/QSPI/internal Flash page/erase adapter | ✅ |
| NOR 512-byte logical-block translation for FatFS / USB MSC | ✅ |
| Text line API and streaming file editing | ✅ |
| Typed binary I/O with explicit endianness | ✅ |
| Buffer-based JSON parser/writer with dotted paths and arrays | ✅ |
| CSV, Binary and JSONL data logger | ✅ |
| Temporary-file replace and flush-backed append (backend-dependent guarantees) | ✅ |
| Cooperative asynchronous copy, move, scan and delete | ✅ |
| CRC16, CRC32 and checksums | ✅ |
| File/memory stream abstraction | ✅ |
| Optional file read-cache (block cache, runtime on/off + stats) | ✅ |
| Pluggable allocator hooks (external RAM / static pool) | ✅ |
| Hardware decoder registry (optional HW H.264/JPEG/AAC) | ✅ |
| BMP / JPEG / PNG load (streamed, scale-to-fit) and save | ✅ |
| GRAY8, RGB/BGR565, RGB/BGR888, RGBA/BGRA8888 raw layouts | ✅ |
| In-place image effects: invert, grayscale, sepia, brightness, contrast | ✅ |
| Image/video scaling (nearest + bilinear, playback and storage) | ✅ |
| WAV PCM audio read/write | ✅ |
| MP3 playback (vendored minimp3, mono/stereo + downmix) | ✅ |
| Audio saving: WAV, IMA ADPCM and FLAC encoders built in, format from the file name, registry for MP3 / Vorbis / Opus encoders, file-to-file `Audio_Convert` | ✅ |
| FMRV raw video read/write and codec plugin hook | ✅ |
| Video saving: Motion-JPEG in AVI with a PCM sound track (`VideoFileWriter_*`), file-to-file `Video_Convert` | ✅ |
| AVI, MP4 (ISO BMFF), Matroska/WebM and FLV container parsing | ✅ |
| H.264 software decoding via vendored h264bsd (self-healing: keyframe replay after a broken reference chain) | ✅ |
| MJPEG decoding via vendored picojpeg (USB/ESP32 cameras) | ✅ |
| Audio-track demux from MP4 (AAC/ADTS) and AVI (PCM) | ✅ |
| Media player: folder scan, playlist, next/prev, seek | ✅ |
| Audio recorder (WAV into a folder, auto-named) | ✅ |
| EEPROM page-aware read/write/update/erase adapter | ✅ |
| USB MSC block bridge for TinyUSB/STM32 USB ports | ✅ |
| Thread-safe core (FreeRTOS / CMSIS-RTOS2 / pthread ports) | ✅ |
| Dynamic allocation in the file / storage core | None (decoders: see [Memory allocation hooks](#memory-allocation-hooks)) |

### Exact media format contract

File/container formats and decoded raw-buffer layouts are separate concepts:

| Operation | Built in | Optional/plugin behavior |
|---|---|---|
| Audio read | WAV/PCM and MP3 are detected from file bytes and decoded to PCM16 | AAC/ADTS decoder registry |
| Audio write | `.wav` (PCM16, or IMA ADPCM when asked for) and `.flac` are encoded internally | `.mp3` / `.ogg` / `.opus` are selected from the name but require an encoder registered with `Audio_RegisterEncoder`; otherwise `FM_ERR_UNSUPPORTED` and no file is created |
| Image file read/write | BMP, JPEG, PNG (magic-byte detect; Save picks the encoder from the destination extension). PNG read: every colour type and bit depth, Adam7 interlace, CRC and Adler-32 verified; output is RGB888 | STM32 JPEG peripheral via `Image_RegisterCodec` |
| Image raw output | GRAY8, RGB565, BGR565, RGB888, BGR888, RGBA8888, BGRA8888 | — |
| Video read | FMRV, AVI, MP4, Matroska/WebM and FLV container detection; RAW, bundled MJPEG and H.264 decode where supported | Hardware codec registry |
| Video raw output | Same caller-selected pixel layouts as images | Conversion uses a caller-owned RGB888 work buffer |
| Video write | FMRV raw RGB888; AVI with MJPEG pictures (encoded here, or ready JPEG frames from a camera) and 16-bit PCM sound | There is no H.264 / MP4 encoder |

Reading is content-based: renaming an MP3 file to `.wav` does not turn it into
WAV. Writing starts from raw PCM/pixels, so the desired output cannot be
"detected" from the input buffer; `AUTO` deliberately selects it from the
destination extension and then requires a real matching encoder.

## Architecture

```text
Application
    │
    ▼
High-level File / Directory / Media API
    │
    ├── Text / Binary / JSON / BMP / WAV / FMRV
    ├── Logger / Explorer / Async
    └── Safe write / CRC / Streams
    │
    ▼
Virtual File System (VFS)
    │
    ▼
Filesystem adapter
    ├── FMFS  (built-in fixed-table filesystem)
    ├── POSIX (host testing)
    ├── LittleFS (vendored littlefs)
    ├── FatFS (uses the FatFS generated by STM32CubeMX)
    └── user adapter: custom...
    │
    ▼
Storage callbacks
    ├── pointer-backed RAM / SDRAM / memory map
    └── user driver: SD, eMMC, NOR, QSPI, NAND...
```

## Storage model

A pointer-backed region can be initialized with:

```c
MemoryStorage_InitTyped(&storage, "QSPI", FM_STORAGE_QSPI,
                        mapped_address, capacity, block_size, FM_FALSE);
Storage_Register(&storage);
Storage_Mount("QSPI");
FMFS_Init(&fs);
FS_MountDefault(&fs);
```

For SD/eMMC/USB block devices, the standard adapter already models the common vendor API (`read_blocks`, `write_blocks`, `sync`, optional `erase_blocks`, `is_ready`):

```c
FM_BlockDeviceConfig sd_api = {
    .name = "SD", .type = FM_STORAGE_SD,
    .capacity = sd_capacity, .block_size = 512,
    .present = FM_TRUE, .context = &sd_context,
    .init = sd_init, .deinit = sd_deinit,
    .is_ready = sd_ready, .sync = sd_sync,
    .read_blocks = sd_read_blocks,
    .write_blocks = sd_write_blocks,
    .erase_blocks = sd_erase_blocks
};
FM_BlockDevice sd_device;
BlockDevice_Init(&sd_device, &sd_api);
Storage_InitBlockDevice(&storage, &sd_device);
FileManager_MountStorage(&storage, &fs);
```

For QSPI/NOR/internal Flash, use `FM_FlashDeviceConfig` with `read`, `program` (page-aware wrapper) and `erase` callbacks.

For other command-based hardware, applications provide callbacks through `Storage_InitDriver` and then use the same high-level mount helper:

```c
FM_StorageConfig driver = {
    .name = "SD",
    .type = FM_STORAGE_SD,
    .capacity = sd_capacity,
    .block_size = 512,
    .read_size = 512,
    .write_size = 512,
    .erase_size = 4096,
    .present = FM_TRUE,
    .user_data = &sd_context,
    .read = sd_read,
    .write = sd_write,
    .erase = sd_erase,
    .sync = sd_sync
};

Storage_InitDriver(&storage, &driver);
FileManager_MountStorage(&storage, &fs);
File_WriteText("/boot/status.txt", "ready");
```

The hardware callbacks remain device-specific, but registration, mounting, metadata and all file operations stay behind the same API.

Rules that hold for every storage and file system:

* **`Storage_Unmount` / `Storage_Format` are `FM_ERR_BUSY` while a file
  system is mounted on the storage.**  `FS_Unmount` first (it unmounts
  the storage too); to empty a mounted volume use `FS_Format`.
* **`FS_Mount` of an `FM_FS` that was never initialised** (all zero, or
  the `*_Init` call failed and its result was not checked) is
  `FM_ERR_INVALID_ARGUMENT`.
* **`FM_MAX_FS` (4) volumes per kind of file system.**  `FMFS_Init`,
  `LittleFS_Init` and `FatFS_Init` each hand out one of 4 contexts;
  `FS_Unmount` and a failed `FS_Mount` give it back.  When all are
  handed out, the next `*_Init` takes over the oldest one that is not in
  a mount -- the `FM_FS` that had it answers `FM_ERR_NOT_READY` to
  `FS_Mount` until it is initialised again.  A mounted volume never
  loses its context.
* **`FS_Format` leaves the volume mounted and empty.**  A LittleFS
  volume that does not mount (auto-format off, medium blank or damaged)
  has no mount path to format: `LittleFS_FormatStorage(&storage)`, then
  `FS_Mount` with the same `FM_FS`.
* **`FS_Check`** verifies the metadata, not the content of files: on
  FMFS the table (checksum, and that it makes sense as a whole); on
  LittleFS every metadata block of the volume.  On FatFS and POSIX it
  examines nothing and answers `FM_OK`.
* **LittleFS geometry.**  A storage whose erase unit is smaller than
  128 bytes (RAM, SDRAM: 1) gets blocks that are a multiple of the unit
  and at least 512 bytes; a storage with room for fewer than 2 blocks is
  `FM_ERR_NO_SPACE` at mount.
* **`FM_Qspi_Write` erases only when it has to.**  Per sector: bytes
  that are already there cost nothing; bytes that are still `0xFF` are
  programmed in place (no erase -- this is how LittleFS appends); a
  whole sector is erased and programmed; anything else is
  read-modify-erase-write through the sector buffer given to
  `FM_Qspi_Init` (`FM_ERR_UNSUPPORTED` without one).  After a soft reset
  of the chip the driver enters 4-byte address mode again (parts above
  16 MB), and an erase that timed out is repeated, not reported as done.

## EEPROM and USB

EEPROMs can be used directly for byte operations:

```c
EEPROM_Write(&eeprom, 10, packet, packet_size);
EEPROM_Update(&eeprom, 10, packet, packet_size); /* skips unchanged data */
EEPROM_Read(&eeprom, 10, packet, packet_size);
```

They can also be wrapped as an FMFS storage with `EEPROM_StorageInit`.

`USB_MSC` is a transport-neutral bridge. It provides capacity, read, write
and sync callbacks for the USB MSC/SCSI layer used by TinyUSB, STM32 USB
Device or another USB stack:

```c
FM_USB_MSC usb;
USB_MSC_Init(&usb, &storage, 512);
/* The platform USB port calls USB_MSC_ReadBlocks/WriteBlocks. */
USB_MSC_Attach(&usb, &platform_usb_port_ops, platform_context);
```

The bridge does not contain an MCU-specific USB device stack. To have a PC
mount the medium as a normal disk, the exposed bytes must contain a
PC-recognized filesystem such as FAT; FMFS is intended for embedded use and
will appear as raw blocks to the PC unless a host-side FMFS driver is used.

## Build and test

```bash
make                 # library + host test
make test            # run the host integration suite
make examples        # build examples 00-10
make examples_run    # run examples 00-10
```

Changing a switch (`make FM_USE_JSON=0`, `CFLAGS_EXTRA=...`, `CC=clang`)
rebuilds the objects by itself: the compiler and its flags are kept in
`.fm_build_flags` and every object depends on it (GNU make 4.2 or newer;
with an older make run `make clean` first).

A header that changes rebuilds the objects that include it (`-MMD -MP`
dependency files next to the objects; `tools/check_deps.sh` checks it).

**One set of options for the library and the application.**
`FileManager_Init()` is a macro: it passes the sizes the *application*
sees for `FM_File`, `FM_Dir`, `FM_Storage`, `FM_FS` and `FM_DirEntry` to
`FileManager_InitChecked()`, which compares them with the sizes the
*library* was compiled with.  If they differ -- a prebuilt
`libfilemanager.a` and an application with another `FM_PATH_MAX`,
`FM_NAME_MAX` or `FM_USE_*` set -- it returns `FM_ERR_INTERNAL` (-26)
and initialises nothing, instead of letting the two sides write over
each other's structures later.  Check its result.  The check sees only
options that change one of those five sizes; it does not detect layout changes
that preserve the sizes, or incompatible configuration that does not affect
these structures.  `-DFM_NO_ABI_CHECK=1`
removes the macro; `(FileManager_Init)()` calls the plain function.

**Video audio and cache.** `SimpleVideo_AudioDataRead` returns interleaved
16-bit stereo frames (mono is duplicated); provide room for two samples per
frame. It consumes the same track as playback, at most one packet per call.
An unsupported track returns `FM_ERR_UNSUPPORTED`; a mono packet that cannot
fit returns `FM_ERR_TOO_LARGE`. This is a public behaviour change.

The media cache skips an open entry when evicting, but the search is bounded
to four busy candidates; it can fail to make room even if another entry
could be evicted.

**What the tests are, and are not.**  Everything above runs on a PC: the
suite as a 64-bit and as a 32-bit program, under ASan / UBSan / TSan and
in the configurations selected by `tools/check_matrix.sh` and
`tools/check_32bit.sh`. The latest full matrix had an intermittent threaded
media checksum failure; see `../../audit_prompts/01_file_storage_filesystem.md`
for the result and reruns. Previous ARM builds were compiled and measured (code
size, stack frames) but **nothing in this repository has been run on a
board**: the STM32 ports, the QSPI / SD / I2S / DMA examples and the
hardware JPEG path are tested against stub HALs. The firmware edits in the
latest audit fix were not compiled for ARM. Bring-up on real
hardware is still to be done by whoever uses them.

### STM32CubeIDE: the whole folder can sit in the project

STM32CubeIDE compiles every `.c` file under a source folder, and the
whole `FileManager_lib/` folder is meant to be dropped into the project
as it is.  Nothing has to be marked "Exclude from build":

- **Firmware code** lives in `src/`, `backends/`, `third_party/` and
  `top_api/` only.
- **Every other `.c`** (`examples/`, `test/`, `tools/`, the `diag_*` /
  `fm_*` files next to this README) carries the marker `FM-NOT-FIRMWARE`
  and compiles to an empty object in a firmware build.  No second
  `main()`, no second `_sbrk`, no missing host header.

| Files | Guard | Compiled when |
|---|---|---|
| `examples/*.c` (PC examples), `test/**/*.c`, `tools/**/*.c` | `#ifndef USE_HAL_DRIVER` | never in a CubeMX project (it always defines `USE_HAL_DRIVER`); always by `make` / CMake on a PC |
| `examples/sdcard_fatfs.c`, `examples/sdcard_fm_diskio.c` | `FM_EXAMPLES_DIR_BUILD` | only if you define it; `top_api/` holds the same two files and is the copy the firmware builds |
| `diag_heap_test.c` | `FM_DIAG_HEAP_TEST` | only if you define it (has its own `main()`) |
| `fm_override.c` | `FM_DIAG_SYSCALLS_H7` | only if you define it (replaces `_sbrk` and the newlib stubs of `syscalls.c` / `sysmem.c`) |
| `diag_sbrk_patch.c` | `FM_DIAG_SBRK_F4` | only if you define it (STM32F4 variant of the same) |
| `fm_log_test.c` | `FM_DIAG_LOG_TEST` | only if you define it |

`examples/*.h` and `top_api/*.h` are the same 34 headers with the same
include guards; `top_api/` uses paths relative to the project
(`../include/...`), `examples/` uses flat ones.  The board code includes
the `top_api/` copies by relative path, so having both folders in the
project does not define anything twice.

Project settings (Properties > C/C++ Build > Settings > MCU GCC Compiler):

- **Preprocessor:** no `FM_...` symbol is needed; the symbols CubeMX
  generates (`USE_HAL_DRIVER`, `STM32H743xx`, ...) are enough.
  `include/fm_board_config.h` turns the FatFS adapter on in a firmware
  build (`FM_USE_FATFS` 1 when `USE_HAL_DRIVER` is defined; a host build
  keeps the default 0), and the media switches (`FM_USE_AUDIO`,
  `FM_USE_VIDEO`, `FM_USE_IMAGE`, `FM_USE_MP3`, ...) are already 1 on an
  STM32H7.
  Switching features off, buffer sizes and what each one saves:
  [`docs/OPTIONS_FA.md`](docs/OPTIONS_FA.md) (Persian); the switches
  themselves are the commented `//#define FM_USE_... 0` lines at the top
  of `include/fm_board_config.h`.
- **Include paths:** `include`, `backends`, `src`,
  `third_party/littlefs`, `third_party/h264bsd/src`,
  `third_party/minimp3`, `third_party/picojpeg` and
  `third_party/opus/{include,celt,silk,src}`.

`sh tools/check_cubeide.sh` (part of `make check`) enforces the rule: it
compiles every `.c` outside the four firmware folders with the macros of
a CubeMX project and **no include path at all**, and fails if one of
them leaves a symbol behind or does not compile.  A new host-only `.c`
without the guard fails that check.  It also fails when a vendored
source would pull `printf` / `fopen` into the firmware: the littlefs
trace / debug / warn / error messages are compiled only with
`-DLFS_YES_MESSAGES`, and the `FILE *` half of stb_vorbis only with
`-DSTB_VORBIS_YES_STDIO`.

**Three settings of the CubeMX project the library depends on.**  They
are in generated files; `sh tools/check_board.sh` (part of `make check`,
`BOARD=<project folder>`, silent when there is no project next to the
library) reads the first two back:

| Setting | Where | Needed | Why |
|---|---|---|---|
| `_FS_LOCK` | `FATFS/Target/ffconf.h` | 0, or at least 12 (`FM_MAX_OPEN_FILES` 8 + `FM_MAX_OPEN_DIRS` 4); the project has 16 | CubeMX writes 2: a third open object is `FR_TOO_MANY_OPEN_FILES`, so `Directory_Copy` of a folder with one file failed with `FM_ERR_NO_MEMORY`.  The adapter stops the build with an `#error` below 12 (`-DFM_FATFS_ALLOW_SMALL_LOCK=1` to build anyway). **This line is outside the `USER CODE` blocks: CubeMX puts 2 back when it regenerates the project.**  Set the `FS_LOCK` parameter of the FATFS middleware in CubeMX to 16 as well, so that a regeneration writes 16. Alternatively run `python3 tools/restore_fatfs_lock.py` after generation and before building. The repository contains no `.ioc` or CubeIDE project in which to install this as an automatic hook. |
| 1 ms tick priority | `HAL_InitTick(0U)` in `Core/Src/main.c` (`USER CODE BEGIN SysInit`) | numerically lower (= more urgent) than the USB interrupt | With USB mass storage served inside the USB interrupt (`UsbMsc_Storage*` called by the ST device stack), the SD driver waits with `HAL_GetTick()` timeouts in that interrupt.  A tick that cannot interrupt it never counts: a card that stops answering hangs the board for good. |
| `OTG_FS_IRQn` priority | `USB_DEVICE/Target/usbd_conf.c` (`USER CODE BEGIN USB_OTG_FS_MspInit 1`), set to 6 | numerically higher than the tick and than `SDMMC1_IRQn` (1) | same reason: the SD interrupt has to get through while the USB interrupt waits for it. |

The `UsbSd_Poll()` way of serving the PC (from the main loop) never had
the problem of the last two rows.  None of this is tested on hardware.

### CMake (STM32 / ESP32 / desktop)

The repository ships a single `CMakeLists.txt` that works both as a plain
CMake library target and as an ESP-IDF component:

```cmake
# Plain CMake (STM32CubeMX, PlatformIO, desktop):
add_subdirectory(FileManager)
target_link_libraries(app PRIVATE filemanager)

# Optional switches:
#   -DFM_USE_LITTLEFS=OFF -DFM_USE_MP3=OFF -DFM_USE_H264=OFF
#   -DFM_USE_FLAC=OFF -DFM_USE_VORBIS=OFF -DFM_USE_OPUS=OFF
#   -DFM_USE_OPUS_LIB=ON           (real Opus decode, vendored libopus)
#   -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=freertos|pthread|cmsis_rtos2
```

Desktop build with the whole test-suite through CTest:

```bash
cmake -S . -B build -DFM_BUILD_TESTS=ON -DFM_USE_CACHE=ON
cmake --build build && ctest --test-dir build --output-on-failure
```

`FM_BUILD_TESTS=ON` implies `FM_HOST_BUILD=ON` (no HAL) and the POSIX
adapter; with `-DFM_USE_THREADS=ON -DFM_SYNC_IMPL=freertos|cmsis_rtos2` the
suite runs through the real port header on the stand-in kernels of
`tools/rtosstub`.  `make cmake` (`tools/check_cmake.sh`) runs 13 such
configurations and is part of `make check` when cmake is installed.

On a small part (STM32F1/G0/G4/L4) switch the media options off explicitly
(`-DFM_USE_IMAGE=OFF -DFM_USE_AUDIO=OFF -DFM_USE_VIDEO=OFF`): CMake passes
every switch, so the "file-only" defaults `fm_config.h` picks for such a
part by itself do not apply.

Supported port profiles (`FM_PORT`, auto-detected from the CubeMX device
macro): STM32F1, F4, F7, G0, G4, L4, **H5** (Cortex-M33: no core data cache,
no hardware JPEG) and H7, plus `GENERIC` and `HOST`.  Each capability
(`FM_PORT_HAS_*`, `FM_PORT_TIER`) can be overridden by defining it before
`fm_port.h` is included, e.g. in `fm_board_config.h`.

For ESP-IDF, put the folder under `components/` (or add it to
`EXTRA_COMPONENT_DIRS`) — `idf_component_register` is detected
automatically, and `FM_USE_THREADS=ON` defaults to the FreeRTOS port.

Feature switches (defaults shown):

```bash
make FM_USE_LITTLEFS=0     # drop the LittleFS adapter + vendored littlefs
make FM_USE_H264=0         # drop the h264bsd H.264 decoder
make FM_USE_MJPEG=0        # drop the picojpeg MJPEG decoder
make FM_USE_MP3=0          # drop the minimp3 MP3 decoder
make FM_USE_CACHE=0        # drop the file read-cache
make FM_USE_FATFS=1        # enable the FatFS adapter (needs your FatFS headers)
make FM_USE_THREADS=1      # thread-safe core, pthread port (-lpthread)
make FM_USE_THREADS=1 FM_SYNC_IMPL=freertos    # FreeRTOS port (STM32/ESP32)
make FM_USE_THREADS=1 FM_SYNC_IMPL=cmsis_rtos2 # CMSIS-RTOS2 port
```

The host build is warning-clean with the repository's strict C99 warning set and is also validated with `-Werror`.

`make check` is the whole host verification: the suite, the FatFS host
builds, the examples, the port headers, the CMake configurations and a
**32-bit run** (`make m32`, `tools/check_32bit.sh`).  The 32-bit run
builds the suite for a target where `size_t` and pointers are 32 bits
wide, as on the MCU (`make m32 M32=ubsan` does the same under UBSan); it
needs a compiler that can do that: `pip install ziglang` (used as
`python3 -m ziglang cc`), `zig` on the PATH, or a `gcc -m32` with its
32-bit libraries.  Without one it prints `SKIP` and the rest of
`make check` still runs.
It also runs the host tests of the board glue (`make usbmsctest
saveexamplestest audiorecordtest videohooks hwjpegtest tapstest`) and the
`tools/check_*.sh` scripts named in this file.
`make matrix` / `make matrix-asan` build and run every feature and
debug-level combination listed in `tools/check_matrix.sh` (57), each from
scratch; `make tsan` runs the threaded suite under
ThreadSanitizer.

## Audio: WAV + MP3

The audio subsystem plays WAV (8/16-bit PCM) and — with `FM_USE_MP3` — decodes
MP3 streams (vendored [minimp3](https://github.com/lieff/minimp3), CC0).  The
same API serves both; decoded output is always interleaved 16-bit PCM.

```c
FM_Audio a;
FM_AudioInfo info;

Audio_Open("/song.mp3", &a);
Audio_GetInfo(&a, &info);          /* codec "MP3", channels, sample_rate */
int16_t buf[1024];
size_t n = 0;
while (Audio_Read(&a, buf, 1024, &n) == FM_OK) { /* feed DAC */ }
Audio_Close(&a);
```

`Audio_ReadMono()` downmixes stereo sources to a single channel, so mono and
stereo output are available separately.  MP3 input is streamed (no whole-file
buffer). A leading ID3v2 tag is skipped for MP3, AAC, WAV, FLAC,
Vorbis and Opus. Detection reads behind one tag in a file; for an
incomplete tag in a buffer it falls back to the extension or unknown.
`SimpleAudio_Poll` keeps its restart budget through successful refills
of the same track, so a repeatedly failing track eventually stops.
A `SimpleAudio_Stop` requested inside a PCM callback is deferred until
its refill unwinds; during the initial prefill it completes before
`SimpleAudio_Begin` returns. Other callback reentrancy is not guaranteed.

### Audio rules and limits

**Length and position.** `FM_AudioInfo.total_frames` is the length of the
track in sample frames (one frame = one sample per channel) and
`FM_Audio.frame_index` the position; `Player_GetDuration()` and
`Player_GetPosition()` are these two divided by the sample rate.  The
length is exact for WAV (PCM, G.711, IMA and Microsoft ADPCM), FLAC, Vorbis, Opus and
MP3 with a recognized and valid LAME/Lavc/Lavf gapless extension. Its
encoder delay and padding are trimmed; exact seeking in these streams
replays from the beginning, so long seeks can be slow. Other Xing/Info MP3
reports decoded length including delay and padding. For MP3 without a Xing
header length is computed from the
bit rate of the first frame: right for constant bit rate, **wrong for
variable bit rate without a Xing header** (such files play, but length,
position and seek are estimates).  It is 0, "not known", for AAC.

**Reads are whole frames.** `Audio_Read(a, samples, count, &read)` rounds
`count` down to a multiple of the channel count.  A `count` smaller than
one frame is `FM_ERR_INVALID_ARGUMENT`; it is not the end of the track.
`*read` is written on every path, 0 when nothing was delivered.

**Results.** `FM_ERR_EOF` is the end of the audio and nothing else.  A
storage failure is `FM_ERR_IO`, a damaged file `FM_ERR_CORRUPTED`, a
format the library knows and does not decode `FM_ERR_UNSUPPORTED`, a
track too long for its buffer `FM_ERR_TOO_LARGE`.  Damaged stretches
inside an MP3 or AAC stream are skipped to the next frame and playback
goes on.

**Seek.** `Audio_Seek(a, seconds)` refuses a negative time and NaN
(`FM_ERR_INVALID_ARGUMENT`); a time past the end is the end. An MP3
past-end seek returns `FM_ERR_EOF` and leaves subsequent reads at EOF.
It lands on
the exact sample for WAV, FLAC, Vorbis, Opus and constant-bit-rate MP3.
For variable-bit-rate MP3 it goes by the 100-entry table of the Xing
header: the sound starts up to about 0.15 s after the request and
`frame_index` can be some 50 ms off.  AAC does not seek.

**What is decoded.**

| Format | Decoded | Refused |
|---|---|---|
| WAV | PCM 8/16 bit, A-law, mu-law, IMA ADPCM and Microsoft ADPCM (4 bit) mono and stereo | 24-bit, float, more than 2 channels: `FM_ERR_UNSUPPORTED` |
| MP3 | MPEG-1/2/2.5 Layer III; the Xing/Info frame is read, not played | |
| FLAC | up to 8 channels and 24 bits a sample, decoded whole at open, at most `FM_FLAC_MAX_PCM_BYTES` of PCM | longer: `FM_ERR_TOO_LARGE`; 32 bits a sample: `FM_ERR_UNSUPPORTED` |
| Ogg Vorbis | mono / stereo, decoded whole at open | more channels: `FM_ERR_UNSUPPORTED` |
| Ogg Opus | mono / stereo (channel mapping family 0); the output gain of the header is applied | surround (family 1 and up): `FM_ERR_UNSUPPORTED` |
| AAC (ADTS) | through a registered decoder | `.m4a` is an MP4 file: open it with the video API (`Video_ReadAudioFrame`) |

A **chained Ogg file** (several streams one after the other, as internet
radio recordings are) plays its first stream only.  `Audio_ReadMono()`
serves mono and stereo; with more channels it is `FM_ERR_UNSUPPORTED`.

**Stack.** The decoders work on the stack of the task that calls
`Audio_Open` / `Audio_Read` (and `Video_ReadAudioFrame` for the audio
track of a video).  Measured on x86 by an external audit, whole call
chain: MP3 about 19 KB (minimp3 keeps its scratch area there), Opus 24
to 28 KB at open, Vorbis about 8 KB, FLAC about 6 KB, WAV about 4 KB.

For a Cortex-M7 build (clang `-Os`) the compiler reports these as the
largest single stack frames of the library; the functions that call
them add theirs on top, a few hundred bytes each:

| Function | Frame | Reached from |
|---|---|---|
| `mp3dec_decode_frame` (minimp3) | 17.4 KB | `Audio_Read*` on an MP3 file, `Video_ReadAudioFrame` on an MP3 track |
| `drflac_open_with_metadata_private` | 4.9 KB | `Audio_Open` on a FLAC file |
| `flac_encode_block` | 2.7 KB | saving FLAC (`AudioFileWriter_*`, `Audio_Convert`) |
| `jpeg_encode_common` | 2.4 KB | writing a JPEG (`Image_Save` to `.jpg`, `Image_EncodeJPEG*`, every frame of `VideoFileWriter_WriteFrame`) |
| `png_inflate` | 2.2 KB | PNG decode |
| `h264bsdInterpolate*` | 1.9 KB | H.264 decode |
| image layer otherwise | up to 1.4 KB | `Image_StreamDecode`, `Image_GetInfo`, PNG decode |
| file layer | up to 1.2 KB | `FM_Transfer_CopyEx`, `Directory_ScanRecursive`, `Directory_Copy`; the tree walks are loops, their stack does not grow with the depth |

These come from the compiler (`-fstack-size-section`), not from a board:
give a task that plays MP3 or Opus at least 32 KB and measure the
high-water mark on the target.  To repeat the measurement, compile with
`-fstack-size-section` and read the `.stack_sizes` section
(`llvm-readelf --stack-sizes`), or use `-fstack-usage` with GCC.

**Damaged files and the bundled decoders.** `dr_flac` computes its
predictions in signed 32-bit arithmetic; on a damaged FLAC file these can
overflow, which the C standard leaves undefined.  No memory error follows
(the external audit fuzzed it under AddressSanitizer and saw none), the
samples of that block are noise.
Compile `src/fm_flac.c` with `-fwrapv` if your rules require defined
behaviour here.

## Video: AVI / MP4 / H.264 / MJPEG / audio

The video layer parses FMRV, AVI (RIFF), MP4 (ISO BMFF), Matroska/WebM and
FLV containers into a per-frame index and hands frame payloads to a pluggable
codec:

* **H.264** — vendored [h264bsd](https://github.com/oneam/h264bsd)
  (Apache-2.0 + MIT) → RGB888.  **Baseline profile only**: a Main or High
  profile stream (CABAC, B-frames) opens, and every frame is then refused
  with `FM_ERR_INVALID_FORMAT`.  Encode with `-profile:v baseline`.
* **MJPEG** — vendored [picojpeg](https://github.com/richgel999/picojpeg)
  (public domain) → RGB888, for USB cameras / ESP32-CAM.

Codecs auto-register on decode, so playback is one line:

```c
FM_Video v;
uint8_t rgb[W * H * 3];

Video_Open("/movie.mp4", &v);            /* AVI, MP4 or FMRV */
Video_DecodeFrame(&v, rgb, sizeof(rgb)); /* H.264/MJPEG -> RGB888 */
Video_Close(&v);
```

The **audio track** is read with its own calls, independent of the
picture, and comes out as interleaved 16-bit PCM:

```c
FM_AudioTrackInfo ti;
static uint8_t pcm[18432];              /* one FLAC block of stereo PCM;
                                           4608 is enough without FLAC */
size_t size = 0;

Video_GetAudioInfo(&v, &ti);            /* codec, channels, sample_rate */
if (ti.decodable)
{
    while (Video_ReadAudioFrame(&v, pcm, sizeof(pcm), &size) == FM_OK)
    {
        /* size bytes of PCM for the DAC / I2S */
    }
}
```

| Track | In | Decoded by | Notes |
|---|---|---|---|
| PCM 8 / 16 / 24 / 32-bit and 32-bit float, either byte order | AVI, MKV, MP4/MOV, FLV | the video layer | always handed out as 16-bit; 64-bit samples are not a track |
| A-law, mu-law | AVI, MKV, MP4/MOV | the video layer | |
| IMA ADPCM, Microsoft ADPCM | AVI, MKV (`A_MS/ACM`) | the video layer | mono and stereo; the buffer must hold one block (4 x `nBlockAlign` bytes is enough) |
| MP3 | AVI, MP4, MKV, FLV | minimp3 (`FM_USE_MP3`) | buffer of 4608 bytes or more; 16 KB of heap while the file is open; 18 KB of stack |
| FLAC | MP4, MKV | dr_flac (`FM_USE_FLAC`) | up to 24 bits a sample; a 32-bit track has `decodable` false and every read returns `FM_ERR_UNSUPPORTED` |
| Vorbis | MKV / WebM, MP4 | stb_vorbis (`FM_USE_VORBIS`) | heap for the stream's codebooks while the file is open: 150 to 185 KB measured for 22 and 44.1 kHz stereo tracks |
| Opus | MP4, MKV / WebM | libopus (`FM_USE_OPUS_LIB`) | without it `decodable` is false |
| AAC | MP4, MKV, FLV, AVI | a decoder you register under `"AAC"` | without one `decodable` is false and each call returns the raw frame behind a 7-byte ADTS header: a valid `.aac` stream, not PCM |

`decodable` says up front whether PCM will come out.  The results of
`Video_ReadAudioFrame` and what each of them leaves consumed are listed
at its declaration in `fm_video.h`.

**How this is checked.**  Files written by ffmpeg, one per row and
container above, are decoded by the library and by ffmpeg and the PCM is
compared: PCM, G.711, both ADPCM kinds and FLAC are identical byte for
byte; MP3, Vorbis and Opus are within one LSB.

**Encoder delay and padding.**  An MP3, Opus or AAC encoder puts frames
of its own in front of the sound (1105 for LAME, 312 at 48 kHz for
libopus) and rounds the end up to a whole packet.  Where the container
records this, `Video_ReadAudioFrame` removes it: the edit list of an
MP4 / MOV track (start and length), `CodecDelay` and `DiscardPadding` in
Matroska / WebM.  A decoded track then starts with the first sample of
the sound and has its length, as ffmpeg's decode does.  AVI and FLV have
no such record and are played from the first packet (ffmpeg does the
same).  After `Video_SeekAudio()` to a packet other than 0 the MP4 length
limit is not applied, because the number of frames before that packet is
not known.  AAC handed out as ADTS frames is never trimmed.

The test suite keeps small files for every row
(`tools/gen_video_audio_vectors.py`, `test/audit_v3_tests.h`).  Not
recognised: AC-3, DTS and ALAC (the clip opens without an audio track),
PCM with 64-bit samples (the same).

Notes:

* h264bsd allocates its reference pictures with `malloc`/`free`, sized by the
  stream resolution — on MCUs it needs external RAM (SDRAM on STM32 H7, PSRAM
  on ESP32).  The FileManager core itself stays allocation-free.
* MP4/avc1 sample payloads are converted to Annex-B automatically; AVI H.264
  frames are auto-detected (start-code or length-prefixed).
* picojpeg decodes baseline sequential JPEG only and is not thread-safe.
  Chroma layouts: grey (whatever sampling factors the one component
  declares), 4:4:4, 4:2:2, 4:4:0 and 4:2:0 in the usual form (chroma
  sampling factors 1x1), and the form ffmpeg writes for 4:2:2 and 4:4:4
  Motion JPEG (chroma factors 1x2, luma 2x2 or 1x2).  Other layouts
  (4:1:1 with luma 4x1, chroma wider than 1) are refused with
  `FM_ERR_INVALID_FORMAT`.  This holds for JPEG stills too.
* Colour: H.264 is converted with the BT.601 matrix, or BT.709 when the
  stream's VUI names it (`matrix_coefficients` 1); video range
  (16..235) unless the VUI says full range; JPEG is full range.
  Against ffmpeg's decode of the same files the picture is at 48 dB
  (H.264) and 50 dB (MJPEG) PSNR; raw video is identical.
* The container frame/sample index is static (sized by `FM_VIDEO_MAX_FRAMES`
  and `FM_VIDEO_MAX_AUDIO_FRAMES`).  A clip with more frames or audio packets
  than that is **not cut short**: `frame_count` / `sample_count` report the
  real totals, and what lies past the table is located while reading by
  walking the container again (AVI `movi`, Matroska clusters, the MP4 sample
  tables, FLV tags).  Sequential playback costs O(1) per frame; a seek far
  past the table walks forward from the last indexed frame.
* **Static RAM.**  The index tables live in the video slots, which makes
  them the largest static object of the library: `FM_MAX_VIDEO` (4) slots
  of `FM_VIDEO_MAX_FRAMES` (4096) picture and `FM_VIDEO_MAX_AUDIO_FRAMES`
  audio entries are 546 688 bytes of `.bss` on a 64-bit host (about 137 KB
  for each video that can be open at the same time).  On a
  microcontroller set `FM_MAX_VIDEO` to the number you really open (1 for
  a player) and, if needed, the two table sizes: with 1 slot and 1024
  entries it is about 38 KB, and longer clips still play (see the item
  above).
* The parser allocates (`FM_Alloc`, released by `Video_Close`) in two cases
  only: the Matroska lace table of a clip longer than the index, and audio
  setup data (MKV CodecPrivate, MP4 `dfLa`) larger than
  `FM_VIDEO_AUDIO_PRIVATE_MAX` — up to `FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX`
  (64 KiB; 0 = never allocate, such a track is then not decodable).
* **FLV**: H.264 (`AVCPacketType` sequence header parsed, frames returned as
  Annex-B with the parameter sets in front of key frames), Enhanced-FLV
  FourCC tags (`hvc1`, `avc1`, `av01`, `vp09`, `vp08`), AAC (rate/channels
  from the AudioSpecificConfig, ADTS added), MP3, PCM and G.711.  Width,
  height and frame rate come from `onMetaData`; without it the frame rate is
  derived from the tag timestamps and the size is a 320x240 placeholder until
  the decoder reports the real one.  A recording cut inside its last tag
  plays up to that tag.  Not playable: Flash ADPCM, Nellymoser and Speex
  audio (the clip opens without an audio track); VP6 / Sorenson frames are
  only handed to a decoder you register.
* **MP4**: 64-bit box sizes, `co64`, `stsc` runs; audio `mp4a`, `.mp3`,
  `fLaC`, `Opus`, Vorbis, 16-bit PCM little-endian (`sowt`) and big-endian
  (`twos`, byte-swapped while reading), `alaw`/`ulaw`.  For `mp4a` the
  `esds` box decides what the track is: AAC object type, sampling rate and
  channels come from the AudioSpecificConfig (the sample entry cannot hold
  96 kHz and says nothing about the type); an MPEG-1/2 audio track
  (MP3 in `mp4a`) is reported as MP3, object type 0xDD as Vorbis, and any
  other object type (AC-3, DTS, ...) is not offered as an audio track.
  Neither is ALAC.  The same for the picture: an `mp4v` entry of object
  type 0x6C is Motion JPEG (how ffmpeg writes MJPEG into an `.mp4`).
* **AAC without a registered decoder** is passed through with an ADTS
  header per packet.  The header carries the real object type (it used to
  announce AAC Main for everything); HE-AAC is signalled as its AAC-LC
  core, at the core sampling rate, which is how ADTS carries it.

### Saving: what is on the card when it goes wrong

`AudioFileWriter_*` and `VideoFileWriter_*` (and the `SimpleAudio_Rec*` /
`SimpleVideo_Rec*` calls on top of them):

* **Never closed** (reset, power loss).  Every `FM_AUDIO_ENC_SYNC_BYTES`
  (256 KB) / `FM_VIDEO_ENC_SYNC_BYTES` (1 MB) the lengths in the header
  are brought up to date and the file is synced.  A WAV / ADPCM file
  plays up to the last such point; a FLAC file is on the card up to
  there, with "length unknown" in its header; an AVI is a valid file
  without index up to there, and this library opens it.  For the AVI,
  `"<name>.idx"` stays beside it: `VideoFile_Repair(path, &frames)` after
  the restart writes the index and the exact lengths and removes it.
  Set either option to 0 to switch the checkpoints off.
  The distance is in bytes, so in time it depends on the format: 256 KB
  is about 1.5 s of 44.1 kHz stereo WAV, 6 s of 44.1 kHz stereo ADPCM and
  **64 s of 8 kHz mono ADPCM**.  A recording shorter than that has 0 in
  its header lengths after a power loss: on FAT the file size is still 0
  (nothing to play); where the bytes did reach the medium this library
  and libav play them to the end of the file, a reader that trusts the
  header shows an empty track.  Count on nothing before the first
  checkpoint; for low rates make `FM_AUDIO_ENC_SYNC_BYTES` smaller.
  A FLAC file that was cut ends inside a frame: this library plays the
  whole frames and reports the end, libav plays the same frames and then
  reports "invalid data".
  (A WAV whose `data` length is 0 with sound behind it -- from another
  recorder, or a stream -- is played to the end of the file.)
* **Write error / card full.**  The first error stays the answer of every
  later call and of `Close`.  `Close` still completes the file with what
  reached the card: the header lengths equal what is really there (a
  partial sample frame or ADPCM block is cut off; the AVI gets its index
  when there is room for it, otherwise it is left valid without one).
* **Open on a writer that still has a file open** is `FM_ERR_BUSY`; the
  first file is not touched.
* `SimpleAudio_RecPlaying("/folder")` never overwrites a file it did not
  write in the same session: a second track of the same name becomes
  `name_2.wav`, `name_3.wav`, ...

These are host tests against a file system that fails on purpose
(`test/audio_save_tests.h`, `test/video_save_tests.h`): they check every
header field of the files byte by byte.  No external player and no real
card were part of it.

## Images: PNG and the hardware JPEG path

**PNG read** accepts every legal PNG: greyscale 1/2/4/8/16 bit, palette
1/2/4/8 bit, RGB and RGBA 8/16 bit, grey+alpha 8/16 bit, non-interlaced and
Adam7.  The CRC of every `IDAT` chunk and the Adler-32 of the pixel data are
checked — a damaged file gives `FM_ERR_CRC`, never a silently wrong picture.
The result is always RGB888: alpha is dropped and 16-bit samples are rounded
to 8.  Decoding is streamed row by row (32 KiB window); a row longer than
`FM_PNG_MAX_STRIDE` bytes returns `FM_ERR_TOO_LARGE`.  `Image_Save` writes
8-bit RGB PNG (stored blocks).

**JPEG through a hardware codec.**  A codec registered with
`Image_RegisterCodec("JPEG", ...)` receives the compressed picture as one
memory block, so `Image_Load` must hold the whole file for it.  Where that
memory comes from is bounded by configuration:

| Setting | Default | Meaning |
|---|---|---|
| `Image_SetWorkBuffer(buf, size)` | none | used first when the file fits (SDRAM/PSRAM) |
| `FM_JPEG_HW_HEAP` | 1 | 0 = never take a file-sized block from `FM_Alloc` |
| `FM_JPEG_HW_FILE_MAX` | 16 MiB | larger files never go to the hardware; 0 = files never do |

Whenever the file is not handed to the hardware — too large, no memory, or
the codec declines it — the picture is decoded by the bundled streaming
software decoder, which needs no file-sized memory.  With
`-DFM_JPEG_HW_HEAP=0` and a work buffer, the library therefore never
allocates for a JPEG file.

**BMP read**: 24 bpp gives RGB888.  8 bpp goes through the colour table
of the file: a grey ramp (or no table) gives GRAY8, any other table
RGB888 with the colours of the table (a table with more than 256 entries
is `FM_ERR_CORRUPTED`).  `Image_GetInfo()` and the `Image_Probe*` calls
report the layout the decoder will deliver, not the one the file stores,
so a buffer of `info.data_size` bytes always holds the decoded picture.
`FM_ImageDecoder` carries the colour table (768 bytes): it is larger than
it was, mind it when the object lives on a small stack.

## Rename-over and atomic replace

`File_Move`, `File_SafeReplace`/`File_AtomicWrite`, `Json_SaveFile`,
`Text_Replace`, `File_Insert`/`File_DeleteRange`/`File_ReplaceRange` and
the logger all rely on **rename-over** semantics: renaming
onto an existing file replaces it.  This is provided uniformly by every
adapter — POSIX (native), FMFS, LittleFS and FatFS; a non-empty
directory destination is refused with `FM_ERR_NOT_EMPTY`.  The FatFS
adapter moves a destination file aside using a free `~fmXX.tmp` name in
the same folder, renames, and then removes the aside. Existing names are
never deleted to make room. The sequence is not atomic: power loss or a
failed rollback can leave the old file under the aside name; filesystem
and device flush semantics also determine durability. Because FAT names ignore case, `a.txt` -> `A.TXT` (or a long name ->
its 8.3 alias) is recognised as the same entry and simply gets the new
name.  A same-path rename is a no-op, and adapters that
do not implement rename at all are handled safely: `File_Move` /
`Directory_Move` fall back to copy + delete and `File_Rename` /
`Directory_Rename` return `FM_ERR_UNSUPPORTED`.

### File rules worth knowing

* **Appending.** `File_Append()` and a handle opened with `FM_OPEN_APPEND`
  write at the end the file has *at that moment*, as one step with the
  write: two handles appending to one file do not overwrite each other.
* **LittleFS: one writer or many readers.** littlefs keeps a private copy
  of a file's state in each handle, so a second handle on a file that is
  open for writing would lose data without any error.  The adapter
  refuses the open that would create that situation with `FM_ERR_BUSY`.
  Any number of read-only handles is fine.  FatFS, FMFS and POSIX share
  the state and allow it.
* **Truncating** needs a handle that may write (`FM_ERR_ACCESS`
  otherwise) and leaves the position where it was, on every backend.
* **A failed replace keeps the backup.** When `File_Transaction` cannot
  put the original back after a failure (volume full, card gone), the
  backup stays as `<path>.fm_tmp.bak`: it is the last copy of the data.
* **Walking a tree** (`Directory_List`, `_ScanRecursive`, `_Size`,
  `_Copy`, `_DeleteRecursive`): up to `FM_DIR_MAX_DEPTH` (16) levels,
  deeper is `FM_ERR_TOO_LARGE`; stack use does not grow with the depth;
  the callback must not add or remove entries in the tree it is shown.
* **Long names.** An entry name that does not fit `FM_NAME_MAX` (64)
  makes the listing of its directory fail with `FM_ERR_TOO_LARGE`.  For
  cards written on a PC set `FM_NAME_MAX` to 256.  The library does not
  make such a name itself: creating, renaming, copying or moving to a
  name of `FM_NAME_MAX` characters or more is `FM_ERR_TOO_LARGE` on
  every file system.
* **`Directory_Rename`** renames a directory, nothing else: a file is
  `FM_ERR_NOT_DIRECTORY`, a mount point or a destination inside the
  directory itself `FM_ERR_INVALID_ARGUMENT`, a directory with an open
  file on or below it `FM_ERR_BUSY`.  `Directory_Move` on one volume
  follows the same rules.
* **`Text_Replace` / `Text_InsertLine` / `Text_DeleteLine`** work
  through `<path>.fm_tmp`; a path longer than `FM_PATH_MAX` - 8
  characters is `FM_ERR_TOO_LARGE` and the file is not touched.
* **FatFS.**  `File_GetInfo` / `Directory_Exists` of the mount point
  itself work (a directory).  `FM_OPEN_WRITE | FM_OPEN_TRUNCATE` without
  `FM_OPEN_CREATE` does not create a missing file (`FM_ERR_NOT_FOUND`),
  as on the other file systems.
* **`Path_Join`** may write its result over either argument.
* **NaN and infinity.**  `JsonWriter_Float` writes `null` for them (JSON
  has no such numbers) and clamps beyond +-9e9; `Logger_Write` writes
  `nan` / `inf` / `-inf` in CSV and `null` in JSON lines.  A JSON array
  index too large for `size_t` names no element (`FM_ERR_NOT_FOUND`).
* **Explorer.** A listing holds at most `FM_MAX_EXPLORER_ITEMS` entries;
  `Explorer_IsTruncated()` says when the directory has more.
* **`Text_ReadLine`** removes the `\r` of a `\r\n` line end; a `\r`
  anywhere else is part of the line.

Note: buffer-taking APIs such as `Path_GetFilename`, `Path_GetDirectory`
and `Json_ReadString` report `FM_ERR_TOO_LARGE` when the output buffer is
too small (the output is still NUL-terminated) — truncation is never
silently accepted.

## File read-cache

`FM_USE_CACHE` (on by default in the host build) enables a block read-cache in
the file layer.  It speeds up small sequential reads and repeated scans by
fetching whole blocks and satisfying later reads from RAM.

```c
File_SetCacheEnabled(FM_FALSE);         /* runtime off */
File_SetCacheEnabled(FM_TRUE);          /* runtime on  */
uint32_t hits, misses;
File_CacheGetStats(&hits, &misses);     /* measure effectiveness */
```

Block size and count are tuned with `FM_CACHE_LINE_SIZE` / `FM_CACHE_LINES`.
The cache is read-only: writes, truncates and closes invalidate affected
blocks. Reads of at least two cache lines bypass this small-read cache and go
straight to the filesystem backend, preserving FatFS multi-sector transfers
for real-time audio/video streams.

## Debug levels and log output

The library prints through one function pointer, the *log sink*
(`fm_log.h`).  It never touches a UART itself, and until a sink is
registered nothing is formatted at all.

```c
#include "fm_log.h"
#include "fm_log_uart_hal.h"
FM_Log_UseHalUart(&huart1);          /* or FM_Log_SetSink(my_sink, ctx) */
```

How much it prints is a compile-time choice, `FM_DEBUG_LEVEL` in
`fm_board_config.h` (or `-DFM_DEBUG_LEVEL=n`).  There are four values:

| Level | Prints | Flash (Cortex-M7, clang `-Os`, all features, object files) |
|---|---|---|
| `0` OFF | nothing; the messages are not in the firmware | 0 |
| `1` LOW (default) | operations that really failed: bad argument, mount/format failure, a close that could not flush, storage-driver errors, a media file that could not be opened or parsed, a failed allocation with its size.  Nothing on a successful path. | about 7 KB |
| `2` MEDIUM | + one line per life-cycle step: init, mount/unmount/format, file open/close/delete, image loaded, audio/video opened, track change, media-cache decisions | about 9 KB |
| `3` HIGH | + every allocation and release, every QSPI read/write/erase, raw-copy chunks | about 10 KB |

```
[E/fs] mount /sd failed (-6)
[I/file] opened /sd/a.wav (mode 0x1)
[T/alloc] alloc 4096 -> 0x24001000
```

What to know before choosing a level:

* **"Not found" is not an error line.**  Opening or deleting a file that
  is not there returns `FM_ERR_NOT_FOUND` and prints nothing at LOW (an
  event line at MEDIUM).
* **A message costs what the sink costs.**  It is one `vsnprintf` into a
  `FM_LOG_BUF_SIZE` (512 byte) buffer and one sink call.  The ready-made
  UART bridge blocks until the bytes are out: about 87 µs per character
  at 115200 baud, so 4 ms for a 45-character line, and up to
  `FM_LOG_UART_TIMEOUT_MS` (100 ms) per message when nothing drains the
  UART.  The message is sent from wherever it was produced, which can be
  inside a filesystem operation with its lock held.  LOW prints only when
  something failed, so this does not matter there; **HIGH prints a line
  per storage read and will break audio/video timing** -- use it to find
  a fault, with a sink that only copies into a ring buffer if timing
  matters.
* **A sink must not call back into the library** when `FM_USE_THREADS=1`
  (the locks are not recursive).  Without threads a sink that logs, or an
  interrupt that logs in the middle of another message, has its message
  dropped and counted (`FM_Log_Dropped()`) instead of recursing or
  overwriting the buffer.
* **RAM.**  The format buffer is static without threads; with
  `FM_USE_THREADS=1` it is on the stack of the task that logs (add
  `FM_LOG_BUF_SIZE` plus what your `vsnprintf` needs to every task that
  uses the library, or lower `FM_LOG_BUF_SIZE`; 64 is the minimum).
* **No float, no `%llu`.**  The library's messages use only string and
  int/long conversions, so newlib-nano without float printf is enough.
* `FM_LOG_DISABLE` removes the output functions themselves and forces
  the level to OFF.  `FileManager_GetDebugLevel()` returns the level the
  library was really built with (useful when it is a separate static
  library).
* **The test suite is noisy at LOW, on purpose.**  One run of `fm_test`
  prints about 8300 level-1 lines: all of them come from tests that feed
  damaged files, failing storage or failing allocations.  It is not what
  an application sees; a file that cannot be played costs a line or two.
* Not logged at any level: the inside of the decoders (per frame or
  packet), directory walks, the JSON/text helpers.  They report through
  result codes only.

## Memory allocation hooks

The file and storage core stays allocation-free.  These parts need dynamic
memory: the H.264 decoder and the video demuxer (`fm_video.c`), the image
helpers of `fm_image.c`, the optional JPEG file buffer (`FM_JPEG_HW_HEAP`),
the media-player playlist, IMA ADPCM (one block), and the
FLAC, Vorbis and Opus decoders, which hold the compressed file, the decoder
state and the whole decoded track (FLAC: at most `FM_FLAC_MAX_PCM_BYTES`,
Opus: `FM_OPUS_MAX_FILE_BYTES` / `FM_OPUS_MAX_PCM_BYTES`).  WAV PCM, G.711
and MP3 play without any.  All of it goes through `fm_alloc.h` -- the
state of the bundled dr_flac, stb_vorbis and libopus included, nothing
calls the C library's `malloc` behind the allocator's back -- so you can
redirect it to external RAM in one call:

```c
FM_Allocator ext = { sram_alloc, sram_calloc, sram_realloc, sram_free, ctx };
FM_Allocator_Set(&ext);   /* SDRAM on STM32 H7 / PSRAM on ESP32 */
```

## Hardware decoders (optional)

Register hardware/accelerated codecs and the video layer prefers them over the
bundled software decoders:

```c
HWDecoder_Register(FM_VIDEO_CODEC_MJPEG, &my_hw_jpeg_ops); /* STM32 JPEG */
HWDecoder_Register(FM_VIDEO_CODEC_H264,  &my_hw_h264_ops);
```

For audio, register a decoder for a codec tag — this is how AAC (and any
other codec) gets decoded; see below.

## Audio: WAV + MP3 + AAC hook

WAV and MP3 decode out of the box.  AAC (ADTS) files and MP4 audio are
*framed* by the library (with ADTS headers) but **decoded through a
registered decoder**, because freely-redistributable AAC decoders carry
licensing problems (FAAD2 is GPL, FDK has a restrictive license).  A hardware
AAC block or an OS codec API plugs in with one call:

```c
Audio_RegisterDecoder("AAC", &my_aac_ops);
Audio_Open("/song.aac", &a);   /* now plays */
```

The decoder's `decode()` is given one whole frame **including its 7-byte
ADTS header** and room for `out_count` samples.  It reports how many it
wrote in `*out_samples`; a count above `out_count` is treated as a broken
decoder and ends the read with `FM_ERR_INTERNAL`.  Every context `init()`
creates is given back to `close()`, also when the open fails afterwards.

## Resize (images and video)

`Image_Scale()` resizes any pixel buffer (gray/RGB565/RGB888/RGBA8888) with
nearest or bilinear interpolation; `Image_ResizeFile()` resizes a file on
storage, and `Video_DecodeFrameScaled()` decodes and scales video in one step:

```c
uint8_t small[80 * 45 * 3];
Video_DecodeFrameScaled(&v, small, sizeof(small), 80, 45, FM_SCALE_BILINEAR);
```

Bilinear maps pixel centres onto pixel centres and mixes the four source
pixels around each destination pixel (the edge pixel is repeated).  It
does not average the pixels in between: when a picture is made smaller
by more than a factor of 2, source pixels are skipped, as with nearest
(a resizer that filters first, like Pillow's, gives a smoother result
there).

## Media player

A convenience layer over audio/video: scan a folder, step through tracks and
seek within them.

```c
FM_Player p;
Player_Init(&p);
Player_ScanFolder(&p, "/music", FM_PLAYER_KIND_AUDIO); /* sorted playlist */
Player_Play(&p, 0);          /* first track               */
Player_Next(&p);             /* next track (wraps)        */
Player_SeekFwd(&p, 10.0);    /* +10 s                     */
Player_SeekBack(&p, 5.0);    /* -5 s                      */
Player_Deinit(&p);

/* record audio into a folder with an auto-generated name */
FM_AudioWriter w; char path[FM_PATH_MAX];
AudioRecorder_Open("/recordings", 16000, 1, 16, &w, path, sizeof(path));
AudioWriter_Write(&w, samples, n, NULL);
AudioWriter_Close(&w);
```

## Thread safety

With `FM_USE_THREADS=1` several tasks may use the library at the same
time.  Pick a sync port with `FM_SYNC_CUSTOM` + its include path:

| Port | Path | Target |
|------|------|--------|
| FreeRTOS | `ports/freertos` | STM32, ESP32/ESP-IDF |
| CMSIS-RTOS2 | `ports/cmsis_rtos2` | STM32Cube, Cortex-M |
| pthread | `ports/pthread` | host testing, desktop tools |

A port supplies a plain mutex and (all three shipped ports do) the
identity of the calling task; a custom port is one header,
`fm_sync_impl.h` — see `include/fm_sync.h` for the contract.

**Rules for the application**

- Call `FileManager_Init()` once, **before** the tasks that use the
  library start: it creates every lock.  Call `FileManager_Deinit()`
  only after those tasks have stopped.
- One open handle (`FM_File`, `FM_Dir`, `FM_Stream`, `FM_Audio`,
  `FM_Video`) belongs to one task at a time.  Different tasks may work
  on different handles — also of the same file, on the same volume —
  freely.
- Install `FM_Allocator`, log sink, codec registrations and idle/busy
  hooks before the tasks start (or from one task while the others are
  not inside the library).  A custom `FM_Allocator` must be thread-safe
  itself.

**What the library guarantees**

- Every filesystem adapter and storage driver is entered by one task at
  a time **per mounted volume**.  FatFS, LittleFS, FMFS and your own
  storage callbacks need no locking of their own; two volumes still run
  in parallel.
- Mount table, storage registry and all handle pools are consistent.
  `FS_Unmount()` and `FS_Format()` return `FM_ERR_BUSY` while a file or
  directory on the volume is open or another task is in the middle of
  an operation on it — a volume can no longer disappear underneath a
  running call.
- The shared scratch resources — copy buffer (`File_Copy`,
  `Directory_Copy`, `File_Insert`…), transfer buffer (`FM_Transfer_*`),
  image codec state (PNG/JPEG/MJPEG) and the H.264 decoder — are used by
  one task at a time.  **Another task waits its turn; a nested call from
  the task that already holds the resource** (from a progress callback
  or the idle hook) **gets `FM_ERR_BUSY`** instead of dead-locking or
  corrupting the operation in flight.  With a sync port that cannot
  name the calling task the library cannot tell the two cases apart and
  never waits: every second user gets `FM_ERR_BUSY` and retries.
- `File_GetLastError()` is per task (for the `FM_LAST_ERROR_THREADS`
  most recently active tasks), so the reason for a `File_Open()` that
  returned `NULL` is not overwritten by another task's success.
- User callbacks (progress, idle, storage events, log sink) are never
  called with an internal table lock held, so they may call back into
  the library.
- No thread-local storage is used (on bare-metal toolchains `__thread`
  is one copy per program, not per RTOS task).

Cost in RAM: one mutex per mount slot plus eleven global ones; a log
message is formatted on the calling task's stack (`FM_LOG_BUF_SIZE`
bytes) instead of in a shared static buffer.

**How it is verified.**  `make tsan` runs the whole suite with four
tasks hammering three mounted filesystems (POSIX, FMFS, LittleFS),
transfers, the registries and every media decoder under ThreadSanitizer
— zero reports is the pass criterion.  The same suite also runs through
the FreeRTOS and CMSIS-RTOS2 port headers (`make tsan
FM_SYNC_IMPL=freertos`, kernel calls provided by `tools/rtosstub`), and
with every read/write forced through the DMA bounce buffer
(`-DFM_FILE_FORCE_BOUNCE`), which host builds otherwise bypass.  What a
host run cannot show is real interrupt/priority behaviour on the target.

### FMFS notes

- **Power-fail safe (layout version 2).**  A format writes two copies of
  the metadata table and a small move journal.  A change always goes to
  the copy that is not in use, with a sequence number and a CRC; a
  compaction that has to slide a file over itself records its progress
  in the journal, and the next mount finishes it.  After a reset at any
  moment — also in the middle of a compaction, and again in the middle
  of the recovery — every file is either what it was before the
  operation that was running or what that operation leaves:
  create, delete, rename, truncate, append, and every move of data the
  filesystem makes on its own are all-or-nothing.
  `FMFS_IsPowerSafe(&fs)` tells whether a mounted volume has this layout.
- **What it does not cover.**  `File_Write` changes bytes a file already
  has in place; a reset can leave those half written.  On a block
  device that includes the rest of the block being written — for an
  append, the last block of the file as it was.  Other files are never
  affected.  Nothing is checksummed except the metadata: FMFS does not
  detect data that the medium itself corrupts.  Use LittleFS where you
  need copy-on-write data or wear levelling.
- **Blocks.**  Drivers for SD cards and NOR flash rewrite whole blocks,
  and a reset can cost the whole block that was being rewritten.  The
  tables, the journal records and — on anything but RAM — every file
  therefore have blocks to themselves (block = the larger of
  `block_size` and `erase_size`, up to 4096 bytes; larger erase units
  are not covered and are treated as byte storage).  The price is half
  a block per file on average, and `FS_GetFreeSpace()` counts it.
- **Cost.**  The metadata takes about 11.5 KiB instead of 5.7 KiB
  (13 KiB with 512-byte blocks, 24 KiB with 4 KiB erase units).  A
  storage on which that is more than half the capacity is formatted
  with the old single-table layout (version 1), which is not safe
  against a reset; `FM_FMFS_POWER_SAFE` (0 / 1 / 2) moves that limit.
- **Existing volumes.**  Version 1 volumes written by earlier releases
  mount and work as before in every build, and are never converted
  behind your back — `FS_Format()` is the way to the new layout.  On
  RAM/SDRAM storage a volume whose metadata cannot be read (both tables
  on version 2) is formatted at mount; on any other storage the mount
  fails with `FM_ERR_CORRUPTED` and nothing is erased.
- **A table that makes no sense is refused.**  A table can pass its
  checksum and still be nonsense: a file that starts in the metadata
  area, two files on the same bytes; on version 2 also a file behind the
  end of the used area, one name twice, an entry whose folder does not
  exist, a second root.  `FS_Mount` and `FS_Check` answer
  `FM_ERR_CORRUPTED` for these (on RAM / SDRAM the volume is formatted
  at mount, as for any unreadable table).  Before, the first write went
  wherever the table pointed.  One case is repaired instead: a version 1
  volume where a reset fell between saving the entries and saving the
  header mounts, and the end mark is moved behind the last file.
- **Space is reclaimed automatically.**  A file that grows takes the
  space behind it, else moves into the largest hole deleted files left,
  and only then is the data area compacted (and, if needed, the free
  space brought to where the file is).  `FMFS_Repack()` compacts on
  request; open handles stay valid across it.
- **Journal writes are bounded.**  Sliding a file by a distance much
  smaller than the file takes many steps, each a journal record — on
  flash, an erase cycle of the journal's block.  A move may cost at most
  `FM_FMFS_MOVE_STEPS` records (default 64); a hole that would take more
  to close stays where it is until something fits into it.  On a nearly
  full version 2 volume a write can therefore fail with
  `FM_ERR_NO_SPACE` while a little free space is still reported.
  `-DFM_FMFS_MOVE_STEPS=0xFFFFFFFF` restores "fails only when the live
  data really does not fit" (the behaviour of version 1 volumes) at the
  price of unbounded journal traffic — fine on RAM.
- Metadata is rewritten only when it changes: reading a file, closing a
  handle, and overwriting bytes in place do not touch the table.  There
  is no wear levelling: the two tables and the two journal records are
  at fixed places.
- A handle whose file was deleted or replaced by a rename returns
  `FM_ERR_NOT_FOUND`; it can never reach the file that reuses the slot.
- A rename is all or nothing on the mounted volume: every reason to
  refuse it is checked before the table changes, and when the table
  cannot be written the old names — and a file that was to be replaced —
  are put back.
- After an I/O error in the middle of a compaction the volume stays
  usable: the interrupted move is finished at the start of the next
  operation that changes anything (or by the next mount).
- `FMFS_Init()` / `LittleFS_Init()` / `FatFS_Init()` take one of
  `FM_MAX_FS` contexts.  `FS_Unmount()` and a failed `FS_Mount()` give it
  back, so a medium can be mounted and unmounted any number of times.
  The same `FM_FS` can be mounted again without a new init call (card
  out, card in); `FS_Mount()` answers `FM_ERR_NOT_READY` instead when
  its old context now belongs to another `FM_FS` -- with FatFS, where
  the context carries the drive number, also when it has belonged to
  one since -- and the init function then has to be called again.
  Calling it before every mount is always right.  Several FatFS drives
  (`FatFS_Init(&sd, 0)`, `FatFS_Init(&usb, 1)`) may be initialised in
  any order before the first of them is mounted.

**How the power-fail safety is verified.**  On the host, not on a
board: the test suite replays several hundred random operations on
small, nearly full volumes and cuts the power at each storage write of
each of them (about 11 000 cuts, a second cut during recovery where the
mount had work to do), then mounts and compares every file with a
model.  Three devices are simulated: byte storage where a cut write
stops anywhere and leaves a few bytes of noise, and 512-byte and
4096-byte block devices that additionally lose the whole block they
were rewriting.  The same harness run against a version 1 volume does
find damage, and seeded faults in the implementation (a journal record
skipped, a table saved in the wrong order, files sharing a block, …)
are each caught by it.  What this cannot show is a device that
misbehaves in other ways — one that reorders or drops writes it has
acknowledged, for instance.  The storage `sync` callback is called at
the points where the order matters; a driver that caches must honour
it.

## Examples

The host-runnable examples use a caller-owned RAM region with FMFS:

- `examples/setup.c` — initialize FileManager and mount RAM/FMFS
- `examples/file_basics.c` — create, read, overwrite and delete files
- `examples/file_copy_move.c` — copy progress, move and rename
- `examples/advanced_handle.c` — handles, seek, tell, truncate and EOF
- `examples/directory.c` — listing, recursive scan, count, size and delete
- `examples/json_example.c` — JSON writer, typed reads, arrays and embedded-friendly buffers
- `examples/text_lines.c` — line insertion/deletion, replace and search
- `examples/safe_write.c` — atomic replace, safe append, transaction and recovery
- `examples/data_logger.c` — CSV data logging and rotation
- `examples/crc_example.c` — CRC16, CRC32 and checksums
- `examples/image_bmp.c` — BMP load/save, scaling and pixel conversion

STM32H7/SD examples are target-oriented and are not part of `make examples`:

- `top_api/sdcard_fatfs.c` — use the FatFS/diskio generated by CubeMX (`examples/` holds an identical copy that stays empty unless `FM_EXAMPLES_DIR_BUILD` is defined)
- `top_api/sdcard_fm_diskio.c` — use FileManager's polling diskio bridge (same)
- `examples/audio_dma_player.h` — bare-metal circular-DMA audio bridge for Player; hardware access is supplied through port callbacks
- `examples/media_data_io.h` — content-detected audio input, extension-selected audio output, caller-owned audio/video/image buffers and common RGB/BGR layouts
- `examples/qspi_w25q.h` — QUADSPI flash port with JEDEC auto-detect (W25Q64/128/256, MX25, IS25...), engine-driven data phase (`fm_xdma.h`), NOR sector translation, FMFS by default (FatFS opt-in)
- `examples/usb_msc.h` — STM32Cube USB Device MSC callbacks, exclusive MCU/PC ownership and FAT RAM-disk example
- `examples/examples_qspi.h` — the single QSPIExamplesBegin/Format/End + full File examples 01-10 on `/qspi` (reuses an externally mounted `/qspi`)
- `examples/examples_qspi_media.h` — content-detected Audio/Video and RGB/BGR Image processing on QSPI
- `examples/examples_qspi_player.h` — music/video playlist, seek and remote controls on QSPI
- `examples/qspi_transfers.h` — verified data transfers between SD ↔ QSPI ↔ SDRAM ↔ RAM (uses the mount from `examples_qspi.h`)
- `examples/qspi_driver.h` — generic QUADSPI driver with automatic JEDEC chip detection, memory-mapped reads and engine-driven data phase (`fm_xdma.h`)
- `examples/play_from_flash.h` — play music/video directly from QSPI flash through the media cache
- `examples/i2c_eeprom_dma.h` — I2C EEPROM bridge with DMA-driven data phase (ACK-polling write cycle) wired to `FM_Eeprom`; feeds eeprom_simple.h
- `examples/spi_flash_dma_port.h` — serial flash on plain SPI for any STM32 family: DMA-capable transfer callback, FMFS mount on `/flash`, file/CRC/transfer/stats test suite (`SpiFlashDemo_Smoke..5`)
- `examples/usb_sd_presenter.h` — معماری «فقط SD به PC» با سوییچ هوشمند مالکیت (ShowToPc/TakeBack) و فایل دوطرفهٔ FAT
- `examples/usb_present.h` — present SD / QSPI / SPI-flash to the PC as a USB MSC drive (single-LUN switch, owner-switch helpers, full CubeMX checklist)
- `examples/fm_xdma.h` — the one portable DMA engine behind qspi_w25q.h / qspi_driver.h: auto-detected per family (H7 MDMA, H5 GPDMA/OCTOSPI), permanent polling fallback, cache maintenance in one place
- `examples/examples_sdcard.h` — file examples 01-10 on a mounted SD card
- `examples/examples_sdcard_media.h` — WAV/FMRV, media detection and verified media copies
- `examples/examples_sdcard_player.h` — playlist, seek, next/previous and audio/video player controls
- `examples/examples_save_media.h` — saving sound and video to files: plain (your own samples / frames) and through the player callbacks (what is playing); run on the host by `make saveexamplestest`
- `examples/simple_audio.h` — WAV/MP3 playlist + I2S DMA
- `examples/simple_image.h` — photo album + TFT present callback
- `examples/simple_video.h` — folder playlist + TFT present callback + optional I2S
- `examples/hw_jpeg.h` — STM32 JPEG peripheral for Image_Load / SimpleImage
- `examples/spi_tft_dma.h` — H7 SPI master DMA bounce (no ILI9341 driver)
- `examples/README_sdcard_stm32h7.md` — CubeMX setup and STM32H7 SD/DMA/cache troubleshooting

The FatFS adapter and the target example suites are compiled and executed
against the host FatFS mock (three FatFS header generations, one and two
drives, with AddressSanitizer and UndefinedBehaviorSanitizer).  This is
part of `make check`:

```bash
make fatfs          # = sh test/build_fatfs_host.sh
```

## License

MIT
