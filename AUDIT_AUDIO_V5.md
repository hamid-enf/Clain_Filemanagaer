# AUDIT_AUDIO_V5.md — external audit V5, audio part

Commit audited: `fc31b259c8345180a0ca2297c802c8e9f4b7dcdc` (`fc31b25`), branch
`arena/ba6a87b3-clain-filemanagaer`. Report date: 2026-11-13.

---

## 1. Summary in Persian

- کامیت ممیزی‌شده `fc31b25` است. خط پایهٔ `make test` روی ماشین من پیش از هر تغییری
  `Results: 589/589 passed, 0 failures` بود و همان‌طور تأیید شد.
- چهار عیب تأییدشده. سه‌تای اول یک خانواده‌اند («چیزی که به فایل اضافه شده، کتابخانه
  را گمراه می‌کند»)، چهارمی نه: یک **S2** (تشخیص نوع فایل، `AUD5-03`، تگ ID3v2 هر
  فایلی را MP3 گزارش می‌کرد)، یک **S2** (`AUD5-02`: پخش‌کننده روی یک ترک خراب
  بی‌نهایت بار restart می‌کند و لیست هرگز جلو نمی‌رود)، یک **S3** (`AUD5-01`: تگ
  ID3v2 جلوی FLAC/Ogg/WAV/Opus فایل معتبر را با کد خطای غلط رد می‌کرد)، و یک **S3**
  (`AUD5-04`: `Audio_Seek` به بعد از پایان در MP3 موقعیت را جای قبلی رها می‌کرد).
- از نظر شدت، دو **S2** جلوترند؛ اما آنچه در استفادهٔ روزمره بیشتر دیده می‌شود
  **AUD5-01** است: تگ‌گذاری با ID3v2 روی فایل‌های FLAC، Ogg و WAV (کار معمول هر
  tagger و بسیاری از ابزارهای موبایل) فایلِ سالم را در `Audio_Open` با `-17` رد
  می‌کرد. رفعِ V4 (`AUD4-01`) فقط ADTS را از پشت تگ نگاه می‌کرد؛ همان الگو در سه
  مسیر دیگر تکرار شده بود و یک چهارمی (Ogg Opus) بدتر: فایل به دیکدر Vorbis می‌رفت.
- از یافته‌های پیشین این بخش: `AUD4-01`، `AUD4-02`، `AUD4-03`، `SUSP-A` و رفع WAV با
  طول صفر، همه با حذف محلیِ رفع‌عیب و اجرای تست بررسی شدند و **همه تثبیت‌شده‌اند**؛
  تست‌های `AUD4-01..03` با حذف رفع شکست می‌خورند. دو رفع از دور نخست (F2، F30) هنوز
  تستی ندارند که با حذفشان شکست بخورد.
- پاسخ سرنخ‌ها: **W1** اندازه‌گیری شد (۵ پهنای بافر، توقف ۰..۲۰۰ ms، بدون underrun)؛
  **W2** نمونه‌به‌نمونه درست است (`starts=49 ends=48`, maxdiff 0)؛ **W3** دو شکاف
  تستی پیدا شد؛ **W4** ولوم ۲۰۰٪ اشباع می‌کند نه wrap؛ **W5** یک عیب تازه داد
  (`AUD5-04`).
