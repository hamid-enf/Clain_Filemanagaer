@echo off
rem ==================================================================
rem opus_prune.bat - FileManager_lib opus vendor prune helper
rem
rem The vendored libopus was trimmed to a decoder-only tree (see
rem third_party/README.md and docs/OPUS_CUBEIDE.md).  If you updated
rem your project by copying the new FileManager_lib OVER the old one,
rem Windows keeps the deleted files - and CubeIDE then fails on
rem silk\x86\*.c (xmmintrin.h), silk\float\*.c (SigProc_FLP.h),
rem NEON sources, unit tests, etc.
rem
rem This script deletes exactly those stale files.  It is generated
rem from the repository's own deletion list, safe to run repeatedly,
rem and does nothing when the tree is already clean.
rem
rem Usage:  opus_prune.bat [path\to\FileManager_lib]
rem         (default: the FileManager_lib this script lives in)
rem ==================================================================
setlocal
set "ROOT=%~dp0.."
if not "%~1"=="" set "ROOT=%~1"
set "OPUS=%ROOT%\third_party\opus"
if not exist "%OPUS%\include\opus.h" (
    echo [opus_prune] ERROR: %OPUS% not found or incomplete.
    echo [opus_prune]        Pass the FileManager_lib folder as argument.
    exit /b 1
)
echo [opus_prune] Pruning stale files in %OPUS%

rem ---- directories removed entirely from the trimmed vendor ----
if exist "%OPUS%\celt\dump_modes" rd /s /q "%OPUS%\celt\dump_modes"
if exist "%OPUS%\celt\mips" rd /s /q "%OPUS%\celt\mips"
if exist "%OPUS%\celt\tests" rd /s /q "%OPUS%\celt\tests"
if exist "%OPUS%\celt\x86" rd /s /q "%OPUS%\celt\x86"
if exist "%OPUS%\silk\fixed" rd /s /q "%OPUS%\silk\fixed"
if exist "%OPUS%\silk\float" rd /s /q "%OPUS%\silk\float"
if exist "%OPUS%\silk\mips" rd /s /q "%OPUS%\silk\mips"
if exist "%OPUS%\silk\tests" rd /s /q "%OPUS%\silk\tests"
if exist "%OPUS%\silk\x86" rd /s /q "%OPUS%\silk\x86"
if exist "%OPUS%\silk\xtensa" rd /s /q "%OPUS%\silk\xtensa"

rem ---- files removed from directories that are otherwise kept ----
if exist "%OPUS%\celt\arm\arm2gnu.pl" del /f /q "%OPUS%\celt\arm\arm2gnu.pl"
if exist "%OPUS%\celt\arm\armopts.s.in" del /f /q "%OPUS%\celt\arm\armopts.s.in"
if exist "%OPUS%\celt\arm\celt_fft_ne10.c" del /f /q "%OPUS%\celt\arm\celt_fft_ne10.c"
if exist "%OPUS%\celt\arm\celt_mdct_ne10.c" del /f /q "%OPUS%\celt\arm\celt_mdct_ne10.c"
if exist "%OPUS%\celt\arm\celt_neon_intr.c" del /f /q "%OPUS%\celt\arm\celt_neon_intr.c"
if exist "%OPUS%\celt\arm\celt_pitch_xcorr_arm.s" del /f /q "%OPUS%\celt\arm\celt_pitch_xcorr_arm.s"
if exist "%OPUS%\celt\arm\meson.build" del /f /q "%OPUS%\celt\arm\meson.build"
if exist "%OPUS%\celt\arm\pitch_neon_intr.c" del /f /q "%OPUS%\celt\arm\pitch_neon_intr.c"
if exist "%OPUS%\celt\meson.build" del /f /q "%OPUS%\celt\meson.build"
if exist "%OPUS%\celt\opus_custom_demo.c" del /f /q "%OPUS%\celt\opus_custom_demo.c"
if exist "%OPUS%\include\meson.build" del /f /q "%OPUS%\include\meson.build"
if exist "%OPUS%\silk\HP_variable_cutoff.c" del /f /q "%OPUS%\silk\HP_variable_cutoff.c"
if exist "%OPUS%\silk\arm\LPC_inv_pred_gain_neon_intr.c" del /f /q "%OPUS%\silk\arm\LPC_inv_pred_gain_neon_intr.c"
if exist "%OPUS%\silk\arm\NSQ_del_dec_neon_intr.c" del /f /q "%OPUS%\silk\arm\NSQ_del_dec_neon_intr.c"
if exist "%OPUS%\silk\arm\NSQ_neon.c" del /f /q "%OPUS%\silk\arm\NSQ_neon.c"
if exist "%OPUS%\silk\arm\NSQ_neon.h" del /f /q "%OPUS%\silk\arm\NSQ_neon.h"
if exist "%OPUS%\silk\arm\arm_silk_map.c" del /f /q "%OPUS%\silk\arm\arm_silk_map.c"
if exist "%OPUS%\silk\arm\biquad_alt_neon_intr.c" del /f /q "%OPUS%\silk\arm\biquad_alt_neon_intr.c"
if exist "%OPUS%\silk\control_codec.c" del /f /q "%OPUS%\silk\control_codec.c"
if exist "%OPUS%\silk\enc_API.c" del /f /q "%OPUS%\silk\enc_API.c"
if exist "%OPUS%\silk\init_encoder.c" del /f /q "%OPUS%\silk\init_encoder.c"
if exist "%OPUS%\silk\meson.build" del /f /q "%OPUS%\silk\meson.build"
if exist "%OPUS%\src\analysis.c" del /f /q "%OPUS%\src\analysis.c"
if exist "%OPUS%\src\analysis.h" del /f /q "%OPUS%\src\analysis.h"
if exist "%OPUS%\src\meson.build" del /f /q "%OPUS%\src\meson.build"
if exist "%OPUS%\src\mlp.c" del /f /q "%OPUS%\src\mlp.c"
if exist "%OPUS%\src\mlp.h" del /f /q "%OPUS%\src\mlp.h"
if exist "%OPUS%\src\mlp_data.c" del /f /q "%OPUS%\src\mlp_data.c"
if exist "%OPUS%\src\opus_compare.c" del /f /q "%OPUS%\src\opus_compare.c"
if exist "%OPUS%\src\opus_demo.c" del /f /q "%OPUS%\src\opus_demo.c"
if exist "%OPUS%\src\opus_encoder.c" del /f /q "%OPUS%\src\opus_encoder.c"
if exist "%OPUS%\src\opus_multistream_encoder.c" del /f /q "%OPUS%\src\opus_multistream_encoder.c"
if exist "%OPUS%\src\opus_projection_encoder.c" del /f /q "%OPUS%\src\opus_projection_encoder.c"
if exist "%OPUS%\src\osce_compare.c" del /f /q "%OPUS%\src\osce_compare.c"
if exist "%OPUS%\src\qext_compare.c" del /f /q "%OPUS%\src\qext_compare.c"
if exist "%OPUS%\src\repacketizer_demo.c" del /f /q "%OPUS%\src\repacketizer_demo.c"
if exist "%OPUS%\src\tansig_table.h" del /f /q "%OPUS%\src\tansig_table.h"

rem ---- report ----
set N=0
for /f %%I in ('dir /s /b "%OPUS%\*.c" 2^>nul ^| find /c /v ""') do set N=%%I
echo [opus_prune] Done. %N% .c files remain (expected: 102).
echo [opus_prune] Next in CubeIDE: select project, press F5 (refresh),
echo [opus_prune] then Project -^> Clean... -^> Build (regenerates makefiles).
endlocal
