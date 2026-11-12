/*
 * FileManager host tests -- saving audio (src/fm_audio_enc.c).
 *
 * Every encoder is checked against the decoder of the library: what is
 * written must come back, bit for bit for WAV and FLAC, close for ADPCM.
 * Included by test/main.c; uses its TEST / PASS / FAIL macros.
 */

#include <math.h>

#define ASV_MAX_FRAMES 12000u

static int16_t g_asv_in[ASV_MAX_FRAMES * 2u];
static int16_t g_asv_out[(ASV_MAX_FRAMES + 64u) * 2u];
static uint32_t g_asv_seed;

static uint32_t asv_rand(void)
{
    g_asv_seed = g_asv_seed * 1664525u + 1013904223u;
    return g_asv_seed >> 8;
}

/* 0 sine pair  1 full-scale noise  2 silence  3 the same in both
 * channels  4 +max/-max alternating  5 a constant  6 quiet "music" */
static void asv_gen(int kind, int16_t *pcm, uint32_t frames, uint32_t ch)
{
    uint32_t i;
    uint32_t c;

    g_asv_seed = 12345u + (uint32_t)kind;
    for (i = 0u; i < frames; i++)
    {
        for (c = 0u; c < ch; c++)
        {
            double v = 0.0;

            switch (kind)
            {
            case 0:
                v = (c == 0u) ? 12000.0 * sin((double)i * 0.0627)
                              : 7000.0 * sin((double)i * 0.0941 + 1.0);
                break;
            case 1:
                v = (double)(int16_t)(asv_rand() & 0xFFFFu);
                break;
            case 2:
                v = 0.0;
                break;
            case 3:
                v = 9000.0 * sin((double)i * 0.05) +
                    2000.0 * sin((double)i * 0.31);
                break;
            case 4:
                v = ((i + c) & 1u) ? 32767.0 : -32768.0;
                break;
            case 5:
                v = (c == 0u) ? 1234.0 : -4321.0;
                break;
            default:
                v = 3000.0 * sin((double)i * 0.021 + (double)c) +
                    1500.0 * sin((double)i * 0.13) +
                    (double)((int)(asv_rand() & 31u) - 16);
                break;
            }
            pcm[i * ch + c] = (int16_t)v;
        }
    }
}

/* Write `frames` frames in pieces of uneven size (1, 2, 777, 5003 ...),
 * so that every block boundary inside an encoder is crossed. */
static FM_Result asv_write(const char *path, FM_AudioFileFormat format,
                           uint32_t rate, uint32_t ch, uint32_t out_ch,
                           const int16_t *pcm, uint32_t frames)
{
    static const uint32_t steps[] = { 1u, 2u, 777u, 5003u, 3u, 4096u, 129u };
    FM_AudioFileWriter w;
    FM_AudioEncodeConfig cfg;
    uint32_t done = 0u;
    uint32_t s = 0u;
    FM_Result r;
    FM_Result c;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = rate;
    cfg.channels = (uint16_t)ch;
    cfg.bits_per_sample = 16u;
    cfg.out_channels = (uint16_t)out_ch;
    r = AudioFileWriter_Open(&w, path, format, &cfg);
    if (r != FM_OK)
    {
        return r;
    }
    while (done < frames && r == FM_OK)
    {
        uint32_t n = steps[s++ % (sizeof(steps) / sizeof(steps[0]))];
        size_t wrote = 0u;

        if (n > frames - done)
        {
            n = frames - done;
        }
        r = AudioFileWriter_WriteFrames(&w, pcm + (size_t)done * ch, n,
                                        &wrote);
        if (r == FM_OK && wrote != n)
        {
            r = FM_ERR_IO;
        }
        done += n;
    }
    c = AudioFileWriter_Close(&w);
    return (r != FM_OK) ? r : c;
}

/* Decode a whole file; *frames = frames read. */
static FM_Result asv_read(const char *path, int16_t *pcm, uint32_t cap_frames,
                          uint32_t *frames, FM_AudioInfo *info)
{
    static FM_Audio a;
    uint32_t total = 0u;
    uint32_t ch;
    FM_Result r;

    *frames = 0u;
    memset(&a, 0, sizeof(a));
    r = Audio_Open(path, &a);
    if (r != FM_OK)
    {
        return r;
    }
    if (info != NULL)
    {
        *info = a.info;
    }
    ch = a.info.channels;
    while (total < cap_frames)
    {
        size_t got = 0u;
        size_t want = (size_t)(cap_frames - total) * ch;

        if (want > 1999u * ch)
        {
            want = 1999u * ch;
        }
        r = Audio_Read(&a, pcm + (size_t)total * ch, want, &got);
        if (r == FM_ERR_EOF)
        {
            r = FM_OK;
            total += (uint32_t)(got / ch);
            break;
        }
        if (r != FM_OK || got == 0u)
        {
            break;
        }
        total += (uint32_t)(got / ch);
    }
    (void)Audio_Close(&a);
    *frames = total;
    return r;
}

static void asv_negate(void *ctx, int16_t *pcm, size_t samples,
                       uint32_t rate, uint16_t channels)
{
    uint32_t *calls = (uint32_t *)ctx;
    size_t i;

    (void)rate;
    (void)channels;
    (*calls)++;
    for (i = 0u; i < samples; i++)
    {
        pcm[i] = (int16_t)((pcm[i] == -32768) ? 32767 : -pcm[i]);
    }
}

static FM_Bool asv_cancel_at(void *ctx, uint64_t done, uint64_t total)
{
    uint64_t *limit = (uint64_t *)ctx;

    (void)total;
    return (done < *limit) ? FM_TRUE : FM_FALSE;
}

/* A plugin encoder that fails on its second write. */
static int g_asv_enc_inits;
static int g_asv_enc_writes;
static int g_asv_enc_finishes;
static int g_asv_enc_closes;

static FM_Result asv_enc_init(void **ctx, const FM_AudioEncodeConfig *cfg)
{
    (void)cfg;
    g_asv_enc_inits++;
    *ctx = &g_asv_enc_inits;
    return FM_OK;
}

static FM_Result asv_enc_write(void *ctx, FM_File *f, const int16_t *pcm,
                               size_t frames, size_t *used)
{
    uint32_t wr = 0u;

    (void)ctx;
    g_asv_enc_writes++;
    if (g_asv_enc_writes >= 2)
    {
        return FM_ERR_NO_SPACE;
    }
    /* takes one frame at a time: the writer has to call again */
    (void)File_Write(f, pcm, 2u, &wr);
    *used = (frames > 0u) ? 1u : 0u;
    return FM_OK;
}

