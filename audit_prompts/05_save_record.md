# External audit V4 — saving and recording (audio, video) and how the parts work together

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

History of this part. The code is new: `c688b9e` (output switch, persistent
callbacks), `4cbd8a0` (encoders, AVI writer, converters, recording functions),
`523d2da` (save examples, board glue).

| Round | Commit audited | Report | Then |
|---|---|---|---|
| first (V1) | `523d2da` | `AUDIT_SAVE_V1` — its text is **not** in the repository; the table in section 4 is the maintainers' summary of its eight findings | all eight were worked on in one commit, `48a00bc` (29 files, tests included) |
| V2 | `48a00bc` | `ReportV3.txt`, lines 2076–2501 | one finding and one suspicion (SAV2-02, SAV2-03) and two blind tests, all fixed in `fc31b25` |
| V3 | `fc31b25` | `ReportV4.txt`, lines 1154–1904 | three findings (SAV3-01 … -03) and suspicion SAV3-04; -01 and -03 fixed and -02 answered in the texts in `6fe596d`; **SAV3-04 is open** (lead L13) |
| this one (V4) | `6fe596d` | `AUDIT_SAVE_V4.md` | |

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

Your part is **Save**: everything that *writes* media, and the way the writers,
the two players, their callbacks, the board commands and the main loop work
**together**. Section 5 says exactly which files, and how far you may go into
the other parts.

For this part, add to the list above:

- Sound is produced by I2S + DMA on a circular buffer of two halves. When a
  half has been played an interrupt fires, and that half has to be refilled
  before the DMA comes round again. Depending on the player and its
  configuration the refill runs in the interrupt, in the main loop
  (`Simple*_Poll`), or from `FM_Idle()`, a hook that long operations (a picture
  decode, a JPEG encode) call so that sound does not break. **Establish for
  yourself in which context each callback of each player runs**; much of this
  audit depends on it, and the comments in the headers make claims about it
  that you should check.
- The SD card is slow and uneven: a write usually takes a few milliseconds and
  sometimes several hundred.

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

## 4. What happened before, what changed, and your jobs

```bash
git show --stat 48a00bc                 # the fixes of the first audit: read this diff line by line
git diff 5d66e46..523d2da --stat        # the code the first audit saw (37 files, about 11 400 added lines)
git show --stat c688b9e                 # the callback layer under it
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
For your part the references are ffmpeg / ffprobe, `flac -t`, a JPEG decoder
other than this library's, and a media player (VLC, mpv). As far as the
repository records, **no file written by this code has ever been opened by any
of them.**

### How it fits together

```
Library layer (src/, include/)
  AudioFileWriter_Open / WriteFrames / Close      PCM16 -> .wav (PCM or IMA ADPCM) / .flac / plugin
                                                  checkpoint every FM_AUDIO_ENC_SYNC_BYTES (256 KB)
  Audio_Convert                                   file -> your callback -> file
  Image_EncodeJPEGEx                              pixels -> baseline JPEG (quality, 4:4:4 or 4:2:0, swap16)
  VideoFileWriter_Open / WriteFrame / WriteFrameSwap16 / WriteJpeg / RepeatFrame / WriteAudio / Close
                                                  -> AVI 1.0, MJPG pictures, optional PCM16 track,
                                                     index in a side file "<name>.idx" until Close,
                                                     checkpoint every FM_VIDEO_ENC_SYNC_BYTES (1 MB)
  VideoFile_Repair                                a file that was never closed + its ".idx" -> a complete file
  Video_Convert                                   any playable video -> MJPEG AVI

Player layer (top_api/simple_audio.h, top_api/simple_video.h, top_api/audio_dma_player.h)
  music:  decoder -> DATA callback -> save callback -> volume -> MONITOR callback -> DMA
  SimpleAudio_RecStart / RecWrite / RecStop       your own samples -> file
  SimpleAudio_RecPlaying(dest)                    the library saves what is playing
                                                  (folder: one file per track, name_2, name_3 for repeats)
  SimpleAudio_Convert
  video:  frame callback (every presented frame), sound DATA / MONITOR callbacks (per I2S half)
  SimpleVideo_RecStart / RecWrite / RecWritePixels / RecWritePlayerFrame / RecWriteJpeg /
          RecWriteAudio / RecRepeat / RecStop     -> .avi (anything else: raw FMRV, the older path)
  SimpleVideo_SeekCount, SimpleVideoInfo.seek_count / .seek_sound_ms    tell a recorder that the user jumped
  SimpleVideo_Convert

Example layer (top_api/examples_save_media.h)
  SaveDemo_AudioPlain, SaveDemo_VideoPlain        style 1: data made by the example
  SaveDemo_MusicCallback  -> ring in RAM -> SaveDemo_MusicRecPump -> SimpleAudio_RecWrite
  SaveDemo_VideoFrameCallback -> SimpleVideo_RecWritePlayerFrame (JPEG encode + file write, in the callback)
                              -> at a seek: save_demo_video_cut (the file is cut and goes on)
  SaveDemo_VideoSoundCallback -> ring in RAM -> SaveDemo_VideoRecPump -> SimpleVideo_RecWriteAudio
  SaveDemo_MusicFromCallback, SaveDemo_VideoFromCallback   the same, start to end in one function

Board layer (board/app_save_hooks.h, board/app_flow.h, top_api/code_cmd.h, Core/Src/main.c)
  the board's own callbacks (loudness normaliser, frame counter) chained in front of the SaveDemo ones;
  UART commands arec / arecs (music) and vrec / vrecs (video); two Pump calls in while (1)
