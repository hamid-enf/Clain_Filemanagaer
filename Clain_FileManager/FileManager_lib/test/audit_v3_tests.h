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
 *
 * The picture
 * -----------
 * The same comparison for the picture (ffmpeg files, every frame next to
 * ffmpeg's own decode) found:
 *
 *   H.264            25 dB from the reference: every stream was converted
 *                    to RGB as full range, and nearly every stream is
 *                    video range (16..235) -- black came out as 16, white
 *                    as 235
 *   JPEG / MJPEG     37 dB: picojpeg clamped green between its two chroma
 *                    terms; saturated blue had green 15 (third_party/
 *                    picojpeg/picojpeg.c, "FileManager patch").  This is
 *                    the still-image decoder too.
 *   MJPEG in an MP4  not playable: ffmpeg writes it as an "mp4v" entry
 *                    of object type 0x6C, which was taken for MPEG-4
 *
 * After the fixes H.264 is at 48 dB and MJPEG at 50 dB (what is left is
 * rounding and chroma upsampling).  Vectors: four patches, white and
 * black over red and blue, 64x48 --
 *   mp4_h264_patches.mp4       video range
 *   mp4_h264_patches_full.mp4  full range, flagged in the VUI
 *   mp4_mjpeg_patches.mp4      Motion JPEG as "mp4v"
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

/* 22050 stereo frames: the half second the file was made from.  (The
 * decoder gives 22400; the last 350 are the encoder's padding, marked
 * with DiscardPadding in the last block.) */
#define AV3_VORBIS_BYTES   88200u

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

/* ------------------------------------------------------------------ */
/* Encoder delay and padding                                            */
/* ------------------------------------------------------------------ */
/* The delay vectors are 0.2 s of silence, then a tone, 0.6 s in all,
 * mono.  An encoder puts its own delay in front and pads the end; the
 * container says how much (MP4 edit list, MKV CodecDelay and
 * DiscardPadding).  With that applied the tone starts where ffmpeg has
 * it and the track has the length of the sound it was made from
 * (tools/gen_video_audio_vectors.py prints both). */
static long av3_onset(size_t bytes)
{
    size_t k;

    for (k = 0; k + 1u < bytes && k + 1u < sizeof(g_av3_pcm); k += 2u)
    {
        int sample = (int16_t)((uint16_t)g_av3_pcm[k] |
                               ((uint16_t)g_av3_pcm[k + 1u] << 8));

        if (sample >= 1000 || sample <= -1000)
        {
            return (long)(k / 2u);
        }
    }
    return -1;
}

static void av3_delay_case(const char *what, const char *path, size_t cap,
                           size_t frames, long onset)
{
    Av3Run run;
    char m[160];
    FM_Result r;
    long at;

    TEST(what);
    r = av3_decode(path, cap, &run);
    at = av3_onset(run.bytes);
    if (r == FM_OK && run.bad == 0u && run.ended &&
        run.bytes == frames * 2u && at >= onset - 2 && at <= onset + 2)
    {
        PASS();
    }
    else
    {
        (void)snprintf(m, sizeof(m),
                       "open %d, errors %u, end %d, %lu frames (want %lu), "
                       "tone at %ld (want %ld)", (int)r, run.bad, run.ended,
                       (unsigned long)(run.bytes / 2u), (unsigned long)frames,
                       at, onset);
        FAIL(m);
    }
}

static void test_video_audio_delay(void)
{
#if FM_USE_MP3
    av3_delay_case("MP3 in MP4: the edit list removes the encoder delay and the padding",
                   "/test/vectors/mp4_mp3_delay.mp4", 4608u, 26460u, 8823);
    av3_delay_case("MP3 in MKV: CodecDelay and DiscardPadding do the same",
                   "/test/vectors/mkv_mp3_delay.mkv", 4608u, 26460u, 8823);

    TEST("MP3 in MP4: after Video_SeekAudio(0) the track is trimmed again");
    {
        static FM_Video v;
        Av3Run first;
        Av3Run again;
        uint32_t crc = 0u;
        FM_Result r;

        memset(&v, 0, sizeof(v));
        memset(&first, 0, sizeof(first));
        memset(&again, 0, sizeof(again));
        r = Video_Open("/test/vectors/mp4_mp3_delay.mp4", &v);
        if (r == FM_OK)
        {
            av3_drain(&v, 4608u, 500u, &first);
            crc = av3_crc32(g_av3_pcm, first.bytes);
            r = Video_SeekAudio(&v, 0u);
            av3_drain(&v, 4608u, 500u, &again);
            (void)Video_Close(&v);
        }
        if (r == FM_OK && first.bytes == 26460u * 2u &&
            again.bytes == first.bytes && again.ended &&
            av3_crc32(g_av3_pcm, again.bytes) == crc)
        {
            PASS();
        }
        else
        {
            char m[120];

            (void)snprintf(m, sizeof(m), "seek %d, %lu then %lu bytes", (int)r,
                           (unsigned long)first.bytes,
                           (unsigned long)again.bytes);
            FAIL(m);
        }
    }
#endif
#if FM_USE_AUDIO && FM_USE_VORBIS
    /* ffmpeg writes a CodecDelay for Vorbis too; the decoder's silent
     * first packet is that delay, so it must not be removed twice */
    av3_delay_case("Vorbis in MKV: the padding goes, the CodecDelay is not applied twice",
                   "/test/vectors/mkv_vorbis_delay.mkv", 8192u, 13230u, 4411);
    av3_delay_case("Vorbis in MKV: the same through a 64-byte buffer",
                   "/test/vectors/mkv_vorbis_delay.mkv", 64u, 13230u, 4411);
#endif
#if defined(FM_OPUS_USE_LIBOPUS) && FM_OPUS_USE_LIBOPUS
    av3_delay_case("Opus in MKV: pre-skip (CodecDelay) and padding removed",
                   "/test/vectors/mkv_opus_delay.mkv", 8192u, 28800u, 9603);
    av3_delay_case("Opus in MP4: pre-skip and padding removed (edit list)",
                   "/test/vectors/mp4_opus_delay.mp4", 8192u, 28800u, 9603);
#else
    TEST("A track with no decoder: every read says unsupported, and the track ends");
    {
        static FM_Video v;
        FM_AudioTrackInfo ti;
        Av3Run run;
        FM_Result r;

        memset(&v, 0, sizeof(v));
        memset(&ti, 0, sizeof(ti));
        memset(&run, 0, sizeof(run));
        r = Video_Open("/test/vectors/mkv_opus_delay.mkv", &v);
        if (r == FM_OK)
        {
            (void)Video_GetAudioInfo(&v, &ti);
            av3_drain(&v, 8192u, 500u, &run);
            (void)Video_Close(&v);
        }
        if (r == FM_OK && ti.decodable)
        {
            PASS();                      /* a decoder is registered here */
        }
        else if (r == FM_OK && ti.codec == FM_VIDEO_AUDIO_OPUS &&
                 run.ok == 0u && run.bad == 31u &&
                 run.first_bad == FM_ERR_UNSUPPORTED && run.ended)
        {
            PASS();
        }
        else
        {
            char m[120];

            av3_report(&run, m, sizeof(m));
            FAIL(m);
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Sizes from a damaged index; a read that fails for lack of memory     */
/* ------------------------------------------------------------------ */
static size_t   g_av3_alloc_max;      /* largest block asked for        */
static unsigned g_av3_alloc_calls;
static unsigned g_av3_alloc_fail;     /* refuse this call (1-based)     */

static void *av3_alloc(size_t n, void *ctx)
{
    (void)ctx;
    g_av3_alloc_calls++;
    if (n > g_av3_alloc_max)
    {
        g_av3_alloc_max = n;
    }
    if (g_av3_alloc_calls == g_av3_alloc_fail || n > ((size_t)64u << 20))
    {
        return NULL;
    }
    return malloc(n);
}

static void av3_free(void *q, void *ctx)
{
    (void)ctx;
    free(q);
}

/* Copy the MP4 `src` to `dst` with the first entry of its `nth` (0-based)
 * sample-size table set to `value`. */
static FM_Bool av3_patch_stsz(const char *src, const char *dst, unsigned nth,
                              uint32_t value)
{
    size_t len = 0;
    size_t i;
    unsigned seen = 0u;

    if (File_ReadAll(src, g_av3_file, sizeof(g_av3_file), &len) != FM_OK)
    {
        return FM_FALSE;
    }
    for (i = 4u; i + 20u <= len; i++)
    {
        if (memcmp(g_av3_file + i, "stsz", 4) == 0 && seen++ == nth)
        {
            /* type, version + flags, uniform size (0), count, entries */
            uint8_t *e = g_av3_file + i + 16u;

            if (g_av3_file[i + 8u] != 0u || g_av3_file[i + 11u] != 0u)
            {
                return FM_FALSE;         /* a uniform size: no table */
            }
            e[0] = (uint8_t)(value >> 24);
            e[1] = (uint8_t)(value >> 16);
            e[2] = (uint8_t)(value >> 8);
            e[3] = (uint8_t)value;
            return (File_WriteAll(dst, g_av3_file, len) == FM_OK)
                       ? FM_TRUE : FM_FALSE;
        }
    }
    return FM_FALSE;
}

static void test_video_damaged_sizes(void)
{
    static FM_Video v;
    FM_Allocator saved = *FM_Allocator_Get();
    FM_Allocator mine = { av3_alloc, NULL, NULL, av3_free, NULL };
    char m[160];
    FM_Result r;

#if FM_USE_H264
    TEST("A video frame size of 4 GB in the index is not allocated");
    if (!av3_patch_stsz("/test/vectors/mp4_h264_patches.mp4",
                        "/av3_bad_video.mp4", 0u, 0xF8000000u))
    {
        FAIL("cannot build the file");
    }
    else
    {
        static uint8_t rgb[64u * 48u * 3u];
        FM_Result dr = FM_OK;

        g_av3_alloc_max = 0u;
        g_av3_alloc_calls = 0u;
        g_av3_alloc_fail = 0u;
        (void)FM_Allocator_Set(&mine);
        memset(&v, 0, sizeof(v));
        r = Video_Open("/av3_bad_video.mp4", &v);
        if (r == FM_OK)
        {
            dr = Video_DecodeFrame(&v, rgb, sizeof(rgb));
            (void)Video_Close(&v);
        }
        (void)FM_Allocator_Set(&saved);
        if (r == FM_OK && dr == FM_ERR_EOF &&
            g_av3_alloc_max < ((size_t)1u << 20))
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m),
                           "open %d, decode %d, largest request %lu bytes",
                           (int)r, (int)dr, (unsigned long)g_av3_alloc_max);
            FAIL(m);
        }
        (void)File_Delete("/av3_bad_video.mp4");
    }
#endif

#if FM_USE_MP3
    TEST("An audio packet size of 4 GB in the index is not allocated");
    if (!av3_patch_stsz("/test/vectors/mp4_mp3_delay.mp4",
                        "/av3_bad_audio.mp4", 1u, 0xF8000000u))
    {
        FAIL("cannot build the file");
    }
    else
    {
        static uint8_t pcm[8192];
        FM_Result ar = FM_OK;
        size_t n = 0;

        g_av3_alloc_max = 0u;
        g_av3_alloc_calls = 0u;
        g_av3_alloc_fail = 0u;
        (void)FM_Allocator_Set(&mine);
        memset(&v, 0, sizeof(v));
        r = Video_Open("/av3_bad_audio.mp4", &v);
        if (r == FM_OK)
        {
            ar = Video_ReadAudioFrame(&v, pcm, sizeof(pcm), &n);
            (void)Video_Close(&v);
        }
        (void)FM_Allocator_Set(&saved);
        if (r == FM_OK && ar == FM_ERR_EOF &&
            g_av3_alloc_max < ((size_t)1u << 20))
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m),
                           "open %d, read %d, largest request %lu bytes",
                           (int)r, (int)ar, (unsigned long)g_av3_alloc_max);
            FAIL(m);
        }
        (void)File_Delete("/av3_bad_audio.mp4");
    }

    TEST("MP3 track: a read that fails for memory loses nothing");
    {
        Av3Run whole;
        Av3Run run;
        uint32_t crc;
        FM_Result r1 = FM_OK;
        FM_Result r2 = FM_OK;
        uint32_t at = 0u;
        size_t n = 0;

        r = av3_decode("/test/vectors/avi_mp3.avi", 4608u, &whole);
        crc = av3_crc32(g_av3_pcm, whole.bytes);
        memset(&run, 0, sizeof(run));
        memset(&v, 0, sizeof(v));
        if (r == FM_OK)
        {
            r = Video_Open("/test/vectors/avi_mp3.avi", &v);
        }
        if (r == FM_OK)
        {
            static uint8_t pcm[4608];

            /* the decoder state is the first block the track asks for,
             * the packet buffer the second */
            g_av3_alloc_max = 0u;
            g_av3_alloc_calls = 0u;
            g_av3_alloc_fail = 1u;
            (void)FM_Allocator_Set(&mine);
            r1 = Video_ReadAudioFrame(&v, pcm, sizeof(pcm), &n);
            g_av3_alloc_fail = 3u;
            r2 = Video_ReadAudioFrame(&v, pcm, sizeof(pcm), &n);
            g_av3_alloc_fail = 0u;
            at = v.audio_frame_index;
            av3_drain(&v, 4608u, 500u, &run);
            (void)Video_Close(&v);
            (void)FM_Allocator_Set(&saved);
        }
        if (r == FM_OK && r1 == FM_ERR_NO_MEMORY && r2 == FM_ERR_NO_MEMORY &&
            at == 0u && whole.bytes != 0u && run.bytes == whole.bytes &&
            run.bad == 0u && av3_crc32(g_av3_pcm, run.bytes) == crc)
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m),
                           "open %d, reads %d and %d, then at packet %lu; "
                           "%lu of %lu bytes", (int)r, (int)r1, (int)r2,
                           (unsigned long)at, (unsigned long)run.bytes,
                           (unsigned long)whole.bytes);
            FAIL(m);
        }
    }
