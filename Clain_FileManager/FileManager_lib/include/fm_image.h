/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_image.h - Image plugin (optional, self-contained)
 *
 * The image subsystem is a plugin: it never links into the firmware
 * unless FM_USE_IMAGE is 1.
 *
 * Built-in codecs:
 *   BMP  — read + write (24bpp RGB888, 8bpp gray read)
 *   JPEG — still-image decode (picojpeg software, streamed — no heap
 *          slurp) and baseline encode; STM32 JPEG hardware via
 *          Image_RegisterCodec("JPEG", ...) / example 20 (falls back to
 *          picojpeg when the file does not fit the heap)
 *   PNG  — decode of every colour type and bit depth (gray 1/2/4/8/16,
 *          RGB / RGBA / gray+alpha 8/16, palette 1/2/4/8), interlaced
 *          (Adam7) or not; inflate built in, streamed, Adler-32 and
 *          chunk CRCs verified.  Output is RGB888 (alpha dropped).
 *          Write: RGB8, stored zlib, valid PNG
 *
 * Image_Load auto-detects BMP/JPEG/PNG from magic bytes (the file
 * extension is ignored).  When the decoded RGB888 image would not fit
 * the caller buffer it is downscaled in-place so a 12 MP photo still
 * lands on an 800x480 TFT.
 *
 * Image_Save picks BMP / PNG / JPEG from the destination extension.
 *
 * Large BMP images can be streamed line-by-line with Image_StreamDecode.
 */

#ifndef FM_IMAGE_H
#define FM_IMAGE_H

#include "fm_types.h"
#include "fm_stream.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_PIXEL_GRAY8    = 1,
    /* 16-bit values are stored little-endian. RGB565 has R in bits 15..11;
     * BGR565 has B in bits 15..11. */
    FM_PIXEL_RGB565   = 2,
    FM_PIXEL_RGB888   = 3,  /* R,G,B */
    FM_PIXEL_RGBA8888 = 4,  /* R,G,B,A */
    FM_PIXEL_BGR565   = 5,
    FM_PIXEL_BGR888   = 6,  /* B,G,R */
    FM_PIXEL_BGRA8888 = 7   /* B,G,R,A */
} FM_PixelFormat;

typedef enum
{
    FM_IMAGE_EFFECT_INVERT = 0,
    FM_IMAGE_EFFECT_GRAYSCALE,
    FM_IMAGE_EFFECT_SEPIA,
    FM_IMAGE_EFFECT_BRIGHTNESS,
    FM_IMAGE_EFFECT_CONTRAST
} FM_ImageEffect;

typedef struct
{
    uint16_t      width;
    uint16_t      height;
    FM_PixelFormat format;
    uint8_t       bit_depth;      /* bits per pixel channel group   */
    uint8_t       channels;       /* 1, 2, 3 or 4                   */
    uint32_t      compression;    /* codec specific                 */
    uint64_t      data_size;      /* raw pixel bytes                */
    uint8_t       *data;          /* user-provided pixel buffer     */
    size_t        data_capacity;  /* capacity of data               */
    char          codec[8];       /* "BMP","JPEG","PNG",...         */
} FM_Image;

typedef struct
{
    uint16_t      width;
    uint16_t      height;
    uint8_t       bit_depth;
    uint8_t       channels;
    uint32_t      compression;
    uint64_t      data_size;
    char          codec[8];
} FM_ImageInfo;

/* ------------------------------------------------------------------ */
/* Whole-image operations (user provides the pixel buffer)             */
/* ------------------------------------------------------------------ */
FM_Result Image_Load(const char *path, FM_Image *img, void *pixel_buffer,
                     size_t capacity);
/* Like Image_Load, then clamp the result to max_w x max_h (0 = no extra
 * clamp beyond `capacity`).  Used by the TFT album to fit photos. */
FM_Result Image_LoadEx(const char *path, FM_Image *img, void *pixel_buffer,
                       size_t capacity, uint16_t max_w, uint16_t max_h);
FM_Result Image_Save(const char *path, const FM_Image *img);
FM_Result Image_GetInfo(const char *path, FM_ImageInfo *info);

