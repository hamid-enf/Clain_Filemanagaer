/*
 * eeprom_simple.h — کار با EEPROM، با انتخاب مدل
 * ==================================================
 *
 * EEPROM جای درست برای تنظیمات، کالیبراسیون، شمارنده و شماره سریال است:
 * کوچک، ماندگار، و بدون نیاز به فایل‌سیستم.
 *
 * ساده‌ترین شکل:
 *
 *     EEPROM_examples_begin(FM_EEPROM_CHIP_24LC256, my_read, my_write,
 *                           &hi2c1);
 *     exE_1_basic();
 *     exE_2_settings_struct();
 *     exE_3_wear_aware_counter();
 *     exE_4_as_filesystem();
 *
 * شما فقط مدل را می‌گویید؛ ظرفیت، اندازهٔ صفحه و زمان نوشتن خودکار
 * تنظیم می‌شود.
 *
 * ---------------------------------------------------------------------
 * دو چیزی که در EEPROM خراب می‌کند
 * ---------------------------------------------------------------------
 * ۱) مرز صفحه: نوشتن نمی‌تواند از مرز صفحه رد شود (۸ تا ۱۲۸ بایت بسته
 *    به مدل). اگر رد شود داده به اول همان صفحه می‌پیچد. لایهٔ EEPROM
 *    کتابخانه خودش تکه‌تکه می‌کند، ولی اگر مستقیم I2C می‌زنید حواستان
 *    باشد.
 *
 * ۲) عمر: حدود یک میلیون بار نوشتن روی هر سلول. اگر هر ثانیه بنویسید،
 *    یازده روزه تمام می‌شود. سه راهکار در exE_3 نشان داده شده.
 */
#ifndef FM_EXAMPLE_28_EEPROM_SIMPLE_H
#define FM_EXAMPLE_28_EEPROM_SIMPLE_H

#include <stdio.h>
#include <string.h>

#include "file_manager.h"
#include "fm_eeprom.h"
#include "fm_eeprom_chips.h"
#include "fm_board_config.h"

#define printf FM_Log
#define puts(s) FM_Log("%s\n", (s))

static FM_EEPROM g_ee;
static FM_EepromChipInfo g_ee_info;
static FM_Bool g_ee_ready;

