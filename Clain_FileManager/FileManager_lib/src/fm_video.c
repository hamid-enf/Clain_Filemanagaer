/*
 * FileManager - Video containers (FMRV, AVI, MP4)
 *
 * FMRV is the built-in raw container (fixed-size frames + small header).
 * AVI (RIFF) and MP4 (ISO BMFF) are parsed far enough to extract the video
 * stream: dimensions, frame rate, codec and a per-frame offset/size index.
 * Frame payloads are handed to a codec plugin (see fm_h264.h) for decode.
 *
 * The frame index lives in a static pool sized by FM_VIDEO_MAX_FRAMES; no
 * heap is used by the parser itself.
 */

#define FM_DEBUG_TAG "video"
#include "fm_debug.h"
#include "fm_video.h"
#include "fm_internal.h"
#include "fm_hw.h"
#include "fm_alloc.h"
#if FM_USE_IMAGE
#include "fm_image.h"
#endif
#if FM_USE_H264
#include "fm_h264.h"
#endif
#if FM_USE_MJPEG
#include "fm_mjpeg.h"
#endif
#include <string.h>
#include "fm_audio.h"
#if FM_USE_MP3
#include "minimp3.h"
#endif

#if FM_USE_VIDEO

/* Ogg CRC for Vorbis synthesis (same as in stb_vorbis) */
static uint32_t ogg_crc_table[256];
static void ogg_crc_init(void)
{
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i=0;i<256;i++) {
        uint32_t r = (uint32_t)i << 24;
        for (int j=0;j<8;j++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        ogg_crc_table[i] = r;
    }
}
static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    ogg_crc_init();
    for (size_t i=0;i<len;i++) crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}
/* Build a minimal Ogg Vorbis file from MKV CodecPrivate + one audio packet.
 * MKV private: [0]=packets-1 (2), then Xiph lacing for first n-1 packets,
 * then packets. We extract the 3 Vorbis headers and the packet.
 * Returns FM_OK and *out_ogg is FM_Alloc'd with *out_len.
 * Caller must FM_Free. */
static FM_Result vorbis_build_ogg_from_mkv(const uint8_t *priv, uint32_t priv_size,
                                           const uint8_t *pkt, uint32_t pkt_size,
                                           uint8_t **out_ogg, uint32_t *out_len)
{
    if (!priv || !pkt || !out_ogg || !out_len || priv_size < 1u) return FM_ERR_INVALID_ARGUMENT;
    uint8_t n = priv[0];
    uint32_t count = (uint32_t)n + 1u;
    if (count != 3u) return FM_ERR_INVALID_FORMAT; /* Vorbis expects 3 headers */
    /* parse Xiph lengths */
    uint32_t pos = 1u;
    uint32_t lens[3] = {0};
    for (uint32_t i=0;i<count-1u;i++) {
        uint32_t len = 0;
        while (pos < priv_size) {
            uint8_t b = priv[pos++];
            len += b;
            if (b != 255u) break;
        }
        if (pos > priv_size) return FM_ERR_CORRUPTED;
        lens[i] = len;
    }
    lens[count-1u] = priv_size - pos;
    /* verify sum */
    uint32_t sum = lens[0]+lens[1]+lens[2];
    if (sum != priv_size - (1u + (count-1u)) && sum != priv_size - pos + lens[count-1u]) {
        /* Fallback: try simple 16-bit BE lengths (some muxers) */
        if (priv_size >= 5u) {
            lens[0] = ((uint32_t)priv[1]<<8)|priv[2];
            lens[1] = ((uint32_t)priv[3]<<8)|priv[4];
            lens[2] = priv_size - 5u;
            pos = 5u;
        } else return FM_ERR_CORRUPTED;
    }
    /* check that packet data matches */
    if (pos + lens[0] + lens[1] + lens[2] > priv_size) return FM_ERR_CORRUPTED;
    const uint8_t *h0 = priv + pos;
    const uint8_t *h1 = h0 + lens[0];
    const uint8_t *h2 = h1 + lens[1];
    /* Build Ogg: 2 pages: first with 3 headers (continued), second with packet */
    uint32_t page1_data = lens[0]+lens[1]+lens[2];
    uint32_t page2_data = pkt_size;
    uint32_t page1_segs = (page1_data + 254u)/255u;
    if (page1_segs > 255u) return FM_ERR_TOO_LARGE;
    uint32_t page2_segs = (page2_data + 254u)/255u;
    if (page2_segs > 255u) return FM_ERR_TOO_LARGE;
    uint32_t total = 27+page1_segs+page1_data + 27+page2_segs+page2_data;
    uint8_t *out = (uint8_t*)FM_Alloc(total);
    if (!out) return FM_ERR_NO_MEMORY;
    uint32_t off = 0;
    /* Page 1 */
    memcpy(out+off, "OggS", 4); off+=4;
    out[off++] = 0; /* version */
    out[off++] = 0x02; /* BOS */
    out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; /* granule 0 */
    out[off++]=0x01; out[off++]=0; out[off++]=0; out[off++]=0; /* serial 1 */
    out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; /* seq 0 */
    out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; /* crc placeholder */
    out[off++] = (uint8_t)page1_segs;
    uint32_t p = 0;
    for (uint32_t i=0;i<page1_segs;i++) {
        uint8_t seg = (page1_data - p > 255u) ? 255u : (uint8_t)(page1_data - p);
        out[off++] = seg; p += seg;
    }
    uint32_t crc_pos1 = 22;
    memcpy(out+off, h0, lens[0]); off+=lens[0];
    memcpy(out+off, h1, lens[1]); off+=lens[1];
    memcpy(out+off, h2, lens[2]); off+=lens[2];
    uint32_t crc1 = ogg_crc(out, off);
    out[crc_pos1]= (uint8_t)(crc1 &0xFF); out[crc_pos1+1]=(uint8_t)((crc1>>8)&0xFF); out[crc_pos1+2]=(uint8_t)((crc1>>16)&0xFF); out[crc_pos1+3]=(uint8_t)((crc1>>24)&0xFF);
    /* Page 2 */
    uint32_t page2_start = off;
    memcpy(out+off, "OggS",4); off+=4;
    out[off++]=0;
    out[off++]=0x04; /* EOS */
    out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0; /* granule = samples */
    out[off++]=0x01; out[off++]=0; out[off++]=0; out[off++]=0;
    out[off++]=0x01; out[off++]=0; out[off++]=0; out[off++]=0; /* seq 1 */
    uint32_t crc_pos2 = page2_start+22;
    out[off++]=0; out[off++]=0; out[off++]=0; out[off++]=0;
    out[off++] = (uint8_t)page2_segs;
    p=0;
    for (uint32_t i=0;i<page2_segs;i++) {
        uint8_t seg = (page2_data - p > 255u) ? 255u : (uint8_t)(page2_data - p);
        out[off++] = seg; p+=seg;
    }
    memcpy(out+off, pkt, pkt_size); off+=pkt_size;
    uint32_t crc2 = ogg_crc(out+page2_start, off-page2_start);
    out[crc_pos2]= (uint8_t)(crc2 &0xFF); out[crc_pos2+1]=(uint8_t)((crc2>>8)&0xFF); out[crc_pos2+2]=(uint8_t)((crc2>>16)&0xFF); out[crc_pos2+3]=(uint8_t)((crc2>>24)&0xFF);
    *out_ogg = out;
    *out_len = off;
    return FM_OK;
}

#define FMRV_HEADER_SIZE 24u
#define SPSPPS_MAX 512u

/* ------------------------------------------------------------------ */
/* Private per-video state                                             */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint64_t offset;
    uint32_t size;
} FM_VideoFrame;

typedef struct
{
    FM_Bool       in_use;
    uint32_t      count;
    FM_Bool       avc1;              /* MP4 avc1: convert to Annex-B  */
    FM_Bool       raw_bgr;           /* AVI DIB: BGR (bottom-up) rows */
    FM_Bool       raw_top_down;      /* negative biHeight = top-down  */
    uint8_t       sps_pps[SPSPPS_MAX]; /* Annex-B SPS/PPS prefix      */
    uint32_t      sps_pps_len;
    FM_VideoFrame frames[FM_VIDEO_MAX_FRAMES];

    /* demuxed audio track */
    FM_Bool       audio_present;
    uint32_t      audio_codec;       /* FM_VideoAudioCodec            */
    uint32_t      audio_rate;
    uint16_t      audio_channels;
    uint16_t      audio_bits;        /* PCM only                      */
    uint32_t      audio_private_size;
    uint8_t       audio_private[512]; /* MKV CodecPrivate / MP4 dfLa for FLAC */
    uint32_t      audio_count;
    /* PCM chunk larger than the caller's buffer: bytes of the chunk
     * at audio_frame_index-1 already handed out.  The next call
     * continues from here instead of dropping the remainder. */
    uint64_t      audio_part_off;    /* file offset of the remainder  */
    uint32_t      audio_part_left;   /* bytes still to deliver        */
    FM_VideoFrame audio_frames[FM_VIDEO_MAX_FRAMES];
    /* AVI streaming continuation: when the track has more chunks than
     * FM_VIDEO_MAX_FRAMES, keep scanning movi from here on demand. */
    FM_Bool       avi_audio;         /* container is AVI              */
    uint64_t      audio_scan_pos;    /* first un-indexed chunk header */
    uint64_t      audio_scan_end;    /* movi end                      */
    uint32_t      audio_total;       /* total chunks seen in movi     */
    uint64_t      audio_cur_pos;     /* streaming cursor: file pos    */
    uint32_t      audio_cur_idx;     /* streaming cursor: chunk index */
} FM_VideoPriv;

/* Large index tables: move to SDRAM on MCU (saves ~500KB of RAM_D1).
 * FM_VIDEO_MAX_FRAMES=4096 => ~132KB per slot x4 = ~528KB.
 * Placing it in DTCM/AXI-SRAM overflows RAM_D1 (512KB) with the linker
 * error \"region RAM_D1 overflowed\". SDRAM is plentiful (30MB).
 * The SDRAM section is NOLOAD (no startup zero), so we clear it once
 * at first use — in_use must start as 0 for alloc to work. */
#ifndef FM_VIDEO_PRIV_SECTION
#define FM_VIDEO_PRIV_SECTION ".sdram_buffers"
#endif
#if defined(__GNUC__)
static FM_VideoPriv g_video[FM_MAX_VIDEO]
    __attribute__((section(FM_VIDEO_PRIV_SECTION), aligned(32)));
#else
static FM_VideoPriv g_video[FM_MAX_VIDEO];
#endif
static uint8_t g_video_sdram_inited;

/* ------------------------------------------------------------------ */
/* Endian helpers                                                      */
/* ------------------------------------------------------------------ */
static uint16_t video_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t video_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t video_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < 8; i++)
    {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static uint32_t video_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t video_be64(const uint8_t *p)
{
    return ((uint64_t)video_be32(p) << 32) | (uint64_t)video_be32(p + 4);
}

static uint16_t video_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void video_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void video_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static FM_Result video_read_exact(FM_File *f, void *data, uint32_t size)
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
            return FM_ERR_EOF;
        }
        done += rd;
    }
    return FM_OK;
}

static FM_Result video_write_exact(FM_File *f, const void *data,
                                   uint32_t size)
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

