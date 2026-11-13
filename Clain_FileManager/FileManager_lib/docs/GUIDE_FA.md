# FileManager — راهنمای کتابخانه

کتابخانهٔ C برای STM32H7: فایل، حافظه، موزیک، ویدیو و تصویر، هر کدام با چند
تابع ساده.

| | |
|---|---|
| سخت‌افزار مرجع | STM32H743، کارت SD، فلش QSPI، SDRAM، خروجی I2S، TFT با SPI |
| محیط | STM32CubeIDE (HAL) |
| زبان | C11، بدون RTOS (حلقهٔ اصلی) |
| آزمون | بیش از ۵۰۰ تست خودکار روی PC با `make check` |

---

## ۱. چه کارهایی می‌کند

- **یک API برای همهٔ حافظه‌ها.** کارت SD (FAT32)، فلش QSPI، فلش SPI،
  EEPROM و RAM disk. همان `File_...` روی همه کار می‌کند؛ فقط مسیر فرق دارد.
- **فایل و پوشه.** خواندن، نوشتن، کپی، انتقال، تغییر نام، فهرست، کپی کل
  پوشه از یک حافظه به حافظهٔ دیگر.
- **متن، JSON، لاگر داده، نوشتن امن** در برابر قطع برق.
- **موزیک.** WAV، MP3، FLAC، Ogg Vorbis و Opus روی I2S با DMA. بعدی، قبلی،
  پرش، مکث، ولوم تا ۲۰۰٪، چهار حالت تکرار.
- **ویدیو.** AVI، MP4 و MOV با MJPEG یا H.264، صدا و تصویر هماهنگ.
- **تصویر.** BMP، JPEG و PNG. اسلایدشو، چرخش، برعکس، تغییر اندازه، افکت،
  ذخیره.
- **ذخیرهٔ صدا و ویدیو.** صدا به WAV، ADPCM یا FLAC؛ ویدیو به MJPEG در AVI.
  از دادهٔ خودتان، از موزیکِ در حال پخش، یا فایل به فایل با پردازش شما.
- **دادهٔ خام در دست شما.** نمونه‌های صدا و فریم‌های تصویر با یک کالبک به
  برنامه می‌رسند، با پخش یا بدون پخش.
- **USB.** کارت SD مثل یک فلش‌مموری روی کامپیوتر دیده می‌شود.
- **فقط آنچه لازم دارید.** هر بخش با یک خط خاموش می‌شود و از فلش بیرون
  می‌رود.

---

## ۲. شروع سریع

### ۲.۱ جای فایل‌ها

| پوشه | محتوا |
|---|---|
| `FileManager_lib/include` | هدرهای کتابخانه و تنظیمات (`fm_board_config.h`) |
| `FileManager_lib/src`، `backends`، `third_party` | کد کتابخانه و کدک‌ها |
| `FileManager_lib/top_api` | لایهٔ ساده: `simple_audio.h`، `simple_video.h`، `simple_image.h`، حافظه‌ها |
| `FileManager_lib/board` | کد همین برد: نمایشگر، دستورهای UART، ترتیب اجرا |
| `Core/Src/main.c` | ترتیب راه‌اندازی و حلقهٔ اصلی |

هدرهای `top_api` فقط یک‌بار و فقط در `main.c` شامل می‌شوند (از راه
`Core/Src/defines.h`).

### ۲.۲ کمترین برنامه

```c
/* بعد از MX_..._Init ها */
FM_Log_UseHalUart(&huart1);     /* لاگ روی UART */
FM_SD_Hal_Attach(&hsd1);        /* درایور کارت SD */
UsbSd_FatRoot();                /* "/" = کارت SD */
FileManager_Init();             /* خود کتابخانه */

File_WriteText("/hello.txt", "salam");

SimpleAudio_Begin();            /* پخش پوشهٔ /music */

while (1)
{
    SimpleAudio_Poll();         /* هر پلیر یک Poll در حلقه */
}
```

### ۲.۳ سه قاعده

1. **همهٔ توابع `FM_Result` برمی‌گردانند.** `FM_OK` (صفر) یعنی موفق؛ عدد
   منفی یعنی خطا. متن خطا: `File_GetErrorString(r)`.
2. **مسیرها با `/` شروع می‌شوند** و حافظه را مشخص می‌کنند: `/music/a.mp3`
   روی SD، `/qspi/config.json` روی فلش QSPI.
3. **پلیرها در حلقهٔ اصلی کار می‌کنند.** `Begin` یک‌بار، `Poll` در هر دور
   حلقه. توابع کنترلی (Next، Pause، ...) فقط درخواست ثبت می‌کنند و از هر
   جایی قابل صدا زدن‌اند.

```c
FM_Result r = File_WriteText("/a.txt", "x");
if (r != FM_OK)
{
    printf("error: %s\n", File_GetErrorString(r));
}
```

**جواب `FileManager_Init()` را هم نگاه کنید.** اگر کتابخانه جدا کامپایل شده
(مثلاً `libfilemanager.a`) و برنامهٔ شما با گزینه‌های دیگری ساخته شده باشد
(`FM_PATH_MAX`، `FM_NAME_MAX` یا `FM_USE_...` متفاوت)، اندازهٔ ساختارها در
دو طرف یکی نیست. `FileManager_Init()` این را می‌سنجد و `FM_ERR_INTERNAL`
(‎-26) برمی‌گرداند و هیچ کاری نمی‌کند. در پروژهٔ CubeIDE که همه‌چیز با هم
کامپایل می‌شود این حالت پیش نمی‌آید.

### ۲.۴ سه تنظیم پروژهٔ CubeMX که کتابخانه به آن‌ها وابسته است

| تنظیم | کجا | مقدار | چرا |
|---|---|---|---|
| `_FS_LOCK` | `FATFS/Target/ffconf.h` | ۱۶ (یا ۰؛ کمتر از ۱۲ نه) | CubeMX مقدار ۲ می‌نویسد: سومین فایل یا پوشهٔ باز خطا می‌دهد و مثلاً `Directory_Copy` با `FM_ERR_NO_MEMORY` می‌ایستد. با مقدار کمتر از ۱۲ پروژه با `#error` کامپایل نمی‌شود. |
| اولویت تیک ۱ میلی‌ثانیه | `HAL_InitTick(0U)` در `Core/Src/main.c` | ۰ | وقتی کارت از داخل وقفهٔ USB خوانده می‌شود، مهلت‌های درایور SD با همین تیک شمرده می‌شوند؛ تیکی که نتواند وقفهٔ USB را قطع کند هیچ‌وقت نمی‌شمارد و برد قفل می‌شود. |
| اولویت `OTG_FS_IRQn` | `USB_DEVICE/Target/usbd_conf.c` | ۶ | عددِ بزرگ‌تر از تیک (۰) و از `SDMMC1_IRQn` (۱)، به همان دلیل. |

**مهم:** خط `_FS_LOCK` بیرون از بلوک‌های `USER CODE` است. هر بار که CubeMX
پروژه را دوباره تولید کند، مقدار به ۲ برمی‌گردد و بیلد با `#error` می‌ایستد.
مقدار `FS_LOCK` را در تنظیمات FATFS خودِ CubeMX هم ۱۶ بگذارید. دو خط اولویت
داخل بلوک `USER CODE` هستند و می‌مانند. `sh tools/check_board.sh` (بخشی از
`make check`) دو ردیف اول را بررسی می‌کند. هیچ‌کدام روی برد آزمایش نشده است.

---

## ۳. روشن و خاموش کردن بخش‌ها

همه در یک فایل: **`include/fm_board_config.h`**، بلوک «قابلیت‌ها» در ابتدای
فایل. هر خط با `//` شروع شده است:

```c
//#define FM_USE_OPUS  0      /* روشن (پیش‌فرض) */
#define FM_USE_OPUS  0        /* خاموش: «//» را بردارید */
```

بعد از تغییر: **Clean** و **Build**.

| خط | چه چیزی خاموش می‌شود | فلش آزادشده (تقریبی) |
|---|---|---:|
| `FM_USE_AUDIO 0` | کل صدا | ۲۹۱ KB |
| `FM_USE_VIDEO 0` | کل ویدیو | ۱۵۱ KB |
| `FM_USE_IMAGE 0` | کل تصویر | ۳۱ KB |
| `FM_USE_OPUS 0` | Opus | ۱۶۰ KB |
| `FM_USE_H264 0` | H.264 | ۸۱ KB |
| `FM_USE_VORBIS 0` | Ogg Vorbis | ۴۴ KB |
| `FM_USE_FLAC 0` | FLAC | ۳۷ KB |
| `FM_USE_MP3 0` | MP3 | ۳۵ KB |
| `FM_USE_JSON 0` | JSON | ۱۴ KB |
| `FM_USE_PNG 0` / `FM_USE_JPEG 0` | PNG / JPEG | ۱۰ KB هر کدام |
| `FM_USE_DATA_LOGGER 0` | لاگر داده | ۸ KB |
| `FM_USE_TEXT 0` | ویرایش خط‌به‌خط متن | ۴ KB |
| `FM_USE_SAFE_WRITE 0` | نوشتن امن | ۲ KB |

