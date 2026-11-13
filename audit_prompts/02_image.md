# External audit V6 — image

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and give the **exact place** in the code
and a **fix** for it. You do not change the repository.

This audit is one turn of a cycle: audit, the maintainers fix, audit again,
until a turn finds nothing. Five auditors work in parallel, one per part. What
you write is turned into code changes directly, by someone who has only your
report and a clean checkout. An imprecise report costs more than a short one:
read section 2 before you start and section 8 before you write.

How this prompt is organised: 1 what you audit; 2 the rules; 3 how to build,
and what is already established; 4 the history, your jobs and the open leads;
5 your files and the margins you may cross; 6 where to dig; 7 what not to
report; 8 the report. Sections 4 and 6 are the work. If your time runs short,
keep this order: Job 1, the open leads, Job 2, then the rest.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `6fe596d` (`6fe596d445c17569dace9fe490929b8d4f3b0fe9`). Audit exactly
  this commit (`git checkout 6fe596d`) and quote it in your report. Later
  commits on the branch may change the code: audit exactly the pinned commit.
- Library root: `Clain_FileManager/FileManager_lib/`. Paths below are relative to
  it, except: paths that start with `Core/` are in `Clain_FileManager/Core/`
  (the CubeMX firmware project of the reference board), and `ReportV2.txt`,
  `Report.txt` and `audit_prompts/` are at the repository root.

History of this part:

| Round | Commit audited | Report | Then |
|---|---|---|---|
| first | `8dd0f25` | `ReportV2.txt`, lines 1583–2454 | the findings were worked on, up to `a6cf829` |
| follow-up ("V3") | `a6cf829` | none in the repository | the prompt was written (`git show f161ff8:audit_prompts/02_image.md`); no report of it is in the repository, so **nobody outside has checked the fixes of the first round** or read the JPEG patches made after it. Its jobs are carried into this round |
| V4 | `48a00bc` | `ReportV3.txt`, lines 642–1272 | two findings and two suspicions; IMG4-01, IMG4-02 and IMG4-S2 fixed in `fc31b25`, IMG4-S1 open |
| V5 | `fc31b25` | `ReportV4.txt`, lines 268–561 | no finding; two suspicions (IMG5-S1, IMG5-S2), both examined by the maintainers and refuted; no code of this part changed in `6fe596d`, two tests added |
| this one (V6) | `6fe596d` | `AUDIT_IMAGE_V6.md` | |

It is a C99 file-manager and media library for STM32 microcontrollers. The
reference board is an STM32H743 with an SD card, SDRAM, I2S audio out and an
SPI TFT; smaller parts use the library with features switched off. It runs
bare-metal (one main loop plus interrupts: that is how the reference firmware
runs) or under FreeRTOS / CMSIS-RTOS2. Think in those terms while you read:

- `size_t`, `int` and pointers are **32 bits** on the target. The host tests run
  on 64 bits, so overflow and truncation bugs can hide there.
- Stacks are a few kilobytes. RAM is static pools plus an optional allocator
  (`FM_Alloc`) that can fail. A large stack frame or an allocation whose size
  comes from a file is a finding.
- Input files come from SD cards and USB: every file is untrusted.
- A hang is as bad as a crash. An infinite loop on a malformed file stops the
  device.
- A result that looks right and is wrong (wrong pixels, wrong samples, a wrong
  size, a recording that silently lost data) is as bad as a crash too. The
  history of this project shows it is the kind of defect that survives longest
  (section 4).
- The code was written and tested on a PC. The owner builds it with
  STM32CubeIDE for the board; no board result and no CubeIDE build log for any
  commit after `b6bcc4e` is in the repository.

The library is audited in five parts, one auditor each:

| Part | Prompt | What it owns |
|---|---|---|
| Files | `01_file_storage_filesystem.md` | file API, VFS, the filesystems and their adapters, storage drivers, the utilities on top of the file API, the log layer, and the build configuration (`FM_USE_*`, `FM_BUFFERS`, the firmware-build rules) |
| Image | `02_image.md` | BMP / PNG / JPEG decoding and encoding, pixel operations, `SimpleImage` |
| Video | `03_video.md` | containers, H.264, MJPEG, the audio tracks of video files, A/V timing, media cache, the playback side of `SimpleVideo` |
| Audio | `04_audio.md` | audio decoders, the playlist player, type detection, the playback side of `SimpleAudio` / `AudioDma` with its output switch and callbacks, the older WAV writer and recorder |
| Save | `05_save_record.md` | everything that writes media: audio encoders, the AVI writer, the converters, the `Rec*` functions of both players, the save examples, the board glue, and how all of it works together |

Your part is **Image**: BMP / PNG / JPEG decoding and encoding, pixel
operations, and `SimpleImage`. Section 5 says exactly which files, and how far
you may go into the other parts.

## 2. Hard rules

1. **Read-only on GitHub.** No commits, pushes, branches, pull requests, issues
   or comments. Nothing you do may change the repository or its GitHub page.
2. **Your local clone is yours.** You may add and change anything in it: test
   harnesses, fuzzers, instrumentation, and the library's own code — to try a
   fix, to add an assertion, or to break something on purpose and see whether a
   test notices. None of it is sent back; whatever matters goes into the report
   as text (diffs, sources, commands, output).
3. **Every defect comes with its place and its fix.** The place is file,
   function and line numbers at the audited commit. The fix is a unified diff
   against the audited commit. Try it: apply it, run your reproducer again (it
   must pass now), run `make test` (it must still pass), and write down both
   result lines. A fix you could not try is marked "not tested", with the
   reason. When the right fix is a design decision that is not yours to take,
   give the options and the smallest safe change.
4. **Confirmed or suspicion, nothing in between.** A finding you reproduced is
   "confirmed". A finding you believe from reading the code but could not
   trigger is a "suspicion" and goes in its own section, with what stopped you.
   Do not present one as the other.
