# External audit, follow-up round — files, storage and filesystems

You are an independent auditor. Your job is to **find defects** in one part of an
embedded C library, **prove** each one, and **propose** a fix. You do not fix
anything in the repository.

## 1. What you are auditing

- Repository: `https://github.com/hamid-enf/Clain_Filemanagaer`
- Branch: `arena/ba6a87b3-clain-filemanagaer`
- Commit: `2a45d9d` (audit exactly this commit; quote it in your report)
- Previous audit of this part: commit `8dd0f25`, report in `ReportV2.txt` at the
  repository root, lines 1–1087
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
make test                 # whole suite, about 478 tests, about 15 s
make asan                 # same suite under ASan + UBSan
make tsan                 # threaded suite under ThreadSanitizer
make m32                  # suite built and run as a 32-bit program (clang);
                          #   needs:  pip install ziglang   (make m32 M32=ubsan adds UBSan)
make check                # everything: tests, sanitizers, ports, examples, cmake, 32-bit
make matrix-quick         # a subset of the 49 build configurations
make test FM_USE_THREADS=1
make test FM_USE_IMAGE=0  # every FM_USE_* switch can be set like this
make test CFLAGS_EXTRA="-DFM_DEBUG_LEVEL=3"   # debug level 0, 1 (default), 2, 3
FM_TEST_LOG=1 ./fm_test 2>log.txt             # see every message the library prints
```

- The tests are in `test/main.c` (about 20 000 lines; suites are functions named
  `test_*`, registered with `RUN_SUITE` near the end), `test/audit_v2_tests.h`
  (the tests written for the findings of the previous audit, suites
  `test_audit_v2_*`) and `test/debug_log_tests.h`. They are the best source of
  working call sequences for every API.
- Configuration lives in `include/fm_config.h` (`FM_USE_*` feature switches and
  size limits). The `asan` target in the `Makefile` shows how to build an
  instrumented `libfilemanager.a` to link your own harness against
  (`-Iinclude -Ibackends -Isrc`).
- To feed a file to the library from a harness, mount a RAM disk and write the
  bytes into it: see `FileManager_MountMemory(...)` in `test/fatfs_host_test.c`
  and the many `RamDisk_Init` / `Storage_Register` / `FMFS_Init` / `FS_Mount`
  sequences in `test/main.c`.
- For a 32-bit harness of your own, use the compiler `tools/check_32bit.sh`
  uses: `python3 -m ziglang cc -target x86-linux-musl` (a static 32-bit x86
  program that runs on a 64-bit Linux). The same tool cross-compiles for the
  real target, which is enough to measure code size and stack frames:
  `python3 -m ziglang cc -target arm-linux-musleabihf -mcpu=cortex_m7 -mthumb -Os -fstack-size-section`.
  (This clang writes no `.su` files for `-fstack-usage` on that target; the
  frame sizes are in the `.stack_sizes` section of each object, which
  `llvm-readobj --stack-sizes` prints. The frames above 1 KB measured this way
  are in `README.md`, "Stack".)
- Source files use CRLF line endings. Some comments are in Persian; the code and
  identifiers are English.

## 4. This is a follow-up audit: what changed, and your three jobs

This part was audited once before, at commit `8dd0f25`. That report is in the
repository: `ReportV2.txt` at the repository root, lines 1–1087 (the other
line ranges are the other three parts). Read it first, including its sections
"Examined and found sound" and "Not examined". Everything it found has been
worked on since; `git log 8dd0f25..2a45d9d` and
`git diff 8dd0f25..2a45d9d -- src/fm_file.c src/fm_directory.c src/fm_text.c src/fm_stream.c src/fm_safe.c src/fm_logger.c src/fm_log.c src/fm_alloc.c src/fm_filesystem.c backends include` show how.

**What the maintainers found themselves after `cf187af`, and what it tells
you.** While measuring stack frames for the ARM target, one frame led to the
audio track of a video file, and from there files written by ffmpeg were
decoded by the library next to ffmpeg's own decode. That comparison, which no
audit round and no test had made, found: an MP3 track that decoded one frame
and then failed for ever; IMA and Microsoft ADPCM tracks that decoded to noise;
Vorbis tracks that never decoded; H.264 pictures 25 dB from the reference
(wrong colour range); JPEG green wrong in saturated colours; Motion JPEG in an
`.mp4` not playable. All of that had passed the whole suite under every
sanitizer, because the suite checked that a format is *recognised* and that
nothing *crashes*, not that what comes out is *right*. These are fixed
(commits `7d4d009`, `b3525f7`, `2a45d9d`; tests in `test/audit_v3_tests.h`).
Draw the conclusion for your part: **wherever the library produces data,
compare that data with a reference implementation**, on files a real tool
wrote, before anything else.

**Job 1 — check the fixes (do this first, it is the cheapest way to find a
defect).** For every earlier finding listed below, decide: fixed, partly fixed,
not fixed, or fixed but something else broke. A fix that handles exactly the
reported input and not its neighbours is "partly fixed": vary the input (other
sizes, other backends, other build switches, 32 bits, threads on, the same
mistake in the sibling function). Read the test that was added for it
(`test/audit_v2_tests.h`) and judge whether it would really fail if the fix
were removed; if you can, remove the fix locally and see.

| Earlier ID | What was wrong | What the fix claims |
|---|---|---|
| FILE-01 | `File_Truncate` on a read-only handle aborted (LittleFS assert) | refused with `FM_ERR_ACCESS` on every backend |
| FILE-02 | `File_Append` wrote at a stale size and overwrote data | the size is taken with the mount locked. LittleFS shares nothing between two handles of one file, so there the adapter allows **one writer or many readers**: a conflicting open returns `FM_ERR_BUSY` (`lfs_open_conflict` in `backends/fm_littlefs.c`) |
| FILE-03 | FatFS adapter moved the file position on truncate | position kept |
| FILE-04 | `Text_ReadLine` deleted every CR | only the CR of a CRLF line end is removed |
| FILE-05 | `Stream_InitFile` leaked a pool slot | no leak; a second `Stream_InitFile` on a stream in use is refused with `FM_ERR_BUSY` |
| FILE-06 | POSIX adapter opened a directory as a file | `FM_ERR_IS_DIRECTORY` |
| FILE-07 | `FM_DEBUG_LEVEL=0` did not compile with `-Werror` | compiles; levels 0, 2, 3 and `FM_LOG_DISABLE` are in the build matrix |
| SUSP-01 | recursive directory walks could exhaust the stack | all tree walks in `src/fm_directory.c` are loops, depth limited to `FM_DIR_MAX_DEPTH` (16); a deeper tree returns `FM_ERR_TOO_LARGE` |
| SUSP-02, SUSP-04 | `File_Transaction` / safe replace could lose the original when a step failed | the original stays; when the restore itself fails the backup is kept as `<path>.fm_tmp.bak` |
| SUSP-03 | explorer listing silently cut | `Explorer_IsTruncated()` reports it |
| (own) | the `*` filter matcher was exponential (33 s on a 28-character name) | `wildcard_match` in `src/fm_directory.c` and `wildcard_match_local` in `src/fm_logger.c` are loops |

**Job 2 — audit what is new.** Code written since the last audit has been
through the fewest eyes. Go through the diff above line by line.
In this part that is above all: the LittleFS open-conflict rule (can it be
bypassed through rename, delete, truncate by path, or `File_Copy` onto an open
file? does a failed open leave the conflict table wrong? what about threads?),
the iterative directory walks (state after a failure half way, a tree that
changes while it is walked, depth exactly 16 and 17 on every backend), the
restore path of `File_Transaction`, the two rewritten wildcard matchers
(compare them with a reference matcher on random patterns and names), and
**the whole log layer**, which was rewritten and is yours: `src/fm_log.c`,
`include/fm_log.h`, `include/fm_debug.h`, `include/fm_log_uart_hal.h`.

**Job 3 — go where nobody has been.** The previous auditor of this part worked with
`FM_USE_THREADS=0` only, did not get to Tier 3 (the drivers) at all, and read
`fm_binary.c`, `fm_crc.c`, `fm_async.c`, `fm_transfer.c`, and half of
`fm_json.c`, `fm_logger.c` and `fm_storage.c` only in outline. No second
power-fail campaign was written and no differential tester across backends
was built. Start there: concurrency, the drivers, those files, and the
differential tester of section 6.

What is already established (running it again will find nothing): the suite
passes in 49 build configurations under ASan + UBSan, under ThreadSanitizer
through three synchronisation ports, and as a 32-bit x86 program built with
clang, with and without UBSan. Methods most likely to pay off now:

- **Fuzzing** of every parser with ASan + UBSan (libFuzzer or AFL++), seeded
  from `test/vectors/` and from files you generate. Say how many executions
  and what coverage you reached.
- **Differential testing** against a reference implementation.
- **32-bit harnesses**: a 32-bit build exists now (section 3), so size
  arithmetic near 2^32 can be tested for real instead of argued about. Build
  your fuzz and fault-injection harnesses for 32 bits too.
- **Fault injection**: a read or write that fails in the middle, a short read,
  an allocation that fails (every Nth, for every N), a file truncated at every
  byte offset.
- **Static analysis** (`gcc -fanalyzer`, clang static analyzer, cppcheck) —
  but triage it: report a warning only after you have confirmed it is real.
- **Stack measurement** (`-fstack-usage` on the host, `-fstack-size-section`
  for the ARM target, see section 3): frames above about 1 KB in a call path the application reaches.
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

**Debug levels and log output (new since the last audit).** The library prints
through `FM_ERR_LOG` (level 1 and up), `FM_EVENT_LOG` (2 and up) and
`FM_TRACE_LOG` (3); level 0 compiles all of it out. Read the top of
`include/fm_debug.h` and "Rules for a sink" in `include/fm_log.h`, then check
the messages of **your** files (`grep -n "FM_ERR_LOG\|FM_EVENT_LOG\|FM_TRACE_LOG\|FM_CHECK" src/fm_file.c src/fm_filesystem.c src/fm_storage.c src/fm_fmfs.c src/fm_transfer.c src/fm_qspi.c src/fm_spiflash.c src/fm_alloc.c`):

- A `%s` whose argument comes from a file (a name, a tag, a codec string): can
  it be NULL, not terminated, or longer than its buffer? Can file content put
  control characters or a megabyte of text on the user's console?
- A message on a path that runs per frame, per sample block or per storage
  read at level 1 or 2 (level 3 is allowed to be slow, levels 1 and 2 are not).
- An error line for an outcome that is normal (file not found, end of file),
  and the reverse: a real failure in your part that prints nothing at level 1,
  so that the user sees only a result code. List the ones that would have cost
  you time.
- Behaviour that depends on the level: an expression with a side effect inside
  a log argument (it disappears at level 0), a variable that is only assigned
  for a message. Run your harnesses at levels 0 and 3 and compare results.
- Build your part with `-DFM_DEBUG_LEVEL=0`, `=3` and `-DFM_LOG_DISABLE` under
  `-Werror`, with each of your `FM_USE_*` switches off.

The mechanism itself is in your scope and deserves a harness of its own:

- `FM_Log_Tagged` / `FM_Log_V` / `FM_Log_Write` (`src/fm_log.c`): truncation at
  exactly `FM_LOG_BUF_SIZE` − 2, − 1, + 0, + 1 with and without a tag;
  `FM_LOG_BUF_SIZE` at its minimum (64) and with a tag of 23, 24 and 100
  characters; a `vsnprintf` that fails; a NULL format, tag or sink context.
- The one-message-at-a-time guard (`FM_USE_THREADS=0`): a sink that logs, a
  sink that calls a library function that logs, a message from a signal
  handler standing in for an interrupt at every point of a message in
  progress. Nothing may recurse, overwrite the buffer or stay blocked;
  `FM_Log_Dropped()` must count right.
- `FM_USE_THREADS=1`: many threads logging at level 3 under ThreadSanitizer;
  `FM_Log_SetSink` called while others log (the sink and its context are two
  separate words); the stack a message needs in the calling task (the buffer
  is on that stack) against the stack sizes the examples configure.
- The claim in `include/fm_sync.h` about which user callbacks run with a lock
  held, and the rule that a sink must not call back into the library: find
  every log line that is emitted with a lock held and say which lock. Is there
  a line that makes the ready-made blocking UART sink
  (`include/fm_log_uart_hal.h`, 100 ms timeout) stall another task for long?
- `include/fm_debug.h`: `FM_CHECK`, `FM_CHECK_PTR`, `FM_CHECK_VOID` in every
  position a statement can appear (unbraced `if` / `else`), a user-defined
  `FM_DEBUG_PRINTF`, `FM_LOG_DISABLE` together with each level, C++ inclusion.
- Does what `fm_debug.h` and the README section "Debug levels and log output"
  say each level prints match what is printed? (`FM_TEST_LOG=1 ./fm_test` at
  each level gives you the full list.)

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

- A name of `FM_NAME_MAX` (64) characters or more: listing its directory fails
  with `FM_ERR_TOO_LARGE` and `Directory_DeleteRecursive` cannot remove it.
  Documented in `README.md` ("File rules worth knowing"); the workaround is
  `FM_NAME_MAX=256`. The owner has accepted this behaviour.
- Directory trees deeper than `FM_DIR_MAX_DEPTH` (16) are refused with
  `FM_ERR_TOO_LARGE`.
- LittleFS: one writer or many readers per file (see FILE-02 above).
- The log sink is called from wherever the message is produced, which can be
  inside a filesystem operation with its lock held; a sink must not call back
  into the library when `FM_USE_THREADS=1`. With `FM_USE_THREADS=0` a message
  that arrives while another is in progress is dropped and counted. Level 3
  changes timing. All of this is documented; report it only if the code does
  something worse than the documentation says.
- Everything the previous audit reported and the table in section 4 marks as
  fixed or documented: report it again only if your verdict in Job 1 is not
  "fixed".

Vendored third-party code (`third_party/`) is not ours to audit line by line.
A defect inside it counts only if you can reach it through this library's public
API with a concrete input. A defect in the **glue** — how this library calls the
third-party code, what it does with the results — is fully in scope.

## 8. The report

Deliver **one Markdown file** named `AUDIT_FILE_V3.md`. Structure:

1. **Summary in Persian** (5 to 10 lines): how many confirmed defects by
   severity, the single most important one, how many of the earlier findings
   are really fixed, your seven scores, and what you could not examine.
   The rest of the report is in English.
2. **Audit record**: commit audited, machine and compiler, what you built, which
   methods you used and for how long (for a fuzzer: executions and coverage
   reached), and what you ran as a 32-bit program.
3. **Table of findings**: ID, severity, one-line title, file:line, status
   (confirmed / suspicion).
4. **One section per finding**, IDs `FILE3-01`, `FILE3-02`, ...:
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

9. **Verdict on the earlier findings** — one table: earlier ID, your verdict
   (fixed / partly fixed / not fixed / fixed but broke something), how you
   checked (which variants you tried), and whether the test added for it would
   catch a regression.
10. **Scores** — so that this round can be compared with the last one and with
   the next. Give each line a whole number from 0 to 10 and one sentence that
   names the evidence. Write "not assessed" instead of guessing; a score
   without evidence behind it is worth nothing.

   | Axis | What 10 means |
   |---|---|
   | A. Malformed input | no crash, hang or out-of-bounds access on any input you could construct, after real fuzzing |
   | B. Correct results | every valid input you tried gives the reference result |
   | C. Errors and resources | every failure returns the right code, releases what it took and leaves a usable state |
   | D. Memory and timing | stack frames, allocation sizes and worst-case call times fit a microcontroller and are bounded |
   | E. Documentation | headers and `README.md` say what the code does, including the limits |
   | F. Tests | the existing tests of this part would catch a regression in what they cover, and cover what matters |
   | G. Diagnosability | when something fails, the result code and the log line together tell a user why |

   Then one line: **ready for a product / ready under these conditions (list
   them) / not ready**, for your part only.

Put harness sources, generator scripts and crashing inputs in an appendix (or
attach them as files next to the report). Someone must be able to reproduce
every confirmed finding from the report alone.