static FM_Result asv_enc_finish(void *ctx, FM_File *f)
{
    (void)ctx;
    (void)f;
    g_asv_enc_finishes++;
    return FM_OK;
}

static void asv_enc_close(void *ctx)
{
    (void)ctx;
    g_asv_enc_closes++;
}

static void *asv_no_alloc(size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    return NULL;
}

static void asv_no_free(void *p, void *ctx)
{
    (void)p;
    (void)ctx;
}

/* ---- the file read as a stranger would --------------------------------
 * The header of a WAV file, walked by the rules of the format and not
 * through Audio_Open: the reader of this library takes the length of an
 * ADPCM file from the blocks, so a wrong "fact" chunk (the exact number
 * of frames, which other players use) went unseen. */
typedef struct
{
    uint32_t tag;           /* 1 PCM, 0x11 IMA ADPCM                    */
    uint32_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint32_t block_align;
    uint32_t bits;
    uint32_t block_frames;  /* ADPCM: sample frames in a block          */
    uint32_t fact;          /* frames by the "fact" chunk (0 = none)    */
    int      has_fact;
    uint32_t data;          /* bytes of the "data" chunk                */
    uint32_t file_size;
} AsvWav;

static uint32_t asv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 0 = right; any other number names the rule that is broken.  With
 * `cut` (a recording that was not closed) the file may be longer than
 * its header says; without it every length must be exact. */
