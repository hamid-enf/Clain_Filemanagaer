# board/ — کد برد، بیرون از main.c

`Core/Src/main.c` فقط روند کلی را نشان می‌دهد: هر گام یک خط است. خودِ
کارها در هدرهای این پوشه‌اند. این هدرها مستقل نیستند؛ فقط از `main.c` و
به همین ترتیب شامل می‌شوند (یک واحد ترجمه):

| فایل | چه چیزی در آن است |
|---|---|
| `app_display.h` | `App_LcdBlit` و `App_LcdBlitVideo` (تحویل تصویر به TFT)، callbackهای SPI |
| `app_audio_hooks.h` | قلاب‌های PCM قبل و بعد از ولوم، نرمالایزر بلندی (AGC)، ضبط خودکار، `Music_*` |
| `app_video_hooks.h` | قلاب‌های شروع / پایان ویدیو و دادهٔ فریم |
| `app_save_hooks.h` | ذخیرهٔ موزیک / ویدیوی در حال پخش از راه کالبک‌ها: دستورهای `arec` `arecs` `vrec` `vrecs` |
| `app_uart_commands.h` | بافرهای UART، دستورهای ساده، و شامل‌کردن `top_api/code_cmd.h` |
| `app_flow.h` | همهٔ گام‌های `App_*` که `main()` صدا می‌زند، به ترتیب اجرا |
| `examples_file.h` | مثال‌های ۱ تا ۱۱ فایل روی RAM-disk |

## روند main()

```
USER CODE 1      App_MpuUserRegions()      ناحیهٔ ۳ MPU (D2 SRAM بدون کش)
USER CODE 2      ۱) راه‌اندازی             لاگ، SD، UART، SDRAM، نمایشگر، کتابخانه
                 ۲) مثال‌ها                هر گروه یک خط App_Run...Examples()
                 ۳) پخش‌کننده‌ها            App_PlayersBegin()
                 ۴) SD روی PC              App_SdToPc()  (تا دستور sdmcu)
USER CODE WHILE  Poll پخش‌کننده‌ها + دستورهای UART، هر کدام یک خط
```

برای ردکردن یک گروه مثال، همان یک خط را در `main.c` کامنت کنید. برای
تغییر رفتار یک گام، تابع هم‌نامش را در `app_flow.h` ویرایش کنید.

## قاعده‌ها

- **`main.c` فقط ASCII است.** CubeMX با هر Generate فایل را بازنویسی
  می‌کند و می‌تواند متن غیر ASCII بخش‌های USER CODE را خراب کند. توضیح‌های
  فارسی در همین هدرها هستند.
- **هیچ چیز بیرون از `USER CODE BEGIN / END` نیست.** دو بلوکی که قبلاً با
  دست وسط کد ساخته‌شده بودند (ناحیهٔ ۳ MPU داخل `MPU_Config()` و بلوک
  `#if 0` اولویت DMA بعد از `MX_DMA_Init()`) با اولین Generate پاک
  می‌شدند. حالا `App_MpuUserRegions()` و `App_DmaPriorities()` در
  `app_flow.h` هستند.
- **تنها خطِ دست‌خورده در کد ساخته‌شده** `//  MX_USB_DEVICE_Init();` است:
  USB را `UsbSd_ShowToPc()` راه می‌اندازد. Generate این خط را
  برمی‌گرداند؛ یا دوباره کامنتش کنید یا در CubeMX ← Project Manager ←
  Advanced Settings برای آن تیک «Do Not Generate Function Call» بزنید.
- بلوک قلاب‌های PCM در `app_audio_hooks.h` (از `#define AGC_TARGET_PEAK`
  تا خط `TAPS-BLOCK-END`) با `make tapstest` روی کامپیوتر اجرا می‌شود؛
  این دو نشانه را جابه‌جا نکنید.

## MPU

`MPU_Config()` را CubeMX می‌سازد (ناحیه‌های ۰ تا ۲)، ناحیهٔ ۳ از
`App_MpuUserRegions()` می‌آید:

| ناحیه | محدوده | حالت | از کجا |
|---|---|---|---|
| ۰ | کل فضای آدرس، زیرناحیه‌های ۰ تا ۲ و ۷ خاموش | بدون دسترسی | CubeMX |
| ۱ | SDRAM `0xC0000000`، ۳۲MB | کش‌دار write-back | CubeMX |
| ۲ | QSPI `0x90000000`، ۸MB | کش‌دار (حالت memory-mapped) | CubeMX |
| ۳ | D2 SRAM `0x30000000`، ۲۵۶KB | بدون کش، بدون بافر (`.dma_buffer`) | `app_flow.h` |

ناحیهٔ ۲ قبلاً در عمل اعمال نمی‌شد: بلوک دستی ناحیهٔ ۳ بین مقداردهی
ناحیهٔ ۲ و `HAL_MPU_ConfigRegion()` آن نشسته بود و مقادیرش را بازنویسی
می‌کرد. با بیرون‌رفتن آن بلوک، `MPU_Config()` همان چیزی است که CubeMX
می‌سازد و ناحیهٔ ۲ اعمال می‌شود.

---

# تاریخچه

بخش‌های زیر یادداشت‌های نوبت‌های قبلی‌اند و به همان شکل نگه داشته
شده‌اند. هر جا `board/main.c` آمده، منظور کدی است که حالا در
`Core/Src/main.c` و `board/app_*.h` است؛ `board/code_cmd_music.h` حالا
`top_api/code_cmd.h` است و `board/defines.h` همان `Core/Src/defines.h`.

## بعد از فلش چه ببینید

```
cache: icache=1 dcache=1
```

اگر `dcache=0` بود یعنی MPU هنوز مشکل دارد. اگر تصویر خراب شد،
`FM_CACHE_ENABLE` را در `include/fm_board_config.h` صفر کنید — نیازی
به دست‌زدن به `main.c` نیست.

## به‌روزرسانی — مسیر صدا/ویدیو (بعد از لاگ H.264 + PCM)

چهار تغییر در `main.c`:

1. **`start`/`stop` یوآرت دیگر داخل وقفه کار نمی‌کنند.** این دو دستور از
   `HAL_UARTEx_RxEventCallback` صدا زده می‌شوند. قبلاً همان‌جا
   `SimpleVideo_Stop()` (fade با busy-wait تا ۴۰ms + بستن فایل) و
   `SimpleAudio_Begin()` (اسکن پوشه، باز کردن فایل، پر کردن دو نیمهٔ DMA از
   SD) اجرا می‌شد. خواندن SD داخل ISR منتظر وقفهٔ SDMMC می‌ماند که
   نمی‌تواند وقفهٔ UART را قطع کند ⇒ هنگ موقع رفتن به موزیک. حالا فقط
   `music_cmd` ست می‌شود و کار در حلقهٔ اصلی انجام می‌شود (مثل `viedo_start`).

2. **هر دو `SimpleAudio_Poll()` و `SimpleVideo_Poll()` همیشه صدا زده
   می‌شوند.** قبلاً `SimpleAudio_Poll` پشت `music_activated==1` بود که هیچ‌جا
   ۱ نمی‌شد؛ بعد از `start` موزیک شروع می‌شد ولی ring آن پر نمی‌شد ⇒
   underrun و توقف. وقتی یکی از دو پخش‌کننده فعال نیست Poll آن بلافاصله
   برمی‌گردد.

3. **present ویدیو غیرهمزمان شد** (`LcdSpiDma_Begin` +
   `SimpleVideo_SetPresentWait(LcdSpiDma_Wait)`). انتقال SPI هر فریم
   (~۳۶ms برای 320×240) قبلاً به زمان decode اضافه می‌شد و در ۱۵fps
   (بودجهٔ ۶۶ms) فریم‌ها می‌افتادند. حالا با decode فریم بعدی هم‌پوشانی دارد.

4. **`SimpleImage_SetOutputSize` از داخل `App_LcdBlit` حذف شد** — هر بار
   یک RESIZE + blit دوم صف می‌کرد. `UseHwJpeg` هر ۲ ثانیه هم حذف شد؛
   مقدار برگشتی آن `FM_Result` است و `0` یعنی FM_OK (فعال)، نه غیرفعال.

### MPU — `.dma_buffer` هم‌اکنون non-cacheable است