```

### What round V3 found, and what the fixes claim

The report of that round is in the repository: `ReportV4.txt`, lines 1154–1904 (four of the five reports
are in that file; the harnesses it names are not in the repository). Read it, then the diff of the fixes:

```bash
git diff fc31b25..6fe596d -- Makefile tools/check_flag_stamp.sh tools/check_deps.sh tools/save_examples_host_test.c \
    top_api/simple_video.h top_api/usb_msc.h test/usb_msc_host_test.c include/fm_audio.h docs/GUIDE_FA.md README.md \
    ../Core/Src/main.c ../USB_DEVICE/Target/usbd_conf.c ../FATFS/Target/ffconf.h tools/check_board.sh
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. "Mutation: yes" means the maintainers
removed the fix and saw the named check fail.

| ID | What was wrong | What the fix claims | Mutation |
|---|---|---|---|
| SAV3-01 | `tools/check_flag_stamp.sh` said `OK` when an object rule of the `Makefile` lost the flags stamp as a prerequisite (it asked the archive, which is out of date on its own account) | the script asks one object of every object rule. Together with VID5-02 of the video part (`-MMD -MP`, `tools/check_deps.sh`) | yes |
| SAV3-02 | the texts promised more than a cut recording keeps (before the first checkpoint, FLAC) | `include/fm_audio.h`, `README.md` ("Saving: what is on the card when it goes wrong"), `docs/GUIDE_FA.md` now say: before the first checkpoint (256 KB) a cut recording plays to the end of the file; after one, up to the last checkpoint; a cut FLAC plays every whole frame. **Texts only**: check them against the code | n/a |
| SAV3-03 | `fm_saveexamplestest` run from another directory than the library root failed six checks of section `[7]` and did not say that `/test/vectors` was missing | one check "the clips of [7] are there" with a NOTE that names the cause; `make saveexamplestest` is 85 checks | yes |
| SAV3-04 | `SimpleAudio` recording writes to the card **inside the I2S half-buffer refill** (`simple_audio_rec_tap` reached from the DMA refill path, and through `FM_Idle` from the decoders); the auditor counted 24 card writes inside `FM_Idle` and 0 outside | **not fixed.** A warning in `docs/GUIDE_FA.md` only. Whether it is audible needs a board. See lead L13 | n/a |

Fixes of the file part that touch board glue you own or use:

| ID (file part) | What | Where |
|---|---|---|
| CRIT-01 | `UsbMsc_StorageWrite` overran its 4096-byte bounce buffer | `top_api/usb_msc.h` (both twins), `test/usb_msc_host_test.c` (`make usbmsctest`, 48 checks) |
| HIGH-08 | USB interrupt at the priority of the tick | `Core/Src/main.c`, `USB_DEVICE/Target/usbd_conf.c`, `tools/check_board.sh`; **not compiled** |
| HIGH-02 | `_FS_LOCK 2` | `FATFS/Target/ffconf.h:230` is 16; the recorder that copies or converts while a file is open needed more than 2 |
| stack | 1 KB of diagnostic buffers on the interrupt stack in `UsbMsc_StorageRead` | static in `UsbMsc_Lba0Probe` (not re-entrant: it runs in one interrupt only; is that true when `UsbSd_Poll()` is used as well?) |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. SAV3-04 is the open item of this part. What is the longest card write the recorder issues from the refill (a checkpoint: header patch + `File_Flush`), against the time one I2S half lasts at 44.1 / 48 kHz with the ring sizes of `top_api/audio_dma_player.h`? Can the write be moved out of the refill (a flag, the write from `SimpleAudio_Poll` / the main loop) without losing samples, and what does the same question give for the video recorder?
2. The names `a_1.wav` … `a_999.wav`: after 999 the recorder returns `FM_ERR_EXISTS` (now in the guide). What does the UI of the examples do with that code?
3. The V3 report did not examine the leads W2, W6, L2, L3, L4, L5, L6, L8, L9, L11. They are still open, unchanged, below.
4. Nothing of the firmware side of this round was compiled for the target or run on a board.


### What round V2 found, and what the fixes claim

The report of that round is in the repository: `ReportV3.txt`, lines 2076–2501 (the five reports are in one
file; the harnesses the auditors wrote are not in the repository). Read it, then
the diff of the fixes:

```bash
git diff 48a00bc..fc31b25 -- test/audio_save_tests.h test/video_save_tests.h tools/save_examples_host_test.c \
    tools/check_matrix.sh src/fm_video_enc.c docs/GUIDE_FA.md README.md
```

These rows belong to Job 1 and come first. For each: fixed, partly fixed, not
fixed, or fixed but something else broke. The tests are in
`test/audit_v4_tests.h` unless the row says otherwise; remove the fix locally
and see whether the test fails.

| ID | What was wrong | What the fix claims |
|---|---|---|
| SAV2-02 | the "never closed" tests failed when the documented `..._ENC_SYNC_BYTES = 0` was used | both tests follow the switch (`ASV_SYNC_BYTES`, `VSV_SYNC_BYTES`); `tools/check_matrix.sh` has a configuration with both at 0 |
| SAV2-03 | the branch `snd_at > pic_at` of `save_demo_video_cut` was reached by no check | section `[5b]` of `tools/save_examples_host_test.c` demands the exact figures of the slow-player case (4 in sync, 2 silent, 43 392 sound frames); with the branch switched off the new check fails |
| section 8 of the report | removing the `File_Flush` of a checkpoint failed no test | the "never closed" tests write through a counting mount and demand the flushes: each of the four flush sites (`src/fm_audio_enc.c` WAV, ADPCM, FLAC; `src/fm_video_enc.c`) now fails a test when removed |
| section 12 of the report | the checkpoint distance is in bytes; at 8 kHz mono ADPCM it is 64 s | stated in `README.md`, `docs/GUIDE_FA.md`, `docs/OPTIONS_FA.md`, `include/fm_audio.h`. The default was not changed |

Open questions the maintainers have about their own fixes (they are questions,
not findings; nobody has an answer):

