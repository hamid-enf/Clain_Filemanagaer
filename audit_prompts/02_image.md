# External audit — images

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `8dd0f25` (audit exactly this commit; quote it in your report)
- Library root: `Clain_FileManager/FileManager_lib/` (all paths below are relative to it)

It is a C99 file-manager and media library for STM32 microcontrollers (STM32H7
is the main target; smaller parts use it with features switched off). It runs
bare-metal or under FreeRTOS / CMSIS-RTOS2. Think in those terms while you read:

- `size_t`, `int` and pointers are **32 bits** on the target. The host tests run
  on 64 bits, so overflow and truncation bugs can hide there.
- Stacks are a few kilobytes. RAM is static pools plus an optional allocator
  (`FM_Alloc`). A large stack frame or an allocation whose size comes from a
  file is a finding.
- Input files come from SD cards and USB: every file is untrusted.
- A hang is as bad as a crash. An infinite loop on a malformed file stops the
  device.

Your part is **images**. Three other auditors cover the other parts
(files and filesystems, video, audio). Stay in your part; if you trip over something outside it, give it
one line in the "Outside my scope" section and move on.

## 2. Hard rules

1. **Read-only on GitHub.** No commits, pushes, branches, pull requests, issues
   or comments. Nothing you do may change the repository or its GitHub page.
2. Work in a local clone. You **may** add and change files locally (test
   harnesses, fuzzers, instrumentation, experiments). None of it is sent back;
   whatever matters goes into your report as text.
3. Do not fix the library. For each defect, describe the fix and include it as
   a unified diff in the report. If you tried the fix locally and re-ran the
   tests, say so.
4. Report only what you can stand behind. A finding you reproduced is
   "confirmed". A finding you believe from reading the code but could not
   trigger is a "suspicion" and goes in its own section. Do not present one as
   the other.
5. No padding. Style, naming, formatting and "could be refactored" are not
   findings. Ten real defects are worth more than a hundred remarks.

## 3. How to build and run

```bash
cd Clain_FileManager/FileManager_lib
make test                 # whole suite, about 358 tests, about 12 s
make asan                 # same suite under ASan + UBSan
make tsan                 # threaded suite under ThreadSanitizer
make check                # everything: tests, sanitizers, ports, examples, cmake
make matrix-quick         # a subset of the 45 build configurations
make test FM_USE_THREADS=1
make test FM_USE_IMAGE=0  # every FM_USE_* switch can be set like this
```

- All tests are in `test/main.c` (about 20 000 lines; suites are functions named
  `test_*`, registered with `RUN_SUITE` near the end). It is the best source of
  working call sequences for every API.
- Configuration lives in `include/fm_config.h` (`FM_USE_*` feature switches and
  size limits). The `asan` target in the `Makefile` shows how to build an
  instrumented `libfilemanager.a` to link your own harness against
  (`-Iinclude -Ibackends -Isrc`).
- To feed a file to the library from a harness, mount a RAM disk and write the
  bytes into it: see `FileManager_MountMemory(...)` in `test/fatfs_host_test.c`
  and the many `RamDisk_Init` / `Storage_Register` / `FMFS_Init` / `FS_Mount`
  sequences in `test/main.c`.
- Source files use CRLF line endings. Some comments are in Persian; the code and
  identifiers are English.

## 4. What has already been done (do not repeat it)

The suite passes in 45 build configurations under ASan + UBSan, and under
ThreadSanitizer through three synchronisation ports. Running the existing tests
again will find nothing. The defects that are left are the ones those tests do
not reach. Methods that have **not** been applied yet, and are therefore the
most likely to pay off:

- **Fuzzing** of every parser with ASan + UBSan (libFuzzer or AFL++), seeded
  from `test/vectors/` and from files you generate.
- **Differential testing** against a reference implementation.
- **A 32-bit build** (`-m32`, if your machine has the multilib packages) of the
  suite and of your harnesses.
- **Fault injection**: a read or write that fails in the middle, a short read,
  an allocation that fails, a file truncated at every byte offset.