`MPU_Config` واقع در `board/main.c` حالا چهار ناحیه دارد. ناحیهٔ ۳
(`0x30000000` به‌اندازهٔ 256KB) D2 SRAM را که لینکرِ این برد بافرهای
`__attribute__((section(\".dma_buffer\")))` را در آن می‌گذارد (لاگ
`SimpleAudio: dma buf=0x3001a020`)، به‌صورت `MPU_ACCESS_NOT_CACHEABLE` و
`MPU_ACCESS_NOT_BUFFERABLE` تعریف می‌کند.

قبلاً این ناحیه وجود نداشت و با D-Cache روشن، رینگ‌های I2S و بافر bounce
ی LCD به‌اشتباه cacheable می‌بودند. کتابخانه قبل از هر DMA
`FM_Cache_CleanForDma` را صدا می‌زند، پس «معمولاً» درست کار می‌کرد؛ ولی
حالت «profile نشان می‌دهد `i2s_starve=0` ولی صدا بریده‌بریده می‌شود»
دقیقاً همین ناحیهٔ جاافتاده بود. اگر لینکر شما `.dma_buffer` را جای
دیگری می‌گذارد، آدرس/اندازهٔ این ناحیه را از فایل `.ld` تطبیق دهید.

## به‌روزرسانی — مثال‌های کدک‌های جدید + دستورهای حالت پخش (PR #25)

### FileDemo_AudioCodecs (board/examples_file.h + board/main.c)

مثال جدید بعد از `FileDemo_BmpScaleConvert` اجرا می‌شود و هر ۶ دیکدری که اخیراً به
کتابخانه اضافه شده را روی فایل‌های واقعی SD تست می‌کند:

| فایل نمونه (در `/media` یا ریشهٔ SD) | کدک |
|---|---|
| `wav_adpcm_ima_22k.wav` | WAV IMA-ADPCM (فرمت 0x11) |
| `wav_alaw_8k.wav` | WAV A-law |
| `wav_mulaw_8k.wav` | WAV µ-law |
| `test_flac.flac` | FLAC (dr_flac) |
| `test_vorbis.ogg` | Vorbis (stb_vorbis) |
| `test_opus.ogg` | Opus (libopus — دیکد واقعی) |

فایل‌ها از ریپو (`audio_test/` و `test/vectors/`) روی SD کپی می‌شوند.
هر خط: فرمت/نرخ/کاناله + تعداد نمونهٔ دیکدشده + پیک. `peak>0` یعنی
دیکد واقعی؛ `silent` یعنی مسیر fallback (مثلاً `FM_USE_OPUS=0`).
خروجی مورد انتظار روی برد (H7 با کدک‌های فعال):

```
  wav_adpcm_ima_22k.wav    22050 Hz 1 ch ADPCM    22451 smp peak 32356  OK
  wav_alaw_8k.wav          22050 Hz 1 ch ALAW     22050 smp peak  8064  OK
  wav_mulaw_8k.wav         22050 Hz 1 ch ULAW     22050 smp peak  7932  OK
  test_flac.flac           22050 Hz 1 ch FLAC     22050 smp peak  7999  OK
  test_vorbis.ogg          22050 Hz 1 ch VORBIS   22050 smp peak  8147  OK
  test_opus.ogg            48000 Hz 1 ch OPUS     48000 smp peak  8343  OK
```

### دستورهای UART جدید (board/code_cmd_music.h)

| دستور | اثر (روی موزیک و ویدیو با هم) |
|---|---|
| `mode0` | STOP — پایان ترک → توقف (پرزنت) |
| `mode1` | NEXT — پایان ترک → بعدی، آخر لیست → توقف |
| `mode2` | REPEAT_ONE — تکرار همین ترک |
| `mode3` | REPEAT_ALL — بعدی و در آخر → اول (لوپ، پیش‌فرض قدیمی) |

مانند بقیهٔ دستورها، از ISR فقط رشته کپی می‌شود و اجرا در
`CmdMusic_ProcessPending()` در حلقهٔ اصلی است.

### باگ ADPCM که این مثال پیدا کرد (و درست شد)

