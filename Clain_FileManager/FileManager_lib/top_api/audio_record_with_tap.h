/*
 * audio_record_with_tap.h — Audio Record with Pre-I2S Tap (MyPCM) + Video Audio Access
 *
 *   SimpleAudio:  Pre/Post PCM taps before I2S, DSP, and recording
 *   SimpleVideo:  Frame+Audio access, global audio enable flag
 *   Directory:    QSPI <-> SD folder copy/move
 *
 * Wiring:
 *   1. Call SimpleAudio_SetPrePcmTap(my_dsp, NULL) before Begin.
 *      my_dsp receives raw decoded PCM before volume, you may edit in place.
 *      Use SimpleAudio_SetPostPcmTap for monitoring what speaker will get.
 *      SimpleAudio_SetTrackStart / SetTrackEnd tell the DSP WHICH track is
 *      being processed (path + codec/rate/channels on start, same path on
 *      end). Install AFTER Begin (Begin resets the player).
 *   2. For video, SimpleVideo_EnableAudio(0) to save SD/CPU when you only
 *      need pictures (mute in the processing path).
 *      SimpleVideo_SetTrackStartCallback / SetTrackEndCallback report which
 *      video started/ended; SimpleVideo_SetFrameCallback delivers each
 *      decoded frame even when no present callback is installed (data-only
 *      mode — the "60% of the time I only want the data" case).
 *      SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP) + SimpleVideo_Next() lets
 *      the USER advance to the next video manually instead of auto-play.
 *      SimpleVideo_GetFrame(&w,&h) gives RGB888, SimpleVideo_ReadAudio() PCM.
 *   3. Folder copy: Directory_Copy("/qspi/myFolder", "/sd/backup", progress, NULL)
 *      recurses depth-first across QSPI <-> SD; Directory_Move is copy+delete.
 *   4. Recording: SimpleAudio_RecStart("/sd/rec", 16000, 1) then feed PCM via
 *      tap or mic, then SimpleAudio_RecStop().
 */

#ifndef FM_TOPAPI_AUDIO_RECORD_WITH_TAP_H
#define FM_TOPAPI_AUDIO_RECORD_WITH_TAP_H

#include "fm_directory.h"
#include <string.h>
#include "../top_api/simple_audio.h"
#include "../top_api/simple_video.h"

/* ---------- 1. Pre-I2S DSP + Recording via MyPCM Tap ---------- */

static int16_t rec_ring[8192];
static volatile size_t rec_ring_len = 0;
static uint8_t do_record = 0;

/* Pre-gain tap: raw decoded PCM, editable. Here we apply a simple
 * 6 dB boost as DSP example and also copy to recording ring. */
static void audio_record_pre_dsp(void *ctx, int16_t *pcm, size_t samples,
                       uint32_t rate, uint16_t ch)
{
    (void)ctx; (void)rate; (void)ch;
    /* DSP: +6dB with saturation (example, replace with your filter) */
    for (size_t i = 0; i < samples; i++) {
        int32_t v = (int32_t)pcm[i] * 2;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
    /* Also feed recording if requested — just copy, don't write file here */
    if (do_record && samples <= 8192u) {
        memcpy(rec_ring, pcm, samples * sizeof(int16_t));
        rec_ring_len = samples;
    }
}

/* Post-gain tap: exactly what will be sent to I2S/DMA, after volume.
 * Useful for VU meter, FFT, or logging. Read-only is recommended. */
static void audio_record_post_tap(void *ctx, int16_t *pcm, size_t samples,
                        uint32_t rate, uint16_t ch)
{
    (void)ctx; (void)pcm; (void)samples; (void)rate; (void)ch;
    /* e.g., compute peak for UI */
}

/* Track lifecycle callbacks: the DSP learns which track is being processed.
 * start = path + codec/rate/channels; end = same path when it finishes. */
static void audio_record_track_start(void *ctx, const char *path,
                                 const FM_AudioInfo *info)
{
    (void)ctx;
    if (path == NULL) path = "";
    /* e.g. log "processing <path>", switch a DSP preset by codec, etc. */
    (void)info; // info->codec / sample_rate / channels
}
static void audio_record_track_end(void *ctx, const char *path,
                               const FM_AudioInfo *info)
{
    (void)ctx; (void)info;
    /* e.g. flush the DSP tail/buffers now that <path> finished */
    (void)path;
}

static void audio_record_tap_setup(void)
{
    SimpleAudio_SetPrePcmTap(audio_record_pre_dsp, NULL);
    SimpleAudio_SetPostPcmTap(audio_record_post_tap, NULL);
    /* Legacy alias still works: SimpleAudio_SetPcmTap(audio_record_post_tap, NULL) */
    /* Tell the DSP which music is playing / finished (after Begin). */
    SimpleAudio_SetTrackStart(audio_record_track_start, NULL);
    SimpleAudio_SetTrackEnd(audio_record_track_end, NULL);
}

/* Recording flow: start, pump via tap, stop.
 * Call this from main loop, not inside tap. */
static FM_Result audio_record_demo(void)
{
    FM_Result r;
    r = SimpleAudio_RecStart("/sd/rec", 16000, 1);
    if (r != FM_OK) return r;
    do_record = 1;
    /* While music plays, audio_record_pre_dsp will fill rec_ring.
     * In main loop, drain it: */
    // if (rec_ring_len) {
    //     SimpleAudio_RecWrite(rec_ring, rec_ring_len / 1u); // mono
    //     rec_ring_len = 0;
    // }
    // ... after a while ...
    // do_record = 0;
    // SimpleAudio_RecStop(); // writes WAV header, file at SimpleAudio_RecPath()
    return FM_OK;
}

/* ---------- 2. Video Frame + Audio Access, Global Audio Flag ---------- */

static void audio_record_video_pre_tap(void *ctx, int16_t *pcm, size_t samples,
                             uint32_t rate, uint16_t ch)
{
    (void)ctx; (void)rate; (void)ch;
    /* Video audio DSP before I2S — e.g., ducking, echo, or muting */
    for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)(pcm[i] * 0.5);
}
static void audio_record_video_post_tap(void *ctx, int16_t *pcm, size_t samples,
                              uint32_t rate, uint16_t ch)
{
    (void)ctx; (void)pcm; (void)samples; (void)rate; (void)ch;
}

