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

    if (SD_examples_begin() != FM_OK)
    {
        printf("SD_examples_begin FAILED\n");
        return 1;
    }

    printf("\n************ exS_1_file_basic ************\n");
    exS_1_file_basic();

    printf("\n************ exS_2_file_copy_move_rename ************\n");
    exS_2_file_copy_move_rename();

    printf("\n************ exS_3_file_advance ************\n");
    exS_3_file_advance();

    printf("\n************ exS_4_file_directory ************\n");
    exS_4_file_directory();

    printf("\n************ exS_5_file_json ************\n");
    exS_5_file_json();

    printf("\n************ exS_6_file_text_line_edit ************\n");
    exS_6_file_text_line_edit();

    printf("\n************ exS_7_file_Crash_Safe_Writes ************\n");
    exS_7_file_Crash_Safe_Writes();

    printf("\n************ exS_8_file_Data_Logger ************\n");
    exS_8_file_Data_Logger();

    printf("\n************ exS_9_file_crc_cheaksum ************\n");
    exS_9_file_crc_cheaksum();

    printf("\n************ exS_10_file_BMP_Image_Scale_Convert ************\n");
    exS_10_file_BMP_Image_Scale_Convert();

    SD_examples_end();
    printf("\n===== ALL SD EXAMPLES DONE OK =====\n");
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
