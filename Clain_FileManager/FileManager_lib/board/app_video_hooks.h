/*
 * app_video_hooks.h — قلاب‌های ویدیو
 * ==================================
 *
 *   app_video_track_start/end    شروع و پایان هر ویدیو
 *   app_video_frame_cb           هر فریم دیکدشده (حتی بدون نمایش)
 *   Music_InstallVideoHooks()   نصب هر سه — بعد از هر SimpleVideo_Begin()
 *   Music_VideoMute/Unmute      دستورهای vmut / vunmut
 *
 * فقط از main.c شامل می‌شود (بعد از app_audio_hooks.h).
 */
#ifndef FM_BOARD_APP_VIDEO_HOOKS_H_
#define FM_BOARD_APP_VIDEO_HOOKS_H_

/* ---- قلاب‌های ویدیو: شروع/پایان ترک + دادهٔ فریم (DSP مستقل از نمایش) ----
 *   - app_video_track_start/end: می‌گوید کدام ویدیو شروع/تمام شد.
 *   - app_video_frame_cb: هر فریم دیکدشده (تبدیل‌شده به sv.format، پیش‌فرض
 *     RGB565) را تحویل می‌دهد؛ حتی وقتی SimpleVideo_SetPresent(NULL) است
 *     (حالت فقط-داده: ۶۰٪ مواقع فقط داده می‌خواهیم، نه LCD). اینجا فقط یک
 *     شمارنده/تله‌متری سبک — پردازش گران را به تپ PCM یا فریم بدهید.
 *   نصب: SimpleVideo_SetTrackStartCallback/SetTrackEndCallback/SetFrameCallback
 *   بعد از هر SimpleVideo_Begin() (Begin حالت را پاک می‌کند). */
static volatile uint32_t video_frames;          /* فریم‌های دیده‌شده */
static volatile uint32_t video_seq;             /* شمارهٔ ویدیوی در حال پخش */
static char last_video_track[FM_PATH_MAX];

static void app_video_track_start(void *ctx, const char *path,
                                 uint32_t w, uint32_t h, uint32_t fps)
{
    (void)ctx;
    video_seq++;
    video_frames = 0u;
    if (path == NULL) path = "";
    (void)strncpy(last_video_track, path, sizeof(last_video_track) - 1u);
    last_video_track[sizeof(last_video_track) - 1u] = '\0';
    printf("video track #%lu START: %s (%lux%lu @ %lu fps)\n",
           (unsigned long)video_seq, path,
           (unsigned long)w, (unsigned long)h, (unsigned long)fps);
}

static void app_video_track_end(void *ctx, const char *path)
{
    (void)ctx;
    printf("video track END: %s (frames=%lu)\n",
           (path != NULL) ? path : "", (unsigned long)video_frames);
}

static void app_video_frame_cb(void *ctx, const void *px, uint16_t w,
                              uint16_t h, FM_PixelFormat fmt, size_t bytes)
{
    (void)ctx; (void)px; (void)w; (void)h; (void)fmt; (void)bytes;
    video_frames++;
    /* اینجا جای پردازش فریم (مثلاً Image_Convert / ماسک / ویژگی‌گیری) است.
     * سبک بماند؛ برای کار سنگین داده را در بافر خودتان کپی کنید چون بافر
     * دوبل است و تا فراخوانی بعدی معتبر است. */
}

/* نصب هر سه قلاب ویدیو. ماندگارند؛ اینجا شمارندهٔ فریم هم صفر می‌شود. */
static void Music_InstallVideoHooks(void)
{
    video_frames = 0u;
    SimpleVideo_SetTrackStartCallback(app_video_track_start, NULL);
    SimpleVideo_SetTrackEndCallback(app_video_track_end, NULL);
    SimpleVideo_SetFrameCallback(app_video_frame_cb, NULL);
}

/* vmut: بلندگو ساکت، ولی دادهٔ صدا همچنان به کالبک می‌رسد (فقط پردازش).
 * vunmut: بلندگو + کالبک. */
static void Music_VideoMute(void)
{
    (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_CALLBACK);
    printf("vmut: صدای ویدیو فقط به کالبک می‌رود (بلندگو ساکت)\n");
}
static void Music_VideoUnmute(void)
{
    (void)SimpleVideo_SetAudioOutput(FM_AUDIO_OUT_BOTH);
    printf("vunmut: صدای ویدیو روشن شد\n");
}

#endif /* FM_BOARD_APP_VIDEO_HOOKS_H_ */
