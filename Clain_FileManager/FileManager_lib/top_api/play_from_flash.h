/*
 * play_from_flash.h — پخش موسیقی و ویدیو از فلش به‌جای SD
 * ===========================================================
 *
 * چرا سریع‌تر است
 * ---------------
 * هر بلوکی که از SD کارت می‌خوانید یک تراکنش SDMMC است: فرمان، انتظار،
 * پاسخ، داده. فلش QSPI در حالت نگاشت اصلاً فرمانی ندارد — خواندن یک
 * دسترسی حافظه‌ای ساده است.
 *
 * پس الگو این می‌شود: یک‌بار از SD به فلش کپی کن، بعد از فلش پخش کن.
 *
 * سه سطح استفاده
 * --------------
 *   ۱) خودکار کامل — یک خط راه‌اندازی، بعد همیشه از سریع‌ترین جا پخش
 *      می‌شود:
 *
 *          FlashPlay_Begin();
 *          FlashPlay_PlayVideo("/videos/clip.avi");   // خودش کش می‌کند
 *
 *   ۲) کنترل دستی — خودتان تصمیم بگیرید چه چیزی کش شود:
 *
 *          FM_MediaCache_Resolve(src, path, sizeof(path));
 *          SimpleVideo_Play(path);
 *
 *   ۳) کپی صریح — مثلاً موقع راه‌اندازی، همهٔ یک پوشه را ببرید روی فلش:
 *
 *          FlashPlay_PreloadFolder("/videos");
 *
 * چه وقت واقعاً کمک می‌کند
 * ------------------------
 * وقتی فایل چند بار پخش می‌شود، یا SD کند است، یا می‌خواهید کارت را
 * دربیاورید و پخش ادامه یابد.
 *
 * وقتی کمک نمی‌کند: فایل یک‌بار پخش می‌شود و بزرگ است — کپی بیشتر از
 * سودش طول می‌کشد. سیاست IF_SMALL همین را مدیریت می‌کند.
 *
 * هشدار فرسودگی: هر کپی یک چرخهٔ نوشتن روی فلش است (حدود ۱۰۰ هزار بار
 * برای هر سکتور). برای فایل‌هایی که مدام عوض می‌شوند کش نگذارید.
 */
#ifndef FM_TOPAPI_PLAY_FROM_FLASH_H
#define FM_TOPAPI_PLAY_FROM_FLASH_H

#include <stdio.h>
#include <string.h>

#include "file_manager.h"
#include "fm_mediacache.h"
#include "fm_transfer.h"
#include "fm_board_config.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

/* بزرگ‌ترین فایلی که ارزش کپی دارد. برای ویدیوی ۳۲۰x۲۴۰ چند مگابایت
 * منطقی است؛ اگر فلش کوچکی دارید کمترش کنید. */
#ifndef FLASH_PLAY_MAX_CACHED_FILE
#define FLASH_PLAY_MAX_CACHED_FILE (4u * 1024u * 1024u)
#endif

/* بیشترین فضایی که کش بگیرد. صفر = تا جایی که فلش جا دارد. */
#ifndef FLASH_PLAY_MAX_CACHE_TOTAL
#define FLASH_PLAY_MAX_CACHE_TOTAL (24u * 1024u * 1024u)
#endif

/* پوشهٔ کش روی فلش. */
#ifndef FLASH_PLAY_CACHE_ROOT
#define FLASH_PLAY_CACHE_ROOT "/qspi/.mcache"
#endif

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */
/*
 * اگر فلش نباشد یا mount نشده باشد، این تابع تمیز شکست می‌خورد و
 * همه‌چیز مثل قبل از SD پخش می‌شود — فقط کندتر.
 */
static FM_Result FlashPlay_Begin(void)
{
    FM_MediaCacheConfig cfg;
    FM_Result r;

    puts("\n**************** FlashPlay_Begin ****************");

    memset(&cfg, 0, sizeof(cfg));
    cfg.cache_root      = FLASH_PLAY_CACHE_ROOT;
    cfg.policy          = FM_MEDIACACHE_IF_SMALL;
    cfg.max_file_bytes  = FLASH_PLAY_MAX_CACHED_FILE;
    cfg.max_total_bytes = FLASH_PLAY_MAX_CACHE_TOTAL;
    /* بررسی CRC بعد از کپی: کمی کندتر ولی مطمئن. برای فلشی که ممکن
     * است فرسوده باشد ارزشش را دارد. */
    cfg.verify          = FM_TRUE;

    r = FM_MediaCache_Init(&cfg);
    if (r != FM_OK)
    {
        puts("  کش در دسترس نیست — پخش مستقیم از SD ادامه می‌یابد.");
        return r;
    }
    printf("  کش آماده: %s (حداکثر %lu KB برای هر فایل)\n",
           FLASH_PLAY_CACHE_ROOT,
           (unsigned long)(FLASH_PLAY_MAX_CACHED_FILE / 1024u));
    return FM_OK;
}