/* Optional SDRAM (or other) workspace for HW JPEG file slurp / YCbCr.
 * SimpleImage_Begin installs its .sdram_buffers file scratch here so
 * HAL_JPEG_Decode never mallocs the compressed JPEG on the tiny heap.
 * Pass NULL, 0 to clear.
 *
 * A registered hardware "JPEG" codec takes the compressed picture as one
 * memory block, so Image_Load has to hold the whole file for it:
 *   - in this work buffer when the file fits;
 *   - else in a block from FM_Alloc, unless FM_JPEG_HW_HEAP is 0;
 *   - never when the file is larger than FM_JPEG_HW_FILE_MAX.
 * Whenever the file is not handed to the hardware (too large, no memory,
 * or the codec refuses it) the picture is decoded by the streaming
 * software decoder instead, which needs no file-sized memory. */
FM_Result Image_SetWorkBuffer(void *buf, size_t size);
void     *Image_GetWorkBuffer(size_t *size);

/* BMP encode/decode from memory */
FM_Result Image_DecodeBMP(const void *bmp_data, size_t bmp_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity);
FM_Result Image_EncodeBMP(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len);

/* JPEG / PNG still-image decode from a complete in-memory file.
 * JPEG uses a registered "JPEG" codec if present (hardware), otherwise
 * the bundled picojpeg software decoder. PNG is built in. Output is
 * RGB888 (or GRAY8 for gray sources). */
FM_Result Image_DecodeJPEG(const void *jpeg, size_t jpeg_size,
                           FM_Image *img, void *pixel_buffer, size_t capacity);
FM_Result Image_DecodePNG(const void *png, size_t png_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity);
FM_Result Image_ProbeJPEG(const void *jpeg, size_t jpeg_size,
                          FM_ImageInfo *info);
FM_Result Image_ProbePNG(const void *png, size_t png_size,
                         FM_ImageInfo *info);
FM_Result Image_EncodeJPEG(const FM_Image *img, void *out, size_t out_cap,
                           size_t *out_len);
FM_Result Image_EncodePNG(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len);

/* ------------------------------------------------------------------ */
/* Pluggable still-image codecs (e.g. STM32 JPEG peripheral)           */
/* ------------------------------------------------------------------ */
typedef struct FM_ImageCodecOps
{
    const char *name;
    /* Fill width/height/codec. May inspect only the header. */
    FM_Result (*probe)(const uint8_t *data, size_t size, FM_ImageInfo *info);
    /* Decode into RGB888 (or GRAY8) in pixel_buffer. */
    FM_Result (*decode)(const uint8_t *data, size_t size, FM_Image *img,
                        void *pixel_buffer, size_t capacity);
} FM_ImageCodecOps;

/* Register a still-image codec tag ("JPEG", "PNG", ...). Replaces any
 * previous binding for that tag. Hardware JPEG should register "JPEG"
 * so Image_Load prefers it over picojpeg. */
FM_Result Image_RegisterCodec(const char *codec, const FM_ImageCodecOps *ops);
FM_Result Image_UnregisterCodec(const char *codec);
const FM_ImageCodecOps *Image_GetCodec(const char *codec);

/* ------------------------------------------------------------------ */
/* Streaming decode (line by line)                                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    FM_Stream    *stream;
    FM_ImageInfo  info;
    uint8_t       state[40];   /* internal decoder state (BMP) */
} FM_ImageDecoder;

typedef void (*FM_ImageLineCb)(const void *line, uint32_t y,
                               void *user_data);

FM_Result Image_OpenStream(FM_Stream *s, FM_ImageDecoder *dec,
                           FM_ImageInfo *info);
FM_Result Image_ReadLine(FM_ImageDecoder *dec, void *line_out,
                         size_t line_cap);
/* Decode a BMP row by row; line_cb gets every row (top-down, RGB888 or
 * GRAY8).  img->width / height / format / channels / codec are always
 * filled in.  Two modes:
 *   - img->data points at a buffer that holds the whole picture: the
 *     rows are kept there and img->data_size is set;
 *   - otherwise (img->data == NULL or too small) nothing is kept,
 *     img->data_size is 0, and each row passes through an on-stack
 *     buffer of FM_INTERNAL_BUFFER_SIZE bytes -- a wider row is
 *     FM_ERR_TOO_LARGE. */
