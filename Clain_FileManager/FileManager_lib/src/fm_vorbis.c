/*
 * FileManager - Vorbis decoder via stb_vorbis (single file)
 * FM_USE_VORBIS must be 1. Auto-registers via FM_Vorbis_Init().
 */
#define FM_DEBUG_TAG "vorbis"
#include "fm_debug.h"
#include "fm_audio.h"
#include "fm_vorbis.h"
#include "fm_alloc.h"
#include <string.h>
#include <limits.h>

#if FM_USE_AUDIO && FM_USE_VORBIS

/* stb_vorbis is a single-file library: the implementation normally gets
 * compiled wherever the .c is included with STB_VORBIS_IMPLEMENTATION.
 * Here the *only* implementation translation unit is the vendored
 * third_party/stb/stb_vorbis.c itself, which build systems compile as a
 * standalone object (STM32CubeIDE compiles every .c under the project;
 * the host Makefile and CMake list it in OBJS too).  Including it here
 * in header-only mode gives us the public API declarations without any
 * definitions -- compiling the implementation in both places yields
 * "multiple definition of `stb_vorbis_*'" link errors. */
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb/stb_vorbis.c"

typedef struct {
    FM_AudioInfo info;
} VorbisCtx;

static FM_Result fm_vorbis_do_init(void **ctx, const FM_AudioInfo *info)
{
    VorbisCtx *c;
    if (ctx == NULL || info == NULL) return FM_ERR_INVALID_ARGUMENT;
    c = (VorbisCtx *)FM_Alloc(sizeof(VorbisCtx));
    if (c == NULL) return FM_ERR_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->info = *info;
    *ctx = c;
    return FM_OK;
}

static FM_Result fm_vorbis_do_decode(void *ctx, const uint8_t *in, size_t in_size,
                               int16_t *out, size_t out_count, size_t *out_samples)
{
    VorbisCtx *c = (VorbisCtx *)ctx;
    int error = 0;
    stb_vorbis *v = NULL;
    int channels = 0, sample_rate = 0;
    int frames = 0;
    (void)c;

    if (out == NULL || out_samples == NULL) return FM_ERR_INVALID_ARGUMENT;
    *out_samples = 0;
    if (in == NULL || in_size < 32u) return FM_ERR_CORRUPTED;
    if (out_count == 0u) return FM_ERR_TOO_LARGE;

    v = stb_vorbis_open_memory(in, (int)in_size, &error, NULL);
    if (v == NULL) return FM_ERR_CORRUPTED;

    {
        stb_vorbis_info info = stb_vorbis_get_info(v);
        channels = info.channels;
        sample_rate = (int)info.sample_rate;
        (void)sample_rate;
        if (channels <= 0 || channels > 2) {
            stb_vorbis_close(v);
            return FM_ERR_UNSUPPORTED;
        }
        /* stb_vorbis takes the capacity in SHORTS (interleaved samples)
         * and divides by `channels` itself; passing a frame count here
         * halved every stereo decode and made a 1-frame stereo request
         * report a false end-of-stream. */
        size_t max_frames = out_count / (size_t)channels;
        int num_shorts;
        if (max_frames == 0u) {
            stb_vorbis_close(v);
            return FM_ERR_TOO_LARGE;
        }
        if (max_frames > (size_t)(INT_MAX / channels)) {
            max_frames = (size_t)(INT_MAX / channels);
        }
        num_shorts = (int)(max_frames * (size_t)channels);
        frames = stb_vorbis_get_samples_short_interleaved(v, channels, out, num_shorts);
        if (frames < 0) frames = 0;
        *out_samples = (size_t)frames * (size_t)channels;
        stb_vorbis_close(v);
        if (*out_samples == 0) return FM_ERR_EOF;
        return FM_OK;
    }
}

static void fm_vorbis_do_close(void *ctx)
{
    if (ctx != NULL) FM_Free(ctx);
}

static const FM_AudioDecoderOps g_vorbis_ops = {
    "VORBIS",
    fm_vorbis_do_init,
    fm_vorbis_do_decode,
    fm_vorbis_do_close
};

/* FileManager patch in third_party/stb/stb_vorbis.c */
extern void stb_vorbis_fm_init(void);

FM_Result FM_Vorbis_Init(void)
{
    /* build stb_vorbis' CRC table now, single threaded (see the patch) */
    stb_vorbis_fm_init();
    return Audio_RegisterDecoder("VORBIS", &g_vorbis_ops);
}

const FM_AudioDecoderOps *FM_Vorbis_GetOps(void)
{
    return &g_vorbis_ops;
}

