# ID3 chained-tag follow-up

2026-11-10. `File_DetectType`, `Audio_Open` and the MP3 stream opener now advance over multiple consecutive ID3v2 tags rather than only the first one. The MP3 opener also rejects a declared tag extending beyond the file. `Media_DetectBuffer` already handled complete chains.

Regression coverage retains one-tag cases and adds two tags of 1000 and 1200 bytes, with and without footers, ahead of WAV, FLAC, Vorbis and (when enabled) Opus; it compares file/buffer detection and decoded PCM with the original. An MP3 chained-tag case is also included. These are bounded host fixtures, not exhaustive tests of malformed metadata or of real media/storage timing. Incomplete tags still use the detection API's existing extension/unknown fallback.

This change does not implement MP3 encoder delay/padding trimming and does not resolve the recorder's real-time card-write suspicion. Previous reports document those separately; they should not be read as claiming this change addressed them.

Verification: `make check` passed (658/658 core tests; this environment skipped CMake and 32-bit checks because those tools were not on PATH); `make tsan` passed 663/663 with zero warnings; forced clean rebuild with real libopus passed 666/666; ASan/UBSan closed-loop media suite passed all three seeds. As a regression control, the new tests were run against the unmodified baseline in a separate archive: all seven chained MP3/WAV/FLAC/Vorbis cases failed, while the fixed build passed. Opus was verified in the real-libopus build, not in that baseline control.