`FileDemo_AudioCodecs` در اولین اجرا فقط ۱۵۳۶ نمونه از فایل ۱۱KB ای ADPCM دیکد
می‌کرد: مسیر `Audio_Read` برای IMA-ADPCM یک بلاک کامل می‌خواند ولی
نمونه‌های بیش از ظرفیت caller را دور می‌ریخت — با `count` کوچک بیشتر
فایل گم می‌شد. حالا دیکد ADPCM بلاک‌قاب‌فریم است: هر استریم یک
`WavAdpcmState` (lazy، heap) با بافر carry دارد، هر فراخوانی با هر
اندازه‌ای کل استریم را بدون از دست رفتن نمونه برمی‌گرداند
(تست رگرسیون: ۲۲۴۵۱ نمونه یکسان با chunk های ۲۵۷ و ۴۰۹۶ تایی).
`Audio_Seek` هم روی مرز بلاک فرود می‌آید و `block_align` از fmt چانک
WAV در `FM_AudioInfo.block_align` نگه‌داری می‌شود.

## نوبت بعدی: audio_record_with_tap.h، دیسپچ تمیز UART، و عیب‌یابی بلندی صدا

### تست واقعی audio_record_with_tap.h روی هاست (`make audiorecordtest`)

هدر `examples/audio_record_with_tap.h` دیگر فقط «چشمی» چک نمی‌شود؛
`tools/audio_record_host_test.c` همان هدر را با HAL شبیه‌سازی‌شده (`tools/halstub`)
کامپایل و **اجرا** می‌کند و در زنجیرهٔ `make check` هم هست:

```
  SimpleAudio_SetDataCallback/SetMonitorCallback  PASS
  my_pre_dsp: +6 dB with saturation              PASS
  SimpleVideo callbacks + GetFrame/AudioDataRead  PASS
  خروجی فقط بلندگو / فقط کالبک / هر دو، ماندگاری بعد از Stop/Begin  PASS
  SimpleAudio_RecStart→RecWrite→RecStop          PASS
  بازگشایی WAV ضبط‌شده: 16000Hz/1ch، ۴۸۰۰ فریم، پیک > 9000  PASS
  Directory_Copy تودرتو + محتوای فایل‌ها     PASS
```

`audio_record_folder_copy(src, dst)` حالا پارامتری شد؛ روی برد مثل قبل
`audio_record_folder_copy(NULL, NULL)` = کپی `/qspi/myProject` →
`/sd/backup/myProject` با `Directory_Copy` (تا `FM_DIR_MAX_DEPTH`،
برعکس `Directory_CopyTree` که برداشته شد).

### بازنویسی `board/code_cmd_music.h` (بدون هنگ)

- **هیچ چاپی در ISR نیست** — `printf` همان `FM_Log` است و FM_Log خودش
  UART است؛ چاپ داخل وقفهٔ UART یعنی بن‌بست. سرریز/فرمان طولانی حالا
  فقط پرچم می‌گذارد و از حلقهٔ اصلی چاپ می‌شود.
- **snapshot با قفل کوتاه IRQ** در `CmdMusic_ProcessPending()` — دیگر
  فرمانی نصف‌کاره اجرا نمی‌شود.
- **جدول فرمان** به‌جای زنجیرهٔ if-else حرف‌به‌حرف؛ همهٔ نام‌های قبلی
  حفظ شده‌اند + alias های `vstart` / `vpause` / `vresume`.
- شاخهٔ مردهٔ تکراری `vstrt` (که هیچ‌وقت اجرا نمی‌شد) حذف شد؛ `vstrt`
  مثل قبل `viedo_start=1` می‌گذارد و شروع واقعی در حلقهٔ اصلی است.
- بایت‌های غیرچاپی رد می‌شوند (ضد نویز خط).

### بلندی صدا (گزارش «بعضی آهنگ‌ها کم و زیاد می‌شوند»)

دیکدر MP3 کتابخانه سالم است (تست هاست: پیک 17637/18255 روی
tone vectors) پس سه تله‌متری صادقانه اضافه شد تا ریشه روی برد خودش
را نشان دهد:

1. `AudioDma: start_current: track peak=… (%… fs)` — پیک **هر ترک در شروع**
   با اسکن تمام نمونه‌ها (قبلاً `buf check` هر ۶۴مین نمونه را می‌دید
   و عدد 311 گمراه‌کننده بود).
2. `SimpleAudio: buf check` حالا تمام نمونه‌ها را می‌شمارد.
3. هر ۲ ثانیه حین پخش: `audio live: peak=… taps=…` از تابهٔ PCM
   (بعد از ولوم، دقیقاً همان چیزی که به I2S می‌رود).

