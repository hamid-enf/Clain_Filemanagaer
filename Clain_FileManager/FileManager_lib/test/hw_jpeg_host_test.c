/*
 * hw_jpeg_host_test.c - the hardware-JPEG path (examples/hw_jpeg.h and
 * hw_jpeg_video.h) run on the PC against a stand-in for the peripheral.
 *
 * The stand-in writes what the STM32H7 JPEG core writes: not a picture
 * row by row but the MCUs of the file one after another, each made of
 * 8x8 blocks (gray 64 bytes, 4:4:4 192, 4:2:2 256, 4:2:0 384), and like
 * HAL_JPEG_Decode it starts again at the beginning of the output buffer
 * when the buffer is full, and still reports HAL_OK.
 *
 * What is checked: every pixel of every layout, at sizes that are and
 * are not a multiple of the MCU; the sizing of the MCU buffer; what
 * happens when HAL_JPEG_GetInfo fails.  The picture is compared with a
 * reference computed here pixel by pixel, not with a second run of the
 * same converter.
 *
 *   make hwjpegtest
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simple_image.h"
#include "simple_video.h"
#include "hw_jpeg.h"
#include "hw_jpeg_video.h"

static int fails;
static int checks;

static void ok(const char *what, int cond)
{
    checks++;
    if (!cond)
    {
        printf("  %-62s FAIL\n", what);
        fails++;
    }
}

/* --- the picture the stand-in "decodes" ---------------------------- */
enum { L_GRAY = 0, L_444, L_422, L_420 };
static const char *const lname[] = { "gray", "4:4:4", "4:2:2", "4:2:0" };

