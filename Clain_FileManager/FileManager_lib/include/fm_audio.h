/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_audio.h - Audio plugin (optional, self-contained)
 *
 * Reading is content-based, not extension-based: WAV/PCM and bundled MP3
 * are decoded to signed PCM16; AAC/ADTS is available through a registered
 * decoder. WAV writing is built in and compressed writers (such as MP3)
 * use the encoder registry below.
 */

#ifndef FM_AUDIO_H
#define FM_AUDIO_H

#include "fm_types.h"
#include "fm_file.h"

FM_BEGIN_DECLS

typedef struct
{
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t data_offset;      /* byte offset of PCM data in file  */
    uint64_t data_size;        /* PCM bytes                         */
    uint32_t format;           /* 1 = PCM                           */
    char     codec[8];         /* "WAV","MP3",...                   */
    uint16_t block_align;      /* WAV: bytes per block (ADPCM framing) */
    uint64_t total_frames;     /* length in sample frames; 0 = not known
                                * (AAC).  Exact for WAV, FLAC, Vorbis,
                                * Opus and MP3 with a Xing/Info header;
                                * for other MP3 an estimate from the
                                * bitrate of the first frame.           */
} FM_AudioInfo;

typedef void (*FM_AudioCallback)(const int16_t *samples,
                                 size_t sample_count, void *user_data);

typedef struct
{
    FM_File     file;
    FM_AudioInfo info;
    uint64_t    frame_index;   /* current sample frame              */
    FM_AudioCallback callback;
    void       *callback_data;
    void       *codec_ctx;     /* internal codec state (MP3)        */
} FM_Audio;

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */
FM_Result Audio_Open(const char *path, FM_Audio *a);
FM_Result Audio_Close(FM_Audio *a);
FM_Result Audio_GetInfo(const FM_Audio *a, FM_AudioInfo *info);

/* Read up to count samples (one sample = one int16 per channel).
 * *read receives the number of samples actually read. */
FM_Result Audio_Read(FM_Audio *a, int16_t *samples, size_t count,
                     size_t *read);
FM_Result Audio_ReadFrame(FM_Audio *a, int16_t *frame,
                          size_t *channels_read);
/* Read interleaved samples and downmix to mono (averages L/R for stereo
 * sources). `count` and `*read` are mono PCM16 sample counts. */
FM_Result Audio_ReadMono(FM_Audio *a, int16_t *samples, size_t count,
                         size_t *read);
/* Normalize either mono or stereo input to interleaved stereo PCM16.
 * `frame_count` is the output capacity in L/R frames; the output buffer
 * therefore needs 2 * frame_count int16_t values. `*frames_read` counts
 * complete stereo frames, not individual channel samples. Mono input is
 * duplicated to both L and R. */
FM_Result Audio_ReadStereo(FM_Audio *a, int16_t *samples,
                           size_t frame_count, size_t *frames_read);
FM_Result Audio_Seek(FM_Audio *a, double seconds);

/* ------------------------------------------------------------------ */
/* MP3 input pipeline diagnostics                                      */
/*                                                                     */
/* Counters accumulate per open stream since Audio_Open (or since the  */
/* last Audio_Mp3StatsReset).  A realtime player can bracket one DMA   */
/* refill with reset + snapshot to see how much of the refill budget   */
/* went into SD/flash read commands versus decoder work:               */
/*                                                                     */
/*   Audio_Mp3StatsReset(&audio);                                     */
/*   AudioDma_Poll(&player);          // one half-buffer refill   */
/*   Audio_Mp3Stats(&audio, &s);          // s.read_calls small?      */
/*                                                                     */
/* Every read_call is one separate SDMMC command; the command COUNT    */
/* (not the byte count) is what creates refill jitter on real cards.   */
/* Returns FM_ERR_INVALID_ARGUMENT when `a` is not an open MP3 stream. */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint32_t read_calls;      /* File_Read calls on the MP3 input buffer   */
    uint64_t read_bytes;      /* compressed bytes pulled from the file     */
    uint32_t frames;          /* MP3 frames decoded                        */
    uint32_t resync_bytes;    /* bytes skipped while resyncing (corrupt)   */
} FM_Mp3Stats;

FM_Result Audio_Mp3Stats(const FM_Audio *a, FM_Mp3Stats *stats);
FM_Result Audio_Mp3StatsReset(const FM_Audio *a);

/* Stream the whole file to a callback in chunks. */
FM_Result Audio_Stream(FM_Audio *a, FM_AudioCallback cb, void *user_data,
                       size_t chunk_samples);
FM_Result Audio_SetCallback(FM_Audio *a, FM_AudioCallback cb,
                            void *user_data);