بخش خاموش کامپایل نمی‌شود. با خاموش کردن صدا، ویدیو یا تصویر، `main.c`
بدون تغییر ساخته می‌شود؛ توابع `SimpleVideo_...` در این حالت
`FM_ERR_UNSUPPORTED` برمی‌گردانند.

اجرای دموها و پلیرها با خط‌های `App_...` در `main.c` و پرچم‌های
`Core/Src/defines.h` کنترل می‌شود. جزئیات کامل، اندازهٔ بافرها و کش:
[`OPTIONS_FA.md`](OPTIONS_FA.md).

---

## ۴. حافظه‌ها

| حافظه | مسیر | راه‌اندازی | پایان |
|---|---|---|---|
| کارت SD (FAT32) | `/` | `UsbSd_FatRoot()` | – |
| فلش QSPI (W25Q...) | `/qspi` | `QspiFlash_Begin()` | `QspiFlash_End()` |
| فلش SPI | `/flash` | `SpiFlash_Begin(...)` | `SpiFlash_End()` |
| EEPROM روی I2C | آدرس بایت | `EepromSimple_Begin(...)` | `EepromSimple_End()` |

مدل فلش از روی JEDEC ID خودکار شناخته می‌شود.

چند قاعده:

- `Storage_Unmount` و `Storage_Format` تا وقتی فایل‌سیستمی روی آن حافظه مونت
  است `FM_ERR_BUSY` می‌دهند. اول `FS_Unmount`؛ برای خالی کردن حافظهٔ مونت‌شده
  `FS_Format`.
- `FS_Format` حافظه را خالی و **مونت‌شده** تحویل می‌دهد. LittleFS‌ای که اصلاً
  مونت نمی‌شود (فرمت خودکار خاموش، فلش خام یا خراب) مسیر مونت ندارد:
  `LittleFS_FormatStorage(&storage)` و بعد `FS_Mount` با همان `FM_FS`.
- نوشتن روی QSPI فقط وقتی لازم باشد پاک می‌کند: بایتی که همان مقدار را دارد
  هیچ هزینه‌ای ندارد، بایتی که هنوز `0xFF` است بدون پاک‌کردن نوشته می‌شود،
  و فقط نوشتن روی دادهٔ دیگر یک سکتور را پاک و بازنویسی می‌کند.
- کشِ رسانه روی فلش (`play_from_flash.h`) وقتی پر شود **بزرگ‌ترین** فایل را
  بیرون می‌اندازد، نه قدیمی‌ترین را. فایلی که همان لحظه باز است (در حال پخش)
  حذف نمی‌شود و نوبت به بزرگ‌ترینِ بعدی می‌رسد.

**کارت SD روی کامپیوتر (USB):**

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `UsbSd_ShowToPc()` | کارت را به کامپیوتر می‌دهد | – | کارت از دست برنامه خارج می‌شود |
| `UsbSd_TakeBack()` | کارت را پس می‌گیرد و مونت می‌کند | – | `/` دوباره در دسترس |
| `UsbSd_IsWithPc()` | کارت الان دست کامپیوتر است؟ | – | `FM_TRUE` / `FM_FALSE` |
| `UsbSd_Poll()` | سرویس USB | در حلقهٔ اصلی | – |
| `UsbSd_CopyFromQspi(src, dst)` | یک فایل را از QSPI به SD می‌آورد | مسیر روی `/qspi` | فایل روی SD |

قبل از `UsbSd_ShowToPc()` پخش را متوقف کنید؛ قبل از `UsbSd_TakeBack()` در
ویندوز Eject بزنید.

**EEPROM** (بعد از `EepromSimple_Begin`، شیء آماده `g_ee` است):

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `EEPROM_Read(&g_ee, addr, buf, len)` | خواندن | آدرس در EEPROM | `buf` |
| `EEPROM_Write(&g_ee, addr, buf, len)` | نوشتن | `buf` | EEPROM |
| `EEPROM_Update(&g_ee, addr, buf, len)` | فقط بایت‌های عوض‌شده را می‌نویسد (عمر بیشتر) | `buf` | EEPROM |

---

## ۵. فایل و پوشه

### ۵.۱ یک‌خطی‌ها (بدون باز و بسته کردن)

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `File_WriteText(path, text)` | فایل را با متن می‌سازد یا جایگزین می‌کند | رشته | فایل |
| `File_AppendText(path, text)` | متن را به آخر فایل اضافه می‌کند | رشته | فایل |
| `File_ReadText(path, buf, size, &n)` | کل فایل را به‌صورت رشته می‌خواند | فایل | `buf` |
| `File_WriteAll(path, data, size)` | دادهٔ باینری را می‌نویسد | بافر | فایل |
| `File_AppendAll(path, data, size)` | دادهٔ باینری را اضافه می‌کند | بافر | فایل |
| `File_ReadAll(path, buf, size, &n)` | کل فایل را می‌خواند | فایل | `buf` |
| `File_Copy(src, dst, NULL, NULL)` | کپی، حتی بین دو حافظه | فایل مبدأ | فایل مقصد |
| `File_Move(src, dst, NULL, NULL)` | انتقال | فایل مبدأ | فایل مقصد |
| `File_Rename(old, new)` | تغییر نام | – | – |
| `File_Delete(path)` | حذف | – | – |
| `File_Exists(path, &yes)` | هست یا نه | – | `yes` |
| `File_SizeOf(path, &size)` | اندازه به بایت | – | `size` |

```c
char text[64];
size_t n;

File_WriteText("/note.txt", "line 1\n");
File_AppendText("/note.txt", "line 2\n");
File_ReadText("/note.txt", text, sizeof(text), &n);
File_Copy("/note.txt", "/qspi/note.txt", NULL, NULL);   /* SD -> QSPI */
```

### ۵.۲ کار با دستهٔ فایل (فایل بزرگ، تکه‌تکه)

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `File_Open(path, mode)` | باز می‌کند | مسیر + حالت | `FM_File *` (یا `NULL`) |
| `File_Read(f, buf, len, &n)` | می‌خواند | فایل | `buf` |
| `File_Write(f, buf, len, &n)` | می‌نویسد | `buf` | فایل |
| `File_Seek(f, offset, origin)` | جابه‌جایی در فایل | – | – |
| `File_Tell(f, &pos)` / `File_Size(f, &size)` | موقعیت / اندازه | – | عدد |
| `File_Flush(f)` | داده را روی حافظه می‌نشاند | – | – |
| `File_Close(f)` | می‌بندد | – | – |

حالت‌ها: `FM_OPEN_READ`، `FM_OPEN_WRITE`، `FM_OPEN_CREATE`،
`FM_OPEN_APPEND`، `FM_OPEN_TRUNCATE` (با `|` ترکیب می‌شوند).

```c
uint8_t  block[512];
uint32_t n;
FM_File *f = File_Open("/data.bin", FM_OPEN_READ);

if (f != NULL)
{
    while (File_Read(f, block, sizeof(block), &n) == FM_OK && n > 0)
    {
        /* block[0..n) */
    }
    File_Close(f);
}
```

### ۵.۳ پوشه

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `Directory_Create(path)` | پوشه می‌سازد (با پوشه‌های میانی) | – | – |
| `Directory_Delete(path)` | پوشهٔ خالی را حذف می‌کند | – | – |
| `Directory_DeleteRecursive(path)` | پوشه و هرچه در آن است | – | – |
| `Directory_Exists(path, &yes)` | هست یا نه | – | `yes` |
| `Directory_List(path, filter, cb, user)` | برای هر مورد `cb` را صدا می‌زند | پوشه | کالبک شما |
| `Directory_Count(path, type, &count)` | تعداد موارد | پوشه | `count` |
| `Directory_Size(path, &bytes)` | حجم کل | پوشه | `bytes` |
| `Directory_Copy(src, dst, NULL, NULL)` | کپی کل پوشه، حتی بین دو حافظه | پوشهٔ مبدأ | پوشهٔ مقصد |
| `Directory_Move(src, dst, NULL, NULL)` | انتقال کل پوشه | پوشهٔ مبدأ | پوشهٔ مقصد |
| `Directory_Rename(old, new)` | تغییر نام / جابه‌جایی پوشه روی همان حافظه | پوشه | – |

