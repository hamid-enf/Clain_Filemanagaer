# AUDIT_FILE_V3 — files, storage and filesystems (follow-up round)

Persian summary (خلاصهٔ فارسی)

در این دور **شش نقص تأییدشده**: سه **S1**، یک **S2**، دو **S3** — و هر شش مورد وصلهٔ آزموده‌شده دارد.
مهم‌ترین یافته FILE3-06 است: هر ویرایش خطی (`Text_Replace` / `Text_InsertLine` / `Text_DeleteLine`)
بی‌صدا **همهٔ** پایان‌خط‌های CRLF فایل را به LF تبدیل می‌کند (۲۶ بایت → ۲۳ بایت، با کد `FM_OK`، بدون هیچ پیامی؛
در آزمون تصادفی ۳۳۵ از ۴۰۰ فایل خراب شد). دومین یافتهٔ سنگین FILE3-01 است: `File_Delete` روی پوشهٔ خالی
در LittleFS و POSIX با `FM_OK` پوشه را حذف می‌کند (FMFS درست بود و همین تفاوت آن را لو داد).
سوم FILE3-05: `FS_Mount` روی یک `FM_FS` مقداردهی‌نشده `FM_OK` می‌دهد و اولین `File_Open` از اشاره‌گر NULL می‌پرد.
سه مورد دیگر: کد خطای اشتباه در rename روی پوشه در FMFS (FILE3-02)، شرایط رقابتی `FM_Log_SetSink` که پیام را
به ctx اشتباه می‌دهد (FILE3-03)، و اینکه یک خط بلند، `Text_CountLines` را «۰ خط» برمی‌گرداند و محدودیت
۵۱۱ کاراکتری ویرایش‌ها مستند نشده بود (FILE3-04). از هفت یافتهٔ دور قبل **هر هفت** واقعاً برطرف شده‌اند، ولی نیمهٔ
نوشتنِ همان مسیر متن که FILE-04 اصلاح کرده بود شکسته مانده بود — همان FILE3-06.
وصله‌ها با هم روی یک درخت کپی: ۴۷۸/۴۷۸ آزمون، ۴۸۳/۴۸۳ چندریسمانی با TSan تمیز، ۴۷۹/۴۷۹ در حالت ۳۲-بیتی،
`make fatfs` و `make ports` بدون خطا. امتیازها: **A=8، B=5، C=6، D=7، E=5، F=5، G=7**.
بررسی‌نشده: درایورهای واقعی (QSPI/SPI-flash/SD/USB)، FatFS با کارت واقعی، پشتهٔ پیمایش بازگشتی روی هدف.
**آمادهٔ محصول نیست**؛ آمادهٔ محصول تحت شش شرط بخش ۱۴.

---

## 1. Audit record

| | |
|---|---|
| Repository | `github.com/hamid-enf/Clain_Filemanagaer`, `Clain_FileManager/FileManager_lib` |
| Commit audited | `910a60543838d8c844ffea790588ee4dc3d933c2` (2026-11-11 13:02:30 +0000) |
| Requested commit | `2a45d9d` — **not present in this clone** (`git log` has `910a605` and `de9abca`). Everything below is against `910a605`. |
| Machine | 2 × Intel Xeon @ 2.60 GHz, 2 vCPU, x86-64, Debian 12 (container) |
| Compilers | gcc (Debian 12.2.0-14+deb12u1) 12.2.0; `zig cc 0.17.1` (`x86-linux-musl`) for the 32-bit run |
| Sanitizers | ASan + UBSan (`build_lib.sh`), ThreadSanitizer (`make tsan`, pthread port), UBSan under 32-bit zig |
| Library builds | `make test` (478 tests on the unmodified library, **500** with the six fixes and the 22 new regression tests), `make asan` (500), `make tsan` (506), `make ports`, `make fatfs`, `sh tools/check_32bit.sh ubsan` (479 / **501**), `make m32` (501), `make matrix` (49 configurations, 0 failed) |
| Third-party | littlefs, FatFS (mock on the host), h264bsd, minimp3, picojpeg, stb_vorbis — not audited line by line, only through this library's API |

**Time spent.** Four working sessions, roughly 26 hours of wall-clock work in total: about
11 h reading `fm_file.c`, `fm_filesystem.c`, `fm_directory.c`, `fm_path.c`, `fm_storage.c`,
`fm_fmfs.c` and the four adapters; 9 h writing and running the 43 harnesses in
`audit_v3/harness/` (6 310 lines of C); 4 h on the six fixes and re-verification; 2 h on the
report.

**Methods.**

* Reading with a target in mind: every `FM_Result` return path of the Tier-1 files. The seven
  earlier findings came first (Job 1); after that I went through the areas the earlier round
  did not reach, and then through the *other half* of every path the earlier round had
  already touched — that is how FILE3-06 was found under FILE-04 and FILE3-05 under the
  registry work.
* Purpose-built harnesses against the *unmodified* library, each printing observed vs.
  expected, all built with ASan+UBSan. Reproducers for every confirmed finding are in
  `audit_v3/harness/` and named in its section.
* Differential testing against a reference: `diff_fs` runs the same random operation script
  against FMFS, LittleFS and POSIX and compares the resulting trees and every result code
  (seeds 1–60 × 150 ops, seeds 100–160 × 400 ops, 0 divergences); `edit_model` compares the
  three rewrite helpers with an in-memory model (300 ops × 12 seeds × 2 backends);
  `wildcard_ref` compares the two wildcard matchers on 3 828 pattern/name pairs;
  `cksum_model` / `crc_bin` compare checksums with a reference implementation.
* Fuzzing with structure, not coverage-guided: `json_fuzz` 100 000 random JSON documents
  (~260 k inputs over the sessions, 20 599 accepted and fully walked), `text_fuzz`,
  `mixed_end_fuzz` (400 random line-ending/layout cases, see FILE3-06), `fs_zero` and
  `matrix_fs` (zeroed/garbage images and a full operation matrix per backend). No libFuzzer
  coverage number is claimed — this part of the library is I/O-bound, and a harness that
  drives a filesystem through its public API gets more per hour than a byte fuzzer on
  `fm_fmfs.c`.
* Power-fail and I/O-error simulation: `nor_block`, `nor_eeprom`, `async_model` with a
  write-counting block model that checks program-before-erase and range rules
  (626 reads / 1 256 programs / 72 erases, 0 violations).
* Resource accounting: `handle_leak`, `fs_slots`, `cache_stale`, `walk`.
* Verification of every fix: applied to a *copy* of the tree (`audit_v3/edits/all.py`), then
  the whole suite, TSan, 32-bit UBSan, `make fatfs`, `make ports`, and every harness re-run
  against the patched library.

**32-bit.** `sh tools/check_32bit.sh ubsan` builds the whole library and the whole test
program with `zig cc -target x86-linux-musl` (ILP32: 4-byte `size_t`, `long`, pointers) and
runs it: **479/479 passed, 0 failures**, no UBSan trap. `make m32` (the same, plain, no UBSan)
also gives 479/479. This is the only 32-bit coverage; no 32-bit build of the *harnesses* was
made, because they allocate host-sized buffers on purpose.

**Configuration matrix.** `make matrix` builds and runs the suite in 49 `FM_USE_*`
configurations (subsystems on and off, `FM_FMFS_POWER_SAFE` 0/1/2, unlimited move steps,
`FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX=0`, the three sync ports, `-Werror`): **49 configurations,
0 failed** with all six fixes applied. The suite also checks that a disabled subsystem leaves
no static memory behind, so a fix that pulled a module in unconditionally would show there.

**What "confirmed" means here.** Every confirmed finding was reproduced against the
unmodified library, then the proposed change was applied to a copy and the reproducer
re-run, then the full suite, TSan and the 32-bit run were repeated. Findings are counted
once each, even where the same root cause shows on two adapters.

**Regression tests.** Each of the six findings now has a test in the library's own suite
(`audit_v3/edits/f3_tests.py`, applied as `audit_v3/patches/tests_regression.patch`): 22
tests, three backends where the backend matters, +1 test in the threaded build. They were
run against the unmodified library to confirm they fail there — 19 of 19 applicable tests
fail (`481/500`), and the threaded one fails 5 runs out of 5 — and against the fixed tree to
confirm they pass (500/500, `506/506` threaded, 0 TSan warnings, 501/501 in 32-bit). A test
that passes on the broken code would be worth nothing, so this was checked rather than
assumed.

---

## 2. Findings

