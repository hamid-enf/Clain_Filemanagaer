# STM32H7 + SD Card (FatFS) — راهنمای یکپارچه‌سازی و عیب‌یابی HardFault

این سند برای حالتی است که FileManager را از طریق `backends/fm_fatfs.c` به
FatFS تولیدشده توسط CubeMX (SDMMC1 + `sd_diskio.c`) وصل می‌کنید؛ همان
ترکیبی که در `examples/sdcard_fatfs.c` استفاده شده.

```
FileManager (File_WriteText/…)
   └─ VFS: FM_VFS_Resolve → mount "/" → FM_FS (fm_fatfs adapter)
        └─ FatFS: f_open("0:/greeting.txt", FA_WRITE|FA_CREATE_ALWAYS)
             └─ diskio شما (sd_diskio.c) → HAL_SD_WriteBlocks…(hsd1)
```

## ۱) تنظیم‌های لازم پروژه CubeMX

- Symbol لازم نیست. `include/fm_board_config.h` در بیلد فریمور
  (`USE_HAL_DRIVER`) خودش `FM_USE_FATFS` را ۱ می‌کند، و
  `FM_FATFS_PROVIDE_DISKIO` از `FM_SD_DISKIO` همان فایل نتیجه می‌شود
  (`CUBEMX` یعنی ۰، چون `sd_diskio.c` خود Cube را دارید).
- `backends/fm_fatfs.c` باید در build باشد (با `FM_USE_FATFS` برابر ۰ کل
  فایل خالی کامپایل می‌شود و لینک خطای `undefined reference to
  FatFS_Init` می‌دهد).
- `ffconf.h` پیشنهادی برای bare-metal: `FF_FS_REENTRANT=0`،
  `FF_FS_LOCK=0`، `FF_USE_LFN=1` (هر مدی؛ به بخش stack دقت کنید).

## ۲) باگ‌های پیدا/رفع‌شده در این شاخه (branch)

با بازسازی دقیق سناریوی `ex_1_SdCard_file_basic` روی هاست (با شبیه‌ساز
FatFS و AddressSanitizer، `test/build_fatfs_host.sh`) این موارد رفع شد:

1. **نشت استاتیک `g_fatfs[]`** — `fatfs_unmount` آبجکت FATFS را پاک نمی‌کرد
   (`f_mount(NULL,…)` هم `fs_type` را صفر نمی‌کند)؛ بعد از `FM_MAX_FS=4`
   بار mount/unmount، `FatFS_Init` برای همیشه `FM_ERR_NO_MEMORY` می‌داد.
2. **`FatFS_Init` تکراری بدون unmount** یک slot جدید می‌گرفت؛ حالا slot
   قبلی همان `FM_FS` بازیافت می‌شود.
3. **`f_mkfs` و تنوع نسخه‌های FatFS** — امضای `f_mkfs` بین نسل‌ها عوض شده و
   ماکروهای شناسایی نسخه هم یکسان نیستند:
   | نسل | ماکروی نسخه | امضای f_mkfs |
   |---|---|---|
   | R0.14 / R0.15 | `FF_DEFINED` / `FFCONF_DEF` | `(path, const MKFS_PARM*, work, len)` |
   | R0.12b…R0.13c (میدل‌ور STM32Cube، از جمله CubeH7 فعلی شما) | `_FATFS`/`_FFCONF` = 68300 | `(path, BYTE opt, DWORD au, work, len)` |
   | R0.12a و قدیمی‌تر | — | `(path, opt, au, work)` |

   تشخیص خودکار حالا بر اساس «حضور» این ماکروهاست و پیش‌فرضِ حالت
   نامشخص، فرم ۵آرگومانی Cube است. اگر هدرهای غیرمعمولی دارید با
   `FM_FATFS_MKFS_VARIANT=1/2/3` صریح انتخابش کنید.
4. **رژیم‌کردن مصرف stack** در عمیق‌ترین نقطه‌ی زنجیره:
   `file_open_internal` یک بافر `FM_PATH_MAX` (=160B) اضافه داشت و
   `fatfs_file_open` هم ۱۶۰ بافت روی stack می‌ساخت؛ جمعاً ~320B کم شد.
