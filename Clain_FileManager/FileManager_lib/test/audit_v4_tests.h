/*
 * audit_v4_tests.h -- tests for the findings of the external audit V4
 * (ReportV3.txt at the repository root; audited commit 48a00bc).
 *
 * Included by test/main.c.  Every test here failed before its fix; the
 * comment above each one names the finding.
 */

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

#if FM_USE_FMFS || FM_USE_LITTLEFS
/* A driver that buffers: a write reaches g_fc_mem (what the driver has
 * seen), only sync() brings it to av4_medium (what a power cut keeps). */
static uint8_t           av4_medium[sizeof(g_fc_mem[0])];
static FM_Storage_SyncFn av4_real_sync;
static uint32_t          av4_syncs;
static int               av4_power_off;

static FM_Result av4_sync(FM_Storage *s)
{
    FM_Result r = FM_OK;

    if (av4_power_off)
    {
        return FM_OK;       /* nothing reaches the medium any more */
    }
    if (av4_real_sync != NULL) r = av4_real_sync(s);
    if (r == FM_OK || r == FM_ERR_UNSUPPORTED)
    {
        memcpy(av4_medium, g_fc_mem[0], sizeof(av4_medium));
        av4_syncs++;
        r = FM_OK;
    }
    return r;
}

/* mount backend `kind` on the buffering driver (blank: a new volume) */
static FM_Result av4_buffered_mount(int kind, int blank)
{
    FM_Result r = fc_storage(0, kind, blank);

    if (r == FM_OK)
    {
        av4_real_sync = g_fc_st[0].sync;
        g_fc_st[0].sync = av4_sync;
        r = fc_init(kind, &g_av2_fs);
    }
    if (r == FM_OK) r = FS_Mount(g_fc_mount[0], &g_av2_fs);
    return r;
}

/* The power goes: whatever was not synced is gone.  `f` (may be NULL)
 * is a handle that was open at that moment; the library is told to
 * forget it, and what its close writes no longer reaches the medium. */
static FM_Result av4_power_cut(int kind, FM_File *f)
{
    av4_power_off = 1;
    if (f != NULL) (void)File_Close(f);
    fc_drop(0);
    memcpy(g_fc_mem[0], av4_medium, sizeof(av4_medium));
    av4_power_off = 0;
    return av4_buffered_mount(kind, 0);
}

/* FILE4-01: File_Flush after an overwrite in place.  FMFS answered
 * FM_OK without a single sync() -- nothing in its table had changed --
 * and the header patch of a recording checkpoint was lost. */
static void av4_flush_one(int kind, int by_close)
{
    static uint8_t body[4096];
    uint8_t back[8];
    FM_File *f;
    uint32_t n = 0;
    uint32_t before;
    FM_Result r;

    r = av4_buffered_mount(kind, 1);
    f = (r == FM_OK) ? File_Open("/fca/ff.bin", FM_OPEN_WRITE | FM_OPEN_CREATE |
                                                FM_OPEN_TRUNCATE)
                     : NULL;
    if (r == FM_OK && f == NULL) r = File_GetLastError();
    memset(body, 0x5A, sizeof(body));
    if (r == FM_OK) r = File_Write(f, "RIFF0000", 8u, &n);
    if (r == FM_OK) r = File_Write(f, body, sizeof(body), &n);
    if (f != NULL)
    {
        FM_Result c = File_Close(f);

        if (r == FM_OK) r = c;
    }
    /* the file as it stands is on the medium, whoever synced it */
    memcpy(av4_medium, g_fc_mem[0], sizeof(av4_medium));

    f = (r == FM_OK) ? File_Open("/fca/ff.bin", FM_OPEN_READ | FM_OPEN_WRITE)
                     : NULL;
    if (r == FM_OK && f == NULL) r = File_GetLastError();
    before = av4_syncs;
    if (r == FM_OK) r = File_Write(f, "RIFF819F", 8u, &n);
    if (r == FM_OK && !by_close) r = File_Flush(f);
    if (r == FM_OK && by_close)
    {
        r = File_Close(f);
        f = NULL;
    }
    if (r == FM_OK && av4_syncs == before)
    {
        printf("[%s: no sync] ", av2_kind_name(kind));
        r = FM_ERR_IO;
    }
    if (r == FM_OK) r = av4_power_cut(kind, f);
    else if (f != NULL) (void)File_Close(f);
    f = (r == FM_OK) ? File_Open("/fca/ff.bin", FM_OPEN_READ) : NULL;
    if (r == FM_OK && f == NULL) r = File_GetLastError();
    n = 0;
    memset(back, 0, sizeof(back));
    if (r == FM_OK) r = File_Read(f, back, sizeof(back), &n);
    if (f != NULL) (void)File_Close(f);
    if (r == FM_OK && (n != 8u || memcmp(back, "RIFF819F", 8u) != 0))
    {
        printf("[%s: header %.8s] ", av2_kind_name(kind), (const char *)back);
        r = FM_ERR_CRC;
    }
    fc_drop(0);
    CHECK_RESULT(r, by_close ? "close" : "flush");
}

static void av4_flush(void)
{
    int kind;

    for (kind = 0; kind < 2; kind++)
    {
        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "FILE4-01: FMFS, File_Flush puts an overwrite on the medium"
                       : "FILE4-01: LittleFS, File_Flush puts an overwrite on the medium");
        av4_flush_one(kind, 0);
        TEST(kind == 0 ? "FILE4-01: FMFS, File_Close does the same"
                       : "FILE4-01: LittleFS, File_Close does the same");
        av4_flush_one(kind, 1);
    }
}
#else
static void av4_flush(void) { }
#endif

/* FILE4-03: delete / rename / copy onto a file that is open.  Three
 * backends gave three behaviours; LittleFS answered FM_OK for writes
 * into a file that no longer existed.  Now: FM_ERR_BUSY everywhere. */
