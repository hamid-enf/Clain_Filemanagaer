# External audit, follow-up round — images

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `2a45d9d` (audit exactly this commit; quote it in your report)
- Previous audit of this part: commit `8dd0f25`, report in `ReportV2.txt` at the
  repository root, lines 1583–2454
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
make test                 # whole suite, about 478 tests, about 15 s
make asan                 # same suite under ASan + UBSan
make tsan                 # threaded suite under ThreadSanitizer
make m32                  # suite built and run as a 32-bit program (clang);
                          #   needs:  pip install ziglang   (make m32 M32=ubsan adds UBSan)
make check                # everything: tests, sanitizers, ports, examples, cmake, 32-bit
make matrix-quick         # a subset of the 49 build configurations
make test FM_USE_THREADS=1
make test FM_USE_IMAGE=0  # every FM_USE_* switch can be set like this
make test CFLAGS_EXTRA="-DFM_DEBUG_LEVEL=3"   # debug level 0, 1 (default), 2, 3
FM_TEST_LOG=1 ./fm_test 2>log.txt             # see every message the library prints
```

- The tests are in `test/main.c` (about 20 000 lines; suites are functions named
  `test_*`, registered with `RUN_SUITE` near the end), `test/audit_v2_tests.h`
  (the tests written for the findings of the previous audit, suites
  `test_audit_v2_*`) and `test/debug_log_tests.h`. They are the best source of
  working call sequences for every API.
- Configuration lives in `include/fm_config.h` (`FM_USE_*` feature switches and
  size limits). The `asan` target in the `Makefile` shows how to build an
  instrumented `libfilemanager.a` to link your own harness against
  (`-Iinclude -Ibackends -Isrc`).
- To feed a file to the library from a harness, mount a RAM disk and write the
  bytes into it: see `FileManager_MountMemory(...)` in `test/fatfs_host_test.c`
  and the many `RamDisk_Init` / `Storage_Register` / `FMFS_Init` / `FS_Mount`
  sequences in `test/main.c`.
- For a 32-bit harness of your own, use the compiler `tools/check_32bit.sh`
  uses: `python3 -m ziglang cc -target x86-linux-musl` (a static 32-bit x86
  program that runs on a 64-bit Linux). The same tool cross-compiles for the
  real target, which is enough to measure code size and stack frames:
  `python3 -m ziglang cc -target arm-linux-musleabihf -mcpu=cortex_m7 -mthumb -Os -fstack-size-section`.
  (This clang writes no `.su` files for `-fstack-usage` on that target; the
  frame sizes are in the `.stack_sizes` section of each object, which
  `llvm-readobj --stack-sizes` prints. The frames above 1 KB measured this way
  are in `README.md`, "Stack".)
- Source files use CRLF line endings. Some comments are in Persian; the code and
  identifiers are English.

## 4. This is a follow-up audit: what changed, and your three jobs

This part was audited once before, at commit `8dd0f25`. That report is in the
repository: `ReportV2.txt` at the repository root, lines 1583–2454 (the other
line ranges are the other three parts). Read it first, including its sections
"Examined and found sound" and "Not examined". Everything it found has been
worked on since; `git log 8dd0f25..2a45d9d` and
`git diff 8dd0f25..2a45d9d -- src/fm_image.c src/fm_png.c src/fm_jpeg.c src/fm_hw.c include/fm_image.h` show how.

**What the maintainers found themselves after `cf187af`, and what it tells
you.** While measuring stack frames for the ARM target, one frame led to the
audio track of a video file, and from there files written by ffmpeg were
decoded by the library next to ffmpeg's own decode. That comparison, which no
audit round and no test had made, found: an MP3 track that decoded one frame
and then failed for ever; IMA and Microsoft ADPCM tracks that decoded to noise;
Vorbis tracks that never decoded; H.264 pictures 25 dB from the reference
(wrong colour range); JPEG green wrong in saturated colours; Motion JPEG in an
`.mp4` not playable. All of that had passed the whole suite under every
sanitizer, because the suite checked that a format is *recognised* and that
nothing *crashes*, not that what comes out is *right*. These are fixed
(commits `7d4d009`, `b3525f7`, `2a45d9d`; tests in `test/audit_v3_tests.h`).
Draw the conclusion for your part: **wherever the library produces data,
compare that data with a reference implementation**, on files a real tool
wrote, before anything else.

**Job 1 — check the fixes (do this first, it is the cheapest way to find a
defect).** For every earlier finding listed below, decide: fixed, partly fixed,
not fixed, or fixed but something else broke. A fix that handles exactly the
reported input and not its neighbours is "partly fixed": vary the input (other
sizes, other backends, other build switches, 32 bits, threads on, the same
mistake in the sibling function). Read the test that was added for it
(`test/audit_v2_tests.h`) and judge whether it would really fail if the fix
were removed; if you can, remove the fix locally and see.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| IMG-01 | PNG encoder declared a short `IDAT` for many image sizes | chunk length correct for every size; output read back by a reference decoder |
| IMG-02 | `Image_GetInfo` under-reported channels for 1-channel PNGs | channels, bit depth and data size agree with what the decoder produces |
| IMG-03 | 8-bit paletted BMP: palette ignored, indices returned as grey | palette applied (`FM_ImageDecoder` grew by 768 bytes for it — check what that does to the stack of every caller that has one as a local) |
| IMG-S1 | hardware-JPEG path keeps the codec scratch claimed across `hw->decode()` | **not changed** (needs hardware); still open |
| IMG-S2 | software re-decode hides the hardware codec's reason | **not changed**, documented |
| IMG-S3 | `Image_GetInfo` validates only the PNG header | judged not a defect (a probe is a probe); say if you disagree, with an input |
| IMG-S4 | `width × height × bpp` on a 32-bit `size_t` | rejected by reading the code, never run. **A 32-bit build exists now (`make m32`): settle this one by experiment.** |

**Job 2 — audit what is new.** Code written since the last audit has been
through the fewest eyes. Go through the diff above line by line.
In this part: the PNG encoder's size arithmetic, the BMP palette path (palettes
shorter than 256 entries, `biClrUsed` larger than the file, 1-bit and 4-bit
files, RLE with a palette, indices beyond the palette), `Image_GetInfo` for
every format against what a full decode reports, the result logging added
to `Image_LoadEx`, and the **local patch to `third_party/picojpeg/picojpeg.c`**
(search for "FileManager patch"): the green channel used to be computed in two
clamped steps and is now computed in one, with the Cb term of each block kept
in `gCbG[64]` until the Cr block arrives. Check every chroma layout picojpeg
accepts (grey, H1V1, H2V1, H1V2, H2V2), restart markers, a file cut between
the Cb and the Cr block, and compare whole pictures with libjpeg: it was 37 dB
before the patch and 50 dB after on the files the maintainers tried, which
were few.

**Job 3 — go where nobody has been.** The previous auditor of this part could not build for
32 bits, did not run the threaded configuration, and compared the JPEG
**encoder** only loosely. All size arithmetic in this part was "re-derived by
hand for 32 bits": redo it by running. Then the JPEG encoder against libjpeg,
the image codec under `FM_USE_THREADS=1` with ThreadSanitizer (several tasks
loading at once, a nested load from a callback), and the streaming decode API,
which the earlier report covers least.

What is already established (running it again will find nothing): the suite
passes in 49 build configurations under ASan + UBSan, under ThreadSanitizer
through three synchronisation ports, and as a 32-bit x86 program built with
clang, with and without UBSan. Methods most likely to pay off now:

- **Fuzzing** of every parser with ASan + UBSan (libFuzzer or AFL++), seeded
  from `test/vectors/` and from files you generate. Say how many executions
  and what coverage you reached.
- **Differential testing** against a reference implementation.
- **32-bit harnesses**: a 32-bit build exists now (section 3), so size
  arithmetic near 2^32 can be tested for real instead of argued about. Build
  your fuzz and fault-injection harnesses for 32 bits too.
- **Fault injection**: a read or write that fails in the middle, a short read,
  an allocation that fails (every Nth, for every N), a file truncated at every
  byte offset.
- **Static analysis** (`gcc -fanalyzer`, clang static analyzer, cppcheck) —
  but triage it: report a warning only after you have confirmed it is real.
- **Stack measurement** (`-fstack-usage` on the host, `-fstack-size-section`
  for the ARM target, see section 3): frames above about 1 KB in a call path the application reaches.
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

**Debug levels and log output (new since the last audit).** The library prints
through `FM_ERR_LOG` (level 1 and up), `FM_EVENT_LOG` (2 and up) and
`FM_TRACE_LOG` (3); level 0 compiles all of it out. Read the top of
`include/fm_debug.h` and "Rules for a sink" in `include/fm_log.h`, then check
the messages of **your** files (`grep -n "FM_ERR_LOG\|FM_EVENT_LOG\|FM_TRACE_LOG\|FM_CHECK" src/fm_image.c src/fm_png.c src/fm_jpeg.c`):

- A `%s` whose argument comes from a file (a name, a tag, a codec string): can
  it be NULL, not terminated, or longer than its buffer? Can file content put
  control characters or a megabyte of text on the user's console?
- A message on a path that runs per frame, per sample block or per storage
  read at level 1 or 2 (level 3 is allowed to be slow, levels 1 and 2 are not).
- An error line for an outcome that is normal (file not found, end of file),
  and the reverse: a real failure in your part that prints nothing at level 1,
  so that the user sees only a result code. List the ones that would have cost
  you time.
- Behaviour that depends on the level: an expression with a side effect inside
  a log argument (it disappears at level 0), a variable that is only assigned
  for a message. Run your harnesses at levels 0 and 3 and compare results.
- Build your part with `-DFM_DEBUG_LEVEL=0`, `=3` and `-DFM_LOG_DISABLE` under
  `-Werror`, with each of your `FM_USE_*` switches off.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- PNG output is RGB888 only.
- JPEG chroma layouts with a sampling factor other than 1x1 on the chroma
  components are refused (ffmpeg's 4:2:2 and 4:4:4 Motion JPEG is written that
  way); the supported ones are listed in `README.md`.
- JPEG decoding is baseline sequential only (limit of the vendored picojpeg),
  and picojpeg is not thread-safe — how the library guards it with
  `FM_USE_THREADS=1` is in scope.
- The hardware JPEG path needs the whole file in memory.
- Formats that media detection recognises but the library does not decode (GIF,
  TIFF, WebP) are refused; that is by design.

- IMG-S1 and IMG-S2 of the previous audit (hardware JPEG path: scratch kept
  claimed across `hw->decode()`; software fallback hides the codec's reason)
  are known and unchanged.
- BMP reading: see the BMP paragraph of `README.md` for the variants that are
  supported; a variant outside that list being refused is not a defect.
- Everything the previous audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_IMAGE_V3.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, how many of the earlier findings
   are really fixed, your seven scores, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and what you ran as a 32-bit program.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `IMG3-01`, `IMG3-02`, ...:
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

9. **Verdict on the earlier findings** — one table: earlier ID, your verdict
   (fixed / partly fixed / not fixed / fixed but broke something), how you
   checked (which variants you tried), and whether the test added for it would
   catch a regression.
10. **Scores** — so that this round can be compared with the last one and with
   the next. Give each line a whole number from 0 to 10 and one sentence that
   names the evidence. Write "not assessed" instead of guessing; a score
   without evidence behind it is worth nothing.

   | Axis | What 10 means |
   |---|---|
   | A. Malformed input | no crash, hang or out-of-bounds access on any input you could construct, after real fuzzing |
   | B. Correct results | every valid input you tried gives the reference result |
   | C. Errors and resources | every failure returns the right code, releases what it took and leaves a usable state |
   | D. Memory and timing | stack frames, allocation sizes and worst-case call times fit a microcontroller and are bounded |
   | E. Documentation | headers and `README.md` say what the code does, including the limits |
   | F. Tests | the existing tests of this part would catch a regression in what they cover, and cover what matters |
   | G. Diagnosability | when something fails, the result code and the log line together tell a user why |

   Then one line: **ready for a product / ready under these conditions (list
   them) / not ready**, for your part only.

Put harness sources, generator scripts and crashing inputs in an appendix (or
attach them as files next to the report). Someone must be able to reproduce
every confirmed finding from the report alone.
