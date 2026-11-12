/*
 * app_flow.h — گام‌هایی که main() صدا می‌زند، به همان ترتیبِ اجرا
 * ================================================================
 *
 * main.c فقط «چه کاری، به چه ترتیبی» را نشان می‌دهد؛ «چطور» اینجاست.
 * هر تابعِ App_* دقیقاً یک خط در main() است:
 *
 *   USER CODE 1      App_MpuUserRegions()
 *   USER CODE 2      App_PrintBanner() ... App_SdToPc()
 *   USER CODE WHILE  App_MusicCommandPoll() ... App_SdToPcCommandPoll()
 *
 * این فایل هدرِ مستقل نیست: فقط از main.c و بعد از defines.h، tests.h،
 * test_tft.h و بقیهٔ app_*.h شامل می‌شود (یک واحد ترجمه).
 *
 * برای ردکردن یک گروه مثال، همان یک خط را در main.c کامنت کنید.
 */
#ifndef FM_BOARD_APP_FLOW_H_
#define FM_BOARD_APP_FLOW_H_

/* تابعی که خطش در main.c کامنت شده نباید هشدار «unused» بدهد. */
#if defined(__GNUC__)
#define APP_STEP static __attribute__((unused)) void
#else
#define APP_STEP static void
#endif

/* ================================================================== */
/* گام‌های پیش از HAL_Init()                                          */
/* ================================================================== */

/*
 * ناحیهٔ ۳ MPU: قبلاً با دست داخل MPU_Config() نوشته شده بود.
 *
 *   D2 SRAM  0x30000000  256KB  بدون کش، بدون بافر
 *
 * همهٔ بافرهای section(".dma_buffer") روی این برد اینجا می‌نشینند (لاگ
 * SimpleAudio: dma buf=0x3001a020). بدون این ناحیه رینگ‌های I2S و بافر
 * bounce کش‌دار می‌مانند: «i2s_starve=0 ولی صدا بریده‌بریده».
 *
 * چرا اینجا و نه داخل MPU_Config(): MPU_Config() را CubeMX می‌سازد و بخش
 * کاربر ندارد؛ هر چیزی که آنجا با دست اضافه شود با اولین Generate پاک
 * می‌شود. این تابع از USER CODE 1 — «قبل» از MPU_Config() — صدا زده
 * می‌شود. MPU_Config() فقط ناحیه‌های ۰ تا ۲ خودش را می‌نویسد و بعد MPU را
 * روشن می‌کند؛ ناحیهٔ ۳ دست‌نخورده می‌ماند. مقادیر همان قبلی‌اند.
 *
 * اگر بعداً این ناحیه را در خود CubeMX تعریف کردید، این تابع و خطش در
 * main.c را حذف کنید.
 */
APP_STEP App_MpuUserRegions(void)
{
    MPU_Region_InitTypeDef r = {0};

    HAL_MPU_Disable();

    r.Enable           = MPU_REGION_ENABLE;
    r.Number           = MPU_REGION_NUMBER3;
    r.BaseAddress      = 0x30000000;
    r.Size             = MPU_REGION_SIZE_256KB;
    r.SubRegionDisable = 0x0;
    r.TypeExtField     = MPU_TEX_LEVEL0;
    r.AccessPermission = MPU_REGION_FULL_ACCESS;
    r.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    r.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);

    /* روشن‌کردن MPU با MPU_Config() است که بلافاصله بعد اجرا می‌شود. */
}

/* ================================================================== */
/* ۱) راه‌اندازی                                                       */
/* ================================================================== */

/* وضعیت کش و پورت — اگر dcache=0 دیدید یعنی MPU درست نیست. */
APP_STEP App_PrintBanner(void)
{
    printf("\n start \n");
    FM_Cache_Report();
    printf("port=%s  debug=%s\n", FM_PORT_NAME, FM_Debug_LevelName());
}

/* خط فرمان UART: DMA حلقوی + وقفهٔ Idle (پردازش در app_uart_commands.h) */
APP_STEP App_UartCommandsBegin(void)
{
    for (int i = 0; i < UART_RX_BUF_SIZE; i++)
    {
        uart_rx_dma_buffer[i] = 0xff;
    }
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart_rx_dma_buffer,
                                 UART_RX_BUF_SIZE);
    /* __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);  نیمه‌پر را خاموش می‌کند */
}

