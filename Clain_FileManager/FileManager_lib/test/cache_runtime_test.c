/*
 * cache_runtime_test.c — اثبات اینکه نگهداری کش به وضعیت واقعی وابسته است
 *
 * این تست همان باگی را می‌گیرد که به صدا و تصویر نویز می‌داد: کش در main
 * روشن بود، ولی درایورها به یک ماکروی کامپایل‌تایم نگاه می‌کردند که
 * تعریف نشده بود، پس هیچ Clean/Invalidate ای انجام نمی‌شد.
 *
 * روش: یک SCB جعلی می‌سازیم، بیت D-Cache را دستی عوض می‌کنیم، و
 * می‌شماریم که عملیات کش انجام شد یا نه.
 *
 * جدا از fm_test است چون باید هدرهای CMSIS را جعل کند.
 * اجرا:  make cachetest
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

typedef struct { volatile uint32_t CCR; } SCB_Type;
static SCB_Type scb_mem;
#define SCB (&scb_mem)
#define SCB_CCR_DC_Msk (1u<<16)
#define SCB_CCR_IC_Msk (1u<<17)
#define __DCACHE_PRESENT 1U
static int dsb_calls;
#define __DSB() (dsb_calls++)

/* FM_Cache_Report/CheckDmaBuffer از FM_Log استفاده می‌کنند و این تست
 * standalone است (به کتابخانه لینک نمی‌شود). */
int FM_Log(const char *fmt, ...);
int FM_Log(const char *fmt, ...) { (void)fmt; return 0; }

static int isb_calls;
#define __ISB() (isb_calls++)

/* Enable/Disable ساختگی: مثل CMSIS واقعی بیت را در CCR عوض می‌کنند،
 * تا تست بتواند اثر CACHE(ON)/CACHE(OFF) را ببیند. */
static void SCB_EnableDCache(void){ scb_mem.CCR |= SCB_CCR_DC_Msk; }
static void SCB_DisableDCache(void){ scb_mem.CCR &= ~SCB_CCR_DC_Msk; }
static void SCB_EnableICache(void){ scb_mem.CCR |= SCB_CCR_IC_Msk; }
static void SCB_DisableICache(void){ scb_mem.CCR &= ~SCB_CCR_IC_Msk; }

static int clean_calls, inval_calls;
static void SCB_CleanDCache_by_Addr(uint32_t *a,int32_t n){(void)a;(void)n;clean_calls++;}
static void SCB_InvalidateDCache_by_Addr(uint32_t *a,int32_t n){(void)a;(void)n;inval_calls++;}

#define FM_BUFFERS_SWITCH BUFFERS_NORMAL
#include "fm_switches.h"
#include "fm_cache.h"

