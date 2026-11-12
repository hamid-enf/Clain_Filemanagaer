/*
 * media_data_io.h — Caller-owned Audio / Video / Image data
 *
 * این فایل یک لایه‌ی نمونه و header-only روی API اصلی است تا داده‌ی decodeشده
 * مستقیماً داخل بافر شما قرار بگیرد. هیچ بافر تصویر/صدا/ویدئو داخل این wrapper
 * allocate نمی‌شود؛ اندازه و محل RAM/SDRAM را caller تعیین می‌کند.
 *
 * --------------------------------------------------------------------------
 * AUDIO — PCM16 استاندارد
 * --------------------------------------------------------------------------
 *
 *   static MediaIO_AudioReader audio;
 *   static int16_t mono[512];         // 512 نمونه‌ی mono
 *   static int16_t stereo[512 * 2];   // 512 فریم L,R
 *   size_t count;
 *
 *   MediaIO_AudioOpen(&audio, "/music/song.mp3");
 *
 *   // خروجی mono: هر عنصر یک نمونه‌ی PCM16 استریو را downmix می‌کند.
 *   MediaIO_AudioReadMono(&audio, mono, 512, &count);
 *   // mono[0..count-1] اکنون برای DSP شما آماده است.
 *
 *   MediaIO_AudioSeek(&audio, 0.0);
 *   // خروجی stereo: L,R,L,R...؛ count تعداد frame است نه int16_t.
 *   MediaIO_AudioReadStereo(&audio, stereo, 512, &count);
 *   // L = stereo[2*i], R = stereo[2*i+1]
 *
 *   // پسوند مقصد writer را انتخاب می‌کند: WAV داخلی است؛ MP3 باید encoder
 *   // ثبت‌شده داشته باشد وگرنه خطای صریح FM_ERR_UNSUPPORTED برمی‌گردد.
 *   MediaIO_AudioSavePCM16("/processed.wav", stereo, count,
 *                       audio.info.sample_rate, 2);
 *   MediaIO_AudioClose(&audio);
 *
 * برای ضبط/ذخیره‌ی پیوسته و انتخاب خودکار .wav/.mp3 از این سه تابع استفاده
 * کنید: MediaIO_AudioFileWriterBegin، MediaIO_AudioFileWriterWriteFrames و
 * MediaIO_AudioFileWriterEnd. خواندن WAV/MP3 بر اساس محتوای فایل است، نه پسوند.
 * اگر داده از قبل یک فایل MP3/WAV کامل و encodeشده است، دوباره encode نکنید؛
 * Media_SaveEncoded نوع magic را validate و همان bytes را ذخیره می‌کند.
 *
 * --------------------------------------------------------------------------
 * VIDEO — هر فریم در layout انتخابی caller
 * --------------------------------------------------------------------------
 *
 *   static MediaIO_VideoReader video;
 *   static uint8_t rgb_work[320 * 240 * 3];
 *   static uint8_t bgr565[320 * 240 * 2];
 *
 *   MediaIO_VideoOpen(&video, "/clip.avi"); // FMRV/AVI/MP4 از محتوا تشخیص داده می‌شود
 *   MediaIO_VideoReadAs(&video, FM_PIXEL_BGR565,
 *                    bgr565, sizeof(bgr565),
 *                    rgb_work, sizeof(rgb_work));
 *   MediaIO_VideoClose(&video);
 *
 * ذخیره‌ی ویدئو در کتابخانه فعلاً FMRV خام است (نه encoder مربوط به AVI/MP4):
 * MediaIO_VideoWriterBeginRGB888 -> MediaIO_VideoWriterWriteRGB888 ->
 * MediaIO_VideoWriterEnd.
 *
 * --------------------------------------------------------------------------
 * IMAGE — BMP به RGB888، resize و effect
 * --------------------------------------------------------------------------
 *
 *   static uint8_t decode_work[320 * 240];  // فقط برای BMP خاکستری
 *   static uint8_t rgb[320 * 240 * 3];
 *   FM_Image image;
 *
 *   MediaIO_ImageLoadRGB888("/photo.bmp", &image,
 *                        decode_work, sizeof(decode_work),
 *                        rgb, sizeof(rgb));
 *   Image_ApplyEffect(&image, FM_IMAGE_EFFECT_SEPIA, 0);
 *   MediaIO_ImageSaveRGB888("/photo_sepia.bmp", image.data,
 *                        image.width, image.height);
 *
 * افکت‌ها: INVERT، GRAYSCALE، SEPIA، BRIGHTNESS (-255..255)،
 * CONTRAST (-100..100). layoutهای خام: GRAY8، RGB565، BGR565، RGB888،
 * BGR888، RGBA8888 و BGRA8888. فرمت codec تصویری built-in فعلاً BMP است.
 */