/* نوشتن با DMA روی SDRAM و خواندنِ دوباره (توابع در tests.h) */
APP_STEP App_SdramDmaTest(void)
{
    Generate_number();
    start_t = HAL_GetTick();
    SCB_CleanDCache_by_Addr(dataw, size_data * 4);
    HAL_SDRAM_Write_DMA(&hsdram1, (uint32_t *)SdRamBaseAddress, dataw,
                        size_data);
    HAL_Delay(10);
    SCB_InvalidateDCache_by_Addr((uint32_t *)SdRamBaseAddress,
                                 size_data * 4);
    ReadSdram();
    HAL_Delay(100);
}

/* نمایشگر SPI: اتصال درایور DMA و صفحهٔ تست (init_tft در test_tft.h) */
APP_STEP App_DisplayBegin(void)
{
    LcdSpiDma_Bind(&hspi2, tft_cs_GPIO_Port, tft_cs_Pin);
    init_tft();
    HAL_Delay(100);
}

/* تخصیص‌گر SDRAM، خود کتابخانه، JPEG سخت‌افزاری، حالت نمایش ویدیو */
APP_STEP App_LibraryBegin(void)
{
    FM_Result hw_img;
    FM_Result hw_vid;

    SdramAlloc_Init();
    printf("sdram self-test: %d free=%u\r\n",
           SdramAlloc_SelfTest(), (unsigned)SdramAlloc_FreeBytes());
    FileManager_Init();

    /* یک بار کافی است. مقدار برگشتی FM_Result است: 0 یعنی FM_OK (فعال
     * شد)، نه «غیرفعال».
     * FM_ENABLE_HW_JPEG = 0 (fm_board_config.h): واحد JPEG سخت‌افزاری وصل
     * نمی‌شود و JPEG/MJPEG با دیکودر نرم‌افزاری باز می‌شوند. */
#if FM_ENABLE_HW_JPEG
    hw_img = SimpleImage_UseHwJpeg(&hjpeg);
    hw_vid = SimpleVideo_UseHwJpeg();
#else
    hw_img = FM_ERR_UNSUPPORTED;
    hw_vid = FM_ERR_UNSUPPORTED;
#endif
    printf("hwjpeg img=%s vid=%s\n",
           (hw_img == FM_OK) ? "on" : File_GetErrorString(hw_img),
           (hw_vid == FM_OK) ? "on" : File_GetErrorString(hw_vid));

    /* present ویدیو: پیش‌فرض blocking (باس پشت‌سرهم) — توضیح در
     * App_LcdBlitVideo. حالت async با SIMPLE_VIDEO_ASYNC_PRESENT=1. */
#if defined(SIMPLE_VIDEO_ASYNC_PRESENT) && SIMPLE_VIDEO_ASYNC_PRESENT
    SimpleVideo_SetPresentWait(LcdSpiDma_Wait);
#else
    SimpleVideo_SetPresentWait(NULL);
#endif
}

/*
 * اختیاری — اولویت باس: I2S خیلی بالا، SPI نمایشگر پایین.
 * CubeMX همه را LOW می‌گذارد و انتقال ۳۰ms نمایشگر می‌تواند SDMMC را معطل
 * کند. با present blocking اثر درجه‌دوم است؛ فقط اگر با present async هنوز
 * رقابت دیدید APP_DMA_PRIORITY_TUNING را ۱ کنید و App_DmaPriorities() را
 * در USER CODE 2 صدا بزنید (بعد از MX_SPI2_Init/MX_I2S1_Init، وگرنه
 * Instance هنوز NULL است). SDMMC1 روی H7 از IDMA داخلی استفاده می‌کند.
 */
