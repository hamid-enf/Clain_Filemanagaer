/*
 * sdram_alloc.h — STM32H7 SDRAM allocator → FM_Alloc / h264bsd / codecs
 *
 * دیکودر H.264 (h264bsd) برای هر ویدیو چند صد کیلوبایت تا چند مگابایت
 * context و reference picture می‌خواهد و همه را از FM_Alloc می‌گیرد.
 * روی heap داخلی H7 (چند صد KB) جواب نمی‌دهد و نتیجه‌اش همان
 * «decode out of memory (-14)» است.
 *
 * این فایل یک bump allocator ساده با free-list روی SDRAM می‌سازد و
 * آن را به FM_Allocator_Set می‌دهد؛ از آن به بعد همهٔ FM_Alloc های
 * کتابخانه (h264bsd، ایندکس ویدیو، بافر فریم فشرده و ...) از SDRAM
 * تخصیص می‌گیرند و heap داخلی آزاد می‌ماند.
 *
 * استفاده (قبل از FileManager_Init یا حداقل قبل از اولین ویدیو):
 *
 *   #include "sdram_alloc.h"
 *   SdramAlloc_Init();          // یک خط — بعدش H.264 هم پخش می‌شود
 *
 * پیش‌فرض 8MB از سکشن .sdram_buffers می‌گیرد؛ با ماکرو عوض کنید:
 *
 *   #define SDRAM_ALLOC_BYTES (16u * 1024u * 1024u)
 *   #include "sdram_alloc.h"
 *
 * نکته: چون D-Cache در پروژهٔ شما خاموش است (SCB_DisableDCache در
 * main.c) نیازی به mgmt کش نیست. اگر روزی D-Cache را روشن کردید،
 * ناحیهٔ SDRAM را با MPU به Write-Through تبدیل کنید.
 *
 * الگوریتم: first-fit با free-list و ادغام بلوک‌های مجاور آزاد؛ برای
 * الگوی تخصیص codec ها (چند بلوک بزرگ با عمر طولانی) کافی و بدون
 * تکه‌تکه‌شدن قابل توجه است. 8-byte aligned.
 */

#ifndef FM_TOPAPI_SDRAM_ALLOC_H
#define FM_TOPAPI_SDRAM_ALLOC_H


#ifndef SDRAM_ALLOC_H_
#define SDRAM_ALLOC_H_

#include <stddef.h>
#include "fm_board_config.h"   /* آدرس‌ها و پرچم‌های مرکزی */
#include <stdint.h>
#include <string.h>

#include "fm_alloc.h"

#ifndef SDRAM_ALLOC_BYTES
#define SDRAM_ALLOC_BYTES (8u * 1024u * 1024u)
#endif
#ifndef SDRAM_ALLOC_SECTION
#define SDRAM_ALLOC_SECTION ".sdram_buffers"
#endif

__attribute__((section(SDRAM_ALLOC_SECTION), aligned(8)))
static uint8_t sdram_alloc_pool[SDRAM_ALLOC_BYTES];

typedef struct SdramBlock
{
    size_t size;                 /* payload size (aligned)          */
    struct SdramBlock *next;     /* next free block (free list)     */
    uint32_t magic;              /* alloc guard                     */
    uint32_t pad;                /* keep header 16 bytes / aligned  */
} SdramBlock;

#define SDRAM_ALLOC_MAGIC_USED 0x53445241u /* "SDRA" */
#define SDRAM_ALLOC_MAGIC_FREE 0x46524545u /* "FREE" */
#define SDRAM_ALLOC_ALIGN 8u

static SdramBlock *sdram_free_list;
static uint8_t sdram_alloc_ready;

static size_t sdram_align_up(size_t n)
{
    return (n + (SDRAM_ALLOC_ALIGN - 1u)) & ~(size_t)(SDRAM_ALLOC_ALIGN - 1u);
}

static void sdram_pool_reset(void)
{
    SdramBlock *b = (SdramBlock *)(void *)sdram_alloc_pool;

    b->size = SDRAM_ALLOC_BYTES - sizeof(SdramBlock);
    b->next = NULL;
    b->magic = SDRAM_ALLOC_MAGIC_FREE;
    sdram_free_list = b;
}

/* Merge physically adjacent free blocks (list is address-sorted). */
static void sdram_coalesce(void)
{
    SdramBlock *b = sdram_free_list;

    while (b != NULL && b->next != NULL)
    {
        uint8_t *end = (uint8_t *)b + sizeof(SdramBlock) + b->size;

        if (end == (uint8_t *)b->next)
        {
            b->size += sizeof(SdramBlock) + b->next->size;
            b->next = b->next->next;
        }
        else
        {
            b = b->next;
        }
    }
}

static void *sdram_alloc_fn(size_t size, void *ctx)
{
    SdramBlock **prev = &sdram_free_list;
    SdramBlock *b = sdram_free_list;

    (void)ctx;
    if (size == 0u)
    {
        return NULL;
    }
    size = sdram_align_up(size);
    while (b != NULL)
    {
        if (b->size >= size)
        {
            /* split if the remainder is worth keeping */
            if (b->size >= size + sizeof(SdramBlock) + SDRAM_ALLOC_ALIGN)
            {
                SdramBlock *rest = (SdramBlock *)(void *)
                    ((uint8_t *)b + sizeof(SdramBlock) + size);

                rest->size = b->size - size - sizeof(SdramBlock);
                rest->next = b->next;
                rest->magic = SDRAM_ALLOC_MAGIC_FREE;
                b->size = size;
                *prev = rest;
            }
            else
            {
                *prev = b->next;
            }
            b->next = NULL;
            b->magic = SDRAM_ALLOC_MAGIC_USED;
            return (uint8_t *)b + sizeof(SdramBlock);
        }
        prev = &b->next;
        b = b->next;
    }
    return NULL; /* pool exhausted -> caller sees FM_ERR_NO_MEMORY */
}