1. The "never closed" tests still end with a `File_Close` of the handle; what they prove about a power cut is the flush count and nothing below it. A cut below `File_Write` (a torn sector, a FAT whose directory entry was not updated, LittleFS / FMFS write ordering) is still not modelled. The file part has a RAM disk whose `sync` can be switched off (`av4_power_cut` in `test/audit_v4_tests.h`): use it under the writers.
2. Should the checkpoint distance be time (or a number of blocks) and not bytes? At low rates a default of 256 KB loses a minute. Measure what a checkpoint costs on the playback path before you propose a number.
3. `Video_Convert` now writes the exact rate of the source (`fps` / `fps_den` of the writer) and paces the sound with it. Check the written header against `ffprobe` for 29.97, 23.976, 12.5 fps and an MKV source, and the sound length against the picture length.
4. The size arithmetic near 2 GB / 4 GB has still not been run: the suite passes as a 32-bit program, but no test writes a file of that size.
5. Job 4 of the last round (callback contexts, re-entrancy, the board command sequences, persistence across runs) was read only. It is the least examined area of this part.


### Job 1 — check the fixes of the first audit (do this first)

Eight findings, one fixing commit, written in one pass: that is exactly when
new defects get in. For each, decide: fixed, partly fixed, not fixed, or fixed
but something else broke. Vary the input beyond the reported case. Read the
test that was added, then remove the fix locally and see whether the test
fails.

The text of the first report is not in the repository. The column "what was
wrong" is the maintainers' short reconstruction; the column "what the fix
claims" is exact, and is what you judge.

| ID | What was wrong | What the fix claims | Where; test |
|---|---|---|---|
| SAVE1-F1 | A seek while the playing video is saved through its callbacks (`vrec`): the jump in frame numbers was filled with repeated pictures while sound of the old place was still on its way, so picture and sound of the file drifted apart | The file is **cut** at the seek and goes on: the sound of before the seek is written, the last picture is held until both tracks have the same length, then the new place follows with its own sound. A seek is recognised by `SimpleVideo_SeekCount()`, not by the frame number | `save_demo_video_cut`, `save_demo_video_drain`, `SaveDemo_VideoFrameCallback` in `top_api/examples_save_media.h`; `sv_seek_count`, `sv_seek_sound_ms`, `sv_sound_sync_ms` in `top_api/simple_video.h`. Test: section `[5b]` of `tools/save_examples_host_test.c` |
| SAVE1-F2 | `SimpleAudio_RecPlaying("/folder")` overwrote the file of an earlier track with the same name | Never overwrites a file it did not write in the same session: `name_2`, `name_3`, ... up to 999, then `FM_ERR_EXISTS`; the names of the last `SIMPLE_AUDIO_REC_NAMES` (48) tracks are remembered | `top_api/simple_audio.h` (search `done_key`, `SIMPLE_AUDIO_REC_NAMES`). Test: `tools/audio_record_host_test.c` |
| SAVE1-F3 | A recording that is never closed (reset, power loss) was unplayable | Checkpoints: every `FM_AUDIO_ENC_SYNC_BYTES` (262 144) / `FM_VIDEO_ENC_SYNC_BYTES` (1 048 576) bytes the header lengths are brought up to date and `File_Flush` is called (0 switches it off). A WAV / ADPCM file plays up to that point; a FLAC file is there with "total samples = 0" in its header; an AVI is valid without index and this library opens it. New public `VideoFile_Repair(path, &frames)` completes the AVI from `<path>.idx` (`FM_ERR_NOT_FOUND` without it) | `src/fm_audio_enc.c:187`, `src/fm_video_enc.c:492` and `:984`. Tests: "never closed -> ..." and "VideoFile_Repair ..." in `test/audio_save_tests.h`, `test/video_save_tests.h` |
| SAVE1-F4 | `Close` after a write error: what it returned and what it left on the card | The first error stays the answer of every later call and of `Close`; `Close` still completes the file with what reached the card (a partial sample frame or ADPCM block is cut off; the AVI gets its index if there is room, otherwise it stays valid without one) | both encoder sources, the `Close` functions. Tests: "card full ..." in both test headers |
| SAVE1-F5 | A second `Open` on a writer that still had a file open | `FM_ERR_BUSY`, the first file untouched (an open writer is told apart from a never-used structure by a `guard` field) | `src/fm_audio_enc.c:1994`, `src/fm_video_enc.c:395`. Tests: "a second Open ..." |
| SAVE1-F6 | `make` with another switch linked stale objects | flags stamp `.fm_build_flags`; objects depend on it | `Makefile` (search `FLAGS_STAMP`), `tools/check_flag_stamp.sh` |
| SAVE1-F7 | `#if FM_BUFFERS == HUGE` broke when another header defined `HUGE` | the level is computed in `include/fm_buffers_level.h` behind `push_macro`; the rest uses `FM_BUFFERS_LEVEL` 1..4 | `tools/check_switch_names.sh` (13 cases) |
| SAVE1-F8 | The tests read written files back with the library's own readers only | tests that check every header field **byte by byte**, on a file system that runs full at every point | `asv_raw_wav`, `vsv_raw_check` in the two test headers |

What the maintainers already tried on these fixes (mutations of the code, then
the tests):

| Change | Result |
|---|---|
| AVI index offsets shifted | a test fails |
| ADPCM `fact` chunk left 0 | a test fails |
| the whole seek branch of `SaveDemo_VideoFrameCallback` switched off | 11 checks of `[5b]` fail |
| `pic += 0u` instead of the extra silence in `save_demo_video_cut` | 2 checks fail |
| the condition forced to false, `if (0 && snd_at > pic_at)`, in `save_demo_video_cut` | 1 check fails ("the sound that starts late is put behind its silence"). At `48a00bc` nothing failed: lead W1 of that round, SAV2-03 |

Reference output of `./fm_saveexamplestest`, section `[5b]`, at `6fe596d`
(the same figures as at `48a00bc`)
(your build must print the same before you change anything):
forward seek 19 slots / 13 new pictures / 6 held / 13 in sync / 30 400 sound
frames; backward seek 37 / 29 / 8 / 29 / 59 200; slow player with a seek 29
slots / 6 new / 23 held / 4 in sync / 2 silent / 43 392; no picture with the
wrong sound in any of the three.