| ID | Sev | Title | Where (audited commit) | Status |
|---|---|---|---|---|
| FILE3-01 | **S1** | `File_Delete()` deletes an empty *directory* and returns `FM_OK` (LittleFS, POSIX); `Directory_Delete()` deletes a *file* (LittleFS) | `backends/fm_littlefs.c:776,838`, `backends/fm_posix_fs.c:258,318` | confirmed |
| FILE3-02 | S3 | `File_Move(file, existing dir)` on FMFS returns `not a directory` where the two other backends say `is a directory` | `src/fm_fmfs.c:2773` | confirmed |
| FILE3-03 | S3 | `FM_Log_SetSink()` races with `FM_Log*()`: a message can be delivered with a mismatched (sink, ctx) pair | `src/fm_log.c:35-54, 125-135` | confirmed |
| FILE3-04 | S2 | `Text_CountLines()` / `Text_Find()` give up on a file with one long line (127 / 511 characters) and report 0 lines; the per-line limit is undocumented | `src/fm_text.c:188, 224` | confirmed |
| FILE3-05 | **S1** | `FS_Mount()` accepts an `FM_FS` that was never initialised, reports `FM_OK`, and the first `File_Open()` calls a NULL function pointer | `src/fm_filesystem.c:413-456` | confirmed |
| FILE3-06 | **S1** | Every `Text_Replace` / `Text_InsertLine` / `Text_DeleteLine` rewrites *every* CRLF line end in the file as LF (and adds a final newline) | `src/fm_text.c:251-540` | confirmed |

Severity key (from the brief): **S1** memory corruption, crash, hang, loss or corruption of
stored data · **S2** wrong result returned as success · **S3** wrong error handling: leak,
wrong error code, state not restored · **S4** documentation/portability/waste.

---

## 3. FILE3-01 — `File_Delete()` deletes an empty directory (LittleFS, POSIX)

**Severity:** S1 — silent loss of stored data, reported as success.

**Where:** `backends/fm_littlefs.c:776` `lfs_fs_delete()` and `:838` `lfs_fs_rmdir()`;
`backends/fm_posix_fs.c:258` `posix_delete()` and `:318` `posix_rmdir()`.

**Contract.** `include/fm_filesystem.h:97` declares the adapter table:

```c
FM_FS_DeleteFn   delete_file;   /* files only */
FM_FS_RmdirFn    rmdir;         /* empty dirs only */
```

and the public header documents the same split for `File_Delete()` vs
`Directory_Delete()`.

**How to reproduce.** `audit_v3/harness/del_kind.c` (mounts FMFS on a RamDisk, LittleFS on a
RamDisk and POSIX at the root of `/tmp/fm_delkind_root`, creates one directory and one file
in each, then calls the wrong API on each):

```sh
sh audit_v3/build_harness.sh audit_v3/harness/del_kind.c /tmp/del_kind
/tmp/del_kind
```

Observed on the unmodified library:

```
FMFS     File_Delete(dir)  -> is a directory     dir survives: yes
FMFS     Directory_Delete(file) -> not a directory file survives: yes
LittleFS File_Delete(dir)  -> OK                 dir survives: NO (!)
LittleFS Directory_Delete(file) -> OK              file survives: NO (!)
POSIX    File_Delete(dir)  -> OK                 dir survives: NO (!)
POSIX    Directory_Delete(file) -> not a directory file survives: yes
```

Expected: `is a directory` / `not a directory`, nothing removed — which is exactly what FMFS
already does. Also reproducible through the higher-level API alone:
`Directory_Create("/DA/empty")` then `File_Delete("/DA/empty")` returns `FM_OK` and
`Directory_Exists()` turns false (`harness/delete_dir.c`).

**Root cause.** Both adapters are one-line wrappers over an API that removes either kind of
entry: littlefs `lfs_remove()` unlinks a file and removes an empty directory; POSIX
`remove(2)` does the same. The LittleFS adapter's comment even said so —
"littlefs lfs_remove removes empty dirs and files alike" — and then routed `rmdir` *through*
`delete_file`, so the two entry points were the same function. Nothing upstream checks the
entry type before dispatch: `File_Delete()` in `src/fm_file.c:1500` only rejects a NULL op.
The FMFS adapter gets this right because its table is type-typed.

**Proposed fix** (tested, `audit_v3/edits/f3_01.py`, also as
`audit_v3/patches/file3_01_lfs_delete_dir.patch`):

```diff
--- a/backends/fm_littlefs.c
+++ b/backends/fm_littlefs.c
@@ -783,6 +783,24 @@ static FM_Result lfs_fs_delete(FM_FS *fs, const char *path)
         return FM_ERR_INVALID_ARGUMENT;
     }
     lfs_path(path, lpath, sizeof(lpath));
+    /* lfs_remove() removes an empty directory as happily as a file, so
+     * "delete file" used to delete a directory -- silently, and with
+     * FM_OK.  The adapter contract is files only (fm_filesystem.h:
+     * "delete_file: files only"); a directory is refused here, and the
+     * directory API (lfs_fs_rmdir) does the rest. */
+    {
+        struct lfs_info info;
+        int stat_err = lfs_stat(&ctx->lfs, lpath, &info);
+
+        if (stat_err != LFS_ERR_OK)
+        {
+            return lfs_map_err(stat_err);
+        }
+        if (info.type == LFS_TYPE_DIR)
+        {
+            return FM_ERR_IS_DIRECTORY;
+        }
+    }
     return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
 }
@@ -837,8 +855,27 @@
 static FM_Result lfs_fs_rmdir(FM_FS *fs, const char *path)
 {
-    /* littlefs lfs_remove removes empty dirs and files alike. */
-    return lfs_fs_delete(fs, path);
+    LittleFS_Ctx *ctx = lfs_ctx(fs);
+    char lpath[FM_PATH_MAX];
+    struct lfs_info info;
+    int err;
+
+    if (path == NULL)
+    {
+        return FM_ERR_INVALID_ARGUMENT;
+    }
+    lfs_path(path, lpath, sizeof(lpath));
+    /* the mirror image of the check above: a file is not a directory */
+    err = lfs_stat(&ctx->lfs, lpath, &info);
+    if (err != LFS_ERR_OK)
+    {
+        return lfs_map_err(err);
+    }
+    if (info.type != LFS_TYPE_DIR)
+    {
+        return FM_ERR_NOT_DIRECTORY;
+    }
+    return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
 }
--- a/backends/fm_posix_fs.c
+++ b/backends/fm_posix_fs.c
@@ -257,7 +257,15 @@
 static FM_Result posix_delete(FM_FS *fs, const char *path)
 {
+    struct stat st;
+
     path = strip_slash(path);
+    /* remove(2) unlinks a file and removes an empty directory alike, so
+     * File_Delete("/dir") used to succeed and take the directory with
+     * it.  The adapter contract is files only; a directory is refused
+     * here and posix_rmdir() handles it. */
+    if (stat(path, &st) < 0) { return sys2fm(errno); }
+    if (S_ISDIR(st.st_mode)) { return FM_ERR_IS_DIRECTORY; }
     if (remove(path) < 0) { return sys2fm(errno); }
     (void)fs;
     return FM_OK;
@@ -317,7 +325,14 @@
 static FM_Result posix_rmdir(FM_FS *fs, const char *path)
 {
+    struct stat st;
+
     path = strip_slash(path);
+    /* rmdir(2) already refuses a file with ENOTDIR, but the error is
+     * mapped after the fact; check first so the answer does not depend
+     * on the platform's errno mapping. */
+    if (stat(path, &st) < 0) { return sys2fm(errno); }
+    if (!S_ISDIR(st.st_mode)) { return FM_ERR_NOT_DIRECTORY; }
     if (rmdir(path) < 0) { return sys2fm(errno); }
     (void)fs;
     return FM_OK;
```

**Tested?** Yes. With the change applied, `del_kind` prints `is a directory` / `not a directory`
and "survives: yes" for all three backends; `delete_dir`, `delete_nonempty`, `diff_err` and
`regress_v2` unchanged; the full suite 478/478, TSan 483/483, 32-bit 479/479.
**Cost:** one `stat` per delete (a directory lookup on littlefs, a `stat(2)` on POSIX); no
allocation, no stack growth.

**Affected configurations:** any build with `FM_USE_LITTLEFS=1` or `FM_USE_POSIX=1`. FatFS
and FMFS were already correct. The LittleFS `rmdir`→file half is a `S3` (wrong error code, data
lost only if the caller meant to make a directory); it is fixed in the same diff because it is
the same one-line mistake.

---

## 4. FILE3-02 — FMFS answers `not a directory` for a rename onto an existing directory

**Severity:** S3 — wrong error code; nothing is moved, but the caller is told the wrong thing
about which object is the problem.

**Where:** `src/fm_fmfs.c:2773` in `fmfs_rename()` (function starts at `:2720`).

**How to reproduce.** `audit_v3/harness/move_err.c`: a file `/f` and a directory `/d` on
each of the three backends, then `File_Move("/MF/f", "/MF/d", NULL, NULL)`:

```
FMFS       File_Move(file, existing dir) -> not a directory   source=1 moved-inside=0 dst-exists=1
LittleFS   File_Move(file, existing dir) -> is a directory    source=1 moved-inside=0 dst-exists=1
POSIX      File_Move(file, existing dir) -> is a directory    source=1 moved-inside=0 dst-exists=1
```

**Expected:** `is a directory` on all three, with nothing moved — the destination is a
directory and the source is a file, exactly the case POSIX `rename(2)` refuses with `EISDIR`.
(FMFS already returns `FM_ERR_NOT_EMPTY` when the destination directory is not empty, and the
other two adapters reject that case too.)

