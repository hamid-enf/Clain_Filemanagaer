# FatFS, regeneration, and recorder follow-up

2026-11-10. Host-only work; no physical board, SD/I2S timing, firmware link, or CubeMX regeneration was performed.

## LOW-01: FatFS time fallback

On GCC/Clang, when `FM_FATFS_PROVIDE_DISKIO=1`, the FatFS adapter now provides a weak `get_fattime()` fallback (2020-01-01 00:00:00 in FAT format). An application-provided strong RTC implementation wins. The date is fixed, not a clock. If the RTC implementation is in a static archive, its object must actually be linked; a weak definition alone may not cause archive extraction. The function is omitted when `_FS_NORTC` or `FF_FS_NORTC` is enabled. `FM_FATFS_PROVIDE_FATTIME=0/1` and `FM_FATFS_FATTIME_VALUE` control it; on other compilers it defaults off, and if explicitly enabled it is strong. The default CubeMX-diskio configuration is unchanged and still uses the project's own time function.

`sh tools/check_fattime.sh` checks the default, strong override, custom timestamp, explicit on/off, both generations of NORTC switch and external diskio. It is included in the FatFS host suite. Additionally, real ST FatFS at commit `dd9126e957ae841a13ccbe2115d39a1fb7414126` was used with a RAM disk: actual file creation/stat returned FAT date `0x5021` using the fallback and `0x5821` with a strong RTC. Reproduce via `sh tools/check_fattime_real.sh /path/to/stm32-mw-fatfs/source` after obtaining that source separately; it is not vendored. This verifies the timestamp integration with real FatFS, not a physical storage driver.

## CubeMX lock regeneration

`python3 tools/restore_fatfs_lock.py` restores the project's `_FS_LOCK` (or `FF_FS_LOCK`) to 16 after generation; a path and `--slots` may be provided. It preserves the other bytes, rejects ambiguous/missing definitions, and is idempotent. `python3 tools/test_restore_fatfs_lock.py` exercises old/new names, CRLF, custom size and rejection without modification. The compile-time guard remains. This repository does not contain the `.ioc` or CubeIDE project, so no automatic post-generation hook could be installed; the helper must be run or hooked in the user's project. The CubeMX setting itself should also be set to 16.

## SAV3-04: slow recorder writes

The recorder still performs synchronous file writes in the audio refill. A new host characterization in `tools/audio_record_host_test.c` intercepts a write inside refill and simulates the same DMA half completing again before the write returns: the existing underrun flag is raised. This was run against both `examples` and `top_api`, including ASan/UBSan/LeakSanitizer. This demonstrates a state-machine consequence for the injected ordering, **not** measured card speed or a prediction for a particular board.

This issue is not fixed. Merely moving the same blocking write to another function in `SimpleAudio_Poll` would still block the super-loop/`FM_Idle` and could miss DMA deadlines. A real fix needs bounded buffering and an appropriate asynchronous/scheduled storage path, explicit overflow/error semantics and treatment of initial prefill, track boundaries, stop and failure. At the default 32766 channel-samples per half, preserving two prefilled stereo halves before gain would require about 128 KiB of additional PCM storage, which cannot safely be assumed on the embedded target. An untested queue or an unconditional allocation could lose samples or break existing boards, so neither was silently introduced.

## Broader host verification

`make check` passed: 658/658 core, FatFS mock suite, recorder host tests, 13 CMake configurations and 32-bit Zig host 659/659. `make tsan` passed 663/663 with no TSan warnings. A separate clean-workspace full, non-sanitized feature matrix passed 57/57 configurations; two of those configurations are build/link-only, not executed test suites. The matrix has FatFS disabled, so it is not evidence for real FatFS combinations. The recorder stall tests also passed under ASan/UBSan/leak detection with each of the two header copies. These results do not establish exhaustive input, race, or target coverage.