### Job 2 — is what gets written right? Compare with reference tools

Before anything else in the rest of your time, write files with every writer
and judge them with tools that share no code with this library:

- **WAV / ADPCM**: `ffprobe` fields (rate, channels, bits, block align, byte
  rate, duration, `fact` chunk for ADPCM); samples against the input (PCM:
  identical; ADPCM: ffmpeg's decode identical to this library's decode, and
  the error against the input what IMA ADPCM should give).
- **FLAC**: `flac -t` (it verifies the MD5 in STREAMINFO — is one written, and
  is it right?), `ffmpeg` decode identical to the input for every sample; mono
  and stereo; rates 8 k to 48 k and above; lengths of 0, 1, block − 1, block,
  block + 1 frames; silence, full-scale square wave, white noise at full scale
  (the fixed predictors and the Rice parameters at their limits; residuals that
  do not fit, wasted bits, the escape code), a block of `INT16_MIN`.
- **JPEG**: decode with libjpeg / Pillow and compare with the source pixels
  (PSNR per quality step; colour); every pixel format; widths and heights that
  are not multiples of 8 and 16, 1 × 1, 1 × N, N × 1; quality 1, 50, 100;
  `swap16` on and off. (The Image auditor examines this encoder in depth; you
  need it for the pictures inside your video files.)
- **AVI**: `ffprobe -show_streams -show_format`, `ffmpeg -v error -i x.avi -f
  null -`, frame count, duration, frame rate (also 30000 / 1001),
  `dwSuggestedBufferSize`, `idx1` offsets (relative to what?), the padding byte
  of odd chunks, the `strf` of both streams, lengths in `avih` / `strh`, the
  sound track sample for sample, pictures against the source. Files with 0
  frames, 1 frame, sound only before the first picture, no sound, 10 000
  frames. Repeated frames (`RepeatFrame`): do ffmpeg, VLC and this library all
  show them as repeats with the right timing?
- **The files of Job 1**, which exist in no test as files a player sees: a WAV,
  an ADPCM, a FLAC and an AVI **cut at a checkpoint** (kill the process, do not
  close anything — see lead W2), the same AVI after `VideoFile_Repair`, a file
  completed by `Close` after "card full", and an AVI recorded across a seek
  (`[5b]`): what do ffprobe, ffmpeg and a player make of each? For the FLAC
  with "total samples = 0": does `flac -t` accept it, and does a player show a
  length?

### Job 3 — the new code, line by line

`git show 48a00bc` first (it is the newest and least read), then the rest of
section 5. Things that typically go wrong in code like this:

- **Checkpoints** (`FM_AUDIO_ENC_SYNC_BYTES`, `FM_VIDEO_ENC_SYNC_BYTES`): the
  header is patched by a seek back and a seek to the end, in the middle of a
  recording, possibly inside a callback. What is on the card if the power goes
  **between** the header patch and the flush, or half way through the patch?
  How long does a checkpoint take on a slow card, and what happens to the
  sound ring meanwhile? Values of 1, of one sector, of 0, of more than the
  file ever gets. For ADPCM and FLAC: a checkpoint in the middle of a block.
- **`VideoFile_Repair`**: an `.idx` that is shorter or longer than the file, of
  another file, empty, with an entry that points past the end, with 10⁶
  entries; a file that was closed properly (must be left alone); a file that is
  not an AVI; a repair that is itself interrupted (run it again: same result?);
  a repair when the card is full; a repair while the file is open elsewhere.
  It rewrites a file in place from a side file that it trusts: treat the
  `.idx` as untrusted input and fuzz the pair.
- **The cut at a seek** (`save_demo_video_cut`): several seeks in a row before
  the next frame callback; a seek while paused; a seek to 0, past the end, to
  the same place; a seek on a video without sound; a seek while the ring is
  full or has dropped samples; a seek at the very first and the very last
  frame; a seek and then `vrecs` at once. The code computes with a fixed
  factor of 2 samples per sound frame (`n / 2u`, `silence[2u * 64u]`,
  `* 2u`): establish what the sound callback of the video player delivers for
  a mono track and whether the factor is right there.
- **Numbered names** (`SimpleAudio_RecPlaying` in folder mode): 48 remembered
  names and then the 49th track; two tracks whose names differ only in case or
  in the extension; a name at `FM_PATH_MAX` before the `_2` is added; `_999`
  exists; the same folder used by two sessions; the destination folder is the
  folder being played.
- **Error paths of the writers.** A write that fails in the middle, a volume
  that fills up, an allocation that fails at each point (every Nth, for every
  N), `Close` after a failure, `Close` twice, `WriteFrames` after `Close`, a
  structure that was never opened or holds garbage (the `guard` field: what if
  the garbage happens to equal the guard value?). Afterwards: is every handle
  and every allocation released (loop 1000 times), is the `.idx` side file
  gone when it should be, what is left on the card, and does each promise of
  the header comment and of `README.md`, "Saving: what is on the card when it
  goes wrong", hold?
- **Size arithmetic on 32 bits**: byte counts of a recording near 2 GB and
  4 GB (WAV, the AVI limit `FM_VIDEO_ENC_MAX_BYTES`, the `.idx` file, frame
  counters, `uint32_t` sample counts at 48 kHz for hours), `width * height * 3`,
  the "a frame never needs more than its raw size and a header" assumption in
  `vw_write_frame` for tiny or noisy pictures at quality 100; the
  milliseconds-to-samples products in `save_demo_video_cut`.
- **Buffering**: the output buffers that write whole sectors
  (`FM_AUDIO_ENC_OUT_BYTES`, `FM_VIDEO_ENC_OUT_BYTES`), the fallback when the
  allocation is halved, a frame larger than the buffer, the seek back to patch
  headers, alignment for DMA.