#endif
    (void)m;
    (void)r;
    (void)v;
    (void)mine;
    (void)saved;
}

/* ------------------------------------------------------------------ */
/* FLAC tracks: 24 bits plays, 32 bits is "unsupported", not "damaged"  */
/* ------------------------------------------------------------------ */
/* The two tones as 16-bit PCM: what every lossless vector decodes to
 * (tools/gen_video_audio_vectors.py prints both numbers). */
#define AV3_PCM_BYTES   6400u
#define AV3_PCM_CRC     0xD29F0E89u

#if FM_USE_AUDIO && FM_USE_FLAC
static void test_video_flac_track(void)
{
    static FM_Video v;
    FM_AudioTrackInfo ti;
    Av3Run run;
    char m[160];
    FM_Result r;

    TEST("FLAC 24-bit in MP4 decodes to the 16-bit PCM it was made from");
    r = av3_decode("/test/vectors/mp4_flac24.mp4", 16384u, &run);
    if (r == FM_OK && run.bad == 0u && run.ended &&
        run.bytes == AV3_PCM_BYTES &&
        av3_crc32(g_av3_pcm, run.bytes) == AV3_PCM_CRC)
    {
        PASS();
    }
    else
    {
        av3_report(&run, m, sizeof(m));
        FAIL(m);
    }

    TEST("FLAC 32-bit in MP4: not decodable, every read says unsupported, the track ends");
    memset(&v, 0, sizeof(v));
    memset(&ti, 0, sizeof(ti));
    memset(&run, 0, sizeof(run));
    r = Video_Open("/test/vectors/mp4_flac32.mp4", &v);
    if (r == FM_OK)
    {
        (void)Video_GetAudioInfo(&v, &ti);
        av3_drain(&v, 16384u, 500u, &run);
        (void)Video_Close(&v);
    }
    if (r == FM_OK && ti.codec == FM_VIDEO_AUDIO_FLAC && !ti.decodable &&
        ti.bits_per_sample == 32u && run.ok == 0u && run.bad != 0u &&
        run.first_bad == FM_ERR_UNSUPPORTED && run.ended)
    {
        PASS();
    }
    else
    {
        (void)snprintf(m, sizeof(m),
                       "open %d, codec %d, decodable %d, %u bits; reads ok %u, "
                       "errors %u (first %d), end %d", (int)r, (int)ti.codec,
                       (int)ti.decodable, (unsigned)ti.bits_per_sample,
                       run.ok, run.bad, (int)run.first_bad, run.ended);
        FAIL(m);
    }

    TEST("A 32-bit .flac file is refused as unsupported");
    {
        static FM_Audio a;

        memset(&a, 0, sizeof(a));
        r = Audio_Open("/test/vectors/flac32_8k.flac", &a);
        if (r == FM_OK)
        {
            (void)Audio_Close(&a);
        }
        if (r == FM_ERR_UNSUPPORTED)
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m), "Audio_Open -> %d", (int)r);
            FAIL(m);
        }
    }
}
#endif /* FM_USE_AUDIO && FM_USE_FLAC */

