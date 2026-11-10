/*
 * cache.h — مدیریت متمرکز I-Cache / D-Cache برای STM32H7
 * =================================================================
 *
 * چرا این فایل لازم است
 * ---------------------
 * روی STM32H7 هستهٔ Cortex-M7 یک D-Cache دارد که «write-back» است. یعنی
 * وقتی CPU در حافظه می‌نویسد، داده اول در کش می‌ماند و لزوماً هنوز به
 * SDRAM/SRAM نرسیده. حالا اگر DMA (مثلاً SPI به LCD، یا I2S، یا SDMMC)
 * همان ناحیه را بخواند، دادهٔ قدیمی را می‌بیند و تصویر/صدا خراب می‌شود.
 * برعکسش هم هست: وقتی DMA در حافظه می‌نویسد (خواندن از SD کارت)، CPU
 * ممکن است نسخهٔ کهنهٔ کش‌شده را ببیند.
 *
 * برای همین است که «کش را روشن کردم ولی به هم ریخت». راه‌حل خاموش‌کردن
 * کش نیست — راه‌حل این است که سر مرزهای DMA دو عمل درست انجام شود:
 *
 *   قبل از اینکه DMA از بافری بخواند   →  Clean      (کش را به RAM بنویس)
 *   بعد از اینکه DMA در بافری نوشت     →  Invalidate (نسخهٔ کش را دور بریز)
 *
 * این فایل هر دو را در دو تابع می‌گذارد و همهٔ محدودیت‌های ریز را رعایت
 * می‌کند تا لازم نباشد در هر درایور از نو حواستان باشد.
 *
 * دستاورد سرعتی
 * -------------
 * با D-Cache خاموش، هر بایتی که CPU از SDRAM می‌خواند یک تراکنش کامل روی
 * باس FMC است. اندازه‌گیری روی همین پروژه: تبدیل رنگ ۳۲۰x۲۴۰ حدود ۳۵ms
 * طول می‌کشد، یعنی ~۱۷۰ سیکل برای هر پیکسل، در حالی که خود محاسبه ۵ تا ۱۰
 * سیکل بیشتر نیست. تقریباً همهٔ آن زمان انتظار برای حافظه است.
 * با کش روشن انتظار می‌رود این کار چند برابر تندتر شود.
 *
 * سه نکتهٔ حیاتی که معمولاً فراموش می‌شود
 * --------------------------------------
 * ۱) عملیات کش روی خطوط ۳۲ بایتی انجام می‌شود، نه بایت به بایت. اگر بافر
 *    شما ۳۲-بایت-تراز نباشد، Invalidate می‌تواند دادهٔ همسایه را هم دور
 *    بریزد و متغیر بغلی را خراب کند. توابع اینجا آدرس را به پایین و طول
 *    را به بالا گرد می‌کنند تا هرگز نصفهٔ یک خط را دست نزنند — ولی باز هم
 *    بافرهای DMA را با aligned(32) تعریف کنید. ماکروی FM_CACHE_ALIGN
 *    همین کار را می‌کند.
 *
 * ۲) DTCM (آدرس 0x20000000) اصلاً کش نمی‌شود. عملیات کش روی آن بی‌فایده
 *    است (مضر نیست، فقط هدررفت وقت). توابع اینجا این محدوده را رد می‌کنند.
 *
 * ۳) اگر MPU ناحیه‌ای را Non-Cacheable یا Write-Through تعریف کرده باشد،
 *    نیازی به Clean نیست. ساده‌ترین راه امن برای شروع همین است: یک ناحیهٔ
 *    کوچک Non-Cacheable برای بافرهای DMA بگذارید و بقیهٔ SDRAM را
 *    Cacheable بگذارید تا سرعت بگیرید. راهنمای MPU پایین همین فایل است.
 *
 * نحوهٔ استفاده
 * -------------
 *   در main.c، بعد از HAL_Init() و قبل از راه‌اندازی درایورها:
 *
 *       MPU_Config();          // نمونه‌اش پایین همین فایل هست
 *       FM_Cache_Enable();     // به‌جای SCB_DisableDCache()
 *
 *   و در هر درایور DMA:
 *
 *       FM_Cache_CleanForDma(buf, len);        // قبل از TX
 *       FM_Cache_InvalidateAfterDma(buf, len); // بعد از RX
 *
 * اگر کش را روشن نکنید، همهٔ این توابع بی‌اثرند و کد بدون تغییر کار
 * می‌کند.
 */
#ifndef FM_CACHE_EXAMPLE_H
#define FM_CACHE_EXAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "fm_board_config.h"

/* روی هاست (تست‌های PC) HAL وجود ندارد. */
#if !(defined(FM_HOST_BUILD) && FM_HOST_BUILD)
#include "stm32h7xx_hal.h"
#endif

#include "fm_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* پیاده‌سازی واقعی در include/fm_cache.h است. اینجا فقط برای سازگاری
 * با کدی که قبلاً این فایل را include می‌کرد نگه داشته شده. */

