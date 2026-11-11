/*
 * audit_v3_tests.h -- tests for defects found after the second audit round.
 *
 * Included by test/main.c.
 *
 * MP3 audio inside a video file
 * -----------------------------
 * Found while measuring stack frames for the ARM target:
 * Video_ReadAudioFrame had a 16 KB frame.  Reading why showed that it
 * created a new MP3 decoder for every packet, which cannot work (see
 * video_mp3_read() in src/fm_video.c), and that no test had ever decoded
 * an MP3 track from a container.  An AVI written by ffmpeg gave one frame
 * of sound and then FM_ERR_CORRUPTED on every call, for ever.
 *
 * Vectors (tools/gen_avi_mp3_vectors.py):
 *   test/vectors/avi_mp3.avi         47 MP3 frames, one per chunk, 44.1 kHz
 *                                    stereo, a 440 Hz tone
 *   test/vectors/avi_mp3_packed.avi  the same stream, eight frames a chunk
 * ffmpeg decodes the first to 108288 samples; this library's output is
 * the same within one LSB (checked when the fix was made, not here: the
 * reference PCM would be 216 KB in the repository).
 */
#ifndef AUDIT_V3_TESTS_H
#define AUDIT_V3_TESTS_H

#if FM_USE_VIDEO && FM_USE_MP3

#define AV3_MP3_FRAMES   47u
#define AV3_MP3_PCM      (AV3_MP3_FRAMES * 1152u * 2u * 2u)   /* bytes */

static uint8_t g_av3_pcm[AV3_MP3_PCM + 4608u];      /* run under test   */
static uint8_t g_av3_ref[AV3_MP3_PCM + 4608u];      /* plain file, big buffer */
static uint8_t g_av3_file[32768];

typedef struct
{
    unsigned ok;            /* calls that returned PCM          */
    unsigned bad;           /* calls that returned an error     */
    FM_Result first_bad;
    size_t   bytes;
    int      ended;         /* FM_ERR_EOF seen                  */
    size_t   max_chunk;
} Av3Run;

/* Read the audio track from the current position to its end with a
 * buffer of `cap` bytes; at most `limit` calls. */
static void av3_drain(FM_Video *v, size_t cap, unsigned limit, Av3Run *run)
{
    static uint8_t chunk[65536];
    unsigned i;

    memset(run, 0, sizeof(*run));
    for (i = 0; i < limit; i++)
    {
        size_t n = 0;
        FM_Result r = Video_ReadAudioFrame(v, chunk, cap, &n);

        if (r == FM_ERR_EOF)
        {
            run->ended = 1;
            break;
        }
        if (r != FM_OK)
        {
            if (run->bad == 0u)
            {
                run->first_bad = r;
            }
            run->bad++;
            continue;
        }
        run->ok++;
        if (n > run->max_chunk)
        {
            run->max_chunk = n;
        }
        if (run->bytes + n <= sizeof(g_av3_pcm))
        {
            memcpy(g_av3_pcm + run->bytes, chunk, n);
        }
        run->bytes += n;
    }
}

