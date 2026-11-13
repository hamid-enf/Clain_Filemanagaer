# External audit V6 — video

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
  commits on the branch update the audit prompts and library README, not the code under audit.
- Library root: `Clain_FileManager/FileManager_lib/`. Paths below are relative to
  it, except: paths that start with `Core/` are in `Clain_FileManager/Core/`
  (the CubeMX firmware project of the reference board), and `ReportV2.txt`,
  `Report.txt` and `audit_prompts/` are at the repository root.

History of this part:

| Round | Commit audited | Report | Then |
|---|---|---|---|
| first | `8dd0f25` | `ReportV2.txt`, lines 1089–1265 | one finding, fixed; then the maintainers' own comparison with ffmpeg changed much of the audio-track code, up to `a6cf829` |
| follow-up ("V3") | `a6cf829` | none in the repository | the prompt was written (`git show f161ff8:audit_prompts/03_video.md`); no report of it is in the repository. **This part has had the least outside scrutiny of the five.** Its jobs are carried into this round |
| V4 | `48a00bc` | `ReportV3.txt`, lines 1273–2075 | seven findings (VID4-01 … -07), all fixed in `fc31b25`; six suspicions open |
| V5 | `fc31b25` | `ReportV4.txt`, lines 562–1153 | two findings (VID5-01, VID5-02) and three suspicions (S-1 … S-3); both findings, S-1 and S-3 fixed in `6fe596d`, S-2 documented |
| this one (V6) | `6fe596d` | `AUDIT_VIDEO_V6.md` | |

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

Your part is **Video**: containers, the video decoders, the audio tracks of
video files, timing, the media cache and the playback side of `SimpleVideo`.
Section 5 says exactly which files, and how far you may go into the other
parts.

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

Read the first report of this part first (`ReportV2.txt`, lines 1089–1265); it
is short, and its "Not examined" section lists almost the whole scope. Then the
two diffs that matter:

```bash
# everything since the first round up to the follow-up prompt (never read from outside)
git log --oneline 8dd0f25..a6cf829
git diff 8dd0f25..a6cf829 -- src/fm_video.c src/fm_h264.c src/fm_mjpeg.c src/fm_mediacache.c \
    src/fm_player.c src/fm_adpcm.h third_party/picojpeg include/fm_video.h
# what is new since the follow-up prompt
git diff a6cf829..48a00bc -- src/fm_video.c src/fm_h264.c src/fm_player.c third_party/h264bsd \
    include/fm_player.h include/fm_config.h
git diff a6cf829..48a00bc -- top_api/simple_video.h      # the player: about 600 changed lines
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
For your part the reference is ffmpeg / ffprobe: every decoded picture against
ffmpeg's (PSNR), every decoded audio track sample for sample, every duration,
frame count, frame rate and timestamp against ffprobe.

### What round V5 found, and what the fixes claim

The report of that round is in the repository: `ReportV4.txt`, lines 562–1153 (four of the five reports are
in that file; it is written in Persian; the harnesses and patch files it names under `/tmp` are not in the
repository). Read it, then the diff of the fixes:

```bash
git diff fc31b25..6fe596d -- top_api/simple_video.h src/fm_video.c src/fm_mediacache.c include/fm_mediacache.h \
    Makefile tools/check_deps.sh tools/check_flag_stamp.sh tools/save_examples_host_test.c test/audit_v5_tests.h
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. "Mutation: yes" means the maintainers
removed the fix and saw the named test fail.

