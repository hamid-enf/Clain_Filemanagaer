# External audit, follow-up round — audio

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `a6cf829` (audit exactly this commit; quote it in your report)
- Previous audit of this part: commit `8dd0f25`, report in `ReportV2.txt` at the
  repository root, lines 1267–1582
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
repository: `ReportV2.txt` at the repository root, lines 1267–1582 (the other
line ranges are the other three parts). Read it first, including its sections
"Examined and found sound" and "Not examined". Everything it found has been
worked on since; `git log 8dd0f25..a6cf829` and
`git diff 8dd0f25..a6cf829 -- src/fm_audio.c src/fm_flac.c src/fm_vorbis.c src/fm_opus.c src/fm_player.c src/fm_media.c third_party/stb include/fm_audio.h examples/audio_dma_player.h` show how.

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
| F13, F25 | example DMA player: odd half-buffer, tap order | fixed in `examples/audio_dma_player.h` and its copy in `top_api/` |
| F14 | FLAC allocation size came from the header | capped by `FM_FLAC_MAX_PCM_BYTES` (40 MiB); Opus files by `FM_OPUS_MAX_FILE_BYTES` (5 MiB) |
| F18 | Vorbis leak on a corrupt setup header | fixed (visible only under ASan / LeakSanitizer) |
| F21, F22, F27 | MP3: Xing frame played as audio; seek offsets | Xing frame skipped; CBR seek exact; VBR with Xing approximate (see section 7) |
| F23, F24, F26 | recorder left empty files; player extension list; media detection | fixed |
| F28 | decoders called `malloc` directly | every decoder allocation goes through `FM_Allocator` (`FM_ReallocSized` added) |
| F30 | undefined behaviour in vendored decoders on corrupt input | fixed in stb_vorbis only; dr_flac see section 7 |
| F20, F29 | chained Ogg; stack use | **not fixed**, documented (section 7) |
| (own) | WAV writer: data above 4 GiB wrapped the header | `AudioWriter_Close` returns `FM_ERR_TOO_LARGE` above `UINT32_MAX − 36` (no test of its own) |

**Job 2 — audit what is new.** Code written since the last audit has been
through the fewest eyes. Go through the diff above line by line.
In this part the diff is the largest of the four (about 30 findings were fixed in
one pass, which is exactly when new defects get in): the MP3 resync and seek
rewrite, the IMA ADPCM decoder, the AAC frame path, the local patch to
`stb_vorbis.c`, the allocator routing of every decoder (does an allocation
that fails at each point leave the decoder slot free and the file closed?),
the size caps (off-by-one at exactly the cap, behaviour on 32 bits), and
`total_frames` for every format against ffprobe.
Newer still, and yours: the **packet-stream Vorbis decoder** in
`src/fm_vorbis.c` / `include/fm_vorbis.h` (`FM_Vorbis_StreamOpen`,
`StreamDecode`, `StreamPending`, `StreamReset`, `StreamClose`), which the
video layer uses for Vorbis tracks in Matroska and MP4. It frames every packet
as an Ogg page of its own for stb_vorbis's push interface, carries over the
bytes the decoder did not take, and keeps PCM that did not fit the caller's
buffer. Check: every allocation failure in open and decode; packets larger
than 64 KiB (several lacing pages); the carry growing without bound on a
stream the decoder never accepts; `StreamReset` in every state; a header set
that stb_vorbis accepts but that declares more than two channels; and whether
all of stb_vorbis's memory really goes through `FM_Alloc`. A second local
patch to `stb_vorbis.c` (two shifts into the sign bit in the push-data
resync) belongs to it.
And the newest (`git log 2a45d9d..a6cf829`): **Microsoft ADPCM in WAV files**
(`src/fm_adpcm.h`, shared with the video layer: block header, the seven
standard coefficient pairs and a coefficient table from the file, `nBlockAlign`
too small for one block, a last block cut short, seek by blocks), and the
**FLAC wrapper** `src/fm_flac.c` (`FM_Flac_*`): dr_flac is now compiled into
that one file with `DRFLAC_API static` and nothing else may call `drflac_*`;
a stream wider than 24 bits is refused with `FM_ERR_UNSUPPORTED` before the
decode, in a `.flac` file and in a video track.

**Job 3 — go where nobody has been.** The previous auditor of this part was thorough, and
left a list of leads that were **not confirmed and that nobody has examined
since**: VBRI headers; a channel count that changes mid-file; a 14-byte
`fmt ` chunk; the ADPCM allocation derived from `block_align`; example 33
(`examples/33_audio_record_with_tap.h`); unregistering a decoder while a
stream that uses it is open; `aac_find_sync` reading one byte per storage
call. Start with these. Then note what could not be tested last time: nothing
ran on ARM, so every stack figure was an x86 figure (the frames are measured
for Cortex-M7 now, `README.md` "Stack": check them and add the call depth,
which is not there), and several fixes
have weak tests (F7 and F19 only in the libopus build, F4 only with threads,
F18 only under ASan, no test for the stb_vorbis UB fixes or the RIFF size
check): try to break exactly those.

What is already established (running it again will find nothing): the suite
passes in 49 build configurations (at this commit: all 49 plain, and the full
build, the build with libopus and the nine single-switch builds under ASan +
UBSan; all 49 under the sanitizers at `2a45d9d`), under ThreadSanitizer
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
| `src/fm_audio.c`, `include/fm_audio.h` | about 3 000 lines: audio API, WAV (PCM, IMA and Microsoft ADPCM, A-law, mu-law), MP3 and AAC front ends, read-ahead buffering, seek, decoder and encoder registries, writers, recorder. AAC is ADTS framing only: the decoder is a plugin the application registers |
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
storage reads. Measure the stack use of each decoder along its whole call
path (frame sizes: section 3; `README.md` "Stack" has the single frames, the
largest being 17 KB in minimp3): the task that decodes has a few kilobytes
unless the application was told otherwise, so check that it is told, in the
header of every function that needs more.

**Debug levels and log output (new since the last audit).** The library prints
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
- Everything the previous audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_AUDIO_V3.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, how many of the earlier findings
   are really fixed, your seven scores, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and what you ran as a 32-bit program.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `AUD3-01`, `AUD3-02`, ...:
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