/* ------------------------------------------------------------------ */
/* Pluggable audio decoders (hardware / external)                      */
/*                                                                     */
/* The bundled WAV and MP3 codecs are built-in.  AAC (ADTS) files are   */
/* detected and framed by the library, but decoding is delegated to a   */
/* registered decoder — e.g. a hardware AAC block, an OS codec API, or  */
/* a licensed software library.                                         */
/* ------------------------------------------------------------------ */
typedef struct FM_AudioDecoderOps
{
    const char *name;
    /* Create decoder state for the given stream info (sample rate etc.). */
    FM_Result (*init)(void **ctx, const FM_AudioInfo *info);
    /* Decode one complete compressed frame/packet into interleaved int16
     * samples.  Returns FM_OK with *out_samples set, or FM_ERR_EOF. */
    FM_Result (*decode)(void *ctx, const uint8_t *in, size_t in_size,
                        int16_t *out, size_t out_count, size_t *out_samples);
    void (*close)(void *ctx);
} FM_AudioDecoderOps;

/* Register a decoder for a codec tag (e.g. "AAC").  Replaces any previous. */
FM_Result Audio_RegisterDecoder(const char *codec,
                                const FM_AudioDecoderOps *ops);
FM_Result Audio_UnregisterDecoder(const char *codec);
const FM_AudioDecoderOps *Audio_GetDecoder(const char *codec);

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_File     file;
    FM_AudioInfo info;
    uint64_t    data_bytes;    /* PCM bytes written so far          */
} FM_AudioWriter;

FM_Result AudioWriter_Create(const char *path, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w);
FM_Result AudioWriter_Write(FM_AudioWriter *w, const int16_t *samples,
                            size_t count, size_t *written);
/* Patches the WAV header and closes the file. */
FM_Result AudioWriter_Close(FM_AudioWriter *w);

/* ------------------------------------------------------------------ */
/* Saving: PCM16 in, a finished audio file out                         */
/*                                                                     */
/*   format   file          built in                  size (16-bit)    */
/*   WAV      .wav          always                    1 : 1            */
/*   ADPCM    .wav (IMA)    always                    1 : 4, lossy     */
/*   FLAC     .flac         when FM_USE_FLAC is 1     about 1 : 2,     */
/*                                                    lossless         */
/*   MP3      .mp3          no - Audio_RegisterEncoder                 */
/*   VORBIS   .ogg          no - Audio_RegisterEncoder                 */
/*   OPUS     .opus         no - Audio_RegisterEncoder                 */
/*                                                                     */
/* Every file written here is read back by Audio_Open.                 */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_AUDIO_FILE_AUTO = 0,  /* select from the destination extension  */
    FM_AUDIO_FILE_WAV,       /* 16-bit PCM                              */
    FM_AUDIO_FILE_MP3,
    FM_AUDIO_FILE_ADPCM,     /* IMA ADPCM in a .wav: never chosen by
                              * AUTO, ask for it by name                */
    FM_AUDIO_FILE_FLAC,
    FM_AUDIO_FILE_VORBIS,
    FM_AUDIO_FILE_OPUS,
    FM_AUDIO_FILE_COUNT
} FM_AudioFileFormat;

/* Describes the PCM you hand in.  Clear the whole structure first
 * (memset) so that fields added later stay at their default. */
typedef struct
{
    uint32_t sample_rate;     /* of the PCM you hand in                 */
    uint16_t channels;        /* of the PCM you hand in: 1 or 2         */
    uint16_t bits_per_sample; /* 16                                     */
    uint32_t bitrate_kbps;    /* compressed plugin encoders, e.g. MP3   */
    uint16_t out_channels;    /* in the file: 1 = mix down to mono,
                               * 2 = stereo (mono is doubled),
                               * anything else = the same as `channels` */
} FM_AudioEncodeConfig;

/* An encoder.  The built-in ones use this table too; a plugin (a
 * software MP3 or Opus encoder, a hardware codec) is registered with
 * Audio_RegisterEncoder.  FileManager owns the destination FM_File; the
 * encoder owns only its state.
 *
 *   init    `config` describes what `write` will receive: channels and
 *           out_channels are both the channel count in the file.
 *   write   takes `frame_count` PCM16 frames (interleaved for stereo),
 *           writes encoded bytes to `file`, reports how many frames it
 *           used (all of them, or fewer to be called again).
 *   finish  flushes what is buffered and completes the header; may seek.
 *   close   frees the state.  Called exactly once after a successful
 *           init, also when writing failed. */
typedef struct FM_AudioEncoderOps
{
    const char *name;
    FM_Result (*init)(void **ctx, const FM_AudioEncodeConfig *config);
    FM_Result (*write)(void *ctx, FM_File *file, const int16_t *pcm,
                       size_t frame_count, size_t *frames_consumed);
    FM_Result (*finish)(void *ctx, FM_File *file);
    void      (*close)(void *ctx);
} FM_AudioEncoderOps;

typedef struct
{
    FM_AudioFileFormat format;
    FM_AudioEncodeConfig config;    /* as given to Open                  */
    FM_File file;
    const FM_AudioEncoderOps *encoder;
    void *encoder_ctx;
    uint16_t out_channels;          /* channels in the file              */
    uint64_t frames;                /* frames accepted so far            */
    FM_Result error;                /* first failure; the writer then
                                     * refuses more data                 */
    FM_Bool opened;
    uint32_t guard;                 /* set by Open; tells an open writer
                                     * from one that was never used      */
} FM_AudioFileWriter;

/* Register an encoder for a format (not for WAV).  It replaces the
 * built-in one of that format, if there is one. */