/* ------------------------------------------------------------------ */
/* Packet-stream decoding (see fm_vorbis.h)                            */
/* ------------------------------------------------------------------ */
/*
 * stb_vorbis has no entry point for bare packets; its "push" interface
 * takes the bytes of an Ogg stream.  So every packet is wrapped in an
 * Ogg page of its own here (27-byte header, the segment table, a CRC)
 * and pushed.  The decoder object, and with it the overlap from the
 * packet before, lives from open to close.
 */
struct FM_VorbisStream
{
    stb_vorbis *dec;
    uint32_t    channels;
    uint32_t    page_seq;
    uint8_t    *page;           /* bytes for the decoder: what it has
                                 * not taken yet (`carry` bytes), then
                                 * the new packet framed as Ogg page  */
    size_t      page_cap;
    size_t      carry;
    float     **pend;           /* decoded, not fetched yet (owned by
                                 * the decoder, valid until the next
                                 * packet is pushed)                  */
    uint32_t    pend_frames;
    uint32_t    pend_pos;
};

/* More than this left over from earlier packets means the decoder is not
 * taking what it is given: start over rather than let it grow. */
#define VS_CARRY_MAX   (128u * 1024u)

/* Ogg page checksum: CRC-32, polynomial 0x04C11DB7, no reflection.  Bit
 * by bit: a few thousand shifts per packet next to a Vorbis decode, and
 * no table to build or to share between tasks. */
static uint32_t vs_crc(const uint8_t *d, size_t n)
{
    uint32_t crc = 0u;
    size_t i;
    int b;

    for (i = 0u; i < n; i++)
    {
        crc ^= (uint32_t)d[i] << 24;
        for (b = 0; b < 8; b++)
        {
            crc = ((crc & 0x80000000u) != 0u) ? (crc << 1) ^ 0x04C11DB7u
                                              : (crc << 1);
        }
    }
    return crc;
}

/* Bytes one packet needs as Ogg pages. */
static size_t vs_paged_size(uint32_t len)
{
    size_t segs = (size_t)len / 255u + 1u;      /* the last one is < 255 */
    size_t pages = (segs + 254u) / 255u;

    return pages * 27u + segs + (size_t)len;
}

/* Frame one packet.  `first_flags`: 0x02 for the very first page of the
 * stream.  `audio`: the page carries no position (granule -1), so the
 * decoder never trims its output to one. */
static size_t vs_put_packet(FM_VorbisStream *s, uint8_t *out,
                            const uint8_t *pkt, uint32_t len,
                            uint8_t first_flags, FM_Bool audio)
{
    size_t segs = (size_t)len / 255u + 1u;
    size_t seg = 0u;
    size_t done = 0u;
    size_t pos = 0u;

    while (seg < segs)
    {
        size_t n = (segs - seg > 255u) ? 255u : segs - seg;
        size_t start = pos;
        size_t payload = 0u;
        size_t i;
        uint32_t crc;

        memcpy(out + pos, "OggS", 4);
        out[pos + 4u] = 0u;
        out[pos + 5u] = (uint8_t)((seg == 0u) ? first_flags : 0x01u);
        memset(out + pos + 6u, audio ? 0xFF : 0x00, 8);
        out[pos + 14u] = 1u;                    /* stream serial 1 */
        out[pos + 15u] = 0u;
        out[pos + 16u] = 0u;
        out[pos + 17u] = 0u;
        out[pos + 18u] = (uint8_t)(s->page_seq & 0xFFu);
        out[pos + 19u] = (uint8_t)((s->page_seq >> 8) & 0xFFu);
        out[pos + 20u] = (uint8_t)((s->page_seq >> 16) & 0xFFu);
        out[pos + 21u] = (uint8_t)((s->page_seq >> 24) & 0xFFu);
        memset(out + pos + 22u, 0, 4);          /* CRC, filled below */
        out[pos + 26u] = (uint8_t)n;
        pos += 27u;
        for (i = 0u; i < n; i++)
        {
            size_t left = (size_t)len - done - payload;
            uint8_t lace = (left >= 255u) ? 255u : (uint8_t)left;

            out[pos++] = lace;
            payload += lace;
        }
        if (payload != 0u)
        {
            memcpy(out + pos, pkt + done, payload);
        }
        pos += payload;
        done += payload;
        crc = vs_crc(out + start, pos - start);
        out[start + 22u] = (uint8_t)(crc & 0xFFu);
        out[start + 23u] = (uint8_t)((crc >> 8) & 0xFFu);
        out[start + 24u] = (uint8_t)((crc >> 16) & 0xFFu);
        out[start + 25u] = (uint8_t)((crc >> 24) & 0xFFu);
        s->page_seq++;
        seg += n;
    }
    return pos;
}