- `Directory_Rename` فقط پوشه را تغییر نام می‌دهد: برای فایل
  `FM_ERR_NOT_DIRECTORY`، برای خودِ نقطهٔ مونت یا مقصدی داخل همان پوشه
  `FM_ERR_INVALID_ARGUMENT`، و اگر فایلی داخل آن باز باشد `FM_ERR_BUSY`.
- نام فایل یا پوشه با ۶۴ نویسه یا بیشتر (`FM_NAME_MAX`) ساخته نمی‌شود:
  `FM_ERR_TOO_LARGE`. (چنین نامی قابل فهرست‌کردن نبود و پوشه‌اش دیگر پاک
  نمی‌شد.) برای کارت‌هایی که روی کامپیوتر پر می‌شوند `FM_NAME_MAX` را ۲۵۶
  کنید.
- تغییر نام روی FAT: `a.txt` به `A.TXT` همان فایل است و فقط نامش عوض
  می‌شود؛ تغییر نام روی فایلِ موجود، آن را جایگزین می‌کند؛ روی پوشه‌ای که
  خالی نیست `FM_ERR_NOT_EMPTY`.

```c
static FM_Result show(const FM_FileInfo *info, void *user)
{
    (void)user;
    printf("%s  %lu bytes\n", info->name, (unsigned long)info->size);
    return FM_OK;                 /* FM_ERR_CANCELLED = کافی است */
}

Directory_List("/music", NULL, show, NULL);
Directory_Copy("/qspi/project", "/backup/project", NULL, NULL);
```

---

## ۶. متن، JSON، لاگر، نوشتن امن

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `Text_AppendLine(path, line)` | یک خط به آخر فایل | رشته | فایل |
| `Text_ReadLine(f, line, size, &n)` | خط بعدی فایلِ باز | فایل | `line` |
| `Text_CountLines(path, &count)` | تعداد خط‌ها | فایل | `count` |
| `Text_Find(path, text, &line)` | اولین خطی که متن را دارد | فایل | شمارهٔ خط |
| `Text_Replace(path, old, new)` | جایگزینی متن در کل فایل | فایل | همان فایل |
| `Text_InsertLine(path, at, line)` / `Text_DeleteLine(path, at)` | افزودن / حذف یک خط | فایل | همان فایل |
| `Json_LoadFile(path, &doc, text, size, tokens, size)` | فایل JSON را باز می‌کند | فایل | `doc` |
| `Json_ReadInt / ReadFloat / ReadBool(&doc, key, &out)` | مقدار یک کلید | `doc` | `out` |
| `Json_ReadString(&doc, key, out, size)` | مقدار متنی یک کلید | `doc` | `out` |
| `JsonWriter_...` | JSON می‌سازد | مقدارها | بافر متن |
| `Logger_Init / Start / Write / Stop` | لاگر CSV با چرخش فایل | آرایهٔ `float` | فایل‌های پوشهٔ لاگ |
| `File_SafeReplace(path, data, size)` | جایگزینی امن: با قطع برق، یا فایل قدیم می‌ماند یا جدید | بافر | فایل |
| `File_Recover(path)` | بعد از قطع برق، فایل موقتِ جامانده را پاک می‌کند | – | – |

- `Text_Replace` / `Text_InsertLine` / `Text_DeleteLine` از فایل موقت
  `<path>.fm_tmp` استفاده می‌کنند؛ مسیری بلندتر از `FM_PATH_MAX - 8` نویسه
  (۱۵۲) `FM_ERR_TOO_LARGE` می‌دهد و فایل دست نمی‌خورد.
- `JsonWriter_Float` برای NaN و بی‌نهایت `null` می‌نویسد؛ `Logger_Write` در
  CSV `nan` / `inf` / `-inf` و در JSONL `null`.

```c
/* خواندن تنظیمات */
FM_Json  doc;
char     text[256];
uint32_t tokens[FM_JSON_TOKEN_BUFFER_SIZE / 4];
int32_t  baud;

Json_LoadFile("/config.json", &doc, text, sizeof(text), tokens, sizeof(tokens));
Json_ReadInt(&doc, "baudrate", &baud);
```

```c
/* لاگ سه سنسور */
FM_Logger lg;
float     v[3] = { 23.5f, 24.1f, 22.8f };

Directory_Create("/logs");
Logger_Init(&lg, "/logs", "sensor", FM_LOG_FORMAT_CSV, NULL);
Logger_Start(&lg);
Logger_Write(&lg, HAL_GetTick(), v, 3);
Logger_Stop(&lg);
```

---

## ۷. موزیک — `SimpleAudio`

پوشهٔ پیش‌فرض: `/music`. فرمت‌ها: WAV، MP3، FLAC، Ogg Vorbis، Opus. فایل
مونو خودکار استریو پخش می‌شود.

### ۷.۱ کنترل پخش

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleAudio_Begin()` | پوشه را فهرست می‌کند و اولین ترک را پخش می‌کند | `/music` | I2S |
| `SimpleAudio_Poll()` | پخش را زنده نگه می‌دارد | در حلقهٔ اصلی | – |
| `SimpleAudio_Stop()` | توقف کامل | – | – |
| `SimpleAudio_Start()` | اگر مکث است ادامه، اگر متوقف است شروع | – | – |
| `SimpleAudio_Play(path)` | یک فایل مشخص | مسیر فایل | I2S |
| `SimpleAudio_Next()` / `Prev()` | ترک بعدی / قبلی | – | – |
| `SimpleAudio_Pause()` / `Resume()` / `TogglePause()` | مکث / ادامه | – | – |
| `SimpleAudio_SeekTo(sec)` / `SeekBy(sec)` | پرش به ثانیه / جلو و عقب | ثانیه | – |
| `SimpleAudio_SetVolume(pct)` / `GetVolume()` | ولوم ۰ تا ۲۰۰ (۱۰۰ = بدون تغییر) | درصد | – |
| `SimpleAudio_SetPlayMode(mode)` / `GetPlayMode()` | رفتار آخر ترک | جدول ۷.۴ | – |
| `SimpleAudio_IsRunning()` / `IsPaused()` | وضعیت | – | `FM_TRUE` / `FM_FALSE` |
| `SimpleAudio_Info(&info)` | نام، کدک، نرخ، طول، موقعیت، ولوم، حالت | – | `info` |

```c
SimpleAudio_SetVolume(80);
SimpleAudio_Begin();

while (1)
{
    SimpleAudio_Poll();
    if (key_next())  SimpleAudio_Next();
    if (key_pause()) SimpleAudio_TogglePause();
}
```

### ۷.۲ صدا کجا برود؟

```
فایل → دیکد → [کالبک داده] → ولوم → [کالبک مانیتور] → I2S (بلندگو)
```

یک تابع مسیر را تعیین می‌کند:

| دستور | بلندگو | کالبک‌ها | کاربرد |
|---|:---:|:---:|---|
| `SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER)` | ✔ | ✘ | فقط پخش |
| `SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK)` | ✘ | ✔ | فقط محاسبه روی داده، بدون صدا |
| `SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH)` | ✔ | ✔ | پخش و محاسبه با هم (پیش‌فرض) |

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleAudio_SetDataCallback(fn, ctx)` | دادهٔ دیکدشده را قبل از ولوم می‌دهد؛ تغییر شما شنیده می‌شود | دیکدر | تابع شما، بعد بلندگو |
| `SimpleAudio_SetMonitorCallback(fn, ctx)` | همان چیزی که به بلندگو می‌رود (بعد از ولوم) | خروجی نهایی | تابع شما |
| `SimpleAudio_SetTrackStartCallback(fn, ctx)` | شروع هر ترک را خبر می‌دهد | – | مسیر + مشخصات ترک |
| `SimpleAudio_SetTrackEndCallback(fn, ctx)` | پایان هر ترک را خبر می‌دهد | – | مسیر ترک |

- همهٔ این تنظیم‌ها **ماندگارند**: یک‌بار، قبل یا بعد از `Begin`. با
  `Stop` و `Begin` دوباره پاک نمی‌شوند.
- `NULL` به‌جای `fn` یعنی برداشتن کالبک.
- در حالت «فقط کالبک» داده با همان سرعت واقعی پخش می‌رسد.
- کالبک داده و مانیتور باید **کوتاه** باشند: فقط محاسبه یا کپی. داخلشان
  فایل باز نکنید و `printf` نزنید؛ صدا تق‌تق می‌کند.

