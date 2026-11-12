/*
 * app_save_hooks.h — ذخیرهٔ موزیک و ویدیوی در حال پخش، از راه کالبک‌ها
 * ====================================================================
 *
 * خودِ کار در top_api/examples_save_media.h است (SaveDemo_*)؛ این فایل
 * فقط آن را به کالبک‌های همین برد وصل می‌کند و چهار دستور UART می‌دهد:
 *
 *   arec   / arecs    موزیک  -> /save/music_cb.wav   (شروع / پایان)
 *   vrec   / vrecs    ویدیو  -> /save/video_cb.avi   (شروع / پایان)
 *
 * مسیر داده روی این برد:
 *
 *   موزیک   دیکدر ─> app_save_music_cb ─┬─> app_audio_pre_tap     نرمالایزر (tap1/tap0)
 *                                       └─> SaveDemo_MusicCallback  کپی در رینگ
 *           while (1): SaveDemo_MusicRecPump()   رینگ -> فایل
 *
 *   ویدیو   فریم ─> app_save_video_frame_cb ─┬─> app_video_frame_cb        شمارندهٔ برد
 *                                            └─> SaveDemo_VideoFrameCallback  JPEG -> فایل
 *           صدا  ─> app_save_video_sound_cb ─┬─> app_audio_pre_tap          نرمالایزر
 *                                            └─> SaveDemo_VideoSoundCallback  کپی در رینگ
 *           while (1): SaveDemo_VideoRecPump()   رینگ -> فایل، و بستن در پایان ویدیو
 *
 * چون ذخیره «بعد» از نرمالایزر صدا زده می‌شود، با tap1 همان صدای
 * نرمال‌شده در فایل می‌نشیند؛ با tap0 صدای خام فایل.
 *
 * نصب: App_SaveHooksInstall() — app_flow.h آن را بعد از Music_InstallTaps()
 * و Music_InstallVideoHooks() صدا می‌زند (آن دو کالبک داده و فریم را
 * می‌گذارند؛ این تابع زنجیرهٔ بالا را جایشان می‌نشاند).
 *
 * برای اینکه صدا و تصویر فایل ویدیو از یک نقطه شروع شوند، vrec را قبل
 * از vstrt بزنید؛ وسط پخش هم کار می‌کند ولی صدا کمی جلوتر می‌افتد.
 *
 * فقط از main.c شامل می‌شود (بعد از app_video_hooks.h).
 */
#ifndef FM_BOARD_APP_SAVE_HOOKS_H_
#define FM_BOARD_APP_SAVE_HOOKS_H_

/* ---- زنجیرهٔ کالبک‌ها: اول کار خود برد، بعد ذخیره ---------------------- */

static void app_save_music_cb(void *ctx, int16_t *samples, size_t count,
                              uint32_t rate, uint16_t channels)
{
    app_audio_pre_tap(ctx, samples, count, rate, channels);
    SaveDemo_MusicCallback(ctx, samples, count, rate, channels);
}

static void app_save_video_sound_cb(void *ctx, int16_t *samples, size_t count,
                                    uint32_t rate, uint16_t channels)
{
    app_audio_pre_tap(ctx, samples, count, rate, channels);
    SaveDemo_VideoSoundCallback(ctx, samples, count, rate, channels);
}

static void app_save_video_frame_cb(void *ctx, const void *px, uint16_t w,
                                    uint16_t h, FM_PixelFormat fmt,
                                    size_t bytes)
{
    app_video_frame_cb(ctx, px, w, h, fmt, bytes);
    SaveDemo_VideoFrameCallback(ctx, px, w, h, fmt, bytes);
}

/* کالبک‌ها ماندگارند؛ تا وقتی arec / vrec زده نشده SaveDemo_* فوری
 * برمی‌گردد و هزینه‌ای ندارد. */
static void App_SaveHooksInstall(void)
{
    SimpleAudio_SetDataCallback(app_save_music_cb, NULL);
    SimpleVideo_SetAudioDataCallback(app_save_video_sound_cb, NULL);
    SimpleVideo_SetFrameCallback(app_save_video_frame_cb, NULL);
}

/* ---- دستورهای UART (جدول در top_api/code_cmd.h) ------------------------ */

static void app_save_report(const char *cmd, FM_Result r)
{
    printf("%s: %s (%d)\n", cmd, File_GetErrorString(r), (int)r);
}

/* arec: از اولین تکهٔ بعدی کالبک، موزیک در فایل ذخیره می‌شود. خروجی صدا
 * باید «کالبک» را داشته باشد (aout3 = بلندگو + کالبک، پیش‌فرض برد). */
static void Cmd_SaveMusicStart(void)
{
    app_save_report("arec", SaveDemo_MusicRecStart(SAVE_DEMO_MUSIC_COPY));
}

static void Cmd_SaveMusicStop(void)
{
    app_save_report("arecs", SaveDemo_MusicRecStop());
}

/* vrec: از اولین فریم بعدی، تصویر و صدای ویدیو در فایل ذخیره می‌شود؛ در
 * پایان ویدیو خودش بسته می‌شود. */
static void Cmd_SaveVideoStart(void)
{
    app_save_report("vrec", SaveDemo_VideoRecStart(SAVE_DEMO_VIDEO_COPY));
}

static void Cmd_SaveVideoStop(void)
{
    app_save_report("vrecs", SaveDemo_VideoRecStop());
}

#endif /* FM_BOARD_APP_SAVE_HOOKS_H_ */