**Root cause.** In the rename-over branch, the code checks the *destination* entry's type and
then the *source*'s, but reuses `FM_ERR_NOT_DIRECTORY` (which is the answer for "the source is
a directory and the destination is a file") for both directions: the branch at `:2765` (dest is
a directory) falls into `:2771` → `:2773` and returns `NOT_DIRECTORY` although the thing that
is not a directory is the source. The two branches were written symmetrically and the error
constant was not swapped.

**Proposed fix** (tested, `audit_v3/edits/f3_02.py`):

```diff
--- a/src/fm_fmfs.c
+++ b/src/fm_fmfs.c
@@ -2770,7 +2770,10 @@ static FM_Result fmfs_rename(FM_FS *fs, const char *old_path,
             }
             if (ctx->entries[old_index].type != FM_ITEM_DIR)
             {
-                return FM_ERR_NOT_DIRECTORY;
+                /* the DESTINATION is a directory and the source is not:
+                 * that is "is a directory", not "not a directory" (the
+                 * two other backends say FM_ERR_IS_DIRECTORY here) */
+                return FM_ERR_IS_DIRECTORY;
             }
         }
         else
```

**Tested?** Yes: with the fix all three backends print `is a directory`, source present,
destination unchanged, nothing moved inside; `diff_err`, `diff_fs` (seeds 1–60) and
`lfs_swap` unchanged; suite 478/478.

**Affected configurations:** `FM_USE_FMFS=1`. `File_Rename` and `File_Move` on one mount both
go through here; across mounts the copy path reports the platform error instead.

---

## 5. FILE3-03 — `FM_Log_SetSink()` races with the logging calls

**Severity:** S3 — a message is handed to the wrong sink context (a file descriptor belonging
to another subsystem, a UART handle, a user `void *`), i.e. wrong data written somewhere and a
resource used by the wrong owner. Also a data race in the C11 sense, so it is undefined
behaviour, not just a wrong pair.

**Where:** `src/fm_log.c:35-54` (`FM_Log_SetSink`, `FM_Log_GetSink`, `FM_Log_IsActive`) and
`:125-135` (`log_deliver`). Two separate globals, `g_sink` (`:18`) and `g_sink_ctx` (`:19`).

**How to reproduce.** `audit_v3/harness/log_threads.c` with the TSan build:

```sh
sh audit_v3/build_log_threads.sh /tmp/log_threads audit_v3/harness/log_threads.c
TSAN_OPTIONS=halt_on_error=0 /tmp/log_threads
```

One thread calls `FM_Log_SetSink()` alternately with two sinks that each stamp their own id
into the message, one thread logs 160 000 messages. Observed on the unmodified library:

```
WARNING: ThreadSanitizer: data race
  Write of size 8 at 0x... by thread T1:
    #0 FM_Log_SetSink src/fm_log.c:39
  Previous read of size 8 at 0x... by thread T2:
    #0 log_deliver src/fm_log.c:93
messages=160000 with a mismatched (sink, ctx) pair=14075
```

i.e. ~8.8 % of the messages were delivered to a sink with the *other* sink's context.

**Expected:** every message reaches one complete (function, context) pair — the old one or the
new one, never a mix — and TSan is silent.

**Root cause.** The pair is two independent globals written one after the other. The existing
code orders the two stores (`ctx` first, then the function) with a comment explaining that this
covers an interrupt landing between them — and that is all it covers. Between two *tasks*
there is no ordering at all: the reader in `log_deliver()` reads `g_sink` into a local and then
reads `g_sink_ctx` separately, so a `SetSink()` that runs between those two reads produces
exactly the mixed pair above. `FM_Log_IsActive()` reads `g_sink` unsynchronised as well.

**Proposed fix** (tested, `audit_v3/edits/f3_03.py`): read and write the pair as one value
under a two-store spin lock when `FM_USE_THREADS=1`; keep the existing store order (and no
lock, no code) when threads are off.

```diff
--- a/src/fm_log.c
+++ b/src/fm_log.c
@@ -32,26 +32,108 @@
+/*
+ * The sink and its context are one value, not two globals.
+ *
+ * Ordering the two stores (ctx first, then the function) fixes the case
+ * where an interrupt lands between them, and nothing else.  Two tasks --
+ * one calling FM_Log_SetSink(), one logging -- still race: the reader can
+ * take the new function with the old context and hand a message to the
+ * wrong owner (ThreadSanitizer: data race on g_sink / g_sink_ctx at
+ * fm_log.c:39 and :40; measured 6-12 % of 160 000 messages delivered with
+ * a mixed pair when a console and a file sink are swapped every 1 ms).
+ *
+ * The fix is a short spin lock around the two-word copy: it is held for
+ * two stores and never across the sink call, so a sink that logs again
+ * (or calls FM_Log_SetSink) cannot deadlock, and a message from an ISR
+ * waits a few cycles at most.  A lock-free pair slot was tried first and
+ * rejected: with two slots and an atomic index a reader and a writer
+ * still touch the same slot memory, which ThreadSanitizer reports (the
+ * messages were no longer mixed up, the slot memory was still raced).
+ *
+ * Without threads there is a single task, and the store order alone is
+ * enough: ctx first, then the function.
+ */
+#if FM_USE_THREADS
+static unsigned char g_pair_lock;
+
+static void log_lock(void)
+{
+    while (__atomic_test_and_set(&g_pair_lock, __ATOMIC_ACQUIRE))
+    {
+        /* spin: the critical section is two stores long */
+    }
+}
+
+static void log_unlock(void)
+{
+    __atomic_clear(&g_pair_lock, __ATOMIC_RELEASE);
+}
+
+static void log_sink_pair(FM_LogSinkFn *sink_out, void **ctx_out)
+{
+    log_lock();
+    *sink_out = g_sink;
+    if (ctx_out != NULL)
+    {
+        *ctx_out = g_sink_ctx;
+    }
+    log_unlock();
+}
+#else
+static void log_sink_pair(FM_LogSinkFn *sink_out, void **ctx_out)
+{
+    *sink_out = g_sink;
+    if (ctx_out != NULL)
+    {
+        *ctx_out = g_sink_ctx;
+    }
+}
+#endif
+
 void FM_Log_SetSink(FM_LogSinkFn sink, void *ctx)
 {
+#if FM_USE_THREADS
+    log_lock();
+    g_sink = sink;
+    g_sink_ctx = ctx;
+    log_unlock();
+#else
     /* ترتیب مهم است: اول ctx، بعد تابع. ... */
     g_sink_ctx = ctx;
     g_sink = sink;
+#endif
 }
 
 FM_LogSinkFn FM_Log_GetSink(void *ctx_out)
 {
+    FM_LogSinkFn sink;
+    void        *ctx = NULL;
+
+    log_sink_pair(&sink, &ctx);
     if (ctx_out != NULL)
     {
-        *(void **)ctx_out = g_sink_ctx;
+        *(void **)ctx_out = ctx;
     }
-    return g_sink;
+    return sink;
 }
 
 int FM_Log_IsActive(void)
 {
-    return (g_sink != NULL) ? 1 : 0;
+    FM_LogSinkFn sink;
+
+    log_sink_pair(&sink, NULL);
+    return (sink != NULL) ? 1 : 0;
 }
@@ -124,13 +206,18 @@
 static int log_deliver(const char *buf, size_t len)
 {
-    FM_LogSinkFn sink = g_sink;      /* read once: it may be replaced */
+    FM_LogSinkFn sink;
+    void        *ctx = NULL;
 
+    log_sink_pair(&sink, &ctx);
     if (sink == NULL || len == 0u)
     {
         return 0;
     }
-    sink(g_sink_ctx, buf, len);
+    sink(ctx, buf, len);
     return (int)len;
 }
@@ -138,7 +225,7 @@
-    if (buf == NULL || len == 0u || g_sink == NULL)
+    if (buf == NULL || len == 0u || !FM_Log_IsActive())
@@ -171,7 +258,7 @@
-    if (g_sink == NULL || fmt == NULL)
+    if (fmt == NULL || !FM_Log_IsActive())
```

**Tested?** Yes: TSan clean (`ThreadSanitizer warnings: 0`) and 0/160 000 mismatched pairs on
the patched build; `make tsan` 483/483; `log_guard` (an interrupt-handler model that logs under
load) still reports 0 messages with mixed content, 0 dropped outside the storm, and the guard
released afterwards. The first attempt at this fix (two slots + atomic index) removed the mixed
pairs but not the race — it is not the patch above and is documented in the code comment so it
is not tried again.

**Affected configurations:** `FM_USE_THREADS=1` builds only (with ports that really run two
tasks). With `FM_USE_THREADS=0` the change compiles to the original two stores.

---

## 6. FILE3-04 — a long line stops `Text_CountLines()` / `Text_Find()`; the limit is undocumented

**Severity:** S2 — a wrong result returned as success (`Text_CountLines` gives 0 lines for a
file that has lines) plus an S4 documentation gap: the rewrite helpers refuse lines over 511
characters, and no header says so.