```c
/* بلندترین نمونه را پیدا کن؛ صدا هم پخش شود */
static int16_t peak;

static void my_data(void *ctx, int16_t *pcm, size_t samples,
                    uint32_t rate, uint16_t channels)
{
    (void)ctx; (void)rate; (void)channels;
    for (size_t i = 0; i < samples; i++)       /* L,R,L,R,... */
    {
        if (pcm[i] > peak) peak = pcm[i];
    }
}

SimpleAudio_SetDataCallback(my_data, NULL);
SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);      /* یا _CALLBACK: بدون صدا */
SimpleAudio_Begin();
```

### ۷.۳ خواندن مستقیم

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleAudio_DataRead(buf, frames, &n)` | نمونهٔ استریو را مستقیم می‌خواند | ترکِ در حال پخش | `buf` |
| `SimpleAudio_DataReadMono(buf, n, &got)` | همان، مونو | ترکِ در حال پخش | `buf` |

`DataRead` از همان ترکِ در حال پخش برمی‌دارد: آنچه بخوانید دیگر پخش
نمی‌شود. برای دادهٔ هم‌زمان با پخش، کالبک بخش ۷.۲ را استفاده کنید.
ذخیره در فایل: بخش ۷.۵؛ مثال‌های آماده: بخش ۷.۶.

### ۷.۴ حالت‌های پخش (مشترک با ویدیو)

| مقدار | آخر ترک چه می‌شود |
|---|---|
| `FM_PLAY_MODE_STOP` | توقف |
| `FM_PLAY_MODE_NEXT` | بعدی؛ آخر فهرست توقف |
| `FM_PLAY_MODE_REPEAT_ONE` | همان ترک دوباره |
| `FM_PLAY_MODE_REPEAT_ALL` | بعدی؛ آخر فهرست برمی‌گردد به اول |

پیش‌فرض موزیک `REPEAT_ALL` و پیش‌فرض ویدیو `STOP` است.

### ۷.۵ ذخیرهٔ صدا در فایل

فرمت از پسوند فایل مقصد انتخاب می‌شود:

| پسوند | فرمت | حجم نسبت به WAV | کیفیت |
|---|---|---|---|
| `.wav` | WAV شانزده‌بیتی | ۱ | بدون افت |
| `.wav` با `RecSetFormat(FM_AUDIO_FILE_ADPCM)` | IMA ADPCM | یک‌چهارم | با افت کم؛ مناسب صدای ضبط‌شده |
| `.flac` | FLAC | حدود ۰٫۶ تا ۰٫۷ برای موسیقی | بدون افت |
| `.mp3` `.ogg` `.opus` | فقط با انکودری که خودتان ثبت کنید (`Audio_RegisterEncoder`) | – | – |

بدون انکودر، `FM_ERR_UNSUPPORTED` برمی‌گردد و فایلی ساخته نمی‌شود.

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleAudio_RecStart(dest, rate, ch)` | فایل را برای صدای خودتان باز می‌کند | مسیر فایل، یا پوشه (نام خودکار `rec_000001`) | فایل |
| `SimpleAudio_RecWrite(pcm, frames)` | نمونه اضافه می‌کند | بافر شما | فایل |
| `SimpleAudio_RecPlaying(dest)` | موزیکِ در حال پخش را ذخیره می‌کند | پوشه: برای هر ترک یک فایل. فایل: همه در یک فایل | فایل |
| `SimpleAudio_RecStop()` | فایل را کامل می‌کند و می‌بندد | – | نتیجهٔ ذخیره |
| `SimpleAudio_RecPath()` | مسیر آخرین فایل | – | رشته |
| `SimpleAudio_RecActive()` | ذخیره فعال است؟ | – | بله / نه |
| `SimpleAudio_RecSetFormat(fmt)` | فرمت در حالت پوشه، یا ADPCM برای `.wav` | `FM_AUDIO_FILE_...` | – |
| `SimpleAudio_RecSetChannels(n)` | ۰ همان ورودی، ۱ مونو، ۲ استریو | – | – |
| `SimpleAudio_RecSetBitrate(kbps)` | برای انکودرهای ثبت‌شده | – | – |
| `SimpleAudio_Convert(src, dst, fn, ctx)` | فایل به فایل، بدون پخش، با تمام سرعت | فایل | فایل |

**صدای خودتان** (میکروفون، ADC):

```c
SimpleAudio_RecStart("/rec/mic.flac", 16000, 1);   /* ۱۶ کیلوهرتز، مونو */
/* هر بار که داده رسید، از حلقهٔ اصلی: */
SimpleAudio_RecWrite(pcm, frames);
/* پایان: */
SimpleAudio_RecStop();
```

**موزیکِ در حال پخش.** قبل یا وسط پخش:

```c
SimpleAudio_RecPlaying("/rec");            /* /music/a.mp3 -> /rec/a.wav */
/* یا: SimpleAudio_RecPlaying("/rec/all.flac");   همه در یک فایل */
...
SimpleAudio_RecStop();
```

آنچه ذخیره می‌شود صدای قبل از ولوم است. پردازش کالبک دادهٔ شما
(`SetDataCallback`) فقط وقتی در فایل هست که خروجی «کالبک» را هم داشته
باشد:

| `SimpleAudio_SetOutput(...)` | بلندگو | پردازش شما در فایل |
|---|---|---|
| `FM_AUDIO_OUT_SPEAKER` | دارد | ندارد |
| `FM_AUDIO_OUT_BOTH` | دارد | دارد |
| `FM_AUDIO_OUT_CALLBACK` | ساکت | دارد |
| `FM_AUDIO_OUT_NONE` | ساکت | ندارد |

- حالت پوشه تا `RecStop` فعال می‌ماند، حتی بعد از `Stop` و `Begin` دوباره.
- حالت پوشه فایلی را که از قبل هست پاک نمی‌کند. اگر `/rec/a.wav` هست (ترک
  دیگری با همین نام از یک زیرپوشهٔ دیگر، مثل `/music/x/a.mp3` و
  `/music/y/a.mp3`، یا ذخیرهٔ دفعهٔ قبل)، نام بعدی `a_2.wav`، بعد
  `a_3.wav` می‌شود. فقط وقتی همان ترک در همان نوبت `RecPlaying` دوباره
  پخش شود (تکرار فهرست) روی فایل خودش نوشته می‌شود، تا کارت پر نشود.
  شماره‌ها تا `a_999` می‌روند؛ اگر همهٔ ۹۹۹ نام گرفته شده باشد ذخیرهٔ آن
  ترک با `FM_ERR_EXISTS` می‌ایستد (پوشه را خالی کنید).
- **نوشتن روی کارت داخل مسیر پخش است.** در حالت پوشه، دادهٔ هر نیم‌بافرِ
  I2S همان لحظه‌ای که پر می‌شود روی کارت هم نوشته می‌شود، و این مسیر از
  وسط دیکد عکس و ویدیو هم صدا زده می‌شود (`FM_Idle`). اگر کارت برای یک
  نوشتن بیشتر از طول یک نیم‌بافر وقت بگیرد، نیم‌بافر بعدی دیر پر می‌شود و
  صدا می‌بُرد. این فقط از روی کد و روی PC دیده شده و روی برد و کارت واقعی
  اندازه‌گیری نشده است؛ اگر هنگام ضبط بریدگی شنیدید، اول همین را بسنجید.
- فایلی که بسته نشده (قطع برق، ریست): WAV و ADPCM تا آخرین نقطهٔ ذخیره
  پخش می‌شوند (هر ۲۵۶ کیلوبایت، `FM_AUDIO_ENC_SYNC_BYTES`)؛ FLAC تا همان‌جا
  روی کارت هست ولی طولش در هدر «نامعلوم» است و آخرین فریمش بریده است: این
  کتابخانه فریم‌های کامل را پخش می‌کند و «پایان فایل» می‌دهد؛ یک پخش‌کنندهٔ
  سخت‌گیرتر (libav) همان فریم‌ها را پخش می‌کند و بعد «دادهٔ نامعتبر» گزارش
  می‌دهد. کمتر از ۲۵۶ کیلوبایت: طول‌ها در هدر هنوز صفرند. روی FAT اندازهٔ
  فایل تا اولین نقطهٔ ذخیره صفر می‌ماند (چیزی برای پخش نیست)؛ جایی که بایت‌ها
  به حافظه رسیده باشند این کتابخانه و libav تا آخر فایل پخش می‌کنند و
  پخش‌کننده‌ای که به هدر اعتماد کند «ترک خالی» نشان می‌دهد. پیش از اولین
  نقطهٔ ذخیره روی هیچ‌کدام حساب نکنید.
  ۲۵۶ کیلوبایت به زمان: WAV استریوی ۴۴٫۱ کیلوهرتز حدود ۱٫۵ ثانیه، ADPCM
  استریوی ۴۴٫۱ کیلوهرتز حدود ۶ ثانیه، ولی ADPCM مونوی ۸ کیلوهرتز حدود **۶۴
  ثانیه**. برای ضبط با نرخ پایین `FM_AUDIO_ENC_SYNC_BYTES` را کوچک‌تر
  بگذارید (مثلاً ۱۶۳۸۴).