FM_Result Audio_RegisterEncoder(FM_AudioFileFormat format,
                                const FM_AudioEncoderOps *ops);
FM_Result Audio_UnregisterEncoder(FM_AudioFileFormat format);
/* The encoder that would be used: registered, else built in, else NULL. */
const FM_AudioEncoderOps *Audio_GetEncoder(FM_AudioFileFormat format);
FM_Bool Audio_CanEncode(FM_AudioFileFormat format);

/* .wav/.wave, .flac, .mp3, .ogg/.oga, .opus (any letter case); AUTO when
 * the name has none of them. */
FM_AudioFileFormat Audio_FormatFromPath(const char *path);
/* ".wav", ".flac", ... ; "" for AUTO. */
const char *Audio_FormatExtension(FM_AudioFileFormat format);

/* Open: with AUTO the format comes from the extension.  A format with
 * no encoder gives FM_ERR_UNSUPPORTED and creates nothing (no WAV bytes
 * under an .mp3 name).  The folder must exist.
 * Open on a writer that still has a file open is FM_ERR_BUSY (Close it
 * first): nothing is opened, the first file stays as it is.
 * WriteFrames: `frame_count` frames in the layout given to Open.
 * Close: completes the header and closes the file; returns the first
 * error of the whole recording, and always releases the writer.
 *
 * When a write failed (card full, I/O error) the file is not thrown
 * away: Close sets the lengths in a WAV / ADPCM header to what reached
 * the card, so that part opens and plays; the error is still what Close
 * returns.  A FLAC file needs no fixing (a reader stops at the frame
 * that was cut).
 *
 * A recording that is never closed (reset, power loss, card pulled):
 * every FM_AUDIO_ENC_SYNC_BYTES (256 KB; 0 = off) the lengths are
 * brought up to date and the file is synced, so it plays up to the last
 * such point.  Less than that many bytes on the card = an empty file.
 * In time: 1.5 s of 44.1 kHz stereo WAV, 6 s of 44.1 kHz stereo ADPCM,
 * 64 s of 8 kHz mono ADPCM -- choose a smaller value for low rates. */
FM_Result AudioFileWriter_Open(FM_AudioFileWriter *writer, const char *path,
                               FM_AudioFileFormat format,
                               const FM_AudioEncodeConfig *config);
FM_Result AudioFileWriter_WriteFrames(FM_AudioFileWriter *writer,
                                      const int16_t *pcm,
                                      size_t frame_count,
                                      size_t *frames_written);
FM_Result AudioFileWriter_Close(FM_AudioFileWriter *writer);

/* ------------------------------------------------------------------ */
/* File to file: read -> your processing -> write, nothing is played   */
/* ------------------------------------------------------------------ */
/* The one PCM callback type: of both players and of Audio_Convert.
 *   pcm       signed 16-bit samples, interleaved (L,R,L,R...); writable
 *   samples   number of int16_t values (a stereo frame is two)
 * In a player it runs on the buffer-refill path: copy or process and
 * return; no file I/O and no printf in there. */
typedef void (*FM_AudioDataFn)(void *ctx, int16_t *pcm, size_t samples,
                               uint32_t sample_rate, uint16_t channels);
/* Called after every block; return FM_FALSE to cancel (the unfinished
 * destination is deleted).  total_frames is 0 when the length of the
 * source is not known. */
typedef FM_Bool (*FM_AudioProgressFn)(void *ctx, uint64_t done_frames,
                                      uint64_t total_frames);

typedef struct
{
    FM_AudioFileFormat format;      /* AUTO: from the extension of dst   */
    uint16_t out_channels;          /* 0 = as the source, 1 mono, 2 stereo */
    uint32_t bitrate_kbps;          /* plugin encoders                   */
    FM_AudioDataFn process;         /* your DSP, in place; may be NULL   */
    void *process_ctx;
    FM_AudioProgressFn progress;    /* may be NULL                       */
    void *progress_ctx;
} FM_AudioConvertConfig;

/* Decode `src` (anything Audio_Open reads), pass every block through
 * config->process, and encode the result into `dst`.  Runs as fast as
 * the processor and the card allow and returns when the file is done.
 * `config` may be NULL: a plain conversion chosen by the extension of
 * dst.  `frames_out` (may be NULL) receives the frames written.  On any
 * failure dst is removed. */
FM_Result Audio_Convert(const char *src, const char *dst,
                        const FM_AudioConvertConfig *config,
                        uint64_t *frames_out);

/* ------------------------------------------------------------------ */
/* Recorder: write audio into a folder                                */
/* ------------------------------------------------------------------ */
/* Open a new 16-bit/8-bit WAV in `folder` with an auto-generated name
 * (rec_000001.wav, ...).  The chosen path is returned in path_out.  Write
 * with AudioWriter_Write() and finish with AudioWriter_Close(). */
FM_Result AudioRecorder_Open(const char *folder, uint32_t sample_rate,
                             uint16_t channels, uint16_t bits_per_sample,
                             FM_AudioWriter *w, char *path_out,
                             size_t path_out_size);

FM_END_DECLS

#endif /* FM_AUDIO_H */