5. **Evidence is what you ran, not what you expect.** Quote output as it was
   printed. Never write the result of a command you did not run, of a tool you
   do not have, or of a file you did not create. When a tool this prompt names
   is not available to you, say so in the audit record and say what you used
   in its place. Check every line number against
   `git show 6fe596d:<path> | sed -n '<from>,<to>p'` before you write it down:
   a wrong line number makes a finding unusable, and one invented trace makes
   the whole report untrustworthy.
6. **One root cause, one finding.** The same mistake in five places is one
   finding that lists five places. Two symptoms that one fix removes are one
   finding.
7. **No padding.** Style, naming, formatting and "could be refactored" are not
   findings. A warning of a static analyser is not a finding until you have
   shown that it is real. A documented limit (section 7, `README.md`, the header
   comment of the function) is not a finding unless the real behaviour is worse
   than what is written. Ten real defects are worth more than a hundred remarks.
8. **Say what you did not do.** A part of your scope you did not reach, a
   method you did not use, a configuration you did not build: each is listed.
   "Examined, nothing found" may only be written about what was examined, and
   it says how.

## 3. How to build and run

```bash
cd Clain_FileManager/FileManager_lib
make test                 # whole suite: 635 tests at this commit
make asan                 # the same suite under ASan + UBSan (+ LeakSanitizer)
make tsan                 # the suite with FM_USE_THREADS=1 under ThreadSanitizer (640 tests)
                          #   make tsan FM_SYNC_IMPL=freertos | cmsis_rtos2  goes through that port
make m32                  # the suite built and run as a 32-bit x86 program (636 tests);
make m32 M32=ubsan        #   needs:  pip install ziglang
make cachetest videohooks hwjpegtest usbmsctest audiorecordtest saveexamplestest tapstest fatfs
                          # the smaller host tests (stub HAL, mock FatFS)
make check                # all of the above except tsan, plus: every port compiles, every example
                          #   header compiles alone, firmware-build rules, switch names, flags
                          #   stamp, CMake (13 configurations), 32-bit
make matrix-quick         # the 8 IMAGE / VIDEO / AUDIO combinations
make matrix               # every FM_USE_* combination, built from scratch each (about an hour)
make matrix-asan          #   the same under ASan + UBSan
make test FM_USE_THREADS=1
make test FM_USE_IMAGE=0  # every FM_USE_* switch can be set like this
make test CFLAGS_EXTRA="-DFM_DEBUG_LEVEL=3"   # debug level 0, 1 (default), 2, 3
FM_TEST_LOG=1 ./fm_test 2>log.txt             # see every message the library prints
```

- GNU make 4.2 or newer. The `Makefile` keeps the compiler flags in
  `.fm_build_flags` and rebuilds every object when they change, so a switch
  given on the `make` command line needs no `make clean` (with an older make
  this does not work and stale objects get linked: run `make clean` yourself).
  Objects are written next to the sources, so **one build at a time per
  tree**: two `make` running in the same tree link a mix of objects without
  any message. Give every instrumented build its own copy of the tree.
- The tests are in `test/main.c` (about 20 600 lines; suites are functions named
  `test_*`, registered with `RUN_SUITE` near the end) and in the headers it
  includes: `test/audit_v2_tests.h` (tests for the findings of the first
  audit), `test/audit_v3_tests.h` (comparisons with ffmpeg output),
  `test/debug_log_tests.h`, `test/audio_save_tests.h`,
  `test/video_save_tests.h`. They are the best source of working call sequences
  for every API.
- Configuration: `include/fm_config.h` (the `FM_USE_*` feature switches and the
  size limits) and, for the board, `include/fm_board_config.h` (the one file a
  user edits), with `include/fm_switches.h` and `include/fm_buffers_level.h`
  under it. `docs/OPTIONS_FA.md` lists every option (Persian).
- A harness of your own links against `libfilemanager.a` with
  `-Iinclude -Ibackends -Isrc`; the `asan` target of the `Makefile` shows how to
  build an instrumented library. To feed a file to the library, mount a RAM
  disk and write the bytes into it: see `FileManager_MountMemory(...)` in
  `test/fatfs_host_test.c` and the many `RamDisk_Init` / `Storage_Register` /
  `FMFS_Init` / `FS_Mount` sequences in `test/main.c`.
- A 32-bit harness of your own: use the compiler `tools/check_32bit.sh` uses,
  `python3 -m ziglang cc -target x86-linux-musl` (a static 32-bit x86 program
  that runs on 64-bit Linux). The same tool cross-compiles for the real target,
  which is enough to measure code size and stack frames:
  `python3 -m ziglang cc -target arm-linux-musleabihf -mcpu=cortex_m7 -mthumb -Os -fstack-size-section`
  (this clang writes no `.su` files for that target; the frame sizes are in the
  `.stack_sizes` section of each object, `llvm-readobj --stack-sizes` prints
  them; the frames above 1 KB measured this way are in `README.md`, "Stack").
- The player layer (`top_api/*.h`: `SimpleAudio`, `SimpleVideo`, `SimpleImage`,
  `AudioDma`, the storage drivers of the board) is header-only code that needs
  the ST HAL. On a PC it is tested against a stub HAL (`tools/halstub/`), with
  the interrupts raised by the test: `tools/save_examples_host_test.c`,
  `tools/audio_record_host_test.c`, `test/video_hooks_test.c`,
  `test/player_examples_host.c`, `test/media_examples_host.c`,
  `test/sdcard_examples_host.c` show how. Their build lines are in the
  `Makefile` and in `test/build_fatfs_host.sh`.
- `top_api/X.h` and `examples/X.h` are meant to be the same file (the firmware
  includes `top_api/`, the host checks use `examples/`). Compare them with
  `diff <(sed 's#"../top_api/#"#' top_api/X.h) examples/X.h`; a difference
  beyond the `#include` lines is a finding.
