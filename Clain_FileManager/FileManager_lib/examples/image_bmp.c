/*
 * image_bmp.c — BMP Image + Scale + Convert
 *
 * ساخت، ذخیره، بارگذاری، تغییر اندازه و تبدیل فرمت تصویر.
 *
 * APIها:
 *   Image_Save           — ذخیره FM_Image به‌صورت BMP
 *   Image_Load           — بارگذاری BMP
 *   Image_GetInfo        — اطلاعات تصویر بدون بارگذاری کامل
 *   Image_Scale          — تغییر اندازه (nearest / bilinear)
 *   Image_Convert        — تبدیل فرمت پیکسل (RGB888/RGB565/GRAY8)
 *   Image_ResizeFile     — تغییر اندازه فایل BMP روی دیسک
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"

static uint8_t g_storage[128 * 1024];
static FM_Storage g_block;
static FM_FS      g_fs;

int main(void)
{
    FM_ImageInfo info;

    FileManager_Init();
    FileManager_MountMemory(&g_block, &g_fs, "RAM",
                            g_storage, sizeof(g_storage), 1, FM_FALSE);

    /* 1. ساخت تصویر 4x4 RGB888 و ذخیره */
    {
        FM_Image img;
        uint8_t pixels[4 * 4 * 3];

        memset(&img, 0, sizeof(img));
        img.width  = 4;
        img.height = 4;
        img.format = FM_PIXEL_RGB888;
        img.channels = 3;
        img.bit_depth = 24;
        img.data = pixels;
        img.data_capacity = sizeof(pixels);
        img.data_size = sizeof(pixels);

        for (int y = 0; y < 4; y++)
        {
            for (int x = 0; x < 4; x++)
            {
                int i = (y * 4 + x) * 3;
                pixels[i + 0] = (uint8_t)(x * 85);
                pixels[i + 1] = (uint8_t)(y * 85);
                pixels[i + 2] = 128;
            }
        }

        Image_Save("/test.bmp", &img);
        printf("Saved 4x4 BMP to /test.bmp\n");
    }

    /* 2. بارگذاری و بررسی */
    Image_GetInfo("/test.bmp", &info);
    printf("Image: %ux%u, %u-bit, codec=%s\n",
           info.width, info.height, info.bit_depth, info.codec);

    /* 3. تغییر اندازه 4x4 -> 8x8 (nearest) */
    {
        uint8_t src[4 * 4 * 3];
        uint8_t dst[8 * 8 * 3];
        FM_Image src_img;

        memset(&src_img, 0, sizeof(src_img));
        Image_Load("/test.bmp", &src_img, src, sizeof(src));

        Image_Scale(src, 4, 4, FM_PIXEL_RGB888,
                    dst, 8, 8, FM_SCALE_NEAREST);
        printf("Scaled 4x4 -> 8x8 (nearest)\n");

        Image_Scale(src, 4, 4, FM_PIXEL_RGB888,
                    dst, 8, 8, FM_SCALE_BILINEAR);
        printf("Scaled 4x4 -> 8x8 (bilinear)\n");
    }

    /* 4. تبدیل فرمت RGB888 -> RGB565 */
    {
        FM_Image src, dst;
        uint8_t src_px[4 * 4 * 3];
        uint8_t dst_px[4 * 4 * 2];

        memset(&src, 0, sizeof(src));
        Image_Load("/test.bmp", &src, src_px, sizeof(src_px));

        memset(&dst, 0, sizeof(dst));
        dst.data = dst_px;
        dst.data_capacity = sizeof(dst_px);

        Image_Convert(&src, &dst, FM_PIXEL_RGB565);
        printf("Converted RGB888 -> RGB565: %u bytes\n",
               (unsigned)dst.data_size);
    }

    /* 5. Resize فایل مستقیم */
    Image_ResizeFile("/test.bmp", "/small.bmp", 2, 2, FM_SCALE_NEAREST);
    Image_GetInfo("/small.bmp", &info);
    printf("Resized file: %ux%u\n", info.width, info.height);

    FS_Unmount("/");
    FileManager_Deinit();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
