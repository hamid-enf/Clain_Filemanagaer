# External audit — saving and recording (audio, video) and how the parts work together

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `523d2da` (audit exactly this commit; quote it in your report)
- Library root: `Clain_FileManager/FileManager_lib/` (paths below are relative to
  it unless they start with `Core/`, which is `Clain_FileManager/Core/`)
- The code under audit is new. It was written in three commits and **nobody
  outside has read it**:

  | Commit | What it added |
  |---|---|
  | `c688b9e` | the audio output switch (speaker / callback / both / none), the persistent PCM callbacks of both players, track-start / track-end callbacks |
  | `4cbd8a0` | saving: audio encoders (WAV, IMA ADPCM, FLAC), the MJPEG-AVI writer, a JPEG encoder with quality and 4:2:0, file-to-file converters, the recording functions of `SimpleAudio` and `SimpleVideo` |
  | `523d2da` | the save examples (plain, and through the player callbacks), their host test, repeat-frame, byte-swapped 16-bit frames, the board glue and four UART commands |

  `git diff 5d66e46..523d2da --stat` is 37 files, about 11 400 added lines.
  `git show c688b9e --stat` is the callback layer under it.

It is a C library for STM32 microcontrollers (STM32H743 is the reference board:
SD card, SDRAM, I2S audio out, SPI TFT). The firmware is **bare-metal: one main
loop**, plus interrupts. Think in those terms while you read:

- `size_t`, `int` and pointers are **32 bits** on the target. The host tests run
  on 64 bits.
- Stacks are a few kilobytes. RAM is static pools plus an allocator
  (`FM_Alloc`) that can fail.
- Sound is produced by I2S + DMA on a circular buffer of two halves. When a half
  has been played, an interrupt fires and that half has to be refilled before
  the DMA comes round again. Depending on the player, the refill runs in the
  interrupt, in the main loop (`Simple*_Poll`), or from `FM_Idle()`, a hook
  that long operations (a picture decode, a JPEG encode) call so that sound does
  not break. **Establish for yourself in which context each callback of each
  player runs**; much of this audit depends on it, and the comments in the
  headers make claims about it that you should check.
- The SD card is slow and uneven: a write usually takes a few milliseconds and
  sometimes several hundred.
- A recording that silently loses data, or produces a file that looks fine and
  is wrong, is as bad as a crash.

Earlier audits covered the file system, image, video decoding and audio decoding
(prompts `01`–`04` in this folder, reports in `ReportV2.txt`). They are **not**
your subject. Yours is everything that *writes* media, and the way the writers,
the two players, their callbacks and the main loop work **together**.

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
make test                 # whole suite, 546 tests (suites test_audio_save, test_video_save are yours)
make saveexamplestest     # the save examples run against the real players, 42 checks
./fm_saveexamplestest -v  #   the same, also printing what the examples log
make audiorecordtest      # the recorder of SimpleAudio; sections 7 and 8 are the save functions
make videohooks           # frame / track callbacks of SimpleVideo
make tapstest             # the audio callback block of the board, extracted from board/app_audio_hooks.h
make asan                 # the suite under ASan + UBSan
make m32                  # the suite as a 32-bit program (needs: pip install ziglang)
make check                # everything above and the build checks
sh tools/check_examples.sh    # every header in examples/ compiles alone, also with video / media off
sh tools/check_cubeide.sh     # nothing outside the firmware folders gets into a firmware build
make test FM_USE_FLAC=0   # every FM_USE_* switch can be set like this
```

- There is **no hardware here and none where the code was written**: nothing in
  this part has ever run on a board. The players are tested on a PC against a
  stub HAL (`tools/halstub/`), with the interrupts raised by the test.
  `tools/save_examples_host_test.c` is the best model for a harness of your
  own: it shows how to mount a RAM disk, create a source video, raise the I2S
  half / full interrupts (`AudioDma_HalfCallback`, `AudioDma_FullCallback` for
  music; `sv_i2s_half`, `sv_i2s_full` for video, driven from a hook on
  `HAL_GetTick`), and make the simulated player fast or slow. Build line: the
  `saveexamplestest` target of the `Makefile`.
- The firmware is `Core/Src/main.c` (CubeMX project). It cannot be built here
  without the ST HAL; `git clone` of `STMicroelectronics/stm32h7xx_hal_driver`,
  `cmsis_device_h7`, `cmsis_core`, `stm32_mw_usb_device`, `stm32_mw_fatfs`
  (branch `release/r0.12c`) and a cross compiler (`python3 -m ziglang cc
  -target thumb-freestanding-eabihf -mcpu=cortex_m7`, with newlib headers) is
  enough to compile it; linking is not possible. A compile is worth doing: it
  is the only check the board glue has ever had (see section 4, lead L9).
- Reference tools you should have: `ffmpeg` / `ffprobe`, `flac` (`flac -t`),
  `sox` or Python for sample-exact comparisons, a JPEG decoder other than this
  library's (libjpeg, stb_image, Pillow).
- Source files use CRLF line endings. Comments in `top_api/`, `examples/`,
  `board/` and the guide are in Persian; code and identifiers are English.
  `examples/X.h` and `top_api/X.h` are meant to be the same file (the board
  uses `top_api/`, the checks use `examples/`): a difference between the two
  copies beyond their `#include` lines is a finding.