5. `fatfs_file_read` برای `len==0` به‌اشتباه `FM_ERR_EOF` می‌داد.

## ۳) چرا دقیقاً روی اولین `File_WriteText` به SD کارت HardFault می‌گیرید؟

شاهد‌های خروجی شما:

- مثال‌های ۱ تا ۱۰ روی RAM-disk سالم‌اند (زنجیره‌ی فراخوانی کوتاه).
- `test_sdcard()` که فقط **می‌خواند** (mount/opendir/getfree) سالم است.
- کرش دقیقاً در اولین `File_WriteText` رخ می‌دهد = اولین **f_open با
  FA_WRITE** = اولین `disk_write` کل بوت، **و عمیق‌ترین زنجیره‌ی stack**
  در کل برنامه:

```
main → ex_1_SdCard(buf 128B)
     → File_WriteText → File_WriteAll(80B)
     → File_Open → file_open_internal(240B)          [شامل FM_VFS_Resolve 256B تودرتو]
     → fatfs_file_open(64B)
     → f_open  ← FatFS با FF_USE_LFN==1 خودش lbuf[(FF_MAX_LFN+1)*2] = 512B روی stack می‌گذارد
     → f_write/f_sync → disk_write → SD_write → HAL_SD_…
```

جمع این زنجیره با LFN روی stack ≈ **۱.۵ تا ۲ کیلوبایت** است در حالی که
stack پیش‌فرض CubeIDE فقط **`_Min_Stack_Size = 0x400` (۱KB)** است ⇒
MSP از پایین ناحیه‌ی رزروشده بیرون می‌زند، داده‌های استاتیک/هیپ را له
می‌کند و کمی بعد HardFault می‌گیرد. (قبل از patch این شاخه، ۳۲۰ بایت هم
بیشتر بود.)

### اقدام (مهم‌ترین قدم)

در `STM32H743…_FLASH.ld`:

```ld
_Min_Heap_Size  = 0x1000;   /* 4KB */
_Min_Stack_Size = 0x2000;   /* 8KB  (پیش‌فرض 0x400 است!) */
```

و برای اطمینان، در `HardFault_Handler` مقدار MSP را چک کنید — اگر خیلی
زیر `&_estack - _Min_Stack_Size` بود، یعنی overflow:

```c
void HardFault_Handler(void)
{
    volatile uint32_t *st = (uint32_t *)__get_MSP();
    /* stacked: R0 R1 R2 R3 R12 LR PC xPSR */
    FM_Log("HF CFSR=%08lX HFSR=%08lX BFAR=%08lX\n",
           (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
           (unsigned long)SCB->BFAR);
    FM_Log("HF PC=%08lX LR=%08lX MSP=%08lX\n",
           (unsigned long)st[6], (unsigned long)st[5],
           (unsigned long)__get_MSP());
    while (1) { }
}
```

- `CFSR = 0x00008200` (PRECISERR + BFARVALID) + BFAR بی‌معنی → معمولاً
  pointer خراب (نتیجه‌ی له‌شدن داده).
- `HFSR.FORCED=1` با CFSR بنچ‌مارک‌نشده → imprecise bus fault؛ رایج در
  وقتی DMA به آدرس غیرقابل‌دسترس (DTCM) می‌رود.

## ۴) اگر نوشتن روی SD خطا/فایل صفر بایتی داد: diskio

- **ریشه‌ی مشکل روی این برد پیدا شد:** در `FATFS/Target/sd_diskio.c`
  مقدار `ENABLE_SD_DMA_CACHE_MAINTENANCE` باید با وضعیت D-Cache هم‌خوان
  باشد:

  | D-Cache | ENABLE_SD_DMA_CACHE_MAINTENANCE |
  |---|---|
  | خاموش (وضعیت این پروژه — `SCB_EnableDCache` در main نیست) | **0** |
  | روشن | **1** |

  وقتی D-Cache خاموش است ولی maintenance=1 است، مسیر نوشتنِ sector داده
  خطا می‌دهد (نتیجه‌اش `greeting.txt` صفر بایتی بود: entry پوشه نوشته
  می‌شد ولی data sector نه).