/* ------------------------------------------------------------------ */
/* Uncompressed tracks: every width and byte order comes out as the     */
/* same 16-bit PCM                                                      */
/* ------------------------------------------------------------------ */
static void test_video_pcm_track(void)
{
    static const struct
    {
        const char *what;
        const char *path;
        unsigned bits;
    } c[10] = {
        { "PCM 24-bit in AVI (WAVE_FORMAT_EXTENSIBLE)", "/test/vectors/avi_pcm24.avi", 24u },
        { "PCM 32-bit float in AVI", "/test/vectors/avi_pcm_f32.avi", 32u },
        { "PCM 16-bit in MOV (sowt, one sample frame a packet)", "/test/vectors/mov_pcm_s16.mov", 16u },
        { "PCM 24-bit big-endian in MOV (in24)", "/test/vectors/mov_pcm24be.mov", 24u },
        { "PCM 24-bit little-endian in MOV (in24 + enda)", "/test/vectors/mov_pcm24le.mov", 24u },
        { "PCM 32-bit float big-endian in MOV (fl32)", "/test/vectors/mov_pcm_f32be.mov", 32u },
        { "PCM 24-bit in MP4 (ipcm + pcmC)", "/test/vectors/mp4_ipcm24.mp4", 24u },
        { "PCM 32-bit in MKV (A_PCM/INT/LIT)", "/test/vectors/mkv_pcm32.mkv", 32u },
        { "PCM 16-bit big-endian in MKV (A_PCM/INT/BIG)", "/test/vectors/mkv_pcm_s16be.mkv", 16u },
        { "PCM 32-bit float in MKV (A_PCM/FLOAT/IEEE)", "/test/vectors/mkv_pcm_f32.mkv", 32u }
    };
    static FM_Video v;
    FM_AudioTrackInfo ti;
    Av3Run run;
    char m[160];
    FM_Result r;
    unsigned i;

    for (i = 0u; i < 10u; i++)
    {
        TEST(c[i].what);
        memset(&v, 0, sizeof(v));
        memset(&ti, 0, sizeof(ti));
        memset(&run, 0, sizeof(run));
        r = Video_Open(c[i].path, &v);
        if (r == FM_OK)
        {
            (void)Video_GetAudioInfo(&v, &ti);
            av3_drain(&v, 4096u, 2000u, &run);
            (void)Video_Close(&v);
        }
        if (r == FM_OK && ti.codec == FM_VIDEO_AUDIO_PCM && ti.decodable &&
            ti.bits_per_sample == c[i].bits && ti.channels == 2u &&
            ti.sample_rate == 8000u && run.bad == 0u && run.ended &&
            run.bytes == AV3_PCM_BYTES &&
            av3_crc32(g_av3_pcm, run.bytes) == AV3_PCM_CRC)
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m),
                           "open %d, codec %d, %u bits, decodable %d; ok %u, "
                           "errors %u (first %d), %lu bytes, end %d", (int)r,
                           (int)ti.codec, (unsigned)ti.bits_per_sample,
                           (int)ti.decodable, run.ok, run.bad,
                           (int)run.first_bad, (unsigned long)run.bytes,
                           run.ended);
            FAIL(m);
        }
    }

    TEST("PCM 8-bit signed in MOV (sowt with 8 bits)");
    r = av3_decode("/test/vectors/mov_pcm_s8.mov", 4096u, &run);
    if (r == FM_OK && run.bad == 0u && run.ended && run.bytes == 3200u &&
        av3_crc32(g_av3_pcm, run.bytes) == 0x1172072Cu)
    {
        PASS();
    }
    else
    {
        av3_report(&run, m, sizeof(m));
        FAIL(m);
    }

    TEST("PCM 32-bit: a buffer of one sample frame reads the whole track");
    memset(&v, 0, sizeof(v));
    memset(&run, 0, sizeof(run));
    r = Video_Open("/test/vectors/mkv_pcm32.mkv", &v);
    if (r == FM_OK)
    {
        av3_drain(&v, 8u, 2000u, &run);
        (void)Video_Close(&v);
    }
    if (r == FM_OK && run.bad == 0u && run.ended && run.max_chunk == 4u &&
        run.bytes == AV3_PCM_BYTES &&
        av3_crc32(g_av3_pcm, run.bytes) == AV3_PCM_CRC)
    {
        PASS();
    }
    else
    {
        av3_report(&run, m, sizeof(m));
        FAIL(m);
    }

    TEST("PCM 24-bit: a buffer below one sample frame is FM_ERR_TOO_LARGE");
    memset(&v, 0, sizeof(v));
    r = Video_Open("/test/vectors/avi_pcm24.avi", &v);
    if (r == FM_OK)
    {
        uint8_t small[8];
        size_t n = 77u;
        FM_Result rr = Video_ReadAudioFrame(&v, small, 5u, &n);

        (void)Video_Close(&v);
        if (rr == FM_ERR_TOO_LARGE && n == 0u)
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m), "read -> %d, %lu bytes", (int)rr,
                           (unsigned long)n);
            FAIL(m);
        }
    }
    else
    {
        FAIL("cannot open the vector");
    }
}

