/*
 * audio_dma_player.h — STM32H7 bare-metal audio player with circular DMA
 *
 * این bridge خروجی PCM لایه‌ی Player را به یک DMA دو‌نیمه وصل می‌کند، بدون
 * اینکه به مدل codec صوتی یا peripheral خاصی وابسته باشد. کاربرد معمول:
 *
 *   SD/FatFS -> Player_ReadAudio -> [half 0 | half 1] -> SAI/I2S/DAC DMA
 *
 * قانون مهم: callback وقفه فقط AudioDma_HalfCallback یا
 * AudioDma_FullCallback را صدا می‌زند. خواندن SD، decode و cache clean
 * همگی در AudioDma_Poll داخل super-loop انجام می‌شوند.
 *
 * نمونه‌ی اتصال در main.c:
 *
 *   static AudioDma audio;
 *
 *   // بافر DMA نباید در DTCM باشد. سکشن را مطابق linker script خودتان
 *   // در AXI SRAM یا SRAM1/2 قرار دهید. با D-Cache، alignment و اندازه‌ی
 *   // هر نیمه را مضرب 32 نگه دارید.
 *   #define AUDIO_HALF_SAMPLES 8192u  // 85ms per half at 48kHz stereo
 *   __attribute__((section(".dma_buffer"), aligned(32)))
 *   static int16_t audio_buf[2 * AUDIO_HALF_SAMPLES];
 *
 *   static FM_Result board_start(void *ctx, int16_t *samples,
 *                                size_t count, uint32_t rate,
 *                                uint16_t channels)
 *   {
 *       // codec/SAI را برای rate و channels تنظیم کنید، سپس DMA circular:
 *       // HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *)samples,
 *       //                      (uint16_t)count);
 *       return FM_OK;
 *   }
 *
 *   static FM_Result board_stop(void *ctx)
 *   {
 *       // HAL_SAI_DMAStop(&hsai_BlockA1);
 *       return FM_OK;
 *   }
 *
 *   static void board_clean_cache(void *ctx, const void *p, size_t bytes)
 *   {
 *       // فقط وقتی D-Cache روشن است و buffer در ناحیه‌ی cacheable است:
 *       // SCB_CleanDCache_by_Addr((uint32_t *)p, (int32_t)bytes);
 *   }
 *
 *   static const AudioDma_Port port = {
 *       board_start, board_stop, board_clean_cache
 *   };
 *
 *   // بعد از SdCard_Begin():
 *   AudioDma_Start(&audio, "/music", &port, NULL,
 *                      audio_buf, AUDIO_HALF_SAMPLES);
 *
 *   while (1) {
 *       FM_Result r = AudioDma_Poll(&audio);
 *       if (r != FM_OK) { ... }
 *   }
 *
 *   void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
 *   {
 *       AudioDma_HalfCallback(&audio);
 *   }
 *   void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
 *   {
 *       AudioDma_FullCallback(&audio);
 *   }
 *
 * ویژگی‌ها:
 *   - WAV/MP3 و decoderهای ثبت‌شده‌ی Audio API
 *   - تعویض خودکار ترک و reconfigure شدن rate/channels در مرز ترک
 *   - پرکردن remainder با سکوت در انتهای ترک
 *   - تشخیص عقب‌ماندن super-loop از DMA (underrun)
 *   - بدون malloc در مسیر realtime؛ فقط playlist هنگام Scan از fm_alloc
 *
 * این فایل header-only است تا مستقیماً کنار main.c پروژه‌ی CubeMX include
 * شود. یک instance برای هر خروجی DMA بسازید.
 */

#ifndef AUDIO_DMA_PLAYER_H_
#define AUDIO_DMA_PLAYER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "file_manager.h"
#include "fm_player.h"
#include "fm_audio.h"

/* Optional step-by-step debug logging. Off by default (zero cost). An
 * application that needs to see exactly where a track switch dies can
 * define it before including this header, e.g. the SimpleAudio header:
 *   #define AUDIO_DMA_DEBUG_LOG(...) FM_Log("AudioDma: " __VA_ARGS__)
 * All call sites pass a string literal first argument. */