- در `sd_diskio.c` اگر `SD_write` از `HAL_SD_WriteBlocks_DMA` استفاده
  می‌کند و `.bss` در DTCM است: IDMAی SDMMC به DTCM دسترسی ندارد؛ یا
  data را به AXI SRAM منتقل کنید یا polling کنید. بافرهایی هم که به
  DMA می‌روند باید ۴-بایتی (ترجیحاً ۳۲-بایتی) align باشند.
- مسیر جایگزین کامل (بدون sd_diskio.c کلاً): `examples/sdcard_fm_diskio.c`
  — diskio خود FileManager با polling. **نکته:** در `FM_BlockDeviceConfig`
  حتماً `cfg.present = FM_TRUE` بدهید وگرنه `Storage_Mount` با
  `FM_ERR_NOT_READY (-25)` رد می‌شود.
- با D-Cache روشن، در diskio قبل از نوشتن `SCB_CleanDCache_by_Addr` و
  بعد از خواندن `SCB_InvalidateDCache_by_Addr` لازم است.

## ۵) نکته‌هایی که از خروجی سریال شما خواندیم

- **یک‌شبه از کار افتادن همه‌ی مثال‌های RAM** (خروجی سوم): علت، کتابخانه
  نبود؛ متادیتای FMFS روی SDRAM («RAM disk» شما) از جلسات قبلی — ریست
  وسط `fmfs_save` در crash-loopها و جابه‌جایی چیدمان `.sdram_buffers` —
  «معتبر اما پاره» مانده بود (`fmfs_mount` → `FM_ERR_CORRUPTED`) و چون
  `FileManager_MountMemory` چک نمی‌شد، همه‌ی فراخوانی‌های بعدی بی‌صدا
  fail می‌کردند. این حالت روی هاست بازسازی و رفع شد:
  `fmfs_mount` حالا روی رسانه‌ی volatile (RAM/SDRAM) متادیتای خراب را
  خودش format می‌کند (روی فلش غیر volatile همان خطا را برمی‌گرداند تا
  داده‌ی واقعی پاک نشود) و `fmfs_save` هدر را «آخر» می‌نویسد تا ریست
  وسط ذخیره، state سازگار قبلی را نگه دارد. تست رگرسیون:
  «FMFS auto-formats corrupted RAM metadata».
- `f_getfree -> 12` (FR_NOT_ENABLED) در ex_11 نسخه‌ی قبل: ترتیب اشتباه
  بود — بعد از `f_mount(NULL,…,1)` در test_sdcard، درایو 0 ثبت نیست و
  f_getfree خام قبل از FS_Mount کار نمی‌کند. نسخه‌ی جدید: اول
  `FS_Mount("/")` بعد اعتبارسنجی با `FS_GetInfo` (بدون نیاز به ff.h).
- **greeting.txt با حجم صفر**: یعنی نوشتن entry پوشه (metadata) موفق
  بوده ولی نوشتن data sector انجام نشده — به مسیر `SD_write` در
  sd_diskio.c مشکوک است (معمولاً DMA). مطمئن‌ترین راه: مثال ۱۲
  (`examples/sdcard_fm_diskio.c`) — diskio خود FileManager با
  polling: `FM_FATFS_PROVIDE_DISKIO=1`، حذف `sd_diskio.c` از build، و
  `ex_12_sd_diskio_init()` اولِ main (قبل از test_sdcard).
- `Lines: zu` و `( bytes)` خالی: `vsnprintf` لینک‌شده در پروژه‌ی شما
  `%zu` را پشتیبانی نمی‌کند (و `%1l` در ex_9 غلط تایپی است — باید
  `%lu` باشد). نسخه‌ی فعلی `fm_uart_log.c` این ریپو vsnprintf استاندارد
  newlib را صدا می‌زند؛ فایل قدیمی را جایگزین کنید یا از `%lu` با cast
  استفاده کنید.
- **بریدن خروجی وسط جمله** (مثل محتوای CSV در ex_8 که روی «66.» قطع
  می‌شود): بافر داخلی `FM_Log` فقط ۲۵۶ بایت است (`FM_LOG_BUF_SIZE`)؛
  رشته‌های بلندتر truncate می‌شوند. قبل از `#include "fm_uart_log.h"`
  بگذارید `#define FM_LOG_BUF_SIZE 1024` یا متن را تکه‌تکه چاپ کنید.
