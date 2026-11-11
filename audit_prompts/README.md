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