- **The converters**: cancel from the progress callback at every point; a
  source that fails half way; source and destination the same file; a
  destination in a folder that does not exist; a process callback that changes
  nothing / everything; the frame-rate rounding; sound longer or shorter than
  the pictures.
- **The plugin encoder interface** (`Audio_RegisterEncoder`,
  `FM_AudioEncoderOps`): an encoder that uses fewer frames than given, zero
  frames for ever (hang?), fails in `init`, `write`, `finish`; "close is called
  exactly once after a successful init"; replacing an encoder while a writer
  uses it.
- **`RepeatFrame`** reads the last picture back from the card and writes it
  again: with a picture larger than the buffer, with the picture still partly
  in the unwritten buffer, after a failed write, right after a checkpoint,
  `count` of 0 and of 10⁶.

### Job 4 — do the parts work together? (the main point of this part)

Every piece has a test of its own. What has been tested least is the
combination, on the timing of a real board. Build harnesses that put the parts
under the load they will see, and try the sequences a user will really type.
`tools/save_examples_host_test.c` is the model: it mounts a RAM disk, creates a
source video, raises the I2S half / full interrupts (`AudioDma_HalfCallback`,
`AudioDma_FullCallback` for music; `sv_i2s_half`, `sv_i2s_full` for video,
driven from a hook on `HAL_GetTick`), and makes the simulated player fast or
slow.

1. **Contexts and re-entrancy.** For each player, list where the DATA callback,
   the save callback, the MONITOR callback and the frame callback can run
   (interrupt, `Poll`, `FM_Idle` inside another module's long operation). Then
   check every function reachable from there. In particular
   `SimpleAudio_RecPlaying` writes to the file from the save callback: can that
   be entered from `FM_Idle` while the main loop is inside another file-system
   call, inside the JPEG encoder of a `vrec`, or inside a `RecWrite` of the
   same writer? What is the stack depth of the deepest such nesting (picture
   decode → `FM_Idle` → audio decode → FLAC encode → file system)?
2. **The rings of the example** (`SaveDemoRing`: one writer, one reader,
   `volatile` indices, no lock). Is it correct if the writer really is an
   interrupt? On 32 bits? When it is full, when `n` is larger than the ring,
   when it is closed (`FM_Free`) while a callback is half way through a `put`?
   `armed` is set last in `RecStart` and cleared first in `RecStop`: is that
   enough against a callback that fires in between?
3. **Ring sizes against real timing.** The music ring is 70 000 samples and one
   I2S half is up to 32 766; the video ring is 40 000 and one half is 16 384:
   a little more than two halves in both cases. The pump runs once per main
   loop pass. What is the longest a pass can take on the board with the
   slideshow decoding a large JPEG, a video frame being decoded and then
   JPEG-encoded for `vrec`, and a slow card write, and how much sound arrives
   in that time? Simulate it (the host test can make a pass take any amount of
   simulated time) and find the point where samples are dropped. Dropped
   samples are counted and logged; is the *file* still consistent afterwards
   (does the picture track keep its length while the sound track loses time?).
4. **Picture and sound of one file.** `SaveDemo_VideoFrameCallback` numbers the
   file's frames by the frame index of the source, fills gaps with
   `RecRepeat`, and cuts the file at a seek (SAVE1-F1); the sound goes in as it
   is decoded. Check the sync of the
   result (a source with a click and a flash at known times) when: recording
   starts before playback; recording starts in the middle (the comment admits
   the sound leads by up to one I2S buffer — measure it); the player drops
   frames; the player is **paused and resumed**; the user **seeks** forward and
   backward; playback speed / frame-rate changes if the player has such a
   thing; the playlist moves to the next video; autoplay wraps to the first
   video again; the video has no sound; the sound output is switched
   (`vout0`…`vout3`) during the recording; the video ends while the ring still
   holds sound.
5. **Music.** `arec` before `start`, during a track, across a track change with
   the same and with another sample rate, across `next` / `prev` / `pause` /
   `resume` / `stop` / a seek, with `aout1` (no callback: what does the user get
   and is he told?), with the normaliser on. `SimpleAudio_RecPlaying` and the
   callback example **at the same time**; `RecStart` (own samples) while
   `RecPlaying` is active; `rectest` (the older recorder of the board,
   `Music_RecTest` in `board/app_audio_hooks.h`) together with `arec` — do they
   share the one writer of `SimpleAudio`, and what happens?
6. **Music and video.** The board plays either music or video and switches with
   UART commands. `arec` then `vstrt`; `vrec` then `start`; `vrec` while the
   slideshow runs; `vrecs` / `arecs` with nothing started; both recordings
   armed and the player stopped; `sdpc` (the card is handed to the PC over USB)
   while a recording is open — is the file closed first or is the card pulled
   from under an open writer?
7. **The callback chain of the board.** `App_SaveHooksInstall()` replaces the
   callbacks that `Music_InstallTaps()` and `Music_InstallVideoHooks()` have
   just installed. Find every place that installs a callback (`grep -n
   "Set.*Callback" board/ top_api/code_cmd.h Core/Src`) and every command that
   can run after it: is there an order of commands after which the chain is
   gone (recording silently produces nothing) or the normaliser is gone? The
   normaliser now also runs on the sound of a video started at boot; before it
   ran only after `vstrt`. Is anything else different for a user who never
   types `arec` / `vrec`?
8. **Persistence.** The callbacks survive `Stop` / `Begin`. The example keeps
   state in static structures. Run every sequence twice, and after a failure
   (card removed, allocation failed), and see whether the second run starts
   clean.

### Open leads — an answer to each is wanted in the report

These were the leads of the last round. The answers of that round's auditor
are in `ReportV3.txt`, lines 2076–2501 (section 7 of that report); what the
maintainers did with them is noted under each. Give your own answer where a
lead is still open:

- **L13. Card writes inside the audio refill (SAV3-04 of the last round, not
  fixed).** `simple_audio_rec_tap` in `top_api/simple_audio.h` writes the recorded
  block to the card from the path that refills an I2S half buffer
  (`top_api/audio_dma_player.h`), and is reached a second way through
  `FM_Idle` from inside the decoders. The V3 auditor counted, on the host, 24
  card writes inside `FM_Idle` and none outside. On the host that costs
  nothing; on the board a card write can take longer than a half buffer
  lasts. Establish, with numbers: the worst write the recorder issues there
  (block size, the checkpoint with its header patch and `File_Flush`), the
  time budget of one half buffer, what the player does when the refill is
  late (repeat, silence, a count the application can read?), and a change
  that takes the write out of the refill. If you have no board, say what you
  measured on the host and what remains a calculation.
- **W1. An untested branch.** In `save_demo_video_cut` the branch
  `if (snd_at > pic_at)` (the sound of the new place starts later than its
  picture: extra silence) can be switched off (`if (0 && snd_at > pic_at)`)
  and `make saveexamplestest` still passes (the slow-player case then reports
  3 in sync / 3 silent / 32 000 sound frames instead of 4 / 2 / 43 392, and
  the check is `in_sync >= 3`).
  Is the branch right, and which test would hold it?
  *Since `6fe596d`:* a check on the exact figures holds it (SAV2-03). Whether
  the branch is **right** has still been judged by nobody outside: the figures
  are what the code produces, not what a reference says.
- **W2. "Never closed" is modelled by a close.** The tests for SAVE1-F3 drop
  the writer and call `File_Close` on its file handle (`asv_abandon`,
  `vsv_abandon`). A close writes the directory entry and whatever the file
  system buffered; a reset does neither. What is really on the medium after a
  reset depends on what `File_Flush` guarantees on FatFS (`f_sync`), LittleFS
  and FMFS. Build the real experiment: stop the storage at block level right
  after a checkpoint, at every block write between two checkpoints, and half
  way through the header patch; remount; open the file with this library and
  with ffmpeg.
  *Since `6fe596d`:* the last auditor cut at every `File_Write` (harness not in
  the repository) and found the documented contract kept; the tests now count
  the flushes of the checkpoints. The block-level experiment is still not
  built: this lead is open.
- **W3. A slow player with a seek** cannot give every picture its own sound:
  the first picture after the seek is silent, and the sound track of the file
  can be up to about 0.19 s shorter than its picture track. Is that the best
  that can be done, and is the user told (`docs/GUIDE_FA.md`, the comment
  above `save_demo_video_cut`)?
  *Last round:* measured, judged not a defect.
- **W4. Nothing has been played back.** No file of this part was opened with
  an external tool, nothing ran on the board, on a real card, or was built
  with `arm-none-eabi-gcc` (Job 2).
  *Last round:* the auditor opened every writer's files with ffmpeg / ffprobe
  (WAV and FLAC byte-identical, ADPCM identical to ffmpeg's decode, AVI
  clean). Still open: `flac -t` and the STREAMINFO MD5, the board, a real
  card, `arm-none-eabi-gcc`.
- **W5. The documentation after the fixes.** `docs/GUIDE_FA.md` was written for
  `523d2da` and patched for `48a00bc`. Look for sentences that describe the
  old behaviour (that saving a playing video across a seek is not possible,
  that an unclosed AVI cannot be opened), and check whether `VideoFile_Repair`,
  `SimpleVideo_SeekCount`, the limit of 999 numbered names with
  `FM_ERR_EXISTS` are documented, and whether the callback signatures in the
  guide are those of the headers (`SimpleVideo_TrackStartFn` in particular).
- **W6. The board callback chain.** `App_SaveHooksInstall()` is called after
  every `Music_InstallTaps()` / `Music_InstallVideoHooks()` in
  `board/app_flow.h` and replaces what they installed, chaining to it. Is
  there a command order after which the chain or the normaliser is gone?

From the author's own review of `523d2da` (the first report's answers are not
in the repository; treat each as open):