static int asv_raw_wav(const char *path, AsvWav *w, int cut)
{
    static uint8_t h[128];
    uint64_t size = 0u;
    uint32_t got = 0u;
    uint32_t pos = 12u;
    int have_fmt = 0;
    FM_File f;

    memset(w, 0, sizeof(*w));
    if (File_SizeOf(path, &size) != FM_OK || size < 44u)
    {
        return 1;
    }
    w->file_size = (uint32_t)size;
    File_Init(&f);
    if (File_OpenEx(&f, path, FM_OPEN_READ) != FM_OK)
    {
        return 2;
    }
    (void)File_Read(&f, h, sizeof(h), &got);
    (void)File_Close(&f);
    if (got < 44u || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
    {
        return 3;
    }
    if (cut ? (asv_rd32(h + 4) + 8u > w->file_size)
            : (asv_rd32(h + 4) + 8u != w->file_size))
    {
        return 4;                                   /* RIFF length      */
    }
    while (pos + 8u <= got)
    {
        uint32_t n = asv_rd32(h + pos + 4u);

        if (memcmp(h + pos, "fmt ", 4) == 0)
        {
            if (n < 16u || pos + 8u + n > got)
            {
                return 5;
            }
            w->tag = (uint32_t)h[pos + 8u] | ((uint32_t)h[pos + 9u] << 8);
            w->channels = (uint32_t)h[pos + 10u] | ((uint32_t)h[pos + 11u] << 8);
            w->rate = asv_rd32(h + pos + 12u);
            w->byte_rate = asv_rd32(h + pos + 16u);
            w->block_align = (uint32_t)h[pos + 20u] | ((uint32_t)h[pos + 21u] << 8);
            w->bits = (uint32_t)h[pos + 22u] | ((uint32_t)h[pos + 23u] << 8);
            if (n >= 20u)
            {
                w->block_frames = (uint32_t)h[pos + 26u] |
                                  ((uint32_t)h[pos + 27u] << 8);
            }
            have_fmt = 1;
        }
        else if (memcmp(h + pos, "fact", 4) == 0)
        {
            if (n != 4u || pos + 12u > got)
            {
                return 6;
            }
            w->fact = asv_rd32(h + pos + 8u);
            w->has_fact = 1;
        }
        else if (memcmp(h + pos, "data", 4) == 0)
        {
            w->data = n;
            if (!have_fmt)
            {
                return 7;
            }
            if (cut ? (pos + 8u + n > w->file_size)
                    : (pos + 8u + n != w->file_size))
            {
                return 8;                           /* data length      */
            }
            if (asv_rd32(h + 4) != pos + n)
            {
                return 9;           /* the two lengths tell one story   */
            }
            return 0;
        }
        else
        {
            return 10;                              /* an unknown chunk */
        }
        pos += 8u + n + (n & 1u);
    }
    return 11;                                      /* no "data"        */
}

/* What a reset leaves behind: the handle is dropped, nothing is
 * finished, what was still in the encoder's buffer is gone. */
/* The checkpoint distance the library was built with (its default lives
 * in src/fm_audio_enc.c; only "0 or not" and the size matter here). */
#ifdef FM_AUDIO_ENC_SYNC_BYTES
#define ASV_SYNC_BYTES (FM_AUDIO_ENC_SYNC_BYTES + 0u)
#else
#define ASV_SYNC_BYTES 262144u
#endif

static void asv_abandon(FM_AudioFileWriter *w)
{
    (void)File_Close(&w->file);
    if (w->encoder != NULL && w->encoder->close != NULL)
    {
        w->encoder->close(w->encoder_ctx);
    }
    memset(w, 0, sizeof(*w));
}

static void test_audio_save(void)
{
    static const uint32_t lengths[] = { 1u, 2u, 31u, 32u, 33u, 255u, 4095u,
                                        4096u, 4097u, 8192u, 11999u };
    char msg[160];
    FM_AudioInfo info;
    FM_Result r;
    uint32_t got = 0u;
    uint64_t size = 0u;
    FM_Bool exists = FM_FALSE;
    uint32_t i;

    printf("\n[Saving audio: encoders and file-to-file conversion]\n");
    restore_posix_root();
    (void)Directory_Create("/asv");
    (void)info;
    (void)lengths;

    TEST("Save: formats from the file name");
    if (Audio_FormatFromPath("/a/b.WAV") == FM_AUDIO_FILE_WAV &&
        Audio_FormatFromPath("x.flac") == FM_AUDIO_FILE_FLAC &&
        Audio_FormatFromPath("x.Mp3") == FM_AUDIO_FILE_MP3 &&
        Audio_FormatFromPath("x.ogg") == FM_AUDIO_FILE_VORBIS &&
        Audio_FormatFromPath("x.opus") == FM_AUDIO_FILE_OPUS &&
        Audio_FormatFromPath("/a.wav/b") == FM_AUDIO_FILE_AUTO &&
        Audio_FormatFromPath("x.txt") == FM_AUDIO_FILE_AUTO &&
        Audio_FormatFromPath(NULL) == FM_AUDIO_FILE_AUTO &&
        strcmp(Audio_FormatExtension(FM_AUDIO_FILE_FLAC), ".flac") == 0 &&
        strcmp(Audio_FormatExtension(FM_AUDIO_FILE_ADPCM), ".wav") == 0 &&
        Audio_CanEncode(FM_AUDIO_FILE_WAV) &&
        Audio_CanEncode(FM_AUDIO_FILE_ADPCM) &&
        !Audio_CanEncode(FM_AUDIO_FILE_VORBIS) &&
        !Audio_CanEncode(FM_AUDIO_FILE_AUTO) &&
        !Audio_CanEncode(FM_AUDIO_FILE_COUNT))
    {
        PASS();
    }
    else
    {
        FAIL0();
    }

    TEST("Save: WAV comes back bit for bit, any length");
    {
        int bad = 0;

        for (i = 0u; i < sizeof(lengths) / sizeof(lengths[0]) && !bad; i++)
        {
            uint32_t ch;

            for (ch = 1u; ch <= 2u && !bad; ch++)
            {
                uint32_t n = lengths[i];

                asv_gen(1, g_asv_in, n, ch);
                r = asv_write("/asv/t.wav", FM_AUDIO_FILE_AUTO, 44100u, ch,
                              0u, g_asv_in, n);
                if (r == FM_OK)
                    r = asv_read("/asv/t.wav", g_asv_out, ASV_MAX_FRAMES + 64u,
                                 &got, &info);
                if (r == FM_OK) r = File_SizeOf("/asv/t.wav", &size);
                if (r != FM_OK || got != n || info.channels != ch ||
                    info.sample_rate != 44100u ||
                    size != 44u + (uint64_t)n * ch * 2u ||
                    memcmp(g_asv_in, g_asv_out, (size_t)n * ch * 2u) != 0)
                {
                    snprintf(msg, sizeof(msg), "n=%u ch=%u r=%d got=%u",
                             (unsigned)n, (unsigned)ch, (int)r, (unsigned)got);
                    bad = 1;
                }
            }
        }
        if (bad) FAIL(msg); else PASS();
    }

    TEST("Save: stereo to mono and mono to stereo");
    {
        int ok = 1;

        asv_gen(0, g_asv_in, 3000u, 2u);
        r = asv_write("/asv/m.wav", FM_AUDIO_FILE_WAV, 22050u, 2u, 1u,
                      g_asv_in, 3000u);
        if (r == FM_OK)
            r = asv_read("/asv/m.wav", g_asv_out, ASV_MAX_FRAMES, &got, &info);
        if (r != FM_OK || got != 3000u || info.channels != 1u) ok = 0;
        for (i = 0u; ok && i < 3000u; i++)
        {
            int32_t want = ((int32_t)g_asv_in[2u * i] +
                            (int32_t)g_asv_in[2u * i + 1u]) >> 1;

            if (g_asv_out[i] != (int16_t)want) ok = 0;
        }
        asv_gen(0, g_asv_in, 3000u, 1u);
        if (ok)
            r = asv_write("/asv/s.wav", FM_AUDIO_FILE_WAV, 22050u, 1u, 2u,
                          g_asv_in, 3000u);
        if (ok && r == FM_OK)
            r = asv_read("/asv/s.wav", g_asv_out, ASV_MAX_FRAMES, &got, &info);
        if (r != FM_OK || got != 3000u || info.channels != 2u) ok = 0;
        for (i = 0u; ok && i < 3000u; i++)
        {
            if (g_asv_out[2u * i] != g_asv_in[i] ||
                g_asv_out[2u * i + 1u] != g_asv_in[i]) ok = 0;
        }
        if (ok) PASS(); else FAIL0();
    }

#if FM_USE_FLAC
    TEST("Save: FLAC is lossless, every signal and length");
    {
        static const uint32_t rates[] = { 44100u, 48000u, 8000u, 12345u,
                                          96000u, 11025u, 22000u };
        int bad = 0;
        int kind;
        unsigned combos = 0u;

        for (kind = 0; kind <= 6 && !bad; kind++)
        {
            for (i = 0u; i < sizeof(lengths) / sizeof(lengths[0]) && !bad;
                 i++)
            {
                uint32_t ch;

                for (ch = 1u; ch <= 2u && !bad; ch++)
                {
                    uint32_t n = lengths[i];
                    uint32_t rate = rates[((uint32_t)kind + i + ch) % 7u];

                    asv_gen(kind, g_asv_in, n, ch);
                    memset(g_asv_out, 0x55, sizeof(g_asv_out));
                    r = asv_write("/asv/t.flac", FM_AUDIO_FILE_AUTO, rate,
                                  ch, 0u, g_asv_in, n);
                    if (r == FM_OK)
                        r = asv_read("/asv/t.flac", g_asv_out,
                                     ASV_MAX_FRAMES + 64u, &got, &info);
                    combos++;
                    if (r != FM_OK || got != n || info.channels != ch ||
                        info.sample_rate != rate ||
                        info.total_frames != n ||
                        memcmp(g_asv_in, g_asv_out,
                               (size_t)n * ch * 2u) != 0)
                    {
                        snprintf(msg, sizeof(msg),
                                 "kind=%d n=%u ch=%u rate=%u r=%d got=%u",
                                 kind, (unsigned)n, (unsigned)ch,
                                 (unsigned)rate, (int)r, (unsigned)got);
                        bad = 1;
                    }
                }
            }
        }
        if (bad) FAIL(msg);
        else if (combos != 7u * 11u * 2u) FAIL("not every case ran");
        else PASS();
    }

    TEST("Save: FLAC really compresses");
    {
        uint64_t music = 0u;
        uint64_t same = 0u;
        uint64_t silence = 0u;
        uint64_t noise = 0u;
        const uint64_t raw = 12000u * 4u;

        asv_gen(6, g_asv_in, 12000u, 2u);
        r = asv_write("/asv/c.flac", FM_AUDIO_FILE_FLAC, 44100u, 2u, 0u,
                      g_asv_in, 12000u);
        if (r == FM_OK) r = File_SizeOf("/asv/c.flac", &music);
        asv_gen(3, g_asv_in, 12000u, 2u);
        if (r == FM_OK)
            r = asv_write("/asv/c.flac", FM_AUDIO_FILE_FLAC, 44100u, 2u, 0u,
                          g_asv_in, 12000u);
        if (r == FM_OK) r = File_SizeOf("/asv/c.flac", &same);
        asv_gen(2, g_asv_in, 12000u, 2u);
        if (r == FM_OK)
            r = asv_write("/asv/c.flac", FM_AUDIO_FILE_FLAC, 44100u, 2u, 0u,
                          g_asv_in, 12000u);
        if (r == FM_OK) r = File_SizeOf("/asv/c.flac", &silence);
        asv_gen(1, g_asv_in, 12000u, 2u);
        if (r == FM_OK)
            r = asv_write("/asv/c.flac", FM_AUDIO_FILE_FLAC, 44100u, 2u, 0u,
                          g_asv_in, 12000u);
        if (r == FM_OK) r = File_SizeOf("/asv/c.flac", &noise);
        snprintf(msg, sizeof(msg),
                 "raw %lu: music %lu, L=R %lu, silence %lu, noise %lu (r=%d)",
                 (unsigned long)raw, (unsigned long)music,
                 (unsigned long)same, (unsigned long)silence,
                 (unsigned long)noise, (int)r);
        /* quiet music under half; the same signal twice costs one
         * channel; silence is a few bytes a block; noise is stored and
         * may not grow by more than the frame headers */
        if (r == FM_OK && music < raw / 2u && same < raw / 3u &&
            silence < 200u && noise < raw + 300u)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }
#endif

    TEST("Save: ADPCM (4:1) decodes to the same sound");
    {
        int bad = 0;
        uint32_t ch;

        for (ch = 1u; ch <= 2u && !bad; ch++)
        {
            static const uint32_t ns[] = { 1u, 2u, 9u, 504u, 505u, 506u,
                                           2041u, 11000u };
            static const uint32_t rates[] = { 8000u, 22050u, 44100u };
            uint32_t k;

            for (k = 0u; k < 8u && !bad; k++)
            {
                uint32_t n = ns[k];
                uint32_t rate = rates[k % 3u];
                double err = 0.0;
                double sig = 0.0;
                uint32_t j;

                asv_gen(0, g_asv_in, n, ch);
                r = asv_write("/asv/a.wav", FM_AUDIO_FILE_ADPCM, rate, ch,
                              0u, g_asv_in, n);
                if (r == FM_OK)
                    r = asv_read("/asv/a.wav", g_asv_out,
                                 ASV_MAX_FRAMES + 64u, &got, &info);
                if (r == FM_OK) r = File_SizeOf("/asv/a.wav", &size);
                for (j = 0u; r == FM_OK && j < n * ch; j++)
                {
                    double d = (double)g_asv_in[j] - (double)g_asv_out[j];

                    err += d * d;
                    sig += (double)g_asv_in[j] * (double)g_asv_in[j];
                }
                /* whole blocks: the length is rounded up to the next
                 * one, and what pads it stays at the last sample (no
                 * click); the noise must sit 25 dB under the signal
                 * once there is enough signal to measure */
                {
                    uint32_t bf = (rate >= 44100u) ? 2041u
                                : (rate >= 22050u) ? 1017u : 505u;
                    uint32_t want = ((n + bf - 1u) / bf) * bf;
                    uint32_t c2;

                    if (got != want) r = FM_ERR_CORRUPTED;
                    for (c2 = 0u; r == FM_OK && c2 < ch && got > n + 64u;
                         c2++)
                    {
                        int32_t d = (int32_t)g_asv_out[(got - 1u) * ch + c2] -
                                    (int32_t)g_asv_in[(n - 1u) * ch + c2];

                        if (d > 600 || d < -600) r = FM_ERR_CORRUPTED;
                    }
                }
                if (r != FM_OK ||
                    info.format != 0x11u || info.channels != ch ||
                    info.sample_rate != rate || (size & 1u) != 0u ||
                    (n > 500u && err * 316.0 > sig) ||
                    (n > 2000u && size * 3u > (uint64_t)n * ch * 2u))
                {
                    snprintf(msg, sizeof(msg),
                             "n=%u ch=%u r=%d got=%u size=%lu snr=%.1f dB",
                             (unsigned)n, (unsigned)ch, (int)r, (unsigned)got,
                             (unsigned long)size,
                             (err > 0.0) ? 10.0 * log10(sig / err) : 99.0);
                    bad = 1;
                }
            }
        }
        if (bad) FAIL(msg); else PASS();
    }

    TEST("Save: no encoder -> nothing is created");
    {
        FM_AudioFileWriter w;
        FM_AudioEncodeConfig cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = 44100u;
        cfg.channels = 2u;
        cfg.bits_per_sample = 16u;
        (void)File_Delete("/asv/x.ogg");
        r = AudioFileWriter_Open(&w, "/asv/x.ogg", FM_AUDIO_FILE_AUTO, &cfg);
        (void)File_Exists("/asv/x.ogg", &exists);
        if (r == FM_ERR_UNSUPPORTED && !exists &&
            AudioFileWriter_Open(&w, "/asv/x.bin", FM_AUDIO_FILE_AUTO,
                                 &cfg) == FM_ERR_UNSUPPORTED &&
            AudioFileWriter_Open(&w, "/asv/none/x.wav", FM_AUDIO_FILE_AUTO,
                                 &cfg) != FM_OK)
        {
            PASS();
        }
        else
        {
            FAIL0();
        }
    }

    TEST("Save: an empty recording is still a valid file");
    {
        FM_AudioFileWriter w;
        FM_AudioEncodeConfig cfg;
        int ok = 1;

        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = 16000u;
        cfg.channels = 1u;
        cfg.bits_per_sample = 16u;
        r = AudioFileWriter_Open(&w, "/asv/e.wav", FM_AUDIO_FILE_AUTO, &cfg);
        if (r == FM_OK) r = AudioFileWriter_Close(&w);
        if (r == FM_OK) r = File_SizeOf("/asv/e.wav", &size);
        if (r != FM_OK || size != 44u) ok = 0;
        if (ok)
        {
            r = asv_read("/asv/e.wav", g_asv_out, 16u, &got, &info);
            if (got != 0u || info.sample_rate != 16000u) ok = 0;
        }
        r = AudioFileWriter_Open(&w, "/asv/e2.wav", FM_AUDIO_FILE_ADPCM, &cfg);
        if (r == FM_OK) r = AudioFileWriter_Close(&w);
        if (r == FM_OK) r = File_SizeOf("/asv/e2.wav", &size);
        if (r != FM_OK || size != 60u) ok = 0;
#if FM_USE_FLAC
        r = AudioFileWriter_Open(&w, "/asv/e.flac", FM_AUDIO_FILE_AUTO, &cfg);
        if (r == FM_OK) r = AudioFileWriter_Close(&w);
        if (r == FM_OK) r = File_SizeOf("/asv/e.flac", &size);
        if (r != FM_OK || size != 65u) ok = 0;
#endif
        /* closing twice is an error, not a crash */
        if (AudioFileWriter_Close(&w) != FM_ERR_NOT_OPEN) ok = 0;
        if (ok) PASS(); else FAIL0();
    }

    TEST("Save: a failing encoder is reported once, closed once");
    {
        FM_AudioFileWriter w;
        FM_AudioEncodeConfig cfg;
        FM_AudioEncoderOps ops;
        FM_Result r1;
        FM_Result r2;
        FM_Result r3;
        FM_Result rc;
        size_t wrote = 77u;

        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = 8000u;
        cfg.channels = 1u;
        cfg.bits_per_sample = 16u;
        memset(&ops, 0, sizeof(ops));
        ops.name = "failing";
        ops.init = asv_enc_init;
        ops.write = asv_enc_write;
        ops.finish = asv_enc_finish;
        ops.close = asv_enc_close;
        g_asv_enc_inits = g_asv_enc_writes = 0;
        g_asv_enc_finishes = g_asv_enc_closes = 0;
        r = Audio_RegisterEncoder(FM_AUDIO_FILE_OPUS, &ops);
        if (r == FM_OK)
            r = AudioFileWriter_Open(&w, "/asv/f.opus", FM_AUDIO_FILE_AUTO,
                                     &cfg);
        asv_gen(0, g_asv_in, 8u, 1u);
        r1 = AudioFileWriter_WriteFrames(&w, g_asv_in, 1u, &wrote);
        r2 = AudioFileWriter_WriteFrames(&w, g_asv_in, 4u, &wrote);
        r3 = AudioFileWriter_WriteFrames(&w, g_asv_in, 4u, &wrote);
        rc = AudioFileWriter_Close(&w);
        (void)Audio_UnregisterEncoder(FM_AUDIO_FILE_OPUS);
        if (r == FM_OK && r1 == FM_OK && r2 == FM_ERR_NO_SPACE &&
            r3 == FM_ERR_NO_SPACE && rc == FM_ERR_NO_SPACE && wrote == 0u &&
            g_asv_enc_inits == 1 && g_asv_enc_writes == 2 &&
            g_asv_enc_finishes == 0 && g_asv_enc_closes == 1 &&
            !Audio_CanEncode(FM_AUDIO_FILE_OPUS) &&
            Audio_RegisterEncoder(FM_AUDIO_FILE_WAV, &ops) != FM_OK &&
            Audio_RegisterEncoder(FM_AUDIO_FILE_COUNT, &ops) != FM_OK)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d %d %d %d rc=%d i=%d w=%d f=%d c=%d",
                     (int)r, (int)r1, (int)r2, (int)r3, (int)rc,
                     g_asv_enc_inits, g_asv_enc_writes, g_asv_enc_finishes,
                     g_asv_enc_closes);
            FAIL(msg);
        }
        (void)File_Delete("/asv/f.opus");
    }

    TEST("Save: out of memory -> clean error, no file");
    {
        FM_Allocator saved = *FM_Allocator_Get();
        FM_Allocator none;
        FM_AudioFileWriter w;
        FM_AudioEncodeConfig cfg;
        FM_Result ra;
        FM_Result rb;
        FM_Result rf = FM_ERR_NO_MEMORY;

        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = 8000u;
        cfg.channels = 2u;
        cfg.bits_per_sample = 16u;
        memset(&none, 0, sizeof(none));
        none.alloc = asv_no_alloc;
        none.free = asv_no_free;
        (void)File_Delete("/asv/nomem.wav");
        (void)FM_Allocator_Set(&none);
        ra = AudioFileWriter_Open(&w, "/asv/nomem.wav", FM_AUDIO_FILE_WAV,
                                  &cfg);
        rb = AudioFileWriter_Open(&w, "/asv/nomem.wav", FM_AUDIO_FILE_ADPCM,
                                  &cfg);
#if FM_USE_FLAC
        rf = AudioFileWriter_Open(&w, "/asv/nomem.flac", FM_AUDIO_FILE_FLAC,
                                  &cfg);
#endif
        (void)FM_Allocator_Set(&saved);
        (void)File_Exists("/asv/nomem.wav", &exists);
        if (ra == FM_ERR_NO_MEMORY && rb == FM_ERR_NO_MEMORY &&
            rf == FM_ERR_NO_MEMORY && !exists)
        {
            PASS();
        }
        else
        {
            FAIL0();
        }
    }

    TEST("Convert: WAV -> ADPCM -> WAV, whole blocks");
    {
        uint64_t f1 = 0u;
        uint64_t f2 = 0u;
        FM_AudioConvertConfig cc;

        asv_gen(6, g_asv_in, 5050u, 2u);
        r = asv_write("/asv/src.wav", FM_AUDIO_FILE_WAV, 32000u, 2u, 0u,
                      g_asv_in, 5050u);
        memset(&cc, 0, sizeof(cc));
        cc.format = FM_AUDIO_FILE_ADPCM;
        if (r == FM_OK)
            r = Audio_Convert("/asv/src.wav", "/asv/mid.wav", &cc, &f1);
        if (r == FM_OK)
            r = Audio_Convert("/asv/mid.wav", "/asv/back.wav", NULL, &f2);
        if (r == FM_OK)
            r = asv_read("/asv/back.wav", g_asv_out, ASV_MAX_FRAMES, &got,
                         &info);
        snprintf(msg, sizeof(msg), "r=%d f1=%lu f2=%lu got=%u", (int)r,
                 (unsigned long)f1, (unsigned long)f2, (unsigned)got);
        /* 5050 frames = 5 blocks of 1017 at 32 kHz, the last one padded */
        if (r == FM_OK && f1 == 5050u && f2 == 5085u && got == 5085u &&
            info.format == 1u &&
            info.sample_rate == 32000u && info.channels == 2u)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }

#if FM_USE_FLAC
    TEST("Convert: WAV -> FLAC -> WAV is the same file");
    {
        uint64_t f1 = 0u;
        uint64_t f2 = 0u;
        uint64_t s1 = 0u;
        uint64_t s2 = 0u;

        asv_gen(6, g_asv_in, 9000u, 2u);
        r = asv_write("/asv/src.wav", FM_AUDIO_FILE_WAV, 44100u, 2u, 0u,
                      g_asv_in, 9000u);
        if (r == FM_OK)
            r = Audio_Convert("/asv/src.wav", "/asv/mid.flac", NULL, &f1);
        if (r == FM_OK)
            r = Audio_Convert("/asv/mid.flac", "/asv/back.wav", NULL, &f2);
        if (r == FM_OK)
            r = asv_read("/asv/back.wav", g_asv_out, ASV_MAX_FRAMES, &got,
                         &info);
        if (r == FM_OK) r = File_SizeOf("/asv/src.wav", &s1);
        if (r == FM_OK) r = File_SizeOf("/asv/mid.flac", &s2);
        snprintf(msg, sizeof(msg), "r=%d f1=%lu f2=%lu got=%u flac=%lu",
                 (int)r, (unsigned long)f1, (unsigned long)f2, (unsigned)got,
                 (unsigned long)s2);
        if (r == FM_OK && f1 == 9000u && f2 == 9000u && got == 9000u &&
            s2 < s1 / 2u &&
            memcmp(g_asv_in, g_asv_out, 9000u * 4u) == 0)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }
#endif

    TEST("Convert: the processing callback changes what is saved");
    {
        FM_AudioConvertConfig cc;
        uint32_t calls = 0u;
        uint64_t frames = 0u;
        int ok = 1;

        asv_gen(0, g_asv_in, 9000u, 2u);
        r = asv_write("/asv/src.wav", FM_AUDIO_FILE_WAV, 44100u, 2u, 0u,
                      g_asv_in, 9000u);
        memset(&cc, 0, sizeof(cc));
        cc.process = asv_negate;
        cc.process_ctx = &calls;
        cc.out_channels = 1u;
        if (r == FM_OK)
            r = Audio_Convert("/asv/src.wav", "/asv/neg.wav", &cc, &frames);
        if (r == FM_OK)
            r = asv_read("/asv/neg.wav", g_asv_out, ASV_MAX_FRAMES, &got,
                         &info);
        if (r != FM_OK || frames != 9000u || got != 9000u || calls == 0u ||
            info.channels != 1u)
        {
            ok = 0;
        }
        for (i = 0u; ok && i < 9000u; i++)
        {
            /* processing sees the stereo source; the mono mix is made
             * from its result */
            int32_t want = (-(int32_t)g_asv_in[2u * i] -
                            (int32_t)g_asv_in[2u * i + 1u]) >> 1;

            if (g_asv_out[i] != (int16_t)want) ok = 0;
        }
        if (ok) PASS(); else FAIL0();
    }

    TEST("Convert: cancel and errors leave no half file");
    {
        FM_AudioConvertConfig cc;
        uint64_t limit = 4096u;
        uint64_t frames = 55u;
        uint64_t before = 0u;
        uint64_t after = 0u;
        FM_Result r1;
        FM_Result r2;
        FM_Result r3;
        FM_Result r4;
        FM_Bool e1 = FM_TRUE;
        FM_Bool e3 = FM_TRUE;

        memset(&cc, 0, sizeof(cc));
        cc.progress = asv_cancel_at;
        cc.progress_ctx = &limit;
        (void)File_SizeOf("/asv/src.wav", &before);
        r1 = Audio_Convert("/asv/src.wav", "/asv/cancel.wav", &cc, &frames);
        (void)File_Exists("/asv/cancel.wav", &e1);
        r2 = Audio_Convert("/asv/src.wav", "/asv/../asv/SRC.WAV", NULL, NULL);
        (void)File_SizeOf("/asv/src.wav", &after);
        r3 = Audio_Convert("/asv/nothere.wav", "/asv/out3.wav", NULL, NULL);
        (void)File_Exists("/asv/out3.wav", &e3);
        r4 = Audio_Convert("/asv/src.wav", "/asv/out4.ogg", NULL, NULL);
        (void)File_Exists("/asv/out4.ogg", &exists);
        snprintf(msg, sizeof(msg), "r=%d %d %d %d e=%d %d %d sizes %lu %lu",
                 (int)r1, (int)r2, (int)r3, (int)r4, (int)e1, (int)e3,
                 (int)exists, (unsigned long)before, (unsigned long)after);
        if (r1 == FM_ERR_CANCELLED && !e1 && frames == 0u &&
            r2 == FM_ERR_INVALID_ARGUMENT && before == after && before > 44u &&
            r3 != FM_OK && !e3 && r4 == FM_ERR_UNSUPPORTED && !exists &&
            Audio_Convert(NULL, "/asv/x.wav", NULL, NULL) ==
                FM_ERR_INVALID_ARGUMENT)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }

    TEST("Save: WAV and ADPCM headers byte by byte (lengths, \"fact\")");
    {
        static const uint32_t rates[3] = { 8000u, 22050u, 44100u };
        int bad = 0;

        msg[0] = '\0';
        for (i = 0u; i < 3u && !bad; i++)
        {
            uint32_t ch;

            for (ch = 1u; ch <= 2u && !bad; ch++)
            {
                uint32_t n = 3001u + 1777u * i;      /* never a whole block */
                uint32_t mult = (rates[i] >= 44100u) ? 4u
                              : (rates[i] >= 22050u) ? 2u : 1u;
                uint32_t ba = 256u * ch * mult;
                uint32_t bf = 1u + ((ba / ch) - 4u) * 2u;
                AsvWav p;
                AsvWav a;
                int rp;
                int ra;

                asv_gen(0, g_asv_in, n, ch);
                r = asv_write("/asv/h.wav", FM_AUDIO_FILE_WAV, rates[i], ch,
                              0u, g_asv_in, n);
                rp = asv_raw_wav("/asv/h.wav", &p, 0);
                if (r == FM_OK)
                {
                    r = asv_write("/asv/ha.wav", FM_AUDIO_FILE_ADPCM,
                                  rates[i], ch, 0u, g_asv_in, n);
                }
                ra = asv_raw_wav("/asv/ha.wav", &a, 0);
                if (r != FM_OK || rp != 0 || ra != 0 ||
                    p.tag != 1u || p.channels != ch || p.rate != rates[i] ||
                    p.bits != 16u || p.block_align != ch * 2u ||
                    p.byte_rate != rates[i] * ch * 2u ||
                    p.data != n * ch * 2u || p.file_size != 44u + p.data ||
                    a.tag != 0x11u || a.channels != ch ||
                    a.rate != rates[i] || a.bits != 4u ||
                    a.block_align != ba || a.block_frames != bf ||
                    !a.has_fact || a.fact != n ||
                    a.data != ((n + bf - 1u) / bf) * ba ||
                    a.file_size != 60u + a.data)
                {
                    bad = 1;
                    snprintf(msg, sizeof(msg), "rate=%u ch=%u r=%d rule "
                             "pcm=%d adpcm=%d data=%u fact=%u/%u adata=%u "
                             "ba=%u bf=%u", (unsigned)rates[i], (unsigned)ch,
                             (int)r, rp, ra, (unsigned)p.data,
                             (unsigned)a.fact, (unsigned)n, (unsigned)a.data,
                             (unsigned)a.block_align,
                             (unsigned)a.block_frames);
                }
            }
        }
        if (!bad)
        {
            PASS();
        }
        else
        {
            FAIL(msg);
        }
    }

