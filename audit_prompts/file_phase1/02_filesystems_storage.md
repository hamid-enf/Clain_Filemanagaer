# مأموریت مستقل ممیزی فایل و ذخیره‌سازی — فاز ۱

شما یکی از سه ممیز مستقل هستید. این متن را به‌تنهایی اجرا کنید؛ منتظر دو ممیز دیگر نمانید، از گزارش آن‌ها نتیجه‌گیری نکنید و سعی نکنید با آن‌ها به توافق برسید. هم‌پوشانی عمدی است تا بعداً نتایج سه نفر مستقل مقایسه شوند.

## هدف، مبنا و محدودیت

مخزن `https://github.com/hamid-enf/Clain_Filemanagaer`، شاخه `arena/ba6a87b3-clain-filemanagaer`، **کد هدف commit `aec191ba2048f1574f4c1f98bc4587572114ff7e`** است. ممیزی را دقیقاً روی همان tree انجام بده، حتی اگر شاخه بعداً جلو رفته باشد. ابتدا `git rev-parse HEAD`, `git status --short`, نوع سیستم‌عامل، کامپایلر، امکانات و تاریخ را ثبت کن. اگر دسترسی یا SHA با این متن نمی‌خواند توقف کن و این عدم تطابق را گزارش کن؛ به دلخواه tree دیگری را ممیزی نکن. اسکریپت‌ها و منابع بیرونی باید نسخه/commit داشته باشند. به شبکه/گیت‌هاب write، push، PR یا تغییری در مخزن اصلی نده؛ همهٔ تست‌ها و حتی patch آزمایشی فقط در clone یا workspace جدا و موقت باشند. دادهٔ واقعی کاربر را هرگز استفاده، پاک یا بازنویسی نکن. **تست فیزیکی برد یا دستگاه انجام نده**؛ اگر در دسترس نیست، ادعای رفتار واقعی SD، DMA، USB، کش یا قطع برق نکن.

دامنه فقط زیرساخت فایل است: storage/block drivers و قراردادشان، VFS/mount، backendهای فایل‌سیستم، فایل/پوشه/مسیر، خواندن و نوشتن فایل، عملیات باینری/متن/JSON، stream، copy/move، cache/flush، safe write، logger، async و لایه‌های مشترک که مستقیماً برای آن‌ها لازم‌اند. صوت، موسیقی، تصویر، ویدئو، کدک، پخش و ضبط/ذخیرهٔ رسانه صراحتاً خارج از دامنه‌اند؛ حتی اگر تست کل پروژه آن‌ها را اجرا کند، روی آن‌ها یافته یا پیشنهاد ممیزی ارائه نکن. dependency مشترک را فقط به‌اندازهٔ لازم برای تحلیل فایل بررسی کن. STM32 نمونهٔ فعلی است؛ کاربرد بعدی ممکن است روی معماری دیگری مانند Zynq باشد. **هیچ پورت یا فرض معماری جدید پیاده‌سازی نکن**؛ فقط فرض‌های هدف‌وابسته و شرایط قابل‌حمل‌بودن را با شاهد جداگانه گزارش کن. Zynq می‌تواند bare-metal یا سیستم‌عامل داشته باشد؛ هیچ‌کدام را بدون شواهد فرض نکن.

مبنا قبلاً آزمایش شده، اما سبز بودن تست‌ها دلیل نبود باگ نیست. گزارش‌های قدیمی `Report.txt`, `ReportV2.txt`, `ReportV3.txt`, `ReportV4.txt`, `audit_prompts/01_file_storage_filesystem.md`, و `audit_reports/FILE_PHASE1_INTERNAL.md` را پیدا و با همین کد تطبیق بده؛ ادعاهای پیشین را مستقل بازآزمایی کن و آنچه را تکرار نکرده‌ای مشخص کن. اصلاح اخیر FatFS نام‌های موقت `~fmXX.tmp` را حفظ می‌کند ولی **rename-over در FatFS اتمیِ قطع برق نیست**. وجود این اصلاح را فرض اثبات کافی ندان. خروجی صفر یافته مساوی «همه‌چیز بدون خطاست» نیست.