- **L2. Ring margins.** The music ring is 70 000 samples and one I2S half is
  up to 32 766; the video ring is 40 000 and one half is 16 384. Sized for "two
  halves and a bit"; nobody has measured a main-loop pass on the board. A
  checkpoint (SAVE1-F3) now adds a seek, a header write and a flush to some
  passes.
- **L3. JPEG encode time.** The encoder is plain C. For 320 × 240 on a
  Cortex-M7 the author guesses tens to hundreds of milliseconds per frame, so
  `vrec` may turn most pictures into repeats. Nobody measured it. Estimate it
  (instruction counts from a cross-compiled build, with your reasoning) and
  say what frame rate a user should expect. The board has a hardware JPEG
  codec (`top_api/hw_jpeg.h`, used for decoding); the encoder does not use it.
- **L4. `FM_JpegEncodeOptions.swap16`** was added to a public structure.
  Callers that fill `quality` and `subsample` without clearing the structure
  pass garbage in `swap16`. The header says "clear it first". Is there a
  design that cannot be misused this way?
- **L5. Three ways to write a frame** (`RecWrite` RGB888, `RecWritePixels`,
  `RecWritePlayerFrame`) and a byte order that depends on a global of the
  player (`SIMPLE_VIDEO_SWAP16`, `SimpleVideo_SetSwap16`). Can a user get wrong
  colours without an error? `SimpleVideo_SaveFrame` and `SimpleImage` have
  frame buffers of the same kind: do their save paths handle the swap?
- **L6. `SimpleVideo_Info()`** is filled in the first frame callback. Does it
  return stale data after `Stop`, between two videos, or after a failed open —
  including the new fields `seek_count` and `seek_sound_ms`?
- **L7. The last sound block** of a video used to be lost for the callbacks;
  now it is delivered. Is the count right for every length (a video whose
  sound ends exactly on a half boundary, one sample before, one after, sound
  longer than the pictures), and is nothing delivered twice?
