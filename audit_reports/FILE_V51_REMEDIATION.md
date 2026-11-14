# ادامهٔ اصلاحات ReportV51 — وضعیت کار

**به‌روزرسانی:** برای وضعیت جدیدتر و رفع انتشار logger / no-overwrite و اصلاح JSON، [گزارش ادامه](FILE_V51_REMEDIATION_3.md) را ببینید.

این سند ادامهٔ [`FILE_V51_REVIEW.md`](FILE_V51_REVIEW.md) است؛ گزارش قبلی وضعیت پایان دور قبل را ثبت می‌کند و نباید به‌عنوان وضعیت کنونی خوانده شود. مبنای کد شاخه `efd0347` به‌علاوه تغییرات کاری دو دور است. این patch همهٔ موارد گزارش را نبسته است.

## تغییرات این دور

- **مالکیت نام temp:** flag جدید `FM_OPEN_EXCLUSIVE` و capability صریح `FM_FS_CAP_EXCLUSIVE_CREATE`، با پیاده‌سازی POSIX (`O_EXCL`)، FatFS (`FA_CREATE_NEW`)، LittleFS (`LFS_O_EXCL`) و FMFS. آداپتور بدون capability قبل از فراخوانی open، `FM_ERR_UNSUPPORTED` می‌دهد. `File_Copy`، `File_SafeReplace`، editorهای فایل و متن و بازکردن logger به‌جای truncate، temp را با create-if-absent می‌گیرند؛ برخورد نام `FM_ERR_EXISTS` است. این برخوردها برای POSIX/FMFS/LittleFS و FatFS mock تست شده‌اند.
- **بازیابی بی‌مدرک:** `File_Recover` و `Logger_Recover` دیگر فایل را صرفاً به‌خاطر نام حذف نمی‌کنند؛ وجود فایل مشکوک را با `FM_ERR_EXISTS` گزارش می‌کنند. داده باید پیش از پاک‌سازی دستی شناسایی شود. این یک تغییر عمدی قرارداد، نه بازیابی خودکار فایل‌های قدیمی است.
- **Backup تراکنش:** backup مستقیماً با exclusive-create رزرو می‌شود و backup موجود کاربر دست‌نخورده می‌ماند. خطای پاک‌سازی پس از موفقیت replace، `FM_ERR_CLEANUP` می‌دهد؛ یعنی replace موفق گزارش شده اما وضعیت حذف backup نامعلوم است.
- **کپی و انتقال:** `File_Copy` دیگر در نبود rename یا طول نام به نوشتن مستقیم روی مقصد برنمی‌گردد؛ به‌ترتیب `FM_ERR_UNSUPPORTED` / `FM_ERR_TOO_LARGE` می‌دهد. `FM_Transfer_CopyEx` با temp رزروشده کپی و verify می‌کند و سپس rename می‌کند؛ لغو و خطای write تست‌شده مقصد قدیمی را حفظ کردند. `Async_StartCopy` و move بین mountها نیز stage می‌شوند؛ لغو copy تست شده است. move روی یک mount در اولین poll به rename سپرده می‌شود تا FatFS بتواند alias خودش را تشخیص دهد؛ قبل از آن poll می‌توان لغو کرد. این تغییر بر زمان‌بندی و callbackها اثر دارد.
- **دایرکتوری:** شکست `Directory_Rewind` دیگر handle عمومی را آزاد نمی‌کند؛ handle تا close رزرو می‌ماند و last-error به‌روز می‌شود. پس از شکست، خواندن تا reopen موفق خطا می‌دهد.
- **truncate:** سیاست جدید در هر backend، clamp موقعیت به اندازهٔ جدید است؛ POSIX/FMFS/LittleFS روی میزبان تست شده‌اند. اگر seek پس از truncate شکست بخورد، اندازه ممکن است با وجود خطا تغییر کرده باشد. FAT mock این سناریوی دقیق جدید را هنوز جدا تست نکرده است.
- **مسیر بیش‌ازحد بلند edit:** `File_Insert` و خانواده‌اش اکنون `FM_ERR_TOO_LARGE` می‌دهند. اصلاحات دور قبلی برای A-04/A-07/B-01/B-02/B-04/C-04 نیز باقی‌اند.

## موارد باز و محدودهٔ تضمین