- فایل‌های باقی‌مانده در ex_4 بین اجراها همان ماندگاری SDRAM است؛ با
  auto-format جدید دیگر گلودنگ نمی‌شود، ولی اگر خروجی تمیز می‌خواهید
  `memset(g_storage, 0, sizeof(g_storage))` را یک‌بار در اول بوت بزنید.
- `FileManager_Init` دوبار صدا زدن بی‌ضرر است (فقط آخرین خطا را ریست
  می‌کند).

## ۶) دسترسی مستقیم برنامه به داده‌های Media

`examples/media_data_io.h` یک نمونهٔ function-oriented برای برنامه‌های
bare-metal/super-loop است. همهٔ بافرها را برنامه تأمین می‌کند و wrapper هیچ
بافر پنهانی allocate نمی‌کند:

- `Ex14_AudioReadMono` خروجی PCM16 تک‌کاناله و
  `Ex14_AudioReadStereo` خروجی interleaved به‌شکل `L,R,L,R,...` می‌دهد؛
  ورودی Mono در خروجی Stereo روی هر دو کانال تکرار می‌شود.
- `Ex14_AudioSavePCM16` برای ذخیرهٔ یک‌مرحله‌ای WAV است؛ برای دادهٔ زنده یا
  بزرگ از `Ex14_AudioWriterBegin/WriteFrames/End` استفاده کنید.
- `Ex14_VideoReadRGB888` هر frame را در RGB888 متعلق به caller decode می‌کند؛
  writer داخلی ویدئو FMRV خام است و encoder مربوط به AVI/MP4 ادعا نمی‌شود.
- تصویر built-in از نوع BMP است: load/save به RGB888، resize با nearest یا
  bilinear، و افکت‌های invert، grayscale، sepia، brightness و contrast.

همان lifecycle مشترک SD را نگه دارید: یک‌بار `SD_examples_begin()`، سپس
مثال‌های SD/Media/Player و در پایان فقط یک‌بار `SD_examples_end()`.

برای Example 13، تا وقتی DMA Player فعال است `SD_examples_end()` را اجرا
نکنید. نیمهٔ 1024-sample در 48kHz stereo فقط 10.7ms مهلت refill می‌دهد؛ مقدار
شروع مناسب‌تر برای SD و build دیباگ `8192` نمونه در هر نیمه (85.3ms) است.
مسیر PCM WAV اکنون نمونه‌ها را با یک read گروهی می‌خواند و file cache نیز
خواندن‌های بزرگ Media را مستقیم به FatFS می‌دهد؛ در نتیجه یک درخواست چند
کیلوبایتی به چندین read و seek تک‌سکتوری شکسته نمی‌شود.

### پخش MP3 بدون خش/underrun

کلید روانی MP3 (مثل WAV) **تعداد** فرمان‌های SDMMC در هر refill است، نه
حجم بایت: هر `File_Read` یک فرمان جداگانهٔ CMD18 است و هر فرمان هم
overhead ثابت دارد و هم شانس یک stall موقتِ کارت (10-200ms روی کارت‌های
کهنه/ارزان). تغییرات:

- `mp3_decode_one()` در `src/fm_audio.c` حالا با **read-ahead بلوکی** کار
  می‌کند: وقتی باقیماندهٔ بافر ورودی از `FM_MP3_LOW_WATER` (پیش‌فرض
  `FM_MP3_INPUT_BUF/4`) کم شود، کل بافر در **یک** `File_Read` پر می‌شود —
  یعنی هر refill به‌جای «یک read برای هر فریم MP3» (~15 read برای نیمهٔ
  32767 در 48kHz) فقط چند read بزرگ سریال انجام می‌دهد (با بافر 8KB برای
  آهنگ ~70kbps: ~3-4 read). اندازهٔ `FM_MP3_INPUT_BUF` حالا واقعاً مؤثر
  است: با 32KB (هزینهٔ RAM: `FM_MAX_AUDIO × 32KB`) اغلب کل refill در بافر
  جا می‌شود و refill تقریباً بدون read از SD می‌شود.