#ifndef APP_DMA_PRIORITY_TUNING
#define APP_DMA_PRIORITY_TUNING 0
#endif
#if APP_DMA_PRIORITY_TUNING
APP_STEP App_DmaPriorities(void)
{
    extern DMA_HandleTypeDef hdma_spi2_tx;
    extern DMA_HandleTypeDef hdma_i2s1_ext_tx;

    if (hdma_spi2_tx.Instance != NULL)
    {
        DMA_Stream_TypeDef *st = (DMA_Stream_TypeDef *)hdma_spi2_tx.Instance;

        st->CR = (st->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_LOW;
    }
    if (hdma_i2s1_ext_tx.Instance != NULL)
    {
        DMA_Stream_TypeDef *st =
            (DMA_Stream_TypeDef *)hdma_i2s1_ext_tx.Instance;

        st->CR = (st->CR & ~DMA_SxCR_PL_Msk) | DMA_PRIORITY_VERY_HIGH;
    }
}
#endif

/* ================================================================== */
/* ۲) مثال‌ها                                                          */
/* ================================================================== */

static void app_banner(const char *name)
{
    printf("\n*************************** %s ***************************\n",
           name);
}

/* یک مثال: اول نامش چاپ می‌شود، بعد اجرا. */
#define APP_RUN(fn) do { app_banner(#fn); (void)fn(); } while (0)

/* مثال‌های ۱ تا ۱۱ روی RAM-disk (examples_file.h) */
APP_STEP App_RunFileDemos(void)
{
    APP_RUN(FileDemo_Basic);                   HAL_Delay(100);
    APP_RUN(FileDemo_CopyMoveRename);          HAL_Delay(100);
    APP_RUN(FileDemo_Advanced);                HAL_Delay(100);
    APP_RUN(FileDemo_Directory);               HAL_Delay(100);
    APP_RUN(FileDemo_Json);                    HAL_Delay(100);
    APP_RUN(FileDemo_TextLineEdit);            HAL_Delay(100);
    APP_RUN(FileDemo_CrashSafeWrites);         HAL_Delay(100);
    APP_RUN(FileDemo_DataLogger);              HAL_Delay(100);
    APP_RUN(FileDemo_CrcChecksum);             HAL_Delay(100);
    APP_RUN(FileDemo_BmpScaleConvert);         HAL_Delay(100);
    APP_RUN(FileDemo_AudioCodecs);             HAL_Delay(100);
}

/* همان مثال‌ها روی SD، بعد مدیا و کنترل پخش
 * (examples_sdcard.h، examples_sdcard_media.h، examples_sdcard_player.h).
 * SdCard_End() عمداً صدا زده نمی‌شود: پخش‌کننده‌ها بعداً همین مونت را
 * لازم دارند. */
APP_STEP App_RunSdDemos(void)
{
    HAL_Delay(100);
    APP_RUN(SdCard_Begin);                     HAL_Delay(100);
    APP_RUN(SdDemo_CopyMoveRename);            HAL_Delay(100);
    APP_RUN(SdDemo_Advanced);                  HAL_Delay(100);
    APP_RUN(SdDemo_Directory);                 HAL_Delay(100);
    APP_RUN(SdDemo_Json);                      HAL_Delay(100);
    APP_RUN(SdDemo_TextLineEdit);              HAL_Delay(100);
    APP_RUN(SdDemo_CrashSafeWrites);           HAL_Delay(100);
    APP_RUN(SdDemo_DataLogger);                HAL_Delay(100);
    APP_RUN(SdDemo_CrcChecksum);               HAL_Delay(100);
    APP_RUN(SdDemo_BmpScaleConvert);           HAL_Delay(100);

    APP_RUN(SdMediaDemo_AudioWav);             HAL_Delay(100);
    APP_RUN(SdMediaDemo_VideoFmrv);
    APP_RUN(SdMediaDemo_CopySong);
    APP_RUN(SdMediaDemo_CopyVideo);
    APP_RUN(SdMediaDemo_MediaDetect);

    APP_RUN(SdPlayerDemo_MusicControls);
    APP_RUN(SdPlayerDemo_VideoControls);
    APP_RUN(SdPlayerDemo_RemoteButtons);
}

/* فرمت QSPI فقط با تأیید خودتان: اگر دکمهٔ سرویس دارید این را با آن
 * مقدار بدهید. FM_TRUE یعنی «اگر فلش مونت نشد، فرمتش کن». */
static FM_Bool service_button_confirmed = FM_TRUE;

/* فلش QSPI: فایل، مدیا، پخش، انتقال SD↔QSPI↔SDRAM
 * (examples_qspi*.h، qspi_transfers.h). مدل قطعه از JEDEC ID شناخته
 * می‌شود. QspiFlash_End() صدا زده نمی‌شود چون USB بعداً لازمش دارد.
 *
 * مونت دستی به‌جای QspiFlash_Begin() (مثلاً برای پخش از فلش):
 *
 *   static FM_QspiDevice qspi_dev;
 *   static FM_Storage    qspi_st;
 *   static FM_FS         qspi_fs;
 *
 *   if (QspiDriver_Begin(&qspi_dev, &qspi_st, &hqspi,
 *                      FM_QSPI_CHIP_AUTO) == FM_OK)
 *   {
 *       Storage_Mount("qspi");
 *       FMFS_Init(&qspi_fs);
 *       FS_Mount("/qspi", &qspi_fs);
 *       FlashPlay_Begin();               // پخش از فلش به‌جای SD
 *   }
 *
 * و برای فلشی که روی SPI معمولی است (نه QUADSPI):
 *
 *   FM_SpiFlash_Init(&qspi_dev, &my_spi_port, FM_QSPI_CHIP_AUTO,
 *                    sector_buf, sizeof(sector_buf));
 *   FM_Qspi_RegisterStorage(&qspi_dev, &qspi_st, "flash");
 */
APP_STEP App_RunQspiDemos(void)
{
    app_banner("qspi_transfers");
    if (QspiFlash_Begin() == FM_OK)
    {
        APP_RUN(QspiDemo_Basic);
        APP_RUN(QspiDemo_CopyMoveRename);
        APP_RUN(QspiDemo_Advanced);
        APP_RUN(QspiDemo_Directory);
        APP_RUN(QspiDemo_Json);
        APP_RUN(QspiDemo_TextLineEdit);
        APP_RUN(QspiDemo_CrashSafeWrites);
        APP_RUN(QspiDemo_DataLogger);
        APP_RUN(QspiDemo_CrcChecksum);
        APP_RUN(QspiDemo_BmpScaleConvert);

        APP_RUN(QspiMediaDemo_AudioDetectProcessSave);
        APP_RUN(QspiMediaDemo_VideoDetectToBgr565);
        APP_RUN(QspiMediaDemo_ImageEffectResizeSave);

        APP_RUN(QspiPlayerDemo_MusicControls);
        APP_RUN(QspiPlayerDemo_VideoControls);
        APP_RUN(QspiPlayerDemo_RemoteButtons);

        APP_RUN(TransferDemo_SdToQspi);       /* /config.json را اگر نبود می‌سازد */
        APP_RUN(TransferDemo_QspiToSdram);
        APP_RUN(TransferDemo_VerifyRoundtrip);
        APP_RUN(TransferDemo_AllCombinations);
    }
    else if (service_button_confirmed)
    {
        APP_RUN(QspiFlash_Format);   /* فقط بار اول */
    }
}

/* فلش SPI روی SPI4 (spi_flash_dma_port.h). بافر سکتور باید بیرون از DTCM
 * باشد تا DMA1/2 به آن برسند، و هم‌تراز با خط کش. */
static FM_SpiFlashPort spi_port;
static FM_QspiDevice  flash;
static uint8_t sector_buf[4096]
    __attribute__((section(".sdram_buffers"), aligned(32)));

APP_STEP App_RunSpiFlashDemos(void)
{
    app_banner("SpiFlash_Begin");
    SpiFlash_SetCs(GPIOE, GPIO_PIN_4);
    if (SpiFlash_Begin(&flash, &spi_port, &hspi4, FM_QSPI_CHIP_AUTO,
                            sector_buf, sizeof(sector_buf)) == FM_OK)
    {
        /* فلش نو: یک‌بار SpiFlash_Format(); */
        APP_RUN(SpiFlashDemo_Smoke);  /* write/read/verify              */
        APP_RUN(SpiFlashDemo_FileOps);         /* متن + باینری 1K + CRC + پوشه   */
        APP_RUN(SpiFlashDemo_SdToFlash);      /* SD→فلش با وریفای               */
        APP_RUN(SpiFlashDemo_RamRoundtrip);    /* RAM→فلش→RAM                    */
        APP_RUN(SpiFlashDemo_Stats);            /* erase/write/blank-skip/DMA?    */
    }
}

/* EEPROM روی I2C2 با DMA (i2c_eeprom_dma.h، eeprom_simple.h) */
APP_STEP App_RunEepromDemos(void)
{
    app_banner("I2cEeprom_Begin");
    if (I2cEeprom_Begin(FM_EEPROM_CHIP_24LC02, &hi2c2, 0x50) == FM_OK)
    {
        APP_RUN(I2cEepromDemo_Smoke);    /* پروب + بنویس/بخوان/تطبیق */
        APP_RUN(EepromDemo_Basic);
        APP_RUN(EepromDemo_SettingsStruct);
        APP_RUN(EepromDemo_WearAwareCounter);
        APP_RUN(EepromDemo_AsStorage);
    }
}

/* ================================================================== */
/* ۳) پخش‌کننده‌ها                                                     */
/* ================================================================== */

/* شروع ویدیو با قلاب‌هایش — هم در بوت، هم با دستور vstrt */
static void app_video_begin(void)
{
    if (SimpleVideo_Begin() != FM_OK)
    {
        printf(" failed SimpleVideo_Begin ");
    }
    Music_InstallVideoHooks();      /* شروع/پایان ویدیو + دادهٔ فریم */
    SimpleVideo_Start();
    SimpleVideo_SetAutoPlay(1);
}

/* music_activated (در defines.h): ۱ = موزیک، ۰ = ویدیو. اسلایدشو همیشه. */
APP_STEP App_PlayersBegin(void)
{
    if (music_activated == 1)
    {
        app_banner("simple_audio");
        if (SimpleAudio_Begin() != FM_OK)
        {
            printf("SimpleAudio_Begin() != FM_OK \n");
        }
        /* بعد از Begin: Begin حالت را پاک می‌کند و قلاب قبلی می‌افتد. */
        Music_InstallTaps();
    }

    app_banner("SimpleImage_Begin");
    SimpleImage_SetOutputSize(320, 240);
    SimpleImage_SetPresent(App_LcdBlit);
    SimpleImage_ShowTestPattern();  /* باید ۸ نوار عمودی تمیز باشد */
    HAL_Delay(3000);
    SimpleImage_Begin();

    if (music_activated == 0)
    {
        app_banner("SimpleVideo_Begin");
        SimpleVideo_SetPresent(App_LcdBlitVideo);
        SimpleVideo_SetOutputSize(320, 240);
        app_video_begin();
    }
}

/* ================================================================== */
/* ۴) کارت SD روی PC (USB MSC)                                         */
/* ================================================================== */
/*
 * پل USB در USB_DEVICE/App/usbd_storage_if.c به کتابخانه وصل است:
 *
 *   #include "usb_msc.h"                 (بدون USB_MSC_DEFINE_CONTEXT)
 *   STORAGE_Init_FS           -> UsbMsc_StorageInit
 *   STORAGE_GetCapacity_FS    -> UsbMsc_StorageGetCapacity
 *   STORAGE_IsReady_FS        -> UsbMsc_StorageIsReady
 *   STORAGE_IsWriteProtected_FS -> UsbMsc_StorageIsWriteProtected
 *   STORAGE_Read_FS           -> UsbMsc_StorageRead
 *   STORAGE_Write_FS          -> UsbMsc_StorageWrite
 *   STORAGE_GetMaxLun_FS      -> UsbMsc_StorageGetMaxLun
 */

/* همهٔ پخش‌ها بسته، SD به PC داده می‌شود و تا دستور «sdmcu» همین‌جا
 * می‌مانیم؛ بعد SD به MCU برمی‌گردد. دستورهای UART در این انتظار هم
 * پردازش می‌شوند وگرنه sdmcu هیچ‌وقت نمی‌رسد. HAL_Delay اینجا نگذارید:
 * USB از همین حلقه سرویس می‌شود.
 *
 * USB را UsbSd_ShowToPc() راه می‌اندازد (بار اول MX_USB_DEVICE_Init()،
 * بعد از آن USBD_Start). برای همین خط MX_USB_DEVICE_Init() در main() کامنت
 * است. Generate آن خط را برمی‌گرداند: یا دوباره کامنتش کنید، یا در CubeMX
 * ← Project Manager ← Advanced Settings برای MX_USB_DEVICE_Init تیک
 * «Do Not Generate Function Call» بزنید. */
APP_STEP App_SdToPc(void)
{
    SimpleVideo_Stop();
    SimpleImage_Stop();
    SimpleAudio_Stop();

    UsbSd_ShowToPc();

    printf("Sd For Pc \n");
    while (sd_pc_mcu == 1)
    {
        UsbSd_Poll();
        CmdMusic_ProcessPending();
    }
    sd_pc_mcu = 0;

    UsbSd_TakeBack();             /* بعد از Eject در ویندوز */
}

/* ================================================================== */
/* گام‌های حلقهٔ اصلی                                                  */
/* ================================================================== */

/* start / stop موزیک. وقفهٔ UART فقط music_cmd را می‌گذارد؛ کار فایل و
 * I2S اینجاست. */
APP_STEP App_MusicCommandPoll(void)
{
    uint8_t c;

    if (music_cmd == 0u)
    {
        return;
    }
    c = music_cmd;
    music_cmd = 0u;

    if (c == 1u)
    {
        SimpleVideo_Stop();         /* I2S را آزاد می‌کند */
        if (SimpleAudio_Begin() != FM_OK)
        {
            /* دابل-start: Begin وقتی پخش فعال است BUSY می‌دهد. پخش را
             * تمیز متوقف و دوباره راه‌اندازی می‌کنیم. */
            SimpleAudio_Stop();
            if (SimpleAudio_Begin() != FM_OK)
            {
                printf("SimpleAudio_Begin() failed twice\n");
            }
        }
        Music_InstallTaps();        /* بعد از هر Begin */
        tap_peak = 0;
        tap_calls = 0;
    }
    else if (c == 2u)
    {
        SimpleAudio_Stop();
    }
}

/* هر ۲ ثانیه: LED و بلندی زندهٔ صدا (تابهٔ PCM بعد از ولوم، یعنی همان
 * چیزی که به I2S می‌رود). اگر این عدد وسط پخش افت‌وخیز شدید دارد ولی
 * track peak بالاست، مشکل بعد از بافر است (I2S/تقویت‌کننده). */
APP_STEP App_HeartbeatPoll(void)
{
    static uint32_t last;
    static uint8_t  started;

    if (!started)
    {
        started = 1u;
        last = HAL_GetTick();
    }
    if ((HAL_GetTick() - last) < 2000u)     /* wrap-safe */
    {
        return;
    }

    HAL_GPIO_TogglePin(led_GPIO_Port, led_Pin);
    if (tap_calls != 0u)
    {
        printf("audio live: peak=%d taps=%lu\n",
               (int)tap_peak, (unsigned long)tap_calls);
        tap_peak = 0;
        tap_calls = 0;
    }
    last = HAL_GetTick();
}

/* vstrt / vstop. دستور UART فقط viedo_start را ۱ یا ۳ می‌کند. */
APP_STEP App_VideoCommandPoll(void)
{
    if (viedo_start == 1)
    {
        viedo_start = 2;

        SimpleAudio_Stop();
        SimpleVideo_SetPresent(App_LcdBlitVideo);
        SimpleVideo_SetOutputSize(320, 240);
        /* همان نرمالایزر موزیک (tap1) روی صدای ویدیو */
        SimpleVideo_SetPrePcmTap(app_audio_pre_tap, NULL);
        printf("SimpleVideo_Begin \n");
        app_video_begin();
    }

    if (viedo_start == 3)
    {
        viedo_start = 0;
        printf("SimpleVideo_Stop (vstop) \n");
        SimpleVideo_Stop();         /* I2S هم آزاد می‌شود */
    }
}

/* fers: پاک‌کردن کل فلش SPI */
APP_STEP App_FlashEraseCommandPoll(void)
{
    if (qspi_erase == 1)
    {
        qspi_erase = 2;
        printf("QspiFlash_Format \n");
        /* QspiFlash_Format(); */
        SpiFlash_EraseAll();
    }
}

/* sdpc: SD دوباره به PC داده می‌شود (تا sdmcu) */
APP_STEP App_SdToPcCommandPoll(void)
{
    if (sd_pc_mcu == 1)
    {
        App_SdToPc();
    }
}

/* ================================================================== */
/* یادداشت‌ها (کدهایی که در main.c کامنت بودند)                        */
/* ================================================================== */
/*
 * نمایشگر LTDC به‌جای TFT روی SPI — اول USER CODE 2:
 *     MX_LTDC_Init();
 *     SimpleVideo_UseLtdc(&hltdc);
 *
 * DMA2D بدون هندل CubeMX:
 *     SimpleImage_UseDma2d();          به‌جای SimpleImage_UseDma2dHandle()
 *
 * ضبط از میکروفون یا هر منبع دیگر:
 *     SimpleAudio_RecStart("/rec", 16000, 1);
 *     while (mic has data) SimpleAudio_RecWrite(buf, frames);
 *     SimpleAudio_RecStop();           -> /rec/rec_000001.wav
 *
 * کنترل ویدیو از کد:
 *     SimpleVideo_Pause();  SimpleVideo_SeekBy(+10.0);  SimpleVideo_Next();
 *     SimpleVideo_SaveFrame("/snap.bmp");                فریم فعلی
 *
 * کش: SCB_EnableICache/DCache را CubeMX در main() می‌گذارد. اگر در CubeMX
 * خاموششان کردید، FM_Cache_Enable() در USER CODE SysInit همان کار را
 * می‌کند. SCB_DisableDCache() در USER CODE 2 کل تنظیم MPU را بی‌اثر می‌کند.
 */

#endif /* FM_BOARD_APP_FLOW_H_ */