static uint8_t pic_y(uint32_t x, uint32_t y)
{
    return (uint8_t)(x * 7u + y * 13u + ((x ^ y) & 1u) * 40u);
}
static uint8_t pic_cb(uint32_t cx, uint32_t cy)
{
    return (uint8_t)(40u + cx * 11u + cy * 5u);
}
static uint8_t pic_cr(uint32_t cx, uint32_t cy)
{
    return (uint8_t)(200u - cx * 3u + cy * 17u);
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* reference: pixel (x,y) of a picture of this layout, as RGB888 */
static void expect_px(int layout, uint32_t x, uint32_t y, uint8_t *rgb)
{
    int yv = pic_y(x, y);
    uint32_t cx = (layout == L_420 || layout == L_422) ? x / 2u : x;
    uint32_t cy = (layout == L_420) ? y / 2u : y;
    int cb = (int)pic_cb(cx, cy) - 128;
    int cr = (int)pic_cr(cx, cy) - 128;

    if (layout == L_GRAY)
    {
        rgb[0] = rgb[1] = rgb[2] = (uint8_t)yv;
        return;
    }
    rgb[0] = clamp8(yv + ((359 * cr) >> 8));
    rgb[1] = clamp8(yv - ((88 * cb + 183 * cr) >> 8));
    rgb[2] = clamp8(yv + ((453 * cb) >> 8));
}

/* --- a JPEG file that is only its headers --------------------------- */
static size_t make_jpeg(uint8_t *d, int layout, uint32_t w, uint32_t h,
                        uint8_t sof)
{
    size_t n = 0;
    uint8_t nf = (layout == L_GRAY) ? 1u : 3u;
    uint8_t hv = (layout == L_420) ? 0x22u : (layout == L_422) ? 0x21u : 0x11u;
    uint8_t c;

    d[n++] = 0xFF; d[n++] = 0xD8;
    d[n++] = 0xFF; d[n++] = 0xE0; d[n++] = 0; d[n++] = 16;      /* APP0 */
    memcpy(d + n, "JFIF\0\1\1\0\0\1\0\1\0\0", 14); n += 14;
    d[n++] = 0xFF; d[n++] = sof;
    d[n++] = 0; d[n++] = (uint8_t)(8u + 3u * nf);
    d[n++] = 8;
    d[n++] = (uint8_t)(h >> 8); d[n++] = (uint8_t)h;
    d[n++] = (uint8_t)(w >> 8); d[n++] = (uint8_t)w;
    d[n++] = nf;
    for (c = 0; c < nf; c++)
    {
        d[n++] = (uint8_t)(c + 1u);
        d[n++] = (c == 0u) ? hv : 0x11u;
        d[n++] = (c == 0u) ? 0u : 1u;
    }
    d[n++] = 0xFF; d[n++] = 0xDA; d[n++] = 0; d[n++] = 2;       /* SOS */
    d[n++] = 0x00;
    d[n++] = 0xFF; d[n++] = 0xD9;
    return n;
}

/* --- the stand-in peripheral --------------------------------------- */
static int hal_calls;
static int hal_wrapped;
static int hal_getinfo_fails;
static uint32_t hal_outlen;
static JPEG_ConfTypeDef hal_cfg;
static int hal_layout;

static uint8_t *hal_out;
static uint32_t hal_pos;

static void hal_put(uint8_t v)
{
    if (hal_pos >= hal_outlen)
    {
        hal_pos = 0;               /* what the HAL does: start again */
        hal_wrapped = 1;
    }
    hal_out[hal_pos++] = v;
}

/* one 8x8 block of luma whose top-left pixel is (x0,y0); what lies
 * outside the picture is padding and must never reach the screen */
static void hal_block_y(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint32_t i, j;

    for (j = 0; j < 8u; j++)
        for (i = 0; i < 8u; i++)
            hal_put((x0 + i < w && y0 + j < h) ? pic_y(x0 + i, y0 + j) : 0xEEu);
}
static void hal_block_c(int cr, uint32_t cx0, uint32_t cy0)
{
    uint32_t i, j;

    for (j = 0; j < 8u; j++)
        for (i = 0; i < 8u; i++)
            hal_put(cr ? pic_cr(cx0 + i, cy0 + j) : pic_cb(cx0 + i, cy0 + j));
}

HAL_StatusTypeDef HAL_JPEG_Decode(JPEG_HandleTypeDef *hj, uint8_t *in,
                                  uint32_t inlen, uint8_t *out,
                                  uint32_t outlen, uint32_t t)
{
    /* the stand-in reads the frame header at its fixed place in the
     * files make_jpeg() writes: 2 + 18 + 4 bytes in */
    const uint8_t *s = in + 24;
    uint32_t h = ((uint32_t)s[1] << 8) | s[2];
    uint32_t w = ((uint32_t)s[3] << 8) | s[4];
    uint32_t mx, my;

    (void)hj; (void)inlen; (void)t;
    hal_calls++;
    hal_out = out; hal_outlen = outlen; hal_pos = 0; hal_wrapped = 0;
    hal_layout = (s[5] == 1u) ? L_GRAY
               : (s[7] == 0x22u) ? L_420 : (s[7] == 0x21u) ? L_422 : L_444;
    memset(&hal_cfg, 0, sizeof(hal_cfg));
    hal_cfg.ImageWidth = w; hal_cfg.ImageHeight = h;
    hal_cfg.ColorSpace = (hal_layout == L_GRAY) ? JPEG_GRAYSCALE_COLORSPACE
                                                : JPEG_YCBCR_COLORSPACE;
    hal_cfg.ChromaSubsampling = (hal_layout == L_420) ? JPEG_420_SUBSAMPLING
                              : (hal_layout == L_422) ? JPEG_422_SUBSAMPLING
                              : JPEG_444_SUBSAMPLING;
    if (outlen == 0u)
    {
        return HAL_ERROR;
    }
    switch (hal_layout)
    {
    case L_GRAY:
        for (my = 0; my * 8u < h; my++)
            for (mx = 0; mx * 8u < w; mx++)
                hal_block_y(mx * 8u, my * 8u, w, h);
        break;
    case L_444:
        for (my = 0; my * 8u < h; my++)
            for (mx = 0; mx * 8u < w; mx++)
            {
                hal_block_y(mx * 8u, my * 8u, w, h);
                hal_block_c(0, mx * 8u, my * 8u);
                hal_block_c(1, mx * 8u, my * 8u);
            }
        break;
    case L_422:
        for (my = 0; my * 8u < h; my++)
            for (mx = 0; mx * 16u < w; mx++)
            {
                hal_block_y(mx * 16u, my * 8u, w, h);
                hal_block_y(mx * 16u + 8u, my * 8u, w, h);
                hal_block_c(0, mx * 8u, my * 8u);
                hal_block_c(1, mx * 8u, my * 8u);
            }
        break;
    default:
        for (my = 0; my * 16u < h; my++)
            for (mx = 0; mx * 16u < w; mx++)
            {
                hal_block_y(mx * 16u, my * 16u, w, h);
                hal_block_y(mx * 16u + 8u, my * 16u, w, h);
                hal_block_y(mx * 16u, my * 16u + 8u, w, h);
                hal_block_y(mx * 16u + 8u, my * 16u + 8u, w, h);
                hal_block_c(0, mx * 8u, my * 8u);
                hal_block_c(1, mx * 8u, my * 8u);
            }
        break;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_JPEG_GetInfo(JPEG_HandleTypeDef *hj, JPEG_ConfTypeDef *c)
{
    (void)hj;
    if (hal_getinfo_fails)
    {
        return HAL_ERROR;
    }
    *c = hal_cfg;
    return HAL_OK;
}

/* --- the checks ----------------------------------------------------- */
static uint8_t g_jpeg[64];
static uint8_t g_rgb[800u * 480u * 3u];

static uint32_t wrong_pixels(int layout, uint32_t w, uint32_t h)
{
    uint32_t x, y, bad = 0;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            uint8_t e[3];

            expect_px(layout, x, y, e);
            if (memcmp(e, g_rgb + ((size_t)y * w + x) * 3u, 3) != 0) bad++;
        }
    return bad;
}

/* video != 0: through the MJPEG frame decoder instead of Image_DecodeJPEG */
static void one(int layout, uint32_t w, uint32_t h, int video)
{
    char what[96];
    size_t n = make_jpeg(g_jpeg, layout, w, h, 0xC0);
    FM_Image img;
    FM_Result r;
    uint32_t bad;

    memset(g_rgb, 0x5A, (size_t)w * h * 3u);
    memset(&img, 0, sizeof(img));
    if (video)
    {
        r = sv_hw_mjpeg_decode((void *)1, g_jpeg, n, g_rgb, sizeof(g_rgb));
    }
    else
    {
        r = Image_DecodeJPEG(g_jpeg, n, &img, g_rgb, sizeof(g_rgb));
    }
    bad = (r == FM_OK) ? wrong_pixels(layout, w, h) : w * h;
    (void)snprintf(what, sizeof(what), "%s %s %ux%u%s: r=%d, %u wrong pixels%s",
                   video ? "frame" : "picture", lname[layout], (unsigned)w,
                   (unsigned)h, hal_getinfo_fails ? " (no GetInfo)" : "",
                   (int)r, (unsigned)bad, hal_wrapped ? ", HAL wrapped" : "");
    ok(what, r == FM_OK && bad == 0u && !hal_wrapped &&
             (video || (img.width == w && img.height == h)));
}

int main(void)
{
    static const uint32_t sizes[][2] = {
        { 16, 16 }, { 8, 8 }, { 1, 1 }, { 17, 9 }, { 33, 31 }, { 7, 20 },
        { 320, 240 }
    };
    static JPEG_HandleTypeDef hjpeg;
    FM_Image img;
    size_t n;
    size_t i;
    int layout;
    int calls;
    FM_Result r;

    printf("\n=== hardware JPEG path, stand-in peripheral ===\n");
    ok("the codec registers", SimpleImage_UseHwJpeg(&hjpeg) == FM_OK);
    si_color_hw = NULL;

    /* every layout, every pixel */
    for (layout = L_GRAY; layout <= L_420; layout++)
    {
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
            one(layout, sizes[i][0], sizes[i][1], 0);
            one(layout, sizes[i][0], sizes[i][1], 1);
        }
    }

    /* the peripheral does not tell the layout: the file's header does */
    hal_getinfo_fails = 1;
    for (layout = L_GRAY; layout <= L_420; layout++)
    {
        one(layout, 33, 31, 0);
        one(layout, 33, 31, 1);
    }
    hal_getinfo_fails = 0;

    /* the buffer: a full-screen 4:2:0 and 4:2:2 picture still fits ... */
    one(L_420, 800, 480, 0);
    one(L_422, 800, 480, 0);
    one(L_444, 480, 480, 0);
    /* ... a full-screen 4:4:4 one does not, and is refused before the
     * peripheral is started, not decoded into a buffer that wraps */
    n = make_jpeg(g_jpeg, L_444, 800, 480, 0xC0);
    calls = hal_calls;
    r = Image_DecodeJPEG(g_jpeg, n, &img, g_rgb, sizeof(g_rgb));
    ok("4:4:4 800x480 is refused as too large", r == FM_ERR_TOO_LARGE);
    ok("... without starting the peripheral", hal_calls == calls);

    /* a picture larger than the caller's room */
    n = make_jpeg(g_jpeg, L_420, 64, 64, 0xC0);
    calls = hal_calls;
    r = Image_DecodeJPEG(g_jpeg, n, &img, g_rgb, 64u * 64u * 3u - 1u);
    ok("a pixel buffer one byte short is refused",
       r != FM_OK && hal_calls == calls);

    /* progressive: not for the peripheral */
    n = make_jpeg(g_jpeg, L_420, 64, 64, 0xC2);
    calls = hal_calls;
    r = si_hw_jpeg_decode(g_jpeg, n, &img, g_rgb, sizeof(g_rgb));
    ok("a progressive file is not given to the peripheral",
       r != FM_OK && hal_calls == calls);

    /* a header cut short */
    for (i = 0; i < 39u; i++)       /* the frame header ends at byte 39 */
    {
        JPEG_ConfTypeDef c;

        n = make_jpeg(g_jpeg, L_420, 64, 64, 0xC0);
        if (si_jpeg_file_layout(g_jpeg, i, &c) == FM_OK)
        {
            ok("a cut header is not a layout", 0);
        }
    }
    ok("cut headers are refused", 1);

    printf("  %d checks, %d failed\n", checks, fails);
    printf("\n  Result: %s\n\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
#else
typedef int fm_hw_jpeg_host_test_not_in_firmware;
#endif