- **L8. The frame number of the current frame** is taken as
  `SimpleVideo_Info().frame_index - 1`. Is that true in every path (first
  frame, after a seek, after a pause, the hardware-JPEG path, a repeated
  present of the same frame)?
- **L9. The board glue has never executed.** `board/app_save_hooks.h` and the
  changes in `board/app_flow.h`, `top_api/code_cmd.h`, `Core/Src/main.c` were
  only compiled for ARM. `make tapstest` extracts a block of
  `board/app_audio_hooks.h` and does not include the save hooks. Write the
  host test that is missing (stub HAL, the real board headers) and run the
  command sequences of Job 4.
- **L11. Fixed file names.** `arec` and `vrec` always write
  `/save/music_cb.wav` and `/save/video_cb.avi`. What happens to the previous
  file, and what if it is the file being played?
- **L12. Tests that prove less than they seem to.** The host test's source
  video is 64 × 48, 2 s, mono 16 kHz, with pictures that are flat grey levels.
  Flat pictures cannot show a wrong chroma layout, a wrong row order or a
  shifted block; a 2 s clip cannot show drift. Judge every test of this part
  by the one question of section 4, and break things locally to see.

Methods most likely to pay off here: **differential testing against reference
tools** (Job 2), **fault injection** (a write that fails at every point, an
allocation that fails at every point, a card that becomes slow, a stop at
block level), **simulation of the main loop with controlled time** (Job 4),
**32-bit harnesses**, **mutation of the code under test**, and **reading for
state left half-changed on an error path**. Fuzzing matters less here than in
the decoder audits — the writers take trusted input — with two exceptions:
what the converters read, and the `.idx` file that `VideoFile_Repair` trusts.

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
| `src/fm_audio_enc.c`, saving section of `include/fm_audio.h` | 2 400 lines: WAV, IMA ADPCM and FLAC encoders, the encoder registry, `AudioFileWriter_*` with its checkpoints, `Audio_Convert` |
| `src/fm_video_enc.c`, saving section of `include/fm_video.h` | 1 500 lines: AVI writer, `.idx` side file, checkpoints, `RepeatFrame`, `VideoFile_Repair`, `Video_Convert` |
| `src/fm_jpeg.c` (encoder part: `jpeg_row_ycc`, `jpeg_encode`, `jpeg_encode_common`, `Image_EncodeJPEGEx`, `FM_JPEG_EncodeSink` in `src/fm_internal.h`), `include/fm_image.h` | JPEG encoder with quality, 4:2:0, `swap16`, and a sink that writes straight into the video file |
| `top_api/simple_audio.h` (search `Rec`, `Convert`, `simple_audio_rec_`, `done_key`), `top_api/audio_dma_player.h` (`data_cb`, `save_cb`, `monitor_cb`, `audio_dma_fill_half`) | recorder and callbacks of the music player |
| `top_api/simple_video.h` (search `Rec`, `sv_avi`, `Convert`, `sv_i2s_fill_half_inner`, `SimpleVideo_Info`, `sv_swap16_active`, `sv_seek_count`, `sv_sound_sync_ms`) | recorder and callbacks of the video player |
| `top_api/examples_save_media.h` | 1 270 lines: the save examples, the two rings, the cut at a seek |
| `board/app_save_hooks.h`, `board/app_flow.h`, `board/app_audio_hooks.h`, `board/app_video_hooks.h`, `board/app_uart_commands.h`, `top_api/code_cmd.h`, `Core/Src/main.c`, `Core/Src/defines.h` | how the board wires it all: callbacks, commands, the main loop |
| `test/audio_save_tests.h` (19 tests), `test/video_save_tests.h` (19), `tools/save_examples_host_test.c` (74 checks), `tools/audio_record_host_test.c` (99 checks; sections 7, 8), `test/video_hooks_test.c`, `tools/board_taps_host_test.c`, `tools/halstub/` | the tests of this part and the stub HAL they run on |
| `docs/GUIDE_FA.md` sections 7.2, 7.5, 7.6, 8.2–8.5, `board/README.md` (search `arec`), `docs/OPTIONS_FA.md`, `README.md` ("Saving: what is on the card when it goes wrong") | what the user is told (Persian) |

Every header in `top_api/` has a twin in `examples/`; audit one and diff the
other.

Build switches that must each still build and behave: `FM_USE_AUDIO`,
`FM_USE_VIDEO`, `FM_USE_IMAGE`, `FM_USE_JPEG`, `FM_USE_FLAC`, `FM_USE_MP3`,
`SIMPLE_VIDEO_I2S` (0 and 1), `SIMPLE_VIDEO_SWAP16` (0 and 1). With a feature
off the save functions must exist and return `FM_ERR_UNSUPPORTED` (the
application still links). Tunables: `FM_AUDIO_ENC_OUT_BYTES`,
`FM_AUDIO_ENC_SYNC_BYTES`, `FM_FLAC_ENC_BLOCK`, `FM_VIDEO_ENC_OUT_BYTES`,
`FM_VIDEO_ENC_SYNC_BYTES`, `SIMPLE_AUDIO_REC_NAMES`, `FM_VIDEO_ENC_QUALITY`,
`FM_VIDEO_ENC_MAX_BYTES`, `FM_VIDEO_CONVERT_AUDIO_BYTES`, `FM_JPEG_QUALITY`,
`SAVE_DEMO_MUSIC_RING_SAMPLES`, `SAVE_DEMO_VIDEO_RING_SAMPLES`,
`SIMPLE_AUDIO_HALF_SAMPLES`, `SIMPLE_VIDEO_HALF_SAMPLES`: try the extremes the
`#error` checks allow.

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
| `src/fm_file.c`, `backends/fm_fatfs.c`, `backends/fm_littlefs.c`, `src/fm_fmfs.c` (`File_Flush`, seek back and overwrite, `File_Truncate`) | Files | every promise about "what is on the card" rests on them (lead W2) |
| `src/fm_video.c` (AVI reader), `src/fm_audio.c` (WAV / ADPCM), `src/fm_flac.c` | Video, Audio | they read back what you write, also the cut and repaired files; and the converters read every format through them |
| `src/fm_jpeg.c` (encoder) | Image | the pictures inside your video files; the Image auditor examines the encoder itself |
| playback side of `top_api/simple_audio.h`, `top_api/audio_dma_player.h`, `top_api/simple_video.h` | Audio, Video | your recorders hang on their callbacks: in which context each runs, how often, in which order, and what `SimpleVideo_Info()` holds at that moment |
| `top_api/simple_image.h`, `board/app_display.h` | Image | the slideshow runs beside a recording and shares the main loop and the card |
| `include/fm_board_config.h`, `include/fm_buffers_level.h`, `Makefile` | Files | SAVE1-F6 and SAVE1-F7 were fixed there; the Files auditor examines the mechanism, you check the two fixes |