/* Room for `need` bytes; the first `carry` bytes are kept. */
static uint8_t *vs_page_buf(FM_VorbisStream *s, size_t need)
{
    if (need > s->page_cap)
    {
        uint8_t *nb = (uint8_t *)FM_Alloc(need);

        if (nb == NULL)
        {
            return NULL;
        }
        if (s->page != NULL)
        {
            if (s->carry != 0u)
            {
                memcpy(nb, s->page, s->carry);
            }
            FM_Free(s->page);
        }
        s->page = nb;
        s->page_cap = need;
    }
    return s->page;
}

/* Hand out pending PCM: float [-1, 1) -> 16-bit little-endian. */
static size_t vs_fetch(FM_VorbisStream *s, uint8_t *pcm, size_t cap)
{
    size_t room = cap / (2u * (size_t)s->channels);
    size_t n = (size_t)(s->pend_frames - s->pend_pos);
    size_t i;
    uint32_t c;

    if (n > room)
    {
        n = room;
    }
    for (i = 0u; i < n; i++)
    {
        for (c = 0u; c < s->channels; c++)
        {
            float f = s->pend[c][s->pend_pos + i];
            int32_t v;

            if (f != f)              { v = 0; }           /* NaN */
            else if (f >= 1.0f)      { v = 32767; }
            else if (f <= -1.0f)     { v = -32768; }
            else                     { v = (int32_t)(f * 32768.0f); }
            pcm[0] = (uint8_t)((uint32_t)v & 0xFFu);
            pcm[1] = (uint8_t)(((uint32_t)v >> 8) & 0xFFu);
            pcm += 2;
        }
    }
    s->pend_pos += (uint32_t)n;
    if (s->pend_pos >= s->pend_frames)
    {
        s->pend = NULL;
        s->pend_frames = 0u;
        s->pend_pos = 0u;
    }
    return n * 2u * (size_t)s->channels;
}

