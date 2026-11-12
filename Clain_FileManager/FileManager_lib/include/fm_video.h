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
    /* The exact frame rate, fps_num / fps_den pictures a second:
     * 30000/1001 for NTSC, 25/2 for 12.5.  `fps` above is this to the
     * nearest whole number and is what to print; time has to be worked
     * out from the fraction (picture n is shown at n * fps_den /
     * fps_num seconds), or a 29.97 fps film runs 1.8 s ahead of its
     * sound after half an hour.  Both are non-zero after Video_Open
     * (fps/1 when the file gives a whole rate or none). */
    uint32_t      fps_num;
    uint32_t      fps_den;
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
 *   FLAC block size               : one FLAC block is handed out whole
 *       or not at all.  A block is up to 4608 sample frames in an
 *       ordinary file (18432 bytes of stereo PCM; ffmpeg writes 4096 or
 *       4608): give `cap` at least that.  With less room than the block
 *       needs the call returns FM_ERR_TOO_LARGE, takes nothing and stays
 *       on that block, so the same call with a larger buffer continues
 *       without a gap.
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

/* ------------------------------------------------------------------ */
/* Saving video: Motion-JPEG in an AVI file (fm_video_enc.c)           */
/* ------------------------------------------------------------------ */
/*
 * The file is an AVI with "MJPG" pictures and, when asked for, a 16-bit
 * PCM sound track: PCs, phones and video editors play it, and so does
 * this library (Video_Open, SimpleVideo).  Every frame is a JPEG of its
 * own, which is what a microcontroller can encode; H.264 and the like
 * are not written (see the note at the top of fm_video_enc.c).
 *
 *     FM_VideoFileWriter w;
 *     FM_VideoSaveConfig c = {0};
 *     c.width = 480; c.height = 272; c.fps = 15;
 *     c.audio_rate = 16000; c.audio_channels = 1;      // or 0: no sound
 *     VideoFileWriter_Open(&w, "/video/cam.avi", &c);
 *     for (...) {
 *         VideoFileWriter_WriteFrame(&w, pixels, FM_PIXEL_RGB565);
 *         VideoFileWriter_WriteAudio(&w, pcm, frames_of_sound);
 *     }
 *     VideoFileWriter_Close(&w);       // required: writes index + lengths
 *
 * Give the sound that belongs to a picture right after that picture
 * (audio_rate / fps sample frames per picture); players then find both
 * close together in the file.
 *
 * Size: about width * height / 8 bytes a frame at quality 80 for camera
 * pictures (480x272: 15-20 KB), flat graphics far less.  A file stops
 * accepting data at FM_VIDEO_ENC_MAX_BYTES (just under 2 GB):
 * FM_ERR_TOO_LARGE; close it and start the next one.
 *
 * Errors: the first failed write stays the answer of every later call
 * and of Close.  Close still completes the file as far as it got.
 * Open on a writer that still has a file open is FM_ERR_BUSY (Close it
 * first): nothing is opened, the first file stays as it is.
 *
 * A recording that is never closed (reset, power loss, card pulled):
 * every FM_VIDEO_ENC_SYNC_BYTES (1 MB; 0 = off) the header gets the
 * counts so far and the file is synced, so the file on the card is a
 * valid AVI up to the last such point, only without its index (players
 * then read it in order).  "<name>.idx" is left beside it; call
 * VideoFile_Repair(name) after the restart: it gives the file its index
 * and exact length and removes "<name>.idx".
 */
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint32_t fps;             /* frames per second ...                 */
    uint32_t fps_den;         /* ... divided by this (0 or 1 = whole
                               * number; 30000 / 1001 = 29.97)         */
    uint8_t  quality;         /* JPEG 1..100; 0 = FM_VIDEO_ENC_QUALITY
                               * (80)                                  */
    uint8_t  chroma_full;     /* 0 = 4:2:0 (usual for video), 1 = 4:4:4
                               * (sharper coloured text, larger)       */
    uint32_t audio_rate;      /* 0 = no sound track                    */
    uint16_t audio_channels;  /* 1 or 2                                */
} FM_VideoSaveConfig;

typedef struct
{
    FM_File   file;
    FM_File   index;              /* "<name>.idx" while writing        */
    char      index_path[FM_PATH_MAX];
    FM_VideoSaveConfig config;
    uint8_t  *buf;
    uint8_t  *ibuf;
    uint32_t  buf_cap;
    uint32_t  buf_len;
    uint32_t  ibuf_len;
    uint64_t  buf_pos;
    uint64_t  movi_pos;
    uint64_t  audio_bytes;
    uint32_t  frames;             /* pictures written so far           */
    uint32_t  index_entries;
    uint32_t  largest_frame;
    uint32_t  largest_audio;
    uint64_t  last_at;            /* chunk of the last picture ...     */
    uint32_t  last_size;          /* ... and its size (0 = none yet)   */
    uint64_t  synced_pos;         /* file size at the last checkpoint  */
    FM_Result error;
    FM_Bool   opened;
    uint32_t  guard;              /* set by Open; tells an open writer
                                   * from one that was never used      */
} FM_VideoFileWriter;