1. **هنوز بدون claim کامل علیه actor بیرونی:** یک actor دیگر که عمداً temp رزروشده را در طول عملیات حذف/جایگزین می‌کند می‌تواند نام را دوباره استفاده کند. ایجاد انحصاری جلوی برخورد اولیه و نویسندگان همکار کتابخانه با همان نام را می‌گیرد، اما هویت inode و cleanup پس از rename با وضعیت مبهم را تضمین نمی‌کند. recovery خودکار همراه سند مالکیت و سیاست crash همچنان لازم است. آزمون deterministic دو-thread روی `SafeReplace` در این دور اجرا نشده است.
2. **C-06، انتشار logger:** exclusive open و recovery امن‌تر شده‌اند اما `logger_finalize_file` هنوز rename-over می‌کند و اگر actor دیگری نام نهایی را پیش از انتشار بسازد می‌تواند آن را overwrite کند. حل کامل نیازمند primitive انتشار اتمیک no-replace برای هر backend است.
3. **`no_overwrite` در transfer** از check و rename جدا تشکیل شده و در برابر هم‌زمانی/actor خارجی اتمیک نیست. `File_Move` بین mountها در rollback هم pre-check دارد. هنوز نباید به این شرط در فضای نام مشترک اعتماد کرد.
4. **alias و mount:** async move روی یک mount به rename واقعی آن backend می‌رود، ولی 8.3 را در این دور با FatFS واقعی بازتست نکردیم. mountهای متفاوت به یک underlying volume، POSIX hard link/symlink و سایر aliasهای backend شناسایی نشده‌اند.
5. **Commit/durability:** خطای rename یا sync می‌تواند پس از تغییر namespace رخ دهد؛ API عمومی همهٔ این موارد را از پیش‌از-commit تفکیک نمی‌کند (C-08). FatFS rename-over چندمرحله‌ای است؛ reset/power-cut یا خطای rollback تضمین نشده است.
6. **سایر موارد گزارش:** JSON escape و عمق (A-06)، stale pointer پس از close و reuse (A-08)، رفتار listing پس از خطا (A-12)، و محاسبهٔ `CTRL_TRIM` در diskio نیازمند کارند. A-13 وابسته به سیاست verify/commit و A-14 به قرارداد backup/recovery وابسته است. logger finalize بالا همچنان ریسک از دست رفتن داده دارد. بررسی همهٔ short-write consumerها و تخصیص نام temp در تمامی APIها هنوز کامل نیست.
7. **سازگاری:** `FM_FS`, `FM_Dir`, `FM_Async` تغییر layout داده‌اند: تمام مصرف‌کنندگان باید از نو build شوند. custom adapterها را zero-init کنید و فقط اگر واقعاً create-if-absent اتمیک دارند capability را فعال کنید. نام temp موجود دیگر خودکار پاک نمی‌شود. فایل‌سیستم بدون rename/exclusive-create دیگر برای عملیات stage پشتیبانی نمی‌شود.

## تست این دور

- جدیدترین اجرای workspace: `make -s -j2 test`، **675/675**. تست‌ها شامل collision، خطای write، cancel، cleanup fault، rewind، truncate و backendهای POSIX/FMFS/LittleFS هستند.
- archive مجزا از نسخهٔ کمی پیش‌تر (پیش از آخرین تغییر truncate و تست جدید): `make check` موفق، suite **673/673**، ASan/UBSan همان suite، FatFS mock سه نسل هرکدام **54/54** و dual-volume **59/59**؛ CMake و 32-bit با پیام نبود ابزار skip شدند. سایر بخش‌های `make check` طبق خروجی پاس شدند.
- archive مجزا بعد از تغییر truncate و پیش از آخرین تغییر کوچک بررسی خطای close و افزودن تست transfer: `make asan` **674/674**. ThreadSanitizer در snapshot پیش‌تر **678/678**، بدون هشدار گزارش‌شده؛ این تست TSan اثبات absence of races یا تست deterministic مورد C-02 نیست.
- هیچ تست عملی برد، قدرت‌قطع، SD/USB یا زمان‌بندی DMA انجام نشده است. تفاوت snapshotهای بالا مهم است؛ نتایج build قبلی را به آخرین تغییرات تعمیم ندهید.