FM_Result FM_Vorbis_StreamOpen(const uint8_t *const hdr[3],
                               const uint32_t hdr_len[3],
                               FM_VorbisStream **out)
{
    FM_VorbisStream *s;
    stb_vorbis_info info;
    size_t need;
    size_t pos;
    uint8_t *buf;
    int used = 0;
    int error = 0;

    if (out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (hdr == NULL || hdr_len == NULL || hdr[0] == NULL || hdr[1] == NULL ||
        hdr[2] == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* The identification header is exactly 30 bytes; stb_vorbis insists
     * on finding it alone on the first page. */
    if (hdr_len[0] != 30u || hdr_len[1] > 0x01000000u ||
        hdr_len[2] > 0x01000000u)
    {
        return FM_ERR_CORRUPTED;
    }
    s = (FM_VorbisStream *)FM_Alloc(sizeof(*s));
    if (s == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    memset(s, 0, sizeof(*s));
    need = vs_paged_size(hdr_len[0]) + vs_paged_size(hdr_len[1]) +
           vs_paged_size(hdr_len[2]);
    buf = vs_page_buf(s, need);
    if (buf == NULL)
    {
        FM_Free(s);
        return FM_ERR_NO_MEMORY;
    }
    pos = vs_put_packet(s, buf, hdr[0], hdr_len[0], 0x02u, FM_FALSE);
    pos += vs_put_packet(s, buf + pos, hdr[1], hdr_len[1], 0x00u, FM_FALSE);
    pos += vs_put_packet(s, buf + pos, hdr[2], hdr_len[2], 0x00u, FM_FALSE);

    stb_vorbis_fm_init();
    s->dec = stb_vorbis_open_pushdata(buf, (int)pos, &used, &error, NULL);
    if (s->dec == NULL)
    {
        FM_Result r = (error == VORBIS_outofmem) ? FM_ERR_NO_MEMORY
                                                 : FM_ERR_CORRUPTED;

        FM_ERR_LOG("stream headers refused (stb_vorbis error %d)", error);
        FM_Free(s->page);
        FM_Free(s);
        return r;
    }
    info = stb_vorbis_get_info(s->dec);
    if (info.channels < 1 || info.channels > 2)
    {
        FM_ERR_LOG("stream has %d channels: mono and stereo only",
                   info.channels);
        FM_Vorbis_StreamClose(s);
        return FM_ERR_UNSUPPORTED;
    }
    s->channels = (uint32_t)info.channels;
    *out = s;
    return FM_OK;
}

FM_Result FM_Vorbis_StreamDecode(FM_VorbisStream *s, const uint8_t *packet,
                                 uint32_t len, uint8_t *pcm, size_t cap,
                                 size_t *written)
{
    float **outp = NULL;
    int channels = 0;
    int samples = 0;
    size_t total;
    size_t off = 0u;
    FM_Bool refused = FM_FALSE;
    uint8_t *buf;

    if (written == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *written = 0u;
    if (s == NULL || pcm == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (cap < 2u * (size_t)s->channels)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (packet == NULL)
    {
        if (s->pend_frames != 0u)
        {
            *written = vs_fetch(s, pcm, cap);
        }
        return FM_OK;
    }
    if (len == 0u || len > 0x01000000u)
    {
        return FM_ERR_CORRUPTED;
    }
    /* pushing a packet overwrites what the last one left to fetch */
    s->pend = NULL;
    s->pend_frames = 0u;
    s->pend_pos = 0u;

    /* The decoder says how many bytes it took.  Decoding normally that
     * is the whole page.  While it is finding its place again (after a
     * reset or a damaged packet) it holds back the last bytes of what
     * it was given and wants them again in front of the next data: they
     * are carried over here. */
    if (s->carry > VS_CARRY_MAX)
    {
        s->carry = 0u;
        stb_vorbis_flush_pushdata(s->dec);
    }
    total = s->carry + vs_paged_size(len);
    buf = vs_page_buf(s, total);
    if (buf == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    (void)vs_put_packet(s, buf + s->carry, packet, len, 0x00u, FM_TRUE);

    while (off < total)
    {
        int used;
        int error;

        samples = 0;
        outp = NULL;
        used = stb_vorbis_decode_frame_pushdata(s->dec, buf + off,
                                                (int)(total - off),
                                                &channels, &outp, &samples);
        error = stb_vorbis_get_error(s->dec);
        if (error != VORBIS__no_error && error != VORBIS_need_more_data)
        {
            refused = FM_TRUE;
        }
        if (used <= 0 || (size_t)used > total - off)
        {
            break;                       /* it wants more than it has */
        }
        off += (size_t)used;
        if (samples > 0)
        {
            break;                       /* output is valid until the next
                                          * decode call: hand it out first */
        }
    }
    s->carry = total - off;
    if (s->carry != 0u && off != 0u)
    {
        memmove(buf, buf + off, s->carry);
    }
    if (samples > 0 && outp != NULL && (uint32_t)channels == s->channels)
    {
        s->pend = outp;
        s->pend_frames = (uint32_t)samples;
        s->pend_pos = 0u;
        *written = vs_fetch(s, pcm, cap);
        return FM_OK;
    }
    /* Nothing came out.  Either the decoder refused the packet (it finds
     * its place again by itself with the next ones), or this is the
     * silent start of a stream or of a resync. */
    return refused ? FM_ERR_CORRUPTED : FM_OK;
}

size_t FM_Vorbis_StreamPending(const FM_VorbisStream *s)
{
    if (s == NULL)
    {
        return 0u;
    }
    return (size_t)(s->pend_frames - s->pend_pos) * 2u * (size_t)s->channels;
}

uint32_t FM_Vorbis_StreamChannels(const FM_VorbisStream *s)
{
    return (s != NULL) ? s->channels : 0u;
}

void FM_Vorbis_StreamReset(FM_VorbisStream *s)
{
    if (s != NULL && s->dec != NULL)
    {
        stb_vorbis_flush_pushdata(s->dec);
        s->carry = 0u;
        s->pend = NULL;
        s->pend_frames = 0u;
        s->pend_pos = 0u;
    }
}

void FM_Vorbis_StreamClose(FM_VorbisStream *s)
{
    if (s != NULL)
    {
        if (s->dec != NULL)
        {
            stb_vorbis_close(s->dec);
        }
        if (s->page != NULL)
        {
            FM_Free(s->page);
        }
        FM_Free(s);
    }
}

#else
FM_Result FM_Vorbis_Init(void) { return FM_ERR_UNSUPPORTED; }
const FM_AudioDecoderOps *FM_Vorbis_GetOps(void) { return NULL; }
FM_Result FM_Vorbis_StreamOpen(const uint8_t *const hdr[3],
                               const uint32_t hdr_len[3],
                               FM_VorbisStream **out)
{
    (void)hdr; (void)hdr_len;
    if (out != NULL) { *out = NULL; }
    return FM_ERR_UNSUPPORTED;
}
FM_Result FM_Vorbis_StreamDecode(FM_VorbisStream *s, const uint8_t *packet,
                                 uint32_t len, uint8_t *pcm, size_t cap,
                                 size_t *written)
{
    (void)s; (void)packet; (void)len; (void)pcm; (void)cap;
    if (written != NULL) { *written = 0u; }
    return FM_ERR_UNSUPPORTED;
}
size_t FM_Vorbis_StreamPending(const FM_VorbisStream *s) { (void)s; return 0u; }
uint32_t FM_Vorbis_StreamChannels(const FM_VorbisStream *s) { (void)s; return 0u; }
void FM_Vorbis_StreamReset(FM_VorbisStream *s) { (void)s; }
void FM_Vorbis_StreamClose(FM_VorbisStream *s) { (void)s; }
#endif
