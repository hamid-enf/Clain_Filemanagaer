/*
 * diag_heap_test.c
 *
 * تست تشخیصی: Heap_Size و Stack_Size رو در linker script چک کنید
 *
 * مشکل: printf در newlib-nano از _sbrk() استفاده می‌کنه. اگه Heap_Size
 * در linker script خیلی کم باشه یا با stack overlap داشته باشه،
 * _sbrk fail می‌کنه و printf کرش می‌کنه.
 *
 * این فایل heap رو تست می‌کنه بدون استفاده از FileManager.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern char _end;     /* end of BSS */
extern char _estack;  /* top of stack */
extern char _Min_Heap_Size;
extern char _Min_Stack_Size;

void test_heap(void) {
    uint32_t heap_available = (uint32_t)((char *)&_estack - &_end);
    printf("=== Heap Diagnostic ===\n");
    printf("_end       = 0x%08X\n", (uint32_t)&_end);
    printf("_estack    = 0x%08X\n", (uint32_t)&_estack);
    printf("available  = %u bytes\n", heap_available);
    printf("Min_Heap   = %u bytes (0x%X)\n",
           (uint32_t)&_Min_Heap_Size, (uint32_t)&_Min_Heap_Size);
    printf("Min_Stack  = %u bytes (0x%X)\n",
           (uint32_t)&_Min_Stack_Size, (uint32_t)&_Min_Stack_Size);
    fflush(stdout);

    /* تست: malloc بزرگ */
    printf("Testing malloc(512)... ");
    fflush(stdout);
    void *p = malloc(512);
    if (p) {
        printf("OK at 0x%08X\n", (uint32_t)p);
        free(p);
    } else {
        printf("FAILED!\n");
    }
    fflush(stdout);

    /* تست: malloc بزرگ‌تر */
    printf("Testing malloc(2048)... ");
    fflush(stdout);
    p = malloc(2048);
    if (p) {
        printf("OK at 0x%08X\n", (uint32_t)p);
        free(p);
    } else {
        printf("FAILED!\n");
    }
    fflush(stdout);

    /* تست: printf سنگین */
    printf("Testing printf(\"%%llu\", value)... ");
    fflush(stdout);
    uint64_t v = 1234567890123ULL;
    printf("%llu\n", v);
    printf("  (if you see 1234567890123, printf works)\n");
    fflush(stdout);
}

int main(void) {
    test_heap();
    return 0;
}
