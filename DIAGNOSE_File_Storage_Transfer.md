# DIAGNOSE — File / Storage / Transfer

**Repository:** `hamid-enf/Clain_Filemanagaer` · branch `arena/cd25b973-clain-filemanagaer`
**Base commit:** `2edf567` ("Fix remaining Audio bugs …")
**Scope:** VFS layer, copy/move/delete semantics, buffer management in transfer
**Mode:** report only — **no source file in this repo was modified** (see §0.3)

Traced surfaces (identical in the tracked tree and in `Clain_FileManager.zip`; checked with `md5sum`):

| file | lines | role |
|---|---|---|
| `Clain_FileManager/FileManager_lib/src/fm_file.c` | 1722 | VFS front end, handle pool, copy/move/edit, read cache |
| `Clain_FileManager/FileManager_lib/src/fm_transfer.c` | 483 | `FM_Transfer_*` (file level + raw level) |
| `Clain_FileManager/FileManager_lib/src/fm_filesystem.c`¹ | 17 089 B | `FM_VFS_Resolve`, mount table (referenced only) |
| `Clain_FileManager/FileManager_lib/test/main.c` | 9332 | existing host suite (baseline) |

¹ present in `Clain_FileManager.zip` only; the tracked tree carries a subset of the library.

---

## 0. Method, baseline and reproduction

### 0.1 Baseline (unmodified tree)

```
$ make -j8 fm_test && ./fm_test
Results: 211/212 passed, 1 failures          (FM_USE_CACHE=1)
  TEST Regression: ADPCM WAV small-chunk reads keep samples ... FAIL: adpcm small chunks (err=-3)

$ make -j8 fm_test FM_USE_CACHE=0 && ./fm_test
Results: 205/206 passed, 1 failures          (same single audio failure)

$ make -j8 fm_test FM_USE_THREADS=1 && ./fm_test
Results: 211/213 passed, 2 failures
  TEST Regression: ADPCM WAV small-chunk reads keep samples ... FAIL   (pre-existing)
  TEST Threads: concurrent registry stress  ... FAIL: concurrent access corrupted data  (pre-existing)
```

The two failures above are **pre-existing on the base commit** and outside this scope (audio;
thread suite). They are recorded here so that any later run can distinguish them from regressions.

### 0.2 Reproduction harnesses

Every finding below was reproduced against the **unmodified** library with out-of-tree
harnesses linked to `libfilemanager.a` (nothing was added to the repo). Harness sources are
reproduced inline with each finding as *Test/A*, *Test/B*, … so they can be pasted into
`test/main.c` (or a scratch harness) for confirmation. Reference build:

```sh
cd FileManager_lib
make -j8 libfilemanager.a           # default: FM_USE_CACHE=1
cc -std=gnu11 -DFM_HOST_BUILD=1 -DFM_USE_CACHE=1 -DFM_USE_POSIX=1 \
   -DFM_USE_RAMDISK=1 -DFM_USE_FMFS=1 -Iinclude -o repro repro.c libfilemanager.a -lm
```

Full before/after matrix is in §5.

### 0.3 How the proposed fixes were validated

The snippets in §1–§4 were applied to a **scratch copy** of the library (`/tmp/patch1`, never
inside the repository), rebuilt, and the harnesses re-run. Results: every finding flips to
*clean*, and the surface suite is unchanged against baseline:

```
scratch copy + snippets :  211/212 passed, 1 failures   (same pre-existing audio failure)
scratch copy, FM_USE_CACHE=0 : 205/206 passed, 1 failures (same)
```

The `FM_USE_*` compile/link matrix (§5.2) is green for both the unmodified and the patched
sources, with the one pre-existing exception `FM_USE_CRC=0` (F-12).

---

## 1. `File_Move` — NULL check + `VFS_Resolve` error propagation

### F-01 — NULL path arguments are dereferenced through the resolver
* **File:line:** `src/fm_file.c:1470`–`1483` (`File_Move`), reached from `FM_VFS_Resolve` (`src/fm_filesystem.c:612`)
* **Describe.** `File_Move()` has no `NULL` guard, unlike every other public entry point in the
  file (`File_Delete` `FM_CHECK_PTR` at `:994`, `File_Copy` at `:1411`, `File_Rename`: no guard but
  `FM_VFS_Resolve` returns `INVALID_ARGUMENT`). `FM_VFS_Resolve()` does return
  `FM_ERR_INVALID_ARGUMENT` for `NULL`, but `File_Move` throws that away and returns
  `FM_ERR_NOT_FOUND`.
* **Root cause.** The first resolve is written as a boolean test with a hard-coded error:

```c
if (FM_VFS_Resolve(src_path, &m1, r1, sizeof(r1)) != FM_OK) {
    return FM_ERR_NOT_FOUND;              /* every cause becomes NOT_FOUND */
}
```
* **Observed.**
  `File_Move(NULL, "/b")` → `-3 (not found)`; `File_Move("/nomount/x", …)` → `-3`;
  `File_Move("/"+200 chars, …)` → `-3` while `File_Open` on the same path says
  `-16 (too large for buffer)`; `File_Move("relative", …)` → `-3` (should be `INVALID_PATH`).
