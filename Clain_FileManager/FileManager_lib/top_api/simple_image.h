/*
 * simple_image.h — SimpleImage: «عکس ساده» — آلبوم + داده برای TFT
 *
 * همان سبک SimpleAudio: یک include در main.c، دکمه‌ها را صدا می‌زنید،
 * پیکسل آماده‌ی LCD را می‌گیرید. بافرها را در SDRAM بگذارید (شما 32MB دارید).
 *
 *   SimpleImage_SetPresent(App_LcdBlit); // DMA: spi_tft_dma.h / polling — کار برد
 *   SimpleImage_SetOutputSize(320, 240);
 *   SimpleImage_Begin();                 // اسکن /photos + blit اول
 *   SimpleImage_Poll();                  // اسلایدشو / Next / Flip / …
 *   SimpleImage_Next() / Prev();
 *   SimpleImage_Rotate(90);              // اعمال در Poll (امن برای UART)
 *   SimpleImage_FlipH(); SimpleImage_FlipV();
 *   SimpleImage_Resize(320, 240);
 *   SimpleImage_Effect(FM_IMAGE_EFFECT_SEPIA, 0);
 *   SimpleImage_Save("/photos/out.bmp");  // یا .png / .jpg
 *
 * blit فقط پیکسل را به TFT بفرستد — Resize/Next/Begin را داخلش صدا نزن.
 * پنل SPI/LTDC مال برنامه است (present callback). FileManager درایور
 * ILI9341 نیست. برای SPI معمولاً SWAP16=1 و BGR565.
 * FM_PIXEL_GRAY8=1 پس اگر نام فرمت می‌خواهی از fmt-1 ایندکس بگیر.
 *   SimpleImageInfo inf;
 *   SimpleImage_Info(&inf);
 *   // inf.pixels / width / height / format → DMA به LTDC/SPI TFT
 *
 * خروجی پیش‌فرض RGB565 (اکثر TFTها).
 * خواندن: BMP + JPEG + PNG — تشخیص از magic bytes (پسوند مهم نیست).
 * عکس‌های بزرگتر از بافر TFT خودکار کوچک می‌شوند (بدون malloc روی heap).
 * نوشتن Save: BMP / PNG / JPEG از روی پسوند مسیر.
 *
 * تنظیمات (قبل از include):
 *   SIMPLE_IMAGE_FOLDER     پیش‌فرض "/photos"
 *   SIMPLE_IMAGE_MAX_W/H    سقف بافر (پیش‌فرض 800x480)
 *   SIMPLE_IMAGE_OUT_W/H    سایز خروجی TFT (۰ = همان سایز decode)
 *   SIMPLE_IMAGE_FORMAT     فرمت TFT (پیش‌فرض FM_PIXEL_RGB565)
 *   SIMPLE_IMAGE_SWAP16     1 = بایت‌های 565 برای SPI (۰ برای LTDC)
 *   SIMPLE_IMAGE_SECTION    سکشن لینکر SDRAM (پیش‌فرض ".sdram_buffers")
 */

#ifndef SIMPLE_IMAGE_H_
#define SIMPLE_IMAGE_H_

#include <stddef.h>
#include "fm_board_config.h"   /* آدرس‌ها و پرچم‌های مرکزی */
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "fm_log.h"
#include "file_manager.h"
#include "fm_image.h"
#include "fm_player.h"

#ifndef SIMPLE_IMAGE_FOLDER
#define SIMPLE_IMAGE_FOLDER "/photos"
#endif
#ifndef SIMPLE_IMAGE_MAX_W
#define SIMPLE_IMAGE_MAX_W 800u
#endif
#ifndef SIMPLE_IMAGE_MAX_H
#define SIMPLE_IMAGE_MAX_H 480u
#endif
#ifndef SIMPLE_IMAGE_FORMAT
#define SIMPLE_IMAGE_FORMAT FM_PIXEL_RGB565
#endif
#ifndef SIMPLE_IMAGE_SWAP16
#define SIMPLE_IMAGE_SWAP16 1
#endif
#ifndef SIMPLE_IMAGE_OUT_W
#define SIMPLE_IMAGE_OUT_W 0u
#endif
#ifndef SIMPLE_IMAGE_OUT_H
#define SIMPLE_IMAGE_OUT_H 0u
#endif
#ifndef SIMPLE_IMAGE_SECTION
#define SIMPLE_IMAGE_SECTION ".sdram_buffers"
#endif
#ifndef SIMPLE_IMAGE_FILE_MAX
#define SIMPLE_IMAGE_FILE_MAX (2u * 1024u * 1024u)  /* compressed JPEG in SDRAM */
#endif

