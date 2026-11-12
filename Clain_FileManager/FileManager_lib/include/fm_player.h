/*
 * FileManager - Media player (playlist + playback controls)
 *
 * A thin convenience layer over the audio/video subsystems: scan a folder
 * for media files, then play, step next/previous, and seek within a track.
 * The playlist is stored in memory allocated through the fm_alloc.h hooks.
 *
 * Usage:
 *     FM_Player p;
 *     Player_Init(&p);
 *     Player_ScanFolder(&p, "/music", FM_PLAYER_KIND_AUDIO);
 *     Player_Play(&p, 0);
 *     Player_Next(&p);
 *     Player_SeekFwd(&p, 10.0);
 *     Player_Deinit(&p);
 */

#ifndef FM_PLAYER_H
#define FM_PLAYER_H

#include "fm_types.h"
#include "fm_media.h"
#include "fm_audio.h"
#include "fm_video.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_PLAYER_KIND_AUDIO = 1,
    FM_PLAYER_KIND_VIDEO = 2,
    FM_PLAYER_KIND_IMAGE = 4,
    FM_PLAYER_KIND_ALL   = 3
} FM_PlayerKind;

/* Playback persistence mode — what happens when a track / video item ends.
 * موسیقی/ویدیو وقتی تمام شد چه شود: بایستد، برود بعدی، یا همین را تکرار کند.
 * Used by SimpleAudio / SimpleVideo (presentation mode). */
typedef enum
{
    FM_PLAY_MODE_STOP        = 0, /* تک آهنگ/ویدیو تمام شد → توقف (pause/stop) */
    FM_PLAY_MODE_NEXT        = 1, /* تمام شد → بعدی، آخر لیست → توقف (بدون لوپ) */
    FM_PLAY_MODE_REPEAT_ONE  = 2, /* تمام شد → همین ترک/ویدیو دوباره (تکرار یک) */
    FM_PLAY_MODE_REPEAT_ALL  = 3  /* تمام شد → بعدی و آخر → اول (لوپ پوشه) */
} FM_PlayMode;

/* Where the decoded sound goes — one switch, the same for music
 * (SimpleAudio_SetOutput) and for the sound of a video
 * (SimpleVideo_SetAudioOutput).
 *
 *   NONE      nowhere: not decoded for a video; silence for music
 *   SPEAKER   I2S only; your callbacks are not called
 *   CALLBACK  your callbacks only; I2S plays silence (still in real time)
 *   BOTH      your callbacks first, then I2S — what you change is heard
 *             (default; with no callback installed this is just SPEAKER) */
typedef enum
{
    FM_AUDIO_OUT_NONE     = 0,
    FM_AUDIO_OUT_SPEAKER  = 1,
    FM_AUDIO_OUT_CALLBACK = 2,
    FM_AUDIO_OUT_BOTH     = 3
} FM_AudioOutput;

/* The one PCM callback type of both players.
 *   pcm       signed 16-bit samples, interleaved (L,R,L,R...); writable
 *   samples   number of int16_t values (a stereo frame is two)
 * It runs on the buffer-refill path: copy or process and return. No file
 * I/O and no printf in here. */
typedef void (*FM_AudioDataFn)(void *ctx, int16_t *pcm, size_t samples,
                               uint32_t sample_rate, uint16_t channels);

typedef struct
{
    char         path[FM_PATH_MAX];
    FM_MediaType type;
} FM_PlayerItem;

typedef struct
{
    FM_PlayerItem *items;
    uint32_t       count;
    uint32_t       capacity;
    uint32_t       current;
    FM_Bool        is_video;
    FM_Audio       audio;
    FM_Video       video;
} FM_Player;

/* ------------------------------------------------------------------ */
/* Lifecycle + playlist                                                */
/* ------------------------------------------------------------------ */
FM_Result Player_Init(FM_Player *p);
void      Player_Deinit(FM_Player *p);

/* Recursively scan `folder` and build the playlist (sorted by name). */
FM_Result Player_ScanFolder(FM_Player *p, const char *folder,
                            FM_PlayerKind kinds);
void      Player_Clear(FM_Player *p);

uint32_t      Player_Count(const FM_Player *p);
FM_MediaType  Player_ItemType(const FM_Player *p, uint32_t index);
const char   *Player_ItemPath(const FM_Player *p, uint32_t index);

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */
FM_Result Player_Play(FM_Player *p, uint32_t index);
FM_Result Player_Next(FM_Player *p);      /* wraps around               */
FM_Result Player_Prev(FM_Player *p);      /* wraps around               */
FM_Result Player_Stop(FM_Player *p);

FM_Bool Player_IsVideo(const FM_Player *p);
uint32_t Player_Current(const FM_Player *p);

/* Audio: read decoded interleaved 16-bit samples. */
FM_Result Player_ReadAudio(FM_Player *p, int16_t *samples, size_t count,
                           size_t *read);
/* Video: decode the next frame to RGB888; width/height returned via w,h. */
FM_Result Player_DecodeVideo(FM_Player *p, void *pixels, size_t cap,
                             uint16_t *w, uint16_t *h);

/* ------------------------------------------------------------------ */
/* Seek                                                                */
/* ------------------------------------------------------------------ */
/* A negative time or NaN is FM_ERR_INVALID_ARGUMENT. */
FM_Result Player_Seek(FM_Player *p, double seconds);       /* absolute */
FM_Result Player_SeekFwd(FM_Player *p, double seconds);
FM_Result Player_SeekBack(FM_Player *p, double seconds);

/* Position / duration in seconds.  The duration is FM_AudioInfo.total_frames
 * over the sample rate: 0.0 = not known (AAC); an estimate for MP3 without
 * a Xing/Info header (wrong when such a file is variable bit rate). */
double Player_GetPosition(const FM_Player *p);
double Player_GetDuration(const FM_Player *p);

FM_END_DECLS

#endif /* FM_PLAYER_H */
