/*
 * fm_qspi.c — منطق فلش NOR سریال، مستقل از قطعه و از HAL
 *
 * قاعدهٔ این فایل: هیچ خطایی نباید باعث قفل‌شدن یا کرش شود. هر انتظار
 * timeout دارد، هر اشاره‌گر بررسی می‌شود، هر آدرس با ظرفیت قطعه چک
 * می‌شود.
 */

#define FM_DEBUG_TAG "qspi"

#include <string.h>

#include "fm_qspi.h"
#include "fm_debug.h"

/* ------------------------------------------------------------------ */
/* دستورات استاندارد SPI NOR                                           */
/* ------------------------------------------------------------------ */
#define QCMD_WRITE_ENABLE      0x06u
#define QCMD_READ_STATUS1      0x05u
#define QCMD_READ_DATA         0x03u
#define QCMD_FAST_READ         0x0Bu
#define QCMD_PAGE_PROGRAM      0x02u
#define QCMD_SECTOR_ERASE_4K   0x20u
#define QCMD_CHIP_ERASE        0xC7u
#define QCMD_READ_JEDEC_ID     0x9Fu
#define QCMD_ENTER_4B_ADDR     0xB7u
#define QCMD_RESET_ENABLE      0x66u
#define QCMD_RESET             0x99u
#define QCMD_ERASE_RESUME      0x7Au

#define QSTATUS_BUSY           0x01u
#define QSTATUS_WEL            0x02u

/* ------------------------------------------------------------------ */
/* جدول قطعات                                                          */
/* ------------------------------------------------------------------ */
/*
 * زمان‌ها از بدترین حالت دیتاشیت گرفته شده‌اند، نه حالت معمول: اگر
 * خوش‌بینانه بگیریم، یک قطعهٔ کند در دمای بالا timeout می‌خورد و کاربر
 * فکر می‌کند فلش خراب است.
 */
#define MB(n) ((uint64_t)(n) * 1024u * 1024u)

static const FM_QspiChipInfo g_chips[] = {
    /* name          jedec     size    sect  page  block  addr  esec echip  epage */
    { "W25Q16",   0xEF4015u, MB(2),  4096u, 256u, 65536u, 3u,  400u,  25000u, 3u },
    { "W25Q32",   0xEF4016u, MB(4),  4096u, 256u, 65536u, 3u,  400u,  50000u, 3u },
    { "W25Q64",   0xEF4017u, MB(8),  4096u, 256u, 65536u, 3u,  400u, 100000u, 3u },
    { "W25Q128",  0xEF4018u, MB(16), 4096u, 256u, 65536u, 3u,  400u, 200000u, 3u },
    { "W25Q256",  0xEF4019u, MB(32), 4096u, 256u, 65536u, 4u,  400u, 400000u, 3u },
    { "W25Q512",  0xEF4020u, MB(64), 4096u, 256u, 65536u, 4u,  400u, 800000u, 3u },
    { "N25Q128",  0x20BA18u, MB(16), 4096u, 256u, 65536u, 3u,  800u, 250000u, 5u },
    { "N25Q256",  0x20BA19u, MB(32), 4096u, 256u, 65536u, 4u,  800u, 480000u, 5u },
    { "MX25L128", 0xC22018u, MB(16), 4096u, 256u, 65536u, 3u,  400u, 150000u, 3u },
    { "MX25L256", 0xC22019u, MB(32), 4096u, 256u, 65536u, 4u,  400u, 300000u, 3u },
    { "IS25LP128",0x9D6018u, MB(16), 4096u, 256u, 65536u, 3u,  300u, 170000u, 3u },
    { "AT25SF128",0x1F8901u, MB(16), 4096u, 256u, 65536u, 3u,  400u, 200000u, 3u }
};

#define CHIP_COUNT (sizeof(g_chips) / sizeof(g_chips[0]))

/* نگاشت enum به سطر جدول. */
static int qspi_chip_index(FM_QspiChip chip)
{
    switch (chip)
    {
    case FM_QSPI_CHIP_W25Q16:    return 0;
    case FM_QSPI_CHIP_W25Q32:    return 1;
    case FM_QSPI_CHIP_W25Q64:    return 2;
    case FM_QSPI_CHIP_W25Q128:   return 3;
    case FM_QSPI_CHIP_W25Q256:   return 4;
    case FM_QSPI_CHIP_W25Q512:   return 5;
    case FM_QSPI_CHIP_N25Q128:   return 6;
    case FM_QSPI_CHIP_N25Q256:   return 7;
    case FM_QSPI_CHIP_MX25L128:  return 8;
    case FM_QSPI_CHIP_MX25L256:  return 9;
    case FM_QSPI_CHIP_IS25LP128: return 10;
    case FM_QSPI_CHIP_AT25SF128: return 11;
    default:                     return -1;
    }
}

