/*
 * Saving video: the JPEG encoder, the MJPEG AVI writer and
 * Video_Convert.  Files written here are read back with the library's
 * own AVI parser and JPEG decoder; the same files are also checked with
 * ffmpeg outside this suite.
 */

#define VSV_W 96u
#define VSV_H 64u
#define VSV_FRAMES 12u
#define VSV_RATE 8000u

static uint8_t g_vsv_rgb[VSV_W * VSV_H * 3u];
static uint8_t g_vsv_out[VSV_W * VSV_H * 3u];
static uint8_t g_vsv_jpg[VSV_W * VSV_H * 3u + 4096u];
static int16_t g_vsv_pcm[VSV_RATE];
static uint8_t g_vsv_abuf[8192];
#define VSV_BIG 70001u            /* larger than the writer's buffer, odd */
static uint8_t g_vsv_big[2][VSV_BIG + 1u];

/* A picture with smooth colour, a white box that moves with the frame
 * number and a black corner. */
static void vsv_frame(uint8_t *p, uint32_t n)
{
    uint32_t x;
    uint32_t y;

    for (y = 0u; y < VSV_H; y++)
    {
        for (x = 0u; x < VSV_W; x++)
        {
            uint8_t *q = p + 3u * (y * VSV_W + x);

            q[0] = (uint8_t)(x * 255u / (VSV_W - 1u));
            q[1] = (uint8_t)(y * 255u / (VSV_H - 1u));
            q[2] = (uint8_t)(128u + ((x + y) & 63u));
            if (x >= 4u * n && x < 4u * n + 16u && y >= 24u && y < 40u)
            {
                q[0] = q[1] = q[2] = 255u;
            }
            if (x < 12u && y < 12u)
            {
                q[0] = q[1] = q[2] = 0u;
            }
        }
    }
}

/* Mean absolute difference of two RGB888 pictures. */
static uint32_t vsv_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint64_t sum = 0u;
    size_t i;

    for (i = 0u; i < n; i++)
    {
        sum += (uint64_t)((a[i] > b[i]) ? a[i] - b[i] : b[i] - a[i]);
    }
    return (uint32_t)(sum / n);
}

static void vsv_pcm(int16_t *pcm, uint32_t first, uint32_t count)
{
    uint32_t i;

    for (i = 0u; i < count; i++)
    {
        pcm[i] = (int16_t)(((first + i) * 37u) % 20001u) - 10000;
    }
}

/* VSV_FRAMES frames at 12 fps with VSV_RATE mono sound, interleaved. */
static FM_Result vsv_write(const char *path, uint8_t quality)
{
    FM_VideoFileWriter w;
    FM_VideoSaveConfig c;
    uint32_t n;
    uint32_t done = 0u;
    FM_Result r;

    memset(&c, 0, sizeof(c));
    c.width = VSV_W;
    c.height = VSV_H;
    c.fps = 12u;
    c.quality = quality;
    c.audio_rate = VSV_RATE;
    c.audio_channels = 1u;
    r = VideoFileWriter_Open(&w, path, &c);
    for (n = 0u; r == FM_OK && n < VSV_FRAMES; n++)
    {
        uint32_t target = ((n + 1u) * VSV_RATE) / 12u;

        vsv_frame(g_vsv_rgb, n);
        r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb, FM_PIXEL_RGB888);
        if (r == FM_OK)
        {
            vsv_pcm(g_vsv_pcm, done, target - done);
            r = VideoFileWriter_WriteAudio(&w, g_vsv_pcm, target - done);
            done = target;
        }
    }
    if (r == FM_OK)
    {
        r = VideoFileWriter_Close(&w);
    }
    else
    {
        (void)VideoFileWriter_Close(&w);
    }
    return r;
}

static void vsv_invert(void *ctx, uint8_t *rgb, uint16_t w, uint16_t h,
                       uint32_t frame)
{
    size_t i;

    (void)frame;
    if (ctx != NULL)
    {
        (*(uint32_t *)ctx)++;
    }
    for (i = 0u; i < (size_t)w * h * 3u; i++)
    {
        rgb[i] = (uint8_t)(255u - rgb[i]);
    }
}

static FM_Bool vsv_cancel_at_3(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx;
    (void)total;
    return (done < 3u) ? FM_TRUE : FM_FALSE;
}

static void *vsv_no_alloc(size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    return NULL;
}

static void vsv_no_free(void *p, void *ctx)
{
    (void)p;
    (void)ctx;
}

/* ---- the file read as a stranger would --------------------------------
 * Nothing of the writer is used here, and nothing of the library's own
 * AVI reader: the bytes are walked by the rules of the format.  The
 * reader of this library does not need the index (it can read the chunks
 * in order), so a writer bug in the index or in a length went unseen as
 * long as "it plays back" was the only check. */
typedef struct
{
    uint32_t frames;        /* "00dc" chunks in the movi list           */
    uint32_t sound_bytes;   /* bytes in "01wb" chunks                   */
    uint32_t entries;       /* index entries                            */
    int      indexed;       /* the file has an "idx1" chunk             */
    uint32_t file_size;
} VsvRaw;

static uint32_t vsv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 0 = the file is right; any other number names the rule that is broken
 * (it is printed by the test). */