static int16_t av3_left(const uint8_t *pcm, size_t frame)
{
    const uint8_t *p = pcm + frame * 4u;
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void av3_report(const Av3Run *run, char *m, size_t cap)
{
    (void)snprintf(m, cap, "ok %u, errors %u (first %d), %lu bytes, end %d",
                   run->ok, run->bad, (int)run->first_bad,
                   (unsigned long)run->bytes, run->ended);
}

static void test_video_mp3_track(void)
{
    static FM_Video v;
    Av3Run run;
    char m[120];
    FM_Result r;
    int have_ref = 0;

    TEST("MP3 in AVI: every packet decodes and the track ends");
    {
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_mp3.avi", &v);
        if (r != FM_OK)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            if (run.ok != AV3_MP3_FRAMES || run.bad != 0u || !run.ended ||
                run.bytes != (size_t)AV3_MP3_PCM)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                memcpy(g_av3_ref, g_av3_pcm, AV3_MP3_PCM);
                have_ref = 1;
                PASS();
            }
        }
    }

    TEST("MP3 in AVI: the tone is whole (no dropped or silent frame)");
    {
        if (!have_ref)
        {
            FAIL("no decoded track to look at");
        }
        else
        {
            /* 440 Hz at 44.1 kHz between frames 4000 and 50000: 918 zero
             * crossings, and never more than a sample or two near zero.
             * A frame that was dropped shortens the count; one that was
             * replaced by silence is a quiet run of 1152. */
            unsigned crossings = 0u;
            unsigned quiet = 0u;
            unsigned quiet_max = 0u;
            size_t i;

            for (i = 4000u; i < 50000u; i++)
            {
                int16_t a = av3_left(g_av3_ref, i - 1u);
                int16_t b = av3_left(g_av3_ref, i);

                if ((a < 0) != (b < 0))
                {
                    crossings++;
                }
                if (b > -8 && b < 8)
                {
                    quiet++;
                    if (quiet > quiet_max)
                    {
                        quiet_max = quiet;
                    }
                }
                else
                {
                    quiet = 0u;
                }
            }
            if (crossings < 908u || crossings > 928u || quiet_max > 16u)
            {
                (void)snprintf(m, sizeof(m),
                               "%u zero crossings (918 expected), quiet run %u",
                               crossings, quiet_max);
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("MP3 in AVI: a packet of several frames is continued in a small buffer");
    {
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_mp3_packed.avi", &v);
        if (r != FM_OK || !have_ref)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            /* 8192 bytes hold one frame (4608) but not two: one frame a
             * call, 47 calls, and the same PCM as the plain file. */
            av3_drain(&v, 8192u, 500u, &run);
            (void)Video_Close(&v);
            if (run.ok != AV3_MP3_FRAMES || run.bad != 0u || !run.ended ||
                run.bytes != (size_t)AV3_MP3_PCM || run.max_chunk != 4608u)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else if (memcmp(g_av3_pcm, g_av3_ref, AV3_MP3_PCM) != 0)
            {
                FAIL("PCM differs from the one-frame-per-packet file");
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("MP3 in AVI: a large buffer takes a whole packet of frames");
    {
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_mp3_packed.avi", &v);
        if (r != FM_OK || !have_ref)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            if (run.ok != 6u || run.bad != 0u || !run.ended ||
                run.bytes != (size_t)AV3_MP3_PCM ||
                memcmp(g_av3_pcm, g_av3_ref, AV3_MP3_PCM) != 0)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("MP3 in AVI: a buffer smaller than a frame still reaches the end");
    {
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_mp3_packed.avi", &v);
        if (r != FM_OK)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            /* sound is lost (documented), the track must not stall */
            av3_drain(&v, 1000u, 500u, &run);
            (void)Video_Close(&v);
            if (run.ok != AV3_MP3_FRAMES || run.bad != 0u || !run.ended ||
                run.max_chunk != 1000u)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("MP3 in AVI: a damaged packet is reported once and skipped");
    {
        size_t got = 0;
        size_t i;
        unsigned seen = 0u;
        size_t hit = 0u;

        r = File_ReadAll("/test/vectors/avi_mp3.avi", g_av3_file,
                         sizeof(g_av3_file), &got);
        /* wipe the payload of the 20th audio chunk */
        for (i = 0; r == FM_OK && i + 8u < got && hit == 0u; i++)
        {
            if (memcmp(g_av3_file + i, "01wb", 4) == 0 && ++seen == 20u)
            {
                hit = i;
            }
        }
        if (hit != 0u)
        {
            uint32_t sz = (uint32_t)g_av3_file[hit + 4u] |
                          ((uint32_t)g_av3_file[hit + 5u] << 8) |
                          ((uint32_t)g_av3_file[hit + 6u] << 16) |
                          ((uint32_t)g_av3_file[hit + 7u] << 24);

            if (sz < 64u || hit + 8u + sz > got)
            {
                hit = 0u;
            }
            else
            {
                memset(g_av3_file + hit + 8u, 0, sz);
                r = File_WriteAll("/av3_bad.avi", g_av3_file, got);
            }
        }
        memset(&v, 0, sizeof(v));
        if (r != FM_OK || hit == 0u ||
            Video_Open("/av3_bad.avi", &v) != FM_OK)
        {
            FAIL("could not build the damaged file");
        }
        else
        {
            /* 46 packets give PCM (the one after the hole as silence:
             * its data began in the lost frame), one is refused, and
             * the end is reached.  It used to return the same error for
             * every one of the 500 calls. */
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            if (run.ok != AV3_MP3_FRAMES - 1u || run.bad != 1u ||
                run.first_bad != FM_ERR_CORRUPTED || !run.ended ||
                run.bytes != (size_t)AV3_MP3_PCM - 4608u)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
        (void)File_Delete("/av3_bad.avi");
    }

    TEST("MP3 in AVI: after a seek the rest of the track decodes");
    {
        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_mp3.avi", &v);
        if (r != FM_OK)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            size_t n = 0;
            static uint8_t one[8192];

            (void)Video_ReadAudioFrame(&v, one, sizeof(one), &n);
            r = Video_SeekAudio(&v, 20u);
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            if (r != FM_OK || run.ok != AV3_MP3_FRAMES - 20u ||
                run.bad != 0u || !run.ended ||
                run.bytes != (size_t)(AV3_MP3_FRAMES - 20u) * 4608u)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }
}

#endif /* FM_USE_VIDEO && FM_USE_MP3 */

#endif /* AUDIT_V3_TESTS_H */
