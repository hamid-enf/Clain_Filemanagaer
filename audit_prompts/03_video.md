# External audit — video

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
| `src/fm_video.c`, `include/fm_video.h` | about 6 000 lines: container parsers (FMRV, AVI, MP4/MOV, MKV/WebM, FLV), frame index, seek, audio-track demux, FMRV writer |
| `src/fm_h264.c`, `include/fm_h264.h`, `third_party/h264bsd/` | H.264 decoder plugin |
| `src/fm_mjpeg.c`, `include/fm_mjpeg.h` | MJPEG decoder plugin |
| `src/fm_mediacache.c`, `include/fm_mediacache.h` | media cache (copy to fast storage and play from there) |
| `src/fm_player.c` (video paths) | playlist player |
| `examples/simple_video.h`, `examples/hw_jpeg_video.h`, `examples/play_from_flash.h` | how applications use it |
| `test/video_hooks_test.c`, `tools/gen_test_videos.py` | hook test, generator of test videos |

The audio **decoders** (MP3, AAC, FLAC, Vorbis, Opus, PCM) belong to the audio
auditor. Yours is the **demux**: finding the audio track, cutting it into
frames, timestamps, `Video_HasAudio`, `Video_GetAudioInfo`,
`Video_ReadAudioFrame`, `Video_SeekAudio`.

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
- Compare decoded frames with ffmpeg's for Baseline streams, bit-exact.
- `Video_DecodeFrameAs` and `Video_DecodeFrameScaled`: every pixel format,
  output buffer one byte too small, odd dimensions, cropping in the SPS.

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

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Codecs other than RAW, MJPEG and H.264 (MPEG-4 Part 2, MPEG-2, HEVC, VP8,
  VP9, AV1, Theora) are identified and need an external decoder plugin. A
  missing decoder is not a defect; a crash or hang while identifying one is.
- H.264 is Baseline profile only (limit of the vendored decoder).
- FLV without `onMetaData` is reported as 320 × 240.
- Audio tracks not supported inside video: ALAC in MP4; ADPCM, Nellymoser and
  Speex in FLV.
- HE-AAC tracks report the core sample rate.
- The AAC-in-MKV path (`A_AAC`, audio object type handling) has no test of its
  own: defects there are welcome, the missing test is known.

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_VIDEO.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and whether a 32-bit build was possible.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `VID-01`, `VID-02`, ...:
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