- The firmware is `Core/Src/main.c` (a CubeMX project; it includes
  `FileManager_lib/board/app_*.h`, which include `top_api/`). It cannot be
  linked without the ST project files, but it can be compiled, and a compile is
  the only check the board code gets here: clone
  `STMicroelectronics/stm32h7xx_hal_driver`, `cmsis_device_h7`, `cmsis_core`,
  `stm32_mw_usb_device`, `stm32_mw_fatfs` (branch `release/r0.12c`) and newlib
  headers, and compile with `python3 -m ziglang cc -target
  thumb-freestanding-eabihf -mcpu=cortex_m7` (or `arm-none-eabi-gcc` if you
  have it — say which). In a firmware build only `src/`, `backends/`,
  `third_party/` and `top_api/` contain code; every other `.c` must compile to
  nothing (`tools/check_cubeide.sh`).
- Reference tools worth having: `ffmpeg` / `ffprobe`, `flac`, `sox`, libjpeg /
  libpng (or Pillow), Python with `av` and `numpy`. If you cannot install one,
  say so in the audit record and use what you have.
- Source files, `Makefile` and the library's `.md` files use CRLF line endings;
  shell scripts use LF. Comments in `top_api/`, `examples/`, `board/` and the
  guides in `docs/` are in Persian; code and identifiers are English.

**What is established at `6fe596d`** (the maintainers ran it; running it again
will find nothing):

| Check | Result |
|---|---|
| `make test` | `Results: 635/635 passed, 0 failures` |
| `make check` | exit status 0 (the suite, the suite under ASan + UBSan, the ports, the FatFS mock, the small host tests, the `tools/check_*.sh` scripts, CMake 13 configurations, 32-bit) |
| `make m32`, `make m32 M32=ubsan` | 636/636 both (clang of ziglang 0.17.1, `x86-linux-musl`) |
| `make tsan` (pthread port) | 640/640, 0 ThreadSanitizer warnings |
| `make matrix-quick` | 8 configurations, 0 failed |
| `make matrix` | 57 configurations: 55 passed; `TEXT=0` failed to build before the guard fix (fixed and rerun: 629/629); `FM_FILE_FORCE_BOUNCE THREADS=1` failed once (639/640, concurrent media CRC mismatch at step 504), not reproduced in 25 serial reruns; still open |
| `make fatfs` (mock FatFS) | `test/fatfs_host_test.c`: 48 checks in each of three builds (classic, modern, legacy API), 53 with two drives; a compile with `_FS_LOCK = 2` must stop at the adapter's `#error` |
| `make audiorecordtest`, `make saveexamplestest` | 99 and 85 checks pass |
| `make hwjpegtest`, `make videohooks`, `make usbmsctest` | 73, 22 and 48 checks pass (`usbmsctest` was added with the latest fixes: the mass-storage glue `top_api/usb_msc.h` on a RAM disk, under ASan) |

**What is not established** (nobody did it at this commit):

- **the firmware files changed in this round were not compiled**: no ARM
  compiler was available. That is `Core/Src/main.c`,
  `USB_DEVICE/Target/usbd_conf.c`, `FATFS/Target/ffconf.h` (all three in
  `Clain_FileManager/`, outside the library root) and the new
  `UsbMsc_Lba0Probe` in `top_api/usb_msc.h`; `tools/check_board.sh` only
  reads the settings back as text;
- the compile for Cortex-M7 (clang with ST HAL headers; 0 warnings at
  `48a00bc`) was not repeated at this commit;
- nothing ran on a board, on a real SD card, or was built with
  `arm-none-eabi-gcc`; the last CubeIDE log in the repository (`Report.txt`,
  commit `b6bcc4e`) ends in a link error that commit `459ed7d` was written to
  remove, and no later log is recorded;
- `make matrix-asan`, and `make tsan` through the FreeRTOS and CMSIS-RTOS2
  ports, were not run at this commit (last at or before `a6cf829`);
- no fuzzing since `a6cf829` (and the newest code of that time had not been
  fuzzed either);