static void FlashPlay_End(void)
{
    FM_MediaCacheStats st;

    FM_MediaCache_GetStats(&st);
    printf("  کش: %lu بار آماده بود، %lu بار کپی شد، %lu رد شد\n",
           (unsigned long)st.hits, (unsigned long)st.misses,
           (unsigned long)st.bypasses);
    if (st.bytes_copied != 0u)
    {
        printf("  مجموع کپی‌شده: %lu KB\n",
               (unsigned long)(st.bytes_copied / 1024u));
    }
    FM_MediaCache_Deinit();
    puts("**************** FlashPlay_End ****************\n");
}

/* ------------------------------------------------------------------ */
/* FlashPlay_PlayVideo — پخش ویدیو، خودکار از سریع‌ترین جا                          */
/* ------------------------------------------------------------------ */
/*
 * تنها تفاوت با قبل: یک خط Resolve قبل از Play.
 *
 * اگر کش فعال باشد و فایل مناسب باشد، از فلش پخش می‌شود. وگرنه دقیقاً
 * همان مسیر اصلی. کد شما به هر دو حالت بی‌تفاوت است.
 */
static FM_Result FlashPlay_PlayVideo(const char *path)
{
    char play[FM_PATH_MAX];
    FM_Result r;

    r = FM_MediaCache_Resolve(path, play, sizeof(play));
    if (r != FM_OK)
    {
        return r;
    }
    if (strcmp(play, path) != 0)
    {
        printf("  پخش از فلش: %s\n", play);
    }
    else
    {
        printf("  پخش مستقیم: %s\n", play);
    }

    /* اینجا پخش‌کنندهٔ واقعی را صدا بزنید:
     *     return SimpleVideo_Play(play);
     */
    return FM_OK;
}

