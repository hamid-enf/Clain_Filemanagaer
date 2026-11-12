/*
 * FileManager - BMP image support
 *
 * BMP decoding is streamed row-by-row and writes into caller-owned pixel
 * memory.  The public representation is top-down RGB888 or GRAY8, so a
 * caller never has to deal with BMP padding, BGR ordering or bottom-up rows.
 */

#define FM_DEBUG_TAG "image"
#include "fm_debug.h"
#include "fm_image.h"
#include "fm_internal.h"
#include "fm_alloc.h"
#include <string.h>

#if FM_USE_IMAGE

typedef struct
{
    uint64_t data_offset;
    uint32_t row_stride;
    uint32_t width;
    uint32_t height;
    uint32_t next_y;
    uint8_t  bits_per_pixel;
    uint8_t  top_down;
    uint8_t  use_palette;   /* 8 bpp: rows go through dec->palette */
} ImageDecoderState;

#define BMP_PALETTE_MAX 256u

typedef char image_decoder_state_fits[
    (sizeof(ImageDecoderState) <= sizeof(((FM_ImageDecoder *)0)->state)) ? 1 : -1];

FM_IMAGE_SCRATCH
static uint8_t g_image_line[FM_IMAGE_LINE_MAX];

static uint8_t *g_image_work;
static size_t   g_image_work_cap;

/* Pointer and size change together: read and write them as a pair
 * under the lock, or a reader could see the new pointer with the old
 * (larger) size.  The buffer itself is used under the codec claim. */
FM_Result Image_SetWorkBuffer(void *buf, size_t size)
{
    FM_Lock();
    g_image_work = (uint8_t *)buf;
    g_image_work_cap = (buf == NULL) ? 0u : size;
    FM_Unlock();
    return FM_OK;
}

void *Image_GetWorkBuffer(size_t *size)
{
    void *p;

    FM_Lock();
    p = g_image_work;
    if (size != NULL)
    {
        *size = g_image_work_cap;
    }
    FM_Unlock();
    return p;
}

static uint16_t image_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t image_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t image_rdi32(const uint8_t *p)
{
    return (int32_t)image_rd32(p);
}

static void image_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void image_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

size_t Image_BytesPerPixel(FM_PixelFormat format)
{
    switch (format)
    {
    case FM_PIXEL_GRAY8:                         return 1u;
    case FM_PIXEL_RGB565:
    case FM_PIXEL_BGR565:                        return 2u;
    case FM_PIXEL_RGB888:
    case FM_PIXEL_BGR888:                        return 3u;
    case FM_PIXEL_RGBA8888:
    case FM_PIXEL_BGRA8888:                      return 4u;
    default:                                     return 0u;
    }
}

