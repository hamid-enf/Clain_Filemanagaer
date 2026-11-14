/*
 * audit_v5_tests.h -- tests for the findings of the external audit V5
 * (ReportV4.txt at the repository root; audited commit fc31b25).
 *
 * Included by test/main.c.  Every test here failed before its fix; the
 * comment above each one names the finding by the ID of its report.
 */

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

#if FM_USE_FMFS || FM_USE_LITTLEFS
/* CRIT-02: the drivers hand out FM_MAX_FS contexts.  A context used to
 * come back only from the driver's own unmount -- so every FS_Mount
 * that was refused before the driver was asked (no such storage, mount
 * point taken, table full) kept one for good.  After FM_MAX_FS of those
 * every later *_Init failed, left the FM_FS without functions, and a
 * caller that did not look at the result of Init mounted it and crashed
 * in the first File_Open. */
static FM_FS av5_pool_fs[FM_MAX_FS + 2];

static FM_Result av5_pool_refused(int kind)
{
    FM_Result r = FM_OK;
    size_t i;

    for (i = 0; i < sizeof(av5_pool_fs) / sizeof(av5_pool_fs[0]); i++)
    {
        FM_Result ri;
        FM_Result rm;

        memset(&av5_pool_fs[i], 0, sizeof(av5_pool_fs[i]));
        ri = fc_init(kind, &av5_pool_fs[i]);
        rm = FS_Mount("/av5_nostor", &av5_pool_fs[i]);
        if (ri != FM_OK || av5_pool_fs[i].ops.file_open == NULL)
        {
            printf("[init %u: %d] ", (unsigned)i, (int)ri);
            r = (ri != FM_OK) ? ri : FM_ERR_INTERNAL;
        }
        if (rm == FM_OK)
        {
            printf("[mount %u without a storage worked] ", (unsigned)i);
            (void)FS_Unmount("/av5_nostor");
            r = FM_ERR_INTERNAL;
        }
    }
    /* and the pool still serves a real volume */
    if (r == FM_OK) r = fc_storage(0, kind, 1);
    if (r == FM_OK) r = fc_init(kind, &g_av2_fs);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
    if (r == FM_OK) r = File_WriteText("/fca/p.txt", "pool");
    if (r == FM_OK) r = av2_expect("/fca/p.txt", "pool");
    fc_drop(0);
    return r;
}

/* The other half of the rule: a context that is mounted is never the
 * one that is taken over, however many FM_FS are initialised meanwhile. */
static FM_Result av5_pool_keeps_mounted(int kind)
{
    FM_Result r;
    size_t i;

    r = fc_storage(0, kind, 1);
    if (r == FM_OK) r = fc_init(kind, &g_av2_fs);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
    if (r == FM_OK) r = File_WriteText("/fca/keep.txt", "before");
    for (i = 0; r == FM_OK && i < sizeof(av5_pool_fs) / sizeof(av5_pool_fs[0]);
         i++)
    {
        memset(&av5_pool_fs[i], 0, sizeof(av5_pool_fs[i]));
        r = fc_init(kind, &av5_pool_fs[i]);
        if (r != FM_OK) printf("[init %u: %d] ", (unsigned)i, (int)r);
    }
    if (r == FM_OK) r = av2_expect("/fca/keep.txt", "before");
    if (r == FM_OK) r = File_WriteText("/fca/keep2.txt", "after");
    if (r == FM_OK) r = av2_expect("/fca/keep2.txt", "after");
    fc_drop(0);
    return r;
}

/* An FM_FS whose context went to somebody else is refused at its mount
 * (it is not mounted on the other one's context), and works again after
 * a new Init. */
static FM_Result av5_pool_robbed(int kind)
{
    static FM_FS first;
    FM_Result r;
    FM_Result rm;
    size_t i;

    memset(&first, 0, sizeof(first));
    r = fc_init(kind, &first);
    for (i = 0; r == FM_OK && i < (size_t)FM_MAX_FS; i++)
    {
        memset(&av5_pool_fs[i], 0, sizeof(av5_pool_fs[i]));
        r = fc_init(kind, &av5_pool_fs[i]);
    }
    if (r != FM_OK) return r;
    r = fc_storage(0, kind, 1);
    if (r != FM_OK) return r;
    rm = FS_Mount(g_fc_mount[0], &first);
    if (rm == FM_OK)
    {
        printf("[mounted on a context it no longer owns] ");
        r = FM_ERR_INTERNAL;
    }
    else if (rm != FM_ERR_NOT_READY)
    {
        printf("[refused with %d, want %d] ", (int)rm, (int)FM_ERR_NOT_READY);
        r = FM_ERR_INTERNAL;
    }
    if (r == FM_OK) r = fc_init(kind, &first);
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &first);
    if (r == FM_OK) r = File_WriteText("/fca/again.txt", "again");
    if (r == FM_OK) r = av2_expect("/fca/again.txt", "again");
    fc_drop(0);
    return r;
}

static void av5_pool(void)
{
    int kind;

    for (kind = 0; kind < 2; kind++)
    {
        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "CRIT-02: FMFS, refused mounts do not use up the contexts"
                       : "CRIT-02: LittleFS, refused mounts do not use up the contexts");
        { FM_Result av5_r = av5_pool_refused(kind); CHECK_RESULT(av5_r, "failed"); }
        TEST(kind == 0 ? "CRIT-02: FMFS, a mounted context is never taken over"
                       : "CRIT-02: LittleFS, a mounted context is never taken over");
        { FM_Result av5_r = av5_pool_keeps_mounted(kind); CHECK_RESULT(av5_r, "failed"); }
        TEST(kind == 0 ? "CRIT-02: FMFS, an FM_FS that lost its context is refused, then works after Init"
                       : "CRIT-02: LittleFS, an FM_FS that lost its context is refused, then works after Init");
        { FM_Result av5_r = av5_pool_robbed(kind); CHECK_RESULT(av5_r, "failed"); }
    }

    TEST("CRIT-02: FS_Mount refuses an FM_FS that has no file functions");
    {
        static FM_FS zero;
        int k = av2_kind_built(0) ? 0 : 1;
        FM_Result r = fc_storage(0, k, 1);
        FM_Result rm;

        memset(&zero, 0, sizeof(zero));
        rm = (r == FM_OK) ? FS_Mount(g_fc_mount[0], &zero) : r;
        if (rm == FM_OK)
        {
            /* before the fix: mounted, and this call jumped to NULL */
            FAIL("mounted an FM_FS that was never initialised");
        }
        else if (rm != FM_ERR_INVALID_ARGUMENT)
        {
            FAIL("refused, but not with FM_ERR_INVALID_ARGUMENT");
        }
        else
        {
            PASS();
        }
        fc_drop(0);
    }

#if FM_USE_FMFS
    TEST("HIGH-07: FMFS format of an FM_FS that is not mounted is an error, not a NULL read");
    {
        static FM_FS fs;
        FM_Result r;

        memset(&fs, 0, sizeof(fs));
        r = FMFS_Init(&fs);
        if (r == FM_OK && fs.ops.format != NULL)
        {
            r = fs.ops.format(&fs);
            if (r == FM_ERR_NOT_MOUNTED) r = FM_OK;
            else
            {
                printf("[format gave %d] ", (int)r);
                r = FM_ERR_INTERNAL;
            }
        }
        CHECK_RESULT(r, "failed");
    }
#endif

#if FM_USE_LITTLEFS
    /* HIGH-04: RamDisk_Register() gives a medium with an erase unit of
     * one byte.  LittleFS took that as its block size and ran into
     * LFS_ASSERT(block_size >= 128): abort on the host, undefined on a
     * board built with NDEBUG. */
    TEST("HIGH-04: LittleFS on a RAM disk with a 1-byte erase unit mounts and keeps a file");
    {
        static FM_Storage st;
        static FM_FS fs;
        FM_Result r;

        memset(g_fc_mem[1], 0xFF, 64u * 1024u);
        r = RamDisk_Register(&st, "av5r", g_fc_mem[1], 64u * 1024u);
        if (r == FM_OK) r = LittleFS_Init(&fs);
        if (r == FM_OK) r = FS_Mount("/av5r", &fs);
        if (r == FM_OK) r = File_WriteText("/av5r/a.txt", "small blocks");
        if (r == FM_OK) r = av2_expect("/av5r/a.txt", "small blocks");
        (void)FS_Unmount("/av5r");
        (void)Storage_Unmount("av5r");
        (void)Storage_Unregister("av5r");
        CHECK_RESULT(r, "failed");
    }
#endif
}
#else
static void av5_pool(void) { }
#endif

/* CRIT-03: Text_Replace / Text_InsertLine / Text_DeleteLine build the
 * new file as "<path>.fm_tmp".  For a path of 153..159 characters that
 * name was cut to FM_PATH_MAX - 1 characters: at 159 it was the path
 * itself (the file was truncated while being read, then deleted), a
 * few characters shorter it named a neighbour ("<path>.", "<path>.f",
 * ...), which was overwritten and deleted. */