| ID | What was wrong | What the fix claims | Mutation |
|---|---|---|---|
| VID5-01 | `SimpleVideo_AudioDataRead` returned mono data for a mono track and counted twice the frames; the caller's stereo buffer was half garbage | always interleaved 16-bit stereo, `*got_frames` never above `frames`, one packet per call; a mono packet is doubled in place (a packet larger than half the buffer is `FM_ERR_TOO_LARGE`: MP3 mono needs `frames` ≥ 2304, stereo 1152); a track that is not 16-bit PCM after decoding, or has more than 2 channels, is `FM_ERR_UNSUPPORTED`. Both twins (`top_api/` and `examples/`). Section `[8]` of `tools/save_examples_host_test.c` (`make saveexamplestest`, 85 checks). **Public behaviour change** | yes |
| VID5-02 | the `Makefile` did not make the headers under `src/` and `backends/` prerequisites: `make test` after a header edit tested the old objects | `-MMD -MP`, the `.d` files are included; `tools/check_deps.sh` (part of `make check`) touches 6 headers and expects a rebuild | yes |
| S-1 | A-law / µ-law in a video: `Video_ReadAudioFrame` allocated a block per call and checked the caller's capacity after the read | capacity first (`FM_ERR_TOO_LARGE`, nothing consumed), expanded in the caller's buffer, no `FM_Alloc`. Tests `S-1 …` (suite `test_audit_v5_video` in `test/audit_v5_tests.h`), on an FLV built in memory | yes |
| S-2 | `SimpleVideo_Info()` is not valid before the first frame | **documented only** (comment above `SimpleVideo_Info` in both twins); no code change | n/a |
| S-3 | media cache: with the cache full and the oldest entry open, the eviction gave up and the next file was not cached | `mc_evict_one` (`src/fm_mediacache.c`) passes over open entries (at most `MC_EVICT_BUSY_MAX` of them). Test `MediaCache: a cached file that is open is not evicted, the next one is` in `test/main.c`. A real defect, reported as a suspicion | yes |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. VID5-01: "one packet per call" means a caller that asks for more frames than a packet holds gets fewer and must call again. The comment says so; does `board/`, `top_api/` or `examples/` code call it and assume a full buffer? For PCM tracks `Video_ReadAudioFrame` continues a packet larger than `cap` on the next call (the maintainers checked that with a harness of their own that is not in the repository): is the same true for every container, and for a `frames` so small that not one sample frame of a 24-bit or 8-channel track fits?
2. VID5-01: the I2S side of `SimpleVideo` reads the same track. A caller that uses `SimpleVideo_AudioDataRead` while the player is running takes sound away from the speaker (section `[8]` shows it: the total read is below the track length). Is that contract written where a user reads it, and is there a state in which the two readers corrupt each other rather than share?
3. S-3: with more than `MC_EVICT_BUSY_MAX` (4) open cached files the search still ends without evicting. What does the caller see then, and is that the documented behaviour?
4. VID5-02: `.d` files are written for the library and for `fm_test`. Are the other host programs (`fm_cachetest`, `fm_videohooks`, `fm_hwjpegtest`, `fm_usbmsctest`, `fm_audiorecordtest`, `fm_saveexamplestest`, `fm_tapstest`), the ASan build directory and the CMake build covered? `tools/check_deps.sh` checks six headers, including two that rebuild `fm_test`.
5. Still as they were (Job 1 of V4 found them, V5 confirmed them, nobody fixed them): the Microsoft ADPCM track test in an AVI and the BT.709 full-range row of `k_yuv_matrix` cannot fail. The V5 report's mutation table ("16 of 17 caught") names the gaps; they are leads.
6. Sections 8, 10 and 13 of the V5 report (tests that cannot fail, what was not examined, where the next round should look: `ReportV4.txt`, lines 996–1082) were not turned into work. Read them as leads.


### What round V4 found, and what the fixes claim

The report of that round is in the repository: `ReportV3.txt`, lines 1273–2075 (the five reports are in one
file; the harnesses the auditors wrote are not in the repository). Read it, then
the diff of the fixes:

```bash
git diff 48a00bc..fc31b25 -- src/fm_video.c src/fm_player.c src/fm_flac.c src/fm_video_enc.c include/fm_video.h \
    third_party/h264bsd/src/h264bsd_seq_param_set.c top_api/simple_video.h
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. The tests are in
`test/audit_v4_tests.h` unless the row says otherwise; remove the fix locally
and see whether the test fails.

| ID | What was wrong | What the fix claims |
|---|---|---|
| VID4-01 | an H.264 SPS with an absurd picture size crashed the decoder (32-bit wrap of the macroblock count) | `h264bsd_seq_param_set.c` refuses a width or height of 0 and more than 36864 macroblocks. Tests `VID4-01 …` (`av4_h264_sps` in `test/audit_v4_tests.h`); with the fix removed the suite dies with a floating point exception |
| VID4-02 | an MP4 box with a 64-bit size of 0…15 made `Video_Open` loop for ever | `mp4_box_at` returns `FM_ERR_CORRUPTED`. Test `VID4-02 …` (the suite is killed by `alarm` when the fix is removed) |
| VID4-03 | `simple_video_sync_audio` multiplied in 32 bits: after a seek deep into a long file the sound started at the wrong place | `sv_audio_index_at` (64-bit) in `top_api/simple_video.h`; checked in section `[7]` of `tools/save_examples_host_test.c` |
| VID4-04 | MP3, 8-bit and 24-bit PCM tracks were silent in `SimpleVideo` | `sv_track_decodes_inline` asks `Video_GetAudioInfo(...).decodable`; `sv_i2s_start` no longer tests the sample width. Section `[7]` |
| VID4-05 | the frame rate was a rounded integer (29.97 → 30, 12.5 → 13): positions, durations and A/V sync drifted | `FM_VideoInfo.fps_num` / `fps_den` filled by all four container parsers and used by `Player_Seek`, the position / duration of the player, the clock of `SimpleVideo` (`sv_frame_at_ms`, `sv_ms_of_frame`) and `Video_Convert`. `fps` stays the rounded figure. **The struct grew by 8 bytes** |
| VID4-06 | a FLAC block larger than the caller's buffer was cut and returned with `FM_OK` | `flac_decode` and `Video_ReadAudioFrame` return `FM_ERR_TOO_LARGE`, consume nothing and stay on the block; `SIMPLE_VIDEO_PCM_CHUNK` and `FM_VIDEO_CONVERT_AUDIO_BYTES` are 18432 now |
| VID4-07 | the folder scan of the player ignored `.mkv`, `.webm`, `.flv` | `is_video_path` in `src/fm_player.c` |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. VID4-05: what do `fps_num` / `fps_den` say for a variable-frame-rate MP4 (several `stts` entries), an MKV without `DefaultDuration`, an FLV whose first timestamps are not regular, an AVI with `dwMicroSecPerFrame` = 0 but a `strh` rate / scale? Compare every vector and your own files with `ffprobe -show_streams` (`r_frame_rate`, `avg_frame_rate`). `Video_Convert` halves numerator and denominator until the numerator is below 1 000 001: what does that do to an MKV rate?
2. VID4-06: with `FM_BUFFERS=SMALL` `SIMPLE_VIDEO_PCM_CHUNK` is a quarter (4608 bytes), so every FLAC block of a normal file is refused and the track is silent (logged as "PCM read"). Is that documented where a user reads it? Are the other callers of the registry `decode` (`src/fm_audio.c`, the Opus and the stream paths) prepared for `FM_ERR_TOO_LARGE` from `flac_decode`?
3. VID4-01: is 36864 macroblocks the right bound (level 5.1 / 5.2 pictures, a legitimate 4096x2304 stream), and are the other SPS / PPS fields that size an allocation or a loop bounded as well (`num_ref_frames`, `pic_size_in_map_units`, slice groups, the VUI)?
4. `src/fm_media.c` (owned by the audio part) has no type for `.mkv` / `.webm` / `.flv`: what does `File_DetectType` say for the files the player now plays?
5. The six suspicions of the V4 report (S-1 `dr_flac.h` signed overflow, S-2 negative shift in `h264bsd_reconstruct.c`, S-3 MP3 with a buffer below 4608 samples, S-4 end trim of Vorbis / Opus in MP4, S-5 picture clock and I2S clock not tied, S-6 slow fuzz files) were not worked on. Settle them.
6. The last sections of the V4 video report (`ReportV3.txt`, lines 1960–2075) were not turned into work by the maintainers. Read them as leads.
7. Job 1 of V4 found two tests of the earlier fixes that cannot fail: the Microsoft ADPCM track test, and the BT.709 full-range row of `k_yuv_matrix`. They are still as they were.


### Job 1 — check the fixes (do this first; it is the cheapest way to find a defect)

For every earlier finding below, decide: fixed, partly fixed, not fixed, or
fixed but something else broke. Read the test that was added for it
(`test/audit_v2_tests.h`), then remove the fix locally and see whether that
test fails.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| VID-01 | deeply nested MP4 boxes exhausted the stack in `Video_Open` | the box walk is bounded: nesting deeper than `MP4_MAX_NEST` (8) is refused with `FM_ERR_INVALID_FORMAT` (`mp4_find_nested` in `src/fm_video.c`) |

Check VID-01 beyond the reported file: every other place where a container
parser recurses or follows an offset taken from the file (MKV master elements,
AVI `LIST` inside `LIST`, FLV script data objects, MP4 `meta` / `udta`), and
whether a legitimate file can need more than 8 levels (ffmpeg, phone cameras,
QuickTime).

The maintainers' own fixes after the first round count as earlier findings
too, and nobody outside has checked them. Give a verdict on each:

| ID for your table | What was wrong | Where the fix is |
|---|---|---|
| M-01 | MP3 track of a video decoded one frame, then failed for ever | `video_mp3_read` |
| M-02 | IMA and Microsoft ADPCM tracks decoded to noise | `video_adpcm_read`, `src/fm_adpcm.h` |
| M-03 | Vorbis tracks never decoded | `video_vorbis_read`, `vorbis_private_split` |
| M-04 | H.264 pictures 25 dB from the reference (colour range) | `yuv420_to_rgb888` in `src/fm_h264.c`, `k_yuv_matrix[4]` |
| M-05 | Motion JPEG in `.mp4` not playable | `mp4_visual_object_type` |
| M-06 | PCM tracks of other widths and byte orders than 16-bit little-endian | `video_pcm_read`, `mp4_pcm_entry` |
| M-07 | encoder delay and padding of MP3 / Vorbis / Opus tracks not removed | `mp4_audio_edit`, `mkv_group_discard`, `video_audio_trims` |
| M-08 | a size or offset from a damaged index became an allocation of that size | `video_in_file`, the range check in `video_audio_locate` |

### Job 2 — audit what is new

Code written since the last audit has been through the fewest eyes. Two layers
of it are yours.

**(a) Written before `a6cf829`, never read from outside:**

- `Video_ReadAudioFrame` and everything under it in `src/fm_video.c`:
  `video_audio_locate`, `video_audio_decode_packet`, `video_mp3_read` (decoder
  and PCM carry on the heap from the first packet to `Video_Close`, frames
  continued across calls, silence for a frame whose bit reservoir is missing),
  `video_adpcm_read` with `adpcm_ima_block` / `adpcm_ms_block` (block size from
  the container, a chunk of several blocks continued across calls),
  `video_vorbis_read` with `vorbis_private_split` (Xiph lacing of the
  CodecPrivate), and the reset of all of it in `Video_SeekAudio`.
- The contract written above `Video_ReadAudioFrame` in `include/fm_video.h`:
  for every result it says what has been consumed. Check each line of it for
  each codec, with the buffer sizes around each limit (one byte less than a
  block, exactly a block, a buffer of 1, 2, 3, 4 bytes), and check that a read
  loop written from that comment alone always ends.
- `mp4_parse_esds` (object types: 0xDD is Vorbis, unknown types give no track)
  and `mp4_visual_object_type` (0x6C is Motion JPEG).
- `src/fm_h264.c`, `yuv420_to_rgb888`: the colour range and the matrix come
  from the VUI (`k_yuv_matrix[4]`: BT.601 / BT.709, video / full range; only
  `matrix_coefficients == 1` selects BT.709, a VUI that says nothing is BT.601
  video range). There is a test vector for three of the four rows, none for
  BT.709 full range. Check the values it treats as BT.601 (BT.2020 = 9,
  SMPTE 240M = 7, RGB = 0), and streams whose VUI changes with a new SPS.
- `video_pcm_read` and `mp4_pcm_entry`: PCM tracks of 8, 16, 24, 32 bits and
  32-bit float, both byte orders, in AVI / MKV / MP4 / MOV / FLV, converted to
  16-bit **in the caller's buffer**; adjacent small packets are read together
  (`VIDEO_PCM_GATHER`). Check the in-place conversion at every width with
  buffers around one sample frame, packets that are not a whole number of
  frames, and the gathering at the end of the index.
- The trimming wrapper: `Video_ReadAudioFrame` is a layer over
  `video_audio_read_raw` that removes encoder delay and padding
  (`mp4_audio_edit`, MKV `CodecDelay` / `DiscardPadding` in
  `mkv_group_discard`, `video_audio_trims`, `video_audio_rest_bytes`). Check:
  edit lists with several edits, empty edits, a `media_time` beyond the track,
  a duration of 0 or longer than the track; `DiscardPadding` negative, larger
  than the packet, on a block that is not the last; `CodecDelay` near its
  one-second cap; every buffer size (a packet delivered in parts must lose the
  same frames); the frame count after `Video_SeekAudio(0)` and after a seek
  elsewhere; and the frame totals against ffmpeg for every codec and container.
- `video_in_file` and the range check in `video_audio_locate`: look for the
  places that still trust a size or an offset from the file.
- `video_flac_too_wide`: a FLAC track wider than 24 bits is announced with
  `decodable == FM_FALSE`. A read that returns `FM_ERR_UNSUPPORTED` passes the
  packet over, so that the track ends; `FM_ERR_TOO_LARGE` consumes nothing for
  every codec.

  The maintainers ran this audio code on 90 000 mutated copies of the test
  vectors under ASan + UBSan (which found one undefined shift in stb_vorbis's
  resync, now patched) and compared ffmpeg-written files with ffmpeg's decode:
  37 at first, then a sweep of every audio codec in every container that can
  carry it (3 s each; PCM, G.711, ADPCM and FLAC identical, MP3 / Vorbis / Opus
  within one LSB and of the same length). The newest of it (PCM, trimming,
  range checks) has **not** been fuzzed. That is a start, not coverage.

**(b) Written since `a6cf829`:**

- **The player `top_api/simple_video.h`** (3 864 lines, about 600 changed;
  commits `c688b9e`, `4cbd8a0`, `523d2da`, `48a00bc`). Its playback side is
  yours (the `SimpleVideo_Rec*` functions and `SimpleVideo_Convert` belong to
  the Save auditor):
  - the **sound output switch** `SimpleVideo_SetAudioOutput`
    (`FM_AUDIO_OUT_NONE / SPEAKER / CALLBACK / BOTH`, `include/fm_player.h`):
    which callbacks fire in each position, is the I2S output really silent in
    `CALLBACK`, is the track not decoded at all in `NONE`, and **does the
    video still run at the right speed in every position** (find out where
    the player's clock comes from in each) — also when the switch is changed
    during playback, and for a video without sound;
  - the **callbacks** (`SimpleVideo_SetFrameCallback`,
    `SetAudioDataCallback`, `SetAudioMonitorCallback`,
    `SetTrackStartCallback`, `SetTrackEndCallback`): they now survive `Stop` /
    `Begin`. Establish in which context each runs (I2S interrupt, `Poll`,
    `FM_Idle` inside a decode) and whether the comments say the same; count the
    calls for one video (every presented frame exactly once? every sound block
    exactly once, including the last one, which used to be lost? track-start
    and track-end exactly once per video, also on `Next`, `Prev`, autoplay
    wrap, a failed open, `Stop` in the middle?);
  - **`SimpleVideo_Info()`** is filled at the first frame callback (before, it
    returned nothing until the player was "running"): stale data after `Stop`,
    between two videos, after a failed open; `frame_index` in every path
    (first frame, after a seek, after a pause, the hardware-JPEG path, a
    repeated present of the same frame);
  - **seeking** (`SimpleVideo_SeekTo`, `SeekBy`; UART `vnp10` / `vpn10`): new
    in `48a00bc` are `seek_count`, `seek_sound_ms` and an internal
    `sv_sound_sync_ms`, written to fix sound that started about two frames
    ahead of its picture after a seek with a slow player. Measure the A/V
    offset after a seek forward, backward, to 0, past the end, twice in a row,
    during a pause, on a video without sound, and at a track change, with a
    fast and a slow simulated player (the host test can make each picture take
    any amount of simulated time);
  - **`SimpleVideo_SetSwap16`** and the frame buffer byte order at run time.
- **Small changes in the library**: `src/fm_h264.c` (the values h264bsd writes
  through pointers are declared with its `u32` now — on ARM `uint32_t` is
  `unsigned long`, on the host it is `unsigned int`: check that no place still
  mixes the two), `third_party/h264bsd/src/h264bsd_util.h` (`#undef DEBUG`),
  `src/fm_video.c` (a `memset` in the MP4 open), `src/fm_player.c` (compiled
  even with audio and video off, for the playlist half).