static void av4_open_file_ops(void)
{
    int kind;

    for (kind = 0; kind < 3; kind++)
    {
        const char *root = "";
        char x[64], y[64], z[64], tmp[80];
        char back[16];
        size_t got = 0;
        uint32_t n = 0;
        FM_File *f = NULL;
        FM_Bool there = FM_TRUE;
        FM_Result r;

        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "FILE4-03: FMFS, an open file is not deleted or replaced"
           : kind == 1 ? "FILE4-03: LittleFS, an open file is not deleted or replaced"
                       : "FILE4-03: POSIX, an open file is not deleted or replaced");
        r = av2_mount(kind, &root);
        av2_path(x, sizeof(x), root, "av4_x.bin");
        av2_path(y, sizeof(y), root, "av4_y.bin");
        av2_path(z, sizeof(z), root, "av4_z.bin");
        (void)snprintf(tmp, sizeof(tmp), "%s.fm_tmp", x);
        (void)File_Delete(x);
        (void)File_Delete(y);
        (void)File_Delete(z);
        if (r == FM_OK) r = File_WriteAll(y, "ABCDEFGH", 8u);
        f = (r == FM_OK) ? File_Open(x, FM_OPEN_WRITE | FM_OPEN_CREATE |
                                        FM_OPEN_TRUNCATE)
                         : NULL;
        if (r == FM_OK && f == NULL) r = File_GetLastError();
        if (r == FM_OK) r = File_Write(f, "11111", 5u, &n);
        if (r == FM_OK)
        {
            if (File_Delete(x) != FM_ERR_BUSY)
            {
                printf("[delete] ");
                r = FM_ERR_INTERNAL;
            }
            else if (File_Rename(y, x) != FM_ERR_BUSY)
            {
                printf("[rename onto it] ");
                r = FM_ERR_INTERNAL;
            }
            else if (File_Rename(x, z) != FM_ERR_BUSY)
            {
                printf("[rename it away] ");
                r = FM_ERR_INTERNAL;
            }
            else if (File_Copy(y, x, NULL, NULL) == FM_OK)
            {
                printf("[copy onto it] ");
                r = FM_ERR_INTERNAL;
            }
            else if (File_Move(y, x, NULL, NULL) == FM_OK)
            {
                printf("[move onto it] ");
                r = FM_ERR_INTERNAL;
            }
        }
        /* the handle still works, and nothing it writes is lost */
        if (r == FM_OK) r = File_Write(f, "22222", 5u, &n);
        if (f != NULL)
        {
            FM_Result c = File_Close(f);

            if (r == FM_OK) r = c;
        }
        memset(back, 0, sizeof(back));
        if (r == FM_OK) r = File_ReadAll(x, back, sizeof(back) - 1u, &got);
        if (r == FM_OK && (got != 10u || memcmp(back, "1111122222", 10u) != 0))
        {
            printf("[content '%s'] ", back);
            r = FM_ERR_CRC;
        }
        /* the source of the refused copy / move is still there, and no
         * half-made copy was left beside the target */
        memset(back, 0, sizeof(back));
        if (r == FM_OK) r = File_ReadAll(y, back, sizeof(back) - 1u, &got);
        if (r == FM_OK && (got != 8u || memcmp(back, "ABCDEFGH", 8u) != 0))
        {
            r = FM_ERR_CRC;
        }
        if (r == FM_OK) r = File_Exists(tmp, &there);
        if (r == FM_OK && there)
        {
            printf("[%s left behind] ", tmp);
            r = FM_ERR_EXISTS;
        }
        /* closed: everything is allowed again */
        if (r == FM_OK) r = File_Rename(x, z);
        if (r == FM_OK) r = File_Rename(y, z);          /* replaces z */
        if (r == FM_OK) r = File_Delete(z);
        (void)File_Delete(x);
        (void)File_Delete(y);
        (void)File_Delete(z);
        (void)File_Delete(tmp);
        av2_unmount(kind);
        CHECK_RESULT(r, av2_kind_name(kind));
    }
}

/* FILE4-04: File_Delete on a directory and Directory_Delete on a file.
 * FMFS refused, LittleFS and POSIX removed the entry and said FM_OK. */
static void av4_delete_kinds(void)
{
    int kind;

    for (kind = 0; kind < 3; kind++)
    {
        const char *root = "";
        char d[64], inner[80], fl[64];
        FM_Bool there = FM_FALSE;
        FM_Result r;

        if (!av2_kind_built(kind)) continue;
        TEST(kind == 0 ? "FILE4-04: FMFS, File_Delete(dir) / Directory_Delete(file)"
           : kind == 1 ? "FILE4-04: LittleFS, File_Delete(dir) / Directory_Delete(file)"
                       : "FILE4-04: POSIX, File_Delete(dir) / Directory_Delete(file)");
        r = av2_mount(kind, &root);
        av2_path(d, sizeof(d), root, "av4_dir");
        av2_path(fl, sizeof(fl), root, "av4_file.bin");
        (void)snprintf(inner, sizeof(inner), "%s/in.bin", d);
        (void)File_Delete(inner);
        (void)Directory_Delete(d);
        (void)File_Delete(fl);
        if (r == FM_OK) r = Directory_Create(d);
        if (r == FM_OK) r = File_WriteAll(fl, "x", 1u);
        if (r == FM_OK)
        {
            FM_Result a = File_Delete(d);             /* empty directory */
            FM_Result b = Directory_Delete(fl);

            if (a != FM_ERR_IS_DIRECTORY || b != FM_ERR_NOT_DIRECTORY)
            {
                printf("[empty dir %d, file %d] ", (int)a, (int)b);
                r = FM_ERR_INTERNAL;
            }
        }
        if (r == FM_OK) r = File_WriteAll(inner, "y", 1u);
        if (r == FM_OK && File_Delete(d) != FM_ERR_IS_DIRECTORY)
        {
            printf("[full dir] ");
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) r = Directory_Exists(d, &there);
        if (r == FM_OK && !there) r = FM_ERR_NOT_FOUND;
        if (r == FM_OK) r = File_Exists(fl, &there);
        if (r == FM_OK && !there) r = FM_ERR_NOT_FOUND;
        if (r == FM_OK) r = File_Delete(inner);
        if (r == FM_OK) r = Directory_Delete(d);
        if (r == FM_OK) r = File_Delete(fl);
        (void)File_Delete(inner);
        (void)Directory_Delete(d);
        (void)File_Delete(fl);
        av2_unmount(kind);
        CHECK_RESULT(r, av2_kind_name(kind));
    }
}

static void test_audit_v4_file(void)
{
    printf("\n== audit V4: files ==\n");
    av4_flush();
    av4_open_file_ops();
    av4_delete_kinds();
}

/* ================================================================== */
/* IMG4-02: "bilinear" has to mix the neighbours                       */
/* ================================================================== */
#if FM_USE_IMAGE
/* the answer worked out in floating point, pixel centre on pixel centre */
static double av4_bilinear_ref(const uint8_t *src, unsigned sw, unsigned sh,
                               unsigned ch, unsigned nch, unsigned dw,
                               unsigned dh, unsigned x, unsigned y)
{
    double fx = ((double)x + 0.5) * (double)sw / (double)dw - 0.5;
    double fy = ((double)y + 0.5) * (double)sh / (double)dh - 0.5;
    unsigned x1, x2, y1, y2;
    double ax, ay;

    if (fx < 0.0) fx = 0.0;
    if (fy < 0.0) fy = 0.0;
    x1 = (unsigned)fx; y1 = (unsigned)fy;
    ax = fx - (double)x1; ay = fy - (double)y1;
    if (x1 >= sw) { x1 = sw - 1u; ax = 0.0; }
    if (y1 >= sh) { y1 = sh - 1u; ay = 0.0; }
    x2 = (x1 + 1u < sw) ? x1 + 1u : sw - 1u;
    y2 = (y1 + 1u < sh) ? y1 + 1u : sh - 1u;
    return (double)src[((size_t)y1 * sw + x1) * nch + ch] * (1.0 - ax) * (1.0 - ay)
         + (double)src[((size_t)y1 * sw + x2) * nch + ch] * ax * (1.0 - ay)
         + (double)src[((size_t)y2 * sw + x1) * nch + ch] * (1.0 - ax) * ay
         + (double)src[((size_t)y2 * sw + x2) * nch + ch] * ax * ay;
}