FM_Result FM_Qspi_GetChipInfo(FM_QspiChip chip, FM_QspiChipInfo *out)
{
    int idx;

    FM_CHECK_PTR(out, FM_ERR_INVALID_ARGUMENT);
    idx = qspi_chip_index(chip);
    if (idx < 0)
    {
        FM_ERR_LOG("unknown chip id %d", (int)chip);
        return FM_ERR_UNSUPPORTED;
    }
    *out = g_chips[idx];
    return FM_OK;
}

FM_Result FM_Qspi_IdentifyById(uint32_t jedec_id, FM_QspiChipInfo *out)
{
    size_t i;

    FM_CHECK_PTR(out, FM_ERR_INVALID_ARGUMENT);
    for (i = 0; i < CHIP_COUNT; i++)
    {
        if (g_chips[i].jedec_id == jedec_id)
        {
            *out = g_chips[i];
            FM_EVENT_LOG("identified %s (id 0x%06lX)", out->name,
                         (unsigned long)jedec_id);
            return FM_OK;
        }
    }
    FM_ERR_LOG("unknown JEDEC id 0x%06lX", (unsigned long)jedec_id);
    return FM_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* کمکی‌های سطح پایین                                                  */
/* ------------------------------------------------------------------ */

/* اعلام‌های جلوتر: نوشتن/پاک‌کردن باید اول از حالت نگاشت خارج شوند. */
static FM_Result qspi_exit_mmap(FM_QspiDevice *dev);

static FM_Result qspi_write_enable(FM_QspiDevice *dev)
{
    /* در حالت نگاشت هیچ دستوری به قطعه نمی‌رسد -- حتی Write Enable.
     * اگر اینجا خارج نشویم، نوشتن بعدی بی‌صدا گم می‌شود. */
    FM_Result r = qspi_exit_mmap(dev);

    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.cmd(dev->io.ctx, QCMD_WRITE_ENABLE);
    if (r != FM_OK)
    {
        return r;
    }
    /* WEL باید یک شود؛ اگر صفر ماند، WREN روی باس گم شده و erase/program
     * بعدی بی‌صدا نادیده گرفته می‌شود. فقط لاگ می‌کنیم (رفتار عوض نمی‌کند)
     * تا اگر روزی روی برد افتاد، ریشه‌اش در لاگ دیده شود. */
    {
        uint8_t st = 0;

        if (FM_Qspi_ReadStatus(dev, &st) == FM_OK &&
            (st & QSTATUS_WEL) == 0u)
        {
            FM_ERR_LOG("write enable lost (SR=0x%02X): WEL did not set",
                       (unsigned)st);
        }
    }
    return FM_OK;
}

static void qspi_delay(FM_QspiDevice *dev, uint32_t ms)
{
    if (dev->io.delay_ms != NULL)
    {
        dev->io.delay_ms(dev->io.ctx, ms);
    }
}

/* Chips above 16 MB are driven with 4 address bytes, and that is a mode
 * of the chip (command 0xB7), not of the command: it is volatile.  A
 * power-up and a software reset (0x66 / 0x99) both put the chip back to
 * 3 address bytes.  So the mode is entered at start-up AND after every
 * reset this driver sends.  Without the second, the chip took the first
 * three of the four address bytes of every later command as its
 * address: reads returned other data, programs and erases landed 256
 * times lower than asked -- on a volume that had been fine until one
 * erase timed out. */
static void qspi_enter_4byte(FM_QspiDevice *dev)
{
    FM_Result r;

    if (dev->info.addr_bytes != 4u)
    {
        return;
    }
    (void)qspi_write_enable(dev);
    r = dev->io.cmd(dev->io.ctx, QCMD_ENTER_4B_ADDR);
    if (r != FM_OK)
    {
        /* some chips are in this mode from the factory and refuse the
         * command: not fatal */
        FM_EVENT_LOG("4-byte address mode command refused (%d), "
                     "continuing", (int)r);
    }
}

FM_Result FM_Qspi_WaitReady(FM_QspiDevice *dev, uint32_t timeout_ms)
{
    uint32_t waited = 0;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(dev->io.read, FM_ERR_INVALID_ARGUMENT);

    /* خواندن رجیستر وضعیت هم یک دستور است و در حالت نگاشت کار نمی‌کند. */
    {
        FM_Result rx = qspi_exit_mmap(dev);

        if (rx != FM_OK)
        {
            return rx;
        }
    }

    for (;;)
    {
        uint8_t status = 0;
        FM_Result r = dev->io.read(dev->io.ctx, QCMD_READ_STATUS1, 0u, 0u,
                                   0u, &status, 1u);

        if (r != FM_OK)
        {
            FM_ERR_LOG("status read failed: %d", (int)r);
            return r;
        }
        if ((status & QSTATUS_BUSY) == 0u)
        {
            return FM_OK;
        }
        if (waited >= timeout_ms)
        {
            /* قطعه جواب نمی‌دهد. خطا می‌دهیم، نه قفل بی‌پایان. */
            FM_ERR_LOG("timeout after %lums (status 0x%02X)",
                       (unsigned long)waited, status);
            /* ریکاوری: ریست نرم هر op گیرکرده (مثلاً پاک‌سازی‌ای که
             * هیچ‌وقت تمام نمی‌شود) را سقط می‌کند و قطعه را برای opهای
             * بعدی سالم برمی‌گرداند — بدون این، یک op بد همهٔ ادامه را
             * هم با خطا می‌کشد. (لاگ برد: گیرِ گاه‌به‌گاه erase، بعدش
             * همان قطعه سالم جواب می‌داد.) */
            FM_EVENT_LOG("wait timeout - issuing flash soft reset");
            (void)dev->io.cmd(dev->io.ctx, QCMD_RESET_ENABLE);
            (void)dev->io.cmd(dev->io.ctx, QCMD_RESET);
            qspi_delay(dev, 1u);
            /* the reset dropped the 4-byte address mode */
            qspi_enter_4byte(dev);
            return FM_ERR_TIMEOUT;
        }
        qspi_delay(dev, 1u);
        waited++;
    }
}

/* تایم‌اوت‌های دیتاشیت «حد» هستند نه «معمول» — به‌خصوص روی SPI معمولی
 * که هر پول RDSR خودش چند میکروثانیه طول می‌کشد. ۳ برابر + کف ۱ ثانیه،
 * بدون هزینهٔ واقعی (حلقهٔ انتظار به‌محض آماده‌شدن بیرون می‌آید). */
static uint32_t qspi_erase_timeout(const FM_QspiDevice *dev)
{
    uint32_t t = dev->info.erase_sector_ms * 10u;

    /* لاگ برد (دور ۱۲): روی سیم‌بندی سیمی هر «sector erase» بیش از ۴
     * ثانیه BUSY می‌ماند؛ تایم‌اوت ۴ ثانیه‌ای می‌خورد و ریست نرمِ داخل
     * WaitReady خودِ عملیات را می‌کشت. کف انتظار را ۲۰ ثانیه گذاشتیم تا
     * هم erase کند فرصت تمام‌شدن داشته باشد، هم یک chip-erase ناخواسته
     * (۱۶ ثانیه‌ای) وسط راه سقط نشود. حلقهٔ انتظار به‌محض آماده‌شدن
     * بیرون می‌آید، پس هزینهٔ واقعی صفر است. */
    if (t < 20000u) t = 20000u;
    return t;
}

FM_Result FM_Qspi_ReadId(FM_QspiDevice *dev, uint32_t *out_id)
{
    uint8_t id[3] = { 0, 0, 0 };
    FM_Result r;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(out_id, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(dev->io.read, FM_ERR_INVALID_ARGUMENT);

    r = qspi_exit_mmap(dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.read(dev->io.ctx, QCMD_READ_JEDEC_ID, 0u, 0u, 0u, id, 3u);
    if (r != FM_OK)
    {
        return r;
    }
    *out_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) |
              (uint32_t)id[2];
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* راه‌اندازی                                                          */
/* ------------------------------------------------------------------ */

FM_Result FM_Qspi_Init(FM_QspiDevice *dev, const FM_QspiIo *io,
                       FM_QspiChip chip, uint8_t *sector_buf,
                       uint32_t sector_buf_size)
{
    FM_Result r;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(io, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(io->cmd != NULL && io->read != NULL && io->write != NULL &&
                 io->cmd_addr != NULL,
             FM_ERR_INVALID_ARGUMENT, "io callbacks incomplete");

    memset(dev, 0, sizeof(*dev));
    dev->io = *io;
    /* اگر پورت نگاشت را پشتیبانی کند، پیش‌فرض روشن است چون خواندن را
     * سریع‌تر می‌کند و گذارها خودکار مدیریت می‌شوند. */
    dev->mmap_allowed = FM_TRUE;

    if (chip == FM_QSPI_CHIP_AUTO)
    {
        uint32_t id = 0;

        r = FM_Qspi_ReadId(dev, &id);
        if (r != FM_OK)
        {
            FM_ERR_LOG("cannot read JEDEC id: %d", (int)r);
            return r;
        }
        r = FM_Qspi_IdentifyById(id, &dev->info);
        if (r != FM_OK)
        {
            return r;
        }
    }
    else if (chip == FM_QSPI_CHIP_CUSTOM)
    {
        /* کاربر باید dev->info را از قبل پر کرده باشد. */
        FM_CHECK(dev->info.size_bytes != 0u && dev->info.sector_size != 0u,
                 FM_ERR_INVALID_ARGUMENT,
                 "CUSTOM chip needs info filled in first");
    }
    else
    {
        r = FM_Qspi_GetChipInfo(chip, &dev->info);
        if (r != FM_OK)
        {
            return r;
        }
    }

    /* بافر سکتور برای نوشتن‌های جزئی. */
    if (sector_buf != NULL)
    {
        FM_CHECK(sector_buf_size >= dev->info.sector_size,
                 FM_ERR_INVALID_ARGUMENT,
                 "sector buffer %lu < sector size %lu",
                 (unsigned long)sector_buf_size,
                 (unsigned long)dev->info.sector_size);
        dev->sector_buf = sector_buf;
        dev->sector_buf_size = sector_buf_size;
    }

    /* قطعات بزرگ‌تر از ۱۶MB آدرس ۴ بایتی می‌خواهند. */
    qspi_enter_4byte(dev);

    dev->ready = FM_TRUE;
    FM_EVENT_LOG("%s ready: %lu MB, %lu B sectors, %u-byte addressing",
                 dev->info.name,
                 (unsigned long)(dev->info.size_bytes / (1024u * 1024u)),
                 (unsigned long)dev->info.sector_size,
                 (unsigned)dev->info.addr_bytes);
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* خواندن                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* حالت memory-mapped                                                  */
/* ------------------------------------------------------------------ */
/*
 * قاعده: خواندن ترجیح می‌دهد در حالت نگاشت باشد (سریع‌تر)، نوشتن و
 * پاک‌کردن حتماً باید بیرون از آن باشند (وگرنه دستور اصلاً نمی‌رسد).
 * این دو تابع گذار را انجام می‌دهند و اگر از قبل در حالت درست باشیم
 * هیچ کاری نمی‌کنند، پس صدا زدنشان ارزان است.
 */

static FM_Bool qspi_can_mmap(const FM_QspiDevice *dev)
{
    return (dev->mmap_allowed && dev->io.mmap_enter != NULL &&
            dev->io.mmap_exit != NULL && dev->io.mmap_base != NULL)
               ? FM_TRUE : FM_FALSE;
}

/* وارد حالت نگاشت شو. اگر نشد، کشنده نیست — خواندن معمولی می‌کنیم. */
static FM_Result qspi_enter_mmap(FM_QspiDevice *dev)
{
    FM_Result r;

    if (dev->mmap_active)
    {
        return FM_OK;
    }
    if (!qspi_can_mmap(dev))
    {
        return FM_ERR_UNSUPPORTED;
    }
    r = dev->io.mmap_enter(dev->io.ctx);
    if (r != FM_OK)
    {
        FM_ERR_LOG("mmap enter failed: %d", (int)r);
        return r;
    }
    dev->mmap_active = FM_TRUE;
    dev->stat_mmap_switch++;
    FM_TRACE_LOG("mmap on");
    return FM_OK;
}

/* از حالت نگاشت خارج شو. قبل از هر نوشتن/پاک‌کردن حتماً لازم است. */
static FM_Result qspi_exit_mmap(FM_QspiDevice *dev)
{
    FM_Result r;

    if (!dev->mmap_active)
    {
        return FM_OK;
    }
    r = dev->io.mmap_exit(dev->io.ctx);
    if (r != FM_OK)
    {
        /* این یکی جدی است: اگر خارج نشویم، نوشتن بی‌صدا شکست می‌خورد. */
        FM_ERR_LOG("mmap exit failed: %d -- writes would be lost", (int)r);
        return r;
    }
    dev->mmap_active = FM_FALSE;
    dev->stat_mmap_switch++;
    FM_TRACE_LOG("mmap off");
    return FM_OK;
}

FM_Result FM_Qspi_MapForRead(FM_QspiDevice *dev, uint64_t addr,
                             const uint8_t **out_ptr)
{
    FM_Result r;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(out_ptr, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");
    FM_CHECK(addr < dev->info.size_bytes, FM_ERR_INVALID_ARGUMENT,
             "map address past end of chip");
    FM_CHECK(qspi_can_mmap(dev), FM_ERR_UNSUPPORTED,
             "memory-mapped reads not available on this port");

    r = qspi_enter_mmap(dev);
    if (r != FM_OK)
    {
        return r;
    }
    *out_ptr = (const uint8_t *)(dev->io.mmap_base + addr);
    return FM_OK;
}

FM_Result FM_Qspi_Unmap(FM_QspiDevice *dev)
{
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    return qspi_exit_mmap(dev);
}

FM_Bool FM_Qspi_IsMapped(const FM_QspiDevice *dev)
{
    return (dev != NULL && dev->mmap_active) ? FM_TRUE : FM_FALSE;
}

FM_Result FM_Qspi_SetMappedReads(FM_QspiDevice *dev, FM_Bool allowed)
{
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    if (!allowed)
    {
        FM_Result r = qspi_exit_mmap(dev);

        if (r != FM_OK)
        {
            return r;
        }
    }
    dev->mmap_allowed = allowed;
    FM_EVENT_LOG("memory-mapped reads %s", allowed ? "enabled" : "disabled");
    return FM_OK;
}

FM_Result FM_Qspi_Read(FM_QspiDevice *dev, uint64_t addr, void *buf,
                       uint32_t len)
{
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(buf, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");
    if (len == 0u)
    {
        return FM_OK;
    }
    FM_CHECK(addr + len <= dev->info.size_bytes, FM_ERR_INVALID_ARGUMENT,
             "read past end: addr=%lu len=%lu size=%lu",
             (unsigned long)addr, (unsigned long)len,
             (unsigned long)dev->info.size_bytes);

    FM_TRACE_LOG("read %lu @ %lu", (unsigned long)len, (unsigned long)addr);
    dev->stat_reads++;

    /* مسیر سریع: اگر نگاشت در دسترس است، خواندن فقط یک memcpy از
     * فضای آدرس است -- بدون فرستادن دستور روی سیم. */
    if (qspi_can_mmap(dev) && qspi_enter_mmap(dev) == FM_OK)
    {
        const volatile uint8_t *src = dev->io.mmap_base + addr;
        uint8_t *dst = (uint8_t *)buf;
        uint32_t i;

        for (i = 0; i < len; i++)
        {
            dst[i] = src[i];
        }
        dev->stat_mmap_reads++;
        return FM_OK;
    }

    return dev->io.read(dev->io.ctx, QCMD_READ_DATA, (uint32_t)addr,
                        dev->info.addr_bytes, 0u, buf, len);
}

/* ------------------------------------------------------------------ */
/* پاک‌کردن                                                            */
/* ------------------------------------------------------------------ */

/*
 * بهینه‌سازی: سکتور خالی را پاک نکن.
 *
 * خواندن ۴ کیلوبایت (به‌خصوص از راه memory-mapped) در برابر یک
 * پاک‌شدن ۴۵ تا ۴۰۰ میلی‌ثانیه‌ای عملاً مجانی است، و روی فایل‌هایی
 * که مکرر بازنویسی می‌شوند تعداد پاک‌شدن‌ها و فرسایش فلش را واقعاً
 * کم می‌کند. رفتار از نظر دیتا دقیقاً همان است: پاک‌کردن یعنی همهٔ
 * بیت‌ها 1 شوند؛ سکتور خالی همین حالت را دارد.
 */
#ifndef FM_QSPI_ERASE_SKIP_BLANK
#define FM_QSPI_ERASE_SKIP_BLANK 1
#endif

static FM_Bool qspi_sector_is_blank(FM_QspiDevice *dev, uint64_t addr)
{
    uint8_t chunk[128];
    uint64_t a = addr;
    uint32_t left = dev->info.sector_size;

    while (left > 0u)
    {
        uint32_t n = (left < (uint32_t)sizeof(chunk))
                         ? left : (uint32_t)sizeof(chunk);
        uint32_t i;

        /* خواندن ناموفق؟ محتاط باش و پاک کن (safe side). */
        if (FM_Qspi_Read(dev, a, chunk, n) != FM_OK)
        {
            return FM_FALSE;
        }
        for (i = 0; i < n; i++)
        {
            if (chunk[i] != 0xFFu)
            {
                return FM_FALSE;
            }
        }
        a += n;
        left -= n;
    }
    return FM_TRUE;
}

/* یک سکتور را پاک کن. addr باید هم‌تراز سکتور باشد.
 * پاک‌سازی/نوشتن گاهی «گیر» می‌کند (لاگ برد: WIP تا بی‌نهایت). بعد از
 * ریست نرمِ داخل WaitReady، یک تلاش دوباره معنا دارد — در اکثر قریب به
 * اتفاق دفعات دوم سالم تمام می‌شود و یک گیرِ اتفاقی کل سشن را نمی‌کشد. */
static FM_Result qspi_erase_sector(FM_QspiDevice *dev, uint64_t addr)
{
    FM_Result r;

    r = qspi_write_enable(dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.cmd_addr(dev->io.ctx, QCMD_SECTOR_ERASE_4K, (uint32_t)addr,
                         dev->info.addr_bytes);
    if (r != FM_OK)
    {
        FM_ERR_LOG("sector erase @ %lu failed: %d", (unsigned long)addr,
                   (int)r);
        return r;
    }
    dev->stat_erases++;
    r = FM_Qspi_WaitReady(dev, qspi_erase_timeout(dev));
    if (r == FM_ERR_TIMEOUT)
    {
        /* لاگ برد (دورهای ۱۶-۱۷): تایم‌اوت ۴ ثانیه با WIP=0x03، در حالی
         * که خواندن‌ها سالم‌اند و chip erase از مسیر تمیز ~16 ثانیه‌ای
         * سالم تمام شده. WIP+WEL یعنی قطعه واقعاً در حال پاک‌سازی
         * «طولانی» است و تنها op بالای ۴ ثانیه chip erase است. پس قبل از
         * سقط: یک RESUME (اگر قطعه در suspend مانده باشد آزاد می‌شود) و
         * بعد تا ۳۰ ثانیه صبر با لاگ زنده. اگر WIP حدود ۱۶ ثانیه افتاد،
         * مقصر یک دستور chip-erase-تصادفی روی باس بوده؛ اگر تا ۳۰ ثانیه
         * هم بود، گیر واقعی سخت‌افزاری است. */
        uint32_t waited = 0;
        uint8_t st = 0;

        FM_ERR_LOG("sector erase @0x%08lX hanuz busy - 30s montazer",
                   (unsigned long)addr);
        (void)dev->io.cmd(dev->io.ctx, QCMD_ERASE_RESUME);
        while (waited < 30000u)
        {
            FM_Result rs = FM_Qspi_ReadStatus(dev, &st);

            if (rs != FM_OK)
            {
                break;
            }
            if ((st & QSTATUS_BUSY) == 0u)
            {
                break;
            }
            if ((waited % 2000u) == 0u)
            {
                FM_ERR_LOG("erase busy %lums (SR=0x%02X)",
                           (unsigned long)waited, st);
            }
            qspi_delay(dev, 1u);
            waited++;
        }
        /* However the wait ended, this erase did not finish: WaitReady
         * has sent a software reset, and a reset aborts an erase in
         * progress.  The chip is free again, the sector is somewhere
         * between its old content and blank.  This used to return FM_OK
         * here ("the sector may be half erased"); the caller then
         * programmed its data over the leftovers and reported success
         * for a sector that read back wrong.  Erase it again, and only
         * the result of that is the answer. */
        FM_EVENT_LOG("sector erase @0x%08lX did not finish (%lu ms) - "
                     "erasing again", (unsigned long)addr,
                     (unsigned long)waited);
        r = qspi_write_enable(dev);
        if (r == FM_OK)
        {
            r = dev->io.cmd_addr(dev->io.ctx, QCMD_SECTOR_ERASE_4K,
                                 (uint32_t)addr, dev->info.addr_bytes);
            if (r == FM_OK)
            {
                dev->stat_erases++;
                r = FM_Qspi_WaitReady(dev, qspi_erase_timeout(dev));
            }
        }
        if (r != FM_OK)
        {
            FM_ERR_LOG("sector erase @0x%08lX failed twice (%d)",
                       (unsigned long)addr, (int)r);
        }
    }
    return r;
}

FM_Result FM_Qspi_Erase(FM_QspiDevice *dev, uint64_t addr, uint64_t len)
{
    uint64_t first;
    uint64_t last;
    uint64_t a;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");
    if (len == 0u)
    {
        return FM_OK;
    }
    FM_CHECK(addr + len <= dev->info.size_bytes, FM_ERR_INVALID_ARGUMENT,
             "erase past end of chip");

    /* به مرز سکتور گرد کن — پاک‌کردن جزئی روی NOR وجود ندارد. */
    first = addr - (addr % dev->info.sector_size);
    last = addr + len;
    if ((last % dev->info.sector_size) != 0u)
    {
        last += dev->info.sector_size - (last % dev->info.sector_size);
    }

    FM_TRACE_LOG("erase %lu..%lu", (unsigned long)first,
                 (unsigned long)last);
    for (a = first; a < last; a += dev->info.sector_size)
    {
        FM_Result r;

#if FM_QSPI_ERASE_SKIP_BLANK
        if (qspi_sector_is_blank(dev, a))
        {
            dev->stat_erases_skipped++;
            FM_TRACE_LOG("erase skip (blank) @ %lu", (unsigned long)a);
            continue;
        }
#endif
        r = qspi_erase_sector(dev, a);
        if (r != FM_OK)
        {
            return r;
        }
    }
    return FM_OK;
}

FM_Result FM_Qspi_EraseChip(FM_QspiDevice *dev)
{
    FM_Result r;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");

    FM_EVENT_LOG("chip erase (up to %lu ms)",
                 (unsigned long)dev->info.erase_chip_ms);
    r = qspi_write_enable(dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.cmd(dev->io.ctx, QCMD_CHIP_ERASE);
    if (r != FM_OK)
    {
        return r;
    }
    dev->stat_erases++;
    return FM_Qspi_WaitReady(dev, dev->info.erase_chip_ms);
}

FM_Result FM_Qspi_EraseChipStart(FM_QspiDevice *dev)
{
    FM_Result r;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");
    r = qspi_write_enable(dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.cmd(dev->io.ctx, QCMD_CHIP_ERASE);
    if (r != FM_OK)
    {
        return r;
    }
    dev->stat_erases++;
    return FM_OK;
}

FM_Result FM_Qspi_ReadStatus(FM_QspiDevice *dev, uint8_t *out_status)
{
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(out_status, FM_ERR_INVALID_ARGUMENT);
    return dev->io.read(dev->io.ctx, QCMD_READ_STATUS1, 0u, 0u, 0u,
                        out_status, 1u);
}

/* ------------------------------------------------------------------ */
/* نوشتن                                                               */
/* ------------------------------------------------------------------ */

/* نوشتن داخل یک صفحه. طول نباید از مرز صفحه رد شود. */
static FM_Result qspi_program_page(FM_QspiDevice *dev, uint64_t addr,
                                   const uint8_t *buf, uint32_t len)
{
    FM_Result r;

    r = qspi_write_enable(dev);
    if (r != FM_OK)
    {
        return r;
    }
    r = dev->io.write(dev->io.ctx, QCMD_PAGE_PROGRAM, (uint32_t)addr,
                      dev->info.addr_bytes, buf, len);
    if (r != FM_OK)
    {
        FM_ERR_LOG("page program @ %lu failed: %d", (unsigned long)addr,
                   (int)r);
        return r;
    }
    dev->stat_writes++;
    {
        uint32_t t = dev->info.page_program_ms * 10u;
        FM_Result rr;

        if (t < 1000u) t = 1000u;
        rr = FM_Qspi_WaitReady(dev, t);
        if (rr == FM_ERR_TIMEOUT)
        {
            FM_EVENT_LOG("page program stuck - retrying once");
            rr = qspi_write_enable(dev);
            if (rr == FM_OK)
            {
                rr = dev->io.write(dev->io.ctx, QCMD_PAGE_PROGRAM,
                                   (uint32_t)addr, dev->info.addr_bytes,
                                   buf, len);
                if (rr == FM_OK)
                {
                    rr = FM_Qspi_WaitReady(dev, t);
                }
            }
        }
        return rr;
    }
}

/* نوشتن خام در ناحیه‌ای که از قبل پاک شده. مرز صفحه رعایت می‌شود. */
static FM_Result qspi_write_erased(FM_QspiDevice *dev, uint64_t addr,
                                   const uint8_t *buf, uint32_t len)
{
    while (len > 0u)
    {
        uint32_t page_off = (uint32_t)(addr % dev->info.page_size);
        uint32_t room = dev->info.page_size - page_off;
        uint32_t n = (len < room) ? len : room;
        FM_Result r = qspi_program_page(dev, addr, buf, n);

        if (r != FM_OK)
        {
            return r;
        }
        addr += n;
        buf += n;
        len -= n;
    }
    return FM_OK;
}

/* Can [addr, addr + len) take `src` by programming alone?  Yes when
 * every byte there is either erased (0xFF) or already the byte wanted.
 * (A NOR cell could take more -- any change that only clears bits --
 * but programming a byte twice with different values is exactly what
 * the parts with internal error correction forbid, so that case goes
 * the long way.)  *same: the chip already holds exactly these bytes. */
static FM_Result qspi_can_program(FM_QspiDevice *dev, uint64_t addr,
                                  const uint8_t *src, uint32_t len,
                                  FM_Bool *can, FM_Bool *same)
{
    uint8_t have[128];

    *can = FM_TRUE;
    *same = FM_TRUE;
    while (len > 0u)
    {
        uint32_t n = (len < (uint32_t)sizeof(have))
                         ? len : (uint32_t)sizeof(have);
        uint32_t i;
        FM_Result r = FM_Qspi_Read(dev, addr, have, n);

        if (r != FM_OK)
        {
            return r;
        }
        for (i = 0; i < n; i++)
        {
            if (have[i] != src[i])
            {
                *same = FM_FALSE;
                if (have[i] != 0xFFu)
                {
                    *can = FM_FALSE;
                    return FM_OK;
                }
            }
        }
        addr += n;
        src += n;
        len -= n;
    }
    return FM_OK;
}

/* Write `len` bytes at `addr`, whatever is there now.
 *
 * Sector by sector, the cheapest way that gives the right bytes:
 *   1. the chip already holds them          -> nothing to do;
 *   2. they can be programmed over what is
 *      there (an erased range, above all)   -> program, no erase;
 *   3. a whole sector                       -> erase, program;
 *   4. part of a sector that needs an erase -> read the sector, change
 *      it in RAM, erase, program it back.  Needs the sector buffer of
 *      FM_Qspi_Init (FM_ERR_UNSUPPORTED without one).
 *
 * Case 2 is what a log-structured file system does all the time:
 * littlefs erases a block and then fills it 256 bytes at a time.  Each
 * of those writes used to take case 4 -- a 4 KB read, an erase of the
 * sector and a 4 KB program for 256 new bytes, with every earlier,
 * already committed write of that sector in RAM only while the sector
 * was blank: a power cut there lost files that had been closed long
 * before.  Without a sector buffer the same writes were refused and the
 * volume could not even be mounted. */
FM_Result FM_Qspi_Write(FM_QspiDevice *dev, uint64_t addr, const void *buf,
                        uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(buf, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");
    if (len == 0u)
    {
        return FM_OK;
    }
    FM_CHECK(addr + len <= dev->info.size_bytes, FM_ERR_INVALID_ARGUMENT,
             "write past end of chip");

    FM_TRACE_LOG("write %lu @ %lu", (unsigned long)len,
                 (unsigned long)addr);

    while (len > 0u)
    {
        uint64_t sect_base = addr - (addr % dev->info.sector_size);
        uint32_t off = (uint32_t)(addr - sect_base);
        uint32_t room = dev->info.sector_size - off;
        uint32_t n = (len < room) ? len : room;
        FM_Bool can = FM_FALSE;
        FM_Bool same = FM_FALSE;
        FM_Result r;

        r = qspi_can_program(dev, addr, src, n, &can, &same);
        if (r != FM_OK)
        {
            return r;
        }
        if (same)
        {
            /* 1) already there */
        }
        else if (can)
        {
            /* 2) program over what is there */
            r = qspi_write_erased(dev, addr, src, n);
        }
        else if (off == 0u && n == dev->info.sector_size)
        {
            /* 3) a whole sector: erase and program */
            r = qspi_erase_sector(dev, sect_base);
            if (r == FM_OK)
            {
                r = qspi_write_erased(dev, sect_base, src, n);
            }
        }
        else
        {
            /* 4) read-modify-erase-write */
            FM_CHECK(dev->sector_buf != NULL, FM_ERR_UNSUPPORTED,
                     "a write over programmed flash that is not a whole "
                     "sector needs a sector buffer (pass one to "
                     "FM_Qspi_Init)");
            r = FM_Qspi_Read(dev, sect_base, dev->sector_buf,
                             dev->info.sector_size);
            if (r == FM_OK)
            {
                memcpy(dev->sector_buf + off, src, n);
                r = qspi_erase_sector(dev, sect_base);
            }
            if (r == FM_OK)
            {
                r = qspi_write_erased(dev, sect_base, dev->sector_buf,
                                      dev->info.sector_size);
            }
        }
        if (r != FM_OK)
        {
            return r;
        }
        addr += n;
        src += n;
        len -= n;
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* پل به لایهٔ Storage                                                 */
/* ------------------------------------------------------------------ */

static FM_Result qspi_storage_read(FM_Storage *s, uint64_t offset,
                                   void *buf, uint32_t len)
{
    FM_QspiDevice *dev;

    FM_CHECK_PTR(s, FM_ERR_INVALID_ARGUMENT);
    dev = (FM_QspiDevice *)s->user_data;
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    return FM_Qspi_Read(dev, offset, buf, len);
}

static FM_Result qspi_storage_write(FM_Storage *s, uint64_t offset,
                                    const void *buf, uint32_t len)
{
    FM_QspiDevice *dev;

    FM_CHECK_PTR(s, FM_ERR_INVALID_ARGUMENT);
    dev = (FM_QspiDevice *)s->user_data;
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    return FM_Qspi_Write(dev, offset, buf, len);
}

static FM_Result qspi_storage_erase(FM_Storage *s, uint64_t offset,
                                    uint32_t len)
{
    FM_QspiDevice *dev;

    FM_CHECK_PTR(s, FM_ERR_INVALID_ARGUMENT);
    dev = (FM_QspiDevice *)s->user_data;
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    return FM_Qspi_Erase(dev, offset, len);
}

static FM_Result qspi_storage_sync(FM_Storage *s)
{
    FM_QspiDevice *dev;

    FM_CHECK_PTR(s, FM_ERR_INVALID_ARGUMENT);
    dev = (FM_QspiDevice *)s->user_data;
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    /* نوشتن‌ها همگی همان لحظه تمام شده‌اند؛ فقط مطمئن شو busy نیست. */
    return FM_Qspi_WaitReady(dev, qspi_erase_timeout(dev));
}

FM_Result FM_Qspi_RegisterStorage(FM_QspiDevice *dev, FM_Storage *storage,
                                  const char *name)
{
    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(storage, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(name, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(dev->ready, FM_ERR_NOT_READY, "device not initialised");

    {
        FM_StorageConfig cfg;
        FM_Result r;

        memset(&cfg, 0, sizeof(cfg));
        cfg.name = name;
        cfg.type = FM_STORAGE_QSPI;
        cfg.capacity = dev->info.size_bytes;
        cfg.block_size = dev->info.sector_size;
        cfg.read_size = 1u;                    /* NOR reads any byte    */
        cfg.write_size = dev->info.page_size;  /* program granularity   */
        cfg.erase_size = dev->info.sector_size;/* smallest erase unit   */
        cfg.read_only = FM_FALSE;
        cfg.present = FM_TRUE;
        cfg.user_data = dev;
        cfg.read = qspi_storage_read;
        cfg.write = qspi_storage_write;
        cfg.erase = qspi_storage_erase;
        cfg.sync = qspi_storage_sync;

        r = Storage_InitDriver(storage, &cfg);
        if (r != FM_OK)
        {
            FM_ERR_LOG("Storage_InitDriver failed: %d", (int)r);
            return r;
        }
    }

    return Storage_Register(storage);
}