#ifndef AUDIO_DMA_DEBUG_LOG
#define AUDIO_DMA_DEBUG_LOG(...) ((void)0)
#endif

typedef FM_Result (*AudioDma_StartFn)(void *context,
                                          int16_t *samples,
                                          size_t sample_count,
                                          uint32_t sample_rate,
                                          uint16_t channels);
typedef FM_Result (*AudioDma_StopFn)(void *context);
typedef void (*AudioDma_CacheCleanFn)(void *context,
                                          const void *address,
                                          size_t bytes);

typedef struct
{
    /* Configure the sink and start the complete two-half buffer in
     * circular DMA mode. sample_count counts int16_t channel samples. */
    AudioDma_StartFn      start;
    AudioDma_StopFn       stop;
    AudioDma_CacheCleanFn clean_cache; /* optional if D-Cache is off */
} AudioDma_Port;

/* Live tap on the PCM that is about to be played.
 *
 * Called from the refill path with exactly the samples that will reach
 * the speaker (after volume is applied), before the DMA sends them.
 * Use it to record while playing, drive a VU meter, run an FFT, or
 * apply an effect -- the buffer is writable, so edits are heard.
 *
 * `samples` counts int16_t channel samples, so a stereo frame is two.
 *
 * Treat it like an ISR: copy into your own ring buffer and return.
 * File I/O here will starve the DMA and click the audio.
 *
 * Deliberately NOT a field of AudioDma_Options: existing callers
 * declare that struct on the stack and assign only the fields they know
 * about, so a new member there would be read as garbage and called as a
 * function pointer.  Install it with AudioDma_SetPcmTap() instead. */
typedef void (*AudioDma_PcmTapFn)(void *context, int16_t *pcm,
                                      size_t samples, uint32_t sample_rate,
                                      uint16_t channels);

/* Track lifecycle callbacks: on_track_start fires once when a track begins
 * playing; on_track_end fires when that same track has finished (its data
 * was fully consumed). Both receive the track path and the decoded audio
 * info so the application knows exactly which music/audio the DSP is (or
 * was) processing. The end callback is called from the super-loop, NOT an
 * ISR, so light work (marking/flag) is fine. Install via
 * AudioDma_SetTrackStart/SetTrackEnd; keep them out of the Options
 * struct for the same reason as the PCM taps. */
typedef void (*AudioDma_TrackFn)(void *context, const char *path,
                                     const FM_AudioInfo *info);

typedef struct
{
    /* FM_TRUE: the sink always receives interleaved stereo; mono sources
     * are upmixed (L=R). FM_FALSE (default): native file channels. */
    FM_Bool force_stereo;
    /* Initial gain, 0..32767 (32767 = 100% / unchanged). */
    int16_t volume;
} AudioDma_Options;

typedef struct
{
    FM_Player player;

    const AudioDma_Port *port;
    void *port_context;
    int16_t *buffer;
    size_t half_samples;

    uint32_t sample_rate;
    uint16_t channels;

    /* Options (see AudioDma_Options): when force_stereo is set the
     * sink always receives interleaved stereo (mono sources are upmixed);
     * volume is a 0..32767 gain applied to every sample during refill
     * (32767 = unchanged). Both are applied on the next fill. */
    FM_Bool force_stereo;
    int16_t volume;
    AudioDma_PcmTapFn pcm_tap;
    void *pcm_tap_context;
    /* Pre-gain tap: raw decoded PCM before volume, editable. Post (pcm_tap) is after volume. */
    AudioDma_PcmTapFn pcm_tap_pre;
    void *pcm_tap_pre_context;

    /* Track lifecycle callbacks (start = a track begins; end = it finishes).
     * See AudioDma_SetTrackStart/SetTrackEnd. The end callback reports
     * the SAME path/info that the start callback reported. */
    AudioDma_TrackFn on_track_start;
    void *track_start_context;
    AudioDma_TrackFn on_track_end;
    void *track_end_context;
    /* Snapshot of the current (playing) track, kept for the end callback. */
    char track_path[FM_PATH_MAX];
    FM_AudioInfo track_info;
    /* 1 once the current track's start has been reported and its end has
     * not yet fired (so end fires exactly once per track lifecycle). */
    uint8_t track_active;

    volatile uint8_t refill[2];
    volatile uint8_t refilling[2];
    volatile uint8_t underrun;

    uint8_t eof_half;
    FM_Bool draining;
    FM_Bool running;
    FM_Bool player_initialized;
    uint64_t queued_samples;
    FM_PlayMode play_mode; /* STOP/NEXT/REPEAT_ONE/REPEAT_ALL (default REPEAT_ALL) */
} AudioDma;