FM_Result Image_BufferSize(uint16_t width, uint16_t height,
                           FM_PixelFormat format, size_t *bytes)
{
    size_t bpp;

    if (width == 0 || height == 0 || bytes == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bpp = Image_BytesPerPixel(format);
    if (bpp == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    return FM_Image_Bytes(width, height, bpp, bytes);
}

/* The one place a pixel-buffer size is computed.  65535 x 65535 x 4 is
 * 17 GB: in size_t arithmetic on a 32-bit MCU it wraps to a few bytes,
 * the allocation succeeds and the decoder writes the whole picture into
 * it.  So multiply in 64 bits and compare before narrowing. */
FM_Result FM_Image_Bytes(uint32_t width, uint32_t height, size_t bpp,
                         size_t *bytes)
{
    uint64_t total;

    if (width == 0u || height == 0u || bpp == 0u || bytes == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (bpp > 16u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    total = (uint64_t)width * (uint64_t)height * (uint64_t)bpp;
    if (total > (uint64_t)SIZE_MAX || total > (uint64_t)FM_IMAGE_MAX_BYTES)
    {
        return FM_ERR_TOO_LARGE;
    }
    *bytes = (size_t)total;
    return FM_OK;
}

/* data_capacity is what the buffer can hold, data_size (when not 0) how
 * much of it is valid pixels.  Encoders used to look at the capacity
 * only: an image declaring 100x100 over a buffer whose first 3 bytes
 * are valid was encoded from 29997 bytes nobody wrote. */
FM_Result FM_Image_CheckSource(const FM_Image *img, size_t *need)
{
    size_t n = 0;
    FM_Result r;

    if (img == NULL || img->data == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_Image_Bytes(img->width, img->height,
                       Image_BytesPerPixel(img->format), &n);
    if (r != FM_OK)
    {
        return r;
    }
    if (n > img->data_capacity ||
        (img->data_size != 0u && (uint64_t)n > img->data_size))
    {
        return FM_ERR_TOO_LARGE;
    }
    if (need != NULL)
    {
        *need = n;
    }
    return FM_OK;
}

FM_Result FM_Image_FitSize(uint16_t src_w, uint16_t src_h, size_t capacity,
                           size_t bpp, uint16_t max_w, uint16_t max_h,
                           uint16_t *dst_w, uint16_t *dst_h)
{
    uint32_t w;
    uint32_t h;
    uint64_t cap_pix;

    if (src_w == 0u || src_h == 0u || bpp == 0u || dst_w == NULL ||
        dst_h == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    w = src_w;
    h = src_h;
    if (max_w != 0u && w > max_w)
    {
        h = (uint32_t)(((uint64_t)h * max_w) / w);
        w = max_w;
        if (h == 0u)
        {
            h = 1u;
        }
    }
    if (max_h != 0u && h > max_h)
    {
        w = (uint32_t)(((uint64_t)w * max_h) / h);
        h = max_h;
        if (w == 0u)
        {
            w = 1u;
        }
    }
    cap_pix = (uint64_t)(capacity / bpp);
    if (cap_pix == 0u)
    {
        return FM_ERR_TOO_LARGE;
    }
    while ((uint64_t)w * h > cap_pix)
    {
        uint32_t nw = (w * 7u) / 8u;
        uint32_t nh = (h * 7u) / 8u;

        if (nw == w && w > 1u)
        {
            nw = w - 1u;
        }
        if (nh == h && h > 1u)
        {
            nh = h - 1u;
        }
        if (nw == 0u)
        {
            nw = 1u;
        }
        if (nh == 0u)
        {
            nh = 1u;
        }
        if (nw == w && nh == h)
        {
            return FM_ERR_TOO_LARGE;
        }
        w = nw;
        h = nh;
    }
    *dst_w = (uint16_t)w;
    *dst_h = (uint16_t)h;
    return FM_OK;
}

FM_Result FM_Image_ReadExact(FM_File *f, void *data, uint32_t size)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t done = 0;

    if (f == NULL || (data == NULL && size != 0u))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    while (done < size)
    {
        uint32_t rd = 0;
        FM_Result r = File_Read(f, p + done, size - done, &rd);

        if (r != FM_OK && r != FM_ERR_EOF)
        {
            return r;
        }
        if (rd == 0u)
        {
            return FM_ERR_EOF;
        }
        done += rd;
    }
    return FM_OK;
}

static FM_Result image_file_write_exact(FM_File *f, const void *data,
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

static FM_Result image_stream_read_exact(FM_Stream *s, void *data,
                                         size_t size)
{
    uint8_t *p = (uint8_t *)data;
    size_t done = 0;

    while (done < size)
    {
        size_t rd = 0;
        FM_Result r = Stream_Read(s, p + done, size - done, &rd);

        if (r != FM_OK)
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

/* The colour table of an 8-bpp BMP.
 *
 * The bytes of such a picture are indices, not grey levels.  They are
 * grey levels only when entry i of the table is (i, i, i) -- which is
 * what a "greyscale BMP" is, and the one case that needs no table: the
 * rows are then delivered as they are, GRAY8.  Every other table is
 * loaded (as RGB, 3 bytes an entry) and the rows are delivered through
 * it as RGB888.  Entries the file does not have stay black.
 *
 * A file with no table at all (pixel data right after the header) has
 * no colours defined; it is read as grey, as it always was. */
static FM_Result bmp_load_palette(FM_Stream *s, uint32_t dib_size,
                                  uint32_t clr_used, uint64_t data_offset,
                                  uint8_t *palette, uint8_t *use_palette)
{
    uint64_t table = 14u + (uint64_t)dib_size;
    uint32_t count = (clr_used == 0u) ? BMP_PALETTE_MAX : clr_used;
    uint32_t i = 0;
    FM_Bool grey = FM_TRUE;

    *use_palette = 0u;
    if (count > BMP_PALETTE_MAX)
    {
        return FM_ERR_CORRUPTED;
    }
    if (data_offset <= table)
    {
        return FM_OK;                       /* no table */
    }
    if ((data_offset - table) / 4u < count)
    {
        count = (uint32_t)((data_offset - table) / 4u);
    }
    if (count == 0u)
    {
        return FM_OK;
    }
    if (Stream_Seek(s, (int64_t)table, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    memset(palette, 0, BMP_PALETTE_MAX * 3u);
    while (i < count)
    {
        uint8_t quad[16 * 4];
        uint32_t n = count - i;
        uint32_t k;

        if (n > 16u)
        {
            n = 16u;
        }
        if (image_stream_read_exact(s, quad, (size_t)n * 4u) != FM_OK)
        {
            return FM_ERR_EOF;
        }
        for (k = 0; k < n; k++, i++)
        {
            const uint8_t *q = quad + k * 4u;       /* B, G, R, reserved */

            palette[i * 3u] = q[2];
            palette[i * 3u + 1u] = q[1];
            palette[i * 3u + 2u] = q[0];
            if (q[0] != (uint8_t)i || q[1] != (uint8_t)i || q[2] != (uint8_t)i)
            {
                grey = FM_FALSE;
            }
        }
    }
    *use_palette = (uint8_t)(grey ? 0 : 1);
    return FM_OK;
}

static FM_Result image_parse_header(FM_Stream *s, FM_ImageInfo *info,
                                    ImageDecoderState *state,
                                    uint8_t *palette)
{
    uint8_t h[54];
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bits;
    uint32_t dib_size;
    uint32_t compression;
    uint64_t pixels;

    if (Stream_Seek(s, 0, FM_SEEK_SET) != FM_OK ||
        image_stream_read_exact(s, h, sizeof(h)) != FM_OK)
    {
        return FM_ERR_EOF;
    }
    if (h[0] != 'B' || h[1] != 'M')
    {
        return FM_ERR_INVALID_FORMAT;
    }
    dib_size = image_rd32(h + 14);
    width = image_rdi32(h + 18);
    height = image_rdi32(h + 22);
    planes = image_rd16(h + 26);
    bits = image_rd16(h + 28);
    compression = image_rd32(h + 30);
    if (dib_size < 40 || width <= 0 || height == 0 || height == INT32_MIN ||
        planes != 1 || (bits != 8 && bits != 24) || compression != 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if ((uint32_t)width > UINT16_MAX ||
        (uint32_t)(height < 0 ? -height : height) > UINT16_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    pixels = (uint64_t)(uint32_t)width *
             (uint64_t)(uint32_t)(height < 0 ? -height : height);
    state->data_offset = image_rd32(h + 10);
    state->bits_per_pixel = (uint8_t)bits;
    state->width = (uint32_t)width;
    state->height = (uint32_t)(height < 0 ? -height : height);
    state->top_down = (uint8_t)((height < 0) ? 1 : 0);
    state->row_stride = (uint32_t)((((uint64_t)width * bits + 31u) / 32u) * 4u);
    state->next_y = 0;
    state->use_palette = 0u;
    if (bits == 8)
    {
        FM_Result r = bmp_load_palette(s, dib_size, image_rd32(h + 46),
                                       state->data_offset, palette,
                                       &state->use_palette);

        if (r != FM_OK)
        {
            return r;
        }
    }

    /* Reported as delivered: a paletted picture comes out as RGB888 */
    memset(info, 0, sizeof(*info));
    info->width = (uint16_t)state->width;
    info->height = (uint16_t)state->height;
    info->bit_depth = (bits == 24 || state->use_palette) ? 24u : 8u;
    info->channels = (bits == 24 || state->use_palette) ? 3u : 1u;
    info->compression = compression;
    info->data_size = pixels * info->channels;
    (void)strcpy(info->codec, "BMP");
    return FM_OK;
}

FM_Result Image_OpenStream(FM_Stream *s, FM_ImageDecoder *dec,
                           FM_ImageInfo *info)
{
    ImageDecoderState state;
    FM_Result r;

    if (s == NULL || dec == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(dec, 0, sizeof(*dec));
    memset(&state, 0, sizeof(state));
    r = image_parse_header(s, info, &state, dec->palette);
    if (r != FM_OK)
    {
        return r;
    }
    dec->stream = s;
    dec->info = *info;
    memcpy(dec->state, &state, sizeof(state));
    return FM_OK;
}

FM_Result Image_ReadLine(FM_ImageDecoder *dec, void *line_out,
                         size_t line_cap)
{
    ImageDecoderState state;
    uint32_t file_y;
    uint64_t row_offset;
    uint32_t x;
    size_t output_bpp;

    if (dec == NULL || dec->stream == NULL || line_out == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memcpy(&state, dec->state, sizeof(state));
    if (state.next_y >= state.height)
    {
        return FM_ERR_EOF;
    }
    output_bpp = (state.bits_per_pixel == 24u || state.use_palette) ? 3u : 1u;
    if ((uint64_t)state.width * output_bpp > line_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    file_y = state.top_down ? state.next_y : state.height - 1u - state.next_y;
    row_offset = state.data_offset + (uint64_t)file_y * state.row_stride;
    if (Stream_Seek(dec->stream, (int64_t)row_offset, FM_SEEK_SET) != FM_OK)
    {
        return FM_ERR_IO;
    }
    if (state.bits_per_pixel == 24u)
    {
        uint8_t *out = (uint8_t *)line_out;

        for (x = 0; x < state.width; x++)
        {
            uint8_t bgr[3];

            if (image_stream_read_exact(dec->stream, bgr, sizeof(bgr)) != FM_OK)
            {
                return FM_ERR_EOF;
            }
            out[x * 3u] = bgr[2];
            out[x * 3u + 1u] = bgr[1];
            out[x * 3u + 2u] = bgr[0];
        }
    }
    else if (state.use_palette)
    {
        /* The indices are read into the last third of the row and
         * expanded from the front: pixel x is written at 3x..3x+2 and
         * its index sits at 2w+x, so no index is overwritten before it
         * has been used, and no second buffer is needed. */
        uint8_t *out = (uint8_t *)line_out;
        const uint8_t *idx = out + (size_t)state.width * 2u;

        if (image_stream_read_exact(dec->stream, out + (size_t)state.width * 2u,
                                    state.width) != FM_OK)
        {
            return FM_ERR_EOF;
        }
        for (x = 0; x < state.width; x++)
        {
            const uint8_t *c = dec->palette + (size_t)idx[x] * 3u;
            uint8_t r8 = c[0];
            uint8_t g8 = c[1];
            uint8_t b8 = c[2];

            out[x * 3u] = r8;
            out[x * 3u + 1u] = g8;
            out[x * 3u + 2u] = b8;
        }
    }
    else
    {
        if (image_stream_read_exact(dec->stream, line_out, state.width) != FM_OK)
        {
            return FM_ERR_EOF;
        }
    }
    state.next_y++;
    memcpy(dec->state, &state, sizeof(state));
    return FM_OK;
}

void Image_CloseDecoder(FM_ImageDecoder *dec)
{
    if (dec != NULL)
    {
        memset(dec, 0, sizeof(*dec));
    }
}

static FM_Result image_load_stream(FM_Stream *s, FM_Image *img,
                                   void *pixel_buffer, size_t capacity,
                                   uint16_t max_w, uint16_t max_h)
{
    FM_ImageDecoder dec;
    FM_ImageInfo info;
    size_t row_bytes;
    size_t bpp;
    uint16_t dw;
    uint16_t dh;
    uint32_t y;
    FM_Result r;

    r = Image_OpenStream(s, &dec, &info);
    if (r != FM_OK)
    {
        return r;
    }
    bpp = info.channels;
    row_bytes = (size_t)info.width * bpp;
    r = FM_Image_FitSize(info.width, info.height, capacity, bpp,
                         max_w, max_h, &dw, &dh);
    if (r != FM_OK || pixel_buffer == NULL)
    {
        Image_CloseDecoder(&dec);
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }
    memset(img, 0, sizeof(*img));
    img->width = dw;
    img->height = dh;
    img->format = (info.channels == 3) ? FM_PIXEL_RGB888 : FM_PIXEL_GRAY8;
    img->bit_depth = info.bit_depth;
    img->channels = info.channels;
    img->compression = info.compression;
    img->data_size = (uint64_t)dw * dh * bpp;
    img->data = (uint8_t *)pixel_buffer;
    img->data_capacity = capacity;
    (void)strcpy(img->codec, info.codec);

    if (dw == info.width && dh == info.height)
    {
        for (y = 0; y < info.height; y++)
        {
            r = Image_ReadLine(&dec, img->data + (size_t)y * row_bytes,
                               row_bytes);
            if (r != FM_OK)
            {
                Image_CloseDecoder(&dec);
                return r;
            }
            FM_Idle();
        }
        Image_CloseDecoder(&dec);
        return FM_OK;
    }
    if (row_bytes > sizeof(g_image_line))
    {
        Image_CloseDecoder(&dec);
        return FM_ERR_TOO_LARGE;
    }
    r = FM_CodecScratch_Enter();         /* g_image_line is one buffer */
    if (r != FM_OK)
    {
        Image_CloseDecoder(&dec);
        return r;
    }
    for (y = 0; y < info.height; y++)
    {
        uint16_t dy;

        r = Image_ReadLine(&dec, g_image_line, sizeof(g_image_line));
        if (r != FM_OK)
        {
            FM_CodecScratch_Leave();
            Image_CloseDecoder(&dec);
            return r;
        }
        FM_Idle();
        for (dy = 0; dy < dh; dy++)
        {
            if ((uint32_t)dy * info.height / dh != y)
            {
                continue;
            }
            {
                uint8_t *dst = img->data + (size_t)dy * dw * bpp;
                uint16_t dx;

                for (dx = 0; dx < dw; dx++)
                {
                    uint32_t sx = (uint32_t)dx * info.width / dw;
                    size_t src_off = (size_t)sx * bpp;
                    size_t dst_off = (size_t)dx * bpp;
                    size_t c;

                    for (c = 0; c < bpp; c++)
                    {
                        dst[dst_off + c] = g_image_line[src_off + c];
                    }
                }
            }
        }
    }
    FM_CodecScratch_Leave();
    Image_CloseDecoder(&dec);
    return FM_OK;
}

static const uint8_t k_image_png_sig[8] = {
    137, 80, 78, 71, 13, 10, 26, 10
};

FM_Result Image_Load(const char *path, FM_Image *img, void *pixel_buffer,
                     size_t capacity)
{
    return Image_LoadEx(path, img, pixel_buffer, capacity, 0, 0);
}

static FM_Result image_load_ex(const char *path, FM_Image *img,
                               void *pixel_buffer, size_t capacity,
                               uint16_t max_w, uint16_t max_h);

FM_Result Image_LoadEx(const char *path, FM_Image *img, void *pixel_buffer,
                       size_t capacity, uint16_t max_w, uint16_t max_h)
{
    FM_Result r;

    FM_CHECK_PTR(path, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(img, FM_ERR_INVALID_ARGUMENT);
    FM_CHECK_PTR(pixel_buffer, FM_ERR_INVALID_ARGUMENT);
    r = image_load_ex(path, img, pixel_buffer, capacity, max_w, max_h);
    if (r == FM_OK)
    {
        FM_EVENT_LOG("loaded %s (%ux%u)", path, (unsigned)img->width,
                     (unsigned)img->height);
    }
    else
    {
        /* One line for every way a load can fail (unknown format,
         * damaged file, buffer too small, ...), not only a failed open. */
        FM_ERR_LOG("image load %s failed (%d)", path, (int)r);
    }
    return r;
}

static FM_Result image_load_ex(const char *path, FM_Image *img,
                               void *pixel_buffer, size_t capacity,
                               uint16_t max_w, uint16_t max_h)
{
    FM_File file;
    FM_Stream stream;
    FM_Result r;
    uint8_t magic[8];
    uint32_t rd = 0;

    File_Init(&file);
    r = File_OpenEx(&file, path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Read(&file, magic, sizeof(magic), &rd);
    if (r != FM_OK && r != FM_ERR_EOF)
    {
        (void)File_Close(&file);
        return r;
    }
    if (File_Seek(&file, 0, FM_SEEK_SET) != FM_OK)
    {
        (void)File_Close(&file);
        return FM_ERR_IO;
    }
    if (rd >= 2u && magic[0] == 0xFFu && magic[1] == 0xD8u)
    {
#if FM_USE_JPEG
        r = FM_JPEG_LoadFile(&file, img, pixel_buffer, capacity,
                             max_w, max_h);
#else
        r = FM_ERR_UNSUPPORTED;
#endif
        (void)File_Close(&file);
        return r;
    }
    if (rd >= 8u && memcmp(magic, k_image_png_sig, 8) == 0)
    {
#if FM_USE_PNG
        r = FM_PNG_LoadFile(&file, img, pixel_buffer, capacity,
                            max_w, max_h);
#else
        r = FM_ERR_UNSUPPORTED;
#endif
        (void)File_Close(&file);
        return r;
    }
    r = Stream_InitFile(&stream, &file);
    if (r == FM_OK)
    {
        r = image_load_stream(&stream, img, pixel_buffer, capacity,
                              max_w, max_h);
        Stream_Close(&stream);
    }
    (void)File_Close(&file);
    return r;
}

FM_Result Image_GetInfo(const char *path, FM_ImageInfo *info)
{
    FM_File file;
    FM_Stream stream;
    FM_ImageDecoder dec;
    FM_Result r;
    uint8_t magic[40];
    uint32_t rd = 0;

    if (path == NULL || info == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    File_Init(&file);
    r = File_OpenEx(&file, path, FM_OPEN_READ);
    if (r != FM_OK)
    {
        return r;
    }
    r = File_Read(&file, magic, sizeof(magic), &rd);
    if (r != FM_OK && r != FM_ERR_EOF)
    {
        (void)File_Close(&file);
        return r;
    }
    if (rd >= 2u && magic[0] == 0xFFu && magic[1] == 0xD8u)
    {
        if (File_Seek(&file, 0, FM_SEEK_SET) != FM_OK)
        {
            (void)File_Close(&file);
            return FM_ERR_IO;
        }
#if FM_USE_JPEG
        r = FM_JPEG_ProbeFile(&file, info);
#else
        r = FM_ERR_UNSUPPORTED;
#endif
        (void)File_Close(&file);
        return r;
    }
    if (rd >= 8u && memcmp(magic, k_image_png_sig, 8) == 0)
    {
        /* The whole 8-byte signature, as the decoder checks it: two
         * matching bytes used to send any "\x89P..." file to the PNG
         * probe, which then refused it as an invalid PNG. */
        (void)File_Close(&file);
        if (rd < 33u)
        {
            return FM_ERR_CORRUPTED;     /* signature, then no IHDR */
        }
        return Image_ProbePNG(magic, rd, info);
    }
    if (File_Seek(&file, 0, FM_SEEK_SET) != FM_OK)
    {
        (void)File_Close(&file);
        return FM_ERR_IO;
    }
    r = Stream_InitFile(&stream, &file);
    if (r == FM_OK)
    {
        r = Image_OpenStream(&stream, &dec, info);
        Image_CloseDecoder(&dec);
        Stream_Close(&stream);
    }
    (void)File_Close(&file);
    return r;
}

static void image_pixel_rgba(const FM_Image *img, size_t index,
                             uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    const uint8_t *p;

    *a = 0xFFu;
    if (img->format == FM_PIXEL_RGB888)
    {
        p = img->data + index * 3u;
        *r = p[0]; *g = p[1]; *b = p[2];
    }
    else if (img->format == FM_PIXEL_BGR888)
    {
        p = img->data + index * 3u;
        *b = p[0]; *g = p[1]; *r = p[2];
    }
    else if (img->format == FM_PIXEL_RGB565 ||
             img->format == FM_PIXEL_BGR565)
    {
        uint16_t v;
        uint8_t high;
        uint8_t low;

        p = img->data + index * 2u;
        v = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
        high = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
        *g = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        low = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        if (img->format == FM_PIXEL_RGB565)
        {
            *r = high; *b = low;
        }
        else
        {
            *b = high; *r = low;
        }
    }
    else if (img->format == FM_PIXEL_RGBA8888)
    {
        p = img->data + index * 4u;
        *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    }
    else if (img->format == FM_PIXEL_BGRA8888)
    {
        p = img->data + index * 4u;
        *b = p[0]; *g = p[1]; *r = p[2]; *a = p[3];
    }
    else
    {
        uint8_t v = img->data[index];
        *r = v; *g = v; *b = v;
    }
}

static void image_pixel_rgb(const FM_Image *img, size_t index,
                            uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t alpha;

    image_pixel_rgba(img, index, r, g, b, &alpha);
}

static FM_Result image_bmp_header(uint8_t *h, uint16_t width,
                                  uint16_t height, uint32_t file_size)
{
    if (h == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(h, 0, 54);
    h[0] = 'B'; h[1] = 'M';
    image_wr32(h + 2, file_size);
    image_wr32(h + 10, 54);
    image_wr32(h + 14, 40);
    image_wr32(h + 18, width);
    image_wr32(h + 22, height);
    image_wr16(h + 26, 1);
    image_wr16(h + 28, 24);
    /* biSizeImage is the pixel array as stored, row padding included --
     * exactly what follows the 54-byte header (file_size is already
     * range-checked by the callers). */
    image_wr32(h + 34, (file_size >= 54u) ? (file_size - 54u) : 0u);
    return FM_OK;
}

FM_Result Image_EncodeBMP(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len)
{
    uint32_t row_stride;
    uint64_t total;
    uint8_t *dst;
    uint32_t y;
    FM_Result rc;

    if (img == NULL || out == NULL || out_len == NULL || img->data == NULL ||
        img->width == 0 || img->height == 0 ||
        Image_BytesPerPixel(img->format) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    rc = FM_Image_CheckSource(img, NULL);
    if (rc != FM_OK)
    {
        return rc;
    }
    row_stride = ((uint32_t)img->width * 3u + 3u) & ~3u;
    total = 54u + (uint64_t)row_stride * img->height;
    if (total > out_cap || total > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    dst = (uint8_t *)out;
    (void)image_bmp_header(dst, img->width, img->height, (uint32_t)total);
    for (y = 0; y < img->height; y++)
    {
        uint32_t x;
        uint8_t *row = dst + 54u + (size_t)(img->height - 1u - y) * row_stride;

        memset(row, 0, row_stride);
        for (x = 0; x < img->width; x++)
        {
            uint8_t r, g, b;

            image_pixel_rgb(img, (size_t)y * img->width + x, &r, &g, &b);
            row[x * 3u] = b;
            row[x * 3u + 1u] = g;
            row[x * 3u + 2u] = r;
        }
    }
    *out_len = (size_t)total;
    return FM_OK;
}

FM_Result Image_Save(const char *path, const FM_Image *img)
{
    FM_File file;
    uint8_t header[54];
    uint32_t row_stride;
    uint64_t total;
    uint32_t y;
    FM_Result r;
    char ext[8];

    if (path == NULL || img == NULL || img->data == NULL ||
        img->width == 0 || img->height == 0 ||
        Image_BytesPerPixel(img->format) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* data_size (when given) is the number of VALID pixel bytes: an
     * image that claims more pixels than it holds would be encoded
     * from bytes the caller never wrote. */
    r = FM_Image_CheckSource(img, NULL);
    if (r != FM_OK)
    {
        return r;
    }
    ext[0] = '\0';
    (void)Path_GetExtension(path, ext, sizeof(ext));
    {
        /* "PHOTO.PNG" and "photo.png" are the same format (FAT upper-
         * cases 8.3 names). */
        size_t i;

        for (i = 0; i < sizeof(ext) && ext[i] != '\0'; i++)
        {
            if (ext[i] >= 'A' && ext[i] <= 'Z')
            {
                ext[i] = (char)(ext[i] - 'A' + 'a');
            }
        }
    }
    if (ext[0] != '\0' && strcmp(ext, "bmp") != 0)
    {
        /* Pick the encoder BEFORE touching the file.  The file used to be
         * created/truncated first, so saving to an unknown extension, or
         * to a codec that is compiled out, wiped an existing file and
         * left an empty one behind while reporting UNSUPPORTED. */
        FM_Result (*encode)(FM_File *, const FM_Image *) = NULL;

        if (strcmp(ext, "png") == 0)
        {
#if FM_USE_PNG
            encode = FM_PNG_EncodeFile;
#endif
        }
        else if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0 ||
                 strcmp(ext, "jpe") == 0)
        {
#if FM_USE_JPEG
            encode = FM_JPEG_EncodeFile;
#endif
        }
        if (encode == NULL)
        {
            return FM_ERR_UNSUPPORTED;
        }
        File_Init(&file);
        r = File_OpenEx(&file, path,
                        FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
        if (r != FM_OK)
        {
            return r;
        }
        r = encode(&file, img);
        {
            FM_Result c = File_Close(&file);
            if (r == FM_OK)
            {
                r = c;
            }
        }
        if (r != FM_OK)
        {
            /* A half-written PNG/JPEG is worse than none: the next
             * reader would get as far as the truncation and fail. */
            (void)File_Delete(path);
        }
        return r;
    }
    row_stride = ((uint32_t)img->width * 3u + 3u) & ~3u;
    total = 54u + (uint64_t)row_stride * img->height;
    if (total > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }
    File_Init(&file);
    r = File_OpenEx(&file, path,
                    FM_OPEN_WRITE | FM_OPEN_CREATE | FM_OPEN_TRUNCATE);
    if (r != FM_OK)
    {
        return r;
    }
    (void)image_bmp_header(header, img->width, img->height, (uint32_t)total);
    r = image_file_write_exact(&file, header, sizeof(header));
    for (y = 0; r == FM_OK && y < img->height; y++)
    {
        uint32_t x;
        uint8_t pad[3] = {0, 0, 0};

        for (x = 0; r == FM_OK && x < img->width; x++)
        {
            uint8_t pixel[3];
            uint8_t red, green, blue;
            uint32_t source_y = img->height - 1u - y;

            image_pixel_rgb(img, (size_t)source_y * img->width + x,
                            &red, &green, &blue);
            pixel[0] = blue; pixel[1] = green; pixel[2] = red;
            r = image_file_write_exact(&file, pixel, sizeof(pixel));
        }
        if (r == FM_OK)
        {
            r = image_file_write_exact(&file, pad, row_stride - img->width * 3u);
        }
    }
    {
        FM_Result c = File_Close(&file);
        if (r == FM_OK) r = c;
    }
    if (r != FM_OK)
    {
        (void)File_Delete(path);         /* no truncated BMP left behind */
    }
    return r;
}

FM_Result Image_DecodeBMP(const void *bmp_data, size_t bmp_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity)
{
    FM_Stream stream;
    FM_Result r;

    if (bmp_data == NULL || bmp_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Stream_InitMemoryRead(&stream, bmp_data, bmp_size);
    if (r != FM_OK)
    {
        return r;
    }
    r = image_load_stream(&stream, img, pixel_buffer, capacity, 0, 0);
    Stream_Close(&stream);
    return r;
}

FM_Result Image_StreamDecode(FM_Stream *s, FM_Image *img,
                             FM_ImageLineCb line_cb, void *user_data,
                             FM_ProgressCallback progress)
{
    FM_ImageDecoder dec;
    FM_ImageInfo info;
    uint8_t *line;
    uint8_t internal[FM_INTERNAL_BUFFER_SIZE];
    size_t row_bytes;
    uint32_t y;
    FM_Result r;

    if (s == NULL || img == NULL || line_cb == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Image_OpenStream(s, &dec, &info);
    if (r != FM_OK)
    {
        return r;
    }
    row_bytes = (size_t)info.width * info.channels;
    /* The picture's geometry is reported in both modes.  It used to be
     * filled in only when the pixels were kept in img->data, so a
     * callback-only caller (img->data == NULL: the whole point of
     * streaming) got rows without knowing how wide they were. */
    img->width = info.width;
    img->height = info.height;
    img->format = (info.channels == 3) ? FM_PIXEL_RGB888 : FM_PIXEL_GRAY8;
    img->bit_depth = info.bit_depth;
    img->channels = info.channels;
    img->compression = info.compression;
    (void)strcpy(img->codec, info.codec);
    if (img->data != NULL && info.data_size <= img->data_capacity)
    {
        img->data_size = info.data_size;
        line = img->data;
    }
    else
    {
        if (row_bytes > sizeof(internal))
        {
            Image_CloseDecoder(&dec);
            return FM_ERR_TOO_LARGE;
        }
        img->data_size = 0u;                 /* nothing stored in img */
        line = internal;
    }
    for (y = 0; y < info.height; y++)
    {
        r = Image_ReadLine(&dec, line, row_bytes);
        if (r != FM_OK)
        {
            Image_CloseDecoder(&dec);
            return r;
        }
        line_cb(line, y, user_data);
        if (progress != NULL)
        {
            progress((uint64_t)y + 1u, info.height, user_data);
        }
        if (line == img->data)
        {
            line += row_bytes;
        }
    }
    Image_CloseDecoder(&dec);
    return FM_OK;
}

FM_Result Image_ConvertRGB565to888(const void *src, void *dst,
                                   size_t pixel_count)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t i;

    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < pixel_count; i++)
    {
        uint16_t v = (uint16_t)in[i * 2u] |
                     (uint16_t)((uint16_t)in[i * 2u + 1u] << 8);
        out[i * 3u] = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
        out[i * 3u + 1u] = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        out[i * 3u + 2u] = (uint8_t)((v & 0x1Fu) * 255u / 31u);
    }
    return FM_OK;
}

FM_Result Image_ConvertRGB888to565(const void *src, void *dst,
                                   size_t pixel_count)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t i;

    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < pixel_count; i++)
    {
        uint16_t v = (uint16_t)(((uint16_t)in[i * 3u] >> 3) << 11) |
                     (uint16_t)(((uint16_t)in[i * 3u + 1u] >> 2) << 5) |
                     (uint16_t)(in[i * 3u + 2u] >> 3);
        out[i * 2u] = (uint8_t)(v & 0xFFu);
        out[i * 2u + 1u] = (uint8_t)(v >> 8);
    }
    return FM_OK;
}

FM_Result Image_ConvertRGB888toGray(const void *src, void *dst,
                                    size_t pixel_count)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t i;

    if (src == NULL || dst == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < pixel_count; i++)
    {
        out[i] = (uint8_t)(((uint32_t)in[i * 3u] * 77u +
                             (uint32_t)in[i * 3u + 1u] * 150u +
                             (uint32_t)in[i * 3u + 2u] * 29u) >> 8);
    }
    return FM_OK;
}

FM_Result Image_Convert(const FM_Image *src, FM_Image *dst,
                        FM_PixelFormat dst_format)
{
    size_t pixels;
    size_t source_needed;
    size_t needed;
    size_t source_bpp;
    size_t destination_bpp;
    size_t i;

    if (src == NULL || dst == NULL || src->data == NULL || dst->data == NULL ||
        src->width == 0 || src->height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    source_bpp = Image_BytesPerPixel(src->format);
    destination_bpp = Image_BytesPerPixel(dst_format);
    if (source_bpp == 0 || destination_bpp == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    pixels = (size_t)src->width * src->height;
    if (pixels > SIZE_MAX / source_bpp ||
        pixels > SIZE_MAX / destination_bpp)
    {
        return FM_ERR_TOO_LARGE;
    }
    source_needed = pixels * source_bpp;
    needed = pixels * destination_bpp;
    if (source_needed > src->data_capacity ||
        (src->data_size != 0 && source_needed > src->data_size) ||
        needed > dst->data_capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (src->format == dst_format)
    {
        memmove(dst->data, src->data, needed);
    }
    else
    {
        uint8_t *out = dst->data;

        if (src->data == dst->data)
        {
            return FM_ERR_INVALID_ARGUMENT;
        }
        for (i = 0; i < pixels; i++)
        {
            uint8_t r, g, b, a;

            if ((i & 4095u) == 0u)
            {
                FM_Idle();
            }
            image_pixel_rgba(src, i, &r, &g, &b, &a);
            switch (dst_format)
            {
            case FM_PIXEL_GRAY8:
                out[i] = (uint8_t)(((uint32_t)r * 77u +
                                    (uint32_t)g * 150u +
                                    (uint32_t)b * 29u) >> 8);
                break;
            case FM_PIXEL_RGB565:
            case FM_PIXEL_BGR565:
            {
                uint8_t high = (dst_format == FM_PIXEL_RGB565) ? r : b;
                uint8_t low = (dst_format == FM_PIXEL_RGB565) ? b : r;
                uint16_t value =
                    (uint16_t)(((uint16_t)high >> 3) << 11) |
                    (uint16_t)(((uint16_t)g >> 2) << 5) |
                    (uint16_t)(low >> 3);

                out[i * 2u] = (uint8_t)(value & 0xFFu);
                out[i * 2u + 1u] = (uint8_t)(value >> 8);
                break;
            }
            case FM_PIXEL_RGB888:
            case FM_PIXEL_BGR888:
                if (dst_format == FM_PIXEL_RGB888)
                {
                    out[i * 3u] = r; out[i * 3u + 1u] = g;
                    out[i * 3u + 2u] = b;
                }
                else
                {
                    out[i * 3u] = b; out[i * 3u + 1u] = g;
                    out[i * 3u + 2u] = r;
                }
                break;
            case FM_PIXEL_RGBA8888:
            case FM_PIXEL_BGRA8888:
                if (dst_format == FM_PIXEL_RGBA8888)
                {
                    out[i * 4u] = r; out[i * 4u + 1u] = g;
                    out[i * 4u + 2u] = b;
                }
                else
                {
                    out[i * 4u] = b; out[i * 4u + 1u] = g;
                    out[i * 4u + 2u] = r;
                }
                out[i * 4u + 3u] = a;
                break;
            default:
                return FM_ERR_UNSUPPORTED;
            }
        }
    }
    dst->width = src->width;
    dst->height = src->height;
    dst->format = dst_format;
    dst->channels = (uint8_t)destination_bpp;
    dst->bit_depth = (uint8_t)(destination_bpp * 8u);
    dst->data_size = needed;
    (void)strcpy(dst->codec, "RAW");
    return FM_OK;
}

static uint8_t image_effect_clamp(int32_t value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static void image_effect_set_pixel(FM_Image *img, size_t index,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p;

    if (img->format == FM_PIXEL_RGB888 ||
        img->format == FM_PIXEL_BGR888)
    {
        p = img->data + index * 3u;
        if (img->format == FM_PIXEL_RGB888)
        {
            p[0] = r; p[1] = g; p[2] = b;
        }
        else
        {
            p[0] = b; p[1] = g; p[2] = r;
        }
    }
    else if (img->format == FM_PIXEL_RGB565 ||
             img->format == FM_PIXEL_BGR565)
    {
        uint8_t high = (img->format == FM_PIXEL_RGB565) ? r : b;
        uint8_t low = (img->format == FM_PIXEL_RGB565) ? b : r;
        uint16_t v = (uint16_t)(((uint16_t)high >> 3) << 11) |
                     (uint16_t)(((uint16_t)g >> 2) << 5) |
                     (uint16_t)(low >> 3);

        p = img->data + index * 2u;
        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)(v >> 8);
    }
    else if (img->format == FM_PIXEL_RGBA8888 ||
             img->format == FM_PIXEL_BGRA8888)
    {
        p = img->data + index * 4u;
        if (img->format == FM_PIXEL_RGBA8888)
        {
            p[0] = r; p[1] = g; p[2] = b;
        }
        else
        {
            p[0] = b; p[1] = g; p[2] = r;
        }
        /* p[3] alpha remains unchanged. */
    }
    else
    {
        img->data[index] = (uint8_t)(((uint32_t)r * 77u +
                                      (uint32_t)g * 150u +
                                      (uint32_t)b * 29u) >> 8);
    }
}

FM_Result Image_ApplyEffect(FM_Image *img, FM_ImageEffect effect,
                            int32_t amount)
{
    size_t pixels;
    size_t required;
    size_t bpp;
    size_t i;

    if (img == NULL || img->data == NULL || img->width == 0 ||
        img->height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bpp = Image_BytesPerPixel(img->format);
    if (bpp == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (effect < FM_IMAGE_EFFECT_INVERT ||
        effect > FM_IMAGE_EFFECT_CONTRAST)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (effect == FM_IMAGE_EFFECT_BRIGHTNESS &&
        (amount < -255 || amount > 255))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (effect == FM_IMAGE_EFFECT_CONTRAST &&
        (amount < -100 || amount > 100))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (effect == FM_IMAGE_EFFECT_SEPIA && img->format == FM_PIXEL_GRAY8)
    {
        return FM_ERR_UNSUPPORTED;
    }

    pixels = (size_t)img->width * img->height;
    if (pixels > SIZE_MAX / bpp)
    {
        return FM_ERR_TOO_LARGE;
    }
    required = pixels * bpp;
    if (required > img->data_capacity ||
        (img->data_size != 0 && required > img->data_size))
    {
        return FM_ERR_TOO_LARGE;
    }

    for (i = 0; i < pixels; i++)
    {
        uint8_t r8, g8, b8;
        int32_t r;
        int32_t g;
        int32_t b;

        image_pixel_rgb(img, i, &r8, &g8, &b8);
        r = r8; g = g8; b = b8;

        switch (effect)
        {
        case FM_IMAGE_EFFECT_INVERT:
            r = 255 - r; g = 255 - g; b = 255 - b;
            break;
        case FM_IMAGE_EFFECT_GRAYSCALE:
        {
            int32_t y = (r * 77 + g * 150 + b * 29) >> 8;

            r = y; g = y; b = y;
            break;
        }
        case FM_IMAGE_EFFECT_SEPIA:
        {
            int32_t sr = (r * 393 + g * 769 + b * 189) / 1000;
            int32_t sg = (r * 349 + g * 686 + b * 168) / 1000;
            int32_t sb = (r * 272 + g * 534 + b * 131) / 1000;

            r = sr; g = sg; b = sb;
            break;
        }
        case FM_IMAGE_EFFECT_BRIGHTNESS:
            r += amount; g += amount; b += amount;
            break;
        case FM_IMAGE_EFFECT_CONTRAST:
            r = 128 + (r - 128) * (100 + amount) / 100;
            g = 128 + (g - 128) * (100 + amount) / 100;
            b = 128 + (b - 128) * (100 + amount) / 100;
            break;
        default:
            return FM_ERR_INVALID_ARGUMENT;
        }

        image_effect_set_pixel(img, i, image_effect_clamp(r),
                               image_effect_clamp(g), image_effect_clamp(b));
    }
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* Scaling / resizing                                                  */
/* ------------------------------------------------------------------ */
static void image_scale_get(const uint8_t *p, FM_PixelFormat fmt,
                            size_t idx, int *r, int *g, int *b, int *a)
{
    FM_Image image;
    uint8_t r8, g8, b8, a8;

    memset(&image, 0, sizeof(image));
    image.format = fmt;
    image.data = (uint8_t *)p;
    image_pixel_rgba(&image, idx, &r8, &g8, &b8, &a8);
    *r = r8; *g = g8; *b = b8; *a = a8;
}

static void image_scale_set(uint8_t *p, FM_PixelFormat fmt, size_t idx,
                            int r, int g, int b, int a)
{
    switch (fmt)
    {
    case FM_PIXEL_GRAY8:
        p[idx] = (uint8_t)(((r * 77) + (g * 150) + (b * 29)) >> 8);
        break;
    case FM_PIXEL_RGB565:
    case FM_PIXEL_BGR565:
    {
        int high = (fmt == FM_PIXEL_RGB565) ? r : b;
        int low = (fmt == FM_PIXEL_RGB565) ? b : r;
        uint16_t v = (uint16_t)(((uint16_t)(high >> 3) << 11) |
                                ((uint16_t)(g >> 2) << 5) |
                                (uint16_t)(low >> 3));
        p[idx * 2u] = (uint8_t)(v & 0xFFu);
        p[idx * 2u + 1u] = (uint8_t)(v >> 8);
        break;
    }
    case FM_PIXEL_RGBA8888:
    case FM_PIXEL_BGRA8888:
        if (fmt == FM_PIXEL_RGBA8888)
        {
            p[idx * 4u] = (uint8_t)r;
            p[idx * 4u + 1u] = (uint8_t)g;
            p[idx * 4u + 2u] = (uint8_t)b;
        }
        else
        {
            p[idx * 4u] = (uint8_t)b;
            p[idx * 4u + 1u] = (uint8_t)g;
            p[idx * 4u + 2u] = (uint8_t)r;
        }
        p[idx * 4u + 3u] = (uint8_t)a;
        break;
    case FM_PIXEL_RGB888:
    case FM_PIXEL_BGR888:
        if (fmt == FM_PIXEL_RGB888)
        {
            p[idx * 3u] = (uint8_t)r;
            p[idx * 3u + 1u] = (uint8_t)g;
            p[idx * 3u + 2u] = (uint8_t)b;
        }
        else
        {
            p[idx * 3u] = (uint8_t)b;
            p[idx * 3u + 1u] = (uint8_t)g;
            p[idx * 3u + 2u] = (uint8_t)r;
        }
        break;
    default:
        break;
    }
}

static FM_Result image_scale_run(const void *src, uint16_t sw, uint16_t sh,
                                 FM_PixelFormat fmt, void *dst, uint16_t dw,
                                 uint16_t dh, FM_ScaleMode mode)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    uint32_t y;
    uint32_t x;

    if (src == NULL || dst == NULL || sw == 0 || sh == 0 || dw == 0 ||
        dh == 0 || Image_BytesPerPixel(fmt) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    if (mode == FM_SCALE_NEAREST)
    {
        size_t bpp = Image_BytesPerPixel(fmt);

        /*
         * مسیر سریع: کپی مستقیم پیکسل، بدون رمزگشایی رنگ.
         *
         * حالت عمومی پایین برای هر پیکسل دو فراخوانی تابع با یک
         * switch روی فرمت انجام می‌دهد و کانال‌ها را باز و بسته
         * می‌کند. ولی nearest اصلاً رنگ را تغییر نمی‌دهد — فقط
         * پیکسل را جابه‌جا می‌کند. پس وقتی مبدأ و مقصد یک فرمت
         * دارند، کافی است همان چند بایت را کپی کنیم.
         *
         * روی کوچک‌کردن ۷۲۰×۴۸۰ به ۳۲۰×۲۴۰ این تفاوت چشمگیر است:
         * ۷۶۸۰۰ پیکسل × دو فراخوانی و یک switch، تبدیل می‌شود به
         * ۷۶۸۰۰ کپی چندبایتی.
         *
         * ضریب‌های افقی از قبل حساب می‌شوند چون برای همهٔ سطرها
         * یکسان‌اند؛ یک تقسیم به‌ازای هر پیکسل حذف می‌شود.
         */
        if (bpp >= 1u && bpp <= 4u)
        {
            const uint8_t *sp = (const uint8_t *)in;
            uint8_t *dp = (uint8_t *)out;
            uint16_t xi;

            {
                uint16_t xmap[512];
                uint16_t *xs = xmap;
                uint16_t nmap = (dw <= 512u) ? dw : 0u;

                if (nmap != 0u)
                {
                    for (xi = 0; xi < dw; xi++)
                    {
                        xmap[xi] = (uint16_t)((uint32_t)xi * sw / dw);
                    }
                }
                for (y = 0; y < dh; y++)
                {
                    uint32_t sy = (uint32_t)y * sh / dh;
                    const uint8_t *srow = sp + (size_t)sy * sw * bpp;
                    uint8_t *drow = dp + (size_t)y * dw * bpp;

                    if ((y & 7u) == 0u)
                    {
                        FM_Idle();
                    }
                    for (xi = 0; xi < dw; xi++)
                    {
                        uint32_t sx = (nmap != 0u)
                                          ? (uint32_t)xs[xi]
                                          : ((uint32_t)xi * sw / dw);
                        const uint8_t *s2 = srow + (size_t)sx * bpp;
                        uint8_t *d2 = drow + (size_t)xi * bpp;

                        if (bpp == 3u)
                        {
                            d2[0] = s2[0];
                            d2[1] = s2[1];
                            d2[2] = s2[2];
                        }
                        else if (bpp == 2u)
                        {
                            d2[0] = s2[0];
                            d2[1] = s2[1];
                        }
                        else if (bpp == 4u)
                        {
                            d2[0] = s2[0];
                            d2[1] = s2[1];
                            d2[2] = s2[2];
                            d2[3] = s2[3];
                        }
                        else
                        {
                            d2[0] = s2[0];
                        }
                    }
                }
            }
            return FM_OK;
        }
        for (y = 0; y < dh; y++)
        {
            uint32_t sy = (uint32_t)y * sh / dh;

            if ((y & 7u) == 0u)
            {
                FM_Idle();
            }
            for (x = 0; x < dw; x++)
            {
                uint32_t sx = (uint32_t)x * sw / dw;
                int r, g, b, a;

                image_scale_get(in, fmt, (size_t)sy * sw + sx,
                                &r, &g, &b, &a);
                image_scale_set(out, fmt, (size_t)y * dw + x, r, g, b, a);
            }
        }
    }
    else
    {
        for (y = 0; y < dh; y++)
        {
            /* Where this destination row lies in the source, in 1/256
             * of a source pixel.  Pixel centres map onto pixel centres:
             * source = (y + 1/2) * sh / dh - 1/2.  The weight of the
             * lower row is the fraction, 0..255.  (This used to be
             * computed in half pixels with a weight of 0 or 1, which
             * picks one of the four neighbours: the result was the
             * nearest pixel under the name "bilinear".) */
            uint64_t vy = (((uint64_t)y * 2u + 1u) * sh * 128u) / dh;
            uint64_t fy = (vy < 128u) ? 0u : (vy - 128u);
            uint32_t y1 = (uint32_t)(fy / 256u);
            uint32_t y2 = y1 + 1u;
            uint32_t yw = (uint32_t)(fy & 255u);

            if ((y & 7u) == 0u)
            {
                FM_Idle();
            }

            if (y1 >= sh) y1 = sh - 1;      /* the edge is repeated */
            if (y2 >= sh) y2 = sh - 1;

            for (x = 0; x < dw; x++)
            {
                uint64_t vx = (((uint64_t)x * 2u + 1u) * sw * 128u) / dw;
                uint64_t fx = (vx < 128u) ? 0u : (vx - 128u);
                uint32_t x1 = (uint32_t)(fx / 256u);
                uint32_t x2 = x1 + 1u;
                uint32_t xw = (uint32_t)(fx & 255u);
                int r00, g00, b00, a00, r01, g01, b01, a01;
                int r10, g10, b10, a10, r11, g11, b11, a11;
                int r, g, b, a;

                if (x1 >= sw) x1 = sw - 1;
                if (x2 >= sw) x2 = sw - 1;

                image_scale_get(in, fmt, (size_t)y1 * sw + x1,
                                &r00, &g00, &b00, &a00);
                image_scale_get(in, fmt, (size_t)y1 * sw + x2,
                                &r01, &g01, &b01, &a01);
                image_scale_get(in, fmt, (size_t)y2 * sw + x1,
                                &r10, &g10, &b10, &a10);
                image_scale_get(in, fmt, (size_t)y2 * sw + x2,
                                &r11, &g11, &b11, &a11);

                /* the four neighbours, weighted in 1/65536 */
#define SCALE_MIX(v00, v01, v10, v11)                                  \
    ((int)(((uint32_t)(v00) * (256u - xw) * (256u - yw) +              \
            (uint32_t)(v01) * xw * (256u - yw) +                       \
            (uint32_t)(v10) * (256u - xw) * yw +                       \
            (uint32_t)(v11) * xw * yw + 32768u) >> 16))
                r = SCALE_MIX(r00, r01, r10, r11);
                g = SCALE_MIX(g00, g01, g10, g11);
                b = SCALE_MIX(b00, b01, b10, b11);
                a = SCALE_MIX(a00, a01, a10, a11);
#undef SCALE_MIX

                image_scale_set(out, fmt, (size_t)y * dw + x, r, g, b, a);
            }
        }
    }
    return FM_OK;
}

/* Scaling indexes both buffers from the declared dimensions alone, so
 * the dimensions have to be proven against the real buffer sizes first.
 * `src_cap` / `dst_cap` are the bytes available behind `src` / `dst`. */
FM_Result Image_ScaleEx(const void *src, size_t src_cap,
                        uint16_t sw, uint16_t sh, FM_PixelFormat fmt,
                        void *dst, size_t dst_cap,
                        uint16_t dw, uint16_t dh, FM_ScaleMode mode)
{
    size_t bpp = Image_BytesPerPixel(fmt);
    size_t src_need = 0;
    size_t dst_need = 0;
    FM_Result r;

    if (src == NULL || dst == NULL || sw == 0 || sh == 0 || dw == 0 ||
        dh == 0 || bpp == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (mode != FM_SCALE_NEAREST && mode != FM_SCALE_BILINEAR)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = FM_Image_Bytes(sw, sh, bpp, &src_need);
    if (r == FM_OK)
    {
        r = FM_Image_Bytes(dw, dh, bpp, &dst_need);
    }
    if (r != FM_OK)
    {
        return r;
    }
    if (src_need > src_cap || dst_need > dst_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    return image_scale_run(src, sw, sh, fmt, dst, dw, dh, mode);
}

/* Legacy entry point without capacities: the caller vouches that both
 * buffers hold width*height*bpp bytes.  Prefer Image_ScaleEx. */
FM_Result Image_Scale(const void *src, uint16_t sw, uint16_t sh,
                      FM_PixelFormat fmt, void *dst, uint16_t dw, uint16_t dh,
                      FM_ScaleMode mode)
{
    size_t bpp = Image_BytesPerPixel(fmt);
    size_t n = 0;

    if (src == NULL || dst == NULL || sw == 0 || sh == 0 || dw == 0 ||
        dh == 0 || bpp == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    /* at least refuse sizes the address space cannot hold */
    if (FM_Image_Bytes(sw, sh, bpp, &n) != FM_OK ||
        FM_Image_Bytes(dw, dh, bpp, &n) != FM_OK)
    {
        return FM_ERR_TOO_LARGE;
    }
    return image_scale_run(src, sw, sh, fmt, dst, dw, dh, mode);
}

FM_Result Image_ResizeFile(const char *src_path, const char *dst_path,
                           uint16_t dw, uint16_t dh, FM_ScaleMode mode)
{
    FM_Image src;
    FM_Image rgb;
    FM_Image out;
    FM_ImageInfo info;
    uint8_t *src_buf = NULL;
    uint8_t *rgb_buf = NULL;
    uint8_t *dst_buf = NULL;
    size_t src_bytes = 0;
    size_t dst_bytes = 0;
    FM_Result r;

    if (src_path == NULL || dst_path == NULL || dw == 0 || dh == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&src, 0, sizeof(src));
    memset(&rgb, 0, sizeof(rgb));
    memset(&out, 0, sizeof(out));

    r = Image_GetInfo(src_path, &info);
    if (r != FM_OK)
    {
        return r;
    }
    if (info.width == 0 || info.height == 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    /* Sizes from a file header: checked 64-bit products, never a bare
     * size_t multiply (65535 x 65535 x 3 wraps on a 32-bit target). */
    r = FM_Image_Bytes(info.width, info.height, 3u, &src_bytes);
    if (r == FM_OK)
    {
        r = FM_Image_Bytes(dw, dh, 3u, &dst_bytes);
    }
    if (r != FM_OK)
    {
        return r;
    }
    src_buf = (uint8_t *)FM_Alloc(src_bytes);
    if (src_buf == NULL)
    {
        return FM_ERR_NO_MEMORY;
    }
    r = Image_Load(src_path, &src, src_buf, src_bytes);
    if (r != FM_OK)
    {
        FM_Free(src_buf);
        return r;
    }
    if (src.format != FM_PIXEL_RGB888)
    {
        rgb_buf = (uint8_t *)FM_Alloc(src_bytes);
        if (rgb_buf == NULL)
        {
            FM_Free(src_buf);
            return FM_ERR_NO_MEMORY;
        }
        memset(&rgb, 0, sizeof(rgb));
        rgb.data = rgb_buf;
        rgb.data_capacity = src_bytes;
        r = Image_Convert(&src, &rgb, FM_PIXEL_RGB888);
        if (r != FM_OK)
        {
            FM_Free(src_buf);
            FM_Free(rgb_buf);
            return r;
        }
    }
    else
    {
        rgb = src;
    }

    dst_buf = (uint8_t *)FM_Alloc(dst_bytes);
    if (dst_buf == NULL)
    {
        FM_Free(src_buf);
        if (rgb_buf != NULL) FM_Free(rgb_buf);
        return FM_ERR_NO_MEMORY;
    }
    r = Image_ScaleEx(rgb.data, src_bytes, rgb.width, rgb.height,
                      FM_PIXEL_RGB888, dst_buf, dst_bytes, dw, dh, mode);
    if (r != FM_OK)
    {
        FM_Free(src_buf);
        if (rgb_buf != NULL) FM_Free(rgb_buf);
        FM_Free(dst_buf);
        return r;
    }
    out.width = dw;
    out.height = dh;
    out.format = FM_PIXEL_RGB888;
    out.channels = 3;
    out.bit_depth = 24;
    out.data = dst_buf;
    out.data_capacity = dst_bytes;
    out.data_size = dst_bytes;
    (void)strcpy(out.codec, "RAW");
    r = Image_Save(dst_path, &out);

    FM_Free(src_buf);
    if (rgb_buf != NULL) FM_Free(rgb_buf);
    FM_Free(dst_buf);
    return r;
}

static void image_copy_pixel(uint8_t *dst, const uint8_t *src, size_t bpp)
{
    size_t i;

    for (i = 0; i < bpp; i++)
    {
        dst[i] = src[i];
    }
}

FM_Result Image_Flip(FM_Image *img, FM_ImageFlip axis)
{
    size_t bpp;
    uint16_t x;
    uint16_t y;
    FM_Result rc;

    if (img == NULL || img->data == NULL || img->width == 0 ||
        img->height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bpp = Image_BytesPerPixel(img->format);
    if (bpp == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    rc = FM_Image_CheckSource(img, NULL);
    if (rc != FM_OK)
    {
        return rc;
    }

    if (axis == FM_IMAGE_FLIP_HORIZONTAL)
    {
        uint8_t tmp[4];

        for (y = 0; y < img->height; y++)
        {
            uint8_t *row = img->data + (size_t)y * img->width * bpp;

            for (x = 0; x < img->width / 2u; x++)
            {
                uint8_t *a = row + (size_t)x * bpp;
                uint8_t *b = row + (size_t)(img->width - 1u - x) * bpp;

                image_copy_pixel(tmp, a, bpp);
                image_copy_pixel(a, b, bpp);
                image_copy_pixel(b, tmp, bpp);
            }
        }
        return FM_OK;
    }
    if (axis == FM_IMAGE_FLIP_VERTICAL)
    {
        uint8_t tmp[4];
        uint16_t w = img->width;

        for (y = 0; y < img->height / 2u; y++)
        {
            uint8_t *top = img->data + (size_t)y * w * bpp;
            uint8_t *bot = img->data +
                           (size_t)(img->height - 1u - y) * w * bpp;

            for (x = 0; x < w; x++)
            {
                image_copy_pixel(tmp, top + (size_t)x * bpp, bpp);
                image_copy_pixel(top + (size_t)x * bpp,
                                 bot + (size_t)x * bpp, bpp);
                image_copy_pixel(bot + (size_t)x * bpp, tmp, bpp);
            }
        }
        return FM_OK;
    }
    return FM_ERR_INVALID_ARGUMENT;
}

FM_Result Image_Rotate(const FM_Image *src, FM_Image *dst, int degrees)
{
    size_t bpp;
    size_t needed;
    uint16_t dw;
    uint16_t dh;
    uint16_t x;
    uint16_t y;
    int turn;

    if (src == NULL || dst == NULL || src->data == NULL ||
        dst->data == NULL || src->width == 0 || src->height == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    bpp = Image_BytesPerPixel(src->format);
    if (bpp == 0)
    {
        return FM_ERR_UNSUPPORTED;
    }
    turn = degrees % 360;
    if (turn < 0)
    {
        turn += 360;
    }
    if (turn != 0 && turn != 90 && turn != 180 && turn != 270)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    dw = (turn == 90 || turn == 270) ? src->height : src->width;
    dh = (turn == 90 || turn == 270) ? src->width : src->height;
    /* The source is read through width/height just like the
     * destination is written: validate it too (capacity AND data_size),
     * a forged FM_Image used to be read far past its buffer. */
    {
        FM_Result rc = FM_Image_CheckSource(src, &needed);

        if (rc != FM_OK)
        {
            return rc;
        }
    }
    if (needed > dst->data_capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    if (src->data == dst->data && turn != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    if (turn == 0)
    {
        if (src->data != dst->data)
        {
            memmove(dst->data, src->data, needed);
        }
    }
    else
    {
        const uint8_t *in = src->data;
        uint8_t *out = dst->data;
        uint16_t sw = src->width;
        uint16_t sh = src->height;

        for (y = 0; y < sh; y++)
        {
            for (x = 0; x < sw; x++)
            {
                uint16_t dx;
                uint16_t dy;

                if (turn == 90)
                {
                    dx = (uint16_t)(sh - 1u - y);
                    dy = x;
                }
                else if (turn == 180)
                {
                    dx = (uint16_t)(sw - 1u - x);
                    dy = (uint16_t)(sh - 1u - y);
                }
                else
                {
                    dx = y;
                    dy = (uint16_t)(sw - 1u - x);
                }
                image_copy_pixel(out + ((size_t)dy * dw + dx) * bpp,
                                 in + ((size_t)y * sw + x) * bpp, bpp);
            }
        }
    }

    dst->width = dw;
    dst->height = dh;
    dst->format = src->format;
    dst->channels = src->channels;
    dst->bit_depth = src->bit_depth;
    dst->compression = src->compression;
    dst->data_size = needed;
    (void)strcpy(dst->codec, src->codec[0] != '\0' ? src->codec : "RAW");
    return FM_OK;
}


typedef struct
{
    char codec[8];
    const FM_ImageCodecOps *ops;
} ImageCodecEntry;

static ImageCodecEntry g_icodec[4];

FM_Result Image_RegisterCodec(const char *codec, const FM_ImageCodecOps *ops)
{
    uint32_t i;

    if (codec == NULL || codec[0] == '\0' || ops == NULL ||
        ops->decode == NULL || strlen(codec) >= 8u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    FM_Lock();
    /* Replace an existing registration first.  A single first-fit pass
     * took the first FREE slot even when the name was registered in a
     * later one, leaving two entries for one codec (and Unregister then
     * removed only the newer). */
    for (i = 0; i < 4u; i++)
    {
        if (g_icodec[i].codec[0] != '\0' &&
            strcmp(g_icodec[i].codec, codec) == 0)
        {
            g_icodec[i].ops = ops;
            FM_Unlock();
            return FM_OK;
        }
    }
    for (i = 0; i < 4u; i++)
    {
        if (g_icodec[i].codec[0] == '\0')
        {
            (void)strcpy(g_icodec[i].codec, codec);
            g_icodec[i].ops = ops;
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NO_MEMORY;
}

FM_Result Image_UnregisterCodec(const char *codec)
{
    uint32_t i;

    if (codec == NULL || codec[0] == '\0')
    {
        return FM_ERR_INVALID_ARGUMENT;  /* "" would match a free slot */
    }
    FM_Lock();
    for (i = 0; i < 4u; i++)
    {
        if (g_icodec[i].codec[0] != '\0' &&
            strcmp(g_icodec[i].codec, codec) == 0)
        {
            memset(&g_icodec[i], 0, sizeof(g_icodec[i]));
            FM_Unlock();
            return FM_OK;
        }
    }
    FM_Unlock();
    return FM_ERR_NOT_FOUND;
}

/* Looked up under the same lock Register/Unregister write under.  The
 * returned ops must outlive their registration: keep them in static
 * storage, and do not unregister while a decode may still be running. */
const FM_ImageCodecOps *Image_GetCodec(const char *codec)
{
    const FM_ImageCodecOps *ops = NULL;
    uint32_t i;

    if (codec == NULL || codec[0] == '\0')
    {
        return NULL;
    }
    FM_Lock();
    for (i = 0; i < 4u; i++)
    {
        if (g_icodec[i].codec[0] != '\0' &&
            strcmp(g_icodec[i].codec, codec) == 0)
        {
            ops = g_icodec[i].ops;
            break;
        }
    }
    FM_Unlock();
    return ops;
}

#else /* !FM_USE_IMAGE */

/* Image support is compiled out: every public entry point still links
 * and reports FM_ERR_UNSUPPORTED (or the neutral value), with no static
 * storage behind it -- the same contract as FM_USE_VIDEO / FM_USE_AUDIO
 * = 0.  The JPEG / PNG entry points have their stubs in fm_jpeg.c and
 * fm_png.c. */

FM_Result Image_Load(const char *path, FM_Image *img, void *pixel_buffer,
                     size_t capacity)
{
    (void)path;
    (void)img;
    (void)pixel_buffer;
    (void)capacity;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_LoadEx(const char *path, FM_Image *img, void *pixel_buffer,
                       size_t capacity, uint16_t max_w, uint16_t max_h)
{
    (void)path;
    (void)img;
    (void)pixel_buffer;
    (void)capacity;
    (void)max_w;
    (void)max_h;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_Save(const char *path, const FM_Image *img)
{
    (void)path;
    (void)img;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_GetInfo(const char *path, FM_ImageInfo *info)
{
    (void)path;
    (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_SetWorkBuffer(void *buf, size_t size)
{
    (void)buf;
    (void)size;
    return FM_ERR_UNSUPPORTED;
}

void *Image_GetWorkBuffer(size_t *size)
{
    if (size != NULL)
    {
        *size = 0u;
    }
    return NULL;
}

FM_Result Image_DecodeBMP(const void *bmp_data, size_t bmp_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity)
{
    (void)bmp_data;
    (void)bmp_size;
    (void)img;
    (void)pixel_buffer;
    (void)capacity;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_EncodeBMP(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len)
{
    (void)img;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_RegisterCodec(const char *codec, const FM_ImageCodecOps *ops)
{
    (void)codec;
    (void)ops;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_UnregisterCodec(const char *codec)
{
    (void)codec;
    return FM_ERR_UNSUPPORTED;
}

const FM_ImageCodecOps *Image_GetCodec(const char *codec)
{
    (void)codec;
    return NULL;
}

FM_Result Image_OpenStream(FM_Stream *s, FM_ImageDecoder *dec,
                           FM_ImageInfo *info)
{
    (void)s;
    (void)dec;
    (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ReadLine(FM_ImageDecoder *dec, void *line_out,
                         size_t line_cap)
{
    (void)dec;
    (void)line_out;
    (void)line_cap;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_StreamDecode(FM_Stream *s, FM_Image *img,
                             FM_ImageLineCb line_cb, void *user_data,
                             FM_ProgressCallback progress)
{
    (void)s;
    (void)img;
    (void)line_cb;
    (void)user_data;
    (void)progress;
    return FM_ERR_UNSUPPORTED;
}

void Image_CloseDecoder(FM_ImageDecoder *dec)
{
    if (dec != NULL)
    {
        memset(dec, 0, sizeof(*dec));
    }
}

size_t Image_BytesPerPixel(FM_PixelFormat format)
{
    (void)format;
    return 0u;
}

FM_Result Image_BufferSize(uint16_t width, uint16_t height,
                           FM_PixelFormat format, size_t *bytes)
{
    (void)width;
    (void)height;
    (void)format;
    (void)bytes;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_Convert(const FM_Image *src, FM_Image *dst,
                        FM_PixelFormat dst_format)
{
    (void)src;
    (void)dst;
    (void)dst_format;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ConvertRGB565to888(const void *src, void *dst,
                                   size_t pixel_count)
{
    (void)src;
    (void)dst;
    (void)pixel_count;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ConvertRGB888to565(const void *src, void *dst,
                                   size_t pixel_count)
{
    (void)src;
    (void)dst;
    (void)pixel_count;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ConvertRGB888toGray(const void *src, void *dst,
                                    size_t pixel_count)
{
    (void)src;
    (void)dst;
    (void)pixel_count;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ApplyEffect(FM_Image *img, FM_ImageEffect effect,
                            int32_t amount)
{
    (void)img;
    (void)effect;
    (void)amount;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_Scale(const void *src, uint16_t sw, uint16_t sh,
                      FM_PixelFormat fmt, void *dst, uint16_t dw, uint16_t dh,
                      FM_ScaleMode mode)
{
    (void)src;
    (void)sw;
    (void)sh;
    (void)fmt;
    (void)dst;
    (void)dw;
    (void)dh;
    (void)mode;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ScaleEx(const void *src, size_t src_cap, uint16_t sw,
                        uint16_t sh, FM_PixelFormat fmt, void *dst,
                        size_t dst_cap, uint16_t dw, uint16_t dh,
                        FM_ScaleMode mode)
{
    (void)src;
    (void)src_cap;
    (void)sw;
    (void)sh;
    (void)fmt;
    (void)dst;
    (void)dst_cap;
    (void)dw;
    (void)dh;
    (void)mode;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_ResizeFile(const char *src_path, const char *dst_path,
                           uint16_t dw, uint16_t dh, FM_ScaleMode mode)
{
    (void)src_path;
    (void)dst_path;
    (void)dw;
    (void)dh;
    (void)mode;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_Flip(FM_Image *img, FM_ImageFlip axis)
{
    (void)img;
    (void)axis;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_Rotate(const FM_Image *src, FM_Image *dst, int degrees)
{
    (void)src;
    (void)dst;
    (void)degrees;
    return FM_ERR_UNSUPPORTED;
}

#endif /* FM_USE_IMAGE */