- **Static analysis** (`gcc -fanalyzer`, clang static analyzer, cppcheck) —
  but triage it: report a warning only after you have confirmed it is real.
- **Stack measurement** (`-fstack-usage`): frames above about 1 KB in a call
  path the application reaches.
- **Careful reading** for: integer overflow in size arithmetic, lengths taken
  from a file and used unchecked, state left half-changed when a function
  returns an error, handles or slots not released on error paths, loops whose
  exit depends on file content, behaviour that differs from what the header
  comment or `README.md` promises.

## 5. Your scope

| File | What it is |
|---|---|
| `src/fm_image.c`, `include/fm_image.h` | image API, BMP codec, pixel conversion, scaling, effects, flip, rotate, streaming decode |
| `src/fm_png.c` | PNG decoder and encoder, written for this library (including its own inflate) |
| `src/fm_jpeg.c` | JPEG decode (through `third_party/picojpeg`), encode, hardware-codec path |
| `src/fm_hw.c`, `include/fm_hw.h` | hooks for the STM32 hardware JPEG codec |
| `examples/simple_image.h`, `examples/hw_jpeg.h`, `examples/image_bmp.c` | how applications use it |
| `tools/png_ref.py`, `tools/gen_png_vectors.sh`, `test/vectors/` | reference tooling and test images |

Tests to read first in `test/main.c`: `test_media`, `test_scale`,
`test_image_report`, `test_png_formats`, `test_jpeg_hw_file`.

Limits to know (`include/fm_config.h`): `FM_IMAGE_MAX_BYTES`,
`FM_IMAGE_LINE_MAX`, `FM_PNG_WINDOW`, `FM_PNG_MAX_STRIDE`,
`FM_JPEG_PROBE_BUDGET`, `FM_JPEG_HW_FILE_MAX`, `FM_JPEG_HW_HEAP`.

## 6. Where to dig

**Fuzz every entry point that takes file bytes**: `Image_Load`, `Image_LoadEx`,
`Image_GetInfo`, `Image_DecodeBMP`, `Image_DecodePNG`, `Image_DecodeJPEG`,
`Image_ProbePNG`, `Image_ProbeJPEG`, the streaming path (`Image_OpenStream`,
`Image_ReadLine`, `Image_StreamDecode`, `Image_CloseDecoder`) and
`Image_ResizeFile`. One harness per format, with the output buffer and the work
buffer at several sizes including too small. Also truncate each valid test
image at every byte offset. Nothing may crash, read or write out of bounds,
loop, or leave a decoder slot or file handle taken.

**PNG** (`src/fm_png.c` is hand-written — the highest-yield file in your part).

- Every colour type × bit depth the standard allows, with and without Adam7
  interlace, with `tRNS`, with palettes shorter than the pixel values need,
  1-pixel-wide and 1-pixel-high images, the largest dimensions the limits
  admit. Compare every decoded pixel with libpng or Pillow.
- The inflate: stored blocks, fixed and dynamic Huffman, invalid and incomplete
  code-length sets, distances beyond the window or before the start of output,
  IDAT split at every byte, trailing garbage, zlib header variants, wrong
  Adler-32, wrong chunk CRC, chunks out of order, missing `IEND`.
- Size arithmetic: width × height × bytes-per-pixel and row stride in 32 bits.
- The encoder: whatever `Image_EncodePNG` writes must be read back identically
  by libpng, for every pixel format and for odd sizes.

**JPEG.**

- Baseline with 4:4:4, 4:2:2, 4:2:0, 4:1:1 and grayscale; dimensions that are
  not a multiple of the MCU; restart markers; files with EXIF, thumbnails,
  comments and padding before the image; progressive, arithmetic-coded,
  lossless and CMYK files (not supported: each must be refused with an error,
  not decoded to garbage).
- Compare decoded pixels with libjpeg within the tolerance a different IDCT
  allows; look for colour-channel swaps, shifted rows or columns, wrong edge
  blocks.