- WAVی که طول `data` در هدرش صفر است ولی پشتش صدا هست (ضبطِ بسته‌نشدهٔ یک
  دستگاه دیگر، یا فایل استریم) تا آخر فایل پخش می‌شود؛ قبلاً «ترک خالی»
  باز می‌شد.
- اگر وسط ذخیره کارت پر شود، `RecStop` (و `AudioFileWriter_Close`) همان
  خطا را برمی‌گرداند ولی فایل را سالم می‌بندد: طول‌های هدر با آنچه واقعاً
  روی کارت نشسته یکی است.
- حالت تک‌فایل با `RecStop`، با توقف پخش، یا با رسیدن به ترکی که نرخ
  نمونه‌برداری دیگری دارد تمام می‌شود (یک فایل فقط یک نرخ دارد).
- ذخیره با سرعت پخش پیش می‌رود: آهنگ سه‌دقیقه‌ای، سه دقیقه.
- نوشتن روی کارت در همان حلقهٔ پخش انجام می‌شود. اگر پخش قطع شد
  (`Poll` مقدار `FM_ERR_TIMEOUT` داد)، کارت کند است: `SIMPLE_AUDIO_HALF_SAMPLES`
  را بزرگ نگه دارید یا از `SimpleAudio_Convert` استفاده کنید.
- مقصد را پوشه‌ای غیر از پوشهٔ موزیک بگذارید، تا فایل‌های ساخته‌شده وارد
  فهرست پخش نشوند.

**فایل به فایل**، با پردازش شما روی هر بلوک:

```c
static void half_volume(void *ctx, int16_t *pcm, size_t samples,
                        uint32_t rate, uint16_t channels)
{
    for (size_t i = 0; i < samples; i++) pcm[i] /= 2;
}

SimpleAudio_Convert("/music/a.mp3", "/out/a.flac", half_volume, NULL);
```

تا پایان فایل برنمی‌گردد. اگر خطا بدهد، فایل مقصد پاک می‌شود. فایل ADPCM
تا یک بلوک (حداکثر ۲۰۴۱ نمونه) بلندتر از ورودی است؛ انتهای آن تکرار آخرین
نمونه است.

### ۷.۶ مثال‌های آمادهٔ ذخیرهٔ صدا

همه در `top_api/examples_save_media.h` هستند و روی کامپیوتر با
`make saveexamplestest` اجرا و بازخوانی می‌شوند.

| مثال | سبک | چه می‌سازد |
|---|---|---|
| `SaveDemo_AudioPlain()` | ساده: داده را خودتان می‌سازید | بوق ۱ کیلوهرتز، ۲ ثانیه → `/save/tone.wav` و `/save/tone.flac` |
| `SaveDemo_MusicFromCallback(dest, seconds)` | از کالبک، همه در یک تابع | `seconds` ثانیه از موزیکِ پوشهٔ موزیک → `dest` |
| `SaveDemo_MusicCallback` + `SaveDemo_MusicRecStart(dest)` / `RecPump()` / `RecStop()` | از کالبک، تکه‌تکه برای برنامهٔ خودتان | موزیک در حال پخش → `dest` |

**سبک ساده** همان سه خط بالاست: `RecStart`، چند بار `RecWrite`، `RecStop`.

**سبک کالبک.** فرق با `SimpleAudio_RecPlaying`: نمونه‌ها اول به تابع شما
می‌رسند و خودتان تصمیم می‌گیرید چه چیزی، کی و کجا ذخیره شود.

```
دیکدر ─> کالبک داده ─> رینگ در RAM ─> Pump در حلقهٔ اصلی ─> SimpleAudio_RecWrite ─> فایل
         (فقط کپی)
```

```c
/* یک‌بار: */
SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH);                  /* بلندگو + کالبک */
SimpleAudio_SetDataCallback(SaveDemo_MusicCallback, NULL);

/* شروع ذخیره (قبل از Begin یا وسط پخش): */
SaveDemo_MusicRecStart("/save/music_cb.wav");

/* در while (1)، کنار SimpleAudio_Poll(): */
SaveDemo_MusicRecPump();

/* پایان: */
SaveDemo_MusicRecStop();
```

- کالبک داده داخل پُرکردن بافر صدا اجرا می‌شود: در آن فایل ننویسید، فقط
  کپی کنید. نوشتن کار `Pump` است.
- اولین نمونه‌ها قبل از کالبک شروع ترک می‌رسند. `RecStart` را قبل از
  `SimpleAudio_Begin()` بزنید تا ابتدای آهنگ جا نماند. فایل با اولین
  داده باز می‌شود، چون نرخ نمونه‌برداری همان‌جا معلوم می‌شود.
- کالبک همیشه استریو می‌دهد (`channels = 2`)، حتی برای فایل مونو.
- یک فایل یک نرخ نمونه‌برداری دارد: با رسیدن ترکی با نرخ دیگر، فایل بسته
  می‌شود.
- رینگ: `SAVE_DEMO_MUSIC_RING_SAMPLES` (پیش‌فرض ۷۰۰۰۰ نمونه = ۱۴۰ کیلوبایت
  از `FM_Alloc`). اگر `Pump` دیر صدا زده شود و رینگ پُر شود، آن تکه دور
  ریخته و در گزارش پایان با `dropped` شمرده می‌شود.
- اگر کالبک دادهٔ خودتان را دارید، `SaveDemo_MusicCallback` را از داخل آن
  صدا بزنید (نمونه: `board/app_save_hooks.h`).

---

## ۸. ویدیو — `SimpleVideo`

پوشهٔ پیش‌فرض: `/videos`. فرمت‌ها: AVI، MP4 و MOV با MJPEG یا H.264
Baseline. خروجی تصویر تابعی است که شما می‌دهید (نمایشگر مال برنامه است).

### ۸.۱ کنترل پخش

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleVideo_SetPresent(fn)` | تابع نمایش را معرفی می‌کند | – | هر فریم به `fn` |
| `SimpleVideo_SetOutputSize(w, h)` | اندازهٔ خروجی (`0,0` = اندازهٔ فایل) | – | – |
| `SimpleVideo_SetOutputFormat(fmt)` | فرمت پیکسل (پیش‌فرض RGB565) | – | – |
| `SimpleVideo_Begin()` | پوشه را فهرست می‌کند | `/videos` | – |
| `SimpleVideo_Start()` | پخش مورد فعلی را شروع می‌کند | – | نمایشگر + I2S |
| `SimpleVideo_Poll()` | دیکد و نمایش | در حلقهٔ اصلی | – |
| `SimpleVideo_Stop()` | توقف کامل | – | – |
| `SimpleVideo_Play(path)` | یک فایل مشخص | مسیر فایل | نمایشگر + I2S |
| `SimpleVideo_Next()` / `Prev()` | بعدی / قبلی | – | – |
| `SimpleVideo_Pause()` / `Resume()` / `TogglePause()` | مکث / ادامه | – | – |
| `SimpleVideo_SeekTo(sec)` / `SeekBy(sec)` | پرش | ثانیه | – |
| `SimpleVideo_SetVolume(pct)` / `GetVolume()` | ولوم ۰ تا ۲۰۰ | درصد | – |
| `SimpleVideo_SetPlayMode(mode)` / `GetPlayMode()` | رفتار آخر ویدیو | جدول ۷.۴ | – |
| `SimpleVideo_SetAutoPlay(1)` | `Begin` خودش پخش را شروع کند و پوشه تکرار شود | – | – |
| `SimpleVideo_IsRunning()` / `IsPaused()` | وضعیت | – | `FM_TRUE` / `FM_FALSE` |
| `SimpleVideo_Info(&info)` | نام، ابعاد، fps، موقعیت، ولوم | – | `info` |
| `SimpleVideo_SeekCount()` | تعداد پرش‌های انجام‌شده؛ کالبک‌ها با آن می‌فهمند پخش به جای دیگری پریده (همان `info.seek_count`) | – | عدد |

```c
static void lcd_show(const void *px, uint16_t w, uint16_t h,
                     FM_PixelFormat fmt, size_t bytes)
{
    /* px را به نمایشگر بفرست */
}