static void sdram_free_fn(void *p, void *ctx)
{
    SdramBlock *b;
    SdramBlock **prev;
    SdramBlock *it;

    (void)ctx;
    if (p == NULL)
    {
        return;
    }
    b = (SdramBlock *)(void *)((uint8_t *)p - sizeof(SdramBlock));
    if (b->magic != SDRAM_ALLOC_MAGIC_USED)
    {
        return; /* not ours / double free — ignore */
    }
    b->magic = SDRAM_ALLOC_MAGIC_FREE;
    /* insert address-sorted so coalescing works */
    prev = &sdram_free_list;
    it = sdram_free_list;
    while (it != NULL && it < b)
    {
        prev = &it->next;
        it = it->next;
    }
    b->next = it;
    *prev = b;
    sdram_coalesce();
}

static void *sdram_calloc_fn(size_t n, size_t size, void *ctx)
{
    void *p;
    size_t total = n * size;

    if (n != 0u && total / n != size)
    {
        return NULL; /* overflow */
    }
    p = sdram_alloc_fn(total, ctx);
    if (p != NULL)
    {
        memset(p, 0, total);
    }
    return p;
}

static void *sdram_realloc_fn(void *p, size_t size, void *ctx)
{
    SdramBlock *b;
    void *np;

    if (p == NULL)
    {
        return sdram_alloc_fn(size, ctx);
    }
    if (size == 0u)
    {
        sdram_free_fn(p, ctx);
        return NULL;
    }
    b = (SdramBlock *)(void *)((uint8_t *)p - sizeof(SdramBlock));
    if (b->magic == SDRAM_ALLOC_MAGIC_USED && b->size >= size)
    {
        return p; /* shrink in place */
    }
    np = sdram_alloc_fn(size, ctx);
    if (np != NULL && b->magic == SDRAM_ALLOC_MAGIC_USED)
    {
        memcpy(np, p, (b->size < size) ? b->size : size);
        sdram_free_fn(p, ctx);
    }
    return np;
}

/* Route every FM_Alloc / FM_Calloc / FM_Realloc / FM_Free (including
 * h264bsd, whose malloc/free are remapped to FM_Alloc/FM_Free by the
 * build) to the SDRAM pool.  Returns 0 on success. */
static int SdramAlloc_Init(void)
{
    FM_Allocator a;

    if (!sdram_alloc_ready)
    {
        sdram_pool_reset();
        sdram_alloc_ready = 1u;
    }
    a.alloc = sdram_alloc_fn;
    a.calloc = sdram_calloc_fn;
    a.realloc = sdram_realloc_fn;
    a.free = sdram_free_fn;
    a.ctx = NULL;
    return FM_Allocator_Set(&a);
}

/* Is a given pointer inside our pool?  Used by the self-test below. */
static int SdramAlloc_Owns(const void *p)
{
    const uint8_t *q = (const uint8_t *)p;

    return (q >= sdram_alloc_pool) &&
           (q < sdram_alloc_pool + SDRAM_ALLOC_BYTES);
}

/* Total pool size in bytes. */
__attribute__((unused))   /* optional helper: not every project calls it */
static size_t SdramAlloc_TotalBytes(void)
{
    return (size_t)SDRAM_ALLOC_BYTES;
}

/* Prove at runtime that FM_Alloc really lands in the SDRAM pool.
 *
 * A silent mismatch here is the single most common cause of H.264
 * "out of memory (-14)" on a board that *looks* correctly configured:
 * SdramAlloc_Init() was called, but something later re-installed the
 * default malloc allocator, or the library was built against a
 * different fm_alloc.c than the one this header links to, so FM_Alloc
 * still hands out a few hundred bytes from the tiny internal heap.
 *
 * Call it once after SdramAlloc_Init() and print the result:
 *
 *   SdramAlloc_Init();
 *   printf("sdram alloc self-test: %d free=%u/%u\r\n",
 *          SdramAlloc_SelfTest(),
 *          (unsigned)SdramAlloc_FreeBytes(),
 *          (unsigned)SdramAlloc_TotalBytes());
 *
 * Returns 0 when FM_Alloc is wired to the pool, -1 when it is not
 * (allocation came from somewhere else) and -2 when the allocation
 * failed outright.  A 1 MB probe is used because that is the order of
 * magnitude h264bsd needs for a single 720p reference picture. */
static int SdramAlloc_SelfTest(void)
{
    void *p = FM_Alloc(1024u * 1024u);
    int rc;

    if (p == NULL)
    {
        return -2;
    }
    rc = SdramAlloc_Owns(p) ? 0 : -1;
    FM_Free(p);
    return rc;
}

/* Bytes currently free in the pool (largest contiguous run may be less). */
static size_t SdramAlloc_FreeBytes(void)
{
    size_t total = 0;
    const SdramBlock *b = sdram_free_list;

    while (b != NULL)
    {
        total += b->size;
        b = b->next;
    }
    return total;
}

#endif /* SDRAM_ALLOC_H_ */

#endif /* FM_TOPAPI_SDRAM_ALLOC_H */