/* =================================================================
 * MPU آمادهٔ کپی — مخصوص همین پروژه
 * =================================================================
 *
 * تابع زیر را عیناً جایگزین MPU_Config() فعلی‌تان کنید.
 *
 * چهار ناحیه دارد و ترتیبشان مهم است: ناحیهٔ با شمارهٔ بالاتر روی
 * پایین‌تر را می‌پوشاند، پس ناحیهٔ بدون‌کش (۲) از دل SDRAM کش‌دار (۱)
 * بریده می‌شود.
 *
 *   ۰ : کل فضای آدرس، بدون دسترسی — هر اشاره‌گر خراب HardFault می‌دهد
 *   ۱ : SDRAM (۸MB)، کش‌دار write-back — اینجا سرعت به دست می‌آید
 *   ۲ : ۲۵۶KB آخر SDRAM، بدون کش — بافرهای DMA اینجا
 *   ۳ : QSPI (۳۲MB)، کش‌دار فقط‌خواندنی — برای حالت memory-mapped
 *
 * -----------------------------------------------------------------
 *
 *   static void MPU_Config(void)
 *   {
 *       MPU_Region_InitTypeDef r = {0};
 *
 *       HAL_MPU_Disable();
 *
 *       // --- ۰: پس‌زمینه، هیچ دسترسی -------------------------------
 *       r.Enable           = MPU_REGION_ENABLE;
 *       r.Number           = MPU_REGION_NUMBER0;
 *       r.BaseAddress      = 0x00000000;
 *       r.Size             = MPU_REGION_SIZE_4GB;
 *       r.SubRegionDisable = 0x87;
 *       r.TypeExtField     = MPU_TEX_LEVEL0;
 *       r.AccessPermission = MPU_REGION_NO_ACCESS;
 *       r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
 *       r.IsShareable      = MPU_ACCESS_SHAREABLE;
 *       r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
 *       r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
 *       HAL_MPU_ConfigRegion(&r);
 *
 *       // --- ۱: SDRAM، کش‌دار ---------------------------------------
 *       // اندازه ۸MB است نه ۳۲MB، و IsCacheable صریحاً نوشته شده.
 *       r.Number           = MPU_REGION_NUMBER1;
 *       r.BaseAddress      = 0xC0000000;
 *       r.Size             = MPU_REGION_SIZE_8MB;
 *       r.SubRegionDisable = 0x00;
 *       r.TypeExtField     = MPU_TEX_LEVEL1;     // write-back
 *       r.AccessPermission = MPU_REGION_FULL_ACCESS;
 *       r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
 *       r.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
 *       r.IsCacheable      = MPU_ACCESS_CACHEABLE;     // <-- حیاتی
 *       r.IsBufferable     = MPU_ACCESS_BUFFERABLE;    // <-- حیاتی
 *       HAL_MPU_ConfigRegion(&r);
 *
 *       // --- ۲: ۲۵۶KB آخر SDRAM، بدون کش (بافرهای DMA) --------------
 *       r.Number           = MPU_REGION_NUMBER2;
 *       r.BaseAddress      = 0xC0000000 + (8u*1024u*1024u) - (256u*1024u);
 *       r.Size             = MPU_REGION_SIZE_256KB;
 *       r.SubRegionDisable = 0x00;
 *       r.TypeExtField     = MPU_TEX_LEVEL0;
 *       r.AccessPermission = MPU_REGION_FULL_ACCESS;
 *       r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
 *       r.IsShareable      = MPU_ACCESS_SHAREABLE;
 *       r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
 *       r.IsBufferable     = MPU_ACCESS_BUFFERABLE;
 *       HAL_MPU_ConfigRegion(&r);
 *
 *       // --- ۳: QSPI در حالت memory-mapped --------------------------
 *       // بدون این ناحیه خواندن از 0x90000000 هارد-فالت می‌دهد.
 *       // فقط‌خواندنی است چون در حالت نگاشت نوشتن ممکن نیست.
 *       // اگر QSPI ندارید این ناحیه را حذف کنید.
 *       r.Number           = MPU_REGION_NUMBER3;
 *       r.BaseAddress      = 0x90000000;
 *       r.Size             = MPU_REGION_SIZE_32MB;
 *       r.SubRegionDisable = 0x00;
 *       r.TypeExtField     = MPU_TEX_LEVEL1;
 *       r.AccessPermission = MPU_REGION_PRIV_RO_URO;   // فقط خواندن
 *       r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
 *       r.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
 *       r.IsCacheable      = MPU_ACCESS_CACHEABLE;
 *       r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
 *       HAL_MPU_ConfigRegion(&r);
 *
 *       HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
 *   }
 *
 * و در main() به‌جای SCB_DisableDCache():
 *
 *       MPU_Config();
 *       FM_Cache_Enable();
 *       FM_Cache_Report();      // چاپ می‌کند: cache: icache=1 dcache=1
 *
 * -----------------------------------------------------------------
 * سه اشتباه رایج که در MPU قبلی این پروژه بود
 * -----------------------------------------------------------------
 * ۱) IsCacheable برای SDRAM نوشته نشده بود. ساختار بین فراخوانی‌ها
 *    دوباره استفاده می‌شود، پس مقدار NOT_CACHEABLE از ناحیهٔ ۰ باقی
 *    می‌ماند — یعنی SDRAM بدون کش می‌ماند و روشن‌کردن D-Cache هیچ
 *    سرعتی نمی‌دهد. هر فیلد را برای هر ناحیه صریح بنویسید.
 *
 * ۲) اندازه ۳۲MB بود در حالی که SDRAM ۸MB است.
 *
 * ۳) ناحیهٔ بدون‌کش برای بافرهای DMA وجود نداشت.
 *
 * -----------------------------------------------------------------
 * اگر بعد از روشن‌کردن کش تصویر خراب شد
 * -----------------------------------------------------------------
 * ۱) SCB_EnableDCache() را در main کامنت کنید. اگر درست شد، مقصر کش است.
 * ۲) بافر مشکل‌دار را به سکشن ".dma_buffer" ببرید (ناحیهٔ ۲).
 * ۳) یا در درایورش FM_Cache_CleanForDma / InvalidateAfterDma بگذارید.
 * ================================================================= */

#ifdef __cplusplus
}
#endif

#endif /* FM_CACHE_EXAMPLE_H */
