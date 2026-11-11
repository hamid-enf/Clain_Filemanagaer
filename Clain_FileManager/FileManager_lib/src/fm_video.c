/*
 * FileManager - Video containers (FMRV, AVI, MP4, Matroska/WebM, FLV)
 *
 * FMRV is the built-in raw container (fixed-size frames + small header).
 * AVI (RIFF), MP4 (ISO BMFF), Matroska/WebM and FLV are parsed far enough
 * to extract the video stream (dimensions, frame rate, codec, a per-frame
 * offset/size index) and one audio track.  Frame payloads are handed to a
 * codec plugin (see fm_h264.h) for decode.
 *
 * The frame index lives in a static pool sized by FM_VIDEO_MAX_FRAMES.  A
 * clip with more frames than that is still played to its end: the frames
 * past the table are located by walking the container again while
 * reading (VIDEO_TAIL_*), sequentially in O(1) per frame.
 *
 * Heap (FM_Alloc) is used for two things only, both freed by Video_Close:
 * the Matroska lace table of a clip longer than the index, and audio
 * setup data larger than FM_VIDEO_AUDIO_PRIVATE_MAX.
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
#include "fm_vorbis.h"
#include "fm_flac.h"
#if FM_USE_MP3
#include "minimp3.h"
#endif

#if FM_USE_VIDEO

/* Compressed audio tracks are decoded through the fm_audio.h decoder
 * registry.  Without the audio subsystem there is no registry, so the
 * whole decode path (and its lookup tables) is compiled out. */
#define VIDEO_AUDIO_REGISTRY   (FM_USE_AUDIO)
/* Vorbis in MKV / WebM: the three header packets sit in CodecPrivate,
 * the audio packets follow one by one.  The built-in decoder has a
 * packet-stream interface for exactly that (fm_vorbis.h). */
#define VIDEO_VORBIS_STREAM (FM_USE_AUDIO && FM_USE_VORBIS)

#define FMRV_HEADER_SIZE 24u
/* Annex-B parameter-set prefix (SPS/PPS, plus VPS for HEVC whose three
 * sets together routinely exceed the 512 bytes this used to be). */
#define SPSPPS_MAX 1024u

/* ------------------------------------------------------------------ */
/* Private per-video state                                             */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint64_t offset;
    uint32_t size;
} FM_VideoFrame;

/* Where to resume walking the container for entries past an index. */
typedef struct
{
    uint64_t scan_pos;   /* element holding the first un-indexed entry
                          * (0 = everything is in the index)          */
    uint32_t scan_skip;  /* entries of that element already indexed
                          * (MKV laced block split by the index end)  */
    uint32_t total;      /* entries found by the open-time scan       */
    uint64_t cur_pos;    /* cursor: element under inspection (0=unset)*/
    uint32_t cur_idx;    /* cursor: index of that element's 1st entry */
} FM_VideoTail;

#define VIDEO_TAIL_NONE 0u
#define VIDEO_TAIL_AVI  1u
#define VIDEO_TAIL_MKV  2u
#define VIDEO_TAIL_MP4  3u
#define VIDEO_TAIL_FLV  4u

/* A position in the sample tables of one MP4 track. */
typedef struct
{
    uint32_t sample;     /* sample the cursor is on                     */
    uint32_t chunk;      /* 0-based chunk that holds it                 */
    uint32_t in_chunk;   /* samples of that chunk in front of it        */
    uint32_t per_chunk;  /* samples per chunk in force (stsc)           */
    uint32_t stsc_idx;   /* stsc entries already applied                */
    uint64_t off;        /* file offset of the sample (when off_valid)  */
    FM_Bool  off_valid;
} Mp4Cursor;

/* The sample tables of one MP4 track (stco/co64 + stsc + stsz).  They
 * stay in the file: samples past the index table are located from them
 * on demand. */
typedef struct
{
    uint64_t stco_off;
    uint32_t stco_count;
    uint32_t stco_entry;     /* 4 (stco) or 8 (co64) bytes              */
    uint64_t stsc_off;
    uint32_t stsc_count;
    uint64_t stsz_off;
    uint32_t sample_size;    /* uniform size, 0 = per-sample table      */
    uint32_t sample_count;
    Mp4Cursor start;         /* on the last sample of the index table   */
    Mp4Cursor cur;           /* streaming cursor                        */
} Mp4Track;

typedef struct
{
    FM_Bool       in_use;
    uint32_t      count;
    FM_Bool       avc1;              /* length-prefixed NALs (MP4/MKV
                                      * H.264 + HEVC): convert to
                                      * Annex-B, prefix parameter sets */
    uint8_t       nal_len_size;      /* NAL length field, 1..4 bytes  */
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
    uint16_t      audio_bits;        /* PCM: bits a sample in the file */
    uint16_t      audio_wav_tag;     /* ADPCM: 0x0011 IMA, 0x0002 MS  */
    uint16_t      audio_block_align; /* ADPCM: bytes per block        */
    uint8_t       audio_aot;         /* AAC audio object type (0 = not
                                      * known: treated as AAC-LC)     */
    FM_Bool       audio_pcm_be;      /* PCM stored big-endian         */
    FM_Bool       audio_pcm_float;   /* PCM: 32-bit IEEE float        */
    FM_Bool       audio_pcm_s8;      /* PCM: 8-bit samples are signed
                                      * (QuickTime; unsigned elsewhere) */
    uint32_t      audio_private_size;
    uint8_t       audio_private[FM_VIDEO_AUDIO_PRIVATE_MAX]; /* MKV CodecPrivate / MP4 dfLa */
    uint8_t      *audio_private_big; /* ...when it does not fit there
                                      * (FM_Alloc, freed on close)    */
    /* Trimming of a decoded track (Video_ReadAudioFrame): frames an
     * encoder put in front of the sound and behind it, as the container
     * records them. */
    uint32_t      audio_skip;        /* frames dropped at the start
                                      * (MP4 edit list, MKV CodecDelay) */
    uint32_t      audio_skip_left;   /* ...still to drop              */
    uint64_t      audio_limit;       /* frames the track has after the
                                      * skip (MP4 edit list); 0 = not
                                      * recorded                      */
    uint64_t      audio_out;         /* frames handed out so far      */
    FM_Bool       audio_limit_off;   /* after a seek into the track the
                                      * count is not known            */
    uint32_t      audio_pad_packet;  /* MKV DiscardPadding: the packet */
    uint32_t      audio_pad_frames;  /* ...and frames cut off its end */
    uint32_t      audio_count;
    /* PCM chunk larger than the caller's buffer: bytes of the chunk
     * at audio_frame_index-1 already handed out.  The next call
     * continues from here instead of dropping the remainder. */
    uint64_t      audio_part_off;    /* file offset of the remainder  */
    uint32_t      audio_part_left;   /* bytes still to deliver        */
    FM_VideoFrame audio_frames[FM_VIDEO_MAX_AUDIO_FRAMES];
    /* Streaming continuation: frames/packets that did not fit the two
     * fixed index tables are located on demand by walking the container
     * from where the index stopped (AVI movi list, MKV clusters, MP4
     * sample tables). */
    uint8_t       tail_kind;         /* VIDEO_TAIL_*                  */
    uint64_t      tail_end;          /* end of the walkable region    */
    FM_VideoTail  vtail;             /* video frames past frames[]    */
    FM_VideoTail  atail;             /* audio packets past the index  */
    Mp4Track      mp4v;              /* MP4 sample tables, video      */
    Mp4Track      mp4a;              /* MP4 sample tables, audio      */
    uint32_t      mkv_vid_track;     /* MKV track numbers             */
    uint32_t      mkv_aud_track;
    uint32_t      mkv_bad_blocks;    /* blocks dropped while indexing */
    void         *mkv_blk;           /* MkvBlock work area (lace sizes),
                                      * allocated once per MKV         */
    /* Compressed-frame scratch for Video_DecodeFrame and the native
     * RGB scratch for Video_DecodeFrameScaled: grown on demand, reused
     * for every frame and released by Video_Close (they used to be one
     * FM_Alloc/FM_Free pair per frame). */
    uint8_t      *scratch;
    size_t        scratch_cap;
    uint8_t      *rgb_scratch;
    size_t        rgb_scratch_cap;
    FM_Bool       nodec_logged;      /* "no video decoder" said once  */
    uint64_t      file_size;         /* at open; 0 = not known        */
#if VIDEO_AUDIO_REGISTRY
    /* Compressed-track decoder, opened on the first packet and kept for
     * the life of the video (re-created after a seek).  Stateful codecs
     * (Opus) need the context to survive between packets; the scratch
     * buffer replaces one FM_Alloc/FM_Free pair per packet. */
    const FM_AudioDecoderOps *adec_ops;
    void         *adec_ctx;
    uint8_t      *adec_buf;
    size_t        adec_cap;
#endif
    FM_Bool       audio_nodec_logged; /* "no decoder" reported once   */
#if FM_USE_MP3
    /* MP3 track: decoder state and one frame of PCM, kept for the whole
     * file (FM_Alloc on the first packet, freed on close).  See
     * video_mp3_read(). */
    struct FM_VideoMp3 *mp3;
#endif
#if VIDEO_VORBIS_STREAM
    /* Vorbis track: the stream decoder, opened on the first packet and
     * kept until close.  See video_vorbis_read(). */
    FM_VorbisStream *vorbis;
    FM_Result        vorbis_error;   /* != FM_OK: the headers were refused */
#endif
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
    FM_VideoPriv *p = NULL;

    /* The slot is claimed under the VFS lock: two tasks opening a video
     * at the same time must not both be handed slot 0. */
    FM_Lock();
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
            p = &g_video[i];
            break;
        }
    }
    FM_Unlock();
    return p;
}

/* Registry name of the decoder a compressed audio track needs, or NULL
 * when the track is decoded inside this file (PCM, G.711, ADPCM, MP3). */
static const char *video_audio_tag(uint32_t codec)
{
    switch (codec)
    {
    case FM_VIDEO_AUDIO_AAC:    return "AAC";
    case FM_VIDEO_AUDIO_FLAC:   return "FLAC";
    case FM_VIDEO_AUDIO_OPUS:   return "OPUS";
    case FM_VIDEO_AUDIO_VORBIS: return "VORBIS";
    default:                    return NULL;
    }
}

#if FM_USE_MP3
/* About 15.8 KB.  It used to be two locals of Video_ReadAudioFrame: a
 * 16 KB stack frame on top of the 17 KB one minimp3 needs below it. */
struct FM_VideoMp3
{
    mp3dec_t dec;
    int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
};
#endif

/* Close the track decoder but keep the scratch buffer (seek). */
static void video_audio_dec_reset(FM_VideoPriv *p)
{
#if FM_USE_MP3
    if (p->mp3 != NULL)
    {
        /* a seek is a discontinuity: forget the bit reservoir */
        mp3dec_init(&p->mp3->dec);
    }
#endif
#if VIDEO_VORBIS_STREAM
    if (p->vorbis != NULL)
    {
        /* the same for Vorbis; the codebooks are kept */
        FM_Vorbis_StreamReset(p->vorbis);
    }
#endif
#if VIDEO_AUDIO_REGISTRY
    if (p->adec_ops != NULL && p->adec_ctx != NULL &&
        p->adec_ops->close != NULL)
    {
        p->adec_ops->close(p->adec_ctx);
    }
    p->adec_ops = NULL;
    p->adec_ctx = NULL;
#else
    (void)p;
#endif
}

static void video_priv_free(FM_VideoPriv *p)
{
    if (p != NULL)
    {
        video_audio_dec_reset(p);
#if FM_USE_MP3
        if (p->mp3 != NULL)
        {
            FM_Free(p->mp3);
            p->mp3 = NULL;
        }
#endif
#if VIDEO_VORBIS_STREAM
        if (p->vorbis != NULL)
        {
            FM_Vorbis_StreamClose(p->vorbis);
            p->vorbis = NULL;
        }
#endif
#if VIDEO_AUDIO_REGISTRY
        if (p->adec_buf != NULL)
        {
            FM_Free(p->adec_buf);
        }
        p->adec_buf = NULL;
        p->adec_cap = 0u;
#endif
        if (p->scratch != NULL)
        {
            FM_Free(p->scratch);
        }
        if (p->rgb_scratch != NULL)
        {
            FM_Free(p->rgb_scratch);
        }
        if (p->mkv_blk != NULL)
        {
            FM_Free(p->mkv_blk);
        }
        p->mkv_blk = NULL;
        if (p->audio_private_big != NULL)
        {
            FM_Free(p->audio_private_big);
        }
        p->audio_private_big = NULL;
        p->scratch = NULL;
        p->scratch_cap = 0u;
        p->rgb_scratch = NULL;
        p->rgb_scratch_cap = 0u;
        FM_Lock();
        p->in_use = FM_FALSE;
        FM_Unlock();
    }
}

/* Does the index entry (offset, size) lie inside the file?  The sizes
 * in a container's index go straight into an allocation and a read; a
 * damaged entry asked for gigabytes before the read could fail.  A range
 * that is not in the file is treated like the read it would have been:
 * the end of the data. */
static FM_Bool video_in_file(const FM_VideoPriv *priv, uint64_t offset,
                             uint64_t size)
{
    if (priv == NULL || priv->file_size == 0u)
    {
        return FM_TRUE;                  /* size not known: let the read say */
    }
    return (offset <= priv->file_size && size <= priv->file_size - offset)
               ? FM_TRUE : FM_FALSE;
}

/* Return a scratch buffer of at least `need` bytes that lives as long as
 * the video.  Grows (never shrinks); the old block is released first so
 * the peak is one buffer, not two. */
static uint8_t *video_scratch_get(uint8_t **buf, size_t *cap, size_t need)
{
    if (*buf == NULL || *cap < need)
    {
        if (*buf != NULL)
        {
            FM_Free(*buf);
        }
        *cap = 0u;
        *buf = (uint8_t *)FM_Alloc(need);
        if (*buf != NULL)
        {
            *cap = need;
        }
    }
    return *buf;
}

/* MPEG-4 AudioSpecificConfig: object type, sampling rate and channel
 * configuration of an AAC track.  For HE-AAC (object types 5 / 29) the
 * values are those of the AAC-LC core, which is what an ADTS header has to
 * carry.  *rate / *chan are left alone when the config does not say
 * (escape values), so the container's figures stay. */
static FM_Bool video_asc_parse(const uint8_t *c, uint32_t n, uint32_t *aot,
                               uint32_t *rate, uint32_t *chan)
{
    static const uint32_t k_rate[13] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u };
    uint32_t bit = 0u;
    uint32_t total = n * 8u;
    uint32_t val[4];
    uint32_t width[4];
    uint32_t k;
    uint32_t nf = 3u;

    /* fields: object type (5, +6 when 31), frequency index (4, +24 when
     * 15), channel configuration (4) */
    width[0] = 5u; width[1] = 4u; width[2] = 4u; width[3] = 0u;
    for (k = 0u; k < nf; k++)
    {
        uint32_t x = 0u;
        uint32_t i;

        if (bit + width[k] > total)
        {
            return FM_FALSE;
        }
        for (i = 0u; i < width[k]; i++, bit++)
        {
            x = (x << 1) | (((uint32_t)c[bit >> 3] >> (7u - (bit & 7u))) & 1u);
        }
        val[k] = x;
        if (k == 0u && x == 31u)
        {
            /* escaped object type: 32 + the next 6 bits */
            uint32_t e = 0u;

            if (bit + 6u > total)
            {
                return FM_FALSE;
            }
            for (i = 0u; i < 6u; i++, bit++)
            {
                e = (e << 1) |
                    (((uint32_t)c[bit >> 3] >> (7u - (bit & 7u))) & 1u);
            }
            val[0] = 32u + e;
        }
        if (k == 1u && x == 15u)
        {
            /* explicit 24-bit sampling rate */
            uint32_t e = 0u;

            if (bit + 24u > total)
            {
                return FM_FALSE;
            }
            for (i = 0u; i < 24u; i++, bit++)
            {
                e = (e << 1) |
                    (((uint32_t)c[bit >> 3] >> (7u - (bit & 7u))) & 1u);
            }
            val[3] = e;
        }
    }
    if (val[0] == 0u)
    {
        return FM_FALSE;
    }
    *aot = val[0];
    if (val[1] < 13u)
    {
        *rate = k_rate[val[1]];
    }
    else if (val[1] == 15u && val[3] != 0u)
    {
        *rate = val[3];
    }
    if (val[2] >= 1u && val[2] <= 7u)
    {
        *chan = (val[2] == 7u) ? 8u : val[2];
    }
    return FM_TRUE;
}

/* Codec setup data of the audio track (audio_private_size bytes). */
static const uint8_t *video_audio_private(const FM_VideoPriv *priv)
{
    return (priv->audio_private_big != NULL) ? priv->audio_private_big
                                             : priv->audio_private;
}

/* Read the setup data of the audio track (MKV CodecPrivate, MP4 dfLa,
 * FLV AudioSpecificConfig) from the file.  What does not fit the inline
 * buffer goes to the heap; it used to be dropped (MKV) or cut off
 * (MP4), and the track then failed in the decoder. */
