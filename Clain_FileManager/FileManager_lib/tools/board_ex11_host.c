/*
 * board_ex11_host.c — host validation for board/examples_file.h
 *
 * ex_11_audio_codecs() is written for the CubeIDE board project, but it
 * deliberately uses only the portable core API (no HAL).  This harness
 * includes the very same header the board uses and runs the example
 * against real assets, so what ships in board/ is executed, not just
 * eyeballed.
 *
 *   mkdir -p /tmp/boardhost/media && cd /tmp/boardhost
 *   cp audio_test files and test/vectors files into media/
 *   gcc -Iinclude -Ibackends -Isrc -DFM_HOST_BUILD=1 \
 *       tools/board_ex11_host.c libfilemanager.a -lm -o ex11host
 *   ./ex11host | tee ex11.log
 *
 * PosixFS is mounted at "/" (= the process CWD, /tmp/boardhost), so
 * ex_file_begin()'s '/' check passes and ex_11_find()'s "/media/<x>"
 * probe resolves to ./media/<x> — the copied assets.  A
 * "skip (not on SD)" line means assets were not copied: harness setup
 * problem, not a library problem.
 */

/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project.  The rule is checked
 * by tools/check_cubeide.sh. */
#ifndef USE_HAL_DRIVER

#include <stdio.h>
#include "file_manager.h"
#include "fm_filesystem.h"
#include "fm_log.h"

extern FM_Result PosixFS_Init(FM_FS *fs);

#include "../board/examples_file.h"

int main(void)
{
    FM_FS fs;

    if (FileManager_Init() != FM_OK ||
        PosixFS_Init(&fs) != FM_OK ||
        FS_Mount("/", &fs) != FM_OK)
    {
        fprintf(stderr, "FATAL: fs setup\n");
        return 1;
    }

    FM_Log_UseStdout();
    ex_11_audio_codecs();
    return 0;
}

#else  /* USE_HAL_DRIVER */
typedef int fm_not_firmware_t;
#endif /* USE_HAL_DRIVER */