## 4. What was built, and your three jobs

### How it fits together

```
Library layer (src/, include/)
  AudioFileWriter_Open / WriteFrames / Close      PCM16 -> .wav (PCM or IMA ADPCM) / .flac / plugin
  Audio_Convert                                   file -> your callback -> file
  Image_EncodeJPEGEx                              pixels -> baseline JPEG (quality, 4:4:4 or 4:2:0, swap16)
  VideoFileWriter_Open / WriteFrame / WriteFrameSwap16 / WriteJpeg / RepeatFrame / WriteAudio / Close
                                                  -> AVI 1.0, MJPG pictures, optional PCM16 track,
                                                     index in a side file "<name>.idx" until Close
  Video_Convert                                   any playable video -> MJPEG AVI

Player layer (top_api/simple_audio.h, top_api/simple_video.h, top_api/audio_dma_player.h)
  music:  decoder -> DATA callback -> save callback -> volume -> MONITOR callback -> DMA
  SimpleAudio_RecStart / RecWrite / RecStop       your own samples -> file
  SimpleAudio_RecPlaying(dest)                    the library saves what is playing (folder: one file per track)
  SimpleAudio_Convert
  video:  frame callback (every presented frame), sound DATA / MONITOR callbacks (per I2S half)
  SimpleVideo_RecStart / RecWrite / RecWritePixels / RecWritePlayerFrame / RecWriteJpeg /
          RecWriteAudio / RecRepeat / RecStop     -> .avi (anything else: raw FMRV, the older path)
  SimpleVideo_Convert

Example layer (top_api/examples_save_media.h)
  SaveDemo_AudioPlain, SaveDemo_VideoPlain        style 1: data made by the example
  SaveDemo_MusicCallback  -> ring in RAM -> SaveDemo_MusicRecPump -> SimpleAudio_RecWrite
  SaveDemo_VideoFrameCallback -> SimpleVideo_RecWritePlayerFrame (JPEG encode + file write, in the callback)
  SaveDemo_VideoSoundCallback -> ring in RAM -> SaveDemo_VideoRecPump -> SimpleVideo_RecWriteAudio
  SaveDemo_MusicFromCallback, SaveDemo_VideoFromCallback   the same, start to end in one function

Board layer (board/app_save_hooks.h, board/app_flow.h, top_api/code_cmd.h, Core/Src/main.c)
  the board's own callbacks (loudness normaliser, frame counter) chained in front of the SaveDemo ones;
  UART commands arec / arecs (music) and vrec / vrecs (video); two Pump calls in while (1)
```

### Job 1 — is what gets written right? (do this first)

