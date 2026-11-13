# Review of ReportV41.txt — delayed audio V5

The report audited `fc31b25` and arrived after `6fe596d`. Its four diffs were
extracted from the report, applied to the current branch, reviewed and
supplemented. The separate `audit_v5/` harness directory named in the report
was not committed with it; these regression tests are new and independent.

| Item | Outcome | Evidence and limits |
|---|---|---|
| AUD5-01 | Fixed a leading ID3v2 tag for WAV, FLAC, Vorbis and Opus. | Tagged/untagged first PCM blocks compared with and without footer in `test/audit_audio_late_tests.h`; real libopus tested separately. Not exhaustive over tags or allocation failures. |
| AUD5-02 | Restart budget follows the track instead of resetting on every successful poll; a different track failing immediately does not inherit the old budget. Exhaustion stops the player; it does not promise to skip to the next track. | `tools/audio_record_host_test.c`: a track changes MP3 rate after valid frames. Both `examples/` and `top_api/` variants run. Restoring each original header separately made its assertion fail. |
| AUD5-03 | Detection reads behind a leading ID3v2 tag; incomplete tags use the extension or unknown rather than claiming MP3. | Tagged WAV, FLAC, Vorbis and short/incomplete prefixes tested; restoring the original media implementation made tests fail. `File_DetectType` reads behind one tag; the in-memory detector skips complete chained tags in its buffer. |
| AUD5-04 | A past-end MP3 seek returns EOF and parks the decoder at EOF. | Read samples, seek past the end, then assert EOF and zero samples. Restoring the original audio implementation made the test fail. |
| S-01 | Open: standalone MP3 encoder delay/padding is not trimmed. | README now distinguishes decoded length from gapless audible length. A fix needs validated semantics and compatibility tests. |
| S-02 | Not confirmed by the auditor; not claimed fixed. | Callback/state synchronization and reentrancy need dedicated tests. |
| S-03 | Incomplete ID3 prefix/header no longer claims MP3. | Short-prefix and incomplete-header tests in `audit_audio_late_tests.h`. |

Changes to type detection, EOF seek and restart budget are observable API
behavior changes. Leading-tag handling does not claim support for arbitrarily
chained tags in `Audio_Open` or `File_DetectType`.

## Verification

- `make check`: exit 0 after the final source change; default 644/644,
  32-bit 645/645, 13 CMake configurations, ASan/UBSan and host/port checks.
- `make tsan`: 649/649, zero warnings, after the final source change.
- `make test FM_USE_OPUS_LIB=1`: 649/649, including two tagged Opus tests;
  this run preceded the short-ID3-prefix clarification in `38175c5`.
- `make audiorecordtest`: executes both audio header copies; both pass the
  AUD5-02 test. Mutations restoring each original header were caught.
- The full 57-configuration matrix passed, zero failures, on a copy made just
  before `38175c5`. That short-prefix clarification was covered by the
  default suite, full `make check` and TSan, but not another full matrix.
  Matrix ASan was not run.
- One earlier threaded `FM_FILE_FORCE_BOUNCE` matrix run produced a media
  checksum mismatch. This matrix passed that configuration, but one pass does
  not establish that the intermittent issue is gone.

No ARM toolchain or board was used; SD/I2S timing, USB interrupt priority,
hardware underrun and board playback were not verified. The report's fuzzing,
truncation sweeps and ffmpeg comparisons were not independently rerun; its
harness directory is absent. Callback reentrancy, slow callbacks, allocation
failure injection, AAC plugin edge cases and older mutation survivors remain
open.
