/*
 * FileManager - Media player (playlist + playback controls)
 */

#define FM_DEBUG_TAG "player"
#include "fm_debug.h"
#include "fm_player.h"
#include "fm_internal.h"
#include "fm_media.h"
#include "fm_path.h"
#include "fm_alloc.h"

#include <stdlib.h>
#include <string.h>

#if FM_USE_AUDIO || FM_USE_VIDEO

static FM_Bool ext_is(const char *path, const char *e)
{
    char ext[16];

    if (Path_GetExtension(path, ext, sizeof(ext)) != FM_OK)
    {
        return FM_FALSE;
    }
    return strcmp(ext, e) == 0;
}

static FM_Bool is_audio_path(const char *path)
{
    /* What Audio_Open can play.  ".opus" (Ogg Opus) was missing; ".m4a"
     * was listed, but that is AAC in an MP4 container, which Audio_Open
     * refuses -- a scanned folder got entries that could not be played. */
    return ext_is(path, "wav") || ext_is(path, "mp3") ||
           ext_is(path, "aac") || ext_is(path, "opus") ||
           ext_is(path, "flac") || ext_is(path, "ogg");
}

static FM_Bool is_image_path(const char *path)
{
    return ext_is(path, "bmp") || ext_is(path, "jpg") ||
           ext_is(path, "jpeg") || ext_is(path, "png") ||
           ext_is(path, "gif") || ext_is(path, "tif") ||
           ext_is(path, "tiff") || ext_is(path, "webp");
}

static FM_Bool is_video_path(const char *path)
{
    return ext_is(path, "avi") || ext_is(path, "mp4") ||
           ext_is(path, "m4v") || ext_is(path, "mov") ||
           ext_is(path, "fmrv");
}

static FM_Bool kind_wants(FM_PlayerKind kinds, const char *path)
{
    if (kinds & FM_PLAYER_KIND_AUDIO)
    {
        if (is_audio_path(path))
        {
            return FM_TRUE;
        }
    }
    if (kinds & FM_PLAYER_KIND_VIDEO)
    {
        if (is_video_path(path))
        {
            return FM_TRUE;
        }
    }
    if (kinds & FM_PLAYER_KIND_IMAGE)
    {
        if (is_image_path(path))
        {
            return FM_TRUE;
        }
    }
    return FM_FALSE;
}

static int compare_item(const void *a, const void *b)
{
    const FM_PlayerItem *ia = (const FM_PlayerItem *)a;
    const FM_PlayerItem *ib = (const FM_PlayerItem *)b;

    return strcmp(ia->path, ib->path);
}

typedef struct
{
    FM_Player *p;
    FM_PlayerKind kinds;
    FM_Result result;
} ScanCtx;

static FM_Result scan_cb(const FM_FileInfo *info, void *user_data)
{
    ScanCtx *ctx = (ScanCtx *)user_data;
    FM_Player *p = ctx->p;

    if (info->type != FM_ITEM_FILE)
    {
        return FM_OK;
    }
    if (!kind_wants(ctx->kinds, info->path))
    {
        return FM_OK;
    }
    if (p->count == p->capacity)
    {
        uint32_t new_cap = (p->capacity == 0) ? 16 : p->capacity * 2;
        FM_PlayerItem *items = (FM_PlayerItem *)FM_ReallocSized(p->items,
                            (size_t)p->capacity * sizeof(FM_PlayerItem),
                            (size_t)new_cap * sizeof(FM_PlayerItem));

        if (items == NULL)
        {
            ctx->result = FM_ERR_NO_MEMORY;
            return FM_ERR_CANCELLED;
        }
        p->items = items;
        p->capacity = new_cap;
    }
    (void)strcpy(p->items[p->count].path, info->path);
#if FM_USE_MEDIA_DETECT
    p->items[p->count].type = Media_DetectByExtension(info->path);
#else
    p->items[p->count].type = FM_MEDIA_UNKNOWN;
#endif
    p->count++;
    return FM_OK;
}