SimpleVideo_SetPresent(lcd_show);
SimpleVideo_SetOutputSize(320, 240);
SimpleVideo_Begin();
SimpleVideo_Start();

while (1)
{
    SimpleVideo_Poll();
}
```

### ۸.۲ صدای ویدیو کجا برود؟

همان مدل موزیک، با همان نوع کالبک (یک تابع را می‌توان به هر دو داد):

| دستور | بلندگو | کالبک‌ها | توضیح |
|---|:---:|:---:|---|
| `SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_NONE)` | ✘ | ✘ | صدا اصلاً خوانده نمی‌شود (فقط تصویر، سبک‌تر) |
| `SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_SPEAKER)` | ✔ | ✘ | فقط پخش |
| `SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_CALLBACK)` | ✘ | ✔ | فقط محاسبه |
| `SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH)` | ✔ | ✔ | هر دو (پیش‌فرض) |

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleVideo_SetAudioDataCallback(fn, ctx)` | صدای ویدیو قبل از ولوم | ترک صدای ویدیو | تابع شما، بعد بلندگو |
| `SimpleVideo_SetAudioMonitorCallback(fn, ctx)` | صدای ویدیو بعد از ولوم | خروجی نهایی | تابع شما |
| `SimpleVideo_GetAudioInfo(&info)` | نرخ، کانال و کدک صدای ویدیو | – | `info` |

`NONE` از ویدیوی بعدی کامل اثر می‌کند (ترک صدا هنگام شروع هر ویدیو باز
می‌شود)؛ روی ویدیوی در حال پخش بلافاصله ساکت می‌کند.

`SimpleVideo_AudioDataRead(pcm, frames, &got)` (خواندن مستقیم، برای
`SIMPLE_VIDEO_I2S=0`): خروجی همیشه استریوی ۱۶ بیتیِ درهم است؛ `pcm` باید
جای `frames × 2` نمونه داشته باشد و `got` تعداد فریم استریو است (حداکثر
`frames`، در هر صدا زدن یک بسته). ترک مونو دو برابر می‌شود (چپ = راست).
برای ترک MP3 مونو `frames` دست‌کم ۲۳۰۴ و برای استریو ۱۱۵۲ بدهید
(`FM_ERR_TOO_LARGE` اگر کمتر باشد). ترکی که به PCM دیکد نمی‌شود یا بیش از دو
کانال دارد `FM_ERR_UNSUPPORTED` می‌دهد.

### ۸.۳ تصویر در دست شما

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleVideo_SetFrameCallback(fn, ctx)` | هر فریم دیکدشده را می‌دهد، حتی بدون نمایشگر | دیکدر | تابع شما |
| `SimpleVideo_SetTrackStartCallback(fn, ctx)` | شروع هر ویدیو | – | مسیر، ابعاد، fps |
| `SimpleVideo_SetTrackEndCallback(fn, ctx)` | پایان هر ویدیو | – | مسیر |
| `SimpleVideo_GetFrame(&w, &h)` | آخرین فریم به‌صورت RGB888 | – | اشاره‌گر به پیکسل‌ها |
| `SimpleVideo_SaveFrame(path)` | آخرین فریم را عکس می‌کند (`NULL` = BMP شماره‌دار در `/photos`) | فریم فعلی | فایل BMP / PNG / JPEG |

```c
/* فقط داده: نه نمایش، نه صدا */
SimpleVideo_SetPresent(NULL);
SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_NONE);
SimpleVideo_SetFrameCallback(my_frame, NULL);
SimpleVideo_Begin();
SimpleVideo_Start();
```

### ۸.۴ ذخیرهٔ ویدیو در فایل

فایل ساخته‌شده MJPEG در AVI است، با صدای PCM اگر بخواهید. روی کامپیوتر،
گوشی و همین کتابخانه پخش می‌شود. انکودر H.264 یا MP4 وجود ندارد: برای
میکروکنترلر عملی نیست.

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleVideo_RecStart(path, w, h, fps, 0)` | با پسوند `.avi` فایل MJPEG را همان لحظه باز می‌کند | مسیر، ابعاد، فریم در ثانیه | فایل |
| `SimpleVideo_RecWrite(rgb888, bytes)` | یک فریم RGB888 | `w * h * 3` بایت | فایل |
| `SimpleVideo_RecWritePixels(px, format)` | یک فریم با هر فرمت پیکسل | مثلاً `FM_PIXEL_RGB565` از فریم‌بافر | فایل |
| `SimpleVideo_RecWritePlayerFrame(px, format)` | فریمی که خود پخش‌کننده داده (کالبک فریم) | پیکسل و فرمت همان کالبک | فایل |
| `SimpleVideo_RecWriteJpeg(jpg, bytes)` | یک فریم که خودش JPEG است | دوربین در حالت JPEG | فایل |
| `SimpleVideo_RecRepeat(n)` | آخرین فریم را `n` بار دیگر تکرار می‌کند (بدون فشرده‌سازی دوباره، بدون حجم) | – | فایل |
| `SimpleVideo_RecWriteAudio(pcm, frames)` | صدای همان فریم | PCM شانزده‌بیتی | فایل |
| `SimpleVideo_RecStop()` | ایندکس و طول را می‌نویسد و می‌بندد | – | نتیجهٔ ذخیره |
| `SimpleVideo_RecSetQuality(q)` | کیفیت JPEG، ۱ تا ۱۰۰ (پیش‌فرض ۸۰) | – | – |
| `SimpleVideo_RecSetAudio(rate, ch)` | تراک صدا برای فایل بعدی (`0` = بدون صدا) | – | – |
| `SimpleVideo_RecActive()` / `RecFrames()` | وضعیت و تعداد فریم نوشته‌شده | – | – |
| `SimpleVideo_Convert(src, dst, fn, ctx)` | فایل به فایل، با کالبک روی هر فریم | هر ویدیوی قابل‌پخش | MJPEG AVI |

```c
SimpleVideo_RecSetAudio(16000, 1);                    /* اختیاری */
SimpleVideo_RecStart("/videos/cam.avi", 320, 240, 15, 0);
/* برای هر فریم: */
SimpleVideo_RecWritePixels(framebuffer, FM_PIXEL_RGB565);
SimpleVideo_RecWriteAudio(pcm, 16000 / 15);           /* صدای همان فریم */
/* پایان: */
SimpleVideo_RecStop();
```

- `RecStop` را همیشه بزنید: ایندکس فایل آنجا نوشته می‌شود.
- تا `RecStop` یک فایل کمکی `<نام>.idx` کنار ویدیو هست؛ بعد پاک می‌شود.
- فایلی که بسته نشده (قطع برق، ریست): هر یک مگابایت
  (`FM_VIDEO_ENC_SYNC_BYTES`) هدر به‌روز و فایل sync می‌شود، پس فایل تا
  آخرین نقطه یک AVI درست ولی بدون ایندکس است و همین کتابخانه بازش می‌کند.
  `<نام>.idx` کنارش می‌ماند: نشانهٔ فایل ناتمام. بعد از روشن‌شدن:

  ```c
  uint32_t frames;
  if (VideoFile_Repair("/videos/cam.avi", &frames) == FM_OK)
  {
      /* ایندکس و طول دقیق نوشته شد، cam.avi.idx پاک شد */
  }
  ```

  `FM_ERR_NOT_FOUND` یعنی `.idx` نیست و چیزی برای تعمیر وجود ندارد. روی
  فایلی که همین الان در حال ضبط است صدا نزنید.
- اگر وسط ذخیره کارت پر شود، `RecStop` همان خطا را برمی‌گرداند و فایل را
  با همان چیزی که روی کارت نشسته کامل می‌کند (ایندکس، اگر جا باشد).
- `VideoFileWriter_Open` و `AudioFileWriter_Open` روی نویسنده‌ای که هنوز
  فایل باز دارد `FM_ERR_BUSY` می‌دهند و به فایل اول دست نمی‌زنند.
- هر فایل تا حدود ۲ گیگابایت جا دارد؛ بعد از آن نوشتن `FM_ERR_TOO_LARGE`
  می‌دهد: `RecStop` بزنید و فایل بعدی را شروع کنید.
- مسیر با پسوند دیگر، مثل قبل FMRV خام می‌سازد (بدون فشرده‌سازی، با `Poll`
  بعدی باز می‌شود).
