# External audit prompts

Four self-contained prompts, one per auditor. Each auditor gets exactly one
file and nothing else; the prompts do not depend on each other.

| File | Part | Report expected |
|---|---|---|
| `01_file_storage_filesystem.md` | files, storage, filesystems, utilities, drivers | `AUDIT_FILE.md` |
| `02_image.md` | BMP / PNG / JPEG, pixel operations | `AUDIT_IMAGE.md` |
| `03_video.md` | containers, H.264, MJPEG, A/V timing, media cache | `AUDIT_VIDEO.md` |
| `04_audio.md` | audio decoders, writers, recorder, player, type detection | `AUDIT_AUDIO.md` |

All four pin the same commit (`8dd0f25` on `arena/ba6a87b3-clain-filemanagaer`)
and forbid any change on GitHub.
