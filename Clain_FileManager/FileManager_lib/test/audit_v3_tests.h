/*
 * audit_v3_tests.h -- tests for defects found after the second audit round.
 *
 * Included by test/main.c.
 *
 * The audio track of a video file
 * -------------------------------
 * Found while measuring stack frames for the ARM target:
 * Video_ReadAudioFrame had a 16 KB frame.  Reading why showed that it
 * created a new MP3 decoder for every packet, which cannot work, and that
 * no test had ever decoded a compressed audio track out of a container:
 * the tests built their containers by hand and checked that the codec is
 * recognised.  Files written by ffmpeg were then run through every
 * container / codec pair the library lists, next to ffmpeg's own decode:
 *
 *   MP3 (AVI, MP4, MKV, FLV)   one frame of sound, then FM_ERR_CORRUPTED
 *                              on every call, for ever
 *   IMA ADPCM, stereo          noise (decoded as one mono block)
 *   Microsoft ADPCM            full-scale noise (run through the IMA tables)
 *   Vorbis (MKV, WebM)         FM_ERR_CORRUPTED on every call
 *   Vorbis in an MP4 "mp4a"    announced as AAC, handed out in ADTS frames
 *
 * PCM, A-law, mu-law and FLAC were right.  The fixes are in
 * src/fm_video.c (video_mp3_read, video_adpcm_read, video_vorbis_read)
 * and src/fm_vorbis.c (FM_Vorbis_Stream*).
 *
 * Vectors (tools/gen_video_audio_vectors.py), all a 440 Hz tone:
 *   avi_mp3.avi              47 MP3 frames, one per chunk, 44.1 kHz stereo
 *   avi_mp3_packed.avi       the same stream, eight frames a chunk
 *   avi_adpcm_ima.avi        11 IMA blocks, 11.025 kHz stereo
 *   avi_adpcm_ima_packed.avi the same stream, eight blocks a chunk
 *   avi_adpcm_ms.avi         11 Microsoft ADPCM blocks, 11.025 kHz stereo
 *   mkv_vorbis.mkv           45 Vorbis packets, 22.05 kHz stereo
 *
 * ADPCM is integer arithmetic: the PCM must be ffmpeg's byte for byte,
 * and is compared by length and CRC-32 (the numbers the generator
 * prints).  MP3 and Vorbis go through floating point; their output was
 * within one LSB of ffmpeg's when the fixes were made, and is checked
 * here for length and for the tone being whole.
 */
#ifndef AUDIT_V3_TESTS_H
#define AUDIT_V3_TESTS_H

#if FM_USE_VIDEO

#include "fm_vorbis.h"

#define AV3_MP3_FRAMES   47u
#define AV3_MP3_PCM      (AV3_MP3_FRAMES * 1152u * 2u * 2u)   /* bytes */

static uint8_t g_av3_pcm[AV3_MP3_PCM + 4608u];      /* run under test   */
#if FM_USE_MP3 || (FM_USE_AUDIO && FM_USE_VORBIS)
static uint8_t g_av3_ref[AV3_MP3_PCM + 4608u];      /* plain file, big buffer */
#endif
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

/* Left channel of stereo PCM, frames [first, last): zero crossings, and
 * the longest run of samples near zero.  A tone that lost a frame has
 * fewer crossings; one with a frame replaced by silence has a long
 * quiet run. */
static void av3_tone(const uint8_t *pcm, size_t first, size_t last,
                     unsigned *crossings, unsigned *quiet_max)
{
    unsigned quiet = 0u;
    size_t i;

    *crossings = 0u;
    *quiet_max = 0u;
    for (i = first; i < last; i++)
    {
        int16_t a = av3_left(pcm, i - 1u);
        int16_t b = av3_left(pcm, i);

        if ((a < 0) != (b < 0))
        {
            (*crossings)++;
        }
        if (b > -8 && b < 8)
        {
            quiet++;
            if (quiet > *quiet_max)
            {
                *quiet_max = quiet;
            }
        }
        else
        {
            quiet = 0u;
        }
    }
}