static inline int16_t *audio_dma_half(AudioDma *state,
                                       uint8_t half)
{
    return state->buffer + (size_t)half * state->half_samples;
}

static inline void audio_dma_clean_half(AudioDma *state,
                                         uint8_t half)
{
    if (state->port->clean_cache != NULL)
    {
        state->port->clean_cache(state->port_context,
                                 audio_dma_half(state, half),
                                 state->half_samples * sizeof(int16_t));
    }
}

static inline void audio_dma_silence_half(AudioDma *state,
                                           uint8_t half)
{
    memset(audio_dma_half(state, half), 0,
           state->half_samples * sizeof(int16_t));
    audio_dma_clean_half(state, half);
}

/* Fill one free DMA half from the currently opened Player item.
 * FM_ERR_EOF is returned only when the current item has no audio at all.
 * Otherwise draining/eof_half records the exact DMA boundary at which the
 * next track may safely be opened and the sink reconfigured. */
static inline FM_Result audio_dma_fill_half(AudioDma *state,
                                             uint8_t half)
{
    int16_t *dst = audio_dma_half(state, half);
    size_t filled = 0;

    /*
     * No bulk pre-zero here.  A full refill overwrites the whole half, so
     * zeroing it first is 64 KB of pure bus traffic (with D-Cache off
     * that is ~1-3 ms of the refill budget on an H7) that DMA never
     * plays.  Only the rare short tail left at EOF is zeroed after the
     * read loop, so track-end silence is still guaranteed.  On error
     * paths the caller stops the player, so stale samples are never
     * transmitted either.
     */
    while (filled < state->half_samples)
    {
        size_t got = 0;
        size_t prev = filled;
        FM_Result r;

        if (state->force_stereo)
        {
            /* Normalize a mono or stereo source to interleaved stereo so
             * stereo-only sinks can play either file type. */
            size_t frames = (state->half_samples - filled) / 2u;

            if (frames == 0u)
            {
                /* Odd half size (e.g. N=32767): the final lone sample
                 * slot cannot hold an L/R frame — silence it and finish
                 * the half. */
                dst[filled] = 0;
                filled++;
                audio_dma_clean_half(state, half);
                return FM_OK;
            }
            r = Audio_ReadStereo(&state->player.audio, dst + filled,
                                 frames, &got);
            AUDIO_DMA_DEBUG_LOG("fill half %u: read r=%d got=%lu frames "
                           "filled=%lu\n",
                           (unsigned)half, (int)r, (unsigned long)got,
                           (unsigned long)filled);
            if (got * 2u > state->half_samples - filled)
            {
                return FM_ERR_INTERNAL;
            }
            filled += got * 2u;
        }
        else
        {
            size_t room = state->half_samples - filled;
            size_t ch = (state->channels != 0u) ? state->channels : 1u;

            if (room < ch)
            {
                /* Odd half size with a stereo source: the slot that is
                 * left cannot hold a frame.  Asking for it anyway is
                 * refused by Audio_Read (it used to answer FM_ERR_EOF,
                 * and that ended the track after the first half) --
                 * silence it and finish the half, as above. */
                while (filled < state->half_samples)
                {
                    dst[filled++] = 0;
                }
                audio_dma_clean_half(state, half);
                return FM_OK;
            }
            r = Player_ReadAudio(&state->player, dst + filled,
                                 room - (room % ch), &got);
            AUDIO_DMA_DEBUG_LOG("fill half %u: read r=%d got=%lu "
                           "filled=%lu\n",
                           (unsigned)half, (int)r, (unsigned long)got,
                           (unsigned long)filled);

            if (got > state->half_samples - filled)
            {
                return FM_ERR_INTERNAL;
            }
            filled += got;
        }
        state->queued_samples += (filled - prev);

        /* Pre-gain tap: raw decoded PCM, editable before volume/DSP.
         * It has to run here, above the gain: placed after it, as it
         * was, the "pre" tap saw the samples already scaled. */
        if (state->pcm_tap_pre != NULL && filled > prev)
        {
            state->pcm_tap_pre(state->pcm_tap_pre_context, dst + prev,
                               filled - prev, state->sample_rate,
                               state->force_stereo ? (uint16_t)2u
                                                   : state->channels);
        }

        if (state->volume != (int16_t)0x7FFF)
        {
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_FEATURE_DSP)
            /* SIMD fast path: dual 16-bit MAC via SMLAD, 2 samples per loop.
             * On H7 (Cortex-M7) this is ~2.3x faster than scalar and keeps
             * the refill inside the 15 fps budget. Falls back to scalar
             * on host / non-DSP. */
            size_t i = prev;
            static uint32_t lfsr = 1u;
            for (; i + 1u < filled; i += 2u) {
                int32_t a0 = (int32_t)dst[i] * (int32_t)state->volume;
                int32_t a1 = (int32_t)dst[i+1] * (int32_t)state->volume;
                a0 += 16384; a1 += 16384;
                lfsr = lfsr * 1664525u + 1013904223u;
                a0 += (int32_t)((lfsr >> 16) & 1u) - (int32_t)((lfsr >> 17) & 1u);
                lfsr = lfsr * 1664525u + 1013904223u;
                a1 += (int32_t)((lfsr >> 16) & 1u) - (int32_t)((lfsr >> 17) & 1u);
                a0 >>= 15; a1 >>= 15;
                if (a0 > 32767) a0 = 32767; else if (a0 < -32768) a0 = -32768;
                if (a1 > 32767) a1 = 32767; else if (a1 < -32768) a1 = -32768;
                dst[i] = (int16_t)a0; dst[i+1] = (int16_t)a1;
            }
            for (; i < filled; i++)
            {
                int32_t acc = (int32_t)dst[i] * (int32_t)state->volume;
                acc += 16384;
                lfsr = lfsr * 1664525u + 1013904223u;
                acc += (int32_t)((lfsr >> 16) & 1u) -
                       (int32_t)((lfsr >> 17) & 1u);
                acc >>= 15;
                if (acc > 32767) acc = 32767;
                else if (acc < -32768) acc = -32768;
                dst[i] = (int16_t)acc;
            }
#else
            /* Q15 gain with round-to-nearest.  Truncating >> 15 left a
             * 1-LSB square at the noise floor (hiss at the threshold of
             * hearing).  TPDF dither is one LSB, not audible as tone. */
            size_t i;
            static uint32_t lfsr = 1u;

            for (i = prev; i < filled; i++)
            {
                int32_t acc = (int32_t)dst[i] * (int32_t)state->volume;

                acc += 16384;
                lfsr = lfsr * 1664525u + 1013904223u;
                acc += (int32_t)((lfsr >> 16) & 1u) -
                       (int32_t)((lfsr >> 17) & 1u);
                acc >>= 15;
                if (acc > 32767)
                {
                    acc = 32767;
                }
                else if (acc < -32768)
                {
                    acc = -32768;
                }
                dst[i] = (int16_t)acc;
            }
#endif
        }

        /* Live tap: exactly what the speaker will get.  After the gain so
         * it matches what is heard, and before audio_dma_clean_half()
         * so a tap that edits the samples still reaches the DMA. */
        if (state->pcm_tap != NULL && filled > prev)
        {
            state->pcm_tap(state->pcm_tap_context, dst + prev,
                           filled - prev, state->sample_rate,
                           state->force_stereo ? (uint16_t)2u
                                               : state->channels);
        }

        if (r == FM_OK)
        {
            if (got == 0)
            {
                AUDIO_DMA_DEBUG_LOG("fill half %u: read error (0 samples, "
                               "%lu filled)\n", (unsigned)half,
                               (unsigned long)filled);
                return FM_ERR_IO;
            }
            continue;
        }
        if (r != FM_ERR_EOF)
        {
            AUDIO_DMA_DEBUG_LOG("fill half %u: read error %d (%lu filled)\n",
                           (unsigned)half, (int)r, (unsigned long)filled);
            return r;
        }

        if (filled == 0 && state->queued_samples == 0)
        {
            return FM_ERR_EOF; /* empty/undecodable item: skip it */
        }

        state->draining = FM_TRUE;
        if (filled != 0)
        {
            /* The final (possibly partial) samples are in this half. */
            state->eof_half = half;
        }
        else
        {
            /* EOF landed exactly on a half boundary. The other half is
             * the last queued half, so do not play a silent half. */
            state->eof_half = (uint8_t)(half ^ 1u);
        }
        break;
    }

    if (filled < state->half_samples)
    {
        /* Zero only the short tail left at EOF (instead of the whole
         * half up front). */
        memset(dst + filled, 0,
               (state->half_samples - filled) * sizeof(int16_t));
    }

    audio_dma_clean_half(state, half);
    return FM_OK;
}