- امتیازها: **A7 B7 C6 D6 E6 F5 G6**. نسبت جهش‌آزمایی: ۱۶ از ۲۴ جهش در بیلد پیش‌فرض دیده شد
  (۶۷٪؛ با `FM_USE_OPUS_LIB` ۱۷ از ۲۴). بزرگ‌ترین سوراخ تست: کپی `top_api/simple_audio.h`
  هیچ تستی ندارد، در حالی که همان کد در `examples/` تست دارد (جهش #۲۳).
- همهٔ چهار پچ روی یک استخراج تازهٔ `git archive fc31b25` اعمال و دوباره آزموده شد:
  `593/593` و `audiorestarttest` همه PASS و `make check` با کد خروج ۰؛ همان درخت با
  تست‌ها و بدون رفع‌ها `589/593` با چهار شکست.
- انجام‌نشده: اجرا روی برد و ARM، `make matrix` کامل، `make matrix-asan`، tsan روی
  پورت‌های FreeRTOS/CMSIS، و ابزارهایی که نصب نبودند (ffprobe/flac/sox/cppcheck/clang).
- دور بعد روی این بخش احتمالاً S1 پیدا نمی‌کند؛ محتمل‌ترین S2 بعدی تریم نشدن
  delay/padding انکودر MP3 در پخش مستقل (S-01) و بازپذیری کالبک‌هاست.

---

## 2. Audit record

### 2.1 Commit and environment

| | |
|---|---|
| Commit | `fc31b259c8345180a0ca2297c802c8e9f4b7dcdc` — `git rev-parse HEAD` |
| Date of audit | 2026-11-13 |
| Machine | x86-64, 2 cores, Debian 12, kernel 6.1.158+ |
| Compilers | gcc 12.2.0, `python3 -m ziglang cc` 0.17.1 (`x86-linux-musl` for 32-bit) |
| Python | 3.11.2, numpy 2.4.6 |
| ffmpeg | 9.0.2 (the `imageio_ffmpeg` wheel: `/usr/local/lib/python3.11/dist-packages/imageio_ffmpeg/binaries/ffmpeg-linux-x86_64-v9.0.2`) |
| Not available | `ffprobe`, `flac`, `sox`, `clang`, `cppcheck`, `valgrind`, `cmake` — used ffmpeg's decoder output, the library's own info calls and Python/numpy in their place; `check_cmake` SKIPs itself |

### 2.2 Baseline

```
$ cd Clain_FileManager/FileManager_lib && make test
  Results: 589/589 passed, 0 failures
```

### 2.3 What I ran

| Method | Target | Amount | Result |
|---|---|---|---|
| Differential decode vs ffmpeg | WAV (PCM 8/16, A-law, mu-law, IMA, MS ADPCM), FLAC (8/16/24-bit, mono/stereo/6 ch, 8 kHz), Ogg Vorbis, Ogg Opus, MP3 (CBR/VBR, mono/stereo, 8 kHz, 320 kbps) | 28 generated vectors + 12 ID3-tagged copies + 6 extra MP3 (noise CBR/VBR, 10 s VBR) = 46 files, every sample compared | bit-exact for PCM/G.711/ADPCM/FLAC/Vorbis (±1 LSB); MP3 lengths differ by delay+padding (S-01); Opus identical to `libopus` (max 1 LSB) |
| Differential decode, tagged files | ID3v2 in front of FLAC / Ogg-Vorbis / Ogg-Opus / WAV / MP3, with and without footer, 1 000 and 200 000-byte payloads | 12 files × open + full decode | AUD5-01 found (3 formats rejected, Opus misrouted) |
| Fuzzing (in-process, ASan+UBSan) | `Audio_Open` → read → seek → read → close, 1–8 mutations per input | 48 700 executions, 27 runs over 21 distinct inputs (WAV ×4, FLAC ×4, MP3 ×5, Vorbis ×2, tagged ×6; the six ASan runs repeat four of them) | no crash, no leak, no out-of-bounds; 305 `dr_flac` signed-overflow reports (documented, §7) |
| Truncation sweep | every container cut at every byte offset (step 1, then 1 in 7 for the large ones) | 13 files, ≈45 000 cuts | no crash, no over-read: a truncated file never decodes more samples than the whole one |
| Seek self-consistency | `Audio_Seek(t)` then read vs the same offset of a straight decode | 9 files, `t` every 13.7 ms, 2 625 seeks | 0 mismatches for WAV (PCM, IMA, MS ADPCM), FLAC, Vorbis and CBR MP3; the VBR MP3s differ by design and were measured separately against their documented window (§9) |
| DMA playback, simulated I2S | 4 play modes × `musicT`; stall of 0/5/20/50/100 ms before every poll; volume 0…65534; output switch | 3 000–4 000 half-buffers per run, 14 runs | `starts=49 ends=48 errs=0 underruns=0`, PCM byte-identical to the decodes, gap 0 at a track change |
| `SimpleAudio` on simulated time | restart budget, stall sweep, play-mode persistence, volume above 100 % | 3 harnesses, 5 stall values × 400 halves | AUD5-02 found; W1 answered |
| Mutation | the fixes of this round and of V4 | 24 mutations | 16 caught, 8 survived in the default build; 17 caught with `FM_USE_OPUS_LIB` (§8) |
| `make check` (whole) | — | 2 full runs (one in the working tree, one on the fresh extract) | exit 0 both times; 593/593 twice (plain + asan), 32-bit 594/594, ports, FatFS, examples, CubeIDE rules, `audiorestarttest` |
| `make tsan` | suite with `FM_USE_THREADS=1` | 2 runs | 598/598 both times, 0 ThreadSanitizer warnings, exit 0 |
| `make m32`, `make m32 M32=ubsan` | 32-bit `x86-linux-musl` | 2 runs | 594/594 and 594/594, exit 0 |
| `make test FM_USE_OPUS_LIB=1` | the suite with the real `libopus` decoder instead of the stub | 2 runs | 596/596 (patched) and 592/592 (`fc31b25`); mutation §8 #3 is only caught here |
| `make matrix-quick` | 8 IMAGE/VIDEO/AUDIO combinations | 1 run | 8/8 |

### 2.4 What I ran as a 32-bit program

`make m32` and `make m32 M32=ubsan` (the whole suite, 594/594 both times, one
more test than the 64-bit run); the ID3-tag reproducer and the restart-budget
harness are the same code paths and were run through those builds. The size
arithmetic of the tag skip (syncsafe length, `10 + size + 10`) fits in 32 bits
at the maximum a tag can declare (2^28−1), so nothing wrapped there.

### 2.5 Local changes in my clone

Library (the four patches are also delivered as files in `audit_v5/`):

| File | What |
|---|---|
| `src/fm_audio.c` | AUD5-01: `flac_file_open`/`vorbis_file_open`/`opus_file_open`/`audio_parse_wav` take the tag offset; `Audio_Open` remembers where the stream starts behind the tag and reads the Ogg head from there. AUD5-04: a past-the-end MP3 seek parks the stream at the end. |
| `src/fm_media.c` | AUD5-03: `Media_DetectBuffer` skips whole ID3v2 tags (footer, chained) before the magic tests; `File_DetectType` re-reads behind the tag. |
| `top_api/simple_audio.h`, `examples/simple_audio.h` | AUD5-02: `sa.restart_track`; the budget is cleared only when the track changes (identical edit in both copies) |
| `test/audit_v4_tests.h` | tests `AUD5-01`, `AUD5-01b`, `AUD5-03`, `AUD5-04` in `test_audit_v5_audio` |
| `test/main.c` | one line: `RUN_SUITE(test_audit_v5_audio);` |
| `Makefile` | `audiorestarttest` target (AUD5-02 host test), added to `check`, cleaned by `clean` |
| `tools/audio_restart_host_test.c` | new, host-only: two-track playlist, failing track, the budget must end the player (`FM-NOT-FIRMWARE` guarded) |

Harnesses (not part of the library; sources in `audit_v5/tools/`):
`audtool.c` (CLI over the real API), `audtool_opus.c`, `opus_info.c`, `cmp.py`,
`gen.py`, `make_vectors.py`, `fuzzloop.c`, `trunc.py`, `seekself.c`,
`seekread.c`, `seekerr.c`, `detect.c`, `detectbuf.c`, `dmaplay.c`,
`stall_sweep.c`, `underrun_stop.c`, `playmode.c`, `audio_restart_host_test.c`,
and `mut5.py` + `mutspec.json` (the mutation engine of §8).

### 2.6 Self-check of this report

Done, in this order: every patch was applied with `git apply --check` and then
`git apply` to a fresh `git archive fc31b25` extract (a directory outside the
clone, so nothing of the audit's own state leaks in) and the suite ran there —
`Results: 593/593 passed, 0 failures` plus `audiorestarttest` ALL PASS, and
`make check` on that same extract exit 0 with 0 FAIL lines; the same extract
with the test patch **only** gives `589/593`, the four AUD5 tests FAILing with
the messages quoted in §4; the 24 mutations of §8 were then run against that
extract. Every `file:line` below was re-read from
`git show fc31b25:<path>` at the end; every reproducer in §4 was re-run from
the text of the report; every `diff` block in §4 was re-checked against the
file it names (`audit_v5/tools/check_diffs.py`: all four identical, all four
apply to a clean `fc31b25`); the numbers in the Persian summary, the table in
§3, §8, §12 and the index in §14 agree.

---

## 3. Table of findings

| ID | Severity | Status | Part | Where | Title | Fix tested | Test included |
|---|---|---|---|---|---|---|---|
| AUD5-02 | S2 | confirmed | audio / `SimpleAudio` | `top_api/simple_audio.h:1217-1220` (and `examples/simple_audio.h:1218-1221`) | the restart budget is cleared while the same track keeps failing: the player restarts one broken track for ever and the playlist never moves on | yes | yes |
| AUD5-03 | S2 | confirmed | audio / detection | `src/fm_media.c:189-192` | every ID3v2-tagged file is reported as `FM_MEDIA_MP3`, whatever it holds | yes | yes |
| AUD5-01 | S3 | confirmed | audio / `Audio_Open` | `src/fm_audio.c:2591-2613` (+ `513`, `646`, `771`, `1066`, `2617-2649`) | an ID3v2 tag in front of FLAC, Ogg Vorbis, Ogg Opus or WAV is refused (`-17`) or decoded by the wrong decoder | yes | yes |
| AUD5-04 | S3 | confirmed | audio / MP3 seek | `src/fm_audio.c:1805-1808` | `Audio_Seek` past the end of an MP3 says EOF but leaves the stream where it was, so the next read plays from the old position | yes | yes |
| S-01 | — | suspicion | audio / MP3 length | `src/fm_audio.c:1592-1646` | MP3 encoder delay and padding are counted as sound: length +1 836 samples and the first 1 105 samples are priming | no (breaks F8) | no |
| S-02 | — | suspicion | audio / `SimpleAudio` | `top_api/simple_audio.h:1524-1528` (`sa_volume_pct` vs `sa_cfg`) | the volume and play mode `SimpleAudio_Info` reports come from different variables than the ones the DMA plays; measured to agree today | no | no |
| S-03 | — | suspicion | audio / detection | `src/fm_media.c:137-146` | `Media_DetectBuffer` reads a 3-byte "ID3" prefix as a complete tag when the caller passes a short buffer | no | no |

Sorted by severity. S-01…S-03 are in §5, not defects until a reproducer exists.

---

## 4. Confirmed findings

### AUD5-02 — the auto-restart budget is cleared while the same track keeps failing

**Severity** — S2. A file whose reads keep failing restarts for ever: the
playlist never reaches the next track, the sound stutters in a loop of a few
hundred milliseconds, and nothing in the library stops it. No crash, no
corruption.

**Where** — `top_api/simple_audio.h:1217-1220` and the identical
`examples/simple_audio.h:1218-1221`, in `SimpleAudio_Poll`:

```c
1217:    if (r == FM_OK)
1218:    {
1219:        sa.restart_count = 0u;
1220:    }
1221:    return r;
```

The budget itself is at `top_api/simple_audio.h:1191` (`sa.restart_count++`),
`1198` (`if (sa.restart_count <= SIMPLE_AUDIO_MAX_RESTARTS)`), and
`1200` (`audio_dma_play_from(...)`).

**Reach** — `SimpleAudio_Begin` → `SimpleAudio_Poll` (main loop, `FM_Idle`, or
`SimpleAudio_i2s_half`/`_full` depending on the configuration). Any card with a
file that opens but fails to decode part-way — the very case the auto-restart
was added for (the comment at `1176-1191` names "گیرکردن روی آهنگ سوم روی
برد"). Build configurations: all (it is header-only player code); 32 and 64
bits; the firmware path (`top_api/`) and the host path (`examples/`) both have
it. Very likely in ordinary use with one damaged MP3 on a card.

**How to reproduce** — from a clean `fc31b25`:

```bash
cd Clain_FileManager/FileManager_lib
make libfilemanager.a                  # the harness links the library
cp /path/to/audit_v5/tools/audio_restart_host_test.c tools/
# the target is in the patch AUD5-tests.diff; by hand:
gcc -Wall -Wextra -std=gnu11 -Wno-unused-function -DFM_HOST_BUILD=1 \
    -Iinclude -Iexamples -Ibackends -Isrc -Itools/halstub \
    '-DSIMPLE_AUDIO_FOLDER="/aud5r"' \
    tools/audio_restart_host_test.c libfilemanager.a -lm -o fm_audiorestart
./fm_audiorestart
```

The harness builds `/aud5r/a.mp3` from `test/vectors/mp3_m2_22k_32k.mp3`
followed by `test/vectors/tone_mono.mp3` (a 22.05 kHz stream followed by an
8 kHz one, so the read after the change is `FM_ERR_UNSUPPORTED` — this is the
AUD4-02 fix being honest), and `/aud5r/b.wav`. It then raises the two I2S
interrupts by turns from the main loop, as `tools/audio_record_host_test.c`
does. Printed on `fc31b25`:

```
    start /aud5r/a.mp3
    SimpleAudio_Begin -> 0 (OK) folder=/aud5r
  the playlist starts                                        PASS
    end   /aud5r/a.mp3
    start /aud5r/a.mp3
    end   /aud5r/a.mp3
    ...                    (this pair repeats until the half-buffers run out)
    start /aud5r/a.mp3
    halves 20000, starts 77, restarts now 0, running 1, playing /aud5r/a.mp3
  the failing track is given up on (the player stops)        FAIL
  its restarts are bounded by SIMPLE_AUDIO_MAX_RESTARTS      FAIL

FAILURES (2 failures)
```

77 restarts of the same track in 20 000 half-buffers, still running, still on
`a.mp3`: the loop ends only because the harness runs out of half-buffers. With
the fix the same harness prints `halves 1040, starts 4, restarts now 4,
running 0` and both assertions PASS.

**Expected and observed** — expected: after `SIMPLE_AUDIO_MAX_RESTARTS`
(default 3) the player gives up on the track and the playlist moves on, or the
player stops. Observed: the track is started 77 times in 20 000 half-buffers,
the counter printed at the end is 0 (it was cleared by the successful refill
after the last restart), and `Player_Current` never leaves track 0.

**Root cause** — a restart plays the track again from its start, and the first
refills of the restarted track succeed (the failure is further into the file).
Each of those successful polls runs the `if (r == FM_OK)` at the end of
`SimpleAudio_Poll` and zeroes `sa.restart_count`. The counter therefore only
ever measures "failures since the last successful refill", which for a file that
fails once per pass is always 1.

**Proposed fix** (`audit_v5/AUD5-02.diff`, the same edit in both copies):

```diff
diff --git a/examples/simple_audio.h b/examples/simple_audio.h
index 4f0b3c6..3890e60 100644
--- a/examples/simple_audio.h
+++ b/examples/simple_audio.h
@@ -400,6 +400,7 @@ typedef struct
     AudioDma dma;        /* موتور پخش (audio_dma_player.h)          */
 
     uint32_t restart_count;   /* underrun‌های خودکارِ restart    */
+    uint32_t restart_track;   /* the track the budget belongs to */
 
     /* آمار refill (بدون اسپم کنسول) */
     uint32_t refill_count;
@@ -1198,7 +1199,10 @@ FM_Result SimpleAudio_Poll(void)
 
             if (sa.restart_count <= SIMPLE_AUDIO_MAX_RESTARTS)
             {
-                FM_Result r2 = audio_dma_play_from(
+                FM_Result r2;
+
+                sa.restart_track = Player_Current(&sa.dma.player);
+                r2 = audio_dma_play_from(
                     &sa.dma, Player_Current(&sa.dma.player));
 
                 if (r2 == FM_OK)
@@ -1215,8 +1219,14 @@ FM_Result SimpleAudio_Poll(void)
         }
     }
 
-    if (r == FM_OK)
+    if (r == FM_OK && sa.restart_count != 0u &&
+        Player_Current(&sa.dma.player) != sa.restart_track)
     {
+        /* A new track gets a new budget.  Clearing it on every ordinary
+         * success also cleared it while the same track was being
+         * restarted (a restart plays the track from its start, and the
+         * refill only fails later on), so a file whose read kept failing
+         * restarted for ever and the playlist never moved on. */
         sa.restart_count = 0u;
     }
     return r;
diff --git a/top_api/simple_audio.h b/top_api/simple_audio.h
index 5ffcaa5..0d2648c 100644
--- a/top_api/simple_audio.h
+++ b/top_api/simple_audio.h
@@ -399,6 +399,7 @@ typedef struct
     AudioDma dma;        /* موتور پخش (audio_dma_player.h)          */
 
     uint32_t restart_count;   /* underrun‌های خودکارِ restart    */
+    uint32_t restart_track;   /* the track the budget belongs to */
 
     /* آمار refill (بدون اسپم کنسول) */
     uint32_t refill_count;
@@ -1197,7 +1198,10 @@ FM_Result SimpleAudio_Poll(void)
 
             if (sa.restart_count <= SIMPLE_AUDIO_MAX_RESTARTS)
             {
-                FM_Result r2 = audio_dma_play_from(
+                FM_Result r2;
+
+                sa.restart_track = Player_Current(&sa.dma.player);
+                r2 = audio_dma_play_from(
                     &sa.dma, Player_Current(&sa.dma.player));
 
                 if (r2 == FM_OK)
@@ -1214,8 +1218,14 @@ FM_Result SimpleAudio_Poll(void)
         }
     }
 
-    if (r == FM_OK)
+    if (r == FM_OK && sa.restart_count != 0u &&
+        Player_Current(&sa.dma.player) != sa.restart_track)
     {
+        /* A new track gets a new budget.  Clearing it on every ordinary
+         * success also cleared it while the same track was being
+         * restarted (a restart plays the track from its start, and the
+         * refill only fails later on), so a file whose read kept failing
+         * restarted for ever and the playlist never moved on. */
         sa.restart_count = 0u;
     }
     return r;
```

Applied: **yes**. Reproducer after the fix:

```
    halves 1040, starts 4, restarts now 4, running 0, playing /aud5r/a.mp3
  the failing track is given up on (the player stops)        PASS
  its restarts are bounded by SIMPLE_AUDIO_MAX_RESTARTS      PASS

ALL PASS (0 failures)
```

`make test` after the fix: `Results: 593/593 passed, 0 failures`;
`make check` exit 0; `make tsan` 598/598 with 0 warnings; `make m32` and
`make m32 M32=ubsan` 594/594 both times.

**Regression test** — the harness `tools/audio_restart_host_test.c` is added to
the tree as a permanent target (`make audiorestarttest`, part of `make check`),
and its two assertions are exactly the ones above. On `fc31b25` it prints
`FAILURES (2 failures)` and exits 1. It carries the `FM-NOT-FIRMWARE` guard
(`#ifndef USE_HAL_DRIVER`) that `tools/check_cubeide.sh` requires, because
CubeIDE compiles every `.c` under `FileManager_lib/`.

**Siblings** — the two copies of the file are both patched. The same
"counter cleared on success" pattern does not exist in the recorder path
(`simple_audio_rec_*`), and `AudioDma` itself has no retry counter.

**Since** — `c688b9e` (the playback rework) introduced the auto-restart block
with this reset.

### AUD5-03 — an ID3v2 tag makes every file an MP3

**Severity** — S2. `File_DetectType` and `Media_DetectBuffer` return a wrong
type with `FM_MEDIA_MP3` as if it were a confident answer; a caller that routes
by type (`examples_sdcard_media.h:210`, `:618`) sends a FLAC, Ogg or WAV file to
the MP3 path.

**Where** — `src/fm_media.c:189-192` in `Media_DetectBuffer` (`137`):

```c
189:    if (len >= 3 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
190:    {
191:        return FM_MEDIA_MP3;
192:    }
```

and `File_DetectType` (`282-305`) which reads only
`head[FM_MEDIA_MAGIC_SCAN]` (`298`; `FM_MEDIA_MAGIC_SCAN` is 16,
`include/fm_config.h:372`).

**Reach** — `File_DetectType(path)` from any application (the media demo does
it twice per file), `Media_DetectBuffer(buf, len, name)`. Every build with
`FM_USE_MEDIA_DETECT=1` (the default); 32 and 64 bits. As likely as ID3 tagging
is: taggers write ID3v2 into FLAC and WAV, and Ogg files from some phones carry
it.

**How to reproduce** — the `AUD5-03` test does it in the suite; by hand, with a
1000-byte ID3v2.4 tag in front of `test/vectors/test_flac.flac`:

```
$ cd / && /tmp/det_base          # audit_v5/tools/detect.c over the twelve tagged files
  flac_id3      len=80177 detect=13 (BINARY=6 MP3=13 FLAC=15 OGG=16 WAV=12)
```

and the same harness against the patched tree:

```
$ cd / && /tmp/det_fix
  flac_id3      len=80177 detect=15
```

The suite's own output on `fc31b25` (fix removed):

```
  TEST AUD5-03: an ID3v2 tag does not make every file an MP3 ... FAIL: /test/vectors/test_flac.flac tagged (footer 0): buffer 13, file 13, want 15
```

`13` is `FM_MEDIA_MP3`, `15` is `FM_MEDIA_FLAC`. Measured with a small harness
over the twelve tagged files (1 000-byte and 200 000-byte tags, with and
without footer, in front of FLAC, Ogg, WAV and MP3):

| file | before | after |
|---|---|---|
| `flac_id3` (1 000 B tag) | 13 (`MP3`) | 15 (`FLAC`) |
| `ogg_id3` | 13 | 16 (`OGG`) |
| `wav_id3` | 13 | 12 (`WAV`) |
| `mp3_id3` | 13 | 13 |
| `flac_id3_big` (200 000 B tag) | 13 | 15 |
| `wav_id3_big` | 13 | 12 |

**Expected and observed** — expected: the type of the stream behind the tag.
Observed: `FM_MEDIA_MP3` for all four containers, and for the large tags the
magic window (16 bytes) ends inside the tag anyway, so `File_DetectType` cannot
see anything but `ID3`.

**Root cause** — the ID3v2 rule was written for MP3 only and returns
immediately. Nothing parses the tag, and `FM_MEDIA_MAGIC_SCAN` (16) is shorter
than any realistic tag, so the content behind a tag is never looked at.

**Proposed fix** (`audit_v5/AUD5-03.diff`): `Media_DetectBuffer` skips whole
tags in a loop (footer flag, chained tags) before the magic tests, and falls
back to the extension when the buffer ends inside the tag; `File_DetectType`
re-reads behind the tag. Applied: **yes**:

```diff
diff --git a/src/fm_media.c b/src/fm_media.c
index 4484729..3545d03 100644
--- a/src/fm_media.c
+++ b/src/fm_media.c
@@ -150,6 +150,31 @@ FM_MediaType Media_DetectBuffer(const void *buf, size_t len,
     }
 
     /* Magic numbers win over extension. */
+    /* An ID3v2 tag may sit in front of any audio stream, not only an MP3
+     * one: taggers write it into FLAC, Ogg and WAV files too, and the
+     * magic numbers are all behind it.  Every such file was reported as
+     * FM_MEDIA_MP3.  Skip whole tags (a chained file may have several),
+     * then read what the tag was hiding. */
+    while (len >= 10u && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
+    {
+        size_t taglen = 10u + (((size_t)(p[6] & 0x7Fu) << 21) |
+                               ((size_t)(p[7] & 0x7Fu) << 14) |
+                               ((size_t)(p[8] & 0x7Fu) << 7) |
+                               (size_t)(p[9] & 0x7Fu));
+
+        if ((p[5] & 0x10u) != 0u)
+        {
+            taglen += 10u;                   /* the tag has a footer */
+        }
+        if (taglen + 4u > len)
+        {
+            /* the buffer ends inside the tag: nothing to read behind it */
+            return (by_ext != FM_MEDIA_UNKNOWN) ? by_ext : FM_MEDIA_MP3;
+        }
+        p += taglen;
+        len -= taglen;
+    }
+
     if (len >= 2 && p[0] == 0xFFu && p[1] == 0xD8u)
     {
         return FM_MEDIA_JPEG;
@@ -186,10 +211,6 @@ FM_MediaType Media_DetectBuffer(const void *buf, size_t len,
     {
         return FM_MEDIA_WAV;
     }
-    if (len >= 3 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
-    {
-        return FM_MEDIA_MP3;
-    }
     if (len >= 2 && p[0] == 0xFFu && (p[1] & 0xF6u) == 0xF0u)
     {
         return FM_MEDIA_AAC;   /* ADTS sync */
@@ -296,6 +317,31 @@ FM_MediaType File_DetectType(const char *path)
         return Media_DetectByExtension(path);
     }
     r = File_Read(f, head, sizeof(head), &rd32);
+    if ((r == FM_OK || r == FM_ERR_EOF) && rd32 >= 10u &&
+        head[0] == 'I' && head[1] == 'D' && head[2] == '3')
+    {
+        /* An ID3v2 tag in front of the stream: the scan above only
+         * reaches into the tag, so every tagged FLAC, Ogg and WAV file
+         * was reported as MP3.  Read again from behind the tag. */
+        uint64_t skip = 10u + (((uint32_t)(head[6] & 0x7Fu) << 21) |
+                               ((uint32_t)(head[7] & 0x7Fu) << 14) |
+                               ((uint32_t)(head[8] & 0x7Fu) << 7) |
+                               (uint32_t)(head[9] & 0x7Fu));
+
+        if ((head[5] & 0x10u) != 0u)
+        {
+            skip += 10u;                   /* the tag has a footer */
+        }
+        if (File_Seek(f, (int64_t)skip, FM_SEEK_SET) == FM_OK)
+        {
+            rd32 = 0;
+            r = File_Read(f, head, sizeof(head), &rd32);
+        }
+        else
+        {
+            r = FM_ERR_IO;
+        }
+    }
     (void)File_Close(f);
     if (r != FM_OK && r != FM_ERR_EOF)
     {
```

`make test` after the fix:
`Results: 593/593 passed, 0 failures`, `make check` exit 0.

**Regression test** — `AUD5-03` in `test/audit_v4_tests.h`, four containers ×
two footer variants, checking both `Media_DetectBuffer` and `File_DetectType`.
On `fc31b25` it fails (`buffer 13, file 13, want 15`); with the fix it passes.
The first version of the test used footer-less tags only, and deleting the
footer term passed it; the test was then extended to both variants and now both
mutations of that line fail it (§8 #8, #9).

**Siblings** — `Media_DetectByExtension` is unaffected. The video auditor's
container detection has its own magic tests; not pursued.

### AUD5-01 — an ID3v2 tag in front of FLAC, Ogg or WAV is refused

**Severity** — S3. A valid, playable file is rejected with
`FM_ERR_INVALID_FORMAT` (`-17`) — a wrong error code for a file the library
decodes perfectly. The Ogg variant is worse in effect: the file is handed to the
Vorbis decoder and the user sees `-12`.

**Where** — `src/fm_audio.c:2591-2613`, `Audio_Open`:

```c
2591:    if (got >= 10 && memcmp(magic, "ID3", 3) == 0)
...
2604:        if (File_Seek(&a->file, (int64_t)end, FM_SEEK_SET) == FM_OK &&
2605:            File_Read(&a->file, after, sizeof(after), &ag) == FM_OK &&
2606:            ag == 2u && after[0] == 0xFFu && (after[1] & 0xF6u) == 0xF0u)
2607:        {
2608:            adts_at = end;
2609:        }
2610:    }
2611:    r = File_Seek(&a->file, (int64_t)adts_at, FM_SEEK_SET);
```

and the dispatch that then reads from byte 0: `2616-2619` (`RIFF` →
`audio_parse_wav`), `2620-2627` (`fLaC` → `flac_file_open`), `2628-2633` (Ogg
head read from 0, with `File_Seek(...,0)` at `2633`, `OpusHead` searched from
0 at `2635`), `2638` (`opus_file_open`), `2643` (`vorbis_file_open`). The
readers themselves start from byte 0. Three say so outright with
`File_Seek(&a->file, 0, FM_SEEK_SET)` — `513` (`flac_file_open`, seek at `528`),
`646` (`vorbis_file_open`, `659`), `771` (`opus_file_open`, `787`) — and
`1066` (`audio_parse_wav`) reads its 12-byte RIFF header from wherever the file
pointer happens to be, with `pos = 12` (`1071`) as the first chunk offset, i.e.
it assumes that position is the start of the file (it is, because
`Audio_Open` seeks to `adts_at`, which is 0 without a tag).

**Reach** — `Audio_Open` for every caller: `SimpleAudio_Begin` /
`audio_dma_play_from`, `Player_*,` `MediaIO_AudioReader`,
`Audio_Convert`. A tagged file on the card is enough — no special call
sequence. All builds with `FM_USE_FLAC` / `FM_USE_VORBIS` / `FM_USE_OPUS`;
32 and 64 bits; the same on the firmware. Ordinary use: FLAC and WAV files
written by taggers commonly start with `ID3`.

**How to reproduce** — put a 1 000-byte ID3v2.4 tag in front of
`test/vectors/test_flac.flac` (the `AUD5-01` test does exactly this with
`av4_id3()`), then:

```
$ cd / && ./audtool info /tmp/tagged_flac.bin
open: -17 (invalid format)          # fc31b25
```

Full table from `audtool info` (harness `audit_v5/tools/audtool.c`, built
against each tree):

| file | `fc31b25` | with the fix |
|---|---|---|
| `flac_id3` | `open: -17` | `codec=FLAC rate=44100 ch=2 total_frames=220500` |
| `ogg_id3` | `open: -17` | `codec=VORBIS rate=44100 ch=2 total_frames=220500` |
| `wav_id3` | `open: -17` | `codec=WAV rate=44100 ch=1 data_off=144 data_size=176400 total_frames=88200` |
| `flac_id3_big` (200 000 B tag) | `open: -17` | `codec=FLAC … total_frames=220500` |
| `wav_id3_big` | `open: -17` | `codec=WAV … data_off=200054 data_size=176400` |
| `mp3_id3` | `codec=MP3 … total_frames=25344` | unchanged |

and the Ogg Opus case, which is a different symptom — a tagged Opus file is
sent to the Vorbis decoder:

```
$ /tmp/oinfo_base /tmp/opus_plain.ogg      # fc31b25 + libopus
  open 0 codec=OPUS total=48000
$ /tmp/oinfo_base /tmp/opus_id3.ogg
  open -17
```

With the fix both say `open 0 codec=OPUS total=48000`, and the decoded PCM of
the tagged file is byte-identical to the plain file's (`cmp` clean, 96 000
bytes) and to ffmpeg's libopus decode (`maxdiff 1`, `count>2 = 0`).

**Expected and observed** — expected: the tag is metadata in front of the
stream and the stream opens, as it does for MP3 (`mp3_skip_id3`, `1329`) and as
it does for ADTS AAC since AUD4-01. Observed: `-17` for FLAC/Ogg/WAV, `-12` for
Opus behind a tag.

**Root cause** — the V4 fix for AUD4-01 looked behind the tag *only* to test
for an ADTS sync word and, when it found something else, went back to the byte-0
`magic` buffer and dispatched on that. `magic` still says `ID3`, none of the
`RIFF` / `fLaC` / `OggS` tests match, so the file falls through to `mp3_open`,
which skips the tag, finds no MPEG frame and reports "invalid format". The
readers themselves also assume the stream starts at byte 0 (`File_Seek(...,0)`),
so the dispatch alone would not have been enough.

**Proposed fix** (`audit_v5/AUD5-01-only.diff`, 15 hunks, 204 lines; with the
AUD5-04 hunk it is `audit_v5/AUD5-01.diff`, 230 lines): `Audio_Open` keeps the
tag offset in `tag_at`, and the four readers take it as a `base` argument: they
subtract it from `File_Size` (so the size caps stay about the stream), seek to
it before reading, and, for WAV, add it back to `info.data_offset` and to every
chunk offset while making the RIFF-relative arithmetic use the stream length.
The Ogg branch reads `peek` from `tag_at` instead of 0. Applied: **yes**:

```diff
diff --git a/src/fm_audio.c b/src/fm_audio.c
index 57951cd..1c173df 100644
--- a/src/fm_audio.c
+++ b/src/fm_audio.c
@@ -510,7 +510,7 @@ static FM_Result pcmbuf_seek(FM_Audio *a, double seconds) {
 #define AUDIO_SLURP_MAX_BYTES (10u * 1024u * 1024u)
 #endif
 #if FM_USE_FLAC
-static FM_Result flac_file_open(FM_Audio *a) {
+static FM_Result flac_file_open(FM_Audio *a, uint64_t base) {
     uint64_t sz = 0;
     uint8_t *data = NULL;
     uint32_t got = 0;
@@ -522,10 +522,11 @@ static FM_Result flac_file_open(FM_Audio *a) {
     FM_Result r = File_Size(&a->file, &sz);
     if (r != FM_OK) return r;
     if (sz < 42u) return FM_ERR_CORRUPTED;
-    if (sz > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE;  /* valid, just too big */
+    if (sz - base > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE; /* valid, just too big */
+    sz -= base;                                /* an ID3v2 tag in front */
     data = (uint8_t*)FM_Alloc((size_t)sz);
     if (!data) return FM_ERR_NO_MEMORY;
-    (void)File_Seek(&a->file, 0, FM_SEEK_SET);
+    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
     r = File_Read(&a->file, data, (uint32_t)sz, &got);
     if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
     if (FM_Flac_Bits(data, (size_t)sz) > FM_FLAC_MAX_BITS) {
@@ -643,7 +644,7 @@ static FM_Result flac_file_open(FM_Audio *a) {
 }
 #endif
 #if FM_USE_VORBIS
-static FM_Result vorbis_file_open(FM_Audio *a) {
+static FM_Result vorbis_file_open(FM_Audio *a, uint64_t base) {
     uint64_t sz = 0;
     uint8_t *data = NULL;
     uint32_t got = 0;
@@ -653,10 +654,11 @@ static FM_Result vorbis_file_open(FM_Audio *a) {
     FM_Result r = File_Size(&a->file, &sz);
     if (r != FM_OK) return r;
     if (sz < 32u) return FM_ERR_CORRUPTED;
-    if (sz > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE;  /* valid, just too big */
+    if (sz - base > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE; /* valid, just too big */
+    sz -= base;                                /* an ID3v2 tag in front */
     data = (uint8_t*)FM_Alloc((size_t)sz);
     if (!data) return FM_ERR_NO_MEMORY;
-    (void)File_Seek(&a->file, 0, FM_SEEK_SET);
+    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
     r = File_Read(&a->file, data, (uint32_t)sz, &got);
     if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
     v = stb_vorbis_open_memory(data, (int)sz, &error, NULL);
@@ -768,7 +770,7 @@ static int32_t opus_gain_q12(int32_t gain_q8)
  * granulepos of the final page (minus preskip) is the exact total
  * number of 48 kHz samples, so the PCM buffer is sized exactly.
  */
-static FM_Result opus_file_open(FM_Audio *a) {
+static FM_Result opus_file_open(FM_Audio *a, uint64_t base) {
     uint64_t sz = 0;
     uint8_t *data = NULL;
     uint32_t got = 0;
@@ -781,10 +783,11 @@ static FM_Result opus_file_open(FM_Audio *a) {
     r = File_Size(&a->file, &sz);
     if (r != FM_OK) return r;
     if (sz < 27u) return FM_ERR_CORRUPTED;
-    if (sz > FM_OPUS_MAX_FILE_BYTES) return FM_ERR_TOO_LARGE;
+    if (sz - base > FM_OPUS_MAX_FILE_BYTES) return FM_ERR_TOO_LARGE;
+    sz -= base;                            /* an ID3v2 tag in front */
     data = (uint8_t*)FM_Alloc((size_t)sz);
     if (!data) return FM_ERR_NO_MEMORY;
-    (void)File_Seek(&a->file, 0, FM_SEEK_SET);
+    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
     r = File_Read(&a->file, data, (uint32_t)sz, &got);
     if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
     if (memcmp(data, "OggS", 4) != 0) { FM_Free(data); return FM_ERR_INVALID_FORMAT; }
@@ -1063,7 +1066,7 @@ done:
 }
 #endif
 
-static FM_Result audio_parse_wav(FM_Audio *a)
+static FM_Result audio_parse_wav(FM_Audio *a, uint64_t base)
 {
     uint8_t riff[12];
     FM_Bool have_fmt = FM_FALSE;
@@ -1071,6 +1074,10 @@ static FM_Result audio_parse_wav(FM_Audio *a)
     uint64_t pos = 12;
     uint64_t file_size = 0;
 
+    if (base != 0u && File_Seek(&a->file, (int64_t)base, FM_SEEK_SET) != FM_OK)
+    {
+        return FM_ERR_IO;
+    }
     if (file_read_exact(&a->file, riff, sizeof(riff), NULL) != FM_OK ||
         memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
     {
@@ -1080,6 +1087,10 @@ static FM_Result audio_parse_wav(FM_Audio *a)
     {
         return FM_ERR_IO;
     }
+    /* Chunk offsets and the end of the data are relative to the RIFF
+     * header, which sits behind an ID3v2 tag when there is one. */
+    file_size -= base;
+    pos = 12u;
 
     while (pos + 8 <= file_size)
     {
@@ -1162,14 +1173,14 @@ static FM_Result audio_parse_wav(FM_Audio *a)
                 FM_EVENT_LOG("wav: data length 0, %lu bytes follow: played to the end of the file",
                              (unsigned long)chunk_size);
             }
-            a->info.data_offset = payload;
+            a->info.data_offset = base + payload;
             a->info.data_size = chunk_size;
             have_data = FM_TRUE;
         }
 
         if (next > payload)
         {
-            if (File_Seek(&a->file, (int64_t)next, FM_SEEK_SET) != FM_OK)
+            if (File_Seek(&a->file, (int64_t)(base + next), FM_SEEK_SET) != FM_OK)
             {
                 return FM_ERR_IO;
             }
@@ -2571,6 +2601,7 @@ FM_Result Audio_Open(const char *path, FM_Audio *a)
     uint8_t magic[12];
     uint32_t got = 0;
     uint64_t adts_at = 0;       /* ADTS frames behind an ID3v2 tag */
+    uint64_t tag_at = 0;        /* audio stream behind an ID3v2 tag */
     FM_Result r;
 
     FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
@@ -2590,8 +2621,6 @@ FM_Result Audio_Open(const char *path, FM_Audio *a)
      * format".  Look behind the tag first. */
     if (got >= 10 && memcmp(magic, "ID3", 3) == 0)
     {
-        uint8_t after[2];
-        uint32_t ag = 0;
         uint64_t end = 10u + (((uint32_t)(magic[6] & 0x7Fu) << 21) |
                               ((uint32_t)(magic[7] & 0x7Fu) << 14) |
                               ((uint32_t)(magic[8] & 0x7Fu) << 7) |
@@ -2601,11 +2630,24 @@ FM_Result Audio_Open(const char *path, FM_Audio *a)
         {
             end += 10u;                         /* the tag has a footer */
         }
+        /* An ID3v2 tag is allowed in front of any stream, not only ADTS:
+         * FLAC, Ogg and WAV files tagged on the card were read from byte
+         * 0, none of the magic tests below matched, and the MP3 parser
+         * called the track "invalid format". */
         if (File_Seek(&a->file, (int64_t)end, FM_SEEK_SET) == FM_OK &&
-            File_Read(&a->file, after, sizeof(after), &ag) == FM_OK &&
-            ag == 2u && after[0] == 0xFFu && (after[1] & 0xF6u) == 0xF0u)
+            File_Read(&a->file, magic, sizeof(magic), &got) == FM_OK &&
+            got >= 4u)
+        {
+            tag_at = end;
+            if (got >= 2u && magic[0] == 0xFFu && (magic[1] & 0xF6u) == 0xF0u)
+            {
+                adts_at = end;
+            }
+        }
+        else
         {
-            adts_at = end;
+            (void)File_Seek(&a->file, 0, FM_SEEK_SET);
+            (void)File_Read(&a->file, magic, sizeof(magic), &got);
         }
     }
     r = File_Seek(&a->file, (int64_t)adts_at, FM_SEEK_SET);
@@ -2615,12 +2657,12 @@ FM_Result Audio_Open(const char *path, FM_Audio *a)
     }
     else if (r == FM_OK && got >= 4 && memcmp(magic, "RIFF", 4) == 0)
     {
-        r = audio_parse_wav(a);
+        r = audio_parse_wav(a, tag_at);
     }
     else if (r == FM_OK && got >= 4 && memcmp(magic, "fLaC", 4) == 0)
     {
 #if FM_USE_FLAC
-        r = flac_file_open(a);
+        r = flac_file_open(a, tag_at);
 #else
         r = FM_ERR_UNSUPPORTED;
 #endif
@@ -2629,19 +2671,20 @@ FM_Result Audio_Open(const char *path, FM_Audio *a)
     {
         uint8_t peek[256];
         uint32_t pg = 0;
+        (void)File_Seek(&a->file, (int64_t)tag_at, FM_SEEK_SET);
         (void)File_Read(&a->file, peek, sizeof(peek), &pg);
         (void)File_Seek(&a->file, 0, FM_SEEK_SET);
         int is_opus = 0;
         for (uint32_t i=0;i+8<pg;++i) if (memcmp(peek+i, "OpusHead",8)==0) { is_opus=1; break; }
         if (is_opus) {
 #if FM_USE_OPUS
-            r = opus_file_open(a);
+            r = opus_file_open(a, tag_at);
 #else
             r = FM_ERR_UNSUPPORTED;
 #endif
         } else {
 #if FM_USE_VORBIS
-            r = vorbis_file_open(a);
+            r = vorbis_file_open(a, tag_at);
 #else
             r = FM_ERR_UNSUPPORTED;
 #endif
```

On `fc31b25` the two new tests say:

```
  TEST AUD5-01: FLAC / Ogg / WAV behind an ID3v2 tag open and play ... FAIL: /test/vectors/test_flac.flac tagged: open -17, read -17
  TEST AUD5-01b: Ogg Opus behind an ID3v2 tag opens like the plain file ... FAIL: plain -11/0 samples, tagged -17/0
```

with the fixes both PASS. (`audit_v5/AUD5-01.diff` is this block plus the
`AUD5-04` block of §4; applying `AUD5-01-only.diff` and then `AUD5-04-only.diff`
gives the same tree, checked with `git apply --check` on a clean `fc31b25`.)

After the fix: `make test` `Results: 593/593 passed, 0 failures`; `make check`
exit 0; `make tsan` 598/598, 0 warnings; `make m32` / `m32 M32=ubsan`
594/594 and 594/594; the suite with the real Opus decoder
(`make test FM_USE_OPUS_LIB=1`) 596/596.

**Regression test** — `AUD5-01` (three formats, tagged copy vs plain copy: same
sample count, same length, `FM_ERR_EOF` at the end) and `AUD5-01b` (Ogg Opus,
tagged vs plain, plus a byte comparison of the decoded PCM) in
`test/audit_v4_tests.h`. On `fc31b25` both FAIL with the two lines above; with
the fix both PASS. `AUD5-01b` is skipped when the build has no Opus decoder, so
it only guards the `FM_USE_OPUS_LIB` variant — mutation §8 #3 shows the stub
Opus cannot see the offset at all.

**Siblings** — the four readers are all patched (`flac_file_open`,
`vorbis_file_open`, `opus_file_open`, `audio_parse_wav`). `mp3_open` already
skipped tags (`mp3_skip_id3`, `1329`) and is untouched. The ADTS branch still
works (`AUD4-01` tests pass). Not patched: a tag appended at the *end* of a
file, which is ignored by all readers and costs nothing (a trailing ID3v1 or
APE tag does not change the decoded length: measured 889 344 → 884 736 bytes
for both, i.e. the same as ffmpeg).

**Since** — `fc31b25` (the AUD4-01 fix) introduced the "look behind, then read
from 0" structure; the four readers have assumed byte 0 since `8dd0f25`.

### AUD5-04 — a seek past the end of an MP3 leaves the stream at the old position

**Severity** — S3. `Audio_Seek` reports `FM_ERR_EOF` (which the README allows)
but does not move the stream and does not update `frame_index`; a caller that
goes on reading gets the sound from where the seek was asked, not the end of
the track. `MediaIO_AudioSeek` and `Player_Seek` return the code to the
application, but any caller that treats "the end" as a normal outcome (the
board's UART seek path does: `SimpleAudio` calls `audio_dma_handle_eof` and
plays on) continues in an inconsistent state.

**Where** — `src/fm_audio.c:1805-1808`, `mp3_seek`:

```c
1805:    if (duration_frames != 0u && target_frames >= duration_frames)
1806:    {
1807:        return FM_ERR_EOF;
1808:    }
```

Compare `Audio_Seek` for WAV (`2953`, clamp at `3026-3028`): a time past the end
is clamped to `data_offset + data_size`, the next read says `FM_ERR_EOF`, and `frame_index`
is the length. README, "Seek" (`README.md:415`): *"a time past the end is the
end"*.

**Reach** — `Audio_Seek` on any MP3 with a known length (Xing/Info header, or a
CBR estimate): `MediaIO_AudioSeek`, `Player_Seek`, `Player_SeekFwd`,
`SimpleAudio_SeekTo` / `SeekBy` (the UART `>` command past the end).
`FM_USE_MP3=1`, 32 and 64 bits.

**How to reproduce** — harness `audit_v5/tools/seekread.c`:

```
$ /tmp/seekread files/m_cbr128.mp3
read1 0 got 64 fi 32
seek(1e9) -> -15 fi 32
read after failed seek 0 got 64 fi 64        <-- sound, not the end
$ /tmp/seekread files/a_st16.wav
read1 0 got 64 fi 32
seek(1e9) -> 0 fi 88200
read after failed seek -15 got 0 fi 88200    <-- the end, as documented
```

The suite's `AUD5-04` test reports the same:
`the read after it said 0, 512 samples (want FM_ERR_EOF, 0)`.

**Expected and observed** — expected: the stream is at the end afterwards for
every format (`FM_ERR_EOF`, `frame_index == total_frames`). Observed: MP3 stays
where it was and keeps playing.

**Root cause** — `mp3_seek` returns early before it touches the decoder state,
the file offset or `frame_index`; the other formats clamp first and return
success.

**Proposed fix** (`audit_v5/AUD5-04-only.diff`; it is also the last hunk of
`audit_v5/AUD5-01.diff`): on that path, reset
the decode state, seek to `file_size`, set `frame_index` to the end and
`total_samples`, then return `FM_ERR_EOF` — keeping the code the existing
`MP3: Audio_Seek lands on the requested second` test expects
(`test/main.c:2066`, `Audio_Seek(&a, 10.0) == FM_ERR_EOF`):

```diff
diff --git a/src/fm_audio.c b/src/fm_audio.c
index 57951cd..1c173df 100644
--- a/src/fm_audio.c
+++ b/src/fm_audio.c
@@ -1804,6 +1815,25 @@ static FM_Result mp3_seek(FM_Audio *a, double seconds)
     duration_frames = mp3_duration_frames(m);
     if (duration_frames != 0u && target_frames >= duration_frames)
     {
+        /* Past the end: say so (as this did) and also park the stream
+         * after the last frame, as every other format does here.  A
+         * caller that goes on reading after the error (SimpleAudio
+         * does not, Player_Seek callers may) used to get the sound
+         * from where the seek was asked, not the end of the track. */
+        mp3dec_init(&m->dec);
+        m->in_len = 0;
+        m->in_off = 0;
+        m->pcm_len = 0;
+        m->pcm_pos = 0;
+        m->primed_frames = 0u;
+        m->in_file_off = m->file_size;
+        if (m->file_size != 0u &&
+            File_Seek(m->file, (int64_t)m->file_size, FM_SEEK_SET) != FM_OK)
+        {
+            return FM_ERR_IO;
+        }
+        m->total_samples = duration_frames * (uint64_t)a->info.channels;
+        a->frame_index = duration_frames;
         return FM_ERR_EOF;
     }
 
```

Applied: **yes** — `Results: 593/593` after the fix, and the four-format
comparison of `seekread` now ends at the end of the track for MP3 as well
(`seek(1e9) -> -15 fi 222336`, `read after failed seek -15 got 0 fi 222336`).
The length of an MP3 (`total_frames`) is *not* touched by this fix; the
delay/padding question is S-01 in §5.

**Regression test** — `AUD5-04` in `test/audit_v4_tests.h`: read, seek to 1e9
(must be `FM_ERR_EOF`), read again (must be `FM_ERR_EOF` with 0 samples),
`frame_index` must equal `total_frames`. On `fc31b25` it fails
(`the read after it said 0, 512 samples (want FM_ERR_EOF, 0)`); with the fix it
passes, and mutation §8 #24 (the park deleted) is caught by exactly this test.

**Siblings** — `aac_seek`, `pcmbuf_seek` (FLAC/Vorbis/Opus) and the WAV path
all clamp to the end already; checked by `seekread` on `.wav`, `.flac`, `.ogg`
(`seek 1e9 -> 0 frame_index=<length>`).

**Since** — `a6cf829` (the seek rewrite), unchanged since.

---

## 5. Suspicions

### S-01 — MP3 encoder delay and padding are counted as sound

**What I believe.** A standalone MP3 file played through `Audio_Open` /
`Audio_Read` starts with 1 105 samples (LAME) of encoder priming and ends with
the padding the encoder added, and `FM_AudioInfo.total_frames` counts both.
ffmpeg removes both, using the LAME/Lavf tag, so the lengths and the start of the
sound differ.

**Why.** Measured, every sample compared (`audit_v5/tools/cmp.py`):

| file | library | ffmpeg | difference | LAME tag says |
|---|---|---|---|---|
| `m_cbr128.mp3` (5 s, 44.1 kHz stereo) | 444 672 | 441 000 | +3 672 | delay 576, pad 1 260 |
| `m_vbr.mp3` | 444 672 | 441 000 | +3 672 | 576 / 1 260 |
| `n_vbr.mp3` (mono) | 266 112 | 264 600 | +1 512 | 576 / 936 |
| `m_mono.mp3` | 67 392 | 66 150 | +1 242 | 576 / 666 |
| `m_8k.mp3` | 25 344 | 24 000 | +1 344 | 576 / 768 |
| `p44.mp3` | 92 160 | 89 950 | +2 210 | — |

(ffmpeg's byte counts: `m_cbr128.mp3` 882 000, `m_vbr.mp3` 882 000, `n_vbr.mp3`
529 200, `m_mono.mp3` 132 300, `m_8k.mp3` 48 000, `p44.mp3` 179 900 — divided by
2 bytes and by the channel count these are the figures above; the library's
`total_frames` is what `audtool info` prints.)

After the leading priming the samples match ffmpeg within 1 LSB, so this is not
a decoder error: it is length and alignment. README "Encoder delay and padding"
says the delay is removed where the *container* records it (MP4 edit lists,
Matroska `CodecDelay`) and never claims it is removed for a standalone MP3; the
"Length and position" paragraph claims the length is *exact* for MP3 with a Xing
header, which is the part that reads as a promise the code does not keep.

**What stopped me.** The obvious fix — parse `LAME`/`Lavf` at `mp3_try_xing`,
subtract `delay` from the first PCM and `pad` from the total — works on all
seven lengths above (they become exactly ffmpeg's) but it over-trims the
alignment (measured lag 529…1 058 samples, i.e. more than the 576 the tag
declares) and it changes the length of `test/vectors/mp3_info_44k.mp3`, which
breaks the existing test F8 (`test/audit_v2_tests.h:2274` expects 46 080 /
44 100 s):

```
  TEST F8: … mp3_info_44k.mp3: 1.000 s reported, 1.000 s decoded
  Results: 590/591 passed, 1 failures
```

A correct fix needs the real delay for the files in `test/vectors/` measured
first (the tag in `mp3_info_44k.mp3` says 576 / 1 404 and that file has 40
frames, so the arithmetic and the tag disagree) — that is a maintainer's
decision, not an auditor's.

**The experiment that would settle it.** Decode each of the seven files with
ffmpeg and with the library, cross-correlate the first 4 000 samples of the
noise file (`n_vbr.mp3`) to get the true delay per encoder setting, then decide
whether the tag or the correlation is right; then change `total_frames` and the
first-frame trim together and update F8 from the measured value.

### S-02 — the reported volume and the played volume live in two places

**What I believe.** `SimpleAudio_SetVolume` / `SetOutput` / `SetPlayMode` write
`simple_audio_cfg()` *and* the live `sa.dma`. `SimpleAudio_Stop()` →
`SimpleAudio_Begin()` memsets `sa` and re-applies the config, so the settings
survive — measured: `SetPlayMode(NEXT)` before `Begin` gives
`dma.play_mode=1` and `SimpleAudio_GetPlayMode()==1`, before and after a
`Stop`/`Begin` cycle. What I did not settle is the *volume* copy: `sa_volume_pct`
is a separate variable and `SimpleAudio_SetVolume` writes the gain *and* the
percentage, so a `StartConfig` that clamps could leave the percentage and the
gain disagreeing after a failure path.

**What I measured.** `SimpleAudio_SetVolume` writes both places from the same
clamped value (`top_api/simple_audio.h:1479-1480`: `sa_volume_pct = percent;`
and `simple_audio_cfg()->volume = gain;`), and `SimpleAudio_Info` reads
`sa_volume_pct` (`1524`) while the DMA plays `simple_audio_cfg()->volume`, so
the two cannot disagree through that call. Play mode and output are read
straight from `simple_audio_cfg()` (`1527-1528`), the same struct the next
`StartConfig` uses. I found no sequence that produces a disagreement.

**The experiment.** Set volume 200 %, force `AudioDma_StartConfig` to fail
(an empty folder), then `SetVolume(50)` and `Begin` again; compare
`SimpleAudio_GetVolume()` with the gain the first refill applies.

### S-03 — a 3-byte buffer that starts with "ID3" is reported as a whole tag

**What I believe.** `Media_DetectBuffer(buf, 3, ...)` with `buf == "ID3"` returns
`FM_MEDIA_MP3` (line `189`) because the rule only needs 3 bytes, while the tag
length it means to read is at bytes 6…9. The new whole-tag skipper in
`Media_DetectBuffer` needs `len >= 10` and is therefore correct, but the rule
behind it is still a 3-byte rule: any buffer shorter than the tag cannot be
recognised as "tagged audio".

**Measured.** `Media_DetectBuffer("ID3", 3, "x.flac")` → `15`
(`FM_MEDIA_MP3`), and even a 10-byte prefix of a 64-byte tag →
`15`, because the buffer ends inside the tag.

**What stopped me.** It is hard to name a caller that passes 3 bytes and cares;
the class of defect is real (a truncated buffer answered confidently) but the
severity is below the reporting bar for a confirmed finding.

**The experiment.** Feed every length 0…15 of a tagged FLAC to
`Media_DetectBuffer` and list what comes back; the answer should be
`FM_MEDIA_UNKNOWN` (or the extension) until the magic behind the tag is inside
the buffer.

---

## 6. Verdict on the earlier findings

`git diff 48a00bc..fc31b25` touches `src/fm_audio.c` and `src/fm_flac.c`; the
AUD4 fixes are in `Audio_Open`'s dispatch and `mp3_decode_one` /
`aac_decode_one`. Each verdict below includes removing the fix locally and
running the test named for it.

| Earlier ID | Verdict | How I checked | Test fails without the fix? |
|---|---|---|---|
| AUD4-01 (ADTS behind ID3) | fixed | 40 ADTS frames behind a 8 192-byte tag, with and without footer: `codec=AAC 44100/1`, 40 frames, `FM_ERR_EOF`; `tagged_*_past` (a tag whose length runs past the end) is refused | yes (`AUD4-01 …` tests; I also reverted the `adts_at` assignment: §8 #11, which **survived** — that branch is redundant, not untested) |
| AUD4-02 (MP3 form change) | fixed | joined 22.05/44.1 kHz and mono/stereo files: rate change ends the track with `FM_ERR_UNSUPPORTED` and keeps saying so; channel change plays on converted (mono→both, stereo→mean), ffmpeg's decode of the joined file differs from ours only where ffmpeg itself switches channel count | yes (`AUD4-02 …`) |
| AUD4-03 (`aac_find_sync` byte by byte) | fixed | 2 MiB junk tail: call count 1 069 (was 197 284); wall time milliseconds | yes (`AUD4-03 …`) |
| SUSP-A (ADTS form change) | fixed | a frame of another form followed by a second header of that form ends the track with `FM_ERR_UNSUPPORTED`; a lone one is skipped | yes (`AUD4 SUSP-A …`) |
| WAV `data` length 0 | fixed | `data` size 0 followed by `LIST`: plays to the end; `0xFFFFFFFF` clamps the same way | yes (`WAV: a data length of 0 …`) |
| F2 (MP3 resync speed) | fixed | 2 MiB of junk behind one frame opens in milliseconds | **no test** — the suite has no timing oracle |
| F9 (seek NaN / huge) | fixed | NaN and −1 → `-1`; 1e9, 1e30, INF → `FM_ERR_EOF` on MP3 (state now consistent, AUD5-04) and clamp-to-end on WAV/FLAC/Ogg | yes (`F9 …`) |
| F10 (A-law polarity) | fixed | byte-identical to ffmpeg on `a_alaw.wav` | yes (`test_wav_alaw`) |
| F11/F12/F15 (mono/stereo counts) | fixed | `ReadMono` on stereo, counts written on every path, sub-frame reads refused | yes |
| F13/F25 (example DMA player) | fixed | the two copies differ only in `#include` lines; `fm_audiorestart` drives the real one | partial — the host test compiles `examples/`, not `top_api/` (§8 #23) |
| F20/F29/PCM 24-bit | documented | README §"Audio rules and limits" states them | n/a |
| F21/F22/F27 (Xing frame played, seek) | fixed | the Xing frame is not played (sample counts match ffmpeg after priming, S-01); CBR seek exact to the sample | yes |
| F28 (allocator routing) | fixed | ASan/LeakSanitizer clean over 48 700 fuzz executions and 45 000 truncation cuts | no dedicated test |
| F30 (UB in vendored decoders) | fixed in stb_vorbis | 305 `dr_flac` overflow reports remain, all documented (`-fwrapv`) | **no test** |

Nothing regressed: the whole suite, ASan+UBSan, TSan, 32-bit and
`matrix-quick` are green at `fc31b25` and after the four fixes.

## 7. Answers to the open leads

| Lead | Answer | How I checked |
|---|---|---|
| **W1** stutter regression | **Held.** A main loop blocked 0/5/20/50/100 ms before every poll produced identical results: `halves=3000 running=1 starts=37 ends=36 errs=0 underruns=0 captured=6115072` at every stall. With the default 32 766-sample half buffer the half period is 371.5 ms, so a 200 ms block is inside budget; with a 1 024-sample half (6.0 ms period) the same sweep was also clean, because the harness raises the interrupts from the poll that does the refill. The one real behaviour: an underrun is **not** recovered — `AudioDma_Poll` stops the I2S and returns `FM_ERR_TIMEOUT` (`top_api/audio_dma_player.h:1037-1042`); measured `Poll=-7 running=0 underrun=1` when two halves are asked for before one poll. That is the documented policy (the simple player's auto-restart picks it up), not a defect. | `audit_v5/tools/stall_sweep.c`, `underrun_stop.c` |
| **W2** sample-exact playback | **Sound.** `musicT` (MP3 + WAV) through `AudioDma` at volume 32767, monitor tap captured: `starts=49 ends=48 errs=0 underruns=0` over 4 000 halves; the MP3 block is 444 672 samples with `maxdiff 0` against `Audio_Read`, the two WAV tracks 8 000 each with `maxdiff 0`; the gap between the MP3 and the first WAV is 0 samples. | `audit_v5/tools/dmaplay.c` |
| **W3** output-switch tests | Two gaps: (a) setting `output=SPEAKER` is covered, but the *silence* in `CALLBACK` is only checked as "the callback got the samples"; (b) the host test builds `examples/simple_audio.h`, so a mutation in `top_api/simple_audio.h` is invisible (§8 #23). Removing the silence and calling a callback twice were both caught by `make audiorecordtest` (99 checks) and `make tapstest`. | mutation (§8), `make check` |
| **W4** volume above 100 | **Correct, saturating.** Gain 16 383/32 767 = 0.5000 and 65 534/32 767 = 2.0000 exactly; a 0.25-full-scale signal at 200 % peaks at 16 383 (×2) and a full-scale one peaks at 32 765 without wrap. Gain is clamped to 65 534 in `AudioDma_SetVolume` and again in `StartConfig`. | `audit_v5/tools/dmaplay.c`, `fm_test` |
| **W5** remaining leads | One new defect (AUD5-04) and two closed: VBRI is not parsed and a file with one is treated as CBR (its duration is then an estimate — matches the documented "VBR without Xing" limit); a 14-byte `fmt ` chunk is accepted and byte-identical to ffmpeg; the ADPCM allocation is bounded by `FM_AUDIO_PCM_CHUNK`, not by `block_align`; unregistering a decoder with a stream open is refused (`FM_ERR_BUSY`), the open handle keeps its ops pointer. VBR MP3 seek accuracy against the documented window ("the sound starts up to about 0.15 s after the request and `frame_index` can be some 50 ms off"): on a noise VBR file, where cross-correlation is reliable (the repository's two VBR vectors are tonal and cannot be aligned sample-by-sample), the sound starts 0.091…0.158 s after the request and the reported `frame_index` sits 9…53 ms behind the sound in six of the seven requests and 14 ms ahead in the seventh (`seekerr`, requests at 0.5…6.5 s). The worst case of both figures is a little outside the note (0.158 s, 53 ms) — the behaviour is the documented kind of estimate, so this is an optimistic sentence in the README, not a defect; CBR MP3 is exact to the sample (4/4 requests, 0.000 s). The Opus first-packet question from the last round **closed as not a defect**: against ffmpeg's *native* Opus decoder the first 16 954 samples differ (SNR 18.5 dB) but against `libopus` (`ffmpeg -c:a libopus`) the whole file is identical (±1 LSB), i.e. the difference was the reference decoder, not the library. | `audtool_fix7`, `opus_info.c`, `cmp.py` |

## 8. Tests that would not notice

24 mutations of the **fully patched** tree. Each was applied to its own copy of
`git archive fc31b25` + the four patches, rebuilt (`make -j libfilemanager.a`)
and run through `make test`; the three `SimpleAudio` ones also through
`make audiorestarttest`. The engine was `audit_v5/tools/mut5.py` driven by
`audit_v5/tools/mutspec.json` (24 entries, exact anchors): a mutation whose
anchor text is not present is reported as `SITE NOT FOUND` instead of silently
doing nothing. The line numbers in this table are of the **patched** tree (the
one the fix will produce), not of `fc31b25`; everywhere else in this report a
line number refers to `fc31b25`.

| # | File : changed | Expected to fail | Observed |
|---|---|---|---|
| 1 | `src/fm_audio.c:526` `sz -= base;` in `flac_file_open` → deleted | AUD5-01 | **caught** — `592/593`, `AUD5-01 … FAIL: /test/vectors/test_vorbis.ogg tagged: open -6, read -6` |
| 2 | `src/fm_audio.c:658` same line in `vorbis_file_open` | AUD5-01 | **caught** — same message (the FLAC case is caught first, the Vorbis one by the same assertion) |
| 3 | `src/fm_audio.c:2681` `opus_file_open(a, tag_at)` → `(a, 0u)` | AUD5-01b | **survived in the default build** (stub Opus ignores the offset) — **caught with `FM_USE_OPUS_LIB=1`**: `595/596`, `tagged -17/0` |
| 4 | `src/fm_audio.c:2674` Ogg head read from `tag_at` → from `0` | AUD5-01b | **caught** — `AUD5-01b … FAIL: plain -11/0 samples, tagged -12/0` |
| 5 | `src/fm_audio.c:2687` `vorbis_file_open(a, tag_at)` → `(a, 0u)` | AUD5-01 | **caught** — `tagged: open -12, read -12` |
| 6 | `src/fm_audio.c:2660` `audio_parse_wav(a, tag_at)` → `(a, 0u)` | AUD5-01 | **caught** — `wav_alaw_8k.wav tagged: open -17, read -17` |
| 7 | `src/fm_media.c:158` the whole-tag `while` → `while (0)` | AUD5-03 | **caught** — `tagged (footer 0): buffer 6, file 15, want 15` |
| 8 | `src/fm_media.c:165` `taglen += 10u` (footer) deleted | AUD5-03 | **caught** — `tagged (footer 1): buffer 6, file 15, want 15` |
| 9 | `src/fm_media.c:331` `skip += 10u` (footer) deleted | AUD5-03 | **caught** — `tagged (footer 1): buffer 15, file 6, want 15` |
| 10 | `src/fm_media.c:335` the re-read behind the tag → `if (0)` | AUD5-03 | **caught** — `tagged (footer 0): buffer 15, file 6, want 15` |
| 11 | `src/fm_audio.c:2644` `adts_at = end;` → `adts_at = adts_at;` | AUD4-01 | **survived** — the ADTS sync test reads `magic` from behind the tag now, so the `0xFF 0xFx` branch at `2693-2696` finds AAC anyway; this special case is dead code the fix made redundant |
| 12 | `src/fm_audio.c:2637` the "read behind the tag" `if` → `if (0)` | AUD4-01 + AUD5-01 | **caught** — `590/593`, `AUD4-01 … FAIL: footer 0: result -17, codec '' 0 Hz 0 ch, 0 frames`, `AUD5-01 … FAIL: open -17, read -17` |
| 13 | `src/fm_audio.c:503` `if (frame > frames) frame = frames;` → `if (0)` | a past-the-end `Audio_Seek` on a decoded-in-memory stream | **survived** — the narrowing is `(size_t)frame * channels`, and on this 64-bit host nothing wraps; the comment's 32-bit case is not reached by any test |
| 14 | `src/fm_audio.c:847` `if (nseg == 0) break;` → `continue` | — | **survived** — a page with no lacing values still terminates through the `page_end <= off` guard below it |
| 15 | `src/fm_audio.c:853` `granule_final = max(...)` → `= gran` | Opus length | **survived** — every test Opus file has a single audio page |
| 16 | `src/fm_audio.c:2496` `if (out_samples > cap)` → `if (0)` | F5 | **caught** — `F5 … FAIL: 8192 samples delivered (result 0)` |
| 17 | `src/fm_audio.c:487` `n -= n % channels;` deleted | whole-frame reads | **survived** — every caller in the suite passes a frame-aligned count |
| 18 | `src/fm_audio.c:1161` the WAV `chunk_size == 0` branch → `if (0)` | the data-length-0 test | **caught** — `FAIL: result -15, 0 frames in the info, 0 read` |
| 19 | `src/fm_audio.c:1794` `num = …;` → `num = 0;` in `mp3_cbr_position` | MP3 seek test | **caught** — `589/593`, `MP3: Audio_Seek lands on the requested second … FAIL: err=-26` plus three more |
| 20 | `src/fm_audio.c:1485` the `open_hz` check → `if (0)` | AUD4-02 | **caught** — `AUD4-02 … FAIL: result -15 then -15, 48000 Hz, 26496 samples` |
| 21 | `examples/simple_audio.h:1222` reset condition → `if (r == FM_OK)` | `audiorestarttest` | **caught** — `make test` still 593/593, but `audiorestarttest` FAILs both assertions (`the failing track is given up on`, `its restarts are bounded`) |
| 22 | `examples/simple_audio.h:1204` `sa.restart_track = …` deleted | `audiorestarttest` | **survived** — without the assignment the field keeps the value from the previous track, which is enough for this playlist |
| 23 | `top_api/simple_audio.h:1221` reset condition → `if (r == FM_OK)` | `audiorestarttest` | **survived** — the host test compiles `examples/`, never `top_api/` |
| 24 | `src/fm_audio.c:1823` the past-the-end MP3 park deleted | AUD5-04 | **caught** — `AUD5-04 … FAIL: the read after it said 0, 512 samples (want FM_ERR_EOF, 0)` |

Ratio: **16 of 24 caught (67 %)** in the build `make test` gives, 8 survivors;
**17 of 24 (71 %)** if `FM_USE_OPUS_LIB` is counted too, because that build is
where #3 is caught.

What the survivors say, and why they matter more than the ratio:

- **#21 vs #23** — the same defect, the same fix, two copies: the `examples/`
  copy is covered by a host test, the `top_api/` copy is covered by nothing.
  A whole firmware-side player can regress invisibly. This is the single
  biggest test hole in this part (also W3 (b), §13 #2).
- **#3** — the stub Opus decoder ignores the file offset, so the `tag_at`
  argument of `opus_file_open` can be dropped with the whole suite green. Only
  the `FM_USE_OPUS_LIB` build (which `make check` adds) notices: 595/596 there,
  `tagged -17/0`.
- **#11** — the ADTS special case in `Audio_Open` is now redundant: after
  AUD5-01 the magic is read from behind the tag, and a real ADTS frame matches
  the `0xFF 0xFx` test directly. Dead code that a test does not hold in place
  should be deleted in the fix for AUD5-01 rather than kept.
- **#13, #14, #15, #17, #22** — clamps and bookkeeping on paths no test reaches
  (32-bit narrowing, an Opus file with several pages, a short read). Not bugs;
  they are the places where a future edit will not be caught.
- **#8 vs #9/#10** — the footer term existed in two functions and the first
  version of the AUD5-03 test only exercised the footer-less case; the test now
  covers both, and both mutations fail it. That is the loop working.

## 9. Examined and found sound

| Area | Method | Amount | Result |
|---|---|---|---|
| WAV: PCM 8/16, A-law, mu-law, IMA, MS ADPCM | differential vs ffmpeg, sample by sample | 9 files | `maxdiff 0`, same length, `lag 0` |
| FLAC: 8/16/24-bit, mono/stereo, 8 kHz, 6 ch | differential | 6 files | bit-identical; 6 channels refused by `ReadMono`/`ReadStereo` with `-11` as documented |
| Ogg Vorbis mono/stereo | differential | 2 files | `maxdiff 1` |
| Ogg Opus | differential vs `libopus` | 1 file + tagged copy | identical (±1), 48 000 samples = last granule 48 312 − preskip 312 |
| MP3: CBR/VBR/mono/8 kHz/320 kbps/joined | differential + seek self-consistency | 13 files | samples within 1 LSB; lengths as S-01 |
| ID3v2 in front of the five containers | 12 tagged files × open + full decode | 24 runs | AUD5-01 found, rest correct |
| ID3v1 and APE tags at the tail | open + decode of an MP3 with each appended | 3 files | identical length and samples to the untagged file, same as ffmpeg |
| Malformed input | ASan+UBSan mutation fuzzing, 27 runs / 21 inputs | 48 700 executions | no crash, no leak, no out-of-bounds; only the documented `dr_flac` overflow |
| Truncation | every byte offset | 13 files, ≈45 000 cuts | 0 problems |
| Seek | `Seek(t)` vs the same offset of a straight decode | 9 files, 2 625 seeks | 0 mismatches on WAV (PCM/IMA/MS ADPCM), FLAC, Vorbis and CBR MP3; VBR MP3 is an estimate by design and was measured against its documented window (§7 W5) |
| Seek arguments | −1, NaN, INF, 1e9, 1e30 on 4 formats | 20 calls | refused (`-1`) or the end (`-15` / clamp), AUD5-04 aside |
| DMA output path | simulated I2S, 4 play modes, tap captured | 14 runs | no lost or doubled sample, gapless at a track change, silence exactly 0 |
| Volume | gain 0…65 534, 0.25 and 1.0 full scale | 6 points | exact and saturating, no wrap |
| Underrun policy | stall sweep, forced double-callback | 6 runs | stops with `FM_ERR_TIMEOUT` as documented |
| `SimpleAudio` settings through `Stop`/`Begin` | play mode, volume, output, callbacks | 4 sequences | survive |
| Ports / switches | `make check` (ports, examples, CubeIDE rules, FatFS mock, 32-bit), `matrix-quick` | 2 runs, 8 configurations | all green |

## 10. Not examined

- Nothing ran on ARM or on a board; the two host players are simulated.
- `make matrix` (57 configurations from scratch) and `make matrix-asan` were not run.
- `make tsan` through the FreeRTOS and CMSIS-RTOS2 ports.
- `cmake` builds (not installed) and `arm-none-eabi-gcc`.
- Stack frames and call depth on Cortex-M7 (`-fstack-size-section`) — no clang
  for that target here; the figures in `README.md` are from earlier rounds.
- `SimpleAudio_Rec*`, `Audio_Convert` and `AudioFileWriter_*` (Save auditor's).
- The demux of audio tracks inside video files (video auditor's).
- `Audio_Stream*` and `Audio_SetCallback` reentrancy.
- Allocation-failure injection at every `FM_Alloc` (only ASan/leak checks).

## 11. Outside my part

- `src/fm_file.c` — a read error is turned into "end of track" in the ADPCM
  path when the short read is the last block; not pursued. (Files part)
- `top_api/examples_sdcard_media.h:210` — `File_DetectType` result used to pick
  a copy strategy; with AUD5-03 a tagged FLAC was also sent down the "MP3"
  branch. (Files part; the detection defect is mine.)
- `src/fm_video.c` — the MP3 track path does not trim the LAME delay either
  (S-01 applies there too). (Video part)

## 12. Scores

| Axis | Score | Evidence |
|---|---|---|
| A. Malformed input | 7 | 48 700 fuzzed executions and 45 000 truncation cuts under ASan+UBSan with no crash, hang or over-read; only the documented `dr_flac` overflow remains |
| B. Correct results | 7 | 46 files compared with ffmpeg: PCM, G.711, both ADPCM, FLAC and Vorbis exact, MP3 within 1 LSB after priming, Opus identical to libopus; one wrong-result defect found (AUD5-03, the type of a tagged file) and one length discrepancy left open (S-01) |
| C. Errors and resources | 6 | three defects about error codes and stream state found (AUD5-01 `-17` for a good file, AUD5-04 the stream left at the old position, AUD5-02 the restart budget never spent); fuzz and truncation found no leaked handle, slot or block |
| D. Memory and timing | 6 | no new timing hotspot; the 1.3 s AAC scan of last round stays fixed; underrun policy measured; stack not measured here |
| E. Documentation | 6 | README's seek/state promise contradicted by AUD5-04, and its "length is exact for MP3 with Xing" by S-01; the encoder-delay paragraph is otherwise accurate |
| F. Tests | 5 | 16 of 24 mutations caught (17 with `FM_USE_OPUS_LIB`) and the four new tests fail on `fc31b25`; but the firmware twin of `simple_audio.h` has no test at all (#23), the copy in `examples/` is held by one playlist only (#22), the stub Opus hides a real regression in the default build (#3), and four clamps on unreached paths survive (#13, #14, #15, #17) |
| G. Diagnosability | 6 | the restart loop logs `auto-restart %u/%u` and now stops; a refused tagged file logged only `parse failed (-17)` until it stopped failing |

**Ready under these conditions:** host-verified only (no ARM run); the
`FM_USE_AUDIO` paths that change here are covered by the suite, and the firmware
copy of `simple_audio.h` differs from the tested `examples/` copy only in its
`#include` lines (`diff` gives 7 changed lines, all `#include`, plus one blank
line) — but nothing builds the `top_api/` copy, so that equivalence is checked
by `diff`, not by a test (§8 #23).

**Three things to fix first:** 1) AUD5-02 (a broken file blocks the playlist for
ever), 2) AUD5-03 (every tagged file misdetected), 3) AUD5-01 (valid tagged
FLAC/Ogg/WAV/Opus refused).

## 13. Where the next round should look

1. **S-01, MP3 delay/padding** — measured on seven files, a fix is half written
   and the only thing missing is the true delay of the repository's vectors.
   Method: cross-correlate a noise MP3, then change length and trim together.
2. **`top_api/` vs `examples/` coverage** — the host tests compile `examples/`;
   a mutation in `top_api/simple_audio.h` survives everything (§8 #23). Method:
   build the same host tests twice, once with `-Itop_api` in front.
3. **The callbacks (Job 2 (b))** — `SimpleAudio_SetDataCallback(NULL)` from
   inside the callback, a callback that calls `Stop`, a 5 ms callback; the
   refill touches pointers the main loop changes. Method: the stub-HAL harness
   used here, with the callback body varied.
4. **AAC framing** — the stub decoder in the suite always agrees; a decoder that
   returns fewer samples, zero, or an error mid-stream is barely covered.
5. **`Audio_Stream*` / `Audio_SetCallback`** — reentrancy and closing the handle
   from the callback.

My estimate: another S1 is unlikely here (the parsers have been fuzzed twice
and the allocator paths are clean under ASan), another S2 is likely in the
MP3 length/priming area (S-01) or in a callback that re-enters the player.

## 14. Findings index

```
AUD5-02 | S2 | confirmed | audio / SimpleAudio | Clain_FileManager/FileManager_lib/top_api/simple_audio.h:1217 | yes | yes | the restart budget is cleared while the same track keeps failing: one broken track restarts for ever and the playlist never moves on
AUD5-03 | S2 | confirmed | audio / media detection | Clain_FileManager/FileManager_lib/src/fm_media.c:189 | yes | yes | every ID3v2-tagged file is reported FM_MEDIA_MP3 whatever it holds
AUD5-01 | S3 | confirmed | audio / Audio_Open | Clain_FileManager/FileManager_lib/src/fm_audio.c:2591 | yes | yes | an ID3v2 tag in front of FLAC, Ogg Vorbis, Ogg Opus or WAV is refused (-17) or decoded by the wrong decoder
AUD5-04 | S3 | confirmed | audio / MP3 seek | Clain_FileManager/FileManager_lib/src/fm_audio.c:1805 | yes | yes | Audio_Seek past the end of an MP3 says EOF but leaves the stream where it was, so the next read plays from the old position
S-01 | n/a | suspicion | audio / MP3 length | Clain_FileManager/FileManager_lib/src/fm_audio.c:1592 | no | no | MP3 encoder delay and padding are counted as sound: length +1 836 samples, first 1 105 samples are priming
S-02 | n/a | suspicion | audio / SimpleAudio | Clain_FileManager/FileManager_lib/top_api/simple_audio.h:1524-1528 | no | no | the volume/play-mode/output the player reports and the ones it plays come from two variables; measured to agree today, recorded because nothing holds them together
S-03 | n/a | suspicion | audio / media detection | Clain_FileManager/FileManager_lib/src/fm_media.c:137 | no | no | Media_DetectBuffer reads a 3-byte "ID3" prefix as a whole tag when the caller passes a short buffer
```

---

## Appendix

Everything needed to reproduce the four findings is in `audit_v5/` next to this
report:

| File | What |
|---|---|
| `AUD5-01.diff` (230 lines) | `src/fm_audio.c`: the tag offset for FLAC / Ogg / Opus / WAV (AUD5-01) and the past-the-end MP3 seek (AUD5-04), in one file. Apply from `Clain_FileManager/FileManager_lib/`. |
| `AUD5-01-only.diff` (204 lines), `AUD5-04-only.diff` (31 lines) | the same two fixes separately, for a reviewer who wants them one at a time |
| `AUD5-02.diff` (80 lines) | both copies of `simple_audio.h`: the restart budget per track |
| `AUD5-03.diff` (79 lines) | `src/fm_media.c`: whole-tag skip in `Media_DetectBuffer`, re-read in `File_DetectType` |
| `AUD5-tests.diff` (523 lines) | `test/audit_v4_tests.h` (tests `AUD5-01`, `AUD5-01b`, `AUD5-03`, `AUD5-04`), `test/main.c`, `Makefile` (`audiorestarttest`), `tools/audio_restart_host_test.c` (new) |
| `tools/audtool.c`, `tools/audtool_opus.c`, `tools/opus_info.c` | CLI harnesses over the real API (`info`, `s16`, `seek`) |
| `tools/cmp.py`, `tools/gen.py`, `vectors/make_vectors.py` | the ffmpeg differential and the vector generators |
| `tools/fuzzloop.c`, `tools/trunc.py`, `tools/seekself.c` | the fuzzer, the truncation sweep, the seek check |
| `tools/dmaplay.c`, `tools/stall_sweep.c`, `tools/underrun_stop.c`, `tools/playmode.c`, `tools/seekread.c`, `tools/seekerr.c` | the DMA / player / seek harnesses quoted in §4 and §7 |
| `tools/audio_restart_host_test.c` | the AUD5-02 reproducer (same file the test patch adds to the tree) |
| `tools/mut5.py`, `tools/mutspec.json`, `mut5_results.txt` | the mutation engine, the 24 mutations of §8 and their raw output |
| `tools/resplice_diffs.py` | puts the four `*.diff` files back into this report's `diff` blocks (a Markdown edit in a text editor drops the CRs; run this after every edit, then `check_diffs.py`) |
| `tools/check_diffs.py` | re-checks that every `diff` block in this report is byte-identical to the file next to it and applies to a clean `fc31b25`: `python3 tools/check_diffs.py ../AUDIT_AUDIO_V5.md AUD5-02.diff AUD5-03.diff AUD5-01-only.diff AUD5-04-only.diff` |
| `tools/detectbuf.c` | the `Media_DetectBuffer` probe behind S-03 |
| `tools/detect.c` | the twelve tagged files through `Media_DetectBuffer` (AUD5-03 table) |

Applying all four patches to a fresh `git archive fc31b25` extract gives
`Results: 593/593 passed, 0 failures`, `ALL PASS (0 failures)` from
`make audiorestarttest`, and `make check` exit 0; on the same extract with the
test patch only, the four new tests fail (`Results: 589/593 passed,
4 failures`). `mut5_results.txt` is the raw output of the 24 mutations of §8,
each run against that patched extract.