/* ------------------------------------------------------------------ */
/* The picture: colours                                                */
/* ------------------------------------------------------------------ */
#if FM_USE_H264 || FM_USE_MJPEG

#define AV3_PATCH_W   64u
#define AV3_PATCH_H   48u

static uint8_t g_av3_rgb[AV3_PATCH_W * AV3_PATCH_H * 3u];

/* Decode the first frame of `path` (the four-patch picture) and compare
 * the middle of each patch with its colour.  Returns FM_TRUE when all
 * four are within `tol`; `m` gets the first one that is not. */
static FM_Bool av3_patches(const char *path, FM_VideoCodec codec, int tol,
                           char *m, size_t m_cap)
{
    static const struct
    {
        const char *name;
        uint8_t x, y;
        uint8_t rgb[3];
    } want[4] = {
        { "white", 16u, 12u, { 255u, 255u, 255u } },
        { "black", 48u, 12u, {   0u,   0u,   0u } },
        { "red",   16u, 36u, { 255u,   0u,   0u } },
        { "blue",  48u, 36u, {   0u,   0u, 255u } }
    };
    FM_Video v;
    FM_VideoInfo vi;
    FM_Result r;
    unsigned i;
    FM_Bool ok = FM_TRUE;

    memset(&v, 0, sizeof(v));
    memset(&vi, 0, sizeof(vi));
    r = Video_Open(path, &v);
    if (r != FM_OK)
    {
        (void)snprintf(m, m_cap, "open -> %d", (int)r);
        return FM_FALSE;
    }
    (void)Video_GetInfo(&v, &vi);
    if (vi.codec != codec || vi.width != AV3_PATCH_W ||
        vi.height != AV3_PATCH_H)
    {
        (void)snprintf(m, m_cap, "codec %d, %ux%u", (int)vi.codec,
                       (unsigned)vi.width, (unsigned)vi.height);
        (void)Video_Close(&v);
        return FM_FALSE;
    }
    memset(g_av3_rgb, 0x55, sizeof(g_av3_rgb));
    r = Video_DecodeFrame(&v, g_av3_rgb, sizeof(g_av3_rgb));
    (void)Video_Close(&v);
    if (r != FM_OK)
    {
        (void)snprintf(m, m_cap, "decode -> %d", (int)r);
        return FM_FALSE;
    }
    for (i = 0u; i < 4u && ok; i++)
    {
        const uint8_t *px = g_av3_rgb +
                            ((size_t)want[i].y * AV3_PATCH_W + want[i].x) * 3u;
        unsigned c;

        for (c = 0u; c < 3u; c++)
        {
            int d = (int)px[c] - (int)want[i].rgb[c];

            if (d > tol || d < -tol)
            {
                (void)snprintf(m, m_cap, "%s is %u,%u,%u", want[i].name,
                               (unsigned)px[0], (unsigned)px[1],
                               (unsigned)px[2]);
                ok = FM_FALSE;
                break;
            }
        }
    }
    return ok;
}

