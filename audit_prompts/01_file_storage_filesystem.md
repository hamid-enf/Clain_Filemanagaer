# External audit V5 — files, storage, filesystems, build configuration

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
- Commit: `fc31b25` (`fc31b259c8345180a0ca2297c802c8e9f4b7dcdc`). Audit exactly
  this commit (`git checkout fc31b25`) and quote it in your report. Later
  commits on the branch change only the folder `audit_prompts/`.
- Library root: `Clain_FileManager/FileManager_lib/`. Paths below are relative to
  it, except: paths that start with `Core/` are in `Clain_FileManager/Core/`
  (the CubeMX firmware project of the reference board), and `ReportV2.txt`,
  `Report.txt` and `audit_prompts/` are at the repository root.

History of this part:

| Round | Commit audited | Report | Then |
|---|---|---|---|
| first | `8dd0f25` | `ReportV2.txt`, lines 1–1087 | the findings were worked on, up to `a6cf829` |
| follow-up ("V3") | `a6cf829` | none in the repository | the prompt was written (`git show f161ff8:audit_prompts/01_file_storage_filesystem.md`); no report of it is in the repository, so **nobody outside has checked the fixes of the first round**. Its jobs are carried into this round |
| V4 | `48a00bc` | `ReportV3.txt`, lines 1–641 | five findings (FILE4-01 … -05), all fixed in `fc31b25` |
| this one (V5) | `fc31b25` | `AUDIT_FILE_V5.md` | |

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

Your part is **Files**: the file API, the filesystems, storage and drivers, the
utilities, the log layer and the build configuration. Section 5 says exactly
which files, and how far you may go into the other parts.

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
   `git show fc31b25:<path> | sed -n '<from>,<to>p'` before you write it down:
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
make test                 # whole suite: 589 tests at this commit
make asan                 # the same suite under ASan + UBSan (+ LeakSanitizer)
make tsan                 # the suite with FM_USE_THREADS=1 under ThreadSanitizer (594 tests)
                          #   make tsan FM_SYNC_IMPL=freertos | cmsis_rtos2  goes through that port
make m32                  # the suite built and run as a 32-bit x86 program (590 tests);
make m32 M32=ubsan        #   needs:  pip install ziglang
make cachetest videohooks audiorecordtest saveexamplestest tapstest fatfs
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

**What is established at `fc31b25`** (the maintainers ran it; running it again
will find nothing):

| Check | Result |
|---|---|
| `make test` | `Results: 589/589 passed, 0 failures` |
| `make check` | exit status 0 (includes the suite under ASan + UBSan, the ports, the FatFS mock, CMake 13 configurations, 32-bit) |
| `make m32`, `make m32 M32=ubsan` | 590/590 both (clang of ziglang 0.17.1, `x86-linux-musl`) |
| `make tsan` (pthread port) | 594/594, 0 ThreadSanitizer warnings |
| `make matrix-quick` | 8 configurations, 0 failed |
| `make matrix` | 57 configurations, 0 failed (run alone, in a copy of the tree; one build at a time per tree: a second build in the same tree mixes object files). New since the last round: `FM_BUFFERS=SMALL` / `LARGE` / `HUGE` and both `..._ENC_SYNC_BYTES=0` |
| `make audiorecordtest`, `make saveexamplestest` | 99 and 74 checks pass |
| `make hwjpegtest`, `make videohooks` | 73 checks and 22 checks pass (new in this round: the hardware-JPEG glue against a software model of the peripheral) |

**What is not established** (nobody did it at this commit):

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

Read the first report of this part first (`ReportV2.txt`, lines 1–1087),
including its sections "Examined and found sound" and "Not examined". Then the
two diffs that matter:

```bash
# the fixes of the first round (never checked from outside)
git log --oneline 8dd0f25..a6cf829
git diff 8dd0f25..a6cf829 -- src/fm_file.c src/fm_directory.c src/fm_text.c src/fm_stream.c \
    src/fm_safe.c src/fm_logger.c src/fm_log.c src/fm_alloc.c src/fm_filesystem.c backends include
# what is new since the follow-up prompt
git log --oneline a6cf829..48a00bc
git diff --stat a6cf829..48a00bc -- include src/fm_core.c backends Makefile CMakeLists.txt tools \
    top_api board ../Core/Src
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
For your part the reference is another implementation of the same file
semantics: the same sequence of operations on FMFS, LittleFS, FatFS and POSIX
(section 6), and an image written by this library's FatFS / LittleFS adapters
checked with the tools of those filesystems (`fsck.fat`, `mtools`, `littlefs`'s
own `lfs` FUSE or Python tools).

### What round V4 found, and what the fixes claim

The report of that round is in the repository: `ReportV3.txt`, lines 1–641 (the five reports are in one
file; the harnesses the auditors wrote are not in the repository). Read it, then
the diff of the fixes:

```bash
git diff 48a00bc..fc31b25 -- src/fm_fmfs.c src/fm_file.c src/fm_directory.c src/fm_internal.h src/fm_transfer.c \
    include/fm_cache.h include/fm_board_config.h include/fm_file.h backends/fm_littlefs.c tools
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. The tests are in
`test/audit_v4_tests.h` unless the row says otherwise; remove the fix locally
and see whether the test fails.

