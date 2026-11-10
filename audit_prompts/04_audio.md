# External audit — audio

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

Your part is **audio (music playback, decoders, recording, player)**. Three other auditors cover the other parts
(files and filesystems, image, video). Stay in your part; if you trip over something outside it, give it
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
| `src/fm_audio.c`, `include/fm_audio.h` | about 3 000 lines: audio API, WAV (PCM, IMA ADPCM, A-law, mu-law), MP3 and AAC front ends, read-ahead buffering, seek, decoder and encoder registries, writers, recorder. AAC is ADTS framing only: the decoder is a plugin the application registers |
| `src/fm_flac.c`, `src/fm_vorbis.c`, `src/fm_opus.c` | glue to `third_party/dr_flac`, `third_party/stb`, `third_party/opus` |
| `third_party/minimp3/` | MP3 decoder (vendored) |
| `src/fm_player.c`, `include/fm_player.h` | playlist player: scan folder, play, next, previous, seek, position |
| `src/fm_media.c`, `include/fm_media.h` | file-type detection by content and by extension |
| `examples/simple_audio.h`, `examples/audio_dma_player.h`, `examples/33_audio_record_with_tap.h`, `examples/simple_i2s_mux.h` | how applications use it (I2S + DMA) |
| `docs/REGRESSION_AUDIO_STUTTER.md`, `docs/DSP_PROCESSING_BUDGET.md` | history of a real stutter bug and the processing budget |
| `tools/board_taps_host_test.c`, `tools/ex33_host_test.c`, `test/player_examples_host.c` | host tests of the board-side code |

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
- `Audio_RegisterDecoder` / `Audio_RegisterEncoder` and their `Unregister`:
  replace or remove a codec that is in use.

**Writing and recording.**

- `AudioWriter_*`, `AudioFileWriter_*`, `AudioRecorder_Open`: the WAV header
  after a normal close, after a write that failed, after the volume filled up;
  sizes at and above 4 GiB; zero frames written; every bit depth and channel
  count; whatever is written must open in ffmpeg and round-trip through
  `Audio_Open`.
- Example 33 (`examples/33_audio_record_with_tap.h`, tested by
  `tools/ex33_host_test.c` and `tools/board_taps_host_test.c`): gain, clipping
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
storage reads. Measure the stack use of each decoder (`-fstack-usage`): the
task that decodes has a few kilobytes.

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

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_AUDIO.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and whether a 32-bit build was possible.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `AUD-01`, `AUD-02`, ...:
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
