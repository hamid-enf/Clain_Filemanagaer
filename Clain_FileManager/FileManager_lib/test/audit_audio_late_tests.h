/* Regression tests for ReportV41.txt (AUD5-01, -03, -04). */
#if FM_USE_AUDIO
#if FM_USE_AAC
static int aud_late_aac_calls;
static FM_Result aud_late_aac_init(void **ctx, const FM_AudioInfo *info)
{
    (void)info;
    *ctx = &aud_late_aac_calls;
    aud_late_aac_calls = 0;
    return FM_OK;
}
static FM_Result aud_late_aac_decode(void *ctx, const uint8_t *in,
    size_t in_size, int16_t *out, size_t cap, size_t *out_samples)
{
    int *calls = (int *)ctx;
    (void)in; (void)in_size;
    ++*calls;
    *out_samples = 0;
    if (*calls == 1) return FM_OK;          /* priming without samples */
    if (*calls == 3) return FM_ERR_CORRUPTED;
    if (cap == 0) return FM_ERR_INTERNAL;
    out[0] = (int16_t)*calls;
    *out_samples = 1;                      /* short mono frame */
    return FM_OK;
}
static void aud_late_aac_close(void *ctx) { (void)ctx; }
static const FM_AudioDecoderOps aud_late_aac_ops = {
    "AAC", aud_late_aac_init, aud_late_aac_decode, aud_late_aac_close
};
#endif
#if FM_USE_FLAC || FM_USE_VORBIS || (FM_USE_OPUS && FM_OPUS_USE_LIBOPUS)
static size_t aud_alloc_index, aud_alloc_fail_at, aud_alloc_live;
static void *aud_fail_alloc(size_t n, void *ctx)
{
    void *p;
    (void)ctx;
    ++aud_alloc_index;
    if (aud_alloc_index == aud_alloc_fail_at) return NULL;
    p = malloc(n);
    if (p != NULL) ++aud_alloc_live;
    return p;
}
static void aud_fail_free(void *p, void *ctx)
{
    (void)ctx;
    if (p != NULL) { --aud_alloc_live; free(p); }
}
static FM_Result aud_fail_sweep(const char *path)
{
    FM_Allocator saved = *FM_Allocator_Get();
    FM_Allocator inject = { aud_fail_alloc, NULL, NULL, aud_fail_free, NULL };
    size_t fail;
    FM_Result result = FM_ERR_INTERNAL;
    for (fail = 1; fail <= 512; ++fail)
    {
        FM_Audio a;
        int16_t pcm[512];
        size_t n = 0;
        FM_Result r;
        memset(&a, 0, sizeof(a));
        aud_alloc_live = aud_alloc_index = 0;
        aud_alloc_fail_at = fail;
        if (FM_Allocator_Set(&inject) != FM_OK) break;
        r = Audio_Open(path, &a);
        if (r == FM_OK) {
            r = Audio_Read(&a, pcm, 512, &n);
            (void)Audio_Close(&a);
        }
        (void)FM_Allocator_Set(&saved);
        if (aud_alloc_live != 0) {
            printf("[failure %lu: %lu allocations leaked] ",
                   (unsigned long)fail, (unsigned long)aud_alloc_live);
            return FM_ERR_INTERNAL;
        }
        if (aud_alloc_index < fail) {
            if (r == FM_OK && n > 0) result = FM_OK;
            break;
        }
    }
    (void)FM_Allocator_Set(&saved);
    return result;
}
#endif
static int aud_stream_close_calls;
static void aud_stream_close_cb(const int16_t *pcm, size_t count, void *ctx)
{
    (void)pcm; (void)count;
    ++aud_stream_close_calls;
    (void)Audio_Close((FM_Audio *)ctx);
}
static FM_Result aud_stream_close_check(const char *path)
{
    FM_Audio a;
    FM_Result r;
    int16_t pcm[512];
    size_t n = 0;
    memset(&a, 0, sizeof(a));
    r = Audio_Open(path, &a);
    if (r != FM_OK) return r;
    aud_stream_close_calls = 0;
    r = Audio_Stream(&a, aud_stream_close_cb, &a, 512);
    if (r != FM_ERR_INVALID_ARGUMENT || aud_stream_close_calls != 1) {
        (void)Audio_Close(&a); return FM_ERR_INTERNAL;
    }
    memset(&a, 0, sizeof(a));
    r = Audio_Open(path, &a);
    if (r != FM_OK) return r;
    aud_stream_close_calls = 0;
    r = Audio_SetCallback(&a, aud_stream_close_cb, &a);
    if (r == FM_OK) r = Audio_Read(&a, pcm, 512, &n);
    if (r != FM_OK || n == 0 || aud_stream_close_calls != 1) {
        (void)Audio_Close(&a); return FM_ERR_INTERNAL;
    }
    n = 42;
    r = Audio_Read(&a, pcm, 512, &n);
    return (r == FM_ERR_INVALID_ARGUMENT && n == 0) ? FM_OK : FM_ERR_INTERNAL;
}
static FM_Result aud5_tagged(const char *src, int footer, FM_MediaType kind)
{
    static FM_Audio a;
    static int16_t ref[512], gotpcm[512];
    size_t n, got = 0, nref = 0, ngot = 0;
    FM_Result r;
    n = av4_id3(g_av4_abuf, 1000u, footer);
    r = File_ReadAll(src, g_av4_abuf + n, sizeof(g_av4_abuf) - n, &got);
    if (r == FM_OK) r = File_WriteAll("/aud5_tag.bin", g_av4_abuf, n + got);
#if FM_USE_MEDIA_DETECT
    if (r == FM_OK && (Media_DetectBuffer(g_av4_abuf, n + got, NULL) != kind ||
                       File_DetectType("/aud5_tag.bin") != kind)) r = FM_ERR_INTERNAL;
#else
    (void)kind;
#endif
    memset(&a, 0, sizeof(a));
    if (r == FM_OK) r = Audio_Open(src, &a);
    if (r == FM_OK) { r = Audio_Read(&a, ref, 512, &nref); (void)Audio_Close(&a); }
    memset(&a, 0, sizeof(a));
    if (r == FM_OK) r = Audio_Open("/aud5_tag.bin", &a);
    if (r == FM_OK) { r = Audio_Read(&a, gotpcm, 512, &ngot); (void)Audio_Close(&a); }
    if (r == FM_OK && (nref == 0 || nref != ngot || memcmp(ref, gotpcm, nref * sizeof(ref[0])) != 0)) r = FM_ERR_INTERNAL;
    (void)File_Delete("/aud5_tag.bin");
    return r;
}
static void test_audit_v5_audio_late(void)
{
    FM_Result r;
    FM_Audio a;
    int16_t pcm[512];
    size_t got;
    int footer;
    printf("\n== audit V5: audio ==\n");
    TEST("Audio callbacks: close a WAV while streaming or reading");
    r = aud_stream_close_check("/test/vectors/wav_alaw_8k.wav");
    CHECK_RESULT(r, "WAV callback close");
#if FM_USE_MP3
    TEST("Audio callbacks: close an MP3 while streaming or reading");
    r = aud_stream_close_check("/test/vectors/tone_mono.mp3");
    CHECK_RESULT(r, "MP3 callback close");
#endif
#if FM_USE_FLAC
    TEST("Audio callbacks: close a FLAC while streaming or reading");
    r = aud_stream_close_check("/test/vectors/test_flac.flac");
    CHECK_RESULT(r, "FLAC callback close");
#endif
#if FM_USE_VORBIS
    TEST("Audio callbacks: close Vorbis while streaming or reading");
    r = aud_stream_close_check("/test/vectors/test_vorbis.ogg");
    CHECK_RESULT(r, "Vorbis callback close");
#endif
#if FM_USE_FLAC
    TEST("FLAC: every open/read allocation can fail without leaking");
    r = aud_fail_sweep("/test/vectors/test_flac.flac");
    CHECK_RESULT(r, "FLAC allocation failure sweep");
#endif
#if FM_USE_VORBIS
    TEST("Vorbis: every open/read allocation can fail without leaking");
    r = aud_fail_sweep("/test/vectors/test_vorbis.ogg");
    CHECK_RESULT(r, "Vorbis allocation failure sweep");
#endif
#if FM_USE_OPUS && FM_OPUS_USE_LIBOPUS
    TEST("Opus: every open/read allocation can fail without leaking");
    r = aud_fail_sweep("/test/vectors/test_opus.ogg");
    CHECK_RESULT(r, "Opus allocation failure sweep");
#endif
#if FM_USE_AAC
    TEST("AAC plugin: priming, short output, error, then recovery");
    {
        size_t len, nr = 0;
        int16_t sample = 0;
        FM_Result q;
        memset(&a, 0, sizeof(a));
        r = Audio_RegisterDecoder("AAC", &aud_late_aac_ops);
        len = av2_build_adts(5);
        if (r == FM_OK) r = File_WriteAll("/aud_late.aac", g_av2_abuf, len);
        if (r == FM_OK) r = Audio_Open("/aud_late.aac", &a);
        if (r == FM_OK) {
            q = Audio_Read(&a, &sample, 1, &nr);
            if (q != FM_OK || nr != 1 || sample != 2) r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) {
            nr = 99; q = Audio_Read(&a, &sample, 1, &nr);
            if (q != FM_ERR_CORRUPTED || nr != 0) r = FM_ERR_INTERNAL;
        }
        if (r == FM_OK) {
            nr = 0; q = Audio_Read(&a, &sample, 1, &nr);
            if (q != FM_OK || nr != 1 || sample != 4) r = FM_ERR_INTERNAL;
        }
        (void)Audio_Close(&a);
        (void)Audio_UnregisterDecoder("AAC");
        (void)File_Delete("/aud_late.aac");
    }
    CHECK_RESULT(r, "AAC plugin short/zero/error handling");
#endif
    for (footer = 0; footer <= 1; ++footer)
    {
        TEST(footer ? "AUD5-01: tagged WAV with footer" : "AUD5-01: tagged WAV");
        r = aud5_tagged("/test/vectors/wav_alaw_8k.wav", footer, FM_MEDIA_WAV);
        CHECK_RESULT(r, "tagged WAV");
#if FM_USE_FLAC
        TEST(footer ? "AUD5-01: tagged FLAC with footer" : "AUD5-01: tagged FLAC");
        r = aud5_tagged("/test/vectors/test_flac.flac", footer, FM_MEDIA_FLAC);
        CHECK_RESULT(r, "tagged FLAC");
#endif
#if FM_USE_VORBIS
        TEST(footer ? "AUD5-01: tagged Vorbis with footer" : "AUD5-01: tagged Vorbis");
        r = aud5_tagged("/test/vectors/test_vorbis.ogg", footer, FM_MEDIA_OGG);
        CHECK_RESULT(r, "tagged Vorbis");
#endif
#if FM_USE_OPUS && FM_OPUS_USE_LIBOPUS
        TEST(footer ? "AUD5-01: tagged Opus with footer" : "AUD5-01: tagged Opus");
        r = aud5_tagged("/test/vectors/test_opus.ogg", footer, FM_MEDIA_OGG);
        CHECK_RESULT(r, "tagged Opus");
#endif
    }
#if FM_USE_MEDIA_DETECT
    TEST("AUD5-03: short ID3 is not sufficient to identify MP3");
    r = (Media_DetectBuffer("ID3", 3, NULL) == FM_MEDIA_UNKNOWN &&
         Media_DetectBuffer("ID3", 3, "x.flac") == FM_MEDIA_FLAC) ? FM_OK : FM_ERR_INTERNAL;
    CHECK_RESULT(r, "short tag");
    TEST("S-03: incomplete ID3 tag uses extension or unknown");
    {
        size_t z = av4_id3(g_av4_abuf, 40u, 0);
        (void)z;
        r = (Media_DetectBuffer(g_av4_abuf, 10, NULL) == FM_MEDIA_UNKNOWN &&
             Media_DetectBuffer(g_av4_abuf, 10, "x.flac") == FM_MEDIA_FLAC) ? FM_OK : FM_ERR_INTERNAL;
    }
    CHECK_RESULT(r, "incomplete tag");
#endif
#if FM_USE_MP3
    TEST("AUD5-04: past-end MP3 seek parks the decoder");
    memset(&a, 0, sizeof(a)); got = 0;
    r = Audio_Open("/test/vectors/mp3_info_44k.mp3", &a);
    if (r == FM_OK) r = Audio_Read(&a, pcm, 512, &got);
    if (r == FM_OK && got != 0) {
        r = Audio_Seek(&a, 1000.0);
        if (r == FM_ERR_EOF) { got = 0; r = Audio_Read(&a, pcm, 512, &got); if (r == FM_ERR_EOF && got == 0) r = FM_OK; else r = FM_ERR_INTERNAL; }
        else r = FM_ERR_INTERNAL;
    } else r = FM_ERR_INTERNAL;
    (void)Audio_Close(&a);
    CHECK_RESULT(r, "MP3 past-end seek");
#else
    (void)a; (void)pcm; (void)got;
#endif
}
#endif
