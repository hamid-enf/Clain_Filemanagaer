/*
 * FileManager - Video plugin (optional, self-contained)
 *
 * The video subsystem is codec-plugin based: a container parser extracts
 * frame payloads (compressed or raw) and hands them to a codec plugin for
 * decoding.  No codec is claimed to decode on every MCU.
 *
 * Implemented containers:
 *   FMRV  - built-in raw video container (uncompressed frames)
 *   AVI   - RIFF container (MJPEG, H.264, MPEG-4, VP8/VP9)
 *   MP4/MOV - ISO BMFF container (H.264/HEVC/VP9/AV1 + AAC/MP3/FLAC/Opus)
 *   MKV/WebM - EBML container (H.264/HEVC/VP8/VP9/AV1 + Vorbis/Opus/FLAC)
 *   FLV   - Flash Video (H.264/AAC) stub, frame extraction only
 *
 * Implemented codec plugins:
 *   RAW   - built-in (frame bytes are already uncompressed)
 *   MJPEG - picojpeg / HW JPEG
 *   H264  - h264bsd-backed software decoder (see fm_h264.h)
 *   MPEG4/HEVC/VP8/VP9/AV1 - via HWDecoder_Register (host ffmpeg / HW)
 *
 * AVI/MP4/H.264 can be detected by the media layer and decoded through the
 * FM_VideoCodecOps interface when a codec plugin is registered.
 */

#ifndef FM_VIDEO_H
#define FM_VIDEO_H

#include "fm_types.h"
#include "fm_file.h"
#include "fm_stream.h"
#include "fm_image.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_VIDEO_CODEC_UNKNOWN = 0,
    FM_VIDEO_CODEC_RAW,       /* uncompressed frames                 */
    FM_VIDEO_CODEC_MJPEG,     /* JPEG-compressed frames (container)  */
    FM_VIDEO_CODEC_H264,      /* decode via plugin (fm_h264.h)       */
    FM_VIDEO_CODEC_MPEG4,     /* MPEG-4 Part 2 (XVID/DIVX)           */
    FM_VIDEO_CODEC_MPEG2,     /* MPEG-2 video                        */
    FM_VIDEO_CODEC_HEVC,      /* H.265 / HEVC (hvc1/hev1)            */
    FM_VIDEO_CODEC_VP8,       /* VP8  (VP80 / MKV)                   */
    FM_VIDEO_CODEC_VP9,       /* VP9  (VP90 / MKV/WebM)              */
    FM_VIDEO_CODEC_AV1,       /* AV1  (AV01 / MKV/WebM)              */
    FM_VIDEO_CODEC_THEORA     /* Theora (legacy OGG)                 */
} FM_VideoCodec;

typedef struct
{
    uint16_t      width;
    uint16_t      height;
    uint32_t      fps;
    uint32_t      frame_count;
    uint64_t      data_offset;      /* first frame offset         */
    uint32_t      frame_size;       /* fixed frame size (0=var)   */
    FM_VideoCodec codec;
    char          container[8];     /* "FMRV","AVI","MP4"         */
} FM_VideoInfo;

/* Codec plugin interface (stateful; one context per open video).
 *
 *   init()   - allocate/reset decoder state for the given stream info.
 *   decode_frame() - decode one frame payload into RGB888 pixels
 *                    (frame_cap must be >= width*height*3).
 *   close()  - release decoder state.
 */
typedef struct FM_VideoCodecOps
{
    const char *name;
    FM_Result (*init)(void **ctx, const FM_VideoInfo *info);
    FM_Result (*decode_frame)(void *ctx, const void *compressed, size_t size,
                              void *frame_out, size_t frame_cap);
    void      (*close)(void *ctx);
} FM_VideoCodecOps;

typedef struct
{
    FM_File   file;
    FM_VideoInfo info;
    uint32_t  current_frame;
    uint32_t  variable;          /* variable frame size mode        */
    const FM_VideoCodecOps *codec_plugin;
    void     *codec_ctx;         /* plugin decoder state            */
    void     *priv;              /* container parser private state  */
    uint32_t  audio_frame_index; /* next audio sample to read       */
} FM_Video;

/* ------------------------------------------------------------------ */
/* Embedded audio track (demuxed, not decoded)                         */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_VIDEO_AUDIO_NONE = 0,
    FM_VIDEO_AUDIO_AAC,           /* MP4 "mp4a" / AVI 0x00FF         */
    FM_VIDEO_AUDIO_PCM,           /* AVI "01wb" (8/16/24-bit PCM)    */
    FM_VIDEO_AUDIO_MP3,           /* AVI 0x0055 / MP4 ".mp3"         */
    FM_VIDEO_AUDIO_FLAC,          /* MP4 "fLaC" / MKV A_FLAC         */
    FM_VIDEO_AUDIO_VORBIS,        /* MP4 "vorb" / MKV A_VORBIS       */
    FM_VIDEO_AUDIO_OPUS,          /* MP4 "Opus" / MKV A_OPUS         */
    FM_VIDEO_AUDIO_ALAW,          /* AVI 0x0006 aLaw                 */
    FM_VIDEO_AUDIO_ULAW,          /* AVI 0x0007 muLaw                */
    FM_VIDEO_AUDIO_ADPCM          /* AVI 0x0011 IMA / 0x0002 MS      */
} FM_VideoAudioCodec;

