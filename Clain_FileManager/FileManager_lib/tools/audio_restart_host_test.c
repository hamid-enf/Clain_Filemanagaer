/*
 * audio_restart_host_test.c - host validation for the SimpleAudio
 * auto-restart budget (audit AUD5-02).
 *
 * A playlist folder holds two tracks:
 *   a.mp3  an MP3 whose stream changes form (two files joined back to back:
 *          22.05 kHz then 48 kHz).  SimpleAudio reads it, the read after
 *          the change is FM_ERR_UNSUPPORTED, and the refill fails there.
 *   b.wav  a playable WAV, the next track.
 *
 * The simulated I2S halves drive the player.  A failing track gets
 * SIMPLE_AUDIO_MAX_RESTARTS restarts; after that the player must give up
 * on it and go on with the next track of the playlist.  Before the fix
 * the budget was cleared on the first successful poll after each restart,
 * so the same track restarted for ever and the player never reached the
 * next one.
 *
 *   gcc -Iinclude -Iexamples -Ibackends -Isrc -Itools/halstub \
 *       tools/audio_restart_host_test.c libfilemanager.a -lm -o fm_audiorestart
 */
/* FM-NOT-FIRMWARE: host-only file (make / CMake on a PC).
 * STM32CubeIDE compiles every .c under FileManager_lib; in a firmware
 * build (USE_HAL_DRIVER, set by every CubeMX project) this file compiles
 * to nothing, so it cannot clash with the project. */
#ifndef USE_HAL_DRIVER

#include <stdio.h>
#include <string.h>

#include "file_manager.h"
#include "fm_filesystem.h"

extern FM_Result PosixFS_Init(FM_FS *fs);

#ifndef SIMPLE_AUDIO_FOLDER
#define SIMPLE_AUDIO_FOLDER "/aud5r"
#endif
#ifndef SIMPLE_AUDIO_HALF_SAMPLES
#define SIMPLE_AUDIO_HALF_SAMPLES 512u
#endif

#include "simple_audio.h"

I2S_HandleTypeDef hi2s1;
I2S_HandleTypeDef hi2s2;

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

static int starts, ends;

static void on_start(void *ctx, const char *path, const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    starts++;
    printf("    start %s\n", path);
}

static void on_end(void *ctx, const char *path, const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    ends++;
    printf("    end   %s\n", path);
}

int main(void)
{
    FM_FS fs;
    FM_Result r;
    int n;
    uint32_t restarts;

    (void)FileManager_Init();
    (void)PosixFS_Init(&fs);
    if (FS_Mount("/", &fs) != FM_OK)
    {
        printf("cannot mount the POSIX filesystem at /\n");
        return 1;
    }
    (void)Directory_DeleteRecursive("/aud5r");
    r = Directory_Create("/aud5r");
    printf("    Directory_Create -> %d\n", (int)r);
    if (r != FM_OK && r != FM_ERR_EXISTS)
    {
        printf("cannot create /aud5r\n");
        return 1;
    }
    {
        static uint8_t buf[200000];
        size_t rd = 0;

        if (File_ReadAll("/test/vectors/mp3_m2_22k_32k.mp3", buf,
                         sizeof(buf) / 2u, &rd) != FM_OK || rd == 0u ||
            File_AppendAll("/aud5r/a.mp3", buf, rd) != FM_OK)
        {
            printf("cannot build a.mp3 (read %d, rd %lu)\n", (int)r, (unsigned long)rd);
            return 1;
        }
        rd = 0;
        if (File_ReadAll("/test/vectors/tone_mono.mp3", buf,
                         sizeof(buf), &rd) != FM_OK || rd == 0u ||
            File_AppendAll("/aud5r/a.mp3", buf, rd) != FM_OK)
        {
            printf("cannot append to a.mp3\n");
            return 1;
        }
        rd = 0;
        if (File_ReadAll("/test/vectors/wav_alaw_8k.wav", buf, sizeof(buf),
                         &rd) != FM_OK || rd == 0u ||
            File_WriteAll("/aud5r/b.wav", buf, rd) != FM_OK)
        {
            printf("cannot build b.wav (rd %lu)\n", (unsigned long)rd);
            return 1;
        }
        (void)Directory_Create("/aud5r/z");
        if (File_WriteAll("/aud5r/z/.keep", "", 0u) == FM_OK)
        {
            (void)File_Delete("/aud5r/z/.keep");
        }
    }

    printf("\nAudio restart budget (AUD5-02)\n");
    printf("=============================\n\n");

    SimpleAudio_SetTrackStartCallback(on_start, NULL);
    SimpleAudio_SetTrackEndCallback(on_end, NULL);
    SimpleAudio_SetPlayMode(FM_PLAY_MODE_NEXT);

    {
        FM_Player p;
        uint32_t i;
        (void)Player_Init(&p);
        printf("    scan -> %d count %lu\n",
               (int)Player_ScanFolder(&p, "/aud5r", FM_PLAYER_KIND_AUDIO),
               (unsigned long)Player_Count(&p));
        for (i = 0; i < Player_Count(&p); i++)
            printf("      [%u] %s\n", (unsigned)i, Player_ItemPath(&p, i));
        Player_Deinit(&p);
    }
    r = SimpleAudio_Begin();
    printf("    SimpleAudio_Begin -> %d (%s) folder=%s\n", (int)r,
           FM_ResultString(r), SIMPLE_AUDIO_FOLDER);
    ok("the playlist starts", r == FM_OK);
    if (r == FM_OK)
    {
        /* Enough halves for the failing track to exhaust its budget and
         * for the player to reach the second track.  Each half is 512
         * channel samples, ~64 ms at 8 kHz. */
        for (n = 0; n < 20000 && SimpleAudio_IsRunning(); n++)
        {
            if ((n & 1) == 0) AudioDma_HalfCallback(&sa.dma);
            else              AudioDma_FullCallback(&sa.dma);
            (void)SimpleAudio_Poll();
            if (Player_Current(&sa.dma.player) == 1u)
            {
                break;
            }
        }
        restarts = sa.restart_count;
        printf("    halves %d, starts %d, restarts now %u, running %d, playing %s\n",
               n, starts, (unsigned)restarts, (int)SimpleAudio_IsRunning(),
               Player_ItemPath(&sa.dma.player,
                               Player_Current(&sa.dma.player)));
        ok("the failing track is given up on (the player stops)",
           SimpleAudio_IsRunning() == 0);
        ok("its restarts are bounded by SIMPLE_AUDIO_MAX_RESTARTS",
           starts <= (int)SIMPLE_AUDIO_MAX_RESTARTS + 1);
        (void)SimpleAudio_Stop();
    }
    (void)Directory_DeleteRecursive("/aud5r");

    printf("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES",
           fails, fails == 1 ? "" : "s");
    return (fails == 0) ? 0 : 1;
}

#endif /* USE_HAL_DRIVER */