- no check of a file written by this library's encoders with an external tool
  (ffmpeg, VLC, `flac -t`, a JPEG decoder other than the library's own) is
  recorded;
- no new comparison of decoder output with ffmpeg since `a6cf829`: the suite
  replays the vectors stored in `test/vectors/` and nothing else.

## 4. What happened before, what changed, and your three jobs

Read the first report of this part first (`ReportV2.txt`, lines 1583–2454),
including its sections "Examined and found sound" and "Not examined". Then the
two diffs that matter:

```bash
# the fixes of the first round and the JPEG work after it (never checked from outside)
git log --oneline 8dd0f25..a6cf829
git diff 8dd0f25..a6cf829 -- src/fm_image.c src/fm_png.c src/fm_jpeg.c src/fm_hw.c \
    third_party/picojpeg include/fm_image.h
# what is new since the follow-up prompt
git diff a6cf829..48a00bc -- src/fm_jpeg.c src/fm_internal.h include/fm_image.h \
    third_party/picojpeg top_api/simple_image.h board/app_display.h
```

**The lesson of this project.** After the first audit round the maintainers,
while measuring stack frames, decoded files written by ffmpeg with the library
next to ffmpeg's own decode. That comparison, which no audit and no test had
made, found: an MP3 track that decoded one frame and then failed for ever; IMA
and Microsoft ADPCM tracks that decoded to noise; Vorbis tracks that never
decoded; H.264 pictures 25 dB from the reference (wrong colour range); JPEG
green wrong in saturated colours; Motion JPEG in an `.mp4` not playable. All of
it had passed the whole suite under every sanitizer, because the suite checked
that a format is *recognised* and that nothing *crashes*, not that what comes
out is *right* (fixes: `7d4d009`, `b3525f7`, `2a45d9d`, then
`git log 2a45d9d..a6cf829`; tests in `test/audit_v3_tests.h`). The same thing
happened again with the save code: its tests read the written files back with
the library's own readers, which proves that the two agree and not that either
is right (tests that check the files byte by byte came only with `48a00bc`).
Draw the conclusion for your part:
**wherever the library produces data, compare that data with a reference
implementation, before anything else**, and judge every test by one question:
would it fail if the feature were broken in a plausible way?
For your part the references are libjpeg (or libjpeg-turbo), libpng and
Pillow: every decoded pixel against theirs, and every file this library
**writes** decoded by them.

### What round V5 found, and what was done with it

The report of that round is in the repository: `ReportV4.txt`, lines 268–561 (four of the five reports are
in that file; the harnesses the auditors wrote are not in the repository). It confirmed **no defect** and
raised two suspicions without evidence. The maintainers settled both, and neither is a defect:

| ID | What the report suspected | What the maintainers found | Test |
|---|---|---|---|
| IMG5-S1 | the JPEG encoder's quantiser (in `jpeg_write_block`, `src/fm_jpeg.c`) rounds with a bias | it is the formula of libjpeg-turbo's `quantize()` (`jcdctmgr.c`, release 2.1.5): make the coefficient positive, add half the step, divide, restore the sign. Round half away from zero, symmetric about zero: no bias. No code change | none added |
| IMG5-S2 | the bilinear scaler (`src/fm_image.c`, the `FM_SCALE_BILINEAR` path) weights the edge pixels wrongly | compared with a floating-point centre-to-centre definition on a 37×29 noise picture at 8 target sizes: worst difference 1.88 grey levels (fixed-point rounding). A ramp enlarged 3 times never steps down. No code change; `README.md` and `include/fm_image.h` now say that bilinear has no pre-filter, so shrinking by more than 2 skips source pixels | `IMG5-S2 …` (two tests, suite `test_audit_v5_image` in `test/audit_v5_tests.h`). Mutations: tolerance 2.5 → 0 fails (so the comparison is real); a wrong vertical weight in the scaler (`vy` used without its half-pixel offset) fails at 127 levels |

```bash
git diff fc31b25..6fe596d -- src/fm_image.c src/fm_jpeg.c src/fm_png.c include/fm_image.h top_api/hw_jpeg.h   # the comment in fm_image.h is the whole diff
```

What that round leaves for you (questions, not findings):

1. A round with no finding is either the end of the cycle or a round that did not dig. The V5 report compared no decoder output with libjpeg / libpng on files of its own and ran no fuzzer; its two suspicions were stated without a failing input. Jobs 2 and 3 below were, in effect, not done. Do them.
2. The reference of the IMG5-S2 test is the maintainers' own floating-point model, not another library. Is "centre-to-centre" what a user expects against Pillow / OpenCV (`INTER_LINEAR`) / ffmpeg `scale=flags=bilinear` on the same picture, when enlarging and when shrinking by less than 2?
3. The nearest-neighbour path, the RGB565 targets of `Image_Scale` / `Image_ScaleEx` and the scaled video path (`Video_DecodeFrameScaled`) have no such comparison.
4. IMG4-S1 of the round before is still open (see the row in Job 1).


### What round V4 found, and what the fixes claim

The report of that round is in the repository: `ReportV3.txt`, lines 642–1272 (the five reports are in one
file; the harnesses the auditors wrote are not in the repository). Read it, then
the diff of the fixes:

```bash
git diff 48a00bc..fc31b25 -- src/fm_image.c top_api/hw_jpeg.h top_api/hw_jpeg_video.h tools/halstub test/hw_jpeg_host_test.c test/video_hooks_test.c
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. The tests are in
`test/audit_v4_tests.h` unless the row says otherwise; remove the fix locally
and see whether the test fails.

| ID | What was wrong | What the fix claims |
|---|---|---|
| IMG4-01 | the hardware-JPEG glue (`top_api/hw_jpeg.h`) had no 4:4:4 branch and read grayscale output linearly: wrong pictures returned as success | the MCU output is read block by block for every layout (`si_jpeg_mcus_to_rgb`); the layout comes from the SOF header of the file (`si_jpeg_file_layout`), progressive / CMYK / 12-bit are refused with `FM_ERR_UNSUPPORTED`; the MCU buffer is sized from the header and a picture that does not fit is refused with `FM_ERR_TOO_LARGE` before the peripheral starts. `make hwjpegtest` (73 checks, a software model of the peripheral) |
| IMG4-02 | `FM_SCALE_BILINEAR` was nearest-neighbour | `image_scale_run` (`src/fm_image.c`) interpolates; `SimpleImage_Resize`, `Video_DecodeFrameScaled`, `Image_ResizeFile` inherit it. `jpeg_blit_scaled` stays nearest by design. Tests `IMG4-02 …` in `test/audit_v4_tests.h` |
| IMG4-S2 | `HAL_JPEG_GetInfo` failing made the glue assume 4:2:0 | the layout no longer comes from `HAL_JPEG_GetInfo` at all (see IMG4-01) |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. The peripheral in `make hwjpegtest` is a model written by the maintainers from the reference manual, and the constants in `tools/halstub/stm32h7xx_hal.h` were set by hand: there is no ST HAL header in the repository to compare with. Is the MCU order of the model (4:2:0, 4:2:2, 4:4:4, gray; partial MCUs at the right and bottom edge) what the real codec writes? Compare with the ST HAL utility `JPEG_GetDecodeColorConvertFunc` if you can get it.
2. `top_api/hw_jpeg.h` says that `Image_Load` falls back to the software decoder when the hook returns `FM_ERR_TOO_LARGE` or `FM_ERR_UNSUPPORTED`. Is that true for every caller of the hook (`src/fm_jpeg.c`, the hardware section; the video path `top_api/hw_jpeg_video.h`)? Nobody traced it.
3. Nobody compared the new bilinear output with a reference (Pillow `BILINEAR`, with and without `reducing_gap`): the edges, a 1-pixel-wide source, reduction by more than 2 (no pre-filter: aliasing), the RGB565 path, rounding.
4. IMG4-S1 (the JPEG work buffer stays claimed across `hw->decode()`) was not worked on.
5. The last sections of the V4 image report (`ReportV3.txt`, lines 1152–1272: the tests that would not notice, what was not examined, the scores) were not turned into work by the maintainers. Read them as leads.


### Job 1 — check the fixes (do this first; it is the cheapest way to find a defect)

For every earlier finding below, decide: fixed, partly fixed, not fixed, or
fixed but something else broke. A fix that handles exactly the reported input
and not its neighbours is "partly fixed": vary the input (other sizes, other
pixel formats, other build switches, 32 bits, threads on, the same mistake in
the sibling function). Read the test that was added for it
(`test/audit_v2_tests.h`), then remove the fix locally and see whether that
test fails.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| IMG-01 | PNG encoder declared a short `IDAT` for many image sizes | chunk length correct for every size; output read back by a reference decoder |
| IMG-02 | `Image_GetInfo` under-reported channels for 1-channel PNGs | channels, bit depth and data size agree with what the decoder produces |
| IMG-03 | 8-bit paletted BMP: palette ignored, indices returned as grey | palette applied (`FM_ImageDecoder` grew by 768 bytes for it — check what that does to the stack of every caller that has one as a local) |
| IMG-S1 | hardware-JPEG path keeps the codec scratch claimed across `hw->decode()` | **not changed** (needs hardware); still open |
| IMG-S2 | software re-decode hides the hardware codec's reason | **not changed**, documented |
| IMG-S3 | `Image_GetInfo` validates only the PNG header | judged not a defect (a probe is a probe); say if you disagree, with an input |
| IMG-S4 | `width × height × bpp` on a 32-bit `size_t` | rejected by reading the code, never run. **A 32-bit build exists (`make m32`): settle this one by experiment** |

### Job 2 — audit what is new

Code written since the last audit has been through the fewest eyes. Two layers
of it are yours.

**(a) Written before `a6cf829`, never read from outside**
(`git diff 8dd0f25..a6cf829`): the PNG encoder's size arithmetic; the BMP
palette path (palettes shorter than 256 entries, `biClrUsed` larger than the
file, 1-bit and 4-bit files, RLE with a palette, indices beyond the palette);
`Image_GetInfo` for every format against what a full decode reports; the
result logging added to `Image_LoadEx`; and the **local patches to
`third_party/picojpeg/picojpeg.c`** (search for "FileManager"):

- the green channel used to be computed in two clamped steps and is now
  computed in one, with the Cb term of each block kept in `gCbG[64]` until the
  Cr block arrives;
- layouts picojpeg did not have: chroma sampled 1x2 with luma 1x2 or 2x2 (what
  ffmpeg writes for 4:4:4 and 4:2:2; `gChromaV2`, `gMCUOrg` grown to 8 entries,
  `gCbG` used in two halves), and one-component files with sampling factors
  other than 1x1.

`third_party/README.md` lists every change made to the vendored sources. Check
every layout that is accepted now (one component; luma 1x1, 2x1, 1x2, 2x2 with
chroma 1x1; luma 1x2, 2x2 with chroma 1x2), odd widths and heights in each
(the right and bottom edge of the last MCU), restart markers, a file cut
between the Cb and the Cr block, and that every other layout is refused rather
than decoded wrongly. Compare whole pictures with libjpeg. The maintainers'
figures (`test_jpeg_layouts` in `test/audit_v3_tests.h`, vectors from
`tools/gen_jpeg_vectors.py`): 37 dB before the green patch, 50 to 53 dB after;
odd sizes 48.7 to 54 dB against ffmpeg's decoded planes. The reduced-size mode
of picojpeg (`reduce = 1`) did not get the new branch; the library does not
use it — check that it really cannot be reached.

**(b) Written since `a6cf829`:**

- **The JPEG encoder was rewritten** (`4cbd8a0`, `523d2da`; `src/fm_jpeg.c`
  from `jpeg_huff_build` to the end, about 880 changed lines; `jpeg_fdct`,
  `jpeg_row_ycc`, `jpeg_encode`, `jpeg_encode_common`). It used to write 4:4:4
  at a fixed quality of about 70. Now: quality 1..100 on the libjpeg scale,
  4:4:4 or 4:2:0, 16-bit pixels in either byte order (`swap16`), and three
  outputs — memory (`Image_EncodeJPEG`, `Image_EncodeJPEGEx` with
  `FM_JpegEncodeOptions`), a file (`FM_JPEG_EncodeFile`, used by `Image_Save`)
  and a sink function (`FM_JPEG_EncodeSink` in `src/fm_internal.h`, used by the
  video writer), through a 256-byte buffer (`JPEG_OUT_BUF`).
  `Image_EncodeJPEG` and `Image_Save` now use `FM_JPEG_QUALITY` (85): the
  output of every existing caller changed.
  The Save auditor looks at this encoder as the source of video frames. **You
  own it as an image encoder**; nobody has compared its output with a
  reference decoder (its tests, the four "JPEG: ..." tests at the top of
  `test/video_save_tests.h`, decode with this library's own decoder). Do:
  - decode its output with libjpeg and compare with the source pixels: PSNR
    per quality step against what libjpeg's own encoder reaches at the same
    quality and layout; the quantisation tables against libjpeg's for the same
    quality; the colour conversion (JFIF full-range BT.601 — the decoders of
    this library had the wrong range and matrix once, check the encoder the
    same way: pure red, green, blue, white, black, mid grey, each must come
    back within what the quality allows);
  - every pixel format (`FM_PIXEL_*`), `swap16` on and off for the two 16-bit
    formats and ignored for the others;
  - sizes that are not multiples of 8 and of 16 (edge replication, in 4:2:0
    both directions), 1 × 1, 1 × N, N × 1, the largest width and height the
    types and limits admit; stride not equal to width × bytes per pixel;
  - quality 0 (means the default), 1, 100, and values above 100; a structure
    that was not cleared (the header says "clear it first": what does garbage
    in `subsample` or `swap16` do?);
  - the three outputs must give the same bytes; an output buffer one byte too
    small, a sink or a file write that fails at every call: the error comes
    back, nothing is written past the buffer, `out_len` is right;
  - a picture of noise at quality 100: Huffman codes of 16 bits, coefficient
    values at the limits of the standard tables (can a DC difference or an AC
    value exceed what the tables can code?), byte stuffing after every `0xFF`,
    the final partial byte;
  - the header claims the output suits the STM32 JPEG peripheral: check the
    markers and tables against what that peripheral accepts;
  - the round trip through **this library's** decoder: 4:2:0 written here is
    luma 2x2 with chroma 1x1, a layout picojpeg decodes — compare the round
    trip with the libjpeg decode of the same file.
- **picojpeg**: three statements of the form `*p++ = f(p[0], x)` were split in
  two (unsequenced read and increment). The decoded output must be identical
  to what `a6cf829` produced for every vector: decode everything in
  `test/vectors/` with both commits and compare byte for byte.
- **`SimpleImage`** (`top_api/simple_image.h`, 792 lines; `5d66e46`): the
  slideshow period now survives `Stop` / `Begin`. Check the rest of its state
  the same way (format, output size, present function, effect, rotation),
  `SimpleImage_Save` (does the saved file have the right colours when
  `SIMPLE_IMAGE_SWAP16` is 1, the default?), `SimpleImage_Resize` /
  `Rotate` / `Effect` on the frame buffer, and `board/app_display.h`, which is
  new and has only ever been compiled.
- **`FM_USE_IMAGE=0`** and the JPEG / PNG switches: the public API still links
  and answers `FM_ERR_UNSUPPORTED` (`da04bd2`); the new encoder entry points
  have stubs at the end of `src/fm_jpeg.c`. Check that each stub exists, has
  the same signature and writes its output parameters.

### Job 3 — go where nobody has been

The first auditor of this part could not build for 32 bits, did not run the
threaded configuration, and compared the JPEG encoder only loosely. All size
arithmetic in this part was "re-derived by hand for 32 bits": redo it by
running. Then the image codecs under `FM_USE_THREADS=1` with ThreadSanitizer
(several tasks loading at once, a nested load from a callback), and the
streaming decode API, which the first report covers least. The first round
fuzzed the three decoders with its own structure-aware mutators (about 1.2 M
iterations in all, without coverage guidance); nothing has been fuzzed since,
and the decoders changed since (the BMP palette path, the picojpeg patches).
A coverage-guided campaign (libFuzzer, AFL++) has never been run on this part:
`src/fm_png.c`, hand-written with its own inflate, is the first target.

### Open leads — an answer to each is wanted in the report

- **W1. Byte order of 16-bit pixels, end to end.** Frame buffers for SPI
  displays hold RGB565 high byte first (`SIMPLE_IMAGE_SWAP16`,
  `SIMPLE_VIDEO_SWAP16`, `SimpleVideo_SetSwap16`); the image API works on
  `uint16_t` in CPU order. Follow one red pixel from a decoded file through
  `Image_Convert`, scaling, the effect functions, the frame buffer, the display
  driver (`top_api/spi_tft_dma.h`, `ltdc_present.h`, `dma2d_ycbcr.h`) and back
  into a saved file (`SimpleImage_Save`, `SimpleVideo_SaveFrame`,
  `Image_EncodeJPEGEx` with `swap16`). Is there a path where the bytes are
  swapped twice or not at all, without an error?
- **W2. Stack of the encoder and decoders on ARM.** `JpegOut` now carries a
  256-byte buffer, `FM_ImageDecoder` grew by 768 bytes, and the encoder is
  called from inside the video player's frame callback. Measure the frames for
  Cortex-M7 (section 3) and the deepest chain that reaches them.
- **W3. Encode time.** The encoder is plain C and nobody measured it. Estimate
  the time for 320 × 240 and 480 × 272 on the STM32H743 (instruction
  counts of a cross-compiled build, with your reasoning), for 4:4:4 and 4:2:0.
- **W4. Tests of the encoder.** They decode with the library's own decoder and
  use flat or smooth pictures. Break the encoder in plausible ways (swap Cb and
  Cr, shift a block by one, wrong edge replication, wrong table for chroma) and
  see which of them the tests notice.

**Methods.** What section 3 lists as established will find nothing if you run
it again. These are the methods most likely to pay off now:

- **Differential testing** against a reference implementation, on files a real
  tool wrote. This is the method that found the most serious defects of this
  project so far (see "The lesson of this project" above).
- **Fuzzing** of everything that takes file bytes, with ASan + UBSan (libFuzzer
  or AFL++), seeded from `test/vectors/` and from files you generate. Say how
  many executions and what coverage you reached; without those two numbers a
  fuzzing claim is worth nothing.
- **Fault injection**: a read or write that fails in the middle, a short read,
  a volume that fills up, an allocation that fails (every Nth, for every N), a
  file truncated at every byte offset. After each failure check three things:
  the result code, that everything taken was given back (handles, slots,
  allocator blocks: loop the failing call 1000 times), and that the next valid
  call still works.
- **32-bit harnesses** (section 3): size arithmetic near 2^31 and 2^32 is to be
  settled by running, not by reading. Build your fuzz and fault-injection
  harnesses for 32 bits too.
- **Mutation of the code under test**: for every earlier fix, and for every
  area where you rely on an existing test, break the code in a plausible way
  (invert a condition, drop a line, shift by one) and run the tests. A break
  that no test notices is a gap in the tests; list it (section 8, "Tests that
  would not notice"). The maintainers found one such gap this way at this
  commit (in `top_api/examples_save_media.h`, function `save_demo_video_cut`,
  the condition `if (snd_at > pic_at)` can be forced to false and
  `make saveexamplestest` still passes); there are certainly more.
- **Static analysis** (`gcc -fanalyzer`, clang static analyzer, cppcheck),
  triaged: report a warning only after you have shown it is real.
- **Stack measurement** (`-fstack-usage` on the host, `-fstack-size-section`
  for the ARM target, see section 3): frames above about 1 KB in a call path
  the application reaches, and the depth of the deepest call chain.
- **Careful reading** for: integer overflow in size arithmetic, lengths taken
  from a file and used unchecked, state left half-changed when a function
  returns an error, handles or slots not released on error paths, loops whose
  exit depends on file content, code that runs in an interrupt and touches what
  the main loop touches, behaviour that differs from what the header comment,
  `README.md` or `docs/GUIDE_FA.md` promises.

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

New in this round (Job 2 (b)): the encoder half of `src/fm_jpeg.c` with
`Image_EncodeJPEGEx` / `FM_JpegEncodeOptions` in `include/fm_image.h` and the
sink in `src/fm_internal.h`; `top_api/simple_image.h` (the twin of
`examples/simple_image.h`; the firmware uses the `top_api/` copy) and
`board/app_display.h`. Tests to add to your reading list: the four "JPEG: ..."
tests at the top of `test/video_save_tests.h`, and `test_jpeg_layouts` in
`test/audit_v3_tests.h`.

### Your part first; its margins when your evaluation needs them

Spend most of your time (about four fifths) inside the scope above. You are
allowed, and expected, to go into another part when your evaluation needs it:

- a call chain that starts in your part ends there, or the reverse, and the
  question is whether the two sides fit: what one side assumes and the other
  does not guarantee;
- you found a mistake in your part and the same pattern is likely in a sibling
  elsewhere: check the sibling (one finding, all places);
- you need the other part to build a reproducer or to judge severity (who
  calls this, with which values, from which context, how often);
- a test or a document of your part depends on behaviour of another part.

What you find there is held to the same standard as everything else: a defect
outside your part that you **confirmed** gets a full finding section (place,
proof, fix) and names the part that owns it. Only what you noticed and did not
pursue goes under "Outside my part", one line each with `file:line`. Overlap
between auditors is intended: a defect that two reports find independently is
confirmed twice.

What you should not do is audit another part for its own sake: no fuzzing
campaign of somebody else's parser, no line-by-line reading of somebody else's
file.

The margins that matter most for this part:

| Margin | Owner | Why it concerns you |
|---|---|---|
| `src/fm_video_enc.c` (`vw_write_frame`, `VideoFileWriter_WriteFrame`, `WriteFrameSwap16`) | Save | the caller of `FM_JPEG_EncodeSink`: which options it passes, what it does when the sink reports an error half way through a picture |
| `src/fm_mjpeg.c`, `src/fm_video.c` (MJPEG frames) | Video | the same picojpeg decodes video frames: a state left over from a failed still-image decode, the guard that keeps two decodes apart (picojpeg has global state) |
| `top_api/simple_video.h` (`SimpleVideo_SaveFrame`, the frame buffer and its swap option) | Video | lead W1 |
| `top_api/spi_tft_dma.h`, `top_api/ltdc_present.h`, `top_api/dma2d_ycbcr.h`, `top_api/hw_jpeg.h`, `top_api/hw_jpeg_video.h` | Image / Video | what the display and the hardware codec expect from the pixel data: byte order, alignment, DMA length |
| `src/fm_file.c` (reads) | Files | what the decoders do on a short read, a read error, a file that changes size |
| `src/fm_player.c`, `board/app_flow.h` (slideshow) | Audio / Save | the slideshow runs while music plays: a picture decode calls `FM_Idle()` so that sound does not break — what may run inside that hook while the decoder holds its work buffer? |

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

**Debug levels and log output (new since the first audit, never checked from outside).** The library prints
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

**The JPEG encoder (added before the preceding round)** — see Job 2 (b); it is the newest and
least examined code of this part. Add a differential harness that encodes
random and natural pictures at random options and decodes them with libjpeg;
run it for 32 bits too.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- PNG output is RGB888 only.
- JPEG layouts outside the list in `README.md` are refused: an MCU of more
  than 8 blocks does not fit picojpeg (luma 4x1, chroma 2x1 and the like), and
  so do four-component (CMYK) files.
- JPEG decoding is baseline sequential only (limit of the vendored picojpeg),
  and picojpeg is not thread-safe — how the library guards it with
  `FM_USE_THREADS=1` is in scope.
- The hardware JPEG path needs the whole file in memory.
- Formats that media detection recognises but the library does not decode (GIF,
  TIFF, WebP) are refused; that is by design.

- IMG-S1 and IMG-S2 of the first audit (hardware JPEG path: scratch kept
  claimed across `hw->decode()`; software fallback hides the codec's reason)
  are known and unchanged.
- BMP reading: see the BMP paragraph of `README.md` for the variants that are
  supported; a variant outside that list being refused is not a defect.
- Everything the first audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_IMAGE_V6.md`. It is used as a work list:
each finding is opened by someone who has not seen your session and has only
the report and a clean checkout of `6fe596d`. Use this structure, in this
order, with these headings:

1. **Summary in Persian** (8 to 12 lines): how many confirmed defects by
   severity, the single most important one, how many of the earlier findings
   are really fixed, your answers to the open leads in one line, your scores, what you could not examine,
   and whether you expect another round on this part to find more. The rest of
   the report is in English.
2. **Audit record**
   - the commit audited (full hash from `git rev-parse HEAD`) and the date;
   - machine, operating system, compilers and their versions; the tools you
     had, and the tools this prompt names that you did not have (with what you
     used in their place);
   - the result line of `make test` on your machine before you changed
     anything (expected: `Results: 635/635 passed, 0 failures`; if yours
     differs, that comes first in the report);
   - a table of what you did: method, target, amount (executions, files,
     cases, hours), coverage reached, result;
   - what you ran as a 32-bit program;
   - your local changes: every file you added or changed in your clone, one
     line each.
3. **Table of findings**: ID, severity, status (confirmed / suspicion), owning
   part, `file:line`, one-line title, fix tested (yes / no), regression test
   included (yes / no). Sorted by severity.
4. **One section per confirmed finding**, IDs `IMG6-01`, `IMG6-02`, ...:
   - **Severity** — S1: memory corruption, crash, hang, loss or corruption of
     stored data. S2: wrong result returned as success (wrong pixels, samples,
     bytes, sizes, timestamps; a recording that lost or misplaced data). S3:
     wrong error handling — a leaked handle or slot, a wrong error code, state
     not restored after a failure. S4: mismatch with the documentation,
     portability, wasted time or memory, a test that cannot fail.
   - **Where** — `path:from-to`, function, at `6fe596d`, and the lines
     themselves (quote at most 15).
   - **Reach** — which public function leads there, which action of a user of
     the board (a UART command, a file on the card, a sequence of calls), which
     build configurations, 32 bits / 64 bits / both, and how likely it is with
     ordinary files and ordinary use.
   - **How to reproduce** — from a clean checkout of `6fe596d`: the files to
     add (full source in the appendix), the exact commands, and the output as
     it was printed (sanitizer report, wrong value, hang with the time you
     waited). For an input file: a hex dump if it is under 200 bytes, otherwise
     the script that generates it.
   - **Expected and observed.**
   - **Root cause** — why it happens, not just where.
   - **Proposed fix** — a unified diff against `6fe596d` in a `diff` block.
     Produce it with `git diff`, do not type it: the sources have CRLF line
     endings and a typed diff will not apply. Then state: applied (yes / no),
     the reproducer after the fix, the result line of `make test` after the
     fix, and anything else you ran with it (`make asan`, `make m32`, the
     smaller host tests).
   - **Regression test** — the test to add (which file, the code), with its
     result on `6fe596d` (must fail) and with your fix (must pass). If a test
     is not possible on a PC, say why.
   - **Siblings** — where else you looked for the same mistake (other formats,
     other backends, the twin in `examples/`, the other player) and what you
     found there.
   - **Since** (optional) — the commit that introduced it, when `git blame` or
     `git log -S` shows it quickly.
5. **Suspicions** — the same format without a reproducer: what you believe,
   why, what stopped you, and the experiment that would settle it.
6. **Verdict on the earlier findings** — one table: earlier ID, your verdict
   (fixed / partly fixed / not fixed / fixed but broke something), how you
   checked (which variants you tried), and whether the test that was added for
   it fails when the fix is removed (tried / not tried).
7. **Answers to the open leads** — one table: every lead of section 4, your
   answer (confirmed defect → finding ID / not a defect / measured: the value
   / could not settle), and how you checked.
8. **Tests that would not notice** — one table: the change you made to the
   code (`file:line`, what), the test you expected to fail, what happened. List
   the changes that were caught too; the ratio is the information.
9. **Examined and found sound** — each area you covered and how, with numbers
   (for example "BMP decoder: 6 h of libFuzzer, 41 M executions, 92 % line
   coverage of the decoder, no findings"). This section matters as much as the
   findings: it tells us what an empty result is worth.
10. **Not examined** — everything in your scope you did not get to, and every
    method of section 4 you did not use.
11. **Outside my part** — what you noticed in other parts and did not pursue:
    one line each, with `file:line` and the owning part. (What you did pursue
    and confirmed is a normal finding in section 4 of the report.)
12. **Scores** — so that this round can be compared with the last one and with
    the next. Give each line a whole number from 0 to 10 and one sentence that
    names the evidence. Write "not assessed" instead of guessing; a score
    without evidence behind it is worth nothing.

    | Axis | What 10 means |
    |---|---|
    | A. Malformed input | no crash, hang or out-of-bounds access on any input you could construct, after real fuzzing |
    | B. Correct results | every valid input you tried gives the reference result |
    | C. Errors and resources | every failure returns the right code, releases what it took and leaves a usable state |
    | D. Memory and timing | stack frames, allocation sizes and worst-case call times fit a microcontroller and are bounded |
    | E. Documentation | headers, `README.md` and the guides say what the code does, including the limits |
    | F. Tests | the existing tests of this part would catch a regression in what they cover, and cover what matters |
    | G. Diagnosability | when something fails, the result code and the log line together tell a user why |
    Then one line: **ready for a product / ready under these conditions (list
    them) / not ready**, for your part only. And a second line: the **three
    things to fix first**, in order.
13. **Where the next round should look** — at most five places, ordered by how
    much you expect them to yield, each with the reason and the method. Then
    your estimate, with its reasoning, of what is still in this part: is
    another S1 or S2 defect likely, and where.
14. **Findings index** — a fenced block with one line per finding and
    suspicion, in exactly this form, so that the five reports can be merged:

    ```
    ID | severity | confirmed or suspicion | owning part | path:line | fix tested yes/no | test included yes/no | title
    ```

**Appendix** — harness sources, generator scripts, crashing inputs, changed
library files as diffs (or attach them as files next to the report). Someone
must be able to reproduce every confirmed finding from the report alone.

**Before you deliver, check your own report** (and say in the audit record
that you did):

- Make a fresh clone at `6fe596d` and re-run every reproducer from the text of
  the report alone. Drop or downgrade what does not reproduce.
- Run `git apply --check` for every diff on that clean tree.
- Re-check every `file:line` against `git show 6fe596d:<path>`.
- The numbers in the Persian summary, in the table of findings and in the
  findings index agree.
- No finding rests on a documented limit, and no "confirmed" finding contains
  "may", "might" or "could" in its observed behaviour.
- Every number in the report has the command that produced it.
