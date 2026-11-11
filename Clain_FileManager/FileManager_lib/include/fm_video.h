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
 *   FLV   - Flash Video: H.264 / HEVC (classic and Enhanced-FLV tags,
 *           parameter sets prefixed like MP4), MP3 / AAC / PCM / G.711
 *           audio, onMetaData size and frame rate
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
    FM_VIDEO_CODEC_THEORA,    /* Theora (legacy OGG)                 */
    FM_VIDEO_CODEC_COUNT      /* number of codec ids (not a codec)   */
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
 *                    FM_ERR_NOT_READY = the payload was consumed but held
 *                    no picture (parameter sets / SEI only): not an
 *                    error, the previous picture stays valid.
 *   close()  - release decoder state.
 *
 * The bundled H.264 decoder writes the picture at the size the stream
 * itself declares (SPS + cropping rectangle); Video_DecodeFrame() copies
 * that into info.width/height after the first decoded frame, so read the
 * size from Video_GetInfo() AFTER decoding when it matters.
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
/* Embedded audio track: demuxed and decoded to 16-bit PCM             */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_VIDEO_AUDIO_NONE = 0,
    FM_VIDEO_AUDIO_AAC,           /* MP4 "mp4a" / AVI 0x00FF         */
    FM_VIDEO_AUDIO_PCM,           /* AVI, MKV, MP4, MOV, FLV: 8/16/24/
                                   * 32-bit integer, 32-bit float    */
    FM_VIDEO_AUDIO_MP3,           /* AVI 0x0055 / MP4 ".mp3" / MKV   */
    FM_VIDEO_AUDIO_FLAC,          /* MP4 "fLaC" / MKV A_FLAC         */
    FM_VIDEO_AUDIO_VORBIS,        /* MKV A_VORBIS / MP4 "mp4a" 0xDD  */
    FM_VIDEO_AUDIO_OPUS,          /* MP4 "Opus" / MKV A_OPUS         */
    FM_VIDEO_AUDIO_ALAW,          /* 0x0006 aLaw                     */
    FM_VIDEO_AUDIO_ULAW,          /* 0x0007 muLaw                    */
    FM_VIDEO_AUDIO_ADPCM          /* AVI, MKV: 0x0011 IMA / 0x0002 MS */
} FM_VideoAudioCodec;

typedef struct
{
    FM_VideoAudioCodec codec;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;     /* width of a sample IN THE FILE: PCM,
                                   * and FLAC when the container says it;
                                   * for the other codecs whatever the
                                   * container states (often 0 or 16).
                                   * What Video_ReadAudioFrame returns is
                                   * 16-bit whatever this says.       */
    uint32_t sample_count;        /* packets in the track (container
                                   * samples / chunks / blocks): the unit
                                   * of Video_SeekAudio, NOT sample
                                   * frames.                          */
    FM_Bool  decodable;           /* FM_TRUE: Video_ReadAudioFrame returns
                                   * playable 16-bit PCM for this track.
                                   * FM_FALSE: no decoder is compiled in /
                                   * registered, or the track is outside
                                   * what the decoder does (see
                                   * Video_ReadAudioFrame). */
} FM_AudioTrackInfo;