/* همان برای موسیقی. */
static FM_Result FlashPlay_PlayMusic(const char *path)
{
    char play[FM_PATH_MAX];
    FM_Result r;

    r = FM_MediaCache_Resolve(path, play, sizeof(play));
    if (r != FM_OK)
    {
        return r;
    }
    printf("  موسیقی: %s\n", play);
    /*     return SimpleAudio_Play(play);   */
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* FlashPlay_PreloadFolder — پیش‌بارگذاری یک پوشه                                       */
/* ------------------------------------------------------------------ */
/*
 * موقع راه‌اندازی همهٔ فایل‌های یک پوشه را روی فلش ببر. بعد از این،
 * پخش هیچ‌وقت منتظر SD نمی‌ماند — و حتی می‌توانید کارت را دربیاورید.
 *
 * چون هر کپی یک چرخهٔ نوشتن است، این را در هر بوت صدا نزنید؛ فقط وقتی
 * محتوا عوض شده. (Resolve خودش تشخیص می‌دهد که از قبل هست و دوباره
 * کپی نمی‌کند، پس صدا زدن دوباره‌اش هم بی‌ضرر است — فقط کمی وقت
 * می‌گیرد.)
 */
static FM_Result FlashPlay_PreloadFolder(const char *folder)
{
    FM_Dir *d;
    FM_DirEntry e;
    uint32_t done = 0;
    uint32_t skipped = 0;

    printf("\n  پیش‌بارگذاری %s ...\n", folder);

    d = Directory_Open(folder);
    if (d == NULL)
    {
        printf("  %s باز نشد\n", folder);
        return FM_ERR_NOT_FOUND;
    }
    while (Directory_Read(d, &e) == FM_OK)
    {
        char src[FM_PATH_MAX];
        char dst[FM_PATH_MAX];

        if (e.type == FM_ITEM_DIR)
        {
            continue;
        }
        if (snprintf(src, sizeof(src), "%s/%s", folder, e.name) < 0)
        {
            continue;
        }
        if (FM_MediaCache_Resolve(src, dst, sizeof(dst)) == FM_OK &&
            strcmp(src, dst) != 0)
        {
            done++;
        }
        else
        {
            skipped++;
        }
    }
    (void)Directory_Close(d);

    printf("  %lu فایل روی فلش، %lu رد شد\n",
           (unsigned long)done, (unsigned long)skipped);
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* FlashPlay_Benchmark — مقایسهٔ سرعت                                               */
/* ------------------------------------------------------------------ */
/*
 * برای اینکه ببینید کش واقعاً چقدر کمک می‌کند، نه اینکه فرض کنید.
 *
 * زمان را با HAL_GetTick بگیرید و همان فایل را یک‌بار از SD و یک‌بار
 * از فلش کامل بخوانید.
 *
 *     uint32_t t0 = HAL_GetTick();
 *     ... خواندن کامل فایل ...
 *     uint32_t ms = HAL_GetTick() - t0;
 */
static void FlashPlay_Benchmark(const char *path, uint32_t (*get_ms)(void))
{
    static uint8_t buf[4096];
    char cached[FM_PATH_MAX];
    FM_File f;
    uint32_t t0;
    uint32_t sd_ms = 0;
    uint32_t fl_ms = 0;

    puts("\n  --- مقایسهٔ سرعت خواندن ---");
    if (get_ms == NULL)
    {
        puts("  (تابع زمان داده نشده)");
        return;
    }

    /* از مبدأ */
    t0 = get_ms();
    memset(&f, 0, sizeof(f));
    if (File_OpenEx(&f, path, FM_OPEN_READ) == FM_OK)
    {
        uint32_t got = 0;

        while (File_Read(&f, buf, sizeof(buf), &got) == FM_OK && got > 0u)
        {
            /* فقط خواندن */
        }
        (void)File_Close(&f);
    }
    sd_ms = get_ms() - t0;

    /* از کش */
    if (FM_MediaCache_Resolve(path, cached, sizeof(cached)) == FM_OK &&
        strcmp(cached, path) != 0)
    {
        t0 = get_ms();
        memset(&f, 0, sizeof(f));
        if (File_OpenEx(&f, cached, FM_OPEN_READ) == FM_OK)
        {
            uint32_t got = 0;

            while (File_Read(&f, buf, sizeof(buf), &got) == FM_OK &&
                   got > 0u)
            {
                /* فقط خواندن */
            }
            (void)File_Close(&f);
        }
        fl_ms = get_ms() - t0;

        printf("  از مبدأ: %lu ms\n", (unsigned long)sd_ms);
        printf("  از فلش : %lu ms\n", (unsigned long)fl_ms);
        if (fl_ms != 0u && sd_ms > fl_ms)
        {
            printf("  %lu%% سریع‌تر\n",
                   (unsigned long)(((sd_ms - fl_ms) * 100u) / sd_ms));
        }
        else if (fl_ms >= sd_ms)
        {
            puts("  فلش سریع‌تر نبود — کش برای این حالت ارزش ندارد.");
        }
    }
    else
    {
        printf("  این فایل کش نشده (از مبدأ %lu ms)\n",
               (unsigned long)sd_ms);
    }
}

/* ------------------------------------------------------------------ */
/* FlashPlay_ManualCopy — کپی صریح بین حافظه‌ها                                      */
/* ------------------------------------------------------------------ */
/*
 * اگر نمی‌خواهید کش خودکار باشد و ترجیح می‌دهید خودتان کنترل کنید.
 * FM_Transfer تکه‌تکه کپی می‌کند و اندازهٔ تکه را با سکتور فلش هم‌تراز
 * می‌کند تا پاک‌کردن اضافی رخ ندهد.
 */
static void FlashPlay_ManualCopy(void)
{
    FM_TransferOpts o;
    FM_TransferInfo info;
    FM_Result r;

    puts("\n  --- کپی دستی SD -> QSPI ---");

    memset(&o, 0, sizeof(o));
    o.verify = FM_TRUE;

    r = FM_Transfer_CopyEx("/videos/clip.avi", "/qspi/clip.avi", &o,
                           &info);
    if (r == FM_OK)
    {
        printf("  %lu KB منتقل شد، CRC 0x%08lX تأیید شد\n",
               (unsigned long)(info.bytes / 1024u),
               (unsigned long)info.crc_dest);
    }
    else
    {
        printf("  کپی نشد: %s (%d)\n", File_GetErrorString(r), (int)r);
    }
}

#endif /* FM_TOPAPI_PLAY_FROM_FLASH_H */