typedef void (*SimpleImage_PresentFn)(const void *pixels, uint16_t width,
                                      uint16_t height, FM_PixelFormat format,
                                      size_t bytes);

typedef struct
{
    char     name[FM_PATH_MAX];
    char     codec[8];
    uint16_t width;
    uint16_t height;
    FM_PixelFormat format;
    const void *pixels;
    size_t   bytes;
    uint32_t album_count;
    uint32_t album_index;
} SimpleImageInfo;

#define SI_CMD_NONE    0u
#define SI_CMD_NEXT    1u
#define SI_CMD_PREV    2u
#define SI_CMD_SHOW    3u
#define SI_CMD_FLIP_H  4u
#define SI_CMD_FLIP_V  5u
#define SI_CMD_ROTATE  6u
#define SI_CMD_EFFECT  7u
#define SI_CMD_RESIZE  8u
#define SI_CMD_FORMAT  9u
#define SI_CMD_BLIT    10u
#define SI_CMD_SAVE    11u
#define SI_CMD_TEST    12u

typedef struct
{
    FM_Player album;
    FM_Bool album_ok;
    FM_Image img;
    volatile uint8_t pending_cmd;
    char pending_path[FM_PATH_MAX];
    int pending_rotate;
    FM_ImageEffect pending_effect;
    int32_t pending_amount;
    uint16_t pending_w;
    uint16_t pending_h;
    FM_PixelFormat pending_format;
    SimpleImage_PresentFn present;
    uint8_t presenting;          /* 1 while inside the blit callback   */
    uint16_t out_w;              /* 0 = keep decoded size              */
    uint16_t out_h;
    uint32_t slideshow_ms;
    uint32_t slideshow_last;
    FM_Bool loaded;
} SimpleImageState;

static SimpleImageState si;

__attribute__((section(SIMPLE_IMAGE_SECTION), aligned(32)))
static uint8_t si_pixels[(size_t)SIMPLE_IMAGE_MAX_W * SIMPLE_IMAGE_MAX_H * 4u];
__attribute__((section(SIMPLE_IMAGE_SECTION), aligned(32)))
static uint8_t si_work[(size_t)SIMPLE_IMAGE_MAX_W * SIMPLE_IMAGE_MAX_H * 4u];
__attribute__((section(SIMPLE_IMAGE_SECTION), aligned(32)))
static uint8_t si_file[SIMPLE_IMAGE_FILE_MAX];

static void simple_image_present(void)
{
    const uint8_t *px;
    size_t bytes;

    if (si.present == NULL || !si.loaded || si.presenting != 0u)
    {
        return;
    }
    px = si.img.data;
    bytes = (size_t)si.img.data_size;
#if SIMPLE_IMAGE_SWAP16
    /* SPI TFT (ILI9341/ST7789) wants RGB565 big-endian on the wire.
     * Memory stays LE so Save/Rotate keep working. */
    if ((si.img.format == FM_PIXEL_RGB565 ||
         si.img.format == FM_PIXEL_BGR565) &&
        (bytes & 1u) == 0u && bytes <= sizeof(si_work))
    {
        size_t i;
        uint8_t *d = si_work;
        const uint8_t *s = si.img.data;

        for (i = 0; i + 4u <= bytes; i += 4u)
        {
            d[i] = s[i + 1u];
            d[i + 1u] = s[i];
            d[i + 2u] = s[i + 3u];
            d[i + 3u] = s[i + 2u];
        }
        if (i < bytes)
        {
            d[i] = s[i + 1u];
            d[i + 1u] = s[i];
        }
        px = d;
    }
#endif
    si.presenting = 1u;
    /* Do not pause I2S (DMAPause/Resume clicks the DAC). A 320x240 SPI
     * frame is ~25 ms; the audio half-buffer is ~341 ms. */
    si.present(px, si.img.width, si.img.height, si.img.format, bytes);
    si.presenting = 0u;
}

static FM_Result simple_image_show_test(void);