#ifndef FM_TOPAPI_MEDIA_DATA_IO_H
#define FM_TOPAPI_MEDIA_DATA_IO_H


#ifndef MEDIA_DATA_IO_H_
#define MEDIA_DATA_IO_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "file_manager.h"
#include "fm_audio.h"
#include "fm_video.h"
#include "fm_image.h"

/* ========================================================================== */
/* Audio                                                                       */
/* ========================================================================== */

typedef struct
{
    FM_Audio handle;
    FM_AudioInfo info;
    FM_Bool opened;
} MediaIO_AudioReader;

static inline FM_Result MediaIO_AudioOpen(MediaIO_AudioReader *reader,
                                       const char *path)
{
    FM_Result r;

    if (reader == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(reader, 0, sizeof(*reader));
    r = Audio_Open(path, &reader->handle);
    if (r != FM_OK)
    {
        return r;
    }
    r = Audio_GetInfo(&reader->handle, &reader->info);
    if (r != FM_OK)
    {
        (void)Audio_Close(&reader->handle);
        return r;
    }
    reader->opened = FM_TRUE;
    return FM_OK;
}

/* Native interleaved PCM16. `capacity` and `sample_count` count individual
 * int16_t channel samples. Use reader.info.channels to interpret the data. */
static inline FM_Result MediaIO_AudioReadNative(MediaIO_AudioReader *reader,
                                              int16_t *pcm,
                                              size_t capacity,
                                              size_t *sample_count)
{
    if (reader == NULL || !reader->opened || pcm == NULL || capacity == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Audio_Read(&reader->handle, pcm, capacity, sample_count);
}

/* Normalized mono PCM16: one output value per sample time. */
static inline FM_Result MediaIO_AudioReadMono(MediaIO_AudioReader *reader,
                                            int16_t *mono,
                                            size_t capacity,
                                            size_t *sample_count)
{
    if (reader == NULL || !reader->opened || mono == NULL || capacity == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Audio_ReadMono(&reader->handle, mono, capacity, sample_count);
}

/* Normalized stereo PCM16: output is L,R,L,R... and capacity counts frames.
 * The destination needs `frame_capacity * 2` int16_t values. */
static inline FM_Result MediaIO_AudioReadStereo(MediaIO_AudioReader *reader,
                                              int16_t *stereo,
                                              size_t frame_capacity,
                                              size_t *frames_read)
{
    if (reader == NULL || !reader->opened)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Audio_ReadStereo(&reader->handle, stereo, frame_capacity,
                            frames_read);
}

static inline FM_Result MediaIO_AudioSeek(MediaIO_AudioReader *reader,
                                       double seconds)
{
    if (reader == NULL || !reader->opened)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Audio_Seek(&reader->handle, seconds);
}

static inline FM_Result MediaIO_AudioClose(MediaIO_AudioReader *reader)
{
    FM_Result r;

    if (reader == NULL || !reader->opened)
    {
        return FM_ERR_NOT_OPEN;
    }
    r = Audio_Close(&reader->handle);
    reader->opened = FM_FALSE;
    return r;
}

/* Streaming WAV writer: counts are PCM frames, not individual channel values. */
static inline FM_Result MediaIO_AudioWriterBegin(FM_AudioWriter *writer,
                                               const char *path,
                                               uint32_t sample_rate,
                                               uint16_t channels)
{
    return AudioWriter_Create(path, sample_rate, channels, 16, writer);
}

static inline FM_Result MediaIO_AudioWriterWriteFrames(FM_AudioWriter *writer,
                                                     const int16_t *pcm,
                                                     size_t frame_count,
                                                     size_t *frames_written)
{
    size_t samples;
    size_t written = 0;
    FM_Result r;

    if (writer == NULL || pcm == NULL || writer->info.channels == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > SIZE_MAX / writer->info.channels)
    {
        return FM_ERR_TOO_LARGE;
    }
    samples = frame_count * writer->info.channels;
    r = AudioWriter_Write(writer, pcm, samples, &written);
    if (frames_written != NULL)
    {
        *frames_written = written / writer->info.channels;
    }
    if (r == FM_OK && written != samples)
    {
        return FM_ERR_IO;
    }
    return r;
}

static inline FM_Result MediaIO_AudioWriterEnd(FM_AudioWriter *writer)
{
    return AudioWriter_Close(writer);
}

/* Generic streaming writer. AUTO selects WAV or MP3 from the path extension.
 * WAV is built-in. MP3 uses the FM_AudioEncoderOps registered for
 * FM_AUDIO_FILE_MP3; unsupported output is reported, never mislabeled. */
static inline FM_Result MediaIO_AudioFileWriterBegin(
    FM_AudioFileWriter *writer, const char *path, uint32_t sample_rate,
    uint16_t channels, uint32_t bitrate_kbps)
{
    FM_AudioEncodeConfig config;

    memset(&config, 0, sizeof(config));
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.bits_per_sample = 16;
    config.bitrate_kbps = bitrate_kbps;
    return AudioFileWriter_Open(writer, path, FM_AUDIO_FILE_AUTO, &config);
}

static inline FM_Result MediaIO_AudioFileWriterWriteFrames(
    FM_AudioFileWriter *writer, const int16_t *pcm, size_t frame_count,
    size_t *frames_written)
{
    return AudioFileWriter_WriteFrames(writer, pcm, frame_count,
                                       frames_written);
}

static inline FM_Result MediaIO_AudioFileWriterEnd(FM_AudioFileWriter *writer)
{
    return AudioFileWriter_Close(writer);
}

/* One-shot save for a caller-owned PCM16 buffer. The extension selects the
 * actual encoder. For large/live recordings use the generic streaming writer
 * above so the complete sound need not fit in RAM. */
static inline FM_Result MediaIO_AudioSavePCM16(const char *path,
                                             const int16_t *pcm,
                                             size_t frame_count,
                                             uint32_t sample_rate,
                                             uint16_t channels)
{
    FM_AudioFileWriter writer;
    size_t total = 0;
    FM_Result r;

    if (path == NULL || pcm == NULL || frame_count == 0 ||
        (channels != 1 && channels != 2))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&writer, 0, sizeof(writer));
    r = MediaIO_AudioFileWriterBegin(&writer, path, sample_rate, channels, 128);
    if (r != FM_OK) return r;

    while (total < frame_count)
    {
        size_t written = 0;

        r = MediaIO_AudioFileWriterWriteFrames(&writer,
                                             pcm + total * channels,
                                             frame_count - total, &written);
        total += written;
        if (r != FM_OK || written == 0)
        {
            (void)MediaIO_AudioFileWriterEnd(&writer);
            return (r != FM_OK) ? r : FM_ERR_IO;
        }
    }
    return MediaIO_AudioFileWriterEnd(&writer);
}

/* ========================================================================== */
/* Video                                                                       */
/* ========================================================================== */

typedef struct
{
    FM_Video handle;
    FM_VideoInfo info;
    FM_Bool opened;
} MediaIO_VideoReader;

static inline FM_Result MediaIO_VideoOpen(MediaIO_VideoReader *reader,
                                       const char *path)
{
    FM_Result r;

    if (reader == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(reader, 0, sizeof(*reader));
    r = Video_Open(path, &reader->handle);
    if (r != FM_OK) return r;

    r = Video_GetInfo(&reader->handle, &reader->info);
    if (r != FM_OK)
    {
        (void)Video_Close(&reader->handle);
        return r;
    }
    reader->opened = FM_TRUE;
    return FM_OK;
}

static inline size_t MediaIO_VideoFrameBytes(const MediaIO_VideoReader *reader,
                                          FM_PixelFormat format)
{
    size_t bytes = 0;

    if (reader == NULL ||
        Image_BufferSize(reader->info.width, reader->info.height,
                         format, &bytes) != FM_OK)
    {
        return 0;
    }
    return bytes;
}

static inline size_t MediaIO_VideoRGB888FrameBytes(
    const MediaIO_VideoReader *reader)
{
    return MediaIO_VideoFrameBytes(reader, FM_PIXEL_RGB888);
}

/* Decode exactly one frame to a caller-selected raw layout. For RGB888,
 * rgb_work may be NULL. Other layouts need a separate width*height*3 work
 * buffer because bundled decoders produce RGB888 before conversion. */
static inline FM_Result MediaIO_VideoReadAs(
    MediaIO_VideoReader *reader, FM_PixelFormat format,
    void *pixels, size_t capacity, void *rgb_work, size_t rgb_work_capacity)
{
    if (reader == NULL || !reader->opened)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Video_DecodeFrameAs(&reader->handle, format, pixels, capacity,
                               rgb_work, rgb_work_capacity);
}

/* Decode exactly one frame to top-left-to-bottom-right RGB888. */
static inline FM_Result MediaIO_VideoReadRGB888(MediaIO_VideoReader *reader,
                                              uint8_t *rgb,
                                              size_t capacity)
{
    size_t needed;

    if (reader == NULL || !reader->opened || rgb == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    needed = MediaIO_VideoRGB888FrameBytes(reader);
    if (needed == 0 || capacity < needed)
    {
        return FM_ERR_TOO_LARGE;
    }
    return Video_DecodeFrame(&reader->handle, rgb, capacity);
}

/* Decode into caller-owned native_rgb and then resize into scaled_rgb. This
 * variant performs no hidden allocation. */
static inline FM_Result MediaIO_VideoReadRGB888Scaled(
    MediaIO_VideoReader *reader,
    uint8_t *native_rgb, size_t native_capacity,
    uint8_t *scaled_rgb, size_t scaled_capacity,
    uint16_t out_width, uint16_t out_height, FM_ScaleMode mode)
{
    uint64_t output_bytes;
    size_t output_size;
    FM_Result r;

    if (reader == NULL || native_rgb == NULL || scaled_rgb == NULL ||
        out_width == 0 || out_height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    output_bytes = (uint64_t)out_width * out_height * 3u;
    if (output_bytes > SIZE_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    output_size = (size_t)output_bytes;
    if (scaled_capacity < output_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    r = MediaIO_VideoReadRGB888(reader, native_rgb, native_capacity);
    if (r != FM_OK) return r;

    return Image_Scale(native_rgb, reader->info.width, reader->info.height,
                       FM_PIXEL_RGB888, scaled_rgb, out_width, out_height,
                       mode);
}

static inline FM_Result MediaIO_VideoClose(MediaIO_VideoReader *reader)
{
    FM_Result r;

    if (reader == NULL || !reader->opened)
    {
        return FM_ERR_NOT_OPEN;
    }
    r = Video_Close(&reader->handle);
    reader->opened = FM_FALSE;
    return r;
}

/* Streaming RGB888 video writer. The built-in output container is FMRV raw;
 * an AVI/MP4 encoder is intentionally not claimed by this API. */
static inline FM_Result MediaIO_VideoWriterBeginRGB888(
    FM_VideoWriter *writer, const char *path,
    uint16_t width, uint16_t height, uint32_t fps,
    uint32_t expected_frames)
{
    uint64_t bytes = (uint64_t)width * height * 3u;

    if (writer == NULL || path == NULL || width == 0 || height == 0 ||
        fps == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (bytes > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    return VideoWriter_Create(path, width, height, fps, expected_frames,
                              (uint32_t)bytes, writer);
}

static inline FM_Result MediaIO_VideoWriterWriteRGB888(
    FM_VideoWriter *writer, const uint8_t *rgb, size_t rgb_size)
{
    return VideoWriter_WriteFrame(writer, rgb, rgb_size);
}

static inline FM_Result MediaIO_VideoWriterEnd(FM_VideoWriter *writer)
{
    return VideoWriter_Close(writer);
}

/* ========================================================================== */
/* Image                                                                       */
/* ========================================================================== */

/* Load built-in BMP input and normalize it to RGB888. A 24-bit BMP decodes
 * directly into rgb_buffer. An 8-bit grayscale BMP uses gray_work first and
 * then expands to RGB888. `image->data` points at rgb_buffer on success. */
static inline FM_Result MediaIO_ImageLoadRGB888(
    const char *path, FM_Image *image,
    uint8_t *gray_work, size_t gray_capacity,
    uint8_t *rgb_buffer, size_t rgb_capacity)
{
    FM_ImageInfo info;
    FM_Result r;
    size_t pixels;
    size_t rgb_needed;

    if (path == NULL || image == NULL || rgb_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_GetInfo(path, &info);
    if (r != FM_OK) return r;

    pixels = (size_t)info.width * info.height;
    if (pixels > SIZE_MAX / 3u)
    {
        return FM_ERR_TOO_LARGE;
    }
    rgb_needed = pixels * 3u;
    if (rgb_capacity < rgb_needed)
    {
        return FM_ERR_TOO_LARGE;
    }

    if (info.channels == 3)
    {
        return Image_Load(path, image, rgb_buffer, rgb_capacity);
    }
    if (info.channels != 1 || gray_work == NULL || gray_capacity < pixels)
    {
        return FM_ERR_TOO_LARGE;
    }

    {
        FM_Image gray;
        FM_Image rgb;

        memset(&gray, 0, sizeof(gray));
        memset(&rgb, 0, sizeof(rgb));
        r = Image_Load(path, &gray, gray_work, gray_capacity);
        if (r != FM_OK) return r;

        rgb.data = rgb_buffer;
        rgb.data_capacity = rgb_capacity;
        r = Image_Convert(&gray, &rgb, FM_PIXEL_RGB888);
        if (r == FM_OK) *image = rgb;
    }
    return r;
}

/* Load BMP and convert to a selected caller-owned raw pixel layout. The
 * decoder's native RGB888 image is placed in rgb_work; for RGB888 output,
 * rgb_work and pixel_buffer may be the same pointer. */
static inline FM_Result MediaIO_ImageLoadAs(
    const char *path, FM_Image *image, FM_PixelFormat format,
    uint8_t *gray_work, size_t gray_capacity,
    uint8_t *rgb_work, size_t rgb_capacity,
    uint8_t *pixel_buffer, size_t pixel_capacity)
{
    FM_Image rgb;
    FM_Image output;
    FM_Result r;

    if (image == NULL || rgb_work == NULL || pixel_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&rgb, 0, sizeof(rgb));
    r = MediaIO_ImageLoadRGB888(path, &rgb, gray_work, gray_capacity,
                             rgb_work, rgb_capacity);
    if (r != FM_OK) return r;

    memset(&output, 0, sizeof(output));
    output.data = pixel_buffer;
    output.data_capacity = pixel_capacity;
    r = Image_Convert(&rgb, &output, format);
    if (r == FM_OK) *image = output;
    return r;
}

static inline FM_Result MediaIO_ImageSaveBMP(const char *path,
                                           const uint8_t *pixels,
                                           uint16_t width, uint16_t height,
                                           FM_PixelFormat format)
{
    FM_Image image;
    size_t bytes;
    FM_Result r;

    if (path == NULL || pixels == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_BufferSize(width, height, format, &bytes);
    if (r != FM_OK) return r;

    memset(&image, 0, sizeof(image));
    image.width = width;
    image.height = height;
    image.format = format;
    image.channels = (uint8_t)Image_BytesPerPixel(format);
    image.bit_depth = (uint8_t)(image.channels * 8u);
    image.data = (uint8_t *)pixels;
    image.data_capacity = bytes;
    image.data_size = bytes;
    return Image_Save(path, &image);
}

static inline FM_Result MediaIO_ImageSaveRGB888(const char *path,
                                              const uint8_t *rgb,
                                              uint16_t width,
                                              uint16_t height)
{
    return MediaIO_ImageSaveBMP(path, rgb, width, height, FM_PIXEL_RGB888);
}

/* Resize a caller-owned image while preserving its raw pixel layout. */
static inline FM_Result MediaIO_ImageResize(
    const uint8_t *source, uint16_t source_width, uint16_t source_height,
    uint8_t *destination, size_t destination_capacity,
    uint16_t destination_width, uint16_t destination_height,
    FM_PixelFormat format, FM_ScaleMode mode)
{
    size_t needed;
    FM_Result r;

    if (source == NULL || destination == NULL || source_width == 0 ||
        source_height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_BufferSize(destination_width, destination_height, format,
                         &needed);
    if (r != FM_OK) return r;
    if (destination_capacity < needed)
    {
        return FM_ERR_TOO_LARGE;
    }
    return Image_Scale(source, source_width, source_height, format,
                       destination, destination_width, destination_height,
                       mode);
}

static inline FM_Result MediaIO_ImageResizeRGB888(
    const uint8_t *source, uint16_t source_width, uint16_t source_height,
    uint8_t *destination, size_t destination_capacity,
    uint16_t destination_width, uint16_t destination_height,
    FM_ScaleMode mode)
{
    return MediaIO_ImageResize(source, source_width, source_height,
                            destination, destination_capacity,
                            destination_width, destination_height,
                            FM_PIXEL_RGB888, mode);
}

#endif /* MEDIA_DATA_IO_H_ */

#endif /* FM_TOPAPI_MEDIA_DATA_IO_H */