تفسیر: اگر track peak بالا ولی `audio live` وسط آهنگ سقوط می‌کند →
مشکل بعد از بافر (I2S/تقویت‌کننده/ولوم سخت‌افزاری)؛ اگر خود ترک
پیک پایین دارد → فایل همین‌قدر آرام ضبط/ریپ شده است (فرق MP3 و WAV
در لاگ قبلی همین‌جا مشخص می‌شود). `start` دوباره هم دیگر خطا نمی‌دهد:
پخش تمیز stop و دوباره begin می‌شود.

## audio_record_with_tap.h روی برد: قلاب‌ها + دستورهای اتوماتیک (299f201 → بعد از آن)

بخش «قلاب‌های PCM» در `board/main.c` بازسازی شد تا از روی برد هم مشخص
باشد کدام تابع کِی اجرا می‌شود:

```
فایل → دیکد
  ▼
۱) app_audio_pre_tap   ← قبل از پردازش صدا (قبل از ولوم) — DSP/نرمالایزر
  ▼
۲) ولوم نرم‌افزاری (vol50/vol90)
  ▼
۳) app_audio_pcm_tap         ← بعد از پردازش صدا (بعد از ولوم) — VU/تله‌متری/ضبط
  ▼
DMA → I2S → بلندگو
```

نصب هر دو با `Music_InstallTaps()` انجام می‌شود: `SimpleAudio_SetDataCallback`
(قبل از ولوم) و `SimpleAudio_SetMonitorCallback` (بعد از ولوم). کالبک‌ها
ماندگارند؛ یک‌بار کافی است و با `Stop`/`Begin` پاک نمی‌شوند. اسلات نرمالایزر
بلندی داخل `app_audio_pre_tap` است.

مسیر صدا با یک تابع انتخاب می‌شود:

| دستور UART | تابع | نتیجه |
|---|---|---|
| `aout1` | `SimpleAudio_SetOutput(FM_AUDIO_OUT_SPEAKER)` | فقط بلندگو؛ کالبک‌ها صدا زده نمی‌شوند |
| `aout2` | `SimpleAudio_SetOutput(FM_AUDIO_OUT_CALLBACK)` | فقط کالبک؛ بلندگو ساکت |
| `aout3` | `SimpleAudio_SetOutput(FM_AUDIO_OUT_BOTH)` | هر دو (پیش‌فرض) |
| `vout0` … `vout3` | `SimpleVideo_SetAudioOutput(...)` | همان برای صدای ویدیو؛ `vout0` = صدا اصلاً خوانده نشود |

### قلاب‌های شروع/پایان (DSP می‌داند چه چیزی پردازش می‌شود)

برای پردازش، لایهٔ DSP باید بداند «کدام موزیک/ویدیو» در حال پردازش است:

- **آهنگ:** `SimpleAudio_SetTrackStartCallback` / `SetTrackEndCallback` — شروع: مسیر +
  کدک/نرخ/کانال؛ پایان: همان مسیر وقتی ترک تمام/قطع شود. در
  `Music_InstallTaps()` نصب می‌شوند و با `app_audio_track_start/end` چاپ می‌شوند.
- **ویدیو:** `SimpleVideo_SetTrackStartCallback` / `SetTrackEndCallback` —
  شروع: مسیر + ابعاد + fps؛ پایان: مسیر + شمار فریم‌های دیده‌شده.
- **فریم داده:** `SimpleVideo_SetFrameCallback` — هر فریم دیکدشده (تبدیل‌شده
  به `sv.format`، پیش‌فرض RGB565) را تحویل می‌دهد؛ **حتی وقتی `SetPresent(NULL)`
  است** (حالت فقط-داده). این همان مسیرِ «۶۰٪ مواقع فقط داده می‌خواهیم» است؛
  بافر دوبل است، پس اگر داده را تا فراخوانی بعدی نگه می‌دارید در بافر خودتان کپی کنید.
  نصب با `Music_InstallVideoHooks()`؛ این‌ها هم ماندگارند.

بعدیِ دستی ویدیو (`SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP)` + `SimpleVideo_Next()`)
همان «فیلم بعدی دست خودم باشه» است: ویدیو سر EOF می‌ایستد و فقط وقتی خودتان
`vnext` بدهید جلو می‌رود.