- `Audio_Mp3Stats()/Audio_Mp3StatsReset()` تعداد readها، بایت‌ها، فریم‌ها و
  بایت‌های resync را می‌دهند. برای سنجش در super-loop:

      Audio_Mp3StatsReset(&player_audio);
      Ex13_AudioDmaPoll(&player);              // یک refill
      Audio_Mp3Stats(&player_audio, &s);
      // s.read_calls باید تک‌رقم بماند؛ s.frames ≈ 14 در نیمهٔ 48kHz

- decode با `-O2` و FPU سخت‌افزاری (Single-Precision در CubeMX) بسازید؛
  soft-float decode را چند برابر کندتر می‌کند. اگر D-Cache را فعال کنید،
  `port.clean_cache` را مطابق Example 13 بگذارید تا DMA دادهٔ تازه ببیند.

## ۷) SimpleAudio — «صدا ساده»: پخش + داده + ضبط در یک header

`examples/simple_audio.h` لایهٔ «جعبه‌سیاه» روی Example 13 است:
WAV/MP3 و mono/stereo را **خودکار از هدر** تشخیص می‌دهد و شما فقط
«دکمه‌ها» را صدا می‌زنید. مثل adapter قدیمی (`test_audio_player.h`) فقط در
main.c یک‌بار include می‌شود (callbackهای HAL_I2S را تعریف می‌کند) —
جایگزین مستقیم همان adapter است:

| دکمه | کار |
|---|---|
| `SimpleAudio_Begin()` | اسکن پوشه + پخش اولین فایل + I2S/DMA |
| `SimpleAudio_Poll()` | داخل `while(1)` — refill، تعویض خودکار ترک، آمار، auto-restart |
| `SimpleAudio_Stop()` / `SimpleAudio_IsRunning()` | ⏹ توقف کامل / وضعیت |
| `SimpleAudio_Next()` / `SimpleAudio_Prev()` | ⏭⏮ **فوری** (نه انتهای ترک) |
| `SimpleAudio_Play("/music/x.mp3")` | پخش یک فایل مشخص |
| `SimpleAudio_SeekTo(30.0)` / `SimpleAudio_SeekBy(-10.0)` | ⏩⏪ پرش **فوری** (منفی = عقب) |
| `SimpleAudio_SetVolume(50)` | صدای نرم‌افزاری 0–100٪ |
| `SimpleAudio_Info(&i)` | اسم، کدک، فرکانس، کانال، طول، موقعیت، صدا، لیست |
| `SimpleAudio_DataRead(buf, frames, &got)` | دیتای موزیک استریو (L,R) — همان استریم بلندگو |
| `SimpleAudio_DataReadMono(buf, n, &got)` | همان ولی مونو (میانگین L/R) |
| `SimpleAudio_RecStart("/rec", rate, ch)` | «تابهٔ ضبط»: پوشه (نام خودکار) یا آدرس دقیق .wav |
| `SimpleAudio_RecWrite(pcm, frames)` | هر چقدر data داری بده |
| `SimpleAudio_RecStop()` / `SimpleAudio_RecPath()` | ذخیره (رفع هدر WAV) + آدرس نهایی |

```c
#include "simple_audio.h"        /* یک‌بار در main.c */
...
SD_examples_begin();
SimpleAudio_Begin();
while (1)
{
    SimpleAudio_Poll();
    if (Key_Pressed(KEY_NEXT))  SimpleAudio_Next();
    if (Key_Pressed(KEY_BACK))  SimpleAudio_SeekBy(-10.0);

    /* ضبط از هر منبع: داده را به تابه بده، خودش حفظ می‌کند */
    SimpleAudio_RecStart("/rec", 16000, 1);
    while (mic_has_data()) SimpleAudio_RecWrite(mic_buf, frames);
    SimpleAudio_RecStop();                    /* -> /rec/rec_000001.wav */
}
```

تفاوت‌ها با adapter قدیمی:
- **فایل mono هم پخش می‌شود** (upmix خودکار L=R روی I2S استریو؛ قبلاً
  mono رد می‌شد).
