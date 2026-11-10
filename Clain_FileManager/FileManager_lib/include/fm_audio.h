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
/*   Ex13_AudioDmaPoll(&player);          // one half-buffer refill   */
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
/* Format-dispatching PCM16 file writer                                */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_AUDIO_FILE_AUTO = 0,  /* select from destination extension */
    FM_AUDIO_FILE_WAV,
    FM_AUDIO_FILE_MP3
} FM_AudioFileFormat;

typedef struct
{
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample; /* currently PCM16 input */
    uint32_t bitrate_kbps;    /* compressed encoders, e.g. MP3 */
} FM_AudioEncodeConfig;

/* Compressed encoders are optional plugins. FileManager owns the destination
 * FM_File and all file I/O; the plugin owns only its codec state. `write`
 * consumes PCM16 frames (interleaved for stereo) and writes encoded bytes to
 * `file`. This keeps format dispatch inside FileManager while allowing a
 * software or hardware MP3 encoder to be selected per target. */
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
    FM_AudioEncodeConfig config;
    FM_AudioWriter wav;
    FM_File encoded_file;
    const FM_AudioEncoderOps *encoder;
    void *encoder_ctx;
    FM_Bool opened;
} FM_AudioFileWriter;

FM_Result Audio_RegisterEncoder(FM_AudioFileFormat format,
                                const FM_AudioEncoderOps *ops);
FM_Result Audio_UnregisterEncoder(FM_AudioFileFormat format);
const FM_AudioEncoderOps *Audio_GetEncoder(FM_AudioFileFormat format);

/* AUTO uses .wav/.wave or .mp3 (case-insensitive). WAV is built in. MP3
 * requires one registered MP3 encoder; otherwise Open returns
 * FM_ERR_UNSUPPORTED rather than writing fake WAV bytes into an .mp3 file. */
FM_Result AudioFileWriter_Open(FM_AudioFileWriter *writer, const char *path,
                               FM_AudioFileFormat format,
                               const FM_AudioEncodeConfig *config);
FM_Result AudioFileWriter_WriteFrames(FM_AudioFileWriter *writer,
                                      const int16_t *pcm,
                                      size_t frame_count,
                                      size_t *frames_written);
FM_Result AudioFileWriter_Close(FM_AudioFileWriter *writer);

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