## 6. More places to dig

**`SimpleAudio` recorder.** `RecStart` with a file path and with a folder (the
automatic name `rec_000001.<ext>`: what if 999 999 exist, if the folder does not
exist, if the name is at the path limit); `RecSetFormat`, `RecSetChannels`,
`RecSetBitrate` before and during a recording; mono in / stereo out and the
reverse; `RecStop` without `RecStart`; `RecPath` and `RecActive` in every
state; `RecPlaying` in folder mode across 100 tracks (one file per track: are
all closed, are names unique, what if two tracks have the same base name, what
if the destination folder is the music folder — the guide warns, the code?);
`RecPlaying` in single-file mode across a sample-rate change, across `Stop` /
`Begin`; the claim "what is saved is the sound before the volume, with the DATA
callback's processing only when the output includes the callback".

**The output switch** (`FM_AUDIO_OUT_NONE / SPEAKER / CALLBACK / BOTH`) for both
players, switched while playing and while recording: which callbacks fire, is
the DMA really silent, does the player's clock still advance when the speaker
is off (a video with `vout2` must not run fast), is the saved file the same in
all four positions where it should be.

**`SimpleVideo` recorder.** `RecStart` with width, height or fps 0; with a path
that is not `.avi` (the raw FMRV path: the `Rec*` functions that are AVI-only
must refuse cleanly); `RecSetAudio` after `RecStart`; `RecWriteAudio` on a file
without a sound track; `RecWrite` with a wrong byte count; `RecFrames` after
repeats; `RecStop` twice; recording while the player plays the **same** file
that is being written; `SimpleVideo_Convert` while playing (must be
`FM_ERR_BUSY`) and its memory need for a 480 × 272 source.

**The examples as examples.** They are what users will copy. Read
`top_api/examples_save_media.h` as a user would: does each example do what its
comment says; does the "all in one" function restore everything it changed (the
callbacks, the output switch) on every exit path, including the error paths; is
any advice in the comments wrong or dangerous on a real board; are the stub
versions (feature off) complete.

**Documentation against code.** `docs/GUIDE_FA.md` 7.5–7.6 and 8.4–8.5 and
`board/README.md` make concrete promises (tables of which output position puts
what into the file, what happens at a track change, what `dropped` means, which
commands exist). Check each sentence that states behaviour. A promise the code
does not keep is a finding (S4, or higher if a user loses data by believing
it).

**Log output.** With `./fm_saveexamplestest -v` and at `FM_DEBUG_LEVEL` 0 to 3:
a failure in this part that prints nothing; a message per audio block or per
frame at level 1 or 2 (the refill path must stay quiet); a `printf` reachable
from a callback that can run in an interrupt.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Nothing has run on hardware. All timing statements are estimates.
- Video is written as Motion-JPEG in AVI 1.0 only. No H.264 / MP4 writer. The
  sound track is 16-bit PCM only. A file stops at `FM_VIDEO_ENC_MAX_BYTES`
  (just under 2 GB) with `FM_ERR_TOO_LARGE`.
- MP3, Vorbis and Opus are written only through an encoder the application
  registers; without one `FM_ERR_UNSUPPORTED` and no file.
- FLAC uses fixed predictors only (no LPC), 16-bit, one or two channels.
- `Video_Convert` writes whole-number frame rates (29.97 becomes 30).
- A recording that was never closed is kept only up to its last checkpoint;
  what came after it is lost. A FLAC file in that state has "total samples =
  0" in its header. An AVI in that state has no index until `VideoFile_Repair`
  has run, and `VideoFile_Repair` needs the `.idx` file (`README.md`, "Saving:
  what is on the card when it goes wrong"). Whether these promises hold is in
  scope; that they are not stronger is not a finding.
- `SimpleAudio_RecPlaying` in folder mode numbers at most 999 files of one
  name and remembers the names of the last 48 tracks of a session.
- At a seek during `vrec` the file is cut: the last picture before the seek is
  held for up to about one I2S buffer of sound, and with a slow player the
  first picture after the seek can be silent (lead W3 asks whether it is worse
  than that).
- While a video file is written a second file `<name>.idx` exists next to it.
- The save examples are header files with static state: one recording of each
  kind at a time, included from one `.c` file only (as every header of
  `top_api/`).
- Saving through a callback runs at playback speed; the converters are the
  fast path.
- Recording that starts in the middle of a video: the sound may lead the
  picture by up to one I2S buffer (measure it; report only if it is more).

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's
public API with a concrete input. A defect in how this library calls it is
fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_SAVE_V4.md`. It is used as a work list:
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
4. **One section per confirmed finding**, IDs `SAV4-01`, `SAV4-02`, ...:
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
    | H. Working together | recording, both players, their callbacks, the board commands and the main loop behave correctly in combination and in every order a user can produce |

    For this part read axis A as "no crash, hang or out-of-bounds access for
    any call order, argument or converter input you could construct" and axis
    B as "every file written is what a reference tool says it should be:
    samples, pixels, colours, lengths, timing".
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
