# External audit prompts (follow-up round)

Four self-contained prompts, one per auditor. Each auditor gets exactly one
file and nothing else; the prompts do not depend on each other.

| File | Part | Report expected |
|---|---|---|
| `01_file_storage_filesystem.md` | files, storage, filesystems, utilities, drivers, the log layer | `AUDIT_FILE_V3.md` |
| `02_image.md` | BMP / PNG / JPEG, pixel operations | `AUDIT_IMAGE_V3.md` |
| `03_video.md` | containers, H.264, MJPEG, A/V timing, media cache | `AUDIT_VIDEO_V3.md` |
| `04_audio.md` | audio decoders, writers, recorder, player, type detection | `AUDIT_AUDIO_V3.md` |

All four pin the same commit (`a6cf829` on `arena/ba6a87b3-clain-filemanagaer`)
and forbid any change on GitHub.

This is the second time the four parts are audited. The first round audited
commit `8dd0f25`; its reports are in `ReportV2.txt` at the repository root, and
the prompts used then are in the history of this folder (commit `2ba7864`).

What is different in this round:

- Each auditor first **checks the fixes** of the earlier findings of that part
  (fixed / partly fixed / not fixed / fixed but broke something), then audits
  the code that changed, then goes on to what the first round did not reach.
- Each report ends with a **verdict table** for the earlier findings and
  **seven scores** (0 to 10, each with its evidence) on the same axes for all
  four parts, so that the rounds can be compared.
- After the first follow-up prompts were written the maintainers compared the
  library's output with ffmpeg's on real files and found defects that the
  first round and the whole suite had missed (audio tracks of video files,
  H.264 and JPEG colours). Every prompt says so and asks for the same
  comparison in its own part before anything else.
- A 32-bit build is available (`make m32`), so 32-bit questions are to be
  settled by running, not by reading.
- The debug levels and the log output are in scope: each auditor checks the
  messages of that part; the file auditor also audits the mechanism.

## Round for the save / record part (`05_save_record.md`)

| File | Part | Report expected |
|---|---|---|
| `05_save_record.md` | saving and recording: audio encoders, MJPEG-AVI writer, JPEG encoder, the recorders and callbacks of `SimpleAudio` / `SimpleVideo`, the save examples, the board glue — and how these work together | `AUDIT_SAVE_V1.md` |

It pins commit `523d2da` on `arena/ba6a87b3-clain-filemanagaer` and, like the
others, forbids any change on GitHub. It is self-contained: the auditor gets
this one file and nothing else.

How it differs from prompts `01`–`04`:

- This code is new (`c688b9e`, `4cbd8a0`, `523d2da`) and has had no audit, so
  there is no table of earlier findings. In its place is a list of twelve
  **leads from the author's own review** (L1–L12): things he suspects or could
  not measure. The report must give a verdict on each.
- The main job is **combination**, not single functions: recording together
  with both players, their callbacks, the UART commands of the board and the
  timing of the main loop.
- The first job is again **comparison with reference tools** (ffmpeg, `flac
  -t`, another JPEG decoder), because the suite reads most files back with the
  library's own readers.
- Scores: the seven axes A–G of the other prompts, plus **H. Working
  together**; the report ends with "the three things to fix first".