- Next/Prev/Seek **فوری**اند: DMA متوقف → بافرها از موقعیت جدید پر → شروع.
- همهٔ مدیریت خطا/restart/آمار درون `Poll()` است — main.c فقط دکمه‌ها.
- RAM: همان بافر DMA قبلی (N=32767 → ~128KB در سکشن `.dma_buffer` —
  بدون تغییر linker)؛ با `SIMPLE_AUDIO_I2S_REC=1` (میکروفون روی خط I2S)
  +16KB برای بافر RX اضافه می‌شود و callbackهای RX خودکار داده را به
  تابهٔ ضبط می‌رسانند.

## ۷‑ب) SimpleImage / SimpleVideo — همان سبک «دکمه بزن»

`examples/simple_image.h` و `examples/simple_video.h`
مثل SimpleAudio جعبه‌سیاه‌اند. بافر پیکسل را در SDRAM بگذارید (سکشن
`.sdram`، شما 32MB دارید). خروجی پیش‌فرض TFT: RGB565.

**عکس — آلبوم `/photos`، BMP + JPEG + PNG:**

```c
#include "simple_image.h"
#include "hw_jpeg.h"     /* اختیاری: JPEG Codec سخت‌افزار */

MX_JPEG_Init();
SimpleImage_UseHwJpeg(&hjpeg);
SimpleImage_SetPresent(my_lcd_blit); /* SPI DMA/polling یا LTDC — مال برد */
SimpleImage_SetOutputSize(320, 240);
SimpleImage_Begin();
/* در while(1): SimpleAudio_Poll(); SimpleImage_Poll(); */
```

FileManager درایور ILI9341 نیست. `present` را برنامه می‌نویسد.
برای SPI: `SIMPLE_IMAGE_SWAP16 1` و معمولاً `FM_PIXEL_BGR565`.
پنجره: `Set_Address(0,0,w-1,h-1)` و همان `bytes` از callback.

بدون example 20، JPEG با picojpeg نرم‌افزاری decode می‌شود. PNG
(۸-بیت gray/RGB/RGBA/palette، غیر interlaced) built-in است.

**SPI DMA روی STM32H7 (ویدیو، نه عکس):** polling 1KB برای آلبوم کافی است.
سود DMA این است که CPU همزمان فریم بعد را decode کند. `examples/spi_tft_dma.h`
هلپر جنریک است (درایور ILI9341 نیست):

- DMA1/DMA2 از FMC SDRAM (`0xC0…`) و DTCM (`0x2000…`) نمی‌خواند — bounce
  در AXI-SRAM (`0x24000000`) یا D2 (`0x30000000`).
- `HAL_SPI_Transmit_DMA` هر تکه SPI را ریست می‌کند و روی H7 مرز تکه
  یک بایت جابه‌جا می‌شود. هلپر TSIZE=0 می‌گذارد تا SPI تا آخر فریم روشن بماند.
- استریم DMA در `stm32h7xx_it.c` لازم است. FIFO DMA را روشن کنید.
- `LcdSpiDma_Send` برای عکس (blocking)؛ برای ویدیو `Begin` + `Poll`.
- تعویض عکس I2S را Pause نمی‌کند (کلیک اسپیکر از DMAPause/Resume).
  وسط SPI هم Poll/SD صدا را قاطی نمی‌کند (`LCD_SPI_DMA_IDLE 0`).
- LVGL: همان `Send` را در `flush_cb` برای ناحیهٔ dirty صدا بزن؛ FileManager
  درایور LVGL نیست.

```c
#include "spi_tft_dma.h"
LcdSpiDma_Bind(&hspi2, LCD_CS_GPIO_Port, LCD_CS_Pin);
/* در HAL_SPI_TxCpltCallback / ErrorCallback: LcdSpiDma_OnTxCplt/OnError */
```

**ویدئو — آلبوم `/videos` + صدا از I2S:**

| کانتینر | ویدیو | صدا از بلندگو |
|---|---|---|
| FMRV | خام RGB888 (خواندن+نوشتن) | ندارد |
| AVI | MJPEG یا H.264 | PCM 16-bit (با `SIMPLE_VIDEO_I2S 1`) |
| MP4 | H.264 | AAC فقط demux — decoder AAC نیست، بی‌صدا |

