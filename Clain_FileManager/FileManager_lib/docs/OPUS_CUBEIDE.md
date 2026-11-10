# Opus in STM32CubeIDE — build guide & post-mortem

## Symptoms (before the fix)

Adding the Opus feature to the CubeIDE project (`FileManager_lib` pulled
into `Projects/FileManager`) broke the build with errors like:

```
opus_decoder.c:34: error: #error "OPUS_BUILD _MUST_ be defined to build Opus..."
celt/stack_alloc.h:39: error: #error "Opus requires one of VAR_ARRAYS, USE_ALLOCA,
                     or NONTHREADSAFE_PSEUDOSTACK be defined..."
celt/float_cast.h:142: warning: "Don't have the functions lrint() and lrintf()."
```

## Root cause

`third_party/opus/` is a vendored copy of the **libopus reference codec**
(the same Xiph.org implementation used by Firefox/Chrome/WhatsApp).  The
library build systems for it (CMake/meson) pass `-DOPUS_BUILD
-DVAR_ARRAYS ...` per target.  STM32CubeIDE instead compiles **every
`.c` file** under the project with one global flag set — and those
defines were never added to `.cproject`.  Two further landmines were
waiting behind that one: ~18 vendored demo/test files contain their own
`main()` (duplicate-symbol link errors), and the SILK encoder sources
need `-Isilk/float -Isilk/fixed`, which the project also does not have.

On top of the build errors, the feature itself was hollow: `src/fm_opus.c`
only *silenced* — it never called `opus_decode()` — and
`opus_file_open()` allocated a PCM buffer and threw the compressed data
away without decoding.  Even a successful build played silence.

## Fix (this repo; nothing to add in CubeIDE)

1. **Vendor trimmed to a decoder-only tree** — no `main()`-bearing
   demo/test files, no encoder sources, no NEON/x86/MIPS SIMD dirs
   (details in `third_party/README.md § opus`).  The remaining object
   set is link-complete: verified by linking *all* objects with a
   decoder-only test program and no `--gc-sections`.
2. **IDE-safe defaults inside the vendor** (search `FM vendor patch`):
   `OPUS_BUILD=1` via `include/opus_types.h`, `VAR_ARRAYS` via
   `celt/stack_alloc.h`, `HAVE_LRINTF` via `celt/float_cast.h` on
   glibc/newlib.  Explicit `-D` flags from `.cproject` still win;
   `-DFM_OPUS_NO_VENDOR_DEFAULTS` disables the defaults.
   **→ No `.cproject` changes are required.**  The include paths you
   already have (`third_party/opus{,/include,/celt,/silk,/src}`) are
   sufficient.
3. **Real decode wired in** `src/fm_opus.c` + `opus_file_open()`
   (`FM_OPUS_USE_LIBOPUS=1` automatically on-target):
   * OpusHead parse (channels, pre-skip), full Ogg page/lacing walker
     (incl. packets spanning pages), exact duration from the final page
     granulepos, RFC-accurate TOC frame-duration math.
   * PLC (packet-loss concealment) for missing packets via
     `opus_decode(NULL)`.
   * Also serves `FM_VIDEO_AUDIO_OPUS` tracks in Matroska video.

### If you prefer explicit flags anyway

Project → Properties → C/C++ Build → Settings → MCU GCC Compiler →
Preprocessor → Defined symbols:

```
OPUS_BUILD=1
VAR_ARRAYS=1
HAVE_LRINTF=1
```

(Optional: `FIXED_POINT=1` for the fixed-point build — smaller, FPU-free.)

## Host verification (what was run)

* `make -j4 test` — 211/211 pass (silence fallback path).
* `make -j4 FM_USE_OPUS_LIB=1 test` — 211/211 pass; asset decodes to
  exactly 48000 samples (1.000 s) with peak amplitude 8343 (real audio).
* `make asan FM_USE_OPUS_LIB=1` — 211/211 under ASan+UBSan (the Ogg
  walker is memory-clean).