static void e_step(const char *what, FM_Result r)
{
    if (r != FM_OK)
    {
        printf("  %s -> %s (%d)\n", what, File_GetErrorString(r), (int)r);
    }
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */
/*
 * read و write دو تابع کوچک شما هستند که با I2C/SPI کار می‌کنند.
 * نمونهٔ I2C در انتهای همین فایل هست.
 */
static FM_Result EEPROM_examples_begin(FM_EepromChip chip,
                                       FM_EEPROM_ReadFn read,
                                       FM_EEPROM_WriteFn write,
                                       void *context)
{
    FM_EEPROMConfig cfg;
    FM_Result r;

    puts("\n*************** EEPROM_examples_begin ***************");

    r = FM_Eeprom_GetChipInfo(chip, &g_ee_info);
    if (r != FM_OK)
    {
        puts("  مدل ناشناخته است.");
        g_ee_ready = FM_FALSE;
        return r;
    }

    r = FM_Eeprom_ConfigFromChip(&cfg, chip);
    if (r != FM_OK)
    {
        g_ee_ready = FM_FALSE;
        return r;
    }
    cfg.read = read;
    cfg.write = write;
    cfg.context = context;

    r = EEPROM_Init(&g_ee, &cfg);
    e_step("EEPROM_Init", r);
    g_ee_ready = (r == FM_OK) ? FM_TRUE : FM_FALSE;

    if (g_ee_ready)
    {
        printf("  %s: %lu بایت، صفحهٔ %lu بایتی، آدرس %u بایتی\n",
               g_ee_info.name, (unsigned long)g_ee_info.capacity,
               (unsigned long)g_ee_info.page_size,
               (unsigned)g_ee_info.addr_bytes);
    }
    return r;
}

static void EEPROM_examples_end(void)
{
    if (g_ee_ready)
    {
        (void)EEPROM_Sync(&g_ee);
        g_ee_ready = FM_FALSE;
    }
    puts("*************** EEPROM_examples_end ***************\n");
}

#define E_REQUIRE()                                                        \
    do {                                                                   \
        if (!g_ee_ready) { puts("  (EEPROM آماده نیست — رد شد)"); return; }\
    } while (0)

/* ------------------------------------------------------------------ */
/* exE_1 — خواندن و نوشتن ساده                                         */
/* ------------------------------------------------------------------ */
static void exE_1_basic(void)
{
    char text[32];

    puts("\n=================== exE_1_basic ==================");
    E_REQUIRE();

    e_step("write", EEPROM_Write(&g_ee, 0u, "board-01", 9u));

    memset(text, 0, sizeof(text));
    e_step("read", EEPROM_Read(&g_ee, 0u, text, 9u));
    printf("  خوانده شد: %s\n", text);

    /* نوشتن روی مرز صفحه — لایه خودش تکه می‌کند */
    {
        static uint8_t big[200];
        static uint8_t back[200];
        size_t i;
        FM_Bool same = FM_TRUE;

        for (i = 0; i < sizeof(big); i++)
        {
            big[i] = (uint8_t)(i + 1u);
        }
        /* آدرس ۵۰ عمداً وسط صفحه است تا مرز رد شود */
        e_step("cross-page write", EEPROM_Write(&g_ee, 50u, big,
                                                sizeof(big)));
        e_step("cross-page read", EEPROM_Read(&g_ee, 50u, back,
                                              sizeof(back)));
        for (i = 0; i < sizeof(big); i++)
        {
            if (back[i] != big[i]) { same = FM_FALSE; break; }
        }
        printf("  نوشتن ۲۰۰ بایتی از مرز صفحه: %s\n",
               same ? "درست" : "خراب");
    }
}

/* ------------------------------------------------------------------ */
/* exE_2 — ذخیرهٔ یک ساختار تنظیمات، با CRC                            */
/* ------------------------------------------------------------------ */
/*
 * الگوی درست: ساختار + شمارهٔ نسخه + CRC. اگر برق وسط نوشتن برود یا
 * نسخهٔ نرم‌افزار عوض شود، تشخیص می‌دهید و مقدار پیش‌فرض می‌گذارید.
 */
typedef struct
{
    uint16_t version;
    uint16_t brightness;
    uint32_t baudrate;
    uint8_t  language;
    uint8_t  reserved[3];
    uint32_t crc;            /* همیشه آخرین فیلد */
} ExE_Settings;

#define EXE_SETTINGS_ADDR    256u
#define EXE_SETTINGS_VERSION 2u

static uint32_t exE_settings_crc(const ExE_Settings *s)
{
    /* CRC روی همه چیز جز خود فیلد crc */
    return CRC_Compute32(s, sizeof(*s) - sizeof(uint32_t), 0u);
}

static void exE_2_settings_struct(void)
{
    ExE_Settings s;
    ExE_Settings back;

    puts("\n============== exE_2_settings_struct =============");
    E_REQUIRE();

    /* این دمو آدرس 256 را لازم دارد — روی چیپ‌های کوچک (مثل 24LC02
     * با ۲۵۶ بایت) جا نیست؛ به‌جای خطای مبهم، تمیز رد شو. */
    if (g_ee_info.capacity <
        EXE_SETTINGS_ADDR + sizeof(ExE_Settings))
    {
        printf("  ظرفیت چیپ (%lu بایت) برای این دمو کم است - رد شد\n",
               (unsigned long)g_ee_info.capacity);
        return;
    }

    memset(&s, 0, sizeof(s));
    s.version = EXE_SETTINGS_VERSION;
    s.brightness = 80u;
    s.baudrate = 500000u;
    s.language = 1u;
    s.crc = exE_settings_crc(&s);

    e_step("save", EEPROM_Write(&g_ee, EXE_SETTINGS_ADDR, &s, sizeof(s)));

    memset(&back, 0, sizeof(back));
    e_step("load", EEPROM_Read(&g_ee, EXE_SETTINGS_ADDR, &back,
                               sizeof(back)));

    if (back.crc != exE_settings_crc(&back))
    {
        puts("  CRC غلط — تنظیمات خراب است، پیش‌فرض بگذارید.");
    }
    else if (back.version != EXE_SETTINGS_VERSION)
    {
        printf("  نسخهٔ %u ذخیره شده ولی نرم‌افزار %u می‌خواهد — "
               "مهاجرت لازم است\n",
               (unsigned)back.version, (unsigned)EXE_SETTINGS_VERSION);
    }
    else
    {
        printf("  brightness=%u baud=%lu lang=%u  (معتبر)\n",
               (unsigned)back.brightness, (unsigned long)back.baudrate,
               (unsigned)back.language);
    }
}

/* ------------------------------------------------------------------ */
/* exE_3 — شمارنده با ملاحظهٔ عمر                                      */
/* ------------------------------------------------------------------ */
/*
 * سه ترفند برای اینکه EEPROM زود نسوزد:
 *
 *  ۱) EEPROM_Update به‌جای EEPROM_Write — اول می‌خواند، اگر همان بود
 *     اصلاً نمی‌نویسد. برای مقادیری که کم عوض می‌شوند عالی است.
 *
 *  ۲) چرخاندن جا (wear leveling سبک): شمارنده را در N جای مختلف
 *     بچرخانید. عمر مفید N برابر می‌شود.
 *
 *  ۳) در RAM نگه دارید و فقط موقع خاموش‌شدن یا هر چند دقیقه بنویسید.
 */
#define EXE_SLOTS      16u
#define EXE_SLOT_BASE  1024u

static void exE_3_wear_aware_counter(void)
{
    uint32_t boot_count = 0;

    puts("\n============ exE_3_wear_aware_counter ============");
    E_REQUIRE();

    /* 16 اسلات ۴بایتی از آدرس 1024 — روی چیپ‌های کوچک جا نیست. */
    if (g_ee_info.capacity <
        EXE_SLOT_BASE + (EXE_SLOTS * 4u))
    {
        printf("  ظرفیت چیپ (%lu بایت) برای این دمو کم است - رد شد\n",
               (unsigned long)g_ee_info.capacity);
        return;
    }

    /* ۱) Update: فقط وقتی واقعاً فرق دارد می‌نویسد */
    {
        uint8_t v = 42u;

        e_step("update (first)", EEPROM_Update(&g_ee, 512u, &v, 1u));
        /* بار دوم همان مقدار — نباید چرخهٔ نوشتنی مصرف شود */
        e_step("update (same)", EEPROM_Update(&g_ee, 512u, &v, 1u));
        puts("  EEPROM_Update: نوشتن دوم چرخه‌ای مصرف نکرد.");
    }

    /* ۲) چرخاندن جا برای شمارندهٔ بوت */
    {
        uint32_t best = 0;
        uint32_t best_slot = 0;
        uint32_t i;

        /* بزرگ‌ترین مقدار = تازه‌ترین */
        for (i = 0; i < EXE_SLOTS; i++)
        {
            uint32_t v = 0;

            if (EEPROM_Read(&g_ee, EXE_SLOT_BASE + (i * 4u), &v,
                            4u) == FM_OK)
            {
                if (v != 0xFFFFFFFFu && v >= best)
                {
                    best = v;
                    best_slot = i;
                }
            }
        }
        boot_count = best + 1u;
        /* در خانهٔ بعدی بنویس */
        {
            uint32_t next_slot = (best_slot + 1u) % EXE_SLOTS;

            e_step("rotate", EEPROM_Write(&g_ee,
                                          EXE_SLOT_BASE + (next_slot * 4u),
                                          &boot_count, 4u));
            printf("  بوت شمارهٔ %lu، در خانهٔ %lu ذخیره شد "
                   "(عمر %lu برابر شد)\n",
                   (unsigned long)boot_count, (unsigned long)next_slot,
                   (unsigned long)EXE_SLOTS);
        }
    }
}

/* ------------------------------------------------------------------ */
/* exE_4 — EEPROM به‌عنوان یک حافظهٔ عادی کتابخانه                     */
/* ------------------------------------------------------------------ */
/*
 * می‌شود EEPROM را به لایهٔ Storage وصل کرد و بعد با همان API فایل
 * باش کار کرد — یا حتی با FM_Transfer داده را بین آن و SD جابه‌جا کرد.
 *
 * برای قطعات خیلی کوچک (کمتر از ۴KB) فایل‌سیستم صرف نمی‌کند؛ آدرس
 * مستقیم بهتر است. برای ۶۴KB به بالا منطقی است.
 */
static void exE_4_as_storage(void)
{
    static FM_Storage ee_storage;
    FM_Result r;

    puts("\n=============== exE_4_as_storage ================");
    E_REQUIRE();

    r = EEPROM_StorageInit(&g_ee, &ee_storage, FM_STORAGE_CUSTOM);
    e_step("EEPROM_StorageInit", r);
    if (r != FM_OK)
    {
        return;
    }
    r = Storage_Register(&ee_storage);
    e_step("Storage_Register", r);
    if (r != FM_OK)
    {
        return;
    }

    puts("  حالا 'eeprom' یک حافظهٔ عادی است:");
    puts("    Storage_Mount(\"eeprom\")");
    puts("    FM_Transfer_Copy(\"/sd/cal.bin\", \"/ee/cal.bin\", NULL)");

    {
        uint64_t cap = 0;

        if (Storage_GetCapacity("eeprom", &cap) == FM_OK)
        {
            printf("  ظرفیت ثبت‌شده: %lu بایت\n", (unsigned long)cap);
        }
    }
    (void)Storage_Unregister("eeprom");
}

/*
 * ---------------------------------------------------------------------
 * نمونهٔ توابع I2C برای STM32
 * ---------------------------------------------------------------------
 * این دو تابع را در main.c خودتان بگذارید:
 *
 *   static FM_Result my_ee_read(void *ctx, uint32_t addr, void *buf,
 *                               uint32_t len)
 *   {
 *       I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)ctx;
 *
 *       // 0xA0 = آدرس پایه شیفت‌شده. برای قطعات ≤16Kbit بیت‌های بالای
 *       // آدرس داخل آدرس دستگاه می‌رود؛ برای بزرگ‌ترها آدرس ۲ بایتی است.
 *       if (HAL_I2C_Mem_Read(hi2c, 0xA0, (uint16_t)addr,
 *                            I2C_MEMADD_SIZE_16BIT, (uint8_t *)buf,
 *                            (uint16_t)len, 1000) != HAL_OK)
 *       {
 *           return FM_ERR_IO;
 *       }
 *       return FM_OK;
 *   }
 *
 *   static FM_Result my_ee_write(void *ctx, uint32_t addr,
 *                                const void *buf, uint32_t len)
 *   {
 *       I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)ctx;
 *
 *       if (HAL_I2C_Mem_Write(hi2c, 0xA0, (uint16_t)addr,
 *                             I2C_MEMADD_SIZE_16BIT, (uint8_t *)buf,
 *                             (uint16_t)len, 1000) != HAL_OK)
 *       {
 *           return FM_ERR_IO;
 *       }
 *       // حیاتی: بعد از هر نوشتن قطعه مشغول است و به هیچ دستوری جواب
 *       // نمی‌دهد. یا این تأخیر، یا ACK polling.
 *       HAL_Delay(5);
 *       return FM_OK;
 *   }
 *
 * و بعد:
 *
 *   EEPROM_examples_begin(FM_EEPROM_CHIP_24LC256, my_ee_read,
 *                         my_ee_write, &hi2c1);
 * ---------------------------------------------------------------------
 */

#endif /* FM_EXAMPLE_28_EEPROM_SIMPLE_H */