## روش مشترک و قواعد شواهد

1. نقشهٔ call chain واقعی از public API تا backend و storage را همراه محل فایل/تابع بساز؛ مسیرهای فراخوان، حالت‌های خطا، قرارداد منابع و تفاوت backendها را از خود کد بخوان. شرط‌های `FM_USE_*`, گزینه‌های build، اندازه‌ها و versionهای FatFS/LittleFS را ذکر کن.
2. حداقل یک baseline clean در workspace جدا بساز و تست‌های مربوط را اجرا کن؛ نمونه‌ها: `make test`, `sh test/build_fatfs_host.sh`, `make check`, `make tsan`, `sh tools/check_32bit.sh` و `sh tools/check_matrix.sh` فقط اگر ابزار/زمان مناسب موجود است. همه را اجباری یا موفق فرض نکن. برای هر دستور working directory، exit code، تعداد پاس/شکست/skip و نسخهٔ ابزار را ثبت کن. تست‌هایی که فایل‌های مشترک می‌سازند را موازی در یک workspace اجرا نکن. بعد از تغییر flagها clean یا build مجزا انجام بده. هیچ فایل یا dataset حجیم را وارد Git نکن.
3. برای هر ادعای باگ، حداقل یک reproducer کوچک و قابل اجرا، ورودی دقیق، نتیجهٔ مورد انتظار مبتنی بر قرارداد مستند یا oracle مستقل، نتیجهٔ واقعی، و فایل/تابع/خط در tree پین‌شده بده. خطای محیط/تست یا assumption را با defect محصول قاطی نکن. baseline و fixed/test-patch را در workspaceهای جدا مقایسه کن. برای باینری یا سناریوی قدرت‌قطع، seed و ترتیب وقایع را ذخیره کن؛ اگر نامطمئن است «فرضیه» بنویس نه تأییدشده.
4. مرزهای عددی، 32/64-bit، overflow، null/اندازه صفر، مسیر نامعتبر و بلند، منابع ناکافی، short read/write، خطای هر عملیات، callback، lock و cancellation را جایی که با مأموریتت مرتبط است امتحان کن. دادهٔ مستقل و oracle مستقل (POSIX، فایل‌سیستم واقعی یا مدل ساده در حافظه) به‌کار ببر؛ round-trip خود کتابخانه تنها oracle نیست. نسخه، تنظیمات و تفاوت‌های مشروع backendها را روشن کن. ASan/UBSan/LSan و TSan را در صورت امکان درست instrument کن؛ نبود ابزار یا سخت‌افزار را صادقانه skip کن.
5. هدف mutation tests است، نه فقط شمارش تست. حداقل ۶ mutation هدفمند و قابل بازگشت را در clone جدا معرفی کن: هر mutation باید یک شرط واقعی، قرارداد یا خطای قابل تصور را مختل کند؛ بنویس کدام تست آن را کشت/زنده گذاشت یا واقعاً اجرا نشد. هیچ‌وقت mutation را در مخزن کاربر اعمال نکن. پوشش ظاهری خط یا `PASS` کلی به‌تنهایی کافی نیست.
6. از اصلاحات قبلی نمونهٔ منفی بساز، مخصوصاً collision نام موقت، source/destination یکسان یا alias، failure در میانهٔ کار و برگشت خطا. برای failpoint سراسری از before/after state و قابل‌خواندن‌بودن داده مقایسه بگیر؛ میان «atomic visibility»، «durability پس از power loss» و «error return» تفاوت بگذار. برای backend/دستگاه بدون ضمانت، الزام ناممکن وضع نکن: نقص مستندات، ضعف API و خطای پیاده‌سازی را جدا کن.

## قالب تحویل الزامی

