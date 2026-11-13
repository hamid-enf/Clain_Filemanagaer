/*
 * FileManager - WAV/PCM audio support
 *
 * The implementation intentionally keeps the public API simple: callers
 * deal in signed 16-bit samples while the codec handles little-endian WAV
 * headers and 8/16-bit PCM conversion internally.
 */

#define FM_DEBUG_TAG "audio"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_internal.h"
#include <string.h>
#include "fm_alloc.h"
#if FM_USE_FLAC
#include "fm_flac.h"
#endif
#if FM_USE_AUDIO
#include "fm_adpcm.h"       /* Microsoft ADPCM blocks (WAV format 0x02) */
#endif
#if FM_USE_VORBIS
struct stb_vorbis;
typedef struct stb_vorbis stb_vorbis;
extern stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, void *alloc);
extern void stb_vorbis_close(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
extern int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
typedef struct { unsigned int sample_rate; int channels; unsigned int setup_memory_required; unsigned int setup_temp_memory_required; unsigned int temp_memory_required; int max_frame_size; } stb_vorbis_info;
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
#endif
#if FM_USE_OPUS
#include "fm_opus.h"
#endif

#if FM_USE_AUDIO

#if FM_AUDIO_TRACE
#include "fm_log.h"
/* Per-read tracing, its own switch because it prints on every refill.
 * One line per message, long values printed as unsigned long: the small
 * printf of newlib-nano has no "%llu". */
#define FM_AUDIO_LOG(...) ((void)FM_Log_Tagged('T', "audio", __VA_ARGS__))
#else
#define FM_AUDIO_LOG(...) ((void)0)
#endif

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static FM_Result file_read_exact(FM_File *f, void *data, uint32_t size,
                                 uint32_t *read_out)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t done = 0;

    while (done < size)
    {
        uint32_t rd = 0;
        FM_Result r = File_Read(f, p + done, size - done, &rd);

        if (r != FM_OK && r != FM_ERR_EOF)
        {
            return r;
        }
        if (rd == 0)
        {
            break;                          /* EOF: keep whatever we got */
        }
        done += rd;
    }
    if (read_out != NULL) *read_out = done;
    return (done == size) ? FM_OK : FM_ERR_EOF;
}

static FM_Result file_write_exact(FM_File *f, const void *data, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t done = 0;

    while (done < size)
    {
        uint32_t wr = 0;
        FM_Result r = File_Write(f, p + done, size - done, &wr);

        if (r != FM_OK)
        {
            return r;
        }
        if (wr == 0)
        {
            return FM_ERR_IO;
        }
        done += wr;
    }
    return FM_OK;
}

/* ALAW/ULAW decode (G.711) — for WAV format 6/7 */
static int16_t wav_alaw_decode(uint8_t a)
{
    int sign, exp, mant, sample;

    a ^= 0x55u;
    /* G.711: in A-law the sign bit is SET for a positive sample (after
     * the 0x55 toggle), the opposite of the natural reading -- and of
     * what this line had.  Every sample came out negated: inaudible on
     * its own, a cancellation when mixed with a correct source. */
    sign = (a & 0x80u) ? 1 : -1;
    exp = (a >> 4) & 0x07;
    mant = a & 0x0F;
    sample = (mant << 4) + 8;
    if (exp != 0) sample += 0x100;
    if (exp > 1) sample <<= (exp - 1);
    return (int16_t)(sign * sample);          /* |sample| <= 32256 */
}
static int16_t wav_ulaw_decode(uint8_t u)
{
    int sign, exp, mant, sample;

    u = (uint8_t)~u;
    sign = (u & 0x80u) ? -1 : 1;
    exp = (u >> 4) & 0x07;
    mant = u & 0x0F;
    sample = ((mant << 3) + 0x84) << exp;
    sample -= 0x84;
    return (int16_t)(sign * sample);          /* |sample| <= 32124 */
}
static const int16_t wav_adpcm_step[89] = {
 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t wav_adpcm_index[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
/* One IMA nibble: next sample of one channel */
static int16_t wav_adpcm_nibble(int32_t *pred, int *idx, int nib)
{
    int step = wav_adpcm_step[*idx];
    int diff = step >> 3;

    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) *pred -= diff; else *pred += diff;
    if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
    *idx += wav_adpcm_index[nib & 0x0F];
    if (*idx > 88) *idx = 88; else if (*idx < 0) *idx = 0;
    return (int16_t)*pred;
}

/* Stereo IMA block: a 4-byte header per channel, then groups of 8 bytes
 * -- 4 bytes (8 samples) of the left channel, 4 bytes of the right.
 * Output is interleaved L/R.  (The mono decoder below, fed such a block,
 * read the right channel's header and data as left-channel samples.) */
static size_t wav_adpcm_decode_block_stereo(const uint8_t *src, size_t src_bytes,
                                            int16_t *dst, size_t dst_cap)
{
    int32_t pred[2];
    int idx[2];
    size_t out = 0;
    size_t pos = 8;
    int c;

    if (src_bytes < 8u || dst_cap < 2u) return 0;
    for (c = 0; c < 2; c++) {
        pred[c] = (int32_t)(int16_t)(src[c * 4] | (src[c * 4 + 1] << 8));
        idx[c] = src[c * 4 + 2];
        if (idx[c] > 88) idx[c] = 88;
        dst[out++] = (int16_t)pred[c];
    }
    while (pos + 8u <= src_bytes && out + 16u <= dst_cap) {
        for (c = 0; c < 2; c++) {
            int i;

            for (i = 0; i < 4; i++) {
                uint8_t b = src[pos + (size_t)c * 4u + (size_t)i];

                dst[out + (size_t)(i * 2) * 2u + (size_t)c] =
                    wav_adpcm_nibble(&pred[c], &idx[c], b & 0x0F);
                dst[out + (size_t)(i * 2 + 1) * 2u + (size_t)c] =
                    wav_adpcm_nibble(&pred[c], &idx[c], b >> 4);
            }
        }
        out += 16u;
        pos += 8u;
    }
    return out;
}

static size_t wav_adpcm_decode_block(const uint8_t *src, size_t src_bytes, int16_t *dst, size_t dst_cap)
{
    if (src_bytes < 4) return 0;
    int32_t pred = (int32_t)(int16_t)(src[0] | (src[1] << 8));
    /* idx is signed: IMA index can go negative on step-down nibbles.
     * unsigned would wrap to 255 and get clamped to 88 instead of 0. */
    int idx = src[2];
    if (idx > 88) idx = 88;
    if (idx < 0) idx = 0;
    size_t out = 0;
    if (out < dst_cap) dst[out++] = (int16_t)pred;
    int step = wav_adpcm_step[idx];
    for (size_t i = 4; i < src_bytes && out < dst_cap; ++i) {
        uint8_t b = src[i];
        for (int nibble = 0; nibble < 2 && out < dst_cap; ++nibble) {
            int nib = (nibble == 0) ? (b & 0x0F) : (b >> 4);
            int diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            /* Accumulate in int32_t so pred cannot overflow int16_t
             * before the saturation clamp is applied. */
            if (nib & 8) pred -= diff; else pred += diff;
            if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
            idx += wav_adpcm_index[nib & 0x0F];
            if (idx > 88) idx = 88; else if (idx < 0) idx = 0;
            step = wav_adpcm_step[idx];
            dst[out++] = (int16_t)pred;
        }
    }
    return out;
}
/* ------------------------------------------------------------------ */
/* ADPCM WAV, mono or stereo: block-framed decoding                     */
/* IMA (format 0x11) and Microsoft (format 0x02) share everything here  */
/* but the block decoder and the number of frames a block holds.        */
/*                                                                     */
/* IMA-ADPCM داده را در بلاک‌های مستقل فریم‌بندی می‌کند (هر بلاک       */
/* سرصفحهٔ ۴ بایتی predictor/index خودش را دارد). دیکدر بی‌حالت است،   */
/* پس فقط از مرز بلاک می‌تواند شروع کند. هر فراخوانی Audio_Read چند   */
/* بلاک کامل را دیکد می‌کند و نمونه‌های اضافه را در carry نگه می‌دارد */
/* تا فراخوانی بعدی — به این ترتیب هر count ای (کوچک یا بزرگ) کل      */
/* استریم را بدون گم‌شدن نمونه تولید می‌کند.                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t block_align;   /* bytes per block (sanitised >= 4)      */
    size_t   carry_len;     /* decoded samples sitting in carry      */
    size_t   carry_pos;     /* first unserved sample in carry        */
    /* flexible tail: uint8_t raw[block_align]; int16_t carry[];     */
} WavAdpcmState;

#define WAV_ADPCM_RAW(st)   ((uint8_t *)((st) + 1))
/* Carry is int16_t[], so round block_align up to even to keep it aligned
 * even when the WAV declares an odd block_align (bug #3). */
#define WAV_ADPCM_CARRY_OFF(st) (((size_t)(st)->block_align + 1u) & ~(size_t)1u)
#define WAV_ADPCM_CARRY(st)     ((int16_t *)(WAV_ADPCM_RAW(st) + WAV_ADPCM_CARRY_OFF(st)))

/* Most samples one block decodes to (the mono count; a stereo block
 * gives fewer: 2 + 2 * (ba - 8) rounded down to whole groups). */
static size_t wav_adpcm_carry_cap(uint16_t ba)
{
    return (size_t)1u + ((size_t)ba - 4u) * 2u;
}

/* Sample frames in one full block */
static size_t wav_adpcm_block_frames(uint16_t ba, uint16_t channels)
{
    if (channels == 2u)
    {
        return (ba < 8u) ? 0u : (size_t)1u + (((size_t)ba - 8u) / 8u) * 8u;
    }
    return wav_adpcm_carry_cap(ba);
}

/* Sample frames in `bytes` of block data (the last block may be short) */
static uint64_t wav_adpcm_total_frames(uint64_t bytes, uint16_t ba,
                                       uint16_t channels)
{
    uint64_t full = bytes / ba;
    uint64_t tail = bytes % ba;
    uint64_t frames = full * wav_adpcm_block_frames(ba, channels);

    if (channels == 2u)
    {
        if (tail >= 8u) frames += 1u + ((tail - 8u) / 8u) * 8u;
    }
    else if (tail >= 4u)
    {
        frames += 1u + (tail - 4u) * 2u;
    }
    return frames;
}

static uint16_t wav_adpcm_block_align_of(const FM_Audio *a)
{
    uint16_t ba = a->info.block_align;
    if (ba < 4u || ba > 32768u) ba = 1024u;
    return ba;
}

/* The same three questions for either coder (format 0x11 / 0x02) */
static size_t wav_adpcm_frames_of(const FM_Audio *a, uint16_t ba)
{
    return (a->info.format == 0x02u)
               ? adpcm_block_frames(0x0002u, a->info.channels, ba)
               : wav_adpcm_block_frames(ba, a->info.channels);
}

static uint64_t wav_adpcm_length_of(const FM_Audio *a)
{
    uint16_t ba = wav_adpcm_block_align_of(a);

    if (a->info.format == 0x02u)
    {
        return (a->info.data_size / ba) *
                   adpcm_block_frames(0x0002u, a->info.channels, ba) +
               adpcm_block_frames(0x0002u, a->info.channels,
                                  (size_t)(a->info.data_size % ba));
    }
    return wav_adpcm_total_frames(a->info.data_size, ba, a->info.channels);
}

/* Samples (not frames) the carry has to hold for one block */
static size_t wav_adpcm_carry_of(const FM_Audio *a, uint16_t ba)
{
    return (a->info.format == 0x02u) ? (size_t)ba * 2u
                                     : wav_adpcm_carry_cap(ba);
}

/* One Microsoft ADPCM block into the carry.  The block decoder writes
 * little-endian bytes; the carry holds samples in host order. */
static size_t wav_adpcm_ms_decode(const uint8_t *src, size_t n, uint32_t ch,
                                  int16_t *dst)
{
    size_t samples = adpcm_block_frames(0x0002u, ch, n) * ch;
    uint8_t *b = (uint8_t *)dst;
    size_t i;

    if (samples == 0u)
    {
        return 0u;
    }
    (void)adpcm_ms_block(src, n, ch, b);
    for (i = 0u; i < samples; i++)
    {
        dst[i] = (int16_t)adpcm_rd16(b + 2u * i);
    }
    return samples;
}

static void wav_adpcm_reset(FM_Audio *a)
{
    WavAdpcmState *st = (WavAdpcmState *)a->codec_ctx;
    if (st != NULL) { st->carry_len = 0u; st->carry_pos = 0u; }
}

/* Serve one Audio_Read() call: whole-block decode with a sample carry,
 * so any caller chunk size yields the complete stream losslessly. */
static FM_Result wav_adpcm_serve(FM_Audio *a, int16_t *samples,
                                 size_t count, size_t *read)
{
    WavAdpcmState *st = (WavAdpcmState *)a->codec_ctx;
    uint64_t pos, byte_pos, available;
    size_t out = 0;
    FM_Result r2;

    /* Lazy per-stream state: block staging + decoded-sample carry. */
    if (st == NULL) {
        uint16_t ba = wav_adpcm_block_align_of(a);
        size_t carry_off = ((size_t)ba + 1u) & ~(size_t)1u; /* align carry */
        size_t cap = wav_adpcm_carry_of(a, ba);
        st = (WavAdpcmState *)FM_Alloc(sizeof(*st) + carry_off +
                                       cap * sizeof(int16_t));
        if (st == NULL) return FM_ERR_NO_MEMORY;
        memset(st, 0, sizeof(*st));
        st->block_align = ba;
        a->codec_ctx = st;
    }

    /* 1) Leftover samples from a previously decoded block. */
    if (st->carry_pos < st->carry_len) {
        size_t n = st->carry_len - st->carry_pos;
        if (n > count) n = count;
        memcpy(samples, WAV_ADPCM_CARRY(st) + st->carry_pos,
               n * sizeof(int16_t));
        st->carry_pos += n;
        out = n;
    }

    /* 2) Decode further whole blocks while the caller has room. */
    while (out < count) {
        size_t take, got, n;

        if (File_Tell(&a->file, &pos) != FM_OK ||
            pos < a->info.data_offset)
            return (out != 0u) ? FM_OK : FM_ERR_IO;
        byte_pos = pos - a->info.data_offset;
        available = (byte_pos < a->info.data_size)
                        ? a->info.data_size - byte_pos : 0;
        if (available == 0)
            break;                          /* end of data chunk */

        take = (size_t)st->block_align;
        if ((uint64_t)take > available)
            take = (size_t)available;       /* final partial block */
        {
            uint32_t got_bytes = 0;
            r2 = file_read_exact(&a->file, WAV_ADPCM_RAW(st), (uint32_t)take,
                                 &got_bytes);
            if (r2 != FM_OK && r2 != FM_ERR_EOF) {
                if (out != 0u) break;
                return r2;
            }
            /* Decode only the bytes that were actually read — a short
             * final read at EOF must not decode stale/uninitialised
             * tail bytes (bug #3). */
            take = got_bytes;
        }
        got = (a->info.format == 0x02u)
                  ? wav_adpcm_ms_decode(WAV_ADPCM_RAW(st), take,
                                        a->info.channels, WAV_ADPCM_CARRY(st))
              : (a->info.channels == 2u)
                  ? wav_adpcm_decode_block_stereo(
                        WAV_ADPCM_RAW(st), take, WAV_ADPCM_CARRY(st),
                        wav_adpcm_carry_cap(st->block_align))
                  : wav_adpcm_decode_block(
                        WAV_ADPCM_RAW(st), take, WAV_ADPCM_CARRY(st),
                        wav_adpcm_carry_cap(st->block_align));
        st->carry_len = got;
        st->carry_pos = 0;
        if (got == 0u)                      /* < 4 bytes: tail junk */
            break;
        n = (got > count - out) ? count - out : got;
        memcpy(samples + out, WAV_ADPCM_CARRY(st),
               n * sizeof(int16_t));
        st->carry_pos = n;
        out += n;
    }

    if (out == 0u)
        return FM_ERR_EOF;
    if (read != NULL) *read = out;
    a->frame_index += (uint64_t)(out / a->info.channels);
    if (a->callback) a->callback(samples, out, a->callback_data);
    return FM_OK;
}

