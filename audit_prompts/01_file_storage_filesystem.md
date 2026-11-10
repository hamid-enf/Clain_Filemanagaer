# External audit — files, storage and filesystems

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `8dd0f25` (audit exactly this commit; quote it in your report)
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

Your part is **files, storage and filesystems**. Three other auditors cover the other parts
(image, video, audio). Stay in your part; if you trip over something outside it, give it
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
make test                 # whole suite, about 358 tests, about 12 s
make asan                 # same suite under ASan + UBSan
make tsan                 # threaded suite under ThreadSanitizer
make check                # everything: tests, sanitizers, ports, examples, cmake
make matrix-quick         # a subset of the 45 build configurations
make test FM_USE_THREADS=1
make test FM_USE_IMAGE=0  # every FM_USE_* switch can be set like this
```

- All tests are in `test/main.c` (about 20 000 lines; suites are functions named
  `test_*`, registered with `RUN_SUITE` near the end). It is the best source of
  working call sequences for every API.
- Configuration lives in `include/fm_config.h` (`FM_USE_*` feature switches and
  size limits). The `asan` target in the `Makefile` shows how to build an
  instrumented `libfilemanager.a` to link your own harness against
  (`-Iinclude -Ibackends -Isrc`).
- To feed a file to the library from a harness, mount a RAM disk and write the
  bytes into it: see `FileManager_MountMemory(...)` in `test/fatfs_host_test.c`
  and the many `RamDisk_Init` / `Storage_Register` / `FMFS_Init` / `FS_Mount`
  sequences in `test/main.c`.
- Source files use CRLF line endings. Some comments are in Persian; the code and
  identifiers are English.

## 4. What has already been done (do not repeat it)

The suite passes in 45 build configurations under ASan + UBSan, and under
ThreadSanitizer through three synchronisation ports. Running the existing tests
again will find nothing. The defects that are left are the ones those tests do
not reach. Methods that have **not** been applied yet, and are therefore the
most likely to pay off:

- **Fuzzing** of every parser with ASan + UBSan (libFuzzer or AFL++), seeded
  from `test/vectors/` and from files you generate.
- **Differential testing** against a reference implementation.
- **A 32-bit build** (`-m32`, if your machine has the multilib packages) of the
  suite and of your harnesses.
- **Fault injection**: a read or write that fails in the middle, a short read,
  an allocation that fails, a file truncated at every byte offset.
- **Static analysis** (`gcc -fanalyzer`, clang static analyzer, cppcheck) —
  but triage it: report a warning only after you have confirmed it is real.
- **Stack measurement** (`-fstack-usage`): frames above about 1 KB in a call
  path the application reaches.
- **Careful reading** for: integer overflow in size arithmetic, lengths taken
  from a file and used unchecked, state left half-changed when a function
  returns an error, handles or slots not released on error paths, loops whose
  exit depends on file content, behaviour that differs from what the header
  comment or `README.md` promises.

## 5. Your scope

This is the largest part, about half of the library. Work through it in this
order and say in the report how far you got.

**Tier 1 — the core (spend most of your time here)**

| File | What it is |
|---|---|
| `src/fm_file.c`, `include/fm_file.h` | file API: open, read, write, seek, truncate, copy, move, edit |
| `src/fm_filesystem.c`, `include/fm_filesystem.h` | VFS: mount table, locking, dispatch to backends |
| `src/fm_directory.c`, `src/fm_path.c` | directories, path normalisation |
| `src/fm_storage.c` | storage registry, block devices |
| `src/fm_fmfs.c` | FMFS, the library's own filesystem (two tables, move journal, power-fail safe layout "v2") |
| `backends/fm_littlefs.c`, `backends/fm_fatfs.c`, `backends/fm_posix_fs.c`, `backends/fm_ramdisk.c` | filesystem adapters |
| `src/fm_core.c`, `include/fm_sync.h`, `ports/*/fm_sync_impl.h` | init, locks, RTOS ports |

**Tier 2 — utilities built on the file API**

`src/fm_stream.c`, `fm_text.c`, `fm_binary.c`, `fm_json.c`, `fm_logger.c`,
`fm_safe.c` (crash-safe writes), `fm_crc.c`, `fm_async.c`, `fm_explorer.c`,
`fm_transfer.c`, `fm_alloc.c`, `fm_log.c`.

**Tier 3 — device drivers (no hardware available: review by reading)**

`src/fm_device.c`, `fm_eeprom.c`, `fm_eeprom_chips.c`, `fm_qspi.c`,
`fm_spiflash.c`, `fm_sd_hal.c`, `fm_usb_msc.c`, and their host tests
(`test_qspi`, `test_spiflash`, `test_eeprom_usb` in `test/main.c`).

## 6. Where to dig

**The file API across backends.** The same sequence of operations must give the
same observable result on FMFS (RAM disk), LittleFS, POSIX and FatFS (mock, see
`make fatfs` and `test/fatfs_mock/`). Write a differential tester: random
sequences of create / write / append / seek-past-end + write / truncate up and
down / rename / delete / mkdir / rmdir / list, run on every backend, compare
contents, sizes, listings and error codes. Any difference is a finding unless
`README.md` documents it.

- Open-mode matrix (read, write, create, truncate, append and their
  combinations) on existing and missing files, on directories, on read-only
  storage.
- Rename onto an existing file, onto a directory, onto itself, into its own
  subtree; delete or rename a file that is open; unmount or format with handles
  open.
- Paths: `..`, `.`, `//`, trailing slashes, the longest path `FM_PATH_MAX`
  allows and one byte more, names that are a prefix of a mount point (`/sd` and
  `/sdcard`), empty components, non-ASCII bytes.
- Handle pools: open until exhaustion, then check that every error path gives
  the slot back (loop 10 000 times over each failing call and see if a later
  valid call still succeeds).
- Offsets and sizes near 2^31, 2^32 and above: which fields are 32-bit, which
  64-bit, and where they are mixed.
- `File_Copy` / `File_Move` across two mounts when the destination fills up or
  a write fails half way: what is left behind?
- Every function when the storage returns an error on its Nth read or write,
  for every N. The harness in `test/main.c` (functions `pf_*`, suite
  `test_fmfs_powerfail`) already does this for FMFS; extend it with other
  seeds, volume sizes, block sizes and operation mixes, and apply the same idea
  to LittleFS.

**FMFS on-disk format.** Read the long comment at the top of `src/fm_fmfs.c`
first. Then:

- Corrupt the stored image and mount it: bit flips and random bytes in both
  tables and in the journal, impossible offsets and sizes, overlapping files,
  entries pointing past the end, a journal record that refers to a file that is
  gone. Mount must refuse cleanly or recover; it must never crash, loop, or
  write outside the volume. The table has a CRC — also test images where the
  CRC is valid and the content is nonsense.
- The power-fail guarantee is stated in `README.md` ("FMFS notes"). Try to
  break it: longer campaigns, cuts during recovery of a recovery, volumes
  filled to the last block, the smallest volume that still gets the v2 layout,
  `FM_FMFS_POWER_SAFE` 0 / 1 / 2, `FM_FMFS_MOVE_STEPS` at 1 and at the default.
- Old "v1" volumes must still mount and work.

**Concurrency** (`FM_USE_THREADS=1`). `make tsan` is clean on the existing
tests (`test_threads`, `test_threads_wide`). Write new stress tests: many
threads on one file, on one directory, mounting and unmounting while others do
I/O, callbacks that call back into the library. Look for lock-order inversions,
callbacks run with a lock held, and anything reachable from an interrupt.

**Utilities.**

- `fm_json.c` is a hand-written parser and writer: fuzz it. Deep nesting, every
  escape form, numbers at the limits, truncated input, output buffer exactly
  full.
- `fm_safe.c`: interrupt it between every two storage writes; after a restart
  the file must be the old version or the new one, never a mix or missing.
- `fm_text.c` line editing at the first line, the last line, a line longer than
  the buffer, a file without a final newline, CRLF.
- `fm_logger.c` rotation and the volume filling up.
- `fm_alloc.c`: alignment, size arithmetic overflow, fragmentation, freeing a
  pointer twice or one that was never allocated.
- `fm_async.c`: cancel at every stage; completion callbacks after shutdown.

## 7. Known limits — do not report these

They are documented and accepted. Report them only if the real behaviour is
worse than what is written here.

- Nothing has been run on real hardware. Power cuts and I/O errors are
  simulated on the host only.
- FMFS: overwriting bytes in place is not atomic (the file being written may
  tear, including the rest of the block being written; other files and the
  metadata must not be affected). No wear levelling. File data is not
  checksummed. Erase units larger than 4096 bytes get no block guarantee. A
  nearly full v2 volume can answer "no space" while a little free space is
  shown. Small volumes fall back to the v1 layout, which is not power-fail
  safe.
- A synchronisation port that cannot identify the calling thread answers
  `FM_ERR_BUSY` on re-entry.
- FatFS adapter: the `FM_FATFS_PROVIDE_DISKIO=1` path is not built on the host;
  `FatFS_MountDefault()` returns `FM_ERR_UNSUPPORTED` when the adapter does not
  provide the disk I/O.
- A filesystem context (`FMFS_Init`, `LittleFS_Init`, `FatFS_Init`) that is
  initialised and never mounted stays taken until the same `FM_FS` is
  initialised again.

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_FILE.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and whether a 32-bit build was possible.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `FILE-01`, `FILE-02`, ...:
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

Put harness sources, generator scripts and crashing inputs in an appendix (or
attach them as files next to the report). Someone must be able to reproduce
every confirmed finding from the report alone.