### دستورهای UART جدید

| دستور | کار |
|---|---|
| `tap1` / `tap0` | **نرمالایزر بلندی (AGC)** قبل از ولوم: روشن / خاموش — تراک آرام تا ×۲ (+۶dB) تقویت می‌شود، تراک بلند دست‌نخورده می‌ماند (بدون کلیپ)؛ روی موزیک **و** ویدیو |
| `rectest` | ضبط اتوماتیک ۵ ثانیه از «صدای بلندگو» → `/rec/rec_XXXXXX.wav` (باز/نوشتن فایل فقط در حلقهٔ اصلی — بدون هنگ) |
| `vinfo` | مشخصات ترک صوتی ویدیو (نرخ/کانال/بیت/تعداد نمونه) |
| `pools` | دیاگ خطای −۱۴: هندل‌های باز + `FM_Alloc` شکست‌ها + SDRAM آزاد |
| `vvol2` | ولوم ویدیو = ۲۰۰٪ (تقویت نرم‌افزاری، اجازهٔ بالای ۱۰۰) |
| `istart` | اسلایدشو تصویر از حلقهٔ اصلی (همان `SimpleImage_Begin` بوت، بدون ری‌ست) |
| `EnableAudio` | صدای ویدیو خاموش/روشن (تست ایزولهٔ تصویر): `FM_AUDIO_OUT_NONE` / `_BOTH` |
| `vmut` / `vunmut` | بلندگو ساکت ولی داده به کالبک می‌رسد (`FM_AUDIO_OUT_CALLBACK`) / هر دو |
| `vnopres` | **تست A/B کراکل**: نمایش LCD ویدیو خاموش/روشن؛ دیکد و صدا ادامه دارد. اگر بدون LCD صدا تمیز شد ⇒ ریشهٔ کراکل رقابت DMA ارائهٔ LCD با I2S است |

`rectest` این‌طور کار می‌کند: قلاب فقط در حافظه کپی می‌کند (سبک)؛
`Music_RecPump()` در `while(1)` فایل را باز/نوشته/بسته می‌کند و بعد از
۵ ثانیه خودکار `RecStop` می‌زند و مسیر فایل را چاپ می‌کند.

### ذخیرهٔ چیزی که پخش می‌شود، از راه کالبک‌ها (`arec`، `vrec`)

کار اصلی در `top_api/examples_save_media.h` است (توابع `SaveDemo_*`)؛
`board/app_save_hooks.h` آن را به کالبک‌های همین برد زنجیر می‌کند.

| دستور | کار | فایل |
|---|---|---|
| `arec` | از تکهٔ بعدی، موزیک در حال پخش ذخیره می‌شود | `/save/music_cb.wav` |
| `arecs` | پایان ذخیرهٔ موزیک؛ طول فایل چاپ می‌شود | – |
| `vrec` | از فریم بعدی، تصویر و صدای ویدیو ذخیره می‌شود؛ در پایان ویدیو خودش بسته می‌شود | `/save/video_cb.avi` |
| `vrecs` | پایان ذخیرهٔ ویدیو، وسط پخش | – |

```
موزیک   دیکدر ─> app_save_music_cb ─┬─> app_audio_pre_tap        نرمالایزر (tap1 / tap0)
                                    └─> SaveDemo_MusicCallback    کپی در رینگ
        while (1): SaveDemo_MusicRecPump()                        رینگ -> فایل

ویدیو   فریم ─> app_save_video_frame_cb ─┬─> app_video_frame_cb         شمارندهٔ برد
                                         └─> SaveDemo_VideoFrameCallback  JPEG -> فایل
        صدا  ─> app_save_video_sound_cb ─┬─> app_audio_pre_tap           نرمالایزر
                                         └─> SaveDemo_VideoSoundCallback  کپی در رینگ
        while (1): SaveDemo_VideoRecPump()                         رینگ -> فایل
```

- خروجی صدا باید «کالبک» را داشته باشد: `aout3` برای موزیک و `vout3` برای
  ویدیو (هر دو پیش‌فرض‌اند). با `aout1` فایل موزیک خالی می‌ماند و با `vout1`
  فایل ویدیو تراک صدا ندارد.