static void av4_bilinear(void)
{
    static uint8_t src[64 * 64 * 4];
    static uint8_t dst[200 * 200 * 4];

    TEST("IMG4-02: bilinear 2 -> 8 pixels is a ramp, not steps");
    {
        static const uint8_t in[2] = { 0, 200 };
        static const uint8_t want[8] = { 0, 0, 25, 75, 125, 175, 200, 200 };
        uint8_t out[8];
        FM_Result r = Image_Scale(in, 2, 1, FM_PIXEL_GRAY8, out, 8, 1,
                                  FM_SCALE_BILINEAR);

        if (r == FM_OK && memcmp(out, want, 8) != 0)
        {
            printf("[%u %u %u %u %u %u %u %u] ", out[0], out[1], out[2],
                   out[3], out[4], out[5], out[6], out[7]);
            r = FM_ERR_INTERNAL;
        }
        /* nearest is what it always was */
        if (r == FM_OK) r = Image_Scale(in, 2, 1, FM_PIXEL_GRAY8, out, 8, 1,
                                        FM_SCALE_NEAREST);
        if (r == FM_OK && (out[3] != 0u || out[4] != 200u)) r = FM_ERR_INTERNAL;
        CHECK_RESULT(r, "bilinear upscale repeats pixels");
    }

    TEST("IMG4-02: bilinear 4 -> 2 pixels mixes both neighbours");
    {
        static const uint8_t in[4] = { 0, 200, 0, 0 };
        uint8_t out[2] = { 7, 7 };
        FM_Result r = Image_Scale(in, 4, 1, FM_PIXEL_GRAY8, out, 2, 1,
                                  FM_SCALE_BILINEAR);

        if (r == FM_OK && (out[0] != 100u || out[1] != 0u))
        {
            printf("[%u %u] ", out[0], out[1]);
            r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "one of the two neighbours was dropped");
    }

    TEST("IMG4-02: bilinear follows the exact answer, all formats");
    {
        static const struct { uint16_t w, h; } to[] = {
            { 64, 64 }, { 32, 32 }, { 128, 128 }, { 1, 1 }, { 200, 3 },
            { 3, 200 }, { 127, 63 }, { 200, 200 }, { 65, 64 }, { 63, 64 }
        };
        static const FM_PixelFormat fmts[] = {
            FM_PIXEL_GRAY8, FM_PIXEL_RGB888, FM_PIXEL_BGR888,
            FM_PIXEL_RGBA8888, FM_PIXEL_BGRA8888
        };
        FM_Result r = FM_OK;
        double worst = 0.0;
        size_t f, t, i;

        for (f = 0; f < sizeof(fmts) / sizeof(fmts[0]) && r == FM_OK; f++)
        {
            unsigned nch = (fmts[f] == FM_PIXEL_GRAY8) ? 1u
                         : (fmts[f] == FM_PIXEL_RGB888 ||
                            fmts[f] == FM_PIXEL_BGR888) ? 3u : 4u;

            /* a picture with hard edges and a slope in every channel */
            for (i = 0; i < 64u * 64u; i++)
            {
                unsigned x = (unsigned)(i % 64u), y = (unsigned)(i / 64u);
                unsigned ch;

                for (ch = 0; ch < nch; ch++)
                {
                    src[i * nch + ch] = (uint8_t)
                        ((((x / 5u) ^ (y / 3u)) & 1u) ? 230u - x * 2u - ch * 9u
                                                      : 10u + y * 3u + ch * 20u);
                }
            }
            for (t = 0; t < sizeof(to) / sizeof(to[0]) && r == FM_OK; t++)
            {
                unsigned x, y, ch;

                memset(dst, 0xA5, sizeof(dst));
                r = Image_ScaleEx(src, sizeof(src), 64, 64, fmts[f], dst,
                                  sizeof(dst), to[t].w, to[t].h,
                                  FM_SCALE_BILINEAR);
                for (y = 0; y < to[t].h && r == FM_OK; y++)
                {
                    for (x = 0; x < to[t].w; x++)
                    {
                        for (ch = 0; ch < nch; ch++)
                        {
                            double want = av4_bilinear_ref(src, 64, 64, ch, nch,
                                                           to[t].w, to[t].h,
                                                           x, y);
                            double got = (double)
                                dst[((size_t)y * to[t].w + x) * nch + ch];
                            double e = (got > want) ? got - want : want - got;

                            if (e > worst) worst = e;
                        }
                    }
                }
                /* nothing written behind the picture */
                if (r == FM_OK && (size_t)to[t].w * to[t].h * nch < sizeof(dst) &&
                    dst[(size_t)to[t].w * to[t].h * nch] != 0xA5u)
                {
                    r = FM_ERR_INTERNAL;
                }
            }
        }
        /* the weights are 1/256 of a pixel: two levels is the rounding,
         * the nearest pixel is wrong by more than a hundred here */
        if (r == FM_OK && worst > 2.0)
        {
            printf("[worst error %.2f levels] ", worst);
            r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "bilinear is far from the exact answer");
    }

    TEST("IMG4-02: bilinear RGB565 mixes inside each colour field");
    {
        /* pure red next to pure blue: the pixel between them holds half
         * of each and no green */
        static const uint16_t in[2] = { 0xF800u, 0x001Fu };
        uint16_t out[4] = { 0, 0, 0, 0 };
        FM_Result r = Image_Scale(in, 2, 1, FM_PIXEL_RGB565, out, 4, 1,
                                  FM_SCALE_BILINEAR);

        if (r == FM_OK)
        {
            unsigned red1 = out[1] >> 11, grn1 = (out[1] >> 5) & 63u;
            unsigned blu1 = out[1] & 31u, red2 = out[2] >> 11;
            unsigned blu2 = out[2] & 31u;

            if (out[0] != 0xF800u || out[3] != 0x001Fu || grn1 > 1u ||
                red1 < 21u || red1 > 25u || blu1 < 6u || blu1 > 10u ||
                red2 < 6u || red2 > 10u || blu2 < 21u || blu2 > 25u)
            {
                printf("[%04X %04X %04X %04X] ", out[0], out[1], out[2], out[3]);
                r = FM_ERR_INTERNAL;
            }
        }
        CHECK_RESULT(r, "RGB565 bilinear");
    }
}

static void test_audit_v4_image(void)
{
    printf("\n== audit V4: image ==\n");
    av4_bilinear();
}
#endif /* FM_USE_IMAGE */

/* ================================================================== */
/* Video                                                               */
/* ================================================================== */
#if FM_USE_VIDEO
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>                 /* alarm(): a parser that hangs is killed */
#define AV4_ALARM(sec) ((void)alarm(sec))
#else
#define AV4_ALARM(sec) ((void)0)
#endif

#if FM_USE_H264
#define AV4_SPS_OK 0u               /* HANTRO_OK of h264bsd_util.h */
/* --- VID4-01: an SPS whose picture size is not a picture ------------ */
typedef struct { uint8_t *d; size_t bit; } Av4Bits;

static void av4_put(Av4Bits *b, uint32_t v, unsigned n)
{
    while (n-- != 0u)
    {
        if ((v >> n) & 1u) b->d[b->bit / 8u] |= (uint8_t)(0x80u >> (b->bit % 8u));
        b->bit++;
    }
}
static void av4_ue(Av4Bits *b, uint32_t v)
{
    unsigned len = 0;
    uint32_t x = v + 1u;

    while ((x >> (len + 1u)) != 0u) len++;
    av4_put(b, 0u, len);
    av4_put(b, x, len + 1u);
}

/* A baseline SPS of `w` x `h` macroblocks: the NAL header byte and the
 * payload as the parser reads it (not yet escaped for a byte stream). */