FM_Result Image_StreamDecode(FM_Stream *s, FM_Image *img,
                             FM_ImageLineCb line_cb, void *user_data,
                             FM_ProgressCallback progress);
void      Image_CloseDecoder(FM_ImageDecoder *dec);

/* ------------------------------------------------------------------ */
/* Pixel format conversion helpers                                     */
/* ------------------------------------------------------------------ */
/* Raw pixel-buffer sizing helpers. Image_BufferSize detects overflow. */
size_t    Image_BytesPerPixel(FM_PixelFormat format);
FM_Result Image_BufferSize(uint16_t width, uint16_t height,
                           FM_PixelFormat format, size_t *bytes);

FM_Result Image_Convert(const FM_Image *src, FM_Image *dst,
                        FM_PixelFormat dst_format);
FM_Result Image_ConvertRGB565to888(const void *src, void *dst,
                                   size_t pixel_count);
FM_Result Image_ConvertRGB888to565(const void *src, void *dst,
                                   size_t pixel_count);
FM_Result Image_ConvertRGB888toGray(const void *src, void *dst,
                                    size_t pixel_count);

/* Apply an effect in-place while preserving the image pixel format.
 * INVERT, GRAYSCALE and SEPIA ignore `amount`.
 * BRIGHTNESS uses -255..255; CONTRAST uses -100..100 percent.
 * RGBA/BGRA alpha is preserved. SEPIA is unsupported for GRAY8 because
 * that format cannot retain the generated color. */
FM_Result Image_ApplyEffect(FM_Image *img, FM_ImageEffect effect,
                            int32_t amount);

/* ------------------------------------------------------------------ */
/* Scaling / resizing                                                  */
/* ------------------------------------------------------------------ */
/* Scale a raw pixel buffer from (sw x sh) to (dw x dh), preserving the
 * pixel format. Supports GRAY8, RGB/BGR 565, RGB/BGR 888 and RGBA/BGRA8888.
 * Alpha is preserved/interpolated. Nearest is fastest; bilinear is smoother. */
FM_Result Image_Scale(const void *src, uint16_t sw, uint16_t sh,
                      FM_PixelFormat fmt, void *dst, uint16_t dw, uint16_t dh,
                      FM_ScaleMode mode);

/* Same, with the size of both buffers: FM_ERR_TOO_LARGE when either is
 * smaller than width*height*bytes-per-pixel, before a single pixel is
 * touched.  Image_Scale() cannot check that (it only sees pointers) and
 * is kept for callers whose buffers are sized by construction; new code
 * should call this one. */
FM_Result Image_ScaleEx(const void *src, size_t src_cap,
                        uint16_t sw, uint16_t sh, FM_PixelFormat fmt,
                        void *dst, size_t dst_cap,
                        uint16_t dw, uint16_t dh, FM_ScaleMode mode);

/* Load an image, scale it, and save it (BMP) — resizing "on storage".
 * Uses the fm_alloc.h allocation hooks for the working buffers. */
FM_Result Image_ResizeFile(const char *src_path, const char *dst_path,
                           uint16_t dw, uint16_t dh, FM_ScaleMode mode);

/* ------------------------------------------------------------------ */
/* Flip / rotate                                                       */
/* ------------------------------------------------------------------ */
typedef enum
{
    FM_IMAGE_FLIP_HORIZONTAL = 0, /* left <-> right */
    FM_IMAGE_FLIP_VERTICAL        /* top <-> bottom */
} FM_ImageFlip;

/* In-place flip. Width/height stay the same. */
FM_Result Image_Flip(FM_Image *img, FM_ImageFlip axis);

/* Rotate `degrees` clockwise (0/90/180/270 or -90). 90/270 swap width
 * and height. `dst` must be a different buffer with capacity for the
 * rotated size; format is preserved. */
FM_Result Image_Rotate(const FM_Image *src, FM_Image *dst, int degrees);

FM_END_DECLS

#endif /* FM_IMAGE_H */