**Where:** `src/fm_text.c:188` (`Text_CountLines`, `char line[FM_NAME_MAX * 2]` = 128 bytes)
and `:224` (`Text_Find`, 512 bytes); the same 512-byte buffer is used by `Text_Replace`
(`:255`), `Text_InsertLine` (`:373`) and `Text_DeleteLine` (`:476`). Documentation:
`include/fm_text.h:36-43`, `README.md` §"File rules worth knowing".

**How to reproduce (a).** `audit_v3/harness/disklines.c` — a file of three lines whose first
line is 300 characters:

```
Text_CountLines(3 lines, first 300 chars) -> too large for buffer count=0 (want OK, 3)
Text_Find("third")                        -> too large for buffer, line 0 (want OK, 3)
```

`harness/limit_scan.c` measures the exact thresholds: 127 characters for `Text_CountLines`
(128-byte buffer), 511 for `Text_Find`. `harness/text_count.c` shows the 130/131/132-character
cases all returning `count=0`.

**How to reproduce (b), the rewrite limit.** `audit_v3/harness/long_rewrite.c` — a 1500-character
line followed by `name=old`:

```
  written: 1500 x 'A' + name=old     OK   1510 bytes
  Text_Replace(old -> new) -> too large for buffer
  Text_Replace on a too-long line only -> too large for buffer
  Text_InsertLine(1) -> too large for buffer
  Text_DeleteLine(1) on the only (long) line -> too large for buffer
  Text_DeleteLine(1) of two 700-char lines -> too large for buffer
```

The refusal itself is defensible (the alternative is to truncate); what is missing is that
nothing in `include/fm_text.h` or `README.md` mentions it. The header actually says the
opposite: *"Streaming replace: every occurrence of old_sub in each line is replaced by
new_sub. Works on arbitrarily large files."* — true for large files, false for a long line
inside one, which is precisely the case a CSV row or a minified JSON line hits.

**Expected:** `Text_CountLines` counts every line regardless of length (counting does not need
the content); `Text_Find` searches as much of a line as it can and does not abandon the whole
call; the headers state the line limit of the three rewriting helpers.

**Observed:** the first long line ends the whole call with the original error, and the counter
stays 0. (Before the fix, the two-700-character case was worse than an error — see
"Data-destroying before the fix" below.)

**Root cause.** `Text_ReadLine()` returns `FM_ERR_TOO_LARGE` when the line does not fit, but it
has already *consumed* the line, so the stream is positioned correctly for the next one. Both
callers treat any non-`FM_OK` return as fatal, so a single long line in the file ends the loop.
`FM_NAME_MAX * 2` (128) was chosen for the counting buffer as "a line is short"; a log file has
no such rule.

**Data-destroying before the fix (part of this finding, not a separate one).** In
`Text_DeleteLine` / `Text_Replace` the line that did not fit was *written back* as its truncated
prefix by `Text_WriteLine`. In the unmodified library, `Text_DeleteLine(1)` on a two-line file
of 700-character lines therefore removed the first line and rewrote the second as its first 511
characters, returning `FM_OK`. With the fix below the call refuses instead, so no data is lost —
but the operation the user asked for still does not happen for lines over 511 characters.
The first draft of the fix made this worse by shrinking the buffer to `char line[2]`: it
truncated a 1510-byte file to 520 bytes, because the *other* well-formed lines were then
"long" too. That is why the fix keeps the full 512-byte frame and only relaxes the error —
shrinking the buffer is not a fix, it is a second bug.

**Proposed fix** (tested, `audit_v3/edits/f3_04.py` for the code and `f3_04_doc.py` for the
documentation):

```diff
--- a/src/fm_text.c
+++ b/src/fm_text.c
@@ -185,14 +185,21 @@ FM_Result Text_CountLines(const char *path, size_t *count)
     }
     for (;;)
     {
-        char line[FM_NAME_MAX * 2];
+        /* Lines are counted, not kept, so the buffer only has to hold a
+         * line far enough to notice where it ends -- and Text_ReadLine()
+         * consumes a line that does not fit anyway (it returns
+         * FM_ERR_TOO_LARGE with the part that fits).  It used to be
+         * FM_NAME_MAX * 2 = 128 bytes and the whole call failed as soon
+         * as ONE line was longer than 127 characters: a log file, a CSV
+         * row or a JSON line made the count 0. */
+        char line[FM_INTERNAL_BUFFER_SIZE];
 
         r = Text_ReadLine(f, line, sizeof(line), NULL);
         if (r == FM_ERR_EOF)
         {
             break;
         }
-        if (r != FM_OK)
+        if (r != FM_OK && r != FM_ERR_TOO_LARGE)
         {
             (void)File_Close(f);
             return r;
@@ -228,12 +235,16 @@
         {
             break;
         }
-        if (r != FM_OK)
+        if (r != FM_OK && r != FM_ERR_TOO_LARGE)
         {
             (void)File_Close(f);
             return r;
         }
         n++;
+        /* FM_ERR_TOO_LARGE means the line was longer than the buffer:
+         * the last (FM_INTERNAL_BUFFER_SIZE - 1) bytes of it were not
+         * examined.  The line is still searched and the count still moves
+         * on, instead of the whole search stopping at that line. */
         if (strstr(line, needle) != NULL)
```

Documentation, in `include/fm_text.h`:

```diff
-/* Streaming replace: every occurrence of old_sub in each line is
- * replaced by new_sub.  Works on arbitrarily large files. */
+/* Streaming replace: every occurrence of old_sub in each line is
+ * replaced by new_sub.  Works on arbitrarily large files, one line at a
+ * time -- but not on arbitrarily long lines: a line that does not fit
+ * the internal line buffer (FM_INTERNAL_BUFFER_SIZE - 1 characters, 511
+ * by default) makes the whole call fail with FM_ERR_TOO_LARGE and the
+ * file is left as it was.  See "Line length" below. */
 ...
-/* Count lines in a file. */
+/* Count lines in a file.  Does not stop at a long line: a line longer
+ * than the internal buffer is counted (its tail is not examined, which
+ * counting does not need), so the count is exact for any input. */
 FM_Result Text_CountLines(const char *path, size_t *count);
+
+/* Line length.  Text_Replace / Text_InsertLine / Text_DeleteLine rewrite
+ * the file through a line buffer of FM_INTERNAL_BUFFER_SIZE bytes and
+ * refuse a line of FM_INTERNAL_BUFFER_SIZE characters or more with
+ * FM_ERR_TOO_LARGE, leaving the file untouched -- they never truncate.
+ * Text_Find searches the part of a long line that fits; an occurrence
+ * past that point is not found (the call still returns FM_ERR_NOT_FOUND
+ * rather than FM_ERR_TOO_LARGE).  Raise FM_INTERNAL_BUFFER_SIZE for
+ * longer lines, at the cost of that many bytes of stack per call. */
```

and the same limit as a bullet in `README.md` under "File rules worth knowing".

**Tested?** Yes: `disklines` now `count=3`, `line=3`; `text_count` counts lines of 130/131/132
characters; `long_rewrite` prints the refusals above with the file byte-identical after each
failed call (1510 B and 1501 B preserved); `text_long`, `text_tail`, `text_resync`, `text_fuzz`
unchanged; suite 478/478. The remaining limitation — `Text_Replace`/`Insert`/`Delete` do not
work on lines ≥ 512 characters — is documented, not fixed: a real fix needs a streaming rewrite
of those three helpers (write the unchanged prefix of a too-long line as it streams past and
edit only its head), which is a redesign of four functions and was not attempted here.

**Affected configurations:** all (`FM_USE_TEXT=1`). `FM_INTERNAL_BUFFER_SIZE` is 512 by
default and a `#define`, so the threshold moves with the build.

---

## 7. FILE3-05 — `FS_Mount()` publishes an uninitialised adapter; `File_Open()` then jumps through NULL

**Severity:** S1 — crash (hard fault on a target, SEGV at address 0 on the host) from a
sequence that reports success.

**Where:** `src/fm_filesystem.c:413-456` `mount_bind()`, reached from `FS_Mount()` at `:459`.
`FS_Register()` at `:182` has the check that a mount does not.

**How to reproduce.** `audit_v3/harness/uninit_fs.c`:

```sh
sh audit_v3/build_harness.sh audit_v3/harness/uninit_fs.c /tmp/uninit_fs
/tmp/uninit_fs
```

```
FM_FS.ops.file_open = (nil), ops.mount = (nil)
FS_Mount(all-zero FM_FS)   -> OK                 [want invalid argument]
  ...published; the next call would jump through NULL
FS_Register(all-zero)      -> invalid argument   [want invalid argument]
FS_Mount(half-filled)      -> OK                 [want invalid argument]
FS_Mount(FMFS_Init'ed)     -> OK                 [want OK]
  File_Open on it          -> OK
  File_Write               -> OK, 3 bytes

uninit_fs: FAIL
```

Adding `-DFM_UNINIT_CRASH=1` takes the next step and the process dies:

```
AddressSanitizer: SEGV on unknown address 0x000000000000 (pc 0x000000000000 ...)
    #1 File_Open src/fm_file.c:989
```

