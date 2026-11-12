# External audit V4 — audio

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
- Commit: `48a00bc` (`48a00bc613cac4eb407f99b56c923de187d20544`). Audit exactly
  this commit (`git checkout 48a00bc`) and quote it in your report. Later
  commits on the branch change only the folder `audit_prompts/`.
- Library root: `Clain_FileManager/FileManager_lib/`. Paths below are relative to
  it, except: paths that start with `Core/` are in `Clain_FileManager/Core/`
  (the CubeMX firmware project of the reference board), and `ReportV2.txt`,
  `Report.txt` and `audit_prompts/` are at the repository root.

History of this part:

| Round | Commit audited | Report | Then |
|---|---|---|---|
| first | `8dd0f25` | `ReportV2.txt`, lines 1267–1582 (findings F1–F30) | about 30 findings were fixed in one pass, up to `a6cf829` |
| follow-up ("V3") | `a6cf829` | none in the repository | the prompt was written (`git show f161ff8:audit_prompts/04_audio.md`); no report of it is in the repository, so **nobody outside has checked the fixes of the first round**. Its jobs are carried into this round |
| this one (V4) | `48a00bc` | `AUDIT_AUDIO_V4.md` | |

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

Your part is **Audio**: the decoders, the playlist player, type detection, and
the playback side of the two music players (`SimpleAudio`, `AudioDma`) with
their output switch and callbacks. Section 5 says exactly which files, and how
far you may go into the other parts.

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
   `git show 48a00bc:<path> | sed -n '<from>,<to>p'` before you write it down:
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
make test                 # whole suite: 557 tests at this commit
make asan                 # the same suite under ASan + UBSan (+ LeakSanitizer)
make tsan                 # the suite with FM_USE_THREADS=1 under ThreadSanitizer (562 tests)
                          #   make tsan FM_SYNC_IMPL=freertos | cmsis_rtos2  goes through that port
make m32                  # the suite built and run as a 32-bit x86 program (558 tests);
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

**What is established at `48a00bc`** (the maintainers ran it; running it again
will find nothing):

| Check | Result |
|---|---|
| `make test` | `Results: 557/557 passed, 0 failures` |
| `make check` | exit status 0 (includes the suite under ASan + UBSan, the ports, the FatFS mock, CMake 13 configurations, 32-bit) |
| `make m32`, `make m32 M32=ubsan` | 558/558 both (clang of ziglang 0.17.1, `x86-linux-musl`) |
| `make tsan` (pthread port) | 562/562, 0 ThreadSanitizer warnings |
| `make matrix-quick` | 8 configurations, 0 failed |
| `make matrix` | 53 configurations. 52 passed in the run. The 53rd, `FM_FILE_FORCE_BOUNCE` with `FM_USE_THREADS=1`, failed (`concurrent png (err=-18)`) because a second build was started in the same tree while it was being built; the same failure was then reproduced on purpose that way in a copy of the tree. Built alone, that configuration passes: 562/562 in 20 runs (8 of them with every CPU loaded), and 562/562 with 0 warnings under ThreadSanitizer |
| `make audiorecordtest`, `make saveexamplestest` | 99 and 55 checks pass |
| `Core/Src/main.c`, `src/fm_audio_enc.c`, `src/fm_video_enc.c` compiled for Cortex-M7 (clang, ST HAL headers) | 0 warnings; not linked, not run |

**What is not established** (nobody did it at this commit):

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

Read the first report of this part first (`ReportV2.txt`, lines 1267–1582),
including its sections "Examined and found sound" and "Not examined". Then the
two diffs that matter:

```bash
# the fixes of the first round and the work after it (never checked from outside)
git log --oneline 8dd0f25..a6cf829
git diff 8dd0f25..a6cf829 -- src/fm_audio.c src/fm_flac.c src/fm_vorbis.c src/fm_opus.c src/fm_adpcm.h \
    src/fm_player.c src/fm_media.c third_party/stb include/fm_audio.h include/fm_player.h
# what is new since the follow-up prompt
git show --stat c688b9e                       # output switch, callbacks, music / video parity
git diff a6cf829..48a00bc -- src/fm_audio.c src/fm_flac.c src/fm_player.c include/fm_audio.h include/fm_player.h
git diff a6cf829..48a00bc -- top_api/audio_dma_player.h top_api/simple_audio.h board/app_audio_hooks.h
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
For your part the reference is ffmpeg / ffprobe (and sox): every decoded
sample against ffmpeg's, every duration, frame total, rate and channel count
against ffprobe.

### Job 1 — check the fixes (do this first; it is the cheapest way to find a defect)

For every earlier finding below, decide: fixed, partly fixed, not fixed, or
fixed but something else broke. A fix that handles exactly the reported input
and not its neighbours is "partly fixed": vary the input (other sizes, other
formats, other build switches, 32 bits, threads on, the same mistake in the
sibling function). Read the test that was added for it
(`test/audit_v2_tests.h`), then remove the fix locally and see whether that
test fails.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| F1 | crafted Vorbis comment header corrupted memory in `Audio_Open` | refused with `FM_ERR_CORRUPTED`; `third_party/stb/stb_vorbis.c` is **patched locally** — diff it against upstream 1.22 and audit the patch |
| F2 | MP3 resync advanced one byte at a time | a damaged frame costs about 0.1 s instead of 0.6 s; 2 MiB of non-MP3 data is rejected in milliseconds |
| F3 | unsupported ADPCM variants decoded to noise | stereo IMA ADPCM is bit-identical to ffmpeg. Microsoft ADPCM was refused by that fix and **is decoded since** (`src/fm_adpcm.h`, `wav_adpcm_ms_decode` in `src/fm_audio.c`; two files byte-identical with ffmpeg in `test_wav_adpcm_ms`) |
| F4 | shared WAV buffer used without a lock (`FM_USE_THREADS=1`) | locked |
| F5, F6, F16, F17 | AAC path: sample count not clamped, errors reported as end of track, header zeroed, context leak | all four fixed |
| F7, F19 | Opus 5.1 decoded as stereo noise; header gain ignored | multistream refused; gain applied (both testable only with `FM_USE_OPUS_LIB=1`) |
| F8 | `Player_GetDuration` wrong for six formats | `FM_AudioInfo.total_frames` added and used |
| F9 | `Audio_Seek` with NaN / huge times | refused or clamped; `Player_SeekFwd` / `Player_SeekBack` too |
| F10 | A-law polarity inverted | corrected (**output changed** for every A-law file) |
| F11, F12, F15 | `Audio_ReadMono` on multichannel; reads smaller than a frame; output counts not written | fixed; `Audio_Read` with room for less than one frame returns `FM_ERR_INVALID_ARGUMENT` |
| F13, F25 | example DMA player: odd half-buffer, tap order | fixed in `examples/audio_dma_player.h` and its copy in `top_api/` (**this file was reworked again since**, see Job 2 (b): check that both fixes survived) |
| F14 | FLAC allocation size came from the header | capped by `FM_FLAC_MAX_PCM_BYTES` (40 MiB); Opus files by `FM_OPUS_MAX_FILE_BYTES` (5 MiB) |
| F18 | Vorbis leak on a corrupt setup header | fixed (visible only under ASan / LeakSanitizer) |
| F21, F22, F27 | MP3: Xing frame played as audio; seek offsets | Xing frame skipped; CBR seek exact; VBR with Xing approximate (see section 7) |
| F23, F24, F26 | recorder left empty files; player extension list; media detection | fixed |
| F28 | decoders called `malloc` directly | every decoder allocation goes through `FM_Allocator` (`FM_ReallocSized` added) |
| F30 | undefined behaviour in vendored decoders on corrupt input | fixed in stb_vorbis only; dr_flac see section 7 |
| F20, F29 | chained Ogg; stack use | **not fixed**, documented (section 7) |
| (own) | WAV writer: data above 4 GiB wrapped the header | `AudioWriter_Close` returns `FM_ERR_TOO_LARGE` above `UINT32_MAX − 36` (no test of its own) |

### Job 2 — audit what is new

Code written since the last audit has been through the fewest eyes. Two layers
of it are yours.

**(a) Written before `a6cf829`, never read from outside.** About 30 findings
were fixed in one pass, which is exactly when new defects get in: the MP3
resync and seek rewrite, the IMA ADPCM decoder, the AAC frame path, the local
patch to `stb_vorbis.c`, the allocator routing of every decoder (does an
allocation that fails at each point leave the decoder slot free and the file
closed?), the size caps (off-by-one at exactly the cap, behaviour on 32 bits),
and `total_frames` for every format against ffprobe. Then:

- the **packet-stream Vorbis decoder** in `src/fm_vorbis.c` /
  `include/fm_vorbis.h` (`FM_Vorbis_StreamOpen`, `StreamDecode`,
  `StreamPending`, `StreamReset`, `StreamClose`), which the video layer uses
  for Vorbis tracks in Matroska and MP4. It frames every packet as an Ogg page
  of its own for stb_vorbis's push interface, carries over the bytes the
  decoder did not take, and keeps PCM that did not fit the caller's buffer.
  Check: every allocation failure in open and decode; packets larger than
  64 KiB (several lacing pages); the carry growing without bound on a stream
  the decoder never accepts; `StreamReset` in every state; a header set that
  stb_vorbis accepts but that declares more than two channels; and whether all
  of stb_vorbis's memory really goes through `FM_Alloc`. A second local patch
  to `stb_vorbis.c` (two shifts into the sign bit in the push-data resync)
  belongs to it;
- **Microsoft ADPCM in WAV files** (`src/fm_adpcm.h`, shared with the video
  layer: block header, the seven standard coefficient pairs and a coefficient
  table from the file, `nBlockAlign` too small for one block, a last block cut
  short, seek by blocks);
- the **FLAC wrapper** `src/fm_flac.c` (`FM_Flac_*`): dr_flac is compiled into
  that one file with `DRFLAC_API static` and nothing else may call `drflac_*`;
  a stream wider than 24 bits is refused with `FM_ERR_UNSUPPORTED` before the
  decode, in a `.flac` file and in a video track.

**(b) Written since `a6cf829`** — the playback layer was reworked (`c688b9e`,
then touched by `4cbd8a0`, `523d2da`, `48a00bc`):
`top_api/audio_dma_player.h` (1 119 lines, about 470 changed) and
`top_api/simple_audio.h` (2 345 lines, about 1 100 changed). Its recording
functions (`SimpleAudio_Rec*`, `SimpleAudio_Convert`, `AudioDma_SetSaveCallback`)
belong to the Save auditor; **the playback path is yours**:

- **One output switch** (`FM_AudioOutput` in `include/fm_player.h`;
  `AudioDma_SetOutput`, `SimpleAudio_SetOutput`; UART `aout1`..`aout3`):
  `SPEAKER` skips the callbacks, `CALLBACK` calls them and sends silence to the
  DMA so that data still arrives in real time, `BOTH` (default) does both.
  Check each position, a change during playback (in the middle of a half
  buffer, during a pause, at a track change), `NONE` given to the music
  player, and that the DMA buffer really holds silence, not the last block.
- **One callback type** (`FM_AudioDataFn`, `include/fm_audio.h`) for the DATA
  callback (before the volume; what it changes is heard) and the MONITOR
  callback (after the volume), replacing the older pre / post taps. The order
  in the refill is: decoder → DATA callback → save callback → volume → MONITOR
  callback → DMA. Check: the sample counts each callback gets for mono and
  stereo, at a track change with another rate or channel count, for the last,
  short block of a track, after a seek; that every sample reaches each
  callback exactly once; what a callback may do (the comments say "light work
  only": what happens when it takes 5 ms? when it calls `SimpleAudio_Next`,
  `Stop`, `SetVolume`, or `SetDataCallback(NULL)` on itself?).
- **In which context does each callback run?** The refill runs in the I2S
  half / full interrupt, in `SimpleAudio_Poll` / `AudioDma_Poll`, or from
  `FM_Idle()` (a hook that long operations of other modules call so that sound
  does not break), depending on the configuration. Establish it for each
  player and each configuration, compare with what the comments promise, and
  then look at everything the refill touches that the main loop also touches
  (the player state, the decoder, the file handle, the callback pointers which
  the main loop can change at any time): is each access safe when the refill
  really is an interrupt, on a 32-bit CPU, without a lock?
- **Settings survive `Stop` / `Begin`** (`AudioDma_StartConfig` applies output,
  callbacks, volume and play mode before the first buffer; `SimpleAudio` keeps
  them outside the playback state). Run every sequence twice, and after a
  failure (file missing, allocation failed, card removed), and see whether the
  second run starts clean and with the settings the user left.
- **Track callbacks** (`SetTrackStartCallback`, `SetTrackEndCallback` on both
  players): exactly one start and one end per track — on `Next`, `Prev`, the
  end of the list in each play mode, a track that fails to open, `Stop` in the
  middle, a callback installed while a track is already playing (the code
  fires a start then; check the cases around it: the same callback again,
  another context, NULL).
- **Parity functions**: `SimpleAudio_Start`, `SimpleAudio_GetVolume`, volume
  0..200 (above 100 amplifies: clipping or wrap-around at full scale?),
  `SimpleAudio_Info` with its `running` flag, `SimpleAudio_DataRead` and
  `SimpleAudio_DataReadMono` (UART `dtrd`, `dtrdm`).
- **The board's audio hooks** `board/app_audio_hooks.h` (a loudness normaliser
  installed as the DATA callback, the taps; `make tapstest` extracts one block
  of it and runs that on the PC): gain, clipping and filter arithmetic at full
  scale, at silence, at a rate change, and what the normaliser does to the
  first block after a track change.
- **Removed and renamed API** (`SetPcmTap`, `SetPrePcmTap`, `SetPostPcmTap`,
  `SimpleVideo_ReadAudio` → `SimpleVideo_AudioDataRead`, the identifiers with
  example numbers, commit `dd6239c`): does any document, comment or example
  still tell the user to call something that no longer exists
  (`docs/GUIDE_FA.md`, `board/README.md`, `README.md`,
  `docs/REGRESSION_AUDIO_STUTTER.md`, `docs/DSP_PROCESSING_BUDGET.md`)?
- **In the library**: `src/fm_audio.c` lost its encoder registry and
  `AudioFileWriter_*` (moved to `src/fm_audio_enc.c`, Save auditor) and got
  stubs for `FM_USE_AUDIO=0` (every entry point answers `FM_ERR_UNSUPPORTED`
  and writes its output counts: check each against its real signature and
  against what callers do with the result); `src/fm_player.c` is compiled even
  with audio and video off; `src/fm_flac.c` silences a warning with a pragma
  around the include.

### Job 3 — go where nobody has been

The first auditor of this part was thorough, and left a list of leads that
were **not confirmed and that nobody has examined since**: VBRI headers; a
channel count that changes mid-file; a 14-byte `fmt ` chunk; the ADPCM
allocation derived from `block_align`; the tap recorder
(`top_api/audio_record_with_tap.h`); unregistering a decoder while a stream
that uses it is open; `aac_find_sync` reading one byte per storage call. Start
with these. Then note what could not be tested then: nothing ran on ARM, so
every stack figure was an x86 figure (the frames are measured for Cortex-M7
now, `README.md` "Stack": check them and add the call depth, which is not
there), and several fixes have weak tests (F7 and F19 only in the libopus
build, F4 only with threads, F18 only under ASan, no test for the stb_vorbis
UB fixes or the RIFF size check): try to break exactly those.

### Open leads — an answer to each is wanted in the report

- **W1. A stutter regression is on record.** Read
  `docs/REGRESSION_AUDIO_STUTTER.md` and `docs/DSP_PROCESSING_BUDGET.md`, then
  check whether the rework of `c688b9e` kept what that fix needed (who refills
  when the main loop is busy, how long the refill may take). Simulate a main
  loop that is blocked for 50, 200 and 1000 ms (a slow card, a large picture
  decode) and count underruns for each configuration of the player.
- **W2. Sample-exact playback.** Play each test file through `SimpleAudio` on
  the host with the stub HAL (`tools/audio_record_host_test.c` shows how),
  collect what reaches the DMA buffer in `SPEAKER` at volume 100, and compare
  it with `Audio_Read` of the same file and with ffmpeg: no sample lost or
  doubled at a half-buffer boundary, at a track change, after a seek, after
  pause / resume.
- **W3. The tests of the output switch** (`tools/audio_record_host_test.c`,
  section 6): remove the silence in `CALLBACK`, call a callback twice, skip the
  MONITOR callback — which of these does a test notice?
- **W4. Volume above 100** is new. Find the test that covers it at full
  scale, or show what a full-scale signal does at volume 200.
- **W5. The remaining leads of the first report** (Job 3).

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
| `src/fm_audio.c`, `include/fm_audio.h` | about 3 000 lines: audio API, WAV (PCM, IMA and Microsoft ADPCM, A-law, mu-law), MP3 and AAC front ends, read-ahead buffering, seek, the decoder registry, the older WAV writer (`AudioWriter_*`) and recorder (`AudioRecorder_*`). AAC is ADTS framing only: the decoder is a plugin the application registers |
| `src/fm_flac.c`, `src/fm_vorbis.c`, `src/fm_opus.c` | glue to `third_party/dr_flac`, `third_party/stb`, `third_party/opus` |
| `third_party/minimp3/` | MP3 decoder (vendored) |
| `src/fm_player.c`, `include/fm_player.h` | playlist player: scan folder, play, next, previous, seek, position |
| `src/fm_media.c`, `include/fm_media.h` | file-type detection by content and by extension |
| `examples/simple_audio.h`, `examples/audio_dma_player.h`, `examples/audio_record_with_tap.h`, `examples/simple_i2s_mux.h` | how applications use it (I2S + DMA) |
| `docs/REGRESSION_AUDIO_STUTTER.md`, `docs/DSP_PROCESSING_BUDGET.md` | history of a real stutter bug and the processing budget |
| `tools/board_taps_host_test.c`, `tools/audio_record_host_test.c`, `test/player_examples_host.c` | host tests of the board-side code |

The **demux** of audio tracks inside video files (MP4, MKV, AVI, FLV) belongs
to the video auditor. Yours are the decoders those tracks are handed to, and
everything for stand-alone audio files.

Tests to read first in `test/main.c`: `test_mp3`, `test_mp3_readahead`,
`test_audio_hook`, `test_player`, `test_opus_ogg`, `test_adpcm_small_chunks`,
`test_simple_audio_track_change`, `test_wav_truncated_board_geometry`,
`test_async_and_codecs`.

Build switches: `FM_USE_AUDIO`, `FM_USE_MP3`, `FM_USE_FLAC`, `FM_USE_VORBIS`,
`FM_USE_OPUS`, and `FM_USE_OPUS_LIB=1` (without it Opus files are refused as
unsupported). Limits: `FM_MP3_INPUT_BUF`, `FM_MP3_LOW_WATER`,
`FM_AUDIO_PCM_CHUNK`, `FM_MEDIA_MAGIC_SCAN`.

New in this round (Job 2 (b)): the playback side of
`top_api/audio_dma_player.h` and `top_api/simple_audio.h` (the twins of the
files in `examples/`; the firmware uses the `top_api/` copies) with
`FM_AudioOutput` in `include/fm_player.h` and `FM_AudioDataFn` in
`include/fm_audio.h`; `board/app_audio_hooks.h`; the host tests
`tools/audio_record_host_test.c` (`make audiorecordtest`, 99 checks; its
section 6 is the output switch and persistence) and
`tools/board_taps_host_test.c` (`make tapstest`). The encoders and
`AudioFileWriter_*` (`src/fm_audio_enc.c`), `Audio_Convert` and the
`SimpleAudio_Rec*` functions belong to the Save auditor; the older
`AudioWriter_*` and `AudioRecorder_*` in `src/fm_audio.c` stay yours.

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
| `top_api/simple_audio.h` (`SimpleAudio_Rec*`, `RecPlaying`, `Convert`), `top_api/audio_dma_player.h` (`save_fn`) | Save | the recorder hangs in your refill, between the DATA callback and the volume, and writes to the card from there: what it assumes about context and timing is your contract |
| `src/fm_audio_enc.c` (`AudioFileWriter_*`, `Audio_Convert`) | Save | writes WAV / ADPCM / FLAC files your decoders must read back, also when cut at a checkpoint; `Audio_Convert` reads every format through your API at full speed |
| `src/fm_video.c` (`Video_ReadAudioFrame` and below) | Video | hands packets to your decoders (`FM_Vorbis_Stream*`, `FM_Flac_*`, minimp3, `src/fm_adpcm.h`): cut, empty, repeated packets |
| `top_api/simple_video.h` (its I2S path), `top_api/simple_i2s_mux.h`, `top_api/simple_idle_mux.h` | Video | the I2S output and the `FM_Idle` hook are shared between the music player and the video player: who owns them when the board switches from one to the other, and back |
| `src/fm_file.c` (reads) | Files | a decoder that turns a read error into "end of track" (F6 was that) |
| `board/app_flow.h`, `board/app_save_hooks.h`, `top_api/code_cmd.h` | Save (as glue) | the order in which the board installs callbacks: `App_SaveHooksInstall()` replaces what `Music_InstallTaps()` installed and chains to it |

## 6. Where to dig

**Fuzz every format** through the public API: `Audio_Open`, `Audio_GetInfo`,
then a fuzzer-chosen mix of `Audio_Read`, `Audio_ReadFrame`, `Audio_ReadMono`,
`Audio_ReadStereo` with buffer sizes from 1 sample upward, `Audio_Seek`, then
`Audio_Close`. One harness per format (WAV, MP3, AAC/ADTS, FLAC, Ogg Vorbis,
Ogg Opus), seeded from `test/vectors/` and from files made with ffmpeg and sox.
Also truncate each valid file at every byte offset. Use a short per-input
timeout: a decoder that spins on garbage is a finding.

**Differential decoding.** For each format decode a set of real files here and
with ffmpeg, and compare sample by sample (bit-exact for PCM, ADPCM, A-law,
mu-law, FLAC; within the usual tolerance for MP3, AAC, Vorbis, Opus). Compare
also what `Audio_GetInfo` says — sample rate, channels, bits, duration, total
frames — with ffprobe. A wrong duration or a few samples lost or doubled at a
buffer boundary are typical finds.

Per format:

- **WAV**: `WAVE_FORMAT_EXTENSIBLE`; `fmt ` chunks of 14, 16, 18 and 40 bytes;
  chunks before `fmt ` and between `fmt ` and `data` (`LIST`, `fact`, `bext`,
  `JUNK`); odd-sized chunks and their pad byte; `data` size 0, 0xFFFFFFFF, or
  larger than the file; 8, 16, 24, 32-bit and float PCM; more than two
  channels; block align that contradicts channels × bits; sample rate 0.
- **IMA ADPCM, A-law, mu-law**: reads that end inside a block, at a block
  boundary, of one sample; the last, short block; seek into the middle of a
  block; stereo interleave.
- **MP3**: large ID3v2 tags (with unsynchronisation, with an embedded picture
  of several MB), ID3v1, APE tags; Xing, Info and VBRI headers, right and
  wrong; VBR and free-format; MPEG-1, 2 and 2.5 at every sample rate; mono,
  stereo, joint stereo, and a change mid-file; garbage between frames; a
  truncated last frame; the first frames referring to a bit reservoir that is
  not there. Then the read-ahead logic in `src/fm_audio.c` (`FM_MP3_INPUT_BUF`,
  `FM_MP3_LOW_WATER`): storage reads that return short, a frame that straddles
  the refill point, a frame larger than the buffer, low-water mark hit exactly.
  Seek accuracy on VBR files with and without a seek table, and the position
  reported afterwards.
- **AAC**: the library does the ADTS framing itself and hands each frame to a
  decoder the application registers (`Audio_RegisterDecoder`); there is no
  built-in AAC decoder. Audit the framing with a stub decoder of your own, and
  with a real one (FAAD2, FDK) if you can link it: ADTS headers with a wrong
  frame length, with and without CRC, several raw data blocks per frame,
  sample-rate index and channel configuration out of range or changing
  mid-stream, garbage between frames, a decoder that returns more samples than
  the frame buffer holds (`FM_AAC_PCM_SAMPLES`), fewer, zero, or an error.
- **FLAC**: block sizes from 16 to 65535, 8 to 24 bits, 1 to 8 channels, files
  without a seek table, very large metadata blocks, a wrong MD5, a stream that
  starts mid-frame.
- **Ogg Vorbis and Opus**: packets continued across pages, pages with 255
  segments, chained streams, a wrong page CRC, holes; Vorbis setup headers that
  are huge; Opus pre-skip, end trimming, output gain, channel mapping families;
  seeking by granule position at the start, the end and across a chain.

**The reading API itself.**

- `Audio_Read` (it counts 16-bit samples) with room for 1 sample, for an odd
  number of samples on a stereo file, for less than one decoder frame;
  zero-length reads; reads after end of file (must
  keep returning end of file, not wrap or repeat); `Audio_ReadMono` on stereo
  and `Audio_ReadStereo` on mono — rounding, clipping, channel order.
- `Audio_Seek` to 0, to the end, past the end, to a negative or NaN position,
  twice in a row, immediately after open, immediately before close.
- `Audio_Stream` and `Audio_SetCallback`: a callback that calls back into the
  audio API, that closes the handle, that returns an error.
- Decoder slots: there is a small pool per codec. Open until it is exhausted,
  fail opens at every stage, and check that each slot comes back (loop 1000
  times). With `FM_USE_THREADS=1`, open, decode and close from several threads
  under ThreadSanitizer — a data race in slot release was fixed recently; look
  for the next one.
- `Audio_RegisterDecoder` and its `Unregister` (the encoder registry belongs
  to the Save auditor):
  replace or remove a codec that is in use.

**Writing and recording.**

- `AudioWriter_*`, `AudioRecorder_Open` (`AudioFileWriter_*` and the encoders
  belong to the Save auditor): the WAV header
  after a normal close, after a write that failed, after the volume filled up;
  sizes at and above 4 GiB; zero frames written; every bit depth and channel
  count; whatever is written must open in ffmpeg and round-trip through
  `Audio_Open`.
- The tap recorder (`top_api/audio_record_with_tap.h`, tested by
  `tools/audio_record_host_test.c` and `tools/board_taps_host_test.c`): gain, clipping
  and filter arithmetic at full scale, buffer hand-over between the DMA
  half-complete and complete callbacks.

**Player and detection.**

- `Player_ScanFolder` on an empty folder, a folder with only non-media files,
  more files than the playlist holds, names at the path-length limit, nested
  folders; `Player_Next` / `Player_Prev` at both ends; `Player_Seek`,
  `Player_SeekFwd`, `Player_SeekBack` at the ends and during a track change; a
  playlist that mixes audio and video; a track that fails to open in the
  middle of the list; `Player_GetPosition` / `Player_GetDuration` against the
  truth.
- `File_DetectType`, `Media_DetectBuffer`, `Media_DetectByExtension`: buffers
  shorter than the magic number, the magic at the edge of
  `FM_MEDIA_MAGIC_SCAN`, content that contradicts the extension, an ID3 tag
  larger than the scan window in front of an MP3.

**Real-time behaviour.** Audio is pulled from a DMA callback budget (read
`docs/REGRESSION_AUDIO_STUTTER.md`). Find any call on the playback path whose
worst case is much worse than its typical case: a read that scans the rest of
the file, a seek that decodes from the start, a refill that blocks for several
storage reads. Measure the stack use of each decoder along its whole call
path (frame sizes: section 3; `README.md` "Stack" has the single frames, the
largest being 17 KB in minimp3): the task that decodes has a few kilobytes
unless the application was told otherwise, so check that it is told, in the
header of every function that needs more.

**Debug levels and log output (new since the first audit, never checked from outside).** The library prints
through `FM_ERR_LOG` (level 1 and up), `FM_EVENT_LOG` (2 and up) and
`FM_TRACE_LOG` (3); level 0 compiles all of it out. Read the top of
`include/fm_debug.h` and "Rules for a sink" in `include/fm_log.h`, then check
the messages of **your** files (`grep -n "FM_ERR_LOG\|FM_EVENT_LOG\|FM_TRACE_LOG\|FM_CHECK" src/fm_audio.c src/fm_flac.c src/fm_vorbis.c src/fm_opus.c src/fm_player.c`):

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

**The player on simulated time (new in this round).**
`tools/audio_record_host_test.c` and `tools/save_examples_host_test.c` show how
to run `SimpleAudio` on a PC with a stub HAL and the I2S half / full interrupts
raised by the test (`AudioDma_HalfCallback`, `AudioDma_FullCallback`). Build
your own harness on that model for Job 2 (b) and leads W1–W3: every statement
about call counts, contexts and lost samples in your report should come from
such a run, not from reading.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Opus needs the build switch `FM_USE_OPUS_LIB=1`; without it Opus files are
  refused as unsupported.
- HE-AAC reports the core sample rate.
- Formats that are recognised but not decoded are refused; a missing decoder is
  not a defect, garbage output or a hang is.
- Audio tracks inside video that are not supported: ALAC in MP4; ADPCM,
  Nellymoser and Speex in FLV (video auditor's area anyway).
- Nothing has been run on real hardware by the maintainers of this branch: the
  I2S / DMA examples are compiled against a stub HAL and their logic is tested
  on the host only.

- Chained Ogg streams: only the first stream is played (F20 of the previous
  audit).
- VBR MP3 without a Xing / Info header: duration and seek positions are
  estimates. With a Xing header: a seek may start up to about 0.15 s late and
  the reported position is within about 50 ms.
- WAV: 24-bit and float PCM and more than two channels are refused
  (`FM_ERR_UNSUPPORTED`); the video layer does read 24 / 32-bit and float PCM
  tracks. FLAC above 24 bits a sample is refused (limit of dr_flac v0.13.4).
- Stack use of the decoders is large. Single frames on Cortex-M7 (clang
  `-Os`): `mp3dec_decode_frame` 17.4 KB, `drflac_open_with_metadata_private`
  4.9 KB, everything else below 2.3 KB; `README.md` "Stack" advises 32 KB for a
  task that plays MP3 or Opus. Whole call paths have not been summed —
  **doing that is wanted**, repeating that the frames are large is not.
- dr_flac has signed-overflow undefined behaviour on damaged files; building
  with `-fwrapv` is the documented advice. A crash or out-of-bounds access
  that follows from it is still a finding.
- `.m4a` is MP4 (video auditor's parser) and is not in the player scan.
- Everything the first audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_AUDIO_V4.md`. It is used as a work list:
each finding is opened by someone who has not seen your session and has only
the report and a clean checkout of `48a00bc`. Use this structure, in this
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
     anything (expected: `Results: 557/557 passed, 0 failures`; if yours
     differs, that comes first in the report);
   - a table of what you did: method, target, amount (executions, files,
     cases, hours), coverage reached, result;
   - what you ran as a 32-bit program;
   - your local changes: every file you added or changed in your clone, one
     line each.