The earlier rounds of this project taught one lesson: the suite checked that
files were *recognised* and nothing *crashed*, and missed that the *content* was
wrong. For the writers the suite mostly reads files back **with the library's
own readers**, which proves the two agree, not that either is right. So before
anything else, write files with every writer and judge them with tools that do
not share code with this library:

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
  (PSNR per quality step; colour — the earlier rounds found wrong matrices and
  ranges in the *decoders*, check the encoder's RGB → YCbCr the same way);
  every pixel format; widths and heights that are not multiples of 8 and 16
  (edge replication in 4:2:0), 1 × 1, 1 × N, N × 1, the largest size; quality
  1, 50, 100; `swap16` on and off. The header claims the output suits the STM32
  JPEG peripheral: check the markers against what that peripheral requires.
- **AVI**: `ffprobe -show_streams -show_format`, `ffmpeg -v error -i x.avi -f
  null -`, frame count, duration, frame rate (also 30000 / 1001), `dwSuggested
  BufferSize`, `idx1` offsets (relative to what?), the padding byte of odd
  chunks, the `strf` of both streams, lengths in `avih` / `strh`, the sound
  track sample for sample, pictures against the source. Files with 0 frames,
  1 frame, sound only before the first picture, no sound, 10 000 frames.
  Repeated frames (`RepeatFrame`): how are they stored, and do ffmpeg, VLC and
  this library all show them as repeats with the right timing?

### Job 2 — audit what is new, line by line

Everything in section 5. Things that typically go wrong in code like this:

- **Error paths of the writers.** A write that fails in the middle, a volume
  that fills up, an allocation that fails at each point (every Nth, for every
  N), `Close` after a failure, `Close` twice, `WriteFrames` after `Close`, a
  second `Open` on a writer that is open. Afterwards: is every handle and every
  allocation released (loop 1000 times), is the `.idx` side file gone, what is
  left on the card, and does the header comment's promise hold ("the first
  failed write stays the answer of every later call", "on any failure dst is
  removed")?
- **Size arithmetic on 32 bits**: byte counts of a recording near 2 GB and
  4 GB (WAV, the AVI limit `FM_VIDEO_ENC_MAX_BYTES`, the `.idx` file, frame
  counters, `uint32_t` sample counts at 48 kHz for hours), `width * height * 3`,
  the "a frame never needs more than its raw size and a header" assumption in
  `vw_write_frame` for tiny or noisy pictures at quality 100.
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
  in the unwritten buffer, after a failed write, `count` of 0 and of 10⁶.

### Job 3 — do the parts work together? (the main point of this audit)

Every piece has a test of its own. What has been tested least is the
combination, on the timing of a real board. Build harnesses that put the parts
under the load they will see, and try the sequences a user will really type:

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
   file's frames by the frame index of the source and fills gaps with
   `RecRepeat`; the sound goes in as it is decoded. Check the sync of the
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

### What the author already suspects — start here

The author reviewed his own work after the commit and listed what he could not
settle. None of it is fixed. Confirm, refute or measure each one; an answer for
every item below is wanted in the report (section 9 of the report format).

- **L1. Seek during `vrec`.** After a forward seek the frame callback sees a
  jump in the frame index and fills it with repeated pictures, while the sound
  ring still holds sound that was decoded ahead and then never played. A quick
  experiment (seek +0.8 s in the 2 s test video) gave a file with 7 repeated
  pictures and a sound track 0.02 s *longer* than the source. On a long video
  with a 10 s jump the picture and the sound of the file should end up seconds
  apart. Not measured on a long file.
- **L2. Ring margins** (Job 3, item 3). Sized for "two halves and a bit";
  nobody has measured a main-loop pass on the board.
- **L3. JPEG encode time.** The encoder is plain C. For 320 × 240 on a
  Cortex-M7 the author guesses tens to hundreds of milliseconds per frame, so
  `vrec` may turn most pictures into repeats. Nobody measured it. Estimate it
  (instruction counts from a cross-compiled build, or a cycle-accurate guess
  with your reasoning) and say what frame rate a user should expect. The
  board has a hardware JPEG codec (`top_api/hw_jpeg.h`, used for decoding);
  the encoder does not use it.
- **L4. `FM_JpegEncodeOptions.swap16`** was added to a public structure.
  Callers that fill `quality` and `subsample` without clearing the structure
  now pass garbage in `swap16`. The header says "clear it first"; code written
  against the previous commit does not. The author found no such caller left in
  the repository (the video writer sets all three fields, the tests clear the
  structure); check that, and say whether there is a design that cannot be
  misused this way.
- **L5. Three ways to write a frame** (`RecWrite` RGB888, `RecWritePixels`,
  `RecWritePlayerFrame`) and a byte order that depends on a global of the
  player (`SIMPLE_VIDEO_SWAP16`, `SimpleVideo_SetSwap16`). Can a user get wrong
  colours without an error? `SimpleVideo_SaveFrame` and `SimpleImage` have
  frame buffers of the same kind: do their save paths handle the swap?
- **L6. `SimpleVideo_Info()`** was changed so that it is filled in the first
  frame callback (before, it returned nothing until the player was "running").
  Does it now return stale data after `Stop`, between two videos, or after a
  failed open? Who else calls it (`grep -rn SimpleVideo_Info`)?
- **L7. The last sound block** of a video used to be lost for the callbacks
  (an early `return` in `sv_i2s_fill_half_inner`); now it is delivered. Is the
  count right for every length (a video whose sound ends exactly on a half
  boundary, one sample before, one after, sound longer than the pictures), and
  is nothing delivered twice?
- **L8. The frame number of the current frame** is taken as
  `SimpleVideo_Info().frame_index - 1`. Is that true in every path (first
  frame, after a seek, after a pause, hardware-JPEG path, a repeated present of
  the same frame)?
- **L9. The board glue has never executed.** `board/app_save_hooks.h` and the
  changes in `board/app_flow.h`, `top_api/code_cmd.h`, `Core/Src/main.c` were
  only compiled for ARM. `make tapstest` extracts a block of
  `board/app_audio_hooks.h` and does not include the new file. Write the host
  test that is missing (stub HAL, the real board headers) and run the command
  sequences of Job 3.
- **L10. A recording that is not closed** (power loss, reset) leaves an AVI
  without index and a WAV / FLAC with an unfinished header, plus a `.idx` file.
  The library's own player refuses the AVI. Is there anything that cleans up or
  repairs at the next start? What do ffmpeg and VLC make of each?
- **L11. Fixed file names.** `arec` and `vrec` always write
  `/save/music_cb.wav` and `/save/video_cb.avi`. What happens to the previous
  file, and what if it is the file being played?
- **L12. Tests that prove less than they seem to.** The host test's source
  video is 64 × 48, 2 s, flat grey pictures, mono 16 kHz. Flat pictures cannot
  show a wrong chroma layout, a wrong row order or a shifted block; a 2 s clip
  cannot show drift. Judge every test of this part by one question: would it
  fail if the feature were broken in a plausible way? Break things locally and
  see.

Methods most likely to pay off: **differential testing against reference
tools** (Job 1), **fault injection** (a write that fails at every point, an
allocation that fails at every point, a card that becomes slow), **simulation
of the main loop with controlled time** (Job 3), **32-bit harnesses**, **reading
for state left half-changed on an error path**. Fuzzing matters less here than
in the decoder audits: the writers take trusted input, except for what the
converters read.

## 5. Your scope

| File | What it is |
|---|---|
| `src/fm_audio_enc.c`, saving section of `include/fm_audio.h` | 2 200 lines: WAV, IMA ADPCM and FLAC encoders, the encoder registry, `AudioFileWriter_*`, `Audio_Convert` |
| `src/fm_video_enc.c`, saving section of `include/fm_video.h` | 1 100 lines: AVI writer, `.idx` side file, `RepeatFrame`, `Video_Convert` |
| `src/fm_jpeg.c` (encoder part: `jpeg_row_ycc`, `jpeg_encode`, `jpeg_encode_common`, `Image_EncodeJPEGEx`, `FM_JPEG_EncodeSink` in `src/fm_internal.h`), `include/fm_image.h` | JPEG encoder with quality, 4:2:0, `swap16`, and a sink that writes straight into the video file |
| `top_api/simple_audio.h` (search `Rec`, `Convert`, `simple_audio_rec_`), `top_api/audio_dma_player.h` (`data_cb`, `save_cb`, `monitor_cb`, `audio_dma_fill_half`) | recorder and callbacks of the music player |
| `top_api/simple_video.h` (search `Rec`, `sv_avi`, `Convert`, `sv_i2s_fill_half_inner`, `SimpleVideo_Info`, `sv_swap16_active`) | recorder and callbacks of the video player |
| `top_api/examples_save_media.h` | 1 080 lines: the save examples and the two rings |
| `board/app_save_hooks.h`, `board/app_flow.h`, `board/app_audio_hooks.h`, `board/app_video_hooks.h`, `board/app_uart_commands.h`, `top_api/code_cmd.h`, `Core/Src/main.c`, `Core/Src/defines.h` | how the board wires it all: callbacks, commands, the main loop |
| `test/audio_save_tests.h` (14 tests), `test/video_save_tests.h` (13), `tools/save_examples_host_test.c` (42 checks), `tools/audio_record_host_test.c` (sections 7, 8), `test/video_hooks_test.c`, `tools/board_taps_host_test.c`, `tools/halstub/` | the tests of this part and the stub HAL they run on |
| `docs/GUIDE_FA.md` sections 7.2, 7.5, 7.6, 8.2–8.5, `board/README.md` (search `arec`), `docs/OPTIONS_FA.md` | what the user is told (Persian) |

Every header in `top_api/` has a twin in `examples/`; audit one and diff the
other.

Out of scope: the decoders, the demuxers and the file systems themselves
(audited before). They are in scope only where a writer or a converter depends
on something they do not guarantee.

Build switches that must each still build and behave: `FM_USE_AUDIO`,
`FM_USE_VIDEO`, `FM_USE_IMAGE`, `FM_USE_JPEG`, `FM_USE_FLAC`, `FM_USE_MP3`,
`SIMPLE_VIDEO_I2S` (0 and 1), `SIMPLE_VIDEO_SWAP16` (0 and 1). With a feature
off the save functions must exist and return `FM_ERR_UNSUPPORTED` (the
application still links). Tunables: `FM_AUDIO_ENC_OUT_BYTES`,
`FM_FLAC_ENC_BLOCK`, `FM_VIDEO_ENC_OUT_BYTES`, `FM_VIDEO_ENC_QUALITY`,
`FM_VIDEO_ENC_MAX_BYTES`, `FM_VIDEO_CONVERT_AUDIO_BYTES`, `FM_JPEG_QUALITY`,
`SAVE_DEMO_MUSIC_RING_SAMPLES`, `SAVE_DEMO_VIDEO_RING_SAMPLES`,
`SIMPLE_AUDIO_HALF_SAMPLES`, `SIMPLE_VIDEO_HALF_SAMPLES`: try the extremes the
`#error` checks allow.

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
- An AVI that was not closed has no index and this library does not open it
  (L10 asks what happens *around* that, not for the limit itself).
- While a video file is written a second file `<name>.idx` exists next to it.
- The save examples are header files with static state: one recording of each
  kind at a time, included from one `.c` file only (as every header of
  `top_api/`).
- Saving through a callback runs at playback speed; the converters are the
  fast path.
- Recording that starts in the middle of a video: the sound may lead the
  picture by up to one I2S buffer (measure it; report only if it is more).

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect in how this library calls it is in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_SAVE_V1.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, your verdict on the author's leads
   L1–L12 in one line, your eight scores, and what you could not examine. The
   rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long, which reference tools and versions, what
   you ran as a 32-bit program, what you compiled for ARM.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `SAV1-01`, `SAV1-02`, ...:
   - **Severity** — S1: memory corruption, crash, hang, loss or corruption of
     stored data (a recording that loses sound or pictures without saying so
     is S1). S2: wrong result returned as success (wrong samples, pixels,
     colours, sizes, durations, picture and sound out of step). S3: wrong error
     handling — leak of a handle, slot or allocation, a side file left behind,
     wrong error code, state not restored after a failure. S4: mismatch with
     the documentation, portability, wasted time or memory.
   - **Where** — file, function, line numbers at the audited commit.
   - **How to reproduce** — the smallest program, command sequence or input
     that shows it, the exact build command and the exact output. For a timing
     defect: the simulated timing that triggers it and why a board can have
     that timing.
   - **Expected and observed.**
   - **Root cause** — why it happens, not just where.
   - **Proposed fix** — as a unified diff, and whether you tested it.
   - **Which build configurations are affected**, if not all.
5. **Suspicions** — same format, without a reproducer, with what stopped you.
6. **Examined and found sound** — each area you covered and how (for example
   "FLAC encoder: 400 inputs of 0 to 200 000 frames, mono and stereo, verified
   with `flac -t` and sample-compared through ffmpeg, no findings"). This
   section matters as much as the findings: it tells us what an empty result is
   worth.
7. **Not examined** — everything in your scope you did not get to.
8. **Outside my scope** — one line each.
9. **Verdict on the author's leads** — one table: L1 … L12, your verdict
   (confirmed defect → finding ID / not a defect / could not settle), how you
   checked, and the number you measured where the lead asks for one.
10. **Scores.** Give each line a whole number from 0 to 10 and one sentence
    that names the evidence. Write "not assessed" instead of guessing; a score
    without evidence behind it is worth nothing. Axes A to G are the ones the
    earlier audits of this project used, so that the parts can be compared; H
    is new for this part.

    | Axis | What 10 means |
    |---|---|
    | A. Malformed and hostile use | no crash, hang or out-of-bounds access for any call order, argument or converter input you could construct |
    | B. Correct results | every file written is what a reference tool says it should be: samples, pixels, colours, lengths, timing |
    | C. Errors and resources | every failure returns the right code, releases what it took, leaves nothing half-written that claims to be whole, and leaves a usable state |
    | D. Memory and timing | stack, allocations and worst-case call times fit a microcontroller main loop; nothing blocks the audio refill |
    | E. Documentation | headers, the guide and the board README say what the code does, including the limits |
    | F. Tests | the tests of this part would catch a plausible regression, and cover what matters |
    | G. Diagnosability | when something fails or data is dropped, the result code and the log line together tell a user why |
    | H. Working together | recording, both players, their callbacks, the board commands and the main loop behave correctly in combination and in every order a user can produce |

    Then one line: **ready for a product / ready under these conditions (list
    them) / not ready**, for this part only. And a second line: the **three
    things to fix first**, in order.

Put harness sources, generator scripts and the files that show a defect in an
appendix (or attach them next to the report). Someone must be able to reproduce
every confirmed finding from the report alone.