#if FM_USE_TEXT && FM_PATH_MAX == 160 && FM_NAME_MAX >= 64
static void av5_long_path(char *out, size_t len)
{
    size_t i;

    /* "/av5t" + "/" + 60 + "/" + 60 + "/" + the rest */
    memcpy(out, "/av5t/", 6);
    for (i = 6; i < len; i++) out[i] = (char)('a' + (i % 26u));
    out[66] = '/';
    out[127] = '/';
    out[len] = '\0';
}

static FM_Result av5_text_tmp_one(size_t len)
{
    static const char body[] = "alpha\nbeta\n";
    char path[FM_PATH_MAX + 8];
    char side[FM_PATH_MAX + 8];
    char dir[FM_PATH_MAX];
    size_t cut = (size_t)(FM_PATH_MAX - 1) - len;   /* 0..6 */
    FM_Result r;
    FM_Result got[3];
    int i;

    av5_long_path(path, len);
    memcpy(dir, path, 66); dir[66] = '\0';
    (void)Directory_Create("/av5t");
    (void)Directory_Create(dir);
    memcpy(dir, path, 127); dir[127] = '\0';
    (void)Directory_Create(dir);

    r = File_WriteText(path, body);
    /* the file the cut name would have pointed at */
    memcpy(side, path, len);
    memcpy(side + len, ".fm_tmp", cut);
    side[len + cut] = '\0';
    if (r == FM_OK && cut > 0u) r = File_WriteText(side, "neighbour");
    if (r != FM_OK)
    {
        printf("[setup %u: %d] ", (unsigned)len, (int)r);
        return r;
    }

    got[0] = Text_Replace(path, "alpha", "gamma");
    got[1] = Text_InsertLine(path, 1u, "first");
    got[2] = Text_DeleteLine(path, 1u);
    for (i = 0; i < 3; i++)
    {
        if (got[i] != FM_ERR_TOO_LARGE)
        {
            printf("[len %u call %d: %d] ", (unsigned)len, i, (int)got[i]);
            r = FM_ERR_INTERNAL;
        }
    }
    {
        FM_Result e = av2_expect(path, body);

        if (e != FM_OK)
        {
            printf("[len %u: the file is damaged or gone (%d)] ",
                   (unsigned)len, (int)e);
            r = FM_ERR_INTERNAL;
        }
    }
    if (cut > 0u)
    {
        FM_Result e = av2_expect(side, "neighbour");

        if (e != FM_OK)
        {
            printf("[len %u: the neighbour is damaged or gone (%d)] ",
                   (unsigned)len, (int)e);
            r = FM_ERR_INTERNAL;
        }
    }
    return r;
}

static void av5_text_tmp(void)
{
    TEST("CRIT-03: Text_* on a path whose temporary name does not fit touch nothing");
    {
        FM_Result r = FM_OK;
        size_t len;

        for (len = 153u; len <= 159u; len++)
        {
            FM_Result one = av5_text_tmp_one(len);

            if (r == FM_OK) r = one;
        }
        CHECK_RESULT(r, "failed");
    }

    TEST("CRIT-03: the longest path that still fits is edited as before");
    {
        char path[FM_PATH_MAX + 8];
        FM_Result r;

        av5_long_path(path, 152u);
        r = File_WriteText(path, "alpha\nbeta\n");
        if (r == FM_OK) r = Text_Replace(path, "alpha", "gamma");
        if (r == FM_OK) r = Text_InsertLine(path, 1u, "first");
        if (r == FM_OK) r = av2_expect(path, "first\ngamma\nbeta\n");
        if (r == FM_OK) r = Text_DeleteLine(path, 1u);
        if (r == FM_OK) r = av2_expect(path, "gamma\nbeta\n");
        CHECK_RESULT(r, "failed");
    }
    (void)Directory_DeleteRecursive("/av5t");
}
#else
static void av5_text_tmp(void) { }
#endif

/* ------------------------------------------------------------------ */
/* CRIT-04 / CRIT-05: the NOR flash driver                             */
/* ------------------------------------------------------------------ */
/* A chip that can hang in an erase, forgets its address mode at a reset
 * and decodes addresses the way the real one does: in 3-byte mode it
 * takes the first three of the bytes it is sent. */
#define AV5Q_SIZE   (64u * 1024u)
#define AV5Q_SECTOR 4096u

static uint8_t  av5q_mem[AV5Q_SIZE];
static uint8_t  av5q_wel;
static uint8_t  av5q_mode4;        /* chip is in 4-byte address mode   */
static uint8_t  av5q_busy;         /* stuck until a reset              */
static uint32_t av5q_hang_erases;  /* that many of the next erases hang */
static uint32_t av5q_resets;
static uint32_t av5q_misaddressed; /* commands decoded at a wrong place */
static uint32_t av5q_erases;
static uint32_t av5q_programs;

static uint32_t av5q_addr(uint32_t addr, uint8_t ab)
{
    if (ab == 4u && !av5q_mode4)
    {
        av5q_misaddressed++;
        return addr >> 8;
    }
    return addr;
}

static FM_Result av5q_cmd(void *ctx, uint8_t op)
{
    (void)ctx;
    if (op == 0x06u) { if (!av5q_busy) av5q_wel = 1u; }
    else if (op == 0xB7u) { if (!av5q_busy) av5q_mode4 = 1u; }
    else if (op == 0x99u)
    {
        av5q_busy = 0u;
        av5q_wel = 0u;
        av5q_mode4 = 0u;        /* volatile: back to the power-up state */
        av5q_resets++;
    }
    return FM_OK;
}

static FM_Result av5q_cmd_addr(void *ctx, uint8_t op, uint32_t addr,
                               uint8_t ab)
{
    (void)ctx;
    if (op != 0x20u || av5q_busy || !av5q_wel) return FM_OK;
    addr = av5q_addr(addr, ab);
    addr -= addr % AV5Q_SECTOR;
    av5q_wel = 0u;
    av5q_erases++;
    if (addr + AV5Q_SECTOR > AV5Q_SIZE) return FM_OK;
    if (av5q_hang_erases > 0u)
    {
        /* half done, then stuck */
        av5q_hang_erases--;
        memset(av5q_mem + addr, 0xFF, AV5Q_SECTOR / 2u);
        av5q_busy = 1u;
        return FM_OK;
    }
    memset(av5q_mem + addr, 0xFF, AV5Q_SECTOR);
    return FM_OK;
}

static FM_Result av5q_read(void *ctx, uint8_t op, uint32_t addr, uint8_t ab,
                           uint8_t dummy, void *buf, uint32_t len)
{
    (void)ctx; (void)dummy;
    if (op == 0x05u)
    {
        *(uint8_t *)buf = (uint8_t)((av5q_busy ? 1u : 0u) |
                                    (av5q_wel ? 2u : 0u));
        return FM_OK;
    }
    if (op == 0x9Fu) return FM_OK;
    addr = av5q_addr(addr, ab);
    if (addr + len > AV5Q_SIZE) { memset(buf, 0xFF, len); return FM_OK; }
    memcpy(buf, av5q_mem + addr, len);
    return FM_OK;
}

static FM_Result av5q_write(void *ctx, uint8_t op, uint32_t addr, uint8_t ab,
                            const void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t i;

    (void)ctx; (void)op;
    if (av5q_busy || !av5q_wel) return FM_OK;
    addr = av5q_addr(addr, ab);
    av5q_wel = 0u;
    av5q_programs++;
    if (addr + len > AV5Q_SIZE) return FM_OK;
    for (i = 0; i < len; i++) av5q_mem[addr + i] &= src[i];
    return FM_OK;
}

static FM_Result av5q_open(FM_QspiDevice *d, FM_QspiChip chip, uint8_t *sbuf,
                           uint32_t sbuf_size)
{
    FM_QspiIo io;
    FM_Result r;

    memset(&io, 0, sizeof(io));
    io.cmd = av5q_cmd;
    io.cmd_addr = av5q_cmd_addr;
    io.read = av5q_read;
    io.write = av5q_write;
    memset(av5q_mem, 0xFF, sizeof(av5q_mem));
    av5q_wel = 0u; av5q_mode4 = 0u; av5q_busy = 0u;
    av5q_hang_erases = 0u; av5q_resets = 0u; av5q_misaddressed = 0u;
    av5q_erases = 0u; av5q_programs = 0u;
    r = FM_Qspi_Init(d, &io, chip, sbuf, sbuf_size);
    if (r == FM_OK) d->info.size_bytes = AV5Q_SIZE;
    return r;
}

/* write a sector of `a`, let the next erase hang, write a sector of `b`
 * over it: the sector has to read back as `b` */
