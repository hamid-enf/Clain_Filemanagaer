/*
 * تست مسیر ویدیو: قلاب‌های شتاب‌دهنده و بازگشت نرم‌افزاری.
 *
 * این‌ها روی برد اجرا نمی‌شوند، پس اگر امضایی عوض شود یا یک قلاب
 * وصل نشود، هیچ‌کس تا لحظهٔ فلش‌کردن نمی‌فهمد. اینجا با HAL
 * شبیه‌سازی‌شده بررسی می‌شوند.
 *
 *   gcc -Iinclude -Iexamples -Itools/halstub test/video_hooks_test.c \
 *       libfilemanager.a -o fm_videohooks
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <string.h>
#include "simple_image.h"
#include "simple_video.h"
#include "hw_jpeg.h"
#include "dma2d_ycbcr.h"
#include "ltdc_present.h"

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

/* --- جاسوس‌های YCbCr --------------------------------------------- */
static int ycbcr_calls;
static size_t ycbcr_saw_bytes;

static FM_Result ycbcr_spy(const JPEG_ConfTypeDef *c, const uint8_t *y,
                           size_t n, uint8_t *r, FM_PixelFormat f)
{
    (void)c; (void)y; (void)r; (void)f;
    ycbcr_calls++; ycbcr_saw_bytes = n;
    return FM_ERR_TIMEOUT;          /* همیشه شکست => باید نرم‌افزار بیاید */
}

/* --- جاسوس RGB565 ------------------------------------------------- */
static int p565_calls;
static uint16_t p565_w, p565_h;
static uint8_t p565_bgr, p565_swap;

static FM_Result p565_spy(const uint8_t *s, uint8_t *d, uint16_t w,
                          uint16_t h, uint8_t bgr, uint8_t swap)
{
    (void)s; (void)d;
    p565_calls++; p565_w = w; p565_h = h; p565_bgr = bgr; p565_swap = swap;
    return FM_ERR_TIMEOUT;
}

/* --- spies for the new track/frame callbacks ---------------------- */
static int sv_track_start_calls;
static int sv_track_end_calls;
static int sv_frame_calls;
static char sv_track_start_path[FM_PATH_MAX];
static char sv_track_end_path[FM_PATH_MAX];

static void sv_track_start_spy(void *ctx, const char *path, uint32_t w,
                               uint32_t h, uint32_t fps)
{
    (void)ctx; (void)w; (void)h; (void)fps;
    sv_track_start_calls++;
    if (path != NULL)
    {
        (void)strncpy(sv_track_start_path, path,
                      sizeof(sv_track_start_path) - 1u);
        sv_track_start_path[sizeof(sv_track_start_path) - 1u] = '\0';
    }
}

static void sv_track_end_spy(void *ctx, const char *path)
{
    (void)ctx;
    sv_track_end_calls++;
    if (path != NULL)
    {
        (void)strncpy(sv_track_end_path, path,
                      sizeof(sv_track_end_path) - 1u);
        sv_track_end_path[sizeof(sv_track_end_path) - 1u] = '\0';
    }
}

static void sv_frame_spy(void *ctx, const void *pixels, uint16_t w,
                         uint16_t h, FM_PixelFormat fmt, size_t bytes)
{
    (void)ctx; (void)pixels; (void)w; (void)h; (void)fmt; (void)bytes;
    sv_frame_calls++;
}