- حجم: تصویر آزمایشی ۴۸۰×۲۷۲ با کیفیت ۸۵ حدود ۱۴ کیلوبایت برای هر فریم شد.
  سرعت انکود روی برد اندازه‌گیری نشده است.
- `RecWritePixels` پیکسل ۱۶ بیتی را با بایت کم‌ارزش اول می‌خواند (همان
  `uint16_t` در حافظهٔ میکروکنترلر). فریم‌بافری که برای LCD از نوع SPI
  بایت‌جابه‌جا شده (خروجی خود پخش‌کننده با `SIMPLE_VIDEO_SWAP16 = 1`) را با
  `RecWritePlayerFrame` بدهید؛ وگرنه رنگ‌های فایل غلط می‌شود. در لایهٔ
  پایین‌تر: `VideoFileWriter_WriteFrameSwap16` و `FM_JpegEncodeOptions.swap16`.

**فایل به فایل.** هر فریم به‌صورت RGB888 به تابع شما می‌رسد؛ درجا تغییرش
بدهید:

```c
static void red_bar(void *ctx, uint8_t *rgb, uint16_t w, uint16_t h,
                    uint32_t frame)
{
    for (uint32_t x = 0; x < 2u * w; x++)             /* دو سطر بالا */
    {
        rgb[3u * x] = 255; rgb[3u * x + 1u] = 0; rgb[3u * x + 2u] = 0;
    }
}

SimpleVideo_Convert("/videos/a.mp4", "/out/a.avi", red_bar, NULL);
```

تا پایان فایل برنمی‌گردد و وقتی ویدیویی در حال پخش است `FM_ERR_BUSY`
می‌دهد. حافظه: یک فریم RGB888 (`w * h * 3` بایت) از `FM_Alloc`، به‌اضافهٔ
نیاز دیکدر. برای اندازهٔ دیگر، پردازش صدا، پیشرفت یا لغو: `Video_Convert`
با `FM_VideoConvertConfig` (بخش ۱۰).

### ۸.۵ مثال‌های آمادهٔ ذخیرهٔ ویدیو

در همان `top_api/examples_save_media.h`:

| مثال | سبک | چه می‌سازد |
|---|---|---|
| `SaveDemo_VideoPlain()` | ساده: فریم و صدا را خودتان می‌سازید | ۳۰ فریم ۱۶۰×۱۲۰ (نوار رنگی + مربع متحرک) و بوق → `/save/bars.avi` |
| `SaveDemo_VideoFromCallback(dest, max_frames)` | از کالبک، همه در یک تابع | اولین ویدیوی پوشهٔ ویدیو (`max_frames = 0` یعنی تا آخر) → `dest` |
| `SaveDemo_VideoFrameCallback` + `SaveDemo_VideoSoundCallback` + `SaveDemo_VideoRecStart(dest)` / `RecPump()` / `RecStop()` | از کالبک، تکه‌تکه | ویدیوی در حال پخش، تصویر و صدا → `dest` |

```
فریم ─> کالبک فریم ─> SimpleVideo_RecWritePlayerFrame ─────────────────────> فایل
صدا  ─> کالبک صدا  ─> رینگ در RAM ─> Pump ─> SimpleVideo_RecWriteAudio ─> فایل
```

```c
/* یک‌بار، بعد از SimpleVideo_Begin(): */
SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);             /* بلندگو + کالبک */
SimpleVideo_SetFrameCallback(SaveDemo_VideoFrameCallback, NULL);
SimpleVideo_SetAudioDataCallback(SaveDemo_VideoSoundCallback, NULL);

/* شروع ذخیره (بهتر است قبل از SimpleVideo_Start()): */
SaveDemo_VideoRecStart("/save/video_cb.avi");

/* در while (1)، کنار SimpleVideo_Poll(): */
SaveDemo_VideoRecPump();

/* پایان (در انتهای ویدیو خودش بسته می‌شود): */
SaveDemo_VideoRecStop();
```

- کالبک فریم در حلقهٔ اصلی اجرا می‌شود (داخل `SimpleVideo_Poll`)؛ همان‌جا
  فشرده و نوشته می‌شود. کالبک صدا داخل پُرکردن بافر I2S است: فقط کپی.
- اندازه و نرخ فریم فایل از `SimpleVideo_Info()` در اولین کالبک فریم
  گرفته می‌شود؛ فایل همان‌جا باز می‌شود.
- اگر پخش‌کننده فریم جا بیندازد (فشرده‌سازی JPEG وقت می‌برد)، جای آن‌ها
  با `SimpleVideo_RecRepeat` پُر می‌شود: تعداد فریم‌ها و زمان فایل با
  ویدیوی اصلی و با صدا یکی می‌ماند، فقط حرکت کم‌تر می‌شود.
- صدای فایل همیشه استریو و کامل است، حتی وقتی فریم جا افتاده.
- هر فایل یک ویدیو است: در پایان ویدیو، با رفتن به ویدیوی بعدی، یا با
  تکرار همان ویدیو از اول، فایل بسته می‌شود.
- پرش وسط ذخیره (`SeekTo` / `SeekBy`، جلو یا عقب): فایل ادامه پیدا می‌کند و
  در جای پرش «برش» می‌خورد، مثل چیزی که دیده و شنیده شد؛ تصویر و صدای جای
  تازه زیر هم می‌مانند. در جای برش آخرین تصویر ممکن است چند فریم بیشتر
  بماند (صدایی که پخش‌کننده جلوتر دیکد کرده بود، حدود یک بافر I2S). پرش
  از `SimpleVideo_SeekCount()` فهمیده می‌شود، نه از شمارهٔ فریم؛ اگر
  کالبک‌های خودتان را می‌نویسید همین کار را بکنید
  (`save_demo_video_cut` در `examples_save_media.h`).
- بدون خروجی «کالبک» (`FM_AUDIO_OUT_SPEAKER`) فایل تصویر دارد ولی تراک
  صدا ندارد.
- رینگ صدا: `SAVE_DEMO_VIDEO_RING_SAMPLES` (پیش‌فرض ۴۰۰۰۰ نمونه = ۸۰
  کیلوبایت).

روی برد دستورهای `arec` / `arecs` و `vrec` / `vrecs` همین‌ها را صدا
می‌زنند: `board/README.md`.

---

## ۹. تصویر — `SimpleImage`

پوشهٔ پیش‌فرض: `/photos`. خواندن: BMP، JPEG، PNG (از روی محتوا، نه پسوند).
عکس بزرگ‌تر از بافر خودکار کوچک می‌شود.

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `SimpleImage_SetPresent(fn)` | تابع نمایش را معرفی می‌کند | – | هر عکس به `fn` |
| `SimpleImage_SetOutputSize(w, h)` | اندازهٔ خروجی | – | – |
| `SimpleImage_SetFormat(fmt)` | فرمت پیکسل (پیش‌فرض RGB565) | – | – |
| `SimpleImage_Begin()` | پوشه را فهرست و اولین عکس را نشان می‌دهد | `/photos` | نمایشگر |
| `SimpleImage_Poll()` | اجرای درخواست‌ها و اسلایدشو | در حلقهٔ اصلی | – |
| `SimpleImage_Stop()` | پایان | – | – |
| `SimpleImage_Show(path)` | یک عکس مشخص | مسیر فایل | نمایشگر |
| `SimpleImage_Next()` / `Prev()` | عکس بعدی / قبلی | – | نمایشگر |
| `SimpleImage_SetSlideshow(ms)` | هر `ms` میلی‌ثانیه عکس بعدی (`0` = خاموش)؛ ماندگار | – | – |
| `SimpleImage_Rotate(deg)` | چرخش ۹۰ / ۱۸۰ / ۲۷۰ | عکس فعلی | نمایشگر |
| `SimpleImage_FlipH()` / `FlipV()` | برعکس افقی / عمودی | عکس فعلی | نمایشگر |
| `SimpleImage_Resize(w, h)` | تغییر اندازه | عکس فعلی | نمایشگر |
| `SimpleImage_Effect(effect, amount)` | نگاتیو، سیاه‌وسفید، سپیا، روشنایی، کنتراست | عکس فعلی | نمایشگر |
| `SimpleImage_Save(path)` | عکس فعلی را ذخیره می‌کند (فرمت از پسوند) | عکس فعلی | فایل BMP / PNG / JPEG |
| `SimpleImage_Data(&w, &h, ...)` | پیکسل‌های عکس فعلی | – | اشاره‌گر به پیکسل‌ها |
| `SimpleImage_Info(&info)` | نام، ابعاد، فرمت، شمارهٔ عکس | – | `info` |