typedef struct
{
    FM_VideoAudioCodec codec;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;     /* PCM only (0 for AAC)            */
    uint32_t sample_count;        /* number of audio samples/frames  */
    FM_Bool  decodable;           /* FM_TRUE: Video_ReadAudioFrame returns
                                   * playable 16-bit PCM for this track.
                                   * FM_FALSE: no decoder is compiled in /
                                   * registered (see Video_ReadAudioFrame). */
} FM_AudioTrackInfo;

/* True if the container has an audio track. */
FM_Result Video_HasAudio(const FM_Video *v, FM_Bool *has);
FM_Result Video_GetAudioInfo(const FM_Video *v, FM_AudioTrackInfo *info);
/* Read the next audio packet into buf as interleaved 16-bit PCM.
 *
 *   PCM (8/16-bit), A-law, mu-law, ADPCM : decoded here, always available.
 *   MP3                                  : decoded here when FM_USE_MP3=1.
 *   AAC, FLAC, Opus, Vorbis              : decoded by the decoder registered
 *       under "AAC" / "FLAC" / "OPUS" / "VORBIS" (Audio_RegisterDecoder;
 *       FileManager_Init registers the built-in ones that are compiled in).
 *       The decoder is opened on the first packet and kept until
 *       Video_Close / Video_SeekAudio.
 *
 * When no decoder is registered:
 *   AAC               -> the raw sample is returned prefixed with a 7-byte
 *                        ADTS header (*size includes it): a valid .aac
 *                        stream for extraction, NOT PCM.
 *   FLAC/Opus/Vorbis  -> FM_ERR_UNSUPPORTED (nothing is written).
 * FM_AudioTrackInfo.decodable tells the two situations apart up front.
 *
 * A packet the decoder rejects returns FM_ERR_CORRUPTED and is skipped, so
 * the next call continues with the following packet. */
FM_Result Video_ReadAudioFrame(FM_Video *v, uint8_t *buf, size_t cap,
                               size_t *size);
FM_Result Video_SeekAudio(FM_Video *v, uint32_t sample_index);

/* ------------------------------------------------------------------ */
FM_Result Video_Open(const char *path, FM_Video *v);
FM_Result Video_Close(FM_Video *v);
FM_Result Video_GetInfo(const FM_Video *v, FM_VideoInfo *info);
/* Read the next frame blob (compressed or raw) into frame_out.
 * *size receives the frame byte size.  For MP4/avc1 the returned blob is
 * an Annex-B byte stream (start-code delimited NAL units). */
FM_Result Video_ReadFrame(FM_Video *v, void *frame_out, size_t frame_cap,
                          size_t *size);
/* Decode the current frame with the registered codec plugin. */
FM_Result Video_DecodeFrame(FM_Video *v, void *pixels_out,
                            size_t pixels_cap);
/* Decode one frame and convert it to a caller-selected raw pixel format.
 * Codec plugins decode natively to RGB888. For non-RGB888 output the caller
 * supplies an additional width*height*3 RGB work buffer; no hidden frame
 * allocation is performed. */
FM_Result Video_DecodeFrameAs(FM_Video *v, FM_PixelFormat output_format,
                              void *pixels_out, size_t pixels_cap,
                              void *rgb_work, size_t rgb_work_cap);
/* Decode and scale in one step (e.g. playback at a smaller display size).
 * out_w/out_h are the target dimensions (0 = native size).  Requires
 * FM_USE_IMAGE for the scaler.  Uses the fm_alloc.h hook for a scratch
 * native-size frame buffer. */
FM_Result Video_DecodeFrameScaled(FM_Video *v, void *pixels_out,
                                  size_t pixels_cap, uint16_t out_w,
                                  uint16_t out_h, FM_ScaleMode mode);
FM_Result Video_Seek(FM_Video *v, uint32_t frame_index);

/* Attach a codec plugin (e.g. H264_GetCodecOps()). */
FM_Result Video_RegisterCodec(FM_Video *v, const FM_VideoCodecOps *ops);

/* Re-initialise the attached codec plugin (close + init with the same
 * ops).  Use after a discontinuous jump (Video_Seek) on stateful codecs
 * such as H.264 so the decoder does not try to reference pictures that
 * were never decoded.  Returns FM_OK when no plugin is attached. */
FM_Result Video_ResetCodec(FM_Video *v);

/* ------------------------------------------------------------------ */
/* Writer (built-in FMRV raw container)                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_File file;
    FM_VideoInfo info;
    uint32_t frames_written;
} FM_VideoWriter;

FM_Result VideoWriter_Create(const char *path, uint16_t width,
                             uint16_t height, uint32_t fps,
                             uint32_t frame_count, uint32_t frame_size,
                             FM_VideoWriter *w);
FM_Result VideoWriter_WriteFrame(FM_VideoWriter *w, const void *frame,
                                 size_t size);
FM_Result VideoWriter_Close(FM_VideoWriter *w);

FM_END_DECLS

#endif /* FM_VIDEO_H */