#if FM_USE_FLAC
    TEST("Save: FLAC header byte by byte (STREAMINFO)");
    {
        uint8_t h[64];
        uint32_t n = 9001u;
        uint32_t hg = 0u;
        uint32_t total = 0u;
        uint32_t rate = 0u;
        uint32_t minf = 0u;
        uint32_t maxf = 0u;
        FM_File f;

        memset(h, 0, sizeof(h));
        asv_gen(6, g_asv_in, n, 2u);
        r = asv_write("/asv/h.flac", FM_AUDIO_FILE_FLAC, 44100u, 2u, 0u,
                      g_asv_in, n);
        File_Init(&f);
        if (r == FM_OK && File_OpenEx(&f, "/asv/h.flac", FM_OPEN_READ) == FM_OK)
        {
            (void)File_Read(&f, h, sizeof(h), &hg);
            (void)File_Close(&f);
        }
        (void)File_SizeOf("/asv/h.flac", &size);
        rate = ((uint32_t)h[18] << 12) | ((uint32_t)h[19] << 4) |
               ((uint32_t)h[20] >> 4);
        total = ((uint32_t)h[22] << 24) | ((uint32_t)h[23] << 16) |
                ((uint32_t)h[24] << 8) | (uint32_t)h[25];
        minf = ((uint32_t)h[12] << 16) | ((uint32_t)h[13] << 8) | h[14];
        maxf = ((uint32_t)h[15] << 16) | ((uint32_t)h[16] << 8) | h[17];
        if (r == FM_OK && hg == sizeof(h) && memcmp(h, "fLaC", 4) == 0 &&
            (h[4] & 0x7Fu) == 0u && h[7] == 34u &&       /* STREAMINFO   */
            (((uint32_t)h[8] << 8) | h[9]) == 4096u &&    /* block sizes  */
            (((uint32_t)h[10] << 8) | h[11]) == 4096u &&
            rate == 44100u && ((h[20] >> 1) & 7u) == 1u && /* 2 channels  */
            ((((uint32_t)h[20] & 1u) << 4) | (h[21] >> 4)) == 15u && /* 16 bit */
            (h[21] & 0x0Fu) == 0u && total == n &&
            minf != 0u && minf <= maxf && (uint64_t)maxf < size)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d rate=%u total=%u/%u frame "
                     "%u..%u size=%lu", (int)r, (unsigned)rate,
                     (unsigned)total, (unsigned)n, (unsigned)minf,
                     (unsigned)maxf, (unsigned long)size);
            FAIL(msg);
        }
    }
