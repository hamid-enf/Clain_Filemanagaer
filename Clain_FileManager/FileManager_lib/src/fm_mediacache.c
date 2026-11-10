/*
 * fm_mediacache.c — کش رسانه روی فلش سریع
 *
 * قاعدهٔ اصلی این فایل: هیچ شکستی نباید پخش را غیرممکن کند. هر مسیری
 * که به خطا بخورد، مسیر اصلی برگردانده می‌شود و پخش (کندتر) ادامه
 * می‌یابد.
 */

#define FM_DEBUG_TAG "mcache"

#include <string.h>
#include <stdio.h>

#include "fm_mediacache.h"
#include "fm_transfer.h"
#include "fm_file.h"
#include "fm_directory.h"
#include "fm_path.h"
#include "fm_crc.h"
#include "fm_debug.h"

static FM_MediaCacheConfig g_cfg;
static FM_MediaCacheStats  g_stats;
static FM_Bool             g_ready;

/* ------------------------------------------------------------------ */
/* کمکی                                                                */
/* ------------------------------------------------------------------ */

/* آخرین جزء مسیر (نام فایل). */
static const char *mc_basename(const char *path)
{
    const char *p = path;
    const char *last = path;

    while (*p != '\0')
    {
        if (*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
        p++;
    }
    return last;
}

/*
 * مسیر کش‌شده را بساز.
 *
 * فقط نام فایل استفاده می‌شود، نه کل مسیر — چون فایل‌سیستم‌های ساده
 * پوشه‌های تودرتو را گران انجام می‌دهند. برای جلوگیری از برخورد دو
 * فایل هم‌نام از پوشه‌های مختلف، یک پیشوند کوتاه از CRC کل مسیر
 * اضافه می‌شود.
 */
/* The tag only has to tell two same-named files from different folders
 * apart; it is not an integrity check.  Use CRC-32 when the CRC module is
 * built, a 32-bit FNV-1a otherwise, so the media cache does not drag the
 * CRC module in (FM_USE_CRC=0 used to fail at link here). */
static uint32_t mc_path_tag(const char *src)
{
#if FM_USE_CRC
    return CRC_Compute32(src, strlen(src), 0u);
#else
    uint32_t h = 2166136261u;

    while (*src != '\0')
    {
        h ^= (uint8_t)*src++;
        h *= 16777619u;
    }
    return h;
#endif
}

static FM_Result mc_cache_path(const char *src, char *out, size_t cap)
{
    uint32_t tag = mc_path_tag(src);
    const char *base = mc_basename(src);
    int n;

    n = snprintf(out, cap, "%s/%08lX_%s", g_cfg.cache_root,
                 (unsigned long)tag, base);
    if (n < 0 || (size_t)n >= cap)
    {
        FM_ERR_LOG("cache path too long for %s", src);
        return FM_ERR_TOO_LARGE;
    }
    return FM_OK;
}

/* آیا نسخهٔ کش‌شده با مبدأ می‌خواند؟ اندازه معیار سریع و کافی است. */
static FM_Bool mc_is_valid(const char *src, const char *cached)
{
    uint64_t a = 0;
    uint64_t b = 0;

    if (File_SizeOf(src, &a) != FM_OK)
    {
        return FM_FALSE;
    }
    if (File_SizeOf(cached, &b) != FM_OK)
    {
        return FM_FALSE;
    }
    return (a == b && a != 0u) ? FM_TRUE : FM_FALSE;
}

/* مجموع حجم کش. */
static uint64_t mc_total_bytes(void)
{
    FM_Dir *d;
    FM_DirEntry e;
    uint64_t total = 0;

    d = Directory_Open(g_cfg.cache_root);
    if (d == NULL)
    {
        return 0;
    }
    while (Directory_Read(d, &e) == FM_OK)
    {
        if (e.type != FM_ITEM_DIR)
        {
            total += e.size;
        }
    }
    (void)Directory_Close(d);
    return total;
}

/*
 * برای باز کردن جا، یک ورودی حذف کن.
 *
 * سادگی عمدی: بزرگ‌ترین فایل حذف می‌شود، نه قدیمی‌ترین. فایل‌سیستم‌های
 * کوچک زمان دقیق نگه نمی‌دارند، و برای باز کردن جا بزرگ‌ترین انتخاب
 * مؤثرتری است.
 */
static FM_Result mc_evict_one(const char *keep)
{
    FM_Dir *d;
    FM_DirEntry e;
    char victim[FM_PATH_MAX];
    uint64_t biggest = 0;
    FM_Bool found = FM_FALSE;

    d = Directory_Open(g_cfg.cache_root);
    if (d == NULL)
    {
        return FM_ERR_NOT_FOUND;
    }
    while (Directory_Read(d, &e) == FM_OK)
    {
        if (e.type == FM_ITEM_DIR)
        {
            continue;
        }
        if (e.size >= biggest)
        {
            char full[FM_PATH_MAX];

            if (snprintf(full, sizeof(full), "%s/%s", g_cfg.cache_root,
                         e.name) < 0)
            {
                continue;
            }
            if (keep != NULL && strcmp(full, keep) == 0)
            {
                continue;          /* همانی که الان می‌سازیم */
            }
            biggest = e.size;
            memcpy(victim, full, sizeof(victim));
            found = FM_TRUE;
        }
    }
    (void)Directory_Close(d);

    if (!found)
    {
        return FM_ERR_NOT_FOUND;
    }
    FM_EVENT_LOG("evicting %s (%lu bytes)", victim,
                 (unsigned long)biggest);
    g_stats.evictions++;
    return File_Delete(victim);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

FM_Result FM_MediaCache_Init(const FM_MediaCacheConfig *cfg)
{
    FM_Result r;

    FM_CHECK_PTR(cfg, FM_ERR_INVALID_ARGUMENT);

    memset(&g_stats, 0, sizeof(g_stats));
    g_ready = FM_FALSE;
    g_cfg = *cfg;

    if (g_cfg.cache_root == NULL || g_cfg.policy == FM_MEDIACACHE_NEVER)
    {
        FM_EVENT_LOG("cache disabled");
        return FM_OK;                  /* خاموش، ولی خطا نیست */
    }

    /* پوشهٔ کش باید وجود داشته باشد. اگر ساختنش نشد، کش خاموش می‌ماند
     * و همه چیز bypass می‌شود — پخش همچنان کار می‌کند. */
    r = Directory_Create(g_cfg.cache_root);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        FM_ERR_LOG("cannot create %s (%d) -- caching off",
                   g_cfg.cache_root, (int)r);
        g_cfg.cache_root = NULL;
        return r;
    }

    /* Directory_Create succeeding is not proof that the directory is
     * usable: some backends report success for a path whose parent does
     * not exist.  Write and delete a probe file so an unusable cache is
     * discovered here, once, rather than on every Resolve. */
    {
        char probe[FM_PATH_MAX];
        static const uint8_t byte = 0x4Du;

        if (snprintf(probe, sizeof(probe), "%s/.probe",
                     g_cfg.cache_root) < 0)
        {
            g_cfg.cache_root = NULL;
            return FM_ERR_TOO_LARGE;
        }
        r = File_WriteAll(probe, &byte, 1u);
        if (r != FM_OK)
        {
            FM_ERR_LOG("%s is not writable (%d) -- caching off",
                       g_cfg.cache_root, (int)r);
            g_cfg.cache_root = NULL;
            return r;
        }
        (void)File_Delete(probe);
    }

    if (g_cfg.max_file_bytes == 0u)
    {
        g_cfg.max_file_bytes = 8u * 1024u * 1024u;   /* پیش‌فرض معقول */
    }
    g_ready = FM_TRUE;
    FM_EVENT_LOG("cache ready at %s (policy %d, max file %lu KB)",
                 g_cfg.cache_root, (int)g_cfg.policy,
                 (unsigned long)(g_cfg.max_file_bytes / 1024u));
    return FM_OK;
}

void FM_MediaCache_Deinit(void)
{
    g_ready = FM_FALSE;
    g_cfg.cache_root = NULL;
}

FM_Result FM_MediaCache_Resolve(const char *source_path, char *out_path,
                                size_t out_cap)
{
    char cached[FM_PATH_MAX];
    uint64_t size = 0;
    FM_TransferOpts opts;
    FM_Result r;

    FM_CHECK_PTR(source_path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(out_path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(out_cap > 1u, FM_ERR_INVALID_ARGUMENT, "output buffer too small");

    /* پیش‌فرض همیشه: مسیر اصلی. هر مسیر خطا به همین برمی‌گردد. */
    if (strlen(source_path) >= out_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    strcpy(out_path, source_path);

    if (!g_ready)
    {
        g_stats.bypasses++;
        return FM_OK;
    }

    /* آیا اصلاً ارزش کش دارد؟ */
    if (File_SizeOf(source_path, &size) != FM_OK)
    {
        g_stats.bypasses++;
        return FM_OK;                  /* فایل نیست — بگذار پخش‌کننده بگوید */
    }
    if (g_cfg.policy == FM_MEDIACACHE_IF_SMALL &&
        size > g_cfg.max_file_bytes)
    {
        FM_EVENT_LOG("%s is %lu KB -- too big to cache, playing in place",
                     source_path, (unsigned long)(size / 1024u));
        g_stats.bypasses++;
        return FM_OK;
    }

    if (mc_cache_path(source_path, cached, sizeof(cached)) != FM_OK)
    {
        g_stats.bypasses++;
        return FM_OK;
    }

    /* از قبل هست و سالم است؟ */
    if (mc_is_valid(source_path, cached))
    {
        if (strlen(cached) < out_cap)
        {
            strcpy(out_path, cached);
            g_stats.hits++;
            FM_TRACE_LOG("hit %s", cached);
            return FM_OK;
        }
        g_stats.bypasses++;
        return FM_OK;
    }

    /* باید کپی شود. اول مطمئن شو جا هست. */
    if (g_cfg.max_total_bytes != 0u)
    {
        uint32_t guard = 0;

        while (mc_total_bytes() + size > g_cfg.max_total_bytes &&
               guard < 64u)
        {
            if (mc_evict_one(cached) != FM_OK)
            {
                break;                 /* چیزی برای حذف نماند */
            }
            guard++;
        }
    }

    memset(&opts, 0, sizeof(opts));
    opts.verify = g_cfg.verify;
    r = FM_Transfer_Copy(source_path, cached, &opts);
    if (r != FM_OK)
    {
        /* فضا نبود یا فلش خطا داد: کپی ناقص را پاک کن و از مبدأ پخش
         * کن. این حالت باید بی‌صدا و بی‌ضرر باشد. */
        FM_EVENT_LOG("cache copy of %s failed (%d) -- playing from source",
                     source_path, (int)r);
        (void)File_Delete(cached);
        g_stats.bypasses++;
        return FM_OK;
    }

    g_stats.misses++;
    g_stats.bytes_copied += size;
    if (strlen(cached) < out_cap)
    {
        strcpy(out_path, cached);
        FM_EVENT_LOG("cached %s (%lu KB)", source_path,
                     (unsigned long)(size / 1024u));
    }
    return FM_OK;
}

FM_Result FM_MediaCache_Evict(const char *source_path)
{
    char cached[FM_PATH_MAX];

    FM_CHECK_PTR(source_path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(g_ready, FM_ERR_NOT_READY, "cache not initialised");

    if (mc_cache_path(source_path, cached, sizeof(cached)) != FM_OK)
    {
        return FM_ERR_TOO_LARGE;
    }
    return File_Delete(cached);
}

FM_Result FM_MediaCache_Clear(void)
{
    FM_Dir *d;
    FM_DirEntry e;

    FM_CHECK(g_ready, FM_ERR_NOT_READY, "cache not initialised");

    d = Directory_Open(g_cfg.cache_root);
    if (d == NULL)
    {
        return FM_ERR_NOT_FOUND;
    }
    /* نام‌ها را یکی‌یکی جمع می‌کنیم چون حذف حین پیمایش امن نیست. */
    for (;;)
    {
        char full[FM_PATH_MAX];
        FM_Bool got = FM_FALSE;

        while (Directory_Read(d, &e) == FM_OK)
        {
            if (e.type != FM_ITEM_DIR)
            {
                if (snprintf(full, sizeof(full), "%s/%s",
                             g_cfg.cache_root, e.name) > 0)
                {
                    got = FM_TRUE;
                }
                break;
            }
        }
        (void)Directory_Close(d);
        if (!got)
        {
            break;
        }
        (void)File_Delete(full);
        d = Directory_Open(g_cfg.cache_root);
        if (d == NULL)
        {
            break;
        }
    }
    FM_EVENT_LOG("cache cleared");
    return FM_OK;
}

void FM_MediaCache_GetStats(FM_MediaCacheStats *out)
{
    if (out != NULL)
    {
        *out = g_stats;
    }
}

void FM_MediaCache_ResetStats(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}