/* Seconds to a sample-frame number, for every seek.  The caller has
 * refused negative times and NaN; what is left to guard is the cast: a
 * double too large for uint64_t is undefined behaviour (on x86 it gave
 * 0x8000000000000000, or 0 -- "seek to infinity" played from the start).
 * The result is capped at 2^40 frames, half a year at 96 kHz, so the
 * multiplications that follow cannot overflow either; each format then
 * clamps to its own length. */
static uint64_t audio_time_to_frame(double seconds, uint32_t rate)
{
    const double cap = 1099511627776.0;             /* 2^40 */
    double f = seconds * (double)rate;

    return (f >= cap) ? (uint64_t)1 << 40 : (uint64_t)f;
}

/* Buffered PCM for FLAC/Vorbis/Opus — whole-file decode at open */
typedef struct {
    int16_t *pcm;
    size_t   total;
    size_t   pos;
} PcmBuffer;
static void pcmbuf_free(PcmBuffer *b) {
    if (b) { if (b->pcm) FM_Free(b->pcm); FM_Free(b); }
}
static FM_Result pcmbuf_read(FM_Audio *a, int16_t *out, size_t count, size_t *read) {
    PcmBuffer *b = (PcmBuffer*)a->codec_ctx;
    if (!b || !out) return FM_ERR_INVALID_ARGUMENT;
    if (read) *read = 0;
    if (b->pos >= b->total) return FM_ERR_EOF;
    size_t avail = b->total - b->pos;
    size_t n = (count < avail) ? count : avail;
    n -= n % a->info.channels;              /* whole frames, any layout */
    if (n == 0) return FM_ERR_EOF;
    memcpy(out, b->pcm + b->pos, n * sizeof(int16_t));
    b->pos += n;
    a->frame_index += (uint64_t)(n / a->info.channels);
    if (read) *read = n;
    if (a->callback) a->callback(out, n, a->callback_data);
    return FM_OK;
}
static FM_Result pcmbuf_seek(FM_Audio *a, double seconds) {
    PcmBuffer *b = (PcmBuffer*)a->codec_ctx;
    if (!b || a->info.sample_rate == 0) return FM_ERR_INVALID_ARGUMENT;
    uint64_t frame = audio_time_to_frame(seconds, a->info.sample_rate);
    uint64_t frames = (uint64_t)(b->total / a->info.channels);
    /* Clamp before narrowing: frame * channels in size_t wrapped on a
     * 32-bit target, and a seek far past the end played from the start. */
    if (frame > frames) frame = frames;
    b->pos = (size_t)frame * a->info.channels;
    a->frame_index = frame;
    return FM_OK;
}
#if FM_USE_FLAC || FM_USE_VORBIS
/* FLAC / Vorbis are decoded whole at open: compressed-file size cap. */
#define AUDIO_SLURP_MAX_BYTES (10u * 1024u * 1024u)
#endif
#if FM_USE_FLAC
static FM_Result flac_file_open(FM_Audio *a, uint64_t base) {
    uint64_t sz = 0;
    uint8_t *data = NULL;
    uint32_t got = 0;
    void *p = NULL;
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint64_t total_frames = 0;
    PcmBuffer *b = NULL;
    FM_Result r = File_Size(&a->file, &sz);
    if (r != FM_OK) return r;
    if (sz < 42u) return FM_ERR_CORRUPTED;
    if (sz - base > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE; /* valid, just too big */
    sz -= base;                                /* an ID3v2 tag in front */
    data = (uint8_t*)FM_Alloc((size_t)sz);
    if (!data) return FM_ERR_NO_MEMORY;
    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
    r = File_Read(&a->file, data, (uint32_t)sz, &got);
    if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
    if (FM_Flac_Bits(data, (size_t)sz) > FM_FLAC_MAX_BITS) {
        /* wider than the decoder does (FLAC allows 32 bits): a valid
         * file this build cannot play, not a damaged one.  (dr_flac
         * opens it and then decodes nothing.) */
        FM_ERR_LOG("FLAC with %u bits per sample: at most %u",
                   (unsigned)FM_Flac_Bits(data, (size_t)sz),
                   (unsigned)FM_FLAC_MAX_BITS);
        FM_Free(data);
        return FM_ERR_UNSUPPORTED;
    }
    p = FM_Flac_OpenMemory(data, (size_t)sz, &rate, &channels, &total_frames);
    if (!p) { FM_Free(data); return FM_ERR_CORRUPTED; }
    a->info.sample_rate = rate;
    a->info.channels = (uint16_t)channels;
    a->info.bits_per_sample = 16;
    a->info.format = 0xF1AC;
    (void)strcpy(a->info.codec, "FLAC");
    a->info.data_offset = 0;
    a->info.data_size = sz;
    if (channels == 0u || channels > 8u) {
        FM_Flac_Close(p);
        FM_Free(data);
        return FM_ERR_UNSUPPORTED;
    }
    /* The decoded track is held whole, at most FM_FLAC_MAX_PCM_BYTES of
     * it.  How large the buffer has to be is a field of the file:
     *   - a length that does not fit is refused before anything is
     *     allocated (never a track with its end cut off);
     *   - no length at all (a stream written without seeking back) used
     *     to cost a fixed million frames, 2 MiB a channel, for any file:
     *     such files did not open on a small heap.  The buffer now starts
     *     small and grows with what is decoded;
     *   - a length larger than the audio that is really there leaves the
     *     unused part allocated only until the decode is over. */
    const uint64_t frame_bytes = (uint64_t)channels * sizeof(int16_t);
    const uint64_t max_frames = (uint64_t)FM_FLAC_MAX_PCM_BYTES / frame_bytes;
    uint64_t frames_read = 0;
    uint64_t cap_frames;
    int16_t *pcm;

    if (total_frames > max_frames) {
        FM_Flac_Close(p);
        FM_Free(data);
        return FM_ERR_TOO_LARGE;
    }
    cap_frames = (total_frames != 0u) ? total_frames : 16384u;
    if (cap_frames > max_frames) cap_frames = max_frames;
    pcm = (cap_frames != 0u)
              ? (int16_t*)FM_Alloc((size_t)(cap_frames * frame_bytes)) : NULL;
    if (!pcm) {
        FM_Flac_Close(p);
        FM_Free(data);
        return (cap_frames != 0u) ? FM_ERR_NO_MEMORY : FM_ERR_TOO_LARGE;
    }
    for (;;) {
        uint64_t want = cap_frames - frames_read;
        uint64_t got_frames = FM_Flac_ReadS16(
            p, want, pcm + (size_t)(frames_read * channels));

        frames_read += got_frames;
        if (got_frames < want) break;               /* end of the audio */
        if (total_frames != 0u) break;              /* declared length  */
        /* Length not declared and the buffer is full: is there more? */
        {
            int16_t probe[8];
            uint64_t grown = cap_frames * 2u;
            int16_t *np;

            if (FM_Flac_ReadS16(p, 1, probe) == 0) break;
            if (cap_frames >= max_frames) {
                FM_Flac_Close(p);
                FM_Free(data);
                FM_Free(pcm);
                return FM_ERR_TOO_LARGE;            /* never cut the end */
            }
            if (grown > max_frames) grown = max_frames;
            np = (int16_t*)FM_ReallocSized(pcm,
                                           (size_t)(cap_frames * frame_bytes),
                                           (size_t)(grown * frame_bytes));
            if (np == NULL) {
                FM_Flac_Close(p);
                FM_Free(data);
                FM_Free(pcm);
                return FM_ERR_NO_MEMORY;
            }
            pcm = np;
            memcpy(pcm + (size_t)(frames_read * channels), probe,
                   (size_t)frame_bytes);
            frames_read++;
            cap_frames = grown;
        }
    }
    FM_Flac_Close(p);
    FM_Free(data);
    if (frames_read == 0) { FM_Free(pcm); return FM_ERR_CORRUPTED; }
    if (frames_read < cap_frames) {
        /* give back what the header promised and the file did not have */
        int16_t *np = (int16_t*)FM_ReallocSized(
            pcm, (size_t)(cap_frames * frame_bytes),
            (size_t)(frames_read * frame_bytes));

        if (np != NULL) pcm = np;
    }
    size_t total_samples = (size_t)(frames_read * a->info.channels);
    b = (PcmBuffer*)FM_Alloc(sizeof(PcmBuffer));
    if (!b) { FM_Free(pcm); return FM_ERR_NO_MEMORY; }
    a->info.total_frames = frames_read;
    b->pcm = pcm;
    b->total = total_samples;
    b->pos = 0;
    a->codec_ctx = b;
    return FM_OK;
}
#endif
#if FM_USE_VORBIS
static FM_Result vorbis_file_open(FM_Audio *a, uint64_t base) {
    uint64_t sz = 0;
    uint8_t *data = NULL;
    uint32_t got = 0;
    int error = 0;
    stb_vorbis *v = NULL;
    PcmBuffer *b = NULL;
    FM_Result r = File_Size(&a->file, &sz);
    if (r != FM_OK) return r;
    if (sz < 32u) return FM_ERR_CORRUPTED;
    if (sz - base > AUDIO_SLURP_MAX_BYTES) return FM_ERR_TOO_LARGE; /* valid, just too big */
    sz -= base;                                /* an ID3v2 tag in front */
    data = (uint8_t*)FM_Alloc((size_t)sz);
    if (!data) return FM_ERR_NO_MEMORY;
    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
    r = File_Read(&a->file, data, (uint32_t)sz, &got);
    if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
    v = stb_vorbis_open_memory(data, (int)sz, &error, NULL);
    if (!v) { FM_Free(data); return FM_ERR_CORRUPTED; }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    a->info.sample_rate = (uint32_t)info.sample_rate;
    a->info.channels = (uint16_t)info.channels;
    a->info.bits_per_sample = 16;
    a->info.format = 0x566F;
    (void)strcpy(a->info.codec, "VORBIS");
    a->info.data_offset = 0;
    a->info.data_size = sz;
    if (info.channels <=0 || info.channels>2 || info.sample_rate==0) { stb_vorbis_close(v); FM_Free(data); return FM_ERR_UNSUPPORTED; }
    int total_frames = stb_vorbis_stream_length_in_samples(v);
    if (total_frames > 10*1024*1024) {
        stb_vorbis_close(v);
        FM_Free(data);
        return FM_ERR_TOO_LARGE;   /* refuse silent truncation (bug #4) */
    }
    FM_Bool unknown_len = (total_frames <= 0) ? FM_TRUE : FM_FALSE;
    if (unknown_len)
        total_frames = 1024*256;   /* unknown length */
    size_t total_samples = (size_t)total_frames * (size_t)info.channels;
    int16_t *pcm = (int16_t*)FM_Alloc(total_samples * sizeof(int16_t));
    if (!pcm) { stb_vorbis_close(v); FM_Free(data); return FM_ERR_NO_MEMORY; }
    size_t pos = 0;
    while (pos < total_samples) {
        int ch = info.channels;
        int max_frames = (int)((total_samples - pos) / (size_t)ch);
        if (max_frames <=0) break;
        int n = stb_vorbis_get_samples_short_interleaved(v, ch, pcm + pos, max_frames * ch);
        if (n == 0) break;
        pos += (size_t)n * (size_t)ch;
    }
    if (unknown_len && pos == total_samples) {
        /* Estimate exhausted: more audio left means silent truncation. */
        int16_t probe[2];
        if (stb_vorbis_get_samples_short_interleaved(v, info.channels, probe,
                                                     info.channels) > 0) {
            stb_vorbis_close(v);
            FM_Free(data);
            FM_Free(pcm);
            return FM_ERR_TOO_LARGE;
        }
    }
    stb_vorbis_close(v);
    FM_Free(data);
    if (pos == 0) { FM_Free(pcm); return FM_ERR_CORRUPTED; }
    b = (PcmBuffer*)FM_Alloc(sizeof(PcmBuffer));
    if (!b) { FM_Free(pcm); return FM_ERR_NO_MEMORY; }
    a->info.total_frames = pos / a->info.channels;
    b->pcm = pcm;
    b->total = pos;
    b->pos = 0;
    a->codec_ctx = b;
    return FM_OK;
}
#endif
#if FM_USE_OPUS
/* Read a little-endian 64-bit value (Ogg granulepos). */
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* 10^(gain_q8 / (20 * 256)) in Q12, without libm: the gain is split
 * into whole steps of 6.0206 dB (a factor of two each) and a rest that
 * goes through a 16-entry table of 2^(i/16), linearly interpolated --
 * within 0.02 dB of the exact value, far below what can be heard.
 * The result is limited to +-48 dB. */
static int32_t opus_gain_q12(int32_t gain_q8)
{
    static const uint16_t pow2_16th[17] = {     /* 2^(i/16) * 4096 */
        4096, 4277, 4467, 4664, 4871, 5087, 5312, 5547, 5793,
        6049, 6317, 6597, 6889, 7194, 7512, 7845, 8192
    };
    /* gain_q8 / 1541.27 = gain in octaves (6.0206 dB * 256 = 1541.27);
     * work in 1/4096 octave */
    int32_t oct_q12 = (int32_t)(((int64_t)gain_q8 * 4096 * 100) / 154127);
    int32_t whole;
    int32_t frac;
    int32_t idx;
    int32_t rest;
    int32_t v;

    if (oct_q12 > 8 * 4096) oct_q12 = 8 * 4096;
    if (oct_q12 < -8 * 4096) oct_q12 = -8 * 4096;
    whole = oct_q12 / 4096;
    frac = oct_q12 % 4096;
    if (frac < 0) { frac += 4096; whole--; }
    idx = frac / 256;
    rest = frac % 256;
    v = (int32_t)pow2_16th[idx] +
        (((int32_t)pow2_16th[idx + 1] - (int32_t)pow2_16th[idx]) * rest) / 256;
    return (whole >= 0) ? v * (1 << whole) : v >> (-whole);
}

/*
 * Ogg Opus (.opus / OggS + OpusHead), whole-file decode at open — the
 * same memory model as flac_file_open()/vorbis_file_open().
 *
 * Layout reminder (RFC 7845 + RFC 3533):
 *   page header: "OggS" ver(1) type(1) granulepos(8) serial(4) seq(4)
 *                crc(4) numsegments(1) segment table(numsegments)
 *   a lacing value of 255 means "packet continues on the next page".
 *   First packet: "OpusHead"  ver(1) ch(1) preskip(2) rate(4) gain(2) map(1)
 *   Second packet: "OpusTags" ...
 *   Audio packets: Opus frames, always decoded at 48 kHz.
 * granulepos of the final page (minus preskip) is the exact total
 * number of 48 kHz samples, so the PCM buffer is sized exactly.
 */
static FM_Result opus_file_open(FM_Audio *a, uint64_t base) {
    uint64_t sz = 0;
    uint8_t *data = NULL;
    uint32_t got = 0;
    FM_Result r;

    /* No decoder (built without libopus): say so before pulling the
     * whole file into RAM. */
    if (FM_Opus_GetOps() == NULL) return FM_ERR_UNSUPPORTED;

    r = File_Size(&a->file, &sz);
    if (r != FM_OK) return r;
    if (sz < 27u) return FM_ERR_CORRUPTED;
    if (sz - base > FM_OPUS_MAX_FILE_BYTES) return FM_ERR_TOO_LARGE;
    sz -= base;                            /* an ID3v2 tag in front */
    data = (uint8_t*)FM_Alloc((size_t)sz);
    if (!data) return FM_ERR_NO_MEMORY;
    (void)File_Seek(&a->file, (int64_t)base, FM_SEEK_SET);
    r = File_Read(&a->file, data, (uint32_t)sz, &got);
    if (r != FM_OK || got != sz) { FM_Free(data); return FM_ERR_IO; }
    if (memcmp(data, "OggS", 4) != 0) { FM_Free(data); return FM_ERR_INVALID_FORMAT; }

    /* ---------- pass 1: OpusHead + final granulepos ---------- */
    {
        uint32_t head_serial = 0;
        int have_head = 0;
        uint16_t channels = 2;
        uint16_t preskip = 0;
        int32_t gain_q8 = 0;        /* OpusHead output gain, dB * 256 */
        int unsupported_layout = 0;
        uint64_t granule_final = 0;
        size_t off = 0;

        while (off + 27 <= (size_t)sz) {
            uint8_t nseg;
            size_t body, p, page_end;
            uint64_t gran;
            uint32_t serial;
            size_t i;

            if (memcmp(data + off, "OggS", 4) != 0) break;
            nseg = data[off + 26];
            body = off + 27u + nseg;
            if (body > (size_t)sz) break;
            gran   = rd64(data + off + 6);
            serial = rd32(data + off + 14);

            p = body;
            for (i = 0; i < nseg; i++) {
                size_t plen = data[off + 27 + i];
                if (p + plen > (size_t)sz) { nseg = 0; break; }
                if (!have_head && plen >= 19u &&
                    memcmp(data + p, "OpusHead", 8) == 0) {
                    channels = data[p + 9];
                    /* Mapping family 0 is one stream of 1 or 2 channels,
                     * the only layout the decoder below is set up for.
                     * Anything else (5.1 is family 1, several coupled
                     * streams in each packet) used to be "corrected" to
                     * 2 channels and its packets given to a stereo
                     * decoder: FM_OK and clipped noise. */
                    if (channels == 0 || channels > 2 || data[p + 18] != 0u)
                    {
                        unsupported_layout = 1;
                        channels = 2;
                    }
                    preskip = (uint16_t)(data[p + 10] |
                                         ((uint16_t)data[p + 11] << 8));
                    gain_q8 = (int16_t)(data[p + 16] |
                                        ((uint16_t)data[p + 17] << 8));
                    head_serial = serial;
                    have_head = 1;
                }
                p += plen;
            }
            if (nseg == 0) break;
            /* Track the last sane granule for the header's stream.  A
             * granule of -1 ("no data on this page") reads as ~2^64-1
             * and is skipped by the sanity range check below. */
            if (have_head && serial == head_serial &&
                gran <= (uint64_t)48000u * 3600u * 4u) {
                if (gran > granule_final) granule_final = gran;
            }
            page_end = p;                      /* == body + sum(lacing)  */
            if (page_end <= off) break;        /* corrupt: no progress   */
            off = page_end;
        }

        if (!have_head) { FM_Free(data); return FM_ERR_CORRUPTED; }
        if (unsupported_layout) { FM_Free(data); return FM_ERR_UNSUPPORTED; }

        /* ---------- size the PCM buffer ---------- */
        {
            uint64_t total48 = (granule_final > (uint64_t)preskip)
                               ? (granule_final - preskip) : 0u;
            size_t want;
            PcmBuffer *b;
            int16_t *pcm;
            void *ctx = NULL;
            const FM_AudioDecoderOps *ops = FM_Opus_GetOps();
            uint8_t *pend = NULL;   /* cross-page packet assembly       */
            size_t pend_len = 0;
            size_t written = 0;     /* interleaved samples in pcm       */
            size_t cap_samples;
            size_t off2 = 0;
            int truncated = 0;
            int length_known = 1;
            int16_t *scratch = NULL;
            /* Max Opus packet group: 120 ms at 48 kHz, 2 ch. */
            enum { OPUS_SCRATCH_SAMPLES = 5760u * 2u };

            if (ops == NULL || ops->init == NULL || ops->decode == NULL ||
                ops->close == NULL) {
                FM_Free(data);
                return FM_ERR_UNSUPPORTED;
            }

            /* Unknown/zero granule (streaming dump): estimate from size,
             * ~20 ms frames at >= 16 kbps -> >= 40 bytes/frame, i.e. 2400
             * samples/s/ch worst case; over-estimating is corrected after
             * the decode loop by shrinking b->total. */
            if (total48 == 0) {
                length_known = 0;
                total48 = (uint64_t)sz * 120u;      /* generous upper bound */
                if (total48 > (uint64_t)48000u * 3600u) total48 = (uint64_t)48000u * 3600u;
            }

            {
                /* 64-bit: total48 * channels * 2 wraps a 32-bit size_t. */
                uint64_t want64 = total48 * (uint64_t)channels *
                                  (uint64_t)sizeof(int16_t);
                if (want64 > (uint64_t)FM_OPUS_MAX_PCM_BYTES) {
                    if (length_known) {
                        /* The stream itself says it does not fit: refuse
                         * now instead of decoding the whole cap first. */
                        FM_Free(data);
                        return FM_ERR_TOO_LARGE;
                    }
                    want = (size_t)FM_OPUS_MAX_PCM_BYTES;
                    truncated = 1;
                } else {
                    want = (size_t)want64;
                }
            }
            pcm = (int16_t*)FM_Alloc(want);
            if (!pcm) { FM_Free(data); return FM_ERR_NO_MEMORY; }
            scratch = (int16_t*)FM_Alloc(OPUS_SCRATCH_SAMPLES * sizeof(int16_t));
            if (!scratch) { FM_Free(pcm); FM_Free(data); return FM_ERR_NO_MEMORY; }
            cap_samples = want / sizeof(int16_t);

            /* ---------- pass 2: decode every audio packet ---------- */
            r = ops->init(&ctx, &((FM_AudioInfo){ .sample_rate = 48000,
                                                  .channels = channels,
                                                  .bits_per_sample = 16 }));
            if (r != FM_OK) {
                FM_Free(scratch); FM_Free(pcm); FM_Free(data);
                return r;
            }

            while (off2 + 27 <= (size_t)sz && written < cap_samples) {
                uint8_t nseg = data[off2 + 26];
                size_t body = off2 + 27u + nseg;
                size_t p;
                size_t i;
                size_t page_end;

                if (memcmp(data + off2, "OggS", 4) != 0 || body > (size_t)sz)
                    break;
                p = body;
                for (i = 0; i < nseg; i++) {
                    size_t plen = data[off2 + 27 + i];
                    const uint8_t *pkt;
                    size_t pkt_len;

                    if (p + plen > (size_t)sz) { nseg = 0; break; }

                    /* Append this segment to the pending packet. */
                    if (pend_len == 0) {
                        pkt = data + p; pkt_len = plen;
                    } else {
                        uint8_t *np = (uint8_t*)FM_ReallocSized(pend, pend_len,
                                                                  pend_len + plen);
                        if (np == NULL) {
                            FM_Free(pend); pend = NULL; pend_len = 0;  /* leak fix (bug #9) */
                            r = FM_ERR_NO_MEMORY; goto done;
                        }
                        pend = np;
                        memcpy(pend + pend_len, data + p, plen);
                        pend_len += plen;
                        pkt = pend; pkt_len = pend_len;
                    }
                    p += plen;

                    if (data[off2 + 27 + i] == 255u) {
                        /* Packet continues on the next page. */
                        if (pend_len == 0) {
                            pend = (uint8_t*)FM_Alloc(pkt_len);
                            if (pend == NULL) { r = FM_ERR_NO_MEMORY; goto done; }
                            memcpy(pend, pkt, pkt_len);
                            pend_len = pkt_len;
                        }
                        continue;
                    }

                    /* Complete packet: skip the two header packets. */
                    if (pkt_len >= 8 && (memcmp(pkt, "OpusHead", 8) == 0 ||
                                         memcmp(pkt, "OpusTags", 8) == 0)) {
                        /* nothing */
                    } else if (pkt_len >= 1) {
                        size_t out_samples = 0;
                        FM_Result dr = ops->decode(ctx, pkt, pkt_len, scratch,
                                                   OPUS_SCRATCH_SAMPLES,
                                                   &out_samples);
                        if (dr == FM_OK && out_samples > 0) {
                            /* Pre-skip trim (RFC 7845 4.2): the first
                             * `preskip` decoded 48k samples are priming
                             * samples and are not part of the signal. */
                            if (preskip > 0) {
                                size_t skip = (size_t)preskip * channels;
                                if (skip >= out_samples) {
                                    preskip = (uint16_t)((skip - out_samples) / channels);
                                    out_samples = 0;
                                } else {
                                    out_samples -= skip;
                                    memmove(scratch, scratch + skip,
                                            out_samples * sizeof(int16_t));
                                    preskip = 0;
                                }
                            }
                            if (out_samples > cap_samples - written)
                                out_samples = cap_samples - written;
                            memcpy(pcm + written, scratch,
                                   out_samples * sizeof(int16_t));
                            written += out_samples;
                        } else if (dr != FM_OK && dr != FM_ERR_TOO_LARGE) {
                            /* Corrupt packet: skip it (decoder stays
                             * usable; PLC would need ordering info we
                             * do not track here). */
                            FM_AUDIO_LOG("opus: packet decode %d", (int)dr);
                        }
                    }
                    if (pend) { FM_Free(pend); pend = NULL; pend_len = 0; }
                }
                if (nseg == 0) break;
                page_end = p;
                if (page_end <= off2) break;
                off2 = page_end;
            }
            if (pend) { FM_Free(pend); pend = NULL; pend_len = 0; }

done:
            ops->close(ctx);
            FM_Free(scratch);
            FM_Free(data);
            if (r != FM_OK) { FM_Free(pcm); return r; }
            if (written == 0) { FM_Free(pcm); return FM_ERR_CORRUPTED; }
            if (truncated && written >= cap_samples) {
                /* Never report full success for truncated output (bug #4). */
                FM_Free(pcm);
                return FM_ERR_TOO_LARGE;
            }
            /* RFC 7845 5.1: the output gain of the header is part of the
             * stream, a player has to apply it.  It was not read at all,
             * so a file mastered with -6 dB there played twice as loud
             * as everywhere else. */
            if (gain_q8 != 0) {
                int32_t scale = opus_gain_q12(gain_q8);
                size_t k;

                for (k = 0; k < written; k++) {
                    int32_t v = (int32_t)(((int64_t)pcm[k] * scale) / 4096);

                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    pcm[k] = (int16_t)v;
                }
            }

            b = (PcmBuffer*)FM_Alloc(sizeof(PcmBuffer));
            if (!b) { FM_Free(pcm); return FM_ERR_NO_MEMORY; }
            b->pcm = pcm;
            b->total = written;
            b->pos = 0;
            a->info.sample_rate = 48000;
            a->info.channels = channels;
            a->info.bits_per_sample = 16;
            a->info.format = 0x6F70;                 /* 'op' */
            (void)strcpy(a->info.codec, "OPUS");
            a->info.data_offset = 0;
            a->info.data_size = sz;
            a->info.total_frames = written / channels;
            a->codec_ctx = b;
            return FM_OK;
        }
    }
}
#endif

static FM_Result audio_parse_wav(FM_Audio *a, uint64_t base)
{
    uint8_t riff[12];
    FM_Bool have_fmt = FM_FALSE;
    FM_Bool have_data = FM_FALSE;
    uint64_t pos = 12;
    uint64_t file_size = 0;

    if (base != 0u && File_Seek(&a->file, (int64_t)base, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (file_read_exact(&a->file, riff, sizeof(riff), NULL) != FM_OK ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (File_Size(&a->file, &file_size) != FM_OK)
    {
        return FM_ERR_IO;
    }
    /* Chunk offsets and the end of the data are relative to the RIFF
     * header, which sits behind an ID3v2 tag when there is one. */
    file_size -= base;
    pos = 12u;

    while (pos + 8 <= file_size)
    {
        uint8_t chunk[8];
        uint32_t chunk_size;
        uint64_t payload;
        uint64_t next;

        if (file_read_exact(&a->file, chunk, sizeof(chunk), NULL) != FM_OK)
        {
            return FM_ERR_EOF;
        }
        chunk_size = rd32(chunk + 4);
        payload = pos + 8;
        if ((uint64_t)chunk_size > file_size - payload)
        {
            if (memcmp(chunk, "data", 4) == 0 &&
                file_size - payload <= UINT32_MAX)
            {
                /* A data chunk that runs past the end of the file (a
                 * partially copied/recorded WAV): play what is actually
                 * there instead of rejecting the whole file. */
                chunk_size = (uint32_t)(file_size - payload);
            }
            else
            {
                return FM_ERR_CORRUPTED;
            }
        }
        next = payload + chunk_size + (chunk_size & 1u);
        if (next <= pos)
        {
            /* A malformed chunk that does not advance the cursor would
             * spin this parser forever. Reject instead of hanging. */
            return FM_ERR_CORRUPTED;
        }
        if (next > file_size)
        {
            /* A final data chunk may lack its single odd padding byte
             * when the file was truncated mid-chunk (incomplete copy /
             * stopped recording). Accept it; the reader only consumes
             * whole bytes that are actually there. */
            if (memcmp(chunk, "data", 4) != 0 || next != file_size + 1u)
            {
                return FM_ERR_CORRUPTED;
            }
            next = file_size;
        }

        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            uint8_t fmt[16];

            if (chunk_size < sizeof(fmt) || file_read_exact(&a->file, fmt,
                                                            sizeof(fmt), NULL) != FM_OK)
            {
                return FM_ERR_INVALID_FORMAT;
            }
            a->info.format = rd16(fmt);
            a->info.channels = rd16(fmt + 2);
            a->info.sample_rate = rd32(fmt + 4);
            a->info.bits_per_sample = rd16(fmt + 14);
            a->info.block_align = rd16(fmt + 12);
            have_fmt = FM_TRUE;
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            if (chunk_size == 0u && have_fmt && file_size > payload)
            {
                /* A length of 0 with sound behind it: a recording that
                 * was never closed (power lost, reset) before its first
                 * checkpoint -- the writer fills the length in later.
                 * Such a file used to open as an empty track although
                 * all its samples are on the card.  Like other players:
                 * the sound runs to the end of the file. */
                uint64_t rest = file_size - payload;

                chunk_size = (rest > UINT32_MAX) ? UINT32_MAX : (uint32_t)rest;
                next = file_size;
                FM_EVENT_LOG("wav: data length 0, %lu bytes follow: played to the end of the file",
                             (unsigned long)chunk_size);
            }
            a->info.data_offset = base + payload;
            a->info.data_size = chunk_size;
            have_data = FM_TRUE;
        }

        if (next > payload)
        {
            if (File_Seek(&a->file, (int64_t)(base + next), FM_SEEK_SET) != FM_OK)
            {
                return FM_ERR_IO;
            }
        }
        pos = next;
        if (have_fmt && have_data)
        {
            break;
        }
    }
    if (!have_fmt || !have_data ||
        a->info.channels == 0 || a->info.channels > 2 ||
        a->info.sample_rate == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (a->info.format == 1u) {
        if (a->info.bits_per_sample != 8 && a->info.bits_per_sample != 16) return FM_ERR_UNSUPPORTED;
        (void)strcpy(a->info.codec, "WAV");
    } else if (a->info.format == 6u) {
        (void)strcpy(a->info.codec, "ALAW");
        a->info.bits_per_sample = 16;
    } else if (a->info.format == 7u) {
        (void)strcpy(a->info.codec, "ULAW");
        a->info.bits_per_sample = 16;
    } else if (a->info.format == 0x11u) {
        /* IMA ADPCM, 4 bits a sample. */
        if (a->info.bits_per_sample != 4u) return FM_ERR_UNSUPPORTED;
        if (a->info.block_align < 4u * a->info.channels) return FM_ERR_CORRUPTED;
        (void)strcpy(a->info.codec, "ADPCM");
        a->info.bits_per_sample = 16;
    } else if (a->info.format == 0x02u) {
        /* Microsoft ADPCM: a different coder with its own block layout
         * (fm_adpcm.h).  It was once run through the IMA decoder (FM_OK
         * and noise), then refused; it now has its decoder.  A block
         * starts with 7 bytes a channel. */
        if (a->info.bits_per_sample != 4u) return FM_ERR_UNSUPPORTED;
        if (a->info.block_align < 7u * a->info.channels) return FM_ERR_CORRUPTED;
        (void)strcpy(a->info.codec, "ADPCM");
        a->info.bits_per_sample = 16;
    } else {
        return FM_ERR_UNSUPPORTED;
    }
    /* Length in sample frames, for every WAV flavour */
    if (a->info.format == 1u) {
        a->info.total_frames = a->info.data_size /
            ((uint64_t)a->info.channels * (a->info.bits_per_sample / 8u));
    } else if (a->info.format == 0x11u || a->info.format == 0x02u) {
        a->info.total_frames = wav_adpcm_length_of(a);
    } else {
        a->info.total_frames = a->info.data_size / a->info.channels;
    }
    return File_Seek(&a->file, (int64_t)a->info.data_offset, FM_SEEK_SET);
}

/* ------------------------------------------------------------------ */
/* MP3 decoding (minimp3, streaming)                                   */
/* ------------------------------------------------------------------ */
#if FM_USE_MP3

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wcast-align"
#endif
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct
{
    FM_Bool  used;
    FM_File *file;
    mp3dec_t dec;
    uint32_t channels;
    uint32_t hz;
    uint32_t open_channels;      /* what FM_AudioInfo says since the open: */
    uint32_t open_hz;            /* every frame is handed out in this form */
    uint8_t  in[FM_MP3_INPUT_BUF];
    size_t   in_len;
    size_t   in_off;
    int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    size_t   pcm_len;
    size_t   pcm_pos;
    uint64_t total_samples;      /* interleaved samples emitted     */
    uint64_t first_frame;        /* file offset after ID3v2 tag     */
    uint64_t file_size;
    uint32_t bitrate_kbps;
    uint32_t granule_samples;
    uint32_t xing_frames;
    uint32_t xing_bytes;
    uint8_t  xing_toc[100];
    uint8_t  have_toc;
    uint8_t  have_xing;          /* first frame is a Xing/Info header */
    uint8_t  xing_vbr;           /* ...and its tag is "Xing": VBR     */
    uint32_t primed_frames;      /* frames consumed with no output    */
    uint64_t in_file_off;        /* file offset of in[0]              */
    uint64_t frame_file_off;     /* file offset of the frame in pcm[] */
    uint64_t audio_start;        /* file offset of the first frame
                                  * that carries audio                */
    /* Refill diagnostics (see FM_Mp3Stats / Audio_Mp3Stats).  Kept at the
     * end of the struct so the decode state layout stays stable. */
    uint32_t stat_read_calls;    /* File_Read calls on the input buf  */
    uint64_t stat_read_bytes;    /* total bytes pulled from the file  */
    uint32_t stat_frames;        /* MP3 frames decoded                */
    uint32_t stat_resync_bytes;  /* bytes skipped to resync (corrupt) */
} FM_Mp3Stream;

static FM_Mp3Stream g_mp3[FM_MAX_AUDIO];

static uint32_t mp3_syncsafe(const uint8_t *p)
{
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) |
           ((uint32_t)p[2] << 7) | (uint32_t)p[3];
}

static FM_Mp3Stream *mp3_acquire(FM_File *f)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_AUDIO; i++)
    {
        if (!g_mp3[i].used)
        {
            memset(&g_mp3[i], 0, sizeof(g_mp3[i]));
            g_mp3[i].used = FM_TRUE;
            g_mp3[i].file = f;
            FM_Unlock();
            return &g_mp3[i];
        }
    }
    FM_Unlock();
    return NULL;
}

static void mp3_release(FM_Mp3Stream *m)
{
    if (m != NULL)
    {
        /* under the lock mp3_acquire() scans the pool with: another
         * task may be opening a stream while this one closes */
        FM_Lock();
        m->used = FM_FALSE;
        FM_Unlock();
    }
}

/* Skip an ID3v2 tag if present and return the offset of the first byte
 * after it (the audio stream start). */
static FM_Result mp3_skip_id3(FM_File *f, uint64_t *off)
{
    uint8_t h[10];
    FM_Result r;
    uint64_t size;
    *off = 0u;
    r = File_Size(f, &size);
    if (r != FM_OK) return r;
    for (;;)
    {
        uint64_t total;
        r = File_Seek(f, (int64_t)*off, FM_SEEK_SET);
        if (r != FM_OK) return r;
        r = file_read_exact(f, h, sizeof(h), NULL);
        if (r == FM_ERR_EOF) break;
        if (r != FM_OK) return r;
        if (memcmp(h, "ID3", 3) != 0) break;
        total = 10u + (uint64_t)mp3_syncsafe(h + 6);
        if (h[5] & 0x10u) total += 10u;
        if (total > size - *off) return FM_ERR_INVALID_FORMAT;
        *off += total;
    }
    return File_Seek(f, (int64_t)*off, FM_SEEK_SET);
}

/*
 * Read-ahead low-water mark: the input buffer is topped up to full (one
 * File_Read) once the unconsumed tail drops below this many bytes.
 *
 * Batching the stream into a few large reads per audio refill keeps the
 * *number* of SDMMC commands low.  On realtime players that number is
 * what dominates refill jitter: every command adds fixed FatFS/SDMMC
 * overhead plus a chance of an intermittent card stall (10-200 ms), and
 * a refill is as slow as its slowest single read.  A 48 kHz half-buffer
 * refill decodes ~14 MP3 frames, so the old per-frame top-up issued ~15
 * reads; with read-ahead the same refill issues a handful.
 */
static size_t mp3_low_water(void)
{
    size_t buf = (size_t)FM_MP3_INPUT_BUF;
    size_t lw;

    lw = (FM_MP3_LOW_WATER != 0) ? (size_t)FM_MP3_LOW_WATER : buf / 4u;
    if (lw < 1024u)
    {
        lw = 1024u;
    }
    if (lw * 2u > buf)
    {
        /* Never let the mark exceed half the buffer: a top-up must
         * always be able to bring the buffer close to full again. */
        lw = buf / 2u;
    }
    return lw;
}

/* Move unconsumed bytes to the front and top the buffer up to full. */
static FM_Result mp3_fill(FM_Mp3Stream *m)
{
    uint32_t rd = 0;
    size_t avail;
    FM_Result r;

    if (m->in_off > 0)
    {
        memmove(m->in, m->in + m->in_off, m->in_len - m->in_off);
        m->in_len -= m->in_off;
        m->in_file_off += (uint64_t)m->in_off;
        m->in_off = 0;
    }
    avail = sizeof(m->in) - m->in_len;
    if (avail == 0)
    {
        return FM_OK;
    }
    r = File_Read(m->file, m->in + m->in_len, (uint32_t)avail, &rd);
    m->in_len += rd;
    m->stat_read_calls++;
    m->stat_read_bytes += (uint64_t)rd;
    if (r == FM_ERR_EOF || (r == FM_OK && rd == 0u))
    {
        return FM_ERR_EOF;
    }
    return r;
}

/* Decode one frame into m->pcm.  Returns FM_OK (pcm_len set) or FM_ERR_EOF.
 *
 * Input is managed in read-ahead blocks (see mp3_low_water()): as soon as
 * the unconsumed tail falls below the low-water mark the buffer is topped
 * up to full in a single File_Read, BEFORE the next frame is decoded.  A
 * realtime refill of one DMA half therefore issues a handful of large
 * sequential reads instead of one small read per MP3 frame — which is
 * what keeps MP3 refills from approaching the DMA half deadline on cards
 * with per-command latency spikes. */
static FM_Result mp3_decode_one(FM_Mp3Stream *m)
{
    mp3dec_frame_info_t info;
    int samples;
    size_t lw;
    FM_Bool eof_seen = FM_FALSE;

    lw = mp3_low_water();

    for (;;)
    {
        size_t avail = m->in_len - m->in_off;

        /* Read ahead: top the buffer up while the tail is running low.
         * (avail < lw implies the buffer is not full, since
         * lw <= sizeof(m->in) / 2, so a top-up always has room.) */
        if (avail < lw && !eof_seen)
        {
            FM_Result r = mp3_fill(m);

            if (r == FM_OK)
            {
                continue;
            }
            if (r != FM_ERR_EOF)
            {
                return r;
            }
            eof_seen = FM_TRUE;
        }

        samples = mp3dec_decode_frame(&m->dec, m->in + m->in_off,
                                      (int)(m->in_len - m->in_off),
                                      m->pcm, &info);
        if (samples > 0)
        {
            /* A file made by joining two MP3s can change its sample rate
             * or its channel count in the middle.  Everything that reads
             * the stream works with the figures of the open (the info
             * block, positions, the player's I2S set-up), so a frame in
             * another form used to be played as if nothing had changed:
             * stereo read as mono at half speed, and so on.
             *   - another channel count: the frame is converted to the
             *     count of the open (below), the track plays on;
             *   - another sample rate: cannot be played on a clock that
             *     is already running.  FM_ERR_UNSUPPORTED, and the frame
             *     is left where it is, so every further read says the
             *     same: the track ends here with a reason. */
            if (m->open_hz != 0u && (uint32_t)info.hz != m->open_hz)
            {
                FM_ERR_LOG("mp3: sample rate changes in the file (%lu -> %d Hz)",
                           (unsigned long)m->open_hz, info.hz);
                return FM_ERR_UNSUPPORTED;
            }
            if (m->open_channels != 0u &&
                (uint32_t)info.channels != m->open_channels)
            {
                int k;

                if (m->open_channels == 2u && info.channels == 1)
                {
                    for (k = samples - 1; k >= 0; k--)   /* mono -> L = R */
                    {
                        m->pcm[2 * k + 1] = m->pcm[k];
                        m->pcm[2 * k] = m->pcm[k];
                    }
                }
                else if (m->open_channels == 1u && info.channels == 2)
                {
                    for (k = 0; k < samples; k++)        /* (L + R) / 2 */
                    {
                        m->pcm[k] = (int16_t)(((int32_t)m->pcm[2 * k] +
                                               (int32_t)m->pcm[2 * k + 1]) / 2);
                    }
                }
                else
                {
                    return FM_ERR_UNSUPPORTED;
                }
                info.channels = (int)m->open_channels;
            }
            /* Consume the frame exactly as minimp3 reports it:
             * frame_bytes is frame_offset + frame_size, i.e. the distance
             * from the buffer start to the first byte after this frame
             * (garbage skipped during a resync included).  Adding
             * frame_offset again would over-consume and strand a frame. */
            m->frame_file_off = m->in_file_off + (uint64_t)m->in_off +
                                (uint64_t)info.frame_offset;
            m->in_off += (size_t)info.frame_bytes;
            m->channels = (uint32_t)info.channels;
            m->hz = (uint32_t)info.hz;
            m->bitrate_kbps = (uint32_t)info.bitrate_kbps;
            m->granule_samples = (uint32_t)samples;
            m->pcm_len = (size_t)samples * (size_t)info.channels;
            m->pcm_pos = 0;
            m->stat_frames++;
            return FM_OK;
        }
        if (m->dec.header[0] == 0xFFu && info.frame_bytes > 0)
        {
            /* A frame was there and was parsed, but there is nothing to
             * play yet: its audio starts in the bit reservoir of frames
             * this decoder has not seen (the first frames after a seek
             * or after a gap).  minimp3 has kept what the frame adds to
             * the reservoir, so the frame is consumed as a whole and the
             * next one continues from it.
             *
             * This used to fall through to "skip one byte" below.  That
             * made minimp3 start over, without a reservoir, at the very
             * next frame -- again and again, until a frame happened to
             * need none.  Few frames do: a seek into a 32 kbit/s file
             * ran to the end of the file and reported FM_ERR_EOF. */
            m->in_off += (size_t)info.frame_bytes;
            m->primed_frames++;
            continue;
        }
        /* No frame at all in what the buffer holds from in_off on.  More
         * bytes can change that for a frame cut off by the end of the
         * buffer, so first make sure the buffer is as full as it gets:
         * also when it is full but partly consumed (in_off > 0). */
        if (!eof_seen && (m->in_len < sizeof(m->in) || m->in_off > 0u))
        {
            FM_Result r = mp3_fill(m);

            if (r == FM_OK)
            {
                continue;
            }
            if (r != FM_ERR_EOF)
            {
                return r;
            }
            eof_seen = FM_TRUE;
            continue; /* try the final tail once more, then resync below */
        }
        if (m->in_off < m->in_len)
        {
            /* Corrupt bytes: go on at the next place a frame header can
             * be.  Stepping a single byte gave the same answer, but ran
             * the frame search over the whole buffer once per byte: 2 MiB
             * that is not MP3 took seconds to refuse.  The last three
             * bytes stay, a header may continue in the next block. */
            size_t next = m->in_off + 1u;

            while (next + 4u <= m->in_len && !hdr_valid(m->in + next))
            {
                next++;
            }
            m->stat_resync_bytes += (uint32_t)(next - m->in_off);
            m->in_off = next;
        }
        else
        {
            return FM_ERR_EOF;
        }
    }
}

static uint32_t mp3_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Xing/Info is a silent first frame many encoders write.  The TOC lets
 * VBR files seek by file offset instead of decoding every frame. */
static void mp3_try_xing(FM_Mp3Stream *m)
{
    size_t i;

    for (i = 0; i + 12u < m->in_len && i < 80u; i++)
    {
        if (memcmp(m->in + i, "Xing", 4) == 0 ||
            memcmp(m->in + i, "Info", 4) == 0)
        {
            uint32_t flags = mp3_be32(m->in + i + 4u);
            const uint8_t *p = m->in + i + 8u;
            size_t left = m->in_len - (i + 8u);

            m->have_xing = 1u;
            m->xing_vbr = (uint8_t)((m->in[i] == (uint8_t)'X') ? 1u : 0u);

            if ((flags & 1u) != 0u && left >= 4u)
            {
                m->xing_frames = mp3_be32(p);
                p += 4u;
                left -= 4u;
            }
            if ((flags & 2u) != 0u && left >= 4u)
            {
                m->xing_bytes = mp3_be32(p);
                p += 4u;
                left -= 4u;
            }
            if ((flags & 4u) != 0u && left >= 100u)
            {
                memcpy(m->xing_toc, p, 100u);
                m->have_toc = 1u;
            }
            return;
        }
    }
}

static uint64_t mp3_duration_frames(const FM_Mp3Stream *m)
{
    if (m->xing_frames != 0u && m->granule_samples != 0u)
    {
        return (uint64_t)m->xing_frames * (uint64_t)m->granule_samples;
    }
    if (m->bitrate_kbps != 0u && m->hz != 0u && m->file_size > m->audio_start)
    {
        uint64_t audio_bytes = m->file_size - m->audio_start;

        /* seconds = audio_bytes / (bitrate_kbps * 125); frames = seconds * hz */
        return (audio_bytes * (uint64_t)m->hz) /
               ((uint64_t)m->bitrate_kbps * 125u);
    }
    return 0u;
}

static FM_Result mp3_open(FM_Audio *a)
{
    FM_Mp3Stream *m = mp3_acquire(&a->file);
    FM_Result r;

    if (m == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    mp3dec_init(&m->dec);
    r = mp3_skip_id3(&a->file, &m->first_frame);
    if (r != FM_OK)
    {
        mp3_release(m);
        return r;
    }
    m->in_file_off = m->first_frame;
    r = mp3_fill(m);
    if (r == FM_ERR_EOF || m->in_len == 0)
    {
        mp3_release(m);
        return FM_ERR_INVALID_FORMAT;
    }
    if (r != FM_OK)
    {
        mp3_release(m);
        return r;
    }
    mp3_try_xing(m);
    r = mp3_decode_one(m);
    if (r != FM_OK)
    {
        mp3_release(m);
        /* no frame in the whole file: not an MP3.  A read error is a
         * read error, whatever the file is. */
        return (r == FM_ERR_EOF) ? FM_ERR_INVALID_FORMAT : r;
    }
    /* the form of the stream from here on (see mp3_decode_one) */
    m->open_channels = m->channels;
    m->open_hz = m->hz;
    if (m->have_xing)
    {
        /* The Xing/Info frame describes the stream and carries no audio:
         * decoded, it is one frame of silence in front of the track, and
         * one frame more than the header itself counts. */
        m->pcm_len = 0;
        m->pcm_pos = 0;
        m->audio_start = m->in_file_off + (uint64_t)m->in_off;
    }
    else
    {
        m->audio_start = m->frame_file_off;
    }
    (void)File_Size(&a->file, &m->file_size);
    a->info.sample_rate = m->hz;
    a->info.channels = (uint16_t)m->channels;
    a->info.bits_per_sample = 16;
    a->info.format = 1; /* decoded output is 16-bit PCM */
    {
        uint64_t frames = mp3_duration_frames(m);

        if (frames != 0u && a->info.channels != 0u)
        {
            a->info.data_size = frames * (uint64_t)a->info.channels * 2u;
            a->info.total_frames = frames;      /* exact with a Xing
                                                 * header, else estimated
                                                 * from the first frame */
        }
    }
    (void)strcpy(a->info.codec, "MP3");
    a->codec_ctx = m;
    return FM_OK;
}

static FM_Result mp3_read(FM_Audio *a, int16_t *samples, size_t count,
                          size_t *read)
{
    FM_Mp3Stream *m = (FM_Mp3Stream *)a->codec_ctx;
    size_t total = 0;
    FM_Result err = FM_ERR_EOF;

    while (total < count)
    {
        if (m->pcm_pos >= m->pcm_len)
        {
            FM_Result r = mp3_decode_one(m);

            if (r != FM_OK)
            {
                err = r;
                break;
            }
        }
        {
            size_t avail = m->pcm_len - m->pcm_pos;
            size_t n = count - total;

            if (n > avail)
            {
                n = avail;
            }
            memcpy(samples + total, m->pcm + m->pcm_pos,
                   n * sizeof(int16_t));
            m->pcm_pos += n;
            total += n;
            m->total_samples += n;
        }
    }
    if (read != NULL)
    {
        *read = total;
    }
    a->frame_index = m->total_samples / a->info.channels;
    if (total > 0 && a->callback != NULL)
    {
        a->callback(samples, total, a->callback_data);
    }
    /* What was decoded is delivered first.  With nothing to deliver the
     * reason is reported as it is: a failed read used to come back as
     * FM_ERR_EOF, and a player took a card error for the end of the
     * song.  (The next call tries the read again.) */
    return (total == 0) ? err : FM_OK;
}

/* Constant bit rate: where the frame in pcm[] is in the track, in sample
 * frames, from where it is in the file. */
static uint64_t mp3_cbr_position(const FM_Mp3Stream *m)
{
    uint64_t num;
    uint64_t den = (uint64_t)m->granule_samples *
                   (uint64_t)m->bitrate_kbps * 125u;

    if (den == 0u || m->frame_file_off < m->audio_start)
    {
        return 0u;
    }
    num = (m->frame_file_off - m->audio_start) * (uint64_t)m->hz;
    return ((num + den / 2u) / den) * (uint64_t)m->granule_samples;
}

static FM_Result mp3_seek(FM_Audio *a, double seconds)
{
    FM_Mp3Stream *m = (FM_Mp3Stream *)a->codec_ctx;
    uint64_t target_frames;
    uint64_t duration_frames;
    uint64_t audio_bytes;
    uint64_t offset;
    uint64_t position;
    FM_Bool cbr;
    FM_Bool from_start;
    FM_Result r;

    if (m->hz == 0u || a->info.channels == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    target_frames = audio_time_to_frame(seconds, m->hz);
    duration_frames = mp3_duration_frames(m);
    if (duration_frames != 0u && target_frames >= duration_frames)
    {
        /* Past the end: say so (as this did) and also park the stream
         * after the last frame, as every other format does here.  A
         * caller that goes on reading after the error (SimpleAudio
         * does not, Player_Seek callers may) used to get the sound
         * from where the seek was asked, not the end of the track. */
        mp3dec_init(&m->dec);
        m->in_len = 0;
        m->in_off = 0;
        m->pcm_len = 0;
        m->pcm_pos = 0;
        m->primed_frames = 0u;
        m->in_file_off = m->file_size;
        if (m->file_size != 0u &&
            File_Seek(m->file, (int64_t)m->file_size, FM_SEEK_SET) != FM_OK)
        {
            return FM_ERR_IO;
        }
        m->total_samples = duration_frames * (uint64_t)a->info.channels;
        a->frame_index = duration_frames;
        return FM_ERR_EOF;
    }

    audio_bytes = (m->xing_bytes != 0u) ? (uint64_t)m->xing_bytes
                                        : (m->file_size > m->first_frame
                                               ? m->file_size - m->first_frame
                                               : 0u);
    /* Constant bit rate (no header frame, or one tagged "Info"): the byte
     * offset of a frame follows from its number, and its number from its
     * offset.  The table of a "Xing" header is for variable bit rate. */
    cbr = (FM_Bool)((!m->have_xing || !m->xing_vbr) && m->bitrate_kbps != 0u &&
                    m->granule_samples != 0u);
    if (!cbr && m->have_toc && duration_frames != 0u && audio_bytes != 0u)
    {
        uint32_t percent;
        uint32_t fa;
        uint32_t fb;
        uint64_t num;

        percent = (uint32_t)((target_frames * 100u) / duration_frames);
        if (percent > 99u)
        {
            percent = 99u;
        }
        fa = m->xing_toc[percent];
        fb = (percent < 99u) ? m->xing_toc[percent + 1u] : 256u;
        num = target_frames * 100u - (uint64_t)percent * duration_frames;
        offset = m->first_frame +
                 (((uint64_t)fa * duration_frames +
                   (uint64_t)(fb - fa) * num) *
                  audio_bytes) /
                     (256u * duration_frames);
    }
    else if (cbr)
    {
        /* Ten frames before the one the target is in: what a frame needs
         * from the bit reservoir of the frames before it (up to 511
         * bytes) is then there when the target frame is reached, and the
         * frames in between are decoded and not played. */
        uint64_t number = target_frames / m->granule_samples;

        offset = (number > 10u)
                     ? m->audio_start +
                           ((number - 10u) * (uint64_t)m->granule_samples *
                            (uint64_t)m->bitrate_kbps * 125u) / m->hz
                     : m->first_frame;
    }
    else
    {
        offset = m->first_frame;
    }
    from_start = (FM_Bool)(target_frames == 0u || offset <= m->audio_start);
    if (from_start)
    {
        offset = m->first_frame;
    }
    if (m->file_size != 0u && offset >= m->file_size)
    {
        return FM_ERR_EOF;
    }

    mp3dec_init(&m->dec);
    m->in_len = 0;
    m->in_off = 0;
    m->pcm_len = 0;
    m->pcm_pos = 0;
    m->primed_frames = 0u;
    m->in_file_off = offset;
    if (File_Seek(m->file, (int64_t)offset, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    r = mp3_decode_one(m);
    if (r != FM_OK)
    {
        return r;
    }
    if (from_start)
    {
        if (m->have_xing)
        {
            /* the Xing/Info frame, see mp3_open: not audio */
            r = mp3_decode_one(m);
            if (r != FM_OK)
            {
                m->pcm_len = 0;
                m->pcm_pos = 0;
                return r;
            }
        }
        position = 0u;
    }
    else
    {
        /* Landing in the middle of the stream: the first decoded frame
         * has no previous granule for the IMDCT overlap and often
         * clips/pops.  Drop it; keep it if the next frame is the end, so
         * a seek near the end still has something to play. */
        FM_Result r2 = mp3_decode_one(m);

        if (r2 != FM_OK && r2 != FM_ERR_EOF)
        {
            return r2;
        }
        if (r2 == FM_OK)
        {
            m->primed_frames++;         /* the dropped one */
        }
        /* Variable bit rate: the table of the Xing header has 100 entries
         * for the whole track.  The position is the request plus what
         * was passed over (frames that only filled the bit reservoir,
         * and the one dropped above), and can be off by a few frames. */
        position = target_frames +
                   (uint64_t)m->primed_frames * m->granule_samples;
    }
    if (cbr)
    {
        /* Constant bit rate: the frame now in pcm[] is at a known file
         * offset, and frames of one size (give or take the padding byte)
         * make that its number.  Go on to the frame the target is in and
         * start inside it: the seek is exact to the sample, and so is
         * the position.  (Reporting the requested time, as this used to,
         * put the position up to a tenth of a second behind the sound.) */
        uint32_t guard = 0;

        position = mp3_cbr_position(m);
        while (position + m->granule_samples <= target_frames && guard < 32u)
        {
            r = mp3_decode_one(m);
            if (r != FM_OK)
            {
                m->pcm_len = 0;
                m->pcm_pos = 0;
                return r;               /* FM_ERR_EOF: past the last frame */
            }
            position = mp3_cbr_position(m);
            guard++;
        }
        if (position < target_frames &&
            target_frames - position < m->granule_samples)
        {
            m->pcm_pos = (size_t)(target_frames - position) * m->channels;
            if (m->pcm_pos > m->pcm_len)
            {
                m->pcm_pos = m->pcm_len;
            }
            position = target_frames;
        }
    }
    target_frames = position;
    m->total_samples = target_frames * (uint64_t)a->info.channels;
    a->frame_index = target_frames;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* MP3 refill diagnostics                                              */
/* ------------------------------------------------------------------ */

/*
 * Diagnostic counters for the MP3 input pipeline.  Intended for realtime
 * players that refill one DMA half per poll: bracket a refill with
 * Audio_Mp3StatsReset() + Audio_Mp3Stats() to see exactly how the refill
 * split between SD/flash reads and decoder work:
 *
 *   Audio_Mp3StatsReset(&audio);
 *   AudioDma_Poll(&player);                    // one refill
 *   Audio_Mp3Stats(&audio, &s);
 *   // s.read_calls   = File_Read commands the refill issued
 *   // s.read_bytes   = compressed bytes pulled from the file
 *   // s.frames       = MP3 frames decoded
 *   //
 *   refill_ms / read_calls  = per-command cost (SDMMC + FatFS + card)
 *   refill_ms / frames      = per-frame cost (decode + copy)
 *
 * read_calls must stay small (single digits) per realtime refill; if the
 * card shows per-command spikes, the read-ahead block filling above keeps
 * the number of commands — and the chance of hitting one — low.
 */
FM_Result Audio_Mp3Stats(const FM_Audio *a, FM_Mp3Stats *stats)
{
    const FM_Mp3Stream *m;

    if (a == NULL || stats == NULL || a->codec_ctx == NULL ||
        strcmp(a->info.codec, "MP3") != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    m = (const FM_Mp3Stream *)a->codec_ctx;
    stats->read_calls = m->stat_read_calls;
    stats->read_bytes = m->stat_read_bytes;
    stats->frames = m->stat_frames;
    stats->resync_bytes = m->stat_resync_bytes;
    return FM_OK;
}

/* Reset the MP3 diagnostic counters for `a`. */
FM_Result Audio_Mp3StatsReset(const FM_Audio *a)
{
    FM_Mp3Stream *m;

    if (a == NULL || a->codec_ctx == NULL ||
        strcmp(a->info.codec, "MP3") != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    m = (FM_Mp3Stream *)a->codec_ctx;
    m->stat_read_calls = 0u;
    m->stat_read_bytes = 0u;
    m->stat_frames = 0u;
    m->stat_resync_bytes = 0u;
    return FM_OK;
}

#else /* FM_USE_MP3 */

FM_Result Audio_Mp3Stats(const FM_Audio *a, FM_Mp3Stats *stats)
{
    (void)a;
    (void)stats;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Mp3StatsReset(const FM_Audio *a)
{
    (void)a;
    return FM_ERR_UNSUPPORTED;
}

#endif /* FM_USE_MP3 */

/* ------------------------------------------------------------------ */
/* Pluggable audio decoder registry                                    */
/* ------------------------------------------------------------------ */
typedef struct
{
    char codec[8];
    const FM_AudioDecoderOps *ops;
} FM_AudioDecoderEntry;

#define AUDIO_DECODER_SLOTS 4u
static FM_AudioDecoderEntry g_adec[AUDIO_DECODER_SLOTS];

FM_Result Audio_RegisterDecoder(const char *codec,
                                const FM_AudioDecoderOps *ops)
{
    uint32_t i;

    if (codec == NULL || codec[0] == '\0' || ops == NULL ||
        ops->decode == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (strlen(codec) >= 8)
    {
        return FM_ERR_TOO_LARGE;
    }
    FM_Lock();
    /* Pass 1: replace an existing entry of the same name.  Taking the
     * first free slot first would leave a stale duplicate behind whenever
     * an earlier slot had been unregistered in the meantime. */
    for (i = 0; i < AUDIO_DECODER_SLOTS; i++)
    {
        if (g_adec[i].codec[0] != '\0' &&
            strcmp(g_adec[i].codec, codec) == 0)
        {
            g_adec[i].ops = ops;
            FM_Unlock();
            return FM_OK;
        }
    }
    /* Pass 2: first free slot. */
    for (i = 0; i < AUDIO_DECODER_SLOTS; i++)
    {
        if (g_adec[i].codec[0] == '\0')
        {
            (void)strcpy(g_adec[i].codec, codec);
            g_adec[i].ops = ops;
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NO_MEMORY;
}

FM_Result Audio_UnregisterDecoder(const char *codec)
{
    uint32_t i;

    if (codec == NULL || codec[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    for (i = 0; i < AUDIO_DECODER_SLOTS; i++)
    {
        if (g_adec[i].codec[0] != '\0' &&
            strcmp(g_adec[i].codec, codec) == 0)
        {
            memset(&g_adec[i], 0, sizeof(g_adec[i]));
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NOT_FOUND;
}

/* The returned ops pointer is copied out under the lock.  Decoder tables
 * must have static storage duration (every bundled codec does). */
const FM_AudioDecoderOps *Audio_GetDecoder(const char *codec)
{
    const FM_AudioDecoderOps *ops = NULL;
    uint32_t i;

    if (codec == NULL || codec[0] == '\0')
    {
        return NULL;
    }
    FM_Lock();
    for (i = 0; i < AUDIO_DECODER_SLOTS; i++)
    {
        if (g_adec[i].codec[0] != '\0' &&
            strcmp(g_adec[i].codec, codec) == 0)
        {
            ops = g_adec[i].ops;
            break;
        }
    }
    FM_Unlock();
    return ops;
}

/* ------------------------------------------------------------------ */
/* AAC (ADTS) decoding via a registered decoder                        */
/* ------------------------------------------------------------------ */
/* Max decoded samples per AAC frame: 1024 samples/channel x 2 channels.
 * The registered decoder receives out_count, so it can never overrun. */
#ifndef FM_AAC_PCM_SAMPLES
#define FM_AAC_PCM_SAMPLES (1024u * 2u)
#endif

typedef struct
{
    FM_Bool  used;
    FM_File *file;
    const FM_AudioDecoderOps *ops;
    void    *dec;
    uint32_t channels;
    uint32_t hz;
    uint8_t  frame[4096];
    int16_t  pcm[FM_AAC_PCM_SAMPLES];
    size_t   pcm_len;
    size_t   pcm_pos;
    uint64_t total_samples;
    uint64_t first_frame;
} FM_AacStream;

static FM_AacStream g_aac[FM_MAX_AUDIO];

static uint32_t aac_freq_hz(uint32_t idx)
{
    static const uint32_t tbl[13] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };

    return (idx < 13) ? tbl[idx] : 0;
}

static FM_AacStream *aac_acquire(FM_File *f)
{
    uint32_t i;

    FM_Lock();
    for (i = 0; i < FM_MAX_AUDIO; i++)
    {
        if (!g_aac[i].used)
        {
            memset(&g_aac[i], 0, sizeof(g_aac[i]));
            g_aac[i].used = FM_TRUE;
            g_aac[i].file = f;
            FM_Unlock();
            return &g_aac[i];
        }
    }
    FM_Unlock();
    return NULL;
}

static void aac_release(FM_AacStream *s)
{
    if (s != NULL)
    {
        FM_Lock();                  /* see mp3_release() */
        s->used = FM_FALSE;
        FM_Unlock();
    }
}

/* Find the next ADTS sync word from the current position; returns its
 * offset (the file is left there) or -1.
 *
 * Read in blocks.  This used to be one File_Tell, one 2-byte File_Read
 * and one File_Seek for every byte, and it runs inside Audio_Read, on
 * the refill path of the player, whenever a file ends in something that
 * is not AAC: half a second a megabyte on a PC, far more on a card. */
static int64_t aac_find_sync(FM_AacStream *s)
{
    uint8_t buf[256];
    uint64_t base = 0;          /* file offset of buf[0] */
    size_t have = 0;

    if (File_Tell(s->file, &base) != FM_OK)
    {
        return -1;
    }
    for (;;)
    {
        uint32_t got = 0;
        size_t i;
        FM_Result r;

        r = File_Read(s->file, buf + have, (uint32_t)(sizeof(buf) - have),
                      &got);
        if (got == 0u || (r != FM_OK && r != FM_ERR_EOF))
        {
            return -1;          /* the end, or an error: no sync word */
        }
        have += got;
        for (i = 0; i + 1u < have; i++)
        {
            if (buf[i] == 0xFFu && (buf[i + 1u] & 0xF6u) == 0xF0u)
            {
                /* the caller reads from the sync word itself */
                if (File_Seek(s->file, (int64_t)(base + i),
                              FM_SEEK_SET) != FM_OK)
                {
                    return -1;
                }
                return (int64_t)(base + i);
            }
        }
        /* the last byte may be the first half of a sync word */
        base += (uint64_t)(have - 1u);
        buf[0] = buf[have - 1u];
        have = 1u;
    }
}

/* The 7 header bytes go to the front of s->frame: the decoder is given
 * the frame "with its ADTS header" (README), and used to get 7 bytes
 * nobody had written in its place -- the header was read into a local. */
static FM_Result aac_read_header(FM_AacStream *s, uint32_t *frame_len,
                                 uint32_t *hz, uint32_t *channels)
{
    uint8_t *h = s->frame;
    FM_Result r = file_read_exact(s->file, h, 7u, NULL);

    if (r != FM_OK)
    {
        return r;               /* FM_ERR_EOF at the end, else the error */
    }
    if (h[0] != 0xFF || (h[1] & 0xF6u) != 0xF0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    *frame_len = ((uint32_t)(h[3] & 3u) << 11) |
                 ((uint32_t)h[4] << 3) | ((uint32_t)h[5] >> 5);
    if (*frame_len < 7)
    {
        return FM_ERR_CORRUPTED;
    }
    *hz = aac_freq_hz((uint32_t)((h[2] >> 2) & 0x0Fu));
    *channels = ((uint32_t)(h[2] & 1u) << 2) | ((uint32_t)h[3] >> 6);
    if (*channels == 0 || *hz == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    return FM_OK;
}

static FM_Result aac_open(FM_Audio *a)
{
    FM_AacStream *s;
    const FM_AudioDecoderOps *ops = Audio_GetDecoder("AAC");
    FM_Result r;
    uint32_t frame_len;
    uint32_t hz;
    uint32_t channels;

    if (ops == NULL)
    {
        /* No AAC decoder registered: leave the track demux-only via the
         * video layer (ADTS frames), but a standalone .aac cannot be played. */
        return FM_ERR_UNSUPPORTED;
    }
    s = aac_acquire(&a->file);
    if (s == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    s->ops = ops;

    if (aac_find_sync(s) < 0)
    {
        aac_release(s);
        return FM_ERR_INVALID_FORMAT;
    }
    {
        uint64_t pos = 0;

        if (File_Tell(s->file, &pos) == FM_OK)
        {
            s->first_frame = pos;
        }
    }
    r = aac_read_header(s, &frame_len, &hz, &channels);
    if (r != FM_OK)
    {
        aac_release(s);
        return r;
    }
    s->channels = channels;
    s->hz = hz;

    a->info.sample_rate = hz;
    a->info.channels = (uint16_t)channels;
    a->info.bits_per_sample = 16;
    a->info.format = 1;
    (void)strcpy(a->info.codec, "AAC");

    if (ops->init != NULL)
    {
        r = ops->init(&s->dec, &a->info);
        if (r != FM_OK)
        {
            aac_release(s);
            return r;
        }
    }
    /* rewind so the first aac_read starts from the first frame */
    if (File_Seek(s->file, (int64_t)s->first_frame, FM_SEEK_SET) != FM_OK)
    {
        /* the decoder was created above: without this its context
         * stayed behind on every open that failed here */
        if (ops->close != NULL && s->dec != NULL)
        {
            ops->close(s->dec);
        }
        aac_release(s);
        return FM_ERR_IO;
    }
    a->codec_ctx = s;
    return FM_OK;
}

/* Decode the next frame that yields samples.
 *   FM_OK       s->pcm holds pcm_len (> 0) samples
 *   FM_ERR_EOF  the file has ended
 *   other       a read error, or what the decoder said about one frame
 *               (that frame is consumed; the next call goes on after it)
 * Bytes that are not a frame header are skipped up to the next sync
 * word.  They used to end the read with FM_ERR_EOF, and the following
 * read then went on 7 bytes later as if nothing had happened. */
static FM_Result aac_decode_one(FM_AacStream *s)
{
    const size_t cap = sizeof(s->pcm) / sizeof(s->pcm[0]);

    s->pcm_len = 0;
    s->pcm_pos = 0;
    for (;;)
    {
        uint32_t frame_len;
        uint32_t hz;
        uint32_t channels;
        uint64_t start = 0;
        size_t out_samples = 0;
        FM_Result r;

        if (File_Tell(s->file, &start) != FM_OK)
        {
            return FM_ERR_IO;
        }
        r = aac_read_header(s, &frame_len, &hz, &channels);
        if (r == FM_OK && frame_len > sizeof(s->frame))
        {
            r = FM_ERR_CORRUPTED;       /* no ADTS frame is that long */
        }
        if (r == FM_ERR_INVALID_FORMAT || r == FM_ERR_CORRUPTED)
        {
            if (File_Seek(s->file, (int64_t)start + 1, FM_SEEK_SET) != FM_OK)
            {
                return FM_ERR_IO;
            }
            if (aac_find_sync(s) < 0)
            {
                return FM_ERR_EOF;      /* nothing but junk to the end */
            }
            continue;
        }
        if (r != FM_OK)
        {
            return r;
        }
        if (hz != s->hz || channels != s->channels)
        {
            /* Either the stream changes its sample rate or channel count
             * here (two files joined), or these are stray bytes that look
             * like a header.  A real frame is followed by another header
             * of the same form; anything else is skipped like other junk.
             * A real change cannot be played: what the decoder would hand
             * out no longer fits the figures everything was set up with
             * at the open, and under those it is noise at the wrong
             * speed.  The frame is left where it is, so every further
             * read says the same. */
            uint8_t nx[4];
            uint32_t ng = 0;
            FM_Bool real = FM_FALSE;

            if (File_Seek(s->file, (int64_t)(start + frame_len),
                          FM_SEEK_SET) == FM_OK &&
                File_Read(s->file, nx, sizeof(nx), &ng) == FM_OK && ng == 4u &&
                nx[0] == 0xFFu && (nx[1] & 0xF6u) == 0xF0u &&
                aac_freq_hz((uint32_t)((nx[2] >> 2) & 0x0Fu)) == hz &&
                ((((uint32_t)(nx[2] & 1u)) << 2) | ((uint32_t)nx[3] >> 6)) == channels)
            {
                real = FM_TRUE;
            }
            if (File_Seek(s->file, (int64_t)start + (real ? 0 : 1),
                          FM_SEEK_SET) != FM_OK)
            {
                return FM_ERR_IO;
            }
            if (!real)
            {
                if (aac_find_sync(s) < 0)
                {
                    return FM_ERR_EOF;
                }
                continue;
            }
            FM_ERR_LOG("aac: the stream changes form (%lu Hz %lu ch -> %lu Hz %lu ch)",
                       (unsigned long)s->hz, (unsigned long)s->channels,
                       (unsigned long)hz, (unsigned long)channels);
            return FM_ERR_UNSUPPORTED;
        }
        r = file_read_exact(s->file, s->frame + 7, frame_len - 7u, NULL);
        if (r != FM_OK)
        {
            return r;                   /* cut-off last frame: FM_ERR_EOF */
        }
        r = s->ops->decode(s->dec, s->frame, frame_len, s->pcm, cap,
                           &out_samples);
        if (r != FM_OK)
        {
            return r;
        }
        if (out_samples > cap)
        {
            /* A decoder that reports more than it was given room for has
             * written past s->pcm -- or would make the reader copy that
             * much out of it.  Nothing of this frame is used. */
            return FM_ERR_INTERNAL;
        }
        if (out_samples != 0u)
        {
            s->pcm_len = out_samples;
            return FM_OK;
        }
        /* a frame with no output yet (decoder priming): take the next */
    }
}

static FM_Result aac_read(FM_Audio *a, int16_t *samples, size_t count,
                          size_t *read)
{
    FM_AacStream *s = (FM_AacStream *)a->codec_ctx;
    size_t total = 0;
    FM_Result err = FM_ERR_EOF;

    while (total < count)
    {
        if (s->pcm_pos >= s->pcm_len)
        {
            FM_Result r = aac_decode_one(s);

            if (r != FM_OK)
            {
                err = r;
                break;
            }
        }
        {
            size_t avail = s->pcm_len - s->pcm_pos;
            size_t n = count - total;

            if (n > avail)
            {
                n = avail;
            }
            memcpy(samples + total, s->pcm + s->pcm_pos,
                   n * sizeof(int16_t));
            s->pcm_pos += n;
            total += n;
            s->total_samples += n;
        }
    }
    if (read != NULL)
    {
        *read = total;
    }
    a->frame_index = s->total_samples / a->info.channels;
    if (total > 0 && a->callback != NULL)
    {
        a->callback(samples, total, a->callback_data);
    }
    return (total == 0) ? err : FM_OK;      /* as mp3_read() */
}

static FM_Result aac_seek(FM_Audio *a, double seconds)
{
    FM_AacStream *s = (FM_AacStream *)a->codec_ctx;
    uint64_t target = audio_time_to_frame(seconds, a->info.sample_rate);
    uint64_t target_samples = target * a->info.channels;

    if (target_samples <= s->total_samples)
    {
        s->pcm_len = 0;
        s->pcm_pos = 0;
        s->total_samples = 0;
        if (File_Seek(s->file, (int64_t)s->first_frame, FM_SEEK_SET) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    while (s->total_samples < target_samples)
    {
        FM_Result r = aac_decode_one(s);

        if (r != FM_OK)
        {
            break;
        }
        s->total_samples += s->pcm_len;
        s->pcm_pos = s->pcm_len;
    }
    if (s->total_samples > target_samples)
    {
        size_t excess = (size_t)(s->total_samples - target_samples);

        if (excess <= s->pcm_len)
        {
            s->pcm_pos = s->pcm_len - excess;
            s->total_samples = target_samples;
        }
    }
    a->frame_index = s->total_samples / a->info.channels;
    return FM_OK;
}

FM_Result Audio_Open(const char *path, FM_Audio *a)
{
    uint8_t magic[12];
    uint32_t got = 0;
    uint64_t adts_at = 0;       /* ADTS frames behind an ID3v2 tag */
    uint64_t tag_at = 0;        /* audio stream behind an ID3v2 tag */
    FM_Result r;

    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(a, FM_ERR_INVALID_ARGUMENT);
    memset(a, 0, sizeof(*a));
    File_Init(&a->file);
    r = File_OpenEx(&a->file, path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        FM_ERR_LOG("audio open %s: file open failed (%d)", path, (int)r);
        return r;
    }
    (void)File_Read(&a->file, magic, sizeof(magic), &got);
    /* An ID3v2 tag in front of ADTS frames hides the sync word from the
     * tests below: the file went to the MP3 parser, which skips the tag,
     * finds no MP3 frame behind it and calls a playable file "invalid
     * format".  Look behind the tag first. */
    while (got >= 10 && memcmp(magic, "ID3", 3) == 0)
    {
        uint64_t end = tag_at + 10u + (((uint32_t)(magic[6] & 0x7Fu) << 21) |
                              ((uint32_t)(magic[7] & 0x7Fu) << 14) |
                              ((uint32_t)(magic[8] & 0x7Fu) << 7) |
                              (uint32_t)(magic[9] & 0x7Fu));

        if (magic[5] & 0x10u)
        {
            end += 10u;                         /* the tag has a footer */
        }
        /* An ID3v2 tag is allowed in front of any stream, not only ADTS:
         * FLAC, Ogg and WAV files tagged on the card were read from byte
         * 0, none of the magic tests below matched, and the MP3 parser
         * called the track "invalid format". */
        if (File_Seek(&a->file, (int64_t)end, FM_SEEK_SET) == FM_OK &&
            File_Read(&a->file, magic, sizeof(magic), &got) == FM_OK &&
            got >= 4u)
        {
            tag_at = end;
            if (got >= 2u && magic[0] == 0xFFu && (magic[1] & 0xF6u) == 0xF0u)
            {
                adts_at = end;
            }
        }
        else
        {
            (void)File_Seek(&a->file, 0, FM_SEEK_SET);
            (void)File_Read(&a->file, magic, sizeof(magic), &got);
            tag_at = 0;
            break;
        }
    }
    r = File_Seek(&a->file, (int64_t)adts_at, FM_SEEK_SET);
    if (r == FM_OK && adts_at != 0u)
    {
        r = aac_open(a);        /* starts at the first frame it finds */
    }
    else if (r == FM_OK && got >= 4 && memcmp(magic, "RIFF", 4) == 0)
    {
        r = audio_parse_wav(a, tag_at);
    }
    else if (r == FM_OK && got >= 4 && memcmp(magic, "fLaC", 4) == 0)
    {
#if FM_USE_FLAC
        r = flac_file_open(a, tag_at);
#else
        r = FM_ERR_UNSUPPORTED;
#endif
    }
    else if (r == FM_OK && got >= 4 && memcmp(magic, "OggS", 4) == 0)
    {
        uint8_t peek[256];
        uint32_t pg = 0;
        (void)File_Seek(&a->file, (int64_t)tag_at, FM_SEEK_SET);
        (void)File_Read(&a->file, peek, sizeof(peek), &pg);
        (void)File_Seek(&a->file, 0, FM_SEEK_SET);
        int is_opus = 0;
        for (uint32_t i=0;i+8<pg;++i) if (memcmp(peek+i, "OpusHead",8)==0) { is_opus=1; break; }
        if (is_opus) {
#if FM_USE_OPUS
            r = opus_file_open(a, tag_at);
#else
            r = FM_ERR_UNSUPPORTED;
#endif
        } else {
#if FM_USE_VORBIS
            r = vorbis_file_open(a, tag_at);
#else
            r = FM_ERR_UNSUPPORTED;
#endif
        }
    }
    else if (r == FM_OK && got >= 2 &&
             magic[0] == 0xFF && (magic[1] & 0xF6u) == 0xF0u)
    {
        r = aac_open(a);
    }
#if FM_USE_MP3
    else if (r == FM_OK)
    {
        r = mp3_open(a);
    }
#else
    else
    {
        r = FM_ERR_UNSUPPORTED;
    }
#endif
    if (r != FM_OK)
    {
        FM_ERR_LOG("audio open %s: parse failed (%d)", path, (int)r);
        (void)File_Close(&a->file);
        return r;
    }
    a->frame_index = 0;
    FM_EVENT_LOG("opened %s (%lu Hz, %u ch, %s)", path,
                 (unsigned long)a->info.sample_rate,
                 (unsigned)a->info.channels, a->info.codec);
    return FM_OK;
}

FM_Result Audio_Close(FM_Audio *a)
{
    FM_Result r = FM_OK;

    if (a == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->codec_ctx != NULL)
    {
        if (a->info.format == 0x11u || a->info.format == 0x02u)
        {
            /* ADPCM block state (lazy per-stream allocation) */
            FM_Free(a->codec_ctx);
            a->codec_ctx = NULL;
        }
        else if (strcmp(a->info.codec, "AAC") == 0)
        {
            FM_AacStream *s = (FM_AacStream *)a->codec_ctx;
            if (s->used)
            {
                if (s->ops != NULL && s->dec != NULL && s->ops->close != NULL)
                {
                    s->ops->close(s->dec);
                }
                aac_release(s);
            }
        }
        else if (strcmp(a->info.codec, "FLAC") == 0 || strcmp(a->info.codec, "VORBIS") == 0 || strcmp(a->info.codec, "OPUS") == 0)
        {
            pcmbuf_free((PcmBuffer*)a->codec_ctx);
            a->codec_ctx = NULL;
        }
        else
        {
#if FM_USE_MP3
            mp3_release((FM_Mp3Stream *)a->codec_ctx);
#endif
        }
    }
    if (File_IsOpen(&a->file))
    {
        r = File_Close(&a->file);
    }
    memset(a, 0, sizeof(*a));
    return r;
}

FM_Result Audio_GetInfo(const FM_Audio *a, FM_AudioInfo *info)
{
    if (a == NULL || info == NULL || !File_IsOpen(&a->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *info = a->info;
    return FM_OK;
}

/* Aligned bounce for WAV PCM. FatFS/SDMMC DMA must not target a live
 * I2S/SAI circular buffer (STM32H7 AXI deadlock). 16-bit worst case. */
#if FM_AUDIO_PCM_CHUNK < 64u
#error "FM_AUDIO_PCM_CHUNK must be at least 64 samples"
#endif
static uint8_t g_wav_raw[FM_AUDIO_PCM_CHUNK * 2u]
    __attribute__((aligned(32)));

/* g_wav_raw is one buffer for every WAV stream.  A read fills it from
 * the file and then converts it into the caller's samples; a second task
 * reading another WAV in between replaced the bytes, and the first got
 * the other file's audio.  The lock is held from the file read to the end
 * of the conversion, one chunk at a time, and never across the caller's
 * callback.  It sits above the file-layer locks (the read happens under
 * it) and nothing takes it while holding one of those. */
#if FM_USE_THREADS
static FM_Mutex g_wav_raw_lock;
static FM_Bool  g_wav_raw_lock_init = FM_FALSE;

static void wav_raw_lock(void)
{
    FM_Bool ready;

    FM_Lock();
    if (!g_wav_raw_lock_init && FM_Mutex_Create(&g_wav_raw_lock) == FM_OK)
    {
        g_wav_raw_lock_init = FM_TRUE;
    }
    ready = g_wav_raw_lock_init;
    FM_Unlock();
    if (ready)
    {
        FM_Mutex_Lock(&g_wav_raw_lock);
    }
}

static void wav_raw_unlock(void)
{
    if (g_wav_raw_lock_init)
    {
        FM_Mutex_Unlock(&g_wav_raw_lock);
    }
}
#else
#define wav_raw_lock()   ((void)0)
#define wav_raw_unlock() ((void)0)
#endif

FM_Result Audio_Read(FM_Audio *a, int16_t *samples, size_t count,
                     size_t *read)
{
    uint64_t pos;
    uint64_t byte_pos;
    uint64_t available;
    uint32_t bytes_per_sample;
    size_t n;
    size_t done;

    if (read != NULL)
    {
        *read = 0;              /* on every path, also the failing ones */
    }
    if (a == NULL || samples == NULL || !File_IsOpen(&a->file) ||
        a->info.channels == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* Samples are delivered in whole frames.  A count that does not hold
     * one frame cannot be served at all, and that is the caller's error:
     * it used to come back as FM_ERR_EOF with a thousand frames left, and
     * a caller that stops at EOF dropped the rest of the track. */
    count -= count % a->info.channels;
    if (count == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->codec_ctx != NULL)
    {
        if (a->info.format == 0x11u || a->info.format == 0x02u)
        {
            /* ADPCM block state (codec string is "ADPCM") */
            return wav_adpcm_serve(a, samples, count, read);
        }
        if (strcmp(a->info.codec, "AAC") == 0)
        {
            return aac_read(a, samples, count, read);
        }
        if (strcmp(a->info.codec, "FLAC") == 0 || strcmp(a->info.codec, "VORBIS") == 0 || strcmp(a->info.codec, "OPUS") == 0)
        {
            return pcmbuf_read(a, samples, count, read);
        }
#if FM_USE_MP3
        return mp3_read(a, samples, count, read);
#else
        return FM_ERR_UNSUPPORTED;
#endif
    }
    if (a->info.format == 6u || a->info.format == 7u) {
        if (File_Tell(&a->file, &pos) != FM_OK || pos < a->info.data_offset) return FM_ERR_IO;
        byte_pos = pos - a->info.data_offset;
        available = (byte_pos < a->info.data_size) ? a->info.data_size - byte_pos : 0;
        n = (available > (uint64_t)count) ? count : (size_t)available;
        if (n > (size_t)FM_AUDIO_PCM_CHUNK) n = (size_t)FM_AUDIO_PCM_CHUNK;
        n -= n % a->info.channels;
        if (n == 0) return FM_ERR_EOF;
        wav_raw_lock();
        FM_Result r2 = file_read_exact(&a->file, g_wav_raw, (uint32_t)n, NULL);
        if (r2 != FM_OK) { wav_raw_unlock(); return r2; }
        for (size_t i = 0; i < n; i++) {
            uint8_t b = g_wav_raw[i];
            int16_t s = (a->info.format == 6u) ? wav_alaw_decode(b) : wav_ulaw_decode(b);
            samples[i] = s;
        }
        wav_raw_unlock();
        if (read) *read = n;
        a->frame_index += (uint64_t)(n / a->info.channels);
        if (a->callback) a->callback(samples, n, a->callback_data);
        return FM_OK;
    }
    if (a->info.format == 0x11u || a->info.format == 0x02u) {
        return wav_adpcm_serve(a, samples, count, read);
    }
    bytes_per_sample = (uint32_t)(a->info.bits_per_sample / 8u);
    if (bytes_per_sample == 0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (File_Tell(&a->file, &pos) != FM_OK || pos < a->info.data_offset)
    {
        return FM_ERR_IO;
    }
    byte_pos = pos - a->info.data_offset;
    available = (byte_pos < a->info.data_size)
                    ? a->info.data_size - byte_pos
                    : 0;
    n = (available / bytes_per_sample > (uint64_t)count)
            ? count : (size_t)(available / bytes_per_sample);
    /* Stereo PCM is consumed in whole L/R frames. Returning a half
     * frame would leave the file position (and every later read)
     * misaligned on an odd byte, so drop the trailing odd sample
     * instead (at most one sample, i.e. < 0.1 ms at 48 kHz). */
    n -= n % a->info.channels;
    if (n > UINT32_MAX / bytes_per_sample)
    {
        n = UINT32_MAX / bytes_per_sample;
    }
    FM_AUDIO_LOG("read: file pos %lu, avail %lu bytes, want %lu samples",
                 (unsigned long)(a->info.data_offset + byte_pos),
                 (unsigned long)available, (unsigned long)n);
    done = 0;
    while (done < n)
    {
        size_t chunk = n - done;
        uint32_t byte_count;
        FM_Result r;
        size_t i;

        if (chunk > (size_t)FM_AUDIO_PCM_CHUNK)
        {
            chunk = (size_t)FM_AUDIO_PCM_CHUNK;
        }
        byte_count = (uint32_t)chunk * bytes_per_sample;
        wav_raw_lock();
        r = file_read_exact(&a->file, g_wav_raw, byte_count, NULL);
        FM_AUDIO_LOG("read: chunk %lu -> %d",
                     (unsigned long)chunk, (int)r);
        if (r != FM_OK)
        {
            wav_raw_unlock();
            return r;
        }
        if (a->info.bits_per_sample == 16)
        {
            for (i = 0; i < chunk; i++)
            {
                samples[done + i] = (int16_t)rd16(g_wav_raw + i * 2u);
            }
        }
        else
        {
            for (i = 0; i < chunk; i++)
            {
                samples[done + i] =
                    (int16_t)(((int32_t)g_wav_raw[i] - 128) * 256);
            }
        }
        wav_raw_unlock();
        done += chunk;
    }
    if (read != NULL)
    {
        *read = n;
    }
    a->frame_index += (uint64_t)(n / a->info.channels);
    if (n > 0 && a->callback != NULL)
    {
        a->callback(samples, n, a->callback_data);
    }
    return (n == 0) ? FM_ERR_EOF : FM_OK;
}

FM_Result Audio_ReadFrame(FM_Audio *a, int16_t *frame,
                          size_t *channels_read)
{
    size_t n = 0;
    FM_Result r;

    if (a == NULL || frame == NULL || channels_read == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Audio_Read(a, frame, a->info.channels, &n);
    *channels_read = n;
    return r;
}

FM_Result Audio_Seek(FM_Audio *a, double seconds)
{
    uint64_t frame;
    uint64_t frame_bytes;
    uint64_t offset;

    /* !(x >= 0) is true for a negative time and for NaN, which no
     * comparison catches: NaN went through "seconds < 0.0" and became
     * frame 0x8000000000000020 in the formats that do not range-check. */
    if (a == NULL || !(seconds >= 0.0) || a->info.sample_rate == 0 ||
        a->info.channels == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->info.format == 0x11u || a->info.format == 0x02u)
    {
        /* ADPCM: seek must land on a block boundary (the decoder is
         * stateless per block).  Granularity = one block. */
        uint16_t ba = wav_adpcm_block_align_of(a);
        size_t spb = wav_adpcm_frames_of(a, ba);
        uint64_t blk;
        uint64_t target;

        if (spb == 0u)
        {
            return FM_ERR_CORRUPTED;
        }
        blk = audio_time_to_frame(seconds, a->info.sample_rate) / spb;
        if (blk > a->info.data_size / ba)
        {
            /* past the end: the end, so the next read reports EOF */
            target = a->info.data_offset + a->info.data_size;
            blk = a->info.data_size / ba;
        }
        else
        {
            target = a->info.data_offset + (uint64_t)ba * blk;
        }
        if (File_Seek(&a->file, (int64_t)target, FM_SEEK_SET) != FM_OK)
        {
            return FM_ERR_IO;
        }
        wav_adpcm_reset(a);                     /* drop the carry */
        a->frame_index = (uint64_t)spb * blk;
        return FM_OK;
    }
    if (a->codec_ctx != NULL)
    {
        if (strcmp(a->info.codec, "AAC") == 0)
        {
            return aac_seek(a, seconds);
        }
        if (strcmp(a->info.codec, "FLAC") == 0 || strcmp(a->info.codec, "VORBIS") == 0 || strcmp(a->info.codec, "OPUS") == 0)
        {
            return pcmbuf_seek(a, seconds);
        }
#if FM_USE_MP3
        return mp3_seek(a, seconds);
#else
        return FM_ERR_UNSUPPORTED;
#endif
    }
    frame = audio_time_to_frame(seconds, a->info.sample_rate);
    frame_bytes = (uint64_t)a->info.channels *
                  (uint64_t)((a->info.format == 6u || a->info.format == 7u)
                                 ? 1u       /* G.711: one byte a sample,
                                             * reported as 16-bit output */
                                 : a->info.bits_per_sample / 8u);
    if (frame_bytes == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    offset = frame * frame_bytes;           /* frame <= 2^40: no overflow */
    if (offset > a->info.data_size)
    {
        offset = a->info.data_size;
        frame = offset / frame_bytes;
    }
    if (File_Seek(&a->file, (int64_t)(a->info.data_offset + offset),
                  FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    a->frame_index = frame;
    return FM_OK;
}

FM_Result Audio_ReadMono(FM_Audio *a, int16_t *samples, size_t count,
                         size_t *read)
{
    size_t total = 0;
    FM_Result err = FM_ERR_EOF;

    if (read != NULL)
    {
        *read = 0;
    }
    if (a == NULL || samples == NULL || !File_IsOpen(&a->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (a->info.channels == 1)
    {
        return Audio_Read(a, samples, count, read);
    }
    if (a->info.channels != 2)
    {
        /* Only L/R is averaged here.  A 5.1 stream went through the
         * pair loop below as if it were stereo and came out three times
         * as long: interleaved channels, not a downmix.
         * (Audio_ReadStereo refuses it the same way.) */
        return FM_ERR_UNSUPPORTED;
    }
    while (total < count)
    {
        int16_t pair[256];
        size_t want = count - total;
        size_t n = want * 2u;
        size_t got = 0;
        size_t pairs;
        size_t i;
        FM_Result r;

        if (n > sizeof(pair) / sizeof(pair[0]))
        {
            n = sizeof(pair) / sizeof(pair[0]);
            n -= n & 1u;
        }
        if (n < 2)
        {
            break;
        }
        r = Audio_Read(a, pair, n, &got);
        pairs = got / 2u;
        for (i = 0; i < pairs; i++)
        {
            int32_t sum = (int32_t)pair[i * 2u] + (int32_t)pair[i * 2u + 1u];

            samples[total + i] = (int16_t)(sum / 2);
        }
        total += pairs;
        if (r != FM_OK && r != FM_ERR_EOF)
        {
            err = r;                    /* a read error is not the end */
            break;
        }
        if (got < n || r == FM_ERR_EOF)
        {
            break;
        }
    }
    if (read != NULL)
    {
        *read = total;
    }
    return (total == 0) ? err : FM_OK;
}

FM_Result Audio_ReadStereo(FM_Audio *a, int16_t *samples,
                           size_t frame_count, size_t *frames_read)
{
    size_t total = 0;

    if (a == NULL || samples == NULL || !File_IsOpen(&a->file) ||
        frame_count == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (frames_read != NULL)
    {
        *frames_read = 0;
    }

    if (a->info.channels == 2)
    {
        if (frame_count > SIZE_MAX / 2u)
        {
            return FM_ERR_TOO_LARGE;
        }
        while (total < frame_count)
        {
            size_t got = 0;
            size_t want = (frame_count - total) * 2u;
            FM_Result r = Audio_Read(a, samples + total * 2u, want, &got);

            if ((got & 1u) != 0)
            {
                total += got / 2u;
                if (frames_read != NULL) *frames_read = total;
                return FM_ERR_INVALID_FORMAT;
            }
            total += got / 2u;
            if (r != FM_OK && r != FM_ERR_EOF)
            {
                if (frames_read != NULL) *frames_read = total;
                return r;
            }
            if (r == FM_ERR_EOF || got == 0)
            {
                break;
            }
        }
    }
    else if (a->info.channels == 1)
    {
        while (total < frame_count)
        {
            int16_t mono[128];
            size_t want = frame_count - total;
            size_t got = 0;
            size_t i;
            FM_Result r;

            if (want > sizeof(mono) / sizeof(mono[0]))
            {
                want = sizeof(mono) / sizeof(mono[0]);
            }
            r = Audio_Read(a, mono, want, &got);
            for (i = 0; i < got; i++)
            {
                samples[(total + i) * 2u] = mono[i];
                samples[(total + i) * 2u + 1u] = mono[i];
            }
            total += got;
            if (r != FM_OK && r != FM_ERR_EOF)
            {
                if (frames_read != NULL) *frames_read = total;
                return r;
            }
            if (r == FM_ERR_EOF || got == 0)
            {
                break;
            }
        }
    }
    else
    {
        return FM_ERR_UNSUPPORTED;
    }

    if (frames_read != NULL)
    {
        *frames_read = total;
    }
    return (total == 0) ? FM_ERR_EOF : FM_OK;
}

FM_Result Audio_Stream(FM_Audio *a, FM_AudioCallback cb, void *user_data,
                       size_t chunk_samples)
{
    int16_t buffer[FM_INTERNAL_BUFFER_SIZE / sizeof(int16_t)];
    size_t capacity = sizeof(buffer) / sizeof(buffer[0]);

    if (a == NULL || cb == NULL || chunk_samples == 0 || capacity == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (chunk_samples < capacity)
    {
        capacity = chunk_samples;
    }
    /* A chunk smaller than one frame can deliver nothing: that returned
     * FM_OK with the whole track unplayed (see Audio_Read). */
    if (a->info.channels == 0u || capacity < a->info.channels)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (;;)
    {
        size_t n = 0;
        FM_Result r = Audio_Read(a, buffer, capacity, &n);

        if (n > 0)
        {
            cb(buffer, n, user_data);
        }
        if (r == FM_ERR_EOF)
        {
            return FM_OK;
        }
        if (r != FM_OK)
        {
            return r;
        }
    }
}

FM_Result Audio_SetCallback(FM_Audio *a, FM_AudioCallback cb,
                            void *user_data)
{
    if (a == NULL || !File_IsOpen(&a->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    a->callback = cb;
    a->callback_data = user_data;
    return FM_OK;
}

static FM_Result audio_write_header(FM_File *f, uint32_t sample_rate,
                                     uint16_t channels, uint16_t bits,
                                     uint32_t data_size)
{
    /* تراز DMA: h روی پشته ۱‌بایتی تراز است؛ دیسک‌ایو SDMMC روی H7 با
     * بافر غیرتراز خطای FR_DISK_ERR (‎−6 I/O) می‌دهد — همین باعث شکست
     * RecStart روی برد بود (روی هاست PosixFS دیده نمی‌شد). */
    FM_DMA_ALIGNED uint8_t h[44];
    uint32_t byte_rate = sample_rate * (uint32_t)channels * (bits / 8u);
    uint16_t block_align = (uint16_t)(channels * (bits / 8u));

    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    wr32(h + 4, 36u + data_size);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, 1);
    wr16(h + 22, channels);
    wr32(h + 24, sample_rate);
    wr32(h + 28, byte_rate);
    wr16(h + 32, block_align);
    wr16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    wr32(h + 40, data_size);
    return file_write_exact(f, h, sizeof(h));
}

FM_Result AudioWriter_Create(const char *path, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w)
{
    FM_Result r;

    if (path == NULL || w == NULL || sample_rate == 0 ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (sample_rate > UINT32_MAX /
        ((uint32_t)channels * (bits_per_sample / 8u)))
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(w, 0, sizeof(*w));
    File_Init(&w->file);
    r = File_OpenEx(&w->file, path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        return r;
    }
    w->info.sample_rate = sample_rate;
    w->info.channels = channels;
    w->info.bits_per_sample = bits_per_sample;
    w->info.format = 1;
    (void)strcpy(w->info.codec, "WAV");
    w->info.data_offset = 44;
    r = audio_write_header(&w->file, sample_rate, channels, bits_per_sample, 0);
    if (r != FM_OK)
    {
        (void)File_Close(&w->file);
    }
    return r;
}

FM_Result AudioWriter_Write(FM_AudioWriter *w, const int16_t *samples,
                            size_t count, size_t *written)
{
    uint32_t bytes_per_sample;
    size_t i = 0;

    if (w == NULL || samples == NULL || !File_IsOpen(&w->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (written != NULL)
    {
        *written = 0;
    }
    bytes_per_sample = w->info.bits_per_sample / 8u;
    if (count > (SIZE_MAX - w->data_bytes) / bytes_per_sample)
    {
        return FM_ERR_TOO_LARGE;
    }
    while (i < count)
    {
        /* تراز DMA: مثل هدر WAV، بافر دادهی PCM روی پشته ۱-بایتی‌تراز است
         * و درایور SDMMC روی H7 با بافر غیرتراز خطای I/O می‌دهد. */
        FM_DMA_ALIGNED uint8_t buffer[FM_INTERNAL_BUFFER_SIZE];
        size_t cap = sizeof(buffer) / bytes_per_sample;
        size_t n = count - i;
        size_t j;

        if (n > cap)
        {
            n = cap;
        }
        for (j = 0; j < n; j++)
        {
            int16_t sample = samples[i + j];

            if (bytes_per_sample == 2)
            {
                wr16(buffer + j * 2u, (uint16_t)sample);
            }
            else
            {
                int32_t value = ((int32_t)sample / 256) + 128;

                if (value < 0) value = 0;
                if (value > 255) value = 255;
                buffer[j] = (uint8_t)value;
            }
        }
        if (file_write_exact(&w->file, buffer,
                             (uint32_t)(n * bytes_per_sample)) != FM_OK)
        {
            return FM_ERR_IO;
        }
        i += n;
        w->data_bytes += n * bytes_per_sample;
    }
    if (written != NULL)
    {
        *written = count;
    }
    return FM_OK;
}

FM_Result AudioWriter_Close(FM_AudioWriter *w)
{
    FM_Result r = FM_OK;

    if (w == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!File_IsOpen(&w->file))
    {
        return FM_ERR_NOT_OPEN;
    }
    /* the RIFF size field is the data plus 36 bytes of header, in 32 bits */
    if (w->data_bytes > (uint64_t)UINT32_MAX - 36u)
    {
        (void)File_Close(&w->file);
        return FM_ERR_TOO_LARGE;
    }
    if (File_Seek(&w->file, 4, FM_SEEK_SET) != FM_OK)
    {
        r = FM_ERR_IO;
    }
    else
    {
        uint8_t size[4];

        wr32(size, 36u + (uint32_t)w->data_bytes);
        r = file_write_exact(&w->file, size, sizeof(size));
        if (r == FM_OK && File_Seek(&w->file, 40, FM_SEEK_SET) != FM_OK)
        {
            r = FM_ERR_IO;
        }
        if (r == FM_OK)
        {
            wr32(size, (uint32_t)w->data_bytes);
            r = file_write_exact(&w->file, size, sizeof(size));
        }
    }
    if (r == FM_OK)
    {
        r = File_Flush(&w->file);
    }
    {
        FM_Result c = File_Close(&w->file);

        if (r == FM_OK)
        {
            r = c;
        }
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Recorder                                                            */
/* ------------------------------------------------------------------ */
FM_Result AudioRecorder_Open(const char *folder, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w, char *path_out,
                             size_t path_out_size)
{
    static uint32_t g_counter;
    char path[FM_PATH_MAX];
    uint32_t tries;

    if (folder == NULL || w == NULL || path_out == NULL || path_out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (sample_rate == 0 || (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        /* Will the name fit the caller's buffer?  Its length does not
         * depend on the number, so this is known before anything is
         * created.  It used to be found out after the file existed: the
         * call failed and left an empty rec_NNNNNN.wav, one per try. */
        size_t flen = strlen(folder);

        while (flen > 1 && folder[flen - 1] == '/')
        {
            flen--;
        }
        if (flen + 1 + 4 + 6 + 4 + 1 > path_out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
    }
    {
        FM_Result dr = Directory_Create(folder);

        if (dr != FM_OK && dr != FM_ERR_EXISTS)
        {
            return dr;
        }
    }

    for (tries = 0; tries < 10000; tries++)
    {
        FM_Bool exists = FM_FALSE;
        size_t flen = strlen(folder);
        size_t off;
        uint32_t n;
        FM_Result r;

        g_counter++;
        if (g_counter > 999999)
        {
            g_counter = 1;
        }
        /* strip trailing slashes */
        while (flen > 1 && folder[flen - 1] == '/')
        {
            flen--;
        }
        if (flen + 1 + 4 + 6 + 4 + 1 >= sizeof(path))
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(path, folder, flen);
        off = flen;
        path[off++] = '/';
        memcpy(path + off, "rec_", 4); off += 4;
        /* 6 decimal digits */
        for (n = 100000; n > 0; n /= 10)
        {
            path[off++] = (char)('0' + (g_counter / n) % 10);
        }
        memcpy(path + off, ".wav", 4); off += 4;
        path[off] = '\0';

        if (File_Exists(path, &exists) == FM_OK && exists)
        {
            continue;
        }
        r = AudioWriter_Create(path, sample_rate, channels, bits_per_sample, w);
        if (r == FM_OK)
        {
            size_t plen = strlen(path);

            if (plen + 1 > path_out_size)
            {
                (void)AudioWriter_Close(w);
                return FM_ERR_TOO_LARGE;
            }
            memcpy(path_out, path, plen + 1);
        }
        return r;
    }
    return FM_ERR_EXISTS;
}

#else /* FM_USE_AUDIO == 0 */

/* Audio is compiled out.  As with video and images, the public API still
 * links, so application code that calls it (the board examples, the
 * music player) does not need #if around every call: each entry point
 * answers FM_ERR_UNSUPPORTED.  The stubs are stateless: no decoder, no
 * buffers, 0 bytes of .data/.bss. */
FM_Result Audio_Open(const char *path, FM_Audio *a)
{
    (void)path; (void)a;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Close(FM_Audio *a)
{
    (void)a;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_GetInfo(const FM_Audio *a, FM_AudioInfo *info)
{
    (void)a; (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Read(FM_Audio *a, int16_t *samples, size_t count,
                     size_t *read)
{
    (void)a; (void)samples; (void)count;
    if (read != NULL)
    {
        *read = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_ReadFrame(FM_Audio *a, int16_t *frame, size_t *channels_read)
{
    (void)a; (void)frame;
    if (channels_read != NULL)
    {
        *channels_read = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_ReadMono(FM_Audio *a, int16_t *samples, size_t count,
                         size_t *read)
{
    (void)a; (void)samples; (void)count;
    if (read != NULL)
    {
        *read = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_ReadStereo(FM_Audio *a, int16_t *samples,
                           size_t frame_count, size_t *frames_read)
{
    (void)a; (void)samples; (void)frame_count;
    if (frames_read != NULL)
    {
        *frames_read = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Seek(FM_Audio *a, double seconds)
{
    (void)a; (void)seconds;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Mp3Stats(const FM_Audio *a, FM_Mp3Stats *stats)
{
    (void)a; (void)stats;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Mp3StatsReset(const FM_Audio *a)
{
    (void)a;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_Stream(FM_Audio *a, FM_AudioCallback cb, void *user_data,
                       size_t chunk_samples)
{
    (void)a; (void)cb; (void)user_data; (void)chunk_samples;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_SetCallback(FM_Audio *a, FM_AudioCallback cb,
                            void *user_data)
{
    (void)a; (void)cb; (void)user_data;
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioWriter_Create(const char *path, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w)
{
    (void)path; (void)sample_rate; (void)channels; (void)bits_per_sample;
    (void)w;
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioWriter_Write(FM_AudioWriter *w, const int16_t *samples,
                            size_t count, size_t *written)
{
    (void)w; (void)samples; (void)count;
    if (written != NULL)
    {
        *written = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioWriter_Close(FM_AudioWriter *w)
{
    (void)w;
    return FM_ERR_UNSUPPORTED;
}

FM_Result AudioRecorder_Open(const char *folder, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w, char *path_out,
                             size_t path_out_size)
{
    (void)folder; (void)sample_rate; (void)channels; (void)bits_per_sample;
    (void)w; (void)path_out; (void)path_out_size;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_RegisterDecoder(const char *codec,
                                const FM_AudioDecoderOps *ops)
{
    (void)codec;
    (void)ops;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Audio_UnregisterDecoder(const char *codec)
{
    (void)codec;
    return FM_ERR_UNSUPPORTED;
}

const FM_AudioDecoderOps *Audio_GetDecoder(const char *codec)
{
    (void)codec;
    return NULL;
}

#endif /* FM_USE_AUDIO */