#endif

    TEST("Save: a second Open on an open writer is refused");
    {
        FM_AudioFileWriter w;
        FM_AudioEncodeConfig cfg;
        FM_Result second = FM_OK;
        FM_Result same = FM_OK;
        FM_Bool other = FM_TRUE;

        memset(&w, 0, sizeof(w));
        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = 16000u;
        cfg.channels = 1u;
        cfg.bits_per_sample = 16u;
        asv_gen(0, g_asv_in, 2000u, 1u);
        (void)File_Delete("/asv/two.wav");
        r = AudioFileWriter_Open(&w, "/asv/one.wav", FM_AUDIO_FILE_AUTO, &cfg);
        if (r == FM_OK)
        {
            r = AudioFileWriter_WriteFrames(&w, g_asv_in, 1000u, NULL);
        }
        if (r == FM_OK)
        {
            second = AudioFileWriter_Open(&w, "/asv/two.wav",
                                          FM_AUDIO_FILE_AUTO, &cfg);
            same = AudioFileWriter_Open(&w, "/asv/one.wav",
                                        FM_AUDIO_FILE_AUTO, &cfg);
            /* the first file goes on as if nothing had happened */
            r = AudioFileWriter_WriteFrames(&w, g_asv_in + 1000, 1000u, NULL);
        }
        if (r == FM_OK)
        {
            r = AudioFileWriter_Close(&w);
        }
        (void)File_Exists("/asv/two.wav", &other);
        if (r == FM_OK)
        {
            r = asv_read("/asv/one.wav", g_asv_out, ASV_MAX_FRAMES, &got,
                         NULL);
        }
        if (r == FM_OK)
        {
            /* and after Close the same writer opens again */
            r = AudioFileWriter_Open(&w, "/asv/one.wav", FM_AUDIO_FILE_AUTO,
                                     &cfg);
            if (r == FM_OK)
            {
                r = AudioFileWriter_Close(&w);
            }
        }
        if (r == FM_OK && second == FM_ERR_BUSY && same == FM_ERR_BUSY &&
            !other && got == 2000u &&
            memcmp(g_asv_in, g_asv_out, 2000u * sizeof(int16_t)) == 0)
        {
            PASS();
        }
        else
        {
            snprintf(msg, sizeof(msg), "r=%d second=%d same=%d other=%d "
                     "got=%u", (int)r, (int)second, (int)same, (int)other,
                     (unsigned)got);
            FAIL(msg);
        }
    }

    TEST("Save: card full -> the error, and a file that plays what was saved");
    {
        /* The card has `room` bytes free.  Close must report the failure
         * and still leave a WAV whose lengths describe exactly what is
         * on the card (they used to stay 0: a file of 0 seconds). */
        static const uint32_t rooms[] = { 0u, 30u, 44u, 61u, 1000u, 5001u,
                                          16384u, 40000u };
        uint32_t k;
        int bad = 0;

        msg[0] = '\0';
        (void)FS_Unmount("/ram");
        r = wfs_mount("/ram");
        if (r != FM_OK)
        {
            bad = 1;
            snprintf(msg, sizeof(msg), "wfs_mount failed: %d", (int)r);
        }
        asv_gen(0, g_asv_in, ASV_MAX_FRAMES, 2u);
        for (k = 0u; !bad && k < 2u * (sizeof(rooms) / sizeof(rooms[0])); k++)
        {
            uint32_t room = rooms[k / 2u];
            int adpcm = (int)(k & 1u);
            uint32_t head = adpcm ? 60u : 44u;
            AsvWav w;
            FM_Result wr;
            int rc;

            (void)File_Delete("/ram/asv/full.wav");
            g_wfs_card_free = (int64_t)room;
            /* 12000 stereo frames: 48 KB of PCM (more than any `room`
             * above), 12.3 KB of ADPCM (fits in the last two) */
            wr = asv_write("/ram/asv/full.wav",
                           adpcm ? FM_AUDIO_FILE_ADPCM : FM_AUDIO_FILE_WAV,
                           44100u, 2u, 0u, g_asv_in, ASV_MAX_FRAMES);
            if (wr == FM_OK && (!adpcm || room < 12000u))
            {
                /* it all fitted?  Then it was not a full card. */
                bad = 1;
                snprintf(msg, sizeof(msg), "room=%u adpcm=%d: no error",
                         (unsigned)room, adpcm);
                break;
            }
            g_wfs_card_free = -1;
            (void)File_SizeOf("/ram/asv/full.wav", &size);
            rc = (size >= head) ? asv_raw_wav("/ram/asv/full.wav", &w, 0) : 0;
            got = 0u;
            r = FM_OK;
            if (size >= head && rc == 0 && w.data != 0u)
            {
                r = asv_read("/ram/asv/full.wav", g_asv_out, ASV_MAX_FRAMES,
                             &got, NULL);
            }
            if (wr == FM_OK)
            {
                continue;                   /* ADPCM that did fit */
            }
            if (rc != 0 || r != FM_OK || size > (uint64_t)room ||
                (size >= head &&
                 ((!adpcm && (w.data != ((uint32_t)size - 44u) / 4u * 4u ||
                              got != w.data / 4u ||
                              memcmp(g_asv_in, g_asv_out, w.data) != 0)) ||
                  (adpcm && (w.data % w.block_align != 0u ||
                             w.fact != (w.data / w.block_align) *
                                           w.block_frames ||
                             got != w.fact)))) ||
                (!adpcm && room >= 16384u && w.data < 16384u - 44u) ||
                (adpcm && room >= 5001u && w.data < 4096u))
            {
                bad = 1;
                snprintf(msg, sizeof(msg), "room=%u adpcm=%d write=%d "
                         "rule=%d read=%d size=%lu data=%u fact=%u got=%u",
                         (unsigned)room, adpcm, (int)wr, rc, (int)r,
                         (unsigned long)size, (unsigned)w.data,
                         (unsigned)w.fact, (unsigned)got);
            }
        }
        g_wfs_card_free = -1;
        (void)File_Delete("/ram/asv/full.wav");
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

    TEST("Save: never closed -> plays up to the last checkpoint");
    {
        /* 600 KB of PCM (and of ADPCM, FLAC) go to the writer, then the
         * "power is lost".  The checkpoint is every 256 KB, so at least
         * 256 KB of each file must be there, with lengths to match.
         *
         * The file goes through the counting mount: a checkpoint is the
         * lengths *and* a flush that reaches the file system, and only
         * the count shows the second half (asv_abandon closes the handle,
         * which would hide a checkpoint that never flushed).
         *
         * With FM_AUDIO_ENC_SYNC_BYTES = 0 there are no checkpoints: no
         * flush, the lengths stay 0 -- and the reader plays such a WAV
         * to the end of the file all the same. */
        static const FM_AudioFileFormat fmts[3] = {
            FM_AUDIO_FILE_WAV, FM_AUDIO_FILE_ADPCM, FM_AUDIO_FILE_FLAC
        };
        static const char *const names[3] = {
            "/ram/asv/cut.wav", "/ram/asv/cuta.wav", "/ram/asv/cut.flac"
        };
        int bad = 0;

        msg[0] = '\0';
        (void)FS_Unmount("/ram");
        r = wfs_mount("/ram");
        if (r != FM_OK)
        {
            bad = 1;
            snprintf(msg, sizeof(msg), "wfs_mount failed: %d", (int)r);
        }
        for (i = 0u; i < 3u && !bad; i++)
        {
            FM_AudioFileWriter w;
            FM_AudioEncodeConfig cfg;
            AsvWav raw;
            uint32_t pass;
            uint32_t passes = (i == 0u) ? 13u : 60u;
            uint32_t fed = 0u;
            uint32_t flushes;
            int rc = 0;
            int lengths_ok;

            if (!Audio_CanEncode(fmts[i]))
            {
                continue;
            }
            g_wfs_flush_calls = 0u;
            memset(&w, 0, sizeof(w));
            memset(&raw, 0, sizeof(raw));
            memset(&cfg, 0, sizeof(cfg));
            cfg.sample_rate = 44100u;
            cfg.channels = 2u;
            cfg.bits_per_sample = 16u;
            asv_gen(1, g_asv_in, ASV_MAX_FRAMES, 2u);    /* noise: FLAC  */
            r = AudioFileWriter_Open(&w, names[i], fmts[i], &cfg); /* stays big */
            for (pass = 0u; r == FM_OK && pass < passes; pass++)
            {
                r = AudioFileWriter_WriteFrames(&w, g_asv_in, ASV_MAX_FRAMES,
                                                NULL);
                fed += ASV_MAX_FRAMES;
            }
            flushes = g_wfs_flush_calls;
            asv_abandon(&w);
            (void)File_SizeOf(names[i], &size);
            got = 0u;
            if (i < 2u)
            {
                rc = asv_raw_wav(names[i], &raw, 1);
                if (r == FM_OK && rc == 0)
                {
                    r = asv_read(names[i], g_asv_out, ASV_MAX_FRAMES, &got,
                                 NULL);
                }
            }
            else if (r == FM_OK)
            {
                /* a FLAC reader stops at the frame that was cut; all the
                 * blocks before it must come back */
                (void)asv_read(names[i], g_asv_out, ASV_MAX_FRAMES, &got,
                               NULL);
            }
#if ASV_SYNC_BYTES > 0u && ASV_SYNC_BYTES <= 262144u
            /* every file here is past two checkpoints of 256 KB (more of
             * them with a shorter distance) */
            lengths_ok =
                flushes >= 2u &&
                (i != 0u || (raw.data >= 262144u - 44u - 16384u &&
                             raw.data <= fed * 4u && (raw.data % 4u) == 0u)) &&
                (i != 1u || (raw.data >= 262144u - 60u - 16384u &&
                             (raw.data % raw.block_align) == 0u &&
                             raw.fact == (raw.data / raw.block_align) *
                                             raw.block_frames &&
                             raw.fact <= fed));
#elif ASV_SYNC_BYTES == 0u
            lengths_ok = (flushes == 0u) &&
                         (i == 2u || (raw.data == 0u && raw.fact == 0u));
#else
            /* a longer distance: whether a checkpoint falls inside these
             * files is not known here; that they play is checked below */
            lengths_ok = 1;
            (void)flushes;
#endif
            if (r != FM_OK || rc != 0 || size < 262144u || !lengths_ok ||
                got != ASV_MAX_FRAMES ||
                (i == 0u && memcmp(g_asv_in, g_asv_out,
                                   ASV_MAX_FRAMES * 4u) != 0))
            {
                bad = 1;
                snprintf(msg, sizeof(msg), "%s r=%d rule=%d size=%lu "
                         "data=%u fact=%u fed=%u got=%u flushes=%u",
                         names[i], (int)r, rc, (unsigned long)size,
                         (unsigned)raw.data, (unsigned)raw.fact,
                         (unsigned)fed, (unsigned)got, (unsigned)flushes);
            }
            (void)File_Delete(names[i]);
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

    (void)Directory_DeleteRecursive("/asv");
}