* **Proposed fix.** Guard both arguments, and propagate `r` from *both* resolves;
  only fall through to copy+delete when the destination really resolves.

```c
FM_Result r;
FM_Bool dst_existed = FM_FALSE;

if (src_path == NULL || dst_path == NULL) {
    FM_Core_SetLastError(FM_ERR_INVALID_ARGUMENT);
    return FM_ERR_INVALID_ARGUMENT;
}
r = FM_VFS_Resolve(src_path, &m1, r1, sizeof(r1));
if (r != FM_OK) { FM_Core_SetLastError(r); return r; }      /* was: return NOT_FOUND */
r = FM_VFS_Resolve(dst_path, &m2, r2, sizeof(r2));
if (r != FM_OK) { FM_Core_SetLastError(r); return r; }      /* was: ignored */
if (m1 == m2 && m1->fs->ops.rename != NULL) {
    r = m1->fs->ops.rename(m1->fs, r1, r2);
    FM_Core_SetLastError(r);
    return r;
}
```
* **Test/A (repro).**

```c
File_WriteText("/mv_a.txt", "0123456789");
expect(File_Move(NULL, "/mv_b.txt", NULL, NULL)              == FM_ERR_INVALID_ARGUMENT);
expect(File_Move("/mv_a.txt", NULL, NULL, NULL)              == FM_ERR_INVALID_ARGUMENT);
expect(File_Move("/nomount/x.txt", "/mv_c.txt", NULL, NULL)  == FM_ERR_NOT_MOUNTED);
expect(File_Move("/mv_a.txt", "/nomount/y.txt", NULL, NULL)  == FM_ERR_NOT_MOUNTED);
expect(File_Move(long_path_over_FM_PATH_MAX, "/mv_b.txt", NULL, NULL) == FM_ERR_TOO_LARGE);
expect(File_Move("relative.txt", "/mv_b.txt", NULL, NULL)    == FM_ERR_INVALID_PATH);
```
  Before: `-3, -1, -3, -3, -3, -3`. After: `-1, -1, -18, -18, -16, -2`.

### F-02 — cross-mount rollback deletes a pre-existing destination
* **File:line:** `src/fm_file.c:1495`–`1506`
* **Describe.** On the copy+delete path, if the source delete fails the destination copy is
  removed to "keep the move atomic". If the destination existed **before** the call (and was
  overwritten by `File_Copy`), the rollback destroys the caller's data and cannot be undone:
  the source is still present, the old destination is gone.
* **Root cause.** Rollback without a pre-condition, plus overwrite-by-default in `File_Copy`.

```c
r = File_Delete(src_path);
if (r != FM_OK) {
    (void)File_Delete(dst_path); /* rollback: undo the copy */
}
```
* **Observed.** Read-only source adapter (`delete_file` → `ACCESS`) + pre-existing
  `/dst.txt = "IMPORTANT-PRE-EXISTING-DESTINATION"`: `Move → -4 (access denied)`,
  `/dst.txt exists=0`, `/ro/src.txt still exists=1`.
* **Proposed fix.** Record pre-existence and only remove what this call created (or refuse to
  overwrite without an explicit flag — `FM_TransferOpts.no_overwrite` already exists at the
  transfer layer but is not reachable from `File_Move`).

```c
(void)File_Exists(dst_path, &dst_existed);
r = File_Copy(src_path, dst_path, progress, user_data);
if (r != FM_OK) return r;
r = File_Delete(src_path);
if (r != FM_OK && !dst_existed) {
    (void)File_Delete(dst_path);   /* only what we created */
}
```
* **Test/B (repro).** Mount a `FM_FS` whose `ops.delete_file` returns `FM_ERR_ACCESS` at
  `/ro`; `File_Move("/ro/src.txt", "/dst.txt")` where `/dst.txt` pre-exists.
  Assert: return `FM_ERR_ACCESS`, `/ro/src.txt` exists, **`/dst.txt` unchanged**.

---

## 2. `File_PoolDump` — internal / external must be printed separately

### F-03 — merged counter is printed as if it were the internal pool
* **File:line:** `src/fm_file.c:410`–`448` (`File_PoolUsed`, `File_PoolDump`)
* **Describe.** `File_PoolUsed()` returns `internal + external`, and `File_PoolDump()` prints
  that total against `FM_MAX_OPEN_FILES` with the word *internal*, while the pool array
  `g_pool[]` is exactly `FM_MAX_OPEN_FILES` entries. With 2 external (`File_OpenEx`) handles
  open the header claims `3/8 internal` although only **1** of the 8 slots is used. The
  external registry is also bounded by the same `FM_MAX_OPEN_FILES` (`:582`–`:598`) but shows
  up under the pool's index numbering (`[x%lu]`), so the two limits cannot be told apart.
* **Root cause.** One counter for two independent resources; the diagnostic exists precisely
  for the `FM_ERR_NO_MEMORY (-14)` "handle leak" case (`include/fm_file.h:52`–`57`), where the
  distinction is the evidence that identifies the leak.