static size_t av4_sps(uint8_t *out, size_t cap, uint32_t w, uint32_t h)
{
    Av4Bits b;

    memset(out, 0, cap);
    b.d = out; b.bit = 0;
    av4_put(&b, 0x67u, 8);          /* NAL: SPS */
    av4_put(&b, 66u, 8);            /* baseline */
    av4_put(&b, 0u, 8);
    av4_put(&b, 30u, 8);            /* level 3 */
    av4_ue(&b, 0u);                 /* sps id */
    av4_ue(&b, 0u);                 /* log2_max_frame_num - 4 */
    av4_ue(&b, 0u);                 /* poc type 0 */
    av4_ue(&b, 0u);                 /* log2_max_poc_lsb - 4 */
    av4_ue(&b, 1u);                 /* reference frames */
    av4_put(&b, 0u, 1);             /* gaps */
    av4_ue(&b, w - 1u);
    av4_ue(&b, h - 1u);
    av4_put(&b, 1u, 1);             /* frame_mbs_only */
    av4_put(&b, 1u, 1);             /* direct_8x8 */
    av4_put(&b, 0u, 1);             /* no cropping */
    av4_put(&b, 0u, 1);             /* no VUI */
    av4_put(&b, 1u, 1);             /* stop bit */
    return (b.bit + 7u) / 8u;
}

/* ...and escaped for a byte stream: 00 00 0x becomes 00 00 03 0x */
static size_t av4_escape(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i, o = 0;
    unsigned zeros = 0;

    for (i = 0; i < n; i++)
    {
        if (zeros >= 2u && in[i] <= 3u)
        {
            out[o++] = 3u;
            zeros = 0;
        }
        out[o++] = in[i];
        zeros = (in[i] == 0u) ? zeros + 1u : 0u;
    }
    return o;
}

static u32 av4_sps_parse(uint32_t w, uint32_t h)
{
    uint8_t nal[64];
    strmData_t strm;
    seqParamSet_t sps;
    size_t n = av4_sps(nal, sizeof(nal), w, h);
    u32 rc;

    memset(&strm, 0, sizeof(strm));
    memset(&sps, 0, sizeof(sps));
    strm.pStrmBuffStart = nal + 1;
    strm.pStrmCurrPos = nal + 1;
    strm.strmBuffSize = (u32)(n - 1u);
    rc = h264bsdDecodeSeqParamSet(&strm, &sps);
    if (sps.offsetForRefFrame != NULL) FM_Free(sps.offsetForRefFrame);
    if (sps.vuiParameters != NULL) FM_Free(sps.vuiParameters);
    return rc;
}

static void av4_h264_sps(void)
{
    TEST("VID4-01: an SPS larger than any level allows is refused");
    {
        FM_Result r = FM_OK;

        /* real sizes stay: 320x240, 1920x1088, the 4096x2304 of level 5.1 */
        if (av4_sps_parse(20u, 15u) != AV4_SPS_OK ||
            av4_sps_parse(120u, 68u) != AV4_SPS_OK ||
            av4_sps_parse(256u, 144u) != AV4_SPS_OK ||
            av4_sps_parse(36864u, 1u) != AV4_SPS_OK)
        {
            printf("[a real size is refused] ");
            r = FM_ERR_INTERNAL;
        }
        /* one macroblock more, and sizes whose product wraps in 32 bits:
         * 65536 x 65536 is 0, 65537 x 65535 is 65535 */
        if (r == FM_OK &&
            (av4_sps_parse(257u, 144u) == AV4_SPS_OK ||
             av4_sps_parse(36865u, 1u) == AV4_SPS_OK ||
             av4_sps_parse(1u, 36865u) == AV4_SPS_OK ||
             av4_sps_parse(65536u, 65536u) == AV4_SPS_OK ||
             av4_sps_parse(65537u, 65535u) == AV4_SPS_OK ||
             av4_sps_parse(0x100000u, 0x1001u) == AV4_SPS_OK))
        {
            printf("[an impossible size is accepted] ");
            r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "SPS size");
    }

    TEST("VID4-01: a stream with such an SPS fails cleanly, the next one decodes");
    {
        static uint8_t h264[4096];
        static uint8_t stream[4096];
        static uint8_t pix[64 * 64 * 3];
        const uint8_t *nals[8];
        uint32_t sizes[8];
        uint8_t raw[64];
        uint8_t sps[96];
        size_t rd = 0;
        size_t sn;
        uint32_t nn = 0;
        const FM_VideoCodecOps *ops = H264_GetCodecOps();
        void *ctx = NULL;
        FM_Result r;

        restore_posix_root();
        r = File_ReadAll("/test/vectors/tiny64.h264", h264, sizeof(h264), &rd);
        if (r == FM_OK)
        {
            nn = split_nals(h264, (uint32_t)rd, nals, sizes, 8);
            if (nn < 3u) r = FM_ERR_INVALID_FORMAT;
        }
        sn = av4_escape(raw, av4_sps(raw, sizeof(raw), 65536u, 65536u), sps);
        if (r == FM_OK) r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            size_t n = 0;
            FM_Result d;

            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, sps, sn); n += sn;
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[1], sizes[1]); n += sizes[1];
            memcpy(stream + n, "\0\0\0\1", 4); n += 4;
            memcpy(stream + n, nals[2], sizes[2]); n += sizes[2];
            d = ops->decode_frame(ctx, stream, n, pix, sizeof(pix));
            if (d == FM_OK)
            {
                printf("[a picture came out] ");
                r = FM_ERR_INTERNAL;
            }
        }
        if (ctx != NULL) ops->close(ctx);
        ctx = NULL;
        /* the decoder is not left in a state: the real stream decodes */
        if (r == FM_OK) r = ops->init(&ctx, NULL);
        if (r == FM_OK)
        {
            memcpy(stream, h264, rd);
            r = ops->decode_frame(ctx, stream, rd, pix, sizeof(pix));
        }
        if (ctx != NULL) ops->close(ctx);
        CHECK_RESULT(r, "huge SPS in a stream");
    }
}
#endif /* FM_USE_H264 */

/* a vector with the AVI header's microseconds-per-picture replaced */
static FM_Result av4_avi_with_rate(const char *dst, uint32_t us_per_frame)
{
    static uint8_t avi[64 * 1024];
    size_t n = 0;
    FM_Result r = File_ReadAll("/test/vectors/avi_pcm24.avi", avi,
                               sizeof(avi), &n);

    if (r != FM_OK) return r;
    if (n < 64u || memcmp(avi + 24, "avih", 4) != 0) return FM_ERR_INVALID_FORMAT;
    avi[32] = (uint8_t)us_per_frame;
    avi[33] = (uint8_t)(us_per_frame >> 8);
    avi[34] = (uint8_t)(us_per_frame >> 16);
    avi[35] = (uint8_t)(us_per_frame >> 24);
    return File_WriteAll(dst, avi, n);
}