- **New producers of files your readers must read.** The library now writes
  AVI files (`src/fm_video_enc.c`, Save auditor). Your reader is promised to
  open: a closed file; a file cut at a checkpoint (a valid AVI **without**
  `idx1`, header counts from the last checkpoint, more data after them); the
  same file after `VideoFile_Repair`; a file with repeated pictures, with odd
  chunk sizes, with sound before the first picture, with 0 pictures. Read each
  with your code and with ffmpeg and compare frame count, duration, every
  picture, every sound sample. `Video_Convert` is also the first caller that
  reads a whole video as fast as it can instead of in real time: every
  container and codec through it, with the progress callback cancelling at
  every point.

### Job 3 — go where nobody has been

The first audit of this part was thin: one finding, found by reading; no
fuzzing, no differential comparison, no truncation or fault-injection
campaign, no seek or timing checks. **Treat section 6 below as never done and
work through all of it.** `src/fm_video.c` is 7 200 hand-written lines that
take every size and offset from the file.

### Open leads — an answer to each is wanted in the report

- **W1. Clips longer than the index.** `FM_VIDEO_MAX_FRAMES` is 1024 / 4096 /
  4096 / 8192 for `FM_BUFFERS` = SMALL / NORMAL / LARGE / HUGE; beyond it the
  code continues from the container (`video_tail_locate`). For each container:
  a clip of limit − 1, limit, limit + 1 and ten times the limit frames — frame
  count, duration, seek into the tail, the audio track in the tail, the end.
  Build with each level.