* `sh tools/check_ports.sh` — all 8 MCU ports compile.
* Whole vendor tree compiled with **zero** `-D` flags (as CubeIDE does):
  0 errors, 0 warnings.

## Memory on the H7 board

* Whole-file decode model (like FLAC/Vorbis here): file ≤
  `FM_OPUS_MAX_FILE_BYTES` (5 MB), PCM ≤ `FM_OPUS_MAX_PCM_BYTES`
  (16 MB default → ~87 s stereo 48 kHz; tune in `fm_board_config.h`).
* libopus `malloc`s one decoder state (~20 KB stereo) — make sure the
  newlib heap (or your `FM_Allocator_Set` / `_sbrk` redirection) covers
  it; CubeIDE projects typically place the heap in SDRAM already.
* Peak RAM during `Audio_Open`: compressed file + PCM buffer; the
  compressed copy is freed before playback starts.

## Troubleshooting: "xmmintrin.h / SigProc_FLP.h / tests" errors after updating

If after updating your copy you now get errors like:

```
silk/x86/NSQ_sse4_1.c:  fatal error: xmmintrin.h: No such file or directory
silk/x86/x86_silk_map.c: fatal error: SigProc_FLP.h: No such file or directory
silk/float/x86/inner_product_FLP_avx2.c: fatal error: SigProc_FLP.h
silk/tests/test_unit_LPC_inv_pred_gain.c ...
```

your disk still contains files that **no longer exist in this repo**
(the whole `silk/x86`, `silk/float`, `silk/fixed`, `silk/tests`,
`celt/x86`, `celt/mips`, `celt/tests`, `celt/dump_modes`, `silk/mips`,
`silk/xtensa` trees, the NEON/NE10 sources, and the encoder/demo files —
146 paths in total).  This happens when the update was applied by
**copying the new `FileManager_lib` over the old one**: Explorer merge
overwrites same-name files but never deletes removed ones.  The tell is
that the original `OPUS_BUILD`/`VAR_ARRAYS` errors are *gone* (the new
patched headers arrived) yet `silk/x86` still compiles.

Two equivalent fixes:

1. **Clean copy (safest):** delete `FileManager_lib\third_party\opus`
   completely, then copy the `third_party\opus` folder fresh from this
   branch.
2. **Prune script:** run `tools\opus_prune.bat` (shipped in this
   branch).  It deletes exactly the 146 stale paths, is idempotent,
   and prints the remaining `.c` count — it must say **102**.

Either way, finish inside CubeIDE so the generated makefiles are
regenerated without the deleted folders:

1. Select the project in Project Explorer, press **F5** (refresh).
2. **Project → Clean…** → clean the FileManager project (or delete its
   `Debug`/`Release` folder) — the stale `subdir.mk` files under
   `Debug/FileManager_lib/third_party/opus/...` still reference the
   deleted sources.
3. Build.

## Troubleshooting: "multiple definition of `stb_vorbis_*`" at link

After the Opus compile issues are resolved the build reaches the link
step and may fail with ~28 errors like:

```
ld.exe: ./FileManager_lib/third_party/stb/stb_vorbis.o:
        multiple definition of `stb_vorbis_decode_memory';
        ./FileManager_lib/src/fm_vorbis.o: ... first defined here
```

Cause: `stb_vorbis` is a single-file library.  `src/fm_vorbis.c` used
to `#include "../third_party/stb/stb_vorbis.c"` with
`STB_VORBIS_IMPLEMENTATION`, compiling the whole implementation into
`fm_vorbis.o` — while CubeIDE *also* compiles the vendored
`third_party/stb/stb_vorbis.c` as its own object (IDE builds compile
every `.c`).  Two full implementations → duplicate symbols.  The host
Makefile never compiled the vendored `.c`, which is why host CI was
blind to it.

Fix (in this branch, nothing to do in CubeIDE): `fm_vorbis.c` now
includes stb_vorbis in `STB_VORBIS_HEADER_ONLY` mode (declarations
only); the vendored `third_party/stb/stb_vorbis.c` object is the single
implementation, and the host Makefile/CMake compile it too — so every
build system now links exactly one copy.