int main(void){
    static char buf[256] __attribute__((aligned(32)));
    int fail=0;

    /* حالت ۱: کش خاموش -> هیچ عملیات کشی، ولی DSB *باید* بزند.
     *
     * این مهم‌ترین تست این فایل است. حتی با کش خاموش، نوشته‌های CPU
     * می‌توانند در write buffer معلق بمانند و DMA دادهٔ کهنه بخواند.
     * یک بار DSB را داخل شرط کش گذاشتم و نتیجه‌اش خش صدا و پرش تصویر
     * بود، با کشی که اصلاً روشن نبود. */
    scb_mem.CCR = 0;
    clean_calls=inval_calls=dsb_calls=0;
    FM_Cache_CleanForDma(buf,sizeof buf);
    FM_Cache_InvalidateAfterDma(buf,sizeof buf);
    printf("cache OFF : clean=%d inval=%d dsb=%d  (باید 0 0 2)\n",
           clean_calls,inval_calls,dsb_calls);
    if(clean_calls||inval_calls){puts("  FAIL: عملیات کش با کش خاموش");fail=1;}
    if(dsb_calls!=2){puts("  FAIL: DSB نزد -> DMA داده کهنه می خواند -> خش");fail=1;}
    if(FM_Cache_IsDataEnabled()){puts("  FAIL: گزارش اشتباه");fail=1;}

    /* حالت ۲: کش روشن -> باید انجام شود. این همان باگ نویز بود. */
    scb_mem.CCR = SCB_CCR_DC_Msk|SCB_CCR_IC_Msk;
    clean_calls=inval_calls=dsb_calls=0;
    FM_Cache_CleanForDma(buf,sizeof buf);
    FM_Cache_InvalidateAfterDma(buf,sizeof buf);
    printf("cache ON  : clean=%d inval=%d dsb=%d  (باید >0 >0 2)\n",
           clean_calls,inval_calls,dsb_calls);
    if(dsb_calls!=2){puts("  FAIL: DSB نزد");fail=1;}
    if(!clean_calls){puts("  FAIL: کش روشن ولی Clean نشد -> نویز صدا/تصویر");fail=1;}
    if(!inval_calls){puts("  FAIL: کش روشن ولی Invalidate نشد");fail=1;}
    if(!FM_Cache_IsDataEnabled()||!FM_Cache_IsInstEnabled()){puts("  FAIL: گزارش اشتباه");fail=1;}

    /* حالت ۳: بافر ناهم‌تراز -> پروتکل جدید RX:
     * Clean قبل از کیک (محافظت همسایه‌ها) + Invalidate خالص بعد از DMA.
     * (قبلاً InvalidateAfterDma خودش لبه‌ها را Clean می‌کرد؛ روی برد
     * محتوای کهنهٔ خط لبه را روی دادهٔ تازهٔ DMA می‌نوشت — باگ واقعی
     * «آخرین ۸ بایت خواندن I2C صفر شد». حالا Clean در سایتِ RX قبل از
     * انتقال انجام می‌شود: fm_xdma.h و i2c_eeprom_dma.h / spi_flash_dma_port.h.) */
    clean_calls=inval_calls=0;
    FM_Cache_CleanForDma(buf+5,100);            /* سایت RX: قبل از کیک */
    FM_Cache_InvalidateAfterDma(buf+5,100);     /* بعد: فقط Invalidate */
    printf("unaligned : clean=%d inval=%d  (هر دو باید >0)\n",clean_calls,inval_calls);
    if(clean_calls==0||inval_calls==0){puts("  FAIL: پروتکل RX ناقص");fail=1;}
    clean_calls=inval_calls=0;
    FM_Cache_InvalidateAfterDma(buf+5,100);     /* خالص: دیگر Clean ندارد */
    if(clean_calls!=0){puts("  FAIL: InvalidateAfterDma نباید Clean کند (بازنویسی دادهٔ تازهٔ DMA)");fail=1;}
    else{puts("  invalidate-only بعد از DMA: تایید");}

    /* حالت ۴: ورودی بد نباید کرش کند */
    FM_Cache_CleanForDma(NULL,100); FM_Cache_CleanForDma(buf,0);
    FM_Cache_InvalidateAfterDma(NULL,100); FM_Cache_InvalidateAfterDma(buf,0);
    puts("bad args  : بدون کرش");

    /* حالت ۵: FM_CACHE_HW باید ۱ باشد وقتی سخت‌افزار کش دارد.
     * اگر صفر باشد، همهٔ چهار تست بالا هم پاس می‌شوند — چون همه‌چیز
     * no-op است — ولی روی برد هیچ نگهداری‌ای انجام نمی‌شود. این همان
     * حالتی است که به خش صدا و پرش تصویر منجر شد. */
    printf("FM_CACHE_HW=%d  (باید 1 چون __DCACHE_PRESENT هست)\n",
           FM_CACHE_HW);
    if (FM_CACHE_HW != 1)
    {
        puts("  FAIL: نگهداری کش کلا از بیلد حذف شده");
        fail = 1;
    }

    /* حالت ۶: CheckDmaBuffer باید بافر نامتراز را لو بدهد.
     * این همان دامی است که با کش روشن متغیر همسایه را می‌خورد. */
    {
        static FM_CACHE_ALIGN uint8_t good[64];

        if (!FM_Cache_CheckDmaBuffer(good, sizeof(good), "aligned"))
        {
            puts("  FAIL: بافر سالم رد شد");
            fail = 1;
        }
        if (FM_Cache_CheckDmaBuffer(good + 1, 64, "misaligned addr"))
        {
            puts("  FAIL: آدرس نامتراز قبول شد");
            fail = 1;
        }
        if (FM_Cache_CheckDmaBuffer(good, 63, "odd length"))
        {
            puts("  FAIL: طول نامضرب ۳۲ قبول شد");
            fail = 1;
        }
        puts("dma check : تراز آدرس و طول بررسی شد");
    }

    /* ۷) کلیدهای CACHE(...) و BUFFERS(...) ------------------------- */
    /* این تست fm_board_config.h را include نمی‌کند (مستقل است)، پس
     * فقط خود ماکروهای کلید را از fm_switches.h می‌سنجیم: باید به
     * عددِ درست باز شوند و در #if قابل استفاده باشند. */
    {
        /* فلگ FM_CACHE: باید ON=1 و OFF=0 بدهد و در #if کار کند. */
        if (ON != 1 || OFF != 0) {
            puts("  FAIL: ON/OFF عدد درست نمی‌دهند");
            fail = 1;
        }
#if ON != 1
        puts("  FAIL: ON در #if کار نکرد");
        fail = 1;
#endif
        if (!(SMALL < NORMAL && NORMAL < LARGE && LARGE < HUGE)) {
            puts("  FAIL: ترتیب اندازه‌های بافر درست نیست");
            fail = 1;
        }
        /* FM_Cache_Apply هنوز مستقیم قابل استفاده است (کامپایلر
         * غیر GCC، یا کنترل دستی). باید واقعاً بیت را عوض کند. */
        FM_Cache_Apply(1);
        if (!FM_Cache_IsDataEnabled()) {
            puts("  FAIL: FM_Cache_Apply(1) کش را روشن نکرد");
            fail = 1;
        }
        FM_Cache_Apply(0);
        if (FM_Cache_IsDataEnabled()) {
            puts("  FAIL: FM_Cache_Apply(0) کش را خاموش نکرد");
            fail = 1;
        }
        puts("switches  : فلگ FM_CACHE و اندازه‌های بافر درست‌اند");
    }

    puts(fail?"\n=== FAIL ===":"\n=== همه درست ===");
    return fail;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