- **W2. Static RAM of the video slots.** `g_video[FM_MAX_VIDEO]` was about
  547 KB at 4096 frames; what is it at 8192, and does the documentation say
  so?
- **W3. A/V drift on a long file.** Every test clip is a few seconds long.
  Generate a 30-minute file with a click and a flash every second, play it
  through `SimpleVideo` on the host with simulated time, and measure the
  offset at the start, the middle and the end, for 29.97 fps and for a sound
  rate the I2S clock cannot produce exactly.
- **W4. BT.709 full range** has no test vector (Job 2 (a)).
- **W5. Stack on ARM**: `README.md`, "Stack", lists the frames above 1 KB; the
  depth of the deepest chain (picture decode → `FM_Idle` → audio refill →
  audio decode) is not there. Measure it.

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
| `src/fm_video.c`, `include/fm_video.h` | about 7 200 lines: container parsers (FMRV, AVI, MP4/MOV, MKV/WebM, FLV), frame index, seek, audio-track demux, FMRV writer |
| `src/fm_h264.c`, `include/fm_h264.h`, `third_party/h264bsd/` | H.264 decoder plugin |
| `src/fm_mjpeg.c`, `include/fm_mjpeg.h` | MJPEG decoder plugin |
| `src/fm_mediacache.c`, `include/fm_mediacache.h` | media cache (copy to fast storage and play from there) |
| `src/fm_player.c` (video paths) | playlist player |
| `examples/simple_video.h`, `examples/hw_jpeg_video.h`, `examples/play_from_flash.h` | how applications use it |
| `test/video_hooks_test.c`, `tools/gen_test_videos.py` | hook test, generator of test videos |