static FM_Result av5q_hung_erase(FM_QspiChip chip)
{
    static uint8_t sbuf[AV5Q_SECTOR];
    static uint8_t a[AV5Q_SECTOR];
    static uint8_t b[AV5Q_SECTOR];
    static uint8_t back[AV5Q_SECTOR];
    FM_QspiDevice d;
    FM_Result r;
    uint32_t i;

    for (i = 0; i < AV5Q_SECTOR; i++)
    {
        a[i] = (uint8_t)(i * 7u + 1u);
        b[i] = (uint8_t)(i * 13u + 5u);
    }
    r = av5q_open(&d, chip, sbuf, sizeof(sbuf));
    if (r == FM_OK) r = FM_Qspi_Write(&d, 3u * AV5Q_SECTOR, a, AV5Q_SECTOR);
    if (r == FM_OK) r = FM_Qspi_Write(&d, 2u * AV5Q_SECTOR, a, AV5Q_SECTOR);
    if (r != FM_OK) return r;

    av5q_hang_erases = 1u;
    r = FM_Qspi_Write(&d, 3u * AV5Q_SECTOR, b, AV5Q_SECTOR);
    if (av5q_resets == 0u)
    {
        printf("[the erase did not hang: the test tests nothing] ");
        return FM_ERR_INTERNAL;
    }
    if (r != FM_OK)
    {
        printf("[write after the hang: %d] ", (int)r);
        return r;
    }
    if (av5q_misaddressed != 0u)
    {
        printf("[%u commands sent with 4 address bytes to a chip in "
               "3-byte mode] ", (unsigned)av5q_misaddressed);
        return FM_ERR_INTERNAL;
    }
    r = FM_Qspi_Read(&d, 3u * AV5Q_SECTOR, back, AV5Q_SECTOR);
    if (r == FM_OK && memcmp(back, b, AV5Q_SECTOR) != 0)
    {
        printf("[FM_OK, but the sector does not hold what was written] ");
        r = FM_ERR_CRC;
    }
    /* and the neighbour was left alone */
    if (r == FM_OK) r = FM_Qspi_Read(&d, 2u * AV5Q_SECTOR, back, AV5Q_SECTOR);
    if (r == FM_OK && memcmp(back, a, AV5Q_SECTOR) != 0)
    {
        printf("[the sector next to it changed] ");
        r = FM_ERR_CRC;
    }
    return r;
}

static void av5_qspi(void)
{
    TEST("CRIT-04: an erase that ended in a reset is done again, not reported as done");
    { FM_Result av5_r = av5q_hung_erase(FM_QSPI_CHIP_W25Q128); CHECK_RESULT(av5_r, "failed"); }

    TEST("CRIT-04: a chip above 16 MB is back in 4-byte address mode after that reset");
    { FM_Result av5_r = av5q_hung_erase(FM_QSPI_CHIP_W25Q256); CHECK_RESULT(av5_r, "failed"); }

    /* CRIT-05: every write that was not a whole sector read the sector,
     * erased it and wrote it back -- 16 erases to fill one erased
     * sector page by page, each with the earlier pages in RAM only. */
    TEST("CRIT-05: filling an erased sector page by page erases nothing");
    {
        static uint8_t sbuf[AV5Q_SECTOR];
        static uint8_t page[256];
        static uint8_t back[256];
        FM_QspiDevice d;
        FM_Result r;
        uint32_t i;
        uint32_t erases;
        int bad = 0;

        r = av5q_open(&d, FM_QSPI_CHIP_W25Q128, sbuf, sizeof(sbuf));
        if (r == FM_OK) r = FM_Qspi_Erase(&d, AV5Q_SECTOR, AV5Q_SECTOR);
        erases = av5q_erases;
        for (i = 0; r == FM_OK && i < 16u; i++)
        {
            memset(page, (int)(0x10u + i), sizeof(page));
            r = FM_Qspi_Write(&d, AV5Q_SECTOR + i * 256u, page, 256u);
        }
        for (i = 0; r == FM_OK && i < 16u; i++)
        {
            memset(page, (int)(0x10u + i), sizeof(page));
            r = FM_Qspi_Read(&d, AV5Q_SECTOR + i * 256u, back, 256u);
            if (r == FM_OK && memcmp(page, back, 256u) != 0) bad = 1;
        }
        if (r != FM_OK) { CHECK_RESULT(r, "failed"); }
        else if (bad) { FAIL("the pages do not read back"); }
        else if (av5q_erases != erases)
        {
            printf("[%u erases] ", (unsigned)(av5q_erases - erases));
            FAIL("erased a sector to write into erased flash");
        }
        else { PASS(); }
    }

    TEST("CRIT-05: the same without a sector buffer (was FM_ERR_UNSUPPORTED)");
    {
        static uint8_t page[256];
        static uint8_t back[256];
        FM_QspiDevice d;
        FM_Result r;

        memset(page, 0x3C, sizeof(page));
        r = av5q_open(&d, FM_QSPI_CHIP_W25Q128, NULL, 0u);
        if (r == FM_OK) r = FM_Qspi_Write(&d, 512u, page, 256u);
        if (r == FM_OK) r = FM_Qspi_Read(&d, 512u, back, 256u);
        if (r == FM_OK && memcmp(page, back, 256u) != 0) r = FM_ERR_CRC;
        CHECK_RESULT(r, "failed");
    }

    TEST("CRIT-05: a write over other data still goes through erase, the rest of the sector survives");
    {
        static uint8_t sbuf[AV5Q_SECTOR];
        static uint8_t first[AV5Q_SECTOR];
        static uint8_t back[AV5Q_SECTOR];
        static const uint8_t patch[5] = { 0xFF, 0x00, 0xA5, 0x5A, 0xFF };
        FM_QspiDevice d;
        FM_Result r;
        uint32_t i;
        uint32_t erases = 0u;
        uint32_t programs = 0u;

        for (i = 0; i < AV5Q_SECTOR; i++) first[i] = (uint8_t)(i ^ (i >> 8));
        r = av5q_open(&d, FM_QSPI_CHIP_W25Q128, sbuf, sizeof(sbuf));
        if (r == FM_OK) r = FM_Qspi_Write(&d, 0u, first, AV5Q_SECTOR);
        erases = av5q_erases;
        if (r == FM_OK) r = FM_Qspi_Write(&d, 1000u, patch, sizeof(patch));
        if (r == FM_OK && av5q_erases == erases)
        {
            printf("[no erase: bits cannot go from 0 to 1 without one] ");
            r = FM_ERR_INTERNAL;
        }
        memcpy(first + 1000, patch, sizeof(patch));
        if (r == FM_OK) r = FM_Qspi_Read(&d, 0u, back, AV5Q_SECTOR);
        if (r == FM_OK && memcmp(first, back, AV5Q_SECTOR) != 0) r = FM_ERR_CRC;
        /* writing what is already there costs nothing */
        erases = av5q_erases;
        programs = av5q_programs;
        if (r == FM_OK) r = FM_Qspi_Write(&d, 0u, first, AV5Q_SECTOR);
        if (r == FM_OK && (av5q_erases != erases || av5q_programs != programs))
        {
            printf("[rewrote identical data: %u erases, %u programs] ",
                   (unsigned)(av5q_erases - erases),
                   (unsigned)(av5q_programs - programs));
            r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "failed");
    }
}

/* ------------------------------------------------------------------ */
/* HIGH-03 / HIGH-04 / HIGH-05 / MED-05                                */
/* ------------------------------------------------------------------ */
#if FM_USE_FMFS || FM_USE_LITTLEFS
/* HIGH-03: Storage_Format() erased the medium under a mounted file
 * system with a file open on it, and said FM_OK. */
static FM_Result av5_storage_busy(int kind)
{
    FM_Result r = av2_mount(kind, &(const char *){ NULL });
    FM_Result rf;
    FM_Result ru;
    FM_File *f = NULL;

    if (r == FM_OK) r = File_WriteText("/fca/live.txt", "live data");
    if (r == FM_OK)
    {
        f = File_Open("/fca/open.bin", FM_OPEN_WRITE | FM_OPEN_CREATE);
        if (f == NULL) r = File_GetLastError();
    }
    if (r != FM_OK) { av2_unmount(kind); return r; }

    rf = Storage_Format(g_fc_name[0]);
    ru = Storage_Unmount(g_fc_name[0]);
    if (rf != FM_ERR_BUSY || ru != FM_ERR_BUSY)
    {
        printf("[format %d, unmount %d under a live mount] ", (int)rf, (int)ru);
        r = FM_ERR_INTERNAL;
    }
    (void)File_Close(f);
    if (r == FM_OK) r = av2_expect("/fca/live.txt", "live data");
    /* without the mount both are allowed again */
    (void)FS_Unmount(g_fc_mount[0]);
    if (r == FM_OK) r = Storage_Mount(g_fc_name[0]);
    if (r == FM_OK) r = Storage_Format(g_fc_name[0]);
    if (r == FM_OK) r = Storage_Unmount(g_fc_name[0]);
    av2_unmount(kind);
    return r;
}