FM_Result Player_Init(FM_Player *p)
{
    if (p == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    memset(p, 0, sizeof(*p));
    return FM_OK;
}

void Player_Deinit(FM_Player *p)
{
    if (p == NULL)
    {
        return;
    }
    (void)Player_Stop(p);
    if (p->items != NULL)
    {
        FM_Free(p->items);
    }
    memset(p, 0, sizeof(*p));
}

void Player_Clear(FM_Player *p)
{
    if (p == NULL)
    {
        return;
    }
    (void)Player_Stop(p);
    if (p->items != NULL)
    {
        FM_Free(p->items);
    }
    p->items = NULL;
    p->count = 0;
    p->capacity = 0;
    p->current = 0;
}

FM_Result Player_ScanFolder(FM_Player *p, const char *folder,
                            FM_PlayerKind kinds)
{
    ScanCtx ctx;

    if (p == NULL || folder == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    ctx.p = p;
    ctx.kinds = kinds;
    ctx.result = FM_OK;

    /* rebuild the playlist from scratch */
    Player_Clear(p);

    {
        FM_Result r = Directory_ScanRecursive(folder, scan_cb, NULL, &ctx,
                                               NULL);

        /* A missing/inaccessible folder must not silently produce an
         * empty playlist. */
        if (ctx.result != FM_OK)
        {
            Player_Clear(p);
            return ctx.result;
        }
        if (r != FM_OK)
        {
            Player_Clear(p);
            return r;
        }
    }
    if (p->count > 1)
    {
        qsort(p->items, p->count, sizeof(FM_PlayerItem), compare_item);
    }
    return FM_OK;
}

uint32_t Player_Count(const FM_Player *p)
{
    return (p != NULL) ? p->count : 0;
}

FM_MediaType Player_ItemType(const FM_Player *p, uint32_t index)
{
    if (p == NULL || index >= p->count)
    {
        return FM_MEDIA_UNKNOWN;
    }
    return p->items[index].type;
}

const char *Player_ItemPath(const FM_Player *p, uint32_t index)
{
    if (p == NULL || index >= p->count)
    {
        return NULL;
    }
    return p->items[index].path;
}

FM_Result Player_Play(FM_Player *p, uint32_t index)
{
    FM_Result r;

    FM_CHECK_PTR(p, FM_ERR_INVALID_ARGUMENT);
    if (index >= p->count)
    {
        FM_ERR_LOG("player: index %lu out of range (count=%lu)",
                   (unsigned long)index, (unsigned long)p->count);
        return FM_ERR_INVALID_ARGUMENT;
    }
    r = Player_Stop(p);
    if (r != FM_OK && r != FM_ERR_NOT_OPEN)
    {
        return r;
    }
    p->current = index;
    FM_EVENT_LOG("playing [%lu] %s", (unsigned long)index,
                 p->items[index].path);
    if (is_video_path(p->items[index].path))
    {
#if FM_USE_VIDEO
        r = Video_Open(p->items[index].path, &p->video);
        if (r != FM_OK)
        {
            return r;
        }
        p->is_video = FM_TRUE;
#else
        return FM_ERR_UNSUPPORTED;
#endif
    }
    else if (is_audio_path(p->items[index].path))
    {
#if FM_USE_AUDIO
        r = Audio_Open(p->items[index].path, &p->audio);
        if (r != FM_OK)
        {
            return r;
        }
        p->is_video = FM_FALSE;
#else
        return FM_ERR_UNSUPPORTED;
#endif
    }
    else
    {
        /* Image albums are scanned by SimpleImage; Play is audio/video. */
        return FM_ERR_UNSUPPORTED;
    }
    return FM_OK;
}

FM_Result Player_Next(FM_Player *p)
{
    uint32_t next;

    if (p == NULL || p->count == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    next = (p->current + 1) % p->count;
    return Player_Play(p, next);
}

FM_Result Player_Prev(FM_Player *p)
{
    uint32_t prev;

    if (p == NULL || p->count == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    prev = (p->current == 0) ? p->count - 1 : p->current - 1;
    return Player_Play(p, prev);
}

FM_Result Player_Stop(FM_Player *p)
{
    if (p == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (p->is_video)
    {
#if FM_USE_VIDEO
        if (!File_IsOpen(&p->video.file))
        {
            return FM_ERR_NOT_OPEN;
        }
        (void)Video_Close(&p->video);
        p->is_video = FM_FALSE;
#endif
    }
    else
    {
#if FM_USE_AUDIO
        if (!File_IsOpen(&p->audio.file))
        {
            return FM_ERR_NOT_OPEN;
        }
        (void)Audio_Close(&p->audio);
#endif
    }
    return FM_OK;
}

FM_Bool Player_IsVideo(const FM_Player *p)
{
    return (p != NULL && p->is_video) ? FM_TRUE : FM_FALSE;
}

uint32_t Player_Current(const FM_Player *p)
{
    return (p != NULL) ? p->current : 0;
}

FM_Result Player_ReadAudio(FM_Player *p, int16_t *samples, size_t count,
                           size_t *read)
{
#if FM_USE_AUDIO
    if (p == NULL || p->is_video || samples == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return Audio_Read(&p->audio, samples, count, read);
#else
    (void)p; (void)samples; (void)count; (void)read;
    return FM_ERR_UNSUPPORTED;
#endif
}

FM_Result Player_DecodeVideo(FM_Player *p, void *pixels, size_t cap,
                             uint16_t *w, uint16_t *h)
{
#if FM_USE_VIDEO
    FM_VideoInfo info;

    if (p == NULL || !p->is_video || pixels == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (Video_GetInfo(&p->video, &info) == FM_OK)
    {
        if (w != NULL) *w = info.width;
        if (h != NULL) *h = info.height;
    }
    return Video_DecodeFrame(&p->video, pixels, cap);
#else
    (void)p; (void)pixels; (void)cap; (void)w; (void)h;
    return FM_ERR_UNSUPPORTED;
#endif
}

FM_Result Player_Seek(FM_Player *p, double seconds)
{
    /* written so that "not a number" is refused too */
    if (p == NULL || !(seconds >= 0.0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (p->is_video)
    {
#if FM_USE_VIDEO
        if (p->video.info.fps != 0)
        {
            double at = seconds * (double)p->video.info.fps;
            /* a time past what a frame number can hold is the last frame
             * (converting it would be undefined) */
            uint32_t frame = (at >= 4294967295.0) ? UINT32_MAX : (uint32_t)at;

            return Video_Seek(&p->video, frame);
        }
#endif
        return FM_ERR_UNSUPPORTED;
    }
#if FM_USE_AUDIO
    return Audio_Seek(&p->audio, seconds);
#else
    return FM_ERR_UNSUPPORTED;
#endif
}

FM_Result Player_SeekFwd(FM_Player *p, double seconds)
{
    double pos;

    if (p == NULL || !(seconds >= 0.0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    pos = Player_GetPosition(p);
    return Player_Seek(p, pos + seconds);
}

FM_Result Player_SeekBack(FM_Player *p, double seconds)
{
    double pos;

    if (p == NULL || !(seconds >= 0.0))
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    pos = Player_GetPosition(p);
    pos = (pos > seconds) ? pos - seconds : 0.0;
    return Player_Seek(p, pos);
}

double Player_GetPosition(const FM_Player *p)
{
    if (p == NULL)
    {
        return 0.0;
    }
    if (p->is_video)
    {
#if FM_USE_VIDEO
        if (p->video.info.fps != 0)
        {
            return (double)p->video.current_frame / (double)p->video.info.fps;
        }
#endif
        return 0.0;
    }
#if FM_USE_AUDIO
    if (p->audio.info.sample_rate != 0)
    {
        return (double)p->audio.frame_index / (double)p->audio.info.sample_rate;
    }
#endif
    return 0.0;
}

double Player_GetDuration(const FM_Player *p)
{
    if (p == NULL)
    {
        return 0.0;
    }
    if (p->is_video)
    {
#if FM_USE_VIDEO
        if (p->video.info.fps != 0)
        {
            return (double)p->video.info.frame_count /
                   (double)p->video.info.fps;
        }
#endif
        return 0.0;
    }
#if FM_USE_AUDIO
    /* From the length the reader worked out for the format at hand
     * (FM_AudioInfo.total_frames).  This used to divide data_size by the
     * size of a 16-bit frame, which is the duration only for 16-bit PCM:
     * G.711 came out at half, ADPCM at a quarter, and FLAC / Vorbis /
     * Opus -- whose data_size is the compressed file -- at anything.
     * 0.0 = not known (AAC); MP3 without a Xing header is an estimate. */
    if (p->audio.info.sample_rate != 0)
    {
        return (double)p->audio.info.total_frames /
               (double)p->audio.info.sample_rate;
    }
#endif
    return 0.0;
}

#endif /* FM_USE_AUDIO || FM_USE_VIDEO */
