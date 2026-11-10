/*
 * FileManager - PNG still-image decode / encode
 *
 * Inflate (zlib/deflate) is built in so PNG does not pull libpng/zlib.
 * Decode streams IDAT from the file (or a memory blob) through a 32 KB
 * sliding window — the whole file and the filtered image are never
 * malloc'd.  That is what used to return FM_ERR_NO_MEMORY on STM32 when
 * Image_Load slurped camera photos onto the internal heap.
 *
 * Interlaced PNGs and bit depths other than 8 are rejected.
 * Encode writes a valid RGB8 PNG with zlib stored (uncompressed) blocks.
 */

#define FM_DEBUG_TAG "png"
#include "fm_debug.h"
#include "fm_image.h"
#include "fm_internal.h"
#include "fm_crc.h"
#include <string.h>

#if FM_USE_IMAGE && FM_USE_PNG

static uint32_t png_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void png_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#if !FM_USE_CRC
static uint32_t png_crc32(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < len; i++)
    {
        uint8_t j;

        crc ^= p[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 1u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}
#define PNG_CRC_INIT  0xFFFFFFFFu
#define PNG_CRC_FIN(c) ((c) ^ 0xFFFFFFFFu)
#define PNG_CRC_RUN(d, n, c) png_crc32((d), (n), (c))
#else
#define PNG_CRC_INIT  0xFFFFFFFFu
#define PNG_CRC_FIN(c) ((c) ^ 0xFFFFFFFFu)
#define PNG_CRC_RUN(d, n, c) CRC_Compute32((d), (n), (c))
#endif

/* ------------------------------------------------------------------ */
/* Tiny inflate (zlib wrapper + deflate stored/fixed/dynamic)          */
/* ------------------------------------------------------------------ */

typedef struct Inf Inf;

struct Inf
{
    const uint8_t *src;
    size_t src_len;
    size_t src_pos;
    int (*need)(Inf *z);
    void *need_ctx;
    uint32_t bits;
    int nbits;
    uint8_t *window;
    size_t produced;
    uint8_t *row;
    size_t row_len;
    size_t row_fill;
    int (*on_row)(Inf *z);
    void *row_ctx;
};

static int inf_get(Inf *z)
{
    if (z->src_pos >= z->src_len)
    {
        if (z->need == NULL)
        {
            return -1;
        }
        if (z->need(z) != 0)
        {
            return -1;
        }
        if (z->src_pos >= z->src_len)
        {
            return -1;
        }
    }
    return z->src[z->src_pos++];
}

static int inf_bits(Inf *z, int n)
{
    int v;

    while (z->nbits < n)
    {
        int b = inf_get(z);

        if (b < 0)
        {
            return -1;
        }
        z->bits |= (uint32_t)b << z->nbits;
        z->nbits += 8;
    }
    v = (int)(z->bits & ((1u << n) - 1u));
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

static void inf_align(Inf *z)
{
    z->bits = 0;
    z->nbits = 0;
}

typedef struct
{
    uint16_t count[16];
    uint16_t symbol[288];
} Huff;

static int huff_build(Huff *h, const uint8_t *lengths, int n)
{
    int i;
    int len;
    int k;

    memset(h, 0, sizeof(*h));
    for (i = 0; i < n; i++)
    {
        if (lengths[i] > 15)
        {
            return -1;
        }
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;
    k = 0;
    for (len = 1; len <= 15; len++)
    {
        for (i = 0; i < n; i++)
        {
            if (lengths[i] == len)
            {
                h->symbol[k++] = (uint16_t)i;
            }
        }
    }
    return 0;
}

static int huff_decode(Inf *z, const Huff *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    int i;

    for (i = 1; i <= 15; i++)
    {
        int b = inf_bits(z, 1);

        if (b < 0)
        {
            return -1;
        }
        code = (code << 1) | b;
        {
            int count = (int)h->count[i];

            if (code - first < count)
            {
                return (int)h->symbol[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
        }
    }
    return -1;
}

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t k_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inf_put(Inf *z, uint8_t b)
{
    z->window[z->produced & 32767u] = b;
    z->produced++;
    if (z->row != NULL && z->row_len != 0u)
    {
        z->row[z->row_fill++] = b;
        if (z->row_fill >= z->row_len)
        {
            z->row_fill = 0;
            if (z->on_row != NULL && z->on_row(z) != 0)
            {
                return -1;
            }
        }
    }
    return 0;
}

static int inf_copy(Inf *z, int dist, int len)
{
    size_t from;

    if (dist <= 0 || (size_t)dist > z->produced)
    {
        return -1;
    }
    from = z->produced - (size_t)dist;
    while (len-- > 0)
    {
        if (inf_put(z, z->window[from & 32767u]) < 0)
        {
            return -1;
        }
        from++;
    }
    return 0;
}

static int inf_codes(Inf *z, const Huff *lit, const Huff *dist)
{
    for (;;)
    {
        int sym = huff_decode(z, lit);

        if (sym < 0)
        {
            return -1;
        }
        if (sym < 256)
        {
            if (inf_put(z, (uint8_t)sym) < 0)
            {
                return -1;
            }
        }
        else if (sym == 256)
        {
            return 0;
        }
        else
        {
            int len;
            int distv;
            int extra;
            int dsym;

            sym -= 257;
            if (sym >= 29)
            {
                return -1;
            }
            extra = inf_bits(z, (int)k_len_extra[sym]);
            if (extra < 0)
            {
                return -1;
            }
            len = (int)k_len_base[sym] + extra;
            dsym = huff_decode(z, dist);
            if (dsym < 0 || dsym >= 30)
            {
                return -1;
            }
            extra = inf_bits(z, (int)k_dist_extra[dsym]);
            if (extra < 0)
            {
                return -1;
            }
            distv = (int)k_dist_base[dsym] + extra;
            if (inf_copy(z, distv, len) < 0)
            {
                return -1;
            }
        }
    }
}

static int inf_fixed(Inf *z)
{
    uint8_t lengths[288 + 32];
    Huff lit;
    Huff dist;
    int i;

    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    for (i = 0; i < 32; i++) lengths[288 + i] = 5;
    if (huff_build(&lit, lengths, 288) < 0 ||
        huff_build(&dist, lengths + 288, 32) < 0)
    {
        return -1;
    }
    return inf_codes(z, &lit, &dist);
}

static int inf_dynamic(Inf *z)
{
    uint8_t lengths[288 + 32];
    uint8_t clen[19];
    Huff cl;
    Huff lit;
    Huff dist;
    int nlit;
    int ndist;
    int nclen;
    int i;
    int n;

    nlit = inf_bits(z, 5);
    ndist = inf_bits(z, 5);
    nclen = inf_bits(z, 4);
    if (nlit < 0 || ndist < 0 || nclen < 0)
    {
        return -1;
    }
    nlit += 257;
    ndist += 1;
    nclen += 4;
    if (nlit > 288 || ndist > 32)
    {
        return -1;
    }
    memset(clen, 0, sizeof(clen));
    for (i = 0; i < nclen; i++)
    {
        int v = inf_bits(z, 3);

        if (v < 0)
        {
            return -1;
        }
        clen[k_clen_order[i]] = (uint8_t)v;
    }
    if (huff_build(&cl, clen, 19) < 0)
    {
        return -1;
    }
    n = nlit + ndist;
    i = 0;
    while (i < n)
    {
        int sym = huff_decode(z, &cl);
        int rep;
        uint8_t val;

        if (sym < 0)
        {
            return -1;
        }
        if (sym < 16)
        {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16)
        {
            if (i == 0)
            {
                return -1;
            }
            val = lengths[i - 1];
            rep = inf_bits(z, 2);
            if (rep < 0)
            {
                return -1;
            }
            rep += 3;
        }
        else if (sym == 17)
        {
            val = 0;
            rep = inf_bits(z, 3);
            if (rep < 0)
            {
                return -1;
            }
            rep += 3;
        }
        else
        {
            val = 0;
            rep = inf_bits(z, 7);
            if (rep < 0)
            {
                return -1;
            }
            rep += 11;
        }
        if (i + rep > n)
        {
            return -1;
        }
        while (rep-- > 0)
        {
            lengths[i++] = val;
        }
    }
    if (huff_build(&lit, lengths, nlit) < 0 ||
        huff_build(&dist, lengths + nlit, ndist) < 0)
    {
        return -1;
    }
    return inf_codes(z, &lit, &dist);
}

static int inf_stored(Inf *z)
{
    int len;
    int nlen;

    inf_align(z);
    len = inf_get(z);
    len |= inf_get(z) << 8;
    nlen = inf_get(z);
    nlen |= inf_get(z) << 8;
    if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen)
    {
        return -1;
    }
    while (len-- > 0)
    {
        int b = inf_get(z);

        if (b < 0 || inf_put(z, (uint8_t)b) < 0)
        {
            return -1;
        }
    }
    return 0;
}

static FM_Result png_inflate(Inf *z)
{
    int cmf;
    int flg;
    int last;

    cmf = inf_get(z);
    flg = inf_get(z);
    if (cmf < 0 || flg < 0 || (cmf & 0x0F) != 8 ||
        ((cmf * 256 + flg) % 31) != 0 || (flg & 0x20) != 0)
    {
        return FM_ERR_CORRUPTED;
    }
    do
    {
        int bfinal = inf_bits(z, 1);
        int btype = inf_bits(z, 2);
        int rr;

        if (bfinal < 0 || btype < 0)
        {
            return FM_ERR_CORRUPTED;
        }
        last = bfinal;
        if (btype == 0)
        {
            rr = inf_stored(z);
        }
        else if (btype == 1)
        {
            rr = inf_fixed(z);
        }
        else if (btype == 2)
        {
            rr = inf_dynamic(z);
        }
        else
        {
            return FM_ERR_CORRUPTED;
        }
        if (rr < 0)
        {
            return FM_ERR_CORRUPTED;
        }
    } while (!last);
    return FM_OK;
}

/* ------------------------------------------------------------------ */
/* PNG chunks / unfilter                                               */
/* ------------------------------------------------------------------ */

static const uint8_t k_png_sig[8] = {
    137, 80, 78, 71, 13, 10, 26, 10
};

static int png_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p - a;
    int pb = p - b;
    int pc = p - c;

    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc)
    {
        return a;
    }
    if (pb <= pc)
    {
        return b;
    }
    return c;
}

static void png_unfilter(uint8_t *recon, const uint8_t *filt, uint8_t *prev,
                         size_t stride, int bpp)
{
    uint8_t type = filt[0];
    size_t i;

    filt++;
    for (i = 0; i < stride; i++)
    {
        int x = filt[i];
        int a = (i >= (size_t)bpp) ? recon[i - (size_t)bpp] : 0;
        int b = prev[i];
        int c = (i >= (size_t)bpp) ? prev[i - (size_t)bpp] : 0;
        int v;

        switch (type)
        {
        case 0: v = x; break;
        case 1: v = x + a; break;
        case 2: v = x + b; break;
        case 3: v = x + ((a + b) / 2); break;
        case 4: v = x + png_paeth(a, b, c); break;
        default: v = x; break;
        }
        recon[i] = (uint8_t)(v & 255);
    }
}

FM_Result Image_ProbePNG(const void *png, size_t png_size, FM_ImageInfo *info)
{
    const uint8_t *p = (const uint8_t *)png;
    uint32_t w;
    uint32_t h;
    uint8_t depth;
    uint8_t color;

    if (png == NULL || info == NULL || png_size < 33u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (memcmp(p, k_png_sig, 8) != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (png_be32(p + 8) != 13u || memcmp(p + 12, "IHDR", 4) != 0)
    {
        return FM_ERR_CORRUPTED;
    }
    w = png_be32(p + 16);
    h = png_be32(p + 20);
    depth = p[24];
    color = p[25];
    if (w == 0 || h == 0 || w > UINT16_MAX || h > UINT16_MAX || depth != 8u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (color != 0u && color != 2u && color != 3u && color != 6u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    memset(info, 0, sizeof(*info));
    info->width = (uint16_t)w;
    info->height = (uint16_t)h;
    info->channels = (color == 0u) ? 1u : 3u;
    info->bit_depth = (uint8_t)(info->channels * 8u);
    info->data_size = (uint64_t)w * h * info->channels;
    (void)strcpy(info->codec, "PNG");
    return FM_OK;
}

typedef struct
{
    const uint8_t *mem;
    size_t mem_len;
    size_t mem_pos;
    FM_File *file;
} PngRead;

typedef struct
{
    PngRead *in;
    uint32_t idat_left;
    uint32_t crc;
    FM_Bool in_idat;
    FM_Bool ended;
    uint8_t tmp[256];
} PngFeed;

typedef struct
{
    uint8_t *out;
    uint16_t sw;
    uint16_t sh;
    uint16_t dw;
    uint16_t dh;
    uint8_t color;
    int bpp;
    uint8_t palette[256 * 3];
    uint32_t pal_n;
    uint8_t *prev;
    uint8_t *recon;
    uint32_t y;
    FM_Result err;
} PngEmit;

FM_IMAGE_SCRATCH static uint8_t g_png_window[FM_PNG_WINDOW];
FM_IMAGE_SCRATCH static uint8_t g_png_row[FM_PNG_MAX_STRIDE + 1u];
FM_IMAGE_SCRATCH static uint8_t g_png_prev[FM_PNG_MAX_STRIDE];
FM_IMAGE_SCRATCH static uint8_t g_png_recon[FM_PNG_MAX_STRIDE];

static FM_Result png_read(PngRead *r, void *dst, size_t n)
{
    if (n == 0u)
    {
        return FM_OK;
    }
    if (r->file != NULL)
    {
        uint8_t *p = (uint8_t *)dst;
        size_t done = 0;

        if (n > UINT32_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        while (done < n)
        {
            uint32_t rd = 0;
            FM_Result e = File_Read(r->file, p + done, (uint32_t)(n - done),
                                    &rd);

            if (e != FM_OK && e != FM_ERR_EOF)
            {
                return e;
            }
            if (rd == 0u)
            {
                return FM_ERR_EOF;
            }
            done += rd;
        }
        return FM_OK;
    }
    if (r->mem == NULL || r->mem_pos + n > r->mem_len)
    {
        return FM_ERR_EOF;
    }
    memcpy(dst, r->mem + r->mem_pos, n);
    r->mem_pos += n;
    return FM_OK;
}

static FM_Result png_skip(PngRead *r, uint32_t n)
{
    if (r->file != NULL)
    {
        return File_Seek(r->file, (int64_t)n, FM_SEEK_CUR);
    }
    if (r->mem_pos + n > r->mem_len)
    {
        return FM_ERR_EOF;
    }
    r->mem_pos += n;
    return FM_OK;
}

static int png_need(Inf *z)
{
    PngFeed *p = (PngFeed *)z->need_ctx;

    for (;;)
    {
        if (p->ended)
        {
            return -1;
        }
        if (p->in_idat && p->idat_left == 0u)
        {
            uint8_t crc_file[4];
            uint32_t got;

            if (png_read(p->in, crc_file, 4) != FM_OK)
            {
                return -1;
            }
            got = png_be32(crc_file);
            if (got != PNG_CRC_FIN(p->crc))
            {
                return -1;
            }
            p->in_idat = FM_FALSE;
        }
        if (!p->in_idat)
        {
            uint8_t hdr[8];
            uint32_t len;

            if (png_read(p->in, hdr, 8) != FM_OK)
            {
                return -1;
            }
            len = png_be32(hdr);
            if (memcmp(hdr + 4, "IEND", 4) == 0)
            {
                p->ended = FM_TRUE;
                (void)png_skip(p->in, 4);
                return -1;
            }
            if (memcmp(hdr + 4, "IDAT", 4) == 0)
            {
                p->idat_left = len;
                p->in_idat = FM_TRUE;
                p->crc = PNG_CRC_RUN("IDAT", 4, PNG_CRC_INIT);
                if (len == 0u)
                {
                    continue;
                }
            }
            else
            {
                if (png_skip(p->in, len + 4u) != FM_OK)
                {
                    return -1;
                }
                continue;
            }
        }
        {
            uint32_t n = p->idat_left;

            if (n > (uint32_t)sizeof(p->tmp))
            {
                n = (uint32_t)sizeof(p->tmp);
            }
            if (n == 0u)
            {
                continue;
            }
            if (png_read(p->in, p->tmp, n) != FM_OK)
            {
                return -1;
            }
            p->crc = PNG_CRC_RUN(p->tmp, n, p->crc);
            p->idat_left -= n;
            z->src = p->tmp;
            z->src_len = n;
            z->src_pos = 0;
            return 0;
        }
    }
}

static void png_sample_rgb(PngEmit *e, uint32_t sx, uint8_t *dst)
{
    const uint8_t *s = e->recon + (size_t)sx * (size_t)e->bpp;

    if (e->color == 0u)
    {
        dst[0] = dst[1] = dst[2] = s[0];
    }
    else if (e->color == 2u)
    {
        dst[0] = s[0];
        dst[1] = s[1];
        dst[2] = s[2];
    }
    else if (e->color == 3u)
    {
        uint32_t idx = s[0];

        if (idx >= e->pal_n)
        {
            dst[0] = dst[1] = dst[2] = 0;
            e->err = FM_ERR_CORRUPTED;
            return;
        }
        dst[0] = e->palette[idx * 3u];
        dst[1] = e->palette[idx * 3u + 1u];
        dst[2] = e->palette[idx * 3u + 2u];
    }
    else
    {
        dst[0] = s[0];
        dst[1] = s[1];
        dst[2] = s[2];
    }
}

static int png_on_row(Inf *z)
{
    PngEmit *e = (PngEmit *)z->row_ctx;
    uint32_t sy = e->y;
    uint16_t dy;

    if (e->err != FM_OK)
    {
        return -1;
    }
    if (z->row[0] > 4u)
    {
        e->err = FM_ERR_CORRUPTED;
        return -1;
    }
    png_unfilter(e->recon, z->row, e->prev, (size_t)e->sw * (size_t)e->bpp,
                 e->bpp);
    memcpy(e->prev, e->recon, (size_t)e->sw * (size_t)e->bpp);
    for (dy = 0; dy < e->dh; dy++)
    {
        if ((uint32_t)dy * e->sh / e->dh != sy)
        {
            continue;
        }
        {
            uint8_t *dst = e->out + (size_t)dy * e->dw * 3u;
            uint16_t dx;

            for (dx = 0; dx < e->dw; dx++)
            {
                uint32_t sx = (uint32_t)dx * e->sw / e->dw;

                png_sample_rgb(e, sx, dst + (size_t)dx * 3u);
            }
        }
    }
    e->y++;
    FM_Idle();
    return (e->err == FM_OK) ? 0 : -1;
}

static FM_Result png_decode(PngRead *in, FM_Image *img, void *pixel_buffer,
                            size_t capacity, uint16_t max_w, uint16_t max_h)
{
    uint8_t sig[8];
    uint8_t hdr[8];
    uint8_t ihdr[13];
    uint8_t crc_b[4];
    uint32_t w;
    uint32_t h;
    uint8_t depth;
    uint8_t color;
    uint8_t interlace;
    int bpp;
    size_t stride;
    uint16_t dw;
    uint16_t dh;
    Inf z;
    PngFeed feed;
    PngEmit emit;
    FM_Result r;

    if (png_read(in, sig, 8) != FM_OK || memcmp(sig, k_png_sig, 8) != 0)
    {
        return FM_ERR_INVALID_FORMAT;
    }
    if (png_read(in, hdr, 8) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    if (png_be32(hdr) != 13u || memcmp(hdr + 4, "IHDR", 4) != 0)
    {
        return FM_ERR_CORRUPTED;
    }
    if (png_read(in, ihdr, 13) != FM_OK || png_read(in, crc_b, 4) != FM_OK)
    {
        return FM_ERR_CORRUPTED;
    }
    {
        uint32_t crc = PNG_CRC_RUN("IHDR", 4, PNG_CRC_INIT);

        crc = PNG_CRC_RUN(ihdr, 13, crc);
        if (PNG_CRC_FIN(crc) != png_be32(crc_b))
        {
            return FM_ERR_CRC;
        }
    }
    w = png_be32(ihdr);
    h = png_be32(ihdr + 4);
    depth = ihdr[8];
    color = ihdr[9];
    interlace = ihdr[12];
    if (w == 0 || h == 0 || w > UINT16_MAX || h > UINT16_MAX || depth != 8u ||
        ihdr[10] != 0u || ihdr[11] != 0u || interlace != 0u)
    {
        return FM_ERR_UNSUPPORTED;
    }
    if (color == 0u)
    {
        bpp = 1;
    }
    else if (color == 2u)
    {
        bpp = 3;
    }
    else if (color == 3u)
    {
        bpp = 1;
    }
    else if (color == 6u)
    {
        bpp = 4;
    }
    else
    {
        return FM_ERR_UNSUPPORTED;
    }
    stride = (size_t)w * (size_t)bpp;
    if (stride > FM_PNG_MAX_STRIDE || stride + 1u > sizeof(g_png_row))
    {
        return FM_ERR_TOO_LARGE;
    }
    r = FM_Image_FitSize((uint16_t)w, (uint16_t)h, capacity, 3u,
                         max_w, max_h, &dw, &dh);
    if (r != FM_OK || pixel_buffer == NULL)
    {
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }

    memset(&emit, 0, sizeof(emit));
    emit.out = (uint8_t *)pixel_buffer;
    emit.sw = (uint16_t)w;
    emit.sh = (uint16_t)h;
    emit.dw = dw;
    emit.dh = dh;
    emit.color = color;
    emit.bpp = bpp;
    emit.prev = g_png_prev;
    emit.recon = g_png_recon;
    emit.err = FM_OK;
    memset(g_png_prev, 0, stride);

    /* PLTE lives before IDAT.  Walk chunks until the first IDAT, then
     * hand the rest of the IDAT stream to inflate via png_need(). */
    memset(&feed, 0, sizeof(feed));
    feed.in = in;
    for (;;)
    {
        uint32_t len;
        uint8_t type[4];

        if (png_read(in, hdr, 8) != FM_OK)
        {
            return FM_ERR_CORRUPTED;
        }
        len = png_be32(hdr);
        memcpy(type, hdr + 4, 4);
        if (memcmp(type, "PLTE", 4) == 0)
        {
            if (len % 3u != 0u || len > sizeof(emit.palette))
            {
                return FM_ERR_CORRUPTED;
            }
            if (png_read(in, emit.palette, len) != FM_OK ||
                png_skip(in, 4) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
            emit.pal_n = len / 3u;
        }
        else if (memcmp(type, "IDAT", 4) == 0)
        {
            feed.idat_left = len;
            feed.in_idat = FM_TRUE;
            feed.crc = PNG_CRC_RUN("IDAT", 4, PNG_CRC_INIT);
            break;
        }
        else if (memcmp(type, "IEND", 4) == 0)
        {
            return FM_ERR_CORRUPTED;
        }
        else
        {
            if (png_skip(in, len + 4u) != FM_OK)
            {
                return FM_ERR_CORRUPTED;
            }
        }
    }
    if (color == 3u && emit.pal_n == 0u)
    {
        return FM_ERR_CORRUPTED;
    }

    memset(&z, 0, sizeof(z));
    z.need = png_need;
    z.need_ctx = &feed;
    z.window = g_png_window;
    z.row = g_png_row;
    z.row_len = stride + 1u;
    z.on_row = png_on_row;
    z.row_ctx = &emit;
    /* Prime the first IDAT bytes. */
    if (png_need(&z) != 0)
    {
        return FM_ERR_CORRUPTED;
    }
    r = png_inflate(&z);
    if (r != FM_OK)
    {
        return r;
    }
    if (emit.err != FM_OK)
    {
        return emit.err;
    }
    if (emit.y != h)
    {
        return FM_ERR_CORRUPTED;
    }
    memset(img, 0, sizeof(*img));
    img->width = dw;
    img->height = dh;
    img->format = FM_PIXEL_RGB888;
    img->channels = 3;
    img->bit_depth = 24;
    img->data = (uint8_t *)pixel_buffer;
    img->data_capacity = capacity;
    img->data_size = (uint64_t)dw * dh * 3u;
    (void)strcpy(img->codec, "PNG");
    return FM_OK;
}

FM_Result Image_DecodePNG(const void *png, size_t png_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity)
{
    PngRead in;

    if (png == NULL || img == NULL || pixel_buffer == NULL || png_size < 33u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&in, 0, sizeof(in));
    in.mem = (const uint8_t *)png;
    in.mem_len = png_size;
    return png_decode(&in, img, pixel_buffer, capacity, 0, 0);
}

FM_Result FM_PNG_LoadFile(FM_File *f, FM_Image *img, void *pixel_buffer,
                          size_t capacity, uint16_t max_w, uint16_t max_h)
{
    PngRead in;

    if (f == NULL || img == NULL || pixel_buffer == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&in, 0, sizeof(in));
    in.file = f;
    return png_decode(&in, img, pixel_buffer, capacity, max_w, max_h);
}

FM_Result FM_PNG_ProbeFile(FM_File *f, FM_ImageInfo *info)
{
    uint8_t head[33];
    FM_Result r;

    r = FM_Image_ReadExact(f, head, sizeof(head));
    if (r != FM_OK)
    {
        return r;
    }
    return Image_ProbePNG(head, sizeof(head), info);
}

/* ------------------------------------------------------------------ */
/* PNG encode (RGB8, zlib stored blocks)                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    FM_File *file;
    uint8_t *mem;
    size_t   mem_cap;
    size_t   mem_len;
} PngOut;

static FM_Result png_out_bytes(PngOut *o, const void *p, uint32_t n)
{
    if (o->file != NULL)
    {
        const uint8_t *s = (const uint8_t *)p;
        uint32_t done = 0;

        while (done < n)
        {
            uint32_t wr = 0;
            FM_Result r = File_Write(o->file, s + done, n - done, &wr);

            if (r != FM_OK)
            {
                return r;
            }
            if (wr == 0u)
            {
                return FM_ERR_IO;
            }
            done += wr;
        }
        return FM_OK;
    }
    if (o->mem_len + n > o->mem_cap)
    {
        return FM_ERR_TOO_LARGE;
    }
    memcpy(o->mem + o->mem_len, p, n);
    o->mem_len += n;
    return FM_OK;
}

static FM_Result png_write_chunk(PngOut *o, const char *type,
                                 const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    uint8_t crc_b[4];
    uint32_t crc;
    FM_Result r;

    png_wr32(hdr, len);
    memcpy(hdr + 4, type, 4);
    r = png_out_bytes(o, hdr, 8);
    if (r != FM_OK)
    {
        return r;
    }
    crc = PNG_CRC_RUN(type, 4, PNG_CRC_INIT);
    if (len > 0u)
    {
        r = png_out_bytes(o, data, len);
        if (r != FM_OK)
        {
            return r;
        }
        crc = PNG_CRC_RUN(data, len, crc);
    }
    png_wr32(crc_b, PNG_CRC_FIN(crc));
    return png_out_bytes(o, crc_b, 4);
}

static void png_pixel_rgb(const FM_Image *img, uint32_t x, uint32_t y,
                          uint8_t *rgb)
{
    size_t index = (size_t)y * img->width + x;
    const uint8_t *p;

    rgb[0] = rgb[1] = rgb[2] = 0;
    if (img->format == FM_PIXEL_RGB888)
    {
        p = img->data + index * 3u;
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
    }
    else if (img->format == FM_PIXEL_BGR888)
    {
        p = img->data + index * 3u;
        rgb[2] = p[0]; rgb[1] = p[1]; rgb[0] = p[2];
    }
    else if (img->format == FM_PIXEL_RGB565 || img->format == FM_PIXEL_BGR565)
    {
        uint16_t v;

        p = img->data + index * 2u;
        v = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
        rgb[1] = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        if (img->format == FM_PIXEL_RGB565)
        {
            rgb[0] = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
            rgb[2] = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        }
        else
        {
            rgb[2] = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
            rgb[0] = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        }
    }
    else if (img->format == FM_PIXEL_RGBA8888)
    {
        p = img->data + index * 4u;
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
    }
    else if (img->format == FM_PIXEL_BGRA8888)
    {
        p = img->data + index * 4u;
        rgb[2] = p[0]; rgb[1] = p[1]; rgb[0] = p[2];
    }
    else
    {
        rgb[0] = rgb[1] = rgb[2] = img->data[index];
    }
}

static uint32_t png_adler(uint32_t adler, const uint8_t *p, uint32_t n)
{
    uint32_t a = adler & 0xFFFFu;
    uint32_t b = adler >> 16;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static FM_Result png_encode(PngOut *o, const FM_Image *img)
{
    uint8_t ihdr[13];
    uint8_t *rowp;
    size_t row_bytes;
    uint32_t y;
    uint32_t adler = 1;
    FM_Result r;
    uint64_t need;
    uint64_t raw;
    uint32_t nblocks;
    uint64_t zlen;

    if (img == NULL || img->data == NULL || img->width == 0 ||
        img->height == 0 || Image_BytesPerPixel(img->format) == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    need = (uint64_t)img->width * img->height * Image_BytesPerPixel(img->format);
    if (need > img->data_capacity)
    {
        return FM_ERR_TOO_LARGE;
    }
    row_bytes = 1u + (size_t)img->width * 3u;
    if (row_bytes == 0u || row_bytes > 65535u ||
        row_bytes > FM_PNG_MAX_STRIDE + 1u)
    {
        return FM_ERR_TOO_LARGE;
    }
    rowp = g_png_row;
    raw = (uint64_t)img->height * row_bytes;
    nblocks = (uint32_t)((raw + 65534u) / 65535u);
    if (nblocks == 0u)
    {
        nblocks = 1u;
    }
    zlen = 2u + (uint64_t)nblocks * 5u + raw + 4u;
    if (zlen > UINT32_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }

    r = png_out_bytes(o, k_png_sig, 8);
    memset(ihdr, 0, sizeof(ihdr));
    png_wr32(ihdr, img->width);
    png_wr32(ihdr + 4, img->height);
    ihdr[8] = 8;
    ihdr[9] = 2; /* RGB */
    if (r == FM_OK)
    {
        r = png_write_chunk(o, "IHDR", ihdr, 13);
    }
    if (r == FM_OK)
    {
        uint8_t hdr8[8];
        uint8_t zhead[2] = { 0x78, 0x01 };
        uint32_t crc;

        png_wr32(hdr8, (uint32_t)zlen);
        memcpy(hdr8 + 4, "IDAT", 4);
        r = png_out_bytes(o, hdr8, 8);
        crc = PNG_CRC_RUN("IDAT", 4, PNG_CRC_INIT);
        if (r == FM_OK)
        {
            r = png_out_bytes(o, zhead, 2);
            crc = PNG_CRC_RUN(zhead, 2, crc);
        }
        y = 0;
        while (r == FM_OK && y < img->height)
        {
            uint32_t this_raw = 0;
            uint32_t y0;
            uint32_t yy;
            uint8_t blkhdr[5];

            while (y < img->height &&
                   this_raw + (uint32_t)row_bytes <= 65535u)
            {
                this_raw += (uint32_t)row_bytes;
                y++;
            }
            y0 = y - this_raw / (uint32_t)row_bytes;
            blkhdr[0] = (y >= img->height) ? 1u : 0u;
            blkhdr[1] = (uint8_t)(this_raw & 0xFFu);
            blkhdr[2] = (uint8_t)(this_raw >> 8);
            blkhdr[3] = (uint8_t)((this_raw ^ 0xFFFFu) & 0xFFu);
            blkhdr[4] = (uint8_t)(((this_raw ^ 0xFFFFu) >> 8) & 0xFFu);
            r = png_out_bytes(o, blkhdr, 5);
            crc = PNG_CRC_RUN(blkhdr, 5, crc);
            for (yy = y0; r == FM_OK && yy < y; yy++)
            {
                uint32_t x;

                rowp[0] = 0;
                for (x = 0; x < img->width; x++)
                {
                    png_pixel_rgb(img, x, yy, rowp + 1u + x * 3u);
                }
                r = png_out_bytes(o, rowp, (uint32_t)row_bytes);
                crc = PNG_CRC_RUN(rowp, (uint32_t)row_bytes, crc);
                adler = png_adler(adler, rowp, (uint32_t)row_bytes);
            }
        }
        if (r == FM_OK)
        {
            uint8_t ad[4];

            png_wr32(ad, adler);
            r = png_out_bytes(o, ad, 4);
            crc = PNG_CRC_RUN(ad, 4, crc);
        }
        if (r == FM_OK)
        {
            uint8_t c4[4];

            png_wr32(c4, PNG_CRC_FIN(crc));
            r = png_out_bytes(o, c4, 4);
        }
    }
    if (r == FM_OK)
    {
        r = png_write_chunk(o, "IEND", ihdr, 0);
    }
    return r;
}

FM_Result FM_PNG_EncodeFile(FM_File *f, const FM_Image *img)
{
    PngOut o;

    memset(&o, 0, sizeof(o));
    o.file = f;
    return png_encode(&o, img);
}

FM_Result Image_EncodePNG(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len)
{
    PngOut o;
    FM_Result r;

    if (out == NULL || out_len == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(&o, 0, sizeof(o));
    o.mem = (uint8_t *)out;
    o.mem_cap = out_cap;
    r = png_encode(&o, img);
    if (r == FM_OK)
    {
        *out_len = o.mem_len;
    }
    return r;
}

#else /* !PNG */

FM_Result Image_ProbePNG(const void *png, size_t png_size, FM_ImageInfo *info)
{
    (void)png;
    (void)png_size;
    (void)info;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_DecodePNG(const void *png, size_t png_size,
                          FM_Image *img, void *pixel_buffer, size_t capacity)
{
    (void)png;
    (void)png_size;
    (void)img;
    (void)pixel_buffer;
    (void)capacity;
    return FM_ERR_UNSUPPORTED;
}

FM_Result Image_EncodePNG(const FM_Image *img, void *out, size_t out_cap,
                          size_t *out_len)
{
    (void)img;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return FM_ERR_UNSUPPORTED;
}

#endif