/* HIGH-05: Directory_Rename handed its two names to the backend as they
 * came. */
static FM_Result av5_dir_rename(int kind)
{
    const char *root = "";
    char a[64], b[64], c[64], f1[64], f2[64];
    FM_Result r = av2_mount(kind, &root);
    FM_Result got;
    FM_File *f = NULL;
    FM_Bool there = FM_FALSE;

    av2_path(a, sizeof(a), root, "av5d");
    av2_path(b, sizeof(b), root, "av5d/child");
    av2_path(c, sizeof(c), root, "av5e");
    av2_path(f1, sizeof(f1), root, "av5d/in.txt");
    av2_path(f2, sizeof(f2), root, "av5f.txt");
    /* (whatever an earlier, failed run left under these names) */
    (void)Directory_DeleteRecursive(a);
    (void)Directory_DeleteRecursive(c);
    (void)File_Delete(a);
    (void)File_Delete(c);
    if (r == FM_OK) r = Directory_Create(a);
    if (r == FM_OK) r = File_WriteText(f1, "inside");
    if (r == FM_OK) r = File_WriteText(f2, "plain");
    if (r != FM_OK) { av2_unmount(kind); return r; }

    /* a plain file is not a directory */
    got = Directory_Rename(f2, c);
    if (got != FM_ERR_NOT_DIRECTORY)
    {
        printf("[%s: a file: %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    /* into itself */
    got = Directory_Rename(a, b);
    if (got != FM_ERR_INVALID_ARGUMENT)
    {
        printf("[%s: into itself: %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    /* with a file open inside: busy, for Rename and for Move */
    f = File_Open(f1, FM_OPEN_READ);
    if (f == NULL) r = File_GetLastError();
    got = Directory_Rename(a, c);
    if (got != FM_ERR_BUSY)
    {
        printf("[%s: file open inside: %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    got = Directory_Move(a, c, NULL, NULL);
    if (got != FM_ERR_BUSY)
    {
        printf("[%s: Directory_Move, file open inside: %d] ",
               av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    if (f != NULL) (void)File_Close(f);
    /* nothing above changed anything */
    if (r == FM_OK) r = av2_expect(f1, "inside");
    if (r == FM_OK) r = av2_expect(f2, "plain");
    /* and the rename that is allowed works */
    if (r == FM_OK) r = Directory_Rename(a, c);
    if (r == FM_OK)
    {
        av2_path(f1, sizeof(f1), root, "av5e/in.txt");
        r = av2_expect(f1, "inside");
    }
    if (r == FM_OK) r = Directory_Exists(a, &there);
    if (r == FM_OK && there) r = FM_ERR_EXISTS;
    (void)Directory_DeleteRecursive(c);
    (void)Directory_DeleteRecursive(a);
    (void)File_Delete(f2);
    av2_unmount(kind);
    return r;
}

static void av5_mounts(void)
{
    int kind;

    for (kind = 0; kind < 2; kind++)
    {
        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "HIGH-03: FMFS, Storage_Format / Storage_Unmount under a mount are FM_ERR_BUSY"
                       : "HIGH-03: LittleFS, Storage_Format / Storage_Unmount under a mount are FM_ERR_BUSY");
        { FM_Result av5_r = av5_storage_busy(kind); CHECK_RESULT(av5_r, "failed"); }
    }
    for (kind = 0; kind < 3; kind++)
    {
        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "HIGH-05: FMFS, Directory_Rename checks what it renames"
             : kind == 1 ? "HIGH-05: LittleFS, Directory_Rename checks what it renames"
                         : "HIGH-05: POSIX, Directory_Rename checks what it renames");
        { FM_Result av5_r = av5_dir_rename(kind); CHECK_RESULT(av5_r, "failed"); }
    }

#if FM_USE_LITTLEFS
    /* HIGH-04: lfs_format() on the mounted lfs_t: every call after it
     * failed with FM_ERR_CORRUPTED. */
    TEST("HIGH-04: LittleFS, the volume works after FS_Format");
    {
        FM_Bool there = FM_TRUE;
        FM_Result r = av2_mount(1, &(const char *){ NULL });

        if (r == FM_OK) r = File_WriteText("/fca/old.txt", "old");
        if (r == FM_OK) r = FS_Format(g_fc_mount[0]);
        if (r == FM_OK) r = File_Exists("/fca/old.txt", &there);
        if (r == FM_OK && there) r = FM_ERR_EXISTS;
        if (r == FM_OK) r = Directory_Create("/fca/newdir");
        if (r == FM_OK) r = File_WriteText("/fca/newdir/new.txt", "new");
        if (r == FM_OK) r = av2_expect("/fca/newdir/new.txt", "new");
        /* and what was written is on the medium: mount it again */
        (void)FS_Unmount(g_fc_mount[0]);
        if (r == FM_OK) r = fc_init(1, &g_av2_fs);
        if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
        if (r == FM_OK) r = av2_expect("/fca/newdir/new.txt", "new");
        av2_unmount(1);
        CHECK_RESULT(r, "failed");
    }

    TEST("HIGH-04: LittleFS_FormatStorage formats a medium that auto-format off refuses to mount");
    {
        FM_Result r = fc_storage(0, 1, 1);      /* blank */
        FM_Result rm = FM_OK;

        if (r == FM_OK) r = fc_init(1, &g_av2_fs);
        if (r == FM_OK)
        {
            LittleFS_SetAutoFormat(&g_av2_fs, FM_FALSE);
            rm = FS_Mount(g_fc_mount[0], &g_av2_fs);
            if (rm == FM_OK)
            {
                printf("[a blank medium mounted with auto-format off] ");
                r = FM_ERR_INTERNAL;
            }
        }
        if (r == FM_OK) r = LittleFS_FormatStorage(&g_fc_st[0]);
        /* the same FM_FS, not initialised again: the format has not
         * taken its context away */
        if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
        if (r == FM_OK) r = File_WriteText("/fca/f.txt", "formatted");
        if (r == FM_OK) r = av2_expect("/fca/f.txt", "formatted");
        /* a mounted storage is not formatted behind the mount's back */
        if (r == FM_OK &&
            LittleFS_FormatStorage(&g_fc_st[0]) != FM_ERR_BUSY)
        {
            printf("[formatted a mounted storage] ");
            r = FM_ERR_INTERNAL;
        }
        /* ...and not its setting either: wiped again, it does not mount */
        if (r == FM_OK) r = FS_Unmount(g_fc_mount[0]);
        if (r == FM_OK)
        {
            memset(g_fc_mem[0], 0xFF, sizeof(g_fc_mem[0]));
            if (FS_Mount(g_fc_mount[0], &g_av2_fs) == FM_OK)
            {
                printf("[auto-format came back on] ");
                r = FM_ERR_INTERNAL;
            }
        }
        fc_drop(0);
        CHECK_RESULT(r, "failed");
    }

    TEST("HIGH-04: LittleFS_FormatStorage takes no context away from a prepared FM_FS");
    {
        static FM_FS held[FM_MAX_FS];
        FM_Result r = fc_storage(0, 1, 1);
        uint32_t k;

        /* every context handed out, none mounted */
        for (k = 0; r == FM_OK && k < (uint32_t)FM_MAX_FS; k++)
        {
            r = LittleFS_Init(&held[k]);
        }
        if (r == FM_OK) r = LittleFS_FormatStorage(&g_fc_st[0]);
        /* whichever context the format borrowed, its FM_FS still has it:
         * every one of them mounts */
        for (k = 0; r == FM_OK && k < (uint32_t)FM_MAX_FS; k++)
        {
            r = FS_Mount(g_fc_mount[0], &held[k]);
            if (r != FM_OK)
            {
                printf("[FM_FS %u: mount %d] ", (unsigned)k, (int)r);
                break;
            }
            r = File_WriteText("/fca/g.txt", "held");
            if (r == FM_OK) r = av2_expect("/fca/g.txt", "held");
            if (r == FM_OK) r = FS_Unmount(g_fc_mount[0]);
        }
        fc_drop(0);
        CHECK_RESULT(r, "failed");
    }

    /* MED-05: FS_Check answered FM_OK for any mounted volume. */
    TEST("MED-05: LittleFS, FS_Check notices a medium that was wiped under the mount");
    {
        FM_Result r = av2_mount(1, &(const char *){ NULL });
        FM_Result good = FM_ERR_INTERNAL;
        FM_Result bad = FM_OK;
        int i;

        if (r == FM_OK) r = Directory_Create("/fca/d1");
        for (i = 0; r == FM_OK && i < 6; i++)
        {
            char name[32];

            snprintf(name, sizeof(name), "/fca/d1/f%d.txt", i);
            r = File_WriteText(name, "some content that is not inline-small"
                                     " some content that is not inline-small");
        }
        if (r == FM_OK) good = FS_Check(g_fc_mount[0]);
        if (r == FM_OK)
        {
            memset(g_fc_mem[0], 0xAA, sizeof(g_fc_mem[0]));
            bad = FS_Check(g_fc_mount[0]);
        }
        av2_unmount(1);
        if (r != FM_OK) { CHECK_RESULT(r, "set-up"); }
        else if (good != FM_OK) { CHECK_RESULT(good, "a healthy volume fails the check"); }
        else if (bad == FM_OK) { FAIL("FM_OK for a medium that holds nothing but 0xAA"); }
        else { PASS(); }
    }
#endif
}
#else
static void av5_mounts(void) { }
#endif

/* ------------------------------------------------------------------ */
/* HIGH-06 / MED-04 / MED-06 / MED-07                                  */
/* ------------------------------------------------------------------ */
static float av5_float_bits(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

#if FM_USE_JSON
/* what JsonWriter_Float writes for one value, inside "[...]" */
static const char *av5_json_float(float v, int decimals)
{
    static char out[64];
    FM_JsonWriter w;
    FM_Result r = JsonWriter_Init(&w, out, sizeof(out));

    if (r == FM_OK) r = JsonWriter_BeginArray(&w);
    if (r == FM_OK) r = JsonWriter_Float(&w, v, decimals);
    if (r == FM_OK) r = JsonWriter_EndArray(&w);
    if (r != FM_OK) (void)snprintf(out, sizeof(out), "error %d", (int)r);
    return out;
}

static int av5_json_is(float v, int decimals, const char *want)
{
    const char *got = av5_json_float(v, decimals);

    if (strcmp(got, want) == 0) return 1;
    printf("[got %s, expected %s] ", got, want);
    return 0;
}
#endif

/* MED-04: one name of FM_NAME_MAX characters could be created; after it
 * the folder could not be listed, copied or deleted. */
static FM_Result av5_long_name(int kind)
{
    const char *root = "";
    char dir[64], sub[FM_PATH_MAX], path[FM_PATH_MAX], other[FM_PATH_MAX];
    char name[FM_NAME_MAX + 1];
    FM_Result r = av2_mount(kind, &root);
    FM_Result got;
    FM_File *f;
    uint32_t count = 0u;

    av2_path(dir, sizeof(dir), root, "av5n");
    (void)Directory_DeleteRecursive(dir);
    if (r == FM_OK) r = Directory_Create(dir);
    if (r != FM_OK) { av2_unmount(kind); return r; }

    /* FM_NAME_MAX characters: one too many */
    memset(name, 'n', FM_NAME_MAX);
    name[FM_NAME_MAX] = '\0';
    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    (void)snprintf(sub, sizeof(sub), "%s/%s/x", dir, name);
    (void)snprintf(other, sizeof(other), "%s/ok.txt", dir);

    got = File_WriteText(path, "x");
    if (got != FM_ERR_TOO_LARGE)
    {
        printf("[%s: File_WriteText %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    f = File_Open(path, FM_OPEN_WRITE | FM_OPEN_CREATE);
    if (f != NULL)
    {
        (void)File_Close(f);
        printf("[%s: File_Open created it] ", av2_kind_name(kind));
        r = FM_ERR_INTERNAL;
    }
    got = Directory_Create(path);
    if (got != FM_ERR_TOO_LARGE)
    {
        printf("[%s: Directory_Create %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    got = Directory_Create(sub);        /* as a parent to be made */
    if (got != FM_ERR_TOO_LARGE)
    {
        printf("[%s: Directory_Create below it %d] ", av2_kind_name(kind),
               (int)got);
        r = FM_ERR_INTERNAL;
    }
    if (r == FM_OK) r = File_WriteText(other, "fine");
    got = File_Rename(other, path);
    if (got != FM_ERR_TOO_LARGE)
    {
        printf("[%s: File_Rename %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    got = File_Copy(other, path, NULL, NULL);
    if (got != FM_ERR_TOO_LARGE)
    {
        printf("[%s: File_Copy %d] ", av2_kind_name(kind), (int)got);
        r = FM_ERR_INTERNAL;
    }
    /* the longest name that is allowed works, and the folder can still
     * be measured and deleted */
    name[FM_NAME_MAX - 1] = '\0';
    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (r == FM_OK) r = File_WriteText(path, "longest");
    if (r == FM_OK) r = av2_expect(path, "longest");
    if (r == FM_OK) r = av2_expect(other, "fine");
    if (r == FM_OK) r = Directory_Count(dir, FM_ITEM_FILE, &count);
    if (r == FM_OK && count != 2u)
    {
        printf("[%s: %lu files in the folder] ", av2_kind_name(kind),
               (unsigned long)count);
        r = FM_ERR_INTERNAL;
    }
    if (r == FM_OK) r = Directory_DeleteRecursive(dir);
    (void)Directory_DeleteRecursive(dir);
    av2_unmount(kind);
    return r;
}

static void av5_utils(void)
{
    int kind;

#if FM_USE_JSON
    TEST("HIGH-06: JsonWriter_Float writes null for NaN and infinity");
    if (av5_json_is(av5_float_bits(0x7FC00000u), 3, "[null]") &
        av5_json_is(av5_float_bits(0xFFC00000u), 3, "[null]") &
        av5_json_is(av5_float_bits(0x7F800000u), 2, "[null]") &
        av5_json_is(av5_float_bits(0xFF800000u), 0, "[null]"))
    {
        PASS();
    }
    else
    {
        FAIL("a value that is not a number was written as a number");
    }

    TEST("HIGH-06: JsonWriter_Float writes values from 2^32 up as they are");
    if (av5_json_is(4294967296.0f, 0, "[4294967296]") &
        av5_json_is(5000000000.0f, 1, "[5000000000.0]") &
        av5_json_is(-8589934592.0f, 0, "[-8589934592]") &
        av5_json_is(3.0e12f, 0, "[8999999488]") &
        av5_json_is(1234.5f, 2, "[1234.50]") &
        av5_json_is(-0.25f, 3, "[-0.250]") &
        av5_json_is(0.0f, 0, "[0]"))
    {
        PASS();
    }
    else
    {
        FAIL("wrong text");
    }

    TEST("MED-06: a JSON index too large for size_t names no element");
    {
        static const char *text = "{\"a\":[10,20,30]}";
        FM_Json doc;
        uint32_t tokens[32 * FM_JSON_TOKEN_SIZE / 4];
        int32_t v = -1;
        FM_Result r = Json_OpenBuffer(text, strlen(text), &doc, tokens,
                                      sizeof(tokens));
        FM_Result big = FM_OK;
        FM_Result huge = FM_OK;

        if (r == FM_OK) r = Json_ReadInt(&doc, "a[1]", &v);
        if (r == FM_OK && v != 20) r = FM_ERR_INTERNAL;
        if (r == FM_OK)
        {
            /* 2^64 and 2^32: both were index 0 on a 32-bit target, the
             * first one on a 64-bit host too */
            v = -1;
            big = Json_ReadInt(&doc, "a[18446744073709551616]", &v);
            huge = Json_ReadInt(&doc, "a[4294967296]", &v);
        }
        if (r != FM_OK) { CHECK_RESULT(r, "set-up"); }
        else if (big != FM_ERR_NOT_FOUND || huge != FM_ERR_NOT_FOUND || v != -1)
        {
            printf("[2^64: %d, 2^32: %d, value %ld] ", (int)big, (int)huge,
                   (long)v);
            FAIL("the index wrapped around to an element");
        }
        else { PASS(); }
    }
#endif

#if FM_USE_DATA_LOGGER
    TEST("HIGH-06: Logger_Write does not log NaN / infinity as measurements");
    {
        static char text[512];
        FM_Logger lg;
        FM_LoggerConfig cfg;
        float vals[4];
        size_t rd = 0;
        int fmt;
        FM_Result r = FM_OK;

        vals[0] = 1.5f;
        vals[1] = av5_float_bits(0x7FC00000u);
        vals[2] = av5_float_bits(0x7F800000u);
        vals[3] = av5_float_bits(0xFF800000u);
        for (fmt = 0; r == FM_OK && fmt < 2; fmt++)
        {
            const char *want = fmt == 0 ? "7,0,1.500,nan,inf,-inf\n"
                : "{\"ts\":7,\"seq\":0,\"v\":[1.500,null,null,null]}\n";
            FM_LoggerInfo info;

            memset(&cfg, 0, sizeof(cfg));
            (void)Directory_DeleteRecursive("/av5log");
            r = Logger_Init(&lg, "/av5log", "nan",
                            fmt == 0 ? FM_LOG_FORMAT_CSV : FM_LOG_FORMAT_JSONL,
                            &cfg);
            if (r == FM_OK) r = Logger_Start(&lg);
            if (r == FM_OK) r = Logger_Write(&lg, 7u, vals, 4u);
            if (r == FM_OK) r = Logger_GetInfo(&lg, &info);
            if (r == FM_OK) r = Logger_Stop(&lg);
            memset(text, 0, sizeof(text));
            if (r == FM_OK)
                r = File_ReadText(info.file_name, text, sizeof(text), &rd);
            if (r == FM_OK && strstr(text, want) == NULL)
            {
                printf("[%s: got \"%s\"] ", fmt == 0 ? "CSV" : "JSONL", text);
                r = FM_ERR_INVALID_FORMAT;
            }
        }
        (void)Directory_DeleteRecursive("/av5log");
        CHECK_RESULT(r, "failed");
    }
#endif

    TEST("MED-07: Path_Join may write its result over its second argument");
    {
        char buf[64];
        FM_Result r;
        int ok = 1;

        (void)strcpy(buf, "file.txt");
        r = Path_Join("/dir", buf, buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "/dir/file.txt") != 0)
        {
            printf("[out == b: %d \"%s\"] ", (int)r, buf);
            ok = 0;
        }
        (void)strcpy(buf, "/sub/file.txt");
        r = Path_Join("/a/b/", buf, buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "/a/b/sub/file.txt") != 0)
        {
            printf("[out == b, leading '/': %d \"%s\"] ", (int)r, buf);
            ok = 0;
        }
        (void)strcpy(buf, "/dir");
        r = Path_Join(buf, "file.txt", buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "/dir/file.txt") != 0)
        {
            printf("[out == a: %d \"%s\"] ", (int)r, buf);
            ok = 0;
        }
        (void)strcpy(buf, "x");
        r = Path_Join(buf, buf, buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "x/x") != 0)
        {
            printf("[out == a == b: %d \"%s\"] ", (int)r, buf);
            ok = 0;
        }
        (void)strcpy(buf, "abcdef");
        r = Path_Join(buf + 2, buf, buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "cdef/abcdef") != 0) ok = 0;
        (void)strcpy(buf, "abc");
        (void)strcpy(buf + 4, "wxyz");
        r = Path_Join(buf + 4, buf, buf, sizeof(buf));
        if (r != FM_OK || strcmp(buf, "wxyz/abc") != 0) ok = 0;
        {
            char big[FM_PATH_MAX * 4u + 4u];
            size_t n = FM_PATH_MAX * 2u;
            memset(big, 'q', n);
            big[n] = '\0';
            r = Path_Join(big + FM_PATH_MAX, big, big, sizeof(big));
            if (r != FM_ERR_TOO_LARGE || strlen(big) != n) ok = 0;
            for (size_t k = 0; k < n; ++k) if (big[k] != 'q') ok = 0;
        }
        if (ok) { PASS(); } else { FAIL("wrong result"); }
    }

    TEST("LOW-02: FileManager_Init refuses an application built with other options");
    {
        FM_Result same = FileManager_Init();    /* the macro: our sizes */
        FM_Result plain = (FileManager_Init)();
        FM_Result other = FileManager_InitChecked(
            sizeof(FM_File) + sizeof(void *), sizeof(FM_Dir),
            sizeof(FM_Storage), sizeof(FM_FS), sizeof(FM_DirEntry));
        FM_Result name = FileManager_InitChecked(
            sizeof(FM_File), sizeof(FM_Dir), sizeof(FM_Storage),
            sizeof(FM_FS), sizeof(FM_DirEntry) + 192u);

        if (same != FM_OK || plain != FM_OK || other != FM_ERR_INTERNAL ||
            name != FM_ERR_INTERNAL)
        {
            printf("[same %d, plain %d, other FM_File %d, other FM_DirEntry %d] ",
                   (int)same, (int)plain, (int)other, (int)name);
            FAIL("wrong answer");
        }
        else { PASS(); }
    }

    TEST("stack: Path_Normalize takes the path with the most components there can be");
    {
        /* "a/a/.../a": FM_PATH_MAX / 2 components in FM_PATH_MAX - 1
         * characters -- the list of component starts is exactly full */
        char p[FM_PATH_MAX];
        char want[FM_PATH_MAX];
        char dots[FM_PATH_MAX];
        size_t i;
        FM_Result r1;
        FM_Result r2;
        FM_Result r3;

        for (i = 0; i + 1u < sizeof(p); i++) p[i] = (i & 1u) ? '/' : 'a';
        p[sizeof(p) - 1u] = '\0';
        memcpy(want, p, sizeof(p));
        r1 = Path_Normalize(p, sizeof(p));
        /* relative, all "..": every one is kept */
        for (i = 0; i + 2u < sizeof(dots); i += 3u)
        {
            dots[i] = '.'; dots[i + 1u] = '.'; dots[i + 2u] = '/';
        }
        dots[i - 1u] = '\0';
        {
            char dwant[FM_PATH_MAX];

            memcpy(dwant, dots, sizeof(dots));
            r2 = Path_Normalize(dots, sizeof(dots));
            if (r2 == FM_OK && strcmp(dots, dwant) != 0) r2 = FM_ERR_CRC;
        }
        /* and one that folds away completely */
        {
            char fold[FM_PATH_MAX];

            fold[0] = '/';
            for (i = 1; i + 5u < sizeof(fold); i += 5u)
            {
                memcpy(fold + i, "b/../", 5);
            }
            fold[i] = 'z';
            fold[i + 1u] = '\0';
            r3 = Path_Normalize(fold, sizeof(fold));
            if (r3 == FM_OK && strcmp(fold, "/z") != 0) r3 = FM_ERR_CRC;
        }
        if (r1 != FM_OK || strcmp(p, want) != 0 || r2 != FM_OK || r3 != FM_OK)
        {
            printf("[a/a/..: %d \"%.20s\"; ../..: %d; b/../: %d] ", (int)r1, p,
                   (int)r2, (int)r3);
            FAIL("wrong");
        }
        else { PASS(); }
    }

    TEST("LOW-04: get_type with a buffer too small is FM_ERR_TOO_LARGE on every file system");
    {
        int bad = 0;

        for (kind = 0; kind < 2; kind++)
        {
            static FM_FS tfs;
            char small[3];
            char big[16];
            FM_Result a;
            FM_Result b;
            FM_Result c;

            if (!av2_kind_built(kind)) continue;
            memset(&tfs, 0, sizeof(tfs));
#if FM_USE_LITTLEFS
            a = (kind == 0) ? FMFS_Init(&tfs) : LittleFS_Init(&tfs);
#else
            a = FMFS_Init(&tfs);
#endif
            if (a != FM_OK || tfs.ops.get_type == NULL) { bad = 1; continue; }
            memset(small, 'x', sizeof(small));
            a = tfs.ops.get_type(&tfs, small, sizeof(small));
            b = tfs.ops.get_type(&tfs, NULL, 16u);
            c = tfs.ops.get_type(&tfs, big, sizeof(big));
            if (a != FM_ERR_TOO_LARGE || b != FM_ERR_INVALID_ARGUMENT ||
                c != FM_OK || small[0] != 'x' ||
                strcmp(big, kind == 0 ? "FMFS" : "LITTLEFS") != 0)
            {
                printf("[kind %d: small %d, NULL %d, fits %d] ", kind,
                       (int)a, (int)b, (int)c);
                bad = 1;
            }
            /* (the context this took was never mounted: the next
             * *_Init that finds none free takes it over) */
        }
        if (bad) { FAIL("wrong code"); } else { PASS(); }
    }

    TEST("MED-01: File_OpenEx on a handle that is open is FM_ERR_BUSY, and the handle still works");
    {
        const char *root = "";
        FM_Result r = av2_mount(0, &root);
        char path[64];
        static FM_File h;
        char back[8];
        uint32_t rd = 0;

        memset(&h, 0, sizeof(h));
        memset(back, 0, sizeof(back));
        av2_path(path, sizeof(path), root, "m1.txt");
        if (r == FM_OK) r = File_WriteText(path, "abcdef");
        if (r == FM_OK) r = File_OpenEx(&h, path, FM_OPEN_READ);
        if (r == FM_OK)
        {
            if (File_OpenEx(&h, path, FM_OPEN_READ) != FM_ERR_BUSY)
            {
                printf("[second open went through] ");
                r = FM_ERR_INTERNAL;
            }
            if (r == FM_OK) r = File_Read(&h, back, 6u, &rd);
            if (r == FM_OK && (rd != 6u || memcmp(back, "abcdef", 6) != 0))
                r = FM_ERR_CRC;
            (void)File_Close(&h);
        }
        av2_unmount(0);
        CHECK_RESULT(r, "failed");
    }

    for (kind = 0; kind < 3; kind++)
    {
        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "MED-04: FMFS, a name that cannot be listed is not created"
             : kind == 1 ? "MED-04: LittleFS, a name that cannot be listed is not created"
                         : "MED-04: POSIX, a name that cannot be listed is not created");
        { FM_Result av5_r = av5_long_name(kind); CHECK_RESULT(av5_r, "failed"); }
    }
}

/* ------------------------------------------------------------------ */
/* HIGH-07: FMFS tables that are well formed and make no sense         */
/* ------------------------------------------------------------------ */
#if FM_USE_FMFS
#define AV5F_REC   ((uint32_t)FM_PATH_MAX + 20u)

static uint32_t av5f_crc(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void av5f_put(uint8_t *p, uint64_t v, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8u * i));
}

/* the table of a version 2 volume that is in use (the later one) */
static uint8_t *av5f_table(void)
{
    uint32_t slot = (uint32_t)pf_le(g_pf_mem + 36, 4);
    uint32_t seq_a = (uint32_t)pf_le(g_pf_mem + 32, 4);
    uint32_t seq_b = (uint32_t)pf_le(g_pf_mem + slot + 32u, 4);

    return ((int32_t)(seq_b - seq_a) > 0) ? g_pf_mem + slot : g_pf_mem;
}

/* the record of `name` in a table whose records begin at `recs` */
static uint8_t *av5f_record(uint8_t *recs, const char *name)
{
    uint32_t i;

    for (i = 0; i < (uint32_t)FM_FMFS_MAX_ENTRIES; i++)
    {
        uint8_t *rec = recs + i * AV5F_REC;

        if (rec[0] != 0u && strcmp((const char *)rec + 20, name) == 0)
        {
            return rec;
        }
    }
    return NULL;
}

/* a version 2 table has a checksum: make it fit what is there now */
static void av5f_seal(uint8_t *table)
{
    uint32_t crc = av5f_crc(0u, table + 64, (uint32_t)FM_FMFS_MAX_ENTRIES * AV5F_REC);

    crc = av5f_crc(crc, table, 60u);
    av5f_put(table + 60, crc, 4);
}

/* What a mount says to the volume in g_pf_mem after one change.
 *   0 nothing (the checksum is recomputed: must still mount)
 *   1 one.txt begins at byte 0, on top of the table
 *   2 two.txt begins where one.txt begins
 *   3 two.txt ends behind data_next   (version 1: mounts, see below)
 *   4 one.txt is inside a folder that does not exist  (version 2 only)
 *   5 two.txt has the name of one.txt                 (version 2 only) */
static char g_av5f_two[600];

static FM_Result av5f_mount_changed(int v2, int change)
{
    static uint8_t keep[PF_CAP_MAX];
    uint8_t *table = v2 ? av5f_table() : g_pf_mem;
    uint8_t *recs = table + (v2 ? 64 : 32);
    uint8_t *one = av5f_record(recs, "one.txt");
    uint8_t *two = av5f_record(recs, "two.txt");
    FM_Result r;

    if (one == NULL || two == NULL)
    {
        return FM_ERR_INVALID_FORMAT;       /* the test lost its way */
    }
    memcpy(keep, g_pf_mem, g_pf_cap);
    switch (change)
    {
    case 1: av5f_put(one + 4, 0u, 8); break;
    case 2: memcpy(two + 4, one + 4, 8); break;
    case 3: av5f_put(two + 12, pf_le(table + 16, 8) - pf_le(two + 4, 8) + 1u, 8); break;
    case 4: memset(one + 20, 0, FM_PATH_MAX); (void)strcpy((char *)one + 20, "nodir/one.txt"); break;
    case 5: memset(two + 20, 0, FM_PATH_MAX); (void)strcpy((char *)two + 20, "one.txt"); break;
    /* version 1 only: the header of before two.txt was written (records
     * are saved first, the header last; a reset between the two) */
    case 6: av5f_put(table + 16, pf_le(two + 4, 8), 8); break;
    default: break;
    }
    if (v2) av5f_seal(table);
    r = pf_attach(-1);
    if (r == FM_OK && change == 0)
    {
        r = FS_Check(g_pf_mount);
        if (r == FM_OK) r = av2_expect("/pf/one.txt", "first");
    }
    if (r == FM_OK && change == 6)
    {
        /* the volume is usable: a new file does not land on two.txt
         * (it would not without the fix either -- files are placed by
         * the live entries; what the fix changes is that this image
         * mounts and passes FS_Check at all) */
        static char back[sizeof(g_av5f_two)];
        size_t rd = 0;

        r = File_WriteText("/pf/three.txt", "third file, placed at the mark");
        if (r == FM_OK) r = File_ReadAll("/pf/two.txt", back, sizeof(back), &rd);
        if (r == FM_OK && (rd != sizeof(g_av5f_two) - 1u ||
                           memcmp(back, g_av5f_two, rd) != 0))
        {
            printf("[two.txt: %u bytes, changed] ", (unsigned)rd);
            r = FM_ERR_CRC;
        }
        if (r == FM_OK) r = av2_expect("/pf/three.txt", "third file, placed at the mark");
        if (r == FM_OK) r = FS_Check(g_pf_mount);
    }
    pf_detach();
    memcpy(g_pf_mem, keep, g_pf_cap);
    return r;
}

static void av5_fmfs_table(void)
{
    int v2;

    for (v2 = 0; v2 < 2; v2++)
    {
        char *two = g_av5f_two;
        FM_Result r;
        FM_Result got[7];
        int change;
        int bad = 0;

#if !FM_FMFS_POWER_SAFE
        if (v2) continue;
#endif
        TEST(v2 ? "HIGH-07: FMFS, a version 2 table that makes no sense is FM_ERR_CORRUPTED"
                : "HIGH-07: FMFS, a version 1 table that makes no sense is FM_ERR_CORRUPTED");
        g_pf_cap = 32u * 1024u;
        g_pf_block = 512;
        memset(g_pf_mem, 0, g_pf_cap);
        if (!v2) pf_v1_image(g_pf_mem, g_pf_cap);
        memset(two, 't', sizeof(g_av5f_two) - 1u);
        two[sizeof(g_av5f_two) - 1u] = '\0';
        r = pf_attach(-1);
        if (r == FM_OK && (FMFS_IsPowerSafe(&g_pf_fs) ? 1 : 0) != v2)
            r = FM_ERR_INVALID_FORMAT;
        if (r == FM_OK) r = File_WriteText("/pf/one.txt", "first");
        if (r == FM_OK) r = File_WriteText("/pf/two.txt", two);
        if (r == FM_OK) r = FS_Check(g_pf_mount);
        pf_detach();
        if (r != FM_OK) { CHECK_RESULT(r, "set-up"); continue; }

        for (change = 0; change < 6; change++)
        {
            got[change] = av5f_mount_changed(v2, change);
        }
        if (got[0] != FM_OK) bad = 1;
        for (change = 1; change < (v2 ? 6 : 3); change++)
        {
            if (got[change] != FM_ERR_CORRUPTED) bad = 1;
        }
        /* version 1 writes its header last, so a reset can leave this;
         * the file is whole and the volume mounts (the mark is moved) */
        if (!v2 && got[3] != FM_OK) bad = 1;
        got[6] = v2 ? FM_OK : av5f_mount_changed(0, 6);
        if (got[6] != FM_OK)
        {
            printf("[stale data_next, then a new file: %d] ", (int)got[6]);
            bad = 1;
        }
        if (bad)
        {
            printf("[unchanged %d; at byte 0: %d; two files share bytes: %d; "
                   "behind data_next: %d; no parent: %d; one name twice: %d] ",
                   (int)got[0], (int)got[1], (int)got[2], (int)got[3],
                   (int)got[4], (int)got[5]);
            FAIL("mounted");
        }
        else { PASS(); }
    }
}
#else
static void av5_fmfs_table(void) { }
#endif

static void test_audit_v5_file(void)
{
    printf("\n== audit V5: files ==\n");
    av5_pool();
    av5_text_tmp();
    av5_qspi();
    av5_mounts();
    av5_utils();
    av5_fmfs_table();
}

/* ------------------------------------------------------------------ */
/* Video, suspicion S-1 of AUDIT_VIDEO_V5                              */
/* ------------------------------------------------------------------ */
#if FM_USE_VIDEO
static void test_audit_v5_video(void)
{
    int law;

    printf("\n== audit V5: video ==\n");
    for (law = 0; law < 2; law++)
    {
        /* every byte value once, in two packets of 128 */
        static uint8_t fbuf[512];
        static uint8_t all[256];
        uint8_t snd = law == 0 ? 0x70u : 0x80u;     /* FLV: A-law, mu-law */
        uint8_t pcm[2][256];
        uint8_t small[255];
        FM_Video vid;
        Eb e;
        FM_Allocator saved = *FM_Allocator_Get();
        FM_Allocator cnt = saved;
        size_t got = 0;
        FM_Result r;
        FM_Result too_small = FM_OK;
        uint32_t allocs_small = 0;
        uint32_t allocs_read = 0;
        uint32_t i;
        int bad = 0;

        TEST(law == 0 ? "S-1: A-law in a video is expanded in the caller's buffer, capacity first"
                      : "S-1: mu-law in a video is expanded in the caller's buffer, capacity first");
        for (i = 0; i < 256u; i++) all[i] = (uint8_t)i;
        memset(&e, 0, sizeof(e));
        memset(&vid, 0, sizeof(vid));
        memset(pcm, 0, sizeof(pcm));
        e.b = fbuf; e.cap = sizeof(fbuf);
        vr_flv_head(&e);
        vr_flv_tag(&e, 9, 0, "\x22", 1, "VVVV", 4);
        vr_flv_tag(&e, 8, 0, &snd, 1, all, 128);
        vr_flv_tag(&e, 8, 16, &snd, 1, all + 128, 128);
        r = vr_write("/av5law.flv", &e);
        if (r == FM_OK) r = Video_Open("/av5law.flv", &vid);
        if (r == FM_OK)
        {
            cnt.alloc = vr_cnt_alloc;
            g_vr_allocs = 0;
            (void)FM_Allocator_Set(&cnt);
            /* one byte short of the 256 a packet of 128 needs */
            too_small = Video_ReadAudioFrame(&vid, small, sizeof(small), &got);
            allocs_small = g_vr_allocs;
            g_vr_allocs = 0;
            /* exactly enough: the two halves of the buffer touch */
            r = Video_ReadAudioFrame(&vid, pcm[0], sizeof(pcm[0]), &got);
            if (r == FM_OK && got != 256u) r = FM_ERR_INVALID_FORMAT;
            if (r == FM_OK)
                r = Video_ReadAudioFrame(&vid, pcm[1], sizeof(pcm[1]), &got);
            if (r == FM_OK && got != 256u) r = FM_ERR_INVALID_FORMAT;
            allocs_read = g_vr_allocs;
            (void)FM_Allocator_Set(&saved);
            (void)Video_Close(&vid);
        }
        (void)File_Delete("/av5law.flv");
        if (r != FM_OK) { CHECK_RESULT(r, "set-up"); continue; }

        /* G.711: the two codes that differ in the sign bit only are the
         * same sample with the other sign; the loudest is +-32124 (mu)
         * or +-32256 (A), the quietest +-0 (mu) or +-8 (A) */
        for (i = 0; i < 128u; i++)
        {
            int16_t lo = (int16_t)((uint16_t)pcm[0][i * 2u] |
                                   ((uint16_t)pcm[0][i * 2u + 1u] << 8));
            int16_t hi = (int16_t)((uint16_t)pcm[1][i * 2u] |
                                   ((uint16_t)pcm[1][i * 2u + 1u] << 8));

            if ((int)lo != -(int)hi) bad = 1;
        }
        {
            int peak = 0;

            for (i = 0; i < 256u; i++)
            {
                int16_t s = (int16_t)((uint16_t)pcm[i / 128u][(i % 128u) * 2u] |
                                      ((uint16_t)pcm[i / 128u][(i % 128u) * 2u + 1u] << 8));
                int a = s < 0 ? -(int)s : (int)s;

                if (a > peak) peak = a;
            }
            if (peak != (law == 0 ? 32256 : 32124))
            {
                printf("[loudest sample %d] ", peak);
                bad = 1;
            }
        }
        if (too_small != FM_ERR_TOO_LARGE || allocs_small != 0u ||
            allocs_read != 0u || bad)
        {
            printf("[buffer one byte short: %d after %lu FM_Alloc; "
                   "%lu FM_Alloc for two packets; samples %s] ",
                   (int)too_small, (unsigned long)allocs_small,
                   (unsigned long)allocs_read, bad ? "wrong" : "right");
            FAIL("failed");
        }
        else { PASS(); }
    }
}
#endif /* FM_USE_VIDEO */

/* ------------------------------------------------------------------ */
/* Image, suspicion IMG5-S2 of AUDIT_IMAGE_V5                          */
/* ------------------------------------------------------------------ */
#if FM_USE_IMAGE
/* What FM_SCALE_BILINEAR is defined to be, in floating point: pixel
 * centres map onto pixel centres, the edge pixel is repeated, the four
 * neighbours are mixed by distance. */
static double av5i_ref(const uint8_t *src, int sw, int sh, int dw, int dh,
                       int x, int y)
{
    double fx = ((double)x + 0.5) * (double)sw / (double)dw - 0.5;
    double fy = ((double)y + 0.5) * (double)sh / (double)dh - 0.5;
    int x1, y1, x2, y2;
    double wx, wy;

    if (fx < 0.0) fx = 0.0;
    if (fy < 0.0) fy = 0.0;
    x1 = (int)fx; y1 = (int)fy;
    wx = fx - (double)x1; wy = fy - (double)y1;
    x2 = x1 + 1; y2 = y1 + 1;
    if (x1 >= sw) x1 = sw - 1;
    if (x2 >= sw) x2 = sw - 1;
    if (y1 >= sh) y1 = sh - 1;
    if (y2 >= sh) y2 = sh - 1;
    return (1.0 - wx) * (1.0 - wy) * src[y1 * sw + x1] +
           wx * (1.0 - wy) * src[y1 * sw + x2] +
           (1.0 - wx) * wy * src[y2 * sw + x1] +
           wx * wy * src[y2 * sw + x2];
}

static void test_audit_v5_image(void)
{
    enum { SW = 37, SH = 29, DMAX = 96 };
    static uint8_t src[SW * SH];
    static uint8_t dst[DMAX * DMAX];
    static const uint16_t sizes[][2] = {
        { 37, 29 }, { 85, 67 }, { 22, 17 }, { 4, 3 }, { 96, 5 }, { 1, 1 },
        { 74, 58 }, { 36, 28 }
    };
    uint32_t seed = 12345u;
    size_t k;
    int i;

    printf("\n== audit V5: image ==\n");
    for (i = 0; i < SW * SH; i++)
    {
        seed = seed * 1103515245u + 12345u;
        src[i] = (uint8_t)(seed >> 16);
    }

    TEST("IMG5-S2: bilinear scaling is the centre-to-centre mix of four pixels, at every size");
    {
        double worst = 0.0;
        int wk = -1;
        FM_Result r = FM_OK;

        for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]) && r == FM_OK; k++)
        {
            int dw = sizes[k][0];
            int dh = sizes[k][1];
            int x, y;

            memset(dst, 0, sizeof(dst));
            r = Image_Scale(src, SW, SH, FM_PIXEL_GRAY8, dst, (uint16_t)dw,
                            (uint16_t)dh, FM_SCALE_BILINEAR);
            for (y = 0; y < dh && r == FM_OK; y++)
            {
                for (x = 0; x < dw; x++)
                {
                    double d = (double)dst[y * dw + x] -
                               av5i_ref(src, SW, SH, dw, dh, x, y);

                    if (d < 0.0) d = -d;
                    if (d > worst) { worst = d; wk = (int)k; }
                }
            }
        }
        /* the weights are kept in 1/256 of a pixel: with noise as the
         * source (neighbours up to 255 apart) that is up to one grey
         * level per axis, plus the rounding of the result */
        if (r != FM_OK || worst > 2.5)
        {
            printf("[result %d; worst difference %.2f grey levels at size "
                   "%dx%d] ", (int)r, worst, wk < 0 ? 0 : sizes[wk][0],
                   wk < 0 ? 0 : sizes[wk][1]);
            FAIL("differs from the definition");
        }
        else { PASS(); }
    }

    TEST("IMG5-S2: a ramp stays a ramp across the edge (no step between edge and interior)");
    {
        /* a horizontal ramp of 4 grey levels a pixel, enlarged 3 times:
         * every step between neighbours is 0, 1 or 2 -- the first and
         * the last columns included -- and never downwards */
        static uint8_t ramp[40 * 2];
        static uint8_t big[120 * 6];
        FM_Result r;
        int x;
        int bad = 0;

        for (x = 0; x < 40; x++) { ramp[x] = (uint8_t)(x * 4); ramp[40 + x] = (uint8_t)(x * 4); }
        r = Image_Scale(ramp, 40, 2, FM_PIXEL_GRAY8, big, 120, 6,
                        FM_SCALE_BILINEAR);
        for (x = 1; r == FM_OK && x < 120; x++)
        {
            int step = (int)big[3 * 120 + x] - (int)big[3 * 120 + x - 1];

            if (step < 0 || step > 2) bad = x;
        }
        if (r != FM_OK || bad != 0 || big[3 * 120] != 0 ||
            big[3 * 120 + 119] != 156)
        {
            printf("[result %d; step out of range at column %d; ends %d..%d] ",
                   (int)r, bad, (int)big[3 * 120], (int)big[3 * 120 + 119]);
            FAIL("the ramp is broken");
        }
        else { PASS(); }
    }
}
#endif /* FM_USE_IMAGE */