/* CRC-32 as zlib computes it (what the vector generator prints). */
static uint32_t av3_crc32(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;

    for (i = 0; i < n; i++)
    {
        crc ^= d[i];
        for (b = 0; b < 8; b++)
        {
            crc = ((crc & 1u) != 0u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Offset of the payload of the nth (1 = first) audio chunk of an AVI
 * image, 0 when there is none.  The fourcc alone is not enough: it also
 * stands in the index tables, followed by other things than a size. */
static size_t av3_audio_chunk(const uint8_t *avi, size_t len, unsigned nth,
                              uint32_t *size)
{
    size_t i;

    for (i = 0; i + 8u < len; i++)
    {
        if (memcmp(avi + i, "01wb", 4) == 0)
        {
            uint32_t sz = (uint32_t)avi[i + 4u] |
                          ((uint32_t)avi[i + 5u] << 8) |
                          ((uint32_t)avi[i + 6u] << 16) |
                          ((uint32_t)avi[i + 7u] << 24);

            if (sz >= 64u && sz <= 65536u && i + 8u + sz <= len)
            {
                if (--nth == 0u)
                {
                    *size = sz;
                    return i + 8u;
                }
                i += 7u + sz;
            }
        }
    }
    return 0u;
}

/* Open `path`, read its audio track to the end with `cap`. */
static FM_Result av3_decode(const char *path, size_t cap, Av3Run *run)
{
    static FM_Video v;
    FM_Result r;

    memset(&v, 0, sizeof(v));
    memset(run, 0, sizeof(*run));
    r = Video_Open(path, &v);
    if (r == FM_OK)
    {
        av3_drain(&v, cap, 500u, run);
        (void)Video_Close(&v);
    }
    return r;
}

static void av3_report(const Av3Run *run, char *m, size_t cap)
{
    (void)snprintf(m, cap, "ok %u, errors %u (first %d), %lu bytes, end %d",
                   run->ok, run->bad, (int)run->first_bad,
                   (unsigned long)run->bytes, run->ended);
}

#if FM_USE_MP3
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
            unsigned crossings;
            unsigned quiet_max;

            av3_tone(g_av3_ref, 4000u, 50000u, &crossings, &quiet_max);
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
        size_t hit = 0u;
        uint32_t sz = 0u;

        r = File_ReadAll("/test/vectors/avi_mp3.avi", g_av3_file,
                         sizeof(g_av3_file), &got);
        if (r == FM_OK)
        {
            /* wipe the payload of the 20th audio chunk */
            hit = av3_audio_chunk(g_av3_file, got, 20u, &sz);
        }
        if (hit != 0u)
        {
            memset(g_av3_file + hit, 0, sz);
            r = File_WriteAll("/av3_bad.avi", g_av3_file, got);
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

#endif /* FM_USE_MP3 */

/* ------------------------------------------------------------------ */
/* ADPCM                                                               */
/* ------------------------------------------------------------------ */
#define AV3_IMA_BYTES   44748u
#define AV3_IMA_CRC     0x35D60576u
#define AV3_MS_BYTES    44528u
#define AV3_MS_CRC      0xF194CDF0u

static void test_video_adpcm_track(void)
{
    Av3Run run;
    char m[120];
    FM_Result r;

    TEST("ADPCM in AVI: IMA stereo gives ffmpeg's PCM, byte for byte");
    {
        r = av3_decode("/test/vectors/avi_adpcm_ima.avi", 65536u, &run);
        if (r != FM_OK || run.ok != 11u || run.bad != 0u || !run.ended ||
            run.bytes != (size_t)AV3_IMA_BYTES ||
            av3_crc32(g_av3_pcm, run.bytes) != AV3_IMA_CRC)
        {
            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
        else
        {
            unsigned crossings;
            unsigned quiet_max;

            /* and it is the tone: 440 Hz at 11.025 kHz, frames 500..10500 */
            av3_tone(g_av3_pcm, 500u, 10500u, &crossings, &quiet_max);
            if (crossings < 790u || crossings > 806u || quiet_max > 16u)
            {
                (void)snprintf(m, sizeof(m), "%u zero crossings (798 expected)",
                               crossings);
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("ADPCM in AVI: Microsoft ADPCM gives ffmpeg's PCM, byte for byte");
    {
        r = av3_decode("/test/vectors/avi_adpcm_ms.avi", 65536u, &run);
        if (r != FM_OK || run.ok != 11u || run.bad != 0u || !run.ended ||
            run.bytes != (size_t)AV3_MS_BYTES ||
            av3_crc32(g_av3_pcm, run.bytes) != AV3_MS_CRC)
        {
            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
        else
        {
            PASS();
        }
    }

    TEST("ADPCM in AVI: a chunk of several blocks, in a large and a small buffer");
    {
        /* eight blocks a chunk: 2 calls with room for all of them, one
         * block a call (4068 bytes of PCM) with room for one */
        r = av3_decode("/test/vectors/avi_adpcm_ima_packed.avi", 65536u, &run);
        if (r != FM_OK || run.ok != 2u || run.bad != 0u || !run.ended ||
            run.bytes != (size_t)AV3_IMA_BYTES ||
            av3_crc32(g_av3_pcm, run.bytes) != AV3_IMA_CRC)
        {
            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
        else
        {
            r = av3_decode("/test/vectors/avi_adpcm_ima_packed.avi", 4096u,
                           &run);
            if (r != FM_OK || run.ok != 11u || run.bad != 0u || !run.ended ||
                run.bytes != (size_t)AV3_IMA_BYTES ||
                av3_crc32(g_av3_pcm, run.bytes) != AV3_IMA_CRC)
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

    TEST("ADPCM in AVI: a buffer smaller than a block is refused, nothing is lost");
    {
        static FM_Video v;
        static uint8_t small[1000];
        size_t n = 99u;
        FM_Result r1;
        FM_Result r2;

        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/avi_adpcm_ima.avi", &v);
        if (r != FM_OK)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            r1 = Video_ReadAudioFrame(&v, small, sizeof(small), &n);
            r2 = Video_ReadAudioFrame(&v, small, sizeof(small), &n);
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            if (r1 != FM_ERR_TOO_LARGE || r2 != FM_ERR_TOO_LARGE ||
                run.bytes != (size_t)AV3_IMA_BYTES ||
                av3_crc32(g_av3_pcm, run.bytes) != AV3_IMA_CRC)
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

    TEST("ADPCM in AVI: a block with an unknown predictor is silence, the rest plays");
    {
        size_t got = 0;
        size_t i;
        size_t hit = 0u;
        uint32_t sz = 0u;

        r = File_ReadAll("/test/vectors/avi_adpcm_ms.avi", g_av3_file,
                         sizeof(g_av3_file), &got);
        if (r == FM_OK)
        {
            hit = av3_audio_chunk(g_av3_file, got, 1u, &sz);
        }
        if (r != FM_OK || hit == 0u)
        {
            FAIL("could not build the damaged file");
        }
        else
        {
            g_av3_file[hit] = 0x77u;        /* predictor number: 0..6 */
            (void)sz;
            r = File_WriteAll("/av3_bad.avi", g_av3_file, got);
            if (r == FM_OK)
            {
                r = av3_decode("/av3_bad.avi", 65536u, &run);
            }
            /* same length as the intact file; the first block (1012
             * frames) is zeros, the tone starts with the second */
            if (r != FM_OK || run.ok != 11u || run.bad != 0u || !run.ended ||
                run.bytes != (size_t)AV3_MS_BYTES)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                int silent = 1;
                int later = 0;

                for (i = 0; i < 4000u; i++)
                {
                    if (g_av3_pcm[i] != 0u)
                    {
                        silent = 0;
                    }
                }
                for (i = 8000u; i < 12000u; i++)
                {
                    if (g_av3_pcm[i] != 0u)
                    {
                        later = 1;
                    }
                }
                if (!silent || !later)
                {
                    FAIL("the damaged block is not silence, or the next is");
                }
                else
                {
                    PASS();
                }
            }
        }
        (void)File_Delete("/av3_bad.avi");
    }
}

/* ------------------------------------------------------------------ */
/* Vorbis                                                              */
/* ------------------------------------------------------------------ */
#if FM_USE_AUDIO && FM_USE_VORBIS

#define AV3_VORBIS_BYTES   89600u       /* 22400 stereo frames */

static void test_video_vorbis_track(void)
{
    Av3Run run;
    char m[120];
    FM_Result r;
    int have_ref = 0;

    TEST("Vorbis in MKV: every packet decodes and the track ends");
    {
        /* 45 packets; the first has no output of its own and is passed
         * over inside the first call */
        r = av3_decode("/test/vectors/mkv_vorbis.mkv", 65536u, &run);
        if (r != FM_OK || run.ok != 44u || run.bad != 0u || !run.ended ||
            run.bytes != (size_t)AV3_VORBIS_BYTES)
        {
            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
        else
        {
            memcpy(g_av3_ref, g_av3_pcm, AV3_VORBIS_BYTES);
            have_ref = 1;
            PASS();
        }
    }

    TEST("Vorbis in MKV: the tone is whole");
    {
        if (!have_ref)
        {
            FAIL("no decoded track to look at");
        }
        else
        {
            unsigned crossings;
            unsigned quiet_max;

            /* 440 Hz at 22.05 kHz, frames 2000..20000: 718 crossings */
            av3_tone(g_av3_ref, 2000u, 20000u, &crossings, &quiet_max);
            if (crossings < 710u || crossings > 726u || quiet_max > 16u)
            {
                (void)snprintf(m, sizeof(m),
                               "%u zero crossings (718 expected), quiet run %u",
                               crossings, quiet_max);
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
    }

    TEST("Vorbis in MKV: a small buffer gets the same PCM in more calls");
    {
        r = av3_decode("/test/vectors/mkv_vorbis.mkv", 256u, &run);
        if (r != FM_OK || !have_ref || run.bad != 0u || !run.ended ||
            run.max_chunk != 256u || run.bytes != (size_t)AV3_VORBIS_BYTES ||
            memcmp(g_av3_pcm, g_av3_ref, AV3_VORBIS_BYTES) != 0)
        {
            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
        else
        {
            PASS();
        }
    }

    TEST("Vorbis in MKV: after a seek the rest of the track decodes");
    {
        static FM_Video v;
        static uint8_t one[8192];
        size_t n = 0;

        memset(&v, 0, sizeof(v));
        r = Video_Open("/test/vectors/mkv_vorbis.mkv", &v);
        if (r != FM_OK)
        {
            FAIL("cannot open the vector");
        }
        else
        {
            (void)Video_ReadAudioFrame(&v, one, sizeof(one), &n);
            r = Video_SeekAudio(&v, 20u);
            av3_drain(&v, 65536u, 500u, &run);
            (void)Video_Close(&v);
            /* 25 packets are left; the decoder needs up to three of
             * them to find its place again */
            if (r != FM_OK || run.ok < 22u || run.ok > 25u || run.bad != 0u ||
                !run.ended)
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

    TEST("Vorbis in MKV: unusable headers are an error per packet, and the end is reached");
    {
        size_t got = 0;
        size_t i;
        size_t hit = 0u;

        r = File_ReadAll("/test/vectors/mkv_vorbis.mkv", g_av3_file,
                         sizeof(g_av3_file), &got);
        for (i = 0; r == FM_OK && i + 8u < got && hit == 0u; i++)
        {
            if (memcmp(g_av3_file + i, "\x05vorbis", 7) == 0)
            {
                hit = i;                 /* the setup header */
            }
        }
        if (r != FM_OK || hit == 0u)
        {
            FAIL("could not build the damaged file");
        }
        else
        {
            g_av3_file[hit + 3u] = 'X';
            r = File_WriteAll("/av3_bad.mkv", g_av3_file, got);
            if (r == FM_OK)
            {
                r = av3_decode("/av3_bad.mkv", 65536u, &run);
            }
            if (r != FM_OK || run.ok != 0u || run.bad != 45u ||
                run.first_bad != FM_ERR_CORRUPTED || !run.ended)
            {
                av3_report(&run, m, sizeof(m));
                FAIL(m);
            }
            else
            {
                PASS();
            }
        }
        (void)File_Delete("/av3_bad.mkv");
    }

    TEST("Vorbis stream decoder: bad arguments and bad headers are refused");
    {
        static const uint8_t junk[64] = { 1, 'v', 'o', 'r', 'b', 'i', 's', 9 };
        const uint8_t *hdr[3];
        uint32_t len[3];
        FM_VorbisStream *vs = (FM_VorbisStream *)(void *)m;  /* must be cleared */
        uint8_t pcm[16];
        size_t n = 7u;
        int ok = 1;

        hdr[0] = junk; hdr[1] = junk; hdr[2] = junk;
        len[0] = 30u; len[1] = 8u; len[2] = 8u;

        if (FM_Vorbis_StreamOpen(NULL, len, &vs) != FM_ERR_INVALID_ARGUMENT ||
            vs != NULL)
        {
            ok = 0;
        }
        if (FM_Vorbis_StreamOpen(hdr, len, NULL) != FM_ERR_INVALID_ARGUMENT)
        {
            ok = 0;
        }
        /* an identification header that is not one */
        if (FM_Vorbis_StreamOpen(hdr, len, &vs) != FM_ERR_CORRUPTED ||
            vs != NULL)
        {
            ok = 0;
        }
        len[0] = 29u;                     /* it is 30 bytes, always */
        if (FM_Vorbis_StreamOpen(hdr, len, &vs) != FM_ERR_CORRUPTED ||
            vs != NULL)
        {
            ok = 0;
        }
        if (FM_Vorbis_StreamDecode(NULL, junk, 8u, pcm, sizeof(pcm), &n) !=
                FM_ERR_INVALID_ARGUMENT || n != 0u)
        {
            ok = 0;
        }
        if (FM_Vorbis_StreamPending(NULL) != 0u ||
            FM_Vorbis_StreamChannels(NULL) != 0u)
        {
            ok = 0;
        }
        FM_Vorbis_StreamReset(NULL);
        FM_Vorbis_StreamClose(NULL);
        if (!ok)
        {
            FAIL("a call was accepted, or left a handle behind");
        }
        else
        {
            PASS();
        }
    }
}
#endif /* FM_USE_AUDIO && FM_USE_VORBIS */

#endif /* FM_USE_VIDEO */

#endif /* AUDIT_V3_TESTS_H */