**Expected:** `FS_Mount()` refuses an adapter table without `file_open` / `file_read` /
`file_write` — the same three the registry already refuses at `fm_filesystem.c:186` — and no
mount is published. Note the harness also mounts the currently-failing real case:
`FS_Mount("/FC", &fs)` where `fs` was never passed to `FMFS_Init()`.

**Root cause.** An `FM_FS` is a struct the caller owns; `FMFS_Init()` / `LittleFS_Init()` /
`FatFS_Init()` / `PosixFS_Init()` fill in `ops`. A static `FM_FS` that was never initialised is
all zeros, and `mount_bind()` only calls `fs->ops.mount` *if it is not NULL*, then returns
`FM_OK` — so an empty table sails through, `fs->mounted = FM_TRUE` is set, the mount is
published, and the first `File_Open()` calls `f->fs->ops.file_open(...)` at
`src/fm_file.c:29`-ish (via `file_open_internal`) with a NULL pointer. Registration is the
documented route for a custom adapter and it validates; mounting is the route used by every
example, and it did not.

**Proposed fix** (tested, `audit_v3/edits/f3_05.py`):

```diff
--- a/src/fm_filesystem.c
+++ b/src/fm_filesystem.c
@@ -451,9 +451,31 @@ static FM_Result mount_bind(FM_FS *fs, const char *normalized)
     fs->storage = storage;
     if (fs->ops.mount != NULL)
     {
-        return fs->ops.mount(fs, storage);
+        FM_Result r = fs->ops.mount(fs, storage);
+
+        if (r != FM_OK)
+        {
+            return r;               /* the adapter's own diagnosis wins */
+        }
+    }
+    else if (storage == NULL)
+    {
+        return FM_ERR_NOT_READY;
+    }
+
+    /* A table that got this far without a file-open, file-read and
+     * file-write hook is an FM_FS that was never handed to its adapter's
+     * *_Init() (all zeros behind no mount hook) or one that is only half
+     * filled in.  Publishing it makes the first File_Open() call a NULL
+     * function pointer -- a hard fault.  FS_Register() has always refused
+     * exactly these three ops; a mount does not go through the registry,
+     * so it checks them here. */
+    if (fs->ops.file_open == NULL || fs->ops.file_read == NULL ||
+        fs->ops.file_write == NULL)
+    {
+        return FM_ERR_INVALID_ARGUMENT;
     }
-    return (storage == NULL) ? FM_ERR_NOT_READY : FM_OK;
+    return FM_OK;
 }
```

**Tested?** Yes: `uninit_fs` prints `PASS` (all-zero table → `invalid argument`, half-filled
table → `invalid argument`, a real adapter still mounts and writes). The suite's own
"Debug: a mount that fails is an error line" test depends on a *non-zero* table with a failing
`mount()` hook still reaching the log line, which the `r != FM_OK` branch preserves — the first
version of the fix put the check before the hook and turned that test into 477/478; the version
above keeps 478/478. `make tsan` 483/483, 32-bit 479/479, `make fatfs` and `make ports` pass.

**Affected configurations:** all. A build that never passes an uninitialised `FM_FS` sees no
change (one extra NULL comparison per mount).

---

## 8. FILE3-06 — every line edit silently converts a CRLF file to LF

**Severity:** S1 — loss/corruption of stored data, returned as `FM_OK`. The edited word is
right, and every other line ending in the file has changed.

**Where:** `src/fm_text.c` `Text_Replace()` (`:251`), `Text_InsertLine()` (`:373`),
`Text_DeleteLine()` (`:476`); the cause is the pair `Text_ReadLine()` (`:17`) +
`Text_WriteLine()` (`:112`) they copy through.

**How to reproduce.** `audit_v3/harness/line_end.c` — a 26-byte CSV with CRLF line ends, one
word replaced:

```sh
sh audit_v3/build_harness.sh audit_v3/harness/line_end.c /tmp/line_end
/tmp/line_end
```

```
  written (CRLF, 26 bytes)     OK   26 bytes: 69 64 2C 6E 61 6D 65 0D 0A 31 2C 61 6C 70 68 61 0D 0A 32 2C 62 65 74 61 0D 0A
  Text_Replace(alpha -> ALPHA) -> OK
  after                        OK   23 bytes: 69 64 2C 6E 61 6D 65 0A 31 2C 41 4C 50 48 41 0A 32 2C 62 65 74 61 0A
  [the two untouched lines changed their line ending]
```

`harness/line_end2.c` covers the rest of the matrix. Observed on the unmodified library:

```
  FAIL CRLF file, a line inserted          8 bytes (want 11): 61 0A 4E 45 57 0A 62 0A
  FAIL CRLF file, appended at the end      8 bytes (want 11)
  FAIL no final newline stays absent       8 bytes (want 7):  4F 4E 45 0A 74 77 6F 0A
  FAIL mixed CRLF/LF is preserved per line 6 bytes (want 8):  61 0A 42 0A 63 0A
  ok   a lone CR inside a line is kept
  ok   empty file, one line inserted
  FAIL CRLF file, last line deleted        2 bytes (want 3):  61 0A
```

A randomized check (`audit_v3/harness/mixed_end_fuzz.c`, 400 random files with mixed LF/CRLF/
no-final-newline/lone-CR layouts, one edit each, compared with an in-memory reference):
**335 of 400 wrong** on the unmodified library, **0 of 400** with the fix.

**Expected:** an edit changes what it was asked to change and nothing else. `Text_ReadLine`'s
CR removal is *documented* (`README.md`: "removes the `\r` of a `\r\n` line end") and correct
for a reader — the defect is that the three *rewrite* helpers reuse it and then write the line
back with `Text_WriteLine`, which always appends `'\n'`.

**Root cause.** Two independent losses, both from the same shortcut:

1. `Text_ReadLine()` folds `\r\n` to `\n` by design (that is FILE-04's fix, which stopped it
   from deleting *lone* CRs); `Text_WriteLine()` has no way to say "this one ended with CRLF",
   so every line — changed or not — is written back with a bare `\n`.
2. `Text_WriteLine()` always writes an ending, so a file whose last line had no newline gains
   one; `Text_InsertLine()` inserts its new line with `\n` in a CRLF file, and
   `Text_DeleteLine()` of the last line of a CRLF file leaves an LF-only remainder.

Nothing in the API or the headers warns that these three calls, unlike `File_Create` /
`File_WriteText`, normalise line endings; the result code is `FM_OK` and no log line is
produced. On an embedded device this is the kind of change a downstream tool (a spreadsheet, a
Windows-side script) notices, not the firmware.

**Proposed fix** (tested, `audit_v3/edits/f3_06.py`): give the three rewrite helpers their own
reader/writer pair that reports and preserves the ending of each line. `Text_ReadLine` and
`Text_WriteLine` keep their documented behaviour, so no existing caller changes.

```diff
--- a/src/fm_text.c
+++ b/src/fm_text.c
@@ -132,6 +132,103 @@
+/* Read one line without interpreting its ending.  *crlf says the line
+ * ended with CRLF (that CR is not in the buffer), *eol says it ended with
+ * a newline at all -- the last line of a file need not.  Used by the
+ * three rewrite helpers, which must give back the lines they did not
+ * change: Text_ReadLine's documented CR removal turned every CRLF file
+ * into an LF file on any edit. */
+static FM_Result text_read_raw(FM_File *f, char *buf, size_t size,
+                               size_t *len_out, FM_Bool *crlf_out,
+                               FM_Bool *eol_out)
+{
+    size_t n = 0;
+    FM_Bool any = FM_FALSE;
+    FM_Bool crlf = FM_FALSE;
+    FM_Bool eol = FM_FALSE;
+    FM_Bool overflow = FM_FALSE;
+
+    for (;;)
+    {
+        uint32_t rd = 0;
+        char c = 0;
+        FM_Result r = File_Read(f, &c, 1, &rd);
+
+        if (r == FM_ERR_EOF || (r == FM_OK && rd == 0))
+        {
+            if (!any)
+            {
+                return FM_ERR_EOF;
+            }
+            break;
+        }
+        if (r != FM_OK)
+        {
+            return r;
+        }
+        any = FM_TRUE;
+        if (c == '\n')
+        {
+            eol = FM_TRUE;
+            break;
+        }
+        if (n + 1 < size)
+        {
+            buf[n++] = c;
+        }
+        else
+        {
+            overflow = FM_TRUE;
+        }
+    }
+    /* the CR of a CRLF belongs to the ending, not to the text -- but only
+     * when there really is a '\n' after it: a CR that ends the file is
+     * data (Text_ReadLine has the same rule) */
+    if (eol && n > 0 && buf[n - 1] == '\r')
+    {
+        n--;
+        crlf = FM_TRUE;
+    }
+    buf[n] = '\0';
+    if (len_out != NULL)
+    {
+        *len_out = n;
+    }
+    if (crlf_out != NULL)
+    {
+        *crlf_out = crlf;
+    }
+    if (eol_out != NULL)
+    {
+        *eol_out = eol;
+    }
+    return overflow ? FM_ERR_TOO_LARGE : FM_OK;
+}
+
+/* The matching writer: the text as it came out of text_read_raw(), then
+ * the same ending (nothing at all for the last line of a file that had
+ * none). */
+static FM_Result text_write_raw(FM_File *f, const char *buf, size_t len,
+                                FM_Bool crlf, FM_Bool eol)
+{
+    uint32_t wr = 0;
+    FM_Result r;
+
+    if (len > 0)
+    {
+        r = File_Write(f, buf, (uint32_t)len, &wr);
+        if (r != FM_OK)
+        {
+            return r;
+        }
+    }
+    if (!eol)
+    {
+        return FM_OK;
+    }
+    return File_Write(f, crlf ? "\r\n" : "\n", crlf ? 2u : 1u, &wr);
+}
@@ Text_Replace, the copy loop
-        r = Text_ReadLine(src, line, sizeof(line), &line_len);
+        r = text_read_raw(src, line, sizeof(line), &line_len, &line_crlf,
+                          &line_eol);
 ...
-        r = Text_WriteLine(dst, line);
+        r = text_write_raw(dst, line, line_len, line_crlf, line_eol);
@@ Text_InsertLine, the copy loop
         if (n == at_line)
         {
-            r = Text_WriteLine(dst, line);
+            /* the new line takes the ending of the line it goes in front
+             * of, so a CRLF file stays a CRLF file */
+            r = text_write_raw(dst, line, strlen(line), line_crlf, FM_TRUE);
             ...
         }
-        r = Text_WriteLine(dst, buf);
+        last_crlf = line_crlf;
+        r = text_write_raw(dst, buf, line_len, line_crlf, line_eol);
@@ Text_DeleteLine, the copy loop
-        r = Text_WriteLine(dst, buf);
+        r = text_write_raw(dst, buf, line_len, line_crlf, line_eol);
```

