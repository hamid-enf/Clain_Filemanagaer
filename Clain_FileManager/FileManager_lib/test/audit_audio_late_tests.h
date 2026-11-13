/* Regression tests for ReportV41.txt (AUD5-01, -03, -04). */
#if FM_USE_AUDIO
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