static int vsv_raw_check(const char *path, VsvRaw *out)
{
    static uint32_t at[4096];           /* chunk offsets from "movi"   */
    static uint32_t len[4096];
    static uint8_t  kind[4096];         /* 0 picture, 1 sound          */
    uint64_t size64 = 0u;
    uint8_t *d;
    uint32_t size;
    uint32_t got = 0u;
    uint32_t pos;
    uint32_t hdrl_end;
    uint32_t movi = 0u;                 /* position of the word "movi" */
    uint32_t movi_end = 0u;
    uint32_t idx = 0u;
    uint32_t idx_len = 0u;
    uint32_t flags;
    uint32_t total_frames;
    uint32_t streams;
    uint32_t vid_len = 0u;
    uint32_t aud_len = 0u;
    uint32_t aud_block = 0u;
    uint32_t chunks = 0u;
    uint32_t i;
    int rc = 0;
    FM_File f;

    memset(out, 0, sizeof(*out));
    if (File_SizeOf(path, &size64) != FM_OK || size64 < 224u ||
        size64 > 64u * 1024u * 1024u)
    {
        return 1;
    }
    size = (uint32_t)size64;
    out->file_size = size;
    d = (uint8_t *)malloc(size);
    if (d == NULL)
    {
        return 2;
    }
    File_Init(&f);
    if (File_OpenEx(&f, path, FM_OPEN_READ) != FM_OK ||
        File_Read(&f, d, size, &got) != FM_OK || got != size)
    {
        (void)File_Close(&f);
        free(d);
        return 3;
    }
    (void)File_Close(&f);

#define VSV_NEED(cond, code) do { if (!(cond)) { rc = (code); goto done; } } while (0)
    VSV_NEED(memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "AVI ", 4) == 0, 10);
    VSV_NEED(vsv_rd32(d + 4) == size - 8u, 11);      /* RIFF length     */
    VSV_NEED(memcmp(d + 12, "LIST", 4) == 0 &&
             memcmp(d + 20, "hdrl", 4) == 0, 12);
    hdrl_end = 20u + vsv_rd32(d + 16);
    VSV_NEED(hdrl_end < size && memcmp(d + 24, "avih", 4) == 0 &&
             vsv_rd32(d + 28) == 56u, 13);
    flags = vsv_rd32(d + 32 + 12);
    total_frames = vsv_rd32(d + 32 + 16);
    streams = vsv_rd32(d + 32 + 24);
    VSV_NEED(streams == 1u || streams == 2u, 14);
    pos = 32u + 56u;
    for (i = 0u; i < streams; i++)
    {
        uint32_t strl_end;

        VSV_NEED(pos + 12u + 64u <= hdrl_end &&
                 memcmp(d + pos, "LIST", 4) == 0 &&
                 memcmp(d + pos + 8u, "strl", 4) == 0 &&
                 memcmp(d + pos + 12u, "strh", 4) == 0, 15);
        strl_end = pos + 8u + vsv_rd32(d + pos + 4u);
        VSV_NEED(strl_end <= hdrl_end, 16);
        if (memcmp(d + pos + 20u, "vids", 4) == 0)
        {
            VSV_NEED(memcmp(d + pos + 24u, "MJPG", 4) == 0, 17);
            vid_len = vsv_rd32(d + pos + 20u + 32u);
        }
        else
        {
            VSV_NEED(memcmp(d + pos + 20u, "auds", 4) == 0, 18);
            aud_len = vsv_rd32(d + pos + 20u + 32u);
            aud_block = vsv_rd32(d + pos + 20u + 44u);
            VSV_NEED(aud_block == 2u || aud_block == 4u, 19);
        }
        pos = strl_end;
    }
    VSV_NEED(pos == hdrl_end, 20);                   /* hdrl length     */
    VSV_NEED(pos + 12u <= size && memcmp(d + pos, "LIST", 4) == 0 &&
             memcmp(d + pos + 8u, "movi", 4) == 0, 21);
    movi = pos + 8u;
    movi_end = movi + vsv_rd32(d + pos + 4u);
    VSV_NEED(movi_end <= size, 22);                  /* movi length     */

    pos = movi + 4u;
    while (pos < movi_end)
    {
        uint32_t n;

        VSV_NEED(pos + 8u <= movi_end && chunks < 4096u, 30);
        n = vsv_rd32(d + pos + 4u);
        VSV_NEED(n != 0u && pos + 8u + n <= movi_end, 31);
        if (memcmp(d + pos, "00dc", 4) == 0)
        {
            VSV_NEED(n >= 4u && d[pos + 8u] == 0xFFu &&
                     d[pos + 9u] == 0xD8u, 32);       /* a JPEG starts   */
            kind[chunks] = 0u;
            out->frames++;
        }
        else
        {
            VSV_NEED(memcmp(d + pos, "01wb", 4) == 0 && streams == 2u, 33);
            VSV_NEED((n % aud_block) == 0u, 34);
            kind[chunks] = 1u;
            out->sound_bytes += n;
        }
        at[chunks] = pos - movi;
        len[chunks] = n;
        chunks++;
        pos += 8u + n + (n & 1u);                    /* pad to even     */
    }
    VSV_NEED(pos == movi_end, 35);       /* the list ends on a chunk end */

    if (pos < size)
    {
        VSV_NEED(pos + 8u <= size && memcmp(d + pos, "idx1", 4) == 0, 40);
        idx = pos + 8u;
        idx_len = vsv_rd32(d + pos + 4u);
        VSV_NEED(idx + idx_len == size, 41);         /* nothing after   */
        VSV_NEED(idx_len == chunks * 16u, 42);
        out->indexed = 1;
        out->entries = idx_len / 16u;
        for (i = 0u; i < chunks; i++)
        {
            const uint8_t *e = d + idx + 16u * i;

            VSV_NEED(memcmp(e, kind[i] ? "01wb" : "00dc", 4) == 0, 43);
            VSV_NEED(vsv_rd32(e + 8) == at[i], 44);  /* where the chunk is */
            VSV_NEED(vsv_rd32(e + 12) == len[i], 45);
            /* and the entry really lands on that chunk's header */
            VSV_NEED(memcmp(d + movi + vsv_rd32(e + 8), e, 4) == 0 &&
                     vsv_rd32(d + movi + vsv_rd32(e + 8) + 4u) ==
                         vsv_rd32(e + 12), 46);
        }
    }
    VSV_NEED(((flags & 0x10u) != 0u) == (out->indexed != 0), 50);
    VSV_NEED(total_frames == out->frames, 51);
    VSV_NEED(vid_len == out->frames, 52);
    if (streams == 2u)
    {
        VSV_NEED(aud_len == out->sound_bytes / aud_block, 53);
    }
#undef VSV_NEED
done:
    free(d);
    return rc;
}

/* What a reset or a pulled card leaves behind: the handles are dropped,
 * nothing is finished, what was still in the writer's buffer is gone. */
static void vsv_abandon(FM_VideoFileWriter *w)
{
    (void)File_Close(&w->file);
    (void)File_Close(&w->index);
    FM_Free(w->buf);
    memset(w, 0, sizeof(*w));
}

/* `count` large stand-in "JPEG" pictures with 800 sound frames each. */
static FM_Result vsv_write_big(FM_VideoFileWriter *w, uint32_t count)
{
    FM_Result r = FM_OK;
    uint32_t n;

    for (n = 0u; n < 2u; n++)
    {
        uint32_t i;

        for (i = 0u; i < VSV_BIG; i++)
        {
            g_vsv_big[n][i] = (uint8_t)(i * 7u + n * 31u);
        }
        g_vsv_big[n][0] = 0xFFu;
        g_vsv_big[n][1] = 0xD8u;
    }
    vsv_pcm(g_vsv_pcm, 0u, 800u);
    for (n = 0u; r == FM_OK && n < count; n++)
    {
        r = VideoFileWriter_WriteJpeg(w, g_vsv_big[n & 1u], VSV_BIG);
        if (r == FM_OK)
        {
            r = VideoFileWriter_WriteAudio(w, g_vsv_pcm, 800u);
        }
    }
    return r;
}

