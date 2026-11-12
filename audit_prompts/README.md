# External audit prompts — five parts, one auditor each

## راهنمای کوتاه (فارسی)

- پنج فایل، پنج ایجنت: به هر ایجنت **فقط یک فایل** بدهید و هیچ چیز دیگر. هر
  پرامپت کامل و مستقل است (آدرس مخزن، کامیت، روش بیلد، حدود کار، قالب گزارش).
- هر ایجنت بخش خودش را ممیزی می‌کند و هر جا ارزیابی‌اش لازم داشته باشد به
  حاشیهٔ بخش‌های دیگر هم سر می‌زند (بند ۵ هر پرامپت، «margins»).
- ایجنت روی GitHub هیچ تغییری نمی‌دهد؛ روی نسخهٔ محلی خودش آزاد است کد را
  عوض کند، تست بنویسد و اصلاح را امتحان کند. خروجی فقط **یک فایل گزارش** است:
  برای هر باگ جای دقیق (فایل و خط)، روش بازتولید، علت، اصلاح به شکل diff و
  یک تست رگرسیون.
- گزارش‌ها که برگشتند، آن‌ها را در پوشهٔ `audit_reports/` همین مخزن بگذارید
  (همان نام‌هایی که جدول پایین می‌گوید). دور بعد از روی همین گزارش‌ها ساخته
  می‌شود؛ گزارشی که در مخزن نباشد برای دور بعد گم شده است.
- چرخه: ممیزی ← اصلاح ← پرامپت تازه روی کامیت تازه ← ممیزی. بخش «Starting
  the next round» پایین می‌گوید در هر دور چه چیزهایی باید عوض شود. معیار
  پایان: دوری که هیچ یافتهٔ S1 / S2 تأییدشده ندارد و جدول «Tests that would
  not notice» آن خالی است.

## The five prompts

| File | Part | Report expected | Finding IDs |
|---|---|---|---|
| `01_file_storage_filesystem.md` | file API, filesystems, storage drivers, utilities, the log layer, the build configuration | `AUDIT_FILE_V5.md` | `FILE5-nn` |
| `02_image.md` | BMP / PNG / JPEG decode and encode, pixel operations, `SimpleImage` | `AUDIT_IMAGE_V5.md` | `IMG5-nn` |
| `03_video.md` | containers, H.264, MJPEG, audio tracks of video files, A/V timing, media cache, playback side of `SimpleVideo` | `AUDIT_VIDEO_V5.md` | `VID5-nn` |
| `04_audio.md` | audio decoders, playlist player, type detection, playback side of `SimpleAudio` / `AudioDma` (output switch, callbacks) | `AUDIT_AUDIO_V5.md` | `AUD5-nn` |
| `05_save_record.md` | everything that writes media (encoders, AVI writer, converters, `Rec*`, save examples, board glue) and how the parts work together | `AUDIT_SAVE_V3.md` | `SAV3-nn` |

All five pin the same commit, **`fc31b25`** on
`arena/ba6a87b3-clain-filemanagaer`, and forbid any change on GitHub. Each is
self-contained: the auditor gets one file and nothing else.

## What the five have in common

- **Sections**: 1 what is audited (with the history of that part) · 2 hard
  rules · 3 how to build, what is established at the pinned commit and what is
  not · 4 the jobs and the open leads · 5 scope and margins · 6 where to dig ·
  7 known limits · 8 the report.
- **Rules**: read-only on GitHub; the local clone may be changed freely
  (harnesses, instrumentation, trial fixes, deliberate breakage); every defect
  comes with file, line, reproducer, root cause, a diff that applies to the
  pinned commit, and a regression test; "confirmed" and "suspicion" are kept
  apart; nothing is reported that was not run.
- **Margins**: about four fifths of the time inside the own part; crossing into
  another part is allowed when the evaluation needs it (a call chain, a shared
  contract, a sibling of a mistake, a reproducer). A defect confirmed outside
  the own part is a full finding that names its owner. Each prompt lists the
  margins that matter for it. Overlap between auditors is intended.
- **Job 1 is always "check the earlier fixes"**, with a table of the earlier
  findings and what each fix claims; then the code that is new; then what
  nobody has examined. Every prompt carries **open leads** (W…, and for the
  save part also L…): things the maintainers know they did not settle. The
  report must answer each.
- **Mutation**: the auditor breaks the code on purpose and lists which breaks
  the tests did not notice ("Tests that would not notice").
- **The report** has a fixed order: Persian summary, audit record, table of
  findings, one section per finding, suspicions, verdict on the earlier
  findings, answers to the leads, tests that would not notice, examined and
  found sound, not examined, outside my part, scores A–G (the save part adds
  H), where the next round should look, and a machine-readable findings index
  (one line per finding, the same form in all five) so that the five reports
  can be merged into one work list.

## Where this round stands

- Round one: parts 1–4 at `8dd0f25` (reports: `ReportV2.txt` at the repository
  root); part 5 at `523d2da` (`AUDIT_SAVE_V1`; its text is not in the
  repository). A follow-up of parts 1–4 was prepared at `a6cf829`, but no
  report of it exists.
- Round two (V4 for parts 1–4, V2 for part 5): all five parts at `48a00bc`.
  The five reports are in one file, `ReportV3.txt` at the repository root
  (file: lines 1–641, image: 642–1272, video: 1273–2075, save: 2076–2501,
  audio: 2502–3576). It returned 18 confirmed findings (file 5, image 2,
  video 7, audio 3, save 1) and a number of suspicions. All confirmed
  findings and the suspicions IMG4-S2, AUD4 SUSP-A and SAV2-03 were fixed in
  `fc31b25`; what was left open is listed in each prompt.
- This round (V5 for parts 1–4, V3 for part 5) audits `fc31b25`. In every
  prompt, section 4 has a new block in front of Job 1, "What round V4 (V2)
  found, and what the fixes claim": the table of that round's findings with
  the place of each fix and of its test, and the questions the maintainers
  have about their own fixes. The older Job 1 tables stay: the V4 auditors
  gave their verdicts on them, and those verdicts are in `ReportV3.txt`.
- Several fixes of round two change public behaviour (delete / rename of an
  open file is refused, `FM_SCALE_BILINEAR` gives other pixels, `FM_VideoInfo`
  grew, an MP3 that changes its channel count plays on converted, a WAV with
  a `data` length of 0 plays to its end). Each prompt names the ones of its
  part; check them as changes of a contract, not only as fixes.
- Scope of the parts is unchanged against round two.

## Starting the next round

After the reports are in and the fixes are committed:

1. Put the reports into `audit_reports/` and commit them.
2. Merge the five findings indexes into one list; note for each finding the
   fixing commit and the test that was added.
3. In every prompt change: the pinned commit (section 1, and every place
   where the text means "the commit under audit"; older commits stay where
   the text speaks of history), the history table of the part, the table of
   section 3 ("What is established") with figures measured at the new commit,
   the Job 1 table (the findings of this round and what each fix claims), the
   Job 2 list (what the fixing commits changed), the open leads (drop the
   settled ones, add what the reports list under "Not examined" and "Where the
   next round should look"), the known limits, the report name and the ID
   prefix.
4. Check that every path and identifier named in the prompts exists at the new
   commit.
5. Compare the scores A–G (H) of the new reports with the old ones, part by
   part.

A part is done when a round returns no confirmed S1 or S2 finding, its "Tests
that would not notice" table is empty, and its "Not examined" list holds
nothing that an earlier round has not covered.