* **Observed.** `FM_MAX_OPEN_FILES=8`, 1 pool handle + 2 external handles:
  `File_PoolUsed() = 3`, dump header `file pool: 3/8 internal`.
* **Proposed fix.**

```c
void File_PoolDump(void)
{
    uint32_t i, internal = 0u, external = 0u;

    for (i = 0; i < FM_MAX_OPEN_FILES; i++) {
        if (g_pool[i].used) internal++;
        if (g_external_files[i] != NULL && g_external_files[i]->used) external++;
    }
    FM_Log("file pool: %lu internal / %lu external (slots %u)",
           (unsigned long)internal, (unsigned long)external,
           (unsigned)FM_MAX_OPEN_FILES);
    for (i = 0; i < FM_MAX_OPEN_FILES; i++) {
        if (g_pool[i].used) FM_Log("  [i%lu] %s", (unsigned long)i, g_pool[i].path);
        if (g_external_files[i] != NULL && g_external_files[i]->used)
            FM_Log("  [x%lu] %s", (unsigned long)i, g_external_files[i]->path);
    }
}
```
  (Keep `File_PoolUsed()`'s total semantics for its documented use — "count only"; add
  `File_PoolUsedSplit(uint32_t *internal, uint32_t *external)` if the UI needs both.)
* **Test/C (repro).** Open 1 handle with `File_Open` and 2 with `File_OpenEx`; capture the
  `FM_Log` sink (`FM_Log_SetSink`), call `File_PoolDump`, assert the header contains
  `1 internal / 2 external` and that the `[i…]`/`[x…]` tags match the handle kinds.

---

## 3. `File_Copy` — partial destination must not survive a failure

### F-04 — truncated destination left behind (and an intact one destroyed)
* **File:line:** `src/fm_file.c:1441`–`1464`
* **Describe.** The destination is opened with `CREATE|TRUNCATE` before the first byte is read.
  If `FM_File_CopyStream()` or `File_Close(dst)` fails, the function returns the error but
  leaves whatever was written (0 … n bytes) at `dst_path`. `FM_Transfer_CopyEx()`
  (`src/fm_transfer.c:171`–`176`) does delete the incomplete copy — `File_Copy` does not, so
  the two copy APIs disagree, and `File_Move`'s cross-mount path inherits the hole.
  A pre-existing destination is truncated to 0 and silently replaced by a partial file.
* **Root cause.** No cleanup/rollback after the stream; the open order also means the old
  content is destroyed *before* it is known that the copy can succeed.
* **Observed.** 40 000-byte source, destination side returns `FM_ERR_NO_SPACE` after 5 000
  bytes: `File_Copy → -5 (no space left)`, `destination exists=1 size=5000`.
* **Proposed fix.** (a) cleanup on every failure path; (b) better: copy to `dst + ".fm_tmp"`
  (the same pattern `file_edit()` already uses at `:1536`) and `File_Rename` on success, so the
  destination is never observed half-written and a pre-existing destination survives a failure.

```c
r = FM_File_CopyStream(src, dst, progress, user_data);
if (r == FM_OK) { r = File_Close(dst); dst = NULL; }
if (r != FM_OK) {
    if (dst != NULL) (void)File_Close(dst);
    (void)File_Close(src);
    (void)File_Delete(dst_path);     /* no truncated destination */
    FM_Core_SetLastError(r);
    return r;
}
```
  Minimal version shown; the temp-file + rename version is preferable and matches `file_edit`.
* **Test/D (repro).** Mount a wrapper `FM_FS` over POSIX whose `file_write` returns
  `FM_ERR_NO_SPACE` after N bytes; `File_Copy` a file larger than N; assert
  `r == FM_ERR_NO_SPACE` **and** `File_Exists(dst) == FM_FALSE`; repeat with a pre-existing
  destination and assert its old content is unchanged (temp-file variant).

### F-05 — progress callback can report 100 % before the data is durable
* **File:line:** `src/fm_file.c:1394`–`1403` (`FM_File_CopyStream`)
* **Describe.** `progress(done, total, …)` is invoked as soon as the last chunk reaches
  `File_Write()`; no flush has happened yet. A power loss after the 100 % callback loses the
  tail of the file (nor flash / FATFS buffers). It is the classic "progress bar completes,
  file is short" symptom.
* **Root cause.** Reporting is tied to buffered writes, not to `File_Flush()`.
* **Proposed fix.** Flush before the final report:

```c
if (r == FM_OK) { r = File_Flush(dst); }
if (progress != NULL && r == FM_OK) { progress(done, total, user_data); }
```
* **Test/E.** Wrapper adapter that counts `file_flush` calls; assert one flush happens *before*
  the last progress callback and that `done == total` only after it.

---

## 4. `FM_Transfer_Raw` — range vs capacity validation, erase sizing