static FM_Result video_read_at(FM_File *f, uint64_t offset, void *data,
                               uint32_t size)
{
    if (offset > INT64_MAX ||
        File_Seek(f, (int64_t)offset, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    return video_read_exact(f, data, size);
}

/* ------------------------------------------------------------------ */
/* Private context pool                                                */
/* ------------------------------------------------------------------ */
static FM_VideoPriv *video_priv_alloc(void)
{
    uint32_t i;

    if (!g_video_sdram_inited)
    {
        memset(g_video, 0, sizeof(g_video));
        g_video_sdram_inited = 1u;
    }
    for (i = 0; i < FM_MAX_VIDEO; i++)
    {
        if (!g_video[i].in_use)
        {
            memset(&g_video[i], 0, sizeof(g_video[i]));
            g_video[i].in_use = FM_TRUE;
            return &g_video[i];
        }
    }
    return NULL;
}

static void video_priv_free(FM_VideoPriv *p)
{
    if (p != NULL)
    {
        p->in_use = FM_FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* FMRV                                                                */
/* ------------------------------------------------------------------ */
static FM_Result fmrv_parse(FM_Video *v)
{
    uint8_t h[FMRV_HEADER_SIZE];
    uint64_t size;

    if (File_Size(&v->file, &size) != FM_OK || size < FMRV_HEADER_SIZE ||
        File_Seek(&v->file, 0, FM_SEEK_SET) != FM_OK ||
        video_read_exact(&v->file, h, sizeof(h)) != FM_OK)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    memset(&v->info, 0, sizeof(v->info));
    v->info.width = video_rd16(h + 4);
    v->info.height = video_rd16(h + 6);
    v->info.fps = video_rd32(h + 8);
    v->info.frame_count = video_rd32(h + 12);
    v->info.frame_size = video_rd32(h + 16);
    v->info.data_offset = FMRV_HEADER_SIZE;
    v->info.codec = FM_VIDEO_CODEC_RAW;
    (void)strcpy(v->info.container, "FMRV");
    v->variable = 0;
    if (v->info.width == 0 || v->info.height == 0 || v->info.fps == 0 ||
        v->info.frame_size == 0 || size < v->info.data_offset)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (v->info.frame_count == 0)
    {
        uint64_t bytes = size - v->info.data_offset;

        if (bytes % v->info.frame_size != 0 ||
            bytes / v->info.frame_size > UINT32_MAX)
        {
            return FM_ERR_CORRUPTED;
        }
        v->info.frame_count = (uint32_t)(bytes / v->info.frame_size);
    }
    if ((uint64_t)v->info.frame_count * v->info.frame_size >
        size - v->info.data_offset)
    {
        return FM_ERR_CORRUPTED;
    }
    return Video_Seek(v, 0);
}

/* ------------------------------------------------------------------ */
/* AVI (RIFF)                                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint64_t hdrl_start;
    uint64_t hdrl_end;
    uint64_t movi_start;
    uint64_t movi_end;
} AviLayout;

static FM_Result avi_find_lists(FM_Video *v, AviLayout *lay)
{
    uint64_t file_size = 0;
    uint64_t pos;

    if (File_Size(&v->file, &file_size) != FM_OK)
    {
        return FM_ERR_IO;
    }
    lay->hdrl_start = lay->hdrl_end = 0;
    lay->movi_start = lay->movi_end = 0;

    pos = 12; /* after RIFF size + "AVI " */
    while (pos + 8 <= file_size)
    {
        uint8_t ck[12];
        uint32_t sz;
        uint64_t data;

        if (video_read_at(&v->file, pos, ck, 12) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        sz = video_rd32(ck + 4);
        data = pos + 8;
        if (memcmp(ck, "LIST", 4) == 0)
        {
            if (memcmp(ck + 8, "hdrl", 4) == 0)
            {
                lay->hdrl_start = data + 4;
                lay->hdrl_end = data + sz;
            }
            else if (memcmp(ck + 8, "movi", 4) == 0)
            {
                lay->movi_start = data + 4;
                lay->movi_end = data + sz;
            }
        }
        pos = data + sz + (sz & 1u);
    }
    if (lay->movi_start == 0 || lay->movi_end <= lay->movi_start)
    {
        return FM_ERR_CORRUPTED;
    }
    return FM_OK;
}

/* Case-insensitive FourCC compare ("MJPG" matches "mjpg"). */
static FM_Bool fourcc_ci_eq(const uint8_t *a, const char *b)
{
    uint32_t i;

    for (i = 0; i < 4; i++)
    {
        uint8_t ca = a[i];
        uint8_t cb = (uint8_t)b[i];

        if (ca >= 'a' && ca <= 'z') { ca = (uint8_t)(ca - 'a' + 'A'); }
        if (cb >= 'a' && cb <= 'z') { cb = (uint8_t)(cb - 'a' + 'A'); }
        if (ca != cb)
        {
            return FM_FALSE;
        }
    }
    return FM_TRUE;
}

/* Map an AVI FourCC (stream handler or biCompression) to a codec.
 * Extended for broader frame-extraction: VP8/VP9/AV1/HEVC and legacy
 * MPEG-4 aliases so Video_ReadFrame works even when decode is via host. */
static FM_VideoCodec avi_map_fourcc(const uint8_t *f)
{
    if (fourcc_ci_eq(f, "H264") || fourcc_ci_eq(f, "X264") ||
        fourcc_ci_eq(f, "AVC1") || fourcc_ci_eq(f, "DAVC") ||
        fourcc_ci_eq(f, "H264") || fourcc_ci_eq(f, "h264"))
    {
        return FM_VIDEO_CODEC_H264;
    }
    if (fourcc_ci_eq(f, "MJPG") || fourcc_ci_eq(f, "JPEG") ||
        fourcc_ci_eq(f, "DMB1") || fourcc_ci_eq(f, "AVRN") ||
        fourcc_ci_eq(f, "IJPG") || fourcc_ci_eq(f, "MJPA") ||
        fourcc_ci_eq(f, "MJPB"))
    {
        return FM_VIDEO_CODEC_MJPEG;
    }
    if (fourcc_ci_eq(f, "DIB ") || fourcc_ci_eq(f, "RGB ") ||
        fourcc_ci_eq(f, "RAW "))
    {
        return FM_VIDEO_CODEC_RAW;
    }
    if (fourcc_ci_eq(f, "MP4V") || fourcc_ci_eq(f, "XVID") ||
        fourcc_ci_eq(f, "DIVX") || fourcc_ci_eq(f, "DX50") ||
        fourcc_ci_eq(f, "FMP4") || fourcc_ci_eq(f, "MP42") ||
        fourcc_ci_eq(f, "MP43") || fourcc_ci_eq(f, "3IV2"))
    {
        return FM_VIDEO_CODEC_MPEG4;
    }
    if (fourcc_ci_eq(f, "VP80") || fourcc_ci_eq(f, "VP8 ") ||
        fourcc_ci_eq(f, "VP8O"))
    {
        return FM_VIDEO_CODEC_VP8;
    }
    if (fourcc_ci_eq(f, "VP90") || fourcc_ci_eq(f, "VP9 ") ||
        fourcc_ci_eq(f, "VP9O"))
    {
        return FM_VIDEO_CODEC_VP9;
    }
    if (fourcc_ci_eq(f, "AV01") || fourcc_ci_eq(f, "AV1 ") ||
        fourcc_ci_eq(f, "AVON"))
    {
        return FM_VIDEO_CODEC_AV1;
    }
    if (fourcc_ci_eq(f, "HEVC") || fourcc_ci_eq(f, "HVC1") ||
        fourcc_ci_eq(f, "HEV1") || fourcc_ci_eq(f, "h265") ||
        fourcc_ci_eq(f, "H265"))
    {
        return FM_VIDEO_CODEC_HEVC;
    }
    if (fourcc_ci_eq(f, "MPG2") || fourcc_ci_eq(f, "MPEG") ||
        fourcc_ci_eq(f, "MPG1") || fourcc_ci_eq(f, "PIM1"))
    {
        return FM_VIDEO_CODEC_MPEG2;
    }
    if (fourcc_ci_eq(f, "THEO") || fourcc_ci_eq(f, "THRA"))
    {
        return FM_VIDEO_CODEC_THEORA;
    }
    return FM_VIDEO_CODEC_UNKNOWN;
}

static FM_Result avi_parse_header(FM_Video *v, const AviLayout *lay,
                                  FM_VideoPriv *priv)
{
    uint64_t pos = lay->hdrl_start;
    uint64_t end = lay->hdrl_end;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t total_frames = 0;
    uint32_t us_per_frame = 0;
    uint8_t handler[4] = { 0, 0, 0, 0 };
    uint8_t compression[4] = { 0, 0, 0, 0 };
    uint16_t bit_count = 0;
    FM_Bool compression_zero = FM_FALSE; /* BI_RGB uncompressed DIB */
    FM_Bool height_negative = FM_FALSE;  /* negative biHeight = top-down */
    FM_Bool have_video = FM_FALSE;

    while (pos + 8 <= end)
    {
        uint8_t ck[8];
        uint32_t sz;
        uint64_t data;

        if (video_read_at(&v->file, pos, ck, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        sz = video_rd32(ck + 4);
        data = pos + 8;

        if (memcmp(ck, "avih", 4) == 0)
        {
            uint8_t avih[56];

            if (sz < 40 || video_read_at(&v->file, data, avih, 40) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            us_per_frame = video_rd32(avih + 0);
            total_frames = video_rd32(avih + 16);
            width = video_rd32(avih + 32);
            height = video_rd32(avih + 36);
        }
        else if (memcmp(ck, "LIST", 4) == 0 && sz >= 4)
        {
            /* strl list: scan strh/strf inside */
            uint64_t sp = data + 4;
            uint64_t send = data + sz;
            FM_Bool is_video_stream = FM_FALSE;
            FM_Bool is_audio_stream = FM_FALSE;

            while (sp + 8 <= send)
            {
                uint8_t sc[8];
                uint32_t ssz;
                uint64_t sd;

                if (video_read_at(&v->file, sp, sc, 8) != FM_OK)
                {
                    return FM_ERR_CORRUPTED;
                }
                ssz = video_rd32(sc + 4);
                sd = sp + 8;
                if (memcmp(sc, "strh", 4) == 0 && ssz >= 8)
                {
                    uint8_t strh[8];

                    if (video_read_at(&v->file, sd, strh, 8) != FM_OK)
                    {
                        return FM_ERR_CORRUPTED;
                    }
                    if (memcmp(strh, "vids", 4) == 0 && !have_video)
                    {
                        memcpy(handler, strh + 4, 4);
                        have_video = FM_TRUE;
                        is_video_stream = FM_TRUE;
                        is_audio_stream = FM_FALSE;
                    }
                    else if (memcmp(strh, "auds", 4) == 0)
                    {
                        is_audio_stream = FM_TRUE;
                        is_video_stream = FM_FALSE;
                    }
                    else
                    {
                        is_video_stream = FM_FALSE;
                        is_audio_stream = FM_FALSE;
                    }
                }
                else if (memcmp(sc, "strf", 4) == 0 && ssz >= 16)
                {
                    uint8_t strf[40];

                    if (video_read_at(&v->file, sd, strf, 40) != FM_OK)
                    {
                        return FM_ERR_CORRUPTED;
                    }
                    if (is_video_stream)
                    {
                        /* BITMAPINFOHEADER: biWidth(4) biHeight(4) ...
                         * biBitCount(2 @14) biCompression(4 @16) */
                        int32_t bi_h = (int32_t)video_rd32(strf + 8);

                        width = video_rd32(strf + 4);
                        if (bi_h < 0)
                        {
                            height_negative = FM_TRUE;
                            height = (uint32_t)(-bi_h);
                        }
                        else
                        {
                            height = (uint32_t)bi_h;
                        }
                        bit_count = video_rd16(strf + 14);
                        memcpy(compression, strf + 16, 4);
                        compression_zero =
                            (video_rd32(strf + 16) == 0u) ? FM_TRUE : FM_FALSE;
                    }
                    else if (is_audio_stream)
                    {
                        /* WAVEFORMATEX: tag(2) channels(2) rate(4) ... bits(2)
                         * Expanded for MP3(0x0055), aLaw(6), muLaw(7), ADPCM (0x11/2). */
                        uint16_t tag = video_rd16(strf);

                        if (tag == 1u) /* PCM */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_PCM;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = video_rd16(strf + 14);
                            priv->audio_present = FM_TRUE;
                        }
                        else if (tag == 0x0055u) /* MP3 */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_MP3;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = 0u;
                            priv->audio_present = FM_TRUE;
                        }
                        else if (tag == 6u) /* ALAW */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_ALAW;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = 16u; /* decoded to PCM16 */
                            priv->audio_present = FM_TRUE;
                        }
                        else if (tag == 7u) /* ULAW */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_ULAW;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = 16u;
                            priv->audio_present = FM_TRUE;
                        }
                        else if (tag == 0x0011u || tag == 0x0002u) /* IMA/MS ADPCM */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_ADPCM;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = 16u;
                            priv->audio_present = FM_TRUE;
                        }
                        else if (tag == 0x674Fu || tag == 0x6750u || tag == 0xFFu)
                        {
                            /* Vorbis / AAC-ish in AVI (rare) */
                            priv->audio_codec = (tag == 0xFFu) ? FM_VIDEO_AUDIO_AAC
                                                               : FM_VIDEO_AUDIO_VORBIS;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = 0u;
                            priv->audio_present = FM_TRUE;
                        }
                    }
                }
                sp = sd + ssz + (ssz & 1u);
            }
        }
        pos = data + sz + (sz & 1u);
    }

    if (!have_video || width == 0 || height == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    v->info.width = (width > 0xFFFFu) ? 0u : (uint16_t)width;
    v->info.height = (height > 0xFFFFu) ? 0u : (uint16_t)height;
    if (v->info.width == 0 || v->info.height == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* Round to the nearest integer fps instead of truncating: 29.97 fps
     * is 33367 us/frame -> 1000000/33367 = 29 (truncated), i.e. a 3.3%
     * slower video clock.  The audio ring is clocked by the I2S at the
     * real rate, so with truncation the picture drifts behind the sound
     * by ~2 s per minute and the pacing loop keeps catching up in
     * bursts (visible as periodic stutter).  Rounding gives 30. */
    v->info.fps = (us_per_frame != 0)
                      ? ((1000000u + us_per_frame / 2u) / us_per_frame)
                      : 0u;
    v->info.frame_count = total_frames;
    v->info.frame_size = 0;
    v->variable = 1;
    (void)strcpy(v->info.container, "AVI");

    /* Codec detection: the strh handler FourCC is unreliable (often blank
     * or "\0\0\0\0"); biCompression in strf is authoritative, so try it
     * first and fall back to the handler. */
    v->info.codec = avi_map_fourcc(compression);
    if (v->info.codec == FM_VIDEO_CODEC_UNKNOWN)
    {
        if (compression_zero && bit_count >= 16u)
        {
            /* biCompression == BI_RGB: uncompressed DIB frames. */
            v->info.codec = FM_VIDEO_CODEC_RAW;
        }
        else
        {
            v->info.codec = avi_map_fourcc(handler);
        }
    }
    if (v->info.codec == FM_VIDEO_CODEC_RAW)
    {
        /* Only 24-bit BGR DIB frames are decodable as raw video. */
        if (bit_count != 0u && bit_count != 24u)
        {
            return FM_ERR_UNSUPPORTED;
        }
        priv->raw_bgr = FM_TRUE;
        priv->raw_top_down = height_negative;
    }
    return FM_OK;
}

static FM_Result avi_build_index(FM_Video *v, const AviLayout *lay,
                                 FM_VideoPriv *priv)
{
    uint64_t pos = lay->movi_start;
    uint64_t end = lay->movi_end;
    uint64_t file_size = 0;

    (void)File_Size(&v->file, &file_size);
    priv->count = 0;

    while (pos + 8 <= end)
    {
        uint8_t ck[8];
        uint32_t sz;
        uint64_t data;

        if (video_read_at(&v->file, pos, ck, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        sz = video_rd32(ck + 4);
        data = pos + 8;
        if (data + sz > end)
        {
            return FM_ERR_CORRUPTED;
        }
        /* "##" prefix chunks are AVI index metadata; "ix00"/"ix01" too. */
        if (ck[0] == '#' || memcmp(ck, "ix", 2) == 0)
        {
            /* skip index chunks */
        }
        else if (memcmp(ck, "LIST", 4) == 0 && sz >= 4)
        {
            /* Interleaved 'rec ' lists wrap the actual frame chunks;
             * descend into the list instead of skipping it. */
            pos = data + 4;
            continue;
        }
        else if (memcmp(ck + 2, "dc", 2) == 0 || memcmp(ck + 2, "db", 2) == 0)
        {
            /* Video frame chunk.  Zero-size chunks are dropped-frame
             * placeholders; indexing them would make decode fail, so
             * skip them (playback just holds the previous frame). */
            if (sz > 0u && priv->count < FM_VIDEO_MAX_FRAMES)
            {
                priv->frames[priv->count].offset = data;
                priv->frames[priv->count].size = sz;
                priv->count++;
            }
        }
        else if (memcmp(ck + 2, "wb", 2) == 0)
        {
            /* audio (PCM waveform) chunk */
            if (sz > 0u && priv->audio_present)
            {
                if (priv->audio_count < FM_VIDEO_MAX_FRAMES)
                {
                    priv->audio_frames[priv->audio_count].offset = data;
                    priv->audio_frames[priv->audio_count].size = sz;
                    priv->audio_count++;
                }
                else if (priv->audio_scan_pos == 0u)
                {
                    /* Index full: long tracks continue by streaming the
                     * movi list from the first un-indexed chunk.  Without
                     * this the audio used to stop after
                     * FM_VIDEO_MAX_FRAMES chunks (~25 s at 22 kHz). */
                    priv->audio_scan_pos = pos;
                }
                priv->audio_total++;
            }
        }
        pos = data + sz + (sz & 1u);
    }
    priv->avi_audio = FM_TRUE;
    priv->audio_scan_end = end;
    if (priv->count == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (v->info.frame_count == 0 || v->info.frame_count > priv->count)
    {
        /* The video index is a fixed table: a clip with more frames than
         * FM_VIDEO_MAX_FRAMES (68 s at 15 fps, 34 s at 30 fps) silently
         * played only its first part and then "ended".  Playback still
         * stops there, but say so once instead of looking like a short
         * file. */
        if (v->info.frame_count > priv->count &&
            priv->count == (uint32_t)FM_VIDEO_MAX_FRAMES)
        {
            FM_ERR_LOG("avi: %lu frames but index holds %u — playback "
                       "truncated (raise FM_VIDEO_MAX_FRAMES)",
                       (unsigned long)v->info.frame_count,
                       (unsigned)FM_VIDEO_MAX_FRAMES);
        }
        v->info.frame_count = priv->count;
    }
    v->info.data_offset = priv->frames[0].offset;
    return FM_OK;
}

static FM_Result avi_parse(FM_Video *v, FM_VideoPriv *priv)
{
    AviLayout lay;
    FM_Result r = avi_find_lists(v, &lay);

    if (r != FM_OK)
    {
        return r;
    }
    r = avi_parse_header(v, &lay, priv);
    if (r != FM_OK)
    {
        return r;
    }
    r = avi_build_index(v, &lay, priv);
    if (r != FM_OK)
    {
        return r;
    }
    return Video_Seek(v, 0);
}

/* ------------------------------------------------------------------ */
/* MP4 (ISO BMFF)                                                      */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint64_t start;
    uint64_t data;
    uint64_t size;   /* total box size including header */
    char     type[5];
} Mp4Box;

static FM_Result mp4_box_at(FM_Video *v, uint64_t pos, uint64_t end,
                            Mp4Box *box)
{
    uint8_t h[16];

    if (pos + 8 > end)
    {
        return FM_ERR_CORRUPTED;
    }
    if (video_read_at(&v->file, pos, h, 8) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    memcpy(box->type, h + 4, 4);
    box->type[4] = '\0';
    box->start = pos;
    box->size = video_be32(h + 0);
    box->data = pos + 8;
    if (box->size == 1)
    {
        if (pos + 16 > end || video_read_at(&v->file, pos + 8, h, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        box->size = video_rd64(h);
        box->data = pos + 16;
    }
    else if (box->size == 0)
    {
        box->size = end - pos;
    }
    if (box->data > end || box->size > end - pos)
    {
        return FM_ERR_CORRUPTED;
    }
    return FM_OK;
}

/* Iterate child boxes; cb returns FM_OK to continue, other to stop. */
static FM_Result mp4_walk(FM_Video *v, uint64_t start, uint64_t end,
                          const char *wanted, Mp4Box *out)
{
    uint64_t pos = start;

    while (pos + 8 <= end)
    {
        Mp4Box box;
        FM_Result r = mp4_box_at(v, pos, end, &box);

        if (r != FM_OK)
        {
            return r;
        }
        if (wanted == NULL || strcmp(box.type, wanted) == 0)
        {
            if (out != NULL)
            {
                *out = box;
            }
            return FM_OK;
        }
        pos = box.start + box.size;
    }
    return FM_ERR_NOT_FOUND;
}

/* Recursively search for a box, descending into container boxes only
 * (so leaf payloads like mdat or avc1 sample entries are never traversed). */
static FM_Result mp4_find_deep(FM_Video *v, uint64_t start, uint64_t end,
                               const char *wanted, Mp4Box *out)
{
    uint64_t pos = start;

    while (pos + 8 <= end)
    {
        Mp4Box box;
        FM_Result r = mp4_box_at(v, pos, end, &box);

        if (r != FM_OK)
        {
            return r;
        }
        if (strcmp(box.type, wanted) == 0)
        {
            *out = box;
            return FM_OK;
        }
        if (strcmp(box.type, "moov") == 0 || strcmp(box.type, "trak") == 0 ||
            strcmp(box.type, "mdia") == 0 || strcmp(box.type, "minf") == 0 ||
            strcmp(box.type, "stbl") == 0 || strcmp(box.type, "dinf") == 0 ||
            strcmp(box.type, "edts") == 0 || strcmp(box.type, "mvex") == 0 ||
            strcmp(box.type, "udta") == 0)
        {
            r = mp4_find_deep(v, box.data, box.start + box.size, wanted, out);
            if (r == FM_OK)
            {
                return FM_OK;
            }
        }
        pos = box.start + box.size;
    }
    return FM_ERR_NOT_FOUND;
}

static FM_Result mp4_parse_avcc(FM_Video *v, Mp4Box *avcc, FM_VideoPriv *priv)
{
    static const uint8_t start[] = { 0, 0, 0, 1 };
    uint8_t hdr[6];
    uint8_t one;
    uint32_t end;
    uint32_t off;
    uint32_t num;
    uint32_t i;
    uint32_t list;

    if (avcc->size < 8 + 7)
    {
        return FM_ERR_CORRUPTED;
    }
    if (video_read_at(&v->file, avcc->data, hdr, 6) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    (void)hdr[0]; /* configurationVersion */
    (void)hdr[1]; /* profile */
    (void)hdr[2]; /* compat */
    (void)hdr[3]; /* level */
    (void)hdr[4]; /* lengthSizeMinusOne (assumed 4-byte lengths) */
    end = (uint32_t)(avcc->size - 8);
    off = 6;
    priv->sps_pps_len = 0;

    /* SPS list */
    num = hdr[5] & 0x1Fu;
    for (list = 0; list < 2; list++)
    {
        uint32_t n = (list == 0) ? num : 0;

        if (list == 1)
        {
            if (off + 1 > end ||
                video_read_at(&v->file, avcc->data + off, &one, 1) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            off += 1;
            n = one & 0xFFu;
        }
        for (i = 0; i < n; i++)
        {
            uint8_t lb[2];
            uint32_t nlen;

            if (off + 2 > end ||
                video_read_at(&v->file, avcc->data + off, lb, 2) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            nlen = video_be16(lb);
            off += 2;
            if (nlen == 0 || off + nlen > end ||
                priv->sps_pps_len + 4 + nlen > sizeof(priv->sps_pps))
            {
                return FM_ERR_CORRUPTED;
            }
            memcpy(priv->sps_pps + priv->sps_pps_len, start, 4);
            priv->sps_pps_len += 4;
            if (video_read_at(&v->file, avcc->data + off,
                              priv->sps_pps + priv->sps_pps_len, nlen) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            priv->sps_pps_len += nlen;
            off += nlen;
        }
    }
    return FM_OK;
}

/* Returns FM_OK if the trak's handler matches `tag` (e.g. "vide", "soun"). */
static FM_Result mp4_trak_has_handler(FM_Video *v, const Mp4Box *trak,
                                      const char *tag)
{
    Mp4Box hdlr;
    uint8_t ht[4];

    if (mp4_find_deep(v, trak->data, trak->start + trak->size, "hdlr",
                      &hdlr) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (hdlr.size < 8 + 12 ||
        video_read_at(&v->file, hdlr.data + 8, ht, 4) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    return (memcmp(ht, tag, 4) == 0) ? FM_OK : FM_ERR_NOT_FOUND;
}

/* Find the chunk-offset box of an stbl: 32-bit "stco" or 64-bit "co64".
 * *entry_bytes receives the per-entry size (4 or 8). */
static FM_Result mp4_find_chunk_offsets(FM_Video *v, const Mp4Box *stbl,
                                        uint64_t *off, uint32_t *count,
                                        uint32_t *entry_bytes)
{
    Mp4Box box;
    uint8_t h[8];

    if (mp4_find_deep(v, stbl->data, stbl->start + stbl->size, "stco",
                      &box) == FM_OK &&
        box.size >= 8 + 8 &&
        video_read_at(&v->file, box.data, h, 8) == FM_OK)
    {
        *count = video_be32(h + 4);
        *off = box.data + 8;
        *entry_bytes = 4u;
        return FM_OK;
    }
    if (mp4_find_deep(v, stbl->data, stbl->start + stbl->size, "co64",
                      &box) == FM_OK &&
        box.size >= 8 + 8 &&
        video_read_at(&v->file, box.data, h, 8) == FM_OK)
    {
        *count = video_be32(h + 4);
        *off = box.data + 8;
        *entry_bytes = 8u;
        return FM_OK;
    }
    return FM_ERR_NOT_FOUND;
}

/* Build a per-sample (offset,size) index from an MP4 stbl (stsc+stco+stsz). */
static FM_Result mp4_build_index(FM_Video *v, uint64_t stco_off,
                                 uint32_t stco_count, uint32_t stco_entry,
                                 uint64_t stsc_off,
                                 uint32_t stsc_count, uint64_t stsz_off,
                                 uint32_t sample_size, uint32_t sample_count,
                                 FM_VideoFrame *frames, uint32_t max,
                                 uint32_t *out_count)
{
    uint32_t chunk;
    uint32_t sample = 0;

    if (stco_entry != 4u && stco_entry != 8u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    for (chunk = 0; chunk < stco_count && sample < sample_count; chunk++)
    {
        uint8_t h[8];
        uint64_t chunk_off;
        uint32_t per_chunk = 1;
        uint32_t c;

        if (video_read_at(&v->file, stco_off + (uint64_t)chunk * stco_entry,
                          h, stco_entry) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        chunk_off = (stco_entry == 8u) ? video_be64(h) : video_be32(h);

        if (stsc_count > 0)
        {
            uint32_t s;

            for (s = 0; s < stsc_count; s++)
            {
                uint8_t e[12];

                if (video_read_at(&v->file, stsc_off + (uint64_t)s * 12, e, 12) != FM_OK)
                {
                    return FM_ERR_CORRUPTED;
                }
                if (video_be32(e + 0) <= chunk + 1)
                {
                    per_chunk = video_be32(e + 4);
                }
            }
        }

        for (c = 0; c < per_chunk && sample < sample_count; c++)
        {
            uint32_t sz = sample_size;

            if (sample_size == 0)
            {
                uint8_t s[4];

                if (video_read_at(&v->file, stsz_off + (uint64_t)sample * 4, s, 4) != FM_OK)
                {
                    return FM_ERR_CORRUPTED;
                }
                sz = video_be32(s);
            }
            if (*out_count < max)
            {
                frames[*out_count].offset = (uint64_t)chunk_off;
                frames[*out_count].size = sz;
                (*out_count)++;
            }
            chunk_off += sz;
            sample++;
        }
    }
    return (*out_count > 0) ? FM_OK : FM_ERR_NOT_FOUND;
}

/* Parse the audio properties of an MP4 'soun' track. */
static FM_Result mp4_parse_audio_trak(FM_Video *v, const Mp4Box *trak,
                                      FM_VideoPriv *priv)
{
    Mp4Box stbl;
    Mp4Box stsd;
    Mp4Box entry;
    Mp4Box stts;
    Mp4Box stsz;
    Mp4Box stsc;
    Mp4Box stco;
    uint8_t h[16];
    uint32_t sample_size = 0;
    uint32_t sample_count = 0;
    uint32_t stco_count = 0;
    uint32_t stsc_count = 0;
    uint32_t stco_entry = 4;
    uint64_t stsz_off = 0;
    uint64_t stco_off = 0;
    uint64_t stsc_off = 0;
    uint32_t rate = 0;
    uint16_t channels = 0;
    FM_Bool have_mp4a = FM_FALSE;

    if (mp4_find_deep(v, trak->data, trak->start + trak->size, "stbl",
                      &stbl) != FM_OK)
    {
        return FM_ERR_NOT_FOUND;
    }
    if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsd",
                      &stsd) == FM_OK)
    {
        if (mp4_box_at(v, stsd.data + 8, stsd.start + stsd.size, &entry) == FM_OK)
        {
            uint8_t a[28];

            if (entry.size >= 8 + 28 &&
                video_read_at(&v->file, entry.data, a, 28) == FM_OK)
            {
                if (memcmp(entry.type, "mp4a", 4) == 0 ||
                    memcmp(entry.type, "enca", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    priv->audio_codec = FM_VIDEO_AUDIO_AAC;
                    have_mp4a = FM_TRUE;
                }
                else if (memcmp(entry.type, ".mp3", 4) == 0 ||
                         memcmp(entry.type, "mp3 ", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    if (rate == 0u) { rate = 44100u; }
                    priv->audio_codec = FM_VIDEO_AUDIO_MP3;
                    have_mp4a = FM_TRUE;
                }
                else if (memcmp(entry.type, "Opus", 4) == 0 ||
                         memcmp(entry.type, "opus", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    if (rate == 0u) { rate = 48000u; }
                    priv->audio_codec = FM_VIDEO_AUDIO_OPUS;
                    have_mp4a = FM_TRUE;
                }
                else if (memcmp(entry.type, "fLaC", 4) == 0 ||
                         memcmp(entry.type, "flac", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    if (rate == 0u) { rate = 44100u; }
                    priv->audio_codec = FM_VIDEO_AUDIO_FLAC;
                    have_mp4a = FM_TRUE;
                    /* dfLa box holds FLAC STREAMINFO: capture for dr_flac synthesis */
                    {
                        uint64_t dpos = entry.data + 28u;
                        uint64_t dend = entry.start + entry.size;
                        while (dpos + 8u <= dend)
                        {
                            Mp4Box db;
                            if (mp4_box_at(v, dpos, dend, &db) != FM_OK) break;
                            if (memcmp(db.type, "dfLa", 4) == 0)
                            {
                                /* dfLa payload: 4-byte version/flags + STREAMINFO */
                                uint64_t payload = db.data + 4u;
                                uint64_t plen = (db.size > db.data - db.start) ? (db.start + db.size - payload) : 0u;
                                if (plen > sizeof(priv->audio_private)) plen = sizeof(priv->audio_private);
                                if (plen > 0u && video_read_at(&v->file, payload, priv->audio_private, (uint32_t)plen) == FM_OK)
                                {
                                    priv->audio_private_size = (uint32_t)plen;
                                }
                                break;
                            }
                            dpos = db.start + db.size;
                        }
                    }
                }
                else if (memcmp(entry.type, "vorb", 4) == 0 ||
                         memcmp(entry.type, "vvor", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    if (rate == 0u) { rate = 44100u; }
                    priv->audio_codec = FM_VIDEO_AUDIO_VORBIS;
                    have_mp4a = FM_TRUE;
                }
                else if (memcmp(entry.type, "alac", 4) == 0 ||
                         memcmp(entry.type, "ulaw", 4) == 0 ||
                         memcmp(entry.type, "alaw", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    priv->audio_codec = (memcmp(entry.type, "alac", 4) == 0)
                                            ? FM_VIDEO_AUDIO_PCM
                                            : (memcmp(entry.type, "ulaw", 4) == 0
                                                   ? FM_VIDEO_AUDIO_ULAW
                                                   : FM_VIDEO_AUDIO_ALAW);
                    have_mp4a = FM_TRUE;
                }
            }
        }
    }
    if (!have_mp4a)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsz", &stsz) == FM_OK &&
        stsz.size >= 8 + 12 &&
        video_read_at(&v->file, stsz.data, h, 12) == FM_OK)
    {
        sample_size = video_be32(h + 4);
        sample_count = video_be32(h + 8);
        stsz_off = stsz.data + 12;
    }
    if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsc", &stsc) == FM_OK &&
        stsc.size >= 8 + 8 &&
        video_read_at(&v->file, stsc.data, h, 8) == FM_OK)
    {
        stsc_count = video_be32(h + 4);
        stsc_off = stsc.data + 8;
    }
    (void)stco;
    (void)mp4_find_chunk_offsets(v, &stbl, &stco_off, &stco_count,
                                 &stco_entry);
    (void)stts;
    if (sample_count == 0 || stco_count == 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    /* priv->audio_codec already set in the stsd switch above */
    priv->audio_rate = rate;
    priv->audio_channels = channels;
    priv->audio_bits = (priv->audio_codec == FM_VIDEO_AUDIO_PCM ||
                        priv->audio_codec == FM_VIDEO_AUDIO_ALAW ||
                        priv->audio_codec == FM_VIDEO_AUDIO_ULAW ||
                        priv->audio_codec == FM_VIDEO_AUDIO_ADPCM)
                           ? 16u
                           : 0u;
    priv->audio_present = FM_TRUE;
    return mp4_build_index(v, stco_off, stco_count, stco_entry,
                           stsc_off, stsc_count,
                           stsz_off, sample_size, sample_count,
                           priv->audio_frames, FM_VIDEO_MAX_FRAMES,
                           &priv->audio_count);
}

static FM_Result mp4_parse(FM_Video *v, FM_VideoPriv *priv)
{
    uint64_t file_size = 0;
    Mp4Box moov;
    Mp4Box trak;
    Mp4Box video_trak;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t timescale = 0;
    uint32_t frame_delta = 0;
    uint32_t sample_count = 0;
    uint32_t sample_size = 0;   /* stsz uniform size (0 = per-sample) */
    uint64_t stsz_off = 0;
    uint64_t stco_off = 0;
    uint32_t stco_count = 0;
    uint32_t stco_entry = 4;
    uint64_t stsc_off = 0;
    uint32_t stsc_count = 0;
    FM_Bool have_video = FM_FALSE;
    FM_Result r;
    uint64_t tpos;

    if (File_Size(&v->file, &file_size) != FM_OK)
    {
        return FM_ERR_IO;
    }
    r = mp4_walk(v, 0, file_size, "moov", &moov);
    if (r != FM_OK)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* find the video trak (and, if present, the audio trak) */
    tpos = moov.data;
    r = mp4_find_deep(v, moov.data, moov.start + moov.size, "trak", &trak);
    while (r == FM_OK)
    {
        if (mp4_trak_has_handler(v, &trak, "vide") == FM_OK && !have_video)
        {
            have_video = FM_TRUE;
            video_trak = trak;
        }
        else if (mp4_trak_has_handler(v, &trak, "soun") == FM_OK)
        {
            (void)mp4_parse_audio_trak(v, &trak, priv);
        }
        tpos = trak.start + trak.size;
        r = mp4_find_deep(v, tpos, moov.start + moov.size, "trak", &trak);
    }
    if (!have_video)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    trak = video_trak;

    /* mdhd -> timescale */
    {
        Mp4Box mdhd;

        if (mp4_find_deep(v, trak.data, trak.start + trak.size, "mdhd", &mdhd) == FM_OK)
        {
            uint8_t m[20];

            if (video_read_at(&v->file, mdhd.data, m, 20) == FM_OK)
            {
                if (m[0] == 0)
                {
                    timescale = video_be32(m + 12);
                }
            }
        }
    }
    /* stbl */
    {
        Mp4Box stbl;

        if (mp4_find_deep(v, trak.data, trak.start + trak.size, "stbl", &stbl) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        /* stsd -> codec, width/height, avcC */
        {
            Mp4Box stsd;

            if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsd", &stsd) == FM_OK)
            {
                Mp4Box entry;
                uint8_t e[8];

                if (mp4_box_at(v, stsd.data + 8, stsd.start + stsd.size, &entry) == FM_OK &&
                    video_read_at(&v->file, entry.data, e, 8) == FM_OK)
                {
                    if (memcmp(entry.type, "avc1", 4) == 0 ||
                        memcmp(entry.type, "avc3", 4) == 0)
                    {
                        uint8_t dims[4];

                        priv->avc1 = FM_TRUE;
                        v->info.codec = FM_VIDEO_CODEC_H264;
                        /* width/height follow the 24-byte VisualSampleEntry
                         * prefix (after the 8-byte box header). */
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                        /* avcC child box.  Child boxes start after the
                         * 78-byte VisualSampleEntry fixed fields (ISO
                         * 14496-12); scanning from entry.data would read
                         * those fields as a bogus box and miss avcC. */
                        {
                            Mp4Box avcc;

                            if (mp4_find_deep(v, entry.data + 78u,
                                             entry.start + entry.size, "avcC",
                                             &avcc) == FM_OK)
                            {
                                (void)mp4_parse_avcc(v, &avcc, priv);
                            }
                            /* avc3 may carry SPS/PPS in-band instead;
                             * an empty prefix is fine in that case. */
                        }
                    }
                    else if (memcmp(entry.type, "jpeg", 4) == 0 ||
                             memcmp(entry.type, "mjpa", 4) == 0 ||
                             memcmp(entry.type, "mjpb", 4) == 0)
                    {
                        uint8_t dims[4];

                        /* Motion-JPEG in MP4/MOV: samples are complete
                         * JPEG images; no Annex-B conversion. */
                        v->info.codec = FM_VIDEO_CODEC_MJPEG;
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                    }
                    else if (memcmp(entry.type, "hvc1", 4) == 0 ||
                             memcmp(entry.type, "hev1", 4) == 0 ||
                             memcmp(entry.type, "h265", 4) == 0)
                    {
                        uint8_t dims[4];

                        v->info.codec = FM_VIDEO_CODEC_HEVC;
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                    }
                    else if (memcmp(entry.type, "av01", 4) == 0 ||
                             memcmp(entry.type, "av1C", 4) == 0)
                    {
                        uint8_t dims[4];

                        v->info.codec = FM_VIDEO_CODEC_AV1;
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                    }
                    else if (memcmp(entry.type, "vp08", 4) == 0 ||
                             memcmp(entry.type, "vp09", 4) == 0 ||
                             memcmp(entry.type, "VP90", 4) == 0)
                    {
                        uint8_t dims[4];

                        v->info.codec = (entry.type[3] == '8') ? FM_VIDEO_CODEC_VP8
                                                               : FM_VIDEO_CODEC_VP9;
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                    }
                    else if (memcmp(entry.type, "mp4v", 4) == 0)
                    {
                        uint8_t dims2[4];

                        v->info.codec = FM_VIDEO_CODEC_MPEG4;
                        if (video_read_at(&v->file, entry.data + 24, dims2, 4) == FM_OK)
                        {
                            width = video_be16(dims2);
                            height = video_be16(dims2 + 2);
                        }
                    }
                    else if (memcmp(entry.type, "mp4a", 4) == 0 ||
                             memcmp(entry.type, "enca", 4) == 0)
                    {
                        /* Audio-only stsd entry mis-detected: skip */
                    }
                    else
                    {
                        uint8_t dims[4];

                        /* Unknown video codec: keep container parsable for
                         * Video_ReadFrame extraction even without decoder. */
                        v->info.codec = FM_VIDEO_CODEC_UNKNOWN;
                        if (video_read_at(&v->file, entry.data + 24, dims, 4) == FM_OK)
                        {
                            width = video_be16(dims);
                            height = video_be16(dims + 2);
                        }
                    }
                }
            }
        }
        /* stts -> frame_delta */
        {
            Mp4Box stts;
            uint8_t h[16];

            if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stts", &stts) == FM_OK &&
                stts.size >= 8 + 16 &&
                video_read_at(&v->file, stts.data, h, 16) == FM_OK)
            {
                uint32_t entries = video_be32(h + 4);

                if (entries > 0)
                {
                    frame_delta = video_be32(h + 12);
                }
            }
        }
        /* stsz -> sample sizes */
        {
            Mp4Box stsz;
            uint8_t h[12];

            if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsz", &stsz) == FM_OK &&
                stsz.size >= 8 + 12 &&
                video_read_at(&v->file, stsz.data, h, 12) == FM_OK)
            {
                sample_size = video_be32(h + 4);
                sample_count = video_be32(h + 8);
                stsz_off = stsz.data + 12;
            }
        }
        /* stsc */
        {
            Mp4Box stsc;
            uint8_t h[8];

            if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsc", &stsc) == FM_OK &&
                stsc.size >= 8 + 8 &&
                video_read_at(&v->file, stsc.data, h, 8) == FM_OK)
            {
                stsc_count = video_be32(h + 4);
                stsc_off = stsc.data + 8;
            }
        }
        /* stco / co64 */
        (void)mp4_find_chunk_offsets(v, &stbl, &stco_off, &stco_count,
                                     &stco_entry);
    }

    if (width == 0 || height == 0 || sample_count == 0 || stco_count == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    v->info.width = (width > 0xFFFFu) ? 0u : (uint16_t)width;
    v->info.height = (height > 0xFFFFu) ? 0u : (uint16_t)height;
    /* Nearest integer (see the AVI note): 30000/1001 must give 30, not
     * 29, or the video clock runs 3% slow against the audio. */
    v->info.fps = (timescale != 0 && frame_delta != 0)
                      ? ((timescale + frame_delta / 2u) / frame_delta) : 0u;
    v->info.frame_count = sample_count;
    v->info.frame_size = 0;
    v->variable = 1;
    (void)strcpy(v->info.container, "MP4");

    /* Build the video frame index from stsc + stco + stsz. */
    r = mp4_build_index(v, stco_off, stco_count, stco_entry,
                        stsc_off, stsc_count,
                        stsz_off, sample_size, sample_count,
                        priv->frames, FM_VIDEO_MAX_FRAMES, &priv->count);
    if (r != FM_OK)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (sample_count > priv->count)
    {
        FM_ERR_LOG("mp4: %lu samples but index holds %u — playback "
                   "truncated (raise FM_VIDEO_MAX_FRAMES)",
                   (unsigned long)sample_count,
                   (unsigned)FM_VIDEO_MAX_FRAMES);
    }
    v->info.frame_count = priv->count;
    v->info.data_offset = priv->frames[0].offset;
    return Video_Seek(v, 0);
}

/* ------------------------------------------------------------------ */
/* G.711 A-law / mu-law -> linear PCM (for AVI 0x0006 / 0x0007)       */ 
/* ------------------------------------------------------------------ */
static int16_t alaw_decode(uint8_t a)
{
    uint16_t t;
    uint8_t  seg;

    a ^= 0x55u;
    t = (uint16_t)((a & 0x0Fu) << 4);
    seg = (uint8_t)((a & 0x70u) >> 4);
    switch (seg)
    {
    case 0: t += 8u; break;
    case 1: t += 0x108u; break;
    default: t += 0x108u; t <<= (seg - 1u); break;
    }
    return (a & 0x80u) ? (int16_t)t : (int16_t)(-(int16_t)t);
}
static int16_t ulaw_decode(uint8_t u)
{
    int16_t t;
    u = (uint8_t)~u;
    t = (int16_t)(((u & 0x0Fu) << 3) + 0x84) << ((u & 0x70u) >> 4);
    return (u & 0x80u) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

/* IMA ADPCM -> PCM (mono, 4-bit nibbles, step table like WAV) */
static const int16_t adpcm_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t adpcm_index[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
static size_t adpcm_decode_block(const uint8_t *src, size_t src_bytes, int16_t *dst, size_t dst_cap)
{
    int32_t pred = 0;
    int32_t step = 7;
    int32_t idx = 0;
    size_t out = 0;
    size_t i;
    FM_Bool first = FM_TRUE;
    if (src_bytes >= 4u)
    {
        pred = (int16_t)(src[0] | (src[1] << 8));
        idx  = src[2];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        step = adpcm_step[idx];
        src += 4; src_bytes -= 4;
        if (out < dst_cap) dst[out++] = (int16_t)pred;
        first = FM_FALSE;
    }
    for (i = 0; i < src_bytes && out < dst_cap; i++)
    {
        uint8_t b = src[i];
        uint32_t n;
        for (n = 0; n < 2u && out < dst_cap; n++)
        {
            uint8_t nib = (n == 0u) ? (b & 0x0Fu) : (b >> 4);
            int32_t diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            if (nib & 8) pred -= diff; else pred += diff;
            if (pred < -32768) pred = -32768;
            if (pred >  32767) pred =  32767;
            idx += adpcm_index[nib & 0x0Fu];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            step = adpcm_step[idx];
            dst[out++] = (int16_t)pred;
            if (first) { first = FM_FALSE; }
        }
    }
    return out * 2u; /* bytes */
}

/* ------------------------------------------------------------------ */
/* MKV / WebM (EBML) — minimal but functional demux                    */
/* ------------------------------------------------------------------ */
static uint32_t ebml_id_len(uint8_t b)
{
    if (b & 0x80u) return 1u;
    if (b & 0x40u) return 2u;
    if (b & 0x20u) return 3u;
    if (b & 0x10u) return 4u;
    return 0u;
}
static uint32_t ebml_size_len(uint8_t b)
{
    if (b & 0x80u) return 1u;
    if (b & 0x40u) return 2u;
    if (b & 0x20u) return 3u;
    if (b & 0x10u) return 4u;
    if (b & 0x08u) return 5u;
    if (b & 0x04u) return 6u;
    if (b & 0x02u) return 7u;
    if (b & 0x01u) return 8u;
    return 0u;
}
static FM_Result ebml_read(FM_Video *v, uint64_t pos, uint64_t file_end,
                           uint32_t *id, uint64_t *size, uint64_t *data_off)
{
    uint8_t hdr[8];
    uint32_t ilen, slen;
    uint32_t i;
    uint64_t sz = 0;
    if (pos + 2u > file_end) return FM_ERR_CORRUPTED;
    if (video_read_at(&v->file, pos, hdr, 1) != FM_OK) return FM_ERR_CORRUPTED;
    ilen = ebml_id_len(hdr[0]);
    if (ilen == 0u || pos + ilen + 1u > file_end) return FM_ERR_CORRUPTED;
    if (ilen > 1u && video_read_at(&v->file, pos, hdr, ilen + 1u) != FM_OK) return FM_ERR_CORRUPTED;
    *id = 0;
    for (i = 0; i < ilen; i++) *id = (*id << 8) | hdr[i];
    slen = ebml_size_len(hdr[ilen]);
    if (slen == 0u) return FM_ERR_CORRUPTED;
    if (ilen + slen > 8u)
    {
        uint8_t ext[8];
        if (video_read_at(&v->file, pos + ilen, ext, slen) != FM_OK) return FM_ERR_CORRUPTED;
        for (i = 0; i < slen; i++) hdr[i] = ext[i];
    }
    else
    {
        /* already have first size byte in hdr[ilen] */
    }
    /* strip leading 1 */
    uint8_t mask = (uint8_t)(0xFFu >> slen);
    /* for slen bytes, first byte's leading 1 + zeros removed */
    /* Actually first size byte already includes the marker, so mask it */
    sz = hdr[ilen] & mask;
    for (i = 1; i < slen; i++) sz = (sz << 8) | hdr[ilen + i];
    if (sz == ( (slen == 8u) ? UINT64_MAX : ((1ULL << (7u * slen)) - 1ULL) )) sz = file_end - (pos + ilen + slen); /* unknown */
    *size = sz;
    *data_off = pos + ilen + slen;
    return FM_OK;
}
static FM_Result mkv_read_string(FM_Video *v, uint64_t off, uint64_t sz, char *out, size_t cap)
{
    if (sz >= cap) sz = cap - 1u;
    if (video_read_at(&v->file, off, out, (uint32_t)sz) != FM_OK) return FM_ERR_CORRUPTED;
    out[sz] = '\0';
    return FM_OK;
}
static FM_VideoCodec mkv_map_codec(const char *s)
{
    if (strstr(s, "V_MPEG4/ISO/AVC") || strstr(s, "V_MPEG4/ISO/SP") || strstr(s, "AVC")) return FM_VIDEO_CODEC_H264;
    if (strstr(s, "V_MJPEG") || strstr(s, "MJPEG")) return FM_VIDEO_CODEC_MJPEG;
    if (strstr(s, "V_VP8"))  return FM_VIDEO_CODEC_VP8;
    if (strstr(s, "V_VP9"))  return FM_VIDEO_CODEC_VP9;
    if (strstr(s, "V_AV1") || strstr(s, "AV1")) return FM_VIDEO_CODEC_AV1;
    if (strstr(s, "V_MPEGH/ISO/HEVC") || strstr(s, "HEVC") || strstr(s, "H265")) return FM_VIDEO_CODEC_HEVC;
    if (strstr(s, "V_MPEG2")) return FM_VIDEO_CODEC_MPEG2;
    if (strstr(s, "V_THEORA")) return FM_VIDEO_CODEC_THEORA;
    if (strstr(s, "V_UNCOMPRESSED")) return FM_VIDEO_CODEC_RAW;
    return FM_VIDEO_CODEC_UNKNOWN;
}
static FM_Result mkv_parse(FM_Video *v, FM_VideoPriv *priv)
{
    uint64_t file_size = 0;
    uint64_t seg_start = 0, seg_end = 0, seg_data = 0;
    uint32_t id; uint64_t sz, data;
    uint32_t width = 0, height = 0;
    uint64_t def_duration = 0; /* ns per frame */
    uint32_t vid_track = 0xFFFFFFFFu, aud_track = 0xFFFFFFFFu;
    uint32_t aud_rate = 0; uint16_t aud_ch = 0; uint32_t aud_codec = FM_VIDEO_AUDIO_NONE;
    FM_Result r;
    if (File_Size(&v->file, &file_size) != FM_OK) return FM_ERR_IO;
    /* EBML header at 0, then Segment */
    uint64_t pos = 0;
    while (pos < file_size)
    {
        if (ebml_read(v, pos, file_size, &id, &sz, &data) != FM_OK) break;
        if (id == 0x18538067u) { seg_start = pos; seg_data = data; seg_end = data + sz; break; }
        if (id == 0x1A45DFA3u) { /* EBML header, skip */ }
        pos = data + sz;
        if (pos <= seg_start) break;
    }
    if (seg_data == 0u) return FM_ERR_INVALID_FORMAT;
    /* 1st pass: Tracks */
    pos = seg_data;
    while (pos + 4 <= seg_end)
    {
        if (ebml_read(v, pos, seg_end, &id, &sz, &data) != FM_OK) break;
        if (id == 0x1654AE6Bu) /* Tracks */
        {
            uint64_t tpos = data, tend = data + sz;
            while (tpos + 4 <= tend)
            {
                uint32_t tid; uint64_t tsz, tdata;
                if (ebml_read(v, tpos, tend, &tid, &tsz, &tdata) != FM_OK) break;
                if (tid == 0xAEu) /* TrackEntry */
                {
                    uint64_t epos = tdata, eend = tdata + tsz;
                    uint32_t tnum = 0; uint32_t ttype = 0;
                    char codec[64] = {0};
                    uint32_t w = 0, h = 0;
                    uint64_t dur = 0;
                    double arate = 0; uint32_t ach = 0;
                    uint8_t  mkv_priv_tmp[512] = {0};
                    uint32_t mkv_priv_sz = 0;
                    while (epos + 2 <= eend)
                    {
                        uint32_t eid; uint64_t esz, edata;
                        if (ebml_read(v, epos, eend, &eid, &esz, &edata) != FM_OK) break;
                        if (eid == 0xD7u) { uint8_t b[4]; uint32_t n = (esz > 4u)?4u:(uint32_t)esz; if (video_read_at(&v->file, edata, b, n)==FM_OK){ tnum=0; for(uint32_t k=0;k<n;k++) tnum=(tnum<<8)|b[k]; } }
                        else if (eid == 0x83u) { uint8_t b; if (video_read_at(&v->file, edata, &b,1)==FM_OK) ttype=b; }
                        else if (eid == 0x86u) { mkv_read_string(v, edata, esz, codec, sizeof(codec)); }
                        else if (eid == 0x63A2u) { uint32_t take = (esz > sizeof(mkv_priv_tmp)) ? (uint32_t)sizeof(mkv_priv_tmp) : (uint32_t)esz; if (take>0u && video_read_at(&v->file, edata, mkv_priv_tmp, take)==FM_OK) mkv_priv_sz = take; }
                        else if (eid == 0x23E383u) { uint8_t b[8]; uint32_t n=(esz>8u)?8u:(uint32_t)esz; if(video_read_at(&v->file,edata,b,n)==FM_OK){ dur=0; for(uint32_t k=0;k<n;k++) dur=(dur<<8)|b[k]; } }
                        else if (eid == 0xE0u) /* Video */
                        {
                            uint64_t vpos=edata, vend=edata+esz;
                            while(vpos+2<=vend){ uint32_t vid; uint64_t vsz,vdata; if(ebml_read(v,vpos,vend,&vid,&vsz,&vdata)!=FM_OK) break; if(vid==0xB0u){ uint8_t b[4]; if(video_read_at(&v->file,vdata,b,(vsz>4?4:(uint32_t)vsz))==FM_OK){ w=0; for(uint32_t k=0;k<vsz&&k<4;k++) w=(w<<8)|b[k]; } } else if(vid==0xBAu){ uint8_t b[4]; if(video_read_at(&v->file,vdata,b,(vsz>4?4:(uint32_t)vsz))==FM_OK){ h=0; for(uint32_t k=0;k<vsz&&k<4;k++) h=(h<<8)|b[k]; } } vpos=vdata+vsz; }
                        }
                        else if (eid == 0xE1u) /* Audio */
                        {
                            uint64_t apos=edata, aend=edata+esz;
                            while(apos+2<=aend){ uint32_t aid; uint64_t asz,adata; if(ebml_read(v,apos,aend,&aid,&asz,&adata)!=FM_OK) break; if(aid==0xB5u){ uint8_t b[8]; if(video_read_at(&v->file,adata,b,(asz>8?8:(uint32_t)asz))==FM_OK){ uint64_t u=0; for(uint32_t k=0;k<asz&&k<8;k++) u=(u<<8)|b[k]; uint64_t le=0; for(int k=0;k<8;k++) le=(le<<8)|((u>>(k*8))&0xFF); double d; memcpy(&d,&le,8); arate=d; } } else if(aid==0x9Fu){ uint8_t b; if(video_read_at(&v->file,adata,&b,1)==FM_OK) ach=b; } apos=adata+asz; }
                        }
                        epos = edata + esz;
                    }
                    if (ttype==1u && vid_track==0xFFFFFFFFu)
                    {
                        vid_track = tnum;
                        if (w) { width=w; } if (h) { height=h; }
                        if (dur) def_duration=dur;
                        FM_VideoCodec c = mkv_map_codec(codec);
                        if (c != FM_VIDEO_CODEC_UNKNOWN) v->info.codec = c; else if (v->info.codec==FM_VIDEO_CODEC_UNKNOWN) v->info.codec = FM_VIDEO_CODEC_UNKNOWN;
                    }
                    else if (ttype==2u && aud_track==0xFFFFFFFFu)
                    {
                        aud_track = tnum;
                        if (arate) { aud_rate=(uint32_t)arate; } if (ach) { aud_ch=(uint16_t)ach; }
                        if (strstr(codec,"A_PCM")) aud_codec=FM_VIDEO_AUDIO_PCM;
                        else if (strstr(codec,"A_MPEG/L3")||strstr(codec,"MP3")) aud_codec=FM_VIDEO_AUDIO_MP3;
                        else if (strstr(codec,"A_FLAC")) aud_codec=FM_VIDEO_AUDIO_FLAC;
                        else if (strstr(codec,"A_OPUS")) aud_codec=FM_VIDEO_AUDIO_OPUS;
                        else if (strstr(codec,"A_VORBIS")) aud_codec=FM_VIDEO_AUDIO_VORBIS;
                        else if (strstr(codec,"A_AAC")) aud_codec=FM_VIDEO_AUDIO_AAC;
                        else if (strstr(codec,"A_MS/ACM")) aud_codec=FM_VIDEO_AUDIO_PCM;
                        if (mkv_priv_sz>0u && mkv_priv_sz<=sizeof(priv->audio_private))
                        {
                            memcpy(priv->audio_private, mkv_priv_tmp, mkv_priv_sz);
                            priv->audio_private_size = mkv_priv_sz;
                        }
                    }
                }
                tpos = tdata + tsz;
            }
        }
        pos = data + sz;
    }
    if (width==0u) width=320u;
    if (height==0u) height=240u;
    v->info.width = (uint16_t)width;
    v->info.height = (uint16_t)height;
    if (def_duration) v->info.fps = (uint32_t)(1000000000ULL / def_duration);
    if (v->info.fps==0u) v->info.fps = 25u;
    if (v->info.codec==0) v->info.codec = FM_VIDEO_CODEC_UNKNOWN;
    (void)strcpy(v->info.container, "MKV");
    if (aud_track!=0xFFFFFFFFu)
    {
        priv->audio_present = FM_TRUE;
        priv->audio_codec = aud_codec ? aud_codec : FM_VIDEO_AUDIO_PCM;
        priv->audio_rate = aud_rate ? aud_rate : 44100u;
        priv->audio_channels = aud_ch ? aud_ch : 2u;
        priv->audio_bits = 16u;
    }
    /* 2nd pass: Clusters -> Blocks */
    priv->count = 0; priv->audio_count = 0;
    pos = seg_data;
    while (pos + 4 <= seg_end && priv->count < FM_VIDEO_MAX_FRAMES)
    {
        if (ebml_read(v, pos, seg_end, &id, &sz, &data) != FM_OK) break;
        if (id == 0x1F43B675u) /* Cluster */
        {
            uint64_t cpos = data, cend = data + sz;
            while (cpos + 4 <= cend && priv->count < FM_VIDEO_MAX_FRAMES)
            {
                uint32_t cid; uint64_t csz, cdata;
                if (ebml_read(v, cpos, cend, &cid, &csz, &cdata) != FM_OK) break;
                if (cid == 0xA3u || cid == 0xA0u) /* SimpleBlock / BlockGroup */
                {
                    uint64_t bdata = cdata;
                    uint64_t bend = cdata + csz;
                    uint32_t bid = cid;
                    if (bid == 0xA0u) /* BlockGroup -> find Block 0xA1 */
                    {
                        uint64_t gpos=bdata;
                        FM_Bool found=FM_FALSE;
                        while(gpos+4<=bend){ uint32_t gid; uint64_t gsz,gdata; if(ebml_read(v,gpos,bend,&gid,&gsz,&gdata)!=FM_OK) break; if(gid==0xA1u){ bdata=gdata; csz=gsz; found=FM_TRUE; break; } gpos=gdata+gsz; }
                        if(!found){ cpos=cdata+csz; continue; }
                    }
                    /* SimpleBlock header: trackNum varint (1-2 bytes), timecode 2, flags 1, then frame(s) */
                    uint8_t hdr[4];
                    if (video_read_at(&v->file, bdata, hdr, 2) != FM_OK) { cpos = cdata + csz; continue; }
                    uint32_t tnum = 0; uint32_t hlen = 0;
                    if (hdr[0] & 0x80u) { tnum = hdr[0] & 0x7Fu; hlen = 1+2+1; }
                    else if (hdr[0] & 0x40u) { uint8_t b2; video_read_at(&v->file,bdata+1,&b2,1); tnum = ((hdr[0]&0x3Fu)<<8)|b2; hlen=2+2+1; }
                    else { cpos=cdata+csz; continue; }
                    /* lacing per Matroska spec */
                    uint8_t flags = 0; video_read_at(&v->file,bdata+hlen-1,&flags,1);
                    uint8_t lace = (flags & 0x06u) >> 1;
                    uint64_t payload_off = bdata + hlen;
                    uint32_t payload_sz = (uint32_t)(csz - hlen);
                    if (lace == 0u)
                    {
                        if (payload_sz > 0u)
                        {
                            if (tnum == vid_track && priv->count < FM_VIDEO_MAX_FRAMES)
                            {
                                priv->frames[priv->count].offset = payload_off;
                                priv->frames[priv->count].size = payload_sz;
                                priv->count++;
                            }
                            else if (tnum == aud_track && priv->audio_present && priv->audio_count < FM_VIDEO_MAX_FRAMES)
                            {
                                priv->audio_frames[priv->audio_count].offset = payload_off;
                                priv->audio_frames[priv->audio_count].size = payload_sz;
                                priv->audio_count++;
                            }
                        }
                    }
                    else
                    {
                        uint8_t lace_cnt = 0;
                        if (payload_sz < 1u || video_read_at(&v->file,payload_off,&lace_cnt,1)!=FM_OK) { cpos=cdata+csz; continue; }
                        uint32_t frames = (uint32_t)lace_cnt + 1u;
                        if (frames == 0u || frames > 32u) { cpos=cdata+csz; continue; }
                        uint64_t lace_hdr = 1u;
                        uint32_t sizes[32] = {0};
                        uint32_t total_laced = 0u;
                        uint32_t k;
                        if (lace == 1u) /* Xiph */
                        {
                            for (k=0;k<frames-1u;k++)
                            {
                                uint32_t szv=0u;
                                for (;;)
                                {
                                    uint8_t b=0; if (lace_hdr>=payload_sz || video_read_at(&v->file,payload_off+lace_hdr,&b,1)!=FM_OK) break;
                                    lace_hdr++; szv += b; if (b!=255u) break;
                                }
                                sizes[k]=szv; total_laced+=szv;
                            }
                            sizes[frames-1u] = (payload_sz >= lace_hdr + total_laced) ? (payload_sz - (uint32_t)lace_hdr - total_laced) : 0u;
                        }
                        else if (lace == 2u) /* Fixed */
                        {
                            uint32_t each = (payload_sz - (uint32_t)lace_hdr) / frames;
                            for (k=0;k<frames;k++) sizes[k]=each;
                        }
                        else /* 3 = EBML */
                        {
                            uint64_t off = lace_hdr;
                            uint64_t first = 0;
                            /* read EBML unsigned */
                            {
                                uint8_t b = 0;
                                if (off >= payload_sz || video_read_at(&v->file, payload_off + off, &b, 1) != FM_OK) { cpos = cdata + csz; continue; }
                                uint32_t llen = ebml_size_len(b);
                                if (llen == 0u || off + llen > payload_sz) { cpos = cdata + csz; continue; }
                                uint8_t buf[8] = {0};
                                video_read_at(&v->file, payload_off + off, buf, llen);
                                uint8_t m = (uint8_t)(0xFFu >> llen);
                                uint64_t val = buf[0] & m;
                                for (uint32_t ii = 1; ii < llen; ii++) val = (val << 8) | buf[ii];
                                first = val; off += llen;
                            }
                            sizes[0] = (uint32_t)first; total_laced += sizes[0]; lace_hdr = (uint32_t)off;
                            int64_t prev = (int64_t)first;
                            for (k = 1; k < frames - 1u; k++)
                            {
                                uint8_t b = 0;
                                if (off >= payload_sz || video_read_at(&v->file, payload_off + off, &b, 1) != FM_OK) break;
                                uint32_t llen = ebml_size_len(b);
                                if (llen == 0u || off + llen > payload_sz) break;
                                uint8_t buf[8] = {0};
                                video_read_at(&v->file, payload_off + off, buf, llen);
                                uint8_t m = (uint8_t)(0xFFu >> llen);
                                uint64_t raw = buf[0] & m;
                                for (uint32_t ii = 1; ii < llen; ii++) raw = (raw << 8) | buf[ii];
                                off += llen;
                                /* EBML signed: value = raw - (1 << (7*len-1)) +1  ; use raw bias */
                                int64_t bias = (int64_t)(1ULL << (7u * llen - 1u)) - 1LL;
                                int64_t diff = (int64_t)raw - bias;
                                int64_t cur = prev + diff;
                                if (cur < 0) cur = 0;
                                sizes[k] = (uint32_t)cur; prev = cur;
                                total_laced += sizes[k];
                            }
                            lace_hdr = (uint32_t)off;
                            sizes[frames - 1u] = (payload_sz >= lace_hdr + total_laced) ? (payload_sz - lace_hdr - total_laced) : 0u;
                        }
                        uint64_t cur = payload_off + lace_hdr;
                        for (k=0;k<frames;k++)
                        {
                            uint32_t fsz2 = sizes[k];
                            if (fsz2==0u || cur+fsz2> payload_off+payload_sz) break;
                            if (tnum == vid_track && priv->count < FM_VIDEO_MAX_FRAMES)
                            {
                                priv->frames[priv->count].offset = cur;
                                priv->frames[priv->count].size = fsz2;
                                priv->count++;
                            }
                            else if (tnum == aud_track && priv->audio_present && priv->audio_count < FM_VIDEO_MAX_FRAMES)
                            {
                                priv->audio_frames[priv->audio_count].offset = cur;
                                priv->audio_frames[priv->audio_count].size = fsz2;
                                priv->audio_count++;
                            }
                            cur += fsz2;
                            if (priv->count >= FM_VIDEO_MAX_FRAMES) break;
                        }
                    }
                }
                cpos = cdata + csz;
            }
        }
        pos = data + sz;
    }
    if (priv->count==0u) return FM_ERR_INVALID_FORMAT;
    v->info.frame_count = priv->count;
    v->info.data_offset = priv->frames[0].offset;
    v->info.frame_size = 0;
    v->variable = 1;
    r = Video_Seek(v,0);
    return r;
}

/* ------------------------------------------------------------------ */
/* FLV — Flash Video (FLV1) minimal demux                             */
/* ------------------------------------------------------------------ */
static FM_Result flv_parse(FM_Video *v, FM_VideoPriv *priv)
{
    uint64_t file_size=0;
    uint8_t hdr[13];
    uint64_t pos;
    uint32_t width=0,height=0;
    uint32_t fps = 25u;
    FM_VideoCodec vcodec = FM_VIDEO_CODEC_UNKNOWN;
    uint32_t aud_codec = FM_VIDEO_AUDIO_NONE;
    uint32_t aud_rate=44100u; uint16_t aud_ch=2u;
    if (File_Size(&v->file,&file_size)!=FM_OK) return FM_ERR_IO;
    if (video_read_at(&v->file,0,hdr,13)!=FM_OK) return FM_ERR_CORRUPTED;
    if (memcmp(hdr,"FLV",3)!=0) return FM_ERR_INVALID_FORMAT;
    pos = 13;
    /* previousTagSize0 already in hdr[9..12], pos already after */
    while (pos + 15 <= file_size && priv->count < FM_VIDEO_MAX_FRAMES)
    {
        uint8_t th[11];
        uint32_t dsz, ts;
        uint8_t type;
        if (video_read_at(&v->file,pos,th,11)!=FM_OK) break;
        type = th[0];
        dsz = ((uint32_t)th[1]<<16)|((uint32_t)th[2]<<8)|th[3];
        ts = ((uint32_t)th[4]<<16)|((uint32_t)th[5]<<8)|th[6]|((uint32_t)th[7]<<24);
        (void)ts;
        uint64_t data = pos+11;
        uint64_t next = data + dsz + 4; /* + PreviousTagSize */
        if (next > file_size) break;
        if (type==9u) /* video */
        {
            uint8_t ftCodec;
            if (dsz>=1 && video_read_at(&v->file,data,&ftCodec,1)==FM_OK)
            {
                uint8_t cid = ftCodec & 0x0Fu;
                if (cid==7u) vcodec = FM_VIDEO_CODEC_H264;
                else if (cid==12u) vcodec = FM_VIDEO_CODEC_HEVC;
                else if (cid==2u||cid==3u) vcodec = FM_VIDEO_CODEC_MPEG4;
                else if (cid==1u) vcodec = FM_VIDEO_CODEC_MJPEG;
                else vcodec = FM_VIDEO_CODEC_UNKNOWN;
                if (vcodec != FM_VIDEO_CODEC_UNKNOWN)
                {
                    uint8_t avcType=0;
                    if (cid==7u && dsz>=2) video_read_at(&v->file,data+1,&avcType,1);
                    if (!(cid==7u && avcType==0u)) /* skip sequence header */
                    {
                        /* data offset after FLV video tag header (1 + 1 +3) for AVC */
                        uint64_t f_off = data + ((cid==7u)?5u:1u);
                        uint32_t f_sz = dsz - ((cid==7u)?5u:1u);
                        if (f_sz>0u)
                        {
                            priv->frames[priv->count].offset = f_off;
                            priv->frames[priv->count].size = f_sz;
                            priv->count++;
                        }
                    }
                    else if (cid==7u && avcType==0u)
                    {
                        /* parse avcC to get SPS width/height: reuse mp4 logic quickly */
                        /* avcC starts at data+5 */
                        uint64_t avcOff = data+5;
                        uint8_t avcHdr[6];
                        if (dsz>= 11 && video_read_at(&v->file,avcOff,avcHdr,6)==FM_OK)
                        {
                            /* try to extract SPS like mp4_parse_avcc would but we just store */
                            /* width/height unknown here, keep default */
                        }
                    }
                }
            }
        }
        else if (type==8u) /* audio */
        {
            uint8_t snd;
            if (dsz>=1 && video_read_at(&v->file,data,&snd,1)==FM_OK)
            {
                uint8_t fmt = snd>>4;
                uint8_t rateIdx = (snd>>2)&0x03u;
                uint8_t isStereo = snd & 0x01u;
                uint8_t aacType=0;
                if (fmt==10u && dsz>=2) video_read_at(&v->file,data+1,&aacType,1);
                if (fmt==2u) aud_codec=FM_VIDEO_AUDIO_MP3;
                else if (fmt==10u) aud_codec=FM_VIDEO_AUDIO_AAC;
                else if (fmt==0u) { aud_codec=FM_VIDEO_AUDIO_PCM; aud_ch = isStereo?2u:1u; }
                else if (fmt==1u) aud_codec=FM_VIDEO_AUDIO_ADPCM;
                else if (fmt==6u) aud_codec=FM_VIDEO_AUDIO_ALAW;
                else if (fmt==7u) aud_codec=FM_VIDEO_AUDIO_ULAW;
                static const uint32_t rates[4]={5500,11025,22050,44100};
                aud_rate = rates[rateIdx &0x03u];
                aud_ch = isStereo?2u:1u;
                if (!(fmt==10u && aacType==0u))
                {
                    uint64_t f_off = data + ((fmt==10u)?2u:1u);
                    uint32_t f_sz = dsz - ((fmt==10u)?2u:1u);
                    if (f_sz>0u && priv->audio_count < FM_VIDEO_MAX_FRAMES)
                    {
                        priv->audio_frames[priv->audio_count].offset = f_off;
                        priv->audio_frames[priv->audio_count].size = f_sz;
                        priv->audio_count++;
                    }
                }
            }
        }
        pos = next;
        (void)fps;
    }
    if (priv->count==0u) return FM_ERR_INVALID_FORMAT;
    v->info.width = width? (uint16_t)width : 320u;
    v->info.height = height? (uint16_t)height : 240u;
    v->info.fps = fps;
    v->info.codec = (vcodec==FM_VIDEO_CODEC_UNKNOWN)? FM_VIDEO_CODEC_H264 : vcodec;
    (void)strcpy(v->info.container,"FLV");
    v->info.frame_count = priv->count;
    v->info.data_offset = priv->frames[0].offset;
    v->info.frame_size = 0; v->variable = 1;
    if (priv->audio_count>0u)
    {
        priv->audio_present = FM_TRUE;
        priv->audio_codec = aud_codec?aud_codec:FM_VIDEO_AUDIO_AAC;
        priv->audio_rate = aud_rate;
        priv->audio_channels = aud_ch;
        priv->audio_bits = (aud_codec==FM_VIDEO_AUDIO_PCM || aud_codec==FM_VIDEO_AUDIO_ALAW || aud_codec==FM_VIDEO_AUDIO_ULAW || aud_codec==FM_VIDEO_AUDIO_ADPCM)?16u:0u;
    }
    return Video_Seek(v,0);
}

/* ------------------------------------------------------------------ */
/* Open / close / info                                                 */
/* ------------------------------------------------------------------ */
FM_Result Video_Open(const char *path, FM_Video *v)
{
    uint8_t magic[12];
    uint64_t size = 0;
    FM_Result r;
    FM_VideoPriv *priv;

    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(v, FM_ERR_INVALID_ARGUMENT);
    memset(v, 0, sizeof(*v));
    File_Init(&v->file);
    r = File_OpenEx(&v->file, path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        FM_ERR_LOG("video open %s: file open failed (%d)", path, (int)r);
        return r;
    }
    if (File_Size(&v->file, &size) != FM_OK || size < 12)
    {
        FM_ERR_LOG("video open %s: too small (%lu bytes)", path,
                   (unsigned long)size);
        (void)File_Close(&v->file);
        return FM_ERR_INVALID_FORMAT;
    }
    if (video_read_at(&v->file, 0, magic, sizeof(magic)) != FM_OK)
    {
        (void)File_Close(&v->file);
        return FM_ERR_INVALID_FORMAT;
    }

    if (memcmp(magic, "FMRV", 4) == 0)
    {
        r = fmrv_parse(v);
    }
    else if (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "AVI ", 4) == 0)
    {
        priv = video_priv_alloc();
        if (priv == NULL)
        {
            (void)File_Close(&v->file);
            return FM_ERR_NO_MEMORY;
        }
        v->priv = priv;
        r = avi_parse(v, priv);
    }
    else if (memcmp(magic + 4, "ftyp", 4) == 0)
    {
        priv = video_priv_alloc();
        if (priv == NULL)
        {
            (void)File_Close(&v->file);
            return FM_ERR_NO_MEMORY;
        }
        v->priv = priv;
        r = mp4_parse(v, priv);
    }
    else if (magic[0]==0x1Au && magic[1]==0x45u && magic[2]==0xDFu && magic[3]==0xA3u)
    {
        priv = video_priv_alloc();
        if (priv == NULL) { (void)File_Close(&v->file); return FM_ERR_NO_MEMORY; }
        v->priv = priv;
        r = mkv_parse(v, priv);
    }
    else if (memcmp(magic, "FLV", 3) == 0)
    {
        priv = video_priv_alloc();
        if (priv == NULL) { (void)File_Close(&v->file); return FM_ERR_NO_MEMORY; }
        v->priv = priv;
        r = flv_parse(v, priv);
    }
    else
    {
        FM_ERR_LOG("video open %s: unknown container", path);
        (void)File_Close(&v->file);
        return FM_ERR_UNSUPPORTED;
    }

    if (r != FM_OK)
    {
        FM_ERR_LOG("video open %s: parse failed (%d)", path, (int)r);
        (void)File_Close(&v->file);
        video_priv_free((FM_VideoPriv *)v->priv);
        v->priv = NULL;
    }
    else
    {
        FM_EVENT_LOG("opened %s (%ux%u, %.1f fps, %u frames)", path,
                     (unsigned)v->width, (unsigned)v->height,
                     (double)v->fps, (unsigned)v->frame_count);
    }
    return r;
}

FM_Result Video_Close(FM_Video *v)
{
    FM_Result r = FM_OK;

    if (v == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (v->codec_plugin != NULL && v->codec_ctx != NULL &&
        v->codec_plugin->close != NULL)
    {
        v->codec_plugin->close(v->codec_ctx);
        v->codec_ctx = NULL;
    }
    if (File_IsOpen(&v->file))
    {
        r = File_Close(&v->file);
    }
    video_priv_free((FM_VideoPriv *)v->priv);
    memset(v, 0, sizeof(*v));
    return r;
}

FM_Result Video_GetInfo(const FM_Video *v, FM_VideoInfo *info)
{
    if (v == NULL || info == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *info = v->info;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Frame reading (variable-size containers + Annex-B conversion)       */
/* ------------------------------------------------------------------ */
static FM_Result video_read_annexb(FM_File *f, uint64_t offset, uint32_t size,
                                   uint8_t *out, size_t cap, size_t *out_len)
{
    /* Converts length-prefixed (AVCC) NAL units to Annex-B in-place.
     * The 4-byte length is replaced with a 4-byte start code, so the
     * output size equals the input size. */
    uint8_t hdr[4];
    uint32_t pos = 0;
    static const uint8_t start[] = { 0, 0, 0, 1 };

    if (size > cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    while (pos + 4 <= size)
    {
        uint32_t nlen;
        uint32_t step;

        if (video_read_at(f, offset + pos, hdr, 4) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        nlen = video_be32(hdr);
        if (nlen == 0 || pos + 4 + nlen > size)
        {
            return FM_ERR_CORRUPTED;
        }
        memcpy(out + pos, start, 4);
        if (video_read_at(f, offset + pos + 4, out + pos + 4, nlen) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        step = 4 + nlen;
        pos += step;
    }
    *out_len = size;
    return FM_OK;
}

FM_Result Video_ReadFrame(FM_Video *v, void *frame_out, size_t frame_cap,
                          size_t *size)
{
    FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;
    uint64_t offset;
    uint32_t fsize;
    FM_Result r;

    if (v == NULL || frame_out == NULL || size == NULL ||
        !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *size = 0;
    if (v->current_frame >= v->info.frame_count)
    {
        return FM_ERR_EOF;
    }

    if (!v->variable)
    {
        /* FMRV fixed-size frames.  Read in slices with FM_Idle between
         * them: one raw 320x240 frame is 230 KB (~20 ms of SD time) and
         * the audio ring must keep refilling during the transfer. */
        uint32_t left = v->info.frame_size;
        uint8_t *dst = (uint8_t *)frame_out;
        uint64_t off;

        if (v->info.frame_size > frame_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        /*
         * Absolute offsets, not sequential reads.
         *
         * FM_Idle() below lets the application refill its audio ring,
         * and for a file with sound that reads PCM from THIS SAME
         * FM_File -- one shared cursor.  A sequential read would resume
         * wherever the audio demuxer left the cursor, so every slice
         * after the first landed on PCM bytes.  Silent files were fine
         * because nothing moved the cursor: exactly the reported
         * symptom.  Seeking per slice makes the read immune to it.
         */
        if (File_Tell(&v->file, &off) != FM_OK)
        {
            return FM_ERR_IO;
        }
        while (left > 0u)
        {
            uint32_t slice = (left > 32768u) ? 32768u : left;

            r = video_read_at(&v->file, off, dst, slice);
            if (r != FM_OK)
            {
                return r;
            }
            off += slice;
            dst += slice;
            left -= slice;
            FM_Idle();
        }
        /* Leave the cursor where a sequential reader would expect it. */
        if (off <= (uint64_t)INT64_MAX)
        {
            (void)File_Seek(&v->file, (int64_t)off, FM_SEEK_SET);
        }
        *size = v->info.frame_size;
        v->current_frame++;
        return FM_OK;
    }

    if (priv == NULL || v->current_frame >= priv->count)
    {
        return FM_ERR_CORRUPTED;
    }
    offset = priv->frames[v->current_frame].offset;
    fsize = priv->frames[v->current_frame].size;

    if (priv->avc1)
    {
        /* MP4 avc1: prefix SPS/PPS and convert length-prefixed NALs. */
        size_t need = priv->sps_pps_len + fsize;

        if (need > frame_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(frame_out, priv->sps_pps, priv->sps_pps_len);
        r = video_read_annexb(&v->file, offset, fsize,
                              (uint8_t *)frame_out + priv->sps_pps_len,
                              frame_cap - priv->sps_pps_len, size);
        if (r != FM_OK)
        {
            return r;
        }
        *size = priv->sps_pps_len + *size;
    }
    else if (v->info.codec == FM_VIDEO_CODEC_H264)
    {
        /* AVI H.264: detect start-code vs length-prefixed framing. */
        uint8_t hdr[4];

        if (fsize > frame_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        if (fsize >= 4 && video_read_at(&v->file, offset, hdr, 4) == FM_OK &&
            !(hdr[0] == 0 && hdr[1] == 0 &&
              (hdr[2] == 1 || (hdr[2] == 0 && hdr[3] == 1))))
        {
            r = video_read_annexb(&v->file, offset, fsize, (uint8_t *)frame_out,
                                  frame_cap, size);
            if (r != FM_OK)
            {
                return r;
            }
        }
        else
        {
            r = video_read_at(&v->file, offset, frame_out, fsize);
            if (r != FM_OK)
            {
                return r;
            }
            *size = fsize;
        }
    }
    else
    {
        /* MJPEG / raw DIB payloads.  Raw frames are hundreds of KB, so
         * read in slices with FM_Idle between them to keep the audio
         * ring fed during the SD transfer. */
        uint32_t left = fsize;
        uint8_t *dst = (uint8_t *)frame_out;
        uint64_t off = offset;

        if (fsize > frame_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        while (left > 0u)
        {
            uint32_t slice = (left > 32768u) ? 32768u : left;

            r = video_read_at(&v->file, off, dst, slice);
            if (r != FM_OK)
            {
                return r;
            }
            off += slice;
            dst += slice;
            left -= slice;
            if (left > 0u)
            {
                FM_Idle();
            }
        }
        *size = fsize;
    }
    v->current_frame++;
    return FM_OK;
}

FM_Result Video_DecodeFrame(FM_Video *v, void *pixels_out,
                            size_t pixels_cap)
{
    if (v == NULL || pixels_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* auto-register the matching codec so decode "just works":
     * hardware decoders take priority over the bundled software codecs */
    if (v->codec_plugin == NULL)
    {
        const FM_VideoCodecOps *hw = HWDecoder_Get(v->info.codec);

        if (hw != NULL)
        {
            FM_Result rr = Video_RegisterCodec(v, hw);

            if (rr != FM_OK)
            {
                return rr;
            }
        }
        else
        {
#if FM_USE_H264
            if (v->info.codec == FM_VIDEO_CODEC_H264)
            {
                FM_Result rr = Video_RegisterCodec(v, H264_GetCodecOps());

                if (rr != FM_OK)
                {
                    return rr;
                }
            }
#endif
#if FM_USE_MJPEG
            if (v->info.codec == FM_VIDEO_CODEC_MJPEG)
            {
                FM_Result rr = Video_RegisterCodec(v, MJPEG_GetCodecOps());

                if (rr != FM_OK)
                {
                    return rr;
                }
            }
#endif
        }
    }
    if (v->codec_plugin != NULL)
    {
        uint8_t *compressed;
        size_t cap = 0;
        size_t size = 0;
        FM_Result r;
        FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;

        /* End of stream is not an error: report EOF, matching the
         * raw-codec path (Video_ReadFrame). */
        if (v->current_frame >= v->info.frame_count)
        {
            return FM_ERR_EOF;
        }
        if (!v->variable)
        {
            cap = v->info.frame_size;
        }
        else if (priv != NULL && v->current_frame < priv->count)
        {
            cap = priv->frames[v->current_frame].size;
            if (priv->avc1)
            {
                cap += priv->sps_pps_len;
            }
        }
        if (cap == 0u)
        {
            return FM_ERR_CORRUPTED;
        }
        compressed = (uint8_t *)FM_Alloc(cap);
        if (compressed == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        r = Video_ReadFrame(v, compressed, cap, &size);
        if (r == FM_OK)
        {
            r = v->codec_plugin->decode_frame(v->codec_ctx, compressed, size,
                                              pixels_out, pixels_cap);
        }
        FM_Free(compressed);
        return r;
    }
    if (v->info.codec != FM_VIDEO_CODEC_RAW)
    {
        return FM_ERR_UNSUPPORTED;
    }
    {
        size_t size = 0;
        FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;
        FM_Result r = Video_ReadFrame(v, pixels_out, pixels_cap, &size);

        if (r != FM_OK)
        {
            return r;
        }
        /* AVI uncompressed DIB frames are BGR with 4-byte padded rows,
         * stored bottom-up unless biHeight was negative.  Convert
         * in-place to tightly packed top-down RGB888. */
        if (priv != NULL && priv->raw_bgr &&
            v->info.width != 0u && v->info.height != 0u)
        {
            uint8_t *p = (uint8_t *)pixels_out;
            size_t row_used = (size_t)v->info.width * 3u;
            size_t stride = (row_used + 3u) & ~(size_t)3u;
            uint32_t rows = v->info.height;
            uint32_t y;
            size_t x;

            if (size < stride * rows)
            {
                return FM_ERR_CORRUPTED;
            }
            /* Drop the row padding (compact rows in-place). */
            if (stride != row_used)
            {
                for (y = 1; y < rows; y++)
                {
                    memmove(p + (size_t)y * row_used,
                            p + (size_t)y * stride, row_used);
                }
            }
            /* BGR -> RGB */
            for (y = 0; y < rows; y++)
            {
                uint8_t *rp = p + (size_t)y * row_used;

                for (x = 0; x < row_used; x += 3u)
                {
                    uint8_t t = rp[x];

                    rp[x] = rp[x + 2u];
                    rp[x + 2u] = t;
                }
            }
            /* Bottom-up -> top-down (swap rows) unless already top-down. */
            if (!priv->raw_top_down)
            {
                for (y = 0; y < rows / 2u; y++)
                {
                    uint8_t *a = p + (size_t)y * row_used;
                    uint8_t *b = p + (size_t)(rows - 1u - y) * row_used;

                    for (x = 0; x < row_used; x++)
                    {
                        uint8_t t = a[x];

                        a[x] = b[x];
                        b[x] = t;
                    }
                }
            }
            FM_Idle();
        }
        return r;
    }
}

FM_Result Video_DecodeFrameAs(FM_Video *v, FM_PixelFormat output_format,
                              void *pixels_out, size_t pixels_cap,
                              void *rgb_work, size_t rgb_work_cap)
{
#if !FM_USE_IMAGE
    (void)v; (void)output_format; (void)pixels_out; (void)pixels_cap;
    (void)rgb_work; (void)rgb_work_cap;
    return FM_ERR_UNSUPPORTED;
#else
    FM_Image source;
    FM_Image destination;
    size_t rgb_size;
    size_t output_size;
    FM_Result r;

    if (v == NULL || pixels_out == NULL || v->info.width == 0 ||
        v->info.height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_BufferSize(v->info.width, v->info.height, FM_PIXEL_RGB888,
                         &rgb_size);
    if (r != FM_OK) return r;
    r = Image_BufferSize(v->info.width, v->info.height, output_format,
                         &output_size);
    if (r != FM_OK) return r;
    if (pixels_cap < output_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (output_format == FM_PIXEL_RGB888)
    {
        return Video_DecodeFrame(v, pixels_out, pixels_cap);
    }
    if (rgb_work == NULL || rgb_work == pixels_out || rgb_work_cap < rgb_size)
    {
        return FM_ERR_TOO_LARGE;
    }

    r = Video_DecodeFrame(v, rgb_work, rgb_work_cap);
    if (r != FM_OK) return r;

    memset(&source, 0, sizeof(source));
    memset(&destination, 0, sizeof(destination));
    source.width = v->info.width;
    source.height = v->info.height;
    source.format = FM_PIXEL_RGB888;
    source.channels = 3;
    source.bit_depth = 24;
    source.data = (uint8_t *)rgb_work;
    source.data_capacity = rgb_work_cap;
    source.data_size = rgb_size;
    destination.data = (uint8_t *)pixels_out;
    destination.data_capacity = pixels_cap;
    return Image_Convert(&source, &destination, output_format);
#endif
}

FM_Result Video_DecodeFrameScaled(FM_Video *v, void *pixels_out,
                                  size_t pixels_cap, uint16_t out_w,
                                  uint16_t out_h, FM_ScaleMode mode)
{
    uint16_t w;
    uint16_t h;
    size_t native;
    uint8_t *scratch;
    FM_Result r;

#if !FM_USE_IMAGE
    (void)mode;
    return FM_ERR_UNSUPPORTED;
#else
    if (v == NULL || pixels_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    w = v->info.width;
    h = v->info.height;
    if (w == 0 || h == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (out_w == 0)
    {
        out_w = w;
    }
    if (out_h == 0)
    {
        out_h = h;
    }
    if ((size_t)out_w * out_h * 3u > pixels_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    native = (size_t)w * h * 3u;
    scratch = (uint8_t *)FM_Alloc(native);
    if (scratch == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = Video_DecodeFrame(v, scratch, native);
    if (r == FM_OK)
    {
        r = Image_Scale(scratch, w, h, FM_PIXEL_RGB888, pixels_out,
                        out_w, out_h, mode);
    }
    FM_Free(scratch);
    return r;
#endif
}

FM_Result Video_Seek(FM_Video *v, uint32_t frame_index)
{
    if (v == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (frame_index >= v->info.frame_count)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (!v->variable)
    {
        uint64_t offset = v->info.data_offset +
                          (uint64_t)frame_index * v->info.frame_size;

        if (offset > INT64_MAX || File_Seek(&v->file, (int64_t)offset,
                                            FM_SEEK_SET) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    else
    {
        FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;
        uint64_t offset;

        if (priv == NULL || frame_index >= priv->count)
        {
            return FM_ERR_CORRUPTED;
        }
        offset = priv->frames[frame_index].offset;
        if (offset > INT64_MAX || File_Seek(&v->file, (int64_t)offset,
                                            FM_SEEK_SET) != FM_OK)
        {
            return FM_ERR_IO;
        }
    }
    v->current_frame = frame_index;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Embedded audio track (demux)                                        */
/* ------------------------------------------------------------------ */
static uint8_t adts_freq_index(uint32_t rate)
{
    switch (rate)
    {
    case 96000u: return 0;
    case 88200u: return 1;
    case 64000u: return 2;
    case 48000u: return 3;
    case 44100u: return 4;
    case 32000u: return 5;
    case 24000u: return 6;
    case 22050u: return 7;
    case 16000u: return 8;
    case 12000u: return 9;
    case 11025u: return 10;
    case 8000u:  return 11;
    case 7350u:  return 12;
    default:     return 0xFF;
    }
}

static FM_Result adts_build(uint8_t *h, uint32_t rate, uint16_t ch,
                            uint32_t frame_len)
{
    uint8_t fi = adts_freq_index(rate);
    uint32_t total = frame_len + 7u;

    if (fi == 0xFF || ch == 0 || ch > 7 || total > 0x1FFFu)
    {
        return FM_ERR_UNSUPPORTED;
    }
    h[0] = 0xFF;
    h[1] = 0xF1; /* MPEG-4, AAC-LC, no CRC */
    h[2] = (uint8_t)(((uint32_t)fi << 2) | ((uint32_t)ch >> 2));
    h[3] = (uint8_t)((((uint32_t)ch & 3u) << 6) | ((total >> 11) & 3u));
    h[4] = (uint8_t)((total >> 3) & 0xFFu);
    h[5] = (uint8_t)(((total & 7u) << 5) | 0x1Fu);
    h[6] = 0xFC;
    return FM_OK;
}

FM_Result Video_HasAudio(const FM_Video *v, FM_Bool *has)
{
    if (v == NULL || has == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *has = (((const FM_VideoPriv *)v->priv != NULL) &&
            ((const FM_VideoPriv *)v->priv)->audio_present) ? FM_TRUE : FM_FALSE;
    return FM_OK;
}

FM_Result Video_GetAudioInfo(const FM_Video *v, FM_AudioTrackInfo *info)
{
    const FM_VideoPriv *priv;

    if (v == NULL || info == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    priv = (const FM_VideoPriv *)v->priv;
    if (priv == NULL || !priv->audio_present)
    {
        return FM_ERR_UNSUPPORTED;
    }
    memset(info, 0, sizeof(*info));
    info->codec = (FM_VideoAudioCodec)priv->audio_codec;
    info->sample_rate = priv->audio_rate;
    info->channels = priv->audio_channels;
    info->bits_per_sample = priv->audio_bits;
    info->sample_count = (priv->audio_total > priv->audio_count)
                             ? priv->audio_total : priv->audio_count;
    return FM_OK;
}

FM_Result Video_ReadAudioFrame(FM_Video *v, uint8_t *buf, size_t cap,
                               size_t *size)
{
    FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;
    uint64_t offset;
    uint32_t fsize;
    FM_Result r;

    if (v == NULL || buf == NULL || size == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    *size = 0;
    if (priv == NULL || !priv->audio_present)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (priv->audio_part_left != 0u &&
        priv->audio_codec == FM_VIDEO_AUDIO_PCM)
    {
        /* Continue an oversized PCM chunk from where the previous call
         * stopped.  Dropping that remainder (the old behaviour) cut a
         * slice out of the waveform on every chunk larger than the
         * caller's buffer — e.g. 44.1 kHz stereo muxed at 1 chunk/frame
         * is ~11.7 KB against SimpleVideo's 8 KB buffer, so ~30% of the
         * audio went missing: heard as continuous "chopped" sound. */
        if (priv->audio_bits == 8u)
        {
            /* 8-bit unsigned PCM -> 16-bit signed: output is 2x input */
            uint32_t frame_bytes = (uint32_t)priv->audio_channels; /* 8-bit frame */
            uint32_t max_in = (uint32_t)(cap / 2u);
            uint32_t take_in;

            if (frame_bytes == 0u) frame_bytes = 1u;
            if (max_in == 0u)
            {
                priv->audio_part_left = 0u;
                return FM_ERR_TOO_LARGE;
            }
            take_in = (max_in < priv->audio_part_left) ? max_in : priv->audio_part_left;
            take_in -= take_in % frame_bytes;
            if (take_in == 0u)
            {
                priv->audio_part_left = 0u;
                return FM_ERR_TOO_LARGE;
            }
            {
                uint8_t *tmp = (uint8_t *)FM_Alloc(take_in);
                uint32_t i;
                if (tmp == NULL) return FM_ERR_NO_MEMORY;
                r = video_read_at(&v->file, priv->audio_part_off, tmp, take_in);
                if (r != FM_OK) { FM_Free(tmp); return r; }
                /* convert in-place from end to avoid overlap */
                for (i = take_in; i > 0u; )
                {
                    i--;
                    int16_t s = (int16_t)(((int16_t)tmp[i] - 128) << 8);
                    buf[i * 2u] = (uint8_t)(s & 0xFFu);
                    buf[i * 2u + 1u] = (uint8_t)((s >> 8) & 0xFFu);
                }
                FM_Free(tmp);
                priv->audio_part_off += take_in;
                priv->audio_part_left -= take_in;
                if (priv->audio_part_left < frame_bytes)
                {
                    priv->audio_part_left = 0u;
                }
                *size = take_in * 2u;
                return FM_OK;
            }
        }
        else
        {
            uint32_t align = (uint32_t)priv->audio_channels *
                             ((uint32_t)priv->audio_bits / 8u);
            uint32_t take;

            if (align == 0u)
            {
                align = 1u;
            }
            take = (cap < priv->audio_part_left) ? (uint32_t)cap
                                                 : priv->audio_part_left;
            take -= take % align;
            if (take == 0u)
            {
                /* Caller's buffer cannot hold even one sample frame. */
                priv->audio_part_left = 0u;
                return FM_ERR_TOO_LARGE;
            }
            r = video_read_at(&v->file, priv->audio_part_off, buf, take);
            if (r != FM_OK)
            {
                return r;
            }
            priv->audio_part_off += take;
            priv->audio_part_left -= take;
            if (priv->audio_part_left < align)
            {
                priv->audio_part_left = 0u;
            }
            *size = take;
            return FM_OK;
        }
    }
    if (v->audio_frame_index >= priv->audio_count)
    {
        /* Past the prebuilt index: for AVI, stream the remaining chunks
         * straight out of the movi list (tracks longer than
         * FM_VIDEO_MAX_FRAMES chunks used to go silent here). */
        if (!priv->avi_audio || priv->audio_scan_pos == 0u ||
            v->audio_frame_index >= priv->audio_total)
        {
            return FM_ERR_EOF;
        }
        /* (Re)start the streaming cursor on rewind or first use. */
        if (priv->audio_cur_pos == 0u ||
            v->audio_frame_index < priv->audio_cur_idx)
        {
            priv->audio_cur_pos = priv->audio_scan_pos;
            priv->audio_cur_idx = priv->audio_count;
        }
        for (;;)
        {
            uint8_t ck[8];
            uint32_t sz;
            uint64_t data;

            if (priv->audio_cur_pos + 8u > priv->audio_scan_end)
            {
                return FM_ERR_EOF;
            }
            if (video_read_at(&v->file, priv->audio_cur_pos, ck, 8) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            sz = video_rd32(ck + 4);
            data = priv->audio_cur_pos + 8u;
            if (data + sz > priv->audio_scan_end)
            {
                return FM_ERR_EOF;
            }
            if (memcmp(ck, "LIST", 4) == 0 && sz >= 4u)
            {
                priv->audio_cur_pos = data + 4u; /* descend into rec  */
                continue;
            }
            priv->audio_cur_pos = data + sz + (sz & 1u);
            if (ck[0] != '#' && memcmp(ck, "ix", 2) != 0 &&
                memcmp(ck + 2, "wb", 2) == 0 && sz > 0u)
            {
                if (priv->audio_cur_idx == v->audio_frame_index)
                {
                    offset = data;
                    fsize = sz;
                    priv->audio_cur_idx++;
                    goto have_chunk;
                }
                priv->audio_cur_idx++;
            }
        }
    }
    offset = priv->audio_frames[v->audio_frame_index].offset;
    fsize = priv->audio_frames[v->audio_frame_index].size;
have_chunk:

    if (priv->audio_codec == FM_VIDEO_AUDIO_AAC ||
        priv->audio_codec == FM_VIDEO_AUDIO_FLAC ||
        priv->audio_codec == FM_VIDEO_AUDIO_OPUS ||
        priv->audio_codec == FM_VIDEO_AUDIO_VORBIS)
    {
        const char *tag = (priv->audio_codec == FM_VIDEO_AUDIO_AAC) ? "AAC" :
                          (priv->audio_codec == FM_VIDEO_AUDIO_FLAC) ? "FLAC" :
                          (priv->audio_codec == FM_VIDEO_AUDIO_OPUS) ? "OPUS" : "VORBIS";
        const FM_AudioDecoderOps *ops = Audio_GetDecoder(tag);
        if (ops != NULL && ops->decode != NULL)
        {
            FM_AudioInfo ai;
            void *ctx = NULL;
            FM_Result dr;
            uint8_t *tmp = NULL;
            size_t out_cap_samples = cap / 2u;
            size_t out_samples = 0;
            if (out_cap_samples == 0u)
            {
                return FM_ERR_TOO_LARGE;
            }
            memset(&ai, 0, sizeof(ai));
            ai.sample_rate = priv->audio_rate ? priv->audio_rate : 44100u;
            ai.channels = priv->audio_channels ? priv->audio_channels : 2u;
            ai.bits_per_sample = 16u;
            (void)strncpy(ai.codec, tag, sizeof(ai.codec) - 1u);
            if (ops->init != NULL)
            {
                dr = ops->init(&ctx, &ai);
                if (dr != FM_OK)
                {
                    ctx = NULL;
                    ops = NULL;
                }
            }
            if (ops != NULL)
            {
                /* Vorbis MKV: CodecPrivate holds 3 Vorbis headers (ident/comment/setup)
                 * without Ogg framing (Matroska xiph lacing). Synthesize Ogg pages so
                 * stb_vorbis can decode single packets in Video_ReadAudioFrame. */
                if (priv->audio_codec == FM_VIDEO_AUDIO_VORBIS && priv->audio_private_size > 0u)
                {
                    uint8_t *pkt = (uint8_t*)FM_Alloc(fsize);
                    uint8_t *ogg = NULL;
                    uint32_t ogg_len = 0;
                    if (pkt == NULL)
                    {
                        if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                        return FM_ERR_NO_MEMORY;
                    }
                    r = video_read_at(&v->file, offset, pkt, fsize);
                    if (r != FM_OK) { FM_Free(pkt); if (ctx!=NULL&&ops->close!=NULL) ops->close(ctx); return r; }
                    /* If packet already looks like OggS, use it directly */
                    if (fsize >= 4u && pkt[0]=='O' && pkt[1]=='g' && pkt[2]=='g' && pkt[3]=='S')
                    {
                        dr = ops->decode(ctx, pkt, fsize, (int16_t*)buf, out_cap_samples, &out_samples);
                        FM_Free(pkt);
                        if (ctx!=NULL&&ops->close!=NULL) ops->close(ctx);
                        if (dr == FM_OK && out_samples >0u) { *size = out_samples*2u; v->audio_frame_index++; return FM_OK; }
                        if (dr == FM_OK || dr == FM_ERR_EOF) { if(out_samples==0u) return FM_ERR_CORRUPTED; *size=out_samples*2u; v->audio_frame_index++; return FM_OK; }
                        /* fall through to synth on failure */
                    }
                    else
                    {
                        FM_Result sr = vorbis_build_ogg_from_mkv(priv->audio_private, priv->audio_private_size, pkt, fsize, &ogg, &ogg_len);
                        FM_Free(pkt);
                        if (sr == FM_OK && ogg != NULL)
                        {
                            dr = ops->decode(ctx, ogg, ogg_len, (int16_t*)buf, out_cap_samples, &out_samples);
                            FM_Free(ogg);
                            if (ctx!=NULL&&ops->close!=NULL) ops->close(ctx);
                            if (dr == FM_OK && out_samples >0u) { *size = out_samples*2u; v->audio_frame_index++; return FM_OK; }
                            if (dr == FM_OK || dr == FM_ERR_EOF) { if(out_samples==0u) return FM_ERR_CORRUPTED; *size=out_samples*2u; v->audio_frame_index++; return FM_OK; }
                            /* stb_vorbis failed on synthetic Ogg: return silence instead of raw Vorbis */
                            return FM_ERR_CORRUPTED;
                        }
                        /* synth build failed: fall through to raw decode attempt below */
                        if (ogg) FM_Free(ogg);
                        /* try raw packet directly (will fail for stb_vorbis but keep path) */
                        tmp = (uint8_t*)FM_Alloc(fsize);
                        if (tmp == NULL) { if(ctx!=NULL&&ops->close!=NULL) ops->close(ctx); return FM_ERR_NO_MEMORY; }
                        r = video_read_at(&v->file, offset, tmp, fsize);
                        if (r != FM_OK) { FM_Free(tmp); if(ctx!=NULL&&ops->close!=NULL) ops->close(ctx); return r; }
                        dr = ops->decode(ctx, tmp, fsize, (int16_t*)buf, out_cap_samples, &out_samples);
                        FM_Free(tmp);
                        if (ctx!=NULL&&ops->close!=NULL) ops->close(ctx);
                        if (dr == FM_OK && out_samples >0u) { *size=out_samples*2u; v->audio_frame_index++; return FM_OK; }
                        if (dr != FM_OK && dr != FM_ERR_EOF) return dr;
                        if (out_samples==0u) return FM_ERR_CORRUPTED;
                        *size=out_samples*2u; v->audio_frame_index++; return FM_OK;
                    }
                    /* Ogg path returned above; if we fell through due to decode failure, return error */
                    return FM_ERR_CORRUPTED;
                }
                else if (priv->audio_codec == FM_VIDEO_AUDIO_FLAC && priv->audio_private_size > 0u)
                {
                    FM_Bool has_flac = (priv->audio_private_size >= 4u &&
                                        priv->audio_private[0]=='f' && priv->audio_private[1]=='L' &&
                                        priv->audio_private[2]=='a' && priv->audio_private[3]=='C') ? FM_TRUE : FM_FALSE;
                    uint32_t hdr_len = has_flac ? priv->audio_private_size : (priv->audio_private_size + 4u);
                    uint32_t synth_len = hdr_len + fsize;
                    tmp = (uint8_t *)FM_Alloc(synth_len);
                    if (tmp == NULL)
                    {
                        if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                        return FM_ERR_NO_MEMORY;
                    }
                    if (has_flac)
                    {
                        memcpy(tmp, priv->audio_private, priv->audio_private_size);
                    }
                    else
                    {
                        tmp[0]='f'; tmp[1]='L'; tmp[2]='a'; tmp[3]='C';
                        memcpy(tmp+4, priv->audio_private, priv->audio_private_size);
                    }
                    r = video_read_at(&v->file, offset, tmp + hdr_len, fsize);
                    if (r != FM_OK)
                    {
                        FM_Free(tmp);
                        if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                        return r;
                    }
                    dr = ops->decode(ctx, tmp, synth_len, (int16_t *)buf,
                                     out_cap_samples, &out_samples);
                    FM_Free(tmp);
                    if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                    if (dr == FM_OK && out_samples > 0u)
                    {
                        *size = out_samples * 2u;
                        v->audio_frame_index++;
                        return FM_OK;
                    }
                    if (dr == FM_OK || dr == FM_ERR_EOF)
                    {
                        if (out_samples == 0u) return FM_ERR_CORRUPTED;
                        *size = out_samples * 2u;
                        v->audio_frame_index++;
                        return FM_OK;
                    }
                    /* synth decode failed: don't return raw FLAC to I2S (would be noise);
                     * caller (SimpleVideo) will insert silence; extraction can still
                     * use raw path via separate call if needed. */
                    return FM_ERR_CORRUPTED;
                }
                else
                {
                    tmp = (uint8_t *)FM_Alloc(fsize);
                    if (tmp == NULL)
                    {
                        if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                        return FM_ERR_NO_MEMORY;
                    }
                    r = video_read_at(&v->file, offset, tmp, fsize);
                    if (r != FM_OK)
                    {
                        FM_Free(tmp);
                        if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                        return r;
                    }
                    dr = ops->decode(ctx, tmp, fsize, (int16_t *)buf,
                                     out_cap_samples, &out_samples);
                    FM_Free(tmp);
                    if (ctx != NULL && ops->close != NULL) ops->close(ctx);
                    if (dr == FM_OK && out_samples > 0u)
                    {
                        *size = out_samples * 2u;
                        v->audio_frame_index++;
                        return FM_OK;
                    }
                    if (dr != FM_OK && dr != FM_ERR_EOF)
                    {
                        return dr;
                    }
                    if (out_samples == 0u)
                    {
                        return FM_ERR_CORRUPTED;
                    }
                    *size = out_samples * 2u;
                    v->audio_frame_index++;
                    return FM_OK;
                }
            }
        }
        if (priv->audio_codec == FM_VIDEO_AUDIO_AAC)
        {
            uint8_t adts[7];
            if (cap < 7u + fsize)
            {
                return FM_ERR_TOO_LARGE;
            }
            r = adts_build(adts, priv->audio_rate, priv->audio_channels, fsize);
            if (r != FM_OK)
            {
                return r;
            }
            memcpy(buf, adts, 7);
            r = video_read_at(&v->file, offset, buf + 7, fsize);
            if (r != FM_OK)
            {
                return r;
            }
            *size = 7u + fsize;
        }
        else
        {
            uint32_t take = fsize;
            if (cap < fsize)
            {
                uint32_t align = (uint32_t)priv->audio_channels *
                                 ((uint32_t)priv->audio_bits / 8u);
                if (align == 0u) align = 1u;
                take = (uint32_t)(cap - (cap % align));
                if (take == 0u)
                {
                    v->audio_frame_index++;
                    return FM_ERR_TOO_LARGE;
                }
            }
            r = video_read_at(&v->file, offset, buf, take);
            if (r != FM_OK)
            {
                return r;
            }
            *size = take;
            if (take < fsize && (fsize - take) >=
                ((uint32_t)priv->audio_channels *
                 ((uint32_t)priv->audio_bits / 8u)))
            {
                priv->audio_part_off = offset + take;
                priv->audio_part_left = fsize - take;
            }
            v->audio_frame_index++;
            return FM_OK;
        }
    }
    else if (priv->audio_codec == FM_VIDEO_AUDIO_ALAW)
    {
        /* 8-bit A-law -> 16-bit PCM: expand in place via temp */
        uint8_t *tmp = (uint8_t*)FM_Alloc(fsize);
        size_t need = (size_t)fsize * 2u;
        if (tmp == NULL) return FM_ERR_NO_MEMORY;
        if (cap < need) { FM_Free(tmp); return FM_ERR_TOO_LARGE; }
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK) { FM_Free(tmp); return r; }
        for (uint32_t i=0;i<fsize;i++)
        {
            int16_t s = alaw_decode(tmp[i]);
            buf[i*2u] = (uint8_t)(s & 0xFFu);
            buf[i*2u+1u] = (uint8_t)((s>>8)&0xFFu);
        }
        FM_Free(tmp);
        *size = need;
    }
    else if (priv->audio_codec == FM_VIDEO_AUDIO_ULAW)
    {
        uint8_t *tmp = (uint8_t*)FM_Alloc(fsize);
        size_t need = (size_t)fsize * 2u;
        if (tmp == NULL) return FM_ERR_NO_MEMORY;
        if (cap < need) { FM_Free(tmp); return FM_ERR_TOO_LARGE; }
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK) { FM_Free(tmp); return r; }
        for (uint32_t i=0;i<fsize;i++)
        {
            int16_t s = ulaw_decode(tmp[i]);
            buf[i*2u] = (uint8_t)(s & 0xFFu);
            buf[i*2u+1u] = (uint8_t)((s>>8)&0xFFu);
        }
        FM_Free(tmp);
        *size = need;
    }
    else if (priv->audio_codec == FM_VIDEO_AUDIO_ADPCM)
    {
        uint8_t *tmp = (uint8_t*)FM_Alloc(fsize);
        size_t max_out = (cap/2u);
        size_t got_bytes;
        if (tmp == NULL) return FM_ERR_NO_MEMORY;
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK) { FM_Free(tmp); return r; }
        got_bytes = adpcm_decode_block(tmp, fsize, (int16_t*)buf, max_out);
        FM_Free(tmp);
        *size = got_bytes;
    }
#if FM_USE_MP3
    else if (priv->audio_codec == FM_VIDEO_AUDIO_MP3)
    {
        uint8_t *tmp = (uint8_t*)FM_Alloc(fsize);
        if (tmp == NULL) return FM_ERR_NO_MEMORY;
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK) { FM_Free(tmp); return r; }
        mp3dec_t dec; mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
        size_t out_bytes = 0;
        int pos = 0;
        int16_t *out = (int16_t*)buf;
        size_t out_cap_samples = cap / 2u;
        size_t out_samples = 0;
        while (pos < (int)fsize && out_samples < out_cap_samples)
        {
            int samples = mp3dec_decode_frame(&dec, tmp + pos, (int)(fsize - pos), pcm, &info);
            if (samples <= 0) { pos += (info.frame_bytes ? info.frame_bytes : 1); continue; }
            size_t copy = (size_t)samples * (size_t)info.channels;
            if (out_samples + copy > out_cap_samples) copy = out_cap_samples - out_samples;
            memcpy(out + out_samples, pcm, copy * 2u);
            out_samples += copy;
            out_bytes += copy * 2u;
            pos += info.frame_bytes;
            if (info.frame_bytes == 0) break;
        }
        FM_Free(tmp);
        if (out_samples == 0) return FM_ERR_CORRUPTED;
        *size = out_bytes;
    }
#endif
    else if (priv->audio_codec == FM_VIDEO_AUDIO_PCM && priv->audio_bits == 8u)
    {
        /* 8-bit unsigned PCM -> 16-bit signed for I2S */
        uint32_t frame_in = (uint32_t)priv->audio_channels; /* bytes per frame in */
        size_t need = (size_t)fsize * 2u;
        uint32_t take_in = fsize;

        if (frame_in == 0u) frame_in = 1u;
        if (need > cap)
        {
            uint32_t max_in = (uint32_t)(cap / 2u);
            max_in -= max_in % frame_in;
            if (max_in == 0u)
            {
                v->audio_frame_index++;
                return FM_ERR_TOO_LARGE;
            }
            take_in = max_in;
        }
        {
            uint8_t *tmp = (uint8_t *)FM_Alloc(take_in);
            uint32_t i;
            if (tmp == NULL) return FM_ERR_NO_MEMORY;
            r = video_read_at(&v->file, offset, tmp, take_in);
            if (r != FM_OK) { FM_Free(tmp); return r; }
            for (i = 0u; i < take_in; i++)
            {
                int16_t s = (int16_t)(((int16_t)tmp[i] - 128) << 8);
                buf[i * 2u] = (uint8_t)(s & 0xFFu);
                buf[i * 2u + 1u] = (uint8_t)((s >> 8) & 0xFFu);
            }
            FM_Free(tmp);
            *size = take_in * 2u;
            if (take_in < fsize)
            {
                priv->audio_part_off = offset + take_in;
                priv->audio_part_left = fsize - take_in;
            }
        }
    }
    else
    {
        uint32_t take = fsize;

        if (cap < fsize)
        {
            /* Chunk bigger than the caller's buffer.  Returning
             * TOO_LARGE without moving the cursor made the next refill
             * retry the very same chunk, so one oversized chunk muted
             * the rest of the file (and spammed the log).  Deliver as
             * much as fits — truncated to a whole sample frame so the
             * L/R phase is preserved — and remember the remainder so
             * the next call continues the same chunk (nothing is
             * dropped). */
            uint32_t align = (uint32_t)priv->audio_channels *
                             ((uint32_t)priv->audio_bits / 8u);

            if (align == 0u)
            {
                align = 1u;
            }
            take = (uint32_t)(cap - (cap % align));
            if (take == 0u)
            {
                v->audio_frame_index++;
                return FM_ERR_TOO_LARGE;
            }
        }
        r = video_read_at(&v->file, offset, buf, take);
        if (r != FM_OK)
        {
            return r;
        }
        *size = take;
        if (take < fsize && (fsize - take) >=
            ((uint32_t)priv->audio_channels *
             ((uint32_t)priv->audio_bits / 8u)))
        {
            priv->audio_part_off = offset + take;
            priv->audio_part_left = fsize - take;
        }
    }
    v->audio_frame_index++;
    return FM_OK;
}

FM_Result Video_SeekAudio(FM_Video *v, uint32_t sample_index)
{
    FM_VideoPriv *priv;

    if (v == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    priv = (FM_VideoPriv *)v->priv;
    if (priv == NULL || !priv->audio_present)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (sample_index >= priv->audio_count &&
        !(priv->avi_audio && sample_index < priv->audio_total))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* Force the streaming cursor to re-sync if we jumped backwards. */
    if (sample_index < v->audio_frame_index)
    {
        priv->audio_cur_pos = 0u;
    }
    /* A seek abandons any half-delivered oversized chunk. */
    priv->audio_part_left = 0u;
    v->audio_frame_index = sample_index;
    return FM_OK;
}

FM_Result Video_RegisterCodec(FM_Video *v, const FM_VideoCodecOps *ops)
{
    if (v == NULL || ops == NULL || ops->decode_frame == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (v->codec_plugin == ops)
    {
        return FM_OK;
    }
    if (v->codec_plugin != NULL && v->codec_ctx != NULL &&
        v->codec_plugin->close != NULL)
    {
        v->codec_plugin->close(v->codec_ctx);
        v->codec_ctx = NULL;
    }
    v->codec_plugin = ops;
    if (ops->init != NULL)
    {
        FM_Result r = ops->init(&v->codec_ctx, &v->info);

        if (r != FM_OK)
        {
            v->codec_plugin = NULL;
            return r;
        }
    }
    return FM_OK;
}

FM_Result Video_ResetCodec(FM_Video *v)
{
    const FM_VideoCodecOps *ops;

    if (v == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ops = v->codec_plugin;
    if (ops == NULL)
    {
        return FM_OK;
    }
    if (v->codec_ctx != NULL && ops->close != NULL)
    {
        ops->close(v->codec_ctx);
        v->codec_ctx = NULL;
    }
    if (ops->init != NULL)
    {
        FM_Result r = ops->init(&v->codec_ctx, &v->info);

        if (r != FM_OK)
        {
            v->codec_plugin = NULL;
            return r;
        }
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Writer (FMRV)                                                       */
/* ------------------------------------------------------------------ */
FM_Result VideoWriter_Create(const char *path, uint16_t width,
                             uint16_t height, uint32_t fps,
                             uint32_t frame_count, uint32_t frame_size,
                             FM_VideoWriter *w)
{
    uint8_t h[FMRV_HEADER_SIZE];
    FM_Result r;

    if (path == NULL || w == NULL || width == 0 || height == 0 || fps == 0 ||
        frame_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(w, 0, sizeof(*w));
    File_Init(&w->file);
    r = File_OpenEx(&w->file, path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        return r;
    }
    memset(h, 0, sizeof(h));
    memcpy(h, "FMRV", 4);
    video_wr16(h + 4, width);
    video_wr16(h + 6, height);
    video_wr32(h + 8, fps);
    video_wr32(h + 12, frame_count);
    video_wr32(h + 16, frame_size);
    r = video_write_exact(&w->file, h, sizeof(h));
    if (r != FM_OK)
    {
        (void)File_Close(&w->file);
        return r;
    }
    w->info.width = width;
    w->info.height = height;
    w->info.fps = fps;
    w->info.frame_count = frame_count;
    w->info.frame_size = frame_size;
    w->info.data_offset = FMRV_HEADER_SIZE;
    w->info.codec = FM_VIDEO_CODEC_RAW;
    (void)strcpy(w->info.container, "FMRV");
    return FM_OK;
}

FM_Result VideoWriter_WriteFrame(FM_VideoWriter *w, const void *frame,
                                 size_t size)
{
    FM_Result r;

    if (w == NULL || frame == NULL || !File_IsOpen(&w->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (size != w->info.frame_size ||
        (w->info.frame_count != 0 && w->frames_written >= w->info.frame_count))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = video_write_exact(&w->file, frame, (uint32_t)size);
    if (r == FM_OK)
    {
        w->frames_written++;
    }
    return r;
}

FM_Result VideoWriter_Close(FM_VideoWriter *w)
{
    FM_Result r = FM_OK;

    if (w == NULL || !File_IsOpen(&w->file))
    {
        return FM_ERR_NOT_OPEN;
    }
    if (File_Seek(&w->file, 12, FM_SEEK_SET) != FM_OK)
    {
        r = FM_ERR_IO;
    }
    else
    {
        uint8_t count[4];

        video_wr32(count, w->frames_written);
        r = video_write_exact(&w->file, count, sizeof(count));
    }
    if (r == FM_OK)
    {
        r = File_Flush(&w->file);
    }
    {
        FM_Result c = File_Close(&w->file);
        if (r == FM_OK) r = c;
    }
    return r;
}

#endif /* FM_USE_VIDEO */