The audio **decoder libraries** (minimp3, dr_flac, stb_vorbis, libopus and the
registry they sit in) belong to the audio auditor. Yours is everything between
the container and them: finding the audio track, cutting it into packets,
timestamps, `Video_HasAudio`, `Video_GetAudioInfo`, `Video_ReadAudioFrame`,
`Video_SeekAudio`, **and the decoding that happens in `src/fm_video.c`
itself** (PCM, A-law, mu-law, both ADPCM kinds, the MP3 frame loop, the
hand-over of packets to the Vorbis stream decoder).

Tests to read first in `test/main.c`: `test_video`, `test_video_report`,
`test_mjpeg`, `test_mediacache`, `test_player`.

Limits to know (`include/fm_config.h`): `FM_VIDEO_MAX_FRAMES`,
`FM_VIDEO_MAX_AUDIO_FRAMES`, `FM_VIDEO_AUDIO_PRIVATE_MAX`,
`FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX`, `FM_H264_TIER_DEFAULT`.

New in this round (Job 2 (b)): the playback side of `top_api/simple_video.h`
(the twin of `examples/simple_video.h`; the firmware uses the `top_api/` copy)
with `FM_AudioOutput` in `include/fm_player.h`; `board/app_video_hooks.h`; the
host tests that drive the player on a stub HAL: `test/video_hooks_test.c`
(`make videohooks`) and `tools/save_examples_host_test.c`
(`make saveexamplestest`); `test/audit_v3_tests.h` with the generators
`tools/gen_video_audio_vectors.py` and `tools/gen_all_test_assets.py`. The
recording functions in the same header (`SimpleVideo_Rec*`,
`SimpleVideo_Convert`) and `src/fm_video_enc.c` belong to the Save auditor.

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
| `src/fm_video_enc.c` (`VideoFileWriter_*`, `VideoFile_Repair`, `Video_Convert`) | Save | writes the AVI files your reader must open, and reads every container through your API (Job 2 (b), last item) |
| `top_api/simple_video.h` (`SimpleVideo_Rec*`), `top_api/examples_save_media.h` | Save | they hang on your callbacks and on `SimpleVideo_Info()`: what they assume about call counts, order and context is your contract |
| `src/fm_audio.c`, `src/fm_vorbis.c`, `src/fm_flac.c`, `src/fm_opus.c`, `third_party/minimp3` | Audio | the decoders your demux hands packets to: what they do with a packet that is cut, empty or repeated |
| `src/fm_jpeg.c`, `third_party/picojpeg` | Image | MJPEG frames are decoded there; picojpeg has global state |
| `top_api/audio_dma_player.h`, `top_api/simple_i2s_mux.h`, `top_api/simple_idle_mux.h` | Audio | the I2S output is shared between the music player and the video player; who owns it when the board switches from one to the other |
| `src/fm_file.c`, `src/fm_filesystem.c` (reads, seeks) | Files | short reads, read errors, 64-bit offsets on a 32-bit target |
| `board/app_video_hooks.h`, `board/app_flow.h`, `top_api/code_cmd.h` | Save (as glue) | how the board starts, stops and seeks a video, and which callbacks it installs in which order |

## 6. Where to dig