static void av4_video(void)
{
    static FM_Video v;
    FM_Result r;

    restore_posix_root();

    TEST("VID4-02: an MP4 box with a 64-bit size below its header is refused");
    {
        /* ftyp, then a "free" box that says: 64-bit size, and the size
         * is 0.  The walk used to stay on this box for ever. */
        static const uint8_t z[36] = {
            0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm',
            0x00, 0x00, 0x00, 0x00, 'i', 's', 'o', 'm',
            0x00, 0x00, 0x00, 0x01, 'f', 'r', 'e', 'e',
            0, 0, 0, 0, 0, 0, 0, 0
        };
        uint8_t f[36];
        unsigned size64;

        r = FM_OK;
        for (size64 = 0; size64 < 16u && r == FM_OK; size64++)
        {
            memcpy(f, z, sizeof(f));
            f[35] = (uint8_t)size64;
            r = File_WriteAll("/av4_box.mp4", f, sizeof(f));
            memset(&v, 0, sizeof(v));
            if (r == FM_OK)
            {
                FM_Result o;

                AV4_ALARM(20);          /* a walk that stands still is killed */
                o = Video_Open("/av4_box.mp4", &v);
                AV4_ALARM(0);
                if (o == FM_OK)
                {
                    (void)Video_Close(&v);
                    printf("[size %u opened] ", size64);
                    r = FM_ERR_INTERNAL;
                }
            }
        }
        (void)File_Delete("/av4_box.mp4");
        CHECK_RESULT(r, "short 64-bit box");
    }

    TEST("VID4-05: the exact frame rate is kept next to the rounded one");
    {
        static const struct { const char *path; } whole[] = {
            { "/test/vectors/avi_pcm24.avi" }, { "/test/vectors/mov_pcm_s16.mov" },
            { "/test/vectors/mkv_pcm32.mkv" }, { "/test/vectors/mp4_mjpeg_patches.mp4" }
        };
        size_t i;

        r = FM_OK;
        /* a whole rate: the fraction is that rate */
        for (i = 0; i < sizeof(whole) / sizeof(whole[0]) && r == FM_OK; i++)
        {
            memset(&v, 0, sizeof(v));
            r = Video_Open(whole[i].path, &v);
            if (r == FM_OK)
            {
                if (v.info.fps == 0u || v.info.fps_num == 0u ||
                    v.info.fps_den == 0u ||
                    (uint64_t)v.info.fps * v.info.fps_den != v.info.fps_num)
                {
                    printf("[%s: %lu, %lu/%lu] ", whole[i].path,
                           (unsigned long)v.info.fps,
                           (unsigned long)v.info.fps_num,
                           (unsigned long)v.info.fps_den);
                    r = FM_ERR_INTERNAL;
                }
                (void)Video_Close(&v);
            }
        }
        /* 29.97 and 12.5: rounded for show, exact for time */
        if (r == FM_OK) r = av4_avi_with_rate("/av4_rate.avi", 33367u);
        memset(&v, 0, sizeof(v));
        if (r == FM_OK) r = Video_Open("/av4_rate.avi", &v);
        if (r == FM_OK)
        {
            FM_VideoInfo vi;

            memset(&vi, 0, sizeof(vi));
            (void)Video_GetInfo(&v, &vi);
            if (v.info.fps != 30u || v.info.fps_num != 1000000u ||
                v.info.fps_den != 33367u || vi.fps_num != 1000000u ||
                vi.fps_den != 33367u)
            {
                r = FM_ERR_INTERNAL;
            }
            (void)Video_Close(&v);
        }
        if (r == FM_OK) r = av4_avi_with_rate("/av4_rate.avi", 80000u);
        memset(&v, 0, sizeof(v));
        if (r == FM_OK) r = Video_Open("/av4_rate.avi", &v);
        if (r == FM_OK)
        {
            if (v.info.fps != 13u || v.info.fps_num != 1000000u ||
                v.info.fps_den != 80000u)
            {
                r = FM_ERR_INTERNAL;
            }
            (void)Video_Close(&v);
        }
        (void)File_Delete("/av4_rate.avi");
        CHECK_RESULT(r, "fps fraction");
    }

    TEST("VID4-05 / -07: the player finds .mkv, and counts time by the exact rate");
    {
        static FM_Player p;
        uint32_t frames = 0;
        double dur = 0.0;
        double pos = -1.0;

        (void)Directory_DeleteRecursive("/av4_scan");
        r = Directory_Create("/av4_scan");
        if (r == FM_OK) r = File_Copy("/test/vectors/mkv_pcm32.mkv",
                                      "/av4_scan/b.mkv", NULL, NULL);
        if (r == FM_OK) r = File_Copy("/test/vectors/mkv_vorbis.mkv",
                                      "/av4_scan/c.webm", NULL, NULL);
        if (r == FM_OK) r = av4_avi_with_rate("/av4_scan/a.avi", 80000u);
        if (r == FM_OK) r = Player_Init(&p);
        if (r == FM_OK) r = Player_ScanFolder(&p, "/av4_scan", FM_PLAYER_KIND_VIDEO);
        if (r == FM_OK && Player_Count(&p) != 3u)
        {
            printf("[%lu of 3 found] ", (unsigned long)Player_Count(&p));
            r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) r = Player_Play(&p, 0u);          /* a.avi, 12.5 fps */
        if (r == FM_OK)
        {
            frames = p.video.info.frame_count;
            dur = Player_GetDuration(&p);
            /* n pictures of 80 ms; by the rounded 13 fps it was n / 13 */
            if (frames == 0u || dur < (double)frames * 0.08 - 0.0001 ||
                dur > (double)frames * 0.08 + 0.0001)
            {
                printf("[%lu pictures last %.4f s] ", (unsigned long)frames, dur);
                r = FM_ERR_INTERNAL;
            }
        }
        if (r == FM_OK && frames >= 2u)
        {
            /* 0.09 s is inside picture 1 (0.08..0.16); 13 fps said so
             * too, so ask at the far end: the last picture */
            r = Player_Seek(&p, (double)(frames - 1u) * 0.08 + 0.001);
            if (r == FM_OK) pos = Player_GetPosition(&p);
            if (r == FM_OK && p.video.current_frame != frames - 1u)
            {
                printf("[seek landed on picture %lu of %lu, %.3f s] ",
                       (unsigned long)p.video.current_frame,
                       (unsigned long)frames, pos);
                r = FM_ERR_INTERNAL;
            }
        }
        (void)Player_Stop(&p);
        Player_Deinit(&p);
        (void)Directory_DeleteRecursive("/av4_scan");
        CHECK_RESULT(r, "player");
    }

#if FM_USE_AUDIO && FM_USE_FLAC
    TEST("VID4-06: a FLAC block larger than the buffer is not cut in silence");
    {
        static uint8_t small[1024];
        Av3Run run;
        size_t n = 77;
        uint32_t at = 99;
        FM_Result r1 = FM_OK, r2 = FM_OK;

        memset(&run, 0, sizeof(run));
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/mp4_flac24.mp4", &v);
        if (r == FM_OK)
        {
            /* too small for the block: an error, nothing taken, twice */
            r1 = Video_ReadAudioFrame(&v, small, sizeof(small), &n);
            r2 = Video_ReadAudioFrame(&v, small, sizeof(small), &n);
            at = v.audio_frame_index;
            /* a buffer that holds it: the whole track, from its start */
            av3_drain(&v, 16384u, 500u, &run);
            (void)Video_Close(&v);
        }
        if (r == FM_OK &&
            (r1 != FM_ERR_TOO_LARGE || r2 != FM_ERR_TOO_LARGE || at != 0u ||
             run.bad != 0u || !run.ended || run.bytes != AV3_PCM_BYTES ||
             av3_crc32(g_av3_pcm, run.bytes) != AV3_PCM_CRC))
        {
            printf("[reads %d %d, at packet %lu; then %lu bytes, %u errors] ",
                   (int)r1, (int)r2, (unsigned long)at,
                   (unsigned long)run.bytes, run.bad);
            r = FM_ERR_INTERNAL;
        }
        CHECK_RESULT(r, "FLAC block");
    }
#endif
}

