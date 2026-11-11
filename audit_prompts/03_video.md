# External audit, follow-up round — video

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `a6cf829` (audit exactly this commit; quote it in your report)
- Previous audit of this part: commit `8dd0f25`, report in `ReportV2.txt` at the
  repository root, lines 1089–1265
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

Your part is **video (containers, video codecs, audio/video timing)**. Three other auditors cover the other parts
(files and filesystems, image, audio). Stay in your part; if you trip over something outside it, give it
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
make test                 # whole suite, about 513 tests, about 15 s
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
repository: `ReportV2.txt` at the repository root, lines 1089–1265 (the other
line ranges are the other three parts). Read it first, including its sections
"Examined and found sound" and "Not examined". Everything it found has been
worked on since; `git log 8dd0f25..a6cf829` and
`git diff 8dd0f25..a6cf829 -- src/fm_video.c src/fm_h264.c src/fm_mjpeg.c src/fm_mediacache.c src/fm_player.c third_party/picojpeg include/fm_video.h` show how.

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
The same comparison was then widened (`git log 2a45d9d..a6cf829`) and closed what
it had left open: JPEG and Motion JPEG in ffmpeg's 4:2:2 / 4:4:4 layouts,
BT.709 for H.264, sizes taken from a damaged index, the FLAC sample width,
Microsoft ADPCM in WAV files, PCM tracks of every width and byte order, and
the encoder delay and padding of MP3 / Vorbis / Opus tracks. That is the
newest code in the repository and nobody outside has read it.
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
| VID-01 | deeply nested MP4 boxes exhausted the stack in `Video_Open` | the box walk is bounded: nesting deeper than `MP4_MAX_NEST` (8) is refused with `FM_ERR_INVALID_FORMAT` (`mp4_find_nested` in `src/fm_video.c`) |

Check VID-01 beyond the reported file: every other place where a container
parser recurses or follows an offset taken from the file (MKV master elements,
AVI `LIST` inside `LIST`, FLV script data objects, MP4 `meta` / `udta`), and
whether a legitimate file can need more than 8 levels (ffmpeg, phone cameras,
QuickTime).

**Job 2 — audit what is new.** Code written since the last audit has been
through the fewest eyes. Go through the diff above line by line.
In this part the diff was small at `cf187af` and is large now, and the large
part is three days old:

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
  and the new `mp4_visual_object_type` (0x6C is Motion JPEG).
- `src/fm_h264.c`, `yuv420_to_rgb888`: the colour range and the matrix now
  come from the VUI (`k_yuv_matrix[4]`: BT.601 / BT.709, video / full range;
  only `matrix_coefficients == 1` selects BT.709, a VUI that says nothing is
  BT.601 video range). There is a test vector for three of the four rows, none
  for BT.709 full range. Check the values it treats as BT.601 (BT.2020 = 9,
  SMPTE 240M = 7, RGB = 0), and streams whose VUI changes with a new SPS.
- **The newest code, a few days old** (`git log 2a45d9d..a6cf829`):
  - `video_pcm_read` and `mp4_pcm_entry`: PCM tracks of 8, 16, 24, 32 bits and
    32-bit float, both byte orders, in AVI / MKV / MP4 / MOV / FLV, converted
    to 16-bit **in the caller's buffer**; adjacent small packets are read
    together (`VIDEO_PCM_GATHER`). Check the in-place conversion at every
    width with buffers around one sample frame, packets that are not a whole
    number of frames, and the gathering at the end of the index.
  - the trimming wrapper: `Video_ReadAudioFrame` is now a layer over
    `video_audio_read_raw` that removes encoder delay and padding
    (`mp4_audio_edit`, MKV `CodecDelay` / `DiscardPadding` in
    `mkv_group_discard`, `video_audio_trims`, `video_audio_rest_bytes`). The
    rules are written above `Video_ReadAudioFrame` in `include/fm_video.h`.
    Check: edit lists with several edits, empty edits, a `media_time` beyond
    the track, a duration of 0 or longer than the track; `DiscardPadding`
    negative, larger than the packet, on a block that is not the last;
    `CodecDelay` near its one-second cap; every buffer size (a packet
    delivered in parts must lose the same frames); the frame count after
    `Video_SeekAudio(0)` and after a seek elsewhere; and the frame totals
    against ffmpeg for every codec and container.
  - `video_in_file` and the range check in `video_audio_locate`: a size or
    offset from a damaged index used to become an allocation of that size.
    Look for the places that still trust one.
  - `video_flac_too_wide` and `src/fm_flac.c`: a FLAC track wider than 24 bits
    is announced with `decodable == FM_FALSE`.
  - `src/fm_adpcm.h`: the two ADPCM block decoders moved out of `fm_video.c`
    and are shared with the WAV reader.
  - A read that returns `FM_ERR_UNSUPPORTED` now passes the packet over, so
    that the track ends; `FM_ERR_TOO_LARGE` consumes nothing for every codec.
- The audio-track changes that came from the audio audit and touch your demux
  (AAC frame hand-over, Opus header gain and multistream refusal, `.m4a` no
  longer picked up by the player scan), and the log lines of `src/fm_video.c`
  and `src/fm_mediacache.c`.

The maintainers ran the new audio code on 90 000 mutated copies of the test
vectors under ASan + UBSan (which found one undefined shift in stb_vorbis's
resync, now patched) and compared ffmpeg-written files with ffmpeg's decode:
37 at first, then a sweep of every audio codec in every container that can
carry it (3 s each; PCM, G.711, ADPCM and FLAC identical, MP3 / Vorbis / Opus
within one LSB and of the same length). The newest code above has **not** been
fuzzed. That is a start, not coverage: go further on both.

**Job 3 — go where nobody has been.** Be aware that the previous audit of this part was
thin: one finding, found by reading; no fuzzing, no differential comparison,
no truncation or fault-injection campaign, no seek or timing checks. Its "Not
examined" section lists almost the whole scope. **Treat section 6 below as
never done and work through all of it**; this part has had the least outside
scrutiny of the four and `src/fm_video.c` is 7 200 hand-written lines that
take every size and offset from the file.

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

**Debug levels and log output (new since the last audit).** The library prints
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
- Everything the previous audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_VIDEO_V3.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, how many of the earlier findings
   are really fixed, your seven scores, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and what you ran as a 32-bit program.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `VID3-01`, `VID3-02`, ...:
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
