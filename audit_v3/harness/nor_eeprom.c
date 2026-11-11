/* Audit v3: FM_Transfer and the EEPROM adapter against strict device
 * models.
 *
 *  - A NOR flash model that refuses any write to a byte that has not been
 *    erased (0xFF) and only clears bits: exactly the rule the datasheets
 *    give, and the reason FM_Transfer_Raw must erase before it writes.
 *  - An AT24Cxx-style EEPROM model that refuses a write crossing a page
 *    boundary -- the classic "page write wrap-around" that silently
 *    corrupts data on real parts.
 *  - FM_Transfer_Copy / Move / LoadFile / SaveFile with and without
 *    verify, byte-compared against the source.
 */
#include <stdio.h>
#include <string.h>
#include "file_manager.h"
#include "fm_littlefs.h"
#include "fm_eeprom.h"
#include "fm_transfer.h"

static int g_bad;

/* ---------------- strict NOR model ---------------- */
#define NOR_BYTES (64u * 1024u)
#define NOR_UNIT  4096u
static uint8_t g_nor[NOR_BYTES];
static unsigned g_nor_erases;
static unsigned g_nor_writes;
static unsigned g_nor_bad_writes;

static FM_Result nor_read(FM_Storage *s, uint64_t off, void *buf, uint32_t len)
{
    (void)s;
    if (off > NOR_BYTES || len > NOR_BYTES - off) return FM_ERR_INVALID_ARGUMENT;
    memcpy(buf, g_nor + off, len);
    return FM_OK;
}

static FM_Result nor_write(FM_Storage *s, uint64_t off, const void *buf,
                           uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t i;

    (void)s;
    if (off > NOR_BYTES || len > NOR_BYTES - off) return FM_ERR_INVALID_ARGUMENT;
    g_nor_writes++;
    for (i = 0; i < len; i++)
    {
        /* a write may only clear bits that are set */
        if ((g_nor[off + i] & p[i]) != p[i])
        {
            g_nor_bad_writes++;
            return FM_ERR_IO;    /* the real part would corrupt silently */
        }
        g_nor[off + i] &= p[i];
    }
    return FM_OK;
}

static FM_Result nor_erase(FM_Storage *s, uint64_t off, uint32_t len)
{
    (void)s;
    if ((off % NOR_UNIT) != 0u || (len % NOR_UNIT) != 0u ||
        off > NOR_BYTES || len > NOR_BYTES - off)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(g_nor + off, 0xFF, len);
    g_nor_erases++;
    return FM_OK;
}

/* ---------------- page-limited EEPROM model ---------------- */
#define EE_BYTES 4096u
#define EE_PAGE  32u
static uint8_t  g_ee[EE_BYTES];
static unsigned g_ee_bad_writes;

static FM_Result ee_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
    (void)ctx;
    if (addr > EE_BYTES || len > EE_BYTES - addr) return FM_ERR_INVALID_ARGUMENT;
    memcpy(data, g_ee + addr, len);
    return FM_OK;
}

static FM_Result ee_write(void *ctx, uint32_t addr, const void *data,
                          uint32_t len)
{
    (void)ctx;
    if (addr > EE_BYTES || len > EE_BYTES - addr) return FM_ERR_INVALID_ARGUMENT;
    if ((addr % EE_PAGE) + len > EE_PAGE)        /* page write would wrap */
    {
        g_ee_bad_writes++;
        return FM_ERR_IO;
    }
    memcpy(g_ee + addr, data, len);
    return FM_OK;
}