**Fuzz the container parsers** — `src/fm_video.c` is hand-written and takes
every size and offset from the file. One harness per container that does
`Video_Open`, `Video_GetInfo`, `Video_GetAudioInfo`, then `Video_ReadFrame`,
`Video_DecodeFrame`, `Video_ReadAudioFrame`, `Video_Seek`, `Video_SeekAudio`
in an order the fuzzer chooses, then `Video_Close`. Seed with real files made
by ffmpeg in many variants and with `tools/gen_test_videos.py`. Watch for
hangs as closely as for crashes: use a short per-input timeout.

Things a parser must survive:

- Box, chunk, element or tag sizes of 0, of 1 (64-bit size follows), larger
  than the file, larger than the parent, and sizes that make the offset wrap.
- Offsets that point backwards (loops) or into the header itself.
- Deep nesting; the same box twice; mandatory boxes missing; boxes in an
  unusual order (`moov` after `mdat`, index before or after the data).
- Counts that multiply into an overflow: entries × entry size, samples ×
  sample size, width × height × bytes per pixel.
- More frames than `FM_VIDEO_MAX_FRAMES`, more audio frames than
  `FM_VIDEO_MAX_AUDIO_FRAMES`: clean refusal or documented truncation, never an
  overrun.

Per container:

- **MP4/MOV**: `stco` and `co64`; `stsz` with fixed and per-sample sizes;
  `stsc` runs that do not cover all samples; `stts` and `ctts` (B-frames:
  decode order and presentation order differ); `stss` missing (every frame a
  keyframe) or wrong; edit lists; several video or audio tracks; timescales of
  1 and of 2^31; fragmented files (`moof`) — find out if they are supported and
  check that an unsupported file is refused, not half-played.
- **AVI**: `idx1` missing, wrong, or relative to a different base; OpenDML
  files above 1 GiB (`ix##`, `indx`); odd-sized chunks and their padding;
  `JUNK` everywhere; several streams; `rec ` lists; MJPEG frames without a
  Huffman table (very common in AVI: the decoder must supply the default one).
- **MKV/WebM**: EBML sizes of every length and "unknown size"; all three lacing
  forms; SimpleBlock and BlockGroup; Cues missing or wrong; clusters without a
  timecode; `CodecPrivate` missing or malformed for H.264, Vorbis, Opus, AAC.
- **FLV**: tag sizes and "previous tag size" that disagree; script data with
  and without `onMetaData`; AVC sequence headers arriving late or twice.