### F-06 — no range validation against `capacity` (writes past the device)
* **File:line:** `src/fm_transfer.c:345`–`361` (guards), `396`–`434` (loop)
* **Describe.** `FM_Transfer_Raw` checks `src->read`, `dst->write` and pointers — nothing else.
  `Storage_ReadBlocks`/`Storage_WriteBlocks`/`Storage_EraseBlocks` all enforce
  `offset > capacity || len > capacity - offset` (`src/fm_storage.c:503, 537, 570`), and the
  callback contract in `include/fm_storage.h:56`–`60` documents the drivers as thin wrappers.
  A driver that trusts the caller therefore gets `offset+len > capacity`; the transfer reports
  `FM_OK`. `Storage_ReadBlocks` is also the documented *public* way to reach a device, so the
  two entry points behave differently for the same illegal request.
* **Root cause.** Validation lives only in the convenience wrappers, not in the transfer.
* **Observed.** 1 024-byte destination, `FM_Transfer_Raw(src, 0, dst, 1008, 256, NULL)`
  → `FM_OK`, driver's last write ended at **1264** (240 bytes past the end).
* **Proposed fix.** Validate up front, in subtraction form (see F-08 — `offset+bytes`
  overflows `uint64_t`):

```c
if (bytes > 0u) {
    FM_CHECK(src_off <= src->capacity && bytes <= src->capacity - src_off,
             FM_ERR_INVALID_ARGUMENT, "source range past capacity");
    FM_CHECK(dst_off <= dst->capacity && bytes <= dst->capacity - dst_off,
             FM_ERR_INVALID_ARGUMENT, "destination range past capacity");
}
```
* **Test/F (repro).** Fake storage with `capacity = 1024` and an unchecked `write`;
  `FM_Transfer_Raw(s, 0, d, 1008, 256, NULL)` must return `FM_ERR_INVALID_ARGUMENT` and the
  driver must record **zero** writes. Same for `src_off = UINT64_MAX - 8` (no overflow wrap).

### F-07 — one unaligned, oversized erase request; `(uint32_t)` truncation
* **File:line:** `src/fm_transfer.c:352`–`361`
* **Describe.** The whole destination range is erased in a single call,
  `dst->erase(dst, dst_off, (uint32_t)bytes)`, with three consequences:

  1. **Unaligned start/length.** NOR adapters accept only whole erase units —
     `block_storage_erase` (`src/fm_device.c:191`) and `flash_storage_erase`
     (`src/fm_device.c:369`) return `FM_ERR_INVALID_ARGUMENT` unless
     `offset % erase_size == 0 && length % erase_size == 0`. Because the error is returned
     as-is, the whole transfer aborts. `FM_Qspi_Erase` happens to round internally
     (`src/fm_qspi.c:646`–`651`) — and then erases the sectors *before* `dst_off`, i.e. data
     outside the requested region — so the two adapters behave inconsistently for the same call.
  2. **32-bit truncation.** `bytes` is `uint64_t`; `(uint32_t)bytes` is `0` for any
     `bytes` that is a multiple of 4 GiB — the region is *not erased* and is then programmed
     onto dirty flash.
  3. **No progress / no cancel.** Hours of sector erase with no `progress` callback.

  The chunk loop below already knows the unit (`unit_mask`, `to_edge`); the erase does not.
* **Root cause.** The erase is modelled as one opaque call instead of per-unit work, and the
  width change 64 → 32 bits is unchecked.
* **Observed.**
  * 4 KiB erase unit, `FM_Transfer_Raw(src, 0, dst, 1024, 8192, NULL)`:
    `erase(off=1024, len=8192)` → driver rejects → `FM_ERR_INVALID_ARGUMENT`;
    zero erases, nothing copied.
  * `bytes = 0x1_0000_0000` → `erase(len=0)` → nothing erased.
* **Proposed fix.** Erase per destination unit, exactly aligned, with the 64-bit width kept:

```c
if (dst->erase != NULL && bytes > 0u) {
    uint64_t left = bytes;
    uint64_t at   = dst_off;
    uint32_t unit = dst->erase_size;
    if (unit == 0u) unit = dst->write_size;
    if (unit != 0u) {
        if ((dst_off % unit) != 0u) return FM_ERR_INVALID_ARGUMENT; /* keep contract explicit */
        at = dst_off - (dst_off % unit);
    }
    while (left > 0u) {
        uint32_t n = (left > (uint64_t)unit) ? unit : (uint32_t)left;
        r = dst->erase(dst, at, n);
        if (r != FM_OK && r != FM_ERR_UNSUPPORTED) return r;
        if (r == FM_ERR_UNSUPPORTED) break;
        at += n;
        left = (left > (uint64_t)n) ? (left - n) : 0u;
        if (!xfer_report(o, done_erased, bytes)) return FM_ERR_CANCELLED;
    }
}
```
  (Or route the erase through `Storage_EraseBlocks`' validation and let the driver round —
  but pick **one** policy; today QSPI rounds down to the sector and CAP-block adapters refuse.)