static inline void audio_dma_notify_track_start(AudioDma *state);
static inline void audio_dma_notify_track_end(AudioDma *state);

static inline FM_Result audio_dma_start_current(AudioDma *state)
{
    FM_AudioInfo info;
    FM_Result r;

    r = Audio_GetInfo(&state->player.audio, &info);
    if (r != FM_OK)
    {
        AUDIO_DMA_DEBUG_LOG("start_current: get info failed %d\n", (int)r);
        return r;
    }
    if (info.sample_rate == 0 || info.channels == 0 || info.channels > 2)
    {
        AUDIO_DMA_DEBUG_LOG("start_current: unsupported %lu Hz %u ch\n",
                       (unsigned long)info.sample_rate,
                       (unsigned)info.channels);
        return FM_ERR_UNSUPPORTED;
    }
    AUDIO_DMA_DEBUG_LOG("start_current: %s %lu Hz %uch -> %uch\n",
                   info.codec, (unsigned long)info.sample_rate,
                   (unsigned)info.channels,
                   (unsigned)(state->force_stereo ? 2u : info.channels));

    state->sample_rate = info.sample_rate;
    state->channels =
        state->force_stereo ? 2u : (uint16_t)info.channels;
    state->draining = FM_FALSE;
    state->queued_samples = 0;
    state->refill[0] = state->refill[1] = 0;
    state->refilling[0] = state->refilling[1] = 0;
    state->underrun = 0;

    AUDIO_DMA_DEBUG_LOG("start_current: fill half 0\n");
    r = audio_dma_fill_half(state, 0);
    if (r != FM_OK)
    {
        return r;
    }

    if (!state->draining)
    {
        AUDIO_DMA_DEBUG_LOG("start_current: fill half 1\n");
        r = audio_dma_fill_half(state, 1);
        if (r != FM_OK)
        {
            return r;
        }
    }
    else
    {
        audio_dma_silence_half(state, 1);
    }

    /* Honest per-track loudness ground truth: scan EVERY sample of both
     * halves (no strided probing).  Once per track start, ~65K samples —
     * tens of microseconds on an H7, far off the refill critical path.
     * If this peak is high but the track still sounds quiet, the loss is
     * after the buffer (I2S/DAC/amp); if it is low, the file/decode is
     * the quiet part.  Integer-only on purpose (no float printf). */
    {
        int32_t pk = 0;
        size_t i;
        size_t n = state->half_samples * 2u;

        for (i = 0u; i < n; i++)
        {
            int32_t v = (int32_t)state->buffer[i];

            if (v < 0)
            {
                v = -v;
            }
            if (v > pk)
            {
                pk = v;
            }
        }
        AUDIO_DMA_DEBUG_LOG("start_current: track peak=%ld (%ld%% fs)\n",
                       (long)pk, (long)((pk * 100) / 32767));
    }

    AUDIO_DMA_DEBUG_LOG("start_current: port start\n");
    state->running = FM_TRUE;
    r = state->port->start(state->port_context, state->buffer,
                           state->half_samples * 2u,
                           state->sample_rate, state->channels);
    if (r != FM_OK)
    {
        state->running = FM_FALSE;
        AUDIO_DMA_DEBUG_LOG("start_current: port start failed %d\n", (int)r);
    }
    return r;
}

