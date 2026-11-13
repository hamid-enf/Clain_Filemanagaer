# MP3 gapless follow-up

2026-11-10. The previously observed leading and trailing PCM for standalone MP3 has been addressed for streams with a supported Xing/Info + LAME-compatible extension (`LAME`, `Lavc`, `Lavf`). The decoder reads the 12-bit encoder delay and padding and accounts for the 529-sample synthesis delay; `Audio_Read`, `total_frames`, `data_size`, `frame_index` and seek now use the audible sample timeline in this case. This is an observable API behavior change from the earlier untrimmed output.

Safety checks require a valid Layer III header, extension within the first frame, frame count and plausible trim; a Xing byte count, if present, must agree sufficiently with file size. Padding below 529 samples is not supported by this implementation, as it would require a decoder flush. Missing, unknown or unusable metadata falls back to the legacy untrimmed behavior. These checks cannot prove that otherwise plausible metadata is truthful. iTunSMPB and other encoder-specific metadata are not implemented.

For supported gapless streams, seeks are exact by decoding from the start, preserving Layer III reservoir and synthesis history. **This is synchronous and O(position), so seeking in long files may be slow and can interrupt real-time playback.** No hardware timing is asserted. Other MP3 seek behavior is unchanged. No practical board test was performed.

## Verification and reproduction

`python3 tools/test_mp3_gapless.py /path/to/ffmpeg --asan` (NumPy and FFmpeg with libmp3lame required) force-rebuilds the host library with ASan and UBSan and checks PCM against FFmpeg, length, awkward chunk reads, position, seeks at beginning/middle/end and repeat/EOF/restart. Twelve deterministic cases cover mono/stereo, CBR/VBR, 8/12/22.05/44.1/48 kHz and lengths from 441 to 176400 frames. Maximum accepted PCM difference is two 16-bit units. Unknown encoder, inadequate padding, inconsistent byte count, concatenation and missing Xing were checked to remain untrimmed. This tests one encoder family, not all MP3 encoders or malformed metadata. The test against the pre-fix baseline failed immediately on length (1728 versus 441 frames), demonstrating regression sensitivity.

`make check`: 658/658 core tests and host checks passed; CMake and 32-bit checks were skipped in that invocation when tools were not on PATH; rerunning them with the installed tools passed all 13 CMake configurations and the 32-bit host suite (659/659). `make tsan`: 663/663 with zero warnings. The three-seed closed-loop media suite with ASan/UBSan also passed, now checking MP3 length and PCM directly, without alignment. `tools/check_mp3_gapless.py` retains the original four-second CBR/VBR scenario and now fails if trimming is missing.

This addresses S-01 only for the documented supported metadata; it does not close the separate recorder/card timing issue SAV3-04.