3. **Table of findings**: ID, severity, status (confirmed / suspicion), owning
   part, `file:line`, one-line title, fix tested (yes / no), regression test
   included (yes / no). Sorted by severity.
4. **One section per confirmed finding**, IDs `AUD4-01`, `AUD4-02`, ...:
   - **Severity** — S1: memory corruption, crash, hang, loss or corruption of
     stored data. S2: wrong result returned as success (wrong pixels, samples,
     bytes, sizes, timestamps; a recording that lost or misplaced data). S3:
     wrong error handling — a leaked handle or slot, a wrong error code, state
     not restored after a failure. S4: mismatch with the documentation,
     portability, wasted time or memory, a test that cannot fail.
   - **Where** — `path:from-to`, function, at `48a00bc`, and the lines
     themselves (quote at most 15).
   - **Reach** — which public function leads there, which action of a user of
     the board (a UART command, a file on the card, a sequence of calls), which
     build configurations, 32 bits / 64 bits / both, and how likely it is with
     ordinary files and ordinary use.
   - **How to reproduce** — from a clean checkout of `48a00bc`: the files to
     add (full source in the appendix), the exact commands, and the output as
     it was printed (sanitizer report, wrong value, hang with the time you
     waited). For an input file: a hex dump if it is under 200 bytes, otherwise
     the script that generates it.
   - **Expected and observed.**
   - **Root cause** — why it happens, not just where.
   - **Proposed fix** — a unified diff against `48a00bc` in a `diff` block.
     Produce it with `git diff`, do not type it: the sources have CRLF line
     endings and a typed diff will not apply. Then state: applied (yes / no),
     the reproducer after the fix, the result line of `make test` after the
     fix, and anything else you ran with it (`make asan`, `make m32`, the
     smaller host tests).
   - **Regression test** — the test to add (which file, the code), with its
     result on `48a00bc` (must fail) and with your fix (must pass). If a test
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

- Make a fresh clone at `48a00bc` and re-run every reproducer from the text of
  the report alone. Drop or downgrade what does not reproduce.
- Run `git apply --check` for every diff on that clean tree.
- Re-check every `file:line` against `git show 48a00bc:<path>`.
- The numbers in the Persian summary, in the table of findings and in the
  findings index agree.
- No finding rests on a documented limit, and no "confirmed" finding contains
  "may", "might" or "could" in its observed behaviour.
- Every number in the report has the command that produced it.