#if FM_USE_MJPEG && FM_USE_JPEG && FM_USE_IMAGE
/* VID4-05, the converter: the copy keeps the rate of its source */
static void av4_convert_rate(void)
{
    static FM_Video v;
    FM_VideoInfo vi;
    FM_Result r;
    uint32_t frames = 0;
    char msg[120];

    restore_posix_root();
    TEST("VID4-05: Video_Convert writes the exact frame rate of the source");
    memset(&vi, 0, sizeof(vi));
    r = av4_avi_with_rate("/av4_125.avi", 80000u);          /* 12.5 fps */
    if (r == FM_OK) r = Video_Convert("/av4_125.avi", "/av4_125c.avi", NULL, &frames);
    if (r == FM_OK) r = Video_Open("/av4_125c.avi", &v);
    if (r == FM_OK)
    {
        (void)Video_GetInfo(&v, &vi);
        (void)Video_Close(&v);
    }
    (void)File_Delete("/av4_125.avi");
    (void)File_Delete("/av4_125c.avi");
    (void)snprintf(msg, sizeof(msg), "result %d, %lu frames, rate %lu/%lu (fps %lu)",
                   (int)r, (unsigned long)frames, (unsigned long)vi.fps_num,
                   (unsigned long)vi.fps_den, (unsigned long)vi.fps);
    /* 12.5 = 25 / 2; the old copy said 13 */
    if (r != FM_OK || frames == 0u || vi.fps_den == 0u ||
        (uint64_t)vi.fps_num * 2u != (uint64_t)vi.fps_den * 25u) FAIL(msg);
    else PASS();
}
#endif

static void test_audit_v4_video(void)
{
    printf("\n== audit V4: video ==\n");
#if FM_USE_H264
    av4_h264_sps();
#endif
#if FM_USE_MJPEG && FM_USE_JPEG && FM_USE_IMAGE
    av4_convert_rate();
#endif
    av4_video();
}
#endif /* FM_USE_VIDEO */

/* =====================================================================
 * AUDIT_AUDIO_V4
 * ===================================================================== */
#if FM_USE_AUDIO
static uint8_t g_av4_abuf[160 * 1024];

/* `frames` ADTS frames (16 payload bytes each) at dst; returns the bytes */
static size_t av4_adts(uint8_t *dst, uint32_t frames, uint32_t rate_index,
                       uint32_t channels)
{
    uint32_t i;
    size_t n = 0;

    for (i = 0; i < frames; i++)
    {
        uint8_t *h = dst + n;
        uint32_t flen = 7u + 16u;

        h[0] = 0xFF; h[1] = 0xF1;
        h[2] = (uint8_t)((1u << 6) | (rate_index << 2) | (channels >> 2));
        h[3] = (uint8_t)(((channels & 3u) << 6) | (flen >> 11));
        h[4] = (uint8_t)(flen >> 3);
        h[5] = (uint8_t)((flen << 5) | 0x1F);
        h[6] = 0xFC;
        memset(h + 7, (int)(i + 1u), 16);
        n += flen;
    }
    return n;
}

/* An ID3v2.4 tag of `payload` bytes, with or without a footer */
static size_t av4_id3(uint8_t *dst, uint32_t payload, int footer)
{
    size_t n = 10u + payload;

    memset(dst, 0, n + 10u);
    memcpy(dst, "ID3\x04", 4);
    dst[5] = footer ? 0x10u : 0u;
    dst[6] = (uint8_t)((payload >> 21) & 0x7Fu);
    dst[7] = (uint8_t)((payload >> 14) & 0x7Fu);
    dst[8] = (uint8_t)((payload >> 7) & 0x7Fu);
    dst[9] = (uint8_t)(payload & 0x7Fu);
    if (footer)
    {
        memcpy(dst + n, "3DI\x04", 4);
        memcpy(dst + n + 5u, dst + 5, 5);
        n += 10u;
    }
    return n;
}

/* Read a file to its end; *last is what the read after the end says */
static FM_Result av4_read_all(const char *path, FM_AudioInfo *info,
                              uint64_t *samples, FM_Result *again)
{
    static FM_Audio a;
    static int16_t pcm[4096];
    FM_Result r;
    size_t got = 0;

    *samples = 0;
    if (again != NULL) *again = FM_OK;
    memset(&a, 0, sizeof(a));
    r = Audio_Open(path, &a);
    if (r != FM_OK) return r;
    if (info != NULL) *info = a.info;
    while ((r = Audio_Read(&a, pcm, 4096, &got)) == FM_OK && got > 0u)
    {
        *samples += got;
    }
    *samples += got;                    /* what came with the last result */
    if (again != NULL)
    {
        got = 0;
        *again = Audio_Read(&a, pcm, 4096, &got);
        if (got != 0u) *again = FM_OK;  /* it went on after all */
    }
    (void)Audio_Close(&a);
    return r;
}