| ID | What was wrong | What the fix claims |
|---|---|---|
| FILE4-01 | `File_Flush` on an FMFS volume returned `FM_OK` without calling the storage driver's `sync`: data "flushed" before a power cut was lost | `fmfs_file_flush` (`src/fm_fmfs.c`) calls `storage->sync()` and returns its result. Tests `FILE4-01 …` in `test/audit_v4_tests.h` (a RAM disk whose `sync` can be switched off: `av4_power_cut`) |
| FILE4-02 | `#define FM_CACHE on` (lower case, or any word that is not a known value) silently switched the cache off | compile-time check in `include/fm_cache.h`; `tools/check_switch_names.sh` (18 cases) |
| FILE4-03 | delete / rename / copy / move onto a file that is open succeeded and left the open handle on a dead entry | all of them return `FM_ERR_BUSY` while a handle is open (`src/fm_file.c`, `src/fm_internal.h`, `src/fm_directory.c`). **Public behaviour change** |
| FILE4-04 | `File_Delete` on a directory and `Directory_Delete` on a file gave backend-dependent results | `FM_ERR_IS_DIRECTORY` / `FM_ERR_NOT_DIRECTORY` on every backend |
| FILE4-05 | `FM_BUFFERS=SMALL` made the transfer chunk smaller than a flash erase unit | a floor and a log line in `src/fm_transfer.c`; `tools/check_matrix.sh` runs `FM_BUFFERS=SMALL`, `LARGE`, `HUGE` |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. The BUSY rule of FILE4-03 is new and wide. Which internal callers of `File_Delete` / `File_Rename` / `Directory_DeleteRecursive` (`File_SafeReplace`, the transfer layer, the recorders' numbered names, the converters, the media cache) can now meet `FM_ERR_BUSY` on a handle of their own, and what do they do with it? Nobody listed them.
2. How does the rule identify "the same file"? Two spellings of one path (case on FAT, `//`, `/./`, a trailing `/`), two mounts of one volume, a directory that contains an open file being renamed or deleted recursively.
3. Was a mutation run made for FILE4-03, -04, -05? No: only FILE4-01 was checked that way (565/567 with the fix removed). The maintainers suspect that the FILE4-05 test cannot fail.
4. `File_Flush` on FMFS now costs a driver sync. The recorders flush at every checkpoint: what does that cost on a NOR / QSPI volume, and is the cost documented?
5. `FM_BUF_SCALE(base)` multiplies by up to 8 in 32-bit preprocessor arithmetic with no guard; the LittleFS `lookahead` size is not checked for alignment.


### Job 1 — check the fixes (do this first; it is the cheapest way to find a defect)

For every earlier finding below, decide: fixed, partly fixed, not fixed, or
fixed but something else broke. A fix that handles exactly the reported input
and not its neighbours is "partly fixed": vary the input (other sizes, other
backends, other build switches, 32 bits, threads on, the same mistake in the
sibling function). Read the test that was added for it
(`test/audit_v2_tests.h`), then remove the fix locally and see whether that
test fails.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| FILE-01 | `File_Truncate` on a read-only handle aborted (LittleFS assert) | refused with `FM_ERR_ACCESS` on every backend |
| FILE-02 | `File_Append` wrote at a stale size and overwrote data | the size is taken with the mount locked. LittleFS shares nothing between two handles of one file, so there the adapter allows **one writer or many readers**: a conflicting open returns `FM_ERR_BUSY` (`lfs_open_conflict` in `backends/fm_littlefs.c`) |
| FILE-03 | FatFS adapter moved the file position on truncate | position kept |
| FILE-04 | `Text_ReadLine` deleted every CR | only the CR of a CRLF line end is removed |
| FILE-05 | `Stream_InitFile` leaked a pool slot | no leak; a second `Stream_InitFile` on a stream in use is refused with `FM_ERR_BUSY` |
| FILE-06 | POSIX adapter opened a directory as a file | `FM_ERR_IS_DIRECTORY` |
| FILE-07 | `FM_DEBUG_LEVEL=0` did not compile with `-Werror` | compiles; levels 0, 2, 3 and `FM_LOG_DISABLE` are in the build matrix |
| SUSP-01 | recursive directory walks could exhaust the stack | all tree walks in `src/fm_directory.c` are loops, depth limited to `FM_DIR_MAX_DEPTH` (16); a deeper tree returns `FM_ERR_TOO_LARGE` |
| SUSP-02, SUSP-04 | `File_Transaction` / safe replace could lose the original when a step failed | the original stays; when the restore itself fails the backup is kept as `<path>.fm_tmp.bak` |
| SUSP-03 | explorer listing silently cut | `Explorer_IsTruncated()` reports it |
| (own) | the `*` filter matcher was exponential (33 s on a 28-character name) | `wildcard_match` in `src/fm_directory.c` and `wildcard_match_local` in `src/fm_logger.c` are loops |

### Job 2 — audit what is new

Code written since the last audit has been through the fewest eyes. Two layers
of it are yours.

**(a) Written for the first round's findings, never read from outside**
(`git diff 8dd0f25..a6cf829`): the LittleFS open-conflict rule (can it be
bypassed through rename, delete, truncate by path, or `File_Copy` onto an open
file? does a failed open leave the conflict table wrong? what about threads?),
the iterative directory walks (state after a failure half way, a tree that
changes while it is walked, depth exactly 16 and 17 on every backend), the
restore path of `File_Transaction`, the two rewritten wildcard matchers
(compare them with a reference matcher on random patterns and names), and
**the whole log layer**, which was rewritten and is yours: `src/fm_log.c`,
`include/fm_log.h`, `include/fm_debug.h`, `include/fm_log_uart_hal.h`.

**(b) Written since `a6cf829`** (18 commits; `git log a6cf829..48a00bc`).
For this part:

- **The build configuration became a feature of its own**
  (`include/fm_board_config.h`, `include/fm_switches.h`,
  `include/fm_buffers_level.h`, `include/fm_config.h`; commits `08835e0`,
  `459ed7d`, `da04bd2`, `48a00bc`). The promise to the user: edit one file,
  define no `FM_...` symbol in the project, and every combination builds, links
  and answers `FM_ERR_UNSUPPORTED` for what is off. Check it:
  - `FM_BUFFERS` = `SMALL` / `NORMAL` / `LARGE` / `HUGE` becomes
    `FM_BUFFERS_LEVEL` 1..4 and scales the work buffers by 1/4, 1, 4, 8 through
    `FM_BUF_SCALE` (floor 256, rounded down to a multiple of 32). Find every
    macro that goes through it (`grep -n FM_BUF_SCALE include/*.h`) and check
    each scaled value against what its user requires: a multiple of the sector
    size, a DMA length that fits 16 bits (`LCD_SPI_DMA_CHUNK` has a cap for
    that — do the others need one?), a minimum below which the code breaks, an
    `#error` that no longer triggers, a product that overflows 32 bits at ×8.
    Build and run the suite at all four levels (`CFLAGS_EXTRA=-DFM_BUFFERS=...`),
    on 64 and 32 bits.
  - "A value defined in the project wins" (`#ifndef` everywhere, with one
    exception block that states its own rule): find the names where this is not
    true, and the names that are defined in two headers with different
    defaults.
  - `ON`, `OFF`, `SMALL`, `NORMAL`, `LARGE`, `HUGE` are short names that other
    headers may own. `include/fm_buffers_level.h` protects the four size names
    with `push_macro` / `pop_macro`; `tools/check_switch_names.sh` tries 13
    cases. Try the ones it does not: `ON` / `OFF` defined otherwise by an ST or
    RTOS header, a project that defines `FM_BUFFERS` as a number, a compiler
    without `push_macro`, a header order where `fm_switches.h` comes before or
    after `<math.h>` / `<windows.h>`. The comment in `fm_buffers_level.h` says
    some `<math.h>` define `HUGE`; the maintainers reproduced the clash only by
    writing that define by hand and found no toolchain header that has it. If
    you have `arm-none-eabi-gcc` with newlib, settle it.
  - `FM_USE_FATFS` is 1 in a firmware build (`USE_HAL_DRIVER` defined) and 0 on
    a PC, unless the project sets it. That rule was added after a CubeIDE link
    failed with `undefined reference to FatFS_Init` (`Report.txt`). Look for
    the next one of that kind: a switch whose default differs between the
    firmware and the host build, so that the host tests never build what the
    board runs.
  - `FileManager_MountMemory` with `FM_USE_RAMDISK=0` returns
    `FM_ERR_UNSUPPORTED` (`src/fm_core.c`). Check every other "utility" switch
    the same way: off, does the application still link, and is what the
    comment at the top of `include/fm_board_config.h` says about it (the
    example prints "skipped"; a call from the application is undefined at link
    time) true for each?
- **Firmware-build rule** (`641b003`, `tools/check_cubeide.sh`): CubeIDE
  compiles every `.c` under the library folder, so every `.c` outside `src/`,
  `backends/`, `third_party/` and `top_api/` carries the marker
  `FM-NOT-FIRMWARE` and compiles to nothing when `USE_HAL_DRIVER` is defined.
  Check both directions: a file that should be empty and is not (a second
  `main`, `_sbrk`, an include that is not found), and code the firmware needs
  that now compiles to nothing.
- **The `Makefile` flags stamp** (`.fm_build_flags`, `tools/check_flag_stamp.sh`,
  commit `48a00bc`): objects are rebuilt when the flags change. Try what the
  script does not: `make -j`, `CFLAGS_EXTRA` that differs only in quoting or
  order, a switch given through the environment, an interrupted build, GNU
  make older than 4.2 (the stamp then says "flags not tracked": is the user
  told?), and the CMake build, which has no such stamp.
  One limit is known and is not a finding: two builds running in the same
  tree at the same time link a mix of objects (it happened to the maintainers
  during `make matrix`, see the table in section 3; the mixed binary failed a
  threaded PNG test and then crashed).
- **A mechanical rename over the whole user layer** (`dd6239c`): every
  identifier that carried an example number (`Ex13_...`, `EX35_...`) got a
  module name, in 119 files of `top_api/`, `examples/`, `board/`, the tests and
  the documents. A mechanical rename is where a wrong replacement
  hides: two different things that now share a name, a name that now collides
  with a library symbol, a string or a UART command that changed with it, a
  document that names a function that no longer exists. A strong check:
  compile the firmware sources at `dd6239c^` and at `dd6239c` for ARM and
  compare the objects with the symbol names normalised; any other difference
  needs an explanation.
- **The storage drivers of the board** are in that layer and are yours:
  `top_api/qspi_w25q.h`, `qspi_driver.h`, `qspi_transfers.h`,
  `spi_flash_dma_port.h`, `i2c_eeprom_dma.h`, `eeprom_simple.h`, `usb_msc.h`,
  `usb_msc_simple.h`, `usb_sd_presenter.h`, `usb_present.h`, `sdcard_fatfs.c`,
  `sdcard_fm_diskio.c`, `fm_xdma.h`, `sdram_alloc.h`, `cache.h`,
  `media_data_io.h`, and the board files `board/examples_file.h`,
  `board/app_flow.h`, `board/app_uart_commands.h` (host test:
  `tools/board_file_demo_host.c`, `test/sdcard_examples_host.c`).

### Job 3 — go where nobody has been

The first auditor of this part worked with `FM_USE_THREADS=0` only, did not get
to Tier 3 (the drivers) at all, and read `fm_binary.c`, `fm_crc.c`,
`fm_async.c`, `fm_transfer.c`, and half of `fm_json.c`, `fm_logger.c` and
`fm_storage.c` only in outline. No second power-fail campaign was written and
no differential tester across backends was built. Start there: concurrency,
the drivers, those files, and the differential tester of section 6.

### Open leads — an answer to each is wanted in the report

- **W1. What does `File_Flush` guarantee?** The save code (new, another part)
  rests on it: every 256 KB (audio) or 1 MB (video) it patches the header,
  calls `File_Flush` and promises that after a reset the file on the card is
  valid up to that point (`README.md`, "Saving: what is on the card when it
  goes wrong"). The adapters map it to `f_sync`, `lfs_file_sync`, `fsync` and
  `fmfs_file_flush`. For each backend: after `File_Flush` returns and the power
  is cut, are the data **and the size in the directory entry** on the medium?
  Is that written in `include/fm_file.h`? Prove it with the power-fail harness
  (cut after every block write that follows a flush), on FMFS and LittleFS at
  least. The tests of the save code model the reset by dropping the writer and
  calling `File_Close` on its handle (`asv_abandon`, `vsv_abandon` in
  `test/audio_save_tests.h`, `test/video_save_tests.h`) — a close writes the
  directory entry, a reset does not. No test cuts the power at block level.
- **W2. Seek back, overwrite, seek to the end.** The writers patch their
  headers this way at every checkpoint and at `Close`, the AVI writer keeps a
  second file `<name>.idx` open beside the first, and `VideoFile_Repair`
  rewrites a file in place. On LittleFS an overwrite in the middle of a large
  file is copy-on-write: what does it cost in time and in free space, and what
  happens on a nearly full volume when the header patch at `Close` gets "no
  space"? On FatFS without a fast-seek table, what does the seek back and forth
  cost on a 1 GB file?
- **W3. Two handles on one file.** A player reads a file while a writer writes
  the same path; `File_Exists` / `File_SizeOf` are called on a file that is
  open for writing; a file is deleted or renamed while it is being recorded.
  The conflict rule exists for LittleFS only. What does each backend do, and
  does FatFS need `FF_FS_LOCK` for it?
- **W4. The card is taken away under open files.** The board has a UART command
  (`sdpc`) that hands the SD card to the PC over USB, and a card can be pulled.
  What happens to open handles, to the mount, to the read cache, and to the
  next mount? Is anything written to a card the PC is using at that moment?
- **W5. The read cache and DMA** (`FM_CACHE`, `top_api/cache.h`,
  `make cachetest`): the library switches the CPU caches on by itself when
  `FM_CACHE` is `ON` and does the cache maintenance for DMA buffers. Check every
  DMA transfer of the storage drivers for a buffer that is not 32-byte aligned,
  not a multiple of 32 long, or on the stack.

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

This is the largest part, about half of the library. Work through it in this
order and say in the report how far you got.

**Tier 1 — the core (spend most of your time here)**

| File | What it is |
|---|---|
| `src/fm_file.c`, `include/fm_file.h` | file API: open, read, write, seek, truncate, copy, move, edit |
| `src/fm_filesystem.c`, `include/fm_filesystem.h` | VFS: mount table, locking, dispatch to backends |
| `src/fm_directory.c`, `src/fm_path.c` | directories, path normalisation |
| `src/fm_storage.c` | storage registry, block devices |
| `src/fm_fmfs.c` | FMFS, the library's own filesystem (two tables, move journal, power-fail safe layout "v2") |
| `backends/fm_littlefs.c`, `backends/fm_fatfs.c`, `backends/fm_posix_fs.c`, `backends/fm_ramdisk.c` | filesystem adapters |
| `src/fm_core.c`, `include/fm_sync.h`, `ports/*/fm_sync_impl.h` | init, locks, RTOS ports |

**Tier 2 — utilities built on the file API**

`src/fm_stream.c`, `fm_text.c`, `fm_binary.c`, `fm_json.c`, `fm_logger.c`,
`fm_safe.c` (crash-safe writes), `fm_crc.c`, `fm_async.c`, `fm_explorer.c`,
`fm_transfer.c`, `fm_alloc.c`, `fm_log.c`.

**Tier 3 — device drivers (no hardware available: review by reading)**

`src/fm_device.c`, `fm_eeprom.c`, `fm_eeprom_chips.c`, `fm_qspi.c`,
`fm_spiflash.c`, `fm_sd_hal.c`, `fm_usb_msc.c`, and their host tests
(`test_qspi`, `test_spiflash`, `test_eeprom_usb` in `test/main.c`).

**Tier 4 — build configuration and the board's storage layer (new in this round, Job 2 (b))**

| File | What it is |
|---|---|
| `include/fm_board_config.h`, `include/fm_switches.h`, `include/fm_buffers_level.h`, `include/fm_config.h`, `docs/OPTIONS_FA.md` | the one file a user edits, the two flags `FM_CACHE` / `FM_BUFFERS`, the feature switches and size limits |
| `Makefile`, `CMakeLists.txt`, `tools/check_*.sh` | the builds and the checks `make check` runs; a check that cannot fail is a finding |
| `top_api/qspi_*.h`, `spi_flash_dma_port.h`, `i2c_eeprom_dma.h`, `eeprom_simple.h`, `usb_msc*.h`, `usb_sd_presenter.h`, `usb_present.h`, `sdcard_fatfs.c`, `sdcard_fm_diskio.c`, `fm_xdma.h`, `sdram_alloc.h`, `cache.h`, `media_data_io.h` | the storage drivers and glue of the reference board (header-only, ST HAL); each has a twin in `examples/` |
| `board/examples_file.h`, `board/app_flow.h`, `board/app_uart_commands.h`, `Core/Src/main.c`, `Core/Src/defines.h`, `top_api/defines.h` | how the board mounts, uses and releases its storage |
| `test/fatfs_host_test.c`, `test/fatfs_mock/`, `test/sdcard_examples_host.c`, `tools/board_file_demo_host.c`, `test/cache_runtime_test.c`, `tools/halstub/` | their host tests |

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
| `src/fm_audio_enc.c`, `src/fm_video_enc.c` (search `File_`) | Save | the heaviest new client of the file API: `File_OpenEx`, seek back to patch headers, `File_Flush` at checkpoints, `File_Truncate`, `File_Delete`, a side file `<name>.idx`. Leads W1–W3 |
| `top_api/simple_audio.h` (`RecPlaying`, numbered names `_2`, `_3`), `top_api/examples_save_media.h` | Save | `File_Exists` / `File_SizeOf` used to decide names; folders that do not exist; names at `FM_PATH_MAX` |
| `src/fm_video.c`, `src/fm_audio.c`, `src/fm_image.c` (their read calls) | Video, Audio, Image | what the decoders assume about reads: short reads, a read of 0 bytes, an error in the middle, seeks beyond the end, sizes above 2 GB. A decoder that turns `FM_ERR_IO` into "end of file" hides your errors |
| `src/fm_mediacache.c`, `top_api/play_from_flash.h`, `top_api/media_data_io.h` | Video | a whole file copied to another storage and played from there: partial copy, source changed, destination full |
| `src/fm_player.c` (folder scan) | Audio | directory listing with more entries than the list holds, long names, nested folders |
| `Core/Src/main.c`, `Core/Src/defines.h`, `top_api/defines.h`, `top_api/code_cmd.h` | Save (as glue) | where the configuration is really applied: the include order of the firmware, the mount / unmount / `sdpc` flow |

## 6. Where to dig

**The file API across backends.** The same sequence of operations must give the
same observable result on FMFS (RAM disk), LittleFS, POSIX and FatFS (mock, see
`make fatfs` and `test/fatfs_mock/`). Write a differential tester: random
sequences of create / write / append / seek-past-end + write / truncate up and
down / rename / delete / mkdir / rmdir / list, run on every backend, compare
contents, sizes, listings and error codes. Any difference is a finding unless
`README.md` documents it.

- Open-mode matrix (read, write, create, truncate, append and their
  combinations) on existing and missing files, on directories, on read-only
  storage.
- Rename onto an existing file, onto a directory, onto itself, into its own
  subtree; delete or rename a file that is open; unmount or format with handles
  open.
- Paths: `..`, `.`, `//`, trailing slashes, the longest path `FM_PATH_MAX`
  allows and one byte more, names that are a prefix of a mount point (`/sd` and
  `/sdcard`), empty components, non-ASCII bytes.
- Handle pools: open until exhaustion, then check that every error path gives
  the slot back (loop 10 000 times over each failing call and see if a later
  valid call still succeeds).
- Offsets and sizes near 2^31, 2^32 and above: which fields are 32-bit, which
  64-bit, and where they are mixed.
- `File_Copy` / `File_Move` across two mounts when the destination fills up or
  a write fails half way: what is left behind?
- Every function when the storage returns an error on its Nth read or write,
  for every N. The harness in `test/main.c` (functions `pf_*`, suite
  `test_fmfs_powerfail`) already does this for FMFS; extend it with other
  seeds, volume sizes, block sizes and operation mixes, and apply the same idea
  to LittleFS.

**FMFS on-disk format.** Read the long comment at the top of `src/fm_fmfs.c`
first. Then:

- Corrupt the stored image and mount it: bit flips and random bytes in both
  tables and in the journal, impossible offsets and sizes, overlapping files,
  entries pointing past the end, a journal record that refers to a file that is
  gone. Mount must refuse cleanly or recover; it must never crash, loop, or
  write outside the volume. The table has a CRC — also test images where the
  CRC is valid and the content is nonsense.
- The power-fail guarantee is stated in `README.md` ("FMFS notes"). Try to
  break it: longer campaigns, cuts during recovery of a recovery, volumes
  filled to the last block, the smallest volume that still gets the v2 layout,
  `FM_FMFS_POWER_SAFE` 0 / 1 / 2, `FM_FMFS_MOVE_STEPS` at 1 and at the default.
- Old "v1" volumes must still mount and work.

**Concurrency** (`FM_USE_THREADS=1`). `make tsan` is clean on the existing
tests (`test_threads`, `test_threads_wide`). Write new stress tests: many
threads on one file, on one directory, mounting and unmounting while others do
I/O, callbacks that call back into the library. Look for lock-order inversions,
callbacks run with a lock held, and anything reachable from an interrupt.

**Utilities.**

- `fm_json.c` is a hand-written parser and writer: fuzz it. Deep nesting, every
  escape form, numbers at the limits, truncated input, output buffer exactly
  full.
- `fm_safe.c`: interrupt it between every two storage writes; after a restart
  the file must be the old version or the new one, never a mix or missing.
- `fm_text.c` line editing at the first line, the last line, a line longer than
  the buffer, a file without a final newline, CRLF.
- `fm_logger.c` rotation and the volume filling up.
- `fm_alloc.c`: alignment, size arithmetic overflow, fragmentation, freeing a
  pointer twice or one that was never allocated.
- `fm_async.c`: cancel at every stage; completion callbacks after shutdown.

**Debug levels and log output (new since the first audit, never checked from outside).** The library prints
through `FM_ERR_LOG` (level 1 and up), `FM_EVENT_LOG` (2 and up) and
`FM_TRACE_LOG` (3); level 0 compiles all of it out. Read the top of
`include/fm_debug.h` and "Rules for a sink" in `include/fm_log.h`, then check
the messages of **your** files (`grep -n "FM_ERR_LOG\|FM_EVENT_LOG\|FM_TRACE_LOG\|FM_CHECK" src/fm_file.c src/fm_filesystem.c src/fm_storage.c src/fm_fmfs.c src/fm_transfer.c src/fm_qspi.c src/fm_spiflash.c src/fm_alloc.c`):

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

The mechanism itself is in your scope and deserves a harness of its own:

- `FM_Log_Tagged` / `FM_Log_V` / `FM_Log_Write` (`src/fm_log.c`): truncation at
  exactly `FM_LOG_BUF_SIZE` − 2, − 1, + 0, + 1 with and without a tag;
  `FM_LOG_BUF_SIZE` at its minimum (64) and with a tag of 23, 24 and 100
  characters; a `vsnprintf` that fails; a NULL format, tag or sink context.
- The one-message-at-a-time guard (`FM_USE_THREADS=0`): a sink that logs, a
  sink that calls a library function that logs, a message from a signal
  handler standing in for an interrupt at every point of a message in
  progress. Nothing may recurse, overwrite the buffer or stay blocked;
  `FM_Log_Dropped()` must count right.
- `FM_USE_THREADS=1`: many threads logging at level 3 under ThreadSanitizer;
  `FM_Log_SetSink` called while others log (the sink and its context are two
  separate words); the stack a message needs in the calling task (the buffer
  is on that stack) against the stack sizes the examples configure.
- The claim in `include/fm_sync.h` about which user callbacks run with a lock
  held, and the rule that a sink must not call back into the library: find
  every log line that is emitted with a lock held and say which lock. Is there
  a line that makes the ready-made blocking UART sink
  (`include/fm_log_uart_hal.h`, 100 ms timeout) stall another task for long?
- `include/fm_debug.h`: `FM_CHECK`, `FM_CHECK_PTR`, `FM_CHECK_VOID` in every
  position a statement can appear (unbraced `if` / `else`), a user-defined
  `FM_DEBUG_PRINTF`, `FM_LOG_DISABLE` together with each level, C++ inclusion.
- Does what `fm_debug.h` and the README section "Debug levels and log output"
  say each level prints match what is printed? (`FM_TEST_LOG=1 ./fm_test` at
  each level gives you the full list.)

**Build configuration (new in this round).** Treat `include/fm_board_config.h`
as an input format with users who make mistakes:

- every feature line on and off, alone and in the pairs that depend on each
  other (video without image, MJPEG without JPEG, audio off with the player
  on); every level of `FM_BUFFERS`; `FM_CACHE` on and off; with and without
  `USE_HAL_DRIVER`. `make matrix` covers the `FM_USE_*` combinations on the
  host; nothing covers the combinations with `FM_BUFFERS` or the firmware
  defaults.
- every `#error` of the consistency block (section 5 of that file): can it be
  triggered by a legal choice, and is there an illegal choice it lets through?
- a disabled subsystem must own no static memory (`tools/check_matrix.sh`
  checks `.data + .bss = 0` for the listed objects): check the objects that
  are not on its list.
- the same switch set through the `Makefile`, through CMake
  (`CMakeLists.txt`, `tools/check_cmake.sh`) and through the board header must
  build the same thing.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Nothing has been run on real hardware. Power cuts and I/O errors are
  simulated on the host only.
- FMFS: overwriting bytes in place is not atomic (the file being written may
  tear, including the rest of the block being written; other files and the
  metadata must not be affected). No wear levelling. File data is not
  checksummed. Erase units larger than 4096 bytes get no block guarantee. A
  nearly full v2 volume can answer "no space" while a little free space is
  shown. Small volumes fall back to the v1 layout, which is not power-fail
  safe.
- A synchronisation port that cannot identify the calling thread answers
  `FM_ERR_BUSY` on re-entry.
- FatFS adapter: the `FM_FATFS_PROVIDE_DISKIO=1` path is not built on the host;
  `FatFS_MountDefault()` returns `FM_ERR_UNSUPPORTED` when the adapter does not
  provide the disk I/O.
- A filesystem context (`FMFS_Init`, `LittleFS_Init`, `FatFS_Init`) that is
  initialised and never mounted stays taken until the same `FM_FS` is
  initialised again.

- A name of `FM_NAME_MAX` (64) characters or more: listing its directory fails
  with `FM_ERR_TOO_LARGE` and `Directory_DeleteRecursive` cannot remove it.
  Documented in `README.md` ("File rules worth knowing"); the workaround is
  `FM_NAME_MAX=256`. The owner has accepted this behaviour.
- Directory trees deeper than `FM_DIR_MAX_DEPTH` (16) are refused with
  `FM_ERR_TOO_LARGE`.
- LittleFS: one writer or many readers per file (see FILE-02 above).
- The log sink is called from wherever the message is produced, which can be
  inside a filesystem operation with its lock held; a sink must not call back
  into the library when `FM_USE_THREADS=1`. With `FM_USE_THREADS=0` a message
  that arrives while another is in progress is dropped and counted. Level 3
  changes timing. All of this is documented; report it only if the code does
  something worse than the documentation says.
- Everything the first audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

- The firmware is bare-metal; `FM_USE_THREADS` is 0 there. The threaded ports
  are in scope (other users have an RTOS), the board flow under threads is not.
- `tools/check_cubeide.sh` and `tools/check_switch_names.sh` run with the host
  compiler, not with `arm-none-eabi-gcc`. That is a limit of the checks; a
  defect it hides is in scope.

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_FILE_V5.md`. It is used as a work list:
each finding is opened by someone who has not seen your session and has only
the report and a clean checkout of `fc31b25`. Use this structure, in this
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
     anything (expected: `Results: 589/589 passed, 0 failures`; if yours
     differs, that comes first in the report);
   - a table of what you did: method, target, amount (executions, files,
     cases, hours), coverage reached, result;
   - what you ran as a 32-bit program;
   - your local changes: every file you added or changed in your clone, one
     line each.
3. **Table of findings**: ID, severity, status (confirmed / suspicion), owning
   part, `file:line`, one-line title, fix tested (yes / no), regression test
   included (yes / no). Sorted by severity.
4. **One section per confirmed finding**, IDs `FILE5-01`, `FILE5-02`, ...:
   - **Severity** — S1: memory corruption, crash, hang, loss or corruption of
     stored data. S2: wrong result returned as success (wrong pixels, samples,
     bytes, sizes, timestamps; a recording that lost or misplaced data). S3:
     wrong error handling — a leaked handle or slot, a wrong error code, state
     not restored after a failure. S4: mismatch with the documentation,
     portability, wasted time or memory, a test that cannot fail.
   - **Where** — `path:from-to`, function, at `fc31b25`, and the lines
     themselves (quote at most 15).
   - **Reach** — which public function leads there, which action of a user of
     the board (a UART command, a file on the card, a sequence of calls), which
     build configurations, 32 bits / 64 bits / both, and how likely it is with
     ordinary files and ordinary use.
   - **How to reproduce** — from a clean checkout of `fc31b25`: the files to
     add (full source in the appendix), the exact commands, and the output as
     it was printed (sanitizer report, wrong value, hang with the time you
     waited). For an input file: a hex dump if it is under 200 bytes, otherwise
     the script that generates it.
   - **Expected and observed.**
   - **Root cause** — why it happens, not just where.
   - **Proposed fix** — a unified diff against `fc31b25` in a `diff` block.
     Produce it with `git diff`, do not type it: the sources have CRLF line
     endings and a typed diff will not apply. Then state: applied (yes / no),
     the reproducer after the fix, the result line of `make test` after the
     fix, and anything else you ran with it (`make asan`, `make m32`, the
     smaller host tests).
   - **Regression test** — the test to add (which file, the code), with its
     result on `fc31b25` (must fail) and with your fix (must pass). If a test
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

- Make a fresh clone at `fc31b25` and re-run every reproducer from the text of
  the report alone. Drop or downgrade what does not reproduce.
- Run `git apply --check` for every diff on that clean tree.
- Re-check every `file:line` against `git show fc31b25:<path>`.
- The numbers in the Persian summary, in the table of findings and in the
  findings index agree.
- No finding rests on a documented limit, and no "confirmed" finding contains
  "may", "might" or "could" in its observed behaviour.
- Every number in the report has the command that produced it.
