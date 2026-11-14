# گراف تست، شواهد و مرور تاریخچه

[بازگشت به ورودی](README.md) · [فایل](FILE_STORAGE.md) · [رسانه](MEDIA_BOARD.md) · [حافظه](PORTABILITY_MEMORY.md)

دستورهای جدول از `Clain_FileManager/FileManager_lib/` اجرا می‌شوند مگر ذکر شده باشد. هر بار نسخهٔ compiler، flags، خروجی، exit code و skip را ثبت کنید. مقدارهای زیر فهرست مسیرهای تست‌اند، نه ادعای موفقیت دوباره در هر commit.

| نیاز | دستور/فایل | دامنه و محدودیت |
|---|---|---|
| suite عمومی | `make test`, `test/main.c`, `test/audit_v2_tests.h` … `audit_v5_tests.h` | فایل و media و تست‌های embedded-model؛ نتیجهٔ کلی جایگزین coverage هدفمند نیست. |
| مجموعهٔ میزبان | `make check` | شامل چند suite و tools؛ CMake و 32-bit در نبود ابزار ممکن است skip شوند؛ خروجی را بخوانید. |
| هم‌زمانی | `make tsan` | TSan فقط raceهای مسیرهای اجراشده را می‌بیند. |
| ۳۲بیتی | `sh tools/check_32bit.sh`, اختیاری `ubsan` | host x86 32-bit با Zig یا gcc-multilib، نه اجرای ARM؛ در نبود compiler مناسب skip می‌شود. |
| ماتریس features | `sh tools/check_matrix.sh`, `quick`, `asan` | rebuildهای متعدد؛ چند configuration فقط build/link هستند. در workspace مجزا و بدون هم‌زمانی اجرا کنید. |
| سازگاری build | `sh tools/check_cmake.sh`, `check_cubeide.sh`, `check_ports.sh`, `check_board.sh` | `check_cubeide`/`check_board` build/link واقعی firmware یا اجرای board نیستند. |
| FatFS | `sh test/build_fatfs_host.sh`, `test/fatfs_host_test.c`, `test/fatfs_mock/` | mock چند نسخه با sanitizer؛ filesystem واقعی نیست. |
| FatFS واقعی، RAM disk | `sh tools/check_fatfs_real_rename.sh <fatfs-source>`, `check_fattime_real.sh <fatfs-source>` | نسخهٔ خارجی و پیش‌نیازها در اسکریپت؛ دیسک حافظه‌ای، نه سخت‌افزار. |
| فایل/مسیر | `test/audit_v5_tests.h`, `python3 tools/check_path_overlap.py` | differential با seed ثابت؛ full fuzz یا همهٔ aliasها نیست. ابزار overlap در شاخهٔ فعلی اضافه شده و ممکن است در commitهای قدیمی موجود نباشد. |
| codec و رسانهٔ حلقه‌بسته | `python3 tools/closed_loop.py <ffmpeg> [--asan]` | FFmpeg/NumPy و codecهای مشخص؛ synthetic، محدود به سناریوهای ابزار. |
| MP3 gapless | `python3 tools/test_mp3_gapless.py <ffmpeg> [--asan]` | مقایسه و seek برای libmp3lame، نه همهٔ encoderها؛ rebuild می‌کند. |
| audio record و callback | `make audiorecordtest`, `tools/audio_record_host_test.c` | دو نسخهٔ header و eventهای شبیه‌سازی‌شده، نه timing فیزیکی. |
| save و ویدئو | `make saveexamplestest`, `test/video_save_tests.h`, `test/video_hooks_test.c` | منطق میزبان و فایل‌های مصنوعی. |
| سخت‌افزار شبیه‌سازی‌شده | `test/usb_msc_host_test.c`, `test/hw_jpeg_host_test.c`, `tools/halstub/` | stub/mock؛ تست دستگاه واقعی نیست. |
| گراف | از ریشه `python3 project_memory/test_graph.py` و `python3 project_memory/graph.py check` | فایل‌ها/لینک‌های انتخاب‌شده و extractor را بررسی می‌کند، نه درست‌بودن معنای روابط. |

## پیشگیری از نتیجهٔ اشتباه

- `make` با تغییر flagها الزاماً object قبلی را rebuild نمی‌کند: clean یا build مجزا. اسکریپت‌هایی که rebuild اجباری می‌کنند را هم از لحاظ flags بررسی کنید.
- چند `fm_test`/ctest نباید در یک working directory مشترک هم‌زمان اجرا شوند؛ فایل‌های تست مشترک می‌توانند شکست مصنوعی تولید کنند. workspace/checkout مجزا یا اجرای سریالی انتخاب کنید.
- خروجی mock، sanitizer، mutation، مدل power-cut و hardware test از جنس شواهد متفاوت‌اند. هیچ‌کدام را جای دیگری گزارش نکنید.
- تغییر vendor، config یا generated CubeMX می‌تواند فرض‌ها را عوض کند؛ گراف lexical تمام feature branchها، macro expansion، runtime callback یا timing را مدل نمی‌کند.

## راهنمای تاریخچه

| ورودی | موضوع و هشدار |
|---|---|
| [`audit_reports/FILE_PHASE1_INTERNAL.md`](../audit_reports/FILE_PHASE1_INTERNAL.md) | بررسی داخلی فایل، اشکال rename و Path_Join، محدودهٔ تست‌ها و وقایع build. |
| [`audit_prompts/file_phase1/README.md`](../audit_prompts/file_phase1/README.md) | سه پرامپت فایل روی commit پین‌شده؛ نسخهٔ فعلی و گزارش‌های ممیزی را جدا بررسی کنید. |
| [`audit_reports/FATFS_RECORDER_FOLLOWUP.md`](../audit_reports/FATFS_RECORDER_FOLLOWUP.md) | FatFS timestamp، ابزار lock و شبیه‌سازی مشکل recorder؛ گزارشِ رفع SAV3-04 نیست. |
| [`audit_reports/MP3_GAPLESS_FOLLOWUP.md`](../audit_reports/MP3_GAPLESS_FOLLOWUP.md) | محدودهٔ MP3 gapless و seek خطی؛ ادعای همهٔ encoderها نیست. |
| [`audit_reports/CLOSED_LOOP_HOST.md`](../audit_reports/CLOSED_LOOP_HOST.md), [`AUDIO_FOLLOWUP.md`](../audit_reports/AUDIO_FOLLOWUP.md) | شبیه‌سازی media و audio تاریخی؛ یادداشت superseding و commit را بخوانید. |
| `Report.txt`, `ReportV2.txt`, `ReportV3.txt`, `ReportV4.txt`, `ReportV41.txt` | گزارش‌های قبلی؛ صحت و ارتباط هر یافته با HEAD باید مجدداً بررسی شود. |

«کمترین زمان خواندن» یعنی شروع با node مرتبط و سپس بررسی دقیق همان مسیر و تست؛ نه جایگزین‌کردن تحلیل dependencyها با خلاصه‌ای که ممکن است قدیمی شده باشد.