static void test_audit_v4_audio(void)
{
    FM_AudioInfo info;
    FM_Result r;
    FM_Result q;
    FM_Result again;
    uint64_t n = 0;
    size_t len;
    char msg[160];

    printf("\n[External audit v4: audio]\n");
    restore_posix_root();
    (void)av4_id3;
    (void)again;

    /* ---------------- AUD4-01 ---------------- */
    r = Audio_RegisterDecoder("AAC", &g_av2_aac_ops);

    TEST("AUD4-01: ADTS AAC behind an ID3v2 tag opens and plays");
    {
        int bad = 0;
        int footer;

        msg[0] = '\0';
        for (footer = 0; footer < 2 && !bad; footer++)
        {
            len = av4_id3(g_av4_abuf, 8192u, footer);
            len += av4_adts(g_av4_abuf + len, 40, 4, 1);
            q = File_WriteAll("/av4t.aac", g_av4_abuf, (uint32_t)len);
            memset(&info, 0, sizeof(info));
            g_av2_aac_frames = 0; g_av2_aac_claim = 0;
            if (q == FM_OK) q = av4_read_all("/av4t.aac", &info, &n, NULL);
            if (r != FM_OK || q != FM_ERR_EOF || strcmp(info.codec, "AAC") != 0 ||
                info.sample_rate != 44100u || info.channels != 1u ||
                g_av2_aac_frames != 40 || n != 40u * 64u)
            {
                bad = 1;
                (void)snprintf(msg, sizeof(msg),
                               "footer %d: result %d, codec '%s' %lu Hz %u ch, %d frames, %lu samples",
                               footer, (int)q, info.codec,
                               (unsigned long)info.sample_rate,
                               (unsigned)info.channels, g_av2_aac_frames,
                               (unsigned long)n);
            }
        }
        (void)File_Delete("/av4t.aac");
        if (bad) FAIL(msg); else PASS();
    }

    TEST("AUD4-01: a tag in front of something that is not audio is still refused");
    {
        len = av4_id3(g_av4_abuf, 512u, 0);
        memset(g_av4_abuf + len, 0x55, 4096);
        q = File_WriteAll("/av4t.aac", g_av4_abuf, (uint32_t)(len + 4096u));
        if (q == FM_OK) q = av4_read_all("/av4t.aac", NULL, &n, NULL);
        (void)File_Delete("/av4t.aac");
        (void)snprintf(msg, sizeof(msg), "result %d, %lu samples", (int)q,
                       (unsigned long)n);
        if (q == FM_OK || q == FM_ERR_EOF || n != 0u) FAIL(msg); else PASS();
    }

#if FM_USE_MP3
    TEST("AUD4-01: an MP3 behind the same tag is still an MP3");
    {
        size_t got = 0;

        len = av4_id3(g_av4_abuf, 8192u, 0);
        q = File_ReadAll("/test/vectors/tone_stereo.mp3", g_av4_abuf + len,
                         sizeof(g_av4_abuf) - len, &got);
        if (q == FM_OK) q = File_WriteAll("/av4t.mp3", g_av4_abuf, (uint32_t)(len + got));
        memset(&info, 0, sizeof(info));
        if (q == FM_OK) q = av4_read_all("/av4t.mp3", &info, &n, NULL);
        (void)File_Delete("/av4t.mp3");
        (void)snprintf(msg, sizeof(msg), "result %d, codec '%s', %u ch, %lu samples",
                       (int)q, info.codec, (unsigned)info.channels, (unsigned long)n);
        if (q != FM_ERR_EOF || strcmp(info.codec, "MP3") != 0 ||
            info.channels != 2u || n != 2304u) FAIL(msg); else PASS();
    }
#endif

    /* ---------------- AUD4-03 ---------------- */
    TEST("AUD4-03: junk of any length between AAC frames is skipped whole");
    {
        /* the scan reads in blocks: the sync word is put on every
         * position around a block edge, behind junk that ends in 0xFF */
        static const uint32_t spans[][2] = { { 1u, 20u }, { 248u, 264u },
                                             { 504u, 520u }, { 1016u, 1032u } };
        uint32_t k, junk;
        int bad = 0;

        msg[0] = '\0';
        for (k = 0; k < 4u && !bad; k++)
        {
            for (junk = spans[k][0]; junk <= spans[k][1] && !bad; junk++)
            {
                int fill;

                for (fill = 0; fill < 2 && !bad; fill++)
                {
                    len = av4_adts(g_av4_abuf, 20, 4, 1);
                    memset(g_av4_abuf + len, fill ? 0xFF : 0x11, junk);
                    len += junk;
                    len += av4_adts(g_av4_abuf + len, 20, 4, 1);
                    q = File_WriteAll("/av4j.aac", g_av4_abuf, (uint32_t)len);
                    g_av2_aac_frames = 0;
                    if (q == FM_OK) q = av4_read_all("/av4j.aac", NULL, &n, NULL);
                    if (q != FM_ERR_EOF || g_av2_aac_frames != 40)
                    {
                        bad = 1;
                        (void)snprintf(msg, sizeof(msg),
                                       "%lu bytes of 0x%02X: result %d, %d of 40 frames",
                                       (unsigned long)junk, fill ? 0xFF : 0x11,
                                       (int)q, g_av2_aac_frames);
                    }
                }
            }
        }
        (void)File_Delete("/av4j.aac");
        if (bad) FAIL(msg); else PASS();
    }

#if FM_USE_FMFS && FM_USE_SAFE_WRITE
    TEST("AUD4-03: a junk tail behind AAC is read in blocks, not a byte at a time");
    {
        /* counted at the file system: reads and seeks that reach it */
        const uint32_t tail = 96u * 1024u;
        const long budget = 1000000;
        char path[96];
        long used;

        q = av2_faulty_read_mount();
        (void)strcpy(path, g_fc_mount[0]);
        (void)strcat(path, "/tail.aac");
        len = av4_adts(g_av4_abuf, 40, 4, 1);
        memset(g_av4_abuf + len, 0, tail);
        if (q == FM_OK) q = File_WriteAll(path, g_av4_abuf, (uint32_t)(len + tail));
        g_av2_aac_frames = 0;
        av2_read_budget = budget;
        av2_seek_budget = budget;
        if (q == FM_OK) q = av4_read_all(path, NULL, &n, NULL);
        used = (budget - av2_read_budget) + (budget - av2_seek_budget);
        av2_read_budget = -1;
        av2_seek_budget = -1;
        fc_drop(0);
        (void)snprintf(msg, sizeof(msg), "result %d, %d frames, %ld calls of the file system",
                       (int)q, g_av2_aac_frames, used);
        if (q != FM_ERR_EOF || g_av2_aac_frames != 40 || used > 2000) FAIL(msg);
        else PASS();
        restore_posix_root();
    }
#endif

    /* ---------------- SUSP-A ---------------- */
    TEST("AUD4 SUSP-A: an AAC stream that changes form ends with a reason");
    {
        /* 20 mono frames at 44.1 kHz, then 20 stereo frames at 48 kHz */
        len = av4_adts(g_av4_abuf, 20, 4, 1);
        len += av4_adts(g_av4_abuf + len, 20, 3, 2);
        q = File_WriteAll("/av4c.aac", g_av4_abuf, (uint32_t)len);
        g_av2_aac_frames = 0;
        again = FM_OK;
        if (q == FM_OK) q = av4_read_all("/av4c.aac", NULL, &n, &again);
        (void)File_Delete("/av4c.aac");
        (void)snprintf(msg, sizeof(msg), "result %d then %d, %d frames, %lu samples",
                       (int)q, (int)again, g_av2_aac_frames, (unsigned long)n);
        if (q != FM_ERR_UNSUPPORTED || again != FM_ERR_UNSUPPORTED ||
            g_av2_aac_frames != 20 || n != 20u * 64u) FAIL(msg); else PASS();
    }

    TEST("AUD4 SUSP-A: stray bytes that look like another header are only junk");
    {
        /* one "frame" in another form with no frame behind it */
        len = av4_adts(g_av4_abuf, 20, 4, 1);
        len += av4_adts(g_av4_abuf + len, 1, 3, 2);
        memset(g_av4_abuf + len, 0x22, 9);
        len += 9u;
        len += av4_adts(g_av4_abuf + len, 20, 4, 1);
        q = File_WriteAll("/av4c.aac", g_av4_abuf, (uint32_t)len);
        g_av2_aac_frames = 0;
        if (q == FM_OK) q = av4_read_all("/av4c.aac", NULL, &n, NULL);
        (void)File_Delete("/av4c.aac");
        (void)snprintf(msg, sizeof(msg), "result %d, %d frames", (int)q,
                       g_av2_aac_frames);
        if (q != FM_ERR_EOF || g_av2_aac_frames != 40) FAIL(msg); else PASS();
    }
    (void)Audio_UnregisterDecoder("AAC");

    /* ---------------- noted by the save audit (section 11) ---------------- */
    TEST("WAV: a data length of 0 with sound behind it plays to the end of the file");
    {
        /* what a recorder leaves when it is never closed */
        static const uint8_t head[44] = {
            'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
            16,0,0,0, 1,0, 1,0, 0x40,0x1F,0,0, 0x80,0x3E,0,0, 2,0, 16,0,
            'd','a','t','a', 0,0,0,0
        };
        static int16_t pcm[1000];
        uint32_t k;
        int same = 1;

        memcpy(g_av4_abuf, head, sizeof(head));
        for (k = 0; k < 1000u; k++)
        {
            g_av4_abuf[44u + 2u * k] = (uint8_t)k;
            g_av4_abuf[45u + 2u * k] = (uint8_t)(k >> 8);
        }
        g_av4_abuf[2044] = 0x7F;                /* half a sample: not played */
        q = File_WriteAll("/av4z.wav", g_av4_abuf, 2045u);
        memset(&info, 0, sizeof(info));
        if (q == FM_OK)
        {
            static FM_Audio a;
            size_t got = 0;

            memset(&a, 0, sizeof(a));
            q = Audio_Open("/av4z.wav", &a);
            if (q == FM_OK)
            {
                info = a.info;
                q = Audio_Read(&a, pcm, 1000, &got);
                n = got;
                for (k = 0; k < 1000u && k < got; k++)
                {
                    if (pcm[k] != (int16_t)k) same = 0;
                }
                (void)Audio_Close(&a);
            }
        }
        (void)File_Delete("/av4z.wav");
        (void)snprintf(msg, sizeof(msg), "result %d, %lu frames in the info, %lu read, same %d",
                       (int)q, (unsigned long)info.total_frames, (unsigned long)n, same);
        if (q != FM_OK || info.total_frames != 1000u || n != 1000u || !same) FAIL(msg);
        else PASS();
    }

#if FM_USE_MP3
    /* ---------------- AUD4-02 ---------------- */
    {
        static uint8_t mono[2048], stereo[2048];
        static int16_t ref_m[1152], ref_s[2304], out[4800];
        static FM_Audio a;
        size_t lm = 0, ls = 0, got = 0;
        int k, order;

        r = File_ReadAll("/test/vectors/tone_mono.mp3", mono, sizeof(mono), &lm);
        if (r == FM_OK) r = File_ReadAll("/test/vectors/tone_stereo.mp3", stereo,
                                         sizeof(stereo), &ls);
        /* what each part sounds like on its own */
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/test/vectors/tone_mono.mp3", &a);
        if (r == FM_OK)
        {
            r = Audio_Read(&a, ref_m, 1152, &got);
            if (r == FM_OK && got != 1152u) r = FM_ERR_INTERNAL;
            (void)Audio_Close(&a);
        }
        memset(&a, 0, sizeof(a));
        if (r == FM_OK) r = Audio_Open("/test/vectors/tone_stereo.mp3", &a);
        if (r == FM_OK)
        {
            r = Audio_Read(&a, ref_s, 2304, &got);
            if (r == FM_OK && got != 2304u) r = FM_ERR_INTERNAL;
            (void)Audio_Close(&a);
        }

        for (order = 0; order < 2; order++)
        {
            /* order 0: mono then stereo, read as mono
             * order 1: stereo then mono, read as stereo */
            const uint32_t ch = (order == 0) ? 1u : 2u;
            long worst = 0;
            int pairs_differ = 0;
            size_t total = 0;

            TEST(order == 0 ? "AUD4-02: MP3, mono then stereo: the stereo part is mixed down"
                            : "AUD4-02: MP3, stereo then mono: the mono part goes to both sides");
            if (order == 0)
            {
                memcpy(g_av4_abuf, mono, lm);
                memcpy(g_av4_abuf + lm, stereo, ls);
            }
            else
            {
                memcpy(g_av4_abuf, stereo, ls);
                memcpy(g_av4_abuf + ls, mono, lm);
            }
            q = (r == FM_OK) ? File_WriteAll("/av4x.mp3", g_av4_abuf,
                                             (uint32_t)(lm + ls)) : r;
            memset(&a, 0, sizeof(a));
            memset(&info, 0, sizeof(info));
            if (q == FM_OK) q = Audio_Open("/av4x.mp3", &a);
            if (q == FM_OK)
            {
                info = a.info;
                while (total < 4800u &&
                       (q = Audio_Read(&a, out + total, 4800u - total, &got)) == FM_OK &&
                       got > 0u)
                {
                    total += got;
                }
                (void)Audio_Close(&a);
            }
            (void)File_Delete("/av4x.mp3");
            /* 1152 frames of each part, in the form of the first one.  The
             * second part is compared with what that file gives on its
             * own, from its second half on: the first samples carry the
             * tail of the part before it. */
            if (q == FM_ERR_EOF && total == 2304u * ch)
            {
                for (k = 576; k < 1152; k++)
                {
                    long d;

                    if (order == 0)
                    {
                        d = (long)out[1152 + k] -
                            ((long)ref_s[2 * k] + (long)ref_s[2 * k + 1]) / 2;
                    }
                    else
                    {
                        d = (long)out[2304 + 2 * k] - (long)ref_m[k];
                        if (out[2304 + 2 * k] != out[2304 + 2 * k + 1]) pairs_differ++;
                    }
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                }
            }
            (void)snprintf(msg, sizeof(msg),
                           "result %d, %u ch, %lu samples (want %lu), worst difference %ld, %d L/R pairs differ",
                           (int)q, (unsigned)info.channels, (unsigned long)total,
                           (unsigned long)(2304u * ch), worst, pairs_differ);
            if (q != FM_ERR_EOF || info.channels != ch || total != 2304u * ch ||
                worst > 2 || pairs_differ != 0) FAIL(msg); else PASS();
        }

        TEST("AUD4-02: an MP3 whose sample rate changes ends there with a reason");
        {
            /* 12 frames at 48 kHz, then 12 with the header of a 44.1 kHz
             * frame (626 bytes each at this bit rate) */
            size_t at = 0;

            for (k = 0; k < 12 && r == FM_OK; k++)
            {
                memcpy(g_av4_abuf + at, mono, lm);
                at += lm;
            }
            for (k = 0; k < 12 && r == FM_OK; k++)
            {
                memset(g_av4_abuf + at, 0, 626);
                memcpy(g_av4_abuf + at, mono, lm);
                g_av4_abuf[at + 2u] = (uint8_t)(g_av4_abuf[at + 2u] & 0xF3u);
                at += 626u;
            }
            q = (r == FM_OK && lm == 576u) ? File_WriteAll("/av4x.mp3", g_av4_abuf,
                                                           (uint32_t)at) : FM_ERR_INTERNAL;
            memset(&info, 0, sizeof(info));
            again = FM_OK;
            if (q == FM_OK) q = av4_read_all("/av4x.mp3", &info, &n, &again);
            (void)File_Delete("/av4x.mp3");
            (void)snprintf(msg, sizeof(msg),
                           "result %d then %d, %lu Hz, %lu samples",
                           (int)q, (int)again, (unsigned long)info.sample_rate,
                           (unsigned long)n);
            /* minimp3 gives up the last frame in front of the change */
            if (q != FM_ERR_UNSUPPORTED || again != FM_ERR_UNSUPPORTED ||
                info.sample_rate != 48000u || n < 11u * 1152u || n > 12u * 1152u)
                FAIL(msg);
            else PASS();
        }
    }
#endif /* FM_USE_MP3 */
}
#endif /* FM_USE_AUDIO */