**Tested?** Yes. `line_end`: 26 bytes in, 26 bytes out, only the replaced word differs.
`line_end2`: all eight cases `ok` (insert into CRLF, append at end, absent final newline kept
absent, mixed endings preserved per line, lone CR kept, empty file gets `first\n`, CRLF last
line deleted leaving `a\r\n`). `mixed_end_fuzz`: 0/400 wrong. The whole suite 478/478, `make
asan` 478/478, `make tsan` 483/483 with no TSan warning, 32-bit UBSan 479/479, `make fatfs`,
`make ports`. `text_fuzz`, `text_tail`, `text_long`, `text_resync`, `long_rewrite` unchanged
(they now report the *preserved* endings).

**Affected configurations:** all (`FM_USE_TEXT=1`), every backend — this is above the adapter
layer.

**Behaviour change to be aware of.** `Text_Replace` on a file whose last line has no newline
now keeps it without one (before: it added one). `Text_InsertLine` into a CRLF file writes a
CRLF line (before: LF). Both are the point of the fix, but they are visible, and anyone who
relied on the normalisation (a "clean up my line endings" use) loses it; `Text_ReadAll` +
`Text_WriteAll` still does that explicitly.

---

## 9. Suspicions (read, not reproduced — not counted)

**SUSP3-01 — an open LittleFS handle does not keep its file alive.** Confirmed behaviour, not
a speculation, but it is worth one paragraph because it reads like a defect and is not
reported as one (see §10): `harness/lfs_delete_open.c` shows that deleting a file that is open
for writing returns `FM_OK`, the name disappears, the handle keeps accepting writes, and
closing it leaves nothing — the bytes are lost silently. The same for a reader (it keeps
reading the old content after the delete). This is documented ("LittleFS: one writer or many
readers per file", and `File_Delete` is not documented to be refused while open) and matches
POSIX `unlink` semantics, so it is not a finding; but a caller who deletes a file to reclaim
space while a logger still holds it loses the log with no error. A one-line guard (refuse the
delete with `FM_ERR_BUSY` when the adapter has an open handle on that file) would be cheap.

**SUSP3-02 — `Text_Find` on a line longer than 511 characters can miss the needle without
saying so.** After the FILE3-04 fix the search continues over the part of a long line that
fits; an occurrence in the tail of such a line is not found and the call answers
`FM_ERR_NOT_FOUND`. That is now what the header says, but "not found" and "not looked at" are
the same answer to the caller. A `FM_ERR_TOO_LARGE` when the file contains any line that was
truncated would be more honest; I did not change it because it would make `Text_Find` fail on
files where the needle *is* present in a short line.

**SUSP3-03 — `Explorer_Load()` truncation and history depth.** Carried over from the previous
round (SUSP-03) and *not* re-examined in depth; `README.md` now documents
`Explorer_IsTruncated()`, so the listing half is documented. The `Explorer_Enter()` history
depth half was not re-tested.

**SUSP3-04 — a FAT volume's close error is still ignored by `File_SafeReplace`.** The previous
round's SUSP-02. Reading it again at `910a605`: `fm_safe.c` checks the close of the temporary
now, and deletes a stale temp; but `File_Transaction`'s restore path still deletes the backup
after a failed restore (SUSP-04 of the previous round). I could not construct a failing
`File_Close` on the host backends (none of them can fail on close), so this stays unproven and
unreported.

**SUSP3-05 — worst-case stack of the tree walks.** Carried over unchanged: `-fstack-usage`
gives 1 632 B per level for `dir_copy_recursive`, `Directory_Copy` also holds a second
`FM_Dir`/`FM_Path` per level, `FM_DIR_MAX_DEPTH` is 16. The previous round's 16 KiB
`pthread` bisection and the painted-stack numbers still disagree with the static numbers, and I
found no way to reconcile them on x86-64. Needs a target measurement; not a finding.

---

## 10. Examined and found sound

Ordered by the effort spent. Every item says how far the check went, so an empty result here
means something.

**Tier 1 — the VFS and the file layer**

* **`File_Open` / `File_Read` / `File_Write` / `Seek` / `Tell` / `Size` / `Truncate` / `Flush` /
  `Close` across four backends.** `matrix_fs.c`: every operation × four modes × FMFS,
  LittleFS, POSIX, RamDisk, including write-through-append-handle, size after seek past end,
  truncate up, read at EOF, close-twice, and the read-cache interaction — same answer on all
  four. `regress_v2.c` re-checks the seven earlier findings' exact shapes ("all as fixed").
* **Handle and slot accounting.** `handle_leak.c`: 20 000 open/close pairs with injected
  failures (bad path, bad mode, backend error, `FM_MAX_OPEN_FILES` exhausted) leave the pool at
  zero used slots; `File_Open(NULL, …)` and `File_Open(p, 0)` do not leak (that was a real
  defect the previous round fixed — re-verified).
* **Mount registry.** `fs_slots.c`: 1 200 failing mount attempts + 3 000 bad registry calls
  leave all four mount slots usable; same FS on a second path → `busy`; second FS on a taken
  path → `exists`; fifth mount → `no memory`; after 3 000 failures a normal mount works.
  (This harness also found FILE3-05, whose first fix attempt made it print `PROBLEM`: a
  *failing* backend's `mount()` must still be called so the log line and its `FM_ERR_CORRUPTED`
  survive.)
* **FMFS as a filesystem.** `diff_fs.c`: 60 seeds × 150 ops and 7 seeds × 400 ops of random
  create/write/append/truncate/rename/mkdir/delete/repack against FMFS, LittleFS and POSIX, with
  the full tree + size + content compared after each operation: 0 divergences. `edit_model.c`:
  300 ops × 12 seeds × 2 backends of the three text rewrites against an in-memory model: all
  identical. `walk.c`: recursive list/scan/copy/delete on a 4-directory, 5-file tree with
  cancellation before the first entry and in the middle: `seen` counts exact, cancellation
  leaves the tree untouched.
* **Read cache.** `cache_stale.c`: reads through the cache after write, truncate, rename-over
  and delete, on three backends, 200 iterations each: never a stale byte.
* **Path handling.** `diff_err.c` and `matrix_fs.c` exercise `..`/`.`/double slashes, a path
  exactly `FM_PATH_MAX` long, a component longer than `FM_NAME_MAX`, mount-point prefixes
  (`/SD` vs `/SDX`) and relative paths: the prefix bug does not exist here, and every case
  agrees with the reference backend.
* **Write path and power-fail model.** `nor_block.c` (626 reads / 1 256 programs / 72 erases,
  0 program-before-erase violations, remount reads back the same bytes) and `nor_eeprom.c`
  (0 erase-rule violations, out-of-range access refused). `async_model.c`: the async API's
  operation sequence and results are identical to the blocking one on a wide tree (40 entries)
  and a deep one (24 entries), including a scan of the mount root.

**Tier 2 — utilities**

* **The log layer.** `log_trunc.c` at `FM_LOG_BUF_SIZE` 512 and 64 (separately built
  libraries, since the size is a compile-time constant): a message of 509…514 characters is cut
  at 511/63 bytes, never longer than the buffer, the tag prefix is included in the budget, a
  200-character tag with a 999-character message still ends in one `'\n'`, no control bytes are
  emitted, `FM_Log(NULL)`, `FM_Log_Tagged(…, NULL, …)`, `FM_Log_Write(NULL, 0)` and
  `FM_Log_Write("x", 0)` do not crash, `FM_Log_Dropped()` stays 0 in single-threaded use. The
  suite's own "8323 messages, 548 646 bytes, 0 malformed, 3 dropped" check is unchanged.
* **The log sink under load.** `log_guard.c` (a simulated ISR that logs while a task formats):
  991 interrupts logged, 0 messages with mixed content, the drop guard released afterwards.
  `log_threads.c` under TSan: see FILE3-03.
* **Wildcard matching.** `wildcard_ref.c`: the two matchers (`fm_directory.c`'s and
  `fm_logger.c`'s copy) agree on 3 828 pattern/name pairs; the adversarial 62-character filter
  against a 63-character name takes 0.000 s (no backtracking blow-up); a 71-character name is
  still reported as `too large for buffer` when its directory is listed (documented limit).
* **JSON.** `json_fuzz.c`: 100 000 random documents per seed, 20 599 accepted and fully walked
  through every accessor, no ASan/UBSan report, no hang; parse-then-write of the accepted
  subset round-trips.
* **Checksums.** `crc_bin.c`: CRC-32 gives `CBF43926` for "123456789" (the standard check
  value), and the 16-bit variants give `29B1` / `4B37`; `cksum_model.c` agrees with the model on
  two volumes.
* **Text line handling.** `text_fuzz.c` (first/last/only line, empty file, empty lines in the
  middle, a buffer smaller than the line), `text_tail.c` (a file ending without a newline, a
  lone CR at the end of the file), `text_resync.c` (the reader is positioned correctly after a
  `FM_ERR_TOO_LARGE` line, so the next call reads the right line), `text_long.c` (200-character
  lines through `Text_Replace`), `limit_scan.c` (the exact length thresholds). All as expected
  after FILE3-06; `text_fuzz` was failing on the unmodified library exactly because of FILE3-06
  and its two "keep the file" cases, which is how that finding was caught.
* **`fm_safe.c`** (read only): `File_SafeReplace` checks the close of the temporary, deletes a
  stale temp, and uses `<path>.fm_tmp` / `.fm_tmp.bak`; `File_Transaction` keeps the backup when
  the replace fails. The remaining "backup deleted after a failed rollback" path is SUSP3-04.
* **`fm_logger.c`** (read only): the iterative `wildcard_match_local` is the one that is called;
  `logger_circular_cleanup`'s index arithmetic is bounded and matches the documented rotation.

**Tier 3 — drivers (read only)**

`fm_eeprom.c` / `fm_eeprom_chips.c` / `nor_eeprom.c`'s model, `fm_device.c`, `fm_usb_msc.c`
(LBA bounds checked, `harness` covers out-of-range) were read; no defect found that could be
reached without hardware. No driver code was executed.

---

## 11. Not examined

* `src/fm_spiflash.c`, `src/fm_qspi.c`, `src/fm_sd_hal.c` beyond the host models in
  `nor_block.c` / `nor_eeprom.c`; no real SPI flash, SD card or QSPI part was available.
* `fm_usb_msc.c`'s SCSI command handling beyond the out-of-range-LBA test that the existing
  suite runs.
* FatFS adapter behaviour against a real card: `FR_DISK_ERR`, dirty-flush on `f_close()`,
  `f_sync()` failure. The host FatFS mock (`test/fatfs_mock`) cannot produce those, so FILE-03
  of the previous round is "fixed on the mock" and no more.
* The recursive tree walks' stack cost on a 32-bit target (SUSP3-05) — measured only on
  x86-64, with contradictory numbers.
* `src/fm_video.c`, `fm_audio.c`, `fm_image.c`, the codec glue and `third_party/` — another
  auditor's part; touched only where a filesystem call goes through them.
* `src/fm_transfer.c` beyond reading its header: no harness.
* `fm_json.c`'s writer under a full buffer (only the parser was fuzzed).
* `fm_logger.c`'s crash-safe record path under power loss (only the reading path was checked).
* The RTOS ports beyond `make ports` compiling them (`freertos`, `cmsis_rtos2`, `pthread`):
  no scheduler was run.

---

## 12. Outside my scope (one line each)

* Image/video/audio decoding correctness — another auditor's part.
* The board and example projects (`board/`, `examples/`) except as callers.
* Vendored `third_party/` code as such; only this library's glue to it was in scope.
* Hardware electrical behaviour, timing and DMA.
* Build/packaging of the whole repository (CMake, Kconfig) beyond `make ports` / `make fatfs`.

---

## 13. Verdict on the earlier findings

| Earlier ID | Verdict | How I checked (variants) | Would the test catch a regression? |
|---|---|---|---|
| FILE-01 `File_Truncate` on a read-only handle aborts (LittleFS) | **fixed** | `File_Truncate` with `FM_OPEN_READ` on FMFS, LittleFS, POSIX: `access denied` on all three, file 10 B unchanged, handle still usable; also through `File_OpenEx` and after a seek | **yes** — `audit_v2_tests.h:970` runs it on every backend |
| FILE-02 `File_Append` writes at a stale size (overwrites data) | **fixed** | the file grown behind an append handle's back (grow by 4, append 2 → `AAAABB`, 6 B), two append handles taking turns, `FM_OPEN_APPEND` + `Seek` | **yes** — three tests (`:971`, `:972`, `av2_append_ignores_seek`) |
| FILE-03 FatFS adapter leaves the position at the truncate size | **fixed on the mock** | `fm_test`'s FatFS truncate test (position 4, then `456789`); cannot be reproduced on a real card here | yes for the mock path |
| FILE-04 `Text_ReadLine` deletes every CR | **fixed** | a lone CR inside a line, a CR at the end of the file, a CR that does not fit the buffer, `Text_Replace` on each — all three tests pass; `text_tail` / `text_fuzz` agree. **The other half of the same rewrite path was still broken and is now FILE3-06.** | yes for the reader; **no** for the writer — that is why FILE3-06 survived |
| FILE-05 `Stream_InitFile` leaks a slot | **fixed** | re-init on a bound stream → refused, context comes back with `Stream_Close`, 1 000 cycles leave the pool clean | **yes** — two tests (`:475`, `:503`) |
| FILE-06 POSIX adapter opens a directory as a file | **fixed** | `File_Open("/dir", FM_OPEN_READ)` → `is a directory`; also with write+create, and via `File_ReadAll` | **yes** — `:522` |
| FILE-07 `FM_DEBUG_LEVEL=0` does not compile under `-Werror` | **fixed** | `make libfilemanager.a FM_DEBUG_LEVEL=0 CFLAGS_EXTRA=-Werror` returns 0; counts: 478/478 at the default level | partly — a build test, not a run test |
| SUSP-01 tree-walk stack | still open | see SUSP3-05 | — |
| SUSP-02 `File_SafeReplace` ignores the close error | **fixed** (the close is checked) | read + `av2_safe_write` on FMFS | yes on the mock |
| SUSP-03 `Explorer_Load` truncates silently | **fixed/documented** (`Explorer_IsTruncated()`) | read `README.md` + `av2_explorer`; the history-depth half not re-tested | partly |
| SUSP-04 `File_Transaction` deletes the backup after a failed rollback | **not fixed** | read `fm_safe.c`; no way to force the restore to fail on the host | no |

And, for this round's own findings (the tests added here):

| ID | Test added (suite) | Fails on the unfixed code? | Passes on the fixed code? |
|---|---|---|---|
| FILE3-01 | 3 × `FILE3-01: File_Delete on a directory` (one per backend) | yes, 2 of 3 (FMFS was already right — which is the point) | yes |
| FILE3-02 | 3 × `FILE3-02: File_Move onto a directory` | yes, 1 of 3 (FMFS only) | yes |
| FILE3-03 | `FILE3-03: swapping the log sink` (threaded build only) | yes, 5 runs of 5 | yes, 5 runs of 5, TSan clean |
| FILE3-04 | 3 × `FILE3-04: a long line does not stop the call` | yes, 3 of 3 | yes |
| FILE3-05 | `FILE3-05: FS_Mount refuses an FM_FS that was never initialised` | yes | yes |
| FILE3-06 | 4 × 3 backends: CRLF replace / delete / insert, no-final-newline | yes, 12 of 12 | yes |

---

## 14. Scores

| Axis | Score | Evidence |
|---|---|---|
| **A. Malformed input** | **8** | 100 000 random JSON documents + ~260 k over the sessions with no ASan/UBSan report; 400 random line-ending layouts with no wrong byte; zeroed and garbage filesystem images (`fs_zero`, `matrix_fs`); wildcard and path fuzzing without a crash. Not 10: no coverage-guided fuzzer and no 32-bit harness build, and the drivers were not executed at all. |
| **B. Correct results** | **5** | Six confirmed defects, four of which returned `FM_OK` with the wrong result (FILE3-01, 04, 06, and the reader half of 01). Once found, each was small and local — but three of them (FILE3-01, 04, 06) sat in the "simple API" layer that every user calls. |
| **C. Errors and resources** | **6** | Outstanding in this area: a crash reachable from a success return (FILE3-05), a data race (FILE3-03), a wrong error code (FILE3-02), and two adapters answering success for the wrong object (FILE3-01). Against that: the handle pool, mount slots, cache and registry survive 20 000+ injected failures (`handle_leak`, `fs_slots`, `cache_stale`), and all seven earlier resource findings stay fixed. |
| **D. Memory and timing** | **7** | No leak or overflow in any sanitizer run; calls are bounded and the fixes add no allocation and no stack growth (one `stat` per delete, a 512-byte frame that was already there). The manual line edit writes one byte at a time through `File_Read` — correct, but slow on a real card; and the tree-walk stack on a target is unmeasured. |
| **E. Documentation** | **5** | The README is unusually thorough and was right about the seven earlier findings — but it promised `File_Delete` files-only semantics that two adapters did not implement, and said the text rewrites work on "arbitrarily large files" while silently normalising every line ending and refusing long lines. Three of the six findings are documentation-level failures as much as code ones; `fm_text.h` now states the line limit (this round's fix). |
| **F. Tests** | **5** | 478 tests, TSan and a 32-bit run are more than most projects have, and the earlier round's regression tests are real. But nothing covered `File_Delete` on a directory, a CRLF file through a line edit, `FS_Mount` with an uninitialised adapter, or a line longer than the buffer — four of six findings — and FILE-04's tests stopped at the reader. The score is for the state I found; the 22 tests added here (checked to fail on the unfixed code) would raise it to about 7/10 for the areas they cover. |
| **G. Diagnosability** | **7** | Result codes are specific and consistent (that is how FILE3-02 stood out), the log layer is bounded, well-formed and message-exact under load, and a failed mount is an error line. Deductions: three findings returned `FM_OK`, and FILE3-04's `Text_Find` cannot distinguish "absent" from "not looked at". |

**Ready for a product under these conditions** — for my part (files, storage,
filesystems) only:

1. apply the six fixes in §3–§8 (all tested together; 478/478, 483/483 TSan, 479/479 32-bit);
2. call `Directory_Delete()` for directories — after the fix `File_Delete()` on one returns
   `is a directory` instead of silently succeeding, so an existing caller that relied on the
   bug will now see an error;
3. do not edit text files with CRLF line endings through `Text_Replace` /
   `Text_InsertLine` / `Text_DeleteLine` until they ship with the fix, or accept that line
   endings change;
4. treat a line of 512 characters or more as unsupported by those three helpers, and say so in
   the product's limits (the header now does);
5. measure the recursive tree walks' stack on the target before running them on a 2 KB task
   stack;
6. do not ship a caller that can pass an `FM_FS` it never initialised without the §7 fix.

---

## 15. Appendix — reproducer sources and generator scripts

All harness sources are in `audit_v3/harness/` (43 files, 6 310 lines of C), all fixes in
`audit_v3/edits/`, build scripts in `audit_v3/`. Every confirmed finding can be reproduced
from this directory alone.

| File | Purpose |
|---|---|
| `build_lib.sh` | ASan+UBSan library, the project's own `make`, host configuration |
| `build_harness.sh <src.c> <out>` | compile one harness against that library |
| `build_against.sh <tree> <src.c> <out>` | same against a *patched copy* (`/tmp/lib_all6`) |
| `build_log_harness.sh` | harnesses that need a different `FM_LOG_BUF_SIZE` (unsanitized) |
| `build_log_threads.sh`, `build_threads_against.sh` | `FM_USE_THREADS=1` + TSan builds |
| `build_patched.sh`, `build_patched_threads.sh` | patch-one-finding + harness in one step |
| `edits/f3_01.py` … `f3_06.py` | the six fixes as source edits (reliable on this CRLF tree); `edits/all.py` applies them in order |
| `patches/*.patch` | the same six fixes as unified diffs against `910a605`: one file per finding (`file3_01_deletes_a_directory.patch` … `file3_06_crlf_line_endings.patch`) and `all_six.patch`. `patch -p1 < audit_v3/patches/all_six.patch` in `FileManager_lib/` reproduces the tested tree byte for byte |
| `patches/tests_regression.patch` | the 22 regression tests (`test/audit_v3_tests.h`, `test/main.c`), separate so the fixes can be taken without them |
| `tools/crc_ref_check.py` | the CRC reference values used by `crc_bin.c` |

Harnesses, by finding and by area:

* **FILE3-01** `del_kind.c` (both directions, three backends), `delete_dir.c`,
  `delete_nonempty.c`
* **FILE3-02** `move_err.c`, `diff_err.c`
* **FILE3-03** `log_threads.c` (TSan, 160 000 messages), `log_guard.c` (ISR model),
  `log_trunc.c` (buffer limits at 512 and 64), `log_harness.c`
* **FILE3-04** `disklines.c`, `limit_scan.c`, `text_count.c`, `text_long.c`
* **FILE3-05** `uninit_fs.c` (with `-DFM_UNINIT_CRASH=1` for the SEGV)
* **FILE3-06** `line_end.c`, `line_end2.c`, `mixed_end_fuzz.c` (400 random layouts)
* **Regression / differential** `regress_v2.c`, `diff_fs.c`, `edit_model.c`, `matrix_fs.c`,
  `fs_slots.c`, `handle_leak.c`, `cache_stale.c`, `walk.c`
* **Storage models** `nor_block.c`, `nor_eeprom.c`, `async_model.c`, `fs_zero.c`,
  `uninit_fs.c`
* **Utilities** `json_fuzz.c`, `wildcard_ref.c`, `cksum_model.c`, `crc_bin.c`,
  `text_fuzz.c`, `text_tail.c`, `text_resync.c`
* **LittleFS specifics** `lfs_swap.c`, `lfs_conflict.c`, `lfs_delete_open.c`, `lfs_dirs.c`
* Unused drafts kept for reference: `crash_bisect.c`

Applying all fixes and re-running everything:

```sh
mkdir -p /tmp/lib_all && tar -C Clain_FileManager/FileManager_lib -cf - \
  --exclude='*.o' --exclude='*.a' . | tar -C /tmp/lib_all -xf -
cd /tmp/lib_all
python3 /home/user/Clain_Filemanagaer/audit_v3/edits/all.py   # or:
patch -p1 < /home/user/Clain_Filemanagaer/audit_v3/patches/all_six.patch
python3 /home/user/Clain_Filemanagaer/audit_v3/edits/f3_tests.py  # optional:
patch -p1 < /home/user/Clain_Filemanagaer/audit_v3/patches/tests_regression.patch
make -j2 fm_test && ./fm_test          # 500/500 with the tests (478/478 without)
make asan                              # 500/500, sanitizer active
make tsan                              # 506/506, 0 TSan warnings
sh tools/check_32bit.sh ubsan          # 501/501, zig cc 0.17.1, x86-linux-musl
make fatfs && make ports               # host builds, both RTOS ports compile
make m32                               # 501/501, plain 32-bit
make matrix                            # 49 configurations, 0 failed
```

The tests are also the negative control: apply `tests_regression.patch` to the
**unmodified** tree and the suite drops to 481/500 (486/506 threaded), with every failure
naming the finding it belongs to. That is the check that the tests are worth having.

Harnesses against the patched tree (`/tmp/lib_all6` after `edits/all.py`):

```sh
cd audit_v3
sh build_against.sh /tmp/lib_all6 harness/del_kind.c    /tmp/p_del_kind
sh build_against.sh /tmp/lib_all6 harness/move_err.c    /tmp/p_move_err
sh build_against.sh /tmp/lib_all6 harness/uninit_fs.c   /tmp/p_uninit_fs
sh build_against.sh /tmp/lib_all6 harness/line_end.c    /tmp/p_line_end
sh build_against.sh /tmp/lib_all6 harness/line_end2.c   /tmp/p_line_end2
sh build_against.sh /tmp/lib_all6 harness/mixed_end_fuzz.c /tmp/p_me
sh build_against.sh /tmp/lib_all6 harness/disklines.c   /tmp/p_disklines
sh build_against.sh /tmp/lib_all6 harness/long_rewrite.c /tmp/p_long_rewrite
```

Results on the unmodified library and on the patched tree are quoted verbatim in §3–§8.

### How to read this report against the brief

| Brief §8 item | Where |
|---|---|
| 1. Persian summary (5–10 lines) | top of file |
| 2. Audit record: commit, machine, compiler, builds, methods, 32-bit | §1 |
| 3. Table of findings (ID, severity, title, file:line, status) | §2 |
| 4. One section per finding (severity, where, repro + exact output, expected/observed, root cause, diff, tested, configurations) | §3 – §8 |
| 5. Suspicions | §9 |
| 6. Examined and found sound | §10 |
| 7. Not examined | §11 |
| 8. Outside my scope | §12 |
| 9. Verdict on the earlier findings | §13 |
| 10. Scores A–G + readiness line | §14 |
| Appendix: harness sources, generator scripts, crashing inputs | §15 |
