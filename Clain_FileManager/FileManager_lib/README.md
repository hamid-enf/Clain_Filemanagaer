# FileManager

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
| Atomic replace and flush-backed safe append | ✅ |
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
| Extension-selected WAV/MP3 writer with compressed-encoder registry | ✅ |
| FMRV raw video read/write and codec plugin hook | ✅ |
| AVI (RIFF) and MP4 (ISO BMFF) container parsing | ✅ |
| H.264 software decoding via vendored h264bsd (self-healing: keyframe replay after a broken reference chain) | ✅ |
| MJPEG decoding via vendored picojpeg (USB/ESP32 cameras) | ✅ |
| Audio-track demux from MP4 (AAC/ADTS) and AVI (PCM) | ✅ |
| Media player: folder scan, playlist, next/prev, seek | ✅ |
| Audio recorder (WAV into a folder, auto-named) | ✅ |
| EEPROM page-aware read/write/update/erase adapter | ✅ |
| USB MSC block bridge for TinyUSB/STM32 USB ports | ✅ |
| Thread-safe core (FreeRTOS / CMSIS-RTOS2 / pthread ports) | ✅ |
| Dynamic allocation in the core | None |

### Exact media format contract

File/container formats and decoded raw-buffer layouts are separate concepts:

| Operation | Built in | Optional/plugin behavior |
|---|---|---|
| Audio read | WAV/PCM and MP3 are detected from file bytes and decoded to PCM16 | AAC/ADTS decoder registry |
| Audio write | `.wav` is encoded internally | `.mp3` is selected automatically but requires a registered software/hardware MP3 encoder; otherwise returns `FM_ERR_UNSUPPORTED` |
| Image file read/write | BMP, JPEG, PNG (magic-byte detect; Save picks the encoder from the destination extension) | STM32 JPEG peripheral via `Image_RegisterCodec` |
| Image raw output | GRAY8, RGB565, BGR565, RGB888, BGR888, RGBA8888, BGRA8888 | — |
| Video read | FMRV, AVI and MP4 container detection; RAW, bundled MJPEG and H.264 decode where supported | Hardware codec registry |
| Video raw output | Same caller-selected pixel layouts as images | Conversion uses a caller-owned RGB888 work buffer |
| Video write | FMRV raw RGB888 | AVI/MJPEG or MP4/H.264 require real encoder/container plugins and are not mislabeled |

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

### CMake (STM32 / ESP32 / desktop)

The repository ships a single `CMakeLists.txt` that works both as a plain
CMake library target and as an ESP-IDF component:

```cmake
# Plain CMake (STM32CubeMX, PlatformIO, desktop):
add_subdirectory(FileManager)
target_link_libraries(app PRIVATE filemanager)

# Optional switches:
#   -DFM_USE_LITTLEFS=OFF -DFM_USE_MP3=OFF -DFM_USE_H264=OFF
#   -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=freertos|pthread|cmsis_rtos2
```

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
buffer) and ID3v2 tags are skipped automatically.

## Video: AVI / MP4 / H.264 / MJPEG / audio

The video layer parses FMRV, AVI (RIFF) and MP4 (ISO BMFF) containers into a
per-frame index and hands frame payloads to a pluggable codec:

* **H.264** — vendored [h264bsd](https://github.com/oneam/h264bsd)
  (Apache-2.0 + MIT) → RGB888.
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

The **audio track is demuxed separately** (not decoded in-place): MP4 `mp4a`
(AAC) frames are returned with a 7-byte ADTS header (playable as-is), and AVI
PCM (`01wb`) frames are returned raw — mono or stereo per the container:

```c
FM_AudioTrackInfo ti;
uint8_t frame[1024];
size_t size = 0;

Video_GetAudioInfo(&v, &ti);            /* codec, channels, sample_rate */
while (Video_ReadAudioFrame(&v, frame, sizeof(frame), &size) == FM_OK)
{
    /* AAC (with ADTS) or PCM; feed an audio sink / save to file */
}
```

Notes:

* h264bsd allocates its reference pictures with `malloc`/`free`, sized by the
  stream resolution — on MCUs it needs external RAM (SDRAM on STM32 H7, PSRAM
  on ESP32).  The FileManager core itself stays allocation-free.
* MP4/avc1 sample payloads are converted to Annex-B automatically; AVI H.264
  frames are auto-detected (start-code or length-prefixed).
* picojpeg decodes baseline sequential JPEG only and is not thread-safe.
* The container frame/sample index is static (sized by `FM_VIDEO_MAX_FRAMES`);
  no heap is used by the parser itself.

## Rename-over and atomic replace

`File_Move`, `File_SafeReplace`/`File_AtomicWrite`, `Json_SaveFile`,
`Text_Replace`, `File_Insert`/`File_DeleteRange`/`File_ReplaceRange` and
the crash-safe logger all rely on **rename-over** semantics: renaming
onto an existing file replaces it.  This is provided uniformly by every
adapter — POSIX (native), FMFS, LittleFS and FatFS (the adapter removes
the destination first; a non-empty directory destination is refused with
`FM_ERR_NOT_EMPTY`).  A same-path rename is a no-op, and adapters that
do not implement rename at all are handled safely: `File_Move` /
`Directory_Move` fall back to copy + delete and `File_Rename` /
`Directory_Rename` return `FM_ERR_UNSUPPORTED`.

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

## Memory allocation hooks

The core stays allocation-free, but the optional H.264 decoder, image-resize
helpers and media-player playlist need dynamic memory.  All of them go through
`fm_alloc.h`, so you can redirect them to external RAM in one call:

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

## Resize (images and video)

`Image_Scale()` resizes any pixel buffer (gray/RGB565/RGB888/RGBA8888) with
nearest or bilinear interpolation; `Image_ResizeFile()` resizes a file on
storage, and `Video_DecodeFrameScaled()` decodes and scales video in one step:

```c
uint8_t small[80 * 45 * 3];
Video_DecodeFrameScaled(&v, small, sizeof(small), 80, 45, FM_SCALE_BILINEAR);
```

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

- Space is reclaimed automatically: when the tail of the data area is
  used up, a write compacts the area (and, if needed, rotates the
  growing file to the end) before it reports `FM_ERR_NO_SPACE`.  A write
  therefore fails for lack of space only when the live data really does
  not fit, and `FS_GetFreeSpace()` reports capacity minus live data.
  `FMFS_Repack()` does the same compaction on request; open handles stay
  valid across it.
- Metadata is rewritten only when it changes: reading a file, closing a
  handle, and overwriting bytes in place do not touch the table.
- A handle whose file was deleted or replaced by a rename returns
  `FM_ERR_NOT_FOUND`; it can never reach the file that reuses the slot.
- Compaction moves data in place and is **not power-fail safe**: a reset
  in the middle of a compaction can leave files damaged.  Use LittleFS
  where that matters.

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

- `examples/sdcard_fatfs.c` — use the FatFS/diskio generated by CubeMX
- `examples/sdcard_fm_diskio.c` — use FileManager's polling diskio bridge
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
- `examples/i2c_eeprom_dma.h` — I2C EEPROM bridge with DMA-driven data phase (ACK-polling write cycle) wired to `FM_Eeprom`; feeds example 28
- `examples/spi_flash_dma_port.h` — serial flash on plain SPI for any STM32 family: DMA-capable transfer callback, FMFS mount on `/flash`, file/CRC/transfer/stats test suite (`exF_1..5`)
- `examples/usb_sd_presenter.h` — معماری «فقط SD به PC» با سوییچ هوشمند مالکیت (ShowToPc/TakeBack) و فایل دوطرفهٔ FAT
- `examples/usb_present.h` — present SD / QSPI / SPI-flash to the PC as a USB MSC drive (single-LUN switch, owner-switch helpers, full CubeMX checklist)
- `examples/fm_xdma.h` — the one portable DMA engine behind examples 15/26: auto-detected per family (H7 MDMA, H5 GPDMA/OCTOSPI), permanent polling fallback, cache maintenance in one place
- `examples/examples_sdcard.h` — file examples 01-10 on a mounted SD card
- `examples/examples_sdcard_media.h` — WAV/FMRV, media detection and verified media copies
- `examples/examples_sdcard_player.h` — playlist, seek, next/previous and audio/video player controls
- `examples/simple_audio.h` — WAV/MP3 playlist + I2S DMA
- `examples/simple_image.h` — photo album + TFT present callback
- `examples/simple_video.h` — folder playlist + TFT present callback + optional I2S
- `examples/hw_jpeg.h` — STM32 JPEG peripheral for Image_Load / SimpleImage
- `examples/spi_tft_dma.h` — H7 SPI master DMA bounce (no ILI9341 driver)
- `examples/README_sdcard_stm32h7.md` — CubeMX setup and STM32H7 SD/DMA/cache troubleshooting

The target example suites are also compiled and executed against the host FatFS
mock (with AddressSanitizer and UndefinedBehaviorSanitizer):

```bash
sh test/build_fatfs_host.sh
```

## License

MIT