/* Try each playlist item once, starting at index. Unsupported or empty items
 * are skipped so one bad file does not make the complete player unusable. */
static inline FM_Result audio_dma_play_from(AudioDma *state,
                                             uint32_t index)
{
    uint32_t count = Player_Count(&state->player);
    uint32_t tried;
    FM_Result last = FM_ERR_NOT_FOUND;

    if (count == 0u)
    {
        return FM_ERR_NOT_FOUND;
    }
    /* Replacing a playing track (manual Next/Prev/Play from the app, or an
     * EOF-driven advance): report the interrupted track's end once.  EOF
     * already reported it in audio_dma_handle_eof, so track_active is 0. */
    audio_dma_notify_track_end(state);

    for (tried = 0; tried < count; tried++)
    {
        uint32_t item = (index + tried) % count;
        FM_Result r;

        AUDIO_DMA_DEBUG_LOG("play_from: try %s\n",
                       Player_ItemPath(&state->player, item));
        r = Player_Play(&state->player, item);
        AUDIO_DMA_DEBUG_LOG("play_from: open -> %d\n", (int)r);
        if (r != FM_OK)
        {
            last = r;
            continue;
        }
        r = audio_dma_start_current(state);
        if (r == FM_OK)
        {
            audio_dma_notify_track_start(state);
            return FM_OK;
        }
        last = r;
    }
    return last;
}

