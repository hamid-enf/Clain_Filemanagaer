/*
 * Host runner for examples/examples_sdcard.h — executes the exact
 * SD-card example sequence against the mock FatFS so the header is
 * compile- and run-verified before it goes on the board.
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include "../examples/examples_sdcard.h"

int main(void)
{
    /* The examples print through FM_Log (as on the board, where that is
     * the UART).  Without a sink every line -- the "!!" of a step that
     * failed included -- went nowhere and this runner could not fail. */
    FM_Log_UseStdout();

    if (SdCard_Begin() != FM_OK)
    {
        printf("SdCard_Begin FAILED\n");
        return 1;
    }

    printf("\n************ SdDemo_Basic ************\n");
    SdDemo_Basic();

    printf("\n************ SdDemo_CopyMoveRename ************\n");
    SdDemo_CopyMoveRename();

    printf("\n************ SdDemo_Advanced ************\n");
    SdDemo_Advanced();

    printf("\n************ SdDemo_Directory ************\n");
    SdDemo_Directory();

    printf("\n************ SdDemo_Json ************\n");
    SdDemo_Json();

    printf("\n************ SdDemo_TextLineEdit ************\n");
    SdDemo_TextLineEdit();

    printf("\n************ SdDemo_CrashSafeWrites ************\n");
    SdDemo_CrashSafeWrites();

    printf("\n************ SdDemo_DataLogger ************\n");
    SdDemo_DataLogger();

    printf("\n************ SdDemo_CrcChecksum ************\n");
    SdDemo_CrcChecksum();

    printf("\n************ SdDemo_BmpScaleConvert ************\n");
    SdDemo_BmpScaleConvert();

    SdCard_End();
    printf("\n===== ALL SD EXAMPLES DONE OK =====\n");
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