FM_Result VideoFileWriter_Open(FM_VideoFileWriter *w, const char *path,
                               const FM_VideoSaveConfig *cfg);
/* One picture of config.width x config.height in any FM_PixelFormat
 * (RGB565 straight from a frame buffer, RGB888, ...).  Needs FM_USE_JPEG. */
FM_Result VideoFileWriter_WriteFrame(FM_VideoFileWriter *w,
                                     const void *pixels,
                                     FM_PixelFormat format);
/* The same for a 16-bit frame buffer whose pixels are stored high byte
 * first: the byte order SPI LCDs take, and the one the frame callback of
 * SimpleVideo delivers when its swap option is on.  Formats that are not
 * 16-bit are written as by VideoFileWriter_WriteFrame. */
FM_Result VideoFileWriter_WriteFrameSwap16(FM_VideoFileWriter *w,
                                           const void *pixels,
                                           FM_PixelFormat format);
/* One picture that is already a JPEG file (FF D8 ... FF D9) of the size
 * the writer was opened with: a camera in JPEG mode, the JPEG
 * peripheral.  Stored as it is; FM_ERR_INVALID_FORMAT when it does not
 * start like a JPEG. */
FM_Result VideoFileWriter_WriteJpeg(FM_VideoFileWriter *w, const void *jpeg,
                                    size_t size);
/* The last picture again, `count` times.  For live capture that could
 * not keep up: a video file has a fixed frame rate, so a picture that
 * was missed has to be filled in or the file plays too fast and runs
 * ahead of its sound.  The JPEG is copied, not encoded again (a read
 * back from the card and a write).  FM_ERR_NOT_READY before the first
 * picture. */
FM_Result VideoFileWriter_RepeatFrame(FM_VideoFileWriter *w, uint32_t count);
/* `frames` sample frames of interleaved 16-bit PCM for the sound track.
 * FM_ERR_UNSUPPORTED when the file was opened with audio_rate 0. */
FM_Result VideoFileWriter_WriteAudio(FM_VideoFileWriter *w,
                                     const int16_t *pcm, size_t frames);
FM_Result VideoFileWriter_Close(FM_VideoFileWriter *w);
/* Finish a recording that was not closed.  The sign of one is the file
 * "<path>.idx" beside the video.  Every chunk that is whole in the file
 * is kept, a chunk that was cut is dropped, then the index and the
 * lengths are written as Close would have and "<path>.idx" is removed.
 * `frames` (may be NULL) gets the number of pictures in the result.
 *   FM_OK                  repaired
 *   FM_ERR_NOT_FOUND       there is no "<path>.idx": nothing to repair
 *   FM_ERR_INVALID_FORMAT  the file was not made by this writer; both
 *                          files are left as they are
 * Never call it for a file that is being recorded right now. */
FM_Result VideoFile_Repair(const char *path, uint32_t *frames);

/*
 * Video_Convert: decode `src` (any video this build plays: AVI, MP4,
 * MKV, FLV, FMRV with a decoder for its pictures) and write it to `dst`
 * as MJPEG AVI, sound track included when it can be decoded.  Not tied
 * to playback: it runs as fast as the processor and the card allow and
 * returns when the file is complete.
 *
 *   width, height   size of the new video; 0, 0 = as the source
 *   process         called with every decoded frame (RGB888, top row
 *                   first) before it is encoded: draw on it, filter it
 *   audio_process   the same for the sound (interleaved 16-bit PCM)
 *   progress        called after every frame; return FM_FALSE to stop
 *                   (FM_ERR_CANCELLED, `dst` is removed)
 *
 * Memory (FM_Alloc, for the duration of the call): one RGB888 frame
 * (width * height * 3 bytes), what the picture decoder itself needs, the
 * writer's buffer and FM_VIDEO_CONVERT_AUDIO_BYTES for sound.
 * On any failure `dst` is removed.  cfg may be NULL (all defaults).
 * The frame rate written is the whole number Video_GetInfo reports
 * (29.97 becomes 30).
 */
typedef void (*FM_VideoFrameEditFn)(void *ctx, uint8_t *rgb888,
                                    uint16_t width, uint16_t height,
                                    uint32_t frame_index);
typedef FM_Bool (*FM_VideoProgressFn)(void *ctx, uint32_t frames_done,
                                      uint32_t frames_total);
/* The audio callback type of fm_audio.h, repeated so that this header
 * stands alone (C11 allows the identical typedef twice). */
typedef void (*FM_VideoAudioEditFn)(void *ctx, int16_t *pcm, size_t samples,
                                    uint32_t sample_rate,
                                    uint16_t channels);

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t  quality;
    uint8_t  chroma_full;
    FM_Bool  no_audio;            /* FM_TRUE: drop the sound track     */
    FM_VideoFrameEditFn process;
    void    *process_ctx;
    FM_VideoAudioEditFn audio_process;
    void    *audio_ctx;
    FM_VideoProgressFn progress;
    void    *progress_ctx;
} FM_VideoConvertConfig;

FM_Result Video_Convert(const char *src, const char *dst,
                        const FM_VideoConvertConfig *cfg,
                        uint32_t *frames_out);

FM_END_DECLS

#endif /* FM_VIDEO_H */