یک گزارش با نام `AUDIT_FILE_PHASE1_B.md` در workspace بده و محتوای اصلی را در پاسخ هم خلاصه کن. ساختار:
- خلاصهٔ فارسی برای تصمیم‌گیری: تعداد یافته‌های تأییدشده با شدت، فرضیه‌ها و مهم‌ترین blockerها؛ هیچ امتیاز تضمین ایمنی نده.
- جدول ویژگی/سطح کار: مسیر و API، backend و config، تست و oracle، pass/fail/skip، مرز و دلیل؛ دقیقاً معلوم باشد چه چیزی بررسی نشده.
- یافته‌ها با ID FILE-B-01, FILE-B-02, … و شدت Critical/High/Medium/Low/Info با معیار صریح: نابودی داده، violation ایمنی حافظه، crash، رفتار اشتباه، کیفیت/مستندات. هر یافته: commit، مکان، پیش‌شرط، اثر، بازتولید/seed، انتظار/واقعیت، ریشه، patch پیشنهادی مستقل یا شبه‌کد دقیق، تست قبل/بعد، ملاحظات سازگاری و میزان اطمینان. اگر patch نداری واضح بگو.
- جدول recheck یافته‌های پیشین: fixed / هنوز باز / متفاوت / اجرا نشد، با شاهد؛ تکرار یک مشکل با اسم تازه را علامت بزن.
- جدول mutation با هر مورد و killed/survived/not-run؛ گزارش ضعف تست‌ها حتی بدون defect تولیدی.
- فهرست صریح false positiveهای حذف‌شده، unresolved questions، ابزارهای در دسترس/غایب، و محدودیت‌های portability و سخت‌افزار.
- فهرست قابل ادغام: یک خط برای هر finding با `id | severity | status | component | file:function | short cause | reproducer | proposed fix | test`. هر ادعا قابل بررسی باشد؛ چیزی را اجرا نکرده‌ای وانمود نکن.

## مأموریت اختصاصی B — فایل‌سیستم، بلوک و خطاهای ذخیره‌سازی

اولویت با `backends/fm_fatfs.c`, `fm_littlefs.c`, `fm_posix_fs.c`, `fm_ramdisk.c`, `src/fm_fmfs.c`, `fm_storage.c`, `fm_filesystem.c`, SD/QSPI/SPI/EEPROM و `fm_usb_msc.c` فقط از منظر درست‌بودن ذخیره‌سازی است. Video/audio و هر media stack بیرون است.
- contract تستی بلوک با read/write/erase/sync و byte/block units بساز: alignment، sector size، bounds، capacity، read-only، تغییر geometry، write short/error، overflow، cache/bounce و بازگشت خطا. ورودی خراب را از خطای backend جدا کن؛ تغییر رفتار واقعی DMA/cache را بدون برد نتیجه نگیر.
- FAT mock را تنها مرجع نکن: حداقل یک FatFS واقعی نسخه‌دار را روی دیسک حافظه‌ای تست کن و در صورت امکان LittleFS/FMFS مستقل را remount و داده را با یک oracle متفاوت مقایسه کن. فهرست version/config، درایو/چند volume، lock table و حالت CUBEMX در برابر FILEMANAGER و `get_fattime` را بده. از ابزارهای موجود `test/build_fatfs_host.sh`, `tools/check_fatfs_real_rename.sh`, `tools/check_fattime_real.sh` استفاده کن ولی به موفقیت آن‌ها اکتفا نکن.
- اصلاح aec191b را در real FatFS و mock بازآزمایی کن: فایل‌های موجود `~fmrn.tmp`, `~fm00.tmp`, `~fm01.tmp`، مبدأ با نام نامزد، alias حروف/۸.۳، directory، collision تمام ۲۵۶ نام، خطای هر rename/stat/unlink و remount پس از interruption. بررسی کن منابع کاربران و backup پیشین گم نشوند. تشخیص بده کدام فرض‌ها صرفاً به mock محدودند.
- حالات خرابی فرمت/metadata را با تغییرات کوچک و imageهای bounded بررسی کن؛ برای FMFS journal/checkpoint، LittleFS و FatFS محدودهٔ واقعاً تست‌شده را با remount و پیامدها گزارش کن. اگر crash oracle مستقل نداری، تضمین atomicity نده. رفتار storage از ISR، reentrancy و hot-unplug را فقط تا جایی که شبیه‌سازی کرده‌ای گزارش کن.

خروجی، نقشهٔ صریح dependencyها و الزام‌های port برای معماری دیگر (word size, endian, alignment, cache, sync, filesystem, alloc) است، نه کد پورت.