- با `tap1` همان صدای نرمال‌شده در فایل می‌نشیند (ذخیره بعد از نرمالایزر
  زنجیر شده)؛ با `tap0` صدای خام فایل.
- برای اینکه صدا و تصویر فایل ویدیو از یک نقطه شروع شوند: `vstop`، بعد
  `vrec`، بعد `vstrt`.
- فشرده‌سازی JPEG هر فریم وقت می‌برد؛ پخش‌کننده در این مدت فریم جا
  می‌اندازد و جای آن‌ها در فایل تصویر قبلی تکرار می‌شود. طول فایل و صدا
  درست می‌ماند. سرعت روی برد اندازه‌گیری نشده است.
- پرش وسط `vrec` (`vnp10` / `vpn10`): فایل ادامه پیدا می‌کند و در جای پرش
  برش می‌خورد؛ صدا و تصویر بعد از آن زیر هم می‌مانند. در لاگ پایان،
  `seeks` تعداد برش‌هاست. روی برد آزمایش نشده است.
- اگر وسط `vrec` برق رفت یا برد ریست شد، `/save/video_cb.avi.idx` کنار فایل
  می‌ماند؛ `VideoFile_Repair("/save/video_cb.avi", NULL)` فایل را کامل
  می‌کند (راهنما، بخش ۸.۴).
- دو مثال سادهٔ همان فایل در بوت اجرا می‌شوند (`App_RunSaveDemos()` در
  `main.c`): `/save/tone.wav`، `/save/tone.flac`، `/save/bars.avi`.
- روی کامپیوتر: `make saveexamplestest` همهٔ این مثال‌ها را با پخش‌کننده‌های
  واقعی اجرا می‌کند و فایل‌ها را بازخوانی و مقایسه می‌کند.

### تست این بخش روی هاست (`make tapstest`، داخل `check`)

بلوک قلاب‌ها **عیناً از خود `board/app_audio_hooks.h`** استخراج و با استاب‌های
HAL اجرا می‌شود (`tools/board_taps_host_test.c`) — نه کپیِ دستی. پاس:
نصب قلاب‌ها، رفتار نرمالایزر (پلهٔ اول ×۱.۱۲۵، سقف ×۲، تراک بلند
بدون کلیپ، خاموش)، کل ماشین‌حالت ضبط (cfg → RecStart → پمپ →
RecStop خودکار)، `Directory_Copy` واقعی (هر دو شاخه‌ی موفق/خطا)،
vinfo، و دستورهای vvol2/vnopres/EnableAudio/istart.

> نکتهٔ ظریف تست: هر چانک باید «تازه» به قلاب داده شود (مثل دکدر
> واقعی). اگر همان بافر تقویت‌شده دوباره تغذیه شود، AGC خروجی خودش
> را دنبال می‌کند و فیدبک مثبت می‌سازد — این خطا را همین تست گرفت.

### فیکس کتابخانه‌ای که این تست پیدا کرد

کپیِ پوشه (`Directory_Copy`) اول `src` را باز می‌کند و بعد پوشهٔ مقصد را
می‌سازد — هر کپیِ ناموفق یک پوشهٔ خالی نیمه‌کاره جا نمی‌گذارد. (نسخهٔ
قدیمیِ `Directory_CopyTree`/`MoveTree` با مبدأِ غایب مقصد را می‌ساخت و
بعد خطا می‌داد؛ آن واریانت‌ها حذف شدند.)

## نوبت بعدی: عیب‌یابی لاگ سخت‌افزار (خش‌خش، صدای بریده‌بریده ویدیو، -14)

### ۱) خطای −۱۴ (vcopy) — لاگ جدید نتیجه را عوض کرد
در لحظهٔ خطا `pool=1/8` بود؛ یعنی pool هشت‌اسلاتی **پر نیست** و −۱۴ از
شکست تخصیص `FM_Alloc` می‌آید (نه نشت هندل). سه مظنون:

1. **SDRAM allocator ثبت نشده** — اگر `SdramAlloc_Init()` بدون
   `FM_Allocator_Set(...)` بماند، `FM_Alloc` → `malloc` نیونیوک با
   heap فقط ۳۲KB لینکر می‌رود و در کپی‌های بزرگ می‌شکند.