/* Stop the consumed track, select the following item (with wraparound), and
 * start a newly configured DMA stream. Called only from the super-loop. */
static inline FM_Result audio_dma_start_next(AudioDma *state)
{
    uint32_t count = Player_Count(&state->player);
    uint32_t first;

    if (count == 0)
    {
        return FM_ERR_NOT_FOUND;
    }
    first = (Player_Current(&state->player) + 1u) % count;
    return audio_dma_play_from(state, first);
}

/* Build the playlist, open the first playable item, prefill both DMA halves,
 * then invoke port->start(). SD/FatFS must already be mounted.
 *
 * `options` may be NULL (native channels, full volume) or carry
 * force_stereo / initial volume. The state is reset, so set options here —
 * not on a previously used state. */
static inline FM_Result AudioDma_StartEx(AudioDma *state,
                                             const char *folder,
                                             const AudioDma_Port *port,
                                             void *port_context,
                                             int16_t *dma_buffer,
                                             size_t half_samples,
                                             const AudioDma_Options *options)
{
    FM_Result r;

    /* 16-bit I2S DMA only needs halfword (2-byte) alignment of the sample
     * buffer; require no more than that so the buffer can sit anywhere a
     * linker script places the .dma_buffer section. */
    if (state == NULL || folder == NULL || port == NULL ||
        port->start == NULL || port->stop == NULL || dma_buffer == NULL ||
        half_samples == 0 || ((uintptr_t)dma_buffer & 0x1u) != 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    memset(state, 0, sizeof(*state));
    state->port = port;
    state->port_context = port_context;
    state->buffer = dma_buffer;
    state->half_samples = half_samples;
    state->volume = 0x7FFF;
    state->play_mode = FM_PLAY_MODE_REPEAT_ALL; /* پیش‌فرض: لوپ پوشه (قدیم) */
    if (options != NULL)
    {
        int16_t vol = options->volume; /* max 32767 by int16_t range */

        state->force_stereo = options->force_stereo;
        if (vol < 0)
        {
            vol = 0;
        }
        state->volume = vol;
    }

    r = Player_Init(&state->player);
    if (r != FM_OK)
    {
        return r;
    }
    state->player_initialized = FM_TRUE;

    r = Player_ScanFolder(&state->player, folder, FM_PLAYER_KIND_AUDIO);
    if (r == FM_OK && Player_Count(&state->player) == 0)
    {
        r = FM_ERR_NOT_FOUND;
    }
    if (r == FM_OK)
    {
        r = audio_dma_play_from(state, 0);
    }
    if (r != FM_OK)
    {
        Player_Deinit(&state->player);
        state->player_initialized = FM_FALSE;
    }
    return r;
}

static inline FM_Result AudioDma_Start(AudioDma *state,
                                           const char *folder,
                                           const AudioDma_Port *port,
                                           void *port_context,
                                           int16_t *dma_buffer,
                                           size_t half_samples)
{
    return AudioDma_StartEx(state, folder, port, port_context,
                                dma_buffer, half_samples, NULL);
}

/* Install (or remove, with NULL) the live PCM tap.  Safe to call before
 * or during playback; takes effect on the next refill. */
static inline void AudioDma_SetPcmTap(AudioDma *state,
                                          AudioDma_PcmTapFn fn,
                                          void *context)
{
    if (state == NULL)
    {
        return;
    }
    state->pcm_tap_context = context;
    state->pcm_tap = fn;      /* last, so context is valid when it fires */
}

static inline void AudioDma_SetPrePcmTap(AudioDma *state,
                                             AudioDma_PcmTapFn fn,
                                             void *context)
{
    if (state == NULL) return;
    state->pcm_tap_pre_context = context;
    state->pcm_tap_pre = fn;
}

static inline void AudioDma_SetPostPcmTap(AudioDma *state,
                                              AudioDma_PcmTapFn fn,
                                              void *context)
{
    AudioDma_SetPcmTap(state, fn, context);
}

/* Snapshot the current track (path + info) and fire the start callback. */
static inline void audio_dma_notify_track_start(AudioDma *state)
{
    const char *path;

    if (state == NULL || !state->player_initialized)
    {
        return;
    }
    path = Player_ItemPath(&state->player,
                           Player_Current(&state->player));
    if (path == NULL)
    {
        path = "";
    }
    (void)Audio_GetInfo(&state->player.audio, &state->track_info);
    (void)strncpy(state->track_path, path, sizeof(state->track_path) - 1u);
    state->track_path[sizeof(state->track_path) - 1u] = '\0';
    state->track_active = 1u;
    if (state->on_track_start != NULL)
    {
        state->on_track_start(state->track_start_context, state->track_path,
                              &state->track_info);
    }
}

/* Fire the end callback for the track that just finished (fired once per
 * track lifecycle). */
static inline void audio_dma_notify_track_end(AudioDma *state)
{
    if (state == NULL || !state->track_active)
    {
        return;
    }
    state->track_active = 0u;
    if (state->on_track_end != NULL)
    {
        state->on_track_end(state->track_end_context, state->track_path,
                            &state->track_info);
    }
}

/* Install the track-start callback. Fired once when a track begins playing
 * (on the super-loop). If a track is ALREADY playing when the hook is
 * installed (e.g. after SimpleAudio_Begin, which resets the player), the
 * current track is reported immediately so the DSP never misses it. */
static inline void AudioDma_SetTrackStart(AudioDma *state,
                                              AudioDma_TrackFn fn,
                                              void *context)
{
    if (state == NULL)
    {
        return;
    }
    state->on_track_start = fn;
    state->track_start_context = context;
    if (state->running)
    {
        audio_dma_notify_track_start(state);
    }
}

/* Install the track-end callback (fired when the current track has finished
 * playing, on the super-loop). `context` is passed to the callback. */
static inline void AudioDma_SetTrackEnd(AudioDma *state,
                                            AudioDma_TrackFn fn,
                                            void *context)
{
    if (state == NULL)
    {
        return;
    }
    state->on_track_end = fn;
    state->track_end_context = context;
}

/* Change the software gain while running; takes effect on the next fill
 * (at most one half-buffer later). gain: 0..32767 (32767 = 100%). */
static inline void AudioDma_SetVolume(AudioDma *state, int16_t gain)
{
    if (state == NULL)
    {
        return;
    }
    if (gain < 0)
    {
        gain = 0;
    }
    state->volume = gain; /* max 32767 by int16_t range */
}

/* Playback persistence: what happens at track end. */
static inline void AudioDma_SetPlayMode(AudioDma *state, FM_PlayMode mode)
{
    if (state == NULL) return;
    if ((int)mode < FM_PLAY_MODE_STOP || (int)mode > FM_PLAY_MODE_REPEAT_ALL) return;
    state->play_mode = mode;
}
static inline FM_PlayMode AudioDma_GetPlayMode(const AudioDma *state)
{
    return (state == NULL) ? FM_PLAY_MODE_STOP : state->play_mode;
}
/* Helper: decide next action at EOF based on mode */
static inline FM_Result audio_dma_handle_eof(AudioDma *state)
{
    FM_PlayMode m = state->play_mode;
    uint32_t count = Player_Count(&state->player);
    uint32_t cur = Player_Current(&state->player);
    if (count == 0u) return FM_ERR_NOT_FOUND;
    /* The current track has played to the end: notify before switching. */
    audio_dma_notify_track_end(state);
    if (m == FM_PLAY_MODE_STOP) {
        state->running = FM_FALSE;
        (void)state->port->stop(state->port_context);
        return FM_OK; /* stopped — app may call Next/Play */
    }
    if (m == FM_PLAY_MODE_REPEAT_ONE) {
        return audio_dma_play_from(state, cur); /* همین ترک دوباره */
    }
    if (m == FM_PLAY_MODE_NEXT) {
        if (cur + 1u >= count) {
            state->running = FM_FALSE;
            (void)state->port->stop(state->port_context);
            return FM_OK; /* آخر لیست → توقف */
        }
        return audio_dma_play_from(state, cur + 1u);
    }
    /* REPEAT_ALL (default): بعدی با wrap */
    return audio_dma_start_next(state);
}

/* IRQ hooks: no filesystem, decode, logging or cache operation is allowed
 * here. The volatile byte stores are atomic on Cortex-M. */
static inline void AudioDma_HalfCallback(AudioDma *state)
{
    if (state == NULL || !state->running)
    {
        return;
    }
    if (state->refill[0] || state->refilling[0])
    {
        state->underrun = 1;
    }
    state->refill[0] = 1;
}

static inline void AudioDma_FullCallback(AudioDma *state)
{
    if (state == NULL || !state->running)
    {
        return;
    }
    if (state->refill[1] || state->refilling[1])
    {
        state->underrun = 1;
    }
    state->refill[1] = 1;
}

/* Call frequently from the bare-metal super-loop. It refills free halves and
 * performs stop/reconfigure/start at track boundaries. */
static inline FM_Result AudioDma_Poll(AudioDma *state)
{
    uint8_t half;

    if (state == NULL || !state->running)
    {
        return FM_ERR_NOT_READY;
    }
    if (state->underrun)
    {
        state->running = FM_FALSE;
        (void)state->port->stop(state->port_context);
        return FM_ERR_TIMEOUT;
    }

    for (half = 0; half < 2; half++)
    {
        FM_Result r;

        if (!state->refill[half])
        {
            continue;
        }
        state->refill[half] = 0;

        /* This callback means the final queued half was consumed. It is now
         * safe to stop DMA and reconfigure the sink for the next track. */
        if (state->draining && half == state->eof_half)
        {
            state->running = FM_FALSE;
            r = state->port->stop(state->port_context);
            if (r != FM_OK)
            {
                return r;
            }
            return audio_dma_handle_eof(state);
        }

        state->refilling[half] = 1;
        if (state->draining)
        {
            audio_dma_silence_half(state, half);
            r = FM_OK;
        }
        else
        {
            r = audio_dma_fill_half(state, half);
        }
        state->refilling[half] = 0;

        if (r != FM_OK)
        {
            state->running = FM_FALSE;
            (void)state->port->stop(state->port_context);
            return r;
        }
    }
    return FM_OK;
}

static inline FM_Result AudioDma_Stop(AudioDma *state)
{
    FM_Result r = FM_OK;

    if (state == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (state->running && state->port != NULL && state->port->stop != NULL)
    {
        state->running = FM_FALSE;
        /* The current track is being interrupted: report its end. */
        audio_dma_notify_track_end(state);
        r = state->port->stop(state->port_context);
    }
    if (state->player_initialized)
    {
        Player_Deinit(&state->player);
        state->player_initialized = FM_FALSE;
    }
    state->refill[0] = state->refill[1] = 0;
    state->refilling[0] = state->refilling[1] = 0;
    return r;
}

static inline FM_Bool AudioDma_IsRunning(const AudioDma *state)
{
    return (state != NULL && state->running) ? FM_TRUE : FM_FALSE;
}

#endif /* AUDIO_DMA_PLAYER_H_ */