```c
SimpleImage_SetPresent(lcd_show);
SimpleImage_SetOutputSize(320, 240);
SimpleImage_SetSlideshow(3000);          /* هر ۳ ثانیه */
SimpleImage_Begin();

while (1)
{
    SimpleImage_Poll();
    if (key_rotate()) SimpleImage_Rotate(90);
}
```

---

## ۱۰. کار مستقیم با فایل رسانه (بدون پلیر)

وقتی فقط داده می‌خواهید و پخش لازم نیست.

| تابع | کار | ورودی | خروجی |
|---|---|---|---|
| `Audio_Open(path, &a)` / `Audio_Close(&a)` | فایل صدا را باز / بسته می‌کند | فایل | `a` |
| `Audio_GetInfo(&a, &info)` | کدک، نرخ، کانال، طول | `a` | `info` |
| `Audio_ReadStereo(&a, buf, frames, &n)` | نمونهٔ استریو | فایل | `buf` |
| `Audio_ReadMono(&a, buf, count, &n)` | نمونهٔ مونو | فایل | `buf` |
| `Audio_Seek(&a, sec)` | پرش به ثانیه | – | – |
| `AudioWriter_Create / Write / Close` | نوشتن WAV | بافر | فایل WAV |
| `AudioFileWriter_Open / WriteFrames / Close` | نوشتن WAV، ADPCM، FLAC یا فرمت انکودر ثبت‌شده | بافر | فایل |
| `Audio_Convert(src, dst, &cfg, &frames)` | فایل صدا به فایل صدا، با پردازش، پیشرفت و لغو | فایل | فایل |
| `Audio_RegisterEncoder(format, &ops)` | انکودر خودتان (MP3، Vorbis، Opus) | – | – |
| `Image_Load(path, &img, buf, cap)` | عکس را دیکد می‌کند | فایل | پیکسل در `buf` |
| `Image_Save(path, &img)` | عکس را ذخیره می‌کند | `img` | فایل |
| `Image_EncodeJPEGEx(&img, &opt, out, cap, &len)` | JPEG با کیفیت ۱ تا ۱۰۰ و رنگ ۴:۴:۴ یا ۴:۲:۰ | `img` | بایت‌های JPEG |
| `Image_ResizeFile(src, dst, w, h, mode)` | فایل عکس را با اندازهٔ جدید می‌سازد | فایل | فایل |
| `Video_Open(path, &v)` / `Video_Close(&v)` | فایل ویدیو را باز / بسته می‌کند | فایل | `v` |
| `Video_GetInfo(&v, &info)` | ابعاد، fps، تعداد فریم | `v` | `info` |
| `Video_DecodeFrame(&v, buf, cap)` | فریم بعدی را دیکد می‌کند | فایل | پیکسل در `buf` |
| `VideoFileWriter_Open / WriteFrame / WriteJpeg / WriteAudio / Close` | نوشتن MJPEG AVI | پیکسل یا JPEG، و PCM | فایل AVI |
| `Video_Convert(src, dst, &cfg, &frames)` | ویدیو به MJPEG AVI، با تغییر اندازه، پردازش، پیشرفت و لغو | فایل | فایل |

```c
FM_Audio a;
int16_t  pcm[2 * 256];
size_t   n;

if (Audio_Open("/music/a.mp3", &a) == FM_OK)
{
    while (Audio_ReadStereo(&a, pcm, 256, &n) == FM_OK && n > 0)
    {
        /* n فریم استریو در pcm */
    }
    Audio_Close(&a);
}
```

---

## ۱۱. موزیک و ویدیو کنار هم

| کار | موزیک | ویدیو |
|---|---|---|
| شروع / حلقه / توقف | `Begin` `Poll` `Stop` `Start` | `Begin` `Poll` `Stop` `Start` |
| فایل مشخص، بعدی، قبلی | `Play` `Next` `Prev` | `Play` `Next` `Prev` |
| مکث | `Pause` `Resume` `TogglePause` | `Pause` `Resume` `TogglePause` |
| پرش | `SeekTo` `SeekBy` | `SeekTo` `SeekBy` |
| ولوم ۰ تا ۲۰۰ | `SetVolume` `GetVolume` | `SetVolume` `GetVolume` |
| آخر فایل | `SetPlayMode` `GetPlayMode` | `SetPlayMode` `GetPlayMode` |
| وضعیت | `IsRunning` `IsPaused` `Info` | `IsRunning` `IsPaused` `Info` |
| مسیر صدا | `SetOutput` `GetOutput` | `SetAudioOutput` `GetAudioOutput` |
| دادهٔ صدا | `SetDataCallback` `SetMonitorCallback` | `SetAudioDataCallback` `SetAudioMonitorCallback` |
| شروع و پایان | `SetTrackStartCallback` `SetTrackEndCallback` | `SetTrackStartCallback` `SetTrackEndCallback` |

موزیک و ویدیو از یک خروجی I2S استفاده می‌کنند: هر کدام شروع شود، دیگری را
متوقف کنید (`SimpleAudio_Stop()` قبل از `SimpleVideo_Begin()` و برعکس).

---

## ۱۲. خطاهای پرتکرار

| کد | معنی | معمولاً یعنی |
|---|---|---|
| `FM_ERR_NOT_FOUND` (‎-3) | پیدا نشد | مسیر اشتباه یا پوشه خالی است |
| `FM_ERR_BUSY` (‎-8) | مشغول | پلیر در حال کار است؛ اول `Stop` |
| `FM_ERR_UNSUPPORTED` (‎-11) | پشتیبانی نمی‌شود | فرمت ناشناخته، یا بخش خاموش است |
| `FM_ERR_NOT_MOUNTED` (‎-13) | حافظه مونت نیست | `UsbSd_FatRoot()` یا `QspiFlash_Begin()` صدا زده نشده |
| `FM_ERR_NO_MEMORY` (‎-14) | جا نیست | فایل‌های بازمانده را ببندید؛ اندازهٔ بافرها در `fm_board_config.h` |
| `FM_ERR_EOF` (‎-15) | آخر فایل | عادی است |
| `FM_ERR_TOO_LARGE` (‎-16) | بزرگ‌تر از حد | نام ۶۴ نویسه یا بیشتر، مسیر خیلی بلند، یا بافر کوچک |
| `FM_ERR_CORRUPTED` (‎-12) | فایل‌سیستم خراب | جدول FMFS / LittleFS خوانا نیست یا بی‌معنی است؛ چیزی پاک نمی‌شود، `FS_Format` با شماست |
| `FM_ERR_INTERNAL` (‎-26) | ناسازگاری | از `FileManager_Init`: کتابخانه و برنامه با گزینه‌های متفاوت ساخته شده‌اند |

---

## ۱۳. محدودیت‌ها

- H.264 فقط پروفایل Baseline و نرم‌افزاری است: حدود ۱ تا ۳ فریم در ثانیه.
  برای ویدیوی روان MJPEG با ابعاد ۳۲۰×۲۴۰ بسازید.
- JPEG نرم‌افزاری CMYK را باز نمی‌کند.
- FLAC بالاتر از ۲۴ بیت و WAV بالای ۱۶ بیت پشتیبانی نمی‌شود.
- AAC فقط با دیکدری که خودتان ثبت کنید پخش می‌شود.
- پلیرها برای حلقهٔ اصلی نوشته شده‌اند؛ `Poll` را از وقفه صدا نزنید.
- ذخیرهٔ صدا: WAV، ADPCM و FLAC داخلی‌اند. MP3، Vorbis و Opus انکودر داخلی
  ندارند.
- ذخیرهٔ ویدیو فقط MJPEG در AVI است. ذخیرهٔ هم‌زمان ویدیوی در حال پخش وجود
  ندارد؛ `SimpleVideo_Convert` همان کار را فایل به فایل انجام می‌دهد.
- `Video_Convert` نرخ فریم را عدد صحیح می‌نویسد (۲۹٫۹۷ می‌شود ۳۰).

---

## ۱۴. سندهای دیگر

| سند | موضوع |
|---|---|
| [`OPTIONS_FA.md`](OPTIONS_FA.md) | همهٔ گزینه‌ها، اندازهٔ بافرها، کش |
| [`../board/README.md`](../board/README.md) | کد برد، دستورهای UART، تست‌ها |
| [`DSP_PROCESSING_BUDGET.md`](DSP_PROCESSING_BUDGET.md) | زمان مجاز پردازش داخل کالبک‌ها |
| [`OPUS_CUBEIDE.md`](OPUS_CUBEIDE.md) | ساخت Opus در CubeIDE |
| [`../README.md`](../README.md) | مرجع کامل API (انگلیسی) |