static void test_video_picture_colours(void)
{
    char m[120];

#if FM_USE_H264
    TEST("H.264: a video-range stream gives black 0 and white 255");
    if (av3_patches("/test/vectors/mp4_h264_patches.mp4",
                    FM_VIDEO_CODEC_H264, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }

    TEST("H.264: a full-range stream (VUI flag) is not stretched");
    if (av3_patches("/test/vectors/mp4_h264_patches_full.mp4",
                    FM_VIDEO_CODEC_H264, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }

    TEST("H.264: a BT.709 stream (VUI matrix_coefficients 1) keeps its red");
    if (av3_patches("/test/vectors/mp4_h264_patches_709.mp4",
                    FM_VIDEO_CODEC_H264, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }
#endif

#if FM_USE_MJPEG
    TEST("MJPEG in MP4 (mp4v, object type 0x6C) plays; saturated blue has no green");
    if (av3_patches("/test/vectors/mp4_mjpeg_patches.mp4",
                    FM_VIDEO_CODEC_MJPEG, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }

    /* ffmpeg writes 4:2:2 as luma 2x2 / chroma 1x2 and 4:4:4 as 1x2 for
     * every component; picojpeg knew chroma 1x1 only and refused both. */
    TEST("MJPEG 4:2:2 as ffmpeg writes it (chroma sampling 1x2) plays");
    if (av3_patches("/test/vectors/avi_mjpeg422_patches.avi",
                    FM_VIDEO_CODEC_MJPEG, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }

    TEST("MJPEG 4:4:4 as ffmpeg writes it (every component 1x2) plays");
    if (av3_patches("/test/vectors/avi_mjpeg444_patches.avi",
                    FM_VIDEO_CODEC_MJPEG, 3, m, sizeof(m)))
    {
        PASS();
    }
    else
    {
        FAIL(m);
    }
#endif
}

#endif /* FM_USE_H264 || FM_USE_MJPEG */

#endif /* FM_USE_VIDEO */

/* ------------------------------------------------------------------ */
/* Still JPEG: sampling layouts                                        */
/* ------------------------------------------------------------------ */
#if FM_USE_IMAGE && FM_USE_JPEG

/* Load `jpg`, compare with the reference file (`ref_ch` bytes a pixel;
 * the library always delivers 3).  Returns the largest difference, or
 * -1 with `m` filled. */
static int av3_jpeg_diff(const char *jpg, const char *ref, unsigned w,
                         unsigned h, unsigned ref_ch, char *m, size_t m_cap)
{
    static uint8_t px[40u * 24u * 3u];
    static uint8_t want[40u * 24u * 3u];
    FM_Image img;
    size_t got = 0;
    size_t i;
    int worst = 0;
    FM_Result r;

    memset(&img, 0, sizeof(img));
    r = File_ReadAll(ref, want, sizeof(want), &got);
    if (r != FM_OK || got != (size_t)w * h * ref_ch)
    {
        (void)snprintf(m, m_cap, "reference: %d, %lu bytes", (int)r,
                       (unsigned long)got);
        return -1;
    }
    r = Image_Load(jpg, &img, px, sizeof(px));
    if (r != FM_OK)
    {
        (void)snprintf(m, m_cap, "Image_Load -> %d", (int)r);
        return -1;
    }
    if (img.width != w || img.height != h || img.channels != 3u)
    {
        (void)snprintf(m, m_cap, "%ux%u, %u channels", (unsigned)img.width,
                       (unsigned)img.height, (unsigned)img.channels);
        return -1;
    }
    for (i = 0; i < (size_t)w * h * 3u; i++)
    {
        int d = (int)px[i] - (int)want[(ref_ch == 3u) ? i : i / 3u];

        if (d < 0)
        {
            d = -d;
        }
        if (d > worst)
        {
            worst = d;
        }
    }
    return worst;
}

static void test_jpeg_layouts(void)
{
    static const struct
    {
        const char *what;
        const char *jpg;
        const char *ref;
        unsigned w, h, ref_ch;
        int limit;
    } c[3] = {
        { "JPEG: one component with sampling factors 2x2 and restart markers",
          "/test/vectors/jpeg_gray_h2v2.jpg",
          "/test/vectors/jpeg_gray_h2v2.gray", 37u, 21u, 1u, 3 },
        { "JPEG 4:2:2 with chroma 1x2, 33x17 (odd both ways)",
          "/test/vectors/jpeg_422_33x17.jpg",
          "/test/vectors/jpeg_422_33x17.rgb", 33u, 17u, 3u, 5 },
        { "JPEG 4:4:4 with every component 1x2, 9x17",
          "/test/vectors/jpeg_444_9x17.jpg",
          "/test/vectors/jpeg_444_9x17.rgb", 9u, 17u, 3u, 5 }
    };
    char m[120];
    unsigned i;

    for (i = 0u; i < 3u; i++)
    {
        int d;

        TEST(c[i].what);
        d = av3_jpeg_diff(c[i].jpg, c[i].ref, c[i].w, c[i].h, c[i].ref_ch,
                          m, sizeof(m));
        if (d < 0)
        {
            FAIL(m);
        }
        else if (d > c[i].limit)
        {
            (void)snprintf(m, sizeof(m), "largest difference %d (limit %d)",
                           d, c[i].limit);
            FAIL(m);
        }
        else
        {
            PASS();
        }
    }
}

#endif /* FM_USE_IMAGE && FM_USE_JPEG */

/* ------------------------------------------------------------------ */
/* WAV with Microsoft ADPCM                                            */
/* ------------------------------------------------------------------ */
#if FM_USE_AUDIO && FM_USE_VIDEO

/* Read the whole file `chunk` samples at a time into g_av3_pcm. */
static FM_Result av3_wav_read(const char *path, size_t chunk, size_t *bytes,
                              uint64_t *declared)
{
    static FM_Audio a;
    static int16_t buf[1024];
    FM_AudioInfo info;
    FM_Result r;

    *bytes = 0u;
    *declared = 0u;
    memset(&a, 0, sizeof(a));
    memset(&info, 0, sizeof(info));
    r = Audio_Open(path, &a);
    if (r != FM_OK)
    {
        return r;
    }
    (void)Audio_GetInfo(&a, &info);
    *declared = info.total_frames * info.channels * 2u;
    for (;;)
    {
        size_t got = 0;
        size_t i;

        r = Audio_Read(&a, buf, chunk, &got);
        if (r != FM_OK || got == 0u)
        {
            break;
        }
        for (i = 0; i < got && *bytes + 2u <= sizeof(g_av3_pcm); i++)
        {
            g_av3_pcm[*bytes] = (uint8_t)((uint16_t)buf[i] & 0xFFu);
            g_av3_pcm[*bytes + 1u] = (uint8_t)((uint16_t)buf[i] >> 8);
            *bytes += 2u;
        }
    }
    (void)Audio_Close(&a);
    return (r == FM_ERR_EOF) ? FM_OK : r;
}

static void test_wav_adpcm_ms(void)
{
    /* length and CRC-32 of ffmpeg's decode, printed by
     * tools/gen_video_audio_vectors.py */
    static const struct
    {
        const char *what;
        const char *path;
        size_t chunk;
        size_t bytes;
        uint32_t crc;
    } c[3] = {
        { "WAV Microsoft ADPCM, stereo: the samples ffmpeg decodes",
          "/test/vectors/wav_adpcm_ms_stereo_8k.wav", 1024u, 16192u, 0xA04716C3u },
        { "WAV Microsoft ADPCM, stereo, read 333 samples at a time",
          "/test/vectors/wav_adpcm_ms_stereo_8k.wav", 333u, 16192u, 0xA04716C3u },
        { "WAV Microsoft ADPCM, mono: the samples ffmpeg decodes",
          "/test/vectors/wav_adpcm_ms_mono_11k.wav", 1000u, 12216u, 0xD8BB37BB }
    };
    char m[160];
    unsigned i;

    for (i = 0u; i < 3u; i++)
    {
        size_t bytes = 0;
        uint64_t declared = 0;
        FM_Result r;

        TEST(c[i].what);
        r = av3_wav_read(c[i].path, c[i].chunk, &bytes, &declared);
        if (r == FM_OK && bytes == c[i].bytes && declared == c[i].bytes &&
            av3_crc32(g_av3_pcm, bytes) == c[i].crc)
        {
            PASS();
        }
        else
        {
            (void)snprintf(m, sizeof(m),
                           "result %d, %lu bytes (declared %lu), CRC 0x%08lX",
                           (int)r, (unsigned long)bytes,
                           (unsigned long)declared,
                           (unsigned long)av3_crc32(g_av3_pcm, bytes));
            FAIL(m);
        }
    }

    TEST("WAV Microsoft ADPCM: a seek lands on a block and plays the same samples");
    {
        static FM_Audio a;
        static int16_t part[512];
        size_t bytes = 0;
        uint64_t declared = 0;
        size_t got = 0;
        uint64_t at = 0;
        FM_Result r;

        r = av3_wav_read(c[0].path, 1024u, &bytes, &declared);
        memset(&a, 0, sizeof(a));
        if (r == FM_OK)
        {
            r = Audio_Open(c[0].path, &a);
        }
        if (r == FM_OK)
        {
            r = Audio_Seek(&a, 0.25);
            at = a.frame_index;
            if (r == FM_OK)
            {
                r = Audio_Read(&a, part, 512u, &got);
            }
            (void)Audio_Close(&a);
        }
        /* 0.25 s is frame 2000.  The block size is the encoder's choice,
         * so what is checked is: at a block start not past the target,
         * and the samples from there are those of the plain read. */
        if (r == FM_OK && got == 512u && at <= 2000u && at > 1000u &&
            (size_t)at * 4u + 1024u <= bytes)
        {
            size_t k;
            int same = 1;

            for (k = 0; k < 512u; k++)
            {
                const uint8_t *q = g_av3_pcm + (size_t)at * 4u + k * 2u;

                if (part[k] != (int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8)))
                {
                    same = 0;
                }
            }
            if (same)
            {
                PASS();
            }
            else
            {
                (void)snprintf(m, sizeof(m), "samples differ at frame %lu",
                               (unsigned long)at);
                FAIL(m);
            }
        }
        else
        {
            (void)snprintf(m, sizeof(m), "result %d, at frame %lu, %lu samples",
                           (int)r, (unsigned long)at, (unsigned long)got);
            FAIL(m);
        }
    }
}

#endif /* FM_USE_AUDIO && FM_USE_VIDEO */

#endif /* AUDIT_V3_TESTS_H */