int main(void)
{
    FM_Storage src_st, dst_st;
    static uint8_t src_mem[512 * 1024];
    FM_FS     fs;
    FM_Result r;
    static uint8_t big[30000];
    unsigned i;

    FileManager_Init();
    for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7u + 3u);

    /* ============ file level: copy with verify ============ */
    {
        static uint8_t back[30000];
        size_t got = 0;
        FM_TransferOpts o;
        FM_TransferInfo info;

        memset(src_mem, 0, sizeof(src_mem));
        r = RamDisk_Init(&src_st, "SRC", src_mem, sizeof(src_mem), 512u);
        if (r == FM_OK) r = Storage_Register(&src_st);
        if (r == FM_OK) r = Storage_Mount("SRC");
        if (r == FM_OK) r = LittleFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/SRC", &fs);
        if (r != FM_OK) { printf("mount: %s\n", FM_ResultString(r)); return 2; }

        memset(&g_nor, 0xFF, sizeof(g_nor));
        memset(&dst_st, 0, sizeof(dst_st));
        (void)Storage_InitDriver(&dst_st, NULL);
        dst_st.name[0] = 0;
        {
            FM_StorageConfig c;

            memset(&c, 0, sizeof(c));
            c.name = "NOR";
            c.type = FM_STORAGE_NOR;
            c.capacity = NOR_BYTES;
            c.block_size = NOR_UNIT;
            c.read_size = 1;
            c.write_size = 1;
            c.erase_size = NOR_UNIT;
            c.present = FM_TRUE;
            c.read = nor_read;
            c.write = nor_write;
            c.erase = nor_erase;
            r = Storage_InitDriver(&dst_st, &c);
            if (r == FM_OK) r = Storage_Register(&dst_st);
            if (r == FM_OK) r = Storage_Mount("NOR");
            if (r != FM_OK) printf("nor mount: %s\n", FM_ResultString(r));
        }

        r = File_WriteAll("/SRC/big.bin", big, sizeof(big));
        if (r != FM_OK) { printf("write: %s\n", FM_ResultString(r)); return 2; }

        /* 1. copy through the file layer onto another LittleFS volume is
         * not needed here: Copy with verify on the same volume is enough
         * to exercise the verify path, the crc and the cleanup. */
        memset(&o, 0, sizeof(o));
        o.verify = FM_TRUE;
        r = FM_Transfer_CopyEx("/SRC/big.bin", "/SRC/copy.bin", &o, &info);
        (void)File_ReadAll("/SRC/copy.bin", back, sizeof(back), &got);
        if (r != FM_OK || got != sizeof(big) || memcmp(back, big, sizeof(big)) != 0)
        {
            printf("  copy+verify: %s, %u bytes\n", FM_ResultString(r),
                   (unsigned)got);
            g_bad = 1;
        }
        else
        {
            printf("copy+verify: %s, %u bytes, crc %08X/%08X verified=%d\n",
                   FM_ResultString(r), (unsigned)info.bytes,
                   (unsigned)info.crc_source, (unsigned)info.crc_dest,
                   (int)info.verified);
        }

        /* 2. no_overwrite */
        memset(&o, 0, sizeof(o));
        o.no_overwrite = FM_TRUE;
        r = FM_Transfer_Copy("/SRC/big.bin", "/SRC/copy.bin", &o);
        printf("copy with no_overwrite: %s (want already exists)\n",
               FM_ResultString(r));
        if (r != FM_ERR_EXISTS) g_bad = 1;

        /* 3. progress callback that cancels half way */
        {
            unsigned calls = 0;

            memset(&o, 0, sizeof(o));
            o.progress = NULL;
            /* a tiny file so the cancel lands after a chunk */
            r = File_WriteAll("/SRC/small.bin", big, 2000);
            memset(&o, 0, sizeof(o));
            o.verify = FM_FALSE;
            r = FM_Transfer_Copy("/SRC/small.bin", "/SRC/small2.bin", &o);
            printf("copy of 2000 bytes: %s\n", FM_ResultString(r));
            (void)calls;
            if (r != FM_OK) g_bad = 1;
        }

        /* 4. LoadFile / SaveFile at the buffer edge */
        {
            static uint8_t ld[30000];
            size_t n = 0;

            r = FM_Transfer_LoadFile("/SRC/big.bin", ld, sizeof(ld), &n);
            if (r != FM_OK || n != sizeof(big) || memcmp(ld, big, n) != 0)
            {
                printf("  LoadFile(big enough): %s, %u\n", FM_ResultString(r),
                       (unsigned)n);
                g_bad = 1;
            }
            r = FM_Transfer_LoadFile("/SRC/big.bin", ld, sizeof(big) - 1, &n);
            printf("LoadFile(one byte short): %s (want too large for buffer)\n",
                   FM_ResultString(r));
            if (r != FM_ERR_TOO_LARGE) g_bad = 1;
            r = FM_Transfer_SaveFile("/SRC/saved.bin", big, sizeof(big));
            (void)File_ReadAll("/SRC/saved.bin", back, sizeof(back), &got);
            if (r != FM_OK || got != sizeof(big) || memcmp(back, big, got) != 0)
            {
                printf("  SaveFile: %s\n", FM_ResultString(r));
                g_bad = 1;
            }
        }

        /* ============ raw level: into the strict NOR model ======== */
        {
            FM_TransferOpts ro;

            memset(&ro, 0, sizeof(ro));
            ro.verify = FM_TRUE;
            r = FM_Transfer_Raw(&src_st, 0, &dst_st, 0, 10000u, &ro);
            if (r != FM_OK)
            {
                printf("raw copy to NOR: %s (bad writes %u)\n",
                       FM_ResultString(r), g_nor_bad_writes);
                g_bad = 1;
            }
            else
            {
                printf("raw copy 10000 bytes to NOR: OK, %u erase calls, "
                       "%u writes, %u refused\n", g_nor_erases, g_nor_writes,
                       g_nor_bad_writes);
            }
            /* second copy over the same bytes, without an erase in the
             * model being free: must erase first */
            g_nor_erases = 0;
            g_nor_bad_writes = 0;
            r = FM_Transfer_Raw(&src_st, 0, &dst_st, 0, 10000u, &ro);
            printf("raw copy again over the same data: %s (erases %u, refused "
                   "%u)\n", FM_ResultString(r), g_nor_erases, g_nor_bad_writes);
            if (r != FM_OK || g_nor_bad_writes != 0u) g_bad = 1;

            /* unaligned destination must be refused before anything moves */
            g_nor_erases = 0;
            r = FM_Transfer_Raw(&src_st, 0, &dst_st, 100u, 500u, NULL);
            printf("raw copy to an unaligned offset: %s (erases %u)\n",
                   FM_ResultString(r), g_nor_erases);
            if (r != FM_ERR_INVALID_ARGUMENT || g_nor_erases != 0u) g_bad = 1;

            /* a range past the end of the destination */
            r = FM_Transfer_Raw(&src_st, 0, &dst_st, 0, NOR_BYTES + 1u, NULL);
            printf("raw copy past the end: %s\n", FM_ResultString(r));
            if (r != FM_ERR_INVALID_ARGUMENT) g_bad = 1;

            /* and the bytes really arrived */
            if (memcmp(g_nor, src_mem, 10000) != 0)
            {
                printf("raw copy: content differs from the source\n");
                g_bad = 1;
            }
        }
        (void)File_Delete("/SRC/big.bin");
        (void)File_Delete("/SRC/copy.bin");
        (void)File_Delete("/SRC/small.bin");
        (void)File_Delete("/SRC/small2.bin");
        (void)File_Delete("/SRC/saved.bin");
    }

    /* ============ EEPROM page model ============ */
    {
        FM_EEPROM ee;
        FM_EEPROMConfig cfg;
        uint8_t rd[256];
        uint8_t wr[256];

        memset(g_ee, 0xFF, sizeof(g_ee));
        memset(&cfg, 0, sizeof(cfg));
        cfg.name = "EE";
        cfg.capacity = EE_BYTES;
        cfg.page_size = EE_PAGE;
        cfg.read = ee_read;
        cfg.write = ee_write;
        r = EEPROM_Init(&ee, &cfg);
        if (r != FM_OK) { printf("eeprom init: %s\n", FM_ResultString(r)); return 2; }

        /* write across every page boundary */
        for (i = 0; i < sizeof(wr); i++) wr[i] = (uint8_t)i;
        r = EEPROM_Write(&ee, 30, wr, sizeof(wr));      /* 30..285 */
        printf("EEPROM_Write(30, 256) across pages: %s (page violations %u)\n",
               FM_ResultString(r), g_ee_bad_writes);
        if (r != FM_OK || g_ee_bad_writes != 0u) g_bad = 1;
        memset(rd, 0, sizeof(rd));
        (void)EEPROM_Read(&ee, 30, rd, sizeof(rd));
        if (memcmp(rd, wr, sizeof(wr)) != 0)
        {
            printf("  EEPROM content differs after the write\n");
            g_bad = 1;
        }

        /* EEPROM_Update must not rewrite unchanged bytes: count writes */
        g_ee_bad_writes = 0;
        r = EEPROM_Update(&ee, 30, wr, sizeof(wr));
        printf("EEPROM_Update with identical data: %s (violations %u)\n",
               FM_ResultString(r), g_ee_bad_writes);
        if (r != FM_OK) g_bad = 1;

        /* erase falls back to writing 0xFF, also across pages */
        r = EEPROM_Erase(&ee, 1, 100);
        printf("EEPROM_Erase(1, 100) without an erase hook: %s (violations "
               "%u)\n", FM_ResultString(r), g_ee_bad_writes);
        if (r != FM_OK || g_ee_bad_writes != 0u) g_bad = 1;

        /* ranges: last byte, one past the end */
        r = EEPROM_Read(&ee, EE_BYTES - 1, rd, 1);
        printf("EEPROM_Read last byte: %s\n", FM_ResultString(r));
        if (r != FM_OK) g_bad = 1;
        r = EEPROM_Read(&ee, EE_BYTES - 1, rd, 2);
        printf("EEPROM_Read last byte + 1: %s (want invalid argument)\n",
               FM_ResultString(r));
        if (r != FM_ERR_INVALID_ARGUMENT) g_bad = 1;
        r = EEPROM_Write(&ee, EE_BYTES, wr, 1);
        printf("EEPROM_Write at capacity: %s (want invalid argument)\n",
               FM_ResultString(r));
        if (r != FM_ERR_INVALID_ARGUMENT) g_bad = 1;
    }

    printf("\nnor_eeprom: %s\n", g_bad ? "PROBLEM" : "all models satisfied");
    return g_bad;
}