int main(void)
{
    static uint8_t ycbcr[64 * 64 * 2 + 384];
    static uint8_t rgb_a[32 * 32 * 3], rgb_b[32 * 32 * 3];
    static uint8_t o565_a[32 * 32 * 2], o565_b[32 * 32 * 2];
    static uint8_t src888[32 * 32 * 3];
    JPEG_ConfTypeDef cfg;
    size_t i;

    printf("\n=== video hook tests ===\n");

    for (i = 0; i < sizeof(ycbcr); i++) ycbcr[i] = (uint8_t)(i * 7u + 3u);
    for (i = 0; i < sizeof(src888); i++) src888[i] = (uint8_t)(i * 13u + 5u);

    memset(&cfg, 0, sizeof(cfg));
    cfg.ImageWidth = 32; cfg.ImageHeight = 32;
    cfg.ColorSpace = 0; cfg.ChromaSubsampling = JPEG_420_SUBSAMPLING;

    /* 1. بدون قلاب، مسیر نرم‌افزاری */
    si_color_hw = NULL;
    ok("YCbCr with no hook decodes in software",
       si_jpeg_mcus_to_rgb(&cfg, ycbcr, sizeof(ycbcr), rgb_a) == FM_OK);

    /* 2. قلاب صدا زده می‌شود و اندازهٔ واقعی بافر را می‌گیرد */
    ycbcr_calls = 0; ycbcr_saw_bytes = 0;
    si_color_hw = ycbcr_spy;
    (void)si_jpeg_mcus_to_rgb(&cfg, ycbcr, sizeof(ycbcr), rgb_b);
    ok("YCbCr hook is actually called", ycbcr_calls == 1);
    ok("YCbCr hook receives the real buffer size, not a guess",
       ycbcr_saw_bytes == sizeof(ycbcr));

    /* 3. شکست سخت‌افزار => همان پیکسل‌های نرم‌افزاری */
    ok("a failing YCbCr hook falls back byte-identically",
       memcmp(rgb_a, rgb_b, sizeof(rgb_a)) == 0);

    /* 4. grayscale را DMA2D رد می‌کند (YCbCr نیست) */
    si_dma2d = &si_dma2d_own; si_dma2d_ready = 1u;
    cfg.ColorSpace = JPEG_GRAYSCALE_COLORSPACE;
    ok("DMA2D refuses grayscale instead of showing garbage",
       si_dma2d_convert(&cfg, ycbcr, sizeof(ycbcr), rgb_b,
                        FM_PIXEL_RGB888) == FM_ERR_UNSUPPORTED);
    cfg.ColorSpace = 0;

    /* 5. زیرنمونه‌برداری ناشناخته هم رد می‌شود */
    cfg.ChromaSubsampling = 99u;
    ok("DMA2D refuses an unknown chroma sub-sampling",
       si_dma2d_convert(&cfg, ycbcr, sizeof(ycbcr), rgb_b,
                        FM_PIXEL_RGB888) == FM_ERR_UNSUPPORTED);
    cfg.ChromaSubsampling = JPEG_420_SUBSAMPLING;

    /* 6. RGB565: بدون قلاب */
    sv_rgb565_hw = NULL;
    sv_convert_565(src888, o565_a, 32, 32, 1u, 1u);

    /* 7. RGB565: قلاب ابعاد درست را می‌گیرد */
    p565_calls = 0;
    sv_rgb565_hw = p565_spy;
    sv_convert_565(src888, o565_b, 32, 32, 1u, 1u);
    ok("RGB565 hook is called", p565_calls == 1);
    ok("RGB565 hook gets width and height, not just a pixel count",
       p565_w == 32 && p565_h == 32);
    ok("RGB565 hook is told about bgr and swap", p565_bgr == 1u && p565_swap == 1u);
    ok("a failing RGB565 hook falls back byte-identically",
       memcmp(o565_a, o565_b, sizeof(o565_a)) == 0);

    /* 8. LTDC */
    {
        static LTDC_HandleTypeDef h;
        uint32_t okfmt = 0u;

        ok("LTDC rejects a NULL handle",
           SimpleVideo_UseLtdc(NULL) == FM_ERR_INVALID_ARGUMENT);
        ok("LTDC accepts a real handle", SimpleVideo_UseLtdc(&h) == FM_OK);
        ok("LTDC installs itself as the present callback",
           sv.present == Ltdc_Present);
        ok("LTDC clears the SPI drain hook (nothing to drain)",
           sv_present_wait == NULL);
        ok("LTDC turns off the SPI byte swap so colours are right",
           sv_swap16_active() == 0u);
        (void)sv_ltdc_pixfmt(FM_PIXEL_RGB565, &okfmt);
        ok("LTDC supports RGB565", okfmt == 1u);
        (void)sv_ltdc_pixfmt(FM_PIXEL_GRAY8, &okfmt);
        ok("LTDC refuses a format it cannot scan out", okfmt == 0u);
        Ltdc_Present(NULL, 8, 4, FM_PIXEL_RGB565, 64);   /* نباید crash کند */
        Ltdc_Present(o565_a, 0, 0, FM_PIXEL_RGB565, 0);
        ok("LTDC present survives NULL and zero-sized frames", 1);
    }

    /* 9. Track + frame data callbacks (this branch).  Data-only mode:
     * present is cleared yet the frame hook is still installed, so a
     * pipeline can consume frames without an LCD. */
    SimpleVideo_SetFrameCallback(sv_frame_spy, NULL);
    SimpleVideo_SetTrackStartCallback(sv_track_start_spy, NULL);
    SimpleVideo_SetTrackEndCallback(sv_track_end_spy, NULL);
    ok("SetFrameCallback stores the hook", sv.frame_cb == sv_frame_spy);
    ok("SetTrackStartCallback stores the hook",
       sv.track_start == sv_track_start_spy);
    ok("SetTrackEndCallback stores the hook", sv.track_end == sv_track_end_spy);
    SimpleVideo_SetPresent(NULL);
    ok("data-only mode: present cleared, frame hook still installed",
       sv.present == NULL && sv.frame_cb == sv_frame_spy);

    printf("\n  %s (%d failure%s)\n\n",
           fails ? "FAILED" : "همه درست", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