/* True if the container has an audio track. */
FM_Result Video_HasAudio(const FM_Video *v, FM_Bool *has);
FM_Result Video_GetAudioInfo(const FM_Video *v, FM_AudioTrackInfo *info);
/* Read the next piece of the audio track into buf as interleaved 16-bit
 * PCM (little-endian).  Every FM_OK comes with data (*size != 0, a whole
 * number of sample frames).
 *
 *   PCM                           : decoded here, always available.  In
 *       the file a sample is 8 bits (unsigned; signed in QuickTime), 16,
 *       24 or 32 bits or a 32-bit float, in either byte order; out comes
 *       16-bit (wider samples lose their low bits, floats are scaled and
 *       clipped).  The data is converted in the caller's buffer, so one
 *       call gives as many sample frames as `cap` holds in the FILE's
 *       format (24-bit input fills two thirds of the buffer), and `cap`
 *       must hold one such frame: channels * bytes per sample, twice
 *       that for 8-bit.  A packet with more is continued by the next
 *       call; small packets that follow each other in the file are read
 *       together.  Other widths (12, 20, 64 bits): decodable is
 *       FM_FALSE, see "no decoder" below.
 *   A-law, mu-law                 : decoded here, always available.
 *       `cap` must hold twice the packet.
 *   IMA ADPCM, Microsoft ADPCM    : decoded here, mono and stereo, whole
 *       blocks at a time.  `cap` must hold one block (4 * nBlockAlign
 *       bytes is always enough).  A chunk with more blocks than fit is
 *       continued by the next call.
 *   MP3                           : decoded here when FM_USE_MP3=1.
 *       The decoder state (about 16 KB, FM_Alloc) is taken on the first
 *       packet and kept until Video_Close.  One MP3 frame is up to 4608
 *       bytes of PCM: give `cap` at least that.  A packet that holds more
 *       frames than fit is continued by the next call; with less room
 *       than one frame the part that does not fit is lost.  A frame whose
 *       data began in a packet that was not read (the first frames after
 *       a seek, or after a damaged packet) is returned as silence of the
 *       same length, so the time line stays whole.  The decode itself
 *       needs about 18 KB of stack (see the README, "Stack").
 *   Vorbis                        : decoded here when FM_USE_VORBIS=1,
 *       from Matroska / WebM (A_VORBIS) and from an MP4 "mp4a" entry of
 *       object type 0xDD.  The stream decoder (fm_vorbis.h) is opened on
 *       the first packet and kept until Video_Close; its memory is what
 *       stb_vorbis needs for the stream's codebooks.  Any `cap` from 4
 *       bytes works: what does not fit is handed out by the next calls.
 *       The first packet of a stream, and the first two after a seek,
 *       have no output; the call passes over them.
 *   AAC, FLAC, Opus               : decoded by the decoder registered
 *       under "AAC" / "FLAC" / "OPUS" (Audio_RegisterDecoder;
 *       FileManager_Init registers the built-in ones that are compiled
 *       in).  The decoder is opened on the first packet and kept until
 *       Video_Close / Video_SeekAudio.  A decoder registered under
 *       "VORBIS" in place of the built-in one is used the same way.
 *       The built-in FLAC decoder does up to 24 bits a sample; a wider
 *       track (FLAC allows 32) has decodable == FM_FALSE unless the
 *       application registered its own "FLAC" decoder.  Opus has a
 *       decoder only in a build with libopus (FM_OPUS_USE_LIBOPUS=1;
 *       `make FM_USE_OPUS_LIB=1`); AAC has none built in.
 *
 * Encoder delay and padding.  MP3, Vorbis, Opus and AAC encoders put
 * frames in front of the sound (LAME 1105, libopus 312 at 48 kHz) and
 * pad the last packet.  Where the container records them they are
 * removed, so that the track has the length and the start the container
 * states:
 *   MP4 / MOV   the audio track's edit list, when it is one edit that
 *               starts within the first second: the frames before its
 *               start are dropped and nothing is returned past its
 *               length.
 *   MKV / WebM  CodecDelay (up to one second; not for Vorbis, whose
 *               decoder drops its own first packet) at the start, and
 *               DiscardPadding off the end of the block that carries it.
 *   AVI, FLV    record neither: the track comes as the decoder gives it
 *               (an MP3 track is 1105 frames late and a little longer
 *               than the picture).
 * After Video_SeekAudio(v, 0) the start is trimmed again.  After a seek
 * to any other packet nothing is dropped at the start, and the MP4
 * length is not applied (the frame position is no longer known): the
 * padding of the last packet is then returned.  AAC handed out as ADTS
 * (below) is never trimmed.
 *
 * When there is no decoder (decodable == FM_FALSE):
 *   AAC               -> the raw sample is returned prefixed with a 7-byte
 *                        ADTS header (*size includes it): a valid .aac
 *                        stream for extraction, NOT PCM.
 *   everything else   -> FM_ERR_UNSUPPORTED for every packet (nothing is
 *                        written), then FM_ERR_EOF.
 * FM_AudioTrackInfo.decodable tells up front: look at it before the read
 * loop.
 *
 * Results:
 *   FM_OK             *size bytes were written.
 *   FM_ERR_EOF        the track has ended.
 *   FM_ERR_CORRUPTED  a packet was refused.  It has been passed over: the
 *                     next call continues with the following one.
 *   FM_ERR_UNSUPPORTED  no decoder for this track.  The packet has been
 *                     passed over, so a loop that reads until FM_ERR_EOF
 *                     ends; it gets no data.  (Also: the file has no
 *                     audio track.)
 *   FM_ERR_TOO_LARGE  `cap` is too small for this track's smallest unit
 *                     (see above).  Nothing was consumed: call again
 *                     with a larger buffer.
 *   FM_ERR_NO_MEMORY, FM_ERR_IO  nothing was consumed; the call can be
 *                     repeated. */
FM_Result Video_ReadAudioFrame(FM_Video *v, uint8_t *buf, size_t cap,
                               size_t *size);
/* Continue reading at packet `sample_index` (0 .. sample_count - 1; an
 * index past the end is FM_ERR_INVALID_ARGUMENT).  The unit is the
 * container's packet, not a sample frame or a time.  A seek is a break
 * for the decoders: they start afresh, and what was left of a packet
 * delivered in parts is dropped.  See above for what MP3 and Vorbis
 * return right after it, and for the trimming. */
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