- **FMRV** (the library's own format) and `VideoWriter_*`: write, read back,
  compare; a writer closed after a failed write must leave a file the reader
  refuses or reads correctly.

**Seeking and timing.**

- `Video_Seek` to frame 0, the last frame, one past it, a non-keyframe (what is
  decoded next must be a correct picture, or the documented behaviour), and
  repeatedly back and forth; then compare decoded frames with the same frames
  from a straight run.
- Audio timestamps against video timestamps over a long file: drift, and the
  position after `Video_SeekAudio`.
- Frame rate and duration reported by `Video_GetInfo` against ffprobe, for
  constant and variable frame rate.

**H.264** (`src/fm_h264.c` is the glue; `third_party/h264bsd` is the decoder,
which implements the Baseline profile).

- AVCC with NAL length sizes 1, 2, 4 and Annex B; SPS/PPS inside the stream,
  changing mid-stream, a resolution change; Main or High profile streams (must
  be refused with a clear error, see `H264_GetLastError`); corrupted NAL units
  in the middle of a good stream — the next good keyframe must decode.
- The decoder allocates from `FM_Alloc`: make the pool too small, make the
  Nth allocation fail for every N. The result must be a clean error, no leak
  (check allocator statistics), and the decoder usable again after
  `Video_ResetCodec` or close + open.
- Compare decoded frames with ffmpeg's for Baseline streams: the YUV planes
  bit-exact, the RGB output within rounding (the maintainers measured 48 dB
  PSNR after the colour-range fix; say what the remaining difference is).
- `Video_DecodeFrameAs` and `Video_DecodeFrameScaled`: every pixel format,
  output buffer one byte too small, odd dimensions, cropping in the SPS.

**Audio tracks, the decoding in this file**: MP3, both ADPCM kinds, A-law,
mu-law, PCM in every width and byte order, and the hand-over to the Vorbis
stream decoder are decoded *here*, not in the audio part. For each: files from
ffmpeg and from other muxers in every container that can hold it, mono and
stereo, low and high rates, compared with ffmpeg's PCM sample for sample; then
the same files truncated at every offset and mutated; then seeks into the
middle followed by a read to the end.

**MJPEG**: frames of varying size, a frame larger than the work buffer, a
corrupt frame in the middle, the hardware path through the hooks
(`test/video_hooks_test.c`).

**Lifecycle.**

- 1000 open / close cycles per container, including opens that fail at every
  stage: no file handle, codec slot or allocator block may leak.
- Two videos open at once; close in the other order; a codec registered,
  replaced and unregistered while a video is open.
- Media cache: eviction while a cached file is open, the fast storage filling
  up during the copy, source removed half way, statistics.
- Player: a folder with unplayable files between good ones, switching tracks
  in the middle of a frame, seek during a track change.

**Real-time behaviour.** No single call may do work proportional to the file
size after open (and open itself should not read the whole file). Find calls
that do — on an SD card that is a visible stall.

**Debug levels and log output (new since the first audit, never checked from outside).** The library prints
through `FM_ERR_LOG` (level 1 and up), `FM_EVENT_LOG` (2 and up) and
`FM_TRACE_LOG` (3); level 0 compiles all of it out. Read the top of
`include/fm_debug.h` and "Rules for a sink" in `include/fm_log.h`, then check
the messages of **your** files (`grep -n "FM_ERR_LOG\|FM_EVENT_LOG\|FM_TRACE_LOG\|FM_CHECK" src/fm_video.c src/fm_mediacache.c src/fm_h264.c src/fm_mjpeg.c src/fm_player.c`):

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

**The player on simulated time (added before the preceding round).**
`tools/save_examples_host_test.c` shows how to run `SimpleVideo` on a PC with a
stub HAL, a hook on `HAL_GetTick`, and the I2S half / full interrupts raised by
the test (`sv_i2s_half`, `sv_i2s_full`), with a fast or a slow simulated
player. Build your own harness on that model for Job 2 (b) and leads W3 and
W5: every statement about call counts, order and A/V offset in your report
should come from such a run, not from reading.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Codecs other than RAW, MJPEG and H.264 (MPEG-4 Part 2, MPEG-2, HEVC, VP8,
  VP9, AV1, Theora) are identified and need an external decoder plugin. A
  missing decoder is not a defect; a crash or hang while identifying one is.
- H.264 is Baseline profile only (limit of the vendored decoder): Main and
  High profile streams open and every frame is refused.
- H.264 colour: BT.601 and BT.709 only; every other matrix a stream declares
  (BT.2020, SMPTE 240M, RGB) is converted as BT.601.
- Motion JPEG frames in a layout picojpeg cannot hold (an MCU of more than 8
  blocks) are refused (`README.md`, video notes).
- Encoder delay and padding are removed only where the container records
  them (MP4 / MOV edit list, MKV `CodecDelay` / `DiscardPadding`). AVI and FLV
  record neither: an MP3 track there starts 1105 frames late, as in ffmpeg.
  An MP4 edit list of more than one edit, or one that starts later than one
  second, is ignored.
- After `Video_SeekAudio` to a packet other than 0 the MP4 length limit is not
  applied (the padding of the last packet is played) and the first frames of
  an MP3 track are silence; documented at `Video_ReadAudioFrame`.
- A track without a decoder (`decodable == FM_FALSE`) returns
  `FM_ERR_UNSUPPORTED` once for every packet and then `FM_ERR_EOF`. AAC
  without a decoder is handed out as ADTS frames, not PCM.
- FLAC tracks above 24 bits a sample and PCM tracks of 12, 20 or 64 bits are
  not decoded (`decodable == FM_FALSE`). AC-3, DTS and ALAC are not
  recognised: the clip opens without an audio track.
- PCM is always handed out as 16-bit: wider samples lose their low bits.
- The open video handles are static: `g_video[FM_MAX_VIDEO]` is about 547 KB
  of `.bss` with the default `FM_MAX_VIDEO` 4 and `FM_VIDEO_MAX_FRAMES` 4096
  (`README.md` says how to shrink it).
- FLV without `onMetaData` is reported as 320 × 240.
- Audio tracks not supported inside video: ALAC in MP4; Flash ADPCM, Nellymoser and
  Speex in FLV.
- HE-AAC tracks report the core sample rate.
- The AAC-in-MKV path (`A_AAC`, audio object type handling) has no test of its
  own: defects there are welcome, the missing test is known.

- MP4 box nesting deeper than 8 is refused (`MP4_MAX_NEST`). Report it only
  with a real-world file that needs more.
- `.m4a` files are MP4 and are not picked up by the player's folder scan.
- Everything the first audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_VIDEO_V6.md`. It is used as a work list:
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
4. **One section per confirmed finding**, IDs `VID6-01`, `VID6-02`, ...:
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