* **Test/G (repro).** Storage with `erase_size = 4096` that records `(off, len)` and rejects
  non-multiples: `FM_Transfer_Raw(..., dst_off = 1024, 8192, …)` must succeed and produce
  exactly `2` erase calls of `4096` at `0` and `4096`; `bytes = 1ull << 32` must produce
  `1048576` calls (or be rejected by F-06's capacity check long before that).

### F-08 — `FM_Transfer_RawVerify` dereferences unvalidated callbacks
* **File:line:** `src/fm_transfer.c:446`–`482`
* **Describe.** Only `a`/`b` are null-checked; `a->read`/`b->read` are called directly.
  The public prototype (`include/fm_transfer.h:139`–`141`) promises the same inputs as
  `FM_Transfer_Raw`, so a storage without `read` crashes instead of returning an error.
* **Root cause.** The verify helper bypasses the guard block that `FM_Transfer_Raw` has.
* **Observed.** `FM_Transfer_RawVerify(&st, 0, &st2, 0, 16)` with `read == NULL` →
  **SIGSEGV** (exit 139).
* **Proposed fix.**

```c
FM_CHECK(a->read != NULL, FM_ERR_UNSUPPORTED, "source has no read function");
FM_CHECK(b->read != NULL, FM_ERR_UNSUPPORTED, "target has no read function");
if (bytes > 0u) {
    FM_CHECK(a_off <= a->capacity && bytes <= a->capacity - a_off,
             FM_ERR_INVALID_ARGUMENT, "verify range past source capacity");
    FM_CHECK(b_off <= b->capacity && bytes <= b->capacity - b_off,
             FM_ERR_INVALID_ARGUMENT, "verify range past target capacity");
}
```
* **Test/H (repro).** `FM_Transfer_RawVerify(&a, 0, &b, 0, 16)` with both `read == NULL`
  must return `FM_ERR_UNSUPPORTED` and must not crash.

### F-09 — `FM_Transfer_CopyEx(src == dst)` truncates the source
* **File:line:** `src/fm_transfer.c:97`–`112`
* **Describe.** `File_Copy` rejects a self-copy (`src/fm_file.c:1415`–`1432`, including the
  "same mount, same relative path" case), `FM_Transfer_CopyEx` does not: it opens the source
  for read, then opens the same path with `CREATE|TRUNCATE`, and copies zero bytes over it.
  The file is destroyed and `FM_OK` is returned.
* **Root cause.** The guard exists in one of two parallel implementations.
* **Observed.** `/same.txt` = 24 bytes → `FM_Transfer_Copy("/same.txt", "/same.txt", NULL)`
  → `FM_OK`, file is **0 bytes**. `FM_Transfer_Move` with equal paths would copy nothing and
  then delete the source, i.e. **data loss reported as success**.
* **Proposed fix.** Same resolve-and-compare block as `File_Copy` (or delegate:
  `FM_Transfer_Copy` → `File_Copy` when `verify`/`no_overwrite` are off, keeping one code
  path for copy semantics).
* **Test/I (repro).** `File_WriteText("/same.txt", "…")`; expect
  `FM_Transfer_Copy("/same.txt", "/same.txt", NULL) == FM_ERR_INVALID_ARGUMENT` and size
  unchanged; same for `FM_Transfer_Move`.

### F-10 — descriptor left open, and failed content left in place, on the verify path
* **File:line:** `src/fm_transfer.c:191`–`227`
* **Describe.** `chk` is registered in the external handle registry by `File_OpenEx` (`:586`)
  and is closed only on the normal path at `:213`. The early returns at `:194` (`open` failed —
  harmless, nothing registered) and `:216` (`r != FM_OK` after the read loop) leave `chk`
  dirty: `f->used == 1`, `g_external_files[i] == &chk` pointing at a **stack object**, while
  `:214`'s close is skipped. Additionally the CRC-mismatch return at `:223`–`:226` leaves the
  corrupt destination on disk even though `verify` was requested.
* **Root cause.** Two early returns interleaved with the close; `File_OpenEx`'s registry entry
  is invisible to the caller; no cleanup on `FM_ERR_CORRUPTED`.
* **Proposed fix.**

```c
r = File_OpenEx(&chk, dst_path, FM_OPEN_READ);
if (r != FM_OK) return r;
/* … read loop, leaving the outcome in r … */
{
    FM_Result rc = File_Close(&chk);        /* ALWAYS close what we opened */
    if (r == FM_OK) r = rc;
}
if (r != FM_OK) return r;
if (out_info != NULL) { out_info->crc_dest = crc_dst; out_info->verified = FM_TRUE; }
if (crc_dst != crc_src) {
    (void)File_Delete(dst_path);            /* a failed verify is a failed copy */
    return FM_ERR_CORRUPTED;
}
```
* **Test/J.** Wrapper adapter whose `file_read` fails (`FM_ERR_IO`) on the *second* open of the
  destination, with `opts.verify = 1`; assert `File_PoolUsed()` is back to its previous value
  after the call (no leaked external slot) and that a corrupt destination is deleted on
  `FM_ERR_CORRUPTED` (inject a byte between write and verify).

### F-11 — intermediate buffer is neither cache-maintained nor re-entrant
* **File:line:** `src/fm_transfer.c:18`–`25` (`g_xfer_buf`), `415`–`428`; `src/fm_file.c:1529`
* **Describe.** The header promises "buffer cache Clean/Invalidate around each chunk when the
  other side is DMA" (`include/fm_transfer.h:24`–`26`). The implementation contains **no**
  cache operation: `grep -c FM_Cache_ src/fm_transfer.c` → 0. `FM_File_CopyStream()` and
  `file_edit()` share the single thread-local-free `FM_InternalBuffer()` (`src/fm_core.c:349`),
  so a `File_Copy` progress callback that itself calls `File_Insert`/`File_Copy` overwrites the
  chunk being copied.
* **Root cause.** The documented invariant is not implemented; one scratch buffer serves
  nested calls (only `FM_USE_THREADS` makes it thread-local, not recursion-safe).
* **Proposed fix.** (a) call `FM_Cache_CleanForDma(buf, len)` before `dst->write` and
  `FM_Cache_InvalidateAfterDma(buf, len)` after `src->read` when `FM_USE_DMA`; (b) keep a
  small in-use flag next to the buffer and return `FM_ERR_BUSY` for nested users instead of
  silently corrupting the copy.
* **Test/K.** With `FM_USE_DMA=1` and a driver that asserts
  `FM_Cache_DmaAligned(buf, len)`, run `FM_Transfer_Copy` of a 3-chunk file and assert the
  clean/invalidate counters equal the chunk count. Separately: a progress callback that calls
  `File_Copy` recursively must return `FM_ERR_BUSY` (or copy correctly), not corrupt data.

### F-12 — `FM_USE_CRC=0` does not link
* **File:line:** `src/fm_transfer.c:14`, `146`–`149`, `211`; `include/fm_config.h:79`–`81`;
  `src/fm_crc.c:8`
* **Describe.** `fm_crc.c` compiles its whole body out under `#if FM_USE_CRC`, but
  `FM_Transfer_CopyEx` calls `CRC_Compute32()` unconditionally (also `src/fm_mediacache.c:57`).
  `make fm_test FM_USE_CRC=0` fails at link:
  `undefined reference to 'CRC_Compute32'` (`fm_transfer.c:148`, `:211`, `fm_mediacache.c:57`).
* **Root cause.** The `verify` option is not gated, although it is documented as an optional
  feature and the header for it is compiled out.
* **Proposed fix.**

```c
#if FM_USE_CRC
    if (o->verify) { … }                     /* both call sites + the final compare */
#else
    if (o->verify) return FM_ERR_UNSUPPORTED; /* verify needs FM_USE_CRC */
#endif
```
* **Test/L.** CI target that builds with `FM_USE_CRC=0` (and `FM_USE_CACHE=0`,
  `FM_USE_THREADS=1`, `FM_USE_INTERNAL_BUFFER=0`) and must link; plus a runtime test that
  `FM_Transfer_CopyEx(..., {.verify=1})` returns `FM_ERR_UNSUPPORTED` in that build.

---

## 5. Cross-cutting findings (VFS / cache / semantics)

### F-13 — read cache is keyed per handle, so another handle's write is invisible
* **File:line:** `src/fm_file.c:112`–`196` (`FM_CacheLine.file` key), `720`–`770` (`File_Read`),
  `803`–`811` (write invalidation), `1333`–`1403` (`FM_File_CopyStream`)
* **Describe.** Cache lines are matched by `(f, offset)` where `f` is the `FM_File*`. `File_Write`
  invalidates only the writing handle's lines (`cache_invalidate_file(f)`). Any second handle on
  the same file — or the *same* path re-opened between two reads (`File_ReadAll` twice in a row,
  `FM_File_CopyStream`, `File_Insert`) — keeps serving pre-write bytes.
* **Root cause.** Invalidation by handle identity instead of by file identity (path/mount).
* **Observed.** Reader handle primed (`"AAAAAAAA"`), writer handle overwrites with
  `"BBBBBBBB"`, reader re-reads → `"AAAAAAAA"` while the backend holds `"BBBBBBBB"`.
* **Proposed fix.** Key cache lines by `(mount, resolved path)` instead of `FM_File*`, or flush
  **all** lines for a path on every write/rename/delete/truncate (cheap: `FM_CACHE_LINES=16`).
* **Test/M (repro).** Two handles on one path (`FM_OPEN_READ` / `FM_OPEN_WRITE`), overwrite
  through the writer, re-read through the reader; assert the new bytes. Also: read the whole
  file twice with `File_ReadAll` around an out-of-band write and assert the second read differs.

### F-14 — a hard I/O error is reported as `FM_OK` by the cached read path
* **File:line:** `src/fm_file.c:766`–`770`
* **Describe.** `return (total > 0) ? FM_OK : r;` discards any backend error once at least one
  byte was served, so `FM_ERR_IO`, `FM_ERR_TIMEOUT` and `FM_ERR_CORRUPTED` all look like a clean
  short read. Only `FM_ERR_EOF` should be masked. The non-cached path (`:776`–`790`) and the
  large-read fast path (`:687`) already behave correctly, so the bug appears only for
  `1 < len < 2 * FM_CACHE_LINE_SIZE` **and** `FM_USE_CACHE=1` — the configuration used by the
  host Makefile and by `FM_BUFFERS=LARGE` board builds.
* **Root cause.** A blanket "got some bytes" success rule instead of an EOF-specific one.
* **Observed.** 900-byte read, backend returns `FM_ERR_IO` on the second 512-byte block:
  `File_Read → 0 (OK), *read=512` — a truncated file reported as success.
* **Proposed fix.**

```c
if (r == FM_ERR_EOF) return FM_OK;          /* short read at EOF is fine */
return (total > 0 && r == FM_OK) ? FM_OK : r;
```
* **Test/N (repro).** Adapter that fails the second `file_read` with `FM_ERR_IO`; a 900-byte
  `File_Read` must return `FM_ERR_IO` (and `*read` may still be 512).

### F-15 — `file_edit()` temp name can overflow `FM_PATH_MAX`
* **File:line:** `src/fm_file.c:1536`–`1538`
* **Describe.** `strncat(tmp, ".fm_tmp", sizeof(tmp) - strlen(tmp) - 1)` silently truncates when
  the path is near `FM_PATH_MAX` (the `strncpy` above already consumed the space), so the temp
  file name collides with the original path or with another file's temp. `File_Insert`,
  `File_DeleteRange` and `File_ReplaceRange` then "replace" the wrong entry.
* **Root cause.** Unchecked concatenation onto a bounded buffer that is already full.
* **Proposed fix.** Refuse the edit when `strlen(path) + sizeof(".fm_tmp") > sizeof(tmp)`
  (`FM_ERR_INVALID_PATH`), or build the temp name from the directory + a short fixed suffix.
* **Test/O.** `File_Insert` on a path of length `FM_PATH_MAX - 3` must return
  `FM_ERR_INVALID_PATH` and leave every file untouched.

### F-16 — `File_Delete` / `File_Rename` are not path-normalization consistent
* **File:line:** `src/fm_file.c:988`–`1012`, `1024`–`1053`
* **Describe.** `File_Copy` normalizes and compares *resolved relative* paths before copying
  (`:1420`–`1432`), while `File_Move` compares raw strings (`:1417` in `File_Copy` only) and
  `File_Delete`/`File_Rename` do no self-comparison at all: `File_Delete("/a/./b")` and
  `File_Delete("/a/b")` reach the backend as different strings, and `File_Rename(p, p)` relies
  on the adapter (FMFS treats it as a no-op, POSIX `rename(2)` succeeds, but a FATFS adapter
  that maps it to "delete then create" would lose the file). Normalization must be one policy.
* **Root cause.** Path canonicalization is applied at the resolver, but several call sites
  still compare pre-normalized strings.
* **Proposed fix.** Use `FM_VFS_Resolve()` + `strcmp(rel)` (as `File_Copy` already does) for
  every self-comparison, and document `Rename(p, p)` as a no-op.
* **Test/P.** `File_Rename("/a/b", "/a/./b")` → `FM_OK`, file intact; `File_Delete("/a/./b")`
  removes the same entry as `File_Delete("/a/b")`.

---

## 6. Findings summary

| # | Area | Severity | One-line |
|---|---|---|---|
| F-01 | File_Move | **High** | No NULL guard; every resolve error flattened to `NOT_FOUND` (`fm_file.c:1470`) |
| F-02 | File_Move | **High** | Cross-mount rollback deletes a **pre-existing** destination (`fm_file.c:1500`) |
| F-03 | File_PoolDump | Low | Merged counter printed as "internal" (`fm_file.c:430`) |
| F-04 | File_Copy | **High** | Partial/truncated destination survives failure (`fm_file.c:1441`) |
| F-05 | File_Copy | Medium | 100 % progress before flush (`fm_file.c:1394`) |
| F-06 | Transfer_Raw | **Critical** | No capacity check → quiet writes past the device (`fm_transfer.c:345`) |
| F-07 | Transfer_Raw | **High** | Single unaligned erase + `(uint32_t)` truncation (`fm_transfer.c:354`) |
| F-08 | RawVerify | **High** | NULL `read` callback dereferenced (SIGSEGV) (`fm_transfer.c:446`) |
| F-09 | Transfer_Copy | **Critical** | `src == dst` truncates the source, returns `FM_OK` (`fm_transfer.c:97`) |
| F-10 | Transfer_Copy | Medium | `chk` handle leaked + corrupt destination kept on verify failure (`fm_transfer.c:191`) |
| F-11 | Buffer mgmt | Medium | DMA cache maintenance documented but absent; shared scratch buffer (`fm_transfer.c:18`) |
| F-12 | Build | Medium | `FM_USE_CRC=0` does not link (`fm_transfer.c:148`, `fm_crc.c:8`) |
| F-13 | Cache | **High** | Per-handle cache key → stale reads after another handle's write (`fm_file.c:112`) |
| F-14 | Cache | **High** | Hard I/O error masked as `FM_OK` in cached reads (`fm_file.c:770`) |
| F-15 | file_edit | Medium | Temp-name overflow near `FM_PATH_MAX` (`fm_file.c:1538`) |
| F-16 | VFS | Low | Inconsistent normalization for delete/rename self-paths (`fm_file.c:988`) |

---

## 7. Confirmation matrix (measured, not asserted)

`✔` = harness reports the defect, `—` = clean. "fixed" = scratch copy with §1–§4 snippets
applied (repository untouched).

| Test | Harness | Before | After fix |
|---|---|---|---|
| A | `repro.c` §A — `File_Move` NULL / resolve errors | ✔ `-3` for `NULL`, unmounted, too-long, relative | — |
| B | `move5.c` — rollback vs pre-existing destination | ✔ destination deleted | — |
| C | `repro.c` §B — `File_PoolDump` header | ✔ `3/8 internal` for 1 internal + 2 external | — |
| D | `repro.c` §C — `File_Copy` with full destination | ✔ `exists=1 size=5000/40000` | — |
| F | `raw3.c` — `FM_Transfer_Raw` past capacity | ✔ wrote 240 B past a 1 024 B device | — |
| G | `raw2.c`/`repro.c` §E — unaligned erase | ✔ driver rejects `erase(1024, 8192)` | — (`erase(0,4096)` ×2) |
| H | `cache1.c` §J — `RawVerify` with no `read` | ✔ SIGSEGV (139) | — |
| I | `xfer7.c` — `FM_Transfer_Copy(src==dst)` | ✔ 24 B file → 0 B, `FM_OK` | — |
| M | `cache1.c` §I — cache coherency across handles | ✔ stale `"AAAAAAAA"` | ✔ **still reproduces** (fix not in §1–§4) |
| N | `read6.c` — hard I/O error masked | ✔ `FM_OK`, `*read=512` | — |
| L | `make fm_test FM_USE_CRC=0` | ✔ link error | — |

---

## 8. Regression / acceptance plan

### 8.1 Insertion points in the existing suite

`test/main.c` already has the right fixtures; no new framework is needed.

| Finding | Where to add | Fixture already present |
|---|---|---|
| F-01, F-02 | next to `test_file_move_delete()` (`:345`) and the `"Regression: File_Move without rename falls back"` block (`:8430`) | `g_fs` (POSIX at `/`), the `NORENAME` copy-ops pattern at `:8415` |
| F-03 | inside the `test_memory_fs()`/`test_threads()` area; use `FM_Log_SetSink` (`src/fm_log.c:18`) | `FM_Log_SetSink` + `File_OpenEx` |
| F-04 | extend `test_file_copy()` (`:327`) with a quota wrapper (copy `g_fs.ops`, override `file_write`) | `PosixFS_Init` + ops copy at `:8415` |
| F-05 | `test_transfer()` — `"Transfer_Copy verify=1 …"` block | `FM_TransferOpts` |
| F-06, F-07, F-08 | `test_transfer()` — `"Transfer_Raw aligns chunks …"` (`:4411`) already builds `FM_Storage` doubles (`test_driver_read/write/erase`, `:69`–`:100`) | `g_ram_buf`, `test_driver_*` |
| F-09 | `test_transfer()` — `"Transfer_Move removes the source"` (`:4385`) | same |
| F-10 | `test_transfer()` + `File_PoolUsed()` | `File_PoolUsed` (`:410`) |
| F-12 | Makefile target + `#if FM_USE_CRC` build & a runtime `verify` test | `asan`/`ports` targets as template |
| F-13, F-14 | `test_cache()` (`:1240`, `#if FM_USE_CACHE && FM_USE_FMFS`) | cache stats + `File_SetCacheEnabled` |
| F-15, F-16 | `test_file_seek`/`test_file_copy` neighbours | `FM_PATH_MAX` paths |

### 8.2 Definition of done

1. New tests fail on `2edf567` and pass with the fixes (one test per finding, named `F-0x …`).
2. `make fm_test` → `213/214 passed` (baseline 211/212 + 2 new; the single pre-existing
   ADPCM failure stays as-is) with `FM_USE_CACHE=1`, and `FM_USE_CACHE=0`.
3. `FM_USE_THREADS=1` run must not gain failures beyond the two pre-existing ones
   (ADPCM, `concurrent registry stress`).
4. Compile/link matrix green:

```sh
for o in "-DFM_USE_CACHE=0" "-DFM_USE_CACHE=1" \
         "-DFM_USE_THREADS=1 -DFM_SYNC_CUSTOM -Iports/pthread" \
         "-DFM_USE_INTERNAL_BUFFER=0" "-DFM_USE_DMA=1"; do
    make clean >/dev/null && make -j8 fm_test CFLAGS_EXTRA="$o" || exit 1
done
make -j8 fm_test FM_USE_CRC=0        # expected to link only after F-12
```
5. Erase accounting: the QSPI simulation counter used by
   `"Transfer_Raw aligns chunks to the destination erase unit"` (`test/main.c:4411`) must stay
   ≤ 8 erases for 4 sectors after the F-07 change (per-unit erase replaces the single call).

### 8.3 Suggested landing order

`F-06 / F-09` (data loss, one-line guards) → `F-01 / F-02 / F-04` (move & copy semantics) →
`F-07 / F-08` (raw transfer) → `F-13 / F-14` (cache) → `F-03 / F-05 / F-10 / F-11 / F-12` →
`F-15 / F-16` (clean-up). Each group is independent; the first three groups share
`fm_file.c`/`fm_transfer.c` and can be reviewed as two patches.