```c
#define SIMPLE_VIDEO_I2S 1
#define SIMPLE_VIDEO_FORMAT FM_PIXEL_BGR565   /* همان عکس */
#include "simple_audio.h"
#include "simple_video.h"
#include "spi_tft_dma.h"

LcdSpiDma_Bind(&hspi2, LCD_CS_GPIO_Port, LCD_CS_Pin);
SimpleVideo_SetPresent(my_lcd_blit);
SimpleVideo_SetOutputSize(320, 240);
SimpleAudio_Begin();   /* یا SimpleVideo_Begin() — یکی I2S را می‌گیرد */
while (1) {
    (void)LcdSpiDma_Poll();
    (void)SimpleAudio_Poll();
    (void)SimpleVideo_Poll();
}
SimpleVideo_Pause(); SimpleVideo_SeekBy(+10.0); SimpleVideo_Next();
```

هر دو را می‌توانید include کنید. `Begin` موزیک، ویدیو را Stop می‌کند (و برعکس) چون یک `hi2s1` است. Poll هر دو بی‌ضرر است: غیرفعال `NOT_READY` می‌دهد.

نویسندهٔ built-in: FMRV خام. encode به AVI/MP4 ادعا نمی‌شود.
H.264 از `FM_Alloc` هم استفاده می‌کند — allocator را روی SDRAM بگذارید.

## ۸) QSPI W25Q256FV و نمایش حافظه روی USB

### QSPI

1. در CubeMX، QUADSPI و پایه‌های Clock/NCS/IO را فعال کنید.
2. `MX_QUADSPI_Init()` باید قبل از FileManager اجرا شده باشد.
3. `examples/qspi_w25q.h` را include کنید. هندسهٔ پیش‌فرض:
   32MB، page برابر 256B و erase sector برابر 4KB است.
4. اگر SD روی FatFS drive 0 است، `FF_VOLUMES >= 2` و
   `EX15_QSPI_FATFS_DRIVE=1` تنظیم کنید.
5. `QSPI_examples_begin()` از `examples/examples_qspi.h` را اجرا کنید؛
   سپس مثال‌های `exQ_1` تا `exQ_10` دقیقاً روی `/qspi` کار می‌کنند.
   مثال‌های کامل Audio/Video/Image در
   `examples/examples_qspi_media.h` و مثال‌های Player در
   `examples/examples_qspi_player.h` قرار دارند.
6. flash نو را فقط یک‌بار و با فرمان صریح کاربر توسط
   `QSPI_examples_format()` فرمت کنید؛ format خودکار نیست تا داده اشتباهی
   پاک نشود.

لایهٔ `FM_NorBlockDevice` بازنویسی 512 بایتی FatFS را به
read-modify-erase-program چهار کیلوبایتی تبدیل می‌کند و scratch آن متعلق به
برنامه است. برای workload با نوشتن بسیار زیاد، FTL تخصصی مناسب‌تر است.

### USB Mass Storage برای SD، RAM یا QSPI

1. حافظه باید FAT باشد؛ Windows مستقیماً FMFS/LittleFS را نمی‌شناسد.
2. همهٔ handleهای فایل MCU را ببندید و filesystem محلی را unmount کنید.
3. با `Ex16_USB_Begin()` مالکیت volume را به PC بدهید.
4. سپس `MX_USB_DEVICE_Init()` را اجرا کنید. callbackهای دقیق
   `usbd_storage_if.c` در ابتدای `examples/usb_msc.h` آمده‌اند.
5. هنگام PC mode هیچ `File_*` روی همان volume اجرا نکنید.
6. در PC از Eject/Safely Remove استفاده کنید، بعد `USBD_Stop()` و سپس
   `Ex16_USB_End()` را اجرا کنید تا volume برای MCU دوباره mount شود.

برای SD polling، getter و callbackهای مالکیت به Example 12 افزوده شده‌اند؛
برای QSPI callback آماده و برای RAM یک FAT RAM-disk کامل در Example 16 وجود
دارد.

## ۹) تست هاست (بدون برد)

```sh
sh test/build_fatfs_host.sh   # ASan+UBSan روی backends/fm_fatfs.c
make test                     # ۱۱۹ تست کامل کتابخانه
```