/* Video track + frame callbacks: know which video started/ended, and get
 * each decoded frame even when there is no display (data-only). */
static void audio_record_video_track_start(void *ctx, const char *path,
                                 uint32_t w, uint32_t h, uint32_t fps)
{
    (void)ctx; (void)w; (void)h; (void)fps;
    /* e.g. log "processing <path>", reset a per-video feature extractor */
    (void)path;
}
static void audio_record_video_track_end(void *ctx, const char *path)
{
    (void)ctx;
    /* e.g. finalize/store a whole-video result now that <path> ended */
    (void)path;
}
static void audio_record_video_frame(void *ctx, const void *px, uint16_t w,
                           uint16_t h, FM_PixelFormat fmt, size_t bytes)
{
    (void)ctx; (void)w; (void)h; (void)fmt; (void)bytes;
    /* Process px here (brightness/mask/edge/feature).  The driver
     * double-buffers, so copy out if you keep it past this call. */
}

static void audio_record_video_setup(void)
{
    /* If you only need pictures (image processing), disable video audio
     * to save SD bandwidth and decoder CPU. Music is not affected. */
    // SimpleVideo_EnableAudio(0); // pictures only
    SimpleVideo_EnableAudio(1); // audio + pictures

    SimpleVideo_SetPrePcmTap(audio_record_video_pre_tap, NULL);
    SimpleVideo_SetPostPcmTap(audio_record_video_post_tap, NULL);

    /* Which video started/ended + per-frame data (data-only aware). */
    SimpleVideo_SetTrackStartCallback(audio_record_video_track_start, NULL);
    SimpleVideo_SetTrackEndCallback(audio_record_video_track_end, NULL);
    SimpleVideo_SetFrameCallback(audio_record_video_frame, NULL);

    /* If you want to advance to the NEXT video by hand (not auto):
     *   SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP);  // stop at EOF
     *   ... later, on a keypress: SimpleVideo_Next();
     */

    /* In your render loop: */
    // if (SimpleVideo_IsRunning()) {
    //     uint16_t w=0,h=0;
    //     const void *rgb = SimpleVideo_GetFrame(&w,&h); // RGB888
    //     // ... process rgb with Image_Convert / DSP ...
    //     FM_AudioTrackInfo ai;
    //     if (SimpleVideo_GetAudioInfo(&ai) == FM_OK) {
    //         // ai.codec, sample_rate, channels, bits
    //     }
    //     int16_t pcm[2048]; size_t got_frames;
    //     if (SimpleVideo_ReadAudio(pcm, 1024, &got_frames) == FM_OK) {
    //         // got_frames * ai.channels samples in pcm
    //     }
    // }
}

/* ---------- 3. Folder Copy (recursive, cross-mount) ---------- */

/* Copy a nested folder (default QSPI -> SD), any depth up to
 * FM_DIR_MAX_DEPTH.  Directory_Copy recurses depth-first while holding at
 * most one open directory handle, so it works across QSPI <-> SD.
 * Pass NULL/NULL for the documented board demo; explicit paths make
 * the helper reusable (and testable on the host POSIX backend). */
static FM_Result audio_record_folder_copy(const char *src, const char *dst)
{
    FM_Result r;
    if (src == NULL) src = "/qspi/myProject";
    if (dst == NULL) dst = "/sd/backup/myProject";
    r = Directory_Copy(src, dst, NULL, NULL);
    if (r != FM_OK) return r;

    /* Move is copy + delete (atomic fallback if rename not possible cross-mount) */
    // r = Directory_Move(dst, "/qspi/restored", NULL, NULL);
    return r;
}

/* ---------- 4. Entry point ---------- */
static inline FM_Result AudioRecordDemo_Run(void)
{
    audio_record_tap_setup();
    audio_record_video_setup();
    /* Start players as usual */
    // SimpleAudio_Begin();
    // SimpleVideo_Begin();
    // while (1) { SimpleAudio_Poll(); SimpleVideo_Poll(); ... }
    return FM_OK;
}

#endif