/* Stretch to an exact TFT size.  Does not blit; caller presents. */
static FM_Result simple_image_apply_size(uint16_t width, uint16_t height)
{
    size_t need = 0;
    FM_Result r;

    if (!si.loaded)
    {
        return FM_ERR_NOT_READY;
    }
    if (width == 0u || height == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (si.img.width == width && si.img.height == height)
    {
        return FM_OK;
    }
    r = Image_BufferSize(width, height, si.img.format, &need);
    if (r != FM_OK || need > sizeof(si_work))
    {
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }
    r = Image_Scale(si.img.data, si.img.width, si.img.height, si.img.format,
                    si_work, width, height, FM_SCALE_BILINEAR);
    if (r != FM_OK)
    {
        return r;
    }
    memcpy(si_pixels, si_work, need);
    si.img.data = si_pixels;
    si.img.data_capacity = sizeof(si_pixels);
    si.img.width = width;
    si.img.height = height;
    si.img.data_size = need;
    return FM_OK;
}

static FM_Result simple_image_to_format(FM_PixelFormat fmt)
{
    FM_Image tmp;
    FM_Result r;
    size_t need = 0;

    if (!si.loaded)
    {
        return FM_ERR_NOT_READY;
    }
    if (si.img.format == fmt)
    {
        return FM_OK;
    }
    r = Image_BufferSize(si.img.width, si.img.height, fmt, &need);
    if (r != FM_OK)
    {
        return r;
    }
    if (need > sizeof(si_work))
    {
        return FM_ERR_TOO_LARGE;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.data = si_work;
    tmp.data_capacity = sizeof(si_work);
    r = Image_Convert(&si.img, &tmp, fmt);
    if (r != FM_OK)
    {
        return r;
    }
    memcpy(si_pixels, tmp.data, (size_t)tmp.data_size);
    tmp.data = si_pixels;
    tmp.data_capacity = sizeof(si_pixels);
    si.img = tmp;
    return FM_OK;
}

static FM_Result simple_image_load_path(const char *path)
{
    FM_Image native;
    FM_Result r;
    size_t cap = sizeof(si_work);

    memset(&native, 0, sizeof(native));
    r = Image_LoadEx(path, &native, si_work, cap,
                     SIMPLE_IMAGE_MAX_W, SIMPLE_IMAGE_MAX_H);
    if (r != FM_OK)
    {
        FM_Log("SimpleImage: open %s failed: %s (%d)\n", path,
               File_GetErrorString(r), (int)r);
        return r;
    }
    {
        FM_Image out;

        memset(&out, 0, sizeof(out));
        out.data = si_pixels;
        out.data_capacity = sizeof(si_pixels);
        r = Image_Convert(&native, &out, SIMPLE_IMAGE_FORMAT);
        if (r != FM_OK)
        {
            return r;
        }
        si.img = out;
        si.loaded = FM_TRUE;
        si.slideshow_last = HAL_GetTick();
        if (si.out_w != 0u && si.out_h != 0u)
        {
            r = simple_image_apply_size(si.out_w, si.out_h);
            if (r != FM_OK)
            {
                si.loaded = FM_FALSE;
                FM_Log("SimpleImage: scale %s failed: %s (%d)\n", path,
                       File_GetErrorString(r), (int)r);
                return r;
            }
        }
        FM_Log("SimpleImage: %s  %ux%u  fmt=%d\n", path,
               (unsigned)si.img.width, (unsigned)si.img.height,
               (int)si.img.format);
        simple_image_present();
        return FM_OK;
    }
}

static FM_Result simple_image_show_index(uint32_t index)
{
    uint32_t count = Player_Count(&si.album);
    uint32_t tried;

    if (count == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
    for (tried = 0; tried < count; tried++)
    {
        uint32_t item = (index + tried) % count;
        const char *path = Player_ItemPath(&si.album, item);
        FM_Result r = simple_image_load_path(path);

        if (r == FM_OK)
        {
            si.album.current = item;
            return FM_OK;
        }
    }
    return FM_ERR_UNSUPPORTED;
}

static FM_Result simple_image_apply_pending(void)
{
    uint8_t cmd = si.pending_cmd;
    uint32_t count;
    uint32_t idx;

    si.pending_cmd = SI_CMD_NONE;
    count = Player_Count(&si.album);
    if (cmd == SI_CMD_NEXT)
    {
        idx = (count == 0u) ? 0u : (Player_Current(&si.album) + 1u) % count;
        return simple_image_show_index(idx);
    }
    if (cmd == SI_CMD_PREV)
    {
        idx = (count == 0u) ? 0u
                            : (Player_Current(&si.album) == 0u)
                                  ? (count - 1u)
                                  : (Player_Current(&si.album) - 1u);
        return simple_image_show_index(idx);
    }
    if (cmd == SI_CMD_SHOW)
    {
        uint32_t i;

        for (i = 0; i < count; i++)
        {
            if (strcmp(Player_ItemPath(&si.album, i), si.pending_path) == 0)
            {
                return simple_image_show_index(i);
            }
        }
        return simple_image_load_path(si.pending_path);
    }
    if (cmd == SI_CMD_FLIP_H || cmd == SI_CMD_FLIP_V)
    {
        FM_Result r;

        if (!si.loaded)
        {
            return FM_ERR_NOT_READY;
        }
        r = Image_Flip(&si.img, (cmd == SI_CMD_FLIP_H)
                                    ? FM_IMAGE_FLIP_HORIZONTAL
                                    : FM_IMAGE_FLIP_VERTICAL);
        if (r == FM_OK)
        {
            simple_image_present();
        }
        return r;
    }
    if (cmd == SI_CMD_ROTATE)
    {
        FM_Image dst;
        FM_Result r;

        if (!si.loaded)
        {
            return FM_ERR_NOT_READY;
        }
        memset(&dst, 0, sizeof(dst));
        dst.data = si_work;
        dst.data_capacity = sizeof(si_work);
        r = Image_Rotate(&si.img, &dst, si.pending_rotate);
        if (r != FM_OK)
        {
            return r;
        }
        memcpy(si_pixels, dst.data, (size_t)dst.data_size);
        dst.data = si_pixels;
        dst.data_capacity = sizeof(si_pixels);
        si.img = dst;
        simple_image_present();
        return FM_OK;
    }
    if (cmd == SI_CMD_EFFECT)
    {
        FM_Result r;

        if (!si.loaded)
        {
            return FM_ERR_NOT_READY;
        }
        r = Image_ApplyEffect(&si.img, si.pending_effect, si.pending_amount);
        if (r == FM_OK)
        {
            simple_image_present();
        }
        return r;
    }
    if (cmd == SI_CMD_RESIZE)
    {
        uint16_t ow = si.img.width;
        uint16_t oh = si.img.height;
        FM_Result r = simple_image_apply_size(si.pending_w, si.pending_h);

        if (r == FM_OK && (si.img.width != ow || si.img.height != oh))
        {
            simple_image_present();
        }
        return r;
    }
    if (cmd == SI_CMD_FORMAT)
    {
        FM_Result r = simple_image_to_format(si.pending_format);

        if (r == FM_OK)
        {
            simple_image_present();
        }
        return r;
    }
    if (cmd == SI_CMD_BLIT)
    {
        simple_image_present();
        return FM_OK;
    }
    if (cmd == SI_CMD_SAVE)
    {
        if (!si.loaded)
        {
            return FM_ERR_NOT_READY;
        }
        return Image_Save(si.pending_path, &si.img);
    }
    if (cmd == SI_CMD_TEST)
    {
        return simple_image_show_test();
    }
    return FM_OK;
}

FM_Result SimpleImage_Begin(void)
{
    FM_Result r;
    SimpleImage_PresentFn present = si.present;
    uint16_t out_w = si.out_w;
    uint16_t out_h = si.out_h;
    uint32_t slideshow_ms = si.slideshow_ms;

    /* What the application chose is kept across Stop()/Begin(): the
     * present callback, the output size and the slideshow period. */
    memset(&si, 0, sizeof(si));
    si.present = present;
    si.slideshow_ms = slideshow_ms;
    si.slideshow_last = HAL_GetTick();
    si.out_w = (out_w != 0u) ? out_w : (uint16_t)SIMPLE_IMAGE_OUT_W;
    si.out_h = (out_h != 0u) ? out_h : (uint16_t)SIMPLE_IMAGE_OUT_H;
    (void)Image_SetWorkBuffer(si_file, sizeof(si_file));
    r = Player_Init(&si.album);
    if (r != FM_OK)
    {
        return r;
    }
    si.album_ok = FM_TRUE;
    r = Player_ScanFolder(&si.album, SIMPLE_IMAGE_FOLDER,
                          FM_PLAYER_KIND_IMAGE);
    if (r != FM_OK)
    {
        FM_Log("SimpleImage: scan %s failed: %s (%d)\n", SIMPLE_IMAGE_FOLDER,
               File_GetErrorString(r), (int)r);
        return r;
    }
    FM_Log("SimpleImage: album %lu file(s) in %s\n",
           (unsigned long)Player_Count(&si.album), SIMPLE_IMAGE_FOLDER);
    if (Player_Count(&si.album) == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
    return simple_image_show_index(0);
}

FM_Result SimpleImage_Poll(void)
{
    if (si.pending_cmd != SI_CMD_NONE)
    {
        return simple_image_apply_pending();
    }
    if (si.slideshow_ms != 0u && si.loaded)
    {
        uint32_t now = HAL_GetTick();

        if ((now - si.slideshow_last) >= si.slideshow_ms)
        {
            si.slideshow_last = now;
            si.pending_cmd = SI_CMD_NEXT;
            return simple_image_apply_pending();
        }
    }
    return si.loaded ? FM_OK : FM_ERR_NOT_READY;
}

FM_Result SimpleImage_Stop(void)
{
    si.pending_cmd = SI_CMD_NONE;
    si.loaded = FM_FALSE;   /* the slideshow period is kept for the next Begin */
    if (si.album_ok)
    {
        Player_Deinit(&si.album);
        si.album_ok = FM_FALSE;
    }
    FM_Log("SimpleImage: stopped\n");
    return FM_OK;
}

FM_Result SimpleImage_Next(void)
{
    si.pending_cmd = SI_CMD_NEXT;
    return FM_OK;
}

FM_Result SimpleImage_Prev(void)
{
    si.pending_cmd = SI_CMD_PREV;
    return FM_OK;
}

FM_Result SimpleImage_Show(const char *path)
{
    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)strncpy(si.pending_path, path, FM_PATH_MAX - 1u);
    si.pending_path[FM_PATH_MAX - 1u] = '\0';
    si.pending_cmd = SI_CMD_SHOW;
    return FM_OK;
}

void SimpleImage_SetPresent(SimpleImage_PresentFn fn)
{
    si.present = fn;
}

FM_Result SimpleImage_SetOutputSize(uint16_t width, uint16_t height)
{
    if (width > SIMPLE_IMAGE_MAX_W || height > SIMPLE_IMAGE_MAX_H)
    {
        return FM_ERR_TOO_LARGE;
    }
    si.out_w = width;
    si.out_h = height;
    if (!si.loaded || width == 0u || height == 0u)
    {
        return FM_OK;
    }
    if (si.img.width == width && si.img.height == height)
    {
        return FM_OK;
    }
    si.pending_w = width;
    si.pending_h = height;
    si.pending_cmd = SI_CMD_RESIZE;
    return FM_OK;
}

/* Show the next picture every period_ms (0 = off).  Before or after
 * Begin(); kept across Stop()/Begin(). */
FM_Result SimpleImage_SetSlideshow(uint32_t period_ms)
{
    si.slideshow_ms = period_ms;
    si.slideshow_last = HAL_GetTick();
    return FM_OK;
}

FM_Result SimpleImage_Resize(uint16_t width, uint16_t height)
{
    if (width == 0u || height == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (width > SIMPLE_IMAGE_MAX_W || height > SIMPLE_IMAGE_MAX_H)
    {
        return FM_ERR_TOO_LARGE;
    }
    si.out_w = width;
    si.out_h = height;
    si.pending_w = width;
    si.pending_h = height;
    si.pending_cmd = SI_CMD_RESIZE;
    return FM_OK;
}

FM_Result SimpleImage_Rotate(int degrees)
{
    si.pending_rotate = degrees;
    si.pending_cmd = SI_CMD_ROTATE;
    return FM_OK;
}

FM_Result SimpleImage_FlipH(void)
{
    si.pending_cmd = SI_CMD_FLIP_H;
    return FM_OK;
}

FM_Result SimpleImage_FlipV(void)
{
    si.pending_cmd = SI_CMD_FLIP_V;
    return FM_OK;
}

FM_Result SimpleImage_Effect(FM_ImageEffect effect, int32_t amount)
{
    si.pending_effect = effect;
    si.pending_amount = amount;
    si.pending_cmd = SI_CMD_EFFECT;
    return FM_OK;
}

FM_Result SimpleImage_SetFormat(FM_PixelFormat format)
{
    si.pending_format = format;
    si.pending_cmd = SI_CMD_FORMAT;
    return FM_OK;
}

/* 8 vertical color bars (R,G,B,Y,M,C,W,K).  If this is striped, the
 * TFT blit/SPI is wrong — not the PNG/JPEG decoder. */
static FM_Result simple_image_show_test(void)
{
    uint16_t w = (si.out_w != 0u) ? si.out_w : 320u;
    uint16_t h = (si.out_h != 0u) ? si.out_h : 240u;
    size_t need888 = 0;
    uint16_t x;
    uint16_t y;
    FM_Image src;
    FM_Image dst;
    FM_Result r;
    static const uint8_t bars[8][3] = {
        {255, 0, 0},   {0, 255, 0},   {0, 0, 255},   {255, 255, 0},
        {255, 0, 255}, {0, 255, 255}, {255, 255, 255}, {0, 0, 0}
    };

    if (w == 0u || h == 0u)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (w > SIMPLE_IMAGE_MAX_W)
    {
        w = (uint16_t)SIMPLE_IMAGE_MAX_W;
    }
    if (h > SIMPLE_IMAGE_MAX_H)
    {
        h = (uint16_t)SIMPLE_IMAGE_MAX_H;
    }
    r = Image_BufferSize(w, h, FM_PIXEL_RGB888, &need888);
    if (r != FM_OK || need888 > sizeof(si_work))
    {
        return (r != FM_OK) ? r : FM_ERR_TOO_LARGE;
    }
    for (y = 0; y < h; y++)
    {
        uint8_t *row = si_work + y * (size_t)w * 3u;

        for (x = 0; x < w; x++)
        {
            unsigned bar = (unsigned)((x * 8u) / w);
            if (bar > 7u)
            {
                bar = 7u;
            }
            row[x * 3u] = bars[bar][0];
            row[x * 3u + 1u] = bars[bar][1];
            row[x * 3u + 2u] = bars[bar][2];
        }
    }
    memset(&src, 0, sizeof(src));
    src.width = w;
    src.height = h;
    src.format = FM_PIXEL_RGB888;
    src.channels = 3;
    src.bit_depth = 24;
    src.data = si_work;
    src.data_capacity = sizeof(si_work);
    src.data_size = need888;
    memset(&dst, 0, sizeof(dst));
    dst.data = si_pixels;
    dst.data_capacity = sizeof(si_pixels);
    r = Image_Convert(&src, &dst, SIMPLE_IMAGE_FORMAT);
    if (r != FM_OK)
    {
        return r;
    }
    si.img = dst;
    si.loaded = FM_TRUE;
    FM_Log("SimpleImage: test pattern %ux%u fmt=%d\n",
           (unsigned)si.img.width, (unsigned)si.img.height,
           (int)si.img.format);
    simple_image_present();
    return FM_OK;
}

FM_Result SimpleImage_ShowTestPattern(void)
{
    si.pending_cmd = SI_CMD_TEST;
    return FM_OK;
}

FM_Result SimpleImage_Save(const char *path)
{
    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)strncpy(si.pending_path, path, FM_PATH_MAX - 1u);
    si.pending_path[FM_PATH_MAX - 1u] = '\0';
    si.pending_cmd = SI_CMD_SAVE;
    return FM_OK;
}

const void *SimpleImage_Data(uint16_t *width, uint16_t *height,
                             FM_PixelFormat *format, size_t *bytes)
{
    if (!si.loaded)
    {
        if (width != NULL) *width = 0;
        if (height != NULL) *height = 0;
        if (bytes != NULL) *bytes = 0;
        return NULL;
    }
    if (width != NULL) *width = si.img.width;
    if (height != NULL) *height = si.img.height;
    if (format != NULL) *format = si.img.format;
    if (bytes != NULL) *bytes = (size_t)si.img.data_size;
    return si.img.data;
}

void SimpleImage_Info(SimpleImageInfo *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->album_count = Player_Count(&si.album);
    out->album_index = Player_Current(&si.album);
    if (!si.loaded)
    {
        return;
    }
    if (out->album_count > 0u)
    {
        (void)strncpy(out->name, Player_ItemPath(&si.album, out->album_index),
                      FM_PATH_MAX - 1u);
    }
    (void)strncpy(out->codec, si.img.codec, sizeof(out->codec) - 1u);
    out->width = si.img.width;
    out->height = si.img.height;
    out->format = si.img.format;
    out->pixels = si.img.data;
    out->bytes = (size_t)si.img.data_size;
}

#endif /* SIMPLE_IMAGE_H_ */
