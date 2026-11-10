/*
 * FileManager - MJPEG codec plugin (picojpeg backend)
 *
 * Provides an FM_VideoCodecOps implementation backed by the vendored
 * picojpeg decoder (third_party/picojpeg).  It decodes a single
 * JPEG-compressed frame (the payload of an MJPEG/AVI "MJPG" frame, or a
 * frame from a USB camera / ESP32-CAM) into RGB888 pixels.
 *
 * Notes:
 *   - Baseline sequential JPEG only (grayscale + YCbCr).  Progressive JPEG
 *     is not supported.
 *   - picojpeg keeps its work arrays in static memory and is not
 *     thread-safe / reentrant: decode one frame at a time.
 *   - The whole JPEG frame must be available in memory (the video layer
 *     buffers it; MJPEG frames are typically small).
 *
 * Usage:
 *     FM_Video v;
 *     Video_Open("/cam.avi", &v);
 *     Video_RegisterCodec(&v, MJPEG_GetCodecOps());
 *     uint8_t rgb[W*H*3];
 *     Video_DecodeFrame(&v, rgb, sizeof(rgb));
 */

#ifndef FM_MJPEG_H
#define FM_MJPEG_H

#include "fm_video.h"

FM_BEGIN_DECLS

#if FM_USE_MJPEG

/* Return the picojpeg-backed codec plugin for MJPEG frames. */
const FM_VideoCodecOps *MJPEG_GetCodecOps(void);

#endif /* FM_USE_MJPEG */

FM_END_DECLS

#endif /* FM_MJPEG_H */
