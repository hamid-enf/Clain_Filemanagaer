/*
 * fm_spiflash.c — پل SPI به همان درایور فلش
 *
 * تمام کاری که اینجا می‌شود: تبدیل «دستور + آدرس + داده» به یک تراکنش
 * SPI با CS دستی. منطق فلش دست‌نخورده در fm_qspi.c می‌ماند.
 */

#define FM_DEBUG_TAG "spiflash"

#include <string.h>

#include "fm_spiflash.h"
#include "fm_debug.h"

/* بزرگ‌ترین سرآیند: ۱ بایت دستور + ۴ بایت آدرس + ۱ بایت dummy. */
#define SPIF_HDR_MAX 6u

/*
 * ساختن سرآیند در یک بافر کوچک.
 *
 * آدرس همیشه big-endian فرستاده می‌شود — این در تمام قطعات SPI NOR
 * یکسان است، مستقل از endian بودن پردازنده.
 */
static uint32_t spif_build_header(uint8_t *hdr, uint8_t opcode,
                                  uint32_t addr, uint8_t addr_bytes,
                                  uint8_t dummy_bytes)
{
    uint32_t n = 0;

    hdr[n++] = opcode;
    if (addr_bytes == 4u)
    {
        hdr[n++] = (uint8_t)((addr >> 24) & 0xFFu);
    }
    if (addr_bytes >= 3u)
    {
        hdr[n++] = (uint8_t)((addr >> 16) & 0xFFu);
        hdr[n++] = (uint8_t)((addr >> 8) & 0xFFu);
        hdr[n++] = (uint8_t)(addr & 0xFFu);
    }
    while (dummy_bytes-- > 0u && n < SPIF_HDR_MAX)
    {
        hdr[n++] = 0xFFu;
    }
    return n;
}

/* پایین‌ترین لایه: CS بگیر، سرآیند بفرست، داده رد و بدل کن، CS رها کن. */
static FM_Result spif_txn(FM_SpiFlashPort *p, const uint8_t *hdr,
                          uint32_t hdr_len, const uint8_t *tx,
                          uint8_t *rx, uint32_t len)
{
    FM_Result r;

    if (p->cs_select != NULL)
    {
        p->cs_select(p->ctx);
    }

    r = p->transfer(p->ctx, hdr, NULL, hdr_len);
    if (r == FM_OK && len > 0u)
    {
        r = p->transfer(p->ctx, tx, rx, len);
    }

    /* CS باید در هر حالتی رها شود، حتی وقتی انتقال شکست خورده —
     * وگرنه قطعه در وسط یک دستور گیر می‌کند و تراکنش بعدی هم خراب
     * می‌شود. */
    if (p->cs_release != NULL)
    {
        p->cs_release(p->ctx);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* چهار callback ای که FM_QspiIo می‌خواهد                              */
/* ------------------------------------------------------------------ */

static FM_Result spif_cmd(void *ctx, uint8_t opcode)
{
    FM_SpiFlashPort *p = (FM_SpiFlashPort *)ctx;
    uint8_t hdr[SPIF_HDR_MAX];
    uint32_t n;

    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    n = spif_build_header(hdr, opcode, 0u, 0u, 0u);
    return spif_txn(p, hdr, n, NULL, NULL, 0u);
}

static FM_Result spif_cmd_addr(void *ctx, uint8_t opcode, uint32_t addr,
                               uint8_t addr_bytes)
{
    FM_SpiFlashPort *p = (FM_SpiFlashPort *)ctx;
    uint8_t hdr[SPIF_HDR_MAX];
    uint32_t n;

    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    n = spif_build_header(hdr, opcode, addr, addr_bytes, 0u);
    return spif_txn(p, hdr, n, NULL, NULL, 0u);
}

static FM_Result spif_read(void *ctx, uint8_t opcode, uint32_t addr,
                           uint8_t addr_bytes, uint8_t dummy_cycles,
                           void *buf, uint32_t len)
{
    FM_SpiFlashPort *p = (FM_SpiFlashPort *)ctx;
    uint8_t hdr[SPIF_HDR_MAX];
    uint32_t n;

    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(buf, FM_ERR_INVALID_ARGUMENT);
    /* در QSPI dummy بر حسب سیکل است؛ روی SPI تک‌خطی هر بایت ۸ سیکل. */
    n = spif_build_header(hdr, opcode, addr, addr_bytes,
                          (uint8_t)(dummy_cycles / 8u));
    return spif_txn(p, hdr, n, NULL, (uint8_t *)buf, len);
}

static FM_Result spif_write(void *ctx, uint8_t opcode, uint32_t addr,
                            uint8_t addr_bytes, const void *buf,
                            uint32_t len)
{
    FM_SpiFlashPort *p = (FM_SpiFlashPort *)ctx;
    uint8_t hdr[SPIF_HDR_MAX];
    uint32_t n;

    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(buf, FM_ERR_INVALID_ARGUMENT);
    n = spif_build_header(hdr, opcode, addr, addr_bytes, 0u);
    return spif_txn(p, hdr, n, (const uint8_t *)buf, NULL, len);
}

static void spif_delay(void *ctx, uint32_t ms)
{
    FM_SpiFlashPort *p = (FM_SpiFlashPort *)ctx;

    if (p != NULL && p->delay_ms != NULL)
    {
        p->delay_ms(p->ctx, ms);
    }
}

/* ------------------------------------------------------------------ */

FM_Result FM_SpiFlash_Init(FM_QspiDevice *dev, FM_SpiFlashPort *port,
                           FM_QspiChip chip, uint8_t *sector_buf,
                           uint32_t sector_buf_size)
{
    FM_QspiIo io;

    FM_CHECK_PTR(dev, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(port, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK(port->transfer != NULL, FM_ERR_INVALID_ARGUMENT,
             "SPI transfer callback is required");

    /* CS نرم‌افزاری روی SPI معمولی اجباری است: هر «op» باید یک فریم
     * کامل زیر CS باشد. بدون cs_select/cs_release هیچ‌وقت CS کوبیده
     * نمیشود؛ قطعه از اولین دستور به بعد 0x00 جواب میدهد و همهچیز
     * «فیکس-موفق» میشود. (درایور QSPI سخت‌افزاری CS دارد و مستثناست.) */
    if (port->cs_select == NULL || port->cs_release == NULL)
    {
        FM_ERR_LOG("cs_select/cs_release required - CS would never toggle");
        return FM_ERR_INVALID_ARGUMENT;
    }

    memset(&io, 0, sizeof(io));
    io.cmd      = spif_cmd;
    io.cmd_addr = spif_cmd_addr;
    io.read     = spif_read;
    io.write    = spif_write;
    io.delay_ms = spif_delay;
    io.ctx      = port;
    /* حالت نگاشت روی SPI معمولی وجود ندارد: mmap_* عمداً NULL می‌مانند
     * و درایور خودش به مسیر دستوری می‌رود. */

    FM_EVENT_LOG("SPI flash bridge ready");
    return FM_Qspi_Init(dev, &io, chip, sector_buf, sector_buf_size);
}