static void test_video_save(void)
{
    char msg[160];
    FM_Result r;
    FM_Bool exists = FM_FALSE;

    printf("\n[Saving video: JPEG encoder, MJPEG AVI writer, conversion]\n");
    restore_posix_root();
    (void)Directory_DeleteRecursive("/vsv");
    (void)Directory_Create("/vsv");

    TEST("JPEG: white is 255 and black is 0 after a round trip");
    {
        /* The old encoder wrote studio-range colour (16..235), which
         * every viewer shows as grey-ish white and washed-out black. */
        FM_Image img;
        FM_Image dec;
        size_t len = 0u;
        uint32_t i;

        memset(&img, 0, sizeof(img));
        img.width = 32u;
        img.height = 16u;
        img.format = FM_PIXEL_RGB888;
        img.data = g_vsv_rgb;
        img.data_capacity = 32u * 16u * 3u;
        img.data_size = img.data_capacity;
        for (i = 0u; i < 32u * 16u; i++)
        {
            uint8_t v = ((i % 32u) < 16u) ? 255u : 0u;

            g_vsv_rgb[3u * i] = v;
            g_vsv_rgb[3u * i + 1u] = v;
            g_vsv_rgb[3u * i + 2u] = v;
        }
        r = Image_EncodeJPEG(&img, g_vsv_jpg, sizeof(g_vsv_jpg), &len);
        memset(&dec, 0, sizeof(dec));
        if (r == FM_OK)
        {
            r = Image_DecodeJPEG(g_vsv_jpg, len, &dec, g_vsv_out,
                                 sizeof(g_vsv_out));
        }
        if (r == FM_OK && dec.width == 32u && dec.height == 16u &&
            g_vsv_out[3u * (5u * 32u + 4u)] >= 252u &&
            g_vsv_out[3u * (5u * 32u + 4u) + 1u] >= 252u &&
            g_vsv_out[3u * (5u * 32u + 4u) + 2u] >= 252u &&
            g_vsv_out[3u * (5u * 32u + 27u)] <= 3u &&
            g_vsv_out[3u * (5u * 32u + 27u) + 1u] <= 3u &&
            g_vsv_out[3u * (5u * 32u + 27u) + 2u] <= 3u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d white=%u black=%u", (int)r,
                     (unsigned)g_vsv_out[3u * (5u * 32u + 4u)],
                     (unsigned)g_vsv_out[3u * (5u * 32u + 27u)]);
            FAIL(msg);
        }
    }

    TEST("JPEG: quality and 4:2:0 change size and closeness as expected");
    {
        static const uint8_t q[3] = { 20u, 60u, 95u };
        size_t len[3] = { 0u, 0u, 0u };
        uint32_t err[3] = { 0u, 0u, 0u };
        size_t len420 = 0u;
        FM_Image img;
        FM_Image dec;
        FM_JpegEncodeOptions o;
        uint32_t i;

        memset(&o, 0, sizeof(o));
        vsv_frame(g_vsv_rgb, 3u);
        memset(&img, 0, sizeof(img));
        img.width = VSV_W;
        img.height = VSV_H;
        img.format = FM_PIXEL_RGB888;
        img.data = g_vsv_rgb;
        img.data_capacity = sizeof(g_vsv_rgb);
        img.data_size = sizeof(g_vsv_rgb);
        r = FM_OK;
        for (i = 0u; r == FM_OK && i < 3u; i++)
        {
            o.quality = q[i];
            o.subsample = 0u;
            r = Image_EncodeJPEGEx(&img, &o, g_vsv_jpg, sizeof(g_vsv_jpg),
                                   &len[i]);
            memset(&dec, 0, sizeof(dec));
            if (r == FM_OK)
            {
                r = Image_DecodeJPEG(g_vsv_jpg, len[i], &dec, g_vsv_out,
                                     sizeof(g_vsv_out));
            }
            err[i] = vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb));
        }
        o.quality = 60u;
        o.subsample = 1u;
        if (r == FM_OK)
        {
            r = Image_EncodeJPEGEx(&img, &o, g_vsv_jpg, sizeof(g_vsv_jpg),
                                   &len420);
        }
        memset(&dec, 0, sizeof(dec));
        if (r == FM_OK)
        {
            r = Image_DecodeJPEG(g_vsv_jpg, len420, &dec, g_vsv_out,
                                 sizeof(g_vsv_out));
        }
        if (r == FM_OK && len[0] < len[1] && len[1] < len[2] &&
            err[0] >= err[1] && err[1] >= err[2] && err[2] <= 2u &&
            len420 < len[1] && dec.width == VSV_W && dec.height == VSV_H &&
            vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb)) <= 6u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "r=%d len %u %u %u (420 %u) err %u %u %u", (int)r,
                     (unsigned)len[0], (unsigned)len[1], (unsigned)len[2],
                     (unsigned)len420, (unsigned)err[0], (unsigned)err[1],
                     (unsigned)err[2]);
            FAIL(msg);
        }
    }

    TEST("JPEG: every pixel format gives the same picture");
    {
        static const FM_PixelFormat fmts[] = {
            FM_PIXEL_BGR888, FM_PIXEL_RGBA8888, FM_PIXEL_BGRA8888,
            FM_PIXEL_RGB565, FM_PIXEL_BGR565
        };
        static uint8_t px[VSV_W * VSV_H * 4u];
        FM_Image img;
        FM_Image dec;
        uint32_t bad = 0u;
        uint32_t k;
        uint32_t i;

        vsv_frame(g_vsv_rgb, 5u);
        for (k = 0u; k < sizeof(fmts) / sizeof(fmts[0]); k++)
        {
            size_t bpp = Image_BytesPerPixel(fmts[k]);
            size_t len = 0u;

            for (i = 0u; i < VSV_W * VSV_H; i++)
            {
                uint8_t R = g_vsv_rgb[3u * i];
                uint8_t G = g_vsv_rgb[3u * i + 1u];
                uint8_t B = g_vsv_rgb[3u * i + 2u];

                switch (fmts[k])
                {
                case FM_PIXEL_BGR888:
                    px[3u * i] = B; px[3u * i + 1u] = G; px[3u * i + 2u] = R;
                    break;
                case FM_PIXEL_RGBA8888:
                    px[4u * i] = R; px[4u * i + 1u] = G; px[4u * i + 2u] = B;
                    px[4u * i + 3u] = 7u;
                    break;
                case FM_PIXEL_BGRA8888:
                    px[4u * i] = B; px[4u * i + 1u] = G; px[4u * i + 2u] = R;
                    px[4u * i + 3u] = 7u;
                    break;
                default:
                {
                    uint16_t hi = (fmts[k] == FM_PIXEL_RGB565) ? R : B;
                    uint16_t lo = (fmts[k] == FM_PIXEL_RGB565) ? B : R;
                    uint16_t v = (uint16_t)(((hi >> 3) << 11) |
                                            ((uint16_t)(G >> 2) << 5) |
                                            (lo >> 3));

                    px[2u * i] = (uint8_t)(v & 0xFFu);
                    px[2u * i + 1u] = (uint8_t)(v >> 8);
                    break;
                }
                }
            }
            memset(&img, 0, sizeof(img));
            img.width = VSV_W;
            img.height = VSV_H;
            img.format = fmts[k];
            img.data = px;
            img.data_capacity = VSV_W * VSV_H * bpp;
            img.data_size = img.data_capacity;
            r = Image_EncodeJPEG(&img, g_vsv_jpg, sizeof(g_vsv_jpg), &len);
            memset(&dec, 0, sizeof(dec));
            if (r == FM_OK)
            {
                r = Image_DecodeJPEG(g_vsv_jpg, len, &dec, g_vsv_out,
                                     sizeof(g_vsv_out));
            }
            /* 565 loses the low bits of every channel: allow for it */
            if (r != FM_OK ||
                vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb)) >
                    ((bpp == 2u) ? 5u : 2u))
            {
                bad++;
            }
        }
        if (bad == 0u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "%u formats differ", (unsigned)bad);
            FAIL(msg);
        }
    }

    TEST("JPEG / video: 16-bit pixels stored high byte first (SPI LCD)");
    {
        /* The frame buffers of the players are byte-swapped for SPI
         * panels.  With swap16 the encoder reads them as they are; the
         * result must be byte for byte the JPEG of the unswapped buffer. */
        static uint8_t le[VSV_W * VSV_H * 2u];
        static uint8_t be[VSV_W * VSV_H * 2u];
        static uint8_t jpg_be[VSV_W * VSV_H * 3u + 4096u];
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_JpegEncodeOptions o;
        FM_Video v;
        FM_Image img;
        size_t len_le = 0u;
        size_t len_be = 0u;
        size_t len_wrong = 0u;
        uint32_t i;
        uint32_t d0 = 999u;
        uint32_t d1 = 999u;

        vsv_frame(g_vsv_rgb, 2u);
        for (i = 0u; i < VSV_W * VSV_H; i++)
        {
            uint16_t px = (uint16_t)(((g_vsv_rgb[3u * i] >> 3) << 11) |
                                     ((uint16_t)(g_vsv_rgb[3u * i + 1u] >> 2)
                                          << 5) |
                                     (g_vsv_rgb[3u * i + 2u] >> 3));

            le[2u * i] = (uint8_t)(px & 0xFFu);
            le[2u * i + 1u] = (uint8_t)(px >> 8);
            be[2u * i] = (uint8_t)(px >> 8);
            be[2u * i + 1u] = (uint8_t)(px & 0xFFu);
        }
        memset(&img, 0, sizeof(img));
        img.width = VSV_W;
        img.height = VSV_H;
        img.format = FM_PIXEL_RGB565;
        img.data_capacity = sizeof(le);
        img.data_size = sizeof(le);
        memset(&o, 0, sizeof(o));
        o.quality = 90u;
        img.data = le;
        r = Image_EncodeJPEGEx(&img, &o, g_vsv_jpg, sizeof(g_vsv_jpg),
                               &len_le);
        img.data = be;
        if (r == FM_OK)
        {
            r = Image_EncodeJPEGEx(&img, &o, jpg_be, sizeof(jpg_be),
                                   &len_wrong);
        }
        o.swap16 = 1u;
        if (r == FM_OK)
        {
            r = Image_EncodeJPEGEx(&img, &o, jpg_be, sizeof(jpg_be),
                                   &len_be);
        }

        memset(&c, 0, sizeof(c));
        c.width = (uint16_t)VSV_W;
        c.height = (uint16_t)VSV_H;
        c.fps = 10u;
        c.quality = 90u;
        if (r == FM_OK) r = VideoFileWriter_Open(&w, "/vsv/swap.avi", &c);
        if (r == FM_OK) r = VideoFileWriter_WriteFrame(&w, le,
                                                       FM_PIXEL_RGB565);
        if (r == FM_OK) r = VideoFileWriter_WriteFrameSwap16(&w, be,
                                                             FM_PIXEL_RGB565);
        /* not a 16-bit format: the same as WriteFrame */
        if (r == FM_OK) r = VideoFileWriter_WriteFrameSwap16(&w, g_vsv_rgb,
                                                             FM_PIXEL_RGB888);
        if (r == FM_OK)
        {
            r = VideoFileWriter_Close(&w);
        }
        else
        {
            (void)VideoFileWriter_Close(&w);
        }
        if (r == FM_OK) r = Video_Open("/vsv/swap.avi", &v);
        if (r == FM_OK)
        {
            r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
            if (r == FM_OK)
            {
                r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
                d0 = vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb));
            }
            if (r == FM_OK)
            {
                r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
                d1 = vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb));
            }
            (void)Video_Close(&v);
        }
        if (r == FM_OK && len_le != 0u && len_be == len_le &&
            memcmp(g_vsv_jpg, jpg_be, len_le) == 0 && len_wrong != 0u &&
            d0 <= 6u && d1 <= 3u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d le=%u be=%u d0=%u d1=%u", (int)r,
                     (unsigned)len_le, (unsigned)len_be, (unsigned)d0,
                     (unsigned)d1);
            FAIL(msg);
        }
    }

    TEST("Video save: the AVI opens as MJPEG with the right size and rate");
    {
        FM_Video v;
        FM_VideoInfo vi;
        FM_AudioTrackInfo ai;
        FM_Bool idx = FM_TRUE;

        memset(&vi, 0, sizeof(vi));
        memset(&ai, 0, sizeof(ai));
        r = vsv_write("/vsv/a.avi", 85u);
        if (r == FM_OK)
        {
            r = Video_Open("/vsv/a.avi", &v);
            if (r == FM_OK)
            {
                (void)Video_GetInfo(&v, &vi);
                (void)Video_GetAudioInfo(&v, &ai);
                (void)Video_Close(&v);
            }
        }
        (void)File_Exists("/vsv/a.avi.idx", &idx);
        if (r == FM_OK && strcmp(vi.container, "AVI") == 0 &&
            vi.codec == FM_VIDEO_CODEC_MJPEG && vi.width == VSV_W &&
            vi.height == VSV_H && vi.fps == 12u &&
            vi.frame_count == VSV_FRAMES &&
            ai.codec == FM_VIDEO_AUDIO_PCM && ai.sample_rate == VSV_RATE &&
            ai.channels == 1u && ai.decodable && !idx)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "r=%d %s codec=%d %ux%u fps=%u n=%u audio=%d idx=%d",
                     (int)r, vi.container, (int)vi.codec,
                     (unsigned)vi.width, (unsigned)vi.height,
                     (unsigned)vi.fps, (unsigned)vi.frame_count,
                     (int)ai.codec, (int)idx);
            FAIL(msg);
        }
    }

    TEST("Video save: every frame decodes to the picture that was given");
    {
        FM_Video v;
        uint32_t n;
        uint32_t worst = 0u;
        uint32_t decoded = 0u;

        r = Video_Open("/vsv/a.avi", &v);
        for (n = 0u; r == FM_OK && n < VSV_FRAMES; n++)
        {
            r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
            if (r == FM_OK)
            {
                uint32_t d;

                vsv_frame(g_vsv_rgb, n);
                d = vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb));
                if (d > worst)
                {
                    worst = d;
                }
                decoded++;
            }
        }
        if (r == FM_OK)
        {
            FM_Result e = Video_DecodeFrame(&v, g_vsv_out,
                                            sizeof(g_vsv_out));

            if (e != FM_ERR_EOF)
            {
                r = FM_ERR_INTERNAL;
            }
        }
        (void)Video_Close(&v);
        if (r == FM_OK && decoded == VSV_FRAMES && worst <= 6u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d decoded=%u worst=%u", (int)r,
                     (unsigned)decoded, (unsigned)worst);
            FAIL(msg);
        }
    }

    TEST("Video save: the sound track comes back sample for sample");
    {
        FM_Video v;
        uint32_t total = 0u;
        uint32_t bad = 0u;

        r = Video_Open("/vsv/a.avi", &v);
        while (r == FM_OK)
        {
            size_t got = 0u;
            size_t i;

            r = Video_ReadAudioFrame(&v, g_vsv_abuf, sizeof(g_vsv_abuf),
                                     &got);
            if (r != FM_OK)
            {
                break;
            }
            for (i = 0u; i + 1u < got; i += 2u)
            {
                int16_t s = (int16_t)((uint16_t)g_vsv_abuf[i] |
                                      ((uint16_t)g_vsv_abuf[i + 1u] << 8));
                int16_t want;

                vsv_pcm(&want, total, 1u);
                if (s != want)
                {
                    bad++;
                }
                total++;
            }
        }
        (void)Video_Close(&v);
        if (r == FM_ERR_EOF && total == VSV_RATE && bad == 0u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d total=%u bad=%u", (int)r,
                     (unsigned)total, (unsigned)bad);
            FAIL(msg);
        }
    }

    TEST("Video save: ready JPEG frames are stored as they are");
    {
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_Video v;
        FM_VideoInfo vi;
        FM_Image img;
        FM_JpegEncodeOptions o;
        size_t len = 0u;
        size_t got = 0u;
        FM_Result bad_jpeg;

        memset(&o, 0, sizeof(o));
        FM_Result no_track;
        static uint8_t back[sizeof(g_vsv_jpg)];

        memset(&vi, 0, sizeof(vi));
        memset(&c, 0, sizeof(c));
        c.width = VSV_W;
        c.height = VSV_H;
        c.fps = 5u;
        vsv_frame(g_vsv_rgb, 2u);
        memset(&img, 0, sizeof(img));
        img.width = VSV_W;
        img.height = VSV_H;
        img.format = FM_PIXEL_RGB888;
        img.data = g_vsv_rgb;
        img.data_capacity = sizeof(g_vsv_rgb);
        img.data_size = sizeof(g_vsv_rgb);
        o.quality = 70u;
        o.subsample = 1u;
        r = Image_EncodeJPEGEx(&img, &o, g_vsv_jpg, sizeof(g_vsv_jpg), &len);
        if (r == FM_OK)
        {
            r = VideoFileWriter_Open(&w, "/vsv/j.avi", &c);
        }
        bad_jpeg = VideoFileWriter_WriteJpeg(&w, g_vsv_rgb, 100u);
        no_track = VideoFileWriter_WriteAudio(&w, g_vsv_pcm, 10u);
        if (r == FM_OK) r = VideoFileWriter_WriteJpeg(&w, g_vsv_jpg, len);
        if (r == FM_OK) r = VideoFileWriter_WriteJpeg(&w, g_vsv_jpg, len);
        if (r == FM_OK) r = VideoFileWriter_Close(&w);
        if (r == FM_OK) r = Video_Open("/vsv/j.avi", &v);
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            r = Video_ReadFrame(&v, back, sizeof(back), &got);
            (void)Video_Close(&v);
        }
        if (r == FM_OK && bad_jpeg == FM_ERR_INVALID_FORMAT &&
            no_track == FM_ERR_UNSUPPORTED && vi.frame_count == 2u &&
            got == len && memcmp(back, g_vsv_jpg, len) == 0)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d bad=%d notrack=%d n=%u %u/%u",
                     (int)r, (int)bad_jpeg, (int)no_track,
                     (unsigned)vi.frame_count, (unsigned)got, (unsigned)len);
            FAIL(msg);
        }
    }

    TEST("Video save: wrong arguments and closed writers are refused");
    {
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_Result a;
        FM_Result b;
        FM_Result d;
        FM_Result e;
        FM_Result f;

        memset(&c, 0, sizeof(c));
        c.width = 16u;
        c.height = 16u;
        (void)File_Delete("/vsv/bad.avi");
        a = VideoFileWriter_Open(&w, "/vsv/bad.avi", &c);        /* fps 0 */
        c.fps = 10u;
        c.audio_rate = 8000u;
        c.audio_channels = 3u;
        b = VideoFileWriter_Open(&w, "/vsv/bad.avi", &c);
        d = VideoFileWriter_WriteFrame(&w, g_vsv_rgb, FM_PIXEL_RGB888);
        e = VideoFileWriter_Close(&w);
        f = VideoFileWriter_Open(NULL, "/vsv/bad.avi", &c);
        (void)File_Exists("/vsv/bad.avi", &exists);
        if (a == FM_ERR_INVALID_ARGUMENT && b == FM_ERR_INVALID_ARGUMENT &&
            d == FM_ERR_NOT_OPEN && e == FM_ERR_NOT_OPEN &&
            f == FM_ERR_INVALID_ARGUMENT && !exists)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "%d %d %d %d %d exists=%d", (int)a,
                     (int)b, (int)d, (int)e, (int)f, (int)exists);
            FAIL(msg);
        }
    }

    TEST("Video save: out of memory -> clean error, no file");
    {
        FM_Allocator saved = *FM_Allocator_Get();
        FM_Allocator none;
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_Result a;
        FM_Result b;
        FM_Bool e2 = FM_TRUE;

        memset(&c, 0, sizeof(c));
        c.width = 16u;
        c.height = 16u;
        c.fps = 10u;
        memset(&none, 0, sizeof(none));
        none.alloc = vsv_no_alloc;
        none.free = vsv_no_free;
        (void)File_Delete("/vsv/nomem.avi");
        (void)FM_Allocator_Set(&none);
        a = VideoFileWriter_Open(&w, "/vsv/nomem.avi", &c);
        b = Video_Convert("/vsv/a.avi", "/vsv/nomem2.avi", NULL, NULL);
        (void)FM_Allocator_Set(&saved);
        (void)File_Exists("/vsv/nomem.avi", &exists);
        (void)File_Exists("/vsv/nomem2.avi", &e2);
        if (a == FM_ERR_NO_MEMORY && b == FM_ERR_NO_MEMORY && !exists && !e2)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "%d %d exists=%d,%d", (int)a, (int)b,
                     (int)exists, (int)e2);
            FAIL(msg);
        }
    }

    TEST("Video save: a repeated frame is the same picture again");
    {
        /* Live capture that missed frames fills them with the previous
         * picture; the copy is read back from the file being written. */
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_Video v;
        FM_VideoInfo vi;
        FM_Result early;
        uint32_t n;
        uint32_t worst = 0u;
        size_t got = 0u;
        int same = 1;
        static const uint8_t want[5] = { 0u, 0u, 0u, 3u, 3u };

        memset(&c, 0, sizeof(c));
        memset(&vi, 0, sizeof(vi));
        c.width = (uint16_t)VSV_W;
        c.height = (uint16_t)VSV_H;
        c.fps = 10u;
        c.audio_rate = VSV_RATE;
        c.audio_channels = 1u;
        for (n = 0u; n < VSV_BIG; n++)
        {
            g_vsv_big[0][n] = (uint8_t)((n * 7u) ^ (n >> 8));
        }
        g_vsv_big[0][0] = 0xFFu;
        g_vsv_big[0][1] = 0xD8u;

        r = VideoFileWriter_Open(&w, "/vsv/rep.avi", &c);
        early = VideoFileWriter_RepeatFrame(&w, 1u);     /* nothing yet */
        vsv_frame(g_vsv_rgb, 0u);
        if (r == FM_OK) r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb,
                                                       FM_PIXEL_RGB888);
        if (r == FM_OK) r = VideoFileWriter_RepeatFrame(&w, 0u);
        if (r == FM_OK) r = VideoFileWriter_RepeatFrame(&w, 2u);
        vsv_pcm(g_vsv_pcm, 0u, 800u);
        if (r == FM_OK) r = VideoFileWriter_WriteAudio(&w, g_vsv_pcm, 800u);
        vsv_frame(g_vsv_rgb, 3u);
        if (r == FM_OK) r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb,
                                                       FM_PIXEL_RGB888);
        /* the sound chunk written in between must not be what is copied */
        if (r == FM_OK) r = VideoFileWriter_WriteAudio(&w, g_vsv_pcm, 800u);
        if (r == FM_OK) r = VideoFileWriter_RepeatFrame(&w, 1u);
        if (r == FM_OK) r = VideoFileWriter_WriteJpeg(&w, g_vsv_big[0],
                                                      VSV_BIG);
        if (r == FM_OK) r = VideoFileWriter_RepeatFrame(&w, 2u);
        if (r == FM_OK && w.frames != 8u) r = FM_ERR_INTERNAL;
        if (r == FM_OK)
        {
            r = VideoFileWriter_Close(&w);
        }
        else
        {
            (void)VideoFileWriter_Close(&w);
        }

        if (r == FM_OK) r = Video_Open("/vsv/rep.avi", &v);
        if (r == FM_OK)
        {
            r = Video_GetInfo(&v, &vi);
            for (n = 0u; r == FM_OK && n < 5u; n++)
            {
                r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
                if (r == FM_OK)
                {
                    uint32_t d;

                    vsv_frame(g_vsv_rgb, want[n]);
                    d = vsv_diff(g_vsv_rgb, g_vsv_out, sizeof(g_vsv_rgb));
                    if (d > worst)
                    {
                        worst = d;
                    }
                }
            }
            for (n = 5u; r == FM_OK && n < 8u; n++)
            {
                memset(g_vsv_big[1], 0, sizeof(g_vsv_big[1]));
                r = Video_ReadFrame(&v, g_vsv_big[1], sizeof(g_vsv_big[1]),
                                    &got);
                if (r == FM_OK &&
                    (got != VSV_BIG ||
                     memcmp(g_vsv_big[0], g_vsv_big[1], VSV_BIG) != 0))
                {
                    same = 0;
                }
            }
            (void)Video_Close(&v);
        }
        if (r == FM_OK && early == FM_ERR_NOT_READY && vi.frame_count == 8u &&
            worst <= 6u && same)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "r=%d early=%d frames=%u worst=%u same=%d got=%u", (int)r,
                     (int)early, (unsigned)vi.frame_count, (unsigned)worst,
                     same, (unsigned)got);
            FAIL(msg);
        }
    }

    TEST("Video convert: every frame goes through the callback, half size");
    {
        FM_VideoConvertConfig cc;
        FM_Video v;
        FM_VideoInfo vi;
        FM_AudioTrackInfo ai;
        uint32_t calls = 0u;
        uint32_t frames = 0u;
        uint8_t centre[3] = { 0u, 0u, 0u };

        memset(&vi, 0, sizeof(vi));
        memset(&ai, 0, sizeof(ai));
        memset(&cc, 0, sizeof(cc));
        cc.width = VSV_W / 2u;
        cc.height = VSV_H / 2u;
        cc.quality = 90u;
        cc.process = vsv_invert;
        cc.process_ctx = &calls;
        r = Video_Convert("/vsv/a.avi", "/vsv/b.avi", &cc, &frames);
        if (r == FM_OK)
        {
            r = Video_Open("/vsv/b.avi", &v);
        }
        if (r == FM_OK)
        {
            (void)Video_GetInfo(&v, &vi);
            (void)Video_GetAudioInfo(&v, &ai);
            r = Video_DecodeFrame(&v, g_vsv_out, sizeof(g_vsv_out));
            /* frame 0: the white box covers x 0..15, y 24..39 of the
             * source = x 0..7, y 12..19 here; inverted it is black. */
            memcpy(centre, g_vsv_out + 3u * (15u * (VSV_W / 2u) + 3u), 3u);
            (void)Video_Close(&v);
        }
        if (r == FM_OK && frames == VSV_FRAMES && calls == VSV_FRAMES &&
            vi.width == VSV_W / 2u && vi.height == VSV_H / 2u &&
            vi.frame_count == VSV_FRAMES && vi.fps == 12u &&
            ai.sample_rate == VSV_RATE && centre[0] < 30u &&
            centre[1] < 30u && centre[2] < 30u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "r=%d frames=%u calls=%u %ux%u n=%u px=%u,%u,%u",
                     (int)r, (unsigned)frames, (unsigned)calls,
                     (unsigned)vi.width, (unsigned)vi.height,
                     (unsigned)vi.frame_count, (unsigned)centre[0],
                     (unsigned)centre[1], (unsigned)centre[2]);
            FAIL(msg);
        }
    }

    TEST("Video convert: cancel and errors leave no half file");
    {
        FM_VideoConvertConfig cc;
        FM_Result a;
        FM_Result b;
        FM_Result c;
        FM_Bool e1 = FM_TRUE;
        FM_Bool e2 = FM_TRUE;
        FM_Bool e3 = FM_TRUE;
        FM_Bool src = FM_FALSE;

        memset(&cc, 0, sizeof(cc));
        cc.progress = vsv_cancel_at_3;
        a = Video_Convert("/vsv/a.avi", "/vsv/c.avi", &cc, NULL);
        b = Video_Convert("/vsv/missing.avi", "/vsv/d.avi", NULL, NULL);
        c = Video_Convert("/vsv/a.avi", "/vsv/A.AVI", NULL, NULL);
        (void)File_Exists("/vsv/c.avi", &e1);
        (void)File_Exists("/vsv/c.avi.idx", &e2);
        (void)File_Exists("/vsv/d.avi", &e3);
        (void)File_Exists("/vsv/a.avi", &src);
        if (a == FM_ERR_CANCELLED && b != FM_OK &&
            c == FM_ERR_INVALID_ARGUMENT && !e1 && !e2 && !e3 && src)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "%d %d %d exists %d %d %d src=%d",
                     (int)a, (int)b, (int)c, (int)e1, (int)e2, (int)e3,
                     (int)src);
            FAIL(msg);
        }
    }

    TEST("Video save: the file byte by byte: lengths, counts, every index entry");
    {
        /* /vsv/a.avi: pictures and sound; /vsv/j.avi: pictures only */
        VsvRaw a;
        VsvRaw j;
        int ra = vsv_raw_check("/vsv/a.avi", &a);
        int rj = vsv_raw_check("/vsv/j.avi", &j);

        if (ra == 0 && rj == 0 && a.indexed && j.indexed &&
            a.frames == VSV_FRAMES && a.sound_bytes == VSV_RATE * 2u &&
            a.entries == 2u * VSV_FRAMES && j.frames == 2u &&
            j.entries == 2u && j.sound_bytes == 0u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "rule a=%d j=%d frames %u/%u sound %u entries %u/%u",
                     ra, rj, (unsigned)a.frames, (unsigned)j.frames,
                     (unsigned)a.sound_bytes, (unsigned)a.entries,
                     (unsigned)j.entries);
            FAIL(msg);
        }
    }

    TEST("Video save: a second Open on an open writer is refused");
    {
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        VsvRaw raw;
        FM_Result second = FM_OK;
        FM_Result same = FM_OK;
        FM_Bool other = FM_TRUE;
        int rc = -1;

        memset(&w, 0, sizeof(w));
        memset(&c, 0, sizeof(c));
        c.width = VSV_W;
        c.height = VSV_H;
        c.fps = 10u;
        (void)File_Delete("/vsv/two.avi");
        r = VideoFileWriter_Open(&w, "/vsv/one.avi", &c);
        vsv_frame(g_vsv_rgb, 1u);
        if (r == FM_OK)
        {
            r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb, FM_PIXEL_RGB888);
        }
        if (r == FM_OK)
        {
            second = VideoFileWriter_Open(&w, "/vsv/two.avi", &c);
            same = VideoFileWriter_Open(&w, "/vsv/one.avi", &c);
            /* the first file goes on as if nothing had happened */
            r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb, FM_PIXEL_RGB888);
        }
        if (r == FM_OK)
        {
            r = VideoFileWriter_Close(&w);
        }
        (void)File_Exists("/vsv/two.avi", &other);
        if (r == FM_OK)
        {
            rc = vsv_raw_check("/vsv/one.avi", &raw);
            /* and after Close the same writer opens again */
            r = VideoFileWriter_Open(&w, "/vsv/one.avi", &c);
            if (r == FM_OK)
            {
                r = VideoFileWriter_Close(&w);
            }
        }
        if (r == FM_OK && second == FM_ERR_BUSY && same == FM_ERR_BUSY &&
            !other && rc == 0 && raw.frames == 2u)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d second=%d same=%d other=%d "
                     "rule=%d frames=%u", (int)r, (int)second, (int)same,
                     (int)other, rc, (unsigned)raw.frames);
            FAIL(msg);
        }
    }

    TEST("Video save: never closed -> a valid file up to the checkpoint");
    {
        /* 20 pictures of 70 KB: the 1 MB checkpoint falls after the 15th.
         * Then the "power is lost". */
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        FM_Video v;
        FM_VideoInfo vi;
        VsvRaw raw;
        FM_Bool idx = FM_FALSE;
        uint64_t size = 0u;
        uint8_t head[64];
        uint32_t got = 0u;
        uint32_t riff = 0u;
        uint32_t flags = 1u;
        uint32_t counted = 0u;
        uint32_t decoded = 0u;
        FM_File f;

        memset(&w, 0, sizeof(w));
        memset(&c, 0, sizeof(c));
        memset(&vi, 0, sizeof(vi));
        memset(head, 0, sizeof(head));
        c.width = VSV_W;
        c.height = VSV_H;
        c.fps = 10u;
        c.audio_rate = VSV_RATE;
        c.audio_channels = 1u;
        r = VideoFileWriter_Open(&w, "/vsv/cut.avi", &c);
        if (r == FM_OK)
        {
            r = vsv_write_big(&w, 20u);
        }
        vsv_abandon(&w);
        (void)File_Exists("/vsv/cut.avi.idx", &idx);
        (void)File_SizeOf("/vsv/cut.avi", &size);
        File_Init(&f);
        if (r == FM_OK && File_OpenEx(&f, "/vsv/cut.avi", FM_OPEN_READ) == FM_OK)
        {
            (void)File_Read(&f, head, sizeof(head), &got);
            (void)File_Close(&f);
            riff = vsv_rd32(head + 4);
            flags = vsv_rd32(head + 32 + 12);
            counted = vsv_rd32(head + 32 + 16);
        }
        /* the library's own reader takes the file as it is */
        if (r == FM_OK)
        {
            r = Video_Open("/vsv/cut.avi", &v);
            if (r == FM_OK)
            {
                size_t n = 0u;

                (void)Video_GetInfo(&v, &vi);
                while (Video_ReadFrame(&v, g_vsv_big[0], sizeof(g_vsv_big[0]),
                                       &n) == FM_OK && n == VSV_BIG)
                {
                    decoded++;
                }
                (void)Video_Close(&v);
            }
        }
        (void)raw;
        if (r == FM_OK && idx && counted >= 14u && counted <= 20u &&
            (flags & 0x10u) == 0u && riff >= 14u * VSV_BIG &&
            (uint64_t)riff + 8u <= size && decoded >= counted)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d idx=%d counted=%u flags=%x "
                     "riff=%u size=%lu info=%u read=%u", (int)r, (int)idx,
                     (unsigned)counted, (unsigned)flags, (unsigned)riff,
                     (unsigned long)size, (unsigned)vi.frame_count,
                     (unsigned)decoded);
            FAIL(msg);
        }
    }

    TEST("Video save: VideoFile_Repair completes the file that was cut");
    {
        VsvRaw raw;
        FM_Video v;
        FM_VideoInfo vi;
        uint32_t frames = 0u;
        uint32_t none = 77u;
        FM_Bool idx = FM_TRUE;
        FM_Result again;
        FM_Result missing;
        int rc;

        memset(&vi, 0, sizeof(vi));
        r = VideoFile_Repair("/vsv/cut.avi", &frames);
        rc = vsv_raw_check("/vsv/cut.avi", &raw);
        (void)File_Exists("/vsv/cut.avi.idx", &idx);
        again = VideoFile_Repair("/vsv/cut.avi", &none);
        missing = VideoFile_Repair("/vsv/nothing.avi", NULL);
        if (r == FM_OK)
        {
            r = Video_Open("/vsv/cut.avi", &v);
            if (r == FM_OK)
            {
                (void)Video_GetInfo(&v, &vi);
                (void)Video_Close(&v);
            }
        }
        /* 20 x (70 KB + sound) were written through a 32 KB buffer: at
         * most the last picture and its sound are lost */
        if (r == FM_OK && rc == 0 && raw.indexed && frames >= 19u &&
            frames <= 20u && raw.frames == frames &&
            raw.sound_bytes >= (frames - 1u) * 1600u &&
            vi.frame_count == frames && !idx &&
            again == FM_ERR_NOT_FOUND && none == 0u &&
            missing == FM_ERR_NOT_FOUND)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d rule=%d frames=%u raw=%u "
                     "sound=%u info=%u idx=%d again=%d missing=%d", (int)r,
                     rc, (unsigned)frames, (unsigned)raw.frames,
                     (unsigned)raw.sound_bytes, (unsigned)vi.frame_count,
                     (int)idx, (int)again, (int)missing);
            FAIL(msg);
        }
    }

    TEST("Video save: Repair of a short recording, and hands off other files");
    {
        /* Three small pictures: far below the checkpoint, so the header
         * on the card still says "0 frames". */
        FM_VideoFileWriter w;
        FM_VideoSaveConfig c;
        VsvRaw raw;
        uint32_t frames = 0u;
        uint32_t n;
        uint64_t before = 0u;
        uint64_t after = 0u;
        FM_Bool idx = FM_FALSE;
        FM_Result foreign;
        int rc;

        memset(&w, 0, sizeof(w));
        memset(&c, 0, sizeof(c));
        c.width = VSV_W;
        c.height = VSV_H;
        c.fps = 10u;
        r = VideoFileWriter_Open(&w, "/vsv/short.avi", &c);
        for (n = 0u; r == FM_OK && n < 3u; n++)
        {
            vsv_frame(g_vsv_rgb, n);
            r = VideoFileWriter_WriteFrame(&w, g_vsv_rgb, FM_PIXEL_RGB888);
        }
        /* what a sync just before the reset would have put on the card */
        if (r == FM_OK)
        {
            uint32_t wr = 0u;

            r = File_Write(&w.file, w.buf, w.buf_len, &wr);
        }
        vsv_abandon(&w);
        if (r == FM_OK)
        {
            r = VideoFile_Repair("/vsv/short.avi", &frames);
        }
        rc = vsv_raw_check("/vsv/short.avi", &raw);

        /* a file that is not ours, with a stray ".idx" beside it */
        (void)File_WriteAll("/vsv/other.avi", "not a video at all, just "
                            "text that happens to have this name", 60u);
        (void)File_WriteAll("/vsv/other.avi.idx", "x", 1u);
        (void)File_SizeOf("/vsv/other.avi", &before);
        foreign = VideoFile_Repair("/vsv/other.avi", NULL);
        (void)File_SizeOf("/vsv/other.avi", &after);
        (void)File_Exists("/vsv/other.avi.idx", &idx);
        if (r == FM_OK && rc == 0 && frames == 3u && raw.frames == 3u &&
            raw.indexed && foreign == FM_ERR_INVALID_FORMAT &&
            before == 60u && after == 60u && idx)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d rule=%d frames=%u/%u "
                     "foreign=%d size %lu->%lu idx=%d", (int)r, rc,
                     (unsigned)frames, (unsigned)raw.frames, (int)foreign,
                     (unsigned long)before, (unsigned long)after, (int)idx);
            FAIL(msg);
        }
    }

    TEST("Video save: card full at any point -> the error, and a file that is right");
    {
        /* The card has `budget` bytes free after Open.  Whatever the
         * budget, Close must report the failure and leave a file that
         * passes the byte-by-byte check with the pictures that are
         * whole; no ".idx" stays behind. */
        static const uint32_t budgets[] = { 400u, 40000u, 71000u, 150000u,
                                            215000u, 290000u, 431000u };
        uint32_t b;
        int bad = 0;

        msg[0] = '\0';
        (void)FS_Unmount("/ram");
        r = wfs_mount("/ram");
        if (r != FM_OK)
        {
            bad = 1;
            snprintf(msg, sizeof(msg), "wfs_mount failed: %d", (int)r);
        }
        for (b = 0u; !bad && b < sizeof(budgets) / sizeof(budgets[0]); b++)
        {
            FM_VideoFileWriter w;
            FM_VideoSaveConfig c;
            VsvRaw raw;
            FM_Result wrote;
            FM_Result closed;
            FM_Bool idx = FM_TRUE;
            int rc;

            memset(&w, 0, sizeof(w));
            memset(&c, 0, sizeof(c));
            c.width = VSV_W;
            c.height = VSV_H;
            c.fps = 10u;
            c.audio_rate = VSV_RATE;
            c.audio_channels = 1u;
            g_wfs_card_free = -1;
            r = VideoFileWriter_Open(&w, "/ram/vsv/f.avi", &c);
            g_wfs_card_free = (int64_t)budgets[b];
            wrote = (r == FM_OK) ? vsv_write_big(&w, 8u) : r;
            closed = VideoFileWriter_Close(&w);
            g_wfs_card_free = -1;
            rc = vsv_raw_check("/ram/vsv/f.avi", &raw);
            (void)File_Exists("/ram/vsv/f.avi.idx", &idx);
            /* each picture with its sound is 71.6 KB */
            if (r != FM_OK || closed == FM_OK || rc != 0 || idx ||
                raw.frames > budgets[b] / VSV_BIG ||
                (budgets[b] > 75000u &&
                 raw.frames + 1u < (budgets[b] - 33000u) / 71700u))
            {
                bad = 1;
                snprintf(msg, sizeof(msg), "budget=%u open=%d wrote=%d "
                         "close=%d rule=%d frames=%u indexed=%d idx=%d",
                         (unsigned)budgets[b], (int)r, (int)wrote,
                         (int)closed, rc, (unsigned)raw.frames, raw.indexed,
                         (int)idx);
            }
            (void)File_Delete("/ram/vsv/f.avi");
        }
        wfs_unmount("/ram");
        restore_posix_root();
        if (!bad)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }

    (void)Directory_DeleteRecursive("/vsv");
}