static FM_Result video_audio_private_load(FM_Video *v, FM_VideoPriv *priv,
                                          uint64_t off, uint64_t len)
{
    uint8_t *dst = priv->audio_private;

    priv->audio_private_size = 0u;
    if (priv->audio_private_big != NULL)
    {
        FM_Free(priv->audio_private_big);
        priv->audio_private_big = NULL;
    }
    if (len == 0u)
    {
        return FM_OK;
    }
    if (len > sizeof(priv->audio_private))
    {
        if (len > (uint64_t)FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        dst = (uint8_t *)FM_Alloc((size_t)len);
        if (dst == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
    }
    if (video_read_at(&v->file, off, dst, (uint32_t)len) != FM_OK)
    {
        if (dst != priv->audio_private)
        {
            FM_Free(dst);
        }
        return FM_ERR_IO;
    }
    if (dst != priv->audio_private)
    {
        priv->audio_private_big = dst;
    }
    priv->audio_private_size = (uint32_t)len;
    return FM_OK;
}

/* Add one entry to a fixed index table; once the table is full remember
 * where the first entry that did not fit lives so it can be streamed. */
static void video_index_add(FM_VideoFrame *tab, uint32_t cap, uint32_t *count,
                            FM_VideoTail *tail, uint64_t elem_pos,
                            uint32_t elem_entry, uint64_t off, uint32_t size)
{
    if (*count < cap)
    {
        tab[*count].offset = off;
        tab[*count].size = size;
        (*count)++;
    }
    else if (tail->scan_pos == 0u)
    {
        tail->scan_pos = elem_pos;
        tail->scan_skip = elem_entry;
    }
    tail->total++;
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

    /* The RIFF size field is ignored on purpose (a bogus 0x7FFFFFFF costs
     * nothing), and every chunk size is clamped to what the file really
     * holds: sizes come from the file, and a recording cut short -- or a
     * streaming writer that never patched its headers -- declares more
     * than exists.  Clamping keeps what is there playable and keeps
     * hdrl_end / movi_end inside the file. */
    pos = 12; /* after RIFF size + "AVI " */
    while (pos + 8 <= file_size)
    {
        uint8_t ck[12];
        uint64_t sz;
        uint64_t data = pos + 8;

        if (video_read_at(&v->file, pos, ck, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        sz = video_rd32(ck + 4);
        if (sz > file_size - data)
        {
            sz = file_size - data;
        }
        if (memcmp(ck, "LIST", 4) == 0 && sz >= 4u)
        {
            /* The list type is read separately: an 8-byte chunk at the
             * very end of the file must not make a 12-byte read fail. */
            if (video_read_at(&v->file, data, ck + 8, 4) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
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
    FM_Bool core_header = FM_FALSE;      /* strf is a BITMAPCOREHEADER */

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
        if (sz > end - data)
        {
            sz = (uint32_t)(end - data);
        }

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
                if (ssz > send - sd)
                {
                    ssz = (uint32_t)(send - sd);
                }
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
                else if (memcmp(sc, "strf", 4) == 0 && ssz >= 12)
                {
                    /* Read what the chunk really holds, zero-padded to
                     * the 40 bytes parsed below.  This used to read a
                     * fixed 40 bytes behind an `ssz >= 16` guard: a
                     * 16/18-byte WAVEFORMAT that happens to be the last
                     * thing in the file then failed the read and the
                     * whole AVI was rejected as corrupted. */
                    uint8_t strf[40];
                    uint32_t take = (ssz < 40u) ? ssz : 40u;

                    memset(strf, 0, sizeof(strf));
                    if (video_read_at(&v->file, sd, strf, take) != FM_OK)
                    {
                        return FM_ERR_CORRUPTED;
                    }
                    if (is_video_stream && video_rd32(strf) == 12u)
                    {
                        /* BITMAPCOREHEADER: bcWidth/bcHeight are WORDs
                         * and there is no biCompression at all -- parsed
                         * as an INFOHEADER the "compression" came from
                         * whatever followed the 12-byte struct. */
                        width = video_rd16(strf + 4);
                        height = video_rd16(strf + 6);
                        height_negative = FM_FALSE;
                        bit_count = video_rd16(strf + 10);
                        memset(compression, 0, sizeof(compression));
                        compression_zero = FM_TRUE;
                        core_header = FM_TRUE;
                    }
                    else if (is_video_stream)
                    {
                        /* BITMAPINFOHEADER: biWidth(4) biHeight(4) ...
                         * biBitCount(2 @14) biCompression(4 @16) */
                        int32_t bi_h = (int32_t)video_rd32(strf + 8);

                        width = video_rd32(strf + 4);
                        if (bi_h < 0)
                        {
                            height_negative = FM_TRUE;
                            height = 0u - (uint32_t)bi_h;
                        }
                        else
                        {
                            height_negative = FM_FALSE;
                            height = (uint32_t)bi_h;
                        }
                        bit_count = video_rd16(strf + 14);
                        memcpy(compression, strf + 16, 4);
                        compression_zero =
                            (video_rd32(strf + 16) == 0u) ? FM_TRUE : FM_FALSE;
                        core_header = FM_FALSE;
                    }
                    else if (is_audio_stream)
                    {
                        /* WAVEFORMATEX: tag(2) channels(2) rate(4) ... bits(2)
                         * Expanded for MP3(0x0055), aLaw(6), muLaw(7), ADPCM (0x11/2). */
                        uint16_t tag = video_rd16(strf);

                        if (tag == 0xFFFEu && take >= 26u)
                        {
                            /* WAVE_FORMAT_EXTENSIBLE (what is written
                             * for more than 16 bits): the real tag is
                             * the first field of the SubFormat GUID. */
                            tag = video_rd16(strf + 24);
                            if (tag != 1u && tag != 3u)
                            {
                                tag = 0xFFFEu;
                            }
                        }
                        if (tag == 3u) /* IEEE float */
                        {
                            /* 32-bit only; a 64-bit float track is not
                             * announced at all */
                            if (video_rd16(strf + 14) == 32u)
                            {
                                priv->audio_codec = FM_VIDEO_AUDIO_PCM;
                                priv->audio_channels = video_rd16(strf + 2);
                                priv->audio_rate = video_rd32(strf + 4);
                                priv->audio_bits = 32u;
                                priv->audio_pcm_float = FM_TRUE;
                                priv->audio_present = FM_TRUE;
                            }
                        }
                        else if (tag == 1u) /* PCM */
                        {
                            priv->audio_codec = FM_VIDEO_AUDIO_PCM;
                            priv->audio_channels = video_rd16(strf + 2);
                            priv->audio_rate = video_rd32(strf + 4);
                            priv->audio_bits = video_rd16(strf + 14);
                            if (priv->audio_bits == 0u &&
                                priv->audio_channels != 0u)
                            {
                                /* 14-byte WAVEFORMAT has no
                                 * wBitsPerSample: derive it from
                                 * nBlockAlign. */
                                priv->audio_bits = (uint16_t)
                                    ((video_rd16(strf + 12) * 8u) /
                                     priv->audio_channels);
                            }
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
                            priv->audio_wav_tag = tag;
                            priv->audio_block_align = video_rd16(strf + 12);
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
        if (core_header)
        {
            /* A core header cannot name a codec: the stream handler is
             * the only statement there is; plain DIB otherwise. */
            v->info.codec = avi_map_fourcc(handler);
            if (v->info.codec == FM_VIDEO_CODEC_UNKNOWN && bit_count >= 16u)
            {
                v->info.codec = FM_VIDEO_CODEC_RAW;
            }
        }
        else if (compression_zero && bit_count >= 16u)
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
    uint64_t end = lay->movi_end;   /* already clamped to the file */

    priv->count = 0;
    priv->tail_kind = VIDEO_TAIL_AVI;
    priv->tail_end = end;

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
        if (sz > end - data)
        {
            /* The last chunk runs past the list: a truncated recording.
             * Keep the complete frames in front of it instead of
             * rejecting the whole file. */
            FM_ERR_LOG("avi: chunk at %lu runs past the movi list -- "
                       "file truncated, playing what is complete",
                       (unsigned long)pos);
            break;
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
             * skip them (playback just holds the previous frame).
             * Frames past the fixed table are streamed from the movi
             * list (video_tail_locate) -- a clip longer than
             * FM_VIDEO_MAX_FRAMES used to "end" there. */
            if (sz > 0u)
            {
                video_index_add(priv->frames, FM_VIDEO_MAX_FRAMES,
                                &priv->count, &priv->vtail, pos, 0u,
                                data, sz);
            }
        }
        else if (memcmp(ck + 2, "wb", 2) == 0)
        {
            /* audio chunk; same continuation scheme as the video. */
            if (sz > 0u && priv->audio_present)
            {
                video_index_add(priv->audio_frames,
                                FM_VIDEO_MAX_AUDIO_FRAMES,
                                &priv->audio_count, &priv->atail, pos, 0u,
                                data, sz);
            }
        }
        pos = data + sz + (sz & 1u);
    }
    if (priv->count == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* avih.dwTotalFrames is advisory: never promise more frames than
     * the movi list holds. */
    if (v->info.frame_count == 0 || v->info.frame_count > priv->vtail.total)
    {
        v->info.frame_count = priv->vtail.total;
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
        box->size = video_be64(h);      /* big-endian like every MP4 field */
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
 * (so leaf payloads like mdat or avc1 sample entries are never traversed).
 *
 * The nesting is bounded.  The deepest chain a real file has is
 * moov/trak/mdia/minf/stbl -- five levels -- but nothing stops a file
 * from putting a "moov" inside a "moov" a thousand times, 8 bytes each,
 * and every level is a stack frame: 6 KB of file overran a 64 KB stack.
 * A file nested deeper than MP4_MAX_NEST is damaged, and is reported as
 * such instead of being searched. */
#define MP4_MAX_NEST 8u

static FM_Result mp4_find_nested(FM_Video *v, uint64_t start, uint64_t end,
                                 const char *wanted, Mp4Box *out,
                                 uint32_t depth)
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
            if (depth + 1u >= MP4_MAX_NEST)
            {
                return FM_ERR_CORRUPTED;
            }
            r = mp4_find_nested(v, box.data, box.start + box.size, wanted,
                                out, depth + 1u);
            if (r == FM_OK || r == FM_ERR_CORRUPTED)
            {
                return r;       /* found, or nested too deep to go on */
            }
        }
        pos = box.start + box.size;
    }
    return FM_ERR_NOT_FOUND;
}

static FM_Result mp4_find_deep(FM_Video *v, uint64_t start, uint64_t end,
                               const char *wanted, Mp4Box *out)
{
    return mp4_find_nested(v, start, end, wanted, out, 0u);
}

/* Append one parameter-set NAL (4-byte start code + `nlen` bytes read at
 * `off`) to the Annex-B prefix handed out in front of every frame. */
static FM_Result video_param_append(FM_Video *v, FM_VideoPriv *priv,
                                    uint64_t off, uint32_t nlen)
{
    static const uint8_t start[] = { 0, 0, 0, 1 };

    if (nlen == 0u)
    {
        return FM_ERR_CORRUPTED;
    }
    if (priv->sps_pps_len + 4u + nlen > sizeof(priv->sps_pps))
    {
        return FM_ERR_TOO_LARGE;
    }
    memcpy(priv->sps_pps + priv->sps_pps_len, start, 4);
    if (video_read_at(&v->file, off, priv->sps_pps + priv->sps_pps_len + 4u,
                      nlen) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    priv->sps_pps_len += 4u + nlen;
    return FM_OK;
}

/* AVCDecoderConfigurationRecord (MP4 "avcC" payload, MKV CodecPrivate of
 * V_MPEG4/ISO/AVC): `payload` bytes at file offset `data`.
 *
 * lengthSizeMinusOne (low 2 bits of byte 4) used to be thrown away and
 * every sample was converted as if its NAL lengths were 4 bytes wide; a
 * stream muxed with 1- or 2-byte lengths then read rubbish lengths and
 * every frame was "corrupted".  It is kept in priv->nal_len_size now. */
static FM_Result video_parse_avcc(FM_Video *v, uint64_t data,
                                  uint64_t payload, FM_VideoPriv *priv)
{
    uint8_t hdr[6];
    uint8_t one;
    uint64_t off = 6;
    uint32_t num;
    uint32_t i;
    uint32_t list;
    FM_Result r;

    priv->sps_pps_len = 0;
    if (payload < 7u ||
        video_read_at(&v->file, data, hdr, 6) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    if (hdr[0] != 1u) /* configurationVersion */
    {
        return FM_ERR_CORRUPTED;
    }
    priv->nal_len_size = (uint8_t)((hdr[4] & 0x03u) + 1u);

    /* SPS list, then PPS list */
    num = hdr[5] & 0x1Fu;
    for (list = 0; list < 2; list++)
    {
        uint32_t n = (list == 0) ? num : 0;

        if (list == 1)
        {
            if (off + 1u > payload ||
                video_read_at(&v->file, data + off, &one, 1) != FM_OK)
            {
                priv->sps_pps_len = 0;
                return FM_ERR_CORRUPTED;
            }
            off += 1u;
            n = one;
        }
        for (i = 0; i < n; i++)
        {
            uint8_t lb[2];
            uint32_t nlen;

            if (off + 2u > payload ||
                video_read_at(&v->file, data + off, lb, 2) != FM_OK)
            {
                priv->sps_pps_len = 0;
                return FM_ERR_CORRUPTED;
            }
            nlen = video_be16(lb);
            off += 2u;
            if (nlen > payload - off)
            {
                priv->sps_pps_len = 0;
                return FM_ERR_CORRUPTED;
            }
            r = video_param_append(v, priv, data + off, nlen);
            if (r != FM_OK)
            {
                priv->sps_pps_len = 0;
                return r;
            }
            off += nlen;
        }
    }
    return FM_OK;
}

/* HEVCDecoderConfigurationRecord (MP4 "hvcC" payload, MKV CodecPrivate
 * of V_MPEGH/ISO/HEVC).  22 fixed bytes (lengthSizeMinusOne in the low
 * 2 bits of byte 21), numOfArrays, then per array:
 *   byte  = array_completeness(1) reserved(1) NAL_unit_type(6)
 *   word  = numNalus, then numNalus x { word length, NAL bytes }
 * VPS (32), SPS (33) and PPS (34) go into the Annex-B prefix; without
 * them no HEVC decoder -- hardware or not -- can start, and this record
 * used not to be read at all. */
static FM_Result video_parse_hvcc(FM_Video *v, uint64_t data,
                                  uint64_t payload, FM_VideoPriv *priv)
{
    uint8_t h[23];
    uint64_t off = 23;
    uint32_t arrays;
    uint32_t a;
    FM_Result r;

    priv->sps_pps_len = 0;
    if (payload < 23u || video_read_at(&v->file, data, h, 23) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    priv->nal_len_size = (uint8_t)((h[21] & 0x03u) + 1u);
    arrays = h[22];
    for (a = 0; a < arrays; a++)
    {
        uint8_t ah[3];
        uint32_t type;
        uint32_t nals;
        uint32_t n;

        if (off + 3u > payload ||
            video_read_at(&v->file, data + off, ah, 3) != FM_OK)
        {
            priv->sps_pps_len = 0;
            return FM_ERR_CORRUPTED;
        }
        type = ah[0] & 0x3Fu;
        nals = video_be16(ah + 1);
        off += 3u;
        for (n = 0; n < nals; n++)
        {
            uint8_t lb[2];
            uint32_t nlen;

            if (off + 2u > payload ||
                video_read_at(&v->file, data + off, lb, 2) != FM_OK)
            {
                priv->sps_pps_len = 0;
                return FM_ERR_CORRUPTED;
            }
            nlen = video_be16(lb);
            off += 2u;
            if (nlen == 0u || nlen > payload - off)
            {
                priv->sps_pps_len = 0;
                return FM_ERR_CORRUPTED;
            }
            if (type >= 32u && type <= 34u)
            {
                r = video_param_append(v, priv, data + off, nlen);
                if (r != FM_OK)
                {
                    priv->sps_pps_len = 0;
                    return r;
                }
            }
            off += nlen;
        }
    }
    return FM_OK;
}

/* Parse the configuration record held in MP4 box `box` ("avcC"/"hvcC"). */
static FM_Result mp4_parse_nal_config(FM_Video *v, const Mp4Box *box,
                                      FM_Bool hevc, FM_VideoPriv *priv)
{
    uint64_t box_end = box->start + box->size;
    FM_Result r;

    if (box_end < box->data)
    {
        return FM_ERR_CORRUPTED;
    }
    r = hevc ? video_parse_hvcc(v, box->data, box_end - box->data, priv)
             : video_parse_avcc(v, box->data, box_end - box->data, priv);
    if (r != FM_OK)
    {
        FM_ERR_LOG("mp4: %s unusable (%s) -- frames will not decode",
                   hevc ? "hvcC" : "avcC", FM_ResultString(r));
    }
    return r;
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

/* Entries a table box can really hold: `count` as declared, but never
 * more than fit between the table start and the end of the box.  (A
 * damaged count would otherwise be walked as if it were true.) */
static uint32_t mp4_table_count(const Mp4Box *box, uint64_t table_off,
                                uint32_t entry_bytes, uint32_t count)
{
    uint64_t end = box->start + box->size;
    uint64_t room = (end > table_off) ? (end - table_off) / entry_bytes : 0u;

    return (count > room) ? (uint32_t)room : count;
}

/* objectTypeIndication of an "mp4v" sample entry (0 = no esds found).
 * "mp4v" is a wrapper like "mp4a": the esds says what is inside.
 * 0x20 is MPEG-4 Visual, 0x60..0x65 MPEG-2, 0x6C JPEG (how ffmpeg
 * writes Motion JPEG into an .mp4). */
static uint32_t mp4_visual_object_type(FM_Video *v, const Mp4Box *entry)
{
    uint64_t pos = entry->data + 78u;       /* VisualSampleEntry fields */
    uint64_t end = entry->start + entry->size;
    uint8_t d[40];
    uint32_t n;
    uint32_t i = 4u;                        /* version + flags */
    uint32_t pass;

    for (;;)
    {
        Mp4Box b;
        uint64_t len;

        if (pos + 8u > end || mp4_box_at(v, pos, end, &b) != FM_OK)
        {
            return 0u;
        }
        if (memcmp(b.type, "esds", 4) != 0)
        {
            pos = b.start + b.size;
            continue;
        }
        len = b.start + b.size - b.data;
        n = (len > sizeof(d)) ? (uint32_t)sizeof(d) : (uint32_t)len;
        if (video_read_at(&v->file, b.data, d, n) != FM_OK)
        {
            return 0u;
        }
        break;
    }
    /* ES_Descriptor (3), then DecoderConfigDescriptor (4) inside it */
    for (pass = 0u; pass < 2u; pass++)
    {
        uint32_t tag;
        uint32_t k;

        if (i >= n)
        {
            return 0u;
        }
        tag = d[i++];
        for (k = 0u; k < 4u && i < n; k++)
        {
            if ((d[i++] & 0x80u) == 0u)
            {
                break;
            }
        }
        if (pass == 0u)
        {
            uint32_t flags;

            if (tag != 0x03u || i + 3u > n)
            {
                return 0u;
            }
            flags = d[i + 2u];
            i += 3u;
            if ((flags & 0x80u) != 0u) i += 2u;             /* dependsOn */
            if ((flags & 0x40u) != 0u)                      /* URL       */
            {
                if (i >= n) return 0u;
                i += 1u + d[i];
            }
            if ((flags & 0x20u) != 0u) i += 2u;             /* OCR       */
        }
        else if (tag != 0x04u || i >= n)
        {
            return 0u;
        }
    }
    return d[i];
}

/* The "esds" box of an mp4a sample entry: what the track really is.
 *
 *   ES_Descriptor (3) > DecoderConfigDescriptor (4): objectTypeIndication
 *                     > DecoderSpecificInfo (5): the AudioSpecificConfig
 *
 * The sample entry itself only has a 16-bit rate field (96 kHz does not
 * fit, HE-AAC files put the output rate there) and says nothing about
 * the object type; and "mp4a" is also what MP3-in-MP4 uses.
 * Returns the objectTypeIndication (0 = no esds found). */
static uint32_t mp4_parse_esds(FM_Video *v, FM_VideoPriv *priv,
                               const Mp4Box *entry, uint32_t qt_version,
                               uint32_t *rate, uint32_t *channels)
{
    uint64_t pos = entry->data + 28u;
    uint64_t end = entry->start + entry->size;
    uint32_t depth;
    uint8_t d[160];
    uint32_t n = 0u;
    uint32_t i = 4u;            /* version + flags */
    uint32_t oti = 0u;
    uint64_t esds_off = 0u;     /* file offset of d[0] */
    uint64_t esds_len = 0u;     /* bytes in the box, may exceed sizeof(d) */
    FM_Bool found = FM_FALSE;

    /* QuickTime sound description versions 1 / 2 are longer */
    if (qt_version == 1u) pos += 16u;
    else if (qt_version == 2u) pos += 36u;

    for (depth = 0u; depth < 2u && !found; depth++)
    {
        FM_Bool down = FM_FALSE;

        while (pos + 8u <= end)
        {
            Mp4Box b;

            if (mp4_box_at(v, pos, end, &b) != FM_OK)
            {
                break;
            }
            if (memcmp(b.type, "esds", 4) == 0)
            {
                uint64_t len = b.start + b.size - b.data;

                n = (len > sizeof(d)) ? (uint32_t)sizeof(d) : (uint32_t)len;
                if (video_read_at(&v->file, b.data, d, n) == FM_OK)
                {
                    found = FM_TRUE;
                    esds_off = b.data;
                    esds_len = len;
                }
                break;
            }
            if (memcmp(b.type, "wave", 4) == 0)
            {
                /* QuickTime keeps the esds one level down */
                pos = b.data;
                end = b.start + b.size;
                down = FM_TRUE;
                break;
            }
            pos = b.start + b.size;
        }
        if (!down)
        {
            break;
        }
    }
    if (!found)
    {
        return 0u;
    }
    while (i + 2u <= n)
    {
        uint32_t tag = d[i++];
        uint32_t len = 0u;
        uint32_t k;

        for (k = 0u; k < 4u && i < n; k++)
        {
            uint32_t c = d[i++];

            len = (len << 7) | (c & 0x7Fu);
            if ((c & 0x80u) == 0u)
            {
                break;
            }
        }
        if (tag == 0x03u)
        {
            /* ES_ID, flags, then the optional fields the flags name */
            uint32_t flags;

            if (i + 3u > n) break;
            flags = d[i + 2u];
            i += 3u;
            if (flags & 0x80u) i += 2u;
            if (flags & 0x40u)
            {
                if (i >= n) break;
                i += 1u + d[i];
            }
            if (flags & 0x20u) i += 2u;
        }
        else if (tag == 0x04u)
        {
            if (i + 13u > n) break;
            oti = d[i];
            i += 13u;
        }
        else if (tag == 0x05u)
        {
            uint32_t aot = 0u;

            if (oti == 0xDDu)
            {
                /* Vorbis: the descriptor is the three header packets,
                 * laced as in a Matroska CodecPrivate.  They are some
                 * KB, far more than d[] holds: read them from the file. */
                uint64_t room = (esds_len > i) ? esds_len - i : 0u;

                (void)video_audio_private_load(v, priv, esds_off + i,
                                               (len < room) ? len : room);
                break;
            }
            if (len > n - i) len = n - i;
            if ((oti == 0x40u || oti == 0u || (oti >= 0x66u && oti <= 0x68u)) &&
                video_asc_parse(d + i, len, &aot, rate, channels))
            {
                priv->audio_aot = (uint8_t)((aot > 255u) ? 0u : aot);
            }
            break;
        }
        else
        {
            i += len;
        }
    }
    if (oti >= 0x66u && oti <= 0x68u && priv->audio_aot == 0u)
    {
        /* MPEG-2 AAC Main / LC / SSR */
        priv->audio_aot = (uint8_t)(oti - 0x65u);
    }
    return oti;
}

/* Find the chunk-offset box of an stbl: 32-bit "stco" or 64-bit "co64". */
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
        *off = box.data + 8;
        *entry_bytes = 4u;
        *count = mp4_table_count(&box, *off, 4u, video_be32(h + 4));
        return FM_OK;
    }
    if (mp4_find_deep(v, stbl->data, stbl->start + stbl->size, "co64",
                      &box) == FM_OK &&
        box.size >= 8 + 8 &&
        video_read_at(&v->file, box.data, h, 8) == FM_OK)
    {
        *off = box.data + 8;
        *entry_bytes = 8u;
        *count = mp4_table_count(&box, *off, 8u, video_be32(h + 4));
        return FM_OK;
    }
    return FM_ERR_NOT_FOUND;
}

/* Samples-per-chunk for the chunk the cursor is entering: apply every
 * stsc entry whose first_chunk (1-based) has been reached.  Chunks in
 * front of the first entry, or a track without stsc, hold one sample. */
static FM_Result mp4_cur_per_chunk(FM_Video *v, const Mp4Track *t,
                                   Mp4Cursor *c)
{
    while (c->stsc_idx < t->stsc_count)
    {
        uint8_t e[8];

        if (video_read_at(&v->file,
                          t->stsc_off + (uint64_t)c->stsc_idx * 12u, e,
                          8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        if (video_be32(e) > c->chunk + 1u)
        {
            break;
        }
        c->per_chunk = video_be32(e + 4);
        c->stsc_idx++;
    }
    return FM_OK;
}

static FM_Result mp4_sample_size(FM_Video *v, const Mp4Track *t,
                                 uint32_t sample, uint32_t *size)
{
    uint8_t s[4];

    if (t->sample_size != 0u)
    {
        *size = t->sample_size;
        return FM_OK;
    }
    if (video_read_at(&v->file, t->stsz_off + (uint64_t)sample * 4u, s,
                      4) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    *size = video_be32(s);
    return FM_OK;
}

static void mp4_cur_reset(Mp4Cursor *c)
{
    memset(c, 0, sizeof(*c));
    c->per_chunk = 1u;
}

/* Move cursor `c` (which is on a sample <= idx) forward to sample `idx`
 * and report where that sample is.  Whole chunks are stepped over from
 * the stsc table alone; only the samples in front of `idx` inside its
 * own chunk cost a size lookup, so sequential playback is one lookup
 * per sample and a long forward seek is one step per chunk.
 *
 * FM_ERR_EOF when the tables hold no such sample. */
static FM_Result mp4_cur_seek(FM_Video *v, const Mp4Track *t, Mp4Cursor *c,
                              uint32_t idx, uint64_t *off, uint32_t *size)
{
    FM_Result r;

    if (idx >= t->sample_count || idx < c->sample)
    {
        return FM_ERR_EOF;
    }
    for (;;)
    {
        uint32_t left;

        if (c->chunk >= t->stco_count)
        {
            return FM_ERR_EOF;
        }
        if (c->in_chunk == 0u)
        {
            r = mp4_cur_per_chunk(v, t, c);
            if (r != FM_OK)
            {
                return r;
            }
        }
        left = (c->per_chunk > c->in_chunk) ? c->per_chunk - c->in_chunk : 0u;
        if (idx - c->sample < left)
        {
            break;                      /* idx lives in this chunk */
        }
        c->sample += left;
        c->chunk++;
        c->in_chunk = 0u;
        c->off_valid = FM_FALSE;
    }
    if (!c->off_valid)
    {
        uint8_t h[8];

        if (video_read_at(&v->file,
                          t->stco_off + (uint64_t)c->chunk * t->stco_entry,
                          h, t->stco_entry) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        c->off = (t->stco_entry == 8u) ? video_be64(h) : video_be32(h);
        c->off_valid = FM_TRUE;
    }
    while (c->sample < idx)
    {
        uint32_t sz;

        r = mp4_sample_size(v, t, c->sample, &sz);
        if (r != FM_OK)
        {
            return r;
        }
        c->off += sz;
        c->sample++;
        c->in_chunk++;
    }
    r = mp4_sample_size(v, t, idx, size);
    if (r != FM_OK)
    {
        return r;
    }
    *off = c->off;
    return FM_OK;
}

/* Samples the chunk tables really describe (never more than stsz
 * declares): the sum of samples-per-chunk over every chunk.  Reads the
 * stsc table only. */
static FM_Result mp4_count_samples(FM_Video *v, const Mp4Track *t,
                                   uint32_t *total)
{
    Mp4Cursor c;
    uint64_t sum = 0;

    mp4_cur_reset(&c);
    while (c.chunk < t->stco_count && sum < t->sample_count)
    {
        FM_Result r = mp4_cur_per_chunk(v, t, &c);

        if (r != FM_OK)
        {
            return r;
        }
        if (c.stsc_idx >= t->stsc_count)
        {
            /* no further change: the rest of the chunks in one go */
            sum += (uint64_t)(t->stco_count - c.chunk) * c.per_chunk;
            break;
        }
        sum += c.per_chunk;
        c.chunk++;
    }
    *total = (sum > t->sample_count) ? t->sample_count : (uint32_t)sum;
    return FM_OK;
}

/* Fill an index table with the first `max` samples of a track and leave
 * the track ready to stream the rest: *out_count samples are in the
 * table, *total exist. */
static FM_Result mp4_build_index(FM_Video *v, Mp4Track *t,
                                 FM_VideoFrame *frames, uint32_t max,
                                 uint32_t *out_count, uint32_t *total)
{
    Mp4Cursor c;
    FM_Result r;

    if (t->stco_entry != 4u && t->stco_entry != 8u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    mp4_cur_reset(&c);
    *out_count = 0;
    *total = 0;
    while (*out_count < max)
    {
        uint64_t off = 0;
        uint32_t sz = 0;

        r = mp4_cur_seek(v, t, &c, *out_count, &off, &sz);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        frames[*out_count].offset = off;
        frames[*out_count].size = sz;
        (*out_count)++;
    }
    if (*out_count == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
    t->start = c;               /* on the last indexed sample */
    t->cur = c;
    r = mp4_count_samples(v, t, total);
    if (r != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    if (*total < *out_count)
    {
        *total = *out_count;
    }
    return FM_OK;
}

/*
 * Uncompressed audio in MP4 / QuickTime.  FM_TRUE when `entry` (its first
 * 28 bytes in `a`) is a layout that is played; the format goes to priv,
 * the bits a sample to *bits.
 *
 *   sowt / twos   16-bit little / big endian; with 8 bits: signed
 *   raw           8-bit unsigned
 *   in24 / in32   24 / 32-bit, big endian unless an "enda" box says little
 *   fl32          32-bit float, the same rule
 *   lpcm          QuickTime version-2 entry: width and flags in the entry
 *   ipcm / fpcm   ISO 23003-5: width and byte order in a "pcmC" box
 *
 * 64-bit samples (fl64, lpcm / fpcm with 64 bits) are not played and give
 * no track.  ("alac" was once mapped to PCM here: Apple Lossless is a
 * compressed codec, and playing its packets as PCM is noise.)
 */
static FM_Bool mp4_pcm_entry(FM_Video *v, FM_VideoPriv *priv,
                             const Mp4Box *entry, const uint8_t *a,
                             uint32_t *bits)
{
    uint32_t version = video_be16(a + 8);
    uint64_t kids = entry->data + 28u +
                    ((version == 1u) ? 16u : (version == 2u) ? 36u : 0u);
    uint64_t end = entry->start + entry->size;
    uint32_t width = video_be16(a + 18);
    FM_Bool be = FM_TRUE;
    FM_Bool is_float = FM_FALSE;
    FM_Bool s8 = FM_FALSE;
    Mp4Box box;
    uint8_t b[36];

    if (memcmp(entry->type, "sowt", 4) == 0 ||
        memcmp(entry->type, "twos", 4) == 0)
    {
        if (width != 8u && width != 16u)
        {
            return FM_FALSE;
        }
        be = (entry->type[0] == 't') ? FM_TRUE : FM_FALSE;
        s8 = FM_TRUE;
    }
    else if (memcmp(entry->type, "raw ", 4) == 0)
    {
        if (width != 8u)
        {
            return FM_FALSE;
        }
    }
    else if (memcmp(entry->type, "in24", 4) == 0 ||
             memcmp(entry->type, "in32", 4) == 0 ||
             memcmp(entry->type, "fl32", 4) == 0)
    {
        width = (entry->type[3] == '4') ? 24u : 32u;
        is_float = (entry->type[0] == 'f') ? FM_TRUE : FM_FALSE;
        /* wave { frma, enda }: enda 1 = little endian */
        if (kids < end && mp4_find_deep(v, kids, end, "wave", &box) == FM_OK &&
            mp4_find_deep(v, box.data, box.start + box.size, "enda",
                          &box) == FM_OK &&
            box.start + box.size >= box.data + 2u &&
            video_read_at(&v->file, box.data, b, 2) == FM_OK &&
            video_be16(b) == 1u)
        {
            be = FM_FALSE;
        }
    }
    else if (memcmp(entry->type, "lpcm", 4) == 0 && version == 2u)
    {
        /* after the 28 bytes: struct size, rate (f64), channels,
         * 0x7F000000, bits a channel, flags (1 float, 2 big endian,
         * 4 signed), ... */
        if (end < entry->data + 64u ||
            video_read_at(&v->file, entry->data + 28u, b, 36) != FM_OK)
        {
            return FM_FALSE;
        }
        width = video_be32(b + 20);
        is_float = ((video_be32(b + 24) & 1u) != 0u) ? FM_TRUE : FM_FALSE;
        be = ((video_be32(b + 24) & 2u) != 0u) ? FM_TRUE : FM_FALSE;
        s8 = ((video_be32(b + 24) & 4u) != 0u) ? FM_TRUE : FM_FALSE;
    }
    else if (memcmp(entry->type, "ipcm", 4) == 0 ||
             memcmp(entry->type, "fpcm", 4) == 0)
    {
        /* pcmC: version + flags (4), format flags (bit 0 = little
         * endian), sample size */
        if (kids >= end || mp4_find_deep(v, kids, end, "pcmC", &box) != FM_OK ||
            box.start + box.size < box.data + 6u ||
            video_read_at(&v->file, box.data, b, 6) != FM_OK)
        {
            return FM_FALSE;
        }
        is_float = (entry->type[0] == 'f') ? FM_TRUE : FM_FALSE;
        be = ((b[4] & 1u) != 0u) ? FM_FALSE : FM_TRUE;
        width = b[5];
        s8 = FM_TRUE;
    }
    else
    {
        return FM_FALSE;
    }
    if (is_float ? (width != 32u)
                 : (width != 8u && width != 16u && width != 24u &&
                    width != 32u))
    {
        return FM_FALSE;
    }
    priv->audio_pcm_be = (width > 8u) ? be : FM_FALSE;
    priv->audio_pcm_float = is_float;
    priv->audio_pcm_s8 = (width == 8u) ? s8 : FM_FALSE;
    *bits = width;
    return FM_TRUE;
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
    uint32_t pcm_bits = 16u;
    FM_Bool have_mp4a = FM_FALSE;

    priv->audio_pcm_be = FM_FALSE;
    priv->audio_pcm_float = FM_FALSE;
    priv->audio_pcm_s8 = FM_FALSE;
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
                    uint32_t ch32;
                    uint32_t oti;

                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    ch32 = channels;
                    priv->audio_codec = FM_VIDEO_AUDIO_AAC;
                    priv->audio_aot = 0u;
                    oti = mp4_parse_esds(v, priv, &entry, video_be16(a + 8),
                                         &rate, &ch32);
                    channels = (uint16_t)ch32;
                    if (oti == 0x69u || oti == 0x6Bu)
                    {
                        /* MPEG-2 / MPEG-1 audio: MP3 in an "mp4a" entry.
                         * It used to be announced as AAC and its frames
                         * wrapped in ADTS headers. */
                        priv->audio_codec = FM_VIDEO_AUDIO_MP3;
                        if (rate == 0u) { rate = 44100u; }
                    }
                    else if (oti == 0xDDu)
                    {
                        /* Vorbis in an "mp4a" entry (what ffmpeg writes) */
                        priv->audio_codec = FM_VIDEO_AUDIO_VORBIS;
                        if (rate == 0u) { rate = 44100u; }
                    }
                    if (oti == 0u || oti == 0x40u ||
                        (oti >= 0x66u && oti <= 0x69u) || oti == 0x6Bu ||
                        oti == 0xDDu)
                    {
                        have_mp4a = FM_TRUE;
                    }
                    else
                    {
                        /* AC-3, DTS, QCELP, ...: anything else used to be
                         * announced as AAC and handed out in ADTS
                         * frames.  No track is better than a wrong one. */
                        priv->audio_codec = FM_VIDEO_AUDIO_NONE;
                        FM_ERR_LOG("mp4: audio object type 0x%02X not "
                                   "supported -- track ignored",
                                   (unsigned)oti);
                    }
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
                                uint64_t plen = (db.start + db.size > payload) ? (db.start + db.size - payload) : 0u;
                                (void)video_audio_private_load(v, priv, payload, plen);
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
                else if (memcmp(entry.type, "ulaw", 4) == 0 ||
                         memcmp(entry.type, "alaw", 4) == 0)
                {
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    priv->audio_codec = (memcmp(entry.type, "ulaw", 4) == 0)
                                            ? FM_VIDEO_AUDIO_ULAW
                                            : FM_VIDEO_AUDIO_ALAW;
                    have_mp4a = FM_TRUE;
                }
                else if (mp4_pcm_entry(v, priv, &entry, a, &pcm_bits))
                {
                    /* QuickTime little-endian 16-bit PCM: what the PCM
                     * path hands out as it is.  ("alac" used to be
                     * mapped here: Apple Lossless is a compressed
                     * codec, and playing its packets as PCM is noise.
                     * It is now reported as no usable audio track.) */
                    channels = video_be16(a + 16);
                    rate = video_be32(a + 24) >> 16;
                    priv->audio_codec = FM_VIDEO_AUDIO_PCM;
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
        if (sample_size == 0u)
        {
            sample_count = mp4_table_count(&stsz, stsz_off, 4u,
                                           sample_count);
        }
    }
    if (mp4_find_deep(v, stbl.data, stbl.start + stbl.size, "stsc", &stsc) == FM_OK &&
        stsc.size >= 8 + 8 &&
        video_read_at(&v->file, stsc.data, h, 8) == FM_OK)
    {
        stsc_off = stsc.data + 8;
        stsc_count = mp4_table_count(&stsc, stsc_off, 12u,
                                     video_be32(h + 4));
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
    if (priv->audio_codec == FM_VIDEO_AUDIO_PCM && sample_size == 1u &&
        (uint32_t)channels * (pcm_bits / 8u) > 1u)
    {
        /* Classic QuickTime: the sample size of an uncompressed track
         * counts samples, not bytes. */
        sample_size = (uint32_t)channels * (pcm_bits / 8u);
    }
    priv->audio_bits = (priv->audio_codec == FM_VIDEO_AUDIO_PCM)
                           ? (uint16_t)pcm_bits
                     : (priv->audio_codec == FM_VIDEO_AUDIO_ALAW ||
                        priv->audio_codec == FM_VIDEO_AUDIO_ULAW ||
                        priv->audio_codec == FM_VIDEO_AUDIO_ADPCM)
                           ? 16u
                           : 0u;
    priv->audio_present = FM_TRUE;
    memset(&priv->mp4a, 0, sizeof(priv->mp4a));
    priv->mp4a.stco_off = stco_off;
    priv->mp4a.stco_count = stco_count;
    priv->mp4a.stco_entry = stco_entry;
    priv->mp4a.stsc_off = stsc_off;
    priv->mp4a.stsc_count = stsc_count;
    priv->mp4a.stsz_off = stsz_off;
    priv->mp4a.sample_size = sample_size;
    priv->mp4a.sample_count = sample_count;
    /* Packets past the index table are streamed from the sample tables
     * (video_tail_locate); the track used to go silent there. */
    return mp4_build_index(v, &priv->mp4a, priv->audio_frames,
                           FM_VIDEO_MAX_AUDIO_FRAMES, &priv->audio_count,
                           &priv->atail.total);
}

/*
 * The edit list of an audio track: where the sound starts in the decoded
 * stream and how long it is.  An MP3, AAC or Opus encoder puts a fixed
 * number of frames in front (1105 for LAME, 312 for libopus, ...) and
 * rounds the end up to a whole packet; the muxer records the part that
 * is sound as one edit.
 *
 * Used when the track has exactly one edit that starts within the first
 * second (anything else is an edited programme, not an encoder delay,
 * and the track is then played from its first packet as before).
 */
static void mp4_audio_edit(FM_Video *v, const Mp4Box *moov,
                           const Mp4Box *trak, FM_VideoPriv *priv)
{
    Mp4Box box;
    uint8_t b[32];
    uint32_t movie_scale = 0u;
    uint32_t media_scale = 0u;
    uint32_t count;
    uint32_t i;
    uint32_t edits = 0u;
    uint64_t duration = 0u;
    uint64_t start = 0u;
    uint64_t skip;

    if (priv->audio_rate == 0u)
    {
        return;
    }
    /* mvhd / mdhd: version + flags, two times (4 or 8 bytes each), scale */
    if (mp4_find_deep(v, moov->data, moov->start + moov->size, "mvhd",
                      &box) == FM_OK &&
        video_read_at(&v->file, box.data, b, 24) == FM_OK)
    {
        movie_scale = video_be32(b + ((b[0] == 1u) ? 20u : 12u));
    }
    if (mp4_find_deep(v, trak->data, trak->start + trak->size, "mdhd",
                      &box) == FM_OK &&
        video_read_at(&v->file, box.data, b, 24) == FM_OK)
    {
        media_scale = video_be32(b + ((b[0] == 1u) ? 20u : 12u));
    }
    if (movie_scale == 0u || media_scale == 0u ||
        mp4_find_deep(v, trak->data, trak->start + trak->size, "elst",
                      &box) != FM_OK ||
        box.start + box.size < box.data + 8u ||
        video_read_at(&v->file, box.data, b, 8) != FM_OK)
    {
        return;
    }
    count = video_be32(b + 4);
    for (i = 0u; i < count && i < 8u; i++)
    {
        uint32_t entry = (b[0] == 1u) ? 20u : 12u;
        uint8_t e[20];
        uint64_t dur;
        uint64_t time;

        if (box.start + box.size < box.data + 8u + (uint64_t)(i + 1u) * entry ||
            video_read_at(&v->file, box.data + 8u + (uint64_t)i * entry, e,
                          entry) != FM_OK)
        {
            return;
        }
        if (b[0] == 1u)
        {
            dur = ((uint64_t)video_be32(e) << 32) | video_be32(e + 4);
            time = ((uint64_t)video_be32(e + 8) << 32) | video_be32(e + 12);
        }
        else
        {
            dur = video_be32(e);
            time = video_be32(e + 4);
            if (time == 0xFFFFFFFFu)
            {
                time = UINT64_MAX;
            }
        }
        if (time == UINT64_MAX)
        {
            continue;                    /* an empty edit: a pause */
        }
        edits++;
        duration = dur;
        start = time;
    }
    if (edits != 1u || count > 8u || start > media_scale ||
        duration > (uint64_t)0xFFFFFFFFu * 16u)
    {
        return;
    }
    skip = (start * priv->audio_rate + media_scale / 2u) / media_scale;
    priv->audio_skip = (uint32_t)skip;
    priv->audio_limit = (duration * priv->audio_rate + movie_scale / 2u) /
                        movie_scale;
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
    /* mp4_walk() fills the box whenever it returns FM_OK; the zeroing is
     * for compilers that cannot see that (-Wmaybe-uninitialized). */
    memset(&moov, 0, sizeof(moov));
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
            priv->audio_skip = 0u;
            priv->audio_limit = 0u;
            if (mp4_parse_audio_trak(v, &trak, priv) == FM_OK)
            {
                mp4_audio_edit(v, &moov, &trak, priv);
            }
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
                                (void)mp4_parse_nal_config(v, &avcc, FM_FALSE,
                                                           priv);
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
                        /* hvcC: VPS/SPS/PPS + NAL length size.  Samples
                         * are length-prefixed exactly like avc1, so the
                         * same Annex-B conversion applies; with no (or a
                         * broken) hvcC they are handed out untouched as
                         * before. */
                        {
                            Mp4Box hvcc;

                            if (mp4_find_deep(v, entry.data + 78u,
                                             entry.start + entry.size, "hvcC",
                                             &hvcc) == FM_OK &&
                                mp4_parse_nal_config(v, &hvcc, FM_TRUE,
                                                     priv) == FM_OK)
                            {
                                priv->avc1 = FM_TRUE;
                            }
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
                        uint32_t oti = mp4_visual_object_type(v, &entry);

                        if (oti == 0x6Cu)
                        {
                            v->info.codec = FM_VIDEO_CODEC_MJPEG;
                        }
                        else if (oti >= 0x60u && oti <= 0x65u)
                        {
                            v->info.codec = FM_VIDEO_CODEC_MPEG2;
                        }
                        else
                        {
                            v->info.codec = FM_VIDEO_CODEC_MPEG4;
                        }
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
                if (sample_size == 0u)
                {
                    sample_count = mp4_table_count(&stsz, stsz_off, 4u,
                                                   sample_count);
                }
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
                stsc_off = stsc.data + 8;
                stsc_count = mp4_table_count(&stsc, stsc_off, 12u,
                                             video_be32(h + 4));
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
    memset(&priv->mp4v, 0, sizeof(priv->mp4v));
    priv->mp4v.stco_off = stco_off;
    priv->mp4v.stco_count = stco_count;
    priv->mp4v.stco_entry = stco_entry;
    priv->mp4v.stsc_off = stsc_off;
    priv->mp4v.stsc_count = stsc_count;
    priv->mp4v.stsz_off = stsz_off;
    priv->mp4v.sample_size = sample_size;
    priv->mp4v.sample_count = sample_count;
    r = mp4_build_index(v, &priv->mp4v, priv->frames, FM_VIDEO_MAX_FRAMES,
                        &priv->count, &priv->vtail.total);
    if (r != FM_OK)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* Frames past the index table are streamed from the sample tables
     * (video_tail_locate): a clip longer than FM_VIDEO_MAX_FRAMES used
     * to end there. */
    priv->tail_kind = VIDEO_TAIL_MP4;
    v->info.frame_count = priv->vtail.total;
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
    default: t += 0x108u; t = (uint16_t)(t << (seg - 1u)); break;
    }
    return (a & 0x80u) ? (int16_t)t : (int16_t)(-(int16_t)t);
}
static int16_t ulaw_decode(uint8_t u)
{
    int16_t t;
    u = (uint8_t)~u;
    t = (int16_t)((((u & 0x0Fu) << 3) + 0x84u) << ((u & 0x70u) >> 4));
    return (u & 0x80u) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

/*
 * ADPCM as WAV, AVI and MKV (A_MS/ACM) carry it: the stream is a row of
 * blocks of nBlockAlign bytes, each decodable on its own.
 *
 * This used to be one routine that took a whole chunk for a single mono
 * IMA block.  A stereo track came out as noise (both channels' headers
 * and data read as one channel), a chunk with several blocks decoded
 * the later block headers as samples, and Microsoft ADPCM (tag 2), a
 * different codec altogether, was run through the IMA tables: full-scale
 * noise.  Both formats are now decoded block by block and compared
 * sample for sample with ffmpeg (test/audit_v3_tests.h).  The block
 * decoders are in fm_adpcm.h, shared with the WAV reader.
 */
#define FM_ADPCM_WANT_IMA
#include "fm_adpcm.h"

/* ------------------------------------------------------------------ */
/* MKV / WebM (EBML)                                                   */
/* ------------------------------------------------------------------ */
#define MKV_ID_SEGMENT       0x18538067u
#define MKV_ID_TRACKS        0x1654AE6Bu
#define MKV_ID_TRACKENTRY    0xAEu
#define MKV_ID_TRACKNUMBER   0xD7u
#define MKV_ID_TRACKTYPE     0x83u
#define MKV_ID_CODECID       0x86u
#define MKV_ID_CODECPRIVATE  0x63A2u
#define MKV_ID_DEFAULTDUR    0x23E383u
#define MKV_ID_VIDEO         0xE0u
#define MKV_ID_PIXELWIDTH    0xB0u
#define MKV_ID_PIXELHEIGHT   0xBAu
#define MKV_ID_AUDIO         0xE1u
#define MKV_ID_SAMPLINGFREQ  0xB5u
#define MKV_ID_CHANNELS      0x9Fu
#define MKV_ID_BITDEPTH      0x6264u
#define MKV_ID_CODECDELAY    0x56AAu     /* ns of encoder delay           */
#define MKV_ID_DISCARDPAD    0x75A2u     /* ns to drop at a block's end   */
#define MKV_ID_CLUSTER       0x1F43B675u
#define MKV_ID_SIMPLEBLOCK   0xA3u
#define MKV_ID_BLOCKGROUP    0xA0u
#define MKV_ID_BLOCK         0xA1u
#define MKV_MAX_LACE         256u    /* lace count is one byte + 1 */
#define MKV_NO_TRACK         0xFFFFFFFFu

typedef struct
{
    uint32_t id;
    uint64_t data;       /* file offset of the payload                */
    uint64_t size;       /* payload bytes, never past the parent      */
    FM_Bool  truncated;  /* the declared size ran past the parent     */
} EbmlElem;

/* Width of an EBML variable-length integer, from its first byte: the
 * number of leading zero bits + 1.  0 when wider than `max_len`. */
static uint32_t ebml_vint_len(uint8_t b, uint32_t max_len)
{
    uint32_t n;

    for (n = 1u; n <= max_len; n++)
    {
        if ((b & (uint8_t)(0x80u >> (n - 1u))) != 0u)
        {
            return n;
        }
    }
    return 0u;
}

/* Read the element header at `pos` inside a parent ending at `end`.
 *
 * The whole header (ID 1..4 bytes + size 1..8 bytes) is fetched with ONE
 * read of at most 12 bytes and parsed from that buffer.  The previous
 * version read a single byte and then looked at hdr[ilen] -- for every
 * 1-byte ID (SimpleBlock, Timecode, PixelWidth, TrackNumber ...) that
 * was a byte it had never read, so the "size" was stack garbage and the
 * walk derailed after the first such element; with 5..8-byte sizes it
 * also indexed past its 8-byte buffer.
 *
 * An unknown size (all ones) and a size that runs past the parent are
 * both clamped to the parent, so no caller can be sent outside the
 * file by a size field. */
static FM_Result ebml_read(FM_Video *v, uint64_t pos, uint64_t end,
                           EbmlElem *e)
{
    uint8_t b[12];
    uint32_t avail;
    uint32_t ilen;
    uint32_t slen;
    uint32_t i;
    uint64_t sz;
    uint64_t room;

    if (pos >= end || end - pos < 2u)
    {
        return FM_ERR_CORRUPTED;
    }
    avail = (end - pos > sizeof(b)) ? (uint32_t)sizeof(b)
                                    : (uint32_t)(end - pos);
    if (video_read_at(&v->file, pos, b, avail) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    ilen = ebml_vint_len(b[0], 4u);
    if (ilen == 0u || ilen >= avail)
    {
        return FM_ERR_CORRUPTED;
    }
    slen = ebml_vint_len(b[ilen], 8u);
    if (slen == 0u || ilen + slen > avail)
    {
        return FM_ERR_CORRUPTED;
    }
    e->id = 0u;
    for (i = 0; i < ilen; i++)
    {
        e->id = (e->id << 8) | b[i];
    }
    sz = (uint64_t)(b[ilen] & (uint8_t)(0xFFu >> slen));
    for (i = 1; i < slen; i++)
    {
        sz = (sz << 8) | b[ilen + i];
    }
    e->data = pos + ilen + slen;
    room = end - e->data;
    e->truncated = FM_FALSE;
    if (sz == ((1ULL << (7u * slen)) - 1ULL))
    {
        sz = room;                    /* unknown size: to end of parent */
    }
    else if (sz > room)
    {
        sz = room;
        e->truncated = FM_TRUE;
    }
    e->size = sz;
    return FM_OK;
}

/* EBML unsigned integer payload: 0..8 bytes, big-endian.  One helper
 * for every integer element, so the read length and the bytes consumed
 * can no longer disagree (PixelWidth read 4 bytes but looped over the
 * declared size -- a stack over-read for a 5..8-byte value). */
static FM_Result mkv_read_uint(FM_Video *v, const EbmlElem *e, uint64_t *out)
{
    uint8_t b[8];
    uint64_t val = 0;
    uint32_t n;
    uint32_t i;

    if (e->size > 8u)
    {
        return FM_ERR_CORRUPTED;
    }
    n = (uint32_t)e->size;
    if (n != 0u && video_read_at(&v->file, e->data, b, n) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    for (i = 0; i < n; i++)
    {
        val = (val << 8) | b[i];
    }
    *out = val;
    return FM_OK;
}

/* EBML float payload: 4 or 8 bytes, big-endian IEEE-754 (0 bytes = 0.0).
 * SamplingFrequency is one of these; it used to be assembled as an
 * integer and byte-swapped into a double, which is ~0 for every real
 * file, so the rate silently fell back to 44100 Hz. */
static FM_Result mkv_read_float(FM_Video *v, const EbmlElem *e, double *out)
{
    uint8_t b[8];

    if (e->size == 0u)
    {
        *out = 0.0;
        return FM_OK;
    }
    if (e->size == 4u)
    {
        uint32_t u;
        float f;

        if (video_read_at(&v->file, e->data, b, 4) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        u = video_be32(b);
        memcpy(&f, &u, sizeof(f));
        *out = (double)f;
        return FM_OK;
    }
    if (e->size == 8u)
    {
        uint64_t u;
        double d;

        if (video_read_at(&v->file, e->data, b, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        u = video_be64(b);
        memcpy(&d, &u, sizeof(d));
        *out = d;
        return FM_OK;
    }
    return FM_ERR_CORRUPTED;
}

typedef struct
{
    uint64_t number;
    uint64_t type;             /* 1 = video, 2 = audio              */
    uint64_t width;
    uint64_t height;
    uint64_t default_duration; /* ns per frame                      */
    uint64_t channels;
    uint64_t bit_depth;
    uint64_t codec_delay;      /* ns                                */
    double   rate;
    uint64_t priv_off;         /* CodecPrivate location in the file */
    uint64_t priv_size;
    char     codec[32];        /* CodecID, truncated: only ever
                                * compared by prefix               */
} MkvTrack;

static FM_Bool mkv_codec_is(const MkvTrack *t, const char *prefix)
{
    return (strncmp(t->codec, prefix, strlen(prefix)) == 0) ? FM_TRUE
                                                            : FM_FALSE;
}

/* Children of TrackEntry, and of its Video / Audio masters (their IDs
 * do not collide, so one walker serves all three levels). */
static void mkv_parse_track(FM_Video *v, uint64_t pos, uint64_t end,
                            MkvTrack *t, uint32_t depth)
{
    EbmlElem e;

    while (pos < end && ebml_read(v, pos, end, &e) == FM_OK)
    {
        switch (e.id)
        {
        case MKV_ID_TRACKNUMBER:
            (void)mkv_read_uint(v, &e, &t->number);
            break;
        case MKV_ID_TRACKTYPE:
            (void)mkv_read_uint(v, &e, &t->type);
            break;
        case MKV_ID_DEFAULTDUR:
            (void)mkv_read_uint(v, &e, &t->default_duration);
            break;
        case MKV_ID_PIXELWIDTH:
            (void)mkv_read_uint(v, &e, &t->width);
            break;
        case MKV_ID_PIXELHEIGHT:
            (void)mkv_read_uint(v, &e, &t->height);
            break;
        case MKV_ID_CHANNELS:
            (void)mkv_read_uint(v, &e, &t->channels);
            break;
        case MKV_ID_BITDEPTH:
            (void)mkv_read_uint(v, &e, &t->bit_depth);
            break;
        case MKV_ID_CODECDELAY:
            (void)mkv_read_uint(v, &e, &t->codec_delay);
            break;
        case MKV_ID_SAMPLINGFREQ:
            (void)mkv_read_float(v, &e, &t->rate);
            break;
        case MKV_ID_CODECID:
        {
            uint32_t n = (e.size >= sizeof(t->codec))
                             ? (uint32_t)sizeof(t->codec) - 1u
                             : (uint32_t)e.size;

            if (n == 0u ||
                video_read_at(&v->file, e.data, t->codec, n) != FM_OK)
            {
                n = 0u;
            }
            t->codec[n] = '\0';
            break;
        }
        case MKV_ID_CODECPRIVATE:
            t->priv_off = e.data;
            t->priv_size = e.size;
            break;
        case MKV_ID_VIDEO:
        case MKV_ID_AUDIO:
            if (depth == 0u)
            {
                mkv_parse_track(v, e.data, e.data + e.size, t, 1u);
            }
            break;
        default:
            break;
        }
        pos = e.data + e.size;
    }
}

static void mkv_setup_video(FM_Video *v, FM_VideoPriv *priv,
                            const MkvTrack *t)
{
    FM_VideoCodec c = FM_VIDEO_CODEC_UNKNOWN;

    if (mkv_codec_is(t, "V_MPEG4/ISO/AVC"))        { c = FM_VIDEO_CODEC_H264; }
    else if (mkv_codec_is(t, "V_MPEGH/ISO/HEVC"))  { c = FM_VIDEO_CODEC_HEVC; }
    else if (mkv_codec_is(t, "V_MPEG4/"))          { c = FM_VIDEO_CODEC_MPEG4; }
    else if (mkv_codec_is(t, "V_MJPEG"))           { c = FM_VIDEO_CODEC_MJPEG; }
    else if (mkv_codec_is(t, "V_VP8"))             { c = FM_VIDEO_CODEC_VP8; }
    else if (mkv_codec_is(t, "V_VP9"))             { c = FM_VIDEO_CODEC_VP9; }
    else if (mkv_codec_is(t, "V_AV1"))             { c = FM_VIDEO_CODEC_AV1; }
    else if (mkv_codec_is(t, "V_MPEG1") ||
             mkv_codec_is(t, "V_MPEG2"))           { c = FM_VIDEO_CODEC_MPEG2; }
    else if (mkv_codec_is(t, "V_THEORA"))          { c = FM_VIDEO_CODEC_THEORA; }
    else if (mkv_codec_is(t, "V_UNCOMPRESSED"))    { c = FM_VIDEO_CODEC_RAW; }
    else if (mkv_codec_is(t, "V_MS/VFW/FOURCC") && t->priv_size >= 20u)
    {
        /* CodecPrivate is a BITMAPINFOHEADER: biCompression names it. */
        uint8_t bih[20];

        if (video_read_at(&v->file, t->priv_off, bih, 20) == FM_OK)
        {
            c = avi_map_fourcc(bih + 16);
        }
    }
    v->info.codec = c;

    /* Matroska stores H.264/HEVC like MP4 does: length-prefixed NALs
     * in the blocks, parameter sets in CodecPrivate (avcC / hvcC).
     * Without this the blocks reached the decoder with no SPS/PPS. */
    if (t->priv_size != 0u &&
        (c == FM_VIDEO_CODEC_H264 || c == FM_VIDEO_CODEC_HEVC))
    {
        FM_Result r = (c == FM_VIDEO_CODEC_H264)
            ? video_parse_avcc(v, t->priv_off, t->priv_size, priv)
            : video_parse_hvcc(v, t->priv_off, t->priv_size, priv);

        if (r == FM_OK)
        {
            priv->avc1 = FM_TRUE;
        }
        else
        {
            FM_ERR_LOG("mkv: video CodecPrivate unusable (%s) -- frames "
                       "will not decode", FM_ResultString(r));
        }
    }
}

static void mkv_setup_audio(FM_Video *v, FM_VideoPriv *priv,
                            const MkvTrack *t)
{
    uint32_t codec = FM_VIDEO_AUDIO_NONE;
    uint32_t rate = 0u;
    uint32_t channels = (uint32_t)((t->channels >= 1u && t->channels <= 8u)
                                       ? t->channels : 1u); /* spec default */
    uint32_t bits = 16u;
    FM_Bool keep_private = FM_TRUE;

    /* SamplingFrequency: IEEE float, spec default 8000 Hz. */
    if (t->rate >= 1000.0 && t->rate <= 768000.0)
    {
        rate = (uint32_t)(t->rate + 0.5);
    }
    else
    {
        rate = 8000u;
    }

    priv->audio_pcm_be = FM_FALSE;
    priv->audio_pcm_float = FM_FALSE;
    if (mkv_codec_is(t, "A_PCM/INT/LIT") || mkv_codec_is(t, "A_PCM/INT/BIG"))
    {
        codec = FM_VIDEO_AUDIO_PCM;
        bits = (t->bit_depth >= 8u && t->bit_depth <= 32u)
                   ? (uint32_t)t->bit_depth : 16u;
        priv->audio_pcm_be = mkv_codec_is(t, "A_PCM/INT/BIG");
    }
    else if (mkv_codec_is(t, "A_PCM/FLOAT/IEEE"))
    {
        /* little-endian; 32-bit only (64-bit: no track) */
        if (t->bit_depth == 32u)
        {
            codec = FM_VIDEO_AUDIO_PCM;
            bits = 32u;
            priv->audio_pcm_float = FM_TRUE;
        }
    }
    else if (mkv_codec_is(t, "A_MPEG/L3"))  { codec = FM_VIDEO_AUDIO_MP3; }
    else if (mkv_codec_is(t, "A_FLAC"))     { codec = FM_VIDEO_AUDIO_FLAC; }
    else if (mkv_codec_is(t, "A_OPUS"))     { codec = FM_VIDEO_AUDIO_OPUS; }
    else if (mkv_codec_is(t, "A_VORBIS"))   { codec = FM_VIDEO_AUDIO_VORBIS; }
    else if (mkv_codec_is(t, "A_AAC"))      { codec = FM_VIDEO_AUDIO_AAC; }
    else if (mkv_codec_is(t, "A_MS/ACM") && t->priv_size >= 14u)
    {
        /* CodecPrivate is a WAVEFORMATEX: wFormatTag names the codec. */
        uint8_t wf[26];
        uint32_t take = (t->priv_size < 26u) ? (uint32_t)t->priv_size : 26u;

        memset(wf, 0, sizeof(wf));
        if (video_read_at(&v->file, t->priv_off, wf, take) == FM_OK)
        {
            uint16_t tag = video_rd16(wf);

            keep_private = FM_FALSE;
            if (video_rd16(wf + 2) >= 1u && video_rd16(wf + 2) <= 8u)
            {
                channels = video_rd16(wf + 2);
            }
            if (video_rd32(wf + 4) != 0u)
            {
                rate = video_rd32(wf + 4);
            }
            if (tag == 0xFFFEu && take >= 26u &&
                (video_rd16(wf + 24) == 1u || video_rd16(wf + 24) == 3u))
            {
                tag = video_rd16(wf + 24);       /* EXTENSIBLE: SubFormat */
            }
            if (tag == 1u)
            {
                codec = FM_VIDEO_AUDIO_PCM;
                bits = (video_rd16(wf + 14) != 0u) ? video_rd16(wf + 14)
                                                   : 16u;
            }
            else if (tag == 3u)
            {
                if (video_rd16(wf + 14) == 32u)
                {
                    codec = FM_VIDEO_AUDIO_PCM;
                    bits = 32u;
                    priv->audio_pcm_float = FM_TRUE;
                }
            }
            else if (tag == 0x0055u) { codec = FM_VIDEO_AUDIO_MP3; }
            else if (tag == 6u)      { codec = FM_VIDEO_AUDIO_ALAW; }
            else if (tag == 7u)      { codec = FM_VIDEO_AUDIO_ULAW; }
            else if (tag == 0x0011u || tag == 0x0002u)
            {
                codec = FM_VIDEO_AUDIO_ADPCM;
                priv->audio_wav_tag = tag;
                priv->audio_block_align = video_rd16(wf + 12);
            }
        }
    }
    if (codec == FM_VIDEO_AUDIO_NONE)
    {
        /* An unknown codec used to be announced as PCM and its packets
         * played as noise.  No track is better than that. */
        FM_ERR_LOG("mkv: audio codec \"%s\" not supported -- track ignored",
                   t->codec);
        return;
    }
    priv->audio_present = FM_TRUE;
    priv->audio_codec = codec;
    priv->audio_rate = rate;
    priv->audio_channels = (uint16_t)channels;
    /* CodecDelay: frames the encoder put in front of the sound (LAME
     * 1105, libopus 312).  Not for Vorbis: its decoder gives nothing for
     * the first packet, which is that delay already.  More than a second
     * is not an encoder delay and is left alone. */
    priv->audio_skip = 0u;
    if (codec != FM_VIDEO_AUDIO_VORBIS && t->codec_delay != 0u &&
        t->codec_delay <= 1000000000u)
    {
        priv->audio_skip = (uint32_t)((t->codec_delay * rate + 500000000u) /
                                      1000000000u);
    }
    priv->audio_bits = (codec == FM_VIDEO_AUDIO_MP3 ||
                        codec == FM_VIDEO_AUDIO_AAC ||
                        codec == FM_VIDEO_AUDIO_VORBIS)
                           ? 0u : (uint16_t)bits;
    priv->audio_private_size = 0u;
    if (keep_private && t->priv_size != 0u)
    {
        /* Read CodecPrivate straight into its final place (no 512-byte
         * stack copy that silently dropped anything larger -- i.e. most
         * Vorbis setup headers). */
        FM_Result rp = video_audio_private_load(v, priv, t->priv_off,
                                                t->priv_size);

        if (rp == FM_OK && codec == FM_VIDEO_AUDIO_AAC)
        {
            /* A_AAC: CodecPrivate is the AudioSpecificConfig */
            uint32_t aot = 0u;
            uint32_t arate = priv->audio_rate;
            uint32_t chan = priv->audio_channels;

            if (video_asc_parse(video_audio_private(priv),
                                priv->audio_private_size, &aot, &arate,
                                &chan))
            {
                priv->audio_aot = (uint8_t)((aot > 255u) ? 0u : aot);
                priv->audio_rate = arate;
                priv->audio_channels = (uint16_t)chan;
            }
        }
        if (rp != FM_OK)
        {
            FM_ERR_LOG("mkv: audio CodecPrivate (%lu bytes) not kept: %s "
                       "(FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX is %lu) -- the "
                       "track will not decode",
                       (unsigned long)t->priv_size, FM_ResultString(rp),
                       (unsigned long)FM_VIDEO_AUDIO_PRIVATE_HEAP_MAX);
        }
    }
}

/* ---- Block / SimpleBlock payload ------------------------------------ */
typedef struct
{
    uint32_t track;
    uint32_t count;                 /* frames in the block (0..256)   */
    uint64_t first;                 /* file offset of the first frame */
    uint32_t size[MKV_MAX_LACE];    /* frame sizes, zero-size dropped */
} MkvBlock;

/* Forward byte reader over the block header: the lace sizes are read
 * through a small buffer instead of one seek + read per byte. */
typedef struct
{
    FM_Video *v;
    uint64_t  pos;        /* file offset of the next refill */
    uint64_t  end;
    uint32_t  have;
    uint32_t  used;
    uint8_t   buf[32];
} MkvRd;

static FM_Bool mkv_rd_byte(MkvRd *r, uint8_t *b)
{
    if (r->used >= r->have)
    {
        uint64_t left = r->end - r->pos;
        uint32_t n = (left > sizeof(r->buf)) ? (uint32_t)sizeof(r->buf)
                                             : (uint32_t)left;

        if (n == 0u || video_read_at(&r->v->file, r->pos, r->buf, n) != FM_OK)
        {
            return FM_FALSE;
        }
        r->pos += n;
        r->have = n;
        r->used = 0u;
    }
    *b = r->buf[r->used++];
    return FM_TRUE;
}

static uint64_t mkv_rd_tell(const MkvRd *r)
{
    return r->pos - (uint64_t)(r->have - r->used);
}

/* EBML-lacing size: an unsigned vint, or (signed) a vint biased by half
 * its range.  FM_FALSE on a truncated / over-long value. */
static FM_Bool mkv_rd_vint(MkvRd *r, FM_Bool is_signed, int64_t *out)
{
    uint8_t b;
    uint32_t len;
    uint32_t i;
    uint64_t val;

    if (!mkv_rd_byte(r, &b))
    {
        return FM_FALSE;
    }
    len = ebml_vint_len(b, 8u);
    if (len == 0u)
    {
        return FM_FALSE;
    }
    val = (uint64_t)(b & (uint8_t)(0xFFu >> len));
    for (i = 1; i < len; i++)
    {
        if (!mkv_rd_byte(r, &b))
        {
            return FM_FALSE;
        }
        val = (val << 8) | b;
    }
    *out = is_signed
               ? (int64_t)val - (int64_t)((1ULL << (7u * len - 1u)) - 1ULL)
               : (int64_t)val;
    return FM_TRUE;
}

/* Parse the Block header + lacing of the `bsz` bytes at `bdata`.
 *
 * Every size is validated against the bytes the block really has before
 * it is used: a block shorter than its own header (which used to wrap
 * `payload_sz` to ~4 GB and switch the bounds check off), lace sizes
 * that add up to more than the block, and a fixed lace that does not
 * divide evenly are all rejected.  FM_OK with count == 0 is a block
 * with no usable frame. */
static FM_Result mkv_block_parse(FM_Video *v, uint64_t bdata, uint64_t bsz,
                                 MkvBlock *blk)
{
    MkvRd rd;
    uint8_t b0;
    uint8_t b;
    uint32_t hlen;
    uint32_t lace;
    uint32_t n;
    uint32_t k;
    uint32_t kept = 0u;
    uint64_t rem;
    uint64_t total = 0u;

    memset(&rd, 0, sizeof(rd));
    rd.v = v;
    rd.pos = bdata;
    rd.end = bdata + bsz;
    blk->count = 0u;

    if (bsz > 0xFFFFFFFFu || !mkv_rd_byte(&rd, &b0))
    {
        return FM_ERR_CORRUPTED;
    }
    if ((b0 & 0x80u) != 0u)
    {
        blk->track = b0 & 0x7Fu;
        hlen = 4u;
    }
    else if ((b0 & 0x40u) != 0u)
    {
        if (!mkv_rd_byte(&rd, &b))
        {
            return FM_ERR_CORRUPTED;
        }
        blk->track = ((uint32_t)(b0 & 0x3Fu) << 8) | b;
        hlen = 5u;
    }
    else
    {
        return FM_ERR_UNSUPPORTED;    /* track numbers above 16383 */
    }
    if (bsz <= hlen)
    {
        return FM_ERR_CORRUPTED;      /* header only: no payload */
    }
    /* 2-byte timecode, then the flags byte */
    if (!mkv_rd_byte(&rd, &b) || !mkv_rd_byte(&rd, &b) ||
        !mkv_rd_byte(&rd, &b))
    {
        return FM_ERR_CORRUPTED;
    }
    lace = ((uint32_t)b >> 1) & 0x03u;
    if (lace == 0u)
    {
        blk->first = bdata + hlen;
        blk->size[0] = (uint32_t)(bsz - hlen);
        blk->count = 1u;
        return FM_OK;
    }
    if (!mkv_rd_byte(&rd, &b))
    {
        return FM_ERR_CORRUPTED;
    }
    n = (uint32_t)b + 1u;             /* 1..256 frames */
    rem = rd.end - mkv_rd_tell(&rd);  /* lace sizes + frame data */

    if (lace == 2u)                   /* fixed-size lacing */
    {
        if (rem == 0u || (rem % n) != 0u)
        {
            return FM_ERR_CORRUPTED;
        }
        for (k = 0; k < n; k++)
        {
            blk->size[k] = (uint32_t)(rem / n);
        }
    }
    else
    {
        int64_t prev = 0;

        for (k = 0; k + 1u < n; k++)
        {
            uint64_t sz = 0u;

            if (lace == 1u)           /* Xiph: 255,255,...,<255 */
            {
                do
                {
                    if (!mkv_rd_byte(&rd, &b))
                    {
                        return FM_ERR_CORRUPTED;
                    }
                    sz += b;
                } while (b == 255u && sz <= rem);
            }
            else                      /* EBML: size, then signed deltas */
            {
                int64_t val;

                if (!mkv_rd_vint(&rd, (k != 0u) ? FM_TRUE : FM_FALSE, &val))
                {
                    return FM_ERR_CORRUPTED;
                }
                if (k != 0u)
                {
                    val += prev;
                }
                if (val < 0)
                {
                    return FM_ERR_CORRUPTED;
                }
                prev = val;
                sz = (uint64_t)val;
            }
            total += sz;
            if (sz > rem || total > rem)
            {
                return FM_ERR_CORRUPTED;
            }
            blk->size[k] = (uint32_t)sz;
        }
        /* The last frame takes what is left after the lace header. */
        rem = rd.end - mkv_rd_tell(&rd);
        if (total > rem)
        {
            return FM_ERR_CORRUPTED;
        }
        blk->size[n - 1u] = (uint32_t)(rem - total);
    }
    blk->first = mkv_rd_tell(&rd);
    /* Zero-size frames carry nothing and take no room: drop them so the
     * indexer and the streaming cursor number frames identically. */
    for (k = 0; k < n; k++)
    {
        if (blk->size[k] != 0u)
        {
            blk->size[kept++] = blk->size[k];
        }
    }
    blk->count = kept;
    return FM_OK;
}

/* DiscardPadding of a BlockGroup, in sample frames at `rate`; 0 = none
 * (or negative: padding at the front, which CodecDelay covers). */
static uint32_t mkv_group_discard(FM_Video *v, const EbmlElem *group,
                                  uint32_t rate)
{
    uint64_t gpos = group->data;
    uint64_t gend = group->data + group->size;
    EbmlElem g;

    while (gpos < gend && ebml_read(v, gpos, gend, &g) == FM_OK)
    {
        if (g.id == MKV_ID_DISCARDPAD && g.size >= 1u && g.size <= 8u &&
            !g.truncated)
        {
            uint8_t b[8];
            uint64_t ns = 0u;
            uint32_t i;

            if (video_read_at(&v->file, g.data, b, (uint32_t)g.size) != FM_OK ||
                (b[0] & 0x80u) != 0u)
            {
                return 0u;               /* unreadable, or negative */
            }
            for (i = 0u; i < (uint32_t)g.size; i++)
            {
                ns = (ns << 8) | b[i];
            }
            if (ns > 1000000000u)
            {
                return 0u;               /* more than a second: not padding */
            }
            return (uint32_t)((ns * rate + 500000000u) / 1000000000u);
        }
        gpos = g.data + g.size;
    }
    return 0u;
}

/* If `e` carries frame data -- a SimpleBlock, or a BlockGroup whose
 * Block child is located here -- return the block payload. */
static FM_Bool mkv_find_block(FM_Video *v, const EbmlElem *e,
                              uint64_t *bdata, uint64_t *bsz)
{
    if (e->id == MKV_ID_SIMPLEBLOCK)
    {
        if (e->truncated)
        {
            return FM_FALSE;
        }
        *bdata = e->data;
        *bsz = e->size;
        return FM_TRUE;
    }
    if (e->id == MKV_ID_BLOCKGROUP)
    {
        uint64_t gpos = e->data;
        uint64_t gend = e->data + e->size;
        EbmlElem g;

        while (gpos < gend && ebml_read(v, gpos, gend, &g) == FM_OK)
        {
            if (g.id == MKV_ID_BLOCK)
            {
                if (g.truncated)
                {
                    return FM_FALSE;
                }
                *bdata = g.data;
                *bsz = g.size;
                return FM_TRUE;
            }
            gpos = g.data + g.size;
        }
    }
    return FM_FALSE;
}

static FM_Result mkv_parse(FM_Video *v, FM_VideoPriv *priv)
{
    uint64_t file_size = 0;
    uint64_t seg_data = 0;
    uint64_t seg_end = 0;
    uint64_t pos;
    uint64_t def_duration = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    FM_Bool have_tracks = FM_FALSE;
    EbmlElem e;
    MkvBlock *blk;

    if (File_Size(&v->file, &file_size) != FM_OK)
    {
        return FM_ERR_IO;
    }
    priv->mkv_vid_track = MKV_NO_TRACK;
    priv->mkv_aud_track = MKV_NO_TRACK;

    /* EBML header, then the Segment. */
    pos = 0;
    while (pos < file_size && ebml_read(v, pos, file_size, &e) == FM_OK)
    {
        if (e.id == MKV_ID_SEGMENT)
        {
            seg_data = e.data;
            seg_end = e.data + e.size;   /* clamped to the file */
            break;
        }
        pos = e.data + e.size;
    }
    if (seg_end == 0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }

    /* Pass 1: Tracks.  Stops as soon as the element has been parsed. */
    pos = seg_data;
    while (!have_tracks && pos < seg_end &&
           ebml_read(v, pos, seg_end, &e) == FM_OK)
    {
        if (e.id == MKV_ID_TRACKS)
        {
            uint64_t tpos = e.data;
            uint64_t tend = e.data + e.size;
            EbmlElem te;

            have_tracks = FM_TRUE;
            while (tpos < tend && ebml_read(v, tpos, tend, &te) == FM_OK)
            {
                if (te.id == MKV_ID_TRACKENTRY)
                {
                    MkvTrack t;

                    memset(&t, 0, sizeof(t));
                    mkv_parse_track(v, te.data, te.data + te.size, &t, 0u);
                    if (t.number == 0u || t.number >= MKV_NO_TRACK)
                    {
                        /* no usable TrackNumber */
                    }
                    else if (t.type == 1u &&
                             priv->mkv_vid_track == MKV_NO_TRACK)
                    {
                        priv->mkv_vid_track = (uint32_t)t.number;
                        width = (t.width > 0xFFFFu) ? 0x10000u
                                                    : (uint32_t)t.width;
                        height = (t.height > 0xFFFFu) ? 0x10000u
                                                      : (uint32_t)t.height;
                        def_duration = t.default_duration;
                        mkv_setup_video(v, priv, &t);
                    }
                    else if (t.type == 2u &&
                             priv->mkv_aud_track == MKV_NO_TRACK)
                    {
                        mkv_setup_audio(v, priv, &t);
                        if (priv->audio_present)
                        {
                            priv->mkv_aud_track = (uint32_t)t.number;
                        }
                    }
                }
                tpos = te.data + te.size;
            }
        }
        pos = e.data + e.size;
    }
    if (priv->mkv_vid_track == MKV_NO_TRACK)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (width > 0xFFFFu || height > 0xFFFFu)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (width == 0u)  { width = 320u; }
    if (height == 0u) { height = 240u; }
    v->info.width = (uint16_t)width;
    v->info.height = (uint16_t)height;
    /* DefaultDuration is ns per frame; round like the AVI path does. */
    v->info.fps = (def_duration != 0u && def_duration <= 1000000000ULL)
        ? (uint32_t)((1000000000ULL + def_duration / 2u) / def_duration)
        : 25u;
    if (v->info.fps == 0u)
    {
        v->info.fps = 25u;
    }
    (void)strcpy(v->info.container, "MKV");

    /* Pass 2: index every block.  One flat walk of the segment: a
     * Cluster is entered (its own size is not needed, so unknown-size
     * clusters from live muxers work), everything else is skipped by
     * size.  Entries past the fixed tables are counted and streamed
     * later from where the table ended (video_tail_locate). */
    blk = (MkvBlock *)FM_Alloc(sizeof(*blk));
    if (blk == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    priv->mkv_blk = blk;   /* kept for video_tail_locate; freed on close */
    priv->count = 0;
    priv->audio_count = 0;
    priv->tail_kind = VIDEO_TAIL_MKV;
    priv->tail_end = seg_end;
    pos = seg_data;
    while (pos < seg_end && ebml_read(v, pos, seg_end, &e) == FM_OK)
    {
        uint64_t bdata = 0;
        uint64_t bsz = 0;

        if (e.id == MKV_ID_CLUSTER)
        {
            pos = e.data;
            continue;
        }
        if (e.id == MKV_ID_SIMPLEBLOCK || e.id == MKV_ID_BLOCKGROUP)
        {
            if (!mkv_find_block(v, &e, &bdata, &bsz) ||
                mkv_block_parse(v, bdata, bsz, blk) != FM_OK)
            {
                priv->mkv_bad_blocks++;
            }
            else
            {
                FM_Bool is_video = (blk->track == priv->mkv_vid_track)
                                       ? FM_TRUE : FM_FALSE;
                FM_Bool is_audio = (priv->audio_present &&
                                    blk->track == priv->mkv_aud_track)
                                       ? FM_TRUE : FM_FALSE;
                uint64_t off = blk->first;
                uint32_t k;

                for (k = 0; k < blk->count; k++)
                {
                    if (is_video)
                    {
                        video_index_add(priv->frames, FM_VIDEO_MAX_FRAMES,
                                        &priv->count, &priv->vtail, pos, k,
                                        off, blk->size[k]);
                    }
                    else if (is_audio)
                    {
                        video_index_add(priv->audio_frames,
                                        FM_VIDEO_MAX_AUDIO_FRAMES,
                                        &priv->audio_count, &priv->atail,
                                        pos, k, off, blk->size[k]);
                    }
                    off += blk->size[k];
                }
                if (is_audio && e.id == MKV_ID_BLOCKGROUP &&
                    priv->atail.total != 0u)
                {
                    /* DiscardPadding: the encoder rounded the end of
                     * the track up to a whole packet, and says here how
                     * much of this (the last) packet is not sound. */
                    uint32_t pad = mkv_group_discard(v, &e, priv->audio_rate);

                    if (pad != 0u)
                    {
                        priv->audio_pad_packet = priv->atail.total - 1u;
                        priv->audio_pad_frames = pad;
                    }
                }
            }
        }
        pos = e.data + e.size;
    }
    if (priv->mkv_bad_blocks != 0u)
    {
        FM_ERR_LOG("mkv: %lu damaged block(s) skipped",
                   (unsigned long)priv->mkv_bad_blocks);
    }
    if (priv->count == 0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    v->info.frame_count = priv->vtail.total;
    v->info.data_offset = priv->frames[0].offset;
    v->info.frame_size = 0;
    v->variable = 1;
    return Video_Seek(v, 0);
}

/* ------------------------------------------------------------------ */
/* FLV - Flash Video demux                                             */
/* ------------------------------------------------------------------ */
/* Header (9 bytes, "FLV"), then tags:
 *   PreviousTagSize(4)  TagType(1) DataSize(3) Timestamp(3+1) StreamID(3)
 *   payload(DataSize)
 * Video payload: FrameType<<4 | CodecID, then for AVC (7) / HEVC (12,
 * the pre-standard extension) AVCPacketType(1) + CompositionTime(3);
 * or the Enhanced-FLV form 0x80 | FrameType<<4 | PacketType + FourCC.
 * Audio payload: SoundFormat<<4 | rate<<2 | size<<1 | stereo, then for
 * AAC (10) AACPacketType(1). */
#define FLV_SKIP   0u   /* nothing to play in this tag                   */
#define FLV_VFRAME 1u   /* one video frame                               */
#define FLV_APKT   2u   /* one audio packet                              */
#define FLV_VCONF  3u   /* avcC / hvcC record                            */
#define FLV_ACONF  4u   /* AAC AudioSpecificConfig                       */
#define FLV_SCRIPT 5u   /* AMF0 script data (onMetaData)                 */

typedef struct
{
    uint8_t  kind;      /* FLV_*                                         */
    uint8_t  vcodec;    /* FM_VideoCodec of a video tag                  */
    uint8_t  snd;       /* first payload byte of an audio tag            */
    uint32_t ts;        /* milliseconds                                  */
    uint64_t off;       /* frame / packet / record, tag header stripped  */
    uint32_t size;
    uint64_t next;      /* where the following tag starts                */
} FlvTag;

/* SoundFormats this library can do something with.  1 (Flash ADPCM, not
 * the IMA/MS ADPCM of AVI), 4-6 (Nellymoser) and 11 (Speex) are not. */
static FM_Bool flv_audio_usable(uint8_t fmt)
{
    return (fmt == 0u || fmt == 2u || fmt == 3u || fmt == 7u || fmt == 8u ||
            fmt == 10u || fmt == 14u) ? FM_TRUE : FM_FALSE;
}

/* Read and classify the tag at `pos`.  FM_ERR_EOF when there is no
 * complete tag there (end of file, or a recording that was cut off). */
static FM_Result flv_tag_at(FM_Video *v, uint64_t pos, uint64_t end,
                            FlvTag *t)
{
    uint8_t h[16];
    const uint8_t *p = h + 11;
    uint32_t dsz;
    uint32_t have;
    uint32_t skip = 0;
    uint64_t data = pos + 11u;
    uint8_t type;

    memset(t, 0, sizeof(*t));
    if (data > end || video_read_at(&v->file, pos, h, 11) != FM_OK)
    {
        return FM_ERR_EOF;
    }
    dsz = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | (uint32_t)h[3];
    if (dsz > end - data)
    {
        return FM_ERR_EOF;
    }
    /* the PreviousTagSize behind the last tag may be missing */
    t->next = data + dsz + 4u;
    t->ts = ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 8) | (uint32_t)h[6] |
            ((uint32_t)h[7] << 24);
    have = (dsz < 5u) ? dsz : 5u;
    if (have != 0u && video_read_at(&v->file, data, h + 11, have) != FM_OK)
    {
        return FM_ERR_EOF;
    }
    type = h[0] & 0x1Fu;
    if ((h[0] & 0x20u) != 0u || have == 0u)
    {
        return FM_OK;                   /* encrypted, or empty */
    }
    if (type == 9u)
    {
        if ((p[0] & 0x80u) != 0u)
        {
            /* Enhanced FLV: PacketType in the low nibble, FourCC next */
            uint8_t pt = p[0] & 0x0Fu;

            if (have < 5u)
            {
                return FM_OK;
            }
            if (memcmp(p + 1, "hvc1", 4) == 0) t->vcodec = FM_VIDEO_CODEC_HEVC;
            else if (memcmp(p + 1, "avc1", 4) == 0) t->vcodec = FM_VIDEO_CODEC_H264;
            else if (memcmp(p + 1, "av01", 4) == 0) t->vcodec = FM_VIDEO_CODEC_AV1;
            else if (memcmp(p + 1, "vp09", 4) == 0) t->vcodec = FM_VIDEO_CODEC_VP9;
            else if (memcmp(p + 1, "vp08", 4) == 0) t->vcodec = FM_VIDEO_CODEC_VP8;
            else t->vcodec = FM_VIDEO_CODEC_UNKNOWN;
            if (pt == 0u)
            {
                t->kind = FLV_VCONF;
                skip = 5u;
            }
            else if (pt == 1u)
            {
                /* CodedFrames: AVC / HEVC carry a composition time */
                t->kind = FLV_VFRAME;
                skip = (t->vcodec == FM_VIDEO_CODEC_HEVC ||
                        t->vcodec == FM_VIDEO_CODEC_H264) ? 8u : 5u;
            }
            else if (pt == 3u)
            {
                t->kind = FLV_VFRAME;   /* CodedFramesX: no time field */
                skip = 5u;
            }
            else
            {
                return FM_OK;           /* sequence end, metadata, ... */
            }
        }
        else
        {
            uint8_t cid = p[0] & 0x0Fu;

            if ((p[0] >> 4) == 5u)
            {
                return FM_OK;           /* video info / command frame */
            }
            if (cid == 7u || cid == 12u)
            {
                if (have < 5u || p[1] > 1u)
                {
                    return FM_OK;       /* 2 = end of sequence */
                }
                t->vcodec = (cid == 7u) ? FM_VIDEO_CODEC_H264
                                        : FM_VIDEO_CODEC_HEVC;
                t->kind = (p[1] == 0u) ? FLV_VCONF : FLV_VFRAME;
                skip = 5u;
            }
            else
            {
                /* 1 JPEG, 2 Sorenson H.263, 3 screen, 4/5 VP6 (one
                 * extra adjustment byte), 6 screen v2: frames are
                 * handed out for a user decoder */
                t->vcodec = (cid == 1u) ? FM_VIDEO_CODEC_MJPEG
                                        : FM_VIDEO_CODEC_UNKNOWN;
                t->kind = FLV_VFRAME;
                skip = (cid == 4u || cid == 5u) ? 2u : 1u;
            }
        }
    }
    else if (type == 8u)
    {
        uint8_t fmt = p[0] >> 4;

        t->snd = p[0];
        if (!flv_audio_usable(fmt))
        {
            return FM_OK;
        }
        if (fmt == 10u)
        {
            if (have < 2u || p[1] > 1u)
            {
                return FM_OK;
            }
            t->kind = (p[1] == 0u) ? FLV_ACONF : FLV_APKT;
            skip = 2u;
        }
        else
        {
            t->kind = FLV_APKT;
            skip = 1u;
        }
    }
    else if (type == 18u)
    {
        t->kind = FLV_SCRIPT;
    }
    else
    {
        return FM_OK;
    }
    if (dsz <= skip)
    {
        t->kind = FLV_SKIP;             /* header only: nothing inside */
        return FM_OK;
    }
    t->off = data + skip;
    t->size = dsz - skip;
    return FM_OK;
}

/* AMF0 Number (IEEE-754 double, big-endian) times `scale` (<= 1000),
 * rounded to the nearest integer -- without touching floating point,
 * which a soft-float target would pay for dearly. */
static uint32_t flv_amf_number(const uint8_t *b, uint32_t scale)
{
    int32_t exp = (int32_t)((((uint32_t)b[0] & 0x7Fu) << 4) |
                            ((uint32_t)b[1] >> 4)) - 1023;
    uint64_t mant = ((uint64_t)(b[1] & 0x0Fu) << 48) |
                    ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
                    ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                    ((uint64_t)b[6] << 8) | (uint64_t)b[7];
    uint32_t shift;

    if ((b[0] & 0x80u) != 0u || exp < -11 || exp == 1024)
    {
        return 0u;                      /* negative, below 1/2048, NaN */
    }
    if (exp > 31)
    {
        return UINT32_MAX;
    }
    mant = (mant | ((uint64_t)1 << 52)) * scale;    /* < 2^63 */
    shift = (uint32_t)(52 - exp);                   /* 21 .. 63 */
    mant = (mant + ((uint64_t)1 << (shift - 1u))) >> shift;
    return (mant > UINT32_MAX) ? UINT32_MAX : (uint32_t)mant;
}

/* onMetaData: picture size and frame rate.  Walks the top-level
 * properties of the ECMA array / object; stops quietly at anything it
 * does not need to understand. */
static void flv_metadata(FM_Video *v, uint64_t off, uint32_t size,
                         uint32_t *width, uint32_t *height,
                         uint32_t *fps_milli)
{
    uint64_t pos = off;
    uint64_t end = off + size;
    uint8_t b[12];
    uint32_t guard;

    if (size < 4u || video_read_at(&v->file, pos, b, 3) != FM_OK ||
        b[0] != 2u)
    {
        return;
    }
    pos += 3u + video_be16(b + 1);                  /* "onMetaData" */
    if (pos >= end || video_read_at(&v->file, pos, b, 1) != FM_OK)
    {
        return;
    }
    if (b[0] == 8u)
    {
        pos += 5u;                                  /* ECMA array + count */
    }
    else if (b[0] == 3u)
    {
        pos += 1u;                                  /* object */
    }
    else
    {
        return;
    }
    for (guard = 0; guard < 64u; guard++)
    {
        char key[12];
        uint32_t klen;
        uint8_t type;

        if (pos + 3u > end || video_read_at(&v->file, pos, b, 2) != FM_OK)
        {
            return;
        }
        klen = video_be16(b);
        pos += 2u;
        if (klen >= end - pos)
        {
            return;
        }
        memset(key, 0, sizeof(key));
        if (klen != 0u && klen < sizeof(key) &&
            video_read_at(&v->file, pos, key, klen) != FM_OK)
        {
            return;
        }
        pos += klen;
        if (video_read_at(&v->file, pos, &type, 1) != FM_OK)
        {
            return;
        }
        pos += 1u;
        switch (type)
        {
        case 0u:                                    /* number */
            if (pos + 8u > end || video_read_at(&v->file, pos, b, 8) != FM_OK)
            {
                return;
            }
            if (klen == 5u && memcmp(key, "width", 5) == 0)
            {
                *width = flv_amf_number(b, 1u);
            }
            else if (klen == 6u && memcmp(key, "height", 6) == 0)
            {
                *height = flv_amf_number(b, 1u);
            }
            else if (klen == 9u && memcmp(key, "framerate", 9) == 0)
            {
                *fps_milli = flv_amf_number(b, 1000u);
            }
            pos += 8u;
            break;
        case 1u:                                    /* boolean */
            pos += 1u;
            break;
        case 2u:                                    /* string */
            if (pos + 2u > end || video_read_at(&v->file, pos, b, 2) != FM_OK)
            {
                return;
            }
            pos += 2u + video_be16(b);
            break;
        case 5u:                                    /* null */
        case 6u:                                    /* undefined */
            break;
        case 11u:                                   /* date */
            pos += 10u;
            break;
        case 12u:                                   /* long string */
            if (pos + 4u > end || video_read_at(&v->file, pos, b, 4) != FM_OK)
            {
                return;
            }
            pos += 4u + (uint64_t)video_be32(b);
            break;
        default:                    /* nested value, end marker: stop */
            return;
        }
    }
}

static FM_Result flv_parse(FM_Video *v, FM_VideoPriv *priv)
{
    static const uint32_t k_rate[4] = { 5512u, 11025u, 22050u, 44100u };
    uint64_t file_size = 0;
    uint64_t pos;
    uint8_t hdr[9];
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_milli = 0;
    uint32_t fps = 0;
    uint32_t ts_first = 0;
    uint32_t ts_last = 0;
    FM_VideoCodec vcodec = FM_VIDEO_CODEC_UNKNOWN;
    FM_Bool have_vconf = FM_FALSE;
    FM_Bool have_aconf = FM_FALSE;
    FM_Bool have_meta = FM_FALSE;
    uint8_t snd = 0;

    if (File_Size(&v->file, &file_size) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (video_read_at(&v->file, 0, hdr, 9) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    if (memcmp(hdr, "FLV", 3) != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    pos = video_be32(hdr + 5);          /* header size, 9 in version 1 */
    if (pos < 9u || pos >= file_size)
    {
        return FM_ERR_CORRUPTED;
    }
    pos += 4u;                          /* PreviousTagSize0 */
    priv->count = 0;
    priv->audio_count = 0;
    priv->tail_kind = VIDEO_TAIL_FLV;
    priv->tail_end = file_size;
    /* The whole file is walked once: every frame and packet is counted,
     * the first FM_VIDEO_MAX_FRAMES / _AUDIO_FRAMES go into the tables,
     * the rest are streamed (video_tail_locate).  The scan used to stop
     * at the first full table -- picture and sound both ended there. */
    for (;;)
    {
        FlvTag t;

        if (flv_tag_at(v, pos, file_size, &t) != FM_OK)
        {
            break;
        }
        switch (t.kind)
        {
        case FLV_VFRAME:
            if (priv->vtail.total == 0u)
            {
                vcodec = (FM_VideoCodec)t.vcodec;
                ts_first = t.ts;
            }
            ts_last = t.ts;
            video_index_add(priv->frames, FM_VIDEO_MAX_FRAMES, &priv->count,
                            &priv->vtail, pos, 0u, t.off, t.size);
            break;
        case FLV_VCONF:
            /* avcC / hvcC: without the parameter sets in front of the
             * first picture no decoder can start (this record used to
             * be skipped, and the length-prefixed NAL units were handed
             * out as they are). */
            if (!have_vconf)
            {
                FM_Result rc = FM_ERR_UNSUPPORTED;

                if (t.vcodec == FM_VIDEO_CODEC_H264)
                {
                    rc = video_parse_avcc(v, t.off, t.size, priv);
                }
                else if (t.vcodec == FM_VIDEO_CODEC_HEVC)
                {
                    rc = video_parse_hvcc(v, t.off, t.size, priv);
                }
                if (rc == FM_OK)
                {
                    have_vconf = FM_TRUE;
                }
                else if (t.vcodec == FM_VIDEO_CODEC_H264 ||
                         t.vcodec == FM_VIDEO_CODEC_HEVC)
                {
                    FM_ERR_LOG("flv: decoder configuration unusable (%s) "
                               "-- frames will not decode",
                               FM_ResultString(rc));
                }
            }
            break;
        case FLV_APKT:
            if (priv->atail.total == 0u)
            {
                snd = t.snd;
            }
            video_index_add(priv->audio_frames, FM_VIDEO_MAX_AUDIO_FRAMES,
                            &priv->audio_count, &priv->atail, pos, 0u,
                            t.off, t.size);
            break;
        case FLV_ACONF:
            if (!have_aconf &&
                video_audio_private_load(v, priv, t.off, t.size) == FM_OK)
            {
                have_aconf = FM_TRUE;
            }
            break;
        case FLV_SCRIPT:
            if (!have_meta)
            {
                flv_metadata(v, t.off, t.size, &width, &height, &fps_milli);
                have_meta = (width != 0u || fps_milli != 0u) ? FM_TRUE
                                                             : FM_FALSE;
            }
            break;
        default:
            break;
        }
        pos = t.next;
    }
    if (priv->count == 0u)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (vcodec == FM_VIDEO_CODEC_H264 || vcodec == FM_VIDEO_CODEC_HEVC)
    {
        priv->avc1 = FM_TRUE;           /* length-prefixed NAL units */
        if (priv->nal_len_size == 0u)
        {
            priv->nal_len_size = 4u;
        }
    }
    /* Picture size: FLV only has it in onMetaData.  320x240 stays the
     * placeholder when the file carries none (the decoder then knows
     * better: H264_GetOutputSize). */
    v->info.width = (width != 0u && width <= 0xFFFFu) ? (uint16_t)width : 320u;
    v->info.height = (height != 0u && height <= 0xFFFFu) ? (uint16_t)height
                                                         : 240u;
    /* Frame rate: metadata, else the average over the tag timestamps
     * (nearest integer), else 25. */
    if (fps_milli != 0u && fps_milli != UINT32_MAX)
    {
        fps = (fps_milli + 500u) / 1000u;
    }
    if (fps == 0u && priv->vtail.total > 1u && ts_last > ts_first)
    {
        uint64_t span = (uint64_t)ts_last - ts_first;

        fps = (uint32_t)(((uint64_t)(priv->vtail.total - 1u) * 1000u +
                          span / 2u) / span);
    }
    v->info.fps = (fps != 0u) ? fps : 25u;
    v->info.codec = vcodec;
    (void)strcpy(v->info.container, "FLV");
    v->info.frame_count = priv->vtail.total;
    v->info.data_offset = priv->frames[0].offset;
    v->info.frame_size = 0;
    v->variable = 1;
    if (priv->atail.total != 0u)
    {
        uint8_t fmt = snd >> 4;

        priv->audio_present = FM_TRUE;
        priv->audio_rate = k_rate[(snd >> 2) & 0x03u];
        priv->audio_channels = ((snd & 0x01u) != 0u) ? 2u : 1u;
        priv->audio_bits = 0u;
        if (fmt == 0u || fmt == 3u)
        {
            priv->audio_codec = FM_VIDEO_AUDIO_PCM;
            priv->audio_bits = ((snd & 0x02u) != 0u) ? 16u : 8u;
        }
        else if (fmt == 2u || fmt == 14u)
        {
            priv->audio_codec = FM_VIDEO_AUDIO_MP3;
            if (fmt == 14u)
            {
                priv->audio_rate = 8000u;
            }
        }
        else if (fmt == 7u || fmt == 8u)
        {
            /* G.711: 7 is A-law, 8 is mu-law, always 8 kHz */
            priv->audio_codec = (fmt == 7u) ? FM_VIDEO_AUDIO_ALAW
                                            : FM_VIDEO_AUDIO_ULAW;
            priv->audio_rate = 8000u;
            priv->audio_bits = 16u;
        }
        else
        {
            priv->audio_codec = FM_VIDEO_AUDIO_AAC;
            /* the tag header always says 44.1 kHz stereo for AAC; the
             * truth is in the AudioSpecificConfig */
            if (have_aconf && priv->audio_private_size >= 2u)
            {
                uint32_t aot = 0u;
                uint32_t arate = priv->audio_rate;
                uint32_t chan = priv->audio_channels;

                if (video_asc_parse(video_audio_private(priv),
                                    priv->audio_private_size, &aot, &arate,
                                    &chan))
                {
                    priv->audio_aot = (uint8_t)((aot > 255u) ? 0u : aot);
                    priv->audio_rate = arate;
                    priv->audio_channels = (uint16_t)chan;
                }
            }
        }
    }
    return Video_Seek(v, 0);
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
        if (v->priv != NULL)
        {
            ((FM_VideoPriv *)v->priv)->file_size = size;
            ((FM_VideoPriv *)v->priv)->audio_skip_left =
                ((FM_VideoPriv *)v->priv)->audio_skip;
        }
        FM_EVENT_LOG("opened %s (%ux%u, %lu fps, %lu frames)", path,
                     (unsigned)v->info.width, (unsigned)v->info.height,
                     (unsigned long)v->info.fps,
                     (unsigned long)v->info.frame_count);
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
/* Bytes the Annex-B form of a length-prefixed sample needs.  Each NAL
 * trades its `len_size`-byte length for a 4-byte start code, so only
 * 4-byte lengths convert size-for-size; shorter ones grow by
 * (4 - len_size) per NAL and the NALs have to be counted. */
static FM_Result video_annexb_size(FM_File *f, uint64_t offset, uint32_t size,
                                   uint32_t len_size, size_t *out_size)
{
    uint8_t hdr[4];
    uint32_t pos = 0;
    uint64_t total = 0;

    if (len_size < 1u || len_size > 4u)
    {
        return FM_ERR_CORRUPTED;
    }
    if (len_size == 4u)
    {
        *out_size = size;
        return FM_OK;
    }
    while (pos < size)
    {
        uint32_t nlen = 0;
        uint32_t i;

        if (size - pos < len_size ||
            video_read_at(f, offset + pos, hdr, len_size) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        for (i = 0; i < len_size; i++)
        {
            nlen = (nlen << 8) | hdr[i];
        }
        if (nlen == 0u || nlen > size - pos - len_size)
        {
            return FM_ERR_CORRUPTED;
        }
        total += 4u + (uint64_t)nlen;
        pos += len_size + nlen;
    }
    if (total > (uint64_t)SIZE_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    *out_size = (size_t)total;
    return FM_OK;
}

/* Convert a length-prefixed (AVCC/HVCC) sample to Annex-B.
 *
 * `len_size` is the width of the NAL length field (1..4, from the
 * configuration record).  The NALs must tile the sample exactly: a
 * length that runs past the sample, or left-over bytes too short to be
 * a length (silently accepted before), mean the sample is damaged and
 * feeding the partial result to a decoder only breaks its reference
 * chain.  Every write is checked against `cap` because the output is
 * larger than the input whenever len_size < 4. */
static FM_Result video_read_annexb(FM_File *f, uint64_t offset, uint32_t size,
                                   uint32_t len_size, uint8_t *out,
                                   size_t cap, size_t *out_len)
{
    static const uint8_t start[] = { 0, 0, 0, 1 };
    uint8_t hdr[4];
    uint32_t pos = 0;
    size_t o = 0;

    if (len_size < 1u || len_size > 4u)
    {
        return FM_ERR_CORRUPTED;
    }
    while (pos < size)
    {
        uint32_t nlen = 0;
        uint32_t i;

        if (size - pos < len_size ||
            video_read_at(f, offset + pos, hdr, len_size) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        for (i = 0; i < len_size; i++)
        {
            nlen = (nlen << 8) | hdr[i];
        }
        if (nlen == 0u || nlen > size - pos - len_size)
        {
            return FM_ERR_CORRUPTED;
        }
        if (cap - o < 4u || cap - o - 4u < nlen)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(out + o, start, 4);
        if (video_read_at(f, offset + pos + len_size, out + o + 4u,
                          nlen) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        o += 4u + nlen;
        pos += len_size + nlen;
    }
    *out_len = o;
    return FM_OK;
}

static uint32_t video_nal_len_size(const FM_VideoPriv *priv)
{
    return (priv->nal_len_size >= 1u && priv->nal_len_size <= 4u)
               ? priv->nal_len_size : 4u;
}

/* ------------------------------------------------------------------ */
/* Entries past the fixed index tables                                 */
/* ------------------------------------------------------------------ */
/* Locate entry `idx` (a video frame, or an audio packet when `audio`)
 * that did not fit its index table, by walking the container from where
 * the table ended (AVI: the movi list, MKV: the clusters, MP4: the
 * sample tables, FLV: the tags).  A cursor makes sequential playback O(1) per entry;
 * a backward jump restarts from the end of the index.  The cursor is
 * left ON the element that holds the entry, so asking for the same
 * entry twice (size query, then read) costs one header re-read, not a
 * rescan.
 *
 * FM_ERR_EOF when the container has no such entry. */
static FM_Result video_tail_locate(FM_Video *v, FM_VideoPriv *priv,
                                   FM_Bool audio, uint32_t idx,
                                   uint64_t *off, uint32_t *size)
{
    FM_VideoTail *t = audio ? &priv->atail : &priv->vtail;
    uint32_t indexed = audio ? priv->audio_count : priv->count;
    uint64_t end = priv->tail_end;

    if (priv->tail_kind == VIDEO_TAIL_MP4)
    {
        Mp4Track *m = audio ? &priv->mp4a : &priv->mp4v;
        FM_Result r;

        if (idx >= t->total || idx < indexed)
        {
            return FM_ERR_EOF;
        }
        /* cur_pos != 0 marks a live cursor here too (Video_SeekAudio
         * clears it on a backward jump) */
        if (t->cur_pos == 0u || idx < m->cur.sample)
        {
            m->cur = m->start;
            t->cur_pos = 1u;
        }
        r = mp4_cur_seek(v, m, &m->cur, idx, off, size);
        if (r != FM_OK)
        {
            t->cur_pos = 0u;
        }
        return r;
    }
    if (priv->tail_kind == VIDEO_TAIL_NONE || t->scan_pos == 0u ||
        idx >= t->total || idx < indexed)
    {
        return FM_ERR_EOF;
    }
    if (t->cur_pos == 0u || idx < t->cur_idx)
    {
        t->cur_pos = t->scan_pos;
        t->cur_idx = indexed - t->scan_skip;
    }
    if (priv->tail_kind == VIDEO_TAIL_FLV)
    {
        for (;;)
        {
            FlvTag ft;

            if (flv_tag_at(v, t->cur_pos, end, &ft) != FM_OK)
            {
                return FM_ERR_EOF;
            }
            if (ft.kind == (audio ? FLV_APKT : FLV_VFRAME))
            {
                if (t->cur_idx == idx)
                {
                    *off = ft.off;
                    *size = ft.size;
                    return FM_OK;
                }
                t->cur_idx++;
            }
            t->cur_pos = ft.next;
        }
    }
    if (priv->tail_kind == VIDEO_TAIL_AVI)
    {
        for (;;)
        {
            uint8_t ck[8];
            uint32_t sz;
            uint64_t data;
            FM_Bool match;

            if (t->cur_pos + 8u > end)
            {
                return FM_ERR_EOF;
            }
            if (video_read_at(&v->file, t->cur_pos, ck, 8) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            sz = video_rd32(ck + 4);
            data = t->cur_pos + 8u;
            if (sz > end - data)
            {
                return FM_ERR_EOF;
            }
            if (memcmp(ck, "LIST", 4) == 0 && sz >= 4u)
            {
                t->cur_pos = data + 4u;      /* descend into 'rec ' */
                continue;
            }
            match = (ck[0] != '#' && memcmp(ck, "ix", 2) != 0 && sz > 0u &&
                     (audio ? (memcmp(ck + 2, "wb", 2) == 0)
                            : (memcmp(ck + 2, "dc", 2) == 0 ||
                               memcmp(ck + 2, "db", 2) == 0)))
                        ? FM_TRUE : FM_FALSE;
            if (match)
            {
                if (t->cur_idx == idx)
                {
                    *off = data;
                    *size = sz;
                    return FM_OK;
                }
                t->cur_idx++;
            }
            t->cur_pos = data + sz + (sz & 1u);
        }
    }
    else
    {
        uint32_t want = audio ? priv->mkv_aud_track : priv->mkv_vid_track;
        MkvBlock *blk = (MkvBlock *)priv->mkv_blk;
        FM_Result r = FM_ERR_EOF;

        if (blk == NULL)
        {
            return FM_ERR_EOF;
        }
        for (;;)
        {
            EbmlElem e;
            uint64_t bdata = 0;
            uint64_t bsz = 0;

            if (t->cur_pos >= end ||
                ebml_read(v, t->cur_pos, end, &e) != FM_OK)
            {
                break;
            }
            if (e.id == MKV_ID_CLUSTER)
            {
                t->cur_pos = e.data;
                continue;
            }
            if ((e.id == MKV_ID_SIMPLEBLOCK || e.id == MKV_ID_BLOCKGROUP) &&
                mkv_find_block(v, &e, &bdata, &bsz) &&
                mkv_block_parse(v, bdata, bsz, blk) == FM_OK &&
                blk->track == want)
            {
                if (idx - t->cur_idx < blk->count)
                {
                    uint32_t k;
                    uint64_t o = blk->first;

                    for (k = 0; k < idx - t->cur_idx; k++)
                    {
                        o += blk->size[k];
                    }
                    *off = o;
                    *size = blk->size[idx - t->cur_idx];
                    r = FM_OK;
                    break;
                }
                t->cur_idx += blk->count;
            }
            t->cur_pos = e.data + e.size;
        }
        return r;
    }
}

/* File position and size of video frame `idx`: from the index table, or
 * streamed from the container when the clip is longer than the table. */
static FM_Result video_frame_locate(FM_Video *v, FM_VideoPriv *priv,
                                    uint32_t idx, uint64_t *off,
                                    uint32_t *size)
{
    if (idx < priv->count)
    {
        *off = priv->frames[idx].offset;
        *size = priv->frames[idx].size;
        return FM_OK;
    }
    return video_tail_locate(v, priv, FM_FALSE, idx, off, size);
}

FM_Result Video_ReadFrame(FM_Video *v, void *frame_out, size_t frame_cap,
                          size_t *size)
{
    FM_VideoPriv *priv;
    uint64_t offset;
    uint32_t fsize;
    FM_Result r;

    if (v == NULL || frame_out == NULL || size == NULL ||
        !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    priv = (FM_VideoPriv *)v->priv;
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

    if (priv == NULL)
    {
        return FM_ERR_CORRUPTED;
    }
    r = video_frame_locate(v, priv, v->current_frame, &offset, &fsize);
    if (r != FM_OK)
    {
        return (r == FM_ERR_EOF) ? FM_ERR_CORRUPTED : r;
    }

    if (priv->avc1)
    {
        /* Length-prefixed H.264/HEVC (MP4, MKV): prefix the parameter
         * sets and convert the NALs to Annex-B. */
        if (priv->sps_pps_len > frame_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(frame_out, priv->sps_pps, priv->sps_pps_len);
        r = video_read_annexb(&v->file, offset, fsize,
                              video_nal_len_size(priv),
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
            r = video_read_annexb(&v->file, offset, fsize, 4u,
                                  (uint8_t *)frame_out, frame_cap, size);
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

/* Decoder for `codec`: a registered hardware decoder wins, then the
 * bundled software one; NULL when there is neither. */
static const FM_VideoCodecOps *video_pick_codec(FM_VideoCodec codec)
{
    const FM_VideoCodecOps *hw = HWDecoder_Get(codec);

    if (hw != NULL)
    {
        return hw;
    }
#if FM_USE_H264
    if (codec == FM_VIDEO_CODEC_H264)
    {
        return H264_GetCodecOps();
    }
#endif
#if FM_USE_MJPEG
    if (codec == FM_VIDEO_CODEC_MJPEG)
    {
        return MJPEG_GetCodecOps();
    }
#endif
    return NULL;
}

FM_Result Video_DecodeFrame(FM_Video *v, void *pixels_out,
                            size_t pixels_cap)
{
    if (v == NULL || pixels_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* auto-register the matching codec so decode "just works":
     * hardware first, then the bundled software decoder. */
    if (v->codec_plugin == NULL)
    {
        const FM_VideoCodecOps *ops = video_pick_codec(v->info.codec);

        if (ops != NULL)
        {
            FM_Result rr = Video_RegisterCodec(v, ops);

            if (rr != FM_OK)
            {
                return rr;
            }
        }
    }
    if (v->codec_plugin != NULL)
    {
        uint8_t *compressed;
        size_t cap = 0;
        size_t size = 0;
        FM_Result r;
        FM_VideoPriv *priv = (FM_VideoPriv *)v->priv;
        FM_Bool own = FM_FALSE;

        /* End of stream is not an error: report EOF, matching the
         * raw-codec path (Video_ReadFrame). */
        if (v->current_frame >= v->info.frame_count)
        {
            return FM_ERR_EOF;
        }
        /* The plugin contract is RGB888 at info.width x info.height.
         * Enforce it here instead of trusting every (application
         * supplied) decoder to check its own output bound. */
        if ((uint64_t)v->info.width * v->info.height * 3u >
            (uint64_t)pixels_cap)
        {
            return FM_ERR_TOO_LARGE;
        }
        if (!v->variable)
        {
            cap = v->info.frame_size;
        }
        else if (priv != NULL)
        {
            uint64_t off = 0;
            uint32_t fsize = 0;

            r = video_frame_locate(v, priv, v->current_frame, &off, &fsize);
            if (r != FM_OK)
            {
                return (r == FM_ERR_EOF) ? FM_ERR_CORRUPTED : r;
            }
            if (!video_in_file(priv, off, fsize))
            {
                return FM_ERR_EOF;       /* the index points past the file */
            }
            cap = fsize;
            if (priv->avc1)
            {
                r = video_annexb_size(&v->file, off, fsize,
                                      video_nal_len_size(priv), &cap);
                if (r != FM_OK)
                {
                    return r;
                }
                cap += priv->sps_pps_len;
            }
        }
        if (cap == 0u)
        {
            return FM_ERR_CORRUPTED;
        }
        if (priv != NULL)
        {
            /* One scratch per open video, reused for every frame (was
             * an FM_Alloc/FM_Free pair per frame).  Round up so a run
             * of slightly larger frames does not reallocate each time. */
            size_t want = cap + (cap >> 3);

            want = (want + 4095u) & ~(size_t)4095u;
            compressed = (priv->scratch_cap >= cap)
                             ? priv->scratch
                             : video_scratch_get(&priv->scratch,
                                                 &priv->scratch_cap, want);
            if (compressed == NULL)
            {
                /* headroom refused: try the exact size */
                compressed = video_scratch_get(&priv->scratch,
                                               &priv->scratch_cap, cap);
            }
        }
        else
        {
            compressed = (uint8_t *)FM_Alloc(cap);
            own = FM_TRUE;
        }
        if (compressed == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        r = Video_ReadFrame(v, compressed, cap, &size);
        if (r == FM_OK)
        {
            r = v->codec_plugin->decode_frame(v->codec_ctx, compressed, size,
                                              pixels_out, pixels_cap);
#if FM_USE_H264
            if (r == FM_OK && v->codec_plugin == H264_GetCodecOps())
            {
                /* The decoder knows the real picture size (SPS +
                 * cropping); the container may only have the coded or
                 * a rounded size.  Publish what was actually written so
                 * nobody interprets the pixels with the wrong stride. */
                uint16_t ow = 0;
                uint16_t oh = 0;

                if (H264_GetOutputSize(v->codec_ctx, &ow, &oh) == FM_OK &&
                    ow != 0u && oh != 0u)
                {
                    v->info.width = ow;
                    v->info.height = oh;
                }
            }
#endif
        }
        if (own)
        {
            FM_Free(compressed);
        }
        return r;
    }
    if (v->info.codec != FM_VIDEO_CODEC_RAW)
    {
        /* HEVC / VP8 / VP9 / AV1 / MPEG-x have no bundled software
         * decoder: they need HWDecoder_Register().  Say which one, once
         * per video, instead of a bare "unsupported". */
        FM_VideoPriv *np = (FM_VideoPriv *)v->priv;

        if (np == NULL || !np->nodec_logged)
        {
            if (np != NULL)
            {
                np->nodec_logged = FM_TRUE;
            }
            FM_ERR_LOG("no decoder for video codec %d: register one with "
                       "HWDecoder_Register() or Video_RegisterCodec()",
                       (int)v->info.codec);
        }
        return FM_ERR_UNSUPPORTED;
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
    /* The first decode may have replaced the container's size with the
     * decoder's real (cropped) one: size the conversion from that. */
    r = Image_BufferSize(v->info.width, v->info.height, FM_PIXEL_RGB888,
                         &rgb_size);
    if (r != FM_OK) return r;
    r = Image_BufferSize(v->info.width, v->info.height, output_format,
                         &output_size);
    if (r != FM_OK) return r;
    if (rgb_work_cap < rgb_size || pixels_cap < output_size)
    {
        return FM_ERR_TOO_LARGE;
    }

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
#if !FM_USE_IMAGE
    (void)v; (void)pixels_out; (void)pixels_cap;
    (void)out_w; (void)out_h; (void)mode;
    return FM_ERR_UNSUPPORTED;
#else
    uint16_t w;
    uint16_t h;
    size_t native;
    uint8_t *scratch;
    FM_VideoPriv *priv;
    FM_Bool own = FM_FALSE;
    FM_Bool auto_w = (out_w == 0) ? FM_TRUE : FM_FALSE;
    FM_Bool auto_h = (out_h == 0) ? FM_TRUE : FM_FALSE;
    FM_Result r;

    if (v == NULL || pixels_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    priv = (FM_VideoPriv *)v->priv;
    w = v->info.width;
    h = v->info.height;
    if (w == 0 || h == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (auto_w)
    {
        out_w = w;
    }
    if (auto_h)
    {
        out_h = h;
    }
    if ((size_t)out_w * out_h * 3u > pixels_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    native = (size_t)w * h * 3u;
    /* Native-size RGB scratch: kept with the video and reused (it was
     * allocated and freed on every call -- hundreds of KB per frame). */
    if (priv != NULL)
    {
        scratch = video_scratch_get(&priv->rgb_scratch,
                                    &priv->rgb_scratch_cap, native);
    }
    else
    {
        scratch = (uint8_t *)FM_Alloc(native);
        own = FM_TRUE;
    }
    if (scratch == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = Video_DecodeFrame(v, scratch, native);
    if (r == FM_OK)
    {
        /* Scale from the size the decoder really produced (the first
         * decode may have replaced the container's with the cropped
         * one; it always fits `native`, the decoder checked). */
        w = v->info.width;
        h = v->info.height;
        if (auto_w)
        {
            out_w = w;
        }
        if (auto_h)
        {
            out_h = h;
        }
        if (w == 0 || h == 0 || (size_t)w * h * 3u > native ||
            (size_t)out_w * out_h * 3u > pixels_cap)
        {
            r = FM_ERR_TOO_LARGE;
        }
        else
        {
            r = Image_ScaleEx(scratch, native, w, h, FM_PIXEL_RGB888,
                              pixels_out, pixels_cap, out_w, out_h, mode);
        }
    }
    if (own)
    {
        FM_Free(scratch);
    }
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

        if (priv == NULL)
        {
            return FM_ERR_CORRUPTED;
        }
        /* Frames are read by absolute offset, so the file cursor only
         * needs moving for the indexed part; a frame past the index is
         * located by Video_ReadFrame when it is actually wanted. */
        if (frame_index < priv->count)
        {
            offset = priv->frames[frame_index].offset;
            if (offset > INT64_MAX || File_Seek(&v->file, (int64_t)offset,
                                                FM_SEEK_SET) != FM_OK)
            {
                return FM_ERR_IO;
            }
        }
    }
    v->current_frame = frame_index;
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Embedded audio track (demux)                                        */
/* ------------------------------------------------------------------ */
/* Bytes one sample of an uncompressed track takes in the file: 1, 2, 3
 * or 4 (integer), 4 (float).  0 = a width that is not played. */
static uint32_t video_pcm_in_bytes(const FM_VideoPriv *priv)
{
    uint32_t bits = priv->audio_bits;

    if (priv->audio_pcm_float)
    {
        return (bits == 32u) ? 4u : 0u;
    }
    return (bits == 8u || bits == 16u || bits == 24u || bits == 32u)
               ? bits / 8u : 0u;
}

/* One 32-bit float sample (its bits in `u`) as 16-bit: full scale is
 * +-1.0, anything beyond is clipped, NaN is silence. */
static int32_t video_f32_to_s16(uint32_t u)
{
    float f;
    float d;

    memcpy(&f, &u, sizeof(f));
    d = f * 32768.0f;
    if (d >= 32767.0f)
    {
        return 32767;
    }
    if (d <= -32768.0f)
    {
        return -32768;
    }
    if (d > -32768.0f && d < 32767.0f)
    {
        return (int32_t)(d + ((d >= 0.0f) ? 0.5f : -0.5f));
    }
    return 0;
}

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

static FM_Result adts_build(uint8_t *h, uint32_t aot, uint32_t rate,
                            uint16_t ch, uint32_t frame_len)
{
    uint8_t fi = adts_freq_index(rate);
    uint32_t total = frame_len + 7u;
    /* ADTS "profile" is the object type minus one and has room for types
     * 1..4 only.  HE-AAC (5, 29) is signalled as its AAC-LC core; an
     * unknown type is taken as AAC-LC.  (The field used to be left 0,
     * which announces AAC Main for every stream.) */
    uint32_t profile = (aot >= 1u && aot <= 4u) ? (aot - 1u) : 1u;
    uint32_t chcfg = (ch == 8u) ? 7u : (uint32_t)ch;

    if (fi == 0xFF || chcfg == 0u || chcfg > 7u || total > 0x1FFFu)
    {
        return FM_ERR_UNSUPPORTED;
    }
    h[0] = 0xFF;
    h[1] = 0xF1; /* MPEG-4, no CRC */
    h[2] = (uint8_t)((profile << 6) | ((uint32_t)fi << 2) | (chcfg >> 2));
    ch = (uint16_t)chcfg;
    h[3] = (uint8_t)((((uint32_t)ch & 3u) << 6) | ((total >> 11) & 3u));
    h[4] = (uint8_t)((total >> 3) & 0xFFu);
    h[5] = (uint8_t)(((total & 7u) << 5) | 0x1Fu);
    h[6] = 0xFC;
    return FM_OK;
}

/* Bits per sample of a FLAC track, from the STREAMINFO the container
 * carries (MP4 dfLa, MKV CodecPrivate; with or without "fLaC" in front);
 * 0 = not known. */
static uint32_t video_flac_bits(const FM_VideoPriv *priv)
{
    const uint8_t *p = video_audio_private(priv);
    uint32_t n = priv->audio_private_size;

    if (p != NULL && n >= 4u && memcmp(p, "fLaC", 4) == 0)
    {
        p += 4;
        n -= 4u;
    }
    if (p == NULL || n < 18u || (p[0] & 0x7Fu) != 0u)
    {
        return 0u;
    }
    return ((((uint32_t)p[16] & 1u) << 4) | ((uint32_t)p[17] >> 4)) + 1u;
}

/* A FLAC track wider than the bundled decoder does (dr_flac: 24 bits,
 * FLAC itself allows 32).  A decoder the application registered under
 * "FLAC" in its place is asked whatever the width. */
static FM_Bool video_flac_too_wide(const FM_VideoPriv *priv)
{
    return (priv->audio_codec == FM_VIDEO_AUDIO_FLAC &&
            video_flac_bits(priv) > FM_FLAC_MAX_BITS &&
            Audio_GetDecoder("FLAC") == FM_Flac_GetOps())
               ? FM_TRUE : FM_FALSE;
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
    /* Packets in the track: the open-time scan counts every one (AVI,
     * MKV), including those past the index that are streamed. */
    info->sample_count = (priv->atail.total > priv->audio_count)
                             ? priv->atail.total : priv->audio_count;
    /* Will Video_ReadAudioFrame hand out playable PCM for this track? */
    switch (priv->audio_codec)
    {
    case FM_VIDEO_AUDIO_PCM:
        /* 8, 16, 24, 32 bits and 32-bit float; see video_pcm_read() */
        info->decodable = (video_pcm_in_bytes(priv) != 0u) ? FM_TRUE
                                                           : FM_FALSE;
        break;
    case FM_VIDEO_AUDIO_ALAW:
    case FM_VIDEO_AUDIO_ULAW:
        info->decodable = FM_TRUE;
        break;
    case FM_VIDEO_AUDIO_ADPCM:
        /* mono and stereo; see video_adpcm_read() */
        info->decodable = (priv->audio_channels >= 1u &&
                           priv->audio_channels <= 2u) ? FM_TRUE : FM_FALSE;
        break;
    case FM_VIDEO_AUDIO_MP3:
        info->decodable = FM_USE_MP3 ? FM_TRUE : FM_FALSE;
        break;
    default:
    {
        const char *tag = video_audio_tag(priv->audio_codec);

        info->decodable = (tag != NULL && Audio_GetDecoder(tag) != NULL)
                              ? FM_TRUE : FM_FALSE;
        if (priv->audio_codec == FM_VIDEO_AUDIO_FLAC &&
            video_flac_bits(priv) != 0u)
        {
            info->bits_per_sample = (uint16_t)video_flac_bits(priv);
            if (video_flac_too_wide(priv))
            {
                info->decodable = FM_FALSE;
            }
        }
        break;
    }
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Compressed audio track -> PCM through the decoder registry           */
/* ------------------------------------------------------------------ */
#if VIDEO_AUDIO_REGISTRY
/* Make sure the decoder for this track is open.  FM_ERR_UNSUPPORTED =
 * nobody registered one. */
static FM_Result video_audio_dec_get(FM_VideoPriv *priv)
{
    const char *tag = video_audio_tag(priv->audio_codec);
    const FM_AudioDecoderOps *ops;
    FM_AudioInfo ai;
    void *ctx = NULL;

    if (tag == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (priv->adec_ops != NULL)
    {
        return FM_OK;                        /* already open: reuse */
    }
    ops = Audio_GetDecoder(tag);
    if (ops == NULL || ops->decode == NULL)
    {
        return FM_ERR_UNSUPPORTED;
    }
    memset(&ai, 0, sizeof(ai));
    ai.sample_rate = priv->audio_rate ? priv->audio_rate : 44100u;
    ai.channels = priv->audio_channels ? priv->audio_channels : 2u;
    ai.bits_per_sample = 16u;
    (void)strncpy(ai.codec, tag, sizeof(ai.codec) - 1u);
    if (ops->init != NULL)
    {
        FM_Result r = ops->init(&ctx, &ai);

        if (r != FM_OK)
        {
            return (r == FM_ERR_UNSUPPORTED) ? FM_ERR_INVALID_FORMAT : r;
        }
    }
    priv->adec_ops = ops;
    priv->adec_ctx = ctx;
    return FM_OK;
}

/* Scratch buffer for one packet (plus synthesised codec headers), grown
 * on demand and kept until the video is closed. */
static uint8_t *video_audio_scratch(FM_VideoPriv *priv, size_t need)
{
    if (need > priv->adec_cap)
    {
        uint8_t *nb = (uint8_t *)FM_Alloc(need);

        if (nb == NULL)
        {
            return NULL;
        }
        if (priv->adec_buf != NULL)
        {
            FM_Free(priv->adec_buf);
        }
        priv->adec_buf = nb;
        priv->adec_cap = need;
    }
    return priv->adec_buf;
}

/* Decode the packet at offset/fsize into buf as interleaved S16 PCM.
 * The packet is consumed (audio_frame_index advances) whenever it was
 * read successfully -- also when the decoder rejects it: retrying the
 * same damaged packet forever would silence the rest of the track.
 * I/O and memory errors leave the cursor alone so the caller can retry. */
static FM_Result video_audio_decode_packet(FM_Video *v, FM_VideoPriv *priv,
                                           uint64_t offset, uint32_t fsize,
                                           uint8_t *buf, size_t cap,
                                           size_t *size)
{
    const FM_AudioDecoderOps *ops = priv->adec_ops;
    size_t out_cap_samples = cap / 2u;
    size_t out_samples = 0;
    const uint8_t *in;
    size_t in_len;
    uint8_t *tmp;
    FM_Result r;
    FM_Result dr;

    if (out_cap_samples == 0u)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (fsize == 0u)
    {
        v->audio_frame_index++;
        return FM_ERR_CORRUPTED;
    }

    if (priv->audio_codec == FM_VIDEO_AUDIO_FLAC &&
        priv->audio_private_size > 0u)
    {
        /* MP4 dfLa / MKV CodecPrivate hold STREAMINFO; a FLAC decoder
         * needs "fLaC" + STREAMINFO in front of the frame. */
        const uint8_t *setup = video_audio_private(priv);
        FM_Bool has_magic = (priv->audio_private_size >= 4u &&
                             memcmp(setup, "fLaC", 4) == 0)
                                ? FM_TRUE : FM_FALSE;
        size_t hdr_len = (size_t)priv->audio_private_size +
                         (has_magic ? 0u : 4u);

        tmp = video_audio_scratch(priv, hdr_len + (size_t)fsize);
        if (tmp == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        if (has_magic)
        {
            memcpy(tmp, setup, priv->audio_private_size);
        }
        else
        {
            memcpy(tmp, "fLaC", 4);
            memcpy(tmp + 4, setup, priv->audio_private_size);
        }
        r = video_read_at(&v->file, offset, tmp + hdr_len, fsize);
        if (r != FM_OK)
        {
            return r;
        }
        in = tmp;
        in_len = hdr_len + (size_t)fsize;
    }
    else
    {
        tmp = video_audio_scratch(priv, fsize);
        if (tmp == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK)
        {
            return r;
        }
        in = tmp;
        in_len = fsize;
    }

    dr = ops->decode(priv->adec_ctx, in, in_len, (int16_t *)buf,
                     out_cap_samples, &out_samples);
    v->audio_frame_index++;
    if ((dr == FM_OK || dr == FM_ERR_EOF) && out_samples > 0u)
    {
        if (out_samples > out_cap_samples)
        {
            out_samples = out_cap_samples;   /* misbehaving plugin */
        }
        *size = out_samples * 2u;
        return FM_OK;
    }
    return (dr == FM_OK || dr == FM_ERR_EOF) ? FM_ERR_CORRUPTED : dr;
}
#else /* !VIDEO_AUDIO_REGISTRY */
static FM_Result video_audio_dec_get(FM_VideoPriv *priv)
{
    (void)priv;
    return FM_ERR_UNSUPPORTED;
}

static FM_Result video_audio_decode_packet(FM_Video *v, FM_VideoPriv *priv,
                                           uint64_t offset, uint32_t fsize,
                                           uint8_t *buf, size_t cap,
                                           size_t *size)
{
    (void)v; (void)priv; (void)offset; (void)fsize;
    (void)buf; (void)cap; (void)size;
    return FM_ERR_UNSUPPORTED;
}
#endif /* VIDEO_AUDIO_REGISTRY */

/* Where the next audio packet is: from the index, or past its end by
 * walking the container. */
static FM_Result video_audio_locate(FM_Video *v, FM_VideoPriv *priv,
                                    uint64_t *offset, uint32_t *fsize)
{
    FM_Result r = FM_OK;

    if (v->audio_frame_index < priv->audio_count)
    {
        *offset = priv->audio_frames[v->audio_frame_index].offset;
        *fsize = priv->audio_frames[v->audio_frame_index].size;
    }
    else
    {
        r = video_tail_locate(v, priv, FM_TRUE, v->audio_frame_index,
                              offset, fsize);
    }
    if (r == FM_OK && !video_in_file(priv, *offset, *fsize))
    {
        /* an index entry that points past the file: see video_in_file() */
        r = FM_ERR_EOF;
    }
    return r;
}

#if VIDEO_VORBIS_STREAM
/* MKV CodecPrivate of a Vorbis track: the byte 2, the lengths of the
 * first two header packets in Xiph lacing (255, 255, ..., rest), then
 * the three packets. */
static FM_Bool vorbis_private_split(const uint8_t *p, uint32_t size,
                                    const uint8_t *hdr[3], uint32_t len[3])
{
    uint32_t pos = 1u;
    uint32_t i;

    if (p == NULL || size < 3u || p[0] != 2u)
    {
        return FM_FALSE;
    }
    for (i = 0u; i < 2u; i++)
    {
        uint32_t l = 0u;
        uint8_t b;

        do
        {
            if (pos >= size)
            {
                return FM_FALSE;
            }
            b = p[pos++];
            l += b;
            if (l > size)
            {
                return FM_FALSE;
            }
        } while (b == 255u);
        len[i] = l;
    }
    if (len[0] > size - pos || len[1] >= size - pos - len[0])
    {
        return FM_FALSE;
    }
    len[2] = size - pos - len[0] - len[1];
    hdr[0] = p + pos;
    hdr[1] = hdr[0] + len[0];
    hdr[2] = hdr[1] + len[1];
    return FM_TRUE;
}

/*
 * Vorbis track -> interleaved 16-bit PCM.
 *
 * This used to wrap the headers and ONE packet in a made-up Ogg file
 * and open a new decoder on it, for every packet.  A Vorbis packet has
 * no output of its own (it overlaps the one before), and the made-up
 * file put the three headers into one Ogg packet, which the decoder
 * refuses: every call returned FM_ERR_CORRUPTED, for every MKV / WebM
 * file with Vorbis sound.  The stream decoder keeps its state.
 *
 * A packet without output (the first of a stream, the first two after a
 * seek, a damaged one) is passed over: the call goes on to the next
 * packet, up to eight of them, before it reports FM_ERR_CORRUPTED.
 */
static FM_Result video_vorbis_read(FM_Video *v, FM_VideoPriv *priv,
                                   uint8_t *buf, size_t cap, size_t *size)
{
    uint64_t offset;
    uint32_t fsize;
    uint32_t tries;
    FM_Result r;

    if (cap < 4u)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (priv->vorbis == NULL && priv->vorbis_error == FM_OK)
    {
        const uint8_t *hdr[3];
        uint32_t len[3];

        if (!vorbis_private_split(video_audio_private(priv),
                                  priv->audio_private_size, hdr, len))
        {
            r = FM_ERR_CORRUPTED;
        }
        else
        {
            r = FM_Vorbis_StreamOpen(hdr, len, &priv->vorbis);
        }
        if (r == FM_ERR_NO_MEMORY)
        {
            return r;                    /* may work on a later call */
        }
        if (r != FM_OK)
        {
            priv->vorbis_error = r;
            FM_ERR_LOG("vorbis track: headers not usable (%d)", (int)r);
        }
    }
    if (priv->vorbis == NULL)
    {
        /* Nothing to decode with.  The packets are still passed over
         * one a call, so that a reader arrives at FM_ERR_EOF. */
        r = video_audio_locate(v, priv, &offset, &fsize);
        if (r != FM_OK)
        {
            return r;
        }
        v->audio_frame_index++;
        return priv->vorbis_error;
    }
    if (FM_Vorbis_StreamPending(priv->vorbis) != 0u)
    {
        /* the rest of a packet that was larger than the caller's buffer */
        return FM_Vorbis_StreamDecode(priv->vorbis, NULL, 0u, buf, cap, size);
    }
    for (tries = 0u; tries < 8u; tries++)
    {
        uint8_t *tmp;

        r = video_audio_locate(v, priv, &offset, &fsize);
        if (r != FM_OK)
        {
            return r;
        }
        if (fsize == 0u)
        {
            v->audio_frame_index++;
            continue;
        }
        tmp = video_audio_scratch(priv, fsize);
        if (tmp == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        r = video_read_at(&v->file, offset, tmp, fsize);
        if (r != FM_OK)
        {
            return r;                    /* nothing consumed: retry */
        }
        v->audio_frame_index++;
        r = FM_Vorbis_StreamDecode(priv->vorbis, tmp, fsize, buf, cap, size);
        if (r == FM_ERR_NO_MEMORY)
        {
            return r;
        }
        if (r == FM_OK && *size != 0u)
        {
            return FM_OK;
        }
    }
    return FM_ERR_CORRUPTED;
}
#endif /* VIDEO_VORBIS_STREAM */

#if FM_USE_MP3
/*
 * MP3 track -> interleaved 16-bit PCM.
 *
 * The decoder lives as long as the video.  It used to be created afresh
 * for every packet, and MP3 frames are not independent: a frame keeps
 * part of its data in the frames before it (the bit reservoir) and its
 * output overlaps the previous frame's.  Muxers write one frame per
 * packet, so with a new decoder each time every frame that used the
 * reservoir -- nearly all of them -- was rejected: a normal AVI from
 * ffmpeg decoded its first frame and nothing else.  The rejected packet
 * was not skipped either, so the same FM_ERR_CORRUPTED came back on
 * every call and the end of the track was never reached.
 *
 * Now:
 *   - a frame the decoder cannot produce (reservoir not filled yet: the
 *     first frames of a track, or after a seek) is delivered as silence
 *     of the same length, so audio and video stay in step;
 *   - a packet with no frame in it is consumed and reported as
 *     FM_ERR_CORRUPTED once; the next call continues after it;
 *   - a packet that holds more frames than `cap` has room for is
 *     continued by the next call (it used to lose the rest).  One frame
 *     is up to MINIMP3_MAX_SAMPLES_PER_FRAME samples (4608 bytes); with
 *     less room than that the part of a frame that does not fit is
 *     dropped.
 */
static FM_Result video_mp3_read(FM_Video *v, FM_VideoPriv *priv,
                                uint8_t *buf, size_t cap, size_t *size)
{
    struct FM_VideoMp3 *m = priv->mp3;
    uint64_t offset;
    uint32_t fsize;
    uint8_t *tmp;
    size_t cap_samples = cap / 2u;
    size_t out_samples = 0u;
    uint32_t pos = 0u;
    FM_Bool fresh = FM_FALSE;            /* a packet not yet counted */
    FM_Result r;

    if (cap_samples == 0u)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (m == NULL)
    {
        m = (struct FM_VideoMp3 *)FM_Alloc(sizeof(*m));
        if (m == NULL)
        {
            return FM_ERR_NO_MEMORY;
        }
        mp3dec_init(&m->dec);
        priv->mp3 = m;
    }

    if (priv->audio_part_left != 0u)
    {
        /* the rest of the packet the previous call could not finish */
        offset = priv->audio_part_off;
        fsize = priv->audio_part_left;
    }
    else
    {
        r = video_audio_locate(v, priv, &offset, &fsize);
        if (r != FM_OK)
        {
            return r;
        }
        fresh = FM_TRUE;
    }
    if (fsize == 0u)
    {
        if (fresh)
        {
            v->audio_frame_index++;
        }
        priv->audio_part_left = 0u;
        return FM_ERR_CORRUPTED;
    }

    /* Memory and I/O errors take nothing: the same call can be repeated.
     * (The packet used to be counted before it was read, so a failed
     * allocation dropped it, and the rest of a packet in progress.) */
    tmp = (uint8_t *)FM_Alloc(fsize);
    if (tmp == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = video_read_at(&v->file, offset, tmp, fsize);
    if (r != FM_OK)
    {
        FM_Free(tmp);
        return r;
    }
    if (fresh)
    {
        /* From here on the packet is taken, whatever is in it. */
        v->audio_frame_index++;
    }

    while (pos < fsize)
    {
        mp3dec_frame_info_t info;
        size_t n;
        int samples;

        /* Room for a whole frame?  If not, and something has been
         * produced already, leave the rest for the next call. */
        if (out_samples != 0u &&
            cap_samples - out_samples < (size_t)MINIMP3_MAX_SAMPLES_PER_FRAME)
        {
            break;
        }
        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&m->dec, tmp + pos,
                                      (int)(fsize - pos), m->pcm, &info);
        if (info.frame_bytes <= 0 ||
            (uint32_t)info.frame_bytes > fsize - pos)
        {
            pos = fsize;                 /* nothing more in this packet */
            break;
        }
        pos += (uint32_t)info.frame_bytes;
        if (samples > 0 && info.channels > 0)
        {
            n = (size_t)samples * (size_t)info.channels;
        }
        else if (info.hz > 0 && info.channels > 0 && info.layer == 3)
        {
            /* A frame was there but could not be decoded (its data
             * starts in a frame we never saw).  Its length is known:
             * keep the time line, play silence. */
            n = (size_t)((info.hz >= 32000) ? 1152 : 576) *
                (size_t)info.channels;
            memset(m->pcm, 0, n * sizeof(m->pcm[0]));
        }
        else
        {
            continue;                    /* bytes skipped, no frame */
        }
        if (n > cap_samples - out_samples)
        {
            n = cap_samples - out_samples;
        }
        memcpy(buf + out_samples * 2u, m->pcm, n * 2u);
        out_samples += n;
    }
    FM_Free(tmp);

    if (pos < fsize)
    {
        priv->audio_part_off = offset + pos;
        priv->audio_part_left = fsize - pos;
    }
    else
    {
        priv->audio_part_left = 0u;
    }
    if (out_samples == 0u)
    {
        priv->audio_part_left = 0u;
        return FM_ERR_CORRUPTED;
    }
    *size = out_samples * 2u;
    return FM_OK;
}
#endif /* FM_USE_MP3 */

/*
 * ADPCM track -> interleaved 16-bit PCM, whole blocks at a time.
 *
 * `cap` must hold one block (FM_ERR_TOO_LARGE otherwise, nothing is
 * consumed).  A chunk with more blocks than fit is continued by the
 * next call.
 */
static FM_Result video_adpcm_read(FM_Video *v, FM_VideoPriv *priv,
                                  uint8_t *buf, size_t cap, size_t *size)
{
    uint32_t ch = priv->audio_channels;
    uint16_t tag = priv->audio_wav_tag;
    FM_Bool resumed = (priv->audio_part_left != 0u) ? FM_TRUE : FM_FALSE;
    uint64_t offset;
    uint32_t fsize;
    uint32_t ba;
    uint32_t take;
    uint32_t pos;
    size_t block_bytes;
    size_t nblocks;
    size_t out = 0u;
    uint8_t *tmp;
    FM_Result r;

    if (ch < 1u || ch > 2u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (resumed)
    {
        offset = priv->audio_part_off;
        fsize = priv->audio_part_left;
    }
    else
    {
        r = video_audio_locate(v, priv, &offset, &fsize);
        if (r != FM_OK)
        {
            return r;
        }
    }

    ba = priv->audio_block_align;
    if (ba == 0u || ba > fsize)
    {
        ba = fsize;                      /* the chunk is one (short) block */
    }
    block_bytes = adpcm_block_frames(tag, ch, ba) * ch * 2u;
    if (block_bytes == 0u)
    {
        /* not even a block header */
        if (!resumed)
        {
            v->audio_frame_index++;
        }
        priv->audio_part_left = 0u;
        return FM_ERR_CORRUPTED;
    }
    if (cap < block_bytes)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (!resumed)
    {
        v->audio_frame_index++;
    }

    nblocks = cap / block_bytes;
    take = fsize;
    if ((uint64_t)nblocks * ba < (uint64_t)fsize)
    {
        take = (uint32_t)nblocks * ba;
    }
    tmp = (uint8_t *)FM_Alloc(take);
    if (tmp == NULL)
    {
        priv->audio_part_left = 0u;
        return FM_ERR_NO_MEMORY;
    }
    r = video_read_at(&v->file, offset, tmp, take);
    if (r != FM_OK)
    {
        FM_Free(tmp);
        priv->audio_part_left = 0u;
        return r;
    }
    for (pos = 0u; pos < take; pos += ba)
    {
        uint32_t n = (take - pos < ba) ? take - pos : ba;
        size_t frames = adpcm_block_frames(tag, ch, n);

        if (frames == 0u)
        {
            break;                       /* a tail shorter than a header */
        }
        if (tag == 0x0002u)
        {
            (void)adpcm_ms_block(tmp + pos, n, ch, buf + out);
        }
        else
        {
            adpcm_ima_block(tmp + pos, n, ch, buf + out);
        }
        out += frames * ch * 2u;
    }
    FM_Free(tmp);

    if (take < fsize)
    {
        priv->audio_part_off = offset + take;
        priv->audio_part_left = fsize - take;
    }
    else
    {
        priv->audio_part_left = 0u;
    }
    if (out == 0u)
    {
        return FM_ERR_CORRUPTED;
    }
    *size = out;
    return FM_OK;
}

/*
 * Uncompressed track -> interleaved 16-bit little-endian PCM.
 *
 * In the file a sample is 8 bits (unsigned; signed in QuickTime), 16, 24
 * or 32 bits, or a 32-bit float, in either byte order.  Whatever it is,
 * the caller gets 16-bit samples: wider ones lose their low bits, floats
 * are scaled and clipped.
 *
 * The packet is read into the caller's buffer and converted in place, so
 * one call delivers as many sample frames as `cap` holds of the *file's*
 * format (for 24-bit input two thirds of the buffer are filled).  A
 * packet with more than that is continued by the next call.  Packets of
 * less than 4 KB that follow each other in the file are read together:
 * QuickTime writes one sample frame a packet, which was one file read
 * for every four bytes.
 *
 * `cap` below one sample frame: FM_ERR_TOO_LARGE.  Like an I/O error it
 * takes nothing: the same data comes with the next, larger buffer.
 */
#define VIDEO_PCM_GATHER 4096u

static FM_Result video_pcm_read(FM_Video *v, FM_VideoPriv *priv,
                                uint8_t *buf, size_t cap, size_t *size)
{
    uint32_t in_bytes = video_pcm_in_bytes(priv);
    uint32_t ch = (priv->audio_channels != 0u) ? priv->audio_channels : 1u;
    uint32_t frame_in = in_bytes * ch;
    FM_Bool fresh = (priv->audio_part_left == 0u) ? FM_TRUE : FM_FALSE;
    uint32_t first = v->audio_frame_index;
    uint32_t packets = 0u;
    uint64_t offset = priv->audio_part_off;
    uint32_t fsize = priv->audio_part_left;
    uint32_t max_in;
    uint32_t take;
    uint32_t samples;
    uint32_t i;
    FM_Result r;

    if (fresh)
    {
        r = video_audio_locate(v, priv, &offset, &fsize);
        if (r != FM_OK)
        {
            return r;
        }
        packets = 1u;
    }
    if (in_bytes == 0u)
    {
        /* a width that is not played (12, 20, 64 bits): say so for every
         * packet, and let the track end */
        v->audio_frame_index = first + packets;
        priv->audio_part_left = 0u;
        return FM_ERR_UNSUPPORTED;
    }
    max_in = (cap > 0x40000000u) ? 0x40000000u : (uint32_t)cap;
    if (in_bytes == 1u)
    {
        max_in /= 2u;                    /* one byte in, two out */
    }
    max_in -= max_in % frame_in;
    if (max_in == 0u)
    {
        /* nothing taken: neither the packet nor the rest of one */
        v->audio_frame_index = first;
        return FM_ERR_TOO_LARGE;
    }
    if (fresh)
    {
        while (fsize < VIDEO_PCM_GATHER && fsize < max_in)
        {
            uint64_t next_off = 0;
            uint32_t next_size = 0;

            v->audio_frame_index = first + packets;
            if (video_audio_locate(v, priv, &next_off, &next_size) != FM_OK ||
                next_off != offset + fsize || next_size > max_in - fsize)
            {
                break;
            }
            fsize += next_size;
            packets++;
        }
        v->audio_frame_index = first;
    }
    take = (fsize > max_in) ? max_in : fsize;
    take -= take % frame_in;
    if (take == 0u)
    {
        /* a packet shorter than one sample frame */
        v->audio_frame_index = first + packets;
        priv->audio_part_left = 0u;
        return FM_ERR_CORRUPTED;
    }
    r = video_read_at(&v->file, offset, buf, take);
    if (r != FM_OK)
    {
        return r;                        /* nothing taken: retry */
    }
    v->audio_frame_index = first + packets;
    if (fsize - take >= frame_in)
    {
        priv->audio_part_off = offset + take;
        priv->audio_part_left = fsize - take;
    }
    else
    {
        priv->audio_part_left = 0u;
    }

    samples = take / in_bytes;
    if (in_bytes == 1u)
    {
        /* the output is twice the input: from the end */
        for (i = samples; i > 0u; )
        {
            int32_t s;

            i--;
            s = priv->audio_pcm_s8 ? (int32_t)(int8_t)buf[i]
                                   : (int32_t)buf[i] - 128;
            s *= 256;
            buf[2u * i] = (uint8_t)((uint32_t)s & 0xFFu);
            buf[2u * i + 1u] = (uint8_t)(((uint32_t)s >> 8) & 0xFFu);
        }
    }
    else
    {
        /* the output is not longer than the input: from the start */
        for (i = 0u; i < samples; i++)
        {
            const uint8_t *q = buf + (size_t)i * in_bytes;
            uint8_t lo;
            uint8_t hi;

            if (priv->audio_pcm_float)
            {
                int32_t s = video_f32_to_s16(priv->audio_pcm_be
                                                 ? video_be32(q)
                                                 : video_rd32(q));

                lo = (uint8_t)((uint32_t)s & 0xFFu);
                hi = (uint8_t)(((uint32_t)s >> 8) & 0xFFu);
            }
            else if (priv->audio_pcm_be)
            {
                hi = q[0];
                lo = q[1];
            }
            else
            {
                hi = q[in_bytes - 1u];
                lo = q[in_bytes - 2u];
            }
            buf[2u * (size_t)i] = lo;
            buf[2u * (size_t)i + 1u] = hi;
        }
    }
    *size = (size_t)samples * 2u;
    return FM_OK;
}

/* One packet (or part of one) of the track, as the codec gives it:
 * Video_ReadAudioFrame without the trimming. */
static FM_Result video_audio_read_raw(FM_Video *v, FM_VideoPriv *priv,
                                      uint8_t *buf, size_t cap, size_t *size)
{
    uint64_t offset;
    uint32_t fsize;
    FM_Result r;

#if FM_USE_MP3
    if (priv->audio_codec == FM_VIDEO_AUDIO_MP3)
    {
        return video_mp3_read(v, priv, buf, cap, size);
    }
#endif
    if (priv->audio_codec == FM_VIDEO_AUDIO_ADPCM)
    {
        return video_adpcm_read(v, priv, buf, cap, size);
    }
#if VIDEO_VORBIS_STREAM
    /* The built-in Vorbis decoder, and the headers from the container.
     * (A decoder the application registered under "VORBIS" in its place
     * gets the bare packets through the registry, below.) */
    if (priv->audio_codec == FM_VIDEO_AUDIO_VORBIS &&
        priv->audio_private_size != 0u &&
        Audio_GetDecoder("VORBIS") == FM_Vorbis_GetOps())
    {
        return video_vorbis_read(v, priv, buf, cap, size);
    }
#endif
    if (priv->audio_codec == FM_VIDEO_AUDIO_PCM)
    {
        return video_pcm_read(v, priv, buf, cap, size);
    }
    /* From the index, or past its end straight out of the container
     * (AVI movi list / MKV clusters / MP4 sample tables). */
    r = video_audio_locate(v, priv, &offset, &fsize);
    if (r != FM_OK)
    {
        return r;
    }

    if (video_audio_tag(priv->audio_codec) != NULL)
    {
        if (video_flac_too_wide(priv))
        {
            /* A valid track this build cannot play.  The packet is
             * passed over, so the track still ends. */
            if (!priv->audio_nodec_logged)
            {
                priv->audio_nodec_logged = FM_TRUE;
                FM_ERR_LOG("FLAC track with %u bits per sample: the "
                           "decoder does at most %u",
                           (unsigned)video_flac_bits(priv),
                           (unsigned)FM_FLAC_MAX_BITS);
            }
            v->audio_frame_index++;
            return FM_ERR_UNSUPPORTED;
        }
        /* Compressed track: decode to 16-bit PCM with the registered
         * decoder.  The decoder context lives as long as the video. */
        r = video_audio_dec_get(priv);
        if (r == FM_OK)
        {
            return video_audio_decode_packet(v, priv, offset, fsize, buf,
                                             cap, size);
        }
        if (r != FM_ERR_UNSUPPORTED ||
            priv->audio_codec != FM_VIDEO_AUDIO_AAC)
        {
            /* No decoder for FLAC/Opus/Vorbis.  The raw packets are not
             * a playable stream on their own (the codec headers live in
             * the container), and handing them out as if they were PCM
             * is full-scale noise on a DAC -- say so instead. */
            if (r == FM_ERR_UNSUPPORTED && !priv->audio_nodec_logged)
            {
                priv->audio_nodec_logged = FM_TRUE;
                FM_ERR_LOG("audio track needs a \"%s\" decoder: none "
                           "registered (Audio_RegisterDecoder)",
                           video_audio_tag(priv->audio_codec));
            }
            if (r == FM_ERR_UNSUPPORTED)
            {
                /* the packet is passed over, so that the track ends
                 * (the same answer for ever looked like a stalled
                 * player, not like a track that cannot be played) */
                v->audio_frame_index++;
            }
            return r;
        }
        /* AAC without a decoder: documented pass-through, each sample
         * framed with an ADTS header so it is a valid .aac stream. */
        {
            uint8_t adts[7];
            if (cap < 7u + fsize)
            {
                return FM_ERR_TOO_LARGE;
            }
            r = adts_build(adts, priv->audio_aot, priv->audio_rate,
                           priv->audio_channels, fsize);
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
            uint32_t s = (uint32_t)(int32_t)alaw_decode(tmp[i]);
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
            uint32_t s = (uint32_t)(int32_t)ulaw_decode(tmp[i]);
            buf[i*2u] = (uint8_t)(s & 0xFFu);
            buf[i*2u+1u] = (uint8_t)((s>>8)&0xFFu);
        }
        FM_Free(tmp);
        *size = need;
    }
    else
    {
        return FM_ERR_UNSUPPORTED;
    }
    v->audio_frame_index++;
    return FM_OK;
}

/* PCM bytes of the packet last read that the decoder still holds (the
 * Vorbis decoder keeps what did not fit the caller's buffer). */
static size_t video_audio_rest_bytes(const FM_VideoPriv *priv)
{
#if VIDEO_VORBIS_STREAM
    if (priv->vorbis != NULL)
    {
        return FM_Vorbis_StreamPending(priv->vorbis);
    }
#endif
    (void)priv;
    return 0u;
}

/* Does this track get its encoder delay and padding removed?  Only one
 * that comes out as PCM: AAC without a decoder is handed out as ADTS
 * frames, which are not samples. */
static FM_Bool video_audio_trims(const FM_VideoPriv *priv)
{
    if (priv->audio_channels == 0u ||
        (priv->audio_skip == 0u && priv->audio_limit == 0u &&
         priv->audio_pad_frames == 0u))
    {
        return FM_FALSE;
    }
    if (priv->audio_codec == FM_VIDEO_AUDIO_AAC &&
        Audio_GetDecoder("AAC") == NULL)
    {
        return FM_FALSE;
    }
    return FM_TRUE;
}

/*
 * The public read: the codec's output with the frames the container marks
 * as not being sound removed.
 *
 *   start   audio_skip frames (MP4 edit list, MKV CodecDelay), from the
 *           first packet on; after Video_SeekAudio(0) again
 *   end     MP4: nothing past the length the edit list gives;
 *           MKV: DiscardPadding off the end of the packet that carries it
 *
 * After a seek into the track the frame count is not known any more, and
 * the MP4 end limit is not applied (the MKV one still is: it belongs to
 * a packet).  A call whose frames are all removed reads on, so FM_OK
 * always comes with data.
 */
FM_Result Video_ReadAudioFrame(FM_Video *v, uint8_t *buf, size_t cap,
                               size_t *size)
{
    FM_VideoPriv *priv;
    size_t frame_bytes;
    uint32_t tries;

    if (v == NULL || buf == NULL || size == NULL || !File_IsOpen(&v->file))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    priv = (FM_VideoPriv *)v->priv;
    *size = 0;
    if (priv == NULL || !priv->audio_present)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (!video_audio_trims(priv))
    {
        return video_audio_read_raw(v, priv, buf, cap, size);
    }
    frame_bytes = (size_t)priv->audio_channels * 2u;
    for (tries = 0u; tries < 4096u; tries++)
    {
        FM_Bool limited = (priv->audio_limit != 0u && !priv->audio_limit_off)
                              ? FM_TRUE : FM_FALSE;
        size_t frames;
        FM_Result r;

        if (limited && priv->audio_out >= priv->audio_limit)
        {
            return FM_ERR_EOF;
        }
        *size = 0;
        r = video_audio_read_raw(v, priv, buf, cap, size);
        if (r != FM_OK)
        {
            *size = 0;
            return r;
        }
        frames = *size / frame_bytes;
        if (priv->audio_pad_frames != 0u &&
            v->audio_frame_index == priv->audio_pad_packet + 1u &&
            priv->audio_part_left == 0u)
        {
            /* The padded packet: its last pad_frames frames go.  When
             * the packet comes in parts (a small buffer), `rest` frames
             * of it are still in the decoder. */
            size_t rest = video_audio_rest_bytes(priv) / frame_bytes;

            if (rest < priv->audio_pad_frames)
            {
                size_t cut = priv->audio_pad_frames - rest;

                frames = (frames > cut) ? frames - cut : 0u;
            }
        }
        if (priv->audio_skip_left != 0u)
        {
            size_t drop = (frames < priv->audio_skip_left)
                              ? frames : priv->audio_skip_left;

            frames -= drop;
            priv->audio_skip_left -= (uint32_t)drop;
            if (frames != 0u)
            {
                memmove(buf, buf + drop * frame_bytes, frames * frame_bytes);
            }
        }
        if (limited && frames > priv->audio_limit - priv->audio_out)
        {
            frames = (size_t)(priv->audio_limit - priv->audio_out);
        }
        priv->audio_out += frames;
        *size = frames * frame_bytes;
        if (frames != 0u)
        {
            return FM_OK;
        }
    }
    *size = 0;
    return FM_ERR_EOF;
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
        sample_index >= priv->atail.total)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* Force the streaming cursor to re-sync if we jumped backwards. */
    if (sample_index < v->audio_frame_index)
    {
        priv->atail.cur_pos = 0u;
    }
    /* ...and is a discontinuity for a stateful decoder: start it afresh
     * rather than let it predict from packets that are no longer
     * adjacent.  Leaving a half-delivered chunk behind is one too, even
     * when the index does not move. */
    if (sample_index != v->audio_frame_index || priv->audio_part_left != 0u)
    {
        video_audio_dec_reset(priv);
    }
    /* A seek abandons any half-delivered oversized chunk. */
    priv->audio_part_left = 0u;
    v->audio_frame_index = sample_index;
    /* Trimming (Video_ReadAudioFrame): from the start everything is as
     * after the open; anywhere else there is no delay to drop and the
     * number of frames before this point is not known. */
    priv->audio_skip_left = (sample_index == 0u) ? priv->audio_skip : 0u;
    priv->audio_out = 0u;
    priv->audio_limit_off = (sample_index == 0u) ? FM_FALSE : FM_TRUE;
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

#else /* FM_USE_VIDEO == 0 */

/* Video is compiled out.  The public API still links, so shared
 * application code does not need #if around every call, and every entry
 * point says clearly that the operation cannot be performed.  The stubs
 * are stateless: no index pool, no decoder, no .data/.bss. */
FM_Result Video_Open(const char *path, FM_Video *v)
{
    (void)path; (void)v;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_Close(FM_Video *v)
{
    (void)v;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_GetInfo(const FM_Video *v, FM_VideoInfo *info)
{
    (void)v; (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_ReadFrame(FM_Video *v, void *frame_out, size_t frame_cap,
                          size_t *size)
{
    (void)v; (void)frame_out; (void)frame_cap;
    if (size != NULL)
    {
        *size = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_DecodeFrame(FM_Video *v, void *pixels_out, size_t pixels_cap)
{
    (void)v; (void)pixels_out; (void)pixels_cap;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_DecodeFrameAs(FM_Video *v, FM_PixelFormat output_format,
                              void *pixels_out, size_t pixels_cap,
                              void *rgb_work, size_t rgb_work_cap)
{
    (void)v; (void)output_format; (void)pixels_out; (void)pixels_cap;
    (void)rgb_work; (void)rgb_work_cap;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_DecodeFrameScaled(FM_Video *v, void *pixels_out,
                                  size_t pixels_cap, uint16_t out_w,
                                  uint16_t out_h, FM_ScaleMode mode)
{
    (void)v; (void)pixels_out; (void)pixels_cap;
    (void)out_w; (void)out_h; (void)mode;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_Seek(FM_Video *v, uint32_t frame_index)
{
    (void)v; (void)frame_index;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_RegisterCodec(FM_Video *v, const FM_VideoCodecOps *ops)
{
    (void)v; (void)ops;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_ResetCodec(FM_Video *v)
{
    (void)v;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_HasAudio(const FM_Video *v, FM_Bool *has)
{
    (void)v;
    if (has != NULL)
    {
        *has = FM_FALSE;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_GetAudioInfo(const FM_Video *v, FM_AudioTrackInfo *info)
{
    (void)v; (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_ReadAudioFrame(FM_Video *v, uint8_t *buf, size_t cap,
                               size_t *size)
{
    (void)v; (void)buf; (void)cap;
    if (size != NULL)
    {
        *size = 0;
    }
    return FM_ERR_UNSUPPORTED;
}

FM_Result Video_SeekAudio(FM_Video *v, uint32_t sample_index)
{
    (void)v; (void)sample_index;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoWriter_Create(const char *path, uint16_t width,
                             uint16_t height, uint32_t fps,
                             uint32_t frame_count, uint32_t frame_size,
                             FM_VideoWriter *w)
{
    (void)path; (void)width; (void)height; (void)fps;
    (void)frame_count; (void)frame_size; (void)w;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoWriter_WriteFrame(FM_VideoWriter *w, const void *frame,
                                 size_t size)
{
    (void)w; (void)frame; (void)size;
    return FM_ERR_UNSUPPORTED;
}

FM_Result VideoWriter_Close(FM_VideoWriter *w)
{
    (void)w;
    return FM_ERR_UNSUPPORTED;
}

#endif /* FM_USE_VIDEO */