2. **تداخل پول SDRAM با سکشن `.sdram_buffers`** لینکر (0xC0100000).
3. **داخل FatFs**: `FF_USE_LFN==3` (بافر اسم بلند با `ff_memalloc`
   روی همان heap کوچک) یا `FF_FS_LOCK>0` (سهمیهٔ فایل هم‌زمان).

دستور `pools` حالا هر سه را جدا می‌کند:
`pool: X/8 هندل | FM_Alloc fail=N | SDRAM free=MK`

- `fail>0` ⇒ مشکل از کمبود/فرگمانت SDRAM است (ردیف ۱/۲ بالا).
- `fail=0` و pool<8 و باز −۱۴ ⇒ داخل FatFs است (ردیف ۳):
  در ffconf.h یا `FF_USE_LFN` را 1/2 کنید، یا `ff_memalloc/ff_memfree`
  را به `FM_Alloc/FM_Free` وصل کنید:
  ```c
  void *ff_memalloc(size_t m) { return FM_Alloc(m); }
  void ff_memfree(void *p)    { FM_Free(p);        }
  ```
  (و `FF_FS_LOCK` را 0 بگذارید.)

### ۱ب) خطای −۶ (RecStart) — بافر هدر WAV روی پشته بدون تراز DMA
`AudioFileWriter_Open` هدر ۴۴بایتی را در بافر استکیِ ۱‌بایتی‌تراز
می‌نوشت؛ درایور SDMMC روی H7 با بافر غیرتراز خطای I/O می‌دهد (روی
هاست PosixFS دیده نمی‌شد). فیکس: `FM_DMA_ALIGNED` روی بافر هدر
(`src/fm_audio.c`). اگر باز هم −۶ آمد، خروجی `rd_errs=` در پروفایل
ویدیو و `pools` را بفرستید.

### ۲) صدای بریده‌بریدهٔ ویدیو (و خش‌خش هنگام تعویض عکس) = تزاحم DMA
لاگ شما `i2s_starve=0` داشت یعنی حلقهٔ نرم‌افزاری صدا هیچ‌وقت خالی نشد؛
پس بریدگی از سمت سخت‌افزار است: استریم DMA ارسال I2S با استریم DMA
ال‌سی‌دی (و MDMA/DMA2D هنگام MJPEG) روی همان بس تزاحم می‌کنند و FIFO
کوچک I2S دیر سرویس می‌شود → TX underrun = خش‌خش، بی‌آنکه حلقه خالی شده
باشد. این توضیح می‌دهد چرا MJPEG سبک (DMA-heavy) بدترین صدا را دارد و
H.264 سنگینِ نرم‌افزاری بهتر است — دقیقاً مشاهدهٔ شما.

فیکس: `SimpleI2S_BoostDmaPriority()` (در simple_i2s_mux.h، خودکار در
اولین `SimpleI2S_Take`) اولویت استریم DMA ارسال I2S را VERY_HIGH می‌کند
و `LcdSpiDma` اولویت LCD را LOW — لاگِ راه‌اندازی
`SimpleI2S: TX DMA priority = VERY_HIGH` تأییدش می‌کند.

### ۳) صدای ویدیو کم‌تر از موزیک
مسیر ولوم ویدیو و موزیک یکسان است (۱۰۰٪ = بی‌تغییر). راه اصلی که
الان پیاده شده، **نرمالایزر بلندی (AGC) در pre-tap** است که روی موزیک
و ویدیو هر دو نصب می‌شود (`tap1`): پیک ورودی را دنبال می‌کند، تراک
آرام را تا حداکثر ×۲ (+۶dB) بالا می‌برد و تراک بلند را دست نمی‌زند
(گین هرگز < ۱ نمی‌شود) — پس «کم بودن» و «کلیپ بعد از بوست دستی» هر
دو حل می‌شوند. `SimpleVideo_SetVolume` هم تا **۲۰۰٪** می‌گیرد
(`vvol2`) برای تنظیم دستی با اشباع کنترل‌شده.

### تست هاست
`make tapstest` دو تست جدید دارد: `File_PoolUsed()==2` بعد از دو بازکردن
و `==0` بعد از Close (+ اجرای `File_PoolDump`). کل زنجیره سبز:
212/212 + audiorecordtest + tapstest + asan + 8 پورت.