- The hardware-codec path is exercised on the host through test hooks
  (`test_jpeg_hw_file`): check the fallbacks when the file is too large for
  `FM_JPEG_HW_FILE_MAX`, when the heap block is refused, when the hook fails
  half way.
- `Image_EncodeJPEG`: output decodable by libjpeg, quality extremes, odd sizes.

**BMP.** 1, 4, 8, 16, 24, 32 bits per pixel; RLE4 and RLE8; `BI_BITFIELDS` with
unusual masks; top-down files (negative height); every header version; palette
indices out of range; row padding; `biSize`, data offset and file size that
contradict each other; width or height zero or negative. Find out which of
these variants are supported: one that is not must be refused with an error,
never decoded to garbage or read past the buffer.

**Pixel operations.**

- `Image_Convert` and the three fixed converters across all seven pixel formats
  (`FM_PIXEL_*`), 49 pairs: channel order, 565 packing and byte order, alpha,
  gray weighting, in-place conversion where it is allowed.
- `Image_Scale`, `Image_ScaleEx`: 1×1 source or destination, extreme ratios,
  up and down, non-square, rounding at the last row and column, source and
  destination overlapping, stride not equal to width × bytes per pixel.
- `Image_ApplyEffect`, `Image_Flip`, `Image_Rotate`: odd sizes, 1-pixel
  dimensions, each pixel format.
- `Image_BufferSize` / `Image_BytesPerPixel`: overflow, and whether every
  function that needs a buffer checks the size it was given.

**State and configuration.**

- `Image_SetWorkBuffer` with a buffer too small, changed while a streaming
  decode is open, set to NULL.
- Codec registry: register, unregister and replace codecs; unregister one while
  it is decoding.
- Builds with `FM_USE_JPEG=0`, `FM_USE_PNG=0`, both off, `FM_USE_IMAGE=0`: the
  API must answer "unsupported" cleanly and nothing may be left half-linked.
- 1000 load / close cycles of each format, including failing loads: no slot,
  handle or allocator block may leak (check the allocator statistics).

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- PNG output is RGB888 only.
- JPEG decoding is baseline sequential only (limit of the vendored picojpeg),
  and picojpeg is not thread-safe — how the library guards it with
  `FM_USE_THREADS=1` is in scope.
- The hardware JPEG path needs the whole file in memory.
- Formats that media detection recognises but the library does not decode (GIF,
  TIFF, WebP) are refused; that is by design.

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_IMAGE.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and whether a 32-bit build was possible.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `IMG-01`, `IMG-02`, ...:
   - **Severity** — S1: memory corruption, crash, hang, loss or corruption of
     stored data. S2: wrong result returned as success (wrong pixels, samples,
     bytes, sizes, timestamps). S3: wrong error handling — leak of a handle or
     slot, wrong error code, state not restored after a failure. S4: mismatch
     with the documentation, portability, wasted time or memory.
   - **Where** — file, function, line numbers at the audited commit.
   - **How to reproduce** — the smallest program or input that shows it. For an
     input file give a hex dump if it is under 200 bytes, otherwise the script
     that generates it. Give the exact build command and the exact output
     (sanitizer report, wrong value, hang).
   - **Expected and observed.**
   - **Root cause** — why it happens, not just where.
   - **Proposed fix** — as a unified diff, and whether you tested it.
   - **Which build configurations are affected**, if not all.
5. **Suspicions** — same format, without a reproducer, with what stopped you.
6. **Examined and found sound** — list each area you covered and how (for
   example "BMP decoder: 6 h of libFuzzer, 41 M executions, 92 % line coverage
   of the decoder, no findings"). This section matters as much as the findings:
   it tells us what an empty result is worth.
7. **Not examined** — everything in your scope you did not get to.
8. **Outside my scope** — one line each.

Put harness sources, generator scripts and crashing inputs in an appendix (or
attach them as files next to the report). Someone must be able to reproduce
every confirmed finding from the report alone.
