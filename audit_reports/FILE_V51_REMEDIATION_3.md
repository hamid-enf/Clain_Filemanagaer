# ادامهٔ اصلاحات ReportV51 — انتشار بدون overwrite و JSON

این سند وضعیت جدیدتر از [`FILE_V51_REMEDIATION.md`](FILE_V51_REMEDIATION.md) و [`FILE_V51_REVIEW.md`](FILE_V51_REVIEW.md) را ثبت می‌کند؛ مبنای patch کاری همان `efd0347` به‌علاوه تغییرات دو دور پیشین و این دور است. گزارش، بسته‌شدن کل ممیزی نیست.

## این دور چه تغییر کرد

### انتشار مقصد بدون جایگزینی — C-06 و no_overwrite

`FM_FSOps.rename_noreplace` افزوده شد؛ عملیات داخلی `FM_File_RenameNoReplace` پس از resolve/pin و بررسی open handle آن را زیر قفل mount فراخوانی می‌کند. آداپتور سفارشی بدون این عملیات `FM_ERR_UNSUPPORTED` می‌دهد، نه fallback ناامن.

- POSIX: `link` بدون جایگزینی و سپس `unlink` مبدأ. اگر link موفق و unlink شکست بخورد، مقصد منتشر شده و `FM_ERR_CLEANUP` بازمی‌گردد؛ temp باقی می‌ماند. filesystemهایی که hardlink را پشتیبانی نمی‌کنند ممکن است عملیات را رد کنند.
- FatFS: `f_rename` مستقیم، که مقصد موجود را با `FR_EXIST` رد می‌کند. این مسیر مثل rename-over چندمرحله‌ای نیست. تست FatFS واقعی ST روی RAM disk و mock اجرا شد.
- FMFS و LittleFS: lookup و rename زیر قفل همان mount. این شرط فقط وقتی معتبر است که همهٔ دسترسی‌های هم‌زمان به آن instance از همان mount و قفل بگذرند؛ دستکاری مستقیم backend/medium خارج از کتابخانه یا mount/instance نامستقل پوشش داده نشده است.
- `FM_Transfer_CopyEx` با `no_overwrite` حالا هنگام انتشار از این عملیات استفاده می‌کند؛ تست callback پس از شروع کپی عمداً مقصد را می‌سازد و محتوا حفظ می‌شود. `Logger_Stop` در حالت crash-safe همین عملیات را استفاده می‌کند؛ وقتی بعد از start نام نهایی اشغال شود، `FM_ERR_EXISTS` می‌دهد، فایل اشغال‌کننده و log موقت هر دو حفظ می‌شوند. تست‌ها روی POSIX، FMFS، LittleFS و FatFS mock انجام شدند.
- از آنجا که POSIX link و unlink دو مرحله‌اند، این عملیات تضمین دوام، پاک‌سازی بعد از قطع برق یا محافظت علیه حذف/جایگزینی temp توسط actor خارجی نیست. fault تزریق‌شدهٔ جدا برای شکست `unlink` پس از link در این دور اجرا نشده است. سیاست خطای rename/sync نامشخص backendها همچنان باز است.

### JSON — A-06

`\\uXXXX` از UTF-16 به UTF-8 تبدیل می‌شود، surrogate pair معتبر پشتیبانی و surrogate تنها یا escape نامعتبر رد می‌شود. اعتبارسنجی در value و key استفاده می‌شود؛ lookup کلید escapeشده را رمزگشایی می‌کند. کاراکتر کنترل خام در رشته رد می‌شود. writer و parser از `FM_JSON_MAX_DEPTH` مشترک (پیش‌فرض ۶۴، مجاز ۱ تا ۶۴) استفاده می‌کنند؛ writer هشت سطح را round-trip می‌کند. این تغییر اندازهٔ `FM_JsonWriter` و ABI را عوض می‌کند و تمام مصرف‌کنندگان باید rebuild شوند.

محدودیت: متن non-ASCII خام هنوز از لحاظ درست‌بودن UTF-8 اعتبارسنجی نمی‌شود؛ مقدار `\\u0000` در API خروجی C-string با `FM_ERR_INVALID_FORMAT` رد می‌شود چون NUL جاسازی‌شده را نمی‌توان به‌درستی بازگرداند. grammar مسیر `a.b[2]` هنوز کلیدهایی با نقطه و براکت را بدون escape جداگانه آدرس‌دهی نمی‌کند. ادعای سازگاری کامل با تمام JSON و Unicode نمی‌شود.

### موارد دیگر

پل FatFS در `CTRL_TRIM` حالا بازهٔ معکوس، اشاره‌گر null و overflow طول را رد می‌کند؛ با driver جعلی و headerهای FatFS واقعی تست شد. قرارداد stale pointer پس از `File_Close` / `Directory_Close` در header صریح است؛ نسل‌دارکردن واقعی handle با API pointer فعلی حل نشده است.

## آزمون‌ها

- suite میزبان پس از آخرین تست‌ها، **681/681**؛ اجرای سریالی و مجزا از سایر `fm_test`ها. دو اجرای قبلی suite در workspace به‌طور ناخواسته هم‌پوشانی داشتند؛ به نتیجهٔ آن دو به‌عنوان شاهد مستقل تکیه نمی‌شود.
- archive مجزای پیش از افزودن terminator خروجی در شاخهٔ خطای JSON، ASan/UBSan: **681/681**؛ TSan snapshot قبل از آخرین تست NUL/truncation و تغییر شرط preprocessor: **685/685** و بدون هشدار گزارش‌شده. TSan اثبات فقدان race نیست.
- ماتریس quick در archive مجزا، پیش از همان تغییر کوچک JSON: هر **۸ پیکربندی** image/video/audio پاس، صفر شکست.
- FatFS mock با ASan/UBSan: کلاسیک/مدرن/legacy هرکدام **56/56**، دو volume **61/61**؛ مثال‌ها پاس.
- `tools/check_fatfs_real_rename.sh` با FatFS ST commit `dd9126e957ae841a13ccbe2115d39a1fb7414126` روی RAM disk: PASS برای no-replace اشغال‌شده و خالی و سناریوهای قبلی. `tools/check_fatfs_diskio_bounds.sh` با همان headerها و storage جعلی: PASS شامل CTRL_TRIM.
- هیچ تست عملی برد، SD واقعی، قطع برق فیزیکی، USB یا DMA انجام نشده است.

## موارد مهمی که هنوز بازند

- temp ownership پس از حذف/جایگزینی توسط actor خارجی، recovery خودکار همراه مدرک مالکیت، race deterministic دو `SafeReplace`، و سیاست پاک‌سازی نام پس از خطای commit مبهم.
- aliasهای 8.3 در async move در FatFS واقعی، mountهای متعدد بر medium مشترک، hardlink/symlink POSIX و rollback `File_Move` میان mountها. کار قبلی برای async روی یک mount مسیر rename را انتخاب می‌کند، اما آزمون واقعی 8.3 انجام نشده است.
- وضعیت دیدپذیری و دوام پس از خطای rename/sync، خطای بعد از commit و power-cut؛ بررسی یا اصلاح کامل آن صرفاً با انتقال `FM_ERR_IO` ممکن نیست.
- listing پس از نام طولانی/rewind، JSON خام غیر UTF-8، stale pointer پس از close و reused slot، و ممیزی جامع تمام مصرف‌کنندگان short-write و logger rotation. `no_overwrite` روی custom backend تنها با قرارداد درست `rename_noreplace` ایمن است.
