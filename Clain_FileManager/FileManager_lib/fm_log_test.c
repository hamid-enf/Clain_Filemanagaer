/*
 * fm_log_test.c
 *
 * این فایل رو به پروژه STM32 اضافه کنید (بدون FileManager)
 * تا مطمئن بشید FM_Log درست کار می‌کنه و stack overflow
 * نیست.
 *
 * در main.c:
 *   #include "fm_log_test.h"
 *   در main(): fm_log_test_run();
 */

/* FM-NOT-FIRMWARE: optional diagnostic, off by default.
 * STM32CubeIDE compiles every .c under FileManager_lib, and it is a one-off FM_Log self-test.
 * It compiles to nothing unless the project defines FM_DIAG_LOG_TEST.
 * The rule is checked by tools/check_cubeide.sh. */
#ifdef FM_DIAG_LOG_TEST

#include "fm_log.h"

void fm_log_test_run(void)
{
    FM_LOG("\n=== FM_Log Test ===\n");
    FM_LOG("[1] Integer: %d\n", 42);
    FM_LOG("[2] Hex: 0x%08X\n", 0xDEADBEEF);
    FM_LOG("[3] Float: %.2f\n", 3.14);
    FM_LOG("[4] String: %s\n", "hello");
    FM_LOG("[5] Multiple: %d %s %.1f\n", 100, "test", 1.5);
    FM_LOG("[6] Pointer: 0x%08X\n", (unsigned int)&fm_log_test_run);
    FM_LOG("=== END ===\n");
}

#else  /* FM_DIAG_LOG_TEST */
typedef int fm_not_firmware_t;
#endif /* FM_DIAG_LOG_TEST */
